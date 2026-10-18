#pragma once
#include "MenuPreview/CameraApplication.hpp"
#include "Render/NativeColorSnapshots.hpp"
#include <span>
#include <optional>

namespace cvr::menu_preview {
struct UiFrameReceipt;
// An independent raw character eye, before portrait-widget composition.
// The lifetime token, not only the resource pointer, owns its pixel version.
struct EditorSceneImage {
    std::shared_ptr<const CameraApplication> application;
    cvr::render::NativeColorSnapshot pixels;
    Microsoft::WRL::ComPtr<ID3D12Fence> producerFence;
    uint64_t producerValue{},producerUiSubmission{},serial{};
    uint64_t rawExecutionSerial{},publishedAtMs{}; // real Execute proof; independent of native UI consumption
    uintptr_t queue{};
    D3D12_RESOURCE_STATES restingState=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
};
bool EditorSceneCaptureEnabled()noexcept;
std::shared_ptr<const EditorSceneImage> AcquireEditorSceneImage(const UiFrameReceipt&)noexcept;
struct EditorNativeScenePair {
    std::array<std::shared_ptr<const EditorSceneImage>,2> eyes;
    uint32_t nativeFrame{};
    uint64_t controlRevision{},origin{},bindingGeneration{},serial{};
};
std::optional<EditorNativeScenePair> AcquireEditorNativeScenePair(const Metadata& canonical)noexcept;
void ClearEditorSceneImages()noexcept;

struct EditorSceneRecording;
// Called only after the existing native role/RTV/Draw/state/app eligibility proof.
// The native-list snapshot is staging: replay may write it again.
std::shared_ptr<EditorSceneRecording> RecordEditorSceneImage(ID3D12GraphicsCommandList*,
    ID3D12Resource*,D3D12_RESOURCE_STATES,std::shared_ptr<const CameraApplication>,uint64_t recordingGeneration)noexcept;
// Before original Execute, under ImageSubmissionMutex: order replayed staging
// against an earlier final-copy read on a different queue.
void BeforeEditorSceneExecute(ID3D12CommandQueue*,
    std::span<const std::shared_ptr<EditorSceneRecording>>)noexcept;
using EditorSceneExecute=void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*);
// After original Execute: a second owned, fenced copy makes each execution
// immutable. uiSubmission is the actual UiSubmitted result, zero if untracked.
// nativeExecutionValid is ONLY the existing post-real-Execute per-record
// successful-Close/generation proof, never a UI consumption claim.
void SubmittedEditorSceneImages(ID3D12CommandQueue*,
    std::span<const std::shared_ptr<EditorSceneRecording>>,uint64_t uiSubmission,EditorSceneExecute,bool nativeExecutionValid=false)noexcept;
}
