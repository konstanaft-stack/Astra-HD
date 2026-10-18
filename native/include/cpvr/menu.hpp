#pragma once
#include "gpu.hpp"
#include <dxgi1_4.h>
#include <optional>
#include <span>

namespace cpvr {
struct NativeMenuSnapshot { bool observed{}; std::int32_t mode{}; std::uint32_t revision{}; };
struct MenuRouteState {
    bool active=true, native_menu{}, priming=true;
    std::uint64_t epoch=1;
};
// Bootstrap/explicit menu transitions only. A later missing SR frame never
// reclassifies an already running world as a flat menu.
class MenuRoute {
public:
    bool observe(NativeMenuSnapshot);
    bool world_pair_ready();
    MenuRouteState state() const noexcept { return state_; }
private:
    MenuRouteState state_;
};
// Opaque game menu: level, translation-following and yaw with a dead zone.
// Epoch changes invalidate its LOCAL pose before a new tracking sample arrives.
class MenuAnchor {
public:
    std::optional<Pose> update(const Pose&,bool tracking_valid,std::uint64_t menu_epoch,
                               std::uint64_t space_epoch,std::int64_t now_ns);
    void reset() noexcept { *this=MenuAnchor{}; }
private:
    std::optional<Pose> pose_;
    std::uint64_t menu_epoch_{},space_epoch_{};
    std::int64_t last_ns_{};
    float yaw_{};
    bool following_{};
};
namespace menu_detail { std::optional<std::size_t> find_mode_signature(std::span<const std::byte>); }
struct MenuTestAccess;
class NativeMenuHook {
public:
    NativeMenuHook(); ~NativeMenuHook();
    bool install(Log={});
    void remove();
    NativeMenuSnapshot snapshot() const noexcept;
private:
    friend struct MenuTestAccess;
    bool install_at(void*,Log);
    struct Impl;std::unique_ptr<Impl> impl_;
};
struct MenuCaptureFrame {
    GpuImage color;Extent canvas;
    std::uint64_t epoch{},serial{};
};
struct MenuCaptureStatus {
    std::uint64_t captured{},busy_skips{},failed{};
    unsigned slots{};bool unsafe_gpu{};
};
class MenuCapture {
public:
    static constexpr unsigned capacity=3;
    MenuCapture();~MenuCapture();
    bool initialize(ID3D12Device*,Log={});
    void begin_epoch(std::uint64_t);
    bool capture(IDXGISwapChain3*,ID3D12CommandQueue*,std::uint64_t);
    std::shared_ptr<const MenuCaptureFrame> latest() const;
    void pause(bool);
    bool ready_for_resize();
    bool shutdown();
    MenuCaptureStatus status() const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
