#include "Framegen/NativeFgReadback.hpp"
#include "Framegen/NativeFgOutput.hpp"
#include "Render/CommandResources.hpp"
#include "Render/RenderProbeScope.hpp"
#include "../Render/AerPixelEvidence.hpp"
#include <array>
#include <atomic>
#include <memory>
#include <mutex>

extern void Log(const char*,...);
namespace cvr::framegen {
namespace {
using Microsoft::WRL::ComPtr;
using render::pixel_evidence::Surface;
enum class Stage {Recorded,Prepared,Submitted,Unknown,Cancelled,Saved};
struct Capture {
    Stage stage=Stage::Recorded;
    ComPtr<ID3D12Resource> input,interpolated;
    ComPtr<ID3D12Resource> disableSource,disablePixels;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    Surface inputPixels,outputPixels;
    camera::ImagePoseIdentity inputLabel;
    uint64_t call{},recording{};
    uint32_t count{},index{},inputState{},outputState{},disableHeap{};
    bool listRetired{},disableHeapKnown{};
    bool disableCopied{};
    uint64_t pairSerial{},previousPairSerial{};
    uint32_t pairedEye{};
    SrSourceStamp pairedStamp;
};
std::array<std::unique_ptr<Capture>,12> captures;
std::mutex captureMutex;
std::atomic<uint32_t> attempts{};
std::atomic<bool> hasCaptures{};
const std::filesystem::path& Directory(){
    static const auto path=[] {wchar_t value[32768]{};const auto n=GetEnvironmentVariableW(
        L"CPVR_NATIVE_FG_PIXEL_DIR",value,DWORD(std::size(value)));
        const std::filesystem::path p=n&&n<std::size(value)?value:L"";return p.is_absolute()?p:std::filesystem::path{};}();
    return path;
}
bool Texture(const D3D12_RESOURCE_DESC& d){
    return d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&d.Width&&d.Height&&
        d.MipLevels==1&&d.DepthOrArraySize==1&&d.SampleDesc.Count==1;
}
void Release(Capture& c){
    c.input.Reset();c.interpolated.Reset();c.list.Reset();c.queue.Reset();c.fence.Reset();
    c.inputPixels.readback.Reset();c.outputPixels.readback.Reset();
    c.disableSource.Reset();c.disablePixels.Reset();
}
}
bool NativeFgPixelsWanted() noexcept {
    try{return !Directory().empty()&&attempts.load()<captures.size();}catch(...){return false;}
}
void RecordNativeFgReadback(ID3D12GraphicsCommandList* list,ID3D12Resource* input,D3D12_RESOURCE_STATES inputState,
    ID3D12Resource* interpolated,D3D12_RESOURCE_STATES outputState,ID3D12Resource* disable,bool disableStateKnown,D3D12_RESOURCE_STATES disableState,
    const camera::ImagePoseIdentity& label,uint64_t call,uint64_t recording,uint32_t count,uint32_t index,const NativeFgReceipt* paired) noexcept {
    if(!NativeFgPixelsWanted()||!list||!input||!interpolated||input==interpolated||!recording||
       (paired?(!paired->paired||!ValidNativeFgReceipt(*paired)||paired->currentPairSerial<128):
        (!label||!label.pose.mainEyeSource||label.pose.mainEyeSource.sourceId<128)))return;
    // Bounded diagnostic window: two adjacent intervals, all phases and eyes.
    // Paired identities remain paired; never forge an alternating-eye label.
    static uint64_t firstPair{};
    static uint64_t warmupPairs{};
    static uint64_t lastPair{};
    static bool seen[2][2][3]{};
    if(paired){
        std::lock_guard lock(captureMutex);
        if(lastPair!=paired->currentPairSerial){lastPair=paired->currentPairSerial;++warmupPairs;}
        if(warmupPairs<60)return;
        if(!firstPair){if(paired->index!=1)return;firstPair=paired->currentPairSerial;}
        if(paired->currentPairSerial<firstPair||paired->currentPairSerial>firstPair+1)return;
        auto& captured=seen[paired->currentPairSerial-firstPair][paired->pairedEye][paired->index-1];
        if(captured)return;captured=true;
    }
    const auto slot=attempts.fetch_add(1);if(slot>=captures.size())return;
    try{
        const auto inputDesc=input->GetDesc(),outputDesc=interpolated->GetDesc();
        if(!Texture(inputDesc)||!Texture(outputDesc)||inputDesc.Width!=outputDesc.Width||inputDesc.Height!=outputDesc.Height){Log("[NATIVE-FG-PIXELS] rejected call=%llu reason=shape\n",call);return;}
        ComPtr<ID3D12Device> device,inputDevice,outputDevice;
        ComPtr<IUnknown> identity,inputIdentity,outputIdentity;
        if(FAILED(list->GetDevice(IID_PPV_ARGS(&device)))||FAILED(input->GetDevice(IID_PPV_ARGS(&inputDevice)))||
           FAILED(interpolated->GetDevice(IID_PPV_ARGS(&outputDevice)))||FAILED(device.As(&identity))||
           FAILED(inputDevice.As(&inputIdentity))||FAILED(outputDevice.As(&outputIdentity))||identity!=inputIdentity||identity!=outputIdentity){Log("[NATIVE-FG-PIXELS] rejected call=%llu reason=device_identity\n",call);return;}
        auto capture=std::make_unique<Capture>();
        capture->input=input;capture->interpolated=interpolated;capture->list=list;capture->inputLabel=label;
        capture->call=call;capture->recording=recording;capture->count=count;capture->index=index;
        if(paired){capture->pairSerial=paired->currentPairSerial;capture->previousPairSerial=paired->previousPairSerial;capture->pairedEye=paired->pairedEye;capture->pairedStamp=paired->current;}
        capture->inputState=unsigned(inputState);capture->outputState=unsigned(outputState);
        if(disable){D3D12_HEAP_PROPERTIES heap{};D3D12_HEAP_FLAGS flags{};
            capture->disableHeapKnown=SUCCEEDED(disable->GetHeapProperties(&heap,&flags));capture->disableHeap=unsigned(heap.Type);}
        if(disable&&disableStateKnown&&capture->disableHeapKnown&&capture->disableHeap==D3D12_HEAP_TYPE_DEFAULT){
            const auto desc=disable->GetDesc();
            if(desc.Dimension==D3D12_RESOURCE_DIMENSION_BUFFER&&desc.Width>=4){
                D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
                D3D12_RESOURCE_DESC target{};target.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;target.Width=4;
                target.Height=1;target.DepthOrArraySize=1;target.MipLevels=1;target.SampleDesc.Count=1;target.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                if(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&target,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&capture->disablePixels))))capture->disableSource=disable;
            }
        }
        if(FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&capture->fence)))||
           !render::pixel_evidence::Allocate(device.Get(),input,capture->inputPixels)||
           !render::pixel_evidence::Allocate(device.Get(),interpolated,capture->outputPixels)){Log("[NATIVE-FG-PIXELS] rejected call=%llu reason=allocation\n",call);return;}
        if(!gpu::KeepCommandResources(list,{input,interpolated,capture->inputPixels.readback.Get(),
            capture->outputPixels.readback.Get(),capture->fence.Get()})){Log("[NATIVE-FG-PIXELS] rejected call=%llu reason=resource_lease\n",call);return;}
        if(capture->disableSource&&!gpu::KeepCommandResources(list,{capture->disableSource.Get(),capture->disablePixels.Get()})){
            capture->disableSource.Reset();capture->disablePixels.Reset();
        }
        {
            stereo::probe::Scope internal;
            render::pixel_evidence::Copy(list,input,inputState,capture->inputPixels);
            render::pixel_evidence::Copy(list,interpolated,outputState,capture->outputPixels);
            if(capture->disableSource){
                D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition={disable,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,disableState,D3D12_RESOURCE_STATE_COPY_SOURCE};
                if(disableState!=D3D12_RESOURCE_STATE_COPY_SOURCE)list->ResourceBarrier(1,&barrier);
                list->CopyBufferRegion(capture->disablePixels.Get(),0,disable,0,4);
                if(disableState!=D3D12_RESOURCE_STATE_COPY_SOURCE){std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);list->ResourceBarrier(1,&barrier);}
                capture->disableCopied=true;
            }
        }
        {
            std::lock_guard lock(captureMutex);captures[slot]=std::move(capture);
            hasCaptures.store(true,std::memory_order_release);
        }
        Log("[NATIVE-FG-PIXELS] recorded call=%llu recording=%llu inputSource=%lld inputEye=%u list=%p inputState=%u outputState=%u\n",
            call,recording,static_cast<long long>(label.pose.mainEyeSource.sourceId),unsigned(label.pose.mainEyeSource.physicalEye),list,unsigned(inputState),unsigned(outputState));
    }catch(...){Log("[NATIVE-FG-PIXELS] call=%llu recordException=1\n",call);}
}
void NativeFgReadbackReset(ID3D12GraphicsCommandList* list) noexcept {
    if(!hasCaptures.load(std::memory_order_acquire)||!list)return;
    std::lock_guard lock(captureMutex);
    for(auto& c:captures)if(c&&c->list.Get()==list){
        c->listRetired=true;
        if(c->stage==Stage::Recorded){c->stage=Stage::Cancelled;Release(*c);}
    }
}
void NativeFgReadbackBeforeSubmit(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList* const* lists) noexcept {
    if(!hasCaptures.load(std::memory_order_acquire)||!queue||!lists)return;
    std::lock_guard lock(captureMutex);
    for(auto& c:captures)if(c&&!c->listRetired&&(c->stage==Stage::Recorded||c->stage==Stage::Submitted)){
        for(UINT i=0;i<count;++i)if(lists[i]==c->list.Get()){
            if(c->stage==Stage::Submitted){c->stage=Stage::Unknown;Log("[NATIVE-FG-PIXELS] replayRejected call=%llu\n",c->call);break;}
            c->queue=queue;c->stage=Stage::Prepared;break;
        }
    }
}
void NativeFgReadbackSubmitted(ID3D12CommandQueue* queue) noexcept {
    if(!hasCaptures.load(std::memory_order_acquire)||!queue)return;
    std::lock_guard lock(captureMutex);
    for(auto& c:captures)if(c&&c->stage==Stage::Prepared&&c->queue.Get()==queue){
        const auto result=queue->Signal(c->fence.Get(),1);
        c->stage=SUCCEEDED(result)?Stage::Submitted:Stage::Unknown;
        Log("[NATIVE-FG-PIXELS] submitted call=%llu queue=%p signal=%08X\n",c->call,queue,unsigned(result));
    }
}
void PollNativeFgReadbacks() noexcept {
    if(!hasCaptures.load(std::memory_order_acquire))return;
    std::lock_guard submissionLock(camera::ImageSubmissionMutex());
    std::lock_guard lock(captureMutex);
    for(auto& c:captures)if(c&&c->stage==Stage::Submitted){
        const auto completed=c->fence->GetCompletedValue();
        if(completed==UINT64_MAX){c->stage=Stage::Unknown;continue;}
        if(completed<1)continue;
        bool saved=false;
        try{
            std::filesystem::create_directories(Directory());
            const auto stem="fg-call"+std::to_string(c->call)+(c->pairSerial?"-eye"+std::to_string(c->pairedEye)+"-phase"+std::to_string(c->index):"");
            saved=render::pixel_evidence::SaveSurface(c->inputPixels,Directory()/(stem+"-input.raw"))&&
                render::pixel_evidence::SaveSurface(c->outputPixels,Directory()/(stem+"-interpolated.raw"));
            if(saved){
                bool disableKnown=false;unsigned char disableValue=0;
                if(c->disableCopied){void* value{};D3D12_RANGE range{0,4};
                    if(SUCCEEDED(c->disablePixels->Map(0,&range,&value))&&value){disableValue=*static_cast<const unsigned char*>(value);disableKnown=true;D3D12_RANGE empty{};c->disablePixels->Unmap(0,&empty);}}
                std::ofstream out(Directory()/(stem+".json"));const auto& p=c->inputLabel.pose;
                out<<"{\"call\":"<<c->call<<",\"recording\":"<<c->recording<<",\"input_source\":"<<(c->pairSerial?"null":std::to_string(p.mainEyeSource.sourceId))
                    <<",\"input_eye\":"<<(c->pairSerial?c->pairedEye:unsigned(p.mainEyeSource.physicalEye))<<",\"input_render_frame\":"<<(c->pairSerial?c->pairedStamp.nativeFrame:p.renderFrameId)
                    <<",\"input_pose\":"<<(c->pairSerial?c->pairedStamp.poseId:p.poseId)<<",\"input_write\":"<<(c->pairSerial?c->pairedStamp.writeId:p.writeId)
                    <<",\"count\":"<<c->count<<",\"index\":"<<c->index<<",\"completed_fence\":1"
                    <<",\"input_state\":"<<c->inputState<<",\"interpolated_state\":"<<c->outputState
                    <<",\"disable_heap_known\":"<<(c->disableHeapKnown?"true":"false")<<",\"disable_heap_type\":"<<c->disableHeap
                    <<",\"interpolation_allowed\":"<<(disableKnown?(disableValue?"false":"true"):"null")
                    <<",\"disable_value\":"<<(disableKnown?std::to_string(unsigned(disableValue)):"null")
                    <<",\"paired_serial\":"<<c->pairSerial<<",\"previous_paired_serial\":"<<c->previousPairSerial
                    <<",\"generated_eye\":"<<(c->pairSerial?std::to_string(c->pairedEye):"null")<<",\"submitted_to_xr\":false";
                const auto surface=[&](const char* name,const Surface& s){out<<",\""<<name<<"\":{\"width\":"<<s.description.Width
                    <<",\"height\":"<<s.description.Height<<",\"format\":"<<unsigned(s.description.Format)<<",\"row_bytes\":"<<s.rowBytes<<",\"rows\":"<<s.rows<<'}';};
                surface("input",c->inputPixels);surface("interpolated",c->outputPixels);
                out<<"}\n";out.close();saved=bool(out);
            }
        }catch(...){saved=false;}
        c->stage=Stage::Saved;
        Log("[NATIVE-FG-PIXELS] completed call=%llu fence=%llu saved=%d\n",c->call,completed,int(saved));
        Release(*c);
    }
}
}
