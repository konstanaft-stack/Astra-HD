// Test-only backend boundary. The production GenerationWorker performs all
// private copies, MotionRecorder work, fences and lifetime decisions on WARP.
// No NVIDIA library or interpolation implementation is loaded by this fixture.
#include "../../../../native/nvidia_backend/bridge_abi.h"
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstring>
#include <atomic>
using Microsoft::WRL::ComPtr;
static ComPtr<ID3D12Device> device;
static ComPtr<ID3D12CommandQueue> queue;
static ComPtr<ID3D12Fence> fence;
static uint64_t completed{};
static ID3D12Resource* borrowed{};
static std::atomic<int> verified{},runs{};
#define EXPORT extern "C" __declspec(dllexport)
EXPORT int cpvr_nv_configure(const wchar_t*,const char*,const char*,int){return 1;}
EXPORT int fg_nv_attach(ID3D12Device* value,const wchar_t*,int){
    ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter1> adapter;DXGI_ADAPTER_DESC1 desc{};
    if(!value||FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))||
       FAILED(factory->EnumAdapterByLuid(value->GetAdapterLuid(),IID_PPV_ARGS(&adapter)))||
       FAILED(adapter->GetDesc1(&desc))||!(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE))return 0;
    device=value;D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    return SUCCEEDED(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)))&&SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
}
EXPORT void fixture_borrowed_source(ID3D12Resource* value){borrowed=value;verified=runs=0;}
EXPORT int fixture_verified_private_color(){return verified.load();}
EXPORT int fixture_run_count(){return runs.load();}
EXPORT int fg_nv_run(unsigned,uint64_t,const cpvr_nv_frame* previous,const cpvr_nv_frame*,unsigned,ID3D12Resource*const[5]){
    ++runs;if(!previous||!previous->color||previous->color==borrowed)return 0;
    const auto desc=previous->color->GetDesc();if(desc.Width!=32||desc.Height!=16)return 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT rows{};UINT64 rowBytes{},bytes{};
    device->GetCopyableFootprints(&desc,0,1,0,&fp,&rows,&rowBytes,&bytes);
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=bytes;buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=buffer.SampleDesc.Count=1;
    buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;ComPtr<ID3D12Resource> readback;
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)))||
       FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)))||
       FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list))))return 0;
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={previous->color,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE};list->ResourceBarrier(1,&b);
    D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=previous->color;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp;list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    b.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;b.Transition.StateAfter=D3D12_RESOURCE_STATE_COMMON;list->ResourceBarrier(1,&b);
    if(FAILED(list->Close()))return 0;ID3D12CommandList* commands[]{list.Get()};queue->ExecuteCommandLists(1,commands);
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)return 0;
    const bool done=SUCCEEDED(queue->Signal(fence.Get(),++completed))&&SUCCEEDED(fence->SetEventOnCompletion(completed,event))&&
        WaitForSingleObject(event,5000)==WAIT_OBJECT_0&&fence->GetCompletedValue()>=completed;
    CloseHandle(event);if(!done)return 0;
    void* mapped{};D3D12_RANGE range{0,SIZE_T(bytes)};if(FAILED(readback->Map(0,&range,&mapped)))return 0;
    const auto* pixel=static_cast<const unsigned char*>(mapped)+fp.Offset+8*fp.Footprint.RowPitch+16*4;
    verified=pixel[0]==128&&pixel[1]==64&&pixel[2]==32&&pixel[3]==255;readback->Unmap(0,nullptr);
    return 0; // Deliberate vendor suppression; this fixture never claims FG.
}
EXPORT cpvr_nv_status fg_nv_get_status(){cpvr_nv_status s{};s.completed=completed;std::strcpy(s.message,"WARP COPY FIXTURE; vendor generation suppressed");return s;}
EXPORT ID3D12CommandQueue* fg_nv_queue(){return queue.Get();}
EXPORT ID3D12Fence* fg_nv_fence(){return fence.Get();}
EXPORT int fg_nv_detach(){queue.Reset();fence.Reset();device.Reset();return 1;}
