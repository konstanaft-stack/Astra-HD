#include "cpvr/resource_metrics.hpp"
#include <atomic>
#include <mutex>
#include <limits>
namespace cpvr {
namespace {constexpr GUID accounting_tag{0x4dcacb8f,0x28d8,0x43fd,{0xa2,0x7f,0x61,0x18,0x90,0x59,0xee,0x47}};}
struct ResourceAccounting::State {
    std::mutex mutex;ResourceUsage usage;
    struct Ticket final: IUnknown {
        std::atomic<ULONG> references{1};std::shared_ptr<State> owner;std::uint64_t bytes{};bool counted{};
        Ticket(std::shared_ptr<State> s,std::uint64_t n):owner(std::move(s)),bytes(n){}
        ~Ticket(){if(counted){std::lock_guard lock(owner->mutex);--owner->usage.live_resources;owner->usage.live_bytes-=bytes;}}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out)override{
            if(!out)return E_POINTER;*out=nullptr;if(id!=__uuidof(IUnknown))return E_NOINTERFACE;*out=static_cast<IUnknown*>(this);AddRef();return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef()override{return ++references;}
        ULONG STDMETHODCALLTYPE Release()override{const auto remaining=--references;if(!remaining)delete this;return remaining;}
        void count(){std::lock_guard lock(owner->mutex);counted=true;++owner->usage.allocations;++owner->usage.live_resources;
            owner->usage.live_bytes+=bytes;owner->usage.peak_bytes=std::max(owner->usage.peak_bytes,owner->usage.live_bytes);}
    };
};
ResourceAccounting::ResourceAccounting():state_(std::make_shared<State>()){}
bool ResourceAccounting::track(ID3D12Device* device,ID3D12Resource* resource)noexcept{
    const auto fail=[&](){std::lock_guard lock(state_->mutex);++state_->usage.tracking_failures;return false;};
    if(!device||!resource)return fail();
    try{
        UINT existing=0;(void)resource->GetPrivateData(accounting_tag,&existing,nullptr);
        if(existing)return fail();
        ComPtr<ID3D12Device> actual;ComPtr<IUnknown> a,b;
        if(FAILED(resource->GetDevice(IID_PPV_ARGS(&actual)))||FAILED(device->QueryInterface(IID_PPV_ARGS(&a)))||
            FAILED(actual.As(&b))||a.Get()!=b.Get())return fail();
        const auto desc=resource->GetDesc();const auto info=device->GetResourceAllocationInfo(0,1,&desc);
        if(!info.SizeInBytes||info.SizeInBytes==std::numeric_limits<std::uint64_t>::max())return fail();
        auto* ticket=new State::Ticket(state_,info.SizeInBytes);
        const auto result=resource->SetPrivateDataInterface(accounting_tag,ticket);
        if(SUCCEEDED(result))ticket->count();ticket->Release();
        return SUCCEEDED(result)?true:fail();
    }catch(...){return fail();}
}
ResourceUsage ResourceAccounting::snapshot()const noexcept{std::lock_guard lock(state_->mutex);return state_->usage;}
}
