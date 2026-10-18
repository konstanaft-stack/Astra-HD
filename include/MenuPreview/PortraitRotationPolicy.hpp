#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace cvr::menu_preview {
struct PortraitRotationCell {uintptr_t resource{};uint64_t generation{},lastUse{};};
// Bounded source-proof cache, not GPU ownership. Recorded events/snapshots keep
// their independent resource owners after a proof cell has been retired.
inline std::optional<size_t> SelectPortraitRotationSlot(std::span<const PortraitRotationCell> cells,
    uintptr_t resource,uint64_t generation,uintptr_t aliasResource,uint64_t aliasGeneration)noexcept {
    if(!resource||!generation)return {};
    for(size_t i=0;i<cells.size();++i)if(cells[i].resource==resource&&cells[i].generation==generation)return i;
    for(size_t i=0;i<cells.size();++i)if(!cells[i].resource)return i;
    std::optional<size_t> oldest;
    for(size_t i=0;i<cells.size();++i){const auto& cell=cells[i];
        if(aliasResource&&cell.resource==aliasResource&&cell.generation==aliasGeneration)continue;
        if(!oldest||cell.lastUse<cells[*oldest].lastUse)oldest=i;
    }
    return oldest;
}
}
