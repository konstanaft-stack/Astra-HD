#include "Framegen/NativeFgOutput.hpp"
#include "Framegen/ResizeQueueDrain.hpp"
#include "Hooks/CetLifecycle.hpp"
#include "Framegen/NativeFgReadback.hpp"
#include "Framegen/RealCaptureReuse.hpp"
#include "Framegen/ColorRead.hpp"
#include "Render/CommandResources.hpp"
#include "Render/RenderProbeScope.hpp"
#include <mutex>
#include <algorithm>
#include <atomic>
extern void Log(const char*,...);
extern "C" {
__declspec(dllexport) std::atomic<uint32_t> CyberpunkVR_ResizeDrainQueueCount{0};
__declspec(dllexport) std::atomic<uint32_t> CyberpunkVR_ResizeDrainRegistryFailed{0};
}
namespace cvr::framegen {
namespace {
using Microsoft::WRL::ComPtr;
enum class State{Recorded,Prepared,Submitted,Ready,Suppressed,Cancelled,Unknown};
struct RealSnapshot {
    ComPtr<ID3D12Resource> color;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12CommandQueue> queue;
    uint64_t value{};
    RealCaptureLifetime lifetime;
    NativeFgReceipt receipt;
    bool Ready()const{if(!fence||!value)return false;const auto done=fence->GetCompletedValue();return done!=UINT64_MAX&&done>=value;}
};
struct Slot {
    State state=State::Cancelled;
    NativeFgReceipt receipt;
    ComPtr<ID3D12Resource> color,disableReadback;
    std::shared_ptr<RealSnapshot> real;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    uint64_t value{},call{},recording{};
    bool retired=true,interpolationAllowed{},realAdmission=true;
    std::shared_ptr<std::atomic<bool>> valid;
    std::weak_ptr<const NativeFgPacket> packet;
};
std::mutex mutex;
std::array<std::shared_ptr<Slot>,16> slots;
NativeFgOutputStatus status;
size_t nextSlot{};
struct ResizeQueue {ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12Fence> fence;uint64_t value{};};
std::mutex resizeMutex,resizeDrainMutex;
std::vector<ResizeQueue> resizeQueues;
bool resizeQueueFailure{};
uintptr_t Identity(IUnknown* p){ComPtr<IUnknown> id;return p&&SUCCEEDED(p->QueryInterface(IID_PPV_ARGS(&id)))?uintptr_t(id.Get()):0;}
bool SameDevice(ID3D12DeviceChild*a,ID3D12DeviceChild*b){ComPtr<ID3D12Device>x,y;return a&&b&&SUCCEEDED(a->GetDevice(IID_PPV_ARGS(&x)))&&SUCCEEDED(b->GetDevice(IID_PPV_ARGS(&y)))&&Identity(x.Get())==Identity(y.Get());}
void Barrier(ID3D12GraphicsCommandList*l,ID3D12Resource*r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){if(a==b)return;D3D12_RESOURCE_BARRIER t{};t.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;t.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};l->ResourceBarrier(1,&t);}
bool Complete(const Slot&s){if(!s.fence||!s.value)return false;const auto n=s.fence->GetCompletedValue();return n!=UINT64_MAX&&n>=s.value;}
bool OwnsReal(const Slot& s){return s.real&&s.real->fence==s.fence&&s.real->value==s.value;}
void Unknown(Slot&s){s.state=State::Unknown;if(s.valid)s.valid->store(false,std::memory_order_release);
    if(OwnsReal(s))s.real->lifetime.invalid=true;++status.unknown;}
void Poll(){for(auto& s:slots)if(s&&s->state==State::Submitted){
    if(s->fence->GetCompletedValue()==UINT64_MAX){Unknown(*s);continue;}if(!Complete(*s))continue;
    if(!s->realAdmission){s->state=State::Suppressed;s->valid->store(false,std::memory_order_release);continue;}
    void* bytes{};D3D12_RANGE range{0,4};if(FAILED(s->disableReadback->Map(0,&range,&bytes))){Unknown(*s);continue;}
    const bool allowed=*static_cast<const unsigned char*>(bytes)==0;D3D12_RANGE none{};s->disableReadback->Unmap(0,&none);
    s->state=State::Ready;s->interpolationAllowed=allowed;s->valid->store(true,std::memory_order_release);
    if(allowed){++status.ready;if(s->receipt.paired)++status.phaseReady[s->receipt.pairedEye][s->receipt.index-1];}else ++status.suppressed;
}}
}
void RecordNativeFgOutput(ID3D12GraphicsCommandList* list,ID3D12Resource* real,D3D12_RESOURCE_STATES realState,ID3D12Resource* input,D3D12_RESOURCE_STATES colorState,
    ID3D12Resource* disable,D3D12_RESOURCE_STATES disableState,const NativeFgReceipt&receipt,uint64_t call,uint64_t recording) noexcept {
    if(!list||!input||!real||!disable||!call||!recording||!ValidNativeFgReceipt(receipt)||!SameDevice(list,input)||!SameDevice(list,real)||!SameDevice(list,disable))return;
    try{
        const auto desc=input->GetDesc(),realDesc=real->GetDesc(),flag=disable->GetDesc();D3D12_HEAP_PROPERTIES heap{};D3D12_HEAP_FLAGS heapFlags{};
        if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||!desc.Width||!desc.Height||desc.DepthOrArraySize!=1||desc.MipLevels!=1||desc.SampleDesc.Count!=1||
            realDesc.Width!=desc.Width||realDesc.Height!=desc.Height||realDesc.Format!=desc.Format||
            flag.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER||flag.Width<4||FAILED(disable->GetHeapProperties(&heap,&heapFlags))||heap.Type!=D3D12_HEAP_TYPE_DEFAULT)return;
        std::lock_guard lock(mutex);Poll();
        const auto capacity=std::min(slots.size(),size_t((receipt.count+1)*4));
        auto cell=slots.end();for(size_t n=0;n<capacity;++n){const auto i=(nextSlot+n)%capacity;const auto&s=slots[i];
            if(!s||(s.use_count()==1&&s->retired&&(s->state==State::Cancelled||((s->state==State::Ready||s->state==State::Suppressed)&&Complete(*s))))){cell=slots.begin()+i;nextSlot=(i+1)%capacity;break;}}
        if(cell==slots.end()){
            const auto busy=++status.poolBusy;
            if(busy<=4 || busy%256==0){unsigned leased{},unretired{},pending{},unknown{};
                for(size_t i=0;i<capacity;++i)if(const auto&s=slots[i]){
                    leased+=s.use_count()>1;unretired+=!s->retired;
                    pending+=s->state==State::Recorded||s->state==State::Prepared||s->state==State::Submitted;
                    unknown+=s->state==State::Unknown;
                }
                Log("[NATIVE-FG-POOL] busy=%llu capacity=%zu leased=%u unretired=%u pending=%u unknown=%u eye=%u source=%llu index=%u count=%u\n",
                    busy,capacity,leased,unretired,pending,unknown,receipt.pairedEye,receipt.currentPairSerial,receipt.index,receipt.count);
            }return;
        }
        if(!*cell)*cell=std::make_shared<Slot>();auto& s=**cell;
        ComPtr<ID3D12Device> device;if(FAILED(list->GetDevice(IID_PPV_ARGS(&device))))return;
        const auto shapeMatches=[&](ID3D12Resource* r){return r&&SameDevice(r,list)&&r->GetDesc().Width==desc.Width&&r->GetDesc().Height==desc.Height&&r->GetDesc().Format==desc.Format;};
        std::shared_ptr<RealSnapshot> sharedReal;
        if(receipt.paired)for(const auto& prior:slots)if(prior&&prior->real&&
            SamePairedRealSource(prior->real->receipt,receipt)&&prior->real->lifetime.CanShare()&&shapeMatches(prior->real->color.Get())){
            sharedReal=prior->real;break;
        }
        // This cell is retired and has no consumers. Invalidate its old packet
        // before allocation can fail or replace a resource.
        if(s.valid)s.valid->store(false,std::memory_order_release);s.state=State::Cancelled;s.packet.reset();
        const bool same=shapeMatches(s.color.Get())&&s.disableReadback&&s.fence;
        if(!same){s.color.Reset();s.real.reset();s.disableReadback.Reset();s.fence.Reset();s.value=0;
            auto target=desc;target.Flags=D3D12_RESOURCE_FLAG_NONE;target.Alignment=0;heap={};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
            if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&target,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&s.color))))return;
            D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=4;buffer.Height=1;buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            heap.Type=D3D12_HEAP_TYPE_READBACK;
            if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&s.disableReadback)))||FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&s.fence))))return;
        }
        const bool copyReal=!sharedReal;
        if(sharedReal)s.real=std::move(sharedReal);
        else if(!s.real||s.real.use_count()!=1||!shapeMatches(s.real->color.Get())){
            auto fresh=std::make_shared<RealSnapshot>();auto target=desc;target.Flags=D3D12_RESOURCE_FLAG_NONE;target.Alignment=0;
            heap={};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
            if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&target,CaptureColorReadState,nullptr,IID_PPV_ARGS(&fresh->color))))return;
            s.real=std::move(fresh);
        }
        if(!s.real||!s.disableReadback||!s.fence||s.value==UINT64_MAX)return;
        if(!gpu::KeepCommandResources(list,{real,input,disable,s.real->color.Get(),s.color.Get(),s.disableReadback.Get(),s.fence.Get()}))return;
        s.valid=std::make_shared<std::atomic<bool>>(false);s.packet.reset();s.list=list;s.queue.Reset();s.retired=false;s.realAdmission=true;
        s.receipt=receipt;s.call=call;s.recording=recording;++s.value;s.state=State::Recorded;
        if(copyReal){s.real->receipt=receipt;s.real->fence=s.fence;s.real->value=s.value;s.real->lifetime={};s.real->queue.Reset();}
        {cvr::stereo::probe::Scope internal;
            Barrier(list,input,colorState,D3D12_RESOURCE_STATE_COPY_SOURCE);Barrier(list,s.color.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyResource(s.color.Get(),input);Barrier(list,input,D3D12_RESOURCE_STATE_COPY_SOURCE,colorState);Barrier(list,s.color.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
            if(copyReal){
                Barrier(list,real,realState,D3D12_RESOURCE_STATE_COPY_SOURCE);Barrier(list,s.real->color.Get(),CaptureColorReadState,D3D12_RESOURCE_STATE_COPY_DEST);
                list->CopyResource(s.real->color.Get(),real);Barrier(list,real,D3D12_RESOURCE_STATE_COPY_SOURCE,realState);Barrier(list,s.real->color.Get(),D3D12_RESOURCE_STATE_COPY_DEST,CaptureColorReadState);
            }
            Barrier(list,disable,disableState,D3D12_RESOURCE_STATE_COPY_SOURCE);list->CopyBufferRegion(s.disableReadback.Get(),0,disable,0,4);Barrier(list,disable,D3D12_RESOURCE_STATE_COPY_SOURCE,disableState);
        }++status.recorded;
        if(copyReal)++status.realCopies;else ++status.realReused;
        if(status.recorded<=8||status.recorded%240==0)Log("[NATIVE-FG-REAL] copies=%llu reused=%llu dependencyDrops=%llu\n",status.realCopies,status.realReused,status.realDependencyDrops);
        if(receipt.paired)++status.phaseRecorded[receipt.pairedEye][receipt.index-1];
        if(receipt.paired&&NativeFgPixelsWanted())RecordNativeFgReadback(list,real,realState,input,colorState,disable,true,disableState,
            {},call,recording,receipt.count,receipt.index,&receipt);
        if(status.recorded<=8||status.recorded%240==0)Log("[NATIVE-FG-PRODUCT] recorded=%llu call=%llu source=%lld eye=%u previous=%lld fence=%llu paired=%d\n",status.recorded,call,
            static_cast<long long>(receipt.paired?receipt.currentPairSerial:receipt.current.source.sourceId),receipt.paired?receipt.pairedEye:unsigned(receipt.current.source.physicalEye),
            static_cast<long long>(receipt.paired?receipt.previousPairSerial:receipt.previous.source.sourceId),s.value,int(receipt.paired));
    }catch(...){Log("[NATIVE-FG-PRODUCT] record exception\n");}
}
void NativeFgOutputReset(ID3D12GraphicsCommandList*list) noexcept {
    std::lock_guard lock(mutex);for(auto&s:slots)if(s&&s->list.Get()==list&&!s->retired){s->retired=true;
        if(OwnsReal(*s)){s->real->lifetime.retired=true;if(s->state==State::Recorded)s->real->lifetime.invalid=true;}
        if(s->state==State::Recorded){s->state=State::Cancelled;s->valid->store(false);++status.cancelled;
            if(s->receipt.paired)++status.phaseCancelled[s->receipt.pairedEye][s->receipt.index-1];}}
}
void NativeFgOutputBeforeSubmit(ID3D12CommandQueue*queue,UINT count,ID3D12CommandList*const*lists) noexcept {
    if(!queue||!lists)return;
    if(NativeFgStereoRequested()||cvr::cet::LifecycleRequested())ObserveNativeFgResizeQueue(queue);
    std::lock_guard lock(mutex);
    for(auto&s:slots)if(s&&!s->retired)for(UINT i=0;i<count;++i)if(lists[i]==s->list.Get()){
        if(s->state==State::Recorded){
            // Sharing is admitted only after the producer recording was reset.
            // An earlier successful Signal on the SAME queue orders the real
            // copy before this submission; no extra wait is needed. A different
            // queue requires already completed producer work, otherwise discard
            // this capture but still fence its generated copy for safe retirement.
            s->realAdmission=OwnsReal(*s)||s->real->lifetime.CanConsume(s->real->Ready(),s->real->queue.Get()==queue);
            if(!s->realAdmission)++status.realDependencyDrops;
            s->queue=queue;s->state=State::Prepared;
        }
        else if(s->state!=State::Cancelled&&s->state!=State::Unknown)Unknown(*s);break;
    }
}
void NativeFgOutputSubmitted(ID3D12CommandQueue*queue) noexcept {
    std::lock_guard lock(mutex);for(auto&s:slots)if(s&&s->state==State::Prepared&&s->queue.Get()==queue){
        if(SUCCEEDED(queue->Signal(s->fence.Get(),s->value))){s->state=State::Submitted;
            if(OwnsReal(*s)){s->real->queue=queue;s->real->lifetime.submitted=true;}
        }else Unknown(*s);}
}
std::vector<std::shared_ptr<const NativeFgPacket>> CollectNativeFgOutputs(){
    std::vector<std::shared_ptr<const NativeFgPacket>> out;std::lock_guard lock(mutex);Poll();
    for(auto&s:slots)if(s&&s->state==State::Ready&&s->valid->load(std::memory_order_acquire)){
        auto p=s->packet.lock();if(!p){auto fresh=std::make_shared<NativeFgPacket>();fresh->receipt=s->receipt;fresh->color=s->color;fresh->real=s->real->color;fresh->fence=s->fence;fresh->interpolationAllowed=s->interpolationAllowed;
            fresh->value=s->value;fresh->call=s->call;fresh->lifetime=s;fresh->valid=s->valid;s->packet=fresh;p=std::move(fresh);}
        out.push_back(std::move(p));
    }return out;
}
NativeFgOutputStatus ReadNativeFgOutputStatus(){std::lock_guard lock(mutex);Poll();return status;}
void ObserveNativeFgResizeQueue(ID3D12CommandQueue* queue) noexcept {
    if(!queue)return;
    try{
        std::lock_guard lock(resizeMutex);
        for(const auto& q:resizeQueues)if(q.queue.Get()==queue)return;
        if(resizeQueues.size()>=64){resizeQueueFailure=true;return;}
        ResizeQueue q;q.queue=queue;ComPtr<ID3D12Device> device;
        if(FAILED(queue->GetDevice(IID_PPV_ARGS(&device)))||FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&q.fence)))){resizeQueueFailure=true;return;}
        resizeQueues.push_back(std::move(q));
    }catch(...){std::lock_guard lock(resizeMutex);resizeQueueFailure=true;}
}
cvr::gpu::QueueDrainResult DrainNativeFgResizeQueuesChecked(uint32_t timeoutMs) noexcept {
    using Result=cvr::gpu::QueueDrainResult;
    try{
        std::lock_guard drain(resizeDrainMutex);
        std::vector<ResizeQueue> pending;
        {std::lock_guard lock(resizeMutex);
            CyberpunkVR_ResizeDrainQueueCount.store(uint32_t(resizeQueues.size()));
            CyberpunkVR_ResizeDrainRegistryFailed.store(resizeQueueFailure);
            if(resizeQueueFailure)return Result::RegistryFailed;
            if(resizeQueues.empty())return Result::NoQueuedWork;
            for(auto& q:resizeQueues){if(q.value==UINT64_MAX)return Result::Exhausted;++q.value;pending.push_back(q);}}
        const auto start=GetTickCount64();
        // Signal every queue before waiting; do not hold a recorder/registry lock.
        for(auto& q:pending)if(FAILED(q.queue->Signal(q.fence.Get(),q.value)))return Result::SignalFailed;
        for(auto& q:pending)for(;;){const auto completed=q.fence->GetCompletedValue();
            if(completed==UINT64_MAX)return Result::DeviceRemoved;if(completed>=q.value)break;
            if(GetTickCount64()-start>=timeoutMs){Log("[NATIVE-FG-RESIZE] GPU drain timeout queues=%zu elapsed=%llu\n",pending.size(),GetTickCount64()-start);return Result::TimedOut;}Sleep(1);}
        Log("[NATIVE-FG-RESIZE] GPU drain complete queues=%zu elapsed=%llu\n",pending.size(),GetTickCount64()-start);return Result::Complete;
    }catch(...){return Result::Failed;}
}
bool DrainNativeFgResizeQueues(uint32_t timeoutMs) noexcept {
    return DrainNativeFgResizeQueuesChecked(timeoutMs)==cvr::gpu::QueueDrainResult::Complete;
}
}
