#pragma once
#include <array>
#include <cstdint>
#include <cstddef>

namespace cvr::framegen {
struct VrcamOffReadback {
    uint32_t inspected{};
    bool readable{},allDisabled{};
    explicit operator bool()const{return inspected>0 && readable && allDisabled;}
};
template<class Entity,std::size_t Capacity=8> class VrcamOffEntities {
    uint64_t world{};
    Entity current{};
    std::array<Entity,Capacity> previous{};
    bool overflow{};
public:
    void Reset(uint64_t epoch){if(epoch>=world){world=epoch;current={};previous={};overflow=false;}}
    template<class Inspect> VrcamOffReadback Observe(uint64_t epoch,const Entity& player,Inspect&& inspect){
        if(!epoch||epoch<world)return {};
        if(epoch!=world)Reset(epoch);
        if(current && current!=player){
            bool known=false;for(const auto& old:previous)if(old==current)known=true;
            if(!known){std::size_t slot=0;for(;slot<Capacity&&previous[slot];++slot){}
                if(slot<Capacity)previous[slot]=current;else overflow=true;}
        }
        current=player;
        bool previousOff=!overflow;
        for(auto& old:previous)if(old){
            const auto result=inspect(old);
            // An old entity may no longer have camera components. It can retire
            // only after a readable scan; the current entity still needs count>0.
            if(result.readable&&result.allDisabled)old={};else previousOff=false;
        }
        auto result=inspect(current);result.allDisabled&=previousOff;return result;
    }
};
}
