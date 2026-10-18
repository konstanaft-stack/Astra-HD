#include "Framegen/ColorRead.hpp"
#include "Render/SrgbToLinearPass.hpp"
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
using namespace cvr::framegen;
void Log(const char*,...) {}
static void Check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
static void Barrier(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};list->ResourceBarrier(1,&b);
}
struct Commands {
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    explicit Commands(ID3D12Device* device) {
        Check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator))) &&
            SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list))),"WARP command creation failed");
    }
    void Submit(ID3D12CommandQueue* queue,ID3D12Fence* fence,uint64_t value) {
        Check(SUCCEEDED(list->Close()),"WARP command Close failed");ID3D12CommandList* lists[]={list.Get()};queue->ExecuteCommandLists(1,lists);
        Check(SUCCEEDED(queue->Signal(fence,value)),"WARP Signal failed");
    }
};
static ComPtr<ID3D12Resource> Buffer(ID3D12Device* device,uint64_t bytes,D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES heap{};heap.Type=type;D3D12_RESOURCE_DESC desc{};
    desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=bytes;desc.Height=desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;
    desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;ComPtr<ID3D12Resource> out;
    Check(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
        type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&out))),"WARP buffer failed");return out;
}
static D3D12_TEXTURE_COPY_LOCATION Texture(ID3D12Resource* resource) {D3D12_TEXTURE_COPY_LOCATION out{};out.pResource=resource;out.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;return out;}
static D3D12_TEXTURE_COPY_LOCATION Placed(ID3D12Resource* resource,D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint) {
    D3D12_TEXTURE_COPY_LOCATION out{};out.pResource=resource;out.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;out.PlacedFootprint=footprint;return out;
}
static void Complete(ID3D12Fence* fence,uint64_t value,HANDLE event) {
    if(fence->GetCompletedValue()<value)Check(SUCCEEDED(fence->SetEventOnCompletion(value,event)) && WaitForSingleObject(event,5000)==WAIT_OBJECT_0,"WARP bounded completion failed");
    Check(fence->GetCompletedValue()!=UINT64_MAX && fence->GetCompletedValue()>=value,"WARP completion unproven");
}
int main() {
    try {
        ComPtr<ID3D12Debug> debug;
        if(FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {std::cout<<"NOT_RUN D3D12 debug layer unavailable\n";return 77;}
        debug->EnableDebugLayer();
        ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter1> adapter;ComPtr<ID3D12Device> device;
        Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))),"WARP adapter missing");
        DXGI_ADAPTER_DESC1 adapterDesc{};Check(SUCCEEDED(adapter->GetDesc1(&adapterDesc)) && (adapterDesc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE),"probe refused a hardware adapter");
        Check(SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))),"WARP device creation failed");
        ComPtr<ID3D12InfoQueue> info;Check(SUCCEEDED(device.As(&info)),"WARP debug info queue missing");
        D3D12_COMMAND_QUEUE_DESC queueDesc{};queueDesc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> author,worker;
        Check(SUCCEEDED(device->CreateCommandQueue(&queueDesc,IID_PPV_ARGS(&author))) && SUCCEEDED(device->CreateCommandQueue(&queueDesc,IID_PPV_ARGS(&worker))),"WARP queues failed");
        ComPtr<ID3D12Fence> produced,copied,decoded;
        Check(SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&produced))) &&
            SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&copied))) &&
            SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&decoded))),"WARP fences failed");
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);Check(event!=nullptr,"WARP event failed");
        uint64_t sequence=0;
        for(auto format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_R8G8B8A8_TYPELESS})for(unsigned size:{8u,16u}) {
            D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC desc{};
            desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=desc.Height=size;desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;
            desc.Format=format;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            std::array<ComPtr<ID3D12Resource>,3> slots;
            for(auto& slot:slots)Check(SUCCEEDED(CreateMainCaptureColor(device.Get(),heap,desc,slot.GetAddressOf())),"WARP MAIN allocation failed");
            SrgbToLinearPass pass;Check(pass.EnsureInitialized(device.Get(),DXGI_FORMAT_R8G8B8A8_UNORM,size,size),"author decode initialization failed");
            for(unsigned frame=0;frame<9;++frame) {
                auto* source=slots[(frame+1)%3].Get();++sequence;
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT rows{};UINT64 rowBytes{},total{};
                device->GetCopyableFootprints(&desc,0,1,0,&footprint,&rows,&rowBytes,&total);
                auto upload=Buffer(device.Get(),total,D3D12_HEAP_TYPE_UPLOAD);void* mapped{};D3D12_RANGE none{};
                Check(SUCCEEDED(upload->Map(0,&none,&mapped)),"WARP upload map failed");
                for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x) {
                    auto* pixel=static_cast<unsigned char*>(mapped)+footprint.Offset+y*footprint.Footprint.RowPitch+x*4;
                    pixel[0]=128;pixel[1]=64;pixel[2]=32;pixel[3]=255;
                }
                upload->Unmap(0,nullptr);Commands capture(device.Get());auto write=MainCaptureWriteBarrier(source);capture.list->ResourceBarrier(1,&write);
                auto srcUpload=Placed(upload.Get(),footprint),destination=Texture(source);capture.list->CopyTextureRegion(&destination,0,0,0,&srcUpload,nullptr);
                auto ready=MainCaptureReadyBarrier(source);capture.list->ResourceBarrier(1,&ready);capture.Submit(author.Get(),produced.Get(),sequence);
                auto targetDesc=desc;targetDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
                ComPtr<ID3D12Resource> privateCopy,decodeTarget;
                Check(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&targetDesc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&privateCopy))) &&
                    SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&targetDesc,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&decodeTarget))),"WARP output allocation failed");
                device->GetCopyableFootprints(&targetDesc,0,1,0,&footprint,&rows,&rowBytes,&total);
                auto copyReadback=Buffer(device.Get(),total,D3D12_HEAP_TYPE_READBACK),decodeReadback=Buffer(device.Get(),total,D3D12_HEAP_TYPE_READBACK);
                Commands copy(device.Get()),draw(device.Get());
                copy.list->CopyResource(privateCopy.Get(),source);Barrier(copy.list.Get(),privateCopy.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);
                auto copyFrom=Texture(privateCopy.Get()),copyTo=Placed(copyReadback.Get(),footprint);copy.list->CopyTextureRegion(&copyTo,0,0,0,&copyFrom,nullptr);
                Barrier(copy.list.Get(),privateCopy.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
                Check(RecordColorDecode(pass,draw.list.Get(),source,decodeTarget.Get(),0,CaptureColorReadState),"actual author decode recording failed");
                Barrier(draw.list.Get(),decodeTarget.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
                auto decodeFrom=Texture(decodeTarget.Get()),decodeTo=Placed(decodeReadback.Get(),footprint);draw.list->CopyTextureRegion(&decodeTo,0,0,0,&decodeFrom,nullptr);
                Barrier(draw.list.Get(),decodeTarget.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
                // Neither reader waits for the other. Both depend only on the
                // producer and keep the real color in the same shared state.
                Check(SUCCEEDED(worker->Wait(produced.Get(),sequence)) && SUCCEEDED(author->Wait(produced.Get(),sequence)),"WARP producer ordering failed");
                copy.Submit(worker.Get(),copied.Get(),sequence);draw.Submit(author.Get(),decoded.Get(),sequence);
                Complete(copied.Get(),sequence,event);Complete(decoded.Get(),sequence,event);
                const std::array<int,4> raw{128,64,32,255},linear{55,13,4,255};
                for(unsigned kind=0;kind<2;++kind) {
                    auto* readback=kind?decodeReadback.Get():copyReadback.Get();D3D12_RANGE range{0,SIZE_T(total)};
                    Check(SUCCEEDED(readback->Map(0,&range,&mapped)),"WARP readback failed");
                    for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x) {
                        const auto* bytes=static_cast<const unsigned char*>(mapped)+footprint.Offset+y*footprint.Footprint.RowPitch+x*4;
                        for(unsigned channel=0;channel<4;++channel)Check(std::abs(int(bytes[channel])-(kind?linear[channel]:raw[channel]))<=(kind?1:0),"WARP copy/decode pixel mismatch");
                    }
                    readback->Unmap(0,&none);
                }
            }
        }
        CloseHandle(event);unsigned errors=0;
        for(UINT64 index=0;index<info->GetNumStoredMessagesAllowedByRetrievalFilter();++index) {
            SIZE_T bytes{};info->GetMessage(index,nullptr,&bytes);std::vector<unsigned char> storage(bytes);
            auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());info->GetMessage(index,message,&bytes);
            if(message->Severity==D3D12_MESSAGE_SEVERITY_ERROR || message->Severity==D3D12_MESSAGE_SEVERITY_CORRUPTION) {
                if(!errors)std::cerr<<message->pDescription<<'\n';++errors;
            }
        }
        Check(errors==0,"WARP debug layer reported resource/state errors");
        std::cout<<"PASS WARP software API: 54 captures, three slots, resize, RGBA8 formats, independent DIRECT copy/decode, exact raw bytes, decoded pixels, debug errors=0\n";return 0;
    } catch(const std::exception& error) {std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
}
