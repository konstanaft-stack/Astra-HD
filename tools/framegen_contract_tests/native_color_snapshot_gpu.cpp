#include "Render/NativeColorSnapshots.hpp"
#include "Render/CommandResources.hpp"
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <cstdio>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
static unsigned checks;
static void Check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
static void Hr(HRESULT h,const char* why){Check(SUCCEEDED(h),why);}
static void Barrier(ID3D12GraphicsCommandList*l,ID3D12Resource*r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){D3D12_RESOURCE_BARRIER v{};v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;v.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};l->ResourceBarrier(1,&v);}
int main(){try{
    ComPtr<ID3D12Debug> debug;Hr(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)),"debug interface");debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter> warp;ComPtr<ID3D12Device> device;
    Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory");Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)),"WARP");Hr(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"device");
    ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC q{};Hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"queue");
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"allocator");Hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"list");
    D3D12_RESOURCE_DESC td{};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;td.Width=16;td.Height=8;td.DepthOrArraySize=td.MipLevels=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.SampleDesc.Count=1;td.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource> original;
    Hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&original)),"original texture");
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=1;ComPtr<ID3D12DescriptorHeap> heap;Hr(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)),"RTV heap");
    const auto rtv=heap->GetCPUDescriptorHandleForHeapStart();device->CreateRenderTargetView(original.Get(),nullptr,rtv);
    const float first[4]={.2f,.4f,.6f,1};list->ClearRenderTargetView(rtv,first,0,nullptr);Barrier(list.Get(),original.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cvr::render::NativeColorSnapshots pool;auto captured=pool.Record(list.Get(),original.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);Check(bool(captured),"selected source capture unavailable");
    Barrier(list.Get(),original.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float later[4]={.8f,.1f,.3f,1};list->ClearRenderTargetView(rtv,later,0,nullptr);Barrier(list.Get(),original.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes{};device->GetCopyableFootprints(&td,0,1,0,&footprint,nullptr,nullptr,&bytes);
    D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=bytes;bd.Height=bd.DepthOrArraySize=bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;hp.Type=D3D12_HEAP_TYPE_READBACK;ComPtr<ID3D12Resource> readback;
    Hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)),"readback");
    Barrier(list.Get(),captured.texture.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=captured.texture.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.pResource=readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprint;list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
    Barrier(list.Get(),captured.texture.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);Hr(list->Close(),"close");
    ID3D12CommandList* lists[]{list.Get()};auto retained=cvr::gpu::PrepareCommandResources(1,lists);queue->ExecuteCommandLists(1,lists);cvr::gpu::SubmitCommandResources(queue.Get(),std::move(retained));
    ComPtr<ID3D12Fence> fence;Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");Hr(queue->Signal(fence.Get(),1),"signal");HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);Check(event!=nullptr,"event");Hr(fence->SetEventOnCompletion(1,event),"completion");Check(WaitForSingleObject(event,10000)==WAIT_OBJECT_0,"GPU completion");CloseHandle(event);
    void* mapped{};Hr(readback->Map(0,nullptr,&mapped),"map");const auto* p=static_cast<const unsigned char*>(mapped);
    for(unsigned y=0;y<8;++y)for(unsigned x=0;x<16;++x)Check(p[y*footprint.Footprint.RowPitch+x*4]==51,"late writer replaced pixels of the selected source");readback->Unmap(0,nullptr);
    Check(captured.texture.Get()!=original.Get()&&bool(captured.lifetime),"source snapshot does not own independent storage");
    Hr(allocator->Reset(),"reset allocator");Hr(list->Reset(allocator.Get(),nullptr),"reset list");cvr::gpu::ResetCommandResources(list.Get());cvr::gpu::CollectCommandResources();
    std::array<cvr::render::NativeColorSnapshot,7> pending;for(auto& c:pending){c=pool.Record(list.Get(),original.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);Check(bool(c),"bounded pool slot unavailable");}
    Check(!pool.Record(list.Get(),original.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),"pool reused a retained source");
    captured={};pending={};Hr(list->Close(),"close abandoned");Hr(list->Reset(allocator.Get(),nullptr),"discard unsubmitted recording");cvr::gpu::ResetCommandResources(list.Get());cvr::gpu::CollectCommandResources();
    Check(bool(pool.Record(list.Get(),original.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)),"never-executed copy failed to release its pool slot");
    ComPtr<ID3D12InfoQueue> info;Hr(device.As(&info),"info queue");unsigned errors=0;
    for(UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T n{};info->GetMessage(i,nullptr,&n);std::vector<char> storage(n);auto* m=reinterpret_cast<D3D12_MESSAGE*>(storage.data());info->GetMessage(i,m,&n);if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR)++errors;}
    Check(errors==0,"D3D validation errors");std::printf("PASS %u snapshot checks; debug_errors=%u\n",checks,errors);return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}}
