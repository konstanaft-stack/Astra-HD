#include "Framegen/SrHistoryIntegration.hpp"
#include "Framegen/SrReferenceDefaults.hpp"
#include "Framegen/SrIdentityDiagnostics.hpp"
#include "Framegen/Framegen.hpp"
#include "Framegen/NativeFgObservation.hpp"
#include "Camera/PoseIdentity.hpp"
#include "Camera/ImagePoseIdentity.hpp"
#include "Hooks/Hook.hpp"
#include <sl_core_types.h>
#include <wrl/client.h>
#include <bit>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <share.h>
// The same resolved path/parser used by Core/LiveControlsPoll.cpp. Loading just
// framegen settings here avoids starting the launcher or graphics at PreDevice.
extern char g_liveControlPath[MAX_PATH];
extern void InitRuntimePaths();
extern void Log(const char*,...);
extern "C" {
__declspec(dllexport) cvr::framegen::SrHistoryWire CyberpunkVR_SrHistoryStatus;
__declspec(dllexport) std::atomic<uint64_t> CyberpunkVR_SrHistoryStatusSeq{};
}
namespace cvr::framegen {
namespace {
std::atomic<std::shared_ptr<SrHistoryOwner>> history;
std::atomic<bool> requested{false},exceptionLogged{false};
std::mutex installMutex;
std::atomic<uint32_t> statusWriter{};
std::atomic<uint64_t> statusDropped{};
struct TagCopyScope {std::shared_ptr<SrHistoryOwner> owner;SrTagCaptureToken token;};
thread_local std::array<TagCopyScope,4> tagCopyScopes;
void Publish(const std::shared_ptr<SrHistoryOwner>& owner) {
    if(!owner)return;
    const auto s=owner->Status();SrHistoryWire wire;
    wire.sourceId=s.sourceId;wire.historyEpoch=s.historyEpoch;wire.resourceEpoch=s.resourceEpoch;
    wire.pairedCreates=s.pairedCreates;wire.cloneFailures=s.cloneFailures;wire.capturedSources=s.capturedSources;
    wire.correctedEvaluations=s.correctedEvaluations;wire.passthroughEvaluations=s.passthroughEvaluations;wire.resetEvents=s.resetEvents;
    wire.stage=uint32_t(s.stage);wire.snapshotCells=s.snapshotCells;wire.pendingCaptures=s.pendingCaptures;
    wire.livePasses=s.livePasses;wire.livePairs=s.livePairs;wire.resetEyeMask=s.resetEyeMask;
    wire.adapterStatus=uint32_t(s.adapterStatus);wire.nativeResult=uint32_t(s.lastNativeResult);
    wire.companionResult=uint32_t(s.lastCompanionResult);wire.pendingProducers=s.pendingProducers;
    wire.missingNativeParameters=s.missingNativeParameters;wire.crossQueueWaits=s.crossQueueWaits;wire.submissionFailures=s.submissionFailures;
    wire.successfulNativeByEye=s.successfulNativeByEye;wire.lastSelectedHandle=s.lastSelectedHandle;wire.lastPhysicalEye=s.lastPhysicalEye;
    wire.lastReset=s.lastReset;wire.lastDeltaMsec=s.lastDeltaMsec;wire.lastResetKnown=s.lastResetKnown;wire.lastDeltaKnown=s.lastDeltaKnown;
    wire.lastSubmissionResult=s.lastSubmissionResult;
    wire.requested=s.requested;wire.quiescing=s.quiescing;
    uint32_t expected=0;
    if(!statusWriter.compare_exchange_strong(expected,1,std::memory_order_acquire)){++statusDropped;return;}
    wire.dropped=statusDropped.load(std::memory_order_relaxed);
    CyberpunkVR_SrHistoryStatusSeq.fetch_add(1,std::memory_order_acq_rel);
    CyberpunkVR_SrHistoryStatus=wire;
    CyberpunkVR_SrHistoryStatusSeq.fetch_add(1,std::memory_order_release);
    statusWriter.store(0,std::memory_order_release);
}
void FailClosed(const std::shared_ptr<SrHistoryOwner>& owner) noexcept {
    requested.store(false,std::memory_order_release);
    try {if(owner)owner->SetRequested(false);}catch(...){}
    if(!exceptionLogged.exchange(true))Log("[sr-history] callback exception: new SR admissions disabled; host call preserved\n");
}
HRESULT ResolveNative(IUnknown* original,REFIID iid,void** result) {
    if(!result)return E_POINTER;*result=nullptr;if(!original)return E_POINTER;
    using Getter=sl::Result(*)(void*,void**);
    const auto module=GetModuleHandleW(L"sl.interposer.dll");
    const auto getter=module?reinterpret_cast<Getter>(GetProcAddress(module,"slGetNativeInterface")):nullptr;
    if(!getter)return E_NOINTERFACE;
    void* value{};const auto status=getter(original,&value);
    if(status!=sl::Result::eOk||!value)return E_NOINTERFACE;
    // slGetNativeInterface transfers one reference, including identical pointers.
    Microsoft::WRL::ComPtr<IUnknown> native;native.Attach(static_cast<IUnknown*>(value));
    return native->QueryInterface(iid,result);
}
SrSourceStamp Source(const cvr::camera::PoseIdentity& pose) {
    SrSourceStamp s;
    s.source=pose.mainEyeSource;s.poseId=pose.poseId;s.writeId=pose.writeId;s.origin=pose.head.originSerial;
    s.captureClockGeneration=pose.clockGeneration;s.locatedPoseTimeNs=pose.posePredictionNs;
    s.nativeFrame=pose.renderFrameId;s.nativeFrameKnown=pose.hasRenderFrameId;s.engineView=pose.view;s.poseKnown=bool(pose);
    s.appliedEye=pose.appliedEyeReference;
    // PoseIdentity preserves this proof only when the actual serialized/blended
    // camera fingerprint matches. Unknown/changed camera paths clear the proof.
    if(s.appliedEye)s.selectedFingerprint=s.appliedEye.writtenEye;
    return s;
}
SrSourceStamp Source(){return Source(cvr::camera::CurrentNodePoseIdentity());}
std::shared_ptr<SrHistoryOwner> CapturingOwner() {
    return requested.load(std::memory_order_acquire)?history.load(std::memory_order_acquire):nullptr;
}
std::shared_ptr<SrHistoryOwner> ConfiguredOwner(bool enabled) {
    auto owner=std::make_shared<SrHistoryOwner>(&ResolveNative);
    if(NativeFgStereoRequested())owner->ConfigureFrameGeneration([](const NVSDK_NGX_Parameter* p){
        ID3D12Resource* input{};if(!p||p->Get("DLSSG.Backbuffer",&input)!=NVSDK_NGX_Result_Success||!input)return SrSourceStamp{};
        const auto image=camera::ReadImagePose(input);return image?Source(image.pose):SrSourceStamp{};
    },&ObserveNativeFgStereoResult);
    owner->SetConfiguration(sr::ReferenceCp2077SuperSamplingDefaults(true),{true,false,false});
    owner->SetRequested(enabled);return owner;
}
std::shared_ptr<SrNativeHandler> NewHostLifecycle(uint64_t generation) {
    if(!generation)return {};
    // The native hook calls this only after proven retirement and successful
    // host shutdown. A retired owner is never reused or revived from Evaluate.
    std::lock_guard lock(installMutex);
    auto owner=ConfiguredOwner(requested.load(std::memory_order_acquire));
    history.store(owner,std::memory_order_release);Publish(owner);return owner;
}
}
bool InstallSrHistoryIntegration() {
    std::lock_guard lock(installMutex);
    if(history.load(std::memory_order_acquire))return true;
    try {
        InitRuntimePaths();auto settings=GetSettings();
        if(FILE* file=_fsopen(g_liveControlPath,"r",_SH_DENYNO)) {
            char line[512]{};while(fgets(line,sizeof(line),file))ParseSetting(line,settings);fclose(file);
        }
        auto owner=ConfiguredOwner(settings.aerV2);
        if(!SetSrNativeHandler(owner))return false;
        history.store(owner,std::memory_order_release);requested.store(settings.aerV2,std::memory_order_release);
        SetSrNativeLifecycleFactory(&NewHostLifecycle);
        Publish(owner);return true;
    }catch(...){FailClosed(history.load());return false;}
}
void SetSrHistoryRequested(bool value) noexcept {
    std::shared_ptr<SrHistoryOwner> owner;
    try {
        std::lock_guard lock(installMutex);owner=history.load(std::memory_order_acquire);
        if(owner)owner->SetRequested(value);
        requested.store(value,std::memory_order_release);Publish(owner);
    }catch(...){FailClosed(owner);}
}
SrHistoryStatus ReadSrHistoryStatus() noexcept {
    try {const auto owner=history.load(std::memory_order_acquire);return owner?owner->Status():SrHistoryStatus{};}
    catch(...){return {};}
}
std::optional<SrFrameGenerationGuide> AcquireSrFrameGenerationGuide(const camera::PoseIdentity& pose) noexcept {
    try {const auto owner=CapturingOwner();return owner?owner->AcquireFrameGenerationGuide(Source(pose)):std::nullopt;}
    catch(...){return {};}
}
SrConstantsCall BeginSrConstants(const sl::Constants& constants,const sl::FrameToken& token,const sl::ViewportHandle& viewport) noexcept {
    SrConstantsCall call;call.owner=CapturingOwner();if(!call.owner)return call;
    try {
        call.observation.source=Source();SrTraceRecord snapshot;
        SnapshotSrConstants(snapshot,&constants,&token,&viewport);
        auto& o=call.observation;o.viewport=snapshot.slViewport;o.slToken=snapshot.slFrameToken;
        o.viewportKnown=(snapshot.flags&SrViewportKnown)!=0;o.tokenKnown=(snapshot.flags&SrTokenKnown)!=0;
        if(snapshot.flags&SrConstantsKnown){
            SrConstantsWire values{};std::memcpy(&values,snapshot.payload.data(),sizeof(values));
            o.constants.source=CameraConstantsSource::Streamline;o.constants.version=uint32_t(snapshot.constantsVersion);
            for(size_t i=0;i<values.floatBits.size();++i){o.constants.values[i]=std::bit_cast<float>(values.floatBits[i]);o.constants.valuePresent.set(i,(values.valuePresent[i/32]&(1u<<(i%32)))!=0);}
            o.constants.flags=values.flags;
            for(size_t i=0;i<values.flags.size();++i)o.constants.flagPresent.set(i,(values.flagPresent&(1u<<i))!=0);
        }
    }catch(...){FailClosed(call.owner);call={};}return call;
}
void EndSrConstants(SrConstantsCall& call,uint32_t result,bool resultKnown) noexcept {
    auto owner=std::move(call.owner);if(!owner)return;
    try {call.observation.originalResult=result;call.observation.resultKnown=resultKnown;owner->ObserveConstants(call.observation);Publish(owner);}
    catch(...){FailClosed(owner);}
}
SrTagsCall BeginSrTags(const void* viewport,const void* tags,uint32_t count,void* list) noexcept {
    SrTagsCall call;call.owner=CapturingOwner();if(!call.owner)return call;
    try {
        if(count>SrIdentityTraceStorage::MaxTags){call={};return call;}
        const auto source=Source();SrTraceRecord snapshot;SnapshotSrTags(snapshot,viewport,tags,count,list);
        if(snapshot.tagMalformedCount||(snapshot.flags&SrTagsTruncated)){call={};return call;}
        std::array<SrSnapshotTag,SrIdentityTraceStorage::MaxTags> captured{};
        for(uint32_t i=0;i<snapshot.capturedCount;++i){
            SrTagWire wire{};std::memcpy(&wire,reinterpret_cast<const unsigned char*>(snapshot.payload.data())+i*sizeof(wire),sizeof(wire));
            auto& t=captured[i];t.type=wire.type;t.state=wire.state;t.x=wire.left;t.y=wire.top;t.width=wire.width;t.height=wire.height;t.lifecycle=wire.lifecycle;
            t.known=(wire.flags&SrTagKnown)!=0;
            if(wire.flags&SrResourceKnown){
                // An unrelated buffer tag must not hide a depth revoke in the
                // same batch. The owner validates descriptors of required roles.
                if(wire.resourceType!=uint32_t(sl::ResourceType::eTex2d)&&wire.resourceType!=uint32_t(sl::ResourceType::eBuffer)){call={};return call;}
                t.resource=reinterpret_cast<ID3D12Resource*>(uintptr_t(wire.native));
            }
        }
        call.token=call.owner->BeginTags(static_cast<ID3D12GraphicsCommandList*>(list),snapshot.slViewport,(snapshot.flags&SrViewportKnown)!=0,source,std::span(captured.data(),snapshot.capturedCount));
        if(call.token){
            Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> native;
            if(SUCCEEDED(ResolveNative(static_cast<ID3D12GraphicsCommandList*>(list),IID_PPV_ARGS(&native)))&&EnsureNativeResourceCopyObservation(native.Get()))
                for(auto& slot:tagCopyScopes)if(!slot.owner){slot={call.owner,call.token};break;}
        }
    }catch(...){FailClosed(call.owner);call={};}return call;
}
void EndSrTags(SrTagsCall& call,uint32_t result,bool resultKnown) noexcept {
    auto owner=std::move(call.owner);const auto token=call.token;call.token={};if(!owner||!token)return;
    for(auto& slot:tagCopyScopes)if(slot.owner==owner&&slot.token.value==token.value)slot={};
    try {owner->EndTags(token,result,resultKnown);Publish(owner);}catch(...){FailClosed(owner);}
}
void SrObserveTaggedCopy(ID3D12GraphicsCommandList* list,ID3D12Resource* destination,ID3D12Resource* source) noexcept {
    for(auto it=tagCopyScopes.rbegin();it!=tagCopyScopes.rend();++it)if(it->owner){
        try{it->owner->ObserveTaggedCopy(it->token,list,destination,source);}catch(...){FailClosed(it->owner);}
        return;
    }
}
SrEvaluationCall BeginSrEvaluation(uint32_t feature,const sl::FrameToken& token,const sl::BaseStructure*const* inputs,uint32_t count,void* list) noexcept {
    SrEvaluationCall call;call.owner=CapturingOwner();if(!call.owner)return call;
    try {
        SrSlEvaluationObservation observation;observation.source=Source();SrTraceRecord snapshot;
        SnapshotSrEvaluate(snapshot,feature,&token,inputs,count,list);
        observation.viewport=snapshot.slViewport;observation.viewportKnown=(snapshot.flags&SrViewportKnown)!=0;
        observation.slToken=snapshot.slFrameToken;observation.tokenKnown=(snapshot.flags&SrTokenKnown)!=0;
        observation.feature=feature;observation.featureKnown=true;
        observation.commandList=static_cast<ID3D12GraphicsCommandList*>(list);
        observation.stateContract=SrSlStateContract::StreamlineOriginalEvaluate;
        call.token=call.owner->BeginSlEvaluation(observation);
    }catch(...){FailClosed(call.owner);call={};}return call;
}
void EndSrEvaluation(SrEvaluationCall& call,uint32_t result,bool resultKnown) noexcept {
    auto owner=std::move(call.owner);const auto token=call.token;call.token={};if(!owner||!token)return;
    try {owner->EndSlEvaluation(token,result,resultKnown);Publish(owner);}catch(...){FailClosed(owner);}
}
SrSubmissionCall SrBeforeSubmit(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList*const* lists) noexcept {
    SrSubmissionCall call;call.owner=history.load(std::memory_order_acquire);if(!call.owner)return call;
    try {call.token=call.owner->BeforeSubmit(queue,count,lists);}catch(...){FailClosed(call.owner);call={};}
    return call;
}
void SrSubmitted(SrSubmissionCall& call,ID3D12CommandQueue* queue,UINT count,ID3D12CommandList*const* lists) noexcept {
    auto owner=std::move(call.owner);const auto token=call.token;call.token={};if(!owner)return;
    try {if(token)owner->Submitted(token,queue,count,lists);Publish(owner);}catch(...){FailClosed(owner);}
}
void SrResetList(ID3D12GraphicsCommandList* list) noexcept {
    const auto owner=history.load(std::memory_order_acquire);if(!owner)return;
    try {owner->ResetList(list);}catch(...){FailClosed(owner);}
}
CVR_HOOK("SrHistoryOwner",::cvr::hooks::Stage::PreDevice,25,InstallSrHistoryIntegration);
}
