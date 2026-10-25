#pragma once

// Monotonic clock (QueryPerformanceCounter) and frame-pump cadences. Never the world clock: world time
// resets on map load (v1 party-facts: bench cooldown locked across worlds).

#include <cstdint>

namespace elmp::clock {

std::int64_t now_us() noexcept;
double now_s() noexcept;

// A periodic gate driven by the frame pump: due() is true at most once per period.
struct Cadence {
    std::int64_t period_us;
    std::int64_t next_us = 0;

    explicit constexpr Cadence(std::int64_t period_ms) : period_us(period_ms * 1000) {}
    bool due(std::int64_t now) noexcept {
        if (now < next_us) return false;
        next_us = (next_us && now - next_us < period_us) ? next_us + period_us : now + period_us;
        return true;
    }
    void reset() noexcept { next_us = 0; }
};

} // namespace elmp::clock
