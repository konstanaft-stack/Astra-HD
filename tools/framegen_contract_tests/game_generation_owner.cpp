#include "Framegen/GameGenerationOwner.hpp"
#include <cstdio>
#include <cstdlib>
using namespace cvr::framegen;
void Check(bool ok,const char* message){if(!ok){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}}
int main(){
    GameGenerationSelection selection;
    NativePolicyStatus api{};
    Check(ResolveGenerationOwner(true,selection.Read(100),api,false)==GenerationOwner::Unknown,"missing game state is not OFF");
    selection.Observe(GameGenerationProvider::Off,0,100);
    Check(ResolveGenerationOwner(true,selection.Read(101),api,false)==GenerationOwner::Owned,"fresh game OFF without SDK calls must allow own generator");
    selection.Observe(GameGenerationProvider::Nvidia,3,200);
    Check(ResolveGenerationOwner(true,selection.Read(201),{true,false,0,1},false)==GenerationOwner::Game,"temporary SDK OFF in menu must not override selected game FG");
    selection.Observe(GameGenerationProvider::Off,0,300);
    Check(ResolveGenerationOwner(true,selection.Read(301),{true,true,3,2},true)==GenerationOwner::Game,"native ON wins over a pending game OFF selection");
    Check(ResolveGenerationOwner(true,selection.Read(301),{},true)==GenerationOwner::Unknown,"failed native transition must not start second generator");
    Check(ResolveGenerationOwner(true,selection.Read(301),{true,false,0,3},true)==GenerationOwner::Owned,"confirmed native OFF must unlock own generator");
    Check(ResolveGenerationOwner(true,selection.Read(3301),{true,false,0,3},true)==GenerationOwner::Unknown,"expired game state must not unlock controls");
    Check(!selection.Read(299).known,"clock reversal must invalidate selection");
    selection.Observe(GameGenerationProvider::Other,0,400);
    Check(ResolveGenerationOwner(true,selection.Read(401),{true,false,0,4},true)==GenerationOwner::Game,"other in-game generator reserves ownership too");
    Check(ResolveGenerationOwner(false,selection.Read(401),{},false)==GenerationOwner::Game,"unsupported game generator still excludes own generator");
    selection.Observe(GameGenerationProvider::Unknown,0,500);
    Check(!selection.Read(501).known,"invalid/unreadable provider must not become OFF");
    Check(ResolveGenerationOwner(false,selection.Read(501),{},false)==GenerationOwner::Owned,"standalone own-FG path must remain usable");
    for(unsigned multiplier=2;multiplier<=4;++multiplier){
        selection.Observe(GameGenerationProvider::Nvidia,multiplier,600+multiplier);
        auto value=selection.Read(610);
        Check(value.known&&value.multiplier==multiplier,"game X2/X3/X4 readback");
        Check(ResolveGenerationOwner(true,value,{},false)==GenerationOwner::Game,"pending native activation reserves ownership");
    }
    selection.Observe(GameGenerationProvider::Nvidia,99,700);
    Check(selection.Read(701).known&&selection.Read(701).multiplier==0,"invalid multiplier must stay unknown, not clamped to X4");
    std::puts("PASS: game selection freshness, restart/pause states, all owner exclusions and X2/X3/X4");
}
