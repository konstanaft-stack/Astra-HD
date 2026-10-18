#include "MenuPreview/EditorFreshnessProbe.hpp"
#include <cstdio>
#include <cstdlib>
#include <thread>
using namespace cvr::menu_preview;int checks=0;void Check(bool p,const char* s){++checks;if(!p){printf("FAIL %s\n",s);exit(1);}}
int main(){EditorFreshnessRecorder r;FreshnessSample s;s.epoch=1;s.source=123;s.otherSource=246;s.nativeFrame=7;s.revision=9;FreshnessSnapshot out;
r.Scope(false,1,0);r.Note(false,FreshnessEvent::Duplicate,FreshnessStage::Publication,s);Check(!r.Armed()&&!r.Take(false,0,out),"disabled gate doesnotarm/count/emit");
for(int i=0;i<100;++i){r.Scope(true,0,i);Check(!r.Take(true,i,out),"startup/world doesnotspendlogbudget");}Check(!r.Armed(),"nevereditor remainsNOT_ARMED");
r.Scope(true,1,100);for(int i=0;i<4;++i){r.Note(true,FreshnessEvent::Duplicate,FreshnessStage::Publication,s);Check(r.Take(true,100+i,out)&&out.row==unsigned(i+1),"first4 boundedrows");Check(out.counts[size_t(FreshnessEvent::Duplicate)]==1&&out.last[0].source==123&&out.last[0].otherSource==246,"coherenttuple+intervalcounts");}
r.Note(true,FreshnessEvent::SameSourceReplace,FreshnessStage::Publication,s);Check(!r.Take(true,999,out),"ratecap before1sec");Check(r.Take(true,1103,out)&&out.row==5&&out.counts[size_t(FreshnessEvent::SameSourceReplace)]==1,"ratecap at1sec");
r.Scope(true,0,1104);FreshnessSample closed;closed.displayTime=77;r.Note(true,FreshnessEvent::Empty,FreshnessStage::Terminal,closed);Check(r.Take(true,2103,out)&&out.epoch==0&&out.armedEpoch==1&&out.counts[size_t(FreshnessEvent::ScopeAbsent)]==1&&out.counts[size_t(FreshnessEvent::Empty)]==1,"armedclosed scope explicitlylogged notUNKNOWN");
for(unsigned row=7;row<=32;++row)Check(r.Take(true,2103+(row-6)*1000,out)&&out.row==row,"processmax32");Check(!r.Take(true,999999,out),"no33rdrow");r.Scope(true,2,999999);Check(!r.Take(true,1000999,out),"newscope cannotresetprocessrowbudget");
EditorFreshnessRecorder early;FreshnessSample earlySample;earlySample.epoch=8;early.Note(true,FreshnessEvent::Duplicate,FreshnessStage::Publication,earlySample);early.Scope(true,8,99999999);Check(early.Take(true,99999999,out)&&out.intervalMs==0&&!out.intervalKnown,"publication beforeScope firstintervalunknown0 notuptime");Check(early.Take(true,100000000,out)&&out.intervalKnown&&out.intervalMs==1,"nextintervalclockknown");
EditorFreshnessRecorder concurrent;concurrent.Scope(true,3,1);auto writer=[&](uint64_t offset){for(uint64_t i=1;i<=100;++i){FreshnessSample p;p.epoch=3;p.source=offset+i;p.otherSource=p.source*2;p.control=p.source+5;concurrent.Note(true,FreshnessEvent::WriterMismatch,FreshnessStage::Writer,p);}};std::thread a(writer,0),b(writer,1000);a.join();b.join();Check(concurrent.Take(true,1000,out)&&out.counts[size_t(FreshnessEvent::WriterMismatch)]==200,"concurrentexacteventcount");Check(out.last[1].otherSource==out.last[1].source*2&&out.last[1].control==out.last[1].source+5,"scalarIDtuplecoherent");
// Take returned with its lock released: recorder operations may immediately run again.
concurrent.Note(true,FreshnessEvent::Fresh,FreshnessStage::Terminal,s);Check(concurrent.Take(true,1001,out),"emitcallbackcanrecord noheldlock");printf("PASS %d pure recorder checks; no GPU/process probing\n",checks);}
