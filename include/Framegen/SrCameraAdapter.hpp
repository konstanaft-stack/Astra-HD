#pragma once
#include "Camera/AppliedEyeReference.hpp"
#include "Framegen/MainEyeSourceLatch.hpp"
#include "Framegen/ProducerContract.hpp"
#include "Framegen/SrTemporalConstants.hpp"

namespace cvr::framegen::sr {
// Original SL token, native renderer frame and our source ordinal are separate.
// Zero native/token values are valid only when their corresponding known bit is set.
struct CameraSourceKey {
    MainEyeSourceReceipt source;
    uint64_t resourceEpoch{},captureClockGeneration{},historyEpoch{},origin{},poseId{},writeId{};
    uint64_t featurePairKey{},parametersKey{};
    int64_t locatedPoseTimeNs{};
    uint32_t nativeFrame{},slToken{},viewport{},engineView{};
    bool nativeFrameKnown{},slTokenKnown{},viewportKnown{};
    bool operator==(const CameraSourceKey&) const=default;
};
struct AppliedEyeSourceProof {
    // The caller copies this with the same exact PoseIdentity that supplied SL
    // metadata. No latest globals or unlabelled delta may be attached here.
    CameraSourceKey key;
    camera::PoseFingerprint selectedFingerprint;
    camera::AppliedEyeReference reference;
};
struct NgxCameraInputs {
    // Values from this evaluation's actual NGX inputs, not SL-normalized MV
    // scale, render-window dimensions, or inferred per-eye defaults.
    bool known{};
    U2 depthSize{},motionSize{},renderSubrect{},outputSize{};
    V2 jitterPixels{},mvScale{};
    bool lowResMv{},specularMvPresent{},linearDepth{};
};
struct CameraSample {
    CameraSourceKey key;
    CameraConstants original;
    NgxCameraInputs ngx;
    AppliedEyeSourceProof appliedEye;
};
enum class AdapterStatus {
    Ready,Disabled,UnknownConfiguration,UnknownSource,UnknownNgxInputs,
    MissingEyeProof,EyeProofMismatch,MissingConstants,InvalidConstants,
    UnsupportedFlags,UnsupportedProjection,InvalidProjection,NonRigidTransform,
    CameraBasisMismatch,HistoryMismatch,ResetBoundary,BuilderRejected
};
struct AdapterDiagnostics {
    // Index order is oldest, previous, current, matching the input array.
    std::array<double,3> writerBasisResidual{},nativeBasisDeterminant{};
    std::array<double,3> projectionNear{},projectionFar{},rawNear{},rawFar{},fovYRadians{};
    std::array<double,2> affineResidual{},orthogonalResidual{},determinantResidual{};
    std::array<std::array<double,3>,3> shaderLocalEyeDelta{};
    std::array<M4,2> shaderRelative{}; // [0]=current->previous, [1]=previous->oldest
};
struct AdapterResult {
    Constants constants{};
    History history;
    AdapterDiagnostics diagnostics;
    AdapterStatus status=AdapterStatus::UnknownConfiguration;
    Status builderStatus=Status::NeedHistory;
    std::array<uint32_t,2> reserved{};
};
// Pure value adapter. The owner must advance historyEpoch for every observed
// viewport reset, including Constants without an AER receipt, and must supply
// chronological consecutive samples. This function has no history/resource owner.
AdapterResult AdaptCameraHistory(const std::array<CameraSample,3>& oldestToCurrent,
                                 const Parameters& parameters) noexcept;
}
