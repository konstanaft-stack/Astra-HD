#pragma once
#include "MenuPreview/DescriptorProvenancePolicy.hpp"
#include <d3d12.h>
namespace cvr::menu_preview {
void PreviewDescriptorHeapCreated(ID3D12Device*,IUnknown*)noexcept;
uint64_t BeginPreviewDescriptorRtv(ID3D12Device*,D3D12_CPU_DESCRIPTOR_HANDLE)noexcept;
void FinishPreviewDescriptorRtv(ID3D12Device*,ID3D12Resource*,D3D12_CPU_DESCRIPTOR_HANDLE,uint64_t,const D3D12_RENDER_TARGET_VIEW_DESC* view=nullptr)noexcept;
uint64_t BeginPreviewDescriptorCopies(ID3D12Device*,UINT,const D3D12_CPU_DESCRIPTOR_HANDLE*,const UINT*,D3D12_DESCRIPTOR_HEAP_TYPE)noexcept;
void FinishPreviewDescriptorCopies(uint64_t)noexcept;
std::optional<PreviewDescriptorProof> ReadPreviewDescriptor(uintptr_t)noexcept;
bool ValidatePreviewDescriptor(const PreviewDescriptorProof&)noexcept;
}
