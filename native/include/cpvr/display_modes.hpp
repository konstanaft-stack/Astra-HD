#pragma once
#include "core.hpp"
#include <dxgi1_6.h>
#include <span>
#include <memory>
namespace cpvr {
namespace display_detail {
HRESULT emit_modes(std::span<const DXGI_MODE_DESC>,Extent,DXGI_FORMAT,UINT*,DXGI_MODE_DESC*) noexcept;
HRESULT emit_modes(std::span<const DXGI_MODE_DESC1>,Extent,DXGI_FORMAT,UINT*,DXGI_MODE_DESC1*) noexcept;
bool closest_mode(const DXGI_MODE_DESC&,Extent,DXGI_MODE_DESC&) noexcept;
bool closest_mode(const DXGI_MODE_DESC1&,Extent,DXGI_MODE_DESC1&) noexcept;
}
// Derived from ToFu a9c81bb DisplayModes.cpp; receives the real returned adapter/output.
// update_extent must run with the XR-derived startup size before game factory enumeration.
class DisplayModeAdapter {
public:
    DisplayModeAdapter();~DisplayModeAdapter();
    DisplayModeAdapter(const DisplayModeAdapter&)=delete;
    DisplayModeAdapter& operator=(const DisplayModeAdapter&)=delete;
    bool update_extent(Extent);
    bool install_adapter(IDXGIAdapter*);
    bool install_output(IDXGIOutput*);
    void shutdown();
private: struct Impl;std::unique_ptr<Impl> impl_;
};
}
