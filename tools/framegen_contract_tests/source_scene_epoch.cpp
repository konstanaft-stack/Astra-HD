#include "Framegen/SourceSceneEpoch.hpp"
#include <cstdlib>
#include <iostream>

int main() {
    using cvr::framegen::SourceSceneEpoch;
    int n=0;
    auto check=[&](bool ok,const char* why){++n;if(!ok){std::cerr<<why<<'\n';std::exit(1);}};
    SourceSceneEpoch scene;
    check(!scene.BeginFrame().valid,"No attached world must not create a frame key");
    scene.Attach(0);
    check(!scene.BeginFrame().valid,"Null scene accepted");
    scene.Attach(0x1000);
    const auto first=scene.BeginFrame();
    check(first.valid&&first.ordinal==1&&first.worldEpoch!=0,"First real scene event not stamped");
    const auto second=scene.BeginFrame();
    check(second.worldEpoch==first.worldEpoch&&second.ordinal==2,"Frame ordinal not scoped to this world");
    scene.Detach(0x2000);
    check(scene.BeginFrame().valid,"Foreign scene invalidated live owner");
    scene.Pause(true);
    check(!scene.BeginFrame().valid,"Paused world issued active source frame");
    scene.Pause(false);
    check(scene.BeginFrame().valid,"Resume did not restore frame events");
    scene.Detach(0x1000);
    check(!scene.BeginFrame().valid,"Detached world still issues frames");
    scene.Attach(0x1000);
    const auto reused=scene.BeginFrame();
    check(reused.worldEpoch>first.worldEpoch&&reused.ordinal==1,"Reused scene address aliased the old generation");
    scene.Attach(0x1000);
    const auto attachedAgain=scene.BeginFrame();
    check(attachedAgain.worldEpoch>reused.worldEpoch,"Attach event must distinguish pointer reuse");
    scene.Shutdown();
    check(!scene.BeginFrame().valid,"Shutdown retained source eligibility");
    check(scene.Snapshot().worldEpoch>attachedAgain.worldEpoch,"Shutdown did not invalidate published receipts");
    std::cout<<n<<" scene lifecycle checks PASS\n";
}
