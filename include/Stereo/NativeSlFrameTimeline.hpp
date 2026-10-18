#pragma once
#include <array>
#include <cstdint>
#include <optional>
namespace cvr::stereo {
// MAIN factory supplies BOTH values. Never derive one clock from the other or
// retain a view pointer: native view storage is reused before RTT execution.
// The caller serializes this small scalar table, not native rendering calls.
class NativeSlFrameTimeline {
    struct Stamp {uint32_t frame{},index{};bool known{},conflict{};};
    uintptr_t rendererOwner{};
    std::array<Stamp,8> stamps{};
public:
    void Publish(uintptr_t owner,uint32_t frame,uint32_t index) {
        if(!owner)return;
        if(rendererOwner!=owner){rendererOwner=owner;stamps={};}
        auto& slot=stamps[frame%stamps.size()];
        if(slot.known && slot.frame==frame){
            if(slot.index!=index)slot.conflict=true;
        }else slot={frame,index,true,false};
    }
    std::optional<uint32_t> Resolve(uintptr_t owner,uint32_t frame) const {
        if(!owner || owner!=rendererOwner)return {};
        const auto& s=stamps[frame%stamps.size()];
        if(!s.known || s.conflict || s.frame!=frame)return {};
        return s.index;
    }
};
}
