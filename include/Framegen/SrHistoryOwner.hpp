#pragma once
#include "Framegen/SrCameraAdapter.hpp"
#include "Framegen/SrNativeApi.hpp"
#include <functional>
#include <span>
#include <wrl/client.h>

namespace cvr::framegen {
// Value snapshot captured BEFORE the original SL call. No resource/history
// epoch is invented by the hook; SrHistoryOwner adds its own epochs later.
struct SrSourceStamp {
    MainEyeSourceReceipt source;
    uint64_t poseId{},writeId{},origin{},captureClockGeneration{};
    int64_t locatedPoseTimeNs{};
    uint32_t nativeFrame{},engineView{};
    bool poseKnown{},nativeFrameKnown{};
    camera::AppliedEyeReference appliedEye;
    camera::PoseFingerprint selectedFingerprint;
    bool operator==(const SrSourceStamp&) const=default;
};
struct SrConstantsObservation {
    CameraConstants constants;
    SrSourceStamp source;
    uint32_t viewport{},slToken{},originalResult{}; // SL eOk == 0
    bool viewportKnown{},tokenKnown{},resultKnown{};
};
struct SrSnapshotTag {
    ID3D12Resource* resource{}; // borrowed original SL tag interface
    uint32_t type{},state{},x{},y{},width{},height{},lifecycle{};
    bool known{};
};
// Root supplies the existing slGetNativeInterface resolver. On success it
// QueryInterfaces the canonical native object to iid and transfers one AddRef.
// Failure is UNKNOWN, never permission to match only dimensions or raw wrappers.
using SrNativeResolver=std::function<HRESULT(IUnknown*,REFIID,void**)>;
enum class SrSlStateContract:uint32_t {Unknown,StreamlineOriginalEvaluate};
struct SrSlEvaluationObservation {
    SrSourceStamp source;
    uint32_t viewport{},slToken{},feature{};
    bool viewportKnown{},tokenKnown{},featureKnown{};
    ID3D12GraphicsCommandList* commandList{}; // borrowed SL argument; OWNER resolves
    SrSlStateContract stateContract=SrSlStateContract::Unknown;
};
struct SrTagCaptureToken {uint64_t value{};explicit operator bool()const{return value!=0;}};
struct SrSlEvaluationToken {uint64_t value{};explicit operator bool()const{return value!=0;}};
struct SrSubmissionToken {uint64_t value{};explicit operator bool()const{return value!=0;}};
struct SrOptionalOverridePolicy {
    // Explicit verified policy; defaults do not assert knowledge of Luke's
    // optional SpecularMotionVectors or Color replacement branches.
    bool known{},replaceSpecularMotion{},replaceColor{};
};
enum class SrHistoryStage:uint32_t {
    Disabled,UnknownConfiguration,Unpaired,UnknownSource,UnknownNativeIdentity,
    UnknownSlScope,MissingSnapshot,PendingProducer,HistoryWarmup,AdapterRejected,
    PoolBusy,NativeParametersUnknown,Recorded,NativeFailed,SubmissionFailed,Retiring,Retired
};
struct SrHistoryStatus {
    SrHistoryStage stage=SrHistoryStage::Disabled;
    int64_t sourceId=-1;
    uint64_t historyEpoch{},resourceEpoch{},pairedCreates{},cloneFailures{};
    uint64_t capturedSources{},correctedEvaluations{},passthroughEvaluations{},resetEvents{};
    uint64_t missingNativeParameters{},crossQueueWaits{},submissionFailures{};
    std::array<uint64_t,2> successfulNativeByEye{};
    uintptr_t lastSelectedHandle{};uint32_t lastPhysicalEye=UINT32_MAX;
    int32_t lastReset=-1;float lastDeltaMsec{};
    bool lastResetKnown{},lastDeltaKnown{};
    uint32_t snapshotCells{},pendingCaptures{},pendingProducers{},livePasses{},livePairs{},resetEyeMask{};
    sr::AdapterStatus adapterStatus=sr::AdapterStatus::UnknownSource;
    NVSDK_NGX_Result lastNativeResult=NVSDK_NGX_Result_Success;
    NVSDK_NGX_Result lastCompanionResult=NVSDK_NGX_Result_Success;
    HRESULT lastSubmissionResult=S_OK;
    bool requested{},quiescing{};
};
struct SrFrameGenerationGuide {
    // Chronological N-2, N-1, N. Only N-2 and N share a physical eye.
    // The existing verified SR pass has combined the two motion steps.
    std::array<sr::CameraSample,3> history;
    SrSourceStamp previous,current;
    Microsoft::WRL::ComPtr<ID3D12Resource> motion,depth;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    uint64_t fenceValue{};
    bool reset{};
    // Keep both resources immutable until the consumer's GPU completion.
    // The resource AddRefs alone would not prevent pool overwrite.
    std::shared_ptr<void> motionLease,depthLease;
};
using SrFrameGenerationSourceReader=std::function<SrSourceStamp(const NVSDK_NGX_Parameter*)>;
struct NativeFgReceipt;
using SrFrameGenerationReporter=std::function<void(ID3D12GraphicsCommandList*,const NVSDK_NGX_Handle*,const NativeFgReceipt&)>;
class SrHistoryOwner final:public SrNativeHandler {
public:
    static constexpr uint32_t SnapshotCapacity=20,PassCapacity=4;
    explicit SrHistoryOwner(SrNativeResolver);
    ~SrHistoryOwner() override;
    SrHistoryOwner(const SrHistoryOwner&)=delete;
    SrHistoryOwner& operator=(const SrHistoryOwner&)=delete;
    void SetRequested(bool);
    // Configure before host feature creation; an empty reader preserves passthrough.
    bool ConfigureFrameGeneration(SrFrameGenerationSourceReader,SrFrameGenerationReporter={});
    void SetConfiguration(const sr::Parameters&,SrOptionalOverridePolicy);
    void ObserveConstants(const SrConstantsObservation&);
    // All public list/queue/resource arguments are original borrowed interfaces;
    // only the owner applies the injected canonical resolver at every boundary.
    // Before original slSetTag: source tag.state still describes the source.
    // viewport is the actual slSetTag argument; tags have no SL token argument.
    SrTagCaptureToken BeginTags(ID3D12GraphicsCommandList*,uint32_t viewport,bool viewportKnown,
                               const SrSourceStamp&,std::span<const SrSnapshotTag>);
    // Observed full CopyResource inside this exact original SL SetTag call.
    // A descriptor match alone never establishes provenance. Only depth/MV
    // clones of this source on this command-list generation are admitted.
    void ObserveTaggedCopy(SrTagCaptureToken,ID3D12GraphicsCommandList*,ID3D12Resource* destination,ID3D12Resource* source);
    void EndTags(SrTagCaptureToken,uint32_t originalResult,bool resultKnown=true);
    // TLS scope encloses the ORIGINAL SL Evaluate. Native prepass is admitted
    // only inside this matching scope, where SL captured/restores host bindings.
    SrSlEvaluationToken BeginSlEvaluation(const SrSlEvaluationObservation&);
    void EndSlEvaluation(SrSlEvaluationToken,uint32_t originalResult,bool resultKnown=true);
    // Existing hook calls BEFORE actual Execute, outside ImageSubmissionMutex.
    // Only recorded, successfully signaled producer dependencies are queued.
    // A Wait failure stops new owner admissions and retains all lifetime; the
    // hook MUST STILL call original Execute (there is no safe command rollback).
    SrSubmissionToken BeforeSubmit(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*);
    // Existing hook calls AFTER actual original ExecuteCommandLists returned.
    void Submitted(SrSubmissionToken,ID3D12CommandQueue*,UINT,ID3D12CommandList* const*);
    // Only after successful native Reset/discard. Never cancel submitted work.
    void ResetList(ID3D12GraphicsCommandList*);
    void Collect();
    SrHistoryStatus Status() const;
    std::optional<SrFrameGenerationGuide> AcquireFrameGenerationGuide(const SrSourceStamp&);
    NVSDK_NGX_Result Create(const SrNativeApiContext&,ID3D12GraphicsCommandList*,NVSDK_NGX_Feature,
                           NVSDK_NGX_Parameter*,NVSDK_NGX_Handle**) override;
    NVSDK_NGX_Result Evaluate(const SrNativeApiContext&,ID3D12GraphicsCommandList*,const NVSDK_NGX_Handle*,
                             const NVSDK_NGX_Parameter*,PFN_NVSDK_NGX_ProgressCallback) override;
    NVSDK_NGX_Result Release(const SrNativeApiContext&,NVSDK_NGX_Handle*) override;
    SrNativeRetireResult BeforeHostShutdown() override;
private:
    NVSDK_NGX_Result EvaluatePairedFrameGeneration(const SrNativeApiContext&,ID3D12GraphicsCommandList*,const NVSDK_NGX_Handle*,const NVSDK_NGX_Parameter*,PFN_NVSDK_NGX_ProgressCallback,const SrSourceStamp&);
    NVSDK_NGX_Result EvaluateFrameGeneration(const SrNativeApiContext&,ID3D12GraphicsCommandList*,const NVSDK_NGX_Handle*,const NVSDK_NGX_Parameter*,PFN_NVSDK_NGX_ProgressCallback);
    struct Impl;std::unique_ptr<Impl> m_impl;
};
}
