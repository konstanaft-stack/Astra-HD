#pragma once
#include "MenuPreview/SpatialEditorPolicy.hpp"
#include <windows.h>
#include <limits>

namespace cvr::menu_preview {
// Only the recognized engine staging quad may be changed, immediately before
// its native copy. A readable source is not necessarily writable.
inline bool WriteEditorSourceIfUnchanged(void* source,const EditorQuad& before,const EditorQuad& after)noexcept{
    const auto address=reinterpret_cast<uintptr_t>(source);
    constexpr auto bytes=sizeof(EditorQuad);
    if(address<0x10000||bytes>std::numeric_limits<uintptr_t>::max()-address)return false;
    MEMORY_BASIC_INFORMATION region{};
    if(VirtualQuery(source,&region,sizeof(region))!=sizeof(region)||region.State!=MEM_COMMIT||
       (region.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
    const auto protection=region.Protect&0xffu;
    if(protection!=PAGE_READWRITE&&protection!=PAGE_WRITECOPY)return false;
    const auto base=reinterpret_cast<uintptr_t>(region.BaseAddress);
    if(address<base||address-base>region.RegionSize||bytes>region.RegionSize-(address-base))return false;
    __try{
        if(std::memcmp(source,before.data(),bytes))return false;
        std::memcpy(source,after.data(),bytes);
        return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
}
