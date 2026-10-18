#pragma once
#pragma warning(push)
#pragma warning(disable:4100)
#include "CpuD3d.hpp"
struct SrFaultQueue final:CpuChild<ID3D12CommandQueue> {
 using CpuChild::CpuChild;Microsoft::WRL::ComPtr<ID3D12CommandQueue> target;bool failWait{},failSignal{};unsigned executes{},waits{},signals{};D3D12_COMMAND_QUEUE_DESC descriptor{D3D12_COMMAND_LIST_TYPE_DIRECT};
    void STDMETHODCALLTYPE UpdateTileMappings(ID3D12Resource *pResource, UINT NumResourceRegions, const D3D12_TILED_RESOURCE_COORDINATE *pResourceRegionStartCoordinates, const D3D12_TILE_REGION_SIZE *pResourceRegionSizes, ID3D12Heap *pHeap, UINT NumRanges, const D3D12_TILE_RANGE_FLAGS *pRangeFlags, const UINT *pHeapRangeStartOffsets, const UINT *pRangeTileCounts, D3D12_TILE_MAPPING_FLAGS Flags) override {++unexpected;}
    void STDMETHODCALLTYPE CopyTileMappings(ID3D12Resource *pDstResource, const D3D12_TILED_RESOURCE_COORDINATE *pDstRegionStartCoordinate, ID3D12Resource *pSrcResource, const D3D12_TILED_RESOURCE_COORDINATE *pSrcRegionStartCoordinate, const D3D12_TILE_REGION_SIZE *pRegionSize, D3D12_TILE_MAPPING_FLAGS Flags) override {++unexpected;}
    void STDMETHODCALLTYPE ExecuteCommandLists(UINT NumCommandLists, ID3D12CommandList *const *ppCommandLists) override {++executes;target->ExecuteCommandLists(NumCommandLists,ppCommandLists);}
    void STDMETHODCALLTYPE SetMarker(UINT Metadata, const void *pData, UINT Size) override {++unexpected;}
    void STDMETHODCALLTYPE BeginEvent(UINT Metadata, const void *pData, UINT Size) override {++unexpected;}
    void STDMETHODCALLTYPE EndEvent(void) override {++unexpected;}
    HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence *pFence, UINT64 Value) override {++signals;return failSignal?E_FAIL:target->Signal(pFence,Value);}
    HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence *pFence, UINT64 Value) override {++waits;return failWait?E_FAIL:target->Wait(pFence,Value);}
    HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64 *pFrequency) override {++unexpected;return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetClockCalibration(UINT64 *pGpuTimestamp, UINT64 *pCpuTimestamp) override {++unexpected;return E_NOTIMPL;}
    D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE GetDesc(void) override {return descriptor;}
};

#pragma warning(pop)
