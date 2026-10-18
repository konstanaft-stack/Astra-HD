#pragma once
#include <cstdint>

namespace cvr::framegen {
// Pinned Streamline result ABI: eOk=0, eWarnOutOfVRAM=39. Its common
// slEvaluateFeatureInternal returns39 AFTER successful feature evaluation when
// the available VRAM budget is zero. Preserve the original result for the game,
// but keep the completed frame/history. Other failures are not accepted.
// Inputs.cpp asserts these values against the actual SDK used by the DLL.
inline constexpr bool SlEvaluationCompleted(uint32_t result) noexcept {
    return result==0 || result==39;
}
}
