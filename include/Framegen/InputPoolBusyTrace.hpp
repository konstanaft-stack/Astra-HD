#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace cvr::framegen {
enum class InputReuseDecision:uint32_t {
    NotExamined,Empty,Reusable,ExternalAlias,SubmissionMaskMismatch,
    UnknownProducer,FencePending,FenceInvalid,Count
};
enum InputReuseKnown:uint32_t {
    InputSlotPresent=1u,InputIdentityCopied=2u,InputUseCountRead=4u,
    InputMaskPredicateReached=8u,InputProducerPredicateReached=16u,
    InputFencePresentRead=32u,InputCompletionRead=64u
};
struct InputReuseSlot {
    uint64_t generation{},poseId{},poseWriteId{},origin{},clockGeneration{},useCount{};
    uint64_t readyFence{},requiredValue{},completedValue{};
    uint32_t slotIndex{},knownBits{},nativeFrame{},view{},expectedMask{},submittedMask{},claimedMask{};
    uint32_t invalid{},projectionOnly{},unknownProducer{};
    InputReuseDecision decision=InputReuseDecision::NotExamined;
};
struct InputPoolBusyRecord {
    uint64_t requestedPoseId{},requestedPoseWriteId{},requestedOrigin{},requestedClockGeneration{},qpc{};
    uint32_t requestedNativeFrame{},requestedView{},incomingMask{},projectionOnly{},requestedKnownBits{},reserved{};
    std::array<InputReuseSlot,4> slots{};
};
struct InputPoolBusySlot {uint64_t sequence{};InputPoolBusyRecord record;};
struct InputPoolBusyStorage {
    static constexpr uint32_t Capacity=64;
    uint32_t schemaVersion=1,recordSize=sizeof(InputPoolBusyRecord),capacity=Capacity,slotSize=sizeof(InputPoolBusySlot);
    uint32_t slotsOffset{},countersOffset{},storageSize{},decisionCount=uint32_t(InputReuseDecision::Count);
    std::atomic<uint64_t> writeEpoch{},published{},attempted{},dropped{};
    std::atomic<uint32_t> writeLock{};
    // Units: attempted/published/dropped are failed LOOKUPS; counters count
    // first-blocker decisions of their four SLOTS, including dropped lookups.
    std::array<std::atomic<uint64_t>,size_t(InputReuseDecision::Count)> counters{};
    std::array<InputPoolBusySlot,Capacity> slots{};
    InputPoolBusyStorage() noexcept {
        slotsOffset=uint32_t(offsetof(InputPoolBusyStorage,slots));
        countersOffset=uint32_t(offsetof(InputPoolBusyStorage,counters));storageSize=sizeof(*this);
    }
    bool Record(const InputPoolBusyRecord& record) noexcept {
        attempted.fetch_add(1,std::memory_order_relaxed);
        for(const auto& slot:record.slots) {
            const auto reason=size_t(slot.decision);
            if(reason<counters.size())counters[reason].fetch_add(1,std::memory_order_relaxed);
        }
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
};
static_assert(std::is_trivially_copyable_v<InputReuseSlot> && std::is_standard_layout_v<InputReuseSlot>);
static_assert(std::is_trivially_copyable_v<InputPoolBusyRecord> && std::is_standard_layout_v<InputPoolBusyStorage>);
static_assert(sizeof(InputReuseSlot)==120 && sizeof(InputPoolBusyRecord)==544 && sizeof(InputPoolBusySlot)==552);
static_assert(offsetof(InputPoolBusyStorage,writeEpoch)==32 && offsetof(InputPoolBusyStorage,counters)==72);
static_assert(offsetof(InputPoolBusyStorage,slots)==136 && sizeof(InputPoolBusyStorage)==35464);
static_assert(std::atomic<uint64_t>::is_always_lock_free && std::atomic<uint32_t>::is_always_lock_free);
}
extern "C" {
__declspec(dllexport) extern cvr::framegen::InputPoolBusyStorage CyberpunkVR_FramegenInputPoolBusyTrace;
}
