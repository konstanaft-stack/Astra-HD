#include "Framegen/RecordingBufferStates.hpp"
#include <memory>
#include <cstdio>
#include <cstdlib>
using namespace cvr::framegen;
void require(bool value,const char* message){if(!value){std::printf("FAIL %s\n",message);std::exit(1);}}
int main(){
    RecordingBufferStates<std::shared_ptr<int>,4> states;
    auto a=std::make_shared<int>(1),b=std::make_shared<int>(2);
    // Flag A is initialized while the evaluation scope still describes B.
    // The next evaluation must use the observed pre-call state of A.
    states.Transition(10,1,a,0,1024,BarrierPhase::Full,true);
    states.Transition(10,2,b,0,8,BarrierPhase::Full,true);
    require(states.Lookup(10,1)->known&&states.Lookup(10,1)->state==1024,"alternating flag lost pre-evaluation state");
    require(states.Lookup(10,2)->state==8,"second flag state mixed with first");
    states.InvalidateIf(10,[](uintptr_t key,const auto&){return key==2;});
    require(states.Lookup(10,1)->known&&!states.Lookup(10,2)->known,"unrelated alias changed first flag");
    require(!states.Lookup(11,1),"state escaped its recording");
    states.Reset(11);require(!states.Lookup(11,1),"reset retained an unsubmitted transition");
    states.Transition(10,1,a,0,8,BarrierPhase::Full,true);
    require(!states.Lookup(11,1),"late old recording reopened ledger");
    states.Transition(11,1,a,0,8,BarrierPhase::Begin,true);
    require(!states.Lookup(11,1)->known&&states.Lookup(11,1)->split,"split begin became readable");
    states.Access(11,1,a,8);require(!states.Lookup(11,1)->known,"access masked pending split");
    states.Transition(11,1,a,0,8,BarrierPhase::End,true);
    require(states.Lookup(11,1)->known,"matching split did not complete");
    states.Transition(11,2,b,0,8,BarrierPhase::End,true);
    require(!states.Lookup(11,2)->known,"orphan split end trusted");
    states.Transition(11,1,a,8,1024,BarrierPhase::Begin,true);
    states.Transition(11,1,a,0,1024,BarrierPhase::End,true);
    require(!states.Lookup(11,1)->known,"mismatched split end trusted");
    states.Transition(11,1,a,0,8,BarrierPhase::Full,false);
    require(!states.Lookup(11,1)->known,"partial subresource transition trusted for full buffer");
    states.Access(11,1,a,1024);require(states.Lookup(11,1)->known,"copy access not recorded");
    states.Invalidate(11);require(!states.Lookup(11,1)->known,"alias/unknown barrier retained known state");
    std::weak_ptr<int> lifetime=a;a.reset();require(!lifetime.expired(),"resource identity lost lifetime pin");
    states.Reset(12);require(lifetime.expired(),"recording reset leaked lifetime pin");
    for(int i=1;i<=5;++i)states.Access(12,i,std::make_shared<int>(i),8);
    require(!states.Lookup(12,1),"bounded eviction retained stale identity");
    require(states.Lookup(12,5)->known,"new resource missing after bounded eviction");
    // Scope/thread A retained a TLS value for resource1. Another scope/thread
    // filled the bounded cache and then encountered an unclassified barrier.
    states.Invalidate(12);
    require(!states.Seed(12,1,{8,true,false},true).known,"eviction plus invalidation revived stale TLS state");
    require(states.Seed(12,99,{8,true,false},false).known,"uncovered resource policy changed");
    states.Access(0,99,b,8);require(!states.Lookup(0,99),"zero recording accepted");
    std::puts("PASS alternating flags, recording scope, stale events, split/alias invalidation, bounded lifetime");
}
