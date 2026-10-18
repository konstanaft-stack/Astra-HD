#pragma once
#include <array>
#include <bitset>
#include <cmath>
#include <cstdint>

namespace cvr::framegen {
enum class CameraConstantsSource { None,Streamline,NativeStaging };
struct CameraConstants {
    // Existing NVIDIA bridge layout, without guessed defaults. Presence means
    // the exact source supplied this field; validity is checked separately.
    std::array<float,104> values{};
    std::array<uint32_t,7> flags{};
    std::bitset<104> valuePresent;
    std::bitset<7> flagPresent;
    CameraConstantsSource source=CameraConstantsSource::None;
    uint32_t version{};
};
// This is only the capture completeness gate. A later adapter must still
// validate projection/motion semantics and the exact temporal predecessor.
inline bool HasCompleteCameraConstants(const CameraConstants& c) {
    if(!c.valuePresent.all() || !c.flagPresent.all())return false;
    for(float value:c.values)if(!std::isfinite(value) || std::abs(value)>=3.4e38f)return false;
    for(uint32_t flag:c.flags)if(flag>1)return false;
    return true;
}
}
