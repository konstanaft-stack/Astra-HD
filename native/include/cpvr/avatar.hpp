#pragma once
#include "input.hpp"
#include <atomic>
#include <memory>

namespace cpvr {
struct AvatarSnapshot {
    BodySolution body;
    std::array<HandButtons,2> buttons{};
    bool focused{};
};
class AvatarTracker {
public:
    AvatarTracker();
    // May be called by UI/game thread. Applied on the next tracking sample.
    bool request_settings(const BodySettings&);
    void request_recenter() noexcept;
    // Single XR owner calls once per sample before any GPU/FG selection.
    // It never calls OpenXR, game skeletons, physics or graphics itself.
    bool consume(const InputSample&);
    // Game animation reads this once for the LOCAL PLAYER, not once per NPC/bone/eye.
    std::shared_ptr<const AvatarSnapshot> latest() const noexcept;
private:
    SeatedBody body_;
    std::atomic<std::shared_ptr<const BodySettings>> pending_;
    std::atomic<std::shared_ptr<const AvatarSnapshot>> published_;
    std::atomic<bool> recenter_{true};
    std::uint64_t epoch_{};
    std::int64_t time_ns_{};
};
}
