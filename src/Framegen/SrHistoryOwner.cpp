#include "Framegen/SrHistoryOwner.hpp"
#include "Framegen/SlEvaluationResult.hpp"
#include "Render/SrTemporalMotionPass.hpp"
#include "SrHistoryNativeParams.hpp"
#include "NativeFgParameters.hpp"
#include "NativeFgPairedParameters.hpp"
#include "Framegen/NativeFgOutput.hpp"
#include "Framegen/NativeFgObservation.hpp"
#include "Framegen/NativeFgPairedSource.hpp"
#include "Framegen/CpuStageTiming.hpp"
#include "Render/ResourceAllocation.hpp"
#include "SrPixelEvidence.hpp"
#include <mutex>
#include <algorithm>
#include <vector>
#include <optional>
#include <cstring>
#include <new>
#include <atomic>

extern void Log(const char*,...);

namespace cvr::framegen {
namespace {
using Microsoft::WRL::ComPtr;
bool ValidSource(const SrSourceStamp&s){return s.poseKnown&&s.nativeFrameKnown&&s.engineView==1&&bool(s.source)&&s.poseId&&s.writeId&&s.origin&&s.captureClockGeneration&&
    s.locatedPoseTimeNs==s.source.requestedPoseTimeNs&&s.appliedEye.MatchesWrittenFingerprint(s.selectedFingerprint);}
bool SameSourceScope(const SrSourceStamp&a,const SrSourceStamp&b){return a.source.epochs==b.source.epochs&&a.source.requestRevision==b.source.requestRevision&&
    a.source.latchRevision==b.source.latchRevision&&a.source.clockGeneration==b.source.clockGeneration&&a.captureClockGeneration==b.captureClockGeneration&&a.origin==b.origin;}
uintptr_t ComIdentity(IUnknown* p){ComPtr<IUnknown> identity;return p&&SUCCEEDED(p->QueryInterface(IID_PPV_ARGS(&identity)))?reinterpret_cast<uintptr_t>(identity.Get()):0;}
void TraceNativeMismatch(const char* reason,const SrSourceStamp& source,void* raw,uintptr_t actual,uintptr_t expected,unsigned role=UINT32_MAX){
    static const bool enabled=[] {char flag[8]{};return GetEnvironmentVariableA("CPVR_NATIVE_FG_OUTPUT_DIAG",flag,sizeof(flag))==1&&flag[0]=='1';}();
    static std::atomic<unsigned> count{};
    if(!enabled||source.source.sourceId<8||count.fetch_add(1)>=24)return;
    Log("[SR-NATIVE-MISMATCH] reason=%s source=%lld eye=%u role=%u raw=%p actual=%p expected=%p\n",
        reason,static_cast<long long>(source.source.sourceId),unsigned(source.source.physicalEye),role,raw,reinterpret_cast<void*>(actual),reinterpret_cast<void*>(expected));
}
void Transition(ID3D12GraphicsCommandList*l,ID3D12Resource*r,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){if(from==to)return;
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};l->ResourceBarrier(1,&b);}
unsigned Role(uint32_t type){return type==0?0:type==1?1:type==3?2:type==4?3:4;}
DXGI_FORMAT SnapshotFormat(unsigned role,DXGI_FORMAT f){if(role==0)return f==DXGI_FORMAT_R32_TYPELESS||f==DXGI_FORMAT_D32_FLOAT||f==DXGI_FORMAT_R32_FLOAT?DXGI_FORMAT_R32_FLOAT:DXGI_FORMAT_UNKNOWN;
    if(role==1)return f==DXGI_FORMAT_R16G16_FLOAT||f==DXGI_FORMAT_R32G32_FLOAT?f:DXGI_FORMAT_UNKNOWN;
    return f==DXGI_FORMAT_R11G11B10_FLOAT||f==DXGI_FORMAT_R16G16B16A16_FLOAT||f==DXGI_FORMAT_R32G32B32A32_FLOAT?f:DXGI_FORMAT_UNKNOWN;}
bool TextureDescription(const D3D12_RESOURCE_DESC&d){return d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&d.Width&&d.Width<=16384&&d.Height&&d.Height<=16384&&d.DepthOrArraySize==1&&d.MipLevels==1&&d.SampleDesc.Count==1;}
bool KnownCopyState(uint32_t state){constexpr uint32_t read=0x1u|0x2u|0x20u|0x40u|0x80u|0x200u|0x800u|0x2000u;
    if((state&~read)==0)return true;return state==0x4u||state==0x8u||state==0x10u||state==0x100u||state==0x400u||state==0x1000u;}
struct EvaluationScope {SrHistoryOwner* owner{};uint64_t id{};SrSlEvaluationObservation observation;ComPtr<ID3D12GraphicsCommandList> nativeList;uintptr_t listIdentity{};uint64_t listGeneration{};};
struct EvaluationScopes {
    std::array<EvaluationScope,8> entries{};size_t count{};bool overflow{};
    size_t size()const noexcept{return count;}bool empty()const noexcept{return !count;}
    EvaluationScope& back()noexcept{return entries[count-1];}
    void push_back(EvaluationScope&&scope)noexcept{entries[count++]=std::move(scope);}
    void pop_back()noexcept{entries[--count]={};if(count<entries.size())overflow=false;}
};
thread_local EvaluationScopes evaluationScopes;
}
struct SrHistoryOwner::Impl {
    explicit Impl(SrNativeResolver r):resolver(std::move(r)){}
    using MicrosoftPtr=Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>;
    struct Snapshot;
    struct Pair {
        SrNativeApiContext context;NVSDK_NGX_Handle* original{};NVSDK_NGX_Handle* companion{};
        uint64_t key{};uint32_t resetMask=3,inFlight{};bool originalReleased{},companionReleased{},retiring{},releaseUncertain{};
        int createFlags{};bool createFlagsKnown{};
        // Reference pair+14 starts at zero (26C190). A failed optional Get
        // preserves the last successfully supplied value (27019F..2701BF).
        float lastSuppliedDelta{};
        std::array<std::shared_ptr<Snapshot>,3> history{};
        std::array<MainEyeSourceReceipt,2> fgPrevious{};
        std::shared_ptr<const Frame> fgPairedPrevious;
        uint64_t fgPairedWorld{},fgPairedEpoch=1;unsigned fgPairedMain=2;
        ComPtr<ID3D12CommandAllocator> fgInitAllocator;
        ComPtr<ID3D12GraphicsCommandList> fgInitList;
        ComPtr<ID3D12CommandQueue> fgInitQueue;
        ComPtr<ID3D12Fence> fgInitFence;
    };
    enum class WorkState {Free,Recorded,Submitting,Submitted,Unknown};
    struct Image {ComPtr<ID3D12Resource> resource;DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;uint32_t width{},height{};
        D3D12_RESOURCE_STATES state=D3D12_RESOURCE_STATE_COPY_DEST;};
    struct Snapshot {
        SrConstantsObservation observation;uint64_t serial{},historyEpoch{},resourceEpoch{};
        std::array<Image,3> images;std::array<ComPtr<ID3D12Resource>,3> originalPins;
        std::array<ComPtr<ID3D12Resource>,2> taggedCopies;
        std::array<uintptr_t,2> taggedCopyIdentity{};
        std::array<uintptr_t,4> originalIdentity{};std::array<D3D12_RESOURCE_DESC,4> originalDesc{};
        MicrosoftPtr list;uintptr_t listIdentity{};uint64_t listGeneration{};
        ComPtr<ID3D12Fence> fence;ComPtr<ID3D12CommandQueue> queue;uintptr_t queueIdentity{};uint64_t fenceValue{};
        WorkState producer=WorkState::Free;uint32_t readyMask{},pendingMask{},pendingCalls{};bool valid{},sealed{},announced{},nativeKnown{};
        sr::NgxCameraInputs native;
    };
    struct TagOperation {uint64_t id{};uint32_t viewport{},mask{},revokeMask{};std::shared_ptr<Snapshot> snapshot;};
    struct ListEpoch {MicrosoftPtr list;uintptr_t identity{};uint64_t generation{};uint32_t activeScopes{};};
    struct Work {
        WorkState state=WorkState::Free;MicrosoftPtr list;uint64_t listGeneration{};
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;uint64_t value{};
        ComPtr<ID3D12CommandQueue> queue;
        std::shared_ptr<Pair> pair;render::SrTemporalTicket ticket;
        std::array<std::shared_ptr<Snapshot>,3> reads{};
        std::array<Image,4> outputs{};std::shared_ptr<uint64_t> outputLease;
        std::shared_ptr<Snapshot> guideCurrent;
        std::array<sr::CameraSample,3> guideHistory{};
        SrSourceStamp guidePrevious;
        bool guideKnown{},guideReset{};
        std::optional<SrFrameGenerationGuide> fgInputs;
        std::shared_ptr<const NativeFgPairedSource> fgPairedInputs;
        std::unique_ptr<sr_evidence::Capture> evidence;
    };
    sr_evidence::Configuration evidenceConfig;
    mutable std::mutex mutex;
    std::recursive_mutex nativeMutex; // serializes our native calls; NOT metadata
    SrNativeResolver resolver;
    SrFrameGenerationSourceReader fgSource;
    SrFrameGenerationReporter fgReporter;
    SrHistoryStatus status;
    sr::Parameters parameters;
    SrOptionalOverridePolicy optional;
    std::array<std::shared_ptr<Pair>,64> pairs{};
    std::array<Work,PassCapacity+8> work{};
    render::SrTemporalMotionPass pass;
    uint64_t nextPair=1,nextScope=1;uint32_t activeNative{},activeScopes{};bool retirementRunning{},retiredSuccessfully{};
    uint64_t nextSnapshot=1,nextTag=1,nextListGeneration=1;std::array<std::shared_ptr<Snapshot>,SnapshotCapacity> snapshots{};
    std::array<TagOperation,4> tags{};std::array<ListEpoch,64> listEpochs{};
    std::array<std::optional<SrConstantsObservation>,64> constants{};size_t constantsCursor{};
    std::optional<SrSourceStamp> currentScope;
    std::array<uintptr_t,4> resourceIdentity{};std::array<D3D12_RESOURCE_DESC,4> resourceDesc{};
    struct NativeUse {Impl&self;explicit NativeUse(Impl&s):self(s){std::lock_guard l(self.mutex);++self.activeNative;}
        ~NativeUse(){std::lock_guard l(self.mutex);--self.activeNative;}};
    struct SubmitClaim {
        uint64_t id{};ComPtr<ID3D12CommandQueue> queue;uintptr_t queueIdentity{};
        std::array<std::shared_ptr<Snapshot>,SnapshotCapacity> snapshots{};
        std::array<uint64_t,PassCapacity+8> workValues{};
    };
    std::array<SubmitClaim,32> claims{};uint64_t nextClaim=1;
    template<class T>Microsoft::WRL::ComPtr<T> Resolve(IUnknown* value){Microsoft::WRL::ComPtr<T> out;
        if(value&&resolver&&FAILED(resolver(value,__uuidof(T),reinterpret_cast<void**>(out.GetAddressOf()))))out.Reset();return out;}
    bool Matches(const Pair&p,const SrNativeApiContext&c,const NVSDK_NGX_Handle*h)const{return p.original==h&&p.context.coreModule==c.coreModule&&
        p.context.moduleGeneration==c.moduleGeneration&&p.context.lifecycleGeneration==c.lifecycleGeneration&&p.context.createOrdinal==c.createOrdinal;}
    std::shared_ptr<Pair> Find(const SrNativeApiContext&c,const NVSDK_NGX_Handle*h){for(auto&p:pairs)if(p&&Matches(*p,c,h))return p;return {};}
    void ResetHistory(){++status.historyEpoch;++status.resetEvents;for(auto&p:pairs)if(p){p->resetMask=3;p->history={};p->fgPrevious={};p->fgPairedPrevious.reset();++p->fgPairedEpoch;}status.resetEyeMask=3;}
    void SubmissionResult(HRESULT hr){if(FAILED(hr)){status.lastSubmissionResult=hr;++status.submissionFailures;status.quiescing=true;status.stage=SrHistoryStage::SubmissionFailed;}
        else if(!status.quiescing)status.lastSubmissionResult=hr;}
    void RetainUncertainRecorded(HRESULT reason){
        for(auto&w:work)if(w.state==WorkState::Recorded)w.state=WorkState::Unknown;
        for(auto&s:snapshots)if(s&&s->producer==WorkState::Recorded)s->producer=WorkState::Unknown;
        SubmissionResult(reason);
    }
    void Refresh(){status.livePairs=0;status.livePasses=0;for(auto&p:pairs)if(p&&p->companion&&!p->companionReleased)++status.livePairs;
        for(auto&w:work)if(w.state!=WorkState::Free)++status.livePasses;status.snapshotCells=0;status.pendingCaptures=0;status.pendingProducers=0;
        for(auto&s:snapshots)if(s){++status.snapshotCells;if(!ProducerFinished(*s))++status.pendingProducers;}for(auto&t:tags)if(t.id)++status.pendingCaptures;}
    void DropWork(Work&w){if(w.ticket)pass.CancelNeverExecuted(w.ticket);if(w.pair&&w.pair->inFlight)--w.pair->inFlight;
        if(w.pair&&w.pair->context.featureId==11){w.pair->resetMask=3;w.pair->fgPrevious={};w.pair->fgPairedPrevious.reset();}w.fgInputs.reset();w.fgPairedInputs.reset();
        w.ticket={};w.pair.reset();w.reads={};w.outputs={};w.outputLease.reset();w.evidence.reset();w.list.Reset();w.queue.Reset();w.guideCurrent.reset();w.guideKnown=false;w.state=WorkState::Free;}
    void CollectLocked(){pass.Collect();for(auto&w:work)if(w.state==WorkState::Submitted&&w.fence){const auto completed=w.fence->GetCompletedValue();
        if(completed!=UINT64_MAX&&completed>=w.value){if(w.evidence){w.evidence->submissionResult=status.lastSubmissionResult;sr_evidence::Save(*w.evidence);w.evidence.reset();}if(w.pair&&w.pair->inFlight)--w.pair->inFlight;w.ticket={};w.pair.reset();w.reads={};w.fgInputs.reset();w.fgPairedInputs.reset();w.list.Reset();w.state=WorkState::Free;}}
        for(auto&s:snapshots)if(s&&s->producer==WorkState::Submitted&&s->fence){const auto done=s->fence->GetCompletedValue();if(done!=UINT64_MAX&&done>=s->fenceValue){s->originalPins={};if(s->sealed)s->list.Reset();}}
        Refresh();}
    // Caller owns nativeMutex; native release must never run under mutex.
    void CollectRetiredFg(){
        {std::lock_guard lock(mutex);CollectLocked();}
        for(size_t i=0;i<pairs.size();++i){std::shared_ptr<Pair> p;
            {std::lock_guard lock(mutex);p=pairs[i];
                if(!p||p->context.featureId!=11||!p->originalReleased||!p->retiring||
                   p->inFlight||p->releaseUncertain||p->companionReleased)continue;}
            NVSDK_NGX_Result result;
            try{result=p->context.originals.release(p->companion);}catch(...){
                std::lock_guard lock(mutex);p->releaseUncertain=true;throw;}
            {std::lock_guard lock(mutex);status.lastCompanionResult=result;
                if(result==NVSDK_NGX_Result_Success){p->companionReleased=true;if(pairs[i]==p)pairs[i].reset();}
                else p->releaseUncertain=true;Refresh();}
            Log("[NATIVE-FG-RETIRE] deferred companion create=%llu result=%08X\n",p->context.createOrdinal,unsigned(result));
        }
    }
    bool ProducerFinished(const Snapshot&s)const{return s.producer==WorkState::Free||(s.producer==WorkState::Submitted&&s.fence&&s.fence->GetCompletedValue()!=UINT64_MAX&&s.fence->GetCompletedValue()>=s.fenceValue);}
    ListEpoch* List(ID3D12GraphicsCommandList* list){const auto id=ComIdentity(list);if(!id)return nullptr;
        for(auto& l:listEpochs)if(l.identity==id)return &l;
        const auto inactive=[&](const ListEpoch&entry){if(entry.activeScopes)return false;
            for(const auto&w:work)if(w.state!=WorkState::Free&&ComIdentity(w.list.Get())==entry.identity)return false;
            for(const auto&s:snapshots)if(s&&s->listIdentity==entry.identity&&(!ProducerFinished(*s)||s->pendingCalls||(s->valid&&!s->sealed)))return false;return true;};
        for(auto& l:listEpochs)if(!l.identity||inactive(l)){if(nextListGeneration==UINT64_MAX)return nullptr;l={};l.list=list;l.identity=id;l.generation=nextListGeneration++;return &l;}return nullptr;}
    std::shared_ptr<Snapshot> SnapshotFor(const SrConstantsObservation&o,ID3D12GraphicsCommandList*list){
        auto* epoch=List(list);if(!epoch)return {};
        for(auto&s:snapshots)if(s&&s->valid&&s->observation.source==o.source&&s->observation.viewport==o.viewport&&s->observation.slToken==o.slToken&&s->historyEpoch==status.historyEpoch){
            if(s->sealed||s->listIdentity!=epoch->identity||s->listGeneration!=epoch->generation)return {};return s;}
        CollectLocked();for(auto&slot:snapshots)if(!slot||(slot.use_count()==1&&!slot->pendingCalls&&ProducerFinished(*slot))){
            if(!slot)slot=std::make_shared<Snapshot>();auto s=slot;
            const auto images=s->images;const auto fence=s->fence;const auto value=s->fenceValue;
            *s=Snapshot{};s->images=images;s->fence=fence;s->fenceValue=value;s->observation=o;s->serial=nextSnapshot++;s->historyEpoch=status.historyEpoch;
            s->resourceEpoch=status.resourceEpoch;s->list=list;s->listIdentity=epoch->identity;s->listGeneration=epoch->generation;s->valid=true;return s;
        }return {};
    }
    bool CopyTag(Snapshot&s,unsigned role,const SrSnapshotTag&tag,ID3D12Resource*resource){
        if(!KnownCopyState(tag.state))return false;
        const auto desc=resource->GetDesc();if(!TextureDescription(desc)||tag.x||tag.y||
            (tag.width&&tag.width!=desc.Width)||(tag.height&&tag.height!=desc.Height))return false;
        s.originalIdentity[role]=ComIdentity(resource);s.originalDesc[role]=desc;if(!s.originalIdentity[role])return false;
        if(role==3)return true;const auto format=SnapshotFormat(role,desc.Format);if(format==DXGI_FORMAT_UNKNOWN)return false;
        auto& image=s.images[role];ComPtr<ID3D12Device> device;if(FAILED(s.list->GetDevice(IID_PPV_ARGS(&device))))return false;
        ComPtr<ID3D12Device> sourceDevice;if(FAILED(resource->GetDevice(IID_PPV_ARGS(&sourceDevice)))||ComIdentity(device.Get())!=ComIdentity(sourceDevice.Get()))return false;
        const bool reused=image.resource&&image.format==format&&image.width==desc.Width&&image.height==desc.Height;
        if(!reused){D3D12_RESOURCE_DESC target=desc;target.Format=format;target.Flags=D3D12_RESOURCE_FLAG_NONE;target.Alignment=0;
            D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource> fresh;
            if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&target,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&fresh))))return false;
            image={fresh,format,uint32_t(desc.Width),desc.Height};
        }
        if(!s.fence&&FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&s.fence))))return false;
        if(s.producer==WorkState::Free){if(s.fenceValue==UINT64_MAX)return false;++s.fenceValue;s.producer=WorkState::Recorded;}
        if(reused)Transition(s.list.Get(),image.resource.Get(),image.state,D3D12_RESOURCE_STATE_COPY_DEST);
        const auto state=D3D12_RESOURCE_STATES(tag.state);Transition(s.list.Get(),resource,state,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=resource;to.pResource=image.resource.Get();from.Type=to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        s.list->CopyTextureRegion(&to,0,0,0,&from,nullptr);Transition(s.list.Get(),resource,D3D12_RESOURCE_STATE_COPY_SOURCE,state);
        Transition(s.list.Get(),image.resource.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        image.state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;s.originalPins[role]=resource;return true;
    }
    void CommitSnapshot(Snapshot&s){if(s.announced||!s.valid||s.pendingCalls||(s.readyMask&15)!=15)return;
        bool changed=status.resourceEpoch==0;
        for(unsigned i=0;i<4;++i){const auto&a=resourceDesc[i];const auto&b=s.originalDesc[i];changed|=a.Width!=b.Width||a.Height!=b.Height||a.Format!=b.Format;}
        if(changed){ResetHistory();++status.resourceEpoch;resourceIdentity=s.originalIdentity;resourceDesc=s.originalDesc;}
        s.historyEpoch=status.historyEpoch;s.resourceEpoch=status.resourceEpoch;s.announced=true;++status.capturedSources;
    }
    bool ReadyPrevious(const Snapshot&s)const{return s.valid&&s.announced&&s.sealed&&s.nativeKnown&&s.producer==WorkState::Submitted&&s.queue&&s.fence&&s.fenceValue&&s.fence->GetCompletedValue()!=UINT64_MAX;}
    std::shared_ptr<Snapshot> Current(const EvaluationScope&scope){for(auto&s:snapshots)if(s&&s->valid&&s->announced&&!s->pendingCalls&&
        s->historyEpoch==status.historyEpoch&&s->observation.source==scope.observation.source&&s->observation.viewport==scope.observation.viewport&&
        s->observation.slToken==scope.observation.slToken&&s->listIdentity==scope.listIdentity&&s->listGeneration==scope.listGeneration)return s;return {};}
    sr::CameraSample Camera(const Snapshot&s,const Pair&p)const{sr::CameraSample out;const auto&stamp=s.observation.source;auto&k=out.key;
        k.source=stamp.source;k.resourceEpoch=s.resourceEpoch;k.captureClockGeneration=stamp.captureClockGeneration;k.historyEpoch=s.historyEpoch;k.origin=stamp.origin;
        k.poseId=stamp.poseId;k.writeId=stamp.writeId;k.featurePairKey=p.key;k.parametersKey=parameters.key;k.locatedPoseTimeNs=stamp.locatedPoseTimeNs;
        k.nativeFrame=stamp.nativeFrame;k.slToken=s.observation.slToken;k.viewport=s.observation.viewport;k.engineView=stamp.engineView;
        k.nativeFrameKnown=stamp.nativeFrameKnown;k.slTokenKnown=s.observation.tokenKnown;k.viewportKnown=s.observation.viewportKnown;
        out.original=s.observation.constants;out.ngx=s.native;out.appliedEye={k,stamp.selectedFingerprint,stamp.appliedEye};return out;}
    bool Outputs(Work&w,const Snapshot&current){ComPtr<ID3D12Device> device;if(FAILED(current.list->GetDevice(IID_PPV_ARGS(&device))))return false;
        for(unsigned i=0;i<4;++i){const auto&source=current.images[i<2?1:2];auto&image=w.outputs[i];
            if(!image.resource||image.format!=source.format||image.width!=source.width||image.height!=source.height){
                auto desc=source.resource->GetDesc();desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;desc.Alignment=0;D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource> resource;
                if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&resource))))return false;
                image={resource,source.format,source.width,source.height};
            }
        }return true;
    }
    void InitializeOutputs(Work&w,const Snapshot&current){for(unsigned i=0;i<4;++i){const auto&source=current.images[i<2?1:2];auto&image=w.outputs[i];
        Transition(w.list.Get(),source.resource.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(w.list.Get(),image.resource.Get(),image.state,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=source.resource.Get();b.pResource=image.resource.Get();a.Type=b.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        w.list->CopyTextureRegion(&b,0,0,0,&a,nullptr);
        Transition(w.list.Get(),source.resource.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        image.state=D3D12_RESOURCE_STATE_COPY_DEST;
    }}
    render::SrTemporalBinding Binding(const Image&image,std::shared_ptr<void>owner,D3D12_RESOURCE_STATES state)const{return {image.resource,image.format,image.width,image.height,state,std::move(owner)};}
    bool Record(Work&w,const std::array<std::shared_ptr<Snapshot>,3>&sources,const sr::AdapterResult&built,ID3D12Resource*nativeMotion,ID3D12Resource*nativeDepth){
        const auto&old=sources[0];const auto&previous=sources[1];const auto&current=sources[2];if(!Outputs(w,*current))return false;
        w.reads=sources;InitializeOutputs(w,*current);render::SrTemporalMotionRequest request;
        std::memcpy(request.constants.words.data(),&built.constants,sizeof(built.constants));
        request.inputs[0]=Binding(current->images[1],current,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        request.inputs[1]=Binding(previous->images[1],previous,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        const std::array<std::shared_ptr<Snapshot>,3> ordered{current,previous,old};
        for(unsigned i=0;i<3;++i){request.inputs[i+2]=Binding(ordered[i]->images[0],ordered[i],D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);request.inputs[i+5]=Binding(ordered[i]->images[2],ordered[i],D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);}
        for(unsigned i=0;i<4;++i)request.outputs[i]=Binding(w.outputs[i],w.outputLease,w.outputs[i].state);
        request.completion={w.fence.Get(),w.value};request.allowClippedColorStores=true;w.reads=sources;w.ticket=pass.Record(w.list.Get(),request);
        if(!w.ticket)return false;for(auto&image:w.outputs)image.state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        if(!evidenceConfig.directory.empty()){
            const std::array<ID3D12Resource*,9> images{request.inputs[0].resource.Get(),request.inputs[1].resource.Get(),
                request.inputs[2].resource.Get(),request.inputs[3].resource.Get(),request.inputs[4].resource.Get(),
                w.outputs[0].resource.Get(),request.inputs[5].resource.Get(),nativeMotion,nativeDepth};
            w.evidence=sr_evidence::Record(evidenceConfig,w.list.Get(),images,{old->observation,previous->observation,current->observation},
                {old->native,previous->native,current->native},built.constants,current->historyEpoch,current->resourceEpoch,parameters.key,w.value);
        }
        return true;
    }
    Work* ReserveNativeWork(ID3D12GraphicsCommandList* list,const std::shared_ptr<Pair>&pair){
        if(!list)return nullptr;CollectLocked();const bool fg=pair&&pair->context.featureId==11;
        for(auto&w:std::span(work).subspan(fg?PassCapacity:0,fg?8:PassCapacity))if(w.state==WorkState::Free&&(!w.outputLease||w.outputLease.use_count()==1)){
            Microsoft::WRL::ComPtr<ID3D12Device> device;if(FAILED(list->GetDevice(IID_PPV_ARGS(&device))))return nullptr;
            if(!w.fence&&FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&w.fence))))return nullptr;
            if(!w.outputLease){try{w.outputLease=std::make_shared<uint64_t>(0);}catch(const std::bad_alloc&){return nullptr;}}
            auto*epoch=List(list);if(!epoch||w.value==UINT64_MAX)return nullptr;++w.value;w.state=WorkState::Recorded;w.list=list;w.listGeneration=epoch->generation;w.queue.Reset();w.guideCurrent.reset();w.guideKnown=false;w.pair=pair;if(pair)++pair->inFlight;Refresh();return &w;
        }return nullptr;
    }
};
SrHistoryOwner::SrHistoryOwner(SrNativeResolver resolver):m_impl(std::make_unique<Impl>(std::move(resolver))){}
bool SrHistoryOwner::ConfigureFrameGeneration(SrFrameGenerationSourceReader reader,SrFrameGenerationReporter reporter){
    std::lock_guard nativeLock(m_impl->nativeMutex);std::lock_guard lock(m_impl->mutex);
    if(m_impl->status.quiescing||m_impl->activeNative||std::any_of(m_impl->pairs.begin(),m_impl->pairs.end(),[](const auto&p){return bool(p);}))return false;
    m_impl->fgSource=std::move(reader);m_impl->fgReporter=std::move(reporter);return true;
}
SrHistoryOwner::~SrHistoryOwner(){try{if(BeforeHostShutdown()!=SrNativeRetireResult::Retired)(void)m_impl.release();}catch(...){(void)m_impl.release();}}
void SrHistoryOwner::SetRequested(bool requested){std::lock_guard lock(m_impl->mutex);if(m_impl->retiredSuccessfully)return;
    if(m_impl->status.requested!=requested)m_impl->ResetHistory();
    m_impl->status.requested=requested&&!m_impl->status.quiescing;m_impl->status.stage=requested?SrHistoryStage::Unpaired:SrHistoryStage::Disabled;}
void SrHistoryOwner::SetConfiguration(const sr::Parameters&p,SrOptionalOverridePolicy policy){std::lock_guard lock(m_impl->mutex);
    if(m_impl->retiredSuccessfully)return;
    if(m_impl->parameters.key!=p.key||m_impl->parameters.known!=p.known||m_impl->parameters.effectiveFix!=p.effectiveFix||
       m_impl->optional.known!=policy.known||m_impl->optional.replaceColor!=policy.replaceColor||m_impl->optional.replaceSpecularMotion!=policy.replaceSpecularMotion)m_impl->ResetHistory();
    m_impl->parameters=p;m_impl->optional=policy;}
void SrHistoryOwner::ObserveConstants(const SrConstantsObservation&o){std::lock_guard lock(m_impl->mutex);
    if(!m_impl->status.requested||m_impl->status.quiescing||!o.viewportKnown||o.viewport!=0)return;
    if(o.constants.flagPresent[3]&&o.constants.flags[3]==1){m_impl->ResetHistory();for(auto&c:m_impl->constants)c.reset();}
    if(!o.resultKnown||o.originalResult!=0||!o.tokenKnown||!ValidSource(o.source))return;
    if(m_impl->currentScope&&!SameSourceScope(*m_impl->currentScope,o.source)){m_impl->ResetHistory();for(auto&c:m_impl->constants)c.reset();}
    m_impl->currentScope=o.source;m_impl->constants[m_impl->constantsCursor++%m_impl->constants.size()]=o;
}
SrTagCaptureToken SrHistoryOwner::BeginTags(ID3D12GraphicsCommandList*raw,uint32_t viewport,bool known,const SrSourceStamp&source,std::span<const SrSnapshotTag> input){
    if(!known||viewport!=0||input.size()>16)return {};uint32_t revoke=0;for(const auto&t:input)if(t.known&&!t.resource&&Role(t.type)<4)revoke|=1u<<Role(t.type);
    if(revoke){std::lock_guard lock(m_impl->mutex);for(auto&op:m_impl->tags)if(!op.id){op.id=m_impl->nextTag++;op.viewport=viewport;op.revokeMask=revoke;m_impl->Refresh();return {op.id};}return {};}
    for(const auto&tag:input)if(tag.known&&tag.resource&&Role(tag.type)<4&&!KnownCopyState(tag.state))return {};
    if(!ValidSource(source))return {};auto list=m_impl->Resolve<ID3D12GraphicsCommandList>(raw);if(!list)return {};
    std::array<ComPtr<ID3D12Resource>,16> native;for(size_t i=0;i<input.size();++i)if(input[i].known&&Role(input[i].type)<4&&input[i].resource){native[i]=m_impl->Resolve<ID3D12Resource>(input[i].resource);if(!native[i])return {};}
    std::lock_guard lock(m_impl->mutex);if(!m_impl->status.requested||m_impl->status.quiescing)return {};
    auto op=std::find_if(m_impl->tags.begin(),m_impl->tags.end(),[](const auto&x){return !x.id;});if(op==m_impl->tags.end()){m_impl->status.stage=SrHistoryStage::PoolBusy;return {};}
    const SrConstantsObservation* observation{};for(size_t n=0;n<m_impl->constants.size();++n){const auto&c=m_impl->constants[(m_impl->constantsCursor-1-n)%m_impl->constants.size()];
        if(c&&c->source==source&&c->viewport==viewport){observation=&*c;break;}}
    if(!observation)return {};auto snapshot=m_impl->SnapshotFor(*observation,list.Get());if(!snapshot)return {};
    op->id=m_impl->nextTag++;op->viewport=viewport;op->snapshot=snapshot;++snapshot->pendingCalls;
    for(size_t i=0;i<input.size();++i){const auto&tag=input[i];const auto role=Role(tag.type);if(role>=4||!tag.known)continue;
        const auto bit=1u<<role;if(!native[i]||snapshot->readyMask&bit||snapshot->pendingMask&bit||!m_impl->CopyTag(*snapshot,role,tag,native[i].Get())){snapshot->valid=false;continue;}
        op->mask|=bit;snapshot->pendingMask|=bit;
    }m_impl->Refresh();return {op->id};
}
void SrHistoryOwner::ObserveTaggedCopy(SrTagCaptureToken token,ID3D12GraphicsCommandList* raw,
    ID3D12Resource* destination,ID3D12Resource* source){
    if(!token||!raw||!destination||!source)return;
    auto list=m_impl->Resolve<ID3D12GraphicsCommandList>(raw);
    auto to=m_impl->Resolve<ID3D12Resource>(destination),from=m_impl->Resolve<ID3D12Resource>(source);
    if(!list||!to||!from)return;
    const auto listId=ComIdentity(list.Get()),toId=ComIdentity(to.Get()),fromId=ComIdentity(from.Get());
    if(!listId||!toId||!fromId)return;
    const auto td=to->GetDesc(),fd=from->GetDesc();
    std::lock_guard lock(m_impl->mutex);
    const auto op=std::find_if(m_impl->tags.begin(),m_impl->tags.end(),[&](const auto& x){return x.id==token.value;});
    if(op==m_impl->tags.end()||!op->snapshot)return;
    auto& s=*op->snapshot;auto* epoch=m_impl->List(list.Get());
    if(!s.valid||s.sealed||!epoch||s.listIdentity!=listId||s.listGeneration!=epoch->generation)return;
    // Rewriting an observed destination revokes its previous ancestry first.
    for(unsigned i=0;i<2;++i)if(s.taggedCopyIdentity[i]==toId){s.taggedCopies[i].Reset();s.taggedCopyIdentity[i]=0;}
    if(toId==fromId||!TextureDescription(td)||!TextureDescription(fd)||td.Width!=fd.Width||td.Height!=fd.Height||
        td.Format!=fd.Format||td.DepthOrArraySize!=fd.DepthOrArraySize||td.MipLevels!=fd.MipLevels)return;
    ComPtr<ID3D12Device> device,sourceDevice,destinationDevice;
    if(FAILED(list->GetDevice(IID_PPV_ARGS(&device)))||FAILED(from->GetDevice(IID_PPV_ARGS(&sourceDevice)))||
       FAILED(to->GetDevice(IID_PPV_ARGS(&destinationDevice)))||ComIdentity(device.Get())!=ComIdentity(sourceDevice.Get())||ComIdentity(device.Get())!=ComIdentity(destinationDevice.Get()))return;
    for(unsigned i=0;i<2;++i)if((op->mask&(1u<<i))&&s.originalIdentity[i]==fromId){
        s.taggedCopies[i]=to;s.taggedCopyIdentity[i]=toId;
        // Same source, same SL call, same list generation, actual full copy.
        TraceNativeMismatch("observed-tag-copy",s.observation.source,source,toId,fromId,i);
    }
}
void SrHistoryOwner::EndTags(SrTagCaptureToken token,uint32_t result,bool known){std::lock_guard lock(m_impl->mutex);
    for(auto&op:m_impl->tags)if(op.id==token.value&&op.id){if(op.revokeMask){if(known&&result==0){m_impl->ResetHistory();for(auto&s:m_impl->snapshots)if(s)s->valid=false;for(auto&c:m_impl->constants)c.reset();}}
        else if(op.snapshot){auto&s=*op.snapshot;s.pendingMask&=~op.mask;if(s.pendingCalls)--s.pendingCalls;if(known&&result==0)s.readyMask|=op.mask;else{s.valid=false;m_impl->ResetHistory();}m_impl->CommitSnapshot(s);}
        op={};m_impl->Refresh();break;}
}
SrSlEvaluationToken SrHistoryOwner::BeginSlEvaluation(const SrSlEvaluationObservation&o){if(evaluationScopes.size()>=8){evaluationScopes.overflow=true;
    std::lock_guard lock(m_impl->mutex);m_impl->ResetHistory();return {};}
    EvaluationScope scope;scope.owner=this;scope.observation=o;
    scope.nativeList=m_impl->Resolve<ID3D12GraphicsCommandList>(o.commandList);scope.listIdentity=ComIdentity(scope.nativeList.Get());
    {std::lock_guard lock(m_impl->mutex);scope.id=m_impl->nextScope++;if(auto*epoch=m_impl->List(scope.nativeList.Get())){scope.listGeneration=epoch->generation;++epoch->activeScopes;}++m_impl->activeScopes;}
    const auto id=scope.id;evaluationScopes.push_back(std::move(scope));return {id};}
void SrHistoryOwner::EndSlEvaluation(SrSlEvaluationToken token,uint32_t result,bool known){if(evaluationScopes.empty()||evaluationScopes.back().owner!=this||evaluationScopes.back().id!=token.value)return;
    const auto identity=evaluationScopes.back().listIdentity,generation=evaluationScopes.back().listGeneration;evaluationScopes.pop_back();std::lock_guard lock(m_impl->mutex);
    for(auto&entry:m_impl->listEpochs)if(entry.identity==identity&&entry.generation==generation&&entry.activeScopes){--entry.activeScopes;break;}
    if(m_impl->activeScopes)--m_impl->activeScopes;if(!known||!SlEvaluationCompleted(result))m_impl->ResetHistory();}
SrSubmissionToken SrHistoryOwner::BeforeSubmit(ID3D12CommandQueue*raw,UINT count,ID3D12CommandList*const*lists){
    {std::lock_guard lock(m_impl->mutex);if(m_impl->retiredSuccessfully)return {};}
    auto queue=m_impl->Resolve<ID3D12CommandQueue>(raw);if(!lists)return {};const auto queueId=ComIdentity(queue.Get());
    std::lock_guard lock(m_impl->mutex);if(m_impl->retiredSuccessfully)return {};
    auto claim=std::find_if(m_impl->claims.begin(),m_impl->claims.end(),[](const auto&x){return !x.id;});
    if(claim==m_impl->claims.end()||!queue){
        // The host still executes. With no complete claim we cannot later
        // prove which generation ran; never let Reset cancel/reuse this work.
        m_impl->RetainUncertainRecorded(!queue?E_NOINTERFACE:E_OUTOFMEMORY);return {};}
    claim->id=m_impl->nextClaim++;claim->queue=queue;claim->queueIdentity=queueId;
    for(UINT i=0;i<count;++i){auto list=m_impl->Resolve<ID3D12GraphicsCommandList>(lists[i]);if(!list){m_impl->RetainUncertainRecorded(E_NOINTERFACE);continue;}
        auto*epoch=m_impl->List(list.Get());if(!epoch)continue;
        for(unsigned n=0;n<m_impl->snapshots.size();++n){auto&s=m_impl->snapshots[n];if(s&&s->producer==Impl::WorkState::Recorded&&s->listIdentity==epoch->identity&&s->listGeneration==epoch->generation){
            claim->snapshots[n]=s;s->producer=Impl::WorkState::Submitting;}}
        for(unsigned n=0;n<m_impl->work.size();++n){auto&w=m_impl->work[n];if(w.state!=Impl::WorkState::Recorded||ComIdentity(w.list.Get())!=epoch->identity||w.listGeneration!=epoch->generation)continue;
            claim->workValues[n]=w.value;w.state=Impl::WorkState::Submitting;
            if(w.fgInputs&&ComIdentity(w.fgInputs->queue.Get())!=queueId){const auto result=queue->Wait(w.fgInputs->fence.Get(),w.fgInputs->fenceValue);
                m_impl->SubmissionResult(result);if(SUCCEEDED(result))++m_impl->status.crossQueueWaits;}
            if(w.fgPairedInputs){const auto& f=w.fgPairedInputs->frame;
                const auto wait=[&](ID3D12Fence* fence,uint64_t value){const auto r=queue->Wait(fence,value);m_impl->SubmissionResult(r);if(SUCCEEDED(r))++m_impl->status.crossQueueWaits;};
                wait(f.colorCompletion.fence.Get(),f.colorCompletion.fenceValue);
                for(const auto& g:f.inputs)if(g)wait(g->readyFence.Get(),g->fenceValue);
            }
            for(const auto&s:w.reads)if(s&&s->producer==Impl::WorkState::Submitted&&s->queueIdentity!=queueId){const auto result=queue->Wait(s->fence.Get(),s->fenceValue);
                m_impl->SubmissionResult(result);if(SUCCEEDED(result))++m_impl->status.crossQueueWaits;}
        }
    }
    m_impl->Refresh();return {claim->id};
}
void SrHistoryOwner::Submitted(SrSubmissionToken token,ID3D12CommandQueue* raw,UINT,ID3D12CommandList*const*){
    {std::lock_guard lock(m_impl->mutex);if(m_impl->retiredSuccessfully)return;}
    auto queue=m_impl->Resolve<ID3D12CommandQueue>(raw);std::lock_guard lock(m_impl->mutex);
    if(m_impl->retiredSuccessfully)return;
    auto claim=std::find_if(m_impl->claims.begin(),m_impl->claims.end(),[&](const auto&x){return token.value&&x.id==token.value;});if(claim==m_impl->claims.end())return;
    const bool identity=queue&&ComIdentity(queue.Get())==claim->queueIdentity;
    for(auto&s:claim->snapshots)if(s){const auto signal=identity?queue->Signal(s->fence.Get(),s->fenceValue):E_INVALIDARG;const bool signaled=SUCCEEDED(signal);
        s->producer=signaled?Impl::WorkState::Submitted:Impl::WorkState::Unknown;s->queue=claim->queue;s->queueIdentity=claim->queueIdentity;m_impl->SubmissionResult(signal);}
    for(unsigned n=0;n<m_impl->work.size();++n)if(claim->workValues[n]){auto&w=m_impl->work[n];
        if(w.state!=Impl::WorkState::Submitting||w.value!=claim->workValues[n]){m_impl->SubmissionResult(E_UNEXPECTED);continue;}
        const auto signal=identity?queue->Signal(w.fence.Get(),w.value):E_INVALIDARG;const bool signaled=SUCCEEDED(signal);w.state=signaled?Impl::WorkState::Submitted:Impl::WorkState::Unknown;
        if(signaled)w.queue=queue;
        if(w.ticket)m_impl->pass.SubmissionReported(w.ticket,true,signaled);m_impl->SubmissionResult(signal);}
    *claim={};m_impl->CollectLocked();
}
void SrHistoryOwner::ResetList(ID3D12GraphicsCommandList* raw){
    {std::lock_guard lock(m_impl->mutex);if(m_impl->retiredSuccessfully)return;}
    auto list=m_impl->Resolve<ID3D12GraphicsCommandList>(raw);if(!list){std::lock_guard lock(m_impl->mutex);if(m_impl->retiredSuccessfully)return;
    m_impl->ResetHistory();m_impl->RetainUncertainRecorded(E_NOINTERFACE);return;}
    std::lock_guard lock(m_impl->mutex);if(m_impl->retiredSuccessfully)return;
    for(auto&s:m_impl->snapshots)if(s&&s->listIdentity==ComIdentity(list.Get())&&s->producer==Impl::WorkState::Recorded){
        s->producer=Impl::WorkState::Free;s->valid=false;s->originalPins={};s->images={};m_impl->ResetHistory();}
    for(auto&w:m_impl->work)if(w.list==list&&w.state==Impl::WorkState::Recorded)m_impl->DropWork(w);
    if(auto* epoch=m_impl->List(list.Get())){epoch->generation=m_impl->nextListGeneration++;epoch->activeScopes=0;}m_impl->Refresh();}
void SrHistoryOwner::Collect(){std::lock_guard lock(m_impl->mutex);if(!m_impl->retiredSuccessfully)m_impl->CollectLocked();}
SrHistoryStatus SrHistoryOwner::Status()const{std::lock_guard lock(m_impl->mutex);return m_impl->status;}
std::optional<SrFrameGenerationGuide> SrHistoryOwner::AcquireFrameGenerationGuide(const SrSourceStamp& source){
    if(!ValidSource(source))return {};
    std::lock_guard lock(m_impl->mutex);m_impl->CollectLocked();
    if(!m_impl->status.requested||m_impl->status.quiescing)return {};
    for(const auto& w:m_impl->work){
        if(!w.guideKnown||!w.guideCurrent||w.guideCurrent->observation.source!=source||
           w.guideCurrent->historyEpoch!=m_impl->status.historyEpoch||w.guideCurrent->resourceEpoch!=m_impl->status.resourceEpoch||
           (w.state!=Impl::WorkState::Submitted&&w.state!=Impl::WorkState::Free)||!w.fence||!w.queue||!w.value||
           !w.outputLease||!w.outputs[0].resource||w.fence->GetCompletedValue()==UINT64_MAX)continue;
        SrFrameGenerationGuide out;out.history=w.guideHistory;out.motion=w.outputs[0].resource;out.depth=w.guideCurrent->images[0].resource;
        out.previous=w.guidePrevious;out.current=w.guideCurrent->observation.source;
        out.fence=w.fence;out.queue=w.queue;out.fenceValue=w.value;out.reset=w.guideReset;
        out.motionLease=w.outputLease;out.depthLease=w.guideCurrent;return out;
    }return {};
}
NVSDK_NGX_Result SrHistoryOwner::Create(const SrNativeApiContext&c,ID3D12GraphicsCommandList*l,NVSDK_NGX_Feature f,NVSDK_NGX_Parameter*p,NVSDK_NGX_Handle**h){
    std::lock_guard nativeLock(m_impl->nativeMutex);Impl::NativeUse call(*m_impl);
    m_impl->CollectRetiredFg();
    {std::lock_guard lock(m_impl->mutex);if(m_impl->status.quiescing)return NVSDK_NGX_Result_FAIL_NotInitialized;}
    const auto original=c.originals.create(l,f,p,h);
    const bool fg=uint32_t(f)==11&&bool(m_impl->fgSource);
    if(fg&&p){unsigned always{},never{},uir{};const auto a=p->Get("DLSSG.ResourceAlwaysProvidedFlags",&always);
        const auto n=p->Get("DLSSG.ResourceNeverProvidedFlags",&never);const auto u=p->Get("DLSSG.UserInterfaceRecompositionEnabled",&uir);
        Log("[PAIRED-FG-CREATE] always=%u/%08X never=%u/%08X uir=%u/%08X\n",always,unsigned(a),never,unsigned(n),uir,unsigned(u));}
    if(original!=NVSDK_NGX_Result_Success||!h||!*h||(uint32_t(f)!=1&&uint32_t(f)!=13&&!fg)||!c.modulePinned||!c.moduleGeneration||!c.lifecycleGeneration||!c.createOrdinal)return original;
    const auto host=*h;auto canonical=m_impl->Resolve<ID3D12GraphicsCommandList>(l);std::shared_ptr<Impl::Pair> pair;Impl::Work* initWork{};
    {std::lock_guard lock(m_impl->mutex);m_impl->status.lastNativeResult=original;
        if((!m_impl->status.requested&&!fg)||m_impl->status.quiescing||m_impl->Find(c,host))return original;
        if(!fg&&((uint32_t(f)==13&&!m_impl->parameters.known)||(m_impl->parameters.known&&m_impl->parameters.ngxFeature!=uint32_t(f)))){m_impl->status.stage=SrHistoryStage::UnknownConfiguration;return original;}
        if(l&&!canonical){m_impl->status.stage=SrHistoryStage::UnknownNativeIdentity;return original;}
        auto empty=std::find(m_impl->pairs.begin(),m_impl->pairs.end(),nullptr);if(empty==m_impl->pairs.end())return original;
        if(m_impl->nextPair==UINT64_MAX)return original;
        try{pair=std::make_shared<Impl::Pair>();}catch(const std::bad_alloc&){m_impl->status.stage=SrHistoryStage::PoolBusy;return original;}
        pair->context=c;pair->original=host;pair->key=m_impl->nextPair++;
        if(canonical&&!fg){Microsoft::WRL::ComPtr<ID3D12Device> device;if(FAILED(canonical->GetDevice(IID_PPV_ARGS(&device))))return original;
            try{if(!fg)(void)m_impl->pass.Prepare(device.Get());}catch(const std::bad_alloc&){m_impl->status.stage=SrHistoryStage::PoolBusy;return original;}
            initWork=m_impl->ReserveNativeWork(canonical.Get(),pair);if(!initWork)return original;}
        *empty=pair; // Reserve BEFORE getters/companion call can reenter Create.
    }
    pair->createFlagsKnown=p&&p->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,&pair->createFlags)==NVSDK_NGX_Result_Success;
    // Host FG's Create list is not guaranteed to be submitted. Give companion
    // initialization a private, explicitly completed list so its retirement
    // never depends on a phantom host submission after a resolution change.
    if(fg&&canonical){
        ComPtr<ID3D12Device> device;D3D12_COMMAND_QUEUE_DESC queue{};queue.Type=canonical->GetType();
        const bool ready=SUCCEEDED(canonical->GetDevice(IID_PPV_ARGS(&device)))&&
            SUCCEEDED(device->CreateCommandAllocator(queue.Type,IID_PPV_ARGS(&pair->fgInitAllocator)))&&
            SUCCEEDED(device->CreateCommandList(0,queue.Type,pair->fgInitAllocator.Get(),nullptr,IID_PPV_ARGS(&pair->fgInitList)))&&
            SUCCEEDED(device->CreateCommandQueue(&queue,IID_PPV_ARGS(&pair->fgInitQueue)))&&
            SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&pair->fgInitFence)));
        if(!ready){std::lock_guard lock(m_impl->mutex);++m_impl->status.cloneFailures;
            for(auto& stored:m_impl->pairs)if(stored==pair)stored.reset();m_impl->Refresh();return original;}
    }
    NVSDK_NGX_Handle* companion{};const auto clone=c.originals.create(pair->fgInitList?pair->fgInitList.Get():l,f,p,&companion);
    if(pair->fgInitList){
        bool completed=false;
        if(SUCCEEDED(pair->fgInitList->Close())){
            ID3D12CommandList* lists[]{pair->fgInitList.Get()};pair->fgInitQueue->ExecuteCommandLists(1,lists);
            if(SUCCEEDED(pair->fgInitQueue->Signal(pair->fgInitFence.Get(),1))){const auto start=GetTickCount64();
                while(GetTickCount64()-start<2000){const auto done=pair->fgInitFence->GetCompletedValue();
                    if(done==UINT64_MAX)break;if(done>=1){completed=true;break;}Sleep(1);}}
        }
        if(!completed){std::lock_guard lock(m_impl->mutex);pair->companion=companion;
            pair->releaseUncertain=pair->retiring=true;m_impl->Refresh();
            Log("[NATIVE-FG-INIT] completion not proven create=%llu; retained\n",c.createOrdinal);return original;}
        Log("[NATIVE-FG-INIT] private initialization completed create=%llu\n",c.createOrdinal);
        pair->fgInitList.Reset();pair->fgInitAllocator.Reset();pair->fgInitQueue.Reset();pair->fgInitFence.Reset();
    }
    {std::lock_guard lock(m_impl->mutex);m_impl->status.lastCompanionResult=clone;
        const bool collision=std::any_of(m_impl->pairs.begin(),m_impl->pairs.end(),[&](const auto&known){return known&&known!=pair&&(known->original==companion||known->companion==companion);});
        if(clone!=NVSDK_NGX_Result_Success||!companion||companion==host||collision){++m_impl->status.cloneFailures;
            if(initWork)initWork->pair.reset();pair->inFlight=0;for(auto&stored:m_impl->pairs)if(stored==pair)stored.reset();m_impl->Refresh();return original;}
        pair->companion=companion;++m_impl->status.pairedCreates;m_impl->Refresh();
        if(fg)Log("[NATIVE-FG-PAIR] host=%p companion=%p create=%llu result=%08X\n",host,companion,c.createOrdinal,unsigned(clone));
    }return original;
}
NVSDK_NGX_Result SrHistoryOwner::Evaluate(const SrNativeApiContext&c,ID3D12GraphicsCommandList*l,const NVSDK_NGX_Handle*h,const NVSDK_NGX_Parameter*p,PFN_NVSDK_NGX_ProgressCallback cb){
    std::lock_guard nativeLock(m_impl->nativeMutex);Impl::NativeUse call(*m_impl);
    if(c.featureId==11)m_impl->CollectRetiredFg();
    {std::lock_guard lock(m_impl->mutex);if(m_impl->status.quiescing)return NVSDK_NGX_Result_FAIL_NotInitialized;}
    if(c.featureId==11)return EvaluateFrameGeneration(c,l,h,p,cb);
    const auto forward=[&](SrHistoryStage reason){
        {std::lock_guard lock(m_impl->mutex);m_impl->status.stage=reason;if(reason!=SrHistoryStage::Disabled)if(auto pair=m_impl->Find(c,h)){pair->resetMask=3;pair->history={};}}
        int resetValue=-1;float deltaValue{};const bool resetKnown=p&&p->Get(NVSDK_NGX_Parameter_Reset,&resetValue)==NVSDK_NGX_Result_Success;
        const bool deltaKnown=p&&p->Get(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec,&deltaValue)==NVSDK_NGX_Result_Success;
        const auto result=c.originals.evaluate(l,h,p,cb);
        {std::lock_guard lock(m_impl->mutex);++m_impl->status.passthroughEvaluations;m_impl->status.lastNativeResult=result;
            m_impl->status.sourceId=-1;m_impl->status.lastSelectedHandle=reinterpret_cast<uintptr_t>(h);m_impl->status.lastPhysicalEye=UINT32_MAX;
            m_impl->status.lastReset=resetValue;m_impl->status.lastResetKnown=resetKnown;m_impl->status.lastDeltaMsec=deltaValue;m_impl->status.lastDeltaKnown=deltaKnown;
            if(result!=NVSDK_NGX_Result_Success)m_impl->status.stage=SrHistoryStage::NativeFailed;}return result;
    };
    bool requested{},configured{};std::shared_ptr<Impl::Pair> pair;
    {std::lock_guard lock(m_impl->mutex);requested=m_impl->status.requested;configured=m_impl->parameters.known&&m_impl->parameters.effectiveFix&&m_impl->parameters.ngxFeature==c.featureId;pair=m_impl->Find(c,h);}
    if(!requested){
        int savedReset{};bool force=false;{std::lock_guard lock(m_impl->mutex);force=pair&&(pair->resetMask&1u)&&!pair->originalReleased;}
        if(force&&p&&p->Get(NVSDK_NGX_Parameter_Reset,&savedReset)==NVSDK_NGX_Result_Success&&(savedReset==0||savedReset==1)){
            sr_owner::NativeParameters saved;saved.reset=savedReset;NVSDK_NGX_Result result;float suppliedDelta{};
            const bool deltaKnown=p->Get(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec,&suppliedDelta)==NVSDK_NGX_Result_Success;
            {sr_owner::ParameterOverride override(p,saved);override.Reset(1);result=c.originals.evaluate(l,h,p,cb);}
            {std::lock_guard lock(m_impl->mutex);if(result==NVSDK_NGX_Result_Success)pair->resetMask&=~1u;m_impl->status.resetEyeMask=pair->resetMask;
                m_impl->status.lastNativeResult=result;m_impl->status.stage=SrHistoryStage::Disabled;++m_impl->status.passthroughEvaluations;
                m_impl->status.sourceId=-1;m_impl->status.lastSelectedHandle=reinterpret_cast<uintptr_t>(h);m_impl->status.lastPhysicalEye=UINT32_MAX;
                m_impl->status.lastReset=1;m_impl->status.lastResetKnown=true;m_impl->status.lastDeltaMsec=suppliedDelta;m_impl->status.lastDeltaKnown=deltaKnown;}return result;
        }
        return forward(SrHistoryStage::Disabled);
    }
    if(!pair||!pair->companion||pair->retiring||pair->originalReleased||!c.featureKnown)return forward(SrHistoryStage::Unpaired);
    if(!configured)return forward(SrHistoryStage::UnknownConfiguration);
    if(evaluationScopes.empty()||evaluationScopes.overflow||evaluationScopes.back().owner!=this)return forward(SrHistoryStage::UnknownSlScope);
    const auto scope=evaluationScopes.back();
    if(scope.observation.stateContract!=SrSlStateContract::StreamlineOriginalEvaluate||!scope.observation.viewportKnown||scope.observation.viewport!=0||
       !scope.observation.tokenKnown||!scope.observation.featureKnown||!ValidSource(scope.observation.source)||
       !((c.featureId==1&&scope.observation.feature==0)||(c.featureId==13&&scope.observation.feature==1001)))return forward(SrHistoryStage::UnknownSlScope);
    const auto list=m_impl->Resolve<ID3D12GraphicsCommandList>(l);if(!list||ComIdentity(list.Get())!=scope.listIdentity){
        TraceNativeMismatch("list",scope.observation.source,l,ComIdentity(list.Get()),scope.listIdentity);
        return forward(SrHistoryStage::UnknownNativeIdentity);}
    sr_owner::NativeParameters native;const bool nativeValid=native.Read(p);if(!nativeValid){std::lock_guard lock(m_impl->mutex);m_impl->status.missingNativeParameters=native.missing?native.missing:(1ull<<63);}
    if(!nativeValid)return forward(SrHistoryStage::NativeParametersUnknown);
    const std::array<ID3D12Resource*,4> raw{native.depth,native.motion,native.color,native.output};std::array<ComPtr<ID3D12Resource>,4> resources;
    for(unsigned i=0;i<4;++i){resources[i]=m_impl->Resolve<ID3D12Resource>(raw[i]);if(!resources[i]){
        TraceNativeMismatch("resource-resolve",scope.observation.source,raw[i],0,0,i);
        return forward(SrHistoryStage::UnknownNativeIdentity);}}
    if(native.colorX||native.colorY||native.depthX||native.depthY||native.motionX||native.motionY||native.outputX||native.outputY)return forward(SrHistoryStage::NativeParametersUnknown);
    std::shared_ptr<Impl::Snapshot> current;Impl::Work*work{};NVSDK_NGX_Handle* selected{};bool corrected=false;uint32_t eye{};int reset{};float delta=native.delta;
    SrHistoryStage refusal=SrHistoryStage::Recorded;sr::Parameters parameters;SrOptionalOverridePolicy policy;
    {
        std::lock_guard lock(m_impl->mutex);m_impl->CollectLocked();parameters=m_impl->parameters;policy=m_impl->optional;current=m_impl->Current(scope);
        if(!current)refusal=SrHistoryStage::MissingSnapshot;
        else if(!pair->createFlagsKnown||((pair->createFlags&NVSDK_NGX_DLSS_Feature_Flags_DepthInverted)!=0)!=(current->observation.constants.flags[0]==1)||
                ((pair->createFlags&NVSDK_NGX_DLSS_Feature_Flags_MVJittered)!=0)!=(current->observation.constants.flags[6]==1))refusal=SrHistoryStage::NativeParametersUnknown;
        else{
            for(unsigned i=0;i<4;++i)if(ComIdentity(resources[i].Get())!=current->originalIdentity[i]&&
                !(i<2&&current->taggedCopies[i]&&ComIdentity(resources[i].Get())==current->taggedCopyIdentity[i])){
                TraceNativeMismatch("tag-vs-native",scope.observation.source,raw[i],ComIdentity(resources[i].Get()),current->originalIdentity[i],i);
                refusal=SrHistoryStage::UnknownNativeIdentity;}
            if(current->sealed||current->producer!=Impl::WorkState::Recorded||current->listGeneration!=scope.listGeneration)refusal=SrHistoryStage::MissingSnapshot;
            if(refusal==SrHistoryStage::Recorded){
                if(native.reset){m_impl->ResetHistory();current->historyEpoch=m_impl->status.historyEpoch;}
                current->native.known=true;current->native.depthSize={current->images[0].width,current->images[0].height};current->native.motionSize={current->images[1].width,current->images[1].height};
                current->native.renderSubrect={native.width,native.height};current->native.outputSize={uint32_t(current->originalDesc[3].Width),current->originalDesc[3].Height};
                current->native.jitterPixels={native.jitterX,native.jitterY};current->native.mvScale={native.scaleX,native.scaleY};current->native.linearDepth=parameters.linearDepth;
                current->native.lowResMv=(pair->createFlags&NVSDK_NGX_DLSS_Feature_Flags_MVLowRes)!=0;current->native.specularMvPresent=false;
                current->nativeKnown=true;
                auto previous=pair->history[2],old=pair->history[1];
                if(previous&&(previous->observation.source.source.sourceId+1!=current->observation.source.source.sourceId||previous->historyEpoch!=current->historyEpoch||
                    previous->resourceEpoch!=current->resourceEpoch)){pair->history={};pair->resetMask=3;previous.reset();old.reset();}
                if(previous&&old&&(!m_impl->ReadyPrevious(*previous)||!m_impl->ReadyPrevious(*old)))refusal=SrHistoryStage::PendingProducer;
                else{
                    work=m_impl->ReserveNativeWork(list.Get(),pair);if(!work)refusal=SrHistoryStage::PoolBusy;
                    else{
                        work->reads[2]=current;current->sealed=true;eye=uint32_t(current->observation.source.source.physicalEye);selected=eye?pair->companion:pair->original;
                        reset=native.reset||((pair->resetMask&(1u<<eye))!=0)||!previous||!old;
                        if(previous&&old){const std::array<std::shared_ptr<Impl::Snapshot>,3> sources{old,previous,current};
                            const std::array<sr::CameraSample,3> samples{m_impl->Camera(*old,*pair),m_impl->Camera(*previous,*pair),m_impl->Camera(*current,*pair)};
                            const auto built=sr::AdaptCameraHistory(samples,parameters);m_impl->status.adapterStatus=built.status;
                            if(built.status==sr::AdapterStatus::Ready){
                                if(native.deltaKnown)delta=native.delta+pair->lastSuppliedDelta;
                                if(!native.deltaKnown||std::isfinite(delta))corrected=m_impl->Record(*work,sources,built,resources[1].Get(),resources[0].Get());
                                if(corrected){work->guideHistory=samples;work->guideCurrent=current;work->guidePrevious=old->observation.source;work->guideReset=reset!=0;}}
                            if(!corrected){reset=1;pair->resetMask=3;pair->history={};m_impl->status.stage=SrHistoryStage::AdapterRejected;}
                        }else m_impl->status.stage=SrHistoryStage::HistoryWarmup;
                    }
                }
            }
        }
    }
    if(refusal!=SrHistoryStage::Recorded)return forward(refusal);
    // Reference stores the original supplied float before the native call,
    // including calls whose result later fails. Absence never alters this cache.
    if(native.deltaKnown){std::lock_guard lock(m_impl->mutex);pair->lastSuppliedDelta=native.delta;}
    NVSDK_NGX_Result result;
    {
        sr_owner::ParameterOverride overrides(p,native);overrides.Reset(reset);
        if(corrected){if(!(parameters.debugFlags&0x40000u))overrides.Motion(work->outputs[0].resource.Get());overrides.Delta(delta);
            if(policy.known&&policy.replaceColor&&(parameters.debugFlags&0xFF00u)&&current->native.lowResMv&&native.color!=native.output)overrides.Color(work->outputs[3].resource.Get());}
        result=c.originals.evaluate(l,selected,p,cb);
    }
    {std::lock_guard lock(m_impl->mutex);m_impl->status.lastNativeResult=result;
        if(work&&work->evidence){work->evidence->nativeResult=uint32_t(result);work->evidence->nativeResultKnown=true;}
        m_impl->status.sourceId=current->observation.source.source.sourceId;
        m_impl->status.lastSelectedHandle=reinterpret_cast<uintptr_t>(selected);m_impl->status.lastPhysicalEye=eye;
        m_impl->status.lastReset=reset;m_impl->status.lastResetKnown=true;m_impl->status.lastDeltaMsec=corrected?delta:native.delta;m_impl->status.lastDeltaKnown=native.deltaKnown;
        if(result==NVSDK_NGX_Result_Success)++m_impl->status.successfulNativeByEye[eye];
        if(result==NVSDK_NGX_Result_Success&&m_impl->status.requested&&current->historyEpoch==m_impl->status.historyEpoch){
            if(corrected)work->guideKnown=true;
            pair->resetMask&=~(1u<<eye);pair->history={pair->history[1],pair->history[2],current};m_impl->status.resetEyeMask=pair->resetMask;
            if(corrected){++m_impl->status.correctedEvaluations;m_impl->status.stage=SrHistoryStage::Recorded;}
        }else{pair->resetMask=3;pair->history={};m_impl->status.stage=SrHistoryStage::NativeFailed;}
    }return result;
}
NVSDK_NGX_Result SrHistoryOwner::EvaluatePairedFrameGeneration(const SrNativeApiContext&c,ID3D12GraphicsCommandList*l,
        const NVSDK_NGX_Handle*h,const NVSDK_NGX_Parameter*p,PFN_NVSDK_NGX_ProgressCallback cb,const SrSourceStamp&mainStamp){
    cpu_timing::Scope totalTiming(cpu_timing::Stage::NgxPairedTotal);
    std::shared_ptr<Impl::Pair> pair;{std::lock_guard lock(m_impl->mutex);pair=m_impl->Find(c,h);}
    const auto forward=[&](const char* reason){
        constexpr const char* reasons[]={"source-or-pair","native-parameters","native-device","native-output","output-shape","cross-device","output-geometry","gpu-work-pool","private-resources","initial-permission-state"};
        static std::array<std::atomic<uint64_t>,11> reports{};
        size_t bucket=10;for(size_t i=0;i<10;++i)if(std::strcmp(reason,reasons[i])==0){bucket=i;break;}
        const auto total=reports[bucket].fetch_add(1)+1;
        unsigned free{},recorded{},submitting{},submitted{},unknown{},gpuDone{};
        {std::lock_guard lock(m_impl->mutex);if(pair)pair->fgPairedPrevious.reset();
            if(total<=4 || total%256==0)for(const auto&w:std::span(m_impl->work).last(8)){
                if(w.state==Impl::WorkState::Free)++free;
                else if(w.state==Impl::WorkState::Recorded)++recorded;
                else if(w.state==Impl::WorkState::Submitting)++submitting;
                else if(w.state==Impl::WorkState::Unknown)++unknown;
                else{++submitted;if(w.fence){const auto done=w.fence->GetCompletedValue();if(done!=UINT64_MAX&&done>=w.value)++gpuDone;}}
            }
        }
        if(total<=4 || total%256==0)Log("[PAIRED-FG-DROP] reason=%s total=%llu view=%u frame=%u free=%u recorded=%u submitting=%u submitted=%u gpuDone=%u unknown=%u\n",
            reason,total,mainStamp.engineView,mainStamp.nativeFrame,free,recorded,submitting,submitted,gpuDone,unknown);
        return c.originals.evaluate(l,h,p,cb);
    };
    const auto source=AcquireNativeFgPairedSource(mainStamp);
    if(!source||!pair||!pair->companion||pair->retiring||pair->originalReleased)return forward("source-or-pair");
    native_fg::Parameters native;if(!native.Read(p))return forward("native-parameters");
    auto list=m_impl->Resolve<ID3D12GraphicsCommandList>(l);ComPtr<ID3D12Device> device;
    if(!list||FAILED(list->GetDevice(IID_PPV_ARGS(&device))))return forward("native-device");
    const auto& f=source->frame;const unsigned main=source->mainEye,other=main^1;
    const auto resource=[&](const char* key){ID3D12Resource* typed{};void* raw{};
        if(p->Get(key,&typed)==NVSDK_NGX_Result_Success)return typed;
        return p->Get(key,&raw)==NVSDK_NGX_Result_Success?static_cast<ID3D12Resource*>(raw):nullptr;};
    auto* nativeOutput=resource("DLSSG.OutputInterpolated");
    if(!nativeOutput)return forward("native-output");
    const auto outputDesc=nativeOutput->GetDesc();
    if(outputDesc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||outputDesc.MipLevels!=1||outputDesc.DepthOrArraySize!=1||outputDesc.SampleDesc.Count!=1)return forward("output-shape");
    for(unsigned eye=0;eye<2;++eye){const auto& g=f.inputs[eye];
        for(auto* r:{f.color[eye],g->depth.Get(),g->motion.Get()}){ComPtr<ID3D12Device> actual;
            if(!r||FAILED(r->GetDevice(IID_PPV_ARGS(&actual)))||ComIdentity(actual.Get())!=ComIdentity(device.Get()))return forward("cross-device");}
        const auto desc=f.color[eye]->GetDesc();
        const auto compatible=[](DXGI_FORMAT a,DXGI_FORMAT b){return a==b||
            ((a==DXGI_FORMAT_R8G8B8A8_TYPELESS||a==DXGI_FORMAT_R8G8B8A8_UNORM||a==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)&&
             (b==DXGI_FORMAT_R8G8B8A8_TYPELESS||b==DXGI_FORMAT_R8G8B8A8_UNORM||b==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB));};
        if(desc.Width!=outputDesc.Width||desc.Height!=outputDesc.Height||!compatible(desc.Format,outputDesc.Format))return forward("output-geometry");
    }
    Impl::Work* work{};std::shared_ptr<const Frame> previous;unsigned reset{},continuityFailures{};
    {std::lock_guard lock(m_impl->mutex);previous=pair->fgPairedPrevious;
        const bool consecutive=previous&&pair->fgPairedWorld==source->gameWorldEpoch&&pair->fgPairedMain==main&&
            previous->nativeStamp.resourceEpoch==f.nativeStamp.resourceEpoch&&previous->identity.origin==f.identity.origin&&
            previous->nativeStamp.publicationClockGeneration==f.nativeStamp.publicationClockGeneration&&
            previous->nativeStamp.renderPublicationTimeNs<f.nativeStamp.renderPublicationTimeNs&&
            uint32_t(f.nativeStamp.eyes[main].nativeFrame-previous->nativeStamp.eyes[main].nativeFrame)==1;
        reset=native.reset||f.identity.reset||!consecutive;
        if(cpu_timing::Enabled()){
            if(!previous)continuityFailures=1;
            else continuityFailures=(pair->fgPairedWorld!=source->gameWorldEpoch?2u:0u)|(pair->fgPairedMain!=main?4u:0u)|
                (previous->nativeStamp.resourceEpoch!=f.nativeStamp.resourceEpoch?8u:0u)|(previous->identity.origin!=f.identity.origin?16u:0u)|
                (previous->nativeStamp.publicationClockGeneration!=f.nativeStamp.publicationClockGeneration?32u:0u)|
                (previous->nativeStamp.renderPublicationTimeNs>=f.nativeStamp.renderPublicationTimeNs?64u:0u)|
                (uint32_t(f.nativeStamp.eyes[main].nativeFrame-previous->nativeStamp.eyes[main].nativeFrame)!=1?128u:0u);
        }
        if(reset)++pair->fgPairedEpoch;
        work=m_impl->ReserveNativeWork(list.Get(),pair);if(work)work->fgPairedInputs=source;
    }
    if(!work)return forward("gpu-work-pool");
    const auto allocate=[&](Impl::Image& image,D3D12_RESOURCE_DESC desc,D3D12_RESOURCE_STATES state){
        if(image.resource){const auto old=image.resource->GetDesc();if(old.Dimension==desc.Dimension&&old.Width==desc.Width&&old.Height==desc.Height&&old.Format==desc.Format&&old.Flags==desc.Flags)return true;}
        image.resource.Reset();D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&image.resource))))return false;
        (void)cvr::gpu::MarkResourceAllocation(image.resource.Get(),true);image.state=state;return true;
    };
    auto colorDesc=outputDesc;colorDesc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_RESOURCE_DESC flag{};flag.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;flag.Width=4;flag.Height=1;flag.DepthOrArraySize=1;
    flag.MipLevels=1;flag.SampleDesc.Count=1;flag.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;flag.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    auto inputDesc=outputDesc;inputDesc.Flags=D3D12_RESOURCE_FLAG_NONE;
    if(!allocate(work->outputs[0],colorDesc,D3D12_RESOURCE_STATE_UNORDERED_ACCESS)||
       !allocate(work->outputs[1],colorDesc,D3D12_RESOURCE_STATE_UNORDERED_ACCESS)||
       !allocate(work->outputs[2],flag,D3D12_RESOURCE_STATE_COPY_DEST)||
       !allocate(work->outputs[3],inputDesc,D3D12_RESOURCE_STATE_COPY_DEST))return forward("private-resources");
    const auto transition=[&](Impl::Image& image,D3D12_RESOURCE_STATES state){if(image.state==state)return;
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={image.resource.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,image.state,state};
        l->ResourceBarrier(1,&b);image.state=state;};
    transition(work->outputs[3],D3D12_RESOURCE_STATE_COPY_DEST);l->CopyResource(work->outputs[3].resource.Get(),f.color[other]);
    transition(work->outputs[3],D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(work->outputs[2],D3D12_RESOURCE_STATE_COPY_DEST);
    // Copy the real caller's initialization, not an invented allow/deny value.
    if(!CopyNativeFgInitialPermission(l,work->outputs[2].resource.Get()))return forward("initial-permission-state");
    transition(work->outputs[2],D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // The real host evaluation remains exactly once; its native output still
    // drives desktop presentation. The companion has independent resources/history.
    auto* mutableParameters=const_cast<NVSDK_NGX_Parameter*>(p);mutableParameters->Set("DLSSG.Reset",reset);
    const auto producingPolicy=ReadNativeFgPolicyStatus();
    NVSDK_NGX_Result mainResult;
    {cpu_timing::Scope timing(cpu_timing::Stage::NgxHost);mainResult=c.originals.evaluate(l,pair->original,p,cb);}
    mutableParameters->Set("DLSSG.Reset",native.reset);
    NVSDK_NGX_Result otherResult=NVSDK_NGX_Result_FAIL_InvalidParameter;
    BeginNativeFgCompanionOutput(l,work->outputs[3].resource.Get(),work->outputs[0].resource.Get(),work->outputs[2].resource.Get(),work->outputs[1].resource.Get());
    {native_fg::PairedOverride parameters(p,*f.inputs[other],work->outputs[3].resource.Get(),work->outputs[0].resource.Get(),
            work->outputs[1].resource.Get(),work->outputs[2].resource.Get(),reset);
        if(parameters){cpu_timing::Scope timing(cpu_timing::Stage::NgxCompanion);otherResult=c.originals.evaluate(l,pair->companion,p,cb);}
        else{static std::atomic<unsigned> failures{};if(failures.fetch_add(1)<12)Log("[PAIRED-FG-PARAMETERS] rejected=%s\n",parameters.Reason());}
    }
    const auto metadata=[](const Frame& frame){auto value=std::make_shared<Frame>(frame);value->lease.reset();value->colorCompletion={};value->identity.inputs=false;
        for(unsigned eye=0;eye<2;++eye){value->color[eye]=nullptr;value->inputs[eye].reset();if(value->projection[eye])value->projection[eye]->queue.Reset();}return value;};
    const auto current=metadata(f);
    if(mainResult==NVSDK_NGX_Result_Success&&otherResult==NVSDK_NGX_Result_Success){
        const auto policyRevision=ProducedPolicyRevision(producingPolicy,ReadNativeFgPolicyStatus(),native.count);
        for(unsigned eye=0;eye<2;++eye){NativeFgReceipt r;r.policyRevision=policyRevision;r.paired=true;r.pairedEye=eye;r.current=PairedNativeStamp(f.nativeStamp.eyes[eye]);
            if(previous)r.previous=PairedNativeStamp(previous->nativeStamp.eyes[eye]);r.previousPairSerial=previous?previous->identity.serial:0;r.currentPairSerial=f.identity.serial;
            r.gameWorldEpoch=source->gameWorldEpoch;r.moduleGeneration=c.moduleGeneration;r.lifecycleGeneration=c.lifecycleGeneration;r.createOrdinal=c.createOrdinal;
            r.historyEpoch=pair->fgPairedEpoch;r.resourceEpoch=f.nativeStamp.resourceEpoch;r.index=native.index;r.count=native.count;r.reset=reset!=0;
            r.pairedPreviousMetadata=previous;r.pairedCurrentMetadata=current;
            if(eye==main&&m_impl->fgReporter)m_impl->fgReporter(l,h,r);
            else if(eye==other)RecordNativeFgCompanionOutput(l,work->outputs[3].resource.Get(),work->outputs[0].resource.Get(),work->outputs[2].resource.Get(),r);
        }
        if(native.index==native.count){std::lock_guard lock(m_impl->mutex);pair->fgPairedPrevious=current;pair->fgPairedWorld=source->gameWorldEpoch;pair->fgPairedMain=main;}
    }else {std::lock_guard lock(m_impl->mutex);pair->fgPairedPrevious.reset();}
    if(!EndNativeFgCompanionOutput(l)){std::lock_guard lock(m_impl->mutex);pair->retiring=true;pair->fgPairedPrevious.reset();work->state=Impl::WorkState::Unknown;
        Log("[PAIRED-FG] unknown companion resource states; private resources retained, companion disabled\n");}
    static uint64_t calls{};if(++calls<=12||calls%120==0)Log("[PAIRED-FG-EVALUATE] call=%llu serial=%llu frame=%u mainEye=%u reset=%u main=%08X other=%08X\n",
        calls,f.identity.serial,f.nativeStamp.eyes[main].nativeFrame,main,reset,unsigned(mainResult),unsigned(otherResult));
    if(cpu_timing::Enabled()&&(calls<=12||calls%120==0))Log("[PAIRED-FG-RESET] call=%llu serial=%llu native=%u frame=%d continuityFailures=%u index=%u count=%u previousSerial=%llu currentTime=%lld previousTime=%lld\n",
        calls,f.identity.serial,native.reset,int(f.identity.reset),continuityFailures,native.index,native.count,previous?previous->identity.serial:0,
        f.nativeStamp.renderPublicationTimeNs,previous?previous->nativeStamp.renderPublicationTimeNs:0);
    if(calls<=12||calls%120==0)Log("[PAIRED-FG-TIME] serial=%llu contentSkew=%lld poseSkew=%lld timeL=%lld timeR=%lld\n",f.identity.serial,
        f.nativeStamp.eyes[1].contentTimeNs-f.nativeStamp.eyes[0].contentTimeNs,f.nativeStamp.eyes[1].posePredictionNs-f.nativeStamp.eyes[0].posePredictionNs,
        f.nativeStamp.eyes[0].contentTimeNs,f.nativeStamp.eyes[1].contentTimeNs);
    return mainResult;
}
NVSDK_NGX_Result SrHistoryOwner::EvaluateFrameGeneration(const SrNativeApiContext&c,ID3D12GraphicsCommandList*l,
    const NVSDK_NGX_Handle*h,const NVSDK_NGX_Parameter*p,PFN_NVSDK_NGX_ProgressCallback cb){
    std::shared_ptr<Impl::Pair> pair;bool requested{};sr::Parameters configuration;
    {std::lock_guard lock(m_impl->mutex);pair=m_impl->Find(c,h);requested=m_impl->status.requested;configuration=m_impl->parameters;}
    SrSourceStamp source;try{if(m_impl->fgSource)source=m_impl->fgSource(p);}catch(...){}
    if(!source.source){
        const auto paired=AcquireNativeFgPairedSource(source);static std::atomic<uint64_t> ticks{};const auto tick=++ticks;
        if(tick<=6||tick%120==0)Log("[PAIRED-FG-INGRESS] tick=%llu view=%u frame=%u known=%d pose=%llu write=%llu paired=%d serial=%llu\n",
            tick,source.engineView,source.nativeFrame,int(source.nativeFrameKnown),source.poseId,source.writeId,int(bool(paired)),paired?paired->frame.identity.serial:0);
        return EvaluatePairedFrameGeneration(c,l,h,p,cb,source);
    }
    const auto forward=[&](const char* reason){
        {std::lock_guard lock(m_impl->mutex);if(pair){pair->resetMask=3;pair->fgPrevious={};}}
        static std::atomic<unsigned> refusals{};if(source.source.sourceId>=8&&refusals.fetch_add(1)<16)
            Log("[NATIVE-FG-STEREO] source=%lld eye=%u accepted=0 reason=%s\n",static_cast<long long>(source.source.sourceId),unsigned(source.source.physicalEye),reason);
        return c.originals.evaluate(l,h,p,cb);
    };
    if(!requested||!pair||!pair->companion||pair->retiring||pair->originalReleased||!ValidSource(source))return forward("source-or-pair");
    auto guide=AcquireFrameGenerationGuide(source);if(!guide)return forward("sr-guide");
    native_fg::Parameters inputs;if(!inputs.Read(p))return forward("parameters");
    if(!inputs.Matches(*guide))return forward("guide-geometry");
    const auto built=sr::AdaptCameraHistory(guide->history,configuration);
    if(built.status!=sr::AdapterStatus::Ready)return forward("camera-history");
    const auto eye=unsigned(source.source.physicalEye);if(eye>1||guide->history[0].key.source.physicalEye!=source.source.physicalEye)return forward("eye-history");
    const float fov=float(built.diagnostics.fovYRadians[2]);if(!(fov>0&&fov<3.141593f))return forward("projection");
    auto list=m_impl->Resolve<ID3D12GraphicsCommandList>(l);if(!list)return forward("native-list");
    ComPtr<ID3D12Device> device,motionDevice,depthDevice;
    if(FAILED(list->GetDevice(IID_PPV_ARGS(&device)))||FAILED(guide->motion->GetDevice(IID_PPV_ARGS(&motionDevice)))||
       FAILED(guide->depth->GetDevice(IID_PPV_ARGS(&depthDevice)))||ComIdentity(device.Get())!=ComIdentity(motionDevice.Get())||ComIdentity(device.Get())!=ComIdentity(depthDevice.Get()))return forward("native-device");
    Impl::Work* work{};unsigned reset{};NVSDK_NGX_Handle* selected{};
    {std::lock_guard lock(m_impl->mutex);
        if(inputs.reset){pair->resetMask=3;pair->fgPrevious={};}
        reset=unsigned(inputs.reset||guide->reset||(pair->resetMask&(1u<<eye))||pair->fgPrevious[eye]!=guide->history[0].key.source);
        selected=eye?pair->companion:pair->original;work=m_impl->ReserveNativeWork(list.Get(),pair);
        if(work)work->fgInputs=*guide;
    }
    if(!work)return forward("gpu-work-pool");
    NVSDK_NGX_Result result;const auto producingPolicy=ReadNativeFgPolicyStatus();
    {native_fg::Override overrides(p,inputs,*guide,reset,fov);result=c.originals.evaluate(l,selected,p,cb);}
    {std::lock_guard lock(m_impl->mutex);
        if(result==NVSDK_NGX_Result_Success){if(inputs.index==inputs.count)pair->fgPrevious[eye]=source.source;pair->resetMask&=~(1u<<eye);}
        else{pair->fgPrevious={};pair->resetMask=3;}}
    if(result==NVSDK_NGX_Result_Success&&m_impl->fgReporter){NativeFgReceipt receipt;
        receipt.policyRevision=ProducedPolicyRevision(producingPolicy,ReadNativeFgPolicyStatus(),inputs.count);
        receipt.previous=guide->previous;receipt.current=guide->current;receipt.moduleGeneration=c.moduleGeneration;
        receipt.lifecycleGeneration=c.lifecycleGeneration;receipt.createOrdinal=c.createOrdinal;
        receipt.historyEpoch=guide->history[2].key.historyEpoch;receipt.resourceEpoch=guide->history[2].key.resourceEpoch;
        receipt.index=inputs.index;receipt.count=inputs.count;receipt.reset=reset!=0;
        m_impl->fgReporter(l,h,receipt);
    }
    const auto id=source.source.sourceId;if(id<32||id%120==0)
        Log("[NATIVE-FG-STEREO] source=%lld eye=%u accepted=1 previous=%lld host=%p selected=%p reset=%u fov=%.9g producerFence=%llu consumerFence=%llu result=%08X\n",
            static_cast<long long>(id),eye,static_cast<long long>(guide->history[0].key.source.sourceId),h,selected,reset,double(fov),guide->fenceValue,work->value,unsigned(result));
    return result;
}
NVSDK_NGX_Result SrHistoryOwner::Release(const SrNativeApiContext&c,NVSDK_NGX_Handle*h){
    std::lock_guard nativeLock(m_impl->nativeMutex);Impl::NativeUse call(*m_impl);std::shared_ptr<Impl::Pair> pair;
    {std::lock_guard lock(m_impl->mutex);m_impl->CollectLocked();pair=m_impl->Find(c,h);if(pair)pair->retiring=true;}
    // Preserve the game's original FG Release contract. Our companion may
    // still have an unsubmitted last Evaluate: retain it until actual Reset
    // cancels that recording or its observed submission fence completes.
    // Reporting our bookkeeping as a failure of the HOST Release poisons
    // Streamline's next feature after resize (the host never retries it).
    if(pair&&c.featureId==11){
        const auto original=pair->originalReleased?NVSDK_NGX_Result_Success:c.originals.release(h);
        if(original!=NVSDK_NGX_Result_Success)return original;
        {std::lock_guard lock(m_impl->mutex);pair->originalReleased=true;m_impl->status.lastNativeResult=original;}
        m_impl->CollectRetiredFg();return original;
    }
    {std::lock_guard lock(m_impl->mutex);
        if(pair&&(pair->inFlight||pair->releaseUncertain)){m_impl->status.stage=SrHistoryStage::Retiring;return NVSDK_NGX_Result_FAIL_PlatformError;}}
    const auto original=pair&&pair->originalReleased?NVSDK_NGX_Result_Success:c.originals.release(h);if(!pair||original!=NVSDK_NGX_Result_Success)return original;
    pair->originalReleased=true;NVSDK_NGX_Result companion;
    try{companion=c.originals.release(pair->companion);}catch(...){std::lock_guard lock(m_impl->mutex);pair->releaseUncertain=true;pair->retiring=true;throw;}
    {std::lock_guard lock(m_impl->mutex);m_impl->status.lastNativeResult=original;m_impl->status.lastCompanionResult=companion;
        if(companion==NVSDK_NGX_Result_Success){pair->companionReleased=true;for(auto&known:m_impl->pairs)if(known==pair)known.reset();}
        else pair->retiring=true;m_impl->Refresh();}
    return original;
}
SrNativeRetireResult SrHistoryOwner::BeforeHostShutdown(){
    {std::lock_guard lock(m_impl->mutex);if(m_impl->retiredSuccessfully)return SrNativeRetireResult::Retired;
        m_impl->status.quiescing=true;m_impl->status.requested=false;m_impl->CollectLocked();
        if(m_impl->activeNative||m_impl->activeScopes||m_impl->retirementRunning||m_impl->status.livePasses||m_impl->status.pendingProducers||m_impl->status.pendingCaptures)return SrNativeRetireResult::NotReady;
        for(const auto& w:m_impl->work)if(w.outputLease&&w.outputLease.use_count()>1)return SrNativeRetireResult::NotReady;
        m_impl->retirementRunning=true;}
    struct RetirementFlag {Impl&owner;~RetirementFlag(){std::lock_guard lock(owner.mutex);owner.retirementRunning=false;}} retirement{*m_impl};
    std::lock_guard nativeLock(m_impl->nativeMutex);bool all=true;
    for(auto&stored:m_impl->pairs){std::shared_ptr<Impl::Pair> pair;{std::lock_guard lock(m_impl->mutex);pair=stored;}if(!pair||pair->companionReleased)continue;
        if(pair->releaseUncertain){all=false;continue;}NVSDK_NGX_Result result;
        try{result=pair->context.originals.release(pair->companion);}catch(...){std::lock_guard lock(m_impl->mutex);pair->releaseUncertain=true;pair->retiring=true;throw;}
        {std::lock_guard lock(m_impl->mutex);m_impl->status.lastCompanionResult=result;
            if(result==NVSDK_NGX_Result_Success){pair->companionReleased=true;stored.reset();}else all=false;}}
    {std::lock_guard lock(m_impl->mutex);all=all&&m_impl->pass.TryShutdown();
        if(all){m_impl->snapshots={};m_impl->work={};m_impl->listEpochs={};m_impl->constants={};
            // Terminal for this instance, distinct from a merely quiescing or
            // empty-looking NotReady owner. Factory rearm creates a new owner.
            m_impl->retiredSuccessfully=true;}
        m_impl->Refresh();m_impl->status.stage=all?SrHistoryStage::Retired:SrHistoryStage::Retiring;}
    return all?SrNativeRetireResult::Retired:SrNativeRetireResult::NotReady;
}
}
