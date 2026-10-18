#pragma once
#include "gpu.hpp"

namespace cpvr {
struct ResourceUsage {
    std::uint64_t allocations{},live_resources{},live_bytes{},peak_bytes{},tracking_failures{};
};
// Counts this owner's committed D3D12 textures by allocation-info size.
// Does not represent physical residency, allocator heaps, or NVIDIA internals.
class ResourceAccounting {
public:
    ResourceAccounting();
    bool track(ID3D12Device*,ID3D12Resource*) noexcept;
    ResourceUsage snapshot() const noexcept;
private:
    struct State;std::shared_ptr<State> state_;
};
}
