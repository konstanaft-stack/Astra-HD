#pragma once
#include "Runtimes/OpenXRManager.hpp"
#include "Camera/PoseAddressLedger.hpp"
#include "Camera/AppliedEyeReference.hpp"
#include "Framegen/MainEyeSourceLatch.hpp"
#include <cstdint>
extern "C" {
__declspec(dllexport) extern std::atomic<uint64_t> CyberpunkVR_PoseIdFinal[2];
__declspec(dllexport) extern std::atomic<uint64_t> CyberpunkVR_PoseIdFinalMiss[2];
__declspec(dllexport) extern std::atomic<uint64_t> CyberpunkVR_PoseIdVrikSource;
__declspec(dllexport) extern std::atomic<uint64_t> CyberpunkVR_PoseIdVrikInputChanged;
}

namespace cvr::camera {
// Shared render-camera optics. Does not modify the tracked body/hand sample.
bool ApplyReferenceCameraOptics(OpenXRHeadPose& head);
struct PoseIdentity {
    uint64_t poseId{},writeId{};
    uintptr_t component{};
    uint32_t view{};
    OpenXRHeadPose head{};
    XrPosef localHead{};
    // Rendering stamp, populated only by the native node dispatcher. It is not
    // a pose-sample id: several rendered frames can use the same tracked pose.
    uint32_t renderFrameId{};
    bool hasRenderFrameId{};
    bool externalView{}; // selected MAIN is a detached camera, not the player's placed FPP
    // Content time means this camera-application publication event mapped
    // from QPC to XrTime, as in the existing native CameraBridge. It is not a
    // GPU-completion time, capture-submit time or engine simulation tick.
    int64_t cameraApplicationQpc{},contentTimeNs{},posePredictionNs{};
    uint64_t clockGeneration{};
    // Present only when this actual MAIN camera write used an owner-latched
    // physical eye. Source ID is not renderFrameId, poseId or capture serial.
    framegen::MainEyeSourceReceipt mainEyeSource{};
    // Optional actual centre/eye write evidence, sharing this exact source and
    // write identity. Legacy callers and unproved camera blends leave it unknown.
    AppliedEyeReference appliedEyeReference{};
    explicit operator bool() const { return poseId && writeId && head.valid; }
};
using PoseReceipt=PoseAddressLedger<PoseIdentity>::Receipt;

uint64_t NextPoseIdentity();
void InvalidatePoseAddress(uintptr_t address);
PoseReceipt CapturePoseAddress(uintptr_t address);
bool TransferPoseAddress(const PoseReceipt& source,uintptr_t destination);
void PublishComponentPose(uintptr_t component,uint32_t view,uint64_t poseId,
                          const OpenXRHeadPose& head,const framegen::MainEyeSourceReceipt& mainSource={},
                          const AppliedEyeReference& appliedEyeReference={});
void PublishCameraSetupPose(uintptr_t address,uintptr_t component,uint32_t view,uint64_t poseId,
                            const OpenXRHeadPose& head,bool externalView=false,const framegen::MainEyeSourceReceipt& mainSource={},
                            const AppliedEyeReference& appliedEyeReference={});
// Mode owner only. No caller is installed here: actual scene FrameBegin,
// lifecycle epochs and VRCAM-disable proof must come from the native owner.
uint64_t RequestMainEyeSource(bool enabled);
bool PublishMainEyeSceneFrame(uint64_t requestRevision,const framegen::MainEyeSceneFrame& frame,
    int64_t sourceId,framegen::AerV2Eye eye,const framegen::VrcamOffProof& off,
    const framegen::MainEyeClockSnapshot& snapshot);
void InvalidateMainEyeSource();
bool MainEyeSourceRequested();
std::optional<framegen::MainEyeSourceReceipt> AcquireMainEyeSourceForWrite(uint64_t hmdWorldEpoch);
bool ValidateMainEyeSourceReceipt(const framegen::MainEyeSourceReceipt& receipt);
// Native MainRead only: invalidate the old latch/history when the selected
// camera provably moves between the component and scene writer. Missing/stale
// provenance rejects this selection without claiming a camera transition.
bool AdmitMainEyeSelectedCamera(const PoseReceipt& selected,uintptr_t selectedSetup=0);
// Actual source-time query, once per current receipt; serialized copies only
// read the immutable saved result and never re-locate an older source.
bool AcquireMainEyeSourcePose(const framegen::MainEyeSourceReceipt&,OpenXRHeadPose*,
    roomscale::Vec2* consumed,uint64_t* poseId);
bool ReadMainEyeSourcePose(const framegen::MainEyeSourceReceipt&,OpenXRHeadPose*,
    roomscale::Vec2* consumed,uint64_t* poseId);
std::optional<framegen::MainEyeSourceFaultScope> CaptureMainEyeSourceFaultScope(uint64_t worldEpoch,uint64_t ordinal);
bool RevokeMainEyeSourceFault(const framegen::MainEyeSourceFaultScope&);
void PublishSerializedPose(uintptr_t destination,const PoseReceipt& source);
// Exact component whose native SerializeSetup call contains LocateCamera.
uintptr_t CurrentSerializedCameraComponent();
struct WeightedPoseReceipt { PoseReceipt source;float weight{}; };
bool PublishBlendedPose(uintptr_t destination,std::span<const WeightedPoseReceipt> inputs);
bool ReadCameraPoseIdentity(uintptr_t camera,uint32_t view,PoseIdentity* out);
bool ReadContextPoseIdentity(uintptr_t context,PoseIdentity* out);

// Populated at node entry, restored at node exit. Copies after node exit pass
// their saved value explicitly instead of reading an unrelated parent scope.
PoseIdentity CurrentNodePoseIdentity();
PoseIdentity SetNodePoseIdentity(const PoseIdentity& pose);
}
