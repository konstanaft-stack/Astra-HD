#pragma once
#include "core.hpp"

namespace cpvr {
// All positions are metres in the SAME OpenXR LOCAL space: +X right, +Y up,
// -Z forward. Transform the whole solution into game space once, at its owner.
// No camera, animation, GPU, physics or game callbacks occur inside this solver.
struct TrackedPose { Pose pose; bool valid{}; };
struct BodyTracking {
    TrackedPose head;
    std::array<TrackedPose,2> grips; // OpenXR grip poses, never aim/ray poses.
    std::uint64_t space_epoch{};
    std::int64_t time_ns{};
};
struct BodySettings {
    float shoulder_width_m=0.36f;
    float upper_arm_m=0.29f, forearm_m=0.27f;
    float neck_to_shoulder_m=0.09f, shoulder_to_hips_m=0.43f;
    std::array<float,3> head_from_neck_m{0.0f,0.10f,-0.07f};
    float torso_yaw_deadzone_rad=0.30f;
    float torso_yaw_speed_rad_s=3.0f;
    float elbow_min_flexion_rad=0.06f, elbow_max_flexion_rad=2.62f;
    // Per controller binding/device calibration, not a universal guessed angle.
    std::array<Pose,2> grip_to_wrist{};
};
struct HandContact {
    // Collision code supplies an admissible WRIST target for this exact sample.
    // A surface hit alone is insufficient: palm shape/offset must be resolved.
    bool valid{};
    std::uint64_t space_epoch{};
    std::int64_t time_ns{};
    std::array<float,3> wrist_position{};
};
struct ArmSolution {
    bool valid{}, reach_limited{}, contact_limited{}, release_grip{};
    std::array<float,3> shoulder{}, elbow{};
    Pose tracked_wrist{}, visual_wrist{};
    float tracking_separation_m{}, elbow_flexion_rad{};
};
struct BodySolution {
    bool valid{}, calibration_required{};
    Pose view_head; // Exact input pose. The camera does NOT consume IK output.
    Pose torso; // Visual skeleton target; never directly moves the gameplay capsule.
    std::array<float,3> neck{};
    std::array<ArmSolution,2> arms{};
    std::uint64_t space_epoch{};
    std::int64_t time_ns{};
};
class SeatedBody {
public:
    bool configure(const BodySettings&) noexcept;
    bool calibrate(const BodyTracking&) noexcept;
    BodySolution solve(const BodyTracking&, float dt_seconds,
                       const std::array<HandContact,2>& contacts={}) noexcept;
    void reset() noexcept;
private:
    BodySettings settings_{};
    bool calibrated_{};
    std::uint64_t epoch_{};
    std::int64_t last_time_{};
    float torso_yaw_{};
    std::array<std::array<float,3>,2> elbow_poles_{};
    std::array<bool,2> elbow_ready_{};
};
}
