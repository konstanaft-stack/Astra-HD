#pragma once
#include "Framegen/MainEyeSourceLatch.hpp"
#include <array>
#include <cmath>
#include <limits>
#include <optional>
namespace cvr::camera {
inline float MainEyeSceneSign(framegen::AerV2Eye eye){return eye==framegen::AerV2Eye::Right ? 1.f:-1.f;}
inline std::optional<std::array<int32_t,3>> AddSceneSourceTranslation(
    const std::array<int32_t,3>& base,const std::array<float,3>& translation){
    std::array<int32_t,3> out{};
    for(unsigned i=0;i<3;++i){
        const double delta=std::trunc(double(translation[i])*131072.0);
        const double value=double(base[i])+delta;
        if(!std::isfinite(value)||value<(std::numeric_limits<int32_t>::min)()||value>(std::numeric_limits<int32_t>::max)())return {};
        out[i]=int32_t(value);
    }
    return out;
}
inline bool SceneSourceWriteComplete(const framegen::MainEyeSourceReceipt& source,bool exactPose,
    bool orientationWritten,bool translationWritten,bool eyeWritten){
    return bool(source)&&exactPose&&orientationWritten&&translationWritten&&eyeWritten;
}
inline std::optional<std::array<int32_t,3>> AddSceneSourceEye(const std::array<int32_t,3>& center,
    const std::array<int32_t,3>& shift){
    std::array<int32_t,3> out{};
    for(unsigned i=0;i<3;++i){
        const int64_t value=int64_t(center[i])+shift[i];
        if(value<(std::numeric_limits<int32_t>::min)()||value>(std::numeric_limits<int32_t>::max)())return {};
        out[i]=int32_t(value);
    }
    return out;
}
}
