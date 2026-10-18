#pragma once
#include "Framegen/AerScenePolicy.hpp"
namespace cvr::framegen {
struct AerModeSnapshot {
    bool requested{},ready{},cameraActive{};
    uint64_t generation{};
    MainEyeSourceEpochs epochs;
    SourceDisplayFit fit;
    int64_t lastPublishedSourceId=-1,lastRequestedPoseTimeNs{},lastRuntimeTimeNs{};
    uint64_t lastSceneOrdinal{};
};
// The existing XR loop publishes runtime facts; the SDK FrameBegin sink alone
// allocates MAIN source IDs and activates the camera latch. No worker/VR thread.
void InstallAerModeOwner();
void UpdateAerModeRuntime(const AerRuntimeSnapshot&);
AerModeSnapshot ReadAerModeSnapshot();
void ObserveAerAccepted(uint64_t generation,MainEyeSourceEpochs,
                        const SourceDisplaySubmission&);
void ObserveAerNativeAccepted(uint64_t generation,MainEyeSourceEpochs,
                              const SourceDisplayContinuousSubmission&);
void StopAerModeOwner();
// Registered with the existing RED4ext native table. The matching CET selector
// reports READ-BACK disabled components for a current world-scoped request.
void RegisterAerModeNatives();
}
