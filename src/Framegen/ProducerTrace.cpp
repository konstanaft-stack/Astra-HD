#include "Framegen/ProducerTrace.hpp"

extern "C" {
__declspec(dllexport) cvr::framegen::ProducerTraceStorage CyberpunkVR_FramegenProducerTrace;
}
namespace cvr::framegen {
ProducerTraceStorage::ProducerTraceStorage() {
    slotsOffset=uint32_t(offsetof(ProducerTraceStorage,slots));
    countersOffset=uint32_t(offsetof(ProducerTraceStorage,counters));storageSize=uint32_t(sizeof(*this));
}
bool ProducerTraceStorage::Record(const ProducerTraceRecord& record) noexcept {
    attempted.fetch_add(1,std::memory_order_relaxed);
    const auto reason=size_t(record.reason);
    if(reason>=counters.size()) {dropped.fetch_add(1,std::memory_order_relaxed);return false;}
    counters[reason].fetch_add(1,std::memory_order_relaxed);
    uint32_t unlocked=0;
    if(!writeLock.compare_exchange_strong(unlocked,1,std::memory_order_acquire,std::memory_order_relaxed)) {
        dropped.fetch_add(1,std::memory_order_relaxed);return false;
    }
    writeEpoch.fetch_add(1,std::memory_order_acq_rel);
    const auto sequence=published.load(std::memory_order_relaxed)+1;
    slots[(sequence-1)%Capacity]={sequence,record};
    published.store(sequence,std::memory_order_relaxed);
    writeEpoch.fetch_add(1,std::memory_order_release);
    writeLock.store(0,std::memory_order_release);return true;
}
void RecordProducerTrace(const ProducerTraceRecord& record) noexcept {CyberpunkVR_FramegenProducerTrace.Record(record);}
}
