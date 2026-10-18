#pragma once
#include <cpvr/timing.hpp>
#include <atomic>
#include <mutex>

namespace cvr::framegen {
struct CameraApplicationTime {
    int64_t qpc{},xrTime{};
    uint64_t clockGeneration{},spaceEpoch{};
};
// The XR owner publishes an actual runtime QPC conversion. Camera publication
// samples this mapping without waiting. Neither predicted display time nor
// capture-submission time is used as the mapping's current-time anchor.
class SourceClock {
public:
    uint64_t Generation() const noexcept {return generation.load(std::memory_order_acquire);}
    void Invalidate() noexcept {generation.fetch_add(1,std::memory_order_acq_rel);}
    bool Publish(const cpvr::XrClockMapping& value,uint64_t space,uint64_t expectedGeneration) noexcept {
        if(!space || expectedGeneration!=Generation() || !cpvr::xr_time_from_counter(value,value.qpc_anchor))return false;
        std::unique_lock lock(mutex,std::try_to_lock);
        if(!lock.owns_lock() || expectedGeneration!=Generation())return false;
        mapping=value;mappedGeneration=expectedGeneration;mappedSpaceEpoch=space;return true;
    }
    std::optional<CameraApplicationTime> AtCameraApplication(int64_t qpc,uint64_t space) const noexcept {
        std::unique_lock lock(mutex,std::try_to_lock);
        if(!lock.owns_lock() || !space || space!=mappedSpaceEpoch || mappedGeneration!=Generation())return {};
        const auto time=cpvr::xr_time_from_counter(mapping,qpc);
        if(!time || mappedGeneration!=Generation())return {};
        return CameraApplicationTime{qpc,*time,mappedGeneration,space};
    }
private:
    mutable std::mutex mutex;
    std::atomic<uint64_t> generation{1};
    cpvr::XrClockMapping mapping;
    uint64_t mappedGeneration{},mappedSpaceEpoch{};
};
}
