#include "MenuPreview/PanelDrawWitnessPolicy.hpp"
#include <cstdio>
#include <cstdlib>
using namespace cvr::menu_preview;
struct Draw {uint64_t ordinal{};uint32_t bindingIndex{},marker{};};
struct Binding {uint64_t cpu{},version{},resource{},generation{};bool operator==(const Binding&)const=default;};
using Pool=PanelDrawWitnessPool<Draw,Binding>;
static void require(bool ok,const char* message){if(!ok){std::fprintf(stderr,"FAIL %s\n",message);std::exit(1);}}
int main(){
 const PanelWitnessScope scope{3,6,100,200},next{4,6,100,200};
 const PanelWitnessListKey a{0x1000,10,1},b{0x2000,20,2},c{0x3000,30,3};
 Binding descriptor{0x900,7,0x7000,42};Pool p;Pool::Batch batch;
 require(p.Observe(scope,a,{0,0,11},descriptor),"record pre-portrait native UI draw");
 require(!p.TakeReady(scope,a,batch),"unqualified prefix must not be called panel evidence");
 descriptor.version=8;descriptor.resource=0x8000;
 require(p.Observe(scope,a,{0,0,22},descriptor),"record later native binding");
 p.Qualify(scope,a,0x4000,8);
 require(p.Observe(scope,a,{0,0,33},descriptor),"retain post-bridge draw too");
 require(p.TakeReady(scope,a,batch)&&batch.count==3,"qualification must preserve earlier prefix");
 require(batch.records[0].marker==11&&batch.records[0].ordinal==1&&batch.records[2].ordinal==3,"keep chronological draw order");
 descriptor.version=999;
 require(batch.bindings[batch.records[0].bindingIndex].version==7&&batch.bindings[batch.records[0].bindingIndex].resource==0x7000,"deferred submission must not resolve overwritten descriptor");
 require(p.Commit(scope,a)&&!p.Commit(scope,a),"one submitted emission per prepared list");
 p.Observe(scope,b,{0,0,44},descriptor);p.Qualify(scope,b,0x4000,8);
 require(p.TakeReady(scope,b,batch)&&p.Commit(scope,b),"second qualified submitted list");
 require(!p.Observe(scope,c,{0,0,55},descriptor),"no third list after output quota");
 require(p.Observe(next,c,{0,0,66},descriptor),"new epoch must restart bounded observation");
 require(!p.Observe(scope,a,{0,0,99},descriptor),"late observation must not revive older epoch");
 require(!p.TakeReady(scope,a,batch)&&!p.TakeReady(next,c,batch),"old epoch and new unqualified list must not inherit proof");
 p.Qualify(next,c,0x4000,8);require(p.TakeReady(next,c,batch)&&batch.count==1&&batch.records[0].marker==66,"epoch reset removes old records");

 Pool full;
 for(unsigned n=0;n<70;++n)full.Observe(scope,a,{0,0,n},descriptor);
 full.Qualify(scope,a,9,10);
 require(full.TakeReady(scope,a,batch)&&batch.count==64&&batch.recordDrops==6&&batch.prefixIncomplete,"bounded overflow stays explicit, retains first prefix");
 require(batch.records[0].marker==0&&batch.records[63].marker==63,"overflow must not overwrite prefix with newest draws");
 Pool keys;
 for(unsigned n=0;n<17;++n){auto d=descriptor;d.version=n;keys.Observe(scope,a,{0,0,n},d);}
 keys.Qualify(scope,a,9,10);
 require(keys.TakeReady(scope,a,batch)&&batch.bindingCount==16&&batch.bindingDrops==1&&batch.count==16&&batch.prefixIncomplete,"sixteenth binding boundary and explicit rejected seventeenth");
 Pool evict;
 evict.Observe(scope,a,{0,0,1},descriptor);evict.Observe(scope,b,{0,0,2},descriptor);evict.Qualify(scope,b,9,10);
 evict.Observe(scope,c,{0,0,3},descriptor); // Only unqualified A may be evicted.
 require(evict.TakeReady(scope,b,batch)&&batch.records[0].marker==2,"qualified list cannot be evicted for an unrelated prefix");
 evict.Observe(scope,a,{0,0,4},descriptor);evict.Qualify(scope,a,9,10);
 require(evict.TakeReady(scope,a,batch)&&batch.prefixIncomplete&&batch.evictedRecords>=1,"lost prequalification prefix must be disclosed");
 Pool occupied;
 occupied.Observe(scope,a,{0,0,1},descriptor);occupied.Qualify(scope,a,9,10);occupied.Observe(scope,b,{0,0,2},descriptor);occupied.Qualify(scope,b,9,10);
 require(!occupied.Observe(scope,c,{0,0,3},descriptor),"two qualified lists cannot be displaced");
 require(occupied.TakeReady(scope,a,batch)&&batch.poolDrops==1,"pool exhaustion must be explicit in submitted evidence");
 Pool reset;
 reset.Observe(scope,a,{0,0,1},descriptor);reset.Qualify(scope,a,9,10);reset.TakeReady(scope,a,batch);reset.Retire(a);
 require(!reset.Commit(scope,a),"list reset cancels old prepared evidence");
 const PanelWitnessListKey reused{a.list,a.life,100};reset.Observe(scope,reused,{0,0,2},descriptor);
 require(!reset.TakeReady(scope,reused,batch),"reused recording cannot inherit qualified target");
 std::puts("PASS bounded native UI prefix, immutable descriptors, epoch/list reset, quota, eviction and overflow");
}
