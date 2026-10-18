#include <Windows.h>
#include "Framegen/MainEyePoseSample.hpp"
#include "Framegen/SourceSceneDelivery.hpp"
#include "Camera/MainEyeWriteDiagnostics.hpp"
#include "Camera/MainEyeCameraRoute.hpp"
#include "Camera/MainEyeTransportDiagnostics.hpp"
#include "Camera/MainEyeScenePose.hpp"
#include <cstddef>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <limits>
using namespace cvr::framegen;
static void Check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
struct Head {float posX{},posY{},posZ{},oriX{},oriY{},oriZ{},oriW=1;bool valid{};uint64_t originSerial{};int64_t locatedTime{};};
struct Sample {Head head;uint64_t poseId{};};
static MainEyeSourceReceipt Receipt(){return {2,1,2,{20,10,30},0,AerV2Eye::Left,40,5'000'000'000,false};}
static void ActualQueryAndCache(){
    MainEyePoseSample<Sample> cache;auto r=Receipt();int queried=0;
    auto validate=[](const auto&){return true;};
    auto locate=[&](int64_t time,Sample& s){++queried;Check(time==r.requestedPoseTimeNs,"requested time not passed to actual query");s={{1,2,3,0,0,0,1,true,30,time},123};return true;};
    auto a=cache.Acquire(r,validate,locate);Check(a && a->poseId==123 && queried==1,"successful exact query not used");
    auto b=cache.Acquire(r,validate,locate);Check(b && b->head.posX==1 && queried==1,"same receipt was independently located twice");
    ++r.sceneOrdinal;r.sourceId=1;r.physicalEye=AerV2Eye::Right;
    Check(cache.Acquire(r,validate,locate).has_value() && queried==2,"new source reused another frame's sample");
    const auto old=cache.Read(Receipt(),validate);
    Check(old && old->head.posX==1 && queried==2,"serialized older receipt was re-located or lost");
}
static void FailedOrFallbackPose(){
    for(int bad=0;bad<5;++bad){
        MainEyePoseSample<Sample> cache;auto r=Receipt();
        auto v=[](const auto&){return true;};
        auto q=[&](int64_t t,Sample& s){s={{0,0,0,0,0,0,1,true,30,t},7};
            if(bad==0)return false;if(bad==1)s.head.locatedTime=0;if(bad==2)s.head.originSerial=31;
            if(bad==3)s.head.valid=false;if(bad==4)s.head.posX=(std::numeric_limits<float>::infinity)();return true;};
        Check(!cache.Acquire(r,v,q),"failed/wrong-time/wrong-origin/nonfinite query accepted");
    }
}
static void QueryRaceAndOff(){
    MainEyePoseSample<Sample> cache;auto r=Receipt();bool active=false;int n=0;
    auto v=[&](const auto&){return active;};
    auto q=[&](int64_t t,Sample& s){++n;s={{0,0,0,0,0,0,1,true,30,t},9};active=false;return true;};
    Check(!cache.Acquire(r,v,q) && n==0,"OFF receipt issued an XR query");
    active=true;Check(!cache.Acquire(r,v,q) && n==1,"invalidation during locate escaped post-query validation");
    active=true;auto good=[&](int64_t t,Sample& s){++n;s={{0,0,0,0,0,0,1,true,30,t},10};return true;};
    Check(cache.Acquire(r,v,good).has_value(),"fresh retry failed");active=false;
    Check(!cache.Acquire(r,v,good),"cached pose survived owner invalidation");
}
static void BadQuaternionAndZeroIdentity(){
    MainEyePoseSample<Sample> cache;auto r=Receipt();auto v=[](const auto&){return true;};
    Check(!cache.Acquire(r,v,[](int64_t t,Sample& s){s={{0,0,0,0,0,0,0,true,30,t},1};return true;}),"zero quaternion accepted");
    Check(!cache.Acquire(r,v,[](int64_t t,Sample& s){s={{0,0,0,0,0,0,1,true,30,t},0};return true;}),"missing pose write identity accepted");
}
static void OffModeKeepsLegacyAndSourceDoesNotFallback(){
    int source=0,legacy=0;
    auto exact=[&]{++source;return false;};auto old=[&]{++legacy;return true;};
    Check(SelectMainEyePosePath(false,exact,old) && source==0 && legacy==1,"off mode changed legacy head sampling");
    Check(!SelectMainEyePosePath(true,exact,old) && source==1 && legacy==1,"failed source query fell through to cached/legacy time");
}
static void EvictedOldSourceIsNotQueriedAgain(){
    MainEyePoseSample<Sample,2> cache;auto r=Receipt();const auto first=r;int calls=0;
    auto valid=[](const auto&){return true;};
    auto locate=[&](int64_t t,Sample& s){++calls;s={{0,0,0,0,0,0,1,true,30,t},uint64_t(calls)};return true;};
    for(int i=0;i<3;++i){r.sceneOrdinal=uint64_t(i+1);r.sourceId=i;r.physicalEye=AerV2Eye(i&1);Check(cache.Acquire(r,valid,locate).has_value(),"fresh source query rejected");}
    Check(!cache.Read(first,valid) && !cache.Acquire(first,valid,locate) && calls==3,"evicted old source caused another filtered locate");
}
static void OffModeAllowsMenuButNeverNewAerStamp(){
    Check(PlanMainEyeCapture(false,true,false)==MainEyeCaptureMode::Legacy,"revoked old AER receipt blocked ordinary mono menu capture");
    Check(PlanMainEyeCapture(false,false,false)==MainEyeCaptureMode::Legacy,"default OFF changed baseline capture");
    Check(PlanMainEyeCapture(true,true,false)==MainEyeCaptureMode::Reject,"requested AER accepted wrong epoch raw capture");
    Check(PlanMainEyeCapture(true,false,true)==MainEyeCaptureMode::Reject,"empty source became a publication");
    Check(PlanMainEyeCapture(true,true,true)==MainEyeCaptureMode::Source,"valid source capture was blocked");
}
static void FaultRevokesCurrentOnly(){
    SourceSceneEpoch scene;scene.Attach(100);const auto f=scene.BeginFrame();
    SourceSceneDeliveryStamp stamp{3,f};MainEyeSourceLatch latch;const auto r=latch.Request(true);
    MainEyeSceneFrame frame{f.worldEpoch,f.ordinal,20,30,true};VrcamOffProof off{r,f.worldEpoch,true};
    MainEyeClockSnapshot clock{{20,f.worldEpoch,30},40,{},5000};
    latch.OnSceneFrame(r,frame,0,AerV2Eye::Left,off,clock);int faults=0;
    InvokeSourceSceneCallback([&]{throw std::runtime_error("partial source publication");},[&]{
        ++faults;if(SourceSceneDeliveryCurrent(stamp,3,scene.Snapshot())){
            const auto scope=latch.FaultScope(f.worldEpoch,f.ordinal);Check(scope.has_value(),"missing fault revoke scope");
            Check(scene.RejectFrame(f),"current failed frame stayed valid");Check(latch.RevokeFault(*scope),"failed current source was not revoked");
        }});
    Check(faults==1 && !scene.Snapshot().valid && !latch.Requested(),"fault did not close source eligibility/request");
    const auto next=scene.BeginFrame();Check(next.valid && next.ordinal==f.ordinal+1,"fault invented/detached scene frame");
}
static void StaleFaultDoesNotRevokeNewFrame(){
    SourceSceneEpoch scene;scene.Attach(100);const auto f=scene.BeginFrame();SourceSceneDeliveryStamp old{3,f};
    auto newer=scene.BeginFrame();Check(!SourceSceneDeliveryCurrent(old,3,newer),"old frame fault matched new frame");
    Check(!SourceSceneDeliveryCurrent(old,4,f),"old sink fault matched a replaced owner");
    Check(!scene.RejectFrame(f) && scene.Snapshot().valid,"late failure invalidated a new scene frame");
    MainEyeSourceLatch latch;auto r=latch.Request(true);MainEyeClockSnapshot clock{{20,10,30},40,{},5000};
    latch.OnSceneFrame(r,{10,1,20,30,true},0,AerV2Eye::Left,{r,10,true},clock);const auto scope=latch.FaultScope(10,1);
    Check(scope.has_value(),"no initial revoke scope");latch.OnSceneFrame(r,{10,2,20,30,true},1,AerV2Eye::Right,{r,10,true},clock);
    Check(!latch.RevokeFault(*scope) && latch.ForMainWrite(30),"delayed outside-lock revoke killed a new frame");
    const auto newerScope=latch.FaultScope(10,2);Check(newerScope.has_value(),"no current scope");
    latch.Request(false);r=latch.Request(true);latch.OnSceneFrame(r,{10,3,20,30,true},2,AerV2Eye::Left,{r,10,true},clock);
    Check(!latch.RevokeFault(*newerScope) && latch.Requested(),"old request fault killed a new request");
}
static void FaultCancelsPartialRequest(){
    MainEyeSourceLatch latch;latch.Request(true);
    const auto scope=latch.FaultScope(10,1);
    Check(scope && latch.RevokeFault(*scope) && !latch.Requested(),"fault after Request but before publication left raw capture blocked");
}
static void Production64RingBeyond128(){
    MainEyePoseSample<Sample> cache;int queries=0;auto validate=[](const auto&){return true;};
    for(int64_t i=0;i<1024;++i){auto r=Receipt();r.sourceId=i;r.physicalEye=AerV2Eye(i&1);r.sceneOrdinal=uint64_t(i+1);r.requestedPoseTimeNs+=i*1000000;
        auto query=[&](int64_t t,Sample& s){++queries;s={{0,0,0,0,0,0,1,true,30,t},uint64_t(i+1)};return true;};
        const auto first=cache.Acquire(r,validate,query);Check(first && first->poseId==uint64_t(i+1),"fresh source stopped at a cache boundary");
        Check(cache.Acquire(r,validate,query).has_value() && queries==i+1,"duplicate source queried twice after ring wrap");
    }
    Check(queries==1024 && !cache.Read(Receipt(),validate),"64-entry cache did not replace old samples");
}
static void PoseOutcomeDiagnostics(){
    MainEyePoseSample<Sample> cache;auto r=Receipt();MainEyePoseCode code=MainEyePoseCode::None;
    auto valid=[](const auto&){return true;};auto query=[](int64_t t,Sample& s){s={{0,0,0,0,0,0,1,true,30,t},123};return true;};
    Check(cache.Acquire(r,valid,query,&code).has_value() && code==MainEyePoseCode::Stored,"successful locate not distinguished from cached pose");
    Check(cache.Acquire(r,valid,query,&code).has_value() && code==MainEyePoseCode::CacheHit,"repeat not identified as cache hit");
    Check(cache.Read(r,valid,&code).has_value() && code==MainEyePoseCode::ReadHit,"serialization cache hit unavailable");
    Check(!cache.Acquire(r,[](const auto&){return false;},query,&code) && code==MainEyePoseCode::OwnerRejected,"receipt rejection counted as XR failure");
    ++r.sceneOrdinal;r.sourceId=2;
    Check(!cache.Acquire(r,valid,[](int64_t,Sample&){return false;},&code) && code==MainEyePoseCode::LocateFailed,"actual locate failure not reported");
}
static void WriteGateWireAbi(){
    using namespace cvr::camera;
    static_assert(offsetof(MainEyeWriteTelemetry,writeBits)==72 && offsetof(MainEyeWriteTelemetry,sourceId)==80);
    static_assert(offsetof(MainEyeWriteTelemetry,writtenPoseId)==88 && offsetof(MainEyeWriteTelemetry,requestedTimeNs)==96);
    static_assert(offsetof(MainEyeWriteTelemetry,locateCalls)==112 && offsetof(MainEyeWriteTelemetry,locateMissingSource)==120);
    MainEyeWriteTelemetry packet;packet.calls=1154;packet.sourceId=1154;packet.writtenPoseId=(uint64_t{1}<<62)+2307;
    packet.requestedTimeNs=135779134873692;packet.writtenTimeNs=135779145667000;
    packet.writeBits=WriteSourcePresent|WritePoseLocated|WriteOrientationWritten|WritePoseIdPresent;
    packet.locateBits=LocateComponentPresent|LocateComponentIsMain|LocateHasXR;packet.locateCalls=125;
    std::array<unsigned char,128> wire{};std::memcpy(wire.data(),&packet,sizeof(packet));
    uint32_t bytes{},bits{};uint64_t pose{};int64_t target{},written{};
    std::memcpy(&bytes,wire.data()+4,4);std::memcpy(&bits,wire.data()+72,4);std::memcpy(&pose,wire.data()+88,8);
    std::memcpy(&target,wire.data()+96,8);std::memcpy(&written,wire.data()+104,8);
    Check(bytes==128 && pose==packet.writtenPoseId && target==packet.requestedTimeNs && written==packet.writtenTimeNs,"RPM wire ABI lost uint64 identity or distinct pose times");
    Check((bits&WritePoseLocated) && !(bits&WriteTranslationLabelKnown) && !(bits&WritePublisherHasSource),"acquire success appeared as a completed source publication in wire flags");
}
static void SelectedSceneRouteInvalidatesOnlyAtMainRead(){
    using namespace cvr::camera;
    MainEyeSourceLatch latch;MainEyeCameraRouteGate route;const auto revision=latch.Request(true);
    const MainEyeClockSnapshot clock{{20,10,30},40,{},5000};
    auto publish=[&](uint64_t ordinal,int64_t id){return latch.OnSceneFrame(revision,{10,ordinal,20,30,true},id,AerV2Eye(id&1),{revision,10,true},clock);};
    Check(publish(1,0),"initial component source rejected");const auto component=*latch.ForMainWrite(30);
    Check(AdmitSelectedMainEyeRoute(route,latch,MainEyeCameraRoute::Component),"first selected component rejected");
    // Merely serializing a scene blend candidate does not call this boundary.
    const auto unselectedSceneCandidate=component;
    Check(latch.Usable(unselectedSceneCandidate),"unused scene candidate changed selected route");
    Check(!AdmitSelectedMainEyeRoute(route,latch,MainEyeCameraRoute::Scene) && !latch.Usable(component),"selected scene carried old component history");
    Check(publish(2,1),"new frame did not rearm scene source");const auto scene=*latch.ForMainWrite(30);
    Check(scene.latchRevision>component.latchRevision && AdmitSelectedMainEyeRoute(route,latch,MainEyeCameraRoute::Scene),"scene failed after next actual frame");
    Check(!AdmitSelectedMainEyeRoute(route,latch,MainEyeCameraRoute::Component) && !latch.Usable(scene),"return to component reused scene history");
    Check(publish(3,2) && latch.ForMainWrite(30)->latchRevision>scene.latchRevision,"return did not require a new receipt");
    latch.Request(false);Check(!route.Observe(false,MainEyeCameraRoute::Scene),"OFF route requested another invalidation");
}
static void SceneEyeAndTrackingHeadProof(){
    using namespace cvr::camera;auto receipt=Receipt();
    const std::array<int32_t,3> nativeScene{10*131072,20*131072,30*131072};
    const auto center=AddSceneSourceTranslation(nativeScene,{.25f,-.5f,.125f});
    Check(center && (*center)[0]==10*131072+32768 && (*center)[1]==20*131072-65536,"tracking translation was omitted or added twice");
    const int32_t half=4096;const auto left=AddSceneSourceEye(*center,{int32_t(half*MainEyeSceneSign(AerV2Eye::Left)),0,0});
    const auto right=AddSceneSourceEye(*center,{int32_t(half*MainEyeSceneSign(AerV2Eye::Right)),0,0});
    Check(left && right && (*left)[0]==(*center)[0]-half && (*right)[0]==(*center)[0]+half && (*right)[0]-(*left)[0]==2*half,"latched eyes did not symmetrically split actual scene source");
    Check(!SceneSourceWriteComplete(receipt,true,true,false,true),"orientation-only scene was labelled with tracked head translation");
    Check(!SceneSourceWriteComplete(receipt,true,true,true,false),"scene without actual eye write was labelled");
    Check(!SceneSourceWriteComplete(receipt,false,true,true,true),"wrong-time scene received source receipt");
    Check(SceneSourceWriteComplete(receipt,true,true,true,true),"completed actual scene writer lost its source");
    Check(!AddSceneSourceEye({INT32_MAX,0,0},{1,0,0}) && !AddSceneSourceTranslation({INT32_MAX,0,0},{1,0,0}),"scene fixed-point overflow accepted");
}
static void MissingSelectionDoesNotRevokeKnownRoute(){
    using namespace cvr::camera;
    MainEyeSourceLatch latch;MainEyeCameraRouteGate route;const auto revision=latch.Request(true);
    const MainEyeClockSnapshot clock{{20,10,30},40,{},5000};
    auto publish=[&](uint64_t ordinal,int64_t id){return latch.OnSceneFrame(revision,{10,ordinal,20,30,true},id,AerV2Eye(id&1),{revision,10,true},clock);};
    Check(publish(1,0),"initial component frame failed");const auto component=*latch.ForMainWrite(30);
    Check(AdmitSelectedMainEyeRoute(route,latch,MainEyeCameraRoute::Component),"known component failed");
    for(int i=0;i<8;++i){
        Check(!AdmitSelectedMainEyeRoute(route,latch,MainEyeCameraRoute::Unknown),"missing selection accepted");
        Check(latch.Usable(component),"missing selection revoked the known component latch");
        Check(AdmitSelectedMainEyeRoute(route,latch,MainEyeCameraRoute::Component),"known/missing alternation starved component");
    }
    Check(!AdmitSelectedMainEyeRoute(route,latch,MainEyeCameraRoute::Scene) && !latch.Usable(component),"known scene transition kept old history");
    Check(!AdmitSelectedMainEyeRoute(route,latch,MainEyeCameraRoute::Unknown),"stale transition receipt accepted");
    Check(publish(2,1),"actual next scene frame failed");const auto scene=*latch.ForMainWrite(30);
    for(int i=0;i<8;++i){
        Check(AdmitSelectedMainEyeRoute(route,latch,MainEyeCameraRoute::Scene),"scene failed after known transition");
        Check(!AdmitSelectedMainEyeRoute(route,latch,MainEyeCameraRoute::Unknown) && latch.Usable(scene),"missing selection revoked actual scene");
    }
    Check(!AdmitSelectedMainEyeRoute(route,latch,MainEyeCameraRoute::Component) && !latch.Usable(scene),"known return kept scene history");
    Check(publish(3,2) && AdmitSelectedMainEyeRoute(route,latch,MainEyeCameraRoute::Component),"return failed to rearm on actual frame");
    const auto returned=*latch.ForMainWrite(30);
    Check(scene.latchRevision==component.latchRevision+1 && returned.latchRevision==scene.latchRevision+1,"absence advanced latch revision");
}
static void TransportWireAbi(){
    using namespace cvr::camera;
    MainEyeTransportTelemetry d;
    Check(d.bytes==192 && d.mainReadCalls==0 && d.selectedSourceId==-1 && d.currentSourceId==-1,"empty transport packet claims selected source");
    d.mainReadCalls=11;d.mainReadMissing=3;d.routeInvalidations=2;d.selectedSetup=0x0000123412340000;
    d.selectedSourceId=49;d.currentSourceId=75;d.selectedLatch=7;d.currentLatch=8;d.latchAfter=8;
    d.bits=TransportSelectedPresent|TransportSourcePresent|TransportCurrentPresent|TransportRequested;
    std::array<unsigned char,192> wire{};std::memcpy(wire.data(),&d,sizeof(d));
    uint64_t calls{},selected{},current{},after{};int64_t id{};uint32_t bits{};
    std::memcpy(&calls,wire.data()+40,8);std::memcpy(&id,wire.data()+128,8);std::memcpy(&selected,wire.data()+144,8);
    std::memcpy(&current,wire.data()+152,8);std::memcpy(&bits,wire.data()+180,4);std::memcpy(&after,wire.data()+184,8);
    Check(calls==11 && id==49 && selected==7 && current==8 && after==8,"transport wire lost stale/current latch separation");
    Check((bits&TransportSourcePresent) && !(bits&TransportUsable) && !(bits&TransportInvalidated),"stale receipt was reported as valid or caused an observed transition");
}
int main(){int pass=0,fail=0;const std::pair<const char*,void(*)()> tests[]={{"pose_exact_once",ActualQueryAndCache},{"pose_no_fallback",FailedOrFallbackPose},
    {"pose_race_off",QueryRaceAndOff},{"pose_value_guards",BadQuaternionAndZeroIdentity},{"pose_off_mode",OffModeKeepsLegacyAndSourceDoesNotFallback},
    {"pose_evicted_source",EvictedOldSourceIsNotQueriedAgain},{"capture_off_menu",OffModeAllowsMenuButNeverNewAerStamp},
    {"scene_fault_revoke",FaultRevokesCurrentOnly},{"scene_stale_fault",StaleFaultDoesNotRevokeNewFrame},{"scene_partial_request_fault",FaultCancelsPartialRequest},
    {"ring64_beyond128",Production64RingBeyond128},{"pose_outcome",PoseOutcomeDiagnostics},{"write_gate_wire",WriteGateWireAbi},
    {"selected_scene_route",SelectedSceneRouteInvalidatesOnlyAtMainRead},{"scene_eye_head",SceneEyeAndTrackingHeadProof},
    {"missing_selected_route",MissingSelectionDoesNotRevokeKnownRoute},{"transport_wire",TransportWireAbi}};
    for(const auto& [name,test]:tests)try{test();++pass;std::cout<<"PASS "<<name<<'\n';}catch(const std::exception& e){++fail;std::cerr<<"FAIL "<<name<<": "<<e.what()<<'\n';}
    std::cout<<"RESULT "<<pass<<" passed, "<<fail<<" failed\n";return fail?1:0;}
