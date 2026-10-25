#include "base/clock.h"

#include <Windows.h>

namespace elmp::clock {

namespace {
const std::int64_t g_freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
}();
}

std::int64_t now_us() noexcept {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (c.QuadPart / g_freq) * 1'000'000 + (c.QuadPart % g_freq) * 1'000'000 / g_freq;
}

double now_s() noexcept { return static_cast<double>(now_us()) / 1e6; }

} // namespace elmp::clock
