#pragma once
#include <cstdint>
#include <limits>
namespace cvr::menu_preview {
struct UiAllocation {uint32_t kind{};uint64_t heap{},offset{},bytes{};};
inline bool UiAllocationsDisjoint(const UiAllocation& a,const UiAllocation& b) noexcept {
    if(a.kind==1||b.kind==1)return true;
    const auto valid=[](const UiAllocation& v){return v.kind==2&&v.heap&&v.bytes&&v.offset<=std::numeric_limits<uint64_t>::max()-v.bytes;};
    if(!valid(a)||!valid(b))return false;
    return a.heap!=b.heap||a.offset+a.bytes<=b.offset||b.offset+b.bytes<=a.offset;
}
inline bool UiAliasInvalidates(uintptr_t resource,const UiAllocation& allocation,
                              uintptr_t before,const UiAllocation& beforeAllocation,
                              uintptr_t after,const UiAllocation& afterAllocation) noexcept {
    if(resource==before||resource==after)return true;
    if(allocation.kind==1)return false;
    if(after)return !UiAllocationsDisjoint(allocation,afterAllocation);
    if(before)return !UiAllocationsDisjoint(allocation,beforeAllocation);
    return true;
}
}
