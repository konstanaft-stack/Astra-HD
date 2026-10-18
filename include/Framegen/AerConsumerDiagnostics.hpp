#pragma once
#include <array>
#include <cstdint>
namespace cvr::framegen {
struct AerConsumerTelemetry {
    uint32_t version=1,bytes=288;
    // Attempts, including retries of one capture. These are not unique frames.
    std::array<std::array<uint64_t,12>,2> conversions{};
    std::array<uint64_t,2> admissions{},validationRejected{};
    std::array<int64_t,2> selectedSources{-2,-1};
    uint64_t selectionTicks{},realEndFrames{};
    std::array<int64_t,2> submittedSources{-2,-1};
    uint64_t sourceGeneration{};
};
static_assert(sizeof(AerConsumerTelemetry)==288);
}
