#pragma once
#include <d3d12.h>
#include <wrl/client.h>

namespace cvr::gpu {
inline constexpr GUID NativeObjectIdentityKey{0xc9e16cc2,0x8718,0x4a45,{0x9b,0x63,0x71,0x2d,0x53,0xb1,0x84,0x40}};
struct NativeObjectRecord { unsigned long long version{1}; ID3D12Object* native{}; };
// Registered only at the native device/queue creation boundary, before wrappers.
// ReShade forwards Get/SetPrivateData to the native object. Its IUnknown QI
// deliberately returns the wrapper, so COM identity alone cannot unwrap it.
// Store non-owning bytes, not SetPrivateDataInterface(self): the latter creates
// a self-reference cycle. This record dies with the native object. The caller's
// live native/wrapper reference keeps that object alive during QueryInterface.
inline bool RegisterNativeObject(ID3D12Object* object) {
    if(!object)return false;
    NativeObjectRecord current{};UINT size=sizeof(current);
    const auto hr=object->GetPrivateData(NativeObjectIdentityKey,&size,&current);
    if(SUCCEEDED(hr))return size==sizeof(current)&&current.version==1&&current.native==object;
    if(hr!=DXGI_ERROR_NOT_FOUND)return false;
    const NativeObjectRecord record{1,object};
    return SUCCEEDED(object->SetPrivateData(NativeObjectIdentityKey,sizeof(record),&record));
}
template<class T> Microsoft::WRL::ComPtr<T> ResolveNativeObject(ID3D12Object* object) {
    Microsoft::WRL::ComPtr<T> result;
    if(!object)return result;
    NativeObjectRecord record{};UINT size=sizeof(record);
    auto* target=object;
    if(SUCCEEDED(object->GetPrivateData(NativeObjectIdentityKey,&size,&record))&&
       size==sizeof(record)&&record.version==1&&record.native)target=record.native;
    target->QueryInterface(IID_PPV_ARGS(&result));
    return result;
}
inline bool SameNativeDevice(ID3D12Device* a,ID3D12Device* b) {
    const auto first=ResolveNativeObject<IUnknown>(a);
    const auto second=ResolveNativeObject<IUnknown>(b);
    return first&&second&&first.Get()==second.Get();
}
}
