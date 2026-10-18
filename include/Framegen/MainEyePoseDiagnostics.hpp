#pragma once
#include <cstdint>

namespace cvr::framegen {
enum class MainEyePoseCode:uint32_t {
    None,InvalidReceipt,OwnerRejected,CacheHit,StaleScene,LocateFailed,
    InvalidSample,OwnerChanged,Stored,ReadMiss,ReadHit
};
enum class HeadPoseQueryCode:uint32_t {
    NotAttempted,BodyUnavailable,LifetimeBusyOrStopping,InvalidSession,
    InvalidTimeOrSession,ResetBeforeQuery,LocateSpaceFailed,InvalidLocationFlags,
    ResetAfterQuery,BasePoseMissing,Success
};
struct HeadPoseQueryDiagnostics {
    HeadPoseQueryCode code=HeadPoseQueryCode::NotAttempted;
    int32_t xrResult{};
    uint64_t locationFlags{};
};
// Exported read-only seqlock packet. No native renderer frame ID is known at
// this pre-camera boundary: source ID and the scene owner's ordinal are separate.
// actualTime is the existing runtime QPC conversion, never predicted displayTime.
struct MainEyePoseTelemetry {
    uint32_t version=1,bytes=256;
    uint64_t attempts{},success{},cacheHits{},locateFailed{},receiptRejected{},receiptChanged{},staleScene{},invalidSample{};
    uint64_t readAttempts{},readFailed{},published{},publishRejected{},publishClockMissing{},bodyUnavailable{};
    int64_t lastSourceId=-1,lastReadSourceId=-1,lastPublishSourceId=-1;
    int64_t requestedTimeNs{},locatedTimeNs{},actualTimeNs{},qpc{};
    uint64_t sceneOrdinal{},sourceClockGeneration{},captureClockGeneration{},poseId{},writeId{},originSerial{};
    uint32_t acquireCode{},readCode{},queryCode{},actualTimeKnown{};
    int32_t xrResult{};uint32_t reserved{};
    uint64_t locationFlags{};
};
static_assert(sizeof(MainEyePoseTelemetry)==256);
}
