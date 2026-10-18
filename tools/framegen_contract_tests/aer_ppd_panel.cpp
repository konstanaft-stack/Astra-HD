#include "Overlay/PpdPanel.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace cvr::ppd;
static void Check(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
static bool Near(float a,float b,float tolerance=.0001f){return std::abs(a-b)<=tolerance;}
static bool RayNear(float a,float b){return std::abs(a-b)<=4*(std::numeric_limits<float>::epsilon)()*std::max(1.f,std::abs(b));}
static PpdSourceObservation Source(uint64_t resource=1,uint64_t revision=0,uint32_t w=2048,uint32_t h=2048,uint64_t serial=10){
    PpdSourceObservation s;s.scope={resource,resource,10,20,resource+1,resource+1};s.nativeRevision=revision;s.aerV2=true;
    for(unsigned e=0;e<2;++e){auto& v=s.eyes[e];v.mainSource={w,h};v.srInput={w/2,h/2};v.outputEye={w,h};
        v.renderedFov={-float(std::atan(1.0)),float(std::atan(1.0)),float(std::atan(1.0)),-float(std::atan(1.0))};
        v.crop={0,0,float(w),float(h)};v.sourceId=e;v.sourceSerial=serial+e;
        v.actualMainDescriptor=v.nativeSlProjection=v.srInputKnown=v.actualOutputDescriptor=true;}
    return s;
}
static PpdAcceptedObservation Accepted(const PpdSourceObservation& s,uint32_t mask=3){
    return {s.scope,s.nativeRevision,{s.eyes[0].sourceId,s.eyes[1].sourceId},{s.eyes[0].sourceSerial,s.eyes[1].sourceSerial},mask,true,{s.eyes[0].outputEye,s.eyes[1].outputEye}};
}
static NativeApplyStatus Native(uint64_t r,NativeSize size,NativeApplyStage stage=NativeApplyStage::ObservedNative){
    NativeApplyStatus n;n.revision=r;n.stage=stage;n.requested=size;n.observedActive=size;n.observedPending=size;n.dispatchVisit=1;n.observationVisit=2;return n;
}
static void Baseline(PpdPanelController& c){c.SetAerEnabled(true,0);auto s=Source();Check(c.ObserveSource(s,0),"valid baseline source rejected");Check(c.ObserveAccepted(Accepted(s),1),"baseline acceptance rejected");}
static NativeSize Start(PpdPanelController& c,uint64_t r=1){
    Check(c.SetTarget(24),"valid target rejected");NativeSize wanted{};
    Check(c.Apply(10,[&](uint32_t w,uint32_t h){wanted={w,h};return r;}),"calibrated Adapt did not request native size");return wanted;
}
static void ExactRayMetric(){
    auto s=Source();const auto p=CalibrateActualSourcePpd(s);
    const double exact=1024.0*std::acos(-1.0)/180.0;
    Check(p.valid && std::abs(p.ppd-exact)<.00001 && p.nominalWidth==2048 && p.nominalHeight==2048,"90-degree ray derivative incorrect");
    const double angle=.000001;const double pixelDerivative=(1024.0*std::tan(angle)-1024.0*std::tan(-angle))/(2*angle);
    Check(std::abs(p.pixelsPerTangent-pixelDerivative)<.00001,"calibration differs from independent central ray derivative");
}
static void AsymmetricCentersAndCrop(){
    auto s=Source();s.eyes[0].renderedFov.angleLeft=-std::atan(.5f);s.eyes[0].renderedFov.angleRight=std::atan(1.5f);
    s.eyes[1].renderedFov.angleLeft=-std::atan(1.5f);s.eyes[1].renderedFov.angleRight=std::atan(.5f);
    const auto full=CalibrateActualSourcePpd(s);Check(full.valid && RayNear(full.pixelsPerTangent,1024),"opposite real optical centers collapsed to a fake common center");
    for(auto& e:s.eyes)e.crop={128,128,1920,1920};const auto crop=CalibrateActualSourcePpd(s);
    Check(crop.valid && Near(crop.ppd,full.ppd),"crop changed central ray sampling density");
    s.eyes[0].crop[0]=600;Check(!CalibrateActualSourcePpd(s).valid,"crop excluding optical axis accepted");
}
static void UnequalAxesAndEyeDensity(){
    auto s=Source();for(auto& e:s.eyes){e.renderedFov.angleUp=std::atan(2.f);e.renderedFov.angleDown=-std::atan(2.f);}
    const auto p=CalibrateActualSourcePpd(s);Check(p.valid && RayNear(p.pixelsPerTangent,512) && RayNear(p.nominalWidth,1024) && RayNear(p.nominalHeight,2048),"limiting vertical density/nominal extent wrong");
}
static void UnprovenGeometryRejected(){
    auto s=Source();s.eyes[0].nativeSlProjection=false;Check(!CalibrateActualSourcePpd(s).valid,"raw/unproven HMD projection accepted");
    s=Source();s.eyes[1].sourceSerial=s.eyes[0].sourceSerial;Check(!CalibrateActualSourcePpd(s).valid,"two fake IDs from a paired native frame accepted");
    s=Source();s.eyes[1].renderedFov.angleRight=0;Check(!CalibrateActualSourcePpd(s).valid,"invalid frustum accepted");
}
static void AcceptanceRequiredForCalibration(){
    PpdPanelController c;c.SetAerEnabled(true,0);const auto s=Source();c.ObserveSource(s,0);
    Check(!c.Snapshot().calibrated && !c.Snapshot().canAdapt,"source observation alone established accepted calibration");
    c.ObserveAccepted(Accepted(s,1),1);Check(!c.Snapshot().calibrated,"one eye calibrated both");
    c.ObserveAccepted(Accepted(s,2),2);c.SetTarget(24);Check(c.Snapshot().calibrated && c.Snapshot().canAdapt,"same-epoch second accepted eye did not complete calibration");
}
static void RequestedIsNotApplied(){
    PpdPanelController c;Baseline(c);const auto w=Start(c);Check(w.width==2752 && w.height==2752,"nearest16 preview differs from analytic 24PPD target");
    Check(c.Snapshot().stage==PpdPanelStage::Requested && c.Snapshot().appliedTarget==0,"click acknowledged application");
    c.ObserveNative(Native(1,w,NativeApplyStage::Dispatched),11);Check(c.Snapshot().stage==PpdPanelStage::WaitingNative,"pending store acknowledged native resize");
    c.ObserveNative(Native(1,w),12);Check(c.Snapshot().stage==PpdPanelStage::WaitingSource,"native fields alone acknowledged source");
}
static void FullAcknowledgment(){
    PpdPanelController c;Baseline(c);const auto w=Start(c);c.ObserveNative(Native(1,w),12);
    const auto s=Source(2,1,w.width,w.height,20);c.ObserveSource(s,13);Check(c.Snapshot().stage==PpdPanelStage::WaitingEyes,"new descriptors skipped XR acceptance");
    c.ObserveAccepted(Accepted(s,1),14);Check(c.Snapshot().stage==PpdPanelStage::WaitingEyes,"one eye marked Applied");
    c.ObserveAccepted(Accepted(s,2),15);const auto p=c.Snapshot();
    Check(p.stage==PpdPanelStage::Applied && p.acceptedEyeMask==3 && p.appliedTarget==24 && p.appliedMeasured>0,"matching native/source/both-eyes did not apply");
}
static void SameEpochOrOldSerialNotNewResize(){
    PpdPanelController c;Baseline(c);const auto w=Start(c);c.ObserveNative(Native(1,w),12);
    auto s=Source(1,1,w.width,w.height,20);c.ObserveSource(s,13);c.ObserveAccepted(Accepted(s),14);
    Check(c.Snapshot().stage!=PpdPanelStage::Applied,"changed dimensions in old resource epoch applied");
    s=Source(2,1,w.width,w.height,10);c.ObserveSource(s,15);c.ObserveAccepted(Accepted(s),16);
    Check(c.Snapshot().stage!=PpdPanelStage::Applied,"pre-request source serials became resize proof");
}
static void WrongIdentityAndFailedSubmission(){
    PpdPanelController c;Baseline(c);const auto w=Start(c);c.ObserveNative(Native(1,w),12);const auto s=Source(2,1,w.width,w.height,20);c.ObserveSource(s,13);
    auto a=Accepted(s);a.sourceIds[0]+=2;c.ObserveAccepted(a,14);Check(c.Snapshot().stage!=PpdPanelStage::Applied,"unknown source ID accepted");
    a=Accepted(s);a.xrEndFrameAccepted=false;c.ObserveAccepted(a,15);Check(c.Snapshot().stage!=PpdPanelStage::Applied,"failed XR submission accepted");
    a=Accepted(s);++a.scope.modeGeneration;c.ObserveAccepted(a,16);Check(c.Snapshot().stage!=PpdPanelStage::Applied,"wrong generation accepted");
}
static void RevisionsNeverMix(){
    PpdPanelController c;Baseline(c);auto w=Start(c);c.ObserveNative(Native(1,w),12);auto s=Source(2,1,w.width,w.height,20);c.ObserveSource(s,13);
    Check(c.SetTarget(28),"second target rejected");NativeSize second{};c.Apply(14,[&](uint32_t x,uint32_t y){second={x,y};return uint64_t{2};});
    c.ObserveNative(Native(2,second),15);c.ObserveAccepted(Accepted(s),16);Check(c.Snapshot().nativeRevision==2 && c.Snapshot().stage!=PpdPanelStage::Applied,"old accepted pair acknowledged replacement");
    c.ObserveNative(Native(3,second),17);Check(c.Snapshot().stage==PpdPanelStage::Error && c.Snapshot().error==PpdPanelError::RevisionChanged,"external replacement not detected");
}
static void WorldResetAndModeOff(){
    PpdPanelController c;Baseline(c);const auto w=Start(c);c.ObserveNative(Native(1,w),12);
    auto s=Source(2,1,w.width,w.height,20);++s.scope.gameWorldEpoch;c.ObserveSource(s,13);c.ObserveAccepted(Accepted(s),14);
    Check(c.Snapshot().stage==PpdPanelStage::Error && c.Snapshot().error==PpdPanelError::ContextChanged,"world reset applied old intent");
    c.SetAerEnabled(false,15);int requests=0;c.Apply(16,[&](uint32_t,uint32_t){++requests;return uint64_t{2};});
    Check(!c.Snapshot().canAdapt && requests==0,"paired mode dispatched live PPD");
}
static void TimeoutIsSticky(){
    PpdPanelController c;Baseline(c);const auto w=Start(c);c.ObserveNative(Native(1,w),10+PpdPanelController::ApplyTimeoutMs);
    auto s=Source(2,1,w.width,w.height,20);c.ObserveSource(s,20000);c.ObserveAccepted(Accepted(s),20001);
    Check(c.Snapshot().stage==PpdPanelStage::Error && c.Snapshot().error==PpdPanelError::Timeout,"late data resurrected timed-out apply");
}
static void NativeFailureNoAcceptance(){
    PpdPanelController c;Baseline(c);const auto w=Start(c);auto n=Native(1,w,NativeApplyStage::Error);n.error=NativeApplyError::PendingWriteFailed;c.ObserveNative(n,11);
    Check(c.Snapshot().stage==PpdPanelStage::Error && c.Snapshot().error==PpdPanelError::NativeFailure,"native failure became readiness");
}
static void InvalidTargetsAndRequestFailure(){
    PpdPanelController c;Baseline(c);Check(!c.SetTarget(4) && !c.SetTarget(41) && !c.SetTarget(NAN),"invalid target accepted");
    c.SetTarget(24);Check(!c.Apply(10,[](uint32_t,uint32_t){return uint64_t{0};}) && c.Snapshot().error==PpdPanelError::RequestRejected,"failed native request acknowledged");
}
static void SameQuantizedSizeIsNotRequested(){
    PpdPanelController c;Baseline(c);auto s=c.Snapshot();Check(s.sameQuantizedSize && !s.canAdapt,"same-size target was offered as a resize");
    int requests=0;Check(!c.Apply(10,[&](uint32_t,uint32_t){++requests;return uint64_t{1};}) && requests==0,"no-op dispatched a request requiring a new resource epoch");
    c.SetTarget(s.calibration.ppd+.001f);Check(c.Snapshot().sameQuantizedSize && !c.Snapshot().canAdapt,"nearest16 no-op was not detected");
}
static void OldOutputCannotApplyNewSource(){
    PpdPanelController c;Baseline(c);const auto w=Start(c);c.ObserveNative(Native(1,w),12);
    auto old=Source(2,1,w.width,w.height,20);for(auto& e:old.eyes)e.outputEye={2048,2048};
    c.ObserveSource(old,13);c.ObserveAccepted(Accepted(old),14);
    Check(c.Snapshot().stage!=PpdPanelStage::Applied,"new MAIN texture accepted while actual eye output remained old size");
    auto current=old;for(auto& e:current.eyes)e.outputEye=w;
    Check(c.ObserveSource(current,15),"new output descriptor rejected for unchanged held source");
    c.ObserveAccepted(Accepted(old),16);Check(c.Snapshot().stage!=PpdPanelStage::Applied,"old-output ACK used a later output descriptor");
    c.ObserveAccepted(Accepted(current),17);Check(c.Snapshot().stage==PpdPanelStage::Applied,"new output plus exact ACK did not complete application");
}
static void PartialMasksNeverCrossEpoch(){
    PpdPanelController c;Baseline(c);const auto w=Start(c);c.ObserveNative(Native(1,w),12);
    const auto a=Source(2,1,w.width,w.height,20),b=Source(3,1,w.width,w.height,30);
    c.ObserveSource(a,13);c.ObserveAccepted(Accepted(a,1),14);c.ObserveSource(b,15);c.ObserveAccepted(Accepted(b,2),16);
    Check(c.Snapshot().stage!=PpdPanelStage::Applied,"accepted eye masks crossed resource epochs");
    Check(!c.ObserveAccepted(Accepted(a,1),17),"stale epoch ack matched latest scope");
    c.ObserveAccepted(Accepted(b,1),18);Check(c.Snapshot().stage==PpdPanelStage::Applied,"current epoch partial masks failed");
}
static void EvictedObservationCannotAcknowledge(){
    PpdPanelController c;Baseline(c);const auto w=Start(c);c.ObserveNative(Native(1,w),12);
    auto first=Source(2,1,w.width,w.height,20);c.ObserveSource(first,13);
    auto last=first;
    for(unsigned i=1;i<=9;++i){last=Source(2,1,w.width,w.height,20+i*2);last.eyes[0].sourceId=i*2;last.eyes[1].sourceId=i*2+1;c.ObserveSource(last,14+i);}
    Check(!c.ObserveAccepted(Accepted(first),25) && c.Snapshot().stage!=PpdPanelStage::Applied,"evicted source metadata was recreated for an ACK");
    c.ObserveAccepted(Accepted(last),26);Check(c.Snapshot().stage==PpdPanelStage::Applied,"current source ACK did not survive bounded history churn");
}
static void MutatedSourceAndReversedClock(){
    PpdPanelController c;Baseline(c);const auto w=Start(c);c.ObserveNative(Native(1,w),12);auto s=Source(2,1,w.width,w.height,20);c.ObserveSource(s,13);
    s.eyes[0].renderedFov.angleRight+=.01f;Check(!c.ObserveSource(s,14) && c.Snapshot().error==PpdPanelError::InvalidSource,"same source identity accepted changed projection");
    PpdPanelController d;Baseline(d);const auto v=Start(d);d.ObserveNative(Native(1,v),9);Check(d.Snapshot().error==PpdPanelError::ClockChanged,"reversed timeout clock accepted");
}
static void StaleWorldDoesNotRevokeCurrent(){
    PpdPanelController c;Baseline(c);const auto w=Start(c);c.ObserveNative(Native(1,w),12);const auto s=Source(2,1,w.width,w.height,20);c.ObserveSource(s,13);
    auto stale=s;--stale.scope.gameWorldEpoch;Check(!c.ObserveSource(stale,14),"old world source entered current history");
    c.ObserveAccepted(Accepted(s),15);Check(c.Snapshot().stage==PpdPanelStage::Applied,"old world callback revoked current request");
}
static void AppliedWithdrawsOnOutputOrOriginChange(){
    for(int kind=0;kind<2;++kind){
        PpdPanelController c;Baseline(c);const auto w=Start(c);c.ObserveNative(Native(1,w),12);auto s=Source(2,1,w.width,w.height,20);
        c.ObserveSource(s,13);c.ObserveAccepted(Accepted(s),14);Check(c.Snapshot().stage==PpdPanelStage::Applied,"initial proof missing");
        if(kind==0)s.eyes[0].outputEye={2048,2048};else ++s.scope.hmdWorldEpoch;
        c.ObserveSource(s,15);Check(c.Snapshot().stage==PpdPanelStage::Error,"Applied survived changed output or tracking origin");
    }
}
static void NativeObservedFieldsMustMatch(){
    PpdPanelController c;Baseline(c);const auto w=Start(c);auto n=Native(1,w);n.observedActive={2048,2048};c.ObserveNative(n,12);
    const auto s=Source(2,1,w.width,w.height,20);c.ObserveSource(s,13);c.ObserveAccepted(Accepted(s),14);
    Check(c.Snapshot().stage==PpdPanelStage::WaitingNative,"native stage label replaced actual matching native dimensions");
    c.ObserveNative(Native(1,w),15);Check(c.Snapshot().stage==PpdPanelStage::Applied,"matching native fields did not release already valid source/eye proof");
}
static void ReferenceRuntimeFit(){
    // Read-only Luke42DA sample, 2026-10-05, exact simulator PID13316.
    auto s=Source(1,0,1632,1744);
    for(auto& e:s.eyes){
        e.renderedFov={-std::atan(1.0812467336654663f),std::atan(1.0812467336654663f),
            std::atan(1.1554498672485352f),-std::atan(1.1554498672485352f)};
        e.runtimeFov={-.8203047513961792f,.8203047513961792f,.8573929667472839f,-.8573929667472839f};
        e.referenceOptics=true;e.crop={7,0,1626,1744};e.outputEye={1619,1744};
    }
    const auto c=CalibrateActualSourcePpd(s);
    Check(c.valid && Near(c.ppd,13.171727180480957f,.00002f) && Near(c.nominalWidth,1618.599853515625f,.001f)
        && Near(c.nominalHeight,1744.f,.001f),"PPD is fitted to the full rendered frustum instead of the original runtime eye region");
    const auto preview=PreviewCalibratedPpd(c,16);
    Check(preview.width==1968 && preview.height==2112,"reference16PPD nonsquare preview differs");
}
static ReferenceOptics MeasuredOptics(){
    return BuildReferenceOptics({PpdRenderedFov{-.9424777626991272f,.6981316804885864f,.7675957679748535f,-.9471901655197144f},
        PpdRenderedFov{-.6981316804885864f,.9424777626991272f,.7675957679748535f,-.9471901655197144f}});
}
static void ReferenceFovMath(){
    const auto o=MeasuredOptics();Check(o.valid,"measured optics rejected");
    Check(Near(o.pitch,-.08979719877243042f,.0000001f)&&Near(o.yaw,.12217304110527039f,.0000001f),"reference pitch/yaw differs");
    for(const auto& f:o.applied)Check(Near(f.angleLeft,-.8203047513961792f,.0000001f)&&Near(f.angleRight,.8203047513961792f,.0000001f)
        &&Near(f.angleUp,.8573929667472839f,.0000001f)&&Near(f.angleDown,-.8573929667472839f,.0000001f),"applied FOV differs from captured original");
    auto bad=o.raw;bad[1].angleUp=NAN;Check(!BuildReferenceOptics(bad).valid,"invalid runtime FOV accepted");
    const auto c=CalibrateReferenceOptics(1632,1744,o);
    Check(ReferenceEyeCrop(1632,1744,c,o.applied[0])==std::array<float,4>{7,0,1626,1744},"original crop rounding/clamp differs");
}
static PpdSourceObservation OpticsSource(uint64_t epoch,uint64_t revision,uint32_t w,uint32_t h,uint64_t serial){
    auto s=Source(epoch,revision,w,h,serial);const auto o=MeasuredOptics();const auto c=CalibrateReferenceOptics(w,h,o);
    for(unsigned i=0;i<2;++i){auto& e=s.eyes[i];e.referenceOptics=true;e.runtimeFov=o.applied[i];e.renderedFov=ReferenceSourceFov(w,h,c);
        e.crop=ReferenceEyeCrop(w,h,c,e.runtimeFov);e.outputEye={uint32_t(e.crop[2]-e.crop[0]),uint32_t(e.crop[3]-e.crop[1])};}
    return s;
}
static void ReferenceCropApply(){
    PpdPanelController c;c.SetAerEnabled(true,0);auto before=OpticsSource(1,0,1632,1744,10);
    Check(c.ObserveSource(before,0)&&c.ObserveAccepted(Accepted(before),1),"reference baseline rejected");
    c.SetTarget(16);NativeSize wanted{};
    Check(c.Apply(10,[&](uint32_t w,uint32_t h){wanted={w,h};return uint64_t{1};}),"reference Adapt rejected");
    Check(wanted==NativeSize{1968,2112},"reference Adapt native target differs");c.ObserveNative(Native(1,wanted),11);
    auto after=OpticsSource(2,1,wanted.width,wanted.height,20);
    c.ObserveSource(after,12);c.ObserveAccepted(Accepted(after),13);
    Check(c.Snapshot().stage==PpdPanelStage::Applied,"valid cropped eye targets never acknowledge Adapt");
    for(auto& e:after.eyes)e.outputEye=wanted;
    c.ObserveSource(after,14);Check(c.Snapshot().stage==PpdPanelStage::Error,"full-source output accepted in place of original cropped eye output");
}
static PpdSourceObservation PairedSource(uint64_t resource=1,uint64_t revision=0,uint32_t w=2048,uint32_t h=2048,uint64_t serial=10){
    auto s=Source(resource,revision,w,h,serial);s.aerV2=false;s.paired=true;
    for(unsigned eye=0;eye<2;++eye){auto& e=s.eyes[eye];e.sourceSerial=serial;e.sourceId=int64_t(serial);
        e.engineView=eye+1;e.nativeFrame=uint32_t(serial);e.nativeFrameKnown=true;}
    return s;
}
static void PairedCameraCalibration(){
    auto s=PairedSource();Check(CalibrateActualSourcePpd(s).valid,"valid same-frame camera pair rejected");
    auto wrong=s;wrong.eyes[1].nativeFrame++;Check(!CalibrateActualSourcePpd(wrong).valid,"different native frames combined");
    wrong=s;wrong.eyes[1].engineView=1;Check(!CalibrateActualSourcePpd(wrong).valid,"MAIN repeated as both eyes");
    wrong=s;wrong.eyes[1].nativeFrameKnown=false;Check(!CalibrateActualSourcePpd(wrong).valid,"unknown frame accepted");
    wrong=s;wrong.eyes[1].sourceSerial++;Check(!CalibrateActualSourcePpd(wrong).valid,"different capture serials combined");
    wrong=s;std::swap(wrong.eyes[0].engineView,wrong.eyes[1].engineView);Check(CalibrateActualSourcePpd(wrong).valid,"physical eye swap rejected");
    PpdPanelController c;c.SetAerEnabled(true,0);Check(c.ObserveSource(s,0),"paired source rejected by controller");
    Check(!c.Snapshot().calibrated,"unsubmitted pair calibrated");c.ObserveAccepted(Accepted(s),1);
    Check(c.Snapshot().calibrated,"paired acceptance not retained");
    const auto size=Start(c);c.ObserveNative(Native(1,size),12);
    auto next=PairedSource(2,1,size.width,size.height,20);c.ObserveSource(next,13);c.ObserveAccepted(Accepted(next),14);
    Check(c.Snapshot().stage==PpdPanelStage::Applied,"paired PPD never acknowledged both eyes");
}
int main(){unsigned pass=0,fail=0;const std::pair<const char*,void(*)()> tests[]={
    {"paired_camera_calibration",PairedCameraCalibration},
    {"reference_fov_math",ReferenceFovMath},{"reference_crop_apply",ReferenceCropApply},
    {"reference_runtime_fit",ReferenceRuntimeFit},
    {"ray_derivative",ExactRayMetric},{"asymmetric_crop",AsymmetricCentersAndCrop},{"unequal_axes",UnequalAxesAndEyeDensity},{"unknown_geometry",UnprovenGeometryRejected},
    {"accepted_calibration",AcceptanceRequiredForCalibration},{"request_not_applied",RequestedIsNotApplied},{"full_ack",FullAcknowledgment},{"new_epoch",SameEpochOrOldSerialNotNewResize},
    {"exact_identity",WrongIdentityAndFailedSubmission},{"revision_race",RevisionsNeverMix},{"world_mode_reset",WorldResetAndModeOff},{"timeout",TimeoutIsSticky},
    {"native_failure",NativeFailureNoAcceptance},{"bad_request",InvalidTargetsAndRequestFailure},{"quantized_noop",SameQuantizedSizeIsNotRequested},
    {"old_output",OldOutputCannotApplyNewSource},{"mask_epochs",PartialMasksNeverCrossEpoch},{"bounded_history",EvictedObservationCannotAcknowledge},
    {"source_mutation_clock",MutatedSourceAndReversedClock},{"stale_world",StaleWorldDoesNotRevokeCurrent},
    {"applied_invalidates",AppliedWithdrawsOnOutputOrOriginChange},{"native_actual_fields",NativeObservedFieldsMustMatch}};
    for(const auto& [name,test]:tests)try{test();++pass;std::cout<<"PASS "<<name<<'\n';}catch(const std::exception& e){++fail;std::cout<<"FAIL "<<name<<": "<<e.what()<<'\n';}
    std::cout<<"RESULT "<<pass<<" passed, "<<fail<<" failed\n";return fail?1:0;}
