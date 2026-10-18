#pragma once
#include <d3d12.h>
#include <atomic>
#include <mutex>
#include <limits>
namespace cvr::gpu {
inline constexpr GUID AllocationKindKey{0xe52dc843,0xbcc0,0x4ac8,{0xb5,0x13,0xb5,0x47,0x6e,0x9f,0x5d,0x3e}};
inline constexpr GUID AllocationRegionKey{0x6707a54d,0x25fd,0x49c1,{0xac,0x89,0x2e,0x81,0x49,0x04,0xea,0x11}};
inline constexpr GUID AllocationHeapKey{0xa77b65a5,0x6053,0x4981,{0x88,0x78,0xbe,0x52,0x19,0x55,0x05,0x40}};
struct ResourceHeapRegion {UINT64 heap{},offset{},bytes{};};
inline UINT ReadResourceAllocationKind(ID3D12Resource* resource) noexcept {
    UINT kind{},bytes=sizeof(kind);return resource&&SUCCEEDED(resource->GetPrivateData(AllocationKindKey,&bytes,&kind))&&bytes==sizeof(kind)?kind:0;
}
inline ResourceHeapRegion ReadResourceHeapRegion(ID3D12Resource* resource) noexcept {
    ResourceHeapRegion region;UINT bytes=sizeof(region);if(!resource||FAILED(resource->GetPrivateData(AllocationRegionKey,&bytes,&region))||bytes!=sizeof(region))return {};return region;
}
inline HRESULT MarkResourceAllocation(ID3D12Resource* resource,bool committed) noexcept {
    const UINT kind=committed?1u:2u;return resource?resource->SetPrivateData(AllocationKindKey,sizeof(kind),&kind):E_POINTER;
}
inline HRESULT MarkPlacedResourceAllocation(ID3D12Device* device,ID3D12Resource* resource,
    ID3D12Heap* heap,UINT64 offset,const D3D12_RESOURCE_DESC* desc) noexcept {
    const auto marked=MarkResourceAllocation(resource,false);
    if(FAILED(marked)||!device||!heap||!desc)return E_INVALIDARG;
    const auto heapDesc=heap->GetDesc();
    // Separate handles to a shared heap are not proof of separate physical memory.
    if(heapDesc.Flags&(D3D12_HEAP_FLAG_SHARED|D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER))return E_FAIL;
    const auto info=device->GetResourceAllocationInfo(0,1,desc);
    if(!info.SizeInBytes||info.SizeInBytes==std::numeric_limits<UINT64>::max()||
       offset>heapDesc.SizeInBytes||info.SizeInBytes>heapDesc.SizeInBytes-offset)return E_FAIL;
    static std::mutex mutex;static std::atomic<UINT64> serial{};
    UINT64 identity{};
    {std::lock_guard lock(mutex);UINT bytes=sizeof(identity);
     if(FAILED(heap->GetPrivateData(AllocationHeapKey,&bytes,&identity))||bytes!=sizeof(identity)||!identity){
        identity=++serial;if(FAILED(heap->SetPrivateData(AllocationHeapKey,sizeof(identity),&identity)))return E_FAIL;
     }}
    const ResourceHeapRegion region{identity,offset,info.SizeInBytes};
    return resource->SetPrivateData(AllocationRegionKey,sizeof(region),&region);
}
inline bool IsKnownCommittedResource(ID3D12Resource* resource) noexcept {
    UINT kind{},bytes=sizeof(kind);return resource&&SUCCEEDED(resource->GetPrivateData(AllocationKindKey,&bytes,&kind))&&bytes==sizeof(kind)&&kind==1;
}
inline bool AliasingInvalidatesResource(ID3D12Resource* resource,const D3D12_RESOURCE_ALIASING_BARRIER& alias) noexcept {
    if(alias.pResourceBefore==resource||alias.pResourceAfter==resource)return true;
    // NULL is a wildcard for placed/reserved allocations. A resource observed
    // at successful CreateCommittedResource has its own implicit heap.
    return (!alias.pResourceBefore||!alias.pResourceAfter)&&!IsKnownCommittedResource(resource);
}
}
