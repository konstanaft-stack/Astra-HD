#pragma once
#include "MenuPreview/CameraApplication.hpp"
#include "MenuPreview/EditorDrawPolicy.hpp"
#include <d3d12.h>
#include <wrl/client.h>
#include <iosfwd>
namespace cvr::menu_preview {
struct EditorUiPublication;
struct UiFrameReceipt {Metadata scope{};uint32_t eye{},nativeFrame{};uint64_t sourceSequence{},uiSubmission{},cameraRevision{};uintptr_t queue{},backBuffer{};PreviewProjection projection{};bool editorUiRequired{};std::shared_ptr<const EditorUiPublication> editorUi;std::shared_ptr<const CameraApplication> application;};
// BEFORE original Draw. A CPU scalar/binding proof, never GPU-used contents.
struct UiEditorDrawProof {
 UiEditorDrawKind kind{UiEditorDrawKind::Outside};Metadata scope{};
 uintptr_t list{},nativeNode{},targetResource{},sourceResource{},descriptorCPU{},queue{};
 uint64_t objectGeneration{},recordingGeneration{},targetGeneration{},sourceGeneration{},descriptorVersion{},qualifiedSubmission{};
 uint32_t aliasID{},nativeID{},eye{},nativeFrame{};uint64_t sourceSequence{};
 bool targetQualified{},nativeBindingKnown{},associationKnown{};
 std::shared_ptr<const void> scopeLifetime,aliasLifetime;
 std::shared_ptr<const CameraApplication> application;
 Microsoft::WRL::ComPtr<ID3D12Resource> targetOwner,sourceOwner;
};
UiEditorDrawProof InspectEditorUiDraw(ID3D12GraphicsCommandList*,UINT elements,UINT instances)noexcept;
bool AttachEditorUiPublication(ID3D12CommandQueue*,uintptr_t list,uint64_t recordingGeneration,
 uintptr_t target,uint64_t targetGeneration,std::shared_ptr<EditorUiPublication>,bool required)noexcept;
bool UiConsumerEnabled()noexcept;
bool UiRasterDiagnosticsEnabled()noexcept;
void UiRasterViewports(ID3D12GraphicsCommandList*,UINT,const D3D12_VIEWPORT*)noexcept;
void UiRasterScissors(ID3D12GraphicsCommandList*,UINT,const D3D12_RECT*)noexcept;
// 0: no current preview scope, 1: scope without its accepted alias, 2: current accepted alias.
uint32_t UiConsumerTimingScope()noexcept;
inline bool UiStereoRequested()noexcept{return UiConsumerEnabled();}
void ObserveUiOwnedAlias(const Metadata&,uintptr_t backend,uintptr_t resource)noexcept;
void RegisterUiBackBuffer(ID3D12Resource*)noexcept;
void ClearUiBackBuffers()noexcept;
bool ReadUiCanvasExtent(uint32_t& width,uint32_t& height)noexcept;
std::shared_ptr<const UiFrameReceipt> AcquireUiFrameForCapture(ID3D12Resource*,ID3D12CommandQueue*)noexcept;
bool ValidateUiFrameForCapture(const UiFrameReceipt&,ID3D12Resource*,ID3D12CommandQueue*)noexcept;
struct UiRawDrawReceipt {uint64_t pixelGeneration{},drawOrder{};uint32_t elements{},instances{},startElement{},startInstance{};int32_t baseVertex{};bool indexed{};bool operator==(const UiRawDrawReceipt&)const=default;};
struct UiProducerInputs;
bool MatchUiProducerInputs(const std::shared_ptr<const UiProducerInputs>&,const UiRawDrawReceipt&,const CameraApplication&,uintptr_t list,uintptr_t resource)noexcept;
void WriteUiProducerInputsJson(std::ostream&,const std::shared_ptr<const UiProducerInputs>&);
void ObserveUiPortraitWriter(ID3D12GraphicsCommandList*,ID3D12Resource*,std::shared_ptr<const CameraApplication>,
    const UiRawDrawReceipt* diagnosticDraw=nullptr,std::shared_ptr<const UiProducerInputs>* diagnosticInputs=nullptr)noexcept;
void UiDescriptorHeapCreated(ID3D12Device*,ID3D12DescriptorHeap*)noexcept;
struct UiDescriptorMutationData;
struct UiDescriptorMutation {std::shared_ptr<UiDescriptorMutationData> state;explicit operator bool()const noexcept{return bool(state);}};
UiDescriptorMutation BeginUiSrvWrite(D3D12_CPU_DESCRIPTOR_HANDLE)noexcept;
UiDescriptorMutation BeginUiCbvWrite(D3D12_CPU_DESCRIPTOR_HANDLE)noexcept;
void FinishUiCbvWrite(UiDescriptorMutation,const D3D12_CONSTANT_BUFFER_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE)noexcept;
UiDescriptorMutation BeginUiDescriptorOverwrite(D3D12_CPU_DESCRIPTOR_HANDLE)noexcept;
void FinishUiSrvWrite(UiDescriptorMutation,ID3D12Resource*,D3D12_CPU_DESCRIPTOR_HANDLE,bool unorderedAccess=false)noexcept;
UiDescriptorMutation BeginUiDescriptorCopy(ID3D12Device*,UINT,const D3D12_CPU_DESCRIPTOR_HANDLE*,const UINT*,UINT,const D3D12_CPU_DESCRIPTOR_HANDLE*,const UINT*,D3D12_DESCRIPTOR_HEAP_TYPE)noexcept;
void FinishUiDescriptorCopy(UiDescriptorMutation)noexcept;
void UiDescriptorHeaps(ID3D12GraphicsCommandList*,UINT,ID3D12DescriptorHeap* const*)noexcept;
void UiGraphicsRootSignature(ID3D12GraphicsCommandList*,ID3D12RootSignature*)noexcept;
void UiPipelineState(ID3D12GraphicsCommandList*,ID3D12PipelineState*)noexcept;
void UiGraphicsConstantBuffer(ID3D12GraphicsCommandList*,UINT,D3D12_GPU_VIRTUAL_ADDRESS)noexcept;
void UiVertexBuffers(ID3D12GraphicsCommandList*,UINT start,UINT count,const D3D12_VERTEX_BUFFER_VIEW*)noexcept;
void UiGraphicsRootTable(ID3D12GraphicsCommandList*,UINT,D3D12_GPU_DESCRIPTOR_HANDLE,uintptr_t nativeCaller=0)noexcept;
void UiTargets(ID3D12GraphicsCommandList*,UINT,ID3D12Resource* const*)noexcept;
void UiDrawReturned(ID3D12GraphicsCommandList*,UINT elements,UINT instances,UINT firstVertex=~0u,
                    bool indexed=false,UINT firstIndex=0,INT baseVertex=0,UINT firstInstance=0)noexcept;
void UiDispatchReturned(ID3D12GraphicsCommandList*,UINT x,UINT y,UINT z)noexcept;
// Existing CopyResource hook calls this after the original recording returns.
void UiCopyResourceReturned(ID3D12GraphicsCommandList*,ID3D12Resource* destination,ID3D12Resource* source)noexcept;
void UiResourceBarriers(ID3D12GraphicsCommandList*,UINT,const D3D12_RESOURCE_BARRIER*)noexcept;
void UiResetList(ID3D12GraphicsCommandList*)noexcept;
void UiCloseList(ID3D12GraphicsCommandList*,bool success)noexcept;
struct UiSubmissionData;
struct UiSubmission {std::shared_ptr<const UiSubmissionData> state;};
UiSubmission PrepareUiSubmission(UINT,ID3D12CommandList* const*)noexcept;
// Call after successful original ExecuteCommandLists, under the submission lock.
uint64_t UiSubmitted(ID3D12CommandQueue*,UiSubmission&&)noexcept;
// Copied planned binding only; never authorizes a UI receipt by itself.
void ObserveUiNativeBinding(uint32_t textureID,uint32_t slot,uint32_t mip,uint8_t stage,uintptr_t caller)noexcept;
}
