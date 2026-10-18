#pragma once
#include "Framegen/AerV2Policy.hpp"
#include "Framegen/SourceDisplayClock.hpp"
namespace cvr::framegen {
// Different owners; none is interchangeable with a native renderer frame ID.
struct MainEyeSourceEpochs {
    uint64_t renderer{},gameWorld{},hmdWorld{};
    bool operator==(const MainEyeSourceEpochs&) const=default;
    bool Valid() const noexcept{return renderer && gameWorld && hmdWorld;}
};
// ordinal is supplied by the real scene-owned FrameBegin callback. Never use
// FrameInfo::unk10, a wall clock, pose sequence or renderer+4CA4 as that ordinal.
struct MainEyeSceneFrame {uint64_t gameWorldEpoch{},ordinal{},rendererEpoch{},hmdWorldEpoch{};bool gameplayValid{};};
struct VrcamOffProof {uint64_t requestRevision{},gameWorldEpoch{};bool confirmed{};};
// Snapshot by VALUE, taken by the clock owner under its existing lock. Readers
// never access/mutate the live SourceDisplayClock from the camera/game thread.
struct MainEyeClockSnapshot {MainEyeSourceEpochs epochs;uint64_t clockGeneration{};SourceDisplayFit fit;int64_t runtimePredictedTimeNs{};};
struct MainEyeSourceReceipt {
    uint64_t requestRevision{},sceneOrdinal{},latchRevision{};MainEyeSourceEpochs epochs;
    int64_t sourceId=-1;AerV2Eye physicalEye=AerV2Eye::Left;
    uint64_t clockGeneration{};int64_t requestedPoseTimeNs{};bool poseTimeFromFit{};
    bool operator==(const MainEyeSourceReceipt&) const=default;
    explicit operator bool() const noexcept {
        return requestRevision && sceneOrdinal && latchRevision && epochs.Valid() && clockGeneration &&
            sourceId>=0 && sourceId<=SourceDisplayClock::MaxExactSourceId &&
            static_cast<uint32_t>(physicalEye)<2 && (uint64_t(sourceId)&1u)==static_cast<uint32_t>(physicalEye) && requestedPoseTimeNs>0;
    }
    bool MatchesWrittenPose(uint64_t hmdEpoch,int64_t actualPoseTimeNs) const noexcept {
        return bool(*this) && epochs.hmdWorld==hmdEpoch && requestedPoseTimeNs==actualPoseTimeNs;
    }
};
struct MainEyeSourceFaultScope {uint64_t requestRevision{},latchRevision{},gameWorldEpoch{},throughOrdinal{};};
enum class MainEyeCaptureMode {Reject,Legacy,Source};
inline MainEyeCaptureMode PlanMainEyeCapture(bool requested,bool sourcePresent,bool usable){
    // A revoked gameplay receipt may remain on an image when a menu opens.
    // OFF must preserve the ordinary mono capture, without carrying that old
    // receipt as a new AER source. Requested AER remains fail-closed.
    if(!requested)return MainEyeCaptureMode::Legacy;
    return sourcePresent && usable ? MainEyeCaptureMode::Source : MainEyeCaptureMode::Reject;
}
// Single owner / externally serialized. No hook registration, SDK frame-ID
// invention, global eye flip, GPU readiness or frame loop lives in this class.
class MainEyeSourceLatch {
public:
    uint64_t RequestRevision() const noexcept{return requestRevision;}
    uint64_t LatchRevision() const noexcept{return latchRevision;}
    uint64_t Request(bool enabled) noexcept {
        if(exhausted)return 0;
        if(enabled!=requested){
            if(!Advance(requestRevision))return 0;
            requested=enabled;Invalidate();lastSource=-1;sourceGameEpoch=0;
        }
        return requestRevision;
    }
    bool Requested() const noexcept{return requested && !exhausted;}
    bool OnSceneFrame(uint64_t revision,const MainEyeSceneFrame& frame,int64_t sourceId,AerV2Eye eye,
                      const VrcamOffProof& off,const MainEyeClockSnapshot& clock) noexcept {
        if(exhausted || revision!=requestRevision || !frame.gameWorldEpoch || !frame.ordinal)return false;
        const MainEyeSourceEpochs epochs{frame.rendererEpoch,frame.gameWorldEpoch,frame.hmdWorldEpoch};
        if(frame.gameWorldEpoch<lastGameEpoch)return false;
        if(frame.gameWorldEpoch==lastGameEpoch && frame.ordinal<lastOrdinal)return false;
        if(current && (current->epochs!=epochs || current->clockGeneration!=clock.clockGeneration))Invalidate();
        // Even OFF/unconfirmed/invalid-clock frames are observed. A request or
        // late confirmation cannot activate halfway through that same frame.
        if(frame.gameWorldEpoch==lastGameEpoch && frame.ordinal<=lastOrdinal)return false;
        lastGameEpoch=frame.gameWorldEpoch;lastOrdinal=frame.ordinal;current.reset();
        if(!Requested() || !frame.gameplayValid || !epochs.Valid() || !off.confirmed ||
           off.requestRevision!=revision || off.gameWorldEpoch!=frame.gameWorldEpoch ||
           clock.epochs!=epochs || !clock.clockGeneration || sourceId<0 ||
           sourceId>SourceDisplayClock::MaxExactSourceId || static_cast<uint32_t>(eye)>1 ||
           (uint64_t(sourceId)&1u)!=static_cast<uint32_t>(eye) ||
           (sourceGameEpoch==frame.gameWorldEpoch && sourceId<=lastSource))return false;
        int64_t target=clock.runtimePredictedTimeNs;
        if(clock.fit.valid){
            const double ns=(static_cast<double>(sourceId)*clock.fit.secondsPerSourceId+clock.fit.offsetSeconds)*1e9;
            if(!std::isfinite(ns) || ns<1.0 || ns>=0x1p63)return false;
            target=static_cast<int64_t>(ns);
        }
        if(target<=0)return false;
        current=MainEyeSourceReceipt{revision,frame.ordinal,latchRevision,epochs,sourceId,eye,
                                    clock.clockGeneration,target,clock.fit.valid};
        sourceGameEpoch=frame.gameWorldEpoch;lastSource=sourceId;
        return true;
    }
    std::optional<MainEyeSourceReceipt> ForMainWrite(uint64_t hmdEpoch) const noexcept {
        if(!Requested() || !current || current->epochs.hmdWorld!=hmdEpoch)return {};
        return current;
    }
    bool Usable(const MainEyeSourceReceipt& receipt) const noexcept {
        return Requested() && current && bool(receipt) && receipt.requestRevision==requestRevision &&
            receipt.latchRevision==latchRevision && receipt.epochs==current->epochs &&
            receipt.clockGeneration==current->clockGeneration && receipt.sceneOrdinal<=current->sceneOrdinal &&
            receipt.sourceId<=current->sourceId;
    }
    void Invalidate() noexcept {current.reset();Advance(latchRevision);}
    std::optional<MainEyeSourceFaultScope> FaultScope(uint64_t gameEpoch,uint64_t throughOrdinal) const noexcept {
        if(!Requested())return {};
        // A callback can throw after Request(true), before it publishes any
        // receipt. Revoke that pending request too, but never a newer frame.
        const auto world=current ? current->epochs.gameWorld : lastGameEpoch;
        const auto ordinal=current ? current->sceneOrdinal : lastOrdinal;
        if(world>gameEpoch || (world==gameEpoch && ordinal>throughOrdinal))return {};
        return MainEyeSourceFaultScope{requestRevision,latchRevision,gameEpoch,throughOrdinal};
    }
    bool RevokeFault(const MainEyeSourceFaultScope& scope) noexcept {
        if(scope.requestRevision!=requestRevision || scope.latchRevision!=latchRevision ||
           !FaultScope(scope.gameWorldEpoch,scope.throughOrdinal))return false;
        Request(false);return true;
    }
private:
    bool Advance(uint64_t& value) noexcept {
        if(value==(std::numeric_limits<uint64_t>::max)()){
            exhausted=true;requested=false;current.reset();return false;
        }
        ++value;return true;
    }
    bool requested{},exhausted{};
    uint64_t requestRevision=1,latchRevision=1,lastGameEpoch{},lastOrdinal{},sourceGameEpoch{};
    int64_t lastSource=-1;
    std::optional<MainEyeSourceReceipt> current;
};
}
