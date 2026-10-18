#pragma once
#include "Camera/PoseIdentity.hpp"
#include "Camera/ImagePoseLedger.hpp"
extern "C" {
__declspec(dllexport) extern std::atomic<uint64_t> CyberpunkVR_PoseIdCaptured[2];
__declspec(dllexport) extern std::atomic<uint64_t> CyberpunkVR_PoseIdCaptureMiss[2];
// Last-capture/submit arrays use physical XR eye indices, not MAIN/VRCAM roles.
__declspec(dllexport) extern std::atomic<uint64_t> CyberpunkVR_PoseIdLastCapture[2];
__declspec(dllexport) extern std::atomic<uint64_t> CyberpunkVR_PoseIdLastSubmit[2];
__declspec(dllexport) extern std::atomic<uint64_t> CyberpunkVR_PoseIdImageGeneration[2];
}

namespace cvr::camera {
using ImagePoseIdentity=ImagePoseLedger<PoseIdentity>::Image;
using ImagePoseSubmission=ImagePoseLedger<PoseIdentity>::Prepared;
std::recursive_mutex& ImageSubmissionMutex();
void ResetImagePoseList(ID3D12GraphicsCommandList* list);
void RecordImagePose(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,
                     const PoseIdentity& pose,bool stable=false);
// Caller holds ImageSubmissionMutex across these two operations and the one
// actual Execute. Reset/recording may run concurrently; Commit never rereads it.
// A failed snapshot is metadata-only fail-closed; it must not cancel Execute.
ImagePoseSubmission PrepareImagePoseLists(ID3D12CommandQueue* queue,UINT count,
                                         ID3D12CommandList* const* lists) noexcept;
void CommitImagePoseLists(ID3D12CommandQueue* queue,ImagePoseSubmission&& submission) noexcept;
ImagePoseIdentity ReadImagePose(ID3D12Resource* resource);
// Return image and submission timestamp together; recording another command
// must not make an older submitted image appear fresh.
ImagePoseIdentity LatestSubmittedVrcamImageIdentity();
ID3D12Resource* LatestSubmittedVrcamImage(); // borrowed; stable images have the capture owner's lifetime
}
