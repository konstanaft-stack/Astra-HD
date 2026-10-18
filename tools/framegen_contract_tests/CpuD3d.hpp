#pragma once
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <atomic>
#include <vector>
#include <cstring>
// CPU-only COM identities/descriptors/recorded barriers. They never create a
// graphics device, execute a command list, attach NVIDIA or launch a runtime.
struct CpuDevice final:ID3D12Object {
 std::atomic<ULONG> refs{1};
 Microsoft::WRL::ComPtr<ID3D12Object> underlying;
 GUID dataKey{};std::vector<unsigned char> privateBytes;
 explicit CpuDevice(CpuDevice* native=nullptr):underlying(native){}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {
  if(!out)return E_POINTER;*out=nullptr;
  if(id!=__uuidof(IUnknown) && id!=__uuidof(ID3D12Object) && id!=__uuidof(ID3D12Device))return E_NOINTERFACE;
  *out=static_cast<IUnknown*>(this);AddRef();return S_OK;
 }
 ULONG STDMETHODCALLTYPE AddRef() override {return ++refs;}
 ULONG STDMETHODCALLTYPE Release() override {const auto n=--refs;if(!n)delete this;return n;}
 HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID key,UINT* size,void* data) override {
  if(underlying)return underlying->GetPrivateData(key,size,data);
  if(key!=dataKey||privateBytes.empty())return DXGI_ERROR_NOT_FOUND;
  if(!size)return E_INVALIDARG;const auto capacity=*size;*size=UINT(privateBytes.size());
  if(capacity<privateBytes.size())return DXGI_ERROR_MORE_DATA;
  if(data)std::memcpy(data,privateBytes.data(),privateBytes.size());return S_OK;
 }
 HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID key,UINT size,const void* data) override {
  if(underlying)return underlying->SetPrivateData(key,size,data);
  dataKey=key;privateBytes.clear();if(size&&data)privateBytes.assign(static_cast<const unsigned char*>(data),static_cast<const unsigned char*>(data)+size);return S_OK;
 }
 HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID,const IUnknown*) override {return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override {return S_OK;}
 ID3D12Device* Native() {return reinterpret_cast<ID3D12Device*>(static_cast<IUnknown*>(this));}
};
template<class Interface> struct CpuChild:Interface {
 std::atomic<ULONG> refs{1};Microsoft::WRL::ComPtr<IUnknown> device;unsigned unexpected{};
 explicit CpuChild(CpuDevice* value):device(value) {}
 virtual ~CpuChild()=default;
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {
  if(!out)return E_POINTER;*out=nullptr;
  if(id!=__uuidof(IUnknown) && id!=__uuidof(Interface))return E_NOINTERFACE;
  *out=static_cast<Interface*>(this);AddRef();return S_OK;
 }
 ULONG STDMETHODCALLTYPE AddRef() override {return ++refs;}
 ULONG STDMETHODCALLTYPE Release() override {const auto n=--refs;if(!n)delete this;return n;}
 HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID,UINT*,void*) override {return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID,UINT,const void*) override {return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID,const IUnknown*) override {return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override {return S_OK;}
 HRESULT STDMETHODCALLTYPE GetDevice(REFIID id,void** out) override {return device->QueryInterface(id,out);}
};
struct CpuFence final:CpuChild<ID3D12Fence> {
 using CpuChild::CpuChild;std::atomic<uint64_t> completed{};
 UINT64 STDMETHODCALLTYPE GetCompletedValue() override {return completed.load();}
 HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64,HANDLE) override {++unexpected;return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE Signal(UINT64 value) override {completed=value;return S_OK;}
};
struct CpuResource final:CpuChild<ID3D12Resource> {
 using CpuChild::CpuChild;D3D12_RESOURCE_DESC descriptor{};
 HRESULT STDMETHODCALLTYPE Map(UINT,const D3D12_RANGE*,void**) override {++unexpected;return E_NOTIMPL;}
 void STDMETHODCALLTYPE Unmap(UINT,const D3D12_RANGE*) override {++unexpected;}
 D3D12_RESOURCE_DESC STDMETHODCALLTYPE GetDesc() override {return descriptor;}
 D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE GetGPUVirtualAddress() override {++unexpected;return 0;}
 HRESULT STDMETHODCALLTYPE WriteToSubresource(UINT,const D3D12_BOX*,const void*,UINT,UINT) override {++unexpected;return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE ReadFromSubresource(void*,UINT,UINT,UINT,const D3D12_BOX*) override {++unexpected;return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE GetHeapProperties(D3D12_HEAP_PROPERTIES*,D3D12_HEAP_FLAGS*) override {++unexpected;return E_NOTIMPL;}
};
struct CpuQueue final:CpuChild<ID3D12CommandQueue> {
 using CpuChild::CpuChild;D3D12_COMMAND_QUEUE_DESC descriptor{D3D12_COMMAND_LIST_TYPE_DIRECT};
    void STDMETHODCALLTYPE UpdateTileMappings(ID3D12Resource *pResource, UINT NumResourceRegions, const D3D12_TILED_RESOURCE_COORDINATE *pResourceRegionStartCoordinates, const D3D12_TILE_REGION_SIZE *pResourceRegionSizes, ID3D12Heap *pHeap, UINT NumRanges, const D3D12_TILE_RANGE_FLAGS *pRangeFlags, const UINT *pHeapRangeStartOffsets, const UINT *pRangeTileCounts, D3D12_TILE_MAPPING_FLAGS Flags) override {++unexpected;}
    void STDMETHODCALLTYPE CopyTileMappings(ID3D12Resource *pDstResource, const D3D12_TILED_RESOURCE_COORDINATE *pDstRegionStartCoordinate, ID3D12Resource *pSrcResource, const D3D12_TILED_RESOURCE_COORDINATE *pSrcRegionStartCoordinate, const D3D12_TILE_REGION_SIZE *pRegionSize, D3D12_TILE_MAPPING_FLAGS Flags) override {++unexpected;}
    void STDMETHODCALLTYPE ExecuteCommandLists(UINT NumCommandLists, ID3D12CommandList *const *ppCommandLists) override {++unexpected;}
    void STDMETHODCALLTYPE SetMarker(UINT Metadata, const void *pData, UINT Size) override {++unexpected;}
    void STDMETHODCALLTYPE BeginEvent(UINT Metadata, const void *pData, UINT Size) override {++unexpected;}
    void STDMETHODCALLTYPE EndEvent(void) override {++unexpected;}
    HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence *pFence, UINT64 Value) override {++unexpected;return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence *pFence, UINT64 Value) override {++unexpected;return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64 *pFrequency) override {++unexpected;return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetClockCalibration(UINT64 *pGpuTimestamp, UINT64 *pCpuTimestamp) override {++unexpected;return E_NOTIMPL;}
    D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE GetDesc(void) override {return descriptor;}
};
struct CpuGraphicsList final:CpuChild<ID3D12GraphicsCommandList> {
 using CpuChild::CpuChild;std::vector<D3D12_RESOURCE_BARRIER> barriers;
 D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() override {return D3D12_COMMAND_LIST_TYPE_DIRECT;}
    HRESULT STDMETHODCALLTYPE Close(void) override {++unexpected;return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE Reset(ID3D12CommandAllocator *pAllocator, ID3D12PipelineState *pInitialState) override {++unexpected;return E_NOTIMPL;}
    void STDMETHODCALLTYPE ClearState(ID3D12PipelineState *pPipelineState) override {++unexpected;}
    void STDMETHODCALLTYPE DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation) override {++unexpected;}
    void STDMETHODCALLTYPE DrawIndexedInstanced(UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation) override {++unexpected;}
    void STDMETHODCALLTYPE Dispatch(UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ) override {++unexpected;}
    void STDMETHODCALLTYPE CopyBufferRegion(ID3D12Resource *pDstBuffer, UINT64 DstOffset, ID3D12Resource *pSrcBuffer, UINT64 SrcOffset, UINT64 NumBytes) override {++unexpected;}
    void STDMETHODCALLTYPE CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION *pDst, UINT DstX, UINT DstY, UINT DstZ, const D3D12_TEXTURE_COPY_LOCATION *pSrc, const D3D12_BOX *pSrcBox) override {++unexpected;}
    void STDMETHODCALLTYPE CopyResource(ID3D12Resource *pDstResource, ID3D12Resource *pSrcResource) override {++unexpected;}
    void STDMETHODCALLTYPE CopyTiles(ID3D12Resource *pTiledResource, const D3D12_TILED_RESOURCE_COORDINATE *pTileRegionStartCoordinate, const D3D12_TILE_REGION_SIZE *pTileRegionSize, ID3D12Resource *pBuffer, UINT64 BufferStartOffsetInBytes, D3D12_TILE_COPY_FLAGS Flags) override {++unexpected;}
    void STDMETHODCALLTYPE ResolveSubresource(ID3D12Resource *pDstResource, UINT DstSubresource, ID3D12Resource *pSrcResource, UINT SrcSubresource, DXGI_FORMAT Format) override {++unexpected;}
    void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY PrimitiveTopology) override {++unexpected;}
    void STDMETHODCALLTYPE RSSetViewports(UINT NumViewports, const D3D12_VIEWPORT *pViewports) override {++unexpected;}
    void STDMETHODCALLTYPE RSSetScissorRects(UINT NumRects, const D3D12_RECT *pRects) override {++unexpected;}
    void STDMETHODCALLTYPE OMSetBlendFactor(const FLOAT BlendFactor[ 4 ]) override {++unexpected;}
    void STDMETHODCALLTYPE OMSetStencilRef(UINT StencilRef) override {++unexpected;}
    void STDMETHODCALLTYPE SetPipelineState(ID3D12PipelineState *pPipelineState) override {++unexpected;}
    void STDMETHODCALLTYPE ResourceBarrier(UINT NumBarriers, const D3D12_RESOURCE_BARRIER *pBarriers) override {barriers.insert(barriers.end(),pBarriers,pBarriers+NumBarriers);}
    void STDMETHODCALLTYPE ExecuteBundle(ID3D12GraphicsCommandList *pCommandList) override {++unexpected;}
    void STDMETHODCALLTYPE SetDescriptorHeaps(UINT NumDescriptorHeaps, ID3D12DescriptorHeap *const *ppDescriptorHeaps) override {++unexpected;}
    void STDMETHODCALLTYPE SetComputeRootSignature(ID3D12RootSignature *pRootSignature) override {++unexpected;}
    void STDMETHODCALLTYPE SetGraphicsRootSignature(ID3D12RootSignature *pRootSignature) override {++unexpected;}
    void STDMETHODCALLTYPE SetComputeRootDescriptorTable(UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) override {++unexpected;}
    void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) override {++unexpected;}
    void STDMETHODCALLTYPE SetComputeRoot32BitConstant(UINT RootParameterIndex, UINT SrcData, UINT DestOffsetIn32BitValues) override {++unexpected;}
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstant(UINT RootParameterIndex, UINT SrcData, UINT DestOffsetIn32BitValues) override {++unexpected;}
    void STDMETHODCALLTYPE SetComputeRoot32BitConstants(UINT RootParameterIndex, UINT Num32BitValuesToSet, const void *pSrcData, UINT DestOffsetIn32BitValues) override {++unexpected;}
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstants(UINT RootParameterIndex, UINT Num32BitValuesToSet, const void *pSrcData, UINT DestOffsetIn32BitValues) override {++unexpected;}
    void STDMETHODCALLTYPE SetComputeRootConstantBufferView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override {++unexpected;}
    void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override {++unexpected;}
    void STDMETHODCALLTYPE SetComputeRootShaderResourceView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override {++unexpected;}
    void STDMETHODCALLTYPE SetGraphicsRootShaderResourceView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override {++unexpected;}
    void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override {++unexpected;}
    void STDMETHODCALLTYPE SetGraphicsRootUnorderedAccessView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override {++unexpected;}
    void STDMETHODCALLTYPE IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW *pView) override {++unexpected;}
    void STDMETHODCALLTYPE IASetVertexBuffers(UINT StartSlot, UINT NumViews, const D3D12_VERTEX_BUFFER_VIEW *pViews) override {++unexpected;}
    void STDMETHODCALLTYPE SOSetTargets(UINT StartSlot, UINT NumViews, const D3D12_STREAM_OUTPUT_BUFFER_VIEW *pViews) override {++unexpected;}
    void STDMETHODCALLTYPE OMSetRenderTargets(UINT NumRenderTargetDescriptors, const D3D12_CPU_DESCRIPTOR_HANDLE *pRenderTargetDescriptors, BOOL RTsSingleHandleToDescriptorRange, const D3D12_CPU_DESCRIPTOR_HANDLE *pDepthStencilDescriptor) override {++unexpected;}
    void STDMETHODCALLTYPE ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView, D3D12_CLEAR_FLAGS ClearFlags, FLOAT Depth, UINT8 Stencil, UINT NumRects, const D3D12_RECT *pRects) override {++unexpected;}
    void STDMETHODCALLTYPE ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetView, const FLOAT ColorRGBA[ 4 ], UINT NumRects, const D3D12_RECT *pRects) override {++unexpected;}
    void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap, D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, ID3D12Resource *pResource, const UINT Values[ 4 ], UINT NumRects, const D3D12_RECT *pRects) override {++unexpected;}
    void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap, D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, ID3D12Resource *pResource, const FLOAT Values[ 4 ], UINT NumRects, const D3D12_RECT *pRects) override {++unexpected;}
    void STDMETHODCALLTYPE DiscardResource(ID3D12Resource *pResource, const D3D12_DISCARD_REGION *pRegion) override {++unexpected;}
    void STDMETHODCALLTYPE BeginQuery(ID3D12QueryHeap *pQueryHeap, D3D12_QUERY_TYPE Type, UINT Index) override {++unexpected;}
    void STDMETHODCALLTYPE EndQuery(ID3D12QueryHeap *pQueryHeap, D3D12_QUERY_TYPE Type, UINT Index) override {++unexpected;}
    void STDMETHODCALLTYPE ResolveQueryData(ID3D12QueryHeap *pQueryHeap, D3D12_QUERY_TYPE Type, UINT StartIndex, UINT NumQueries, ID3D12Resource *pDestinationBuffer, UINT64 AlignedDestinationBufferOffset) override {++unexpected;}
    void STDMETHODCALLTYPE SetPredication(ID3D12Resource *pBuffer, UINT64 AlignedBufferOffset, D3D12_PREDICATION_OP Operation) override {++unexpected;}
    void STDMETHODCALLTYPE SetMarker(UINT Metadata, const void *pData, UINT Size) override {++unexpected;}
    void STDMETHODCALLTYPE BeginEvent(UINT Metadata, const void *pData, UINT Size) override {++unexpected;}
    void STDMETHODCALLTYPE EndEvent(void) override {++unexpected;}
    void STDMETHODCALLTYPE ExecuteIndirect(ID3D12CommandSignature *pCommandSignature, UINT MaxCommandCount, ID3D12Resource *pArgumentBuffer, UINT64 ArgumentBufferOffset, ID3D12Resource *pCountBuffer, UINT64 CountBufferOffset) override {++unexpected;}
};
