#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace cvr::framegen {
enum class EngineView:uint32_t {Unknown,Main,Vrcam};
// Publication/reservation are not draws or accepted OpenXR submissions.
// The last two stages are reserved and are not emitted by this producer patch.
enum class ProducerStage:uint32_t {Unknown,TagAdmission,GuidesSubmission,ColorPublished,VrcamReadSelection,Draw,ProjectionSubmit};
enum class ProducerReason:uint32_t {
    TagsAccepted,NoNodeScope,InvalidPoseInScope,PoseViewUnsupported,NativeFrameMissing,
    NonDirectCommandList,CameraMissing,CameraFrameMismatch,CameraPoseMismatch,CameraOriginMismatch,
    InputPoolBusy,InputQueueMismatch,InputSignalFailed,GuidesScheduled,
    CapturePoseUnverified,CaptureGuidesMissing,ColorScheduled,ColorSignalFailed,
    VrcamSlotFresh,VrcamSlotReused,VrcamSlotTooOld,VrcamSlotUnavailable,Count
};
enum ProducerValidity:uint64_t {
    NodeScopeActive=1ull<<0,EngineViewKnown=1ull<<1,EngineKeyKnown=1ull<<2,LogicalEyeKnown=1ull<<3,
    SourceIdKnown=1ull<<4,PreviousSourceIdKnown=1ull<<5,NativeFrameKnown=1ull<<6,PreviousNativeFrameKnown=1ull<<7,
    PoseIdentityValid=1ull<<8,HeadPoseValid=1ull<<9,PoseTimeValid=1ull<<10,ContentTimeValid=1ull<<11,
    CaptureSubmissionQpcValid=1ull<<12,ProducerFenceScheduled=1ull<<13,SpaceEpochKnown=1ull<<14,
    ResourceEpochKnown=1ull<<15,ColorSlotReused=1ull<<16,ColorSlotStaleRejected=1ull<<17,
    GuidesAvailable=1ull<<18,CameraConstantsComplete=1ull<<19,
    RequestedCycleModeKnown=1ull<<20,ActiveCycleModeKnown=1ull<<21,ColorReadReserved=1ull<<22,
    PoseIdKnown=1ull<<23,PoseWriteIdKnown=1ull<<24,RequestedSourceIdKnown=1ull<<25,ResourceStateKnown=1ull<<26
};
struct ProducerNodeScope {
    bool active{},viewKeyKnown{};
    EngineView engineView=EngineView::Unknown;
    uint64_t viewKey{},context{};
    uint32_t depth{},workRva{};
};
struct NodePoseEvidence {
    uint64_t poseId{},writeId{},origin{};
    uint32_t view{},nativeFrame{};
    bool headValid{},hasNativeFrame{};
};
inline thread_local ProducerNodeScope producerNodeScope;
inline ProducerNodeScope CurrentProducerNodeScope() {return producerNodeScope;}
inline ProducerNodeScope EnterProducerNodeScope(bool keyKnown,uint64_t key,int side,uint64_t context,uint32_t workRva) {
    const auto previous=producerNodeScope;
    producerNodeScope={true,keyKnown,side==0?EngineView::Main:(side==1?EngineView::Vrcam:EngineView::Unknown),
        key,context,previous.depth+1,workRva};return previous;
}
inline void RestoreProducerNodeScope(const ProducerNodeScope& previous) {producerNodeScope=previous;}

struct ProducerTraceRecord {
    uint64_t flags{},sourceId{},previousSourceId{},requestedSourceId{},poseId{},poseWriteId{},spaceEpoch{},resourceEpoch{};
    uint64_t readyFence{},readyValue{};
    int64_t captureSubmissionQpc{},captureQpcFrequency{},poseTimeNs{},contentTimeNs{};
    uint64_t engineViewKey{},poseContext{};
    ProducerStage stage=ProducerStage::Unknown;
    ProducerReason reason=ProducerReason::TagsAccepted;
    EngineView engineView=EngineView::Unknown;
    uint32_t identityView{},logicalEye{},nativeFrame{},previousNativeFrame{},nodeDepth{},nodeWorkRva{};
    // Domain and validity must be supplied by the actual mode owner. Do not
    // infer AERv2 from an interpolation fraction, FG backend or multiplier.
    uint32_t cycleModeDomain{},requestedCycleMode{},activeCycleMode{},restingState{};
};
inline ProducerTraceRecord DescribeProducerNode(const ProducerNodeScope& node,const NodePoseEvidence& pose) {
    ProducerTraceRecord out;out.stage=ProducerStage::TagAdmission;
    out.engineView=node.engineView;out.engineViewKey=node.viewKey;out.poseContext=node.context;
    out.nodeDepth=node.depth;out.nodeWorkRva=node.workRva;out.identityView=pose.view;
    out.poseId=pose.poseId;out.poseWriteId=pose.writeId;out.spaceEpoch=pose.origin;out.nativeFrame=pose.nativeFrame;
    if(node.active)out.flags|=NodeScopeActive;
    if(node.engineView!=EngineView::Unknown)out.flags|=EngineViewKnown;
    if(node.viewKeyKnown)out.flags|=EngineKeyKnown;
    if(pose.poseId)out.flags|=PoseIdKnown;
    if(pose.writeId)out.flags|=PoseWriteIdKnown;
    if(pose.hasNativeFrame)out.flags|=NativeFrameKnown;
    if(pose.headValid)out.flags|=HeadPoseValid;
    const bool valid=pose.poseId && pose.writeId && pose.headValid;
    if(valid) {out.flags|=PoseIdentityValid;if(pose.origin)out.flags|=SpaceEpochKnown;}
    if(!valid)out.reason=node.active ? ProducerReason::InvalidPoseInScope : ProducerReason::NoNodeScope;
    else if(pose.view<1 || pose.view>2)out.reason=ProducerReason::PoseViewUnsupported;
    else if(!pose.hasNativeFrame)out.reason=ProducerReason::NativeFrameMissing;
    // No clock, logical eye, source predecessor or requested/active eye-cycle
    // mode can be established from the node's engine-view identity alone.
    return out;
}
struct ProducerTraceSlot {uint64_t sequence{};ProducerTraceRecord record;};
struct ProducerTraceStorage {
    static constexpr uint32_t Capacity=64;
    uint32_t schemaVersion=1,recordSize=sizeof(ProducerTraceRecord),capacity=Capacity,slotSize=sizeof(ProducerTraceSlot);
    uint32_t slotsOffset{},countersOffset{},storageSize{},reasonCount=uint32_t(ProducerReason::Count);
    std::atomic<uint64_t> writeEpoch{},published{},attempted{},dropped{};
    std::atomic<uint32_t> writeLock{};
    std::array<std::atomic<uint64_t>,size_t(ProducerReason::Count)> counters{};
    std::array<ProducerTraceSlot,Capacity> slots{};
    // Scalar-only diagnostic storage: no GPU/resource ownership. External
    // readers accept slots only between equal, even writeEpoch reads. Atomic
    // reason counters are independent and include events dropped from slots.
    ProducerTraceStorage();
    bool Record(const ProducerTraceRecord&) noexcept;
};
static_assert(std::is_trivially_copyable_v<ProducerTraceRecord> && std::is_standard_layout_v<ProducerTraceStorage>);
static_assert(std::atomic<uint64_t>::is_always_lock_free && std::atomic<uint32_t>::is_always_lock_free);
void RecordProducerTrace(const ProducerTraceRecord&) noexcept;
}
extern "C" {
__declspec(dllexport) extern cvr::framegen::ProducerTraceStorage CyberpunkVR_FramegenProducerTrace;
}
