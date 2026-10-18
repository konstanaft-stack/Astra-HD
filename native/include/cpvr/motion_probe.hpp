#pragma once
#include "core.hpp"
#include <span>
#include <limits>

namespace cpvr {
// Correspondences must come from an independent projection/depth reference,
// not from the motion vector being tested. Invalid/occluded points stay invalid.
struct MotionCorrespondence {
    std::array<double,2> current_uv{},previous_uv{},raw_motion{};
    bool valid=true;
};
struct MotionProbeOptions {
    std::array<double,2> native_scale{1,1},jitter_removal_bias{};
    Extent comparison_extent;
    unsigned minimum_samples=8;
    // Minimum reference displacement AND sign-bearing raw*scale magnitude.
    // Known jitter bias is excluded from the latter; it cannot identify a sign.
    double minimum_motion_pixels=0.25,maximum_rmse_pixels=0.25,minimum_error_ratio=4;
};
enum class ProbedMotionDirection { Inconclusive, ScaledCurrentToPrevious, ScaledPreviousToCurrent };
enum class MotionProbeReason { InvalidOptions, InsufficientSamples, InsufficientMotion, NeitherFits, Ambiguous, Matched };
struct MotionDirectionReport {
    ProbedMotionDirection direction=ProbedMotionDirection::Inconclusive;
    MotionProbeReason reason=MotionProbeReason::InsufficientSamples;
    std::size_t supplied{},valid{},informative{},rejected{};
    double current_to_previous_rmse_pixels=std::numeric_limits<double>::quiet_NaN();
    double previous_to_current_rmse_pixels=std::numeric_limits<double>::quiet_NaN();
};
// Diagnostic only: no selected runtime convention, native constant, or resource
// is modified. Lack of evidence never selects a default sign.
MotionDirectionReport probe_motion_direction(std::span<const MotionCorrespondence>,const MotionProbeOptions&) noexcept;
}
