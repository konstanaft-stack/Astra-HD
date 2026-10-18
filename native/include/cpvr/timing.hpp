#pragma once
#include <cstdint>
#include <optional>
#include <cmath>
#include <limits>
namespace cpvr {
struct XrClockMapping {
    std::int64_t qpc_anchor{},xr_anchor_ns{},qpc_frequency{};
    bool valid{};
};
inline std::optional<std::int64_t> xr_time_from_counter(const XrClockMapping&m,std::int64_t qpc) noexcept {
    if(!m.valid||m.qpc_frequency<=0||m.qpc_anchor<=0||m.xr_anchor_ns<=0||qpc<=0)return {};
    const long double delta=static_cast<long double>(qpc)-m.qpc_anchor;
    if(std::abs(delta)>static_cast<long double>(m.qpc_frequency)*2)return {}; // stale mapping, never guess an epoch.
    const long double time=m.xr_anchor_ns+delta*1'000'000'000.0L/m.qpc_frequency;
    if(!std::isfinite(time)||time<=0||time>=static_cast<long double>(std::numeric_limits<std::int64_t>::max())-4096)return {};
    return static_cast<std::int64_t>(std::llround(time));
}
}
