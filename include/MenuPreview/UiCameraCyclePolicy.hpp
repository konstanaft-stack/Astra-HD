#pragma once
#include <cstdint>
namespace cvr::menu_preview {
inline bool CapturedCameraRevision(uint32_t epoch,uint32_t eye,uint64_t revision,
    uint32_t capturedEpoch,uint32_t capturedEye,uint64_t capturedRevision) {
    return epoch && eye<2 && revision && epoch==capturedEpoch &&
        eye==capturedEye && revision==capturedRevision;
}
}
