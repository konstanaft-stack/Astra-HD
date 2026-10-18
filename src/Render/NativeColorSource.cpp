#include "Render/NativeColorSource.hpp"
#include "Render/NativeColorSnapshots.hpp"
#include "Core/VrCoreShared.hpp"
#include "Utils/MemorySafe.hpp"
#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace cvr::render {
namespace {
using Ledger=camera::ImagePoseLedger<NativeColorCapture>;
struct Recording {
    NativeColorSequence<NativeColorCapture> sequence;
    NativeColorCapture selected;
    uintptr_t window{};
};
struct Owner {
    Ledger ledger;
    NativeColorSnapshots snapshots;
    std::mutex mutex,lifetimeMutex;
    std::array<uintptr_t,16> windows{};
    std::unordered_map<uintptr_t,Recording> recordings;
};
auto owner=std::make_shared<Owner>();
std::atomic<bool> used{false};
std::atomic<uintptr_t> lastSelectedResource{};
// Process-local observation only. No selection, lifetime or queue rules change.
bool TraceNativeFg(){
    static const bool enabled=[] {char value[8]{};return GetEnvironmentVariableA(
        "CPVR_NATIVE_FG_DIAG",value,sizeof(value))==1&&value[0]=='1';}();
    return enabled;
}
struct NativeFgCounters {
    std::atomic<uint64_t> targets{},candidates{},windows{},chosen{},poseRejected{},frozen{},recorded{},queries{},missing{},samples{};
} nativeFgCounters;
constexpr GUID lifetimeKey{0xd419f98a,0x27f2,0x41f4,{0x9a,0x41,0x68,0xb3,0xfa,0x98,0x35,0x08}};
class Lifetime final:public IUnknown {
    std::atomic<ULONG> refs{1};
    std::weak_ptr<Owner> holder=owner;
    uintptr_t address;
    bool list;
public:
    Lifetime(uintptr_t p,bool l):address(p),list(l){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out)override{
        if(!out)return E_POINTER;*out=nullptr;if(id!=__uuidof(IUnknown))return E_NOINTERFACE;
        *out=this;AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}
    ULONG STDMETHODCALLTYPE Release()override{
        const auto n=--refs;if(!n){if(auto o=holder.lock()){
            if(list){std::lock_guard lock(o->mutex);o->recordings.erase(address);o->ledger.ForgetList(address);}
            else o->ledger.ForgetResource(address);
        }delete this;}return n;
    }
};
bool Track(ID3D12Object* object,bool list){
    if(!object)return false;
    IUnknown* existing{};Microsoft::WRL::ComPtr<Lifetime> created;HRESULT hr{};
    {
        std::lock_guard lock(owner->lifetimeMutex);UINT bytes=sizeof(existing);
        hr=object->GetPrivateData(lifetimeKey,&bytes,&existing);
        if(FAILED(hr)||!existing){created.Attach(new(std::nothrow) Lifetime(reinterpret_cast<uintptr_t>(object),list));
            hr=created?object->SetPrivateDataInterface(lifetimeKey,created.Get()):E_OUTOFMEMORY;}
    }
    if(existing)existing->Release();return SUCCEEDED(hr);
}
bool SamePose(const camera::PoseIdentity& a,const camera::PoseIdentity& b){
    return a && b && a.view==1 && b.view==1 && a.mainEyeSource &&
        a.mainEyeSource==b.mainEyeSource && a.poseId==b.poseId && a.writeId==b.writeId &&
        a.hasRenderFrameId && b.hasRenderFrameId && a.renderFrameId==b.renderFrameId &&
        a.head.originSerial==b.head.originSerial;
}
}
bool NativeColorSourceEnabled(){
    static const bool enabled=[] {char flag[8]{};return !(GetEnvironmentVariableA(
        "CPVR_NATIVE_COLOR_SOURCE",flag,sizeof(flag))==1&&flag[0]=='0');}();
    return enabled;
}
void NativeColorBackbuffers(std::span<const uintptr_t> buffers){
    std::array<uintptr_t,16> next{};std::copy_n(buffers.begin(),std::min(buffers.size(),next.size()),next.begin());
    std::lock_guard lock(owner->mutex);
    if(next!=owner->windows){owner->windows=next;owner->recordings.clear();owner->ledger.InvalidatePublished();}
}
void NativeColorTarget(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,
    const NativeColorDescription& description,const camera::PoseIdentity& pose){
    if(!list||!NativeColorSourceEnabled()||!camera::MainEyeSourceRequested())return;
    if(!Track(list,true))return;
    const auto pointer=reinterpret_cast<uintptr_t>(resource);
    uint32_t width{},height{};
    const auto settings=uintptr_t(g_settingsResPtr);
    if(settings>=0x10000){ReadU32Safe(settings+0x84,&width);ReadU32Safe(settings+0x88,&height);}
    std::lock_guard lock(owner->mutex);
    const bool window=pointer && std::find(owner->windows.begin(),owner->windows.end(),pointer)!=owner->windows.end();
    NativeColorCapture value;
    const bool candidate=resource && NativeColorCandidate(description,width,height) &&
        pose && pose.view==1 && pose.mainEyeSource && pose.hasRenderFrameId;
    if(TraceNativeFg()){
        ++nativeFgCounters.targets;
        if(candidate)++nativeFgCounters.candidates;
        if(window)++nativeFgCounters.windows;
    }
    if(candidate){
        value.description=resource->GetDesc();
        if(value.description.MipLevels==1 && value.description.DepthOrArraySize==1 && value.description.SampleDesc.Count==1){
            value.resource=resource;value.pose=pose;
            static const bool diagnostic=[] {char flag[8]{};return GetEnvironmentVariableA(
                "CPVR_D3D12_VALIDATION",flag,sizeof(flag))==1&&flag[0]=='1';}();
            if(diagnostic)resource->SetName(L"CVR_native_color_selected_source");
        }
    }
    auto& recording=owner->recordings[reinterpret_cast<uintptr_t>(list)];
    if(TraceNativeFg()&&pose.mainEyeSource&&pose.mainEyeSource.sourceId>=32&&pose.mainEyeSource.sourceId<40&&
       nativeFgCounters.samples.fetch_add(1,std::memory_order_relaxed)<96){
        Log("[NATIVE-FG-TARGET] list=%p resource=%p size=%llux%u expected=%ux%u format=%u explicit=%d dimension=%u mip=%u window=%d candidate=%d source=%lld eye=%u view=%u frameKnown=%d frame=%u pose=%llu write=%llu previous=%p\n",
            list,resource,static_cast<unsigned long long>(description.width),description.height,width,height,description.format,
            int(description.explicitView),description.viewDimension,description.mip,int(window),int(candidate),
            static_cast<long long>(pose.mainEyeSource.sourceId),unsigned(pose.mainEyeSource.physicalEye),pose.view,
            int(pose.hasRenderFrameId),pose.renderFrameId,pose.poseId,pose.writeId,
            recording.sequence.Previous()?recording.sequence.Previous()->resource.Get():nullptr);
    }
    const auto chosen=recording.sequence.Bind(window,candidate,value);
    if(!window)return;
    // AF1E90 returns immediately when this list has no preceding native target.
    // In particular, an unrelated overlay's later window bind must not revoke
    // the game's selected image. Its actual pose is revalidated at capture.
    if(!chosen)return;
    if(TraceNativeFg())++nativeFgCounters.chosen;
    if(!SamePose(chosen->pose,pose)){
        if(TraceNativeFg())++nativeFgCounters.poseRejected;
        return;
    }
    const auto frozen=owner->snapshots.Record(list,chosen->resource.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if(!frozen)return;
    if(TraceNativeFg())++nativeFgCounters.frozen;
    recording.selected=*chosen;recording.window=pointer;
    // AF1F1F -> AEA090 explicitly supplies NON_PIXEL_SHADER_RESOURCE.
    // Subsequent observed transitions below update or revoke this contract.
    recording.selected.stateKnown=true;recording.selected.window=pointer;
    lastSelectedResource.store(reinterpret_cast<uintptr_t>(chosen->resource.Get()),std::memory_order_relaxed);
    recording.selected.resource=frozen.texture;recording.selected.lifetime=frozen.lifetime;
    camera::RecordImagePose(list,frozen.texture.Get(),chosen->pose);
    if(Track(resource,false)){
        owner->ledger.Record(reinterpret_cast<uintptr_t>(list),pointer,recording.selected,false);
        if(TraceNativeFg())++nativeFgCounters.recorded;
        used.store(true,std::memory_order_release);
    }
}
void NativeColorUnknownPoseDiagnostic(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,uint32_t node,uintptr_t viewKey){
    static const bool diagnostic=[] {char flag[8]{};return GetEnvironmentVariableA(
        "CPVR_D3D12_VALIDATION",flag,sizeof(flag))==1&&flag[0]=='1';}();
    if(!diagnostic||reinterpret_cast<uintptr_t>(resource)!=lastSelectedResource.load(std::memory_order_relaxed))return;
    static std::atomic<uint32_t> count{};
    if(count.fetch_add(1,std::memory_order_relaxed)<8)
        Log("[NATIVE-COLOR-UNKNOWN-POSE] list=%p resource=%p node=%X viewKey=%llX\n",list,resource,node,
            static_cast<unsigned long long>(viewKey));
}
void NativeColorBarriers(ID3D12GraphicsCommandList* list,UINT count,const D3D12_RESOURCE_BARRIER* barriers){
    if(!used.load(std::memory_order_acquire)||!list||!barriers)return;
    std::lock_guard lock(owner->mutex);const auto it=owner->recordings.find(reinterpret_cast<uintptr_t>(list));
    if(it==owner->recordings.end())return;
    auto& recording=it->second;
    for(UINT i=0;i<count;++i){const auto& b=barriers[i];auto& selected=recording.selected;
        if(b.Type==D3D12_RESOURCE_BARRIER_TYPE_ALIASING){
            const auto touches=[&](ID3D12Resource* r){return !b.Aliasing.pResourceBefore||!b.Aliasing.pResourceAfter||
                b.Aliasing.pResourceBefore==r||b.Aliasing.pResourceAfter==r;};
            if(recording.sequence.Previous() && touches(recording.sequence.Previous()->resource.Get()))recording.sequence.Reset();
            if(selected.resource && !selected.lifetime && touches(selected.resource.Get())){
                selected={};owner->ledger.Record(reinterpret_cast<uintptr_t>(list),recording.window,selected,false);
            }
        }else if(b.Type==D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && selected.resource && !selected.lifetime && b.Transition.pResource==selected.resource.Get()){
            const bool complete=b.Transition.Subresource==D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ||
                (b.Transition.Subresource==0 && selected.description.MipLevels==1 && selected.description.DepthOrArraySize==1);
            selected.stateKnown=complete && !(b.Flags&D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY);
            selected.state=b.Transition.StateAfter;
            owner->ledger.Record(reinterpret_cast<uintptr_t>(list),recording.window,selected,false);
        }
    }
}
void NativeColorReset(ID3D12GraphicsCommandList* list){
    if(!list)return;std::lock_guard lock(owner->mutex);
    owner->recordings.erase(reinterpret_cast<uintptr_t>(list));owner->ledger.Reset(reinterpret_cast<uintptr_t>(list));
}
NativeColorSubmission PrepareNativeColor(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList* const* lists)noexcept{
    if(!used.load(std::memory_order_acquire))return {};
    if(!queue||(count&&!lists)){owner->ledger.InvalidatePublished();return {};}
    return owner->ledger.Prepare(reinterpret_cast<uintptr_t>(queue),count,[&](size_t i){return reinterpret_cast<uintptr_t>(lists[i]);});
}
void CommitNativeColor(ID3D12CommandQueue* queue,NativeColorSubmission&& submission)noexcept{
    owner->ledger.Commit(reinterpret_cast<uintptr_t>(queue),std::move(submission),GetTickCount64());
}
NativeColorCapture AcquireNativeColor(ID3D12Resource* window){
    const auto image=owner->ledger.Read(reinterpret_cast<uintptr_t>(window));
    if(TraceNativeFg()){
        const auto query=++nativeFgCounters.queries;
        if(!image||!image.pose)++nativeFgCounters.missing;
        if(query<=4 || query%300==1){
            Log("[NATIVE-FG-COLOR] query=%llu window=%p image=%d source=%lld eye=%u queue=%p targets=%llu candidates=%llu windows=%llu chosen=%llu poseRejected=%llu frozen=%llu recorded=%llu missing=%llu\n",
                query,window,int(bool(image)&&bool(image.pose)),static_cast<long long>(image.pose.pose.mainEyeSource.sourceId),
                unsigned(image.pose.pose.mainEyeSource.physicalEye),reinterpret_cast<void*>(image.queue),
                nativeFgCounters.targets.load(),nativeFgCounters.candidates.load(),nativeFgCounters.windows.load(),
                nativeFgCounters.chosen.load(),nativeFgCounters.poseRejected.load(),nativeFgCounters.frozen.load(),
                nativeFgCounters.recorded.load(),nativeFgCounters.missing.load());
        }
    }
    if(!image||!image.pose)return {};
    auto result=image.pose;result.queue=image.queue;result.selectionGeneration=image.generation;return result;
}
bool ValidateNativeColor(const NativeColorCapture& capture,ID3D12CommandQueue* queue,const camera::ImagePoseIdentity& image){
    const auto current=owner->ledger.Read(capture.window);
    uint32_t rejected{};
    if(!capture)rejected|=1;
    if(capture.queue!=reinterpret_cast<uintptr_t>(queue))rejected|=2;
    if(!image)rejected|=4;
    if(image.queue!=capture.queue)rejected|=8;
    if(!SamePose(capture.pose,image.pose))rejected|=16;
    if(!camera::ValidateMainEyeSourceReceipt(capture.pose.mainEyeSource))rejected|=32;
    if(!current||current.generation!=capture.selectionGeneration||current.pose.resource!=capture.resource)rejected|=64;
    static const bool diagnostic=[] {char flag[8]{};return GetEnvironmentVariableA(
        "CPVR_D3D12_VALIDATION",flag,sizeof(flag))==1&&flag[0]=='1';}();
    if(diagnostic&&rejected&&capture.pose.mainEyeSource.sourceId>=16){
        static std::array<std::atomic<uint32_t>,128> reasons{};
        if(reasons[rejected].fetch_add(1,std::memory_order_relaxed)<3){
            const auto& a=capture.pose;const auto& b=image.pose;
            Log("[NATIVE-COLOR-REJECT] mask=%u source=%lld/%lld frame=%u/%u frameKnown=%d/%d pose=%llu/%llu write=%llu/%llu view=%u/%u queue=%p/%p/%p generation=%llu/%llu clock=%llu/%llu latch=%llu/%llu\n",
                rejected,static_cast<long long>(a.mainEyeSource.sourceId),static_cast<long long>(b.mainEyeSource.sourceId),
                a.renderFrameId,b.renderFrameId,int(a.hasRenderFrameId),int(b.hasRenderFrameId),
                a.poseId,b.poseId,a.writeId,b.writeId,a.view,b.view,reinterpret_cast<void*>(capture.queue),
                reinterpret_cast<void*>(image.queue),queue,capture.selectionGeneration,current.generation,
                a.mainEyeSource.clockGeneration,b.mainEyeSource.clockGeneration,a.mainEyeSource.latchRevision,b.mainEyeSource.latchRevision);
        }
    }
    return !rejected;
}
}
