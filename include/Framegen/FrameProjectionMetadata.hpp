#pragma once
#include "Framegen/Inputs.hpp"
#include <optional>

namespace cvr::framegen {
// Owned, immutable-by-convention camera/identity metadata for a captured color.
// No Inputs Record, depth/MV resource, guide fence or capture-cell lease is kept.
// The queue reference identifies its device/producer without pinning guide pools.
struct FrameProjectionMetadata {
    CameraConstants constants;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    uint64_t poseId{},poseWriteId{},origin{},clockGeneration{};
    int64_t cameraApplicationQpc{},contentTimeNs{},posePredictionNs{};
    uint32_t frameId{},view{},srInputWidth{},srInputHeight{};
    bool srInputKnown{},reset{};
};
// Copy from the exact identity-matched Inputs while it is available. This
// snapshot does not assert guide/GPU readiness and cannot substitute for the
// strict full AcquireInputs + native adapter checks used by NVIDIA generation.
inline FrameProjectionMetadata SnapshotFrameProjection(const Inputs& input) {
    FrameProjectionMetadata out;out.constants=input.camera.constants;out.queue=input.queue;
    out.poseId=input.poseId;out.poseWriteId=input.poseWriteId;out.origin=input.origin;
    out.clockGeneration=input.clockGeneration;out.cameraApplicationQpc=input.cameraApplicationQpc;
    out.contentTimeNs=input.contentTimeNs;out.posePredictionNs=input.posePredictionNs;
    out.frameId=input.frameId;out.view=input.view;out.srInputWidth=input.width;out.srInputHeight=input.height;
    out.srInputKnown=input.submitted&&input.hasDepth&&input.width&&input.height;
    out.reset=input.camera.reset;return out;
}
std::optional<FrameProjectionMetadata> AcquireFrameProjection(unsigned view,uint32_t frameId,
    uint64_t poseId,uint64_t origin,ID3D12CommandQueue* queue);
}
