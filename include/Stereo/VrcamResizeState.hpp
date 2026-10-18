#pragma once
#include <cstdint>
#include <mutex>

namespace cvr::ppd {
struct VrcamResizeExtent {
    uint32_t width{},height{};
    bool operator==(const VrcamResizeExtent&) const=default;
};
struct VrcamResizeLayout {
    VrcamResizeExtent mainSource{},vrcamSource{},eyes[2]{};
};
enum class VrcamResizePhase {Idle,Requested,Dispatching,Submitted,EngineApplied,ReadyForPresentation,Applied,Error};
enum class VrcamResizeError {None,UnsupportedResource,UnsupportedExecutable,EngineException,TimedOut};
struct VrcamResizeRequest {
    uint64_t id{},worldEpoch{},cameraKey{},baselineResourceEpoch{},baselineSourceSerial{};
    VrcamResizeLayout expected{};
};
struct VrcamResizeObservation {
    uint64_t requestId{},worldEpoch{},cameraKey{},resourceEpoch{},sourceSerial{};
    VrcamResizeExtent mainSource{},vrcamSource{},eyes[2]{};
    uint32_t acceptedEyeMask{};
    bool producerReady{};
};
struct VrcamResizeSnapshot {
    VrcamResizeRequest request{};
    VrcamResizePhase phase=VrcamResizePhase::Idle;
    VrcamResizeError error=VrcamResizeError::None;
    uint64_t appliedResourceEpoch{},appliedSourceSerial{};
};
class VrcamResizeState {
public:
    // The mode owner supplies a generation of its complete stable world owner,
    // not merely tracking-origin serial. Zero means menu/loading/owner unknown.
    // No engine component/resource pointer is retained across callbacks.
    void UpdateWorld(uint64_t epoch) {
        std::lock_guard lock(mutex_);
        if(epoch==worldEpoch_)return;
        worldEpoch_=epoch;
        snapshot_={};
    }
    uint64_t Request(VrcamResizeLayout expected,uint64_t worldEpoch,uint64_t cameraKey,
                     uint64_t baselineResourceEpoch,uint64_t baselineSourceSerial) {
        if(!Valid(expected.mainSource)||!Valid(expected.vrcamSource)||
           !Valid(expected.eyes[0])||!Valid(expected.eyes[1])||
           expected.vrcamSource.width%16||expected.vrcamSource.height%16||
           !worldEpoch||!cameraKey||!baselineResourceEpoch||!baselineSourceSerial)return 0;
        std::lock_guard lock(mutex_);
        if(worldEpoch!=worldEpoch_)return 0;
        if(++nextRequest_==0)++nextRequest_;
        snapshot_={};
        snapshot_.request={nextRequest_,worldEpoch,cameraKey,baselineResourceEpoch,baselineSourceSerial,expected};
        snapshot_.phase=VrcamResizePhase::Requested;
        return nextRequest_;
    }
    VrcamResizeRequest BeginDispatch(uint64_t cameraKey) {
        std::lock_guard lock(mutex_);
        if(snapshot_.phase!=VrcamResizePhase::Requested||!Current(snapshot_.request.id)||
           snapshot_.request.cameraKey!=cameraKey)return {};
        snapshot_.phase=VrcamResizePhase::Dispatching;
        return snapshot_.request;
    }
    // The native setter is void and marshals resource work. Its return is only
    // dispatch evidence; only later ready capture/presentation can apply.
    bool MarkDispatched(uint64_t id) {
        std::lock_guard lock(mutex_);
        if(!Current(id)||snapshot_.phase!=VrcamResizePhase::Dispatching)return false;
        snapshot_.phase=VrcamResizePhase::Submitted;
        return true;
    }
    bool ObserveActual(const VrcamResizeObservation& actual) {
        std::lock_guard lock(mutex_);
        if(!Current(actual.requestId)||actual.worldEpoch!=worldEpoch_||
           actual.cameraKey!=snapshot_.request.cameraKey||
           (snapshot_.phase!=VrcamResizePhase::Submitted &&
            snapshot_.phase!=VrcamResizePhase::EngineApplied &&
            snapshot_.phase!=VrcamResizePhase::ReadyForPresentation &&
            snapshot_.phase!=VrcamResizePhase::Applied)||
           actual.resourceEpoch<=snapshot_.request.baselineResourceEpoch||
           actual.sourceSerial<=snapshot_.request.baselineSourceSerial)return false;
        const auto& expected=snapshot_.request.expected;
        if(actual.vrcamSource!=expected.vrcamSource)return false;
        snapshot_.phase=VrcamResizePhase::EngineApplied;
        if(!actual.producerReady||actual.mainSource!=expected.mainSource||
           actual.eyes[0]!=expected.eyes[0]||actual.eyes[1]!=expected.eyes[1])return false;
        snapshot_.phase=VrcamResizePhase::ReadyForPresentation;
        // This mask belongs to this one request/resourceEpoch/sourceSerial.
        // Never accumulate separate eyes from unrelated frames/epochs.
        if((actual.acceptedEyeMask&3)!=3)return false;
        snapshot_.phase=VrcamResizePhase::Applied;
        snapshot_.appliedResourceEpoch=actual.resourceEpoch;
        snapshot_.appliedSourceSerial=actual.sourceSerial;
        return true;
    }
    bool Fail(uint64_t id,VrcamResizeError error) {
        std::lock_guard lock(mutex_);
        if(!Current(id)||error==VrcamResizeError::None||snapshot_.phase==VrcamResizePhase::Applied)return false;
        snapshot_.phase=VrcamResizePhase::Error;
        snapshot_.error=error;
        return true;
    }
    VrcamResizeSnapshot Read() const {
        std::lock_guard lock(mutex_);
        return snapshot_;
    }
private:
    static bool Valid(VrcamResizeExtent size) {
        return size.width && size.height && size.width<=16384 && size.height<=16384;
    }
    bool Current(uint64_t id) const {
        return id && id==snapshot_.request.id && worldEpoch_ && snapshot_.request.worldEpoch==worldEpoch_;
    }
    mutable std::mutex mutex_;
    uint64_t worldEpoch_{},nextRequest_{};
    VrcamResizeSnapshot snapshot_{};
};
// Shared only by the current plugin's request owner, RTT driver, and accepted
// presentation observer. Default idle leaves ordinary ToFu behavior unchanged.
inline VrcamResizeState g_vrcamResizeState;
struct VrcamResizeDriverView {
    uint64_t cameraKey{};
    void* resource{};
    bool scaleToViewport{},apiSupported{};
};
using VrcamViewportSetter=bool(*)(void*,uint32_t,uint32_t);
// The adapter's bool means the pinned void function was called without a
// guarded exception. It is not an engine return value or a completion signal.
inline bool DispatchVrcamResize(VrcamResizeState& state,VrcamResizeDriverView view,VrcamViewportSetter setter) {
    if(!view.resource)return false;
    const auto work=state.BeginDispatch(view.cameraKey);
    if(!work.id)return false;
    if(!view.apiSupported||!setter){state.Fail(work.id,VrcamResizeError::UnsupportedExecutable);return false;}
    if(!view.scaleToViewport){state.Fail(work.id,VrcamResizeError::UnsupportedResource);return false;}
    if(!setter(view.resource,work.expected.vrcamSource.width,work.expected.vrcamSource.height)){
        state.Fail(work.id,VrcamResizeError::EngineException);return false;
    }
    return state.MarkDispatched(work.id);
}
}
