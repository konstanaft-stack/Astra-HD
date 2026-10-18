#pragma once
#include "Runtimes/CaptureBufferLeases.hpp"
#include <span>
#include <optional>
namespace cvr::capture {
// AER keeps two accepted sources while three capture command contexts and
// completed/retiring display readers overlap. Four cells can recycle a source
// before its ready version is observed. The bounded FIFO replay sustains both
// eyes with eight cells, including the six reconstruction tickets. This is not
// a guarantee against arbitrary GPU stalls: exhaustion still skips safely.
inline constexpr uint32_t MainPoolCapacity(bool aer,bool /*ownGeneration*/) {
    return aer ? 8u : 3u;
}
inline constexpr uint32_t MainPoolMaximum=8;
inline constexpr uint32_t MainEyeQueueSlots=3;
inline constexpr bool MainEyeQueueSlot(uint32_t slot,uint32_t eye){
    return eye<2 && slot/MainEyeQueueSlots==eye;
}
inline std::optional<uint32_t> ReserveMainWrite(std::span<const uintptr_t> pool,
    uintptr_t published,uint32_t last,BufferLeases& leases) {
    if(pool.empty())return {};
    for(uint32_t step=1;step<=pool.size();++step) {
        const auto slot=uint32_t((uint64_t(last)+step)%pool.size());
        if(pool[slot] && pool[slot]!=published && leases.BeginWrite(pool[slot]))return slot;
    }
    return {};
}
}
