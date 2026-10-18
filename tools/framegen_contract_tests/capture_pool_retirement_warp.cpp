#include "Runtimes/CapturePoolRetirement.hpp"
#include <dxgi1_4.h>
#include <d3d12sdklayers.h>
#include <atomic>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
using namespace cvr::capture;
static void Check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
static constexpr GUID WitnessKey={0x18a8a0b1,0xa318,0x43a4,{0xa7,0xc2,0xd6,0x53,0x85,0x1c,0x9c,0x4a}};
struct Witness final:IUnknown {
    std::atomic<ULONG> refs{1};std::shared_ptr<std::atomic<unsigned>> deaths;
    explicit Witness(std::shared_ptr<std::atomic<unsigned>> value):deaths(std::move(value)){}
    ~Witness(){++*deaths;}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;if(id!=__uuidof(IUnknown))return E_NOINTERFACE;
        *out=static_cast<IUnknown*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return ++refs;}
    ULONG STDMETHODCALLTYPE Release() override{const auto value=--refs;if(!value)delete this;return value;}
};
struct Commands {ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;};
struct Context {
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12Fence> fence;ComPtr<ID3D12InfoQueue> info;
    ComPtr<ID3D12Resource> upload;uint64_t value{};
    Context(){
        ComPtr<ID3D12Debug> debug;Check(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))),"NOT_RUN debug layer unavailable");debug->EnableDebugLayer();
        ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter1> adapter;DXGI_ADAPTER_DESC1 desc{};
        Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))&&SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)))&&
            SUCCEEDED(adapter->GetDesc1(&desc))&&(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE),"fixture requires WARP");
        Check(SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))),"WARP device failed");
        D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        Check(SUCCEEDED(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)))&&SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))),"queue/fence failed");
        Check(SUCCEEDED(device.As(&info)),"debug info queue failed");
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_UPLOAD;D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=2048;buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Check(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload))),"upload failed");
    }
    ComPtr<ID3D12Resource> Texture(std::shared_ptr<std::atomic<unsigned>> deaths){
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=d.Height=8;
        d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource> result;
        Check(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&result))),"texture failed");
        auto* witness=new Witness(std::move(deaths));const auto hr=result->SetPrivateDataInterface(WitnessKey,witness);witness->Release();
        Check(SUCCEEDED(hr),"resource lifetime witness failed");return result;
    }
    Commands Write(ID3D12Resource* target){
        Commands commands;Check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&commands.allocator)))&&
            SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,commands.allocator.Get(),nullptr,IID_PPV_ARGS(&commands.list))),"write list failed");
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint={DXGI_FORMAT_R8G8B8A8_UNORM,8,8,1,256};dst.pResource=target;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        commands.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);Check(SUCCEEDED(commands.list->Close()),"write close failed");
        ID3D12CommandList* list=commands.list.Get();queue->ExecuteCommandLists(1,&list);return commands;
    }
    void Wait(uint64_t target){
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);Check(event!=nullptr,"event failed");
        const bool ready=SUCCEEDED(fence->SetEventOnCompletion(target,event))&&WaitForSingleObject(event,5000)==WAIT_OBJECT_0&&
            fence->GetCompletedValue()!=UINT64_MAX&&fence->GetCompletedValue()>=target;
        CloseHandle(event);Check(ready,"completion timed out");
    }
    void Debug(){
        for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T size{};info->GetMessage(i,nullptr,&size);std::vector<unsigned char> bytes(size);
            auto* message=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());info->GetMessage(i,message,&size);
            if(message->Severity==D3D12_MESSAGE_SEVERITY_ERROR||message->Severity==D3D12_MESSAGE_SEVERITY_CORRUPTION)throw std::runtime_error(message->pDescription);
        }
    }
};
int main(){
    try{
        Context c;auto released=std::make_shared<std::atomic<unsigned>>(0);ComPtr<ID3D12Fence> blocker;
        Check(SUCCEEDED(c.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&blocker))),"blocker failed");
        struct Unblock {ID3D12Fence* fence;~Unblock(){fence->Signal(1);}} unblock{blocker.Get()};
        auto color=c.Texture(released);Check(SUCCEEDED(c.queue->Wait(blocker.Get(),1)),"queue wait failed");
        auto write=c.Write(color.Get());const auto completed=++c.value;
        Check(SUCCEEDED(c.queue->Signal(c.fence.Get(),completed)),"producer Signal failed");
        PoolProducerUse use;use.Executed(c.fence.Get(),completed,true);MainPoolRetirement retirement;
        ID3D12Resource* pool[]{color.Get()};Check(!use.Complete()&&retirement.CanRetire(use)&&retirement.Retire(pool,use),"blocked pool was not retained");
        color.Reset();Check(retirement.Collect()==0&&retirement.Pending()==1&&released->load()==0,"last COM reference died before producer completion");
        Check(SUCCEEDED(blocker->Signal(1)),"producer unblock failed");c.Wait(completed);
        Check(retirement.Collect()==1&&retirement.Pending()==0&&released->load()==1,"completed pool/witness did not release");
        std::cout<<"PASS blocked actual WARP write: sole resource owner survives retirement, then releases at Signal completion\n";
        auto unknownDeaths=std::make_shared<std::atomic<unsigned>>(0);std::vector<Commands> unknownCommands;
        {
            MainPoolRetirement unknownPools;PoolProducerUse unknown;
            // These writes really execute; deliberately withhold their own
            // success Signal receipt. A later independent drain is not allowed
            // to erase the production policy's unknown-completion state.
            for(unsigned i=0;i<MainPoolRetirement::MaximumGenerations;++i){
                auto source=c.Texture(unknownDeaths);unknownCommands.push_back(c.Write(source.Get()));
                unknown.Executed(nullptr,0,false);ID3D12Resource* generation[]{source.Get()};
                Check(unknownPools.Retire(generation,unknown),"bounded unknown generation was lost");source.Reset();
            }
            const auto later=++c.value;Check(SUCCEEDED(c.queue->Signal(c.fence.Get(),later)),"independent drain Signal failed");c.Wait(later);
            unknown.Executed(c.fence.Get(),later,true);
            Check(!unknown.Complete()&&!unknownPools.CanRetire(unknown)&&unknownPools.Collect()==0&&
                  unknownPools.Pending()==3&&unknownDeaths->load()==0,"later Signal erased sticky unknown/capacity");
            auto rejectedDeaths=std::make_shared<std::atomic<unsigned>>(0);auto fourth=c.Texture(rejectedDeaths);ID3D12Resource* extra[]{fourth.Get()};
            Check(!unknownPools.Retire(extra,unknown)&&unknownPools.Pending()==3,"fourth retained generation exceeded bound");
            fourth.Reset();Check(rejectedDeaths->load()==1,"never-submitted rejected resource lost caller ownership");
        }
        Check(unknownDeaths->load()==0,"destruction falsely freed unknown producer work");c.Debug();
        std::cout<<"PASS sticky unknown, independent higher completed fence, bound3, rejected fourth, teardown retention; debug errors0\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}
}
