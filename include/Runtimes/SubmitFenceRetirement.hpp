#pragma once
#include <cstdint>
#include <d3d12.h>
#include <wrl/client.h>

namespace cvr::capture {
inline bool SubmitFenceValueComplete(uint64_t completed,uint64_t required) {
    return completed!=UINT64_MAX && required!=UINT64_MAX && completed>=required;
}

// Snapshot the exact completion owner before a capture read leaves the caller.
// A replacement fence's numeric value is never evidence about this point.
struct SubmitReadCompletion {
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    uint64_t value{};
    bool knownComplete{},unknown{};
    bool Complete()const {
        if(knownComplete)return true;
        return !unknown && fence && value &&
            SubmitFenceValueComplete(fence->GetCompletedValue(),value);
    }
};

// Access under the capture owner's publication mutex. The XR submission scalar
// stays monotonic across fence recreation so existing ReadLease callbacks need
// no ABI change. Only actual completed GPU work advances completedThrough_.
class SubmitFenceRetirement {
public:
    void Executed(ID3D12Fence* point,uint64_t at,bool signalSucceeded) {
        if(!point || !signalSucceeded || !at || at==UINT64_MAX || at<=lastValue_ ||
           (fence_ && fence_.Get()!=point && completedThrough_<lastValue_))unknown_=true;
        fence_=point;submitted_=true;
        if(at!=UINT64_MAX && at>lastValue_)lastValue_=at;
    }
    bool PollCompleted() {
        if(unknown_)return false;
        if(!submitted_ || completedThrough_>=lastValue_)return true;
        if(!fence_)return false;
        const auto done=fence_->GetCompletedValue();
        if(done==UINT64_MAX){unknown_=true;return false;}
        if(done>completedThrough_)completedThrough_=done<lastValue_?done:lastValue_;
        return completedThrough_>=lastValue_;
    }
    bool CanRetire(uint64_t lastValue) {
        return PollCompleted() && completedThrough_==lastValue;
    }
    bool RetireFence(uint64_t lastValue) {
        if(!CanRetire(lastValue))return false;
        fence_.Reset();submitted_=false;return true;
    }
    bool Unknown()const {return unknown_;}
    uint64_t CompletedThrough()const {return completedThrough_;}
    SubmitReadCompletion ResolveRead(uint64_t value)const {
        // Zero belongs to an independently completed owner (for example the
        // worker's private-copy fence); it never means a pending XR submission.
        if(!value || (value!=UINT64_MAX && value<=completedThrough_))return {{},value,true,false};
        return {fence_,value,false,unknown_ || !submitted_ || !fence_ ||
            value==UINT64_MAX || value>lastValue_};
    }
private:
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    uint64_t lastValue_{},completedThrough_{};
    bool submitted_{},unknown_{};
};
}
