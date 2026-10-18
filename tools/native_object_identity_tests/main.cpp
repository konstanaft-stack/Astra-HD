#include "Render/NativeObjectIdentity.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using Microsoft::WRL::ComPtr;
void Check(bool ok,const char* text){if(!ok){std::fprintf(stderr,"FAIL: %s\n",text);std::exit(1);}}
struct Object:ID3D12Object {
    ULONG refs{1};unsigned* deaths{};ComPtr<ID3D12Object> underlying;std::vector<unsigned char> data;
    Object(unsigned& destroyed,ID3D12Object* native=nullptr):deaths(&destroyed),underlying(native){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;
        if(id!=__uuidof(IUnknown)&&id!=__uuidof(ID3D12Object))return E_NOINTERFACE;
        *out=static_cast<ID3D12Object*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return ++refs;}
    ULONG STDMETHODCALLTYPE Release() override{const auto n=--refs;if(!n){++*deaths;delete this;}return n;}
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID key,UINT* size,void* value) override {
        if(underlying)return underlying->GetPrivateData(key,size,value);
        if(key!=cvr::gpu::NativeObjectIdentityKey||data.empty())return DXGI_ERROR_NOT_FOUND;
        if(!size)return E_INVALIDARG;
        const auto capacity=*size;*size=static_cast<UINT>(data.size());
        if(capacity<data.size())return DXGI_ERROR_MORE_DATA;
        if(value)std::memcpy(value,data.data(),data.size());return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID key,UINT size,const void* value) override {
        if(underlying)return underlying->SetPrivateData(key,size,value);
        if(key!=cvr::gpu::NativeObjectIdentityKey)return E_INVALIDARG;
        data.clear();if(size&&value)data.assign(static_cast<const unsigned char*>(value),static_cast<const unsigned char*>(value)+size);return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID,const IUnknown*) override{std::abort();}
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override{return S_OK;}
};
int main(){
    unsigned deaths=0;
    ComPtr<Object> native;native.Attach(new Object(deaths));
    ComPtr<Object> other;other.Attach(new Object(deaths));
    Check(cvr::gpu::RegisterNativeObject(native.Get()),"register native");
    ComPtr<Object> wrapper;wrapper.Attach(new Object(deaths,native.Get()));
    for(unsigned i=0;i<1000;++i){
        auto resolved=cvr::gpu::ResolveNativeObject<ID3D12Object>(wrapper.Get());
        Check(resolved.Get()==native.Get(),"forwarding wrapper must resolve to exact producer object");
        Check(resolved.Get()!=other.Get(),"must not substitute another native object");
    }
    Check(native->refs==2,"resolution leaked or consumed a reference");
    Check(!cvr::gpu::RegisterNativeObject(wrapper.Get()),"wrapper must not overwrite native registration");
    Check(cvr::gpu::ResolveNativeObject<ID3D12Object>(wrapper.Get()).Get()==native.Get(),"registration was overwritten");
    Check(cvr::gpu::RegisterNativeObject(native.Get()),"repeat native registration is idempotent");
    Check(cvr::gpu::ResolveNativeObject<ID3D12Object>(other.Get()).Get()==other.Get(),"unregistered native fallback");
    Check(!cvr::gpu::ResolveNativeObject<ID3D12Resource>(wrapper.Get()),"wrong interface must be rejected");
    Check(!cvr::gpu::ResolveNativeObject<ID3D12Object>(nullptr),"null must stay null");
    native.Reset();Check(deaths==0,"wrapper must keep native alive");
    auto owned=cvr::gpu::ResolveNativeObject<ID3D12Object>(wrapper.Get());
    wrapper.Reset();Check(deaths==1,"wrapper destroyed, acquired native must survive");
    owned.Reset();Check(deaths==2,"native must die without a private-data COM cycle");
    other->data.assign(1,0);
    Check(cvr::gpu::ResolveNativeObject<ID3D12Object>(other.Get()).Get()==other.Get(),"malformed record must not be dereferenced");
    other.Reset();Check(deaths==3,"all owners released");
    std::puts("PASS: native and forwarding-wrapper identity, isolation, malformed data and COM lifetime");
}
