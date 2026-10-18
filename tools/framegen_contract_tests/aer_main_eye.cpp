#include <Windows.h>
#include "Framegen/MainEyeSourceLatch.hpp"
#include "Camera/PoseAddressLedger.hpp"
#include <iostream>
#include <stdexcept>
using namespace cvr::framegen;
static void Check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
static MainEyeSceneFrame Frame(uint64_t ordinal=1){return {10,ordinal,20,30,true};}
static MainEyeClockSnapshot Clock(){return {{20,10,30},40,{},5'000'000'000};}
static VrcamOffProof Off(uint64_t request){return {request,10,true};}
static void DormantAndDeferred(){
    MainEyeSourceLatch p;auto r=p.RequestRevision();
    p.OnSceneFrame(r,Frame(),0,AerV2Eye::Left,Off(r),Clock());
    Check(!p.ForMainWrite(30),"default OFF emitted a source receipt");
    r=p.Request(true);
    Check(!p.OnSceneFrame(r,Frame(),0,AerV2Eye::Left,Off(r),Clock()),"same scene frame activated after request");
    Check(!p.ForMainWrite(30),"same-frame enable yielded MAIN eye");
    Check(p.OnSceneFrame(r,Frame(2),0,AerV2Eye::Left,Off(r),Clock()),"fresh valid scene frame did not activate");
    const auto value=p.ForMainWrite(30);
    Check(value && value->sourceId==0 && value->physicalEye==AerV2Eye::Left && value->sceneOrdinal==2,"root identity was remapped");
}
static void OffProofIsScoped(){
    MainEyeSourceLatch p;const auto r=p.Request(true);auto off=Off(r);off.confirmed=false;
    Check(!p.OnSceneFrame(r,Frame(),0,AerV2Eye::Left,off,Clock()),"unconfirmed RTT disable activated");
    Check(!p.OnSceneFrame(r,Frame(),0,AerV2Eye::Left,Off(r),Clock()),"late same-frame confirmation activated");
    off=Off(r);off.gameWorldEpoch=9;
    Check(!p.OnSceneFrame(r,Frame(2),1,AerV2Eye::Right,off,Clock()),"old scene's VRCAM proof activated");
    off=Off(r);off.requestRevision=r-1;
    Check(!p.OnSceneFrame(r,Frame(3),2,AerV2Eye::Left,off,Clock()),"old request's VRCAM proof activated");
    Check(p.OnSceneFrame(r,Frame(4),3,AerV2Eye::Right,Off(r),Clock()),"current scoped proof was rejected");
}
static void DuplicateDoesNotFlip(){
    MainEyeSourceLatch p;const auto r=p.Request(true);
    Check(p.OnSceneFrame(r,Frame(),100,AerV2Eye::Left,Off(r),Clock()),"initial frame rejected");
    const auto before=p.ForMainWrite(30);auto clock=Clock();clock.runtimePredictedTimeNs+=1'000'000;
    Check(!p.OnSceneFrame(r,Frame(),101,AerV2Eye::Right,Off(r),clock),"duplicate scene callback allocated another eye");
    Check(p.ForMainWrite(30)==before,"late clock/eye changes mutated an existing receipt");
    Check(p.OnSceneFrame(r,Frame(2),101,AerV2Eye::Right,Off(r),clock),"new frame did not retain root's new identity");
    Check(p.Usable(*before),"in-flight old receipt was lost within the same epochs");
}
static void DisableRevokes(){
    MainEyeSourceLatch p;auto r=p.Request(true);p.OnSceneFrame(r,Frame(),0,AerV2Eye::Left,Off(r),Clock());
    auto before=p.ForMainWrite(30);Check(before.has_value(),"no source before disable");
    p.Request(false);Check(!p.ForMainWrite(30) && !p.Usable(*before),"disabled owner retained an active receipt");
    auto next=p.Request(true);Check(next!=r,"new request reused old activation proof");
    Check(!p.OnSceneFrame(r,Frame(2),1,AerV2Eye::Right,Off(r),Clock()),"stale publisher revived old request");
}
static void EpochsAreIndependent(){
    for(int changed=0;changed<3;++changed){
        MainEyeSourceLatch p;auto r=p.Request(true);p.OnSceneFrame(r,Frame(),0,AerV2Eye::Left,Off(r),Clock());
        auto old=p.ForMainWrite(30);Check(old.has_value(),"no initial receipt");auto f=Frame(2);auto c=Clock();auto off=Off(r);
        if(changed==0){++f.rendererEpoch;++c.epochs.renderer;}
        if(changed==1){++f.gameWorldEpoch;++c.epochs.gameWorld;++off.gameWorldEpoch;}
        if(changed==2){++f.hmdWorldEpoch;++c.epochs.hmdWorld;}
        Check(p.OnSceneFrame(r,f,1,AerV2Eye::Right,off,c),"new coherent epoch rejected");
        Check(!p.Usable(*old),"retired renderer/game/HMD epoch remained usable");
        Check(p.ForMainWrite(f.hmdWorldEpoch).has_value(),"matching HMD epoch rejected");
        Check(!p.ForMainWrite(f.hmdWorldEpoch+1),"foreign HMD epoch relabelled camera");
    }
}
static void InvalidateWaitsNewFrame(){
    MainEyeSourceLatch p;auto r=p.Request(true);p.OnSceneFrame(r,Frame(),0,AerV2Eye::Left,Off(r),Clock());
    const auto old=p.ForMainWrite(30);Check(old.has_value(),"no receipt before invalidation");p.Invalidate();
    Check(!p.ForMainWrite(30),"invalidation left active camera source");
    Check(!p.OnSceneFrame(r,Frame(),1,AerV2Eye::Right,Off(r),Clock()),"invalidation restarted in the old frame");
    Check(p.OnSceneFrame(r,Frame(2),1,AerV2Eye::Right,Off(r),Clock()),"new frame failed after invalidation");
    Check(!p.Usable(*old),"new frame revived an explicitly invalidated receipt");
}
static void StaleFrameCannotRevokeCurrent(){
    MainEyeSourceLatch p;const auto r=p.Request(true);p.OnSceneFrame(r,Frame(),0,AerV2Eye::Left,Off(r),Clock());
    auto f=Frame(2);auto c=Clock();++f.rendererEpoch;++c.epochs.renderer;
    Check(p.OnSceneFrame(r,f,1,AerV2Eye::Right,Off(r),c),"new renderer frame rejected");
    const auto latest=p.ForMainWrite(30);Check(latest.has_value(),"new renderer receipt missing");
    Check(!p.OnSceneFrame(r,Frame(),0,AerV2Eye::Left,Off(r),Clock()),"stale scene callback accepted");
    Check(p.ForMainWrite(30)==latest,"old callback's retired renderer epoch revoked the current source");
}
static void GuardedInputAndClock(){
    MainEyeSourceLatch p;auto r=p.Request(true);auto c=Clock();c.epochs.renderer=21;
    Check(!p.OnSceneFrame(r,Frame(),0,AerV2Eye::Left,Off(r),c),"wrong-renderer fit snapshot accepted");
    Check(!p.OnSceneFrame(r,Frame(2),INT64_MAX,AerV2Eye::Right,Off(r),Clock()),"unrepresentable source accepted");
    Check(!p.OnSceneFrame(r,Frame(3),2,AerV2Eye::Right,Off(r),Clock()),"physical eye/namespace parity mismatch accepted");
    auto f=Frame(4);f.gameplayValid=false;
    Check(!p.OnSceneFrame(r,f,3,AerV2Eye::Right,Off(r),Clock()),"non-scene frame activated");
    c=Clock();c.fit={true,100,1e100,1e100};
    Check(!p.OnSceneFrame(r,Frame(5),4,AerV2Eye::Left,Off(r),c),"overflowing valid fit silently fell back");
}
static void ClockCopyAndActualWrite(){
    MainEyeSourceLatch p;auto r=p.Request(true);auto c=Clock();c.fit={true,100,.01,100.0};
    Check(p.OnSceneFrame(r,Frame(),2,AerV2Eye::Left,Off(r),c),"fit source rejected");
    auto receipt=p.ForMainWrite(30);Check(receipt.has_value(),"missing fit receipt");
    c.fit.offsetSeconds=900; // passed snapshot must no longer be aliased
    Check(receipt->poseTimeFromFit && receipt->requestedPoseTimeNs==100'020'000'000LL && receipt->clockGeneration==40,"fit snapshot did not bind prediction at source creation");
    Check(receipt->MatchesWrittenPose(30,100'020'000'000LL),"actual written pose time rejected");
    Check(!receipt->MatchesWrittenPose(30,100'020'000'001LL) && !receipt->MatchesWrittenPose(31,100'020'000'000LL),"different actual pose relabelled with requested time");
}
static void ImmutableReceiptSurvivesCopies(){
    MainEyeSourceLatch p;auto r=p.Request(true);p.OnSceneFrame(r,Frame(),0,AerV2Eye::Left,Off(r),Clock());
    auto receipt=p.ForMainWrite(30);Check(receipt.has_value(),"no receipt for copy test");
    cvr::camera::PoseAddressLedger<MainEyeSourceReceipt> ledger;cvr::camera::PoseFingerprint fingerprint{};
    ledger.Publish(100,fingerprint,*receipt);auto source=ledger.Capture(100,fingerprint);
    Check(ledger.Transfer(source,fingerprint,200,fingerprint),"native serialization copy lost source receipt");
    Check(ledger.Capture(200,fingerprint).payload==*receipt,"source/epochs/time changed through ledger copy");
    auto other=*receipt;other.sourceId=1;other.physicalEye=AerV2Eye::Right;
    ledger.Publish(300,fingerprint,other);
    const std::array inputs{decltype(ledger)::BlendInput{source,fingerprint,.5f},decltype(ledger)::BlendInput{ledger.Capture(300,fingerprint),fingerprint,.5f}};
    Check(!ledger.TransferBlend(inputs,400,fingerprint,[](const auto& a,const auto& b){return a==b;}),"different source-eye epochs survived blend");
}
int main(){int passed=0,failed=0;
    const std::pair<const char*,void(*)()> tests[]={{"latch_default_and_deferred",DormantAndDeferred},{"latch_off_proof_scope",OffProofIsScoped},
    {"latch_duplicate_immutable",DuplicateDoesNotFlip},{"latch_disable_revoke",DisableRevokes},{"latch_independent_epochs",EpochsAreIndependent},
    {"latch_invalidation_boundary",InvalidateWaitsNewFrame},{"latch_stale_frame",StaleFrameCannotRevokeCurrent},{"latch_input_clock_guards",GuardedInputAndClock},{"latch_clock_actual_pose",ClockCopyAndActualWrite},
    {"latch_receipt_copy_blend",ImmutableReceiptSurvivesCopies}};
    for(const auto& [name,test]:tests)try{test();++passed;std::cout<<"PASS "<<name<<'\n';}catch(const std::exception& e){++failed;std::cerr<<"FAIL "<<name<<": "<<e.what()<<'\n';}
    std::cout<<"RESULT "<<passed<<" passed, "<<failed<<" failed\n";return failed?1:0;
}
