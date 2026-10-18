#include "MenuPreview/PortraitRotationPolicy.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
using namespace cvr::menu_preview;
void require(bool value,const char* why){if(!value){std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);}}
int main(){
    std::array<PortraitRotationCell,8> cells{};uint64_t stamp{};
    const auto admit=[&](uintptr_t resource,uint64_t generation,uintptr_t alias,uint64_t aliasGeneration){
        const auto slot=SelectPortraitRotationSlot(cells,resource,generation,alias,aliasGeneration);
        if(slot)cells[*slot]={resource,generation,++stamp};
        return slot;
    };
    const auto alias=admit(100,1,0,0);require(bool(alias),"first primary RTT is admitted");
    for(uintptr_t resource=101;resource<1101;++resource){const auto slot=admit(resource,uint64_t(resource),100,1);
        require(slot&&*slot!=*alias,"continued actual RTT rotation cannot exhaust the cache or evict the current alias");}
    require(cells[*alias].resource==100&&cells[*alias].generation==1,"owned alias retains its exact resource generation across 1000 replacements");
    const auto reused=admit(1100,1100,100,1);require(reused&&cells[*reused].resource==1100,"same resource/generation reuses and refreshes its cell");
    const auto newest=*reused;const auto next=admit(1101,1101,100,1);require(next&&*next!=newest,"least-recent cell retires before the most recently used writer");
    require(!SelectPortraitRotationSlot(cells,0,1,100,1)&&!SelectPortraitRotationSlot(cells,200,0,100,1),"unknown resource/generation cannot acquire a proof cell");
    std::array<PortraitRotationCell,1> protectedOnly{{{5,8,10}}};
    require(!SelectPortraitRotationSlot(protectedOnly,6,9,5,8),"a fully protected bounded registry fails closed without discarding its alias");
    require(SelectPortraitRotationSlot(protectedOnly,5,8,5,8)==0,"current alias can refresh its own exact cell");
    require(SelectPortraitRotationSlot(protectedOnly,6,9,5,99)==0,"an old resource generation cannot borrow protection from a different generation");
    std::array<PortraitRotationCell,3> ordered{{{10,1,5},{11,2,8},{12,3,2}}};
    require(SelectPortraitRotationSlot(ordered,20,4,12,3)==0,"oldest unprotected writer retires while the older current alias remains");
    ordered[0].lastUse=99;
    require(SelectPortraitRotationSlot(ordered,10,7,12,3)==1,"new generation cannot match the more recent stale generation at the same address");
    require(!SelectPortraitRotationSlot({},20,4,0,0),"an empty bounded registry fails closed");
    std::puts("PASS bounded primary RTT rotation, exact generations, alias protection and LRU replacement");
}
