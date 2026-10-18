#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace cvr::framegen {
enum class AerV2Eye:uint32_t {Left=0,Right=1};
struct AerV2Request {AerV2Eye eye{};int64_t lowerSource{},upperSource{};};
// Caller attests gpuReady only after its real fence/lease/provenance checks.
// This scheduling helper has no GPU objects and cannot establish completion.
struct AerV2Admission {AerV2Eye eye{};int64_t sourceId{};bool gpuReady{};};
struct AerV2State {
    std::array<int64_t,2> sourceIds{-2,-1};
    std::array<int64_t,2> acceptedFrames{-6,-6};
    int32_t delay=1;
    int64_t lastFrame=-1;
};
enum class AerV2StepStatus {Advanced,InvalidInput};
struct AerV2Step {AerV2StepStatus status=AerV2StepStatus::InvalidInput;int32_t delayForDraw{},nextDelay{};std::array<bool,2> accepted{};};

// Single-owner source-admission policy for the separate AER v2 mode. It owns
// neither an XR loop nor image/history lifetime. IDs use this producer's
// signed, parity-labelled sequence; an unrelated stereo-pair serial is not an
// admissible replacement. F and acceptedFrames are compositor ticks, never FPS.
// Reference: Luke42DA 29F110..29F27D, consumer 29F93F..29FA58; cold anchors
// 2A1E6C and sources 29E90B/29E921. The caller still draws left then right.
class AerV2Policy {
public:
    static constexpr int64_t MaxSourceId=(std::numeric_limits<int64_t>::max)()-2;
    const AerV2State& State() const noexcept {return state;}
    void Reset() noexcept {state={};}

    // Preserve the proven first mode0 tick after a fixed 1/2..1/6 mode.
    // This does not implement those modes or invent an adaptive-delay writer.
    bool SetPriorFixedDelay(int32_t denominator) noexcept {
        if(denominator<2 || denominator>6)return false;
        state.delay=denominator;return true;
    }

    // The callback may return no ready source. Its result must retain explicit
    // eye identity and validated readiness. Invalid results never advance S/A.
    // appliedDelay is an explicit input: 0 means the reference default of1;
    // a negative value is an invalid, unproved domain, not silently clamped.
    // State().delay becomes NEXT tick's value. Both current eye draws must use
    // result.delayForDraw, preserving reference's update-after-draw ordering.
    template<class Consume>
    AerV2Step Step(int64_t frame,int32_t appliedDelay,int64_t sourceUpper,Consume&& consume) {
        AerV2Step result{};result.delayForDraw=state.delay;result.nextDelay=state.delay;
        if(frame<0 || frame<=state.lastFrame || appliedDelay<0 || sourceUpper< -1)return result;
        const auto admit=[&](AerV2Eye eye,int64_t lower,int64_t upper) {
            const AerV2Request request{eye,lower,upper};
            const std::optional<AerV2Admission> value=consume(request);
            const auto index=static_cast<std::size_t>(eye);
            if(!value || !value->gpuReady || value->eye!=eye || value->sourceId<0 ||
               value->sourceId>MaxSourceId || value->sourceId<lower || value->sourceId>upper ||
               (static_cast<uint64_t>(value->sourceId)&1u)!=index)return;
            state.sourceIds[index]=value->sourceId;
            state.acceptedFrames[index]=frame;
            result.accepted[index]=true;
        };
        if(state.delay>1) {
            const auto half=state.delay/2;
            if(AgeReached(frame,state.acceptedFrames[0],state.delay) &&
               AgeReached(frame,state.acceptedFrames[1],half)) {
                const auto lower=state.sourceIds[1]+1;
                admit(AerV2Eye::Left,lower,(std::max)(lower,sourceUpper));
            }
            // A successful left acceptance changes its anchor to frame.
            if(AgeReached(frame,state.acceptedFrames[0],half)) {
                const auto lower=state.sourceIds[0]+1;
                admit(AerV2Eye::Right,lower,(std::max)(lower,sourceUpper));
            }
        } else {
            admit(AerV2Eye::Right,(std::max)(state.sourceIds[0]+1,state.sourceIds[1]+2),
                  (std::numeric_limits<int64_t>::max)());
            // Do not precompute this window before the right callback.
            admit(AerV2Eye::Left,(std::max)(state.sourceIds[1]-1,state.sourceIds[0]+2),
                  state.sourceIds[1]+1);
        }
        state.lastFrame=frame;
        state.delay=appliedDelay!=0 ? appliedDelay : 1;
        result.nextDelay=state.delay;result.status=AerV2StepStatus::Advanced;
        return result;
    }
private:
    static bool AgeReached(int64_t frame,int64_t anchor,int32_t delay) noexcept {
        // frame>=0 and anchor>=-6 are class invariants. Unsigned subtraction
        // represents their nonnegative difference even near signed INT64_MAX;
        // neither signed anchor+delay nor frame-anchor is evaluated.
        return frame>=anchor && static_cast<uint64_t>(frame)-static_cast<uint64_t>(anchor)>=static_cast<uint32_t>(delay);
    }
    AerV2State state;
};
}
