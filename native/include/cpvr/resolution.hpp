#pragma once
#include "core.hpp"
#include <windows.h>
#include <functional>
#include <memory>
#include <span>
#include <cstddef>
#include <chrono>
#include <mutex>
#include <unordered_set>
#include "graphics_host.hpp"

namespace cpvr {
namespace resolution_detail {
// Searches a mapped PE image, never a different module or system DLL.
std::uintptr_t* find_import_slot(std::span<std::byte>,const char* function) noexcept;
bool virtual_rect(HWND requested,HWND bound,Extent,bool window_rect,RECT&) noexcept;
bool map_cursor(POINT&,Extent physical,POINT screen_origin,Extent source,bool to_virtual) noexcept;
bool map_clip(RECT&,Extent physical,POINT screen_origin,Extent source) noexcept;
Validation validate_extent(Extent);
// Physical desktop client only; source/backbuffer dimensions are left intact.
std::optional<Extent> compact_client(Extent source,Extent limit={800,600}) noexcept;
// Donor SetWindowPos/MoveWindow boundary: constrain the physical game window,
// preserving native placement flags and independent source/backbuffer size.
bool compact_placement(HWND requested,HWND bound,Extent source,int& x,int& y,int& width,int& height,UINT flags) noexcept;
// A settings object snapshot reaches through the native validator targets.
// Only target +84/+88 changes. Active +18/+1C remains engine-owned.
bool update_targets(std::span<std::byte> settings,Extent desired) noexcept;
struct BuildProof {
    std::span<const std::byte> parent_entry,caller,leaf,window_getter;
    std::uint32_t parent_begin{},parent_end{},parent_unwind{};
};
bool supported_layout(const BuildProof&) noexcept;
struct ResizeRequest {std::uint64_t token{};WPARAM wparam{};LPARAM lparam{};};
struct ResizeStatus {bool paused{};std::uint64_t pending{},posted{};unsigned windows{},native{};};
// Tracks independent owners of capture pause. Native/window calls and waits
// occur outside this object's mutex; ready() only polls existing retirements.
class ResizeCoordinator {
public:
    void configure(std::function<void(bool)> pause,std::function<bool()> ready);
    WindowSizeDispatch request(WPARAM,LPARAM);
    void cancel_pending();
    std::optional<ResizeRequest> next_ready_message();
    void post_failed(std::uint64_t);
    std::optional<ResizeRequest> take_deferred(std::uint64_t);
    void finish_window(std::uint64_t);
    std::uint64_t begin_native();
    bool native_ready(std::uint64_t);
    // A failed/throwing readiness check still returns its owned token so the
    // native-call completion can retire precisely this pause scope.
    NativeResizeDispatch admit_native(std::chrono::milliseconds timeout);
    void finish_native(std::uint64_t);
    void refresh_pause();
    ResizeStatus status() const;
private:
    void sync();
    bool ready();
    mutable std::mutex mutex_;
    std::function<void(bool)> pause_;std::function<bool()> ready_;
    std::optional<ResizeRequest> pending_;std::uint64_t serial_{},posted_{};
    std::unordered_set<std::uint64_t> windows_,native_;
};
}
struct ResolutionStatus {
    bool game_client_rect_hook{},game_window_rect_hook{};
    unsigned game_cursor_imports{};
    unsigned game_placement_imports{};
    bool installed{},window_bound{},trigger_pending{},window_triggered{};
    Extent requested{},observed_active{},observed_target{},client_extent{};
    std::uint64_t revision{},trigger_revision{},settings_hits{};
    std::uintptr_t settings_object{};
    std::string message;
};
class ResolutionAdapter {
public:
    ResolutionAdapter();
    ~ResolutionAdapter();
    ResolutionAdapter(const ResolutionAdapter&)=delete;
    ResolutionAdapter& operator=(const ResolutionAdapter&)=delete;
    // Candidate installs only in the exact verified Cyberpunk 2.31 host image.
    bool install(std::function<void(const std::string&)> log={});
    bool bind_window(HWND,bool trigger=true);
    Validation request(Extent);
    // Requested source size for the GraphicsHost creation/resize overrides.
    // This is NEVER evidence that NGX or XR accepted the dimensions.
    std::optional<Extent> desired_extent() const;
    // Root WndProc invokes this first; true consumes ONLY our registered message.
    // Native WM_SIZE and all other messages continue to the original game handler.
    bool window_message(HWND,UINT,WPARAM,LPARAM);
    ResolutionStatus status() const;
    void shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
