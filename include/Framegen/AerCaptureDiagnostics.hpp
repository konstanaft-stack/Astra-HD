#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace cvr::framegen {
enum class AerCaptureReason:uint32_t {
    Attempt,Published,NoView,NoResource,NoSlot,FenceTimeout,ResetFailed,
    MainReceiptRejected,NativeStampMissing,GuidesMissing,SignalFailed,Count
};
inline constexpr size_t AerCaptureReasonCount=size_t(AerCaptureReason::Count);
struct AerCaptureRecord {
    uint64_t captureSerial{},rendererEpoch{};
    int64_t sourceId{-1}; // unavailable before the actual image receipt is read
    uint64_t poolCount{},publishedSlot{UINT64_MAX},writerSlot{UINT64_MAX};
    uint64_t pinnedMask{},writingMask{},captureFenceValue{},captureFenceCompleted{};
    uint64_t producerUnknown{},receiptFlags{},guidesMask{};
    // receiptFlags: 1 image receipt read, 2 MAIN requested, 4 receipt present,
    // 8 receipt valid for live owner, 16 native stamp produced.
};
struct AerCaptureSnapshot {
    std::array<uint64_t,AerCaptureReasonCount> counts{};
    AerCaptureRecord last;
    uint64_t sequence{},droppedLastRecords{};
    AerCaptureReason reason{AerCaptureReason::Attempt};
    bool coherent{};
};
// Numeric telemetry only: no allocations, logging, waits or retained resources.
// Atomics make external reads race-free; sequence identifies a coherent record.
struct AerCaptureDiagnostics {
    uint32_t version{1},bytes{sizeof(AerCaptureDiagnostics)};
    std::array<std::atomic<uint64_t>,AerCaptureReasonCount> counts{};
    std::atomic_flag writer=ATOMIC_FLAG_INIT;
    std::atomic<uint64_t> sequence{},droppedLastRecords{},reason{};
    std::atomic<uint64_t> captureSerial{},rendererEpoch{};
    std::atomic<int64_t> sourceId{-1};
    std::atomic<uint64_t> poolCount{},publishedSlot{UINT64_MAX},writerSlot{UINT64_MAX};
    std::atomic<uint64_t> pinnedMask{},writingMask{},captureFenceValue{},captureFenceCompleted{};
    std::atomic<uint64_t> producerUnknown{},receiptFlags{},guidesMask{};
    void Count(AerCaptureReason value) {counts[size_t(value)].fetch_add(1,std::memory_order_relaxed);}
    void Record(AerCaptureReason value,const AerCaptureRecord& record) {
        Count(value);
        if(writer.test_and_set(std::memory_order_acquire)){droppedLastRecords.fetch_add(1,std::memory_order_relaxed);return;}
        sequence.fetch_add(1,std::memory_order_acq_rel);
        reason.store(uint64_t(value),std::memory_order_relaxed);
#define CVR_CAPTURE_STORE(field) field.store(record.field,std::memory_order_relaxed)
        CVR_CAPTURE_STORE(captureSerial);CVR_CAPTURE_STORE(rendererEpoch);CVR_CAPTURE_STORE(sourceId);
        CVR_CAPTURE_STORE(poolCount);CVR_CAPTURE_STORE(publishedSlot);CVR_CAPTURE_STORE(writerSlot);
        CVR_CAPTURE_STORE(pinnedMask);CVR_CAPTURE_STORE(writingMask);
        CVR_CAPTURE_STORE(captureFenceValue);CVR_CAPTURE_STORE(captureFenceCompleted);
        CVR_CAPTURE_STORE(producerUnknown);CVR_CAPTURE_STORE(receiptFlags);CVR_CAPTURE_STORE(guidesMask);
#undef CVR_CAPTURE_STORE
        sequence.fetch_add(1,std::memory_order_release);writer.clear(std::memory_order_release);
    }
    AerCaptureSnapshot Read()const {
        AerCaptureSnapshot out;
        for(size_t i=0;i<counts.size();++i)out.counts[i]=counts[i].load(std::memory_order_relaxed);
        out.droppedLastRecords=droppedLastRecords.load(std::memory_order_relaxed);
        for(unsigned attempt=0;attempt<3;++attempt) {
            const auto before=sequence.load(std::memory_order_acquire);if(before&1)continue;
            out.reason=AerCaptureReason(reason.load(std::memory_order_relaxed));
#define CVR_CAPTURE_LOAD(field) out.last.field=field.load(std::memory_order_relaxed)
            CVR_CAPTURE_LOAD(captureSerial);CVR_CAPTURE_LOAD(rendererEpoch);CVR_CAPTURE_LOAD(sourceId);
            CVR_CAPTURE_LOAD(poolCount);CVR_CAPTURE_LOAD(publishedSlot);CVR_CAPTURE_LOAD(writerSlot);
            CVR_CAPTURE_LOAD(pinnedMask);CVR_CAPTURE_LOAD(writingMask);
            CVR_CAPTURE_LOAD(captureFenceValue);CVR_CAPTURE_LOAD(captureFenceCompleted);
            CVR_CAPTURE_LOAD(producerUnknown);CVR_CAPTURE_LOAD(receiptFlags);CVR_CAPTURE_LOAD(guidesMask);
#undef CVR_CAPTURE_LOAD
            std::atomic_thread_fence(std::memory_order_acquire);
            out.sequence=sequence.load(std::memory_order_relaxed);
            if(before==out.sequence){out.coherent=true;break;}
        }
        return out;
    }
};
static_assert(sizeof(AerCaptureDiagnostics)==232);
static_assert(offsetof(AerCaptureDiagnostics,counts)==8);
static_assert(offsetof(AerCaptureDiagnostics,sequence)==104);
static_assert(offsetof(AerCaptureDiagnostics,captureSerial)==128);
static_assert(offsetof(AerCaptureDiagnostics,guidesMask)==224);
}
extern "C" __declspec(dllexport) extern cvr::framegen::AerCaptureDiagnostics CyberpunkVR_AerCaptureDebug;
namespace cvr::framegen {
inline AerCaptureSnapshot ReadAerCaptureDiagnostics(){return CyberpunkVR_AerCaptureDebug.Read();}
}
