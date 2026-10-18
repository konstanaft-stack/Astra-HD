#pragma once
#include <cstdint>
namespace cvr::menu_preview {
// Remove only the bare writer exclusion for this rejected pixel version.
// Reader pins, slot completion and all GPU/COM ownership remain unchanged.
inline bool AbandonRejectedUiPublication(bool unknown,uintptr_t poolFence,uintptr_t frameFence,
    uintptr_t resource,uint64_t value,uintptr_t& published,uint64_t& publishedValue)noexcept{
    if(unknown||!poolFence||poolFence!=frameFence||!resource||!value||value==UINT64_MAX||
       published!=resource||publishedValue!=value)return false;
    published=0;publishedValue=0;return true;
}
}
