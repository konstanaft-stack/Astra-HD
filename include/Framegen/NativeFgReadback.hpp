#pragma once
#include "Camera/ImagePoseIdentity.hpp"
#include <d3d12.h>

namespace cvr::framegen {
struct NativeFgReceipt;
bool NativeFgPixelsWanted() noexcept;
void RecordNativeFgReadback(ID3D12GraphicsCommandList*,ID3D12Resource* input,D3D12_RESOURCE_STATES inputState,
    ID3D12Resource* interpolated,D3D12_RESOURCE_STATES outputState,ID3D12Resource* disable,bool disableStateKnown,D3D12_RESOURCE_STATES disableState,
    const camera::ImagePoseIdentity&,uint64_t call,uint64_t recording,uint32_t count,uint32_t index,const NativeFgReceipt* paired=nullptr) noexcept;
void NativeFgReadbackReset(ID3D12GraphicsCommandList*) noexcept;
void NativeFgReadbackBeforeSubmit(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*) noexcept;
void NativeFgReadbackSubmitted(ID3D12CommandQueue*) noexcept;
void PollNativeFgReadbacks() noexcept;
}
