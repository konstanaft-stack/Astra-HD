#pragma once
#include "Framegen/MainEyeSourceLatch.hpp"
namespace cvr::framegen {
// Detached legacy cameras have no proved tracking-head mapping. The scene AER
// writer publishes the exact tracked sample with its actual camera writes.
inline bool CapturedHeadTrusted(bool externalView,bool sourceValidated,
                               const MainEyeSourceReceipt& receipt,
                               uint64_t origin,int64_t locatedTime) {
    return !externalView || (sourceValidated && receipt &&
                             receipt.MatchesWrittenPose(origin,locatedTime));
}
}
