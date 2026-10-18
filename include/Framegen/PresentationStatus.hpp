#pragma once
#include <atomic>
#include <cstdint>

namespace cvr::framegen {
struct NativePolicyStatus {
    bool known{}, enabled{};
    unsigned multiplier{};
    unsigned revision{};
};
inline unsigned ProducedPolicyRevision(NativePolicyStatus before,NativePolicyStatus after,unsigned generated) noexcept {
    return before.known && before.enabled && after.known && after.enabled && before.revision &&
        before.revision==after.revision && generated>0 && generated<0xffffffffu && before.multiplier==generated+1 &&
        after.multiplier==before.multiplier ? before.revision : 0;
}
// One atomic packet keeps mode and multiplier from different callbacks apart.
class NativePolicyStatusTracker {
    std::atomic<uint64_t> packet_{};
public:
    void Observe(unsigned viewport, bool succeeded, bool enabled, unsigned generated) noexcept {
        if (viewport != 0) return;
        const bool known = succeeded && (!enabled || (generated > 0 && generated < 0xffffffffu));
        const uint64_t value=known ? (1u | (enabled ? 2u : 0u) |
            (uint64_t(enabled ? generated + 1 : 0) << 32)) : 0;
        constexpr uint64_t revisionMask=0xfffffffc;
        auto prior=packet_.load(std::memory_order_acquire);
        do {
            if((prior & ~revisionMask)==value)return;
            const auto revision=((prior & revisionMask)+4)&revisionMask;
            if(packet_.compare_exchange_weak(prior,value|(revision?revision:4),std::memory_order_acq_rel))return;
        }while(true);
    }
    NativePolicyStatus Read() const noexcept {
        const auto p = packet_.load(std::memory_order_acquire);
        return {(p & 1) != 0, (p & 2) != 0, unsigned(p >> 32),unsigned((p & 0xfffffffc)>>2)};
    }
};
enum class PresentedCamera : unsigned { Unknown, Alternating, Paired };
// Access under the metrics mutex. Only an accepted, valid XR image updates this.
class PresentedCameraTracker {
    PresentedCamera mode_{};
    double submitted_{};
public:
    void Accepted(PresentedCamera mode, double now) noexcept {mode_=mode;submitted_=now;}
    PresentedCamera Read(double now) const noexcept {
        return now>=submitted_ && now-submitted_<=1000 ? mode_ : PresentedCamera::Unknown;
    }
};
class PresentedGenerationTracker {
    PresentedCamera camera_{};
    unsigned policy_{};
    double generated_{};
public:
    void Camera(PresentedCamera camera) noexcept {
        if(camera_!=camera){generated_=0;camera_=camera;}
    }
    void Generated(PresentedCamera camera,unsigned revision,double now) noexcept {
        if(camera==camera_ && camera!=PresentedCamera::Unknown){policy_=revision;generated_=now;}
    }
    bool Active(NativePolicyStatus policy,PresentedCamera camera,double now) const noexcept {
        return policy.known && policy.enabled && policy.revision==policy_ && camera!=PresentedCamera::Unknown &&
            camera==camera_ && generated_>0 && now>=generated_ && now-generated_<=2000;
    }
    bool RecentDelivery(PresentedCamera camera,double now) const noexcept {
        return camera!=PresentedCamera::Unknown && camera==camera_ && generated_>0 &&
            now>=generated_ && now-generated_<=2000;
    }
};
}
