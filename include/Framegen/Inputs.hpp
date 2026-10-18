#pragma once
#include "Framegen/ProducerContract.hpp"
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <memory>

namespace cvr::framegen {
struct CameraData {
    float jitter[2]{},motionScale[2]{};
    float position[3]{},up[3]{},right[3]{},forward[3]{};
    float nearPlane{},farPlane{},verticalFov{},aspect{};
    bool inverted{},infinite{},jittered{},reset{},cameraMotion{},motion3d{};
    CameraConstants constants;
};
inline CameraConstants SnapshotNativeCamera(const CameraData& camera) {
    CameraConstants out;out.source=CameraConstantsSource::NativeStaging;
    auto copy=[&](unsigned first,const float* source,unsigned count) {
        for(unsigned i=0;i<count;++i) {out.values[first+i]=source[i];out.valuePresent.set(first+i);}
    };
    copy(80,camera.jitter,2);copy(82,camera.motionScale,2);
    copy(86,camera.position,3);copy(89,camera.up,3);copy(92,camera.right,3);copy(95,camera.forward,3);
    copy(98,&camera.nearPlane,1);copy(99,&camera.farPlane,1);copy(101,&camera.aspect,1);
    out.flags[0]=camera.inverted;out.flags[1]=camera.cameraMotion;
    out.flags[2]=camera.motion3d;out.flags[3]=camera.reset;
    for(unsigned i=0;i<4;++i)out.flagPresent.set(i);
    // RecordNativeCamera has no proven complete matrix layout, pinhole,
    // invalid-MV sentinel or dilated/jittered flags. Its FOV is derived from
    // one projection element, not the native SL cameraFOV field. Leave all
    // those fields unavailable; never synthesize temporal identities here.
    return out;
}
struct Inputs {
    Microsoft::WRL::ComPtr<ID3D12Resource> depth,motion;
    Microsoft::WRL::ComPtr<ID3D12Fence> readyFence;
    ID3D12CommandQueue* queue{};
    CameraData camera{};
    uint64_t poseId{},origin{},fenceValue{},bytes{};
    uint64_t poseWriteId{},clockGeneration{};
    int64_t cameraApplicationQpc{},contentTimeNs{},posePredictionNs{};
    uint32_t frameId{},view{},width{},height{},motionWidth{},motionHeight{};
    bool hasDepth{},hasMotion{},submitted{},evaluated{};
};
bool ReadRenderFrameIndex(uint32_t* index);
void RecordNativeCamera(void* state,unsigned view);
// Called by the existing Streamline tag hook while the supplied list owns the resources.
void RecordTags(const void* viewport,const void* tags,uint32_t count,void* commandList);
bool InstallInputHooks();
// Fixed-size, strongly owned snapshot of only the records belonging to this
// Execute call. Later Reset/recording cannot replace its source generation.
struct InputSubmission {
    InputSubmission()=default;
    InputSubmission(InputSubmission&&)=default;
    InputSubmission& operator=(InputSubmission&&)=default;
    InputSubmission(const InputSubmission&)=delete;
    InputSubmission& operator=(const InputSubmission&)=delete;
    static constexpr size_t Capacity=8; // Two source-view pools, four records each.
    std::array<std::shared_ptr<const Inputs>,Capacity> records{};
    std::array<uint64_t,Capacity> generations{};
    std::array<uint8_t,Capacity> masks{};
    size_t count{};
};
InputSubmission PrepareInputSubmission(UINT count,ID3D12CommandList* const* lists);
void Submitted(InputSubmission&,ID3D12CommandQueue* queue);
void ResetList(ID3D12GraphicsCommandList* list);
std::shared_ptr<const Inputs> AcquireInputs(unsigned view,uint32_t frameId,
    uint64_t poseId,uint64_t origin,ID3D12CommandQueue* queue);
uint64_t InputVram();
void ReleaseInputs();
}
