#pragma once
#include "Overlay/ReferenceOptics.hpp"
#include "Overlay/PpdNativeApply.hpp"
#include <array>
#include <cstdint>

namespace cvr::ppd {
struct PpdSourceScope {
    uint64_t resourceEpoch{},rendererEpoch{},gameWorldEpoch{},hmdWorldEpoch{},modeGeneration{},sourceGeneration{};
    friend bool operator==(const PpdSourceScope&,const PpdSourceScope&)=default;
};
struct PpdSourceEye {
    NativeSize mainSource{},srInput{},outputEye{};
    PpdRenderedFov renderedFov; // Actual native SL source projection, never raw HMD FOV.
    std::array<float,4> crop{}; // Source pixel coordinates L,T,R,B.
    int64_t sourceId=-1;
    uint64_t sourceSerial{};
    bool actualMainDescriptor{},nativeSlProjection{},srInputKnown{},actualOutputDescriptor{};
    // Applied runtime optics captured with this source, not a later HMD query.
    PpdRenderedFov runtimeFov;
    bool referenceOptics{};
    uint32_t engineView{},nativeFrame{};
    bool nativeFrameKnown{};
    friend bool operator==(const PpdSourceEye&,const PpdSourceEye&)=default;
};
struct PpdSourceObservation {
    PpdSourceScope scope;
    uint64_t nativeRevision{}; // The request associated with these sources; 0 before live PPD.
    std::array<PpdSourceEye,2> eyes;
    bool aerV2{};
    bool paired{}; // Distinct native views of the same rendered frame.
    friend bool operator==(const PpdSourceObservation&,const PpdSourceObservation&)=default;
};
struct PpdAcceptedObservation {
    PpdSourceScope scope;
    uint64_t nativeRevision{};
    std::array<int64_t,2> sourceIds{-1,-1};
    std::array<uint64_t,2> sourceSerials{};
    uint32_t acceptedEyeMask{};
    bool xrEndFrameAccepted{};
    std::array<NativeSize,2> outputEyeSizes{}; // Actual targets of this accepted submission.
};
enum class PpdPanelStage {WaitingSource,Ready,Requested,WaitingNative,WaitingEyes,Applied,Error};
enum class PpdPanelError {None,UnsupportedMode,InvalidSource,ContextChanged,RequestRejected,NativeFailure,RevisionChanged,Timeout,ClockChanged,SourceChanged};
struct PpdPanelSnapshot {
    PpdPanelStage stage=PpdPanelStage::WaitingSource;
    PpdPanelError error=PpdPanelError::None;
    bool aerV2{},calibrated{},canAdapt{},sameQuantizedSize{};
    float target=20,requestedTarget{},appliedTarget{},appliedMeasured{};
    PpdCalibration calibration;
    Resolution preview;
    uint64_t nativeRevision{};
    NativeSize requested{},nativeObserved{};
    PpdSourceObservation measuredSource;
    uint32_t acceptedEyeMask{};
};

// Central angular density from actual native perspective rays. Crop validity is
// checked, but cropping does not change the derivative at an included optical
// axis. Opposite asymmetric centers are not replaced with a fictitious common one.
inline bool ValidPpdSize(NativeSize s){return s.width && s.height && s.width<=16384 && s.height<=16384;}
inline bool PpdOutputMatches(const PpdSourceEye& e){
    if(!e.referenceOptics)return e.outputEye==e.mainSource;
    const float w=e.crop[2]-e.crop[0],h=e.crop[3]-e.crop[1];
    return std::isfinite(w)&&std::isfinite(h)&&w>=1&&h>=1&&w<=16384&&h<=16384&&
        w==std::floor(w)&&h==std::floor(h)&&e.outputEye==NativeSize{uint32_t(w),uint32_t(h)};
}
inline bool ValidPpdScope(const PpdSourceScope& s){return s.resourceEpoch && s.rendererEpoch && s.gameWorldEpoch && s.hmdWorldEpoch && s.modeGeneration && s.sourceGeneration;}
inline PpdCalibration CalibrateActualSourcePpd(const PpdSourceObservation& s){
    if((!s.aerV2&&!s.paired) || (s.aerV2&&s.paired) || !ValidPpdScope(s.scope) || s.eyes[0].mainSource!=s.eyes[1].mainSource)return {};
    if(s.paired){const auto& a=s.eyes[0];const auto& b=s.eyes[1];
        if(a.sourceSerial!=b.sourceSerial||!a.nativeFrameKnown||!b.nativeFrameKnown||a.nativeFrame!=b.nativeFrame||
           a.sourceId!=int64_t(a.nativeFrame)||b.sourceId!=int64_t(b.nativeFrame)||
           a.engineView<1||a.engineView>2||b.engineView<1||b.engineView>2||a.engineView==b.engineView)return {};
    }else if(s.eyes[0].sourceSerial==s.eyes[1].sourceSerial)return {};
    float focal=INFINITY,maxHorizontal=0,maxVertical=0;
    if(s.eyes[0].referenceOptics!=s.eyes[1].referenceOptics)return {};
    for(unsigned i=0;i<2;++i){
        const auto& e=s.eyes[i];const auto& f=e.renderedFov;
        if(!e.actualMainDescriptor || !e.nativeSlProjection || !e.srInputKnown || !e.actualOutputDescriptor ||
           !ValidPpdSize(e.mainSource) || !ValidPpdSize(e.srInput) || !ValidPpdSize(e.outputEye) ||
           e.sourceId<0 || (!s.paired&&(uint64_t(e.sourceId)&1)!=i) || !e.sourceSerial)return {};
        for(float a:{f.angleLeft,f.angleRight,f.angleUp,f.angleDown})
            if(!std::isfinite(a) || a<=-1.57079632679f || a>=1.57079632679f)return {};
        TangentExtent t{std::tan(f.angleUp),-std::tan(f.angleDown),-std::tan(f.angleLeft),std::tan(f.angleRight)};
        for(float v:{t.up,t.down,t.left,t.right})if(!std::isfinite(v)||v<=0)return {};
        const float horizontal=t.left+t.right,vertical=t.up+t.down;
        const SourceProjectionRegion center{t.left/horizontal,t.up/vertical,0,0};
        const float w=float(e.mainSource.width),h=float(e.mainSource.height),ox=w*center.centerX,oy=h*center.centerY;
        const auto& c=e.crop;for(float v:c)if(!std::isfinite(v))return {};
        if(c[0]<0 || c[1]<0 || c[2]>w || c[3]>h || c[0]>=ox || c[2]<=ox || c[1]>=oy || c[3]<=oy)return {};
        const auto eye=CalibratePpd(int(e.mainSource.width),int(e.mainSource.height),center,{t,t});
        if(!eye.valid)return {};
        focal=std::min(focal,eye.pixelsPerTangent);
        maxHorizontal=std::max(maxHorizontal,horizontal);maxVertical=std::max(maxVertical,vertical);
    }
    if(s.eyes[0].referenceOptics){
        const auto& a=s.eyes[0];const auto& b=s.eyes[1];
        if(!ValidReferenceFov(a.runtimeFov)||!ValidReferenceFov(b.runtimeFov))return {};
        const auto c=CalibratePpd(int(a.mainSource.width),int(a.mainSource.height),{},
            {FovTangents(a.runtimeFov),FovTangents(b.runtimeFov)});
        if(!c.valid||std::abs(c.pixelsPerTangent-focal)>c.pixelsPerTangent*.0001f)return {};
        const auto expected=ReferenceSourceFov(a.mainSource.width,a.mainSource.height,c);
        for(const auto& e:s.eyes){
            const auto& f=e.renderedFov;
            if(std::abs(f.angleLeft-expected.angleLeft)>.00002f||std::abs(f.angleRight-expected.angleRight)>.00002f||
               std::abs(f.angleUp-expected.angleUp)>.00002f||std::abs(f.angleDown-expected.angleDown)>.00002f||
               e.crop!=ReferenceEyeCrop(e.mainSource.width,e.mainSource.height,c,e.runtimeFov))return {};
        }
        return c;
    }
    const float ppd=focal*PpdRadiansPerDegree,w=focal*maxHorizontal,h=focal*maxVertical;
    if(!std::isfinite(ppd)||!std::isfinite(w)||!std::isfinite(h)||ppd<=0||w<=0||h<=0)return {};
    return {ppd,w,h,focal,true};
}

// Pure metadata controller; all methods are serialized by its external owner.
// No engine/GPU pointers, XR calls, persistence, worker, or timing thread.
class PpdPanelController {
public:
    static constexpr uint64_t ApplyTimeoutMs=15000;
    void SetAerEnabled(bool enabled,uint64_t now){
        if(state_.aerV2!=enabled){state_.aerV2=enabled;if(!enabled){InvalidateContext();if(transaction_.active)Fail(PpdPanelError::UnsupportedMode);}}
        Refresh(now);
    }
    bool SetTarget(float value){
        if(!std::isfinite(value)||value<5||value>40)return false;
        state_.target=value;targetEdited_=true;return true;
    }
    bool ObserveSource(const PpdSourceObservation& source,uint64_t now){
        Refresh(now);
        if(!state_.aerV2 || !CalibrateActualSourcePpd(source).valid)return false;
        const auto& scope=source.scope;
        if(knownWorld_ && (scope.gameWorldEpoch<knownWorld_ ||
           (scope.gameWorldEpoch==knownWorld_ && scope.hmdWorldEpoch<knownHmd_)))return false;
        if(knownWorld_ && (scope.gameWorldEpoch!=knownWorld_ || scope.hmdWorldEpoch!=knownHmd_)){
            ClearSources();if(transaction_.active)Fail(PpdPanelError::ContextChanged);
        }
        knownWorld_=scope.gameWorldEpoch;knownHmd_=scope.hmdWorldEpoch;
        if(latestScope_.modeGeneration && (scope.modeGeneration<latestScope_.modeGeneration ||
           (scope.modeGeneration==latestScope_.modeGeneration && scope.resourceEpoch<latestScope_.resourceEpoch)))return false;
        const bool wantedLayout=source.eyes[0].mainSource==transaction_.wanted && source.eyes[1].mainSource==transaction_.wanted &&
            PpdOutputMatches(source.eyes[0]) && PpdOutputMatches(source.eyes[1]);
        if(state_.stage==PpdPanelStage::Applied && (scope!=transaction_.candidateScope || !wantedLayout))Fail(PpdPanelError::SourceChanged);
        latestScope_=scope;
        for(const auto& entry:history_)if(entry.valid && entry.source.scope==scope && entry.source.nativeRevision==source.nativeRevision){
            for(unsigned eye=0;eye<2;++eye)if(entry.source.eyes[eye].sourceId==source.eyes[eye].sourceId &&
                entry.source.eyes[eye].sourceSerial==source.eyes[eye].sourceSerial && !SameCapturedEye(entry.source.eyes[eye],source.eyes[eye])){
                    if(transaction_.active)Fail(PpdPanelError::InvalidSource);return false;
                }
        }
        history_[next_]={source,true};next_=(next_+1)%history_.size();
        if(transaction_.sourceSeen && source.nativeRevision==transaction_.revision &&
           scope==transaction_.candidateScope && !wantedLayout){
            transaction_.sourceSeen=false;transaction_.accepted={};transaction_.mask=0;
        }
        if(Qualifies(source)){
            if(!transaction_.sourceSeen || transaction_.candidateScope!=scope){transaction_.accepted={};transaction_.mask=0;}
            transaction_.sourceSeen=true;transaction_.candidateScope=scope;
        }
        Refresh(now);return true;
    }
    void ObserveNative(const NativeApplyStatus& native,uint64_t now){native_=native;Refresh(now);}
    bool ObserveAccepted(const PpdAcceptedObservation& accepted,uint64_t now){
        Refresh(now);
        if(!state_.aerV2 || !accepted.xrEndFrameAccepted || !accepted.acceptedEyeMask ||
           (accepted.acceptedEyeMask&~3u) || accepted.scope!=latestScope_)return false;
        std::array<AcceptedEye,2> found{};
        for(unsigned eye=0;eye<2;++eye)if(accepted.acceptedEyeMask&(1u<<eye)){
            for(const auto& entry:history_)if(entry.valid && entry.source.scope==accepted.scope &&
                entry.source.nativeRevision==accepted.nativeRevision && entry.source.eyes[eye].sourceId==accepted.sourceIds[eye] &&
                entry.source.eyes[eye].sourceSerial==accepted.sourceSerials[eye] && entry.source.eyes[eye].outputEye==accepted.outputEyeSizes[eye]){
                    found[eye]={entry.source.eyes[eye],accepted.scope,accepted.nativeRevision,true,entry.source.paired};break;}
            if(!found[eye].valid)return false;
        }
        for(unsigned eye=0;eye<2;++eye)if(found[eye].valid){
            const unsigned other=eye^1;
            if(accepted_[other].valid && (accepted_[other].scope!=accepted.scope || accepted_[other].revision!=accepted.nativeRevision || accepted_[other].paired!=found[eye].paired))accepted_[other]={};
            accepted_[eye]=found[eye];
            if(transaction_.sourceSeen && accepted.scope==transaction_.candidateScope && accepted.nativeRevision==transaction_.revision &&
               NewEye(found[eye].eye,eye)){
                transaction_.accepted[eye]=found[eye];transaction_.mask|=1u<<eye;
            }
        }
        if(accepted_[0].valid && accepted_[1].valid){
            const auto measured=Pair(accepted_);const auto calibration=CalibrateActualSourcePpd(measured);
            if(calibration.valid){
                state_.calibrated=true;state_.calibration=calibration;state_.measuredSource=measured;
                if(!targetEdited_)state_.target=std::clamp(calibration.ppd,5.f,40.f);
                if(!transaction_.active)state_.stage=PpdPanelStage::Ready;
            }
        }
        Refresh(now);return true;
    }
    void InvalidateContext(){
        ClearSources();knownWorld_=knownHmd_=0;
        if(transaction_.active)Fail(PpdPanelError::ContextChanged);else state_.stage=PpdPanelStage::WaitingSource;
    }
    template<class Request> bool Apply(uint64_t now,Request&& request){
        const auto view=Snapshot();
        if(!view.canAdapt){if(!state_.aerV2)Fail(PpdPanelError::UnsupportedMode);return false;}
        const NativeSize wanted{uint32_t(view.preview.width),uint32_t(view.preview.height)};
        const uint64_t revision=request(wanted.width,wanted.height);
        if(!revision){Fail(PpdPanelError::RequestRejected);return false;}
        transaction_={};transaction_.active=true;transaction_.revision=revision;transaction_.wanted=wanted;transaction_.started=now;
        transaction_.baseline=state_.measuredSource;transaction_.target=state_.target;
        transaction_.needsNewResource=wanted!=state_.measuredSource.eyes[0].mainSource;
        state_.nativeRevision=revision;state_.requested=wanted;state_.requestedTarget=state_.target;
        state_.stage=PpdPanelStage::Requested;state_.error=PpdPanelError::None;state_.acceptedEyeMask=0;
        return true;
    }
    PpdPanelSnapshot Snapshot()const{
        auto value=state_;
        value.preview=PreviewCalibratedPpd(value.calibration,value.target,{.1f,10,-1,false});
        value.sameQuantizedSize=value.calibrated && value.preview.valid &&
            value.measuredSource.eyes[0].mainSource==NativeSize{uint32_t(value.preview.width),uint32_t(value.preview.height)};
        value.canAdapt=value.aerV2 && value.calibrated && value.preview.valid && !value.sameQuantizedSize;
        return value;
    }
private:
    struct Entry {PpdSourceObservation source;bool valid{};};
    struct AcceptedEye {PpdSourceEye eye;PpdSourceScope scope;uint64_t revision{};bool valid{},paired{};};
    struct Transaction {
        bool active{},failed{},sourceSeen{},needsNewResource{};
        uint64_t revision{},started{};NativeSize wanted{};float target{};
        PpdSourceObservation baseline;PpdSourceScope candidateScope;
        std::array<AcceptedEye,2> accepted{};uint32_t mask{};
    } transaction_;
    static PpdSourceObservation Pair(const std::array<AcceptedEye,2>& eyes){
        if(eyes[0].paired!=eyes[1].paired)return {};
        return {eyes[0].scope,eyes[0].revision,{eyes[0].eye,eyes[1].eye},!eyes[0].paired,eyes[0].paired};
    }
    static bool SameCapturedEye(PpdSourceEye a,PpdSourceEye b){
        // Output targets can be recreated while the same immutable source is
        // held. Accepted records identify the actual output dimensions too.
        a.outputEye=b.outputEye={};a.actualOutputDescriptor=b.actualOutputDescriptor=false;
        return a==b;
    }
    void ClearSources(){
        history_={};accepted_={};latestScope_={};next_=0;
        state_.calibrated=false;state_.calibration={};state_.measuredSource={};state_.acceptedEyeMask=0;
        transaction_.sourceSeen=false;transaction_.accepted={};transaction_.mask=0;
    }
    void Fail(PpdPanelError error){
        state_.stage=PpdPanelStage::Error;state_.error=error;state_.acceptedEyeMask=0;transaction_.failed=true;
    }
    bool NewEye(const PpdSourceEye& eye,unsigned index)const{
        // The original eye crop is independent of the native game dimensions.
        // Both descriptors still require exact source identity and XR acceptance.
        return eye.mainSource==transaction_.wanted && PpdOutputMatches(eye) &&
            eye.sourceSerial>transaction_.baseline.eyes[index].sourceSerial;
    }
    bool Qualifies(const PpdSourceObservation& source)const{
        if(!transaction_.active || transaction_.failed || source.nativeRevision!=transaction_.revision)return false;
        const auto& a=source.scope;const auto& b=transaction_.baseline.scope;
        if(a.gameWorldEpoch!=b.gameWorldEpoch || a.hmdWorldEpoch!=b.hmdWorldEpoch || a.rendererEpoch<b.rendererEpoch ||
           a.modeGeneration<b.modeGeneration || a.resourceEpoch<b.resourceEpoch ||
           (transaction_.needsNewResource && a.resourceEpoch==b.resourceEpoch))return false;
        return NewEye(source.eyes[0],0) && NewEye(source.eyes[1],1);
    }
    void Refresh(uint64_t now){
        if(!transaction_.active || transaction_.failed)return;
        if(now<transaction_.started){Fail(PpdPanelError::ClockChanged);return;}
        if(state_.stage!=PpdPanelStage::Applied && now-transaction_.started>=ApplyTimeoutMs){Fail(PpdPanelError::Timeout);return;}
        // A just-created request is polled on the next owner callback. A prior
        // cached native sample cannot be mistaken for the new revision here.
        if(!native_.revision || native_.revision<transaction_.revision)return;
        if(native_.revision!=transaction_.revision || native_.stage==NativeApplyStage::Idle){Fail(PpdPanelError::RevisionChanged);return;}
        if(native_.stage==NativeApplyStage::Error){Fail(PpdPanelError::NativeFailure);return;}
        state_.nativeObserved=native_.observedActive;
        const bool nativeReady=native_.stage==NativeApplyStage::ObservedNative && native_.requested==transaction_.wanted &&
            native_.observedActive==transaction_.wanted && native_.observedPending==transaction_.wanted &&
            native_.dispatchVisit && native_.observationVisit>native_.dispatchVisit;
        if(!nativeReady){state_.stage=PpdPanelStage::WaitingNative;return;}
        if(!transaction_.sourceSeen){state_.stage=PpdPanelStage::WaitingSource;return;}
        state_.acceptedEyeMask=transaction_.mask;
        if(transaction_.mask!=3){state_.stage=PpdPanelStage::WaitingEyes;return;}
        const auto calibration=CalibrateActualSourcePpd(Pair(transaction_.accepted));
        if(!calibration.valid){Fail(PpdPanelError::InvalidSource);return;}
        state_.stage=PpdPanelStage::Applied;state_.appliedTarget=transaction_.target;state_.appliedMeasured=calibration.ppd;
    }
    PpdPanelSnapshot state_;
    NativeApplyStatus native_;
    std::array<Entry,8> history_{};std::size_t next_{};
    std::array<AcceptedEye,2> accepted_{};PpdSourceScope latestScope_;
    uint64_t knownWorld_{},knownHmd_{};bool targetEdited_{};
};

// Root provides validated actual-source metadata before submission, then calls
// Accepted only after successful XR acceptance. Readiness/eye identity remain
// the caller's existing GPU contract; this panel cannot replace those checks.
void ObservePpdSource(const PpdSourceObservation&);
void ObservePpdAccepted(const PpdAcceptedObservation&);
void InvalidatePpdSourceContext();
PpdPanelSnapshot ReadPpdPanelSnapshot();
void DrawPpdPanel();
}
