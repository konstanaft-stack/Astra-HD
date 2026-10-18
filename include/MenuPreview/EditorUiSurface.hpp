#pragma once
#include "MenuPreview/Scope.hpp"
#include "Render/HudPanel.hpp"
#include <d3d12.h>
#include <memory>

namespace cvr::menu_preview {
struct UiEditorDrawProof;
// One independent UI image, never a pair of eye images. The image retains its
// one source owner until the explicit quad's copy has finished.
struct EditorUiPublication {
    Metadata scope{};
    std::shared_ptr<cvr::hud::Frame> image;
    float renderAspect{};
    uint64_t sourceSequence{},cameraRevision{};
    uint32_t nativeFrame{},eye{};
};
bool EditorUiSeparationEnabled()noexcept;
// Only our private copy context: its lifecycle must not enter game/pixel locks.
bool EditorUiPrivateCommands()noexcept;
class EditorUiPrivateCommandScope {
    bool previousInternal{};
public:
    EditorUiPrivateCommandScope()noexcept;
    ~EditorUiPrivateCommandScope();
    EditorUiPrivateCommandScope(const EditorUiPrivateCommandScope&)=delete;
    EditorUiPrivateCommandScope& operator=(const EditorUiPrivateCommandScope&)=delete;
};
struct EditorUiDrawRoute {
    D3D12_CPU_DESCRIPTOR_HANDLE original{};
    std::shared_ptr<const void> lifetime;
    bool redirected{};
};
void ObserveEditorUiTargets(ID3D12GraphicsCommandList*,UINT,
    const D3D12_CPU_DESCRIPTOR_HANDLE*,BOOL,const D3D12_CPU_DESCRIPTOR_HANDLE*,
    ID3D12Resource*)noexcept;
EditorUiDrawRoute BeginEditorUiDraw(ID3D12GraphicsCommandList*,const UiEditorDrawProof&)noexcept;
void EndEditorUiDraw(ID3D12GraphicsCommandList*,const EditorUiDrawRoute&)noexcept;
void ResetEditorUiList(ID3D12GraphicsCommandList*)noexcept;
void CloseEditorUiList(ID3D12GraphicsCommandList*,bool)noexcept;
struct EditorUiSubmissionData;
struct EditorUiSubmission {std::shared_ptr<EditorUiSubmissionData> state;};
EditorUiSubmission PrepareEditorUiSubmission(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*)noexcept;
// After original Execute and UiSubmitted; append the UI snapshot on that same
// application queue before the caller releases its submission lock.
using EditorUiExecute=void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*);
void EditorUiSubmitted(ID3D12CommandQueue*,EditorUiSubmission&&,EditorUiExecute)noexcept;
void ClearEditorUiSurfaces()noexcept;
}
