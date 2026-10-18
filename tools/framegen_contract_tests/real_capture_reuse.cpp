#include "Framegen/RealCaptureReuse.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
struct Receipt {
    bool paired=true;
    uint64_t pairedEye=0,currentPairSerial=42,gameWorldEpoch=1,moduleGeneration=2,lifecycleGeneration=3,
        createOrdinal=4,historyEpoch=5,resourceEpoch=6,policyRevision=7,count=3,current=99,index=1;
};
void Check(bool ok,const char* why){if(!ok){std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);}}
int main(){
    Receipt a,b;b.index=2;
    Check(cvr::framegen::SamePairedRealSource(a,b),"second synthetic phase must reuse the same real source identity");
    b.index=3;Check(cvr::framegen::SamePairedRealSource(a,b),"third phase shares real identity");
    for(auto field:{&Receipt::pairedEye,&Receipt::currentPairSerial,&Receipt::gameWorldEpoch,&Receipt::moduleGeneration,
        &Receipt::lifecycleGeneration,&Receipt::createOrdinal,&Receipt::historyEpoch,&Receipt::resourceEpoch,
        &Receipt::policyRevision,&Receipt::count,&Receipt::current}){
        b=a;++(b.*field);Check(!cvr::framegen::SamePairedRealSource(a,b),"changed eye/source/epoch cannot reuse real pixels");
    }
    b=a;b.paired=false;Check(!cvr::framegen::SamePairedRealSource(a,b),"AER is not covered by paired pixel proof");
    cvr::framegen::RealCaptureLifetime life;
    Check(!life.CanShare(),"unsubmitted snapshot rejected");life.submitted=true;
    Check(!life.CanConsume(true,true),"completed fence without recording retirement can replay");
    life.retired=true;Check(life.CanConsume(true,false),"retired completed snapshot crosses queues safely");
    Check(life.CanConsume(false,true),"same queue preserves prior submitted producer order");
    Check(!life.CanConsume(false,false),"unfinished producer on another queue cannot be consumed");
    life.invalid=true;Check(!life.CanConsume(true,true),"Unknown/replayed snapshot remains rejected");
    std::puts("PASS: same real across phases; all identity boundaries rejected");
}
