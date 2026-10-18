#pragma once
#include "gpu.hpp"
#include <optional>
#include <string>

namespace cpvr {
struct PanelCanvasPoint { float x{},y{}; };
// Source coordinates are CLIENT coordinates in the specified input surface,
// not screen coordinates. The result addresses the entire actual UI texture.
std::optional<PanelCanvasPoint> map_panel_pointer(float x,float y,Extent input_surface,Extent canvas);
struct PanelRendererOptions {
    Extent canvas{1280,960};
    std::wstring font_file; // Explicit font containing Cyrillic; no Latin-only fallback.
    float font_pixels=28;
    float width_m=1.05f, distance_m=1.15f;
};
Validation validate_panel_options(const PanelRendererOptions&);
// Creates a LOCAL anchor when the panel opens/rebases. It is not head-locked.
std::optional<Pose> panel_anchor(const Pose& center_head,float distance_m);
struct PanelFrame {
    // Hold the WHOLE frame until the consumer copy fence completes. Copying its
    // COM pointer alone does not reserve the producer's render-target slot.
    GpuImage color;
    Extent canvas;
    Pose local_pose;
    std::uint64_t space_epoch{}, serial{};
    float width_m{},height_m{};
    // Optional producer slot lease; retained with this whole frame until GPU copy completion.
    std::shared_ptr<const void> lifetime;
    // DX12 ImGui blended onto transparent black: premultiplied RGBA.
};
struct PanelRenderUpdate {
    RuntimeMetrics metrics;
    std::array<Fov,2> fovs;
    Extent maximum_eye_size;
    ViewSample views;
};
enum class PanelInputKind { Key, Text, Pointer, RelativePointer, MouseButton, Wheel, Focus };
struct PanelInputEvent {
    PanelInputKind kind=PanelInputKind::Key;
    int key_or_button{};
    bool down{},foreground=true,repeat{};
    std::uint32_t codepoint{};
    float x{},y{};
    Extent input_surface;
};
struct PanelRendererStatus {
    bool running{},ready{},visible{};
    std::uint64_t rendered_frames{},busy_skips{},input_overflows{};
    std::string message;
};
class PanelRenderer {
public:
    PanelRenderer();
    ~PanelRenderer();
    PanelRenderer(const PanelRenderer&)=delete;
    PanelRenderer& operator=(const PanelRenderer&)=delete;
    // Starts its own UI worker and DIRECT queue; true means accepted, not GPU ready.
    bool start(ID3D12Device*,std::shared_ptr<SettingsControl>,const PanelRendererOptions&,Log={});
    // These methods only enqueue CPU work. They never wait for GPU completion.
    bool update(const PanelRenderUpdate&);
    bool input(const PanelInputEvent&);
    std::shared_ptr<const PanelFrame> latest() const noexcept;
    PanelRendererStatus status() const;
    void request_stop() noexcept;
    // Management/teardown thread only. Never call from an intercepted game draw.
    void shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
