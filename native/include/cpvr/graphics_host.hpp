#pragma once
#include "gpu.hpp"
#include <dxgi1_4.h>
#include <span>
namespace cpvr {
struct WindowSizeDispatch {bool deferred{};std::uint64_t token{};};
struct NativeResizeDispatch {bool ready=true;std::uint64_t token{};};
struct WindowHookStatus {
    bool registered{},ours_at_top{};
    std::uintptr_t current{},hook{},original{};
    bool geometry_valid{},maximized{},minimized{};
    Extent client_size;
    std::array<std::int32_t,4> window_rect{},monitor_rect{};
    std::uint32_t style{},extended_style{};
};
namespace graphics_detail {
// Based on ToFu a9c81bb actual-interface vtable routing. Registry key is
// (actual vtable, slot); original dispatch is never selected by guessed device.
bool patch_vtable_method(void* object,std::size_t slot,void* detour,void* owner);
void* original_vtable_method(void* object,std::size_t slot);
bool restore_vtable_owner(void* owner);
class PresentDispatch {
public:
    void enter() noexcept;
    void leave() noexcept;
    bool accept(bool has_verified_queue) noexcept;
private:
    unsigned depth_{};
    bool delivered_{};
};
DXGI_SWAP_CHAIN_DESC windowed_creation(const DXGI_SWAP_CHAIN_DESC&,bool enforce) noexcept;
DXGI_SWAP_CHAIN_FULLSCREEN_DESC windowed_creation(const DXGI_SWAP_CHAIN_FULLSCREEN_DESC&,bool enforce) noexcept;
BOOL fullscreen_request(BOOL requested,bool enforce) noexcept;
}
struct GraphicsCallbacks {
    std::function<void(IDXGISwapChain3*,ID3D12CommandQueue*)> swapchain_created;
    std::function<void(IDXGISwapChain3*,ID3D12CommandQueue*)> before_present;
    std::function<void(IDXGIAdapter*)> adapter_enumerated;
    std::function<void(IDXGIOutput*)> output_enumerated;
    std::function<std::optional<Extent>(HWND)> requested_extent;
    // Scoped to the real game HWND; enforces DXGI windowed state as well as creation.
    std::function<bool(HWND)> force_windowed;
    std::function<void(IDXGISwapChain*,HRESULT,std::uint64_t)> resized;
    std::function<NativeResizeDispatch(IDXGISwapChain*)> before_resize;
    std::function<WindowSizeDispatch(HWND,WPARAM,LPARAM)> defer_size;
    std::function<void(HWND,std::uint64_t)> size_completed;
    std::function<std::shared_ptr<void>(ID3D12CommandQueue*,std::span<ID3D12CommandList*const>)> before_execute;
    std::function<void(ID3D12CommandQueue*,const std::shared_ptr<void>&)> after_execute;
    std::function<void(ID3D12GraphicsCommandList*,std::span<const D3D12_RESOURCE_BARRIER>)> barriers;
    std::function<void(ID3D12GraphicsCommandList*,HRESULT)> close;
    std::function<void(ID3D12GraphicsCommandList*,HRESULT)> reset;
    // true consumes only the specific UI input message. Window lifecycle remains native.
    std::function<bool(HWND,UINT,WPARAM,LPARAM)> input;
};
class GraphicsHost {
public:
    GraphicsHost();~GraphicsHost();
    bool install(GraphicsCallbacks,Log);
    bool remove();
    bool active() const;
    // Read-only identity probe; another top-level subclass may still forward to us.
    static WindowHookStatus window_status(HWND);
    static LRESULT forward_window_message(HWND,UINT,WPARAM,LPARAM,std::uint64_t size_token=0);
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
