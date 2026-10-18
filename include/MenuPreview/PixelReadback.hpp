#pragma once
#include "MenuPreview/CameraApplication.hpp"
#include "MenuPreview/DescriptorProvenancePolicy.hpp"
#include "MenuPreview/EditorSceneImage.hpp"
#include <d3d12.h>
#include <vector>
namespace cvr::menu_preview {
struct PreviewPixelFlight;
// Diagnostic order only. These records do not admit a state or a GPU writer.
struct PreviewPixelObservation {uintptr_t list{},resource{};uint64_t generation{},recordOrder{};uint32_t kind{},before{},after{},flags{},subresource{},listIndex{},frame{},eye{};uint64_t source{};uint32_t elements{},instances{},startElement{},startInstance{};int32_t baseVertex{};};
struct EditorScenePixelRecording {uintptr_t list{};uint64_t generation{};std::shared_ptr<EditorSceneRecording> recording;bool preparedValid{};};
struct PreviewPixelSubmission {std::vector<std::shared_ptr<PreviewPixelFlight>> flights;std::vector<PreviewPixelObservation> observations;std::array<EditorScenePixelRecording,128> editorScenes{};uint32_t editorSceneCount{};uint64_t batch{};};
bool PreviewPixelsRequested()noexcept;
// Resource/RTV identity may be created before a preview is opened.
bool PreviewMetadataRequested()noexcept;
// CPU writer/descriptor observation is also needed for stereo UI without a raw readback.
bool PreviewObservationRequested()noexcept;
bool LockPreviewPixelLifecycle()noexcept;
void UnlockPreviewPixelLifecycle(bool)noexcept;
// Scalar diagnostic context only; never used by writer/descriptor admission.
struct PreviewPixelNodeDiagnostic {uintptr_t context{},view{};uint64_t key{};uint32_t frame{},epoch{},setup{},eye{};bool nativeDual{};};
size_t BeginPreviewPixelNode(std::shared_ptr<const CameraApplication>,uint32_t work,bool role,bool expectedKnown=false,uintptr_t expectedHandle=0,PreviewPixelNodeDiagnostic diagnostic={});
void EndPreviewPixelNode();
void RestorePreviewPixelNode(size_t);
void PreviewPixelTargetFromDescriptor(ID3D12GraphicsCommandList*,const PreviewDescriptorProof&);
void PreviewPixelTargetsBegin(ID3D12GraphicsCommandList*,UINT,uintptr_t actualHandle=0);
void PreviewPixelDrawReturned(ID3D12GraphicsCommandList*,bool indexed,UINT elements,UINT instances,UINT startElement,INT baseVertex,UINT startInstance);
void PreviewPixelViewports(ID3D12GraphicsCommandList*,UINT,const D3D12_VIEWPORT*)noexcept;
void PreviewPixelScissors(ID3D12GraphicsCommandList*,UINT,const D3D12_RECT*)noexcept;
void PreviewPixelBarriers(ID3D12GraphicsCommandList*,UINT,const D3D12_RESOURCE_BARRIER*);
void ResetPreviewPixelList(ID3D12GraphicsCommandList*);
void ClosePreviewPixelList(ID3D12GraphicsCommandList*,bool success);
void ForgetPreviewPixelObject(uintptr_t,bool list);
PreviewPixelSubmission PreparePreviewPixels(UINT,ID3D12CommandList* const*,ID3D12CommandQueue* queue=nullptr)noexcept;
void SubmittedPreviewPixels(ID3D12CommandQueue*,PreviewPixelSubmission&&,EditorSceneExecute execute=nullptr,uint64_t uiSubmission=0)noexcept;
void PollPreviewPixels()noexcept;
}
