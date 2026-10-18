#pragma once
#include <array>
#include <cstdint>

namespace cpvr {
struct PhaseDurationSummary {
    std::uint64_t count{},rejected{};
    std::int64_t last_ns{},maximum_ns{},window_mean_ns{},p95_ns{},p99_ns{};
    unsigned window_samples{};
};
// CPU monotonic elapsed time only; never interpreted as GPU or XR display time.
// Callers serialize access with their existing status lock; storage is bounded.
class PhaseDurationWindow {
public:
    static constexpr unsigned capacity=128;
    bool observe(std::int64_t begin_ns,std::int64_t end_ns) noexcept;
    PhaseDurationSummary summary() const noexcept;
private:
    std::array<std::int64_t,capacity> values_{};
    std::uint64_t count_{},rejected_{};
    unsigned cursor_{},size_{};
    std::int64_t last_{},maximum_{};
};
}
