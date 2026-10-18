#pragma once
#include "gpu.hpp"

namespace cpvr {
// Getter follows production slGetNativeInterface: success transfers one owned
// reference, including when the returned native pointer equals the input.
template<class T,class Getter> ComPtr<T> native_interface_owned(IUnknown* original,Getter getter){
    ComPtr<T> result;if(!original)return result;
    ComPtr<IUnknown> returned_reference;
    void* unwrapped{};IUnknown* target=original;
    if(getter(original,&unwrapped)&&unwrapped){
        returned_reference.Attach(static_cast<IUnknown*>(unwrapped));target=returned_reference.Get();
    }
    target->QueryInterface(IID_PPV_ARGS(&result));
    return result;
}
}
