#pragma once
#include <d3d12.h>
#include <cstdint>

namespace cvr::framegen {
// Immutable author captures are shared by the worker's COPY read, the XR
// pixel decode and the author's compute packing. No consumer narrows this
// state; only the producer may leave it after obtaining its write lease.
// This supported read-only superset has an exact mapping in the pinned FFX
// backend. An arbitrary copy|pixel|compute FFX enum mask has no such mapping.
inline constexpr auto CaptureColorReadState=D3D12_RESOURCE_STATE_GENERIC_READ;
template<class Device>
HRESULT CreateMainCaptureColor(Device* device,const D3D12_HEAP_PROPERTIES& heap,
    const D3D12_RESOURCE_DESC& description,ID3D12Resource** resource) {
    return device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&description,
        CaptureColorReadState,nullptr,IID_PPV_ARGS(resource));
}
inline D3D12_RESOURCE_BARRIER MainCaptureWriteBarrier(ID3D12Resource* resource) {
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,CaptureColorReadState,D3D12_RESOURCE_STATE_COPY_DEST};
    return barrier;
}
inline D3D12_RESOURCE_BARRIER MainCaptureReadyBarrier(ID3D12Resource* resource) {
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COPY_DEST,CaptureColorReadState};
    return barrier;
}
// The author submit branch calls this same recording boundary in production
// and CPU contract tests. The pass owns its SRV/RTV recording as before.
template<class DecodePass>
bool RecordColorDecode(DecodePass& pass,ID3D12GraphicsCommandList* list,
    ID3D12Resource* source,ID3D12Resource* target,uint32_t descriptorSlot,D3D12_RESOURCE_STATES sourceState) {
    if(!list || !source || !target ||
       (sourceState!=CaptureColorReadState && sourceState!=D3D12_RESOURCE_STATE_COPY_SOURCE))return false;
    const bool exclusive=sourceState==D3D12_RESOURCE_STATE_COPY_SOURCE;
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={source,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
    if(exclusive)list->ResourceBarrier(1,&barrier);
    const bool decoded=pass.Record(list,source,target,descriptorSlot);
    barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
    if(exclusive)list->ResourceBarrier(1,&barrier);
    return decoded;
}
}
