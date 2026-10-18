#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <memory>
#include <cstdint>
#include <functional>
#include "core.hpp"
#include "input.hpp"
#include "timing.hpp"

namespace cpvr {
using Microsoft::WRL::ComPtr;
enum class ColorTransfer { Unknown, SrgbEncoded, Linear };
struct GpuImage {
    // Producer must keep this image immutable until its consumer GPU copy ends.
    // COM ownership alone does not stop the producer overwriting a resource.
    ComPtr<ID3D12Resource> texture;
    ComPtr<ID3D12Fence> ready_fence;
    std::uint64_t ready_value{};
    D3D12_RESOURCE_STATES resting_state = D3D12_RESOURCE_STATE_COMMON;
    // Pool lease follows every consumer copy/packet through its GPU completion.
    // Holding only texture COM ownership does not prevent a pooled overwrite.
    std::shared_ptr<void> lifetime;
    // Storage/view provenance is distinct from transfer function. Replacing
    // a texture with a compatible UNORM copy does not linearize its pixels.
    DXGI_FORMAT source_format=DXGI_FORMAT_UNKNOWN;
    ColorTransfer transfer=ColorTransfer::Unknown;
};
struct SourceGuides {
    GpuImage depth, motion;
    // Existing original NVIDIA backend camera ABI: 104 floats and 7 flags.
    std::array<float,104> camera{};
    std::array<std::uint32_t,7> camera_flags{};
    bool camera_valid{}, reset{};
};
struct EyeFrame {
    Eye eye{};
    std::uint64_t epoch{}, source_id{}, stereo_pair_id{};
    std::uint64_t space_epoch{};
    // XrTime domain (nanoseconds), not the raw QPC or steady_clock epoch.
    std::int64_t content_time_ns{};
    // Pose in the same LOCAL reference space returned by XrOutput::views().
    Pose render_pose;
    Fov fov;
    GpuImage color;
    std::shared_ptr<const SourceGuides> guides;
    bool generated{};
};
struct StereoFrame {
    std::array<std::shared_ptr<const EyeFrame>,2> eyes;
    std::int64_t content_time_ns{};
    std::uint64_t epoch{}, pair_id{};
    std::uint64_t space_epoch{};
    // Exact common content time stays above for synchronized pairs; zero for
    // explicitly asynchronous AlternateEyes. Never relabel per-eye image times.
    std::int64_t presentation_time_ns{};
    Cadence cadence=Cadence::StereoPairs;
    std::int64_t eye_skew_ns{}, max_eye_skew_ns{};
};
struct ViewSample {
    std::array<Pose,2> poses;
    std::array<Fov,2> fovs;
    std::int64_t predicted_time_ns{};
    bool valid{};
    std::uint64_t space_epoch{};
    Pose center_head;
    bool center_valid{};
    std::int64_t predicted_period_ns{};
};
using FrameSelector = std::function<std::shared_ptr<const StereoFrame>(std::int64_t)>;
using InputConsumer = std::function<void(const InputSample&)>;
struct PanelFrame;
using PanelSelector = std::function<std::shared_ptr<const PanelFrame>()>;
// Native game menu surface, independent from our translucent settings panel.
// active suppresses world projection even while its image is not GPU ready.
struct MenuPresentation {
    bool active{};
    std::shared_ptr<const PanelFrame> frame;
    // Placement is CPU tracking state, independent of a new GPU image/fence.
    std::optional<Pose> local_pose;
    std::uint64_t epoch{},space_epoch{};
    // Authoritative producer epoch, also supplied while the menu is inactive.
    // Zero means unknown: no retained world layer may cross that boundary.
    std::uint64_t world_epoch{};
};
using MenuSelector = std::function<MenuPresentation()>;
using Log = std::function<void(const std::string&)>;
// Scalar-only receipt of a successful xrEndFrame projection. No GPU leases are
// retained by diagnostics. Pose/FOV/rect are copied from the submitted views.
struct XrEyeSubmission {
    std::uint64_t source_id{},chain{};
    std::int64_t content_time_ns{};
    Pose pose;Fov fov;Extent size;
    std::array<std::int32_t,2> offset{};
    std::uint32_t array_index{};
    bool generated{};
};
struct XrSubmissionReceipt {
    std::uint64_t sequence{},xr_frame{},pair_id{},epoch{},space_epoch{};
    std::int64_t display_time_ns{},period_ns{};
    std::array<XrEyeSubmission,2> eyes;
    bool repeated_pair{};
    bool reused_images{},retained_projection{};
};
struct XrOutputStatus {
    // Successful submission to runtime, not proof of physical headset display.
    std::uint64_t submitted_pairs{}, last_pair_id{}, last_epoch{};
    std::int64_t last_content_time_ns{};
    std::uint64_t last_space_epoch{};
    // Predicted displayTime of the first accepted EndFrame for this pair.
    // Repeats retain it; this is not a physical-display timestamp.
    std::int64_t first_pair_display_time_ns{};
    std::array<Extent,2> submitted_sizes{};
    bool session_running{}, session_focused{};
    std::uint64_t submitted_panels{};
    std::uint64_t submitted_menus{};
    std::array<std::int64_t,2> last_eye_times_ns{};
    std::uint64_t begin_frames{},end_frames{},end_failures{},world_ticks{},world_missing_pair{},
        world_time_rejected{},world_shape_rejected{},world_copy_unavailable{},world_other_gaps{};
    std::uint64_t world_no_fresh_pair{},world_retained_projection{},world_reused_images{},world_retained_over150ms{};
    std::int64_t retained_age_max_ns{};
    bool exit_requested{};
};
class XrOutput {
public:
    XrOutput();
    ~XrOutput();
    XrOutput(const XrOutput&) = delete;
    XrOutput& operator=(const XrOutput&) = delete;
    bool initialize(ID3D12Device*, ID3D12CommandQueue*, Log);
    // Frame calls/destruction belong to the initializing owner. request_exit is
    // the thread-safe exception, serialized with session handle retirement.
    bool tick(const FrameSelector&, const InputConsumer& = {}, const PanelSelector& = {}, const MenuSelector& = {});
    ViewSample views() const;
    InputSample input() const;
    XrOutputStatus status() const;
    std::vector<XrSubmissionReceipt> recent_submissions() const;
    XrClockMapping clock_mapping() const;
    Extent maximum_image_size() const;
    std::array<Extent,2> recommended_sizes() const;
    void request_exit() noexcept;
    bool shutdown(); // true only after actual owner-thread cleanup succeeded.
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
struct CameraPoseInput {
    ViewSample head;
    Eye eye = Eye::Left;
    bool enabled{};
    std::uint64_t recenter_revision{};
    XrClockMapping clock;
    float requested_vertical_fov_degrees{};
};
struct CameraAnchor {
    Pose neutral;
    std::uint64_t space_epoch{}, revision{};
    bool valid{};
};
struct CameraReceipt {
    std::uint64_t serial{},space_epoch{},recenter_revision{};
    Eye eye{};
    std::int64_t content_time_ns{};
    std::int64_t pose_prediction_ns{},actual_render_qpc{};
    Pose render_pose;
    std::array<std::int32_t,3> game_position{};
    std::array<float,4> game_orientation{0,0,0,1};
    std::array<std::int32_t,3> native_game_position{};
    std::array<float,3> native_up{},native_right{},native_forward{};
    bool native_camera_valid{};
    float game_input_fov_degrees{};
    std::uintptr_t camera_system{},render_output{};
    bool applied{};
    std::uint32_t native_frame_token{};
    bool native_frame_known{};
};
struct CameraReceiptWindow {
    std::array<CameraReceipt,32> records{};
    std::size_t size{};
    std::uint64_t space_epoch{},recenter_revision{};
};
class CameraBridge {
public:
    CameraBridge();
    ~CameraBridge();
    // Only exact supported game executables may be patched. No Luke/other-port hooks.
    bool install(Log);
    void update(const CameraPoseInput&);
    void begin_native_frame(std::uint32_t token);
    void remove();
    bool active() const;
    CameraAnchor anchor() const;
    CameraReceipt receipt() const;
    CameraReceiptWindow receipts() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
