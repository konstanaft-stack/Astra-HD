#pragma once
#include <cstdint>
namespace cvr::gpu {
enum class QueueDrainResult {Complete,NoQueuedWork,RegistryFailed,SignalFailed,DeviceRemoved,TimedOut,Exhausted,Failed};
// The caller holds closed producer admission. This never admits a failed drain.
inline bool ResizeQuiescent(QueueDrainResult result,bool initializedKnown,uint8_t initialized,bool noReferences)noexcept{
    return result==QueueDrainResult::Complete ||
        (result==QueueDrainResult::NoQueuedWork && initializedKnown && initialized==0 && noReferences);
}
}
