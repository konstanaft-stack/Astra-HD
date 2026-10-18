#include "Camera/ImagePoseLedger.hpp"
#include <array>
#include <iostream>
#include <new>
#include <span>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
using cvr::camera::ImagePoseLedger;
static unsigned checks;
static void Check(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
// RED fallback invokes the old live-list Submit exactly as the old hook did.
// It captures only list addresses; it supplies no replacement metadata logic.
struct LegacySubmission {std::vector<uintptr_t> lists;explicit operator bool()const{return true;}};
template<class Ledger>static auto Prepare(Ledger& ledger,uintptr_t queue,std::span<const uintptr_t> lists){
    if constexpr(requires{ledger.Prepare(queue,lists);})return ledger.Prepare(queue,lists);
    else return LegacySubmission{{lists.begin(),lists.end()}};
}
template<class Ledger,class Batch>static void Commit(Ledger& ledger,uintptr_t queue,Batch&& batch,uint64_t stamp){
    if constexpr(requires{ledger.Commit(queue,std::move(batch),stamp);})ledger.Commit(queue,std::move(batch),stamp);
    else for(const auto list:batch.lists)ledger.Submit(queue,list,stamp);
}
template<class Ledger>static auto PrepareOne(Ledger& ledger,uintptr_t queue,uintptr_t list){
    const std::array<uintptr_t,1> lists{list};return Prepare(ledger,queue,std::span<const uintptr_t>(lists));
}
template<class Ledger,class Pose>static void Publish(Ledger& ledger,uintptr_t list,uintptr_t resource,const Pose& pose,bool stable=false){
    ledger.Record(list,resource,pose,stable);auto batch=PrepareOne(ledger,1,list);Commit(ledger,1,std::move(batch),10);
}
static void ResetAfterActualExecute(){
    ImagePoseLedger<int> ledger;ledger.Record(10,100,11,true);
    auto batch=PrepareOne(ledger,1,10);
    // Execute of recording A has returned. D3D12 permits Reset of this command
    // list with another available allocator before CPU post-commit runs.
    std::thread reset([&]{ledger.Reset(10);ledger.Record(10,100,22,true);});reset.join();
    Commit(ledger,1,std::move(batch),20);
    Check(ledger.Read(100).pose==11,"post-Execute Reset replaced submitted A metadata with new recording B");
    auto newer=PrepareOne(ledger,1,10);Commit(ledger,1,std::move(newer),30);
    Check(ledger.Read(100).pose==22&&ledger.Read(100).stampMs==30,"new recording cannot publish on its own later submission");
}
static void LegitimateReplay(){
    ImagePoseLedger<int> ledger;ledger.Record(10,100,11,true);
    auto first=PrepareOne(ledger,1,10);Commit(ledger,1,std::move(first),20);const auto a=ledger.Read(100);
    auto replay=PrepareOne(ledger,1,10);Commit(ledger,1,std::move(replay),30);const auto b=ledger.Read(100);
    Check(a.pose==11&&b.pose==11&&b.generation>a.generation&&b.stampMs==30,"legitimate closed-list replay lost its recorded pose");
    Commit(ledger,1,std::move(replay),40);
    Check(ledger.Read(100).generation==b.generation,"same prepared submission committed twice");
    Check(!std::is_copy_constructible_v<decltype(replay)>,"prepared submission is copyable");
}
static void OrderedMultipleLists(){
    ImagePoseLedger<int> ledger;ledger.Record(10,100,11,false);ledger.Record(10,101,12,false);
    ledger.Record(20,100,21,true);const std::array<uintptr_t,2> lists{10,20};
    auto batch=Prepare(ledger,1,std::span<const uintptr_t>(lists));ledger.Reset(20);ledger.Record(20,100,99,true);
    Commit(ledger,1,std::move(batch),50);
    Check(ledger.Read(100).pose==21&&ledger.Read(101).pose==12,"batch submission order or captured writes changed");
    Check(ledger.LatestStable().pose==21,"stable image came from a different recording");
}
static void ResourceLifetime(){
    ImagePoseLedger<int> ledger;ledger.Record(10,100,11,true);auto old=PrepareOne(ledger,1,10);
    ledger.ForgetResource(100);Publish(ledger,20,100,99,true);const auto fresh=ledger.Read(100);
    Commit(ledger,1,std::move(old),30);
    Check(ledger.Read(100).pose==99&&ledger.Read(100).generation==fresh.generation,"old resource address lifetime overwrote or revoked new object");
    ledger.ForgetResource(100);Check(!ledger.Read(100)&&!ledger.LatestStable(),"resource release retained image metadata");
}
static void ListLifetime(){
    ImagePoseLedger<int> ledger;Publish(ledger,20,100,5,true);
    ledger.Record(10,100,11,true);auto old=PrepareOne(ledger,1,10);ledger.ForgetList(10);
    ledger.Reset(10);ledger.Record(10,100,22,true);Commit(ledger,1,std::move(old),30);
    Check(!ledger.Read(100)&&!ledger.LatestStable(),"destroyed/reused list published new metadata or retained stale image label");
    auto fresh=PrepareOne(ledger,1,10);Commit(ledger,1,std::move(fresh),40);
    Check(ledger.Read(100).pose==22,"new list lifetime rejected");
}
struct CopyFailure {
    int pose{};inline static bool fail{};
    CopyFailure()=default;explicit CopyFailure(int p):pose(p){}
    CopyFailure(const CopyFailure& other):pose(other.pose){if(fail)throw std::bad_alloc();}
    CopyFailure& operator=(const CopyFailure& other){if(fail)throw std::bad_alloc();pose=other.pose;return *this;}
};
static void SnapshotFailure(){
    ImagePoseLedger<CopyFailure> ledger;Publish(ledger,20,100,CopyFailure{5},true);
    ledger.Record(10,100,CopyFailure{11},true);auto outstanding=PrepareOne(ledger,1,10);
    CopyFailure::fail=true;auto failed=PrepareOne(ledger,1,10);CopyFailure::fail=false;
    Check(!failed,"failed snapshot became an eligible submission");
    Check(!ledger.Read(100)&&!ledger.LatestStable(),"failed snapshot retained pre-existing source labels for changed pixels");
    Commit(ledger,1,std::move(outstanding),20);Check(!ledger.Read(100),"batch from before failure restored revoked metadata");
    auto recovery=PrepareOne(ledger,1,10);Commit(ledger,1,std::move(recovery),30);
    Check(ledger.Read(100).pose.pose==11,"new explicit snapshot after metadata failure cannot recover");
}
static void QueueAndOwnerScope(){
    ImagePoseLedger<int> ledger,other;ledger.Record(10,100,11,true);auto wrongQueue=PrepareOne(ledger,1,10);
    Commit(ledger,2,std::move(wrongQueue),20);Check(!ledger.Read(100),"prepared data accepted for another queue");
    auto wrongOwner=PrepareOne(ledger,1,10);Commit(other,1,std::move(wrongOwner),30);
    Check(!other.Read(100),"another ledger accepted a foreign prepared batch");
}
static void CommitFailureAndAbandonment(){
    ImagePoseLedger<CopyFailure> ledger;Publish(ledger,20,100,CopyFailure{5},true);
    ledger.Record(10,100,CopyFailure{11},true);
    {auto abandoned=PrepareOne(ledger,1,10);(void)abandoned;}
    Check(!ledger.Read(100),"lost post-Execute commit left a previous image label");
    auto batch=PrepareOne(ledger,1,10);CopyFailure::fail=true;Commit(ledger,1,std::move(batch),20);CopyFailure::fail=false;
    Check(!ledger.Read(100)&&!ledger.LatestStable(),"failed commit exposed a partial or old image publication");
    Commit(ledger,1,std::move(batch),30);Check(!ledger.Read(100),"failed batch was reused after recovery");
    auto recovery=PrepareOne(ledger,1,10);Commit(ledger,1,std::move(recovery),40);
    Check(ledger.Read(100).pose.pose==11,"fresh replay cannot recover after commit failure");
}
static void UnknownOverwriteAndEmptyReset(){
    ImagePoseLedger<int> ledger;Publish(ledger,10,100,11,true);ledger.Reset(10);ledger.Record(10,100,0,false);
    auto unknown=PrepareOne(ledger,1,10);Commit(ledger,1,std::move(unknown),20);
    Check(ledger.Read(100)&&ledger.Read(100).pose==0,"unknown write retained prior source pose");
    const auto version=ledger.Read(100).generation;ledger.Reset(10);auto empty=PrepareOne(ledger,1,10);Commit(ledger,1,std::move(empty),30);
    Check(ledger.Read(100).generation==version,"empty reset recording manufactured an image publication");
}
int main(){
    unsigned pass=0,fail=0;const std::pair<const char*,void(*)()> cases[]={
        {"reset_after_execute",ResetAfterActualExecute},{"closed_list_replay",LegitimateReplay},
        {"multi_list_order",OrderedMultipleLists},{"resource_lifetime",ResourceLifetime},{"list_lifetime",ListLifetime},
        {"snapshot_failure",SnapshotFailure},{"queue_owner_scope",QueueAndOwnerScope},
        {"commit_failure_abandonment",CommitFailureAndAbandonment},{"unknown_write",UnknownOverwriteAndEmptyReset}};
    for(const auto&[name,run]:cases)try{run();++pass;std::cout<<"PASS "<<name<<'\n';}catch(const std::exception&e){CopyFailure::fail=false;++fail;std::cerr<<"FAIL "<<name<<": "<<e.what()<<'\n';}
    std::cout<<"RESULT "<<pass<<" passed, "<<fail<<" failed; "<<checks<<" checks\n";return fail?1:0;
}
