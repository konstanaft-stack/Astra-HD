#pragma once
#include "resource_metrics.hpp"
#include <atomic>
#include <vector>
namespace cpvr {
struct TexturePoolStats {std::uint64_t reuses{},exhausted{};};
// Owner-thread operations. Consumers keep this lease (not just a COM texture)
// until their last GPU use completes. Unknown submissions never become reusable.
class PooledTexture {
public:
    ComPtr<ID3D12Resource> texture;
    void pending() noexcept;
    bool completion(ID3D12Fence*,std::uint64_t) noexcept;
private:
    friend class TexturePool;
    bool ready() const noexcept;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Fence> fence;
    D3D12_RESOURCE_DESC desc{};
    D3D12_RESOURCE_STATES resting=D3D12_RESOURCE_STATE_COMMON;
    std::uint64_t epoch{},value{};
    bool submitted{};
};
// Bounded by entries, allocated lazily. Each entry has an immutable resting
// state; existing callers acquire/return COMMON, presentation uses GENERIC_READ.
// Only the owner calls acquire/close; status is safe on the UI thread.
class TexturePool {
public:
    TexturePool(ResourceAccounting& accounting,std::size_t capacity):accounting_(accounting),capacity_(capacity){}
    std::shared_ptr<PooledTexture> acquire(ID3D12Device*,const D3D12_RESOURCE_DESC&,std::uint64_t epoch);
    std::shared_ptr<PooledTexture> acquire(ID3D12Device*,const D3D12_RESOURCE_DESC&,std::uint64_t epoch,D3D12_RESOURCE_STATES resting);
    void trim();
    bool close(); // no new acquisitions; pending GPU entries are retained
    TexturePoolStats status() const noexcept{return {reuses_.load(),exhausted_.load()};}
private:
    ResourceAccounting& accounting_;std::size_t capacity_;bool closed_{};
    std::vector<std::shared_ptr<PooledTexture>> entries_;
    std::atomic<std::uint64_t> reuses_{},exhausted_{};
};
}
