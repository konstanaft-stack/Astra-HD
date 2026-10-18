#pragma once
#include "Framegen/Framegen.hpp"
#include <cpvr/generation.hpp>
namespace cvr::framegen {
inline cpvr::GenerationPolicy MakeOwnedGenerationPolicy(const Settings& settings) {
    cpvr::Settings wanted;wanted.multiplier=uint32_t(settings.ownGeneratedFrames+1);
    wanted.frame_generation=settings.enabled;
    // A true paired source needs complete real/generated stereo pairs. The
    // default chessboard pattern mixes R/G across eyes and cannot match its
    // strict same-time paired cadence. AER retains its own existing pattern.
    wanted.display_pattern=settings.aerV2?cpvr::DisplayPattern::Chessboard:cpvr::DisplayPattern::StereoPairs;
    return cpvr::generation_policy_for_source(wanted,
        settings.aerV2?cpvr::Cadence::AlternateEyes:cpvr::Cadence::StereoPairs);
}
}
