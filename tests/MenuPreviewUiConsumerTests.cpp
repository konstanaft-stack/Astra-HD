#include "MenuPreview/UiConsumerPolicy.hpp"
#include <cassert>
#include <array>
#include <barrier>
#include <thread>
using namespace cvr::menu_preview;
struct TestMutation:UiOwnedMutationStorage<uintptr_t>{using UiOwnedMutationStorage<uintptr_t>::UiOwnedMutationStorage;};
int main(){
 // Production per-call payload factory: twelve independent original calls are simultaneously in flight.
 struct Entry {uint64_t version{};uint32_t active{};bool overlap{};};
 std::array<Entry,12> entries{};std::array<std::shared_ptr<TestMutation>,12> payloads{};
 std::barrier rendezvous(12,[&]()noexcept{for(size_t i=0;i<12;++i){assert(payloads[i]&&payloads[i]->token==i+1);assert(entries[i].active==1&&!entries[i].overlap);}});
 std::array<std::thread,12> workers;
 for(size_t i=0;i<12;++i)workers[i]=std::thread([&,i]{payloads[i]=OwnUiMutation<TestMutation>(i+1,1);UiBeginDescriptorMutation(entries[i].version,entries[i].active,entries[i].overlap,payloads[i]->token);rendezvous.arrive_and_wait();assert(UiFinishDescriptorMutation(entries[i].version,entries[i].active,entries[i].overlap,payloads[i]->token));payloads[i].reset();});
 for(auto& worker:workers)worker.join();for(const auto& entry:entries)assert(entry.active==0);
 assert(OwnUiMutation<TestMutation>(100,128)->destinations.size()==128);
 assert(!OwnUiMutation<TestMutation>(100,129));assert(!OwnUiMutation<TestMutation>(100,0));
 Entry overlap;UiBeginDescriptorMutation(overlap.version,overlap.active,overlap.overlap,1);UiBeginDescriptorMutation(overlap.version,overlap.active,overlap.overlap,2);
 assert(!UiFinishDescriptorMutation(overlap.version,overlap.active,overlap.overlap,1)&&overlap.active==1);
 assert(!UiFinishDescriptorMutation(overlap.version,overlap.active,overlap.overlap,2)&&overlap.active==0);
 UiBeginDescriptorMutation(overlap.version,overlap.active,overlap.overlap,3);assert(UiFinishDescriptorMutation(overlap.version,overlap.active,overlap.overlap,3));

 // An unknown copy destination was never enlisted. Its delayed Finish cannot consume a later owner's count.
 auto untracked=OwnUiMutation<TestMutation>(200,1);auto tracked=OwnUiMutation<TestMutation>(201,1);Entry later;
 UiBeginDescriptorMutation(later.version,later.active,later.overlap,tracked->token);tracked->enlisted[0]=1;
 assert(!UiFinishEnlistedMutation(untracked->enlisted[0]!=0,later.version,later.active,later.overlap,untracked->token));
 assert(later.active==1&&later.version==201&&!later.overlap);
 assert(UiFinishEnlistedMutation(tracked->enlisted[0]!=0,later.version,later.active,later.overlap,tracked->token)&&later.active==0);
 // Lifetime-owned registry has no fixed 64-list history limit, and Find never inserts.
 struct Row {uintptr_t identity{};uint64_t objectGeneration{},generation{};bool closed{},overflow{};};
 UiLiveListStore<Row> live;
 for(uint64_t i=1;i<=96;++i){auto* r=live.Ensure(0x1000+i,i);r->closed=true;}
 assert(live.Size()==96);assert(!live.Find(0xFFFF)&&live.Size()==96);
 const auto saved=*live.Find(0x1001);live.Reset(0x1001,1);
 assert(saved.closed&&saved.generation!=live.Find(0x1001)->generation);
 assert(!UiRecordingValid(live.Find(0x1001)->generation,saved.generation,true,false));
 live.Retire(0x1001,999);assert(live.Find(0x1001));
 live.Retire(0x1001,1);assert(!live.Find(0x1001));
 auto* reused=live.Ensure(0x1001,1001);assert(reused->objectGeneration==1001&&reused->generation!=saved.generation);
 live.Retire(0x1001,1);assert(live.Find(0x1001));
 for(uint64_t i=2;i<=96;++i)live.Retire(0x1000+i,i);
 live.Retire(0x1001,1001);assert(live.Size()==0);live.Ensure(0x2000,2000);live.Clear();assert(live.Size()==0);
 UiOrderedWriter none;assert(!UiCanConsume(none,0x100,0x200,3,0));
 UiOrderedWriter left{0x100,0x200,71,10,3,0,0,600};
 assert(UiCanConsume(left,0x100,0x200,3,0));
 assert(!UiCanConsume(left,0x101,0x200,3,0));
 assert(!UiCanConsume(left,0x100,0x201,3,0));
 assert(!UiCanConsume(left,0x100,0x200,4,0));
 assert(!UiCanConsume(left,0x100,0x200,3,1));
 const auto consumedLeft=left;
 UiOrderedWriter right{0x100,0x200,72,11,3,0,1,601};left=right;
 assert(consumedLeft.eye==0&&consumedLeft.source==71&&consumedLeft.revision==10);
 assert(UiCanConsume(right,0x100,0x200,3,0)&&right.eye==1);
 right.eye=2;assert(!UiCanConsume(right,0x100,0x200,3,0));
 right.eye=1;right.revision=0;assert(!UiCanConsume(right,0x100,0x200,3,0));
 right.revision=11;right.source=0;assert(!UiCanConsume(right,0x100,0x200,3,0));
 assert(UiRecordingValid(10,10,true,false));
 assert(!UiRecordingValid(11,10,true,false));
 assert(!UiRecordingValid(10,10,false,false));
 assert(!UiRecordingValid(10,10,true,true));
 assert(UiSelectedBinding(0x100,3,5,2,0x100,3,5,4));
 assert(!UiSelectedBinding(0x100,3,5,2,0x100,4,5,4));
 assert(!UiSelectedBinding(0x100,3,5,2,0x101,3,5,4));
 assert(!UiSelectedBinding(0x100,3,5,4,0x100,3,5,4));
 // Ordered unknown write is a revocation; only a later exact writer restores it.
 UiOrderedWriter stream;stream={};stream=consumedLeft;assert(UiCanConsume(stream,0x100,0x200,3,0));
 stream={};assert(!UiCanConsume(stream,0x100,0x200,3,0));
 uint32_t index;assert(UiTableContains(0x1000,4,32,0x1060,index)&&index==3);
 assert(!UiTableContains(0x1000,4,32,0x1080,index));
 assert(!UiTableContains(0x1000,4,32,0x1021,index));
 assert(!UiTableContains(0x1000,129,32,0x1000,index));
 assert(!UiTableContains(0x1000,4,0,0x1000,index));
 assert(!UiTableContains(0x1000,4,32,0xFFF,index));
}
