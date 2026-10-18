#include "Utils/DebugGate.hpp"
#include "Framegen/Inputs.hpp"
#include "Framegen/StreamlineCameraSnapshot.hpp"
#include "Framegen/NativeCameraSnapshot.hpp"
#include "Framegen/ProducerTrace.hpp"
#include "Framegen/InputPoolBusyTrace.hpp"
#include "Framegen/SrIdentityDiagnostics.hpp"
#include "Framegen/SrNativeApi.hpp"
#include "Framegen/SrHistoryIntegration.hpp"
#include "Framegen/SlEvaluationResult.hpp"
#include "Framegen/InputCopy.hpp"
#include "Render/NativeObjectIdentity.hpp"
#include "Framegen/FrameProjectionMetadata.hpp"
#include "Framegen/SubmittedImageProjection.hpp"
#include "Framegen/Framegen.hpp"
#include "Camera/PoseIdentity.hpp"
#include "Stereo/StereoInternal.hpp"
#include "Stereo/EngineRvas.hpp"
#include <MinHook.h>
#include <sl_core_types.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

extern void Log(const char*,...);
static_assert(uint32_t(sl::Result::eOk)==0 && uint32_t(sl::Result::eWarnOutOfVRAM)==39);
extern "C" int32_t CyberpunkVR_VrcamDlssViewport;
extern "C" __declspec(dllexport) std::atomic<uint64_t> CyberpunkVR_FramegenInputStages[10]{};
extern "C" {
__declspec(dllexport) cvr::framegen::InputPoolBusyStorage CyberpunkVR_FramegenInputPoolBusyTrace;
}
namespace cvr::framegen {
namespace {
using Microsoft::WRL::ComPtr;
// Snapshot callback provenance BEFORE calling original SL. Scalar copy only;
// native render frame, owner source ID and original SL token remain separate.
void BeginSrTrace(SrTraceRecord& out) {
    out.qpcBegin=SrTraceQpc();const auto pose=cvr::camera::CurrentNodePoseIdentity();
    const auto& node=CurrentProducerNodeScope();out.engineViewKey=node.viewKey;out.nodeContext=node.context;
    if(node.viewKeyKnown)out.flags|=SrNodeKeyKnown;
    out.nativeFrame=pose.renderFrameId;out.engineView=pose.view;out.poseId=pose.poseId;out.poseWriteId=pose.writeId;
    out.origin=pose.head.originSerial;out.captureClockGeneration=pose.clockGeneration;
    out.contentTimeNs=pose.contentTimeNs;out.posePredictionNs=pose.posePredictionNs;
    if(pose)out.flags|=SrNodePoseKnown;if(pose.hasRenderFrameId)out.flags|=SrNativeFrameKnown;
    const auto& source=pose.mainEyeSource;
    if(source){
        out.flags|=SrSourceKnown|SrPhysicalEyeKnown;out.sourceId=source.sourceId;out.physicalEye=uint32_t(source.physicalEye);
        out.rendererEpoch=source.epochs.renderer;out.gameWorldEpoch=source.epochs.gameWorld;out.hmdWorldEpoch=source.epochs.hmdWorld;
        out.requestRevision=source.requestRevision;out.latchRevision=source.latchRevision;out.sceneOrdinal=source.sceneOrdinal;
        out.sourceClockGeneration=source.clockGeneration;out.requestedPoseTimeNs=source.requestedPoseTimeNs;
    }
}
struct Constants {CameraData camera{};uint32_t frame{};bool valid{};uint64_t pose{},origin{};cvr::camera::PoseIdentity identity{};};
struct Record {
    Inputs data;
    uint64_t generation{};
    uint8_t expected{},submitted{};
    uint8_t claimedMask{};
    bool invalid{},unknownProducer{},projectionOnly{};
};
struct Link {std::weak_ptr<Record> record;uint64_t generation;uint8_t mask;};
std::mutex mutex;
std::array<Constants,8> cameras[2];
std::array<std::shared_ptr<Record>,4> pools[2];
std::unordered_map<ID3D12CommandList*,std::vector<Link>> pending;
std::atomic<bool> havePending{false};
uint64_t nextGeneration{};
uint64_t projectionPublications{},guidePublications{},recordedGuideCopies{};
uint64_t unrelatedTagRecords{},projectionMisses[2]{};
std::atomic<bool> hooksInstalled{false};
std::atomic<bool> logged[2]{};
bool ProducerFinished(const Record& r) {
    if(r.unknownProducer)return false;
    if(!r.data.readyFence)return true;
    const auto completed=r.data.readyFence->GetCompletedValue();
    return completed!=UINT64_MAX&&completed>=r.data.fenceValue;
}
// Caller owns the registry mutex. Reuse is bounded by the actual four MAIN
// records, all external aliases, and completion of the native producer.
InputReuseDecision ClassifyReusableRecord(const std::shared_ptr<Record>& item,InputReuseSlot* diagnostic=nullptr) {
    const auto finish=[&](InputReuseDecision decision) {
        if(diagnostic)diagnostic->decision=decision;return decision;
    };
    if(!item)return finish(InputReuseDecision::Empty);
    const auto owners=item.use_count();
    if(diagnostic) {
        const auto& data=item->data;
        diagnostic->knownBits=InputSlotPresent|InputIdentityCopied|InputUseCountRead;
        diagnostic->generation=item->generation;diagnostic->poseId=data.poseId;
        diagnostic->poseWriteId=data.poseWriteId;diagnostic->origin=data.origin;
        diagnostic->clockGeneration=data.clockGeneration;diagnostic->useCount=uint64_t(owners);
        diagnostic->nativeFrame=data.frameId;diagnostic->view=data.view;
        diagnostic->expectedMask=item->expected;diagnostic->submittedMask=item->submitted;
        diagnostic->claimedMask=item->claimedMask;diagnostic->invalid=item->invalid;
        diagnostic->projectionOnly=item->projectionOnly;diagnostic->unknownProducer=item->unknownProducer;
    }
    if(owners!=1)return finish(InputReuseDecision::ExternalAlias);
    if(diagnostic)diagnostic->knownBits|=InputMaskPredicateReached;
    if(item->expected!=item->submitted)return finish(InputReuseDecision::SubmissionMaskMismatch);
    if(diagnostic)diagnostic->knownBits|=InputProducerPredicateReached;
    if(item->unknownProducer)return finish(InputReuseDecision::UnknownProducer);
    if(diagnostic)diagnostic->knownBits|=InputFencePresentRead;
    if(!item->data.readyFence)return finish(InputReuseDecision::Reusable);
    const auto completed=item->data.readyFence->GetCompletedValue();
    if(diagnostic) {
        diagnostic->readyFence=reinterpret_cast<uint64_t>(item->data.readyFence.Get());
        diagnostic->requiredValue=item->data.fenceValue;diagnostic->completedValue=completed;
        diagnostic->knownBits|=InputCompletionRead;
    }
    if(completed==UINT64_MAX)return finish(InputReuseDecision::FenceInvalid);
    return finish(completed>=item->data.fenceValue?InputReuseDecision::Reusable:InputReuseDecision::FencePending);
}
std::shared_ptr<Record> ReusableRecord(unsigned side,bool projectionOnly,InputPoolBusyRecord* diagnostic=nullptr) {
    if(side>=2)return {};
    std::shared_ptr<Record> oldest;
    for(auto& item:pools[side]) {
        const auto index=size_t(&item-pools[side].data());
        auto* slot=diagnostic?&diagnostic->slots[index]:nullptr;
        if(slot)slot->slotIndex=uint32_t(index);
        const auto decision=ClassifyReusableRecord(item,slot);
        if(decision!=InputReuseDecision::Empty&&decision!=InputReuseDecision::Reusable)continue;
        if(!item){item=std::make_shared<Record>();return item;}
        if(!projectionOnly)return item;
        // CPU-only records have no guide fence or external Inputs lease to hold
        // them. Keep the newest four identities instead of replacing slot zero.
        if(!oldest||item->generation<oldest->generation)oldest=item;
    }
    return oldest;
}
void TraceGuides(const Inputs& data,ProducerReason reason) {
    ProducerTraceRecord trace;trace.stage=ProducerStage::GuidesSubmission;trace.reason=reason;
    trace.identityView=data.view;trace.engineView=data.view==1?EngineView::Main:(data.view==2?EngineView::Vrcam:EngineView::Unknown);
    if(trace.engineView!=EngineView::Unknown)trace.flags|=EngineViewKnown;
    trace.nativeFrame=data.frameId;trace.flags|=NativeFrameKnown;
    trace.poseId=data.poseId;trace.spaceEpoch=data.origin;
    trace.poseWriteId=data.poseWriteId;if(data.poseWriteId)trace.flags|=PoseWriteIdKnown;
    trace.contentTimeNs=data.contentTimeNs;trace.poseTimeNs=data.posePredictionNs;
    if(data.clockGeneration && data.contentTimeNs>0)trace.flags|=ContentTimeValid;
    if(data.posePredictionNs>0)trace.flags|=PoseTimeValid;
    if(data.poseId)trace.flags|=PoseIdKnown|PoseIdentityValid;
    if(data.origin)trace.flags|=SpaceEpochKnown;
    trace.readyFence=uint64_t(reinterpret_cast<uintptr_t>(data.readyFence.Get()));trace.readyValue=data.fenceValue;
    trace.restingState=uint32_t(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);trace.flags|=ResourceStateKnown;
    if(reason==ProducerReason::GuidesScheduled && data.readyFence && data.fenceValue && data.fenceValue!=UINT64_MAX)
        trace.flags|=ProducerFenceScheduled|GuidesAvailable;
    if(HasCompleteCameraConstants(data.camera.constants))trace.flags|=CameraConstantsComplete;
    RecordProducerTrace(trace);
}
bool ValidCamera(const CameraData& c) {
    if(!c.cameraMotion || c.motion3d || !std::isfinite(c.nearPlane) || c.nearPlane<=0 ||
       !(c.farPlane>c.nearPlane) || !std::isfinite(c.verticalFov) || c.verticalFov<=0 || c.verticalFov>=3.13f ||
       !std::isfinite(c.aspect) || c.aspect<=0 ||
       !std::isfinite(c.motionScale[0]) || !std::isfinite(c.motionScale[1]) ||
       !std::isfinite(c.jitter[0]) || !std::isfinite(c.jitter[1]))return false;
    for(unsigned i=0;i<3;++i)if(!std::isfinite(c.position[i]) || !std::isfinite(c.up[i]) ||
        !std::isfinite(c.right[i]) || !std::isfinite(c.forward[i]))return false;
    return true;
}

bool Camera(const sl::Constants* p,const sl::FrameToken* token,CameraData* out,uint32_t* frame) {
    __try {
        if(!p || !token || p->structType!=sl::Constants::s_structType || p->structVersion<1 || p->structVersion>2)return false;
        *frame=uint32_t(*token);
        CameraData c{};
        c.jitter[0]=p->jitterOffset.x;c.jitter[1]=p->jitterOffset.y;
        c.motionScale[0]=p->mvecScale.x;c.motionScale[1]=p->mvecScale.y;
        std::memcpy(c.position,&p->cameraPos,sizeof(c.position));std::memcpy(c.up,&p->cameraUp,sizeof(c.up));
        std::memcpy(c.right,&p->cameraRight,sizeof(c.right));std::memcpy(c.forward,&p->cameraFwd,sizeof(c.forward));
        c.nearPlane=p->cameraNear;c.farPlane=p->cameraFar;c.verticalFov=p->cameraFOV;c.aspect=p->cameraAspectRatio;
        const float projectionY=std::abs(p->cameraViewToClip[1].y);
        if(std::isfinite(projectionY) && projectionY>0.001f)c.verticalFov=2*std::atan(1/projectionY);
        c.inverted=p->depthInverted==sl::eTrue;c.infinite=std::isinf(c.farPlane);
        c.jittered=p->motionVectorsJittered==sl::eTrue;c.reset=p->reset==sl::eTrue;
        c.cameraMotion=p->cameraMotionIncluded==sl::eTrue;c.motion3d=p->motionVectors3D==sl::eTrue;
        if(p->depthInverted==sl::eInvalid || p->reset==sl::eInvalid ||
           p->orthographicProjection!=sl::eFalse || !ValidCamera(c))return false;
        c.constants=SnapshotStreamlineCamera(*p);
        *out=c;return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
using ConstantsFn=sl::Result (*)(const sl::Constants&,const sl::FrameToken&,const sl::ViewportHandle&);
using FrameTokenFn=sl::Result (*)(sl::FrameToken*&,const uint32_t*);
FrameTokenFn originalFrameToken{};
sl::Result GetFrameToken(sl::FrameToken*& token,const uint32_t* index) {
    uint32_t paired{};
    if(index && cvr::detail::paired_sl_frame_index(&paired))index=&paired;
    return originalFrameToken(token,index);
}
ConstantsFn originalConstants{};
sl::Result SetConstants(const sl::Constants& input,const sl::FrameToken& frame,const sl::ViewportHandle& view) {
    auto historyCall=BeginSrConstants(input,frame,view);
    SrConstantsCleanup historyCleanup(historyCall);
    std::optional<SrTraceRecord> sr;
    if(CaptureInputsEnabled() || SrIdentityTraceRequested()){sr.emplace();BeginSrTrace(*sr);SnapshotSrConstants(*sr,&input,&frame,&view);}
    const auto result=originalConstants(input,frame,view);
    if(NativeFgStereoRequested()){static std::atomic<uint64_t> clockCalls{};const auto n=++clockCalls;
        if(n<=8||n%240==0){const auto pose=cvr::camera::CurrentNodePoseIdentity();Log("[SL-CLOCK] call=%llu viewport=%u token=%u nativeView=%u nativeFrame=%u known=%d result=%u\n",
            n,unsigned(view),unsigned(frame),pose.view,pose.renderFrameId,int(pose.hasRenderFrameId),unsigned(result));}}
    EndSrConstants(historyCall,uint32_t(result));
    if(sr){sr->apiResult=uint32_t(result);sr->flags|=SrResultKnown;sr->qpcEnd=SrTraceQpc();RecordSrIdentity(*sr);}
    if(!CaptureProjectionEnabled())return result;
    const auto viewport=uint32_t(view);
    const int side=viewport==uint32_t(CyberpunkVR_VrcamDlssViewport) ? 1 : (viewport==0 ? 0 : -1);
    const auto pose=cvr::camera::CurrentNodePoseIdentity();
    if(result==sl::Result::eOk && side>=0 && side<2 && pose && pose.hasRenderFrameId && pose.view==unsigned(side+1)) {
        Constants value{};value.valid=Camera(&input,&frame,&value.camera,&value.frame);
        static std::atomic<uint32_t> diagnosticCount{};
        if(cvr::RuntimeDiagnosticsEnabled() && diagnosticCount.fetch_add(1)<4)Log("[framegen] constants viewport=%u valid=%d frame=%u version=%llu near=%.6f far=%.3f fov=%.5f py=%.5f motion=(%.6f,%.6f) flags=%d/%d/%d/%d\n",
            viewport,value.valid,value.frame,input.structVersion,input.cameraNear,input.cameraFar,input.cameraFOV,input.cameraViewToClip[1].y,
            input.mvecScale.x,input.mvecScale.y,input.depthInverted,input.cameraMotionIncluded,input.motionVectors3D,input.reset);
        value.frame=pose.renderFrameId;
        value.pose=pose.poseId;value.origin=pose.head.originSerial;value.identity=pose;
        if(value.valid) {std::lock_guard lock(mutex);cameras[side][value.frame%8]=value;}
    }
    return result;
}
using EvaluateFn=sl::Result (*)(sl::Feature,const sl::FrameToken&,const sl::BaseStructure* const*,uint32_t,void*);
EvaluateFn originalEvaluate{};
sl::Result Evaluate(sl::Feature feature,const sl::FrameToken& frame,const sl::BaseStructure* const* inputs,uint32_t count,void* list) {
    EnsureSrNativeHooks(); // before original SL can lazily create/evaluate NGX
    auto historyCall=BeginSrEvaluation(uint32_t(feature),frame,inputs,count,list);
    SrEvaluationCleanup historyCleanup(historyCall);
    std::optional<SrTraceRecord> sr;
    if(CaptureInputsEnabled() || SrIdentityTraceRequested()){sr.emplace();BeginSrTrace(*sr);SnapshotSrEvaluate(*sr,uint32_t(feature),&frame,inputs,count,list);}
    const auto result=originalEvaluate(feature,frame,inputs,count,list);
    EndSrEvaluation(historyCall,uint32_t(result));
    if(sr){sr->apiResult=uint32_t(result);sr->flags|=SrResultKnown;sr->qpcEnd=SrTraceQpc();RecordSrIdentity(*sr);}
    if(!CaptureProjectionEnabled())return result;
    const auto pose=cvr::camera::CurrentNodePoseIdentity();
    if(SlEvaluationCompleted(uint32_t(result)) && (feature==0 || feature==1001) && pose && pose.hasRenderFrameId && pose.view>=1 && pose.view<=2 && list) {
        // SL receives a ReShade wrapper, while native Execute/Reset hooks see
        // the underlying list. Only normalize registry keys; original SL work
        // must still receive its original game-facing interface.
        auto identity=cvr::gpu::ResolveNativeObject<ID3D12CommandList>(static_cast<ID3D12CommandList*>(list));
        if(!identity)return result;
        std::lock_guard lock(mutex);
        for(auto& item:pools[pose.view-1])if(item && !item->invalid && item->data.frameId==pose.renderFrameId &&
            item->data.poseId==pose.poseId && item->data.origin==pose.head.originSerial &&
            item->data.hasDepth && item->data.hasMotion && !item->data.submitted && !item->data.evaluated) {
            item->data.evaluated=true;item->expected|=4;
            CVR_DIAGNOSTIC(CyberpunkVR_FramegenInputStages[7].fetch_add(1,std::memory_order_relaxed));
            pending[identity.Get()].push_back({item,item->generation,4});
            havePending.store(true,std::memory_order_release);
        }
    }
    return result;
}
bool ReadTag(const sl::ResourceTag* p,InputTag* out) {
    __try {
        if(p->structType!=sl::ResourceTag::s_structType || p->structVersion!=1 ||
           !p->resource || p->resource->structType!=sl::Resource::s_structType ||
           p->resource->structVersion!=1 || p->resource->type!=sl::ResourceType::eTex2d)return false;
        *out={static_cast<ID3D12Resource*>(p->resource->native),p->resource->state,uint32_t(p->type),
            p->extent.left,p->extent.top,p->extent.width,p->extent.height};
        return out->resource && (out->type==0 || out->type==1) && out->state!=UINT32_MAX;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
}

bool ReadRenderFrameIndex(uint32_t* index) {
    // Native renderer frame clock: incremented at RVA294055 and read by
    // temporal rendering passes (e.g. RVA788104). The per-view +1A0 token
    // used by Streamline is permanently zero for this RTT view, so cannot
    // identify its texture history. Read the common clock; never modify it.
    __try {
        if(!index || !cvr::detail::g_exe_base)return false;
        const auto renderer=*reinterpret_cast<const uint8_t* const*>(cvr::detail::g_exe_base+cvr::detail::RENDERER_GLOBAL_RVA);
        if(!renderer)return false;*index=*reinterpret_cast<const uint32_t*>(renderer+0x4CA4);return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
namespace {
bool NativeCamera(void* state,CameraData* result) {
    // The exact source fields copied by native0x78933C into sl::Constants.
    // Its last-frame gate skips slSetConstants when VRCAM's native token
    // stays at zero, though staging already contains that view's camera.
    __try {
        if(!state)return false;const auto* p=static_cast<const uint8_t*>(state);CameraData c{};
        std::memcpy(c.position,p+0x1A0,12);std::memcpy(c.up,p+0x1AC,12);
        std::memcpy(c.right,p+0x1B8,12);std::memcpy(c.forward,p+0x1C4,12);
        std::memcpy(&c.nearPlane,p+0x1D0,4);std::memcpy(&c.farPlane,p+0x1D4,4);
        std::memcpy(&c.aspect,p+0x1DC,4);std::memcpy(c.jitter,p+0x1E0,8);std::memcpy(c.motionScale,p+0x1E8,8);
        const float projectionY=std::abs(*reinterpret_cast<const float*>(p+0xB4));
        c.verticalFov=projectionY>.001f ? 2*std::atan(1/projectionY) : 0;
        c.inverted=p[0x1F1]!=0;c.cameraMotion=p[0x1F3]!=0;c.reset=p[0x1F0]==0;c.infinite=std::isinf(c.farPlane);
        c.motion3d=*reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))+0x32F8598)!=0;
        if(!ValidCamera(c))return false;c.constants=SnapshotNativeCameraState(state,c);*result=c;return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
}
void RecordNativeCamera(void* state,unsigned view) {
    if(!CaptureProjectionEnabled() || view<1 || view>2)return;
    const auto pose=cvr::camera::CurrentNodePoseIdentity();
    if(!pose || !pose.hasRenderFrameId || pose.view!=view)return;
    Constants value{};value.frame=pose.renderFrameId;value.valid=NativeCamera(state,&value.camera);
    value.pose=pose.poseId;value.origin=pose.head.originSerial;value.identity=pose;
    if(value.valid) {std::lock_guard lock(mutex);cameras[view-1][value.frame%8]=value;}
}
void RecordTags(const void* viewport,const void* tags,uint32_t count,void* commandList) {
    if(CaptureInputsEnabled()){
        SrTraceRecord sr;BeginSrTrace(sr);SnapshotSrTags(sr,viewport,tags,count,commandList);
        sr.qpcEnd=SrTraceQpc();RecordSrIdentity(sr); // pre-original request; result/token unknown
    }
    CVR_DIAGNOSTIC(CyberpunkVR_FramegenInputStages[0].fetch_add(1,std::memory_order_relaxed));
    if(!CaptureProjectionEnabled() || !tags || !commandList || count>32)return;
    const bool projectionOnly=!CaptureInputsEnabled();
    CVR_DIAGNOSTIC(CyberpunkVR_FramegenInputStages[1].fetch_add(1,std::memory_order_relaxed));
    const auto pose=cvr::camera::CurrentNodePoseIdentity();
    auto trace=DescribeProducerNode(CurrentProducerNodeScope(),{pose.poseId,pose.writeId,pose.head.originSerial,
        pose.view,pose.renderFrameId,pose.head.valid,pose.hasRenderFrameId});
    const auto reject=[&](ProducerReason reason) {trace.reason=reason;RecordProducerTrace(trace);};
    if(!pose || pose.view<1 || pose.view>2) {RecordProducerTrace(trace);return;}
    CVR_DIAGNOSTIC(CyberpunkVR_FramegenInputStages[2].fetch_add(1,std::memory_order_relaxed));
    if(!pose.hasRenderFrameId) {RecordProducerTrace(trace);return;}
    CVR_DIAGNOSTIC(CyberpunkVR_FramegenInputStages[3].fetch_add(1,std::memory_order_relaxed));
    auto* list=static_cast<ID3D12GraphicsCommandList*>(commandList);
    if(list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT) {reject(ProducerReason::NonDirectCommandList);return;}
    auto identity=cvr::gpu::ResolveNativeObject<ID3D12CommandList>(list);
    if(!identity)return;
    static std::atomic<unsigned> identityLogs{};
    if(identityLogs.fetch_add(1,std::memory_order_relaxed)<4)
        Log("[INPUT-LIST-IDENTITY] tags list=%p native=%p\n",list,identity.Get());
    std::array<InputTag,32> parsedTags{};uint32_t parsedCount=0;uint8_t incomingMask=0;
    for(uint32_t i=0;i<count;++i){InputTag tag{};if(ReadTag(static_cast<const sl::ResourceTag*>(tags)+i,&tag)){
        parsedTags[parsedCount++]=tag;incomingMask|=uint8_t(1u<<tag.type);
    }}
    std::unique_lock lock(mutex);
    const auto side=pose.view-1;const auto constants=cameras[side][pose.renderFrameId%8];
    if(!constants.valid) {reject(ProducerReason::CameraMissing);return;}
    if(constants.frame!=pose.renderFrameId) {reject(ProducerReason::CameraFrameMismatch);return;}
    if(constants.pose!=pose.poseId) {reject(ProducerReason::CameraPoseMismatch);return;}
    if(constants.origin!=pose.head.originSerial) {reject(ProducerReason::CameraOriginMismatch);return;}
    CVR_DIAGNOSTIC(CyberpunkVR_FramegenInputStages[4].fetch_add(1,std::memory_order_relaxed));
    RecordProducerTrace(trace);
    std::shared_ptr<Record> record;
    for(const auto& item:pools[side])if(item && !item->invalid && item->projectionOnly==projectionOnly && !(item->claimedMask&incomingMask) && !item->data.submitted && item->data.frameId==pose.renderFrameId &&
        item->data.poseId==pose.poseId && item->data.origin==pose.head.originSerial) {record=item;break;}
    static const bool busyDiagnostic=[] {char flag[8]{};return GetEnvironmentVariableA("CPVR_FG_HISTORY_DIAG",flag,sizeof(flag))==1&&flag[0]=='1';}();
    std::optional<InputPoolBusyRecord> busyReceipt;
    if(!record&&busyDiagnostic)busyReceipt.emplace();
    if(!record)if((record=ReusableRecord(side,projectionOnly,busyReceipt?&*busyReceipt:nullptr))) {
        if(!incomingMask)++unrelatedTagRecords;
        auto& data=record->data;
        if(projectionOnly){data.depth.Reset();data.motion.Reset();data.readyFence.Reset();data.fenceValue=0;data.bytes=0;}
        data.poseId=pose.poseId;data.origin=pose.head.originSerial;data.frameId=pose.renderFrameId;data.view=pose.view;
        data.poseWriteId=pose.writeId;data.clockGeneration=pose.clockGeneration;
        data.cameraApplicationQpc=pose.cameraApplicationQpc;data.contentTimeNs=pose.contentTimeNs;data.posePredictionNs=pose.posePredictionNs;
        data.camera=constants.camera;data.queue=nullptr;data.hasDepth=data.hasMotion=data.submitted=data.evaluated=false;
        record->generation=++nextGeneration;record->expected=record->submitted=record->claimedMask=0;
        record->invalid=record->unknownProducer=false;record->projectionOnly=projectionOnly;
    }
    if(!record) {
        if(busyReceipt) {
            auto& busy=*busyReceipt;busy.requestedPoseId=pose.poseId;busy.requestedPoseWriteId=pose.writeId;
            busy.requestedOrigin=pose.head.originSerial;busy.requestedClockGeneration=pose.clockGeneration;
            busy.requestedNativeFrame=pose.renderFrameId;busy.requestedView=pose.view;
            busy.incomingMask=incomingMask;busy.projectionOnly=projectionOnly;
            busy.requestedKnownBits=uint32_t(trace.flags);busy.qpc=SrTraceQpc();
            CyberpunkVR_FramegenInputPoolBusyTrace.Record(busy);
        }
        reject(ProducerReason::InputPoolBusy);OnSkip();return;
    }
    // Resource/barrier hooks may enter other renderer locks. Do GPU recording
    // outside the input registry lock, then publish only the fields we wrote.
    // A prior component's queue/fence publication must not be overwritten.
    Inputs copy=record->data;
    lock.unlock();
    uint8_t copied=0;
    for(uint32_t i=0;i<parsedCount;++i) {
        const auto& tag=parsedTags[i];
        CVR_DIAGNOSTIC(CyberpunkVR_FramegenInputStages[5].fetch_add(1,std::memory_order_relaxed));
        if((tag.type==0 && copy.hasDepth) || (tag.type==1 && copy.hasMotion))continue;
        bool observed=false;
        if(projectionOnly){InputExtent extent;if(InspectInputTag(tag,extent)){
            if(tag.type==0){copy.width=extent.width;copy.height=extent.height;copy.hasDepth=true;}
            else{copy.motionWidth=extent.width;copy.motionHeight=extent.height;copy.hasMotion=true;}
            observed=true;
        }}else observed=CopyInput(list,tag,copy);
        if(observed) {
            CVR_DIAGNOSTIC(CyberpunkVR_FramegenInputStages[6].fetch_add(1,std::memory_order_relaxed));
            copied|=tag.type==0 ? 1 : 2;
        }
    }
    lock.lock();
    if(copied) {
        if(!projectionOnly)recordedGuideCopies+=unsigned(bool(copied&1))+unsigned(bool(copied&2));
        auto& data=record->data;
        data.depth=std::move(copy.depth);data.motion=std::move(copy.motion);
        data.width=copy.width;data.height=copy.height;data.motionWidth=copy.motionWidth;data.motionHeight=copy.motionHeight;
        data.hasDepth=copy.hasDepth;data.hasMotion=copy.hasMotion;data.bytes=copy.bytes;
        if(!data.readyFence)data.readyFence=std::move(copy.readyFence);
        record->expected|=copied;
        pending[identity.Get()].push_back({record,record->generation,copied});
        havePending.store(true,std::memory_order_release);
    }
}
bool InstallInputHooks() {
    const bool lifecycleReady=InstallSrSlHooks();
    if(hooksInstalled.load(std::memory_order_acquire))return lifecycleReady;
    const auto module=GetModuleHandleW(L"sl.interposer.dll");if(!module)return false;
    auto* constants=GetProcAddress(module,"slSetConstants");auto* evaluate=GetProcAddress(module,"slEvaluateFeature");
    auto* frameToken=GetProcAddress(module,"slGetNewFrameToken");
    if(!constants || !evaluate || !frameToken)return false;
    if(MH_CreateHook(constants,reinterpret_cast<void*>(&SetConstants),reinterpret_cast<void**>(&originalConstants))!=MH_OK)return false;
    if(MH_CreateHook(evaluate,reinterpret_cast<void*>(&Evaluate),reinterpret_cast<void**>(&originalEvaluate))!=MH_OK) {MH_RemoveHook(constants);return false;}
    if(MH_CreateHook(frameToken,reinterpret_cast<void*>(&GetFrameToken),reinterpret_cast<void**>(&originalFrameToken))!=MH_OK){
        MH_RemoveHook(constants);MH_RemoveHook(evaluate);return false;
    }
    if(MH_EnableHook(frameToken)!=MH_OK || MH_EnableHook(constants)!=MH_OK || MH_EnableHook(evaluate)!=MH_OK) {
        MH_DisableHook(constants);MH_DisableHook(evaluate);MH_DisableHook(frameToken);
        MH_RemoveHook(constants);MH_RemoveHook(evaluate);MH_RemoveHook(frameToken);return false;
    }
    hooksInstalled.store(true,std::memory_order_release);Log("[framegen] native Streamline input hooks installed\n");return lifecycleReady;
}
InputSubmission PrepareInputSubmission(UINT count,ID3D12CommandList* const* lists) {
    InputSubmission batch;if(!lists||!havePending.load(std::memory_order_acquire))return batch;
    std::lock_guard lock(mutex);
    for(UINT i=0;i<count;++i) {
        const auto it=pending.find(lists[i]);if(it==pending.end())continue;
        for(const auto& link:it->second)if(auto record=link.record.lock();record && record->generation==link.generation) {
            size_t slot=0;for(;slot<batch.count;++slot)if(batch.records[slot].get()==&record->data&&batch.generations[slot]==link.generation)break;
            if(slot==batch.count){
                if(batch.count==batch.Capacity){record->invalid=record->unknownProducer=true;continue;}
                batch.records[slot]=std::shared_ptr<const Inputs>(record,&record->data);batch.generations[slot]=link.generation;++batch.count;
            }
            batch.masks[slot]|=link.mask;record->claimedMask|=link.mask;
        }
        pending.erase(it);
    }
    havePending.store(!pending.empty(),std::memory_order_release);return batch;
}
void Submitted(InputSubmission& submission,ID3D12CommandQueue* queue) {
    // Consume exactly once, including failures. Strong aliases retain each
    // record while its fence is attached; never scan the live pending map.
    auto batch=std::move(submission);submission={};
    std::lock_guard lock(mutex);
    for(size_t i=0;i<batch.count;++i)for(auto& pool:pools)for(auto& record:pool)
        if(record && &record->data==batch.records[i].get() && record->generation==batch.generations[i]) {
            auto& data=record->data;
            if(!queue || (data.queue && data.queue!=queue)) {record->invalid=record->unknownProducer=true;TraceGuides(data,ProducerReason::InputQueueMismatch);continue;}
            data.queue=queue;record->submitted|=batch.masks[i];
            if(data.readyFence && FAILED(queue->Signal(data.readyFence.Get(),++data.fenceValue))) {
                record->invalid=record->unknownProducer=true;TraceGuides(data,ProducerReason::InputSignalFailed);
            }
            if(!record->invalid && record->submitted==7 && data.evaluated && !data.submitted) {
                data.submitted=true;
                if(record->projectionOnly)++projectionPublications;else ++guidePublications;
                const auto publications=projectionPublications+guidePublications;
                if(publications<=4||publications%240==0){uint64_t bytes{};for(const auto& pool:pools)for(const auto& r:pool)if(r)bytes+=r->data.bytes;
                    Log("[INPUT-CAPTURE] projectionOnly=%llu fullGuides=%llu recordedGuideCopies=%llu retainedGuideBytes=%llu unrelatedTagRecords=%llu\n",
                        projectionPublications,guidePublications,recordedGuideCopies,bytes,unrelatedTagRecords);
                }
                // MAIN alternates physical eyes in AER; engine view1 alone
                // cannot name that eye. Its consumer reports the proven receipt.
                if(!record->projectionOnly&&!GetSettings().aerV2)OnInput(data.view-1,data.width,data.height);
                if(!record->projectionOnly)TraceGuides(data,ProducerReason::GuidesScheduled);
                if(cvr::RuntimeDiagnosticsEnabled() && !logged[data.view-1].exchange(true))Log(
                    "[framegen] view=%u frame=%u pose=%llu depth=%ux%u mv=%ux%u scale=(%.6f,%.6f) near=%.6f far=%.3f fov=%.5f inverted=%d\n",
                    data.view,data.frameId,data.poseId,data.width,data.height,data.motionWidth,data.motionHeight,
                    data.camera.motionScale[0],data.camera.motionScale[1],data.camera.nearPlane,data.camera.farPlane,data.camera.verticalFov,data.camera.inverted);
                CVR_DIAGNOSTIC(CyberpunkVR_FramegenInputStages[8].fetch_add(1,std::memory_order_relaxed));
            }
    }
}
void ResetList(ID3D12GraphicsCommandList* list) {
    if(!havePending.load(std::memory_order_acquire))return;
    std::lock_guard lock(mutex);const auto it=pending.find(list);if(it==pending.end())return;
    for(const auto& link:it->second)if(auto r=link.record.lock();r && r->generation==link.generation) {r->invalid=true;r->submitted|=link.mask;}
    pending.erase(it);
    havePending.store(!pending.empty(),std::memory_order_release);
}
std::shared_ptr<const Inputs> AcquireInputs(unsigned view,uint32_t frame,uint64_t pose,uint64_t origin,ID3D12CommandQueue* queue) {
    if(view<1 || view>2)return {};
    std::lock_guard lock(mutex);
    for(const auto& r:pools[view-1])if(r && !r->invalid && !r->projectionOnly && r->data.submitted && r->data.frameId==frame &&
        r->data.poseId==pose && r->data.origin==origin && r->data.queue==queue)
        {CVR_DIAGNOSTIC(CyberpunkVR_FramegenInputStages[9].fetch_add(1,std::memory_order_relaxed));return std::shared_ptr<const Inputs>(r,&r->data);}
    return {};
}
std::optional<FrameProjectionMetadata> AcquireFrameProjection(unsigned view,uint32_t frame,uint64_t pose,uint64_t origin,ID3D12CommandQueue* queue) {
    if(view<1||view>2)return {};
    std::lock_guard lock(mutex);
    for(const auto& r:pools[view-1])if(r&&!r->invalid&&r->data.submitted&&r->data.frameId==frame&&
        r->data.poseId==pose&&r->data.origin==origin&&r->data.queue==queue)
        return SnapshotFrameProjection(r->data);
    const auto misses=++projectionMisses[view-1];
    if(misses<=4||misses%240==0){
        unsigned sameFrame{},invalid{},unsubmitted{},wrongPose{},wrongOrigin{},wrongQueue{};
        uint32_t oldest=UINT32_MAX,newest{};
        for(const auto& r:pools[view-1])if(r){oldest=std::min(oldest,r->data.frameId);newest=std::max(newest,r->data.frameId);
            if(r->data.frameId==frame){++sameFrame;invalid+=r->invalid;unsubmitted+=!r->data.submitted;
                wrongPose+=r->data.poseId!=pose;wrongOrigin+=r->data.origin!=origin;wrongQueue+=r->data.queue!=queue;}}
        Log("[PROJECTION-MISS] view=%u count=%llu requested=%u pool=%u..%u sameFrame=%u invalid=%u unsubmitted=%u wrongPose=%u wrongOrigin=%u wrongQueue=%u unrelatedTagRecords=%llu\n",
            view,misses,frame,oldest,newest,sameFrame,invalid,unsubmitted,wrongPose,wrongOrigin,wrongQueue,unrelatedTagRecords);
    }
    return {};
}
std::optional<FrameProjectionMetadata> AcquireSubmittedImageProjection(
        const cvr::camera::ImagePoseIdentity& image,SubmittedProjectionReason* reason) {
    const auto reject=[&](SubmittedProjectionReason code)->std::optional<FrameProjectionMetadata>{if(reason)*reason=code;return {};};
    if(!image||!image.resource||!image.queue)return reject(SubmittedProjectionReason::ImageUnknown);
    const auto& p=image.pose;
    if(!p||!p.hasRenderFrameId||p.view<1||p.view>2||!p.head.originSerial||!p.clockGeneration||
       !p.cameraApplicationQpc||!p.contentTimeNs||!p.posePredictionNs)return reject(SubmittedProjectionReason::IdentityUnknown);
    std::lock_guard lock(mutex);
    const auto& camera=cameras[p.view-1][p.renderFrameId%8];
    if(!camera.valid)return reject(SubmittedProjectionReason::CameraMissing);
    const auto& q=camera.identity;
    if(camera.frame!=p.renderFrameId||camera.pose!=p.poseId||camera.origin!=p.head.originSerial||
       !q||!q.hasRenderFrameId||q.view!=p.view||q.renderFrameId!=p.renderFrameId||q.poseId!=p.poseId||
       q.writeId!=p.writeId||q.head.originSerial!=p.head.originSerial||q.clockGeneration!=p.clockGeneration||
       q.cameraApplicationQpc!=p.cameraApplicationQpc||q.contentTimeNs!=p.contentTimeNs||q.posePredictionNs!=p.posePredictionNs)
        return reject(SubmittedProjectionReason::IdentityMismatch);
    FrameProjectionMetadata out;out.constants=camera.camera.constants;out.queue=reinterpret_cast<ID3D12CommandQueue*>(image.queue);
    out.view=q.view;out.frameId=q.renderFrameId;out.poseId=q.poseId;out.poseWriteId=q.writeId;out.origin=q.head.originSerial;
    out.clockGeneration=q.clockGeneration;out.cameraApplicationQpc=q.cameraApplicationQpc;
    out.contentTimeNs=q.contentTimeNs;out.posePredictionNs=q.posePredictionNs;out.reset=camera.camera.reset;
    // No guide, extent, readiness or evaluation claim follows from camera data.
    if(reason)*reason=SubmittedProjectionReason::Ready;
    return out;
}
uint64_t InputVram() {std::lock_guard lock(mutex);uint64_t bytes=0;for(const auto& pool:pools)for(const auto& r:pool)if(r)bytes+=r->data.bytes;return bytes;}
void ReleaseInputs() {
    std::lock_guard lock(mutex);
    const bool keepProjection=CaptureProjectionEnabled();
    for(auto& pool:pools)for(auto& r:pool)if(r && r.use_count()==1 && r->expected==r->submitted &&
        ProducerFinished(*r)) {
        // Presenter calls this while FG is inactive. Its GPU cleanup does not
        // end the separate color producer: capture can still need these exact
        // identities. CPU-only metadata is bounded by the existing four slots.
        // Separate tag-list submits can complete before Evaluate adds its bit;
        // those valid partial records still belong to the active producer too.
        if(keepProjection&&r->projectionOnly&&!r->invalid)continue;
        r.reset();
    }
}
}
