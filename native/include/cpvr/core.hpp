#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <mutex>

namespace cpvr {
enum class Eye : std::uint32_t { Left = 0, Right = 1 };
struct Extent { std::uint32_t width{}, height{}; bool operator==(const Extent&) const = default; };
struct Fov { float left{}, right{}, up{}, down{}; };
struct Pose {
    std::array<float, 4> orientation{0,0,0,1};
    std::array<float, 3> position{};
};
enum class SrMode { Quality, Balanced, Performance, UltraPerformance, Dlaa, Custom };
enum class SrModel : std::uint32_t { Game = 0, K = 11, L = 12, M = 13 };
enum class DisplayPattern { Chessboard, StereoPairs, Custom };
enum class Cadence { StereoPairs, AlternateEyes, MixedPairs };
struct Settings {
    float ppd = 16.5f;
    SrMode sr_mode = SrMode::Quality;
    SrModel sr_model = SrModel::M;
    float custom_render_scale = 2.0f/3.0f;
    float sharpness = 0.0f;
    bool frame_generation = true;
    std::uint32_t multiplier = 2;
    bool neural_rendering = false;
    float neural_strength = 1.0f;
    DisplayPattern display_pattern = DisplayPattern::Chessboard;
    // Each token names LEFT then RIGHT: R real, G generated. Comma-separated cycle.
    std::string eye_pattern = "GR,RG";
    Cadence cadence = Cadence::AlternateEyes;
    std::string pair_pattern = "RG";
    bool desktop_mirror = false;
    int panel_key = 0x79, fg_key = 0x78, nr_key = 0x7A;
};
struct ResolutionPlan {
    std::array<Extent,2> output{};
    // Estimates only. Runtime must publish the actual dimensions chosen by NGX.
    std::array<Extent,2> estimated_input{};
    float ppd{};
};
struct Validation { bool valid{}; std::string message; };
Validation validate_settings(const Settings&);
// Explicit process-local simulator probe: "N|GR,GG,..."; no file persistence.
// Invalid input leaves every existing setting unchanged.
Validation apply_diagnostic_generation(Settings&,std::string_view);
std::optional<ResolutionPlan> plan_resolution(const Settings&, const std::array<Fov,2>&,
                                             Extent maximum, std::string& error);
struct AppliedState {
    std::uint64_t revision{}, epoch{};
    std::array<Extent,2> actual_input{}, actual_output{}, xr_output{};
    bool sr_evaluating{}, mfg_evaluating{}, nr_evaluating{};
    std::string sr_dll_version, fg_dll_version, nr_dll_version, message;
};
struct SettingsRequest { std::uint64_t revision{}; Settings settings; };
class SettingsControl {
public:
    Settings requested() const;
    AppliedState applied() const;
    std::uint64_t requested_revision() const;
    Validation request(const Settings&);
    std::optional<SettingsRequest> take_pending();
    bool acknowledge(std::uint64_t revision, const AppliedState&);
    bool observe(const AppliedState&);
private:
    mutable std::mutex mutex_;
    Settings requested_{};
    AppliedState applied_{};
    std::uint64_t revision_{};
    std::optional<SettingsRequest> pending_;
    std::optional<std::uint64_t> in_flight_;
};
// Called with foreground game key events. Repeated key-downs never toggle twice.
class Hotkeys {
public:
    bool key(SettingsControl&, int virtual_key, bool down, bool foreground);
    bool panel_visible() const { return panel_visible_; }
    void set_panel_visible(bool value) { panel_visible_ = value; }
private:
    std::array<bool,256> held_{};
    bool panel_visible_{};
};
struct RuntimeMetrics {
    std::array<std::uint64_t,2> real_frames{}, generated_frames{}, submitted_frames{};
    double sr_gpu_ms{}, mfg_gpu_ms{}, nr_gpu_ms{};
    bool gpu_timings_valid{}, headset_submission_active{};
    std::string phase = "Ожидание запуска собственного VR-цикла";
};
struct PanelState {
    Settings draft{};
    std::uint64_t base_revision{};
    bool initialized{}, dirty{};
    std::string message;
};
void draw_panel(SettingsControl&, Hotkeys&, const RuntimeMetrics&,
                const std::array<Fov,2>&, Extent maximum, PanelState&);
}
