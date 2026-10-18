#pragma once
#include "Framegen/MainEyeSourceLatch.hpp"
#include "Framegen/SourceSceneEpoch.hpp"
#include <utility>

namespace cvr::framegen {
struct AerRuntimeSnapshot {
    bool enabled{},gameplay{};
    uint64_t rendererEpoch{},hmdWorldEpoch{};
    int64_t predictedTimeNs{};
};
struct AerSceneDecision {
    bool active{},invalidate{},ignored{};
    uint64_t generation{};
    MainEyeSourceEpochs epochs;
    MainEyeSceneFrame frame;
    int64_t sourceId=-1;
    AerV2Eye eye=AerV2Eye::Left;
    MainEyeClockSnapshot clock;
};
// Metadata only, externally serialized by the mode owner. Actual SDK FrameBegin
// is the only source of scene ordinals. Disable confirmation is an explicit
// component-readback token, never a desired setting or the absence of node hits.
class AerScenePolicy {
public:
    void UpdateRuntime(AerRuntimeSnapshot value) {
        if(value.enabled!=runtime.enabled || value.gameplay!=runtime.gameplay ||
           value.rendererEpoch!=runtime.rendererEpoch || value.hmdWorldEpoch!=runtime.hmdWorldEpoch)Reset();
        runtime=value;
    }
    AerSceneDecision OnSceneFrame(const SourceSceneFrame& value) {
        AerSceneDecision out;
        if(value.worldEpoch<scene.worldEpoch ||
           (value.worldEpoch==scene.worldEpoch && value.ordinal<scene.ordinal)) {out.ignored=true;return out;}
        const bool changed=value.worldEpoch!=scene.worldEpoch || value.scene!=scene.scene ||
            value.valid!=scene.valid || value.paused!=scene.paused;
        if(changed)Reset();
        const bool newer=value.worldEpoch!=scene.worldEpoch || value.ordinal>scene.ordinal;
        scene=value;
        out.invalidate=std::exchange(invalidate,false);
        out.generation=generation;out.epochs=Epochs();
        out.frame={scene.worldEpoch,scene.ordinal,runtime.rendererEpoch,runtime.hmdWorldEpoch,Ready()};
        if(!newer) {out.ignored=!changed && !out.invalidate;return out;}
        if(!Ready() || !confirmed || runtime.predictedTimeNs<=0 ||
           sourceId>SourceDisplayClock::MaxExactSourceId)return out;
        out.active=true;out.sourceId=sourceId++;out.eye=AerV2Eye(uint64_t(out.sourceId)&1u);
        out.clock={out.epochs,clock.Generation(),clock.Fit(),runtime.predictedTimeNs};
        return out;
    }
    uint32_t OffRequestToken() const {return Ready()?token:0;}
    bool ReportVrcamOff(uint32_t reportedToken,uint32_t inspected,bool allDisabled) {
        if(!Ready() || !reportedToken || reportedToken!=token)return false;
        const bool next=inspected>0 && allDisabled;
        if(confirmed && !next) {Reset();return false;}
        confirmed=next;
        if(!confirmed)invalidate=true;
        return confirmed;
    }
    void ObserveAccepted(uint64_t reportedGeneration,MainEyeSourceEpochs epochs,
                         const SourceDisplaySubmission& submission) {
        if(Ready() && confirmed && reportedGeneration==generation && epochs==Epochs())
            clock.ObserveAccepted(submission);
    }
    MainEyeSourceEpochs Epochs() const {return {runtime.rendererEpoch,scene.worldEpoch,runtime.hmdWorldEpoch};}
    void ObserveAcceptedContinuous(uint64_t reportedGeneration,MainEyeSourceEpochs epochs,
                                   const SourceDisplayContinuousSubmission& submission) {
        if(Ready() && confirmed && reportedGeneration==generation && epochs==Epochs())
            clock.ObserveAcceptedContinuous(submission);
    }
    uint64_t Generation() const {return generation;}
    SourceDisplayFit Fit() const {return clock.Fit();}
    bool Requested() const {return runtime.enabled;}
    bool Ready() const {
        return !exhausted && runtime.enabled && runtime.gameplay && scene.valid && !scene.paused &&
            scene.scene && scene.ordinal && Epochs().Valid();
    }
private:
    void Reset() {
        confirmed=false;invalidate=true;sourceId=0;clock.Reset();
        // Int32 tokens round-trip through the script VM without a floating-point
        // uint64 conversion. Exhaustion is an explicit fail-closed boundary.
        if(token==INT32_MAX || generation==(std::numeric_limits<uint64_t>::max)())exhausted=true;
        else {++token;++generation;}
    }
    AerRuntimeSnapshot runtime;
    SourceSceneFrame scene;
    SourceDisplayClock clock;
    uint64_t generation=1;
    uint32_t token=1;
    int64_t sourceId{};
    bool confirmed{},invalidate=true,exhausted{};
};
}
