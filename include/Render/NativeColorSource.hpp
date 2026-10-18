#pragma once
#include "Camera/ImagePoseIdentity.hpp"
#include "Render/NativeColorPolicy.hpp"
#include <wrl/client.h>

namespace cvr::render {
struct NativeColorCapture {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    Microsoft::WRL::ComPtr<IUnknown> lifetime;
    D3D12_RESOURCE_DESC description{};
    camera::PoseIdentity pose;
    D3D12_RESOURCE_STATES state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    uintptr_t window{},queue{};
    uint64_t selectionGeneration{};
    bool stateKnown{};
    explicit operator bool()const{return resource && stateKnown && pose && pose.mainEyeSource;}
};
using NativeColorSubmission=camera::ImagePoseLedger<NativeColorCapture>::Prepared;
bool NativeColorSourceEnabled(); // process-local diagnostic bypass; enabled by default
void NativeColorUnknownPoseDiagnostic(ID3D12GraphicsCommandList*,ID3D12Resource*,uint32_t node,uintptr_t viewKey);
void NativeColorBackbuffers(std::span<const uintptr_t> buffers);
void NativeColorTarget(ID3D12GraphicsCommandList*,ID3D12Resource*,const NativeColorDescription&,
    const camera::PoseIdentity&);
void NativeColorBarriers(ID3D12GraphicsCommandList*,UINT,const D3D12_RESOURCE_BARRIER*);
void NativeColorReset(ID3D12GraphicsCommandList*);
NativeColorSubmission PrepareNativeColor(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*) noexcept;
void CommitNativeColor(ID3D12CommandQueue*,NativeColorSubmission&&) noexcept;
// Capture caller serializes selection/revalidation with actual queue submission.
NativeColorCapture AcquireNativeColor(ID3D12Resource* window);
bool ValidateNativeColor(const NativeColorCapture&,ID3D12CommandQueue*,const camera::ImagePoseIdentity&);
}
