#pragma once
#include <cstdint>
#include <array>
#include "Framegen/MainEyeSourceLatch.hpp"
#include "Overlay/ReferenceOptics.hpp"

namespace cvr::framegen {
enum class CaptureTransfer:uint32_t {Unknown,SrgbEncoded,Linear};
struct NativeEyeStamp {
    uint64_t sourceSerial{},poseId{},poseWriteId{},spaceEpoch{},clockGeneration{};
    int64_t cameraApplicationQpc{},contentTimeNs{},posePredictionNs{};
    uint32_t engineView{},nativeFrame{};
    bool nativeFrameKnown{};
    uint32_t renderedWidth{},renderedHeight{}; // Actual game resource, before capture resampling.
    // Immutable applied MAIN source/eye provenance. Native frame and GPU
    // completion remain independent fields at their existing boundaries.
    MainEyeSourceReceipt mainEyeSource{};
    // Exact source.pose.localHead captured with this MAIN image label. This is
    // the reference-space HEAD, before the per-eye offset, not a current pose.
    std::array<float,7> capturedHead{}; // quaternion xyzw, position xyz
    bool capturedHeadValid{};
    cvr::ppd::ReferenceOptics referenceOptics{};
};
struct NativeFrameStamp {
    // Generation of the author's actual capture allocation pool; not an
    // inferred game frame number or a clock epoch.
    uint64_t resourceEpoch{};
    CaptureTransfer transfer=CaptureTransfer::Unknown;
    // CPU publication event of an actually captured render pair, mapped by
    // the runtime QPC clock. Independent of a camera pose's last write time.
    int64_t renderPublicationTimeNs{};
    uint64_t publicationClockGeneration{};
    NativeEyeStamp eyes[2];
};
struct PresentationTime {
    int64_t target{},period{};
    // Optional read-only timing observation. Never substitutes for target or
    // changes source timestamps/poses, selection deadlines, or GPU readiness.
    int64_t observedNow{};
    uint64_t observedClockGeneration{};
};
}
