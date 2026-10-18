#pragma once
#include "Framegen/SrTemporalConstants.hpp"
#include <bit>

namespace cvr::framegen::sr {
// Explicitly selected CP2077 / NGX SuperSampling baseline, recovered from
// Luke42DA's global ctor -> instance reset -> CP no-op override chain.
// See audit/luke-eye-cycle-20261003/sr-motion-shader/CONFIGURATION-CONTRACT.json.
// Known means this exact chosen package, never an inferred historical/live
// setting. Runtime activation and source/resource/configuration epochs remain
// the caller's responsibility. No feature13/18 variant is implied here.
inline Parameters ReferenceCp2077SuperSamplingDefaults(bool effectiveFix) noexcept {
    Parameters p;
    p.known=true;p.effectiveFix=effectiveFix;p.key=0x7B18232B987EB889ull;p.ngxFeature=1;
    p.compatFlags=0x18;p.debugFlags=0;p.debugInt=0;p.debugFloat=0;p.pad0=0;p.pad1={0,0,0};
    p.useActualParity=true;p.swapEyeOffsets=false;p.linearDepth=false;p.depthMode=2;
    p.mvMultiplier={1,1};p.projectionGain={1,1};p.projectionOffset={0,0};p.depthScale=1;
    p.nearFarOverride.reset();
    p.minEstimatedZ=std::bit_cast<float>(uint32_t{0x3A83126F});
    p.offsetZ=0;p.depthTolerance=std::bit_cast<float>(uint32_t{0x3C23D70A});
    p.mvTolerancePixels=.5f;p.staticThreshold=0;p.staticZExp=1;
    p.mcDepthTolerance=std::bit_cast<float>(uint32_t{0x3C23D70A});
    return p;
}
}
