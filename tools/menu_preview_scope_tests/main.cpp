#include "MenuPreview/Scope.hpp"
#include <iostream>
#include <stdexcept>

using namespace cvr::menu_preview;
struct FakeScope { Metadata metadata; std::weak_ptr<int> life; };
static void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(){
    ScopeStore<FakeScope> store;
    auto life=std::make_shared<int>(7);
    auto make=[&](uint32_t epoch,uint32_t setup){auto s=std::make_shared<FakeScope>();s->metadata.screenEpoch=epoch;s->metadata.setupID=setup;s->metadata.screen=1;s->metadata.controllerIdentity=100;s->metadata.componentIdentity=200;s->metadata.entityIdentity=300;s->metadata.publishedAtMs=1000;s->life=life;return s;};
    Check(!store.Read(),"starts inactive");
    Check(!store.IsEpochOpen(0)&&!store.IsEpochOpen(1),"no epoch is open before Begin");
    Check(store.ObservationEpoch(1000)==0,"world does not request preview descriptor work");
    const auto a=store.Begin();Check(a!=0,"allocated epoch");
    Check(store.IsEpochOpen(a)&&!store.IsEpochOpen(0)&&!store.IsEpochOpen(a+1),
        "only the exact allocated epoch is open even before publication");
    Check(store.Publish(make(a,0)),"publish inventory");
    Check(store.ObservationEpoch(1500)==a&&store.ObservationEpoch(1501)==0,
        "observation is bounded by the same publication freshness as pixel acquisition");
    Check(store.IsEpochOpen(a),"publication freshness does not close an editor lifetime");
    auto old=store.Read();Check(old&&old->metadata.setupID==0,"immutable read");
    Check(store.Publish(make(a,23)),"setup change under same lifetime");
    Check(old->metadata.setupID==0&&store.Read()->metadata.setupID==23,"setup snapshot cannot mutate readers");
    const auto b=store.Begin();Check(b>a&&!store.Read(),"new lifetime invalidates old before readiness");
    Check(!store.IsEpochOpen(a)&&store.IsEpochOpen(b),"Begin closes old lifetime before new publication");
    Check(store.Publish(make(b,1)),"publish new lifetime");
    Check(!store.Clear(a)&&store.Read()->metadata.screenEpoch==b,"stale close preserves newer scope");
    Check(!store.Invalidate(a)&&store.IsEpochOpen(b),"stale invalidation preserves newer open lifetime");
    Check(!store.Publish(make(a,2)),"stale publisher rejected");
    Check(store.Invalidate(b)&&!store.Read(),"not-ready transition removes acquisition");
    Check(store.IsEpochOpen(b),"category gap preserves the exact editor epoch without a current scope");
    Check(store.ObservationEpoch(1000)==0,"not-ready transition stops descriptor observation");
    Check(store.Publish(make(b,3)),"same lifetime can become ready again");
    auto s=store.Read();auto acquired=s->life.lock();life.reset();
    Check(acquired&&s->life.lock(),"acquisition retains lifetime");acquired.reset();Check(!s->life.lock(),"expired weak identity cannot be acquired");
    Check(store.Clear(b)&&!store.Read(),"close removes scope");
    Check(!store.IsEpochOpen(b)&&!store.Invalidate(b),"Clear closes epoch through later not-ready updates");
    Check(store.ObservationEpoch(1000)==0,"return to world stops descriptor observation");
    Check(!store.Publish(make(b,4)),"closed lifetime cannot resurrect on late update");
    Check(ValidPublication(1,1,true,24,23,1),"dynamic setup bounds allow modded slot 23");
    Check(!ValidPublication(1,1,true,24,24,1),"out of range actual setup rejected");
    Check(!ValidPublication(1,1,true,24,0,2),"ambiguous enabled cameras rejected");
    Check(ValidPublication(2,1,true,24,0,1),"supported creation branch can publish a preview");
    Check(!ValidPublication(0,1,true,24,0,1)&&!ValidPublication(1,0,true,24,0,1),"none and zero epoch rejected");
    Check(!ValidPublication(1,1,false,24,0,1),"not-ready is not publishable");
    Check(FreshPublication(1000,1500)&&!FreshPublication(1000,1501)&&!FreshPublication(0,1)&&!FreshPublication(1000,999),"scope timeout and backwards clock fail closed");
    std::cout<<"PASS: preview lifecycle/setup/stale close/weak acquisition/invalidation/ambiguous camera/epoch-open policy\n";
}
