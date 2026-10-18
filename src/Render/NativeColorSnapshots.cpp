#include "Render/NativeColorSnapshots.hpp"
#include "Render/CommandResources.hpp"
#include "Render/RenderProbeScope.hpp"
#include <atomic>
namespace cvr::render {
using Microsoft::WRL::ComPtr;
struct NativeColorSnapshots::Cell {
    ComPtr<ID3D12Resource> texture;
    ComPtr<IUnknown> deviceIdentity;
    D3D12_RESOURCE_DESC description{};
};
namespace {
class SnapshotLifetime final:public IUnknown {
    std::atomic<ULONG> refs{1};std::shared_ptr<void> cell;
public:
    explicit SnapshotLifetime(std::shared_ptr<void> owner):cell(std::move(owner)){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out)override{
        if(!out)return E_POINTER;*out=nullptr;if(id!=__uuidof(IUnknown))return E_NOINTERFACE;*out=this;AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}
    ULONG STDMETHODCALLTYPE Release()override{const auto left=--refs;if(!left)delete this;return left;}
};
void Transition(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){
    if(from==to)return;D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};list->ResourceBarrier(1,&b);
}
}
NativeColorSnapshot NativeColorSnapshots::Record(ID3D12GraphicsCommandList* list,ID3D12Resource* source,D3D12_RESOURCE_STATES state){
    if(!list||!source)return {};
    const auto desc=source->GetDesc();
    if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||!desc.Width||!desc.Height||desc.MipLevels!=1||
       desc.DepthOrArraySize!=1||desc.SampleDesc.Count!=1)return {};
    ComPtr<ID3D12Device> device,sourceDevice;ComPtr<IUnknown> identity,sourceIdentity;
    if(FAILED(list->GetDevice(IID_PPV_ARGS(&device)))||FAILED(source->GetDevice(IID_PPV_ARGS(&sourceDevice)))||
       FAILED(device.As(&identity))||FAILED(sourceDevice.As(&sourceIdentity))||identity!=sourceIdentity)return {};
    std::shared_ptr<Cell>* free{};
    for(auto& cell:cells)if(!cell||cell.use_count()==1){free=&cell;break;}
    if(!free)return {};
    try{
        if(!*free||(*free)->deviceIdentity!=identity||(*free)->description.Width!=desc.Width||
           (*free)->description.Height!=desc.Height||(*free)->description.Format!=desc.Format){
            auto fresh=std::make_shared<Cell>();fresh->description=desc;fresh->deviceIdentity=identity;
            auto target=desc;target.Alignment=0;target.Flags=D3D12_RESOURCE_FLAG_NONE;
            D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
            // Fixed resting state also covers an abandoned recording: no CPU
            // flag claims a transition that the GPU may never have executed.
            if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&target,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&fresh->texture))))return {};
            fresh->texture->SetName(L"CVR_native_selected_snapshot");*free=std::move(fresh);
        }
        ComPtr<IUnknown> lifetime;lifetime.Attach(new SnapshotLifetime(*free));
        if(!gpu::KeepCommandResources(list,{source,lifetime.Get()}))return {};
        stereo::probe::Scope internal;
        auto* destination=(*free)->texture.Get();
        Transition(list,source,state,D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(list,destination,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(destination,source);
        Transition(list,destination,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(list,source,D3D12_RESOURCE_STATE_COPY_SOURCE,state);
        return {(*free)->texture,std::move(lifetime)};
    }catch(...){return {};}
}
}
