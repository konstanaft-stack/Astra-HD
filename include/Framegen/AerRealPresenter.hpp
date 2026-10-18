#pragma once
#include "Framegen/Presenter.hpp"
#include "Framegen/AerV2Policy.hpp"
#include "Framegen/PairedGeometryDiagnostics.hpp"
#include "Render/AerReconstructionPass.hpp"
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>

namespace cvr::framegen {
enum class AerRealCode {
    Ready, NotReady, InvalidInput, UnsupportedProjection, UnsupportedCant,
    MissingCapturedHead, CapturePending, DeviceRemoved, EpochMismatch,
    Ineligible, Duplicate, MissingSourceLease
};
struct AerRealGeometry {
    XrPosef capturedHead{}, eyeFromHead{};
    XrFovf renderedFov{}, effectiveFov{};
    std::array<float,4> crop{}; // Source texture coordinates: L,T,R,B.
    std::array<float,4> tangents{}; // Effective cropped U,D,L,R magnitudes.
    uint32_t width{},height{};
};
struct AerRealCandidate {
    Frame frame; // Exactly one MAIN physical eye; transferred by value to Step.
    MainEyeSourceReceipt receipt;
    AerRealGeometry geometry;
    Microsoft::WRL::ComPtr<ID3D12Resource> colorOwner;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> producerQueue;
};
struct AerRealCandidateResult {
    AerRealCode code=AerRealCode::InvalidInput;
    std::optional<AerRealCandidate> candidate;
    explicit operator bool()const{return code==AerRealCode::Ready&&candidate.has_value();}
};
// One synchronous post-preflight view, before display admission. References
// and pointers are borrowed for this callback only; no batch owner is retained.
struct AerReadyIngress {
    uint64_t generation{},sourceGeneration{},calibrationRevision{},resourceEpoch{};
    uint64_t requestRevision{},latchRevision{},sourceClockGeneration{},captureClockGeneration{};
    MainEyeSourceEpochs epochs;
    std::span<const AerRealCandidate* const> candidates;
};
using AerReadyObserver=std::function<void(const AerReadyIngress&)>;
struct AerRealTick {
    int64_t frame=-1;
    int32_t appliedDelay=0;
    int64_t sourceUpper=-1;
    uint64_t generation=0; // Authoritative AerModeOwner generation, not a source ID.
    MainEyeSourceEpochs epochs;
};
struct AerRealEye {
    Frame frame;
    MainEyeSourceReceipt receipt;
    AerRealGeometry geometry;
    Microsoft::WRL::ComPtr<ID3D12Resource> colorOwner;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> producerQueue;
};
struct AerRealSelection {
    AerRealCode code=AerRealCode::NotReady;
    AerRealCode candidateCode=AerRealCode::NotReady;
    std::array<std::shared_ptr<const AerRealEye>,2> eyes;
    std::array<int64_t,2> sourceIds{-1,-1};
    std::array<bool,2> newlyAccepted{};
    std::array<XrPosef,2> submitPose{};
    std::array<XrFovf,2> fov{};
    std::array<cvr::render::AerWarpParams,2> warp{};
    XrPosef referenceHead{}; // Last ADMITTED source HEAD, in callback order.
    MainEyeSourceReceipt referenceReceipt;
    uint64_t generation{},sourceGeneration{},calibrationRevision{},resourceEpoch{};
    uint64_t sourceClockGeneration{},captureClockGeneration{};
    MainEyeSourceEpochs epochs;
    AerV2Step policyStep;
    explicit operator bool()const{return code==AerRealCode::Ready&&eyes[0]&&eyes[1];}
    // Retain this in AerReconstructionPass::sourceOwner through exact GPU
    // completion. It holds the capture ReadLease, not merely a COM AddRef.
    std::shared_ptr<void> OwnerForEye(uint32_t eye)const;
};
struct AerNativePair {
    std::shared_ptr<NativeSelectionLease> owner;
    std::array<ID3D12Resource*,2> color{};
    std::array<XrPosef,2> submitPose{};
    std::array<XrFovf,2> fov{};
    std::array<cvr::render::AerWarpParams,2> warp{};
    std::array<CaptureTransfer,2> transfer{};
    std::array<bool,2> generated{};
    std::array<int64_t,2> contentTimeNs{};
    // Populated ONLY for a validated all-real alternating (+1 namespace) pair.
    std::array<int64_t,2> aerSourceIds{-1,-1};
    // OWN-adapter continuous timing coordinate; never an integer AER identity.
    // G uses the validated vendor ordinal fraction between original sources.
    std::array<double,2> sourcePositions{};
    uint64_t pairId{},bridgeEpoch{},generation{},sourceGeneration{};
    bool allReal{};
};
// Pure validation/metadata mapping. Caller supplies CURRENT bridge HistoryEpoch
// after the same mode/lifecycle checks as baseline; it is NOT resourceEpoch.
// Full-image NVIDIA products keep the validated per-eye crop and effective FOV.
// No current HMD query, GPU wait, clock observation,
// or invented origins. NativeReadScope still owns generated resource barriers.
std::optional<AerNativePair> BuildAerNativePair(const AerRealSelection& baseline,
    const std::shared_ptr<NativeSelectionLease>&,uint64_t currentBridgeEpoch);
// Single-owner metadata/admission component. No XR calls, queue submission,
// generation, clock observation, or GPU waits. Owns a bounded queue of already
// completed sources, including their real capture leases.
// Call AFTER leaving the capture m_presentMutex: rejected Frame destruction
// invokes the owner's ReadLease release callback and may acquire that mutex.
class AerRealPresenter {
public:
    // Same measured native projection/crop contract for a paired camera view.
    static bool ResolveCapturedGeometry(const CameraConstants&,const cvr::ppd::ReferenceOptics&,
        unsigned eye,AerRealGeometry&);
    // Paired real-eye display may show the game's current, centered uniform
    // zoom while keeping the calibrated source crop and physical XR lens FOV.
    // AER source admission and PPD baseline calibration remain strict.
    // On failure neither output is modified. This does not wait for the GPU.
    static bool ResolvePairedDisplayGeometry(const Frame&,
        std::array<AerRealGeometry,2>&,bool& baselineCalibration,PairedGeometryDiagnostic* diagnostic=nullptr);
    // Uses stamp.capturedHead and an exact Frame.projection value snapshot.
    // Legacy Inputs may supply that snapshot, but are dropped before retention.
    // Frame.fov is deliberately not a source-projection fallback. Empty crop
    // selects the full MAIN CopyResource image. Unsupported canted eye offsets
    // or nonlinear/unknown projections produce explicit failure, no fake pose.
    static AerRealCandidateResult FromMainFrame(Frame frame,AerV2Eye physicalEye,
        std::optional<std::array<float,4>> crop={});
    static constexpr float CalibrationAngularTolerance=.00002f; // radians
    static constexpr float CalibrationPositionTolerance=.00002f; // metres
    static constexpr float CalibrationCropTolerance=.001f; // source pixels
    static bool SameCalibration(const AerRealGeometry&,const AerRealGeometry&);
    AerRealSelection Step(const AerRealTick&,std::optional<AerRealCandidate> candidate={});
    static constexpr size_t MaxReadyCandidates=8;
    static constexpr size_t SourceSlotsPerEye=3; // Original26.03 mode0 ring capacity.
    // Consumes the bounded, already GPU-ready batch. Scope/calibration validation
    // and resets finish BEFORE the original policy callbacks choose the first
    // eligible R/L source. Future ready images retain their actual ReadLease in
    // a FIFO bounded to three total slots per eye, including its held image.
    // Obsolete/incompatible candidates are released; this component never waits.
    // An invalid tick or oversized span is rejected before moving entries;
    // the caller still owns that rejected batch and must clear it.
    // Optional ingress observes all post-filter ready sources, unique by source
    // ID in ascending order (newest capture serial wins a duplicate ID). It may
    // repeat sources on later ticks; backend ingress owns cross-tick dedup.
    // It runs once for a nonempty batch before policy.Step, after scope/reset.
    // The observer must
    // not re-enter/mutate this presenter or retain borrowed pointers. Exceptions
    // propagate before any display admission, releasing incoming candidates
    // while preserving the still-valid pre-existing queue.
    AerRealSelection StepReady(const AerRealTick&,std::span<AerRealCandidate> ready,const AerReadyObserver& observer={});
    void Reset();
    // Revoke obsolete held/queued leases even before a replacement capture can
    // be produced. The producer may be waiting for those very pool cells.
    void ConstrainSourceScope(const MainEyeSourceReceipt&,uint64_t captureClockGeneration);
    uint32_t HeldEyeCount()const;
    uint32_t QueuedEyeCount(uint32_t eye)const;
    const AerV2State& PolicyState()const{return policy_.State();}
private:
    void ClearSources(bool clearObservedCalibration=true);
    AerV2Policy policy_;
    std::array<std::shared_ptr<const AerRealEye>,2> eyes_;
    std::array<std::optional<AerRealCandidate>,SourceSlotsPerEye*2> pending_;
    XrPosef referenceHead_{};
    MainEyeSourceReceipt referenceReceipt_;
    uint64_t generation_{},sourceGeneration_=1,resourceEpoch_{};
    uint64_t calibrationRevision_=1;
    std::array<std::optional<AerRealGeometry>,2> acceptedCalibration_,observedCalibration_;
    uint64_t requestRevision_{},latchRevision_{},sourceClockGeneration_{},captureClockGeneration_{};
    int64_t calibrationSourceFloor_=-1;
    MainEyeSourceEpochs epochs_;
};
} // namespace cvr::framegen
