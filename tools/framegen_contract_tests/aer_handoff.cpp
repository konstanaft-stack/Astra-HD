#include "Framegen/FramePolicy.hpp"
#include <memory>
#include <cstdio>
#include <cstdlib>
namespace cvr::framegen {
struct NativeGenerationBridge {
    unsigned consumers{},collects{};
    void Collect(){++collects;}
    struct StatusValue{unsigned retainedConsumers;};
    StatusValue Status()const{return{consumers};}
};
struct GeneratorFixture{bool completed=true;unsigned resets{};bool Reset(){++resets;return completed;}};
struct Presenter {
    Cadence cadence;GeneratorFixture generator;std::shared_ptr<NativeGenerationBridge> nativeBridge;
    unsigned resets{};
    void Reset(){++resets;cadence.Reset();}
    bool SetNativeBridge(std::shared_ptr<NativeGenerationBridge>);
};
// Exact current production method, extracted by the runner before compilation.
#include "aer_handoff_method.inc"
}
static unsigned checks;
static void check(bool value,const char* text){++checks;if(!value){std::fprintf(stderr,"FAIL %s\n",text);std::exit(1);}}
int main(){
    using namespace cvr::framegen;
    Presenter p;auto bridge=std::make_shared<NativeGenerationBridge>();
    p.cadence.Accepted(Cadence::Next::Midpoint,41);p.generator.completed=false;
    check(!p.SetNativeBridge(bridge) && p.cadence.Pending()==41,"busy GPU retains pending endpoint and old owner");
    p.generator.completed=true;
    check(p.SetNativeBridge(bridge),"explicit new owner can retire old midpoint without displaying obsolete pending real");
    check(!p.cadence.Pending() && p.nativeBridge==bridge,"successful handoff resets old CPU cadence");
    bridge->consumers=1;
    check(!p.SetNativeBridge({}) && p.nativeBridge==bridge,"native GPU consumers prevent early detach");
    bridge->consumers=0;
    check(p.SetNativeBridge({}) && !p.nativeBridge,"completed native consumers allow author-mode return");
    std::printf("PASS %u production handoff checks\n",checks);
}
