// =============================================================================
//  timer.h - High-precision clock
// =============================================================================
//  QueryPerformanceCounter (QPC) is Windows' most precise clock: it measures
//  in "ticks", usually of 100 nanoseconds (0.0001 ms). Reading it costs about
//  20-30 nanoseconds, so it can be used every frame without problems.
//
//  Usage:
//      const std::int64_t start = perf::Now();
//      ...something...
//      const double ms = perf::TicksToMs(perf::Now() - start);
// =============================================================================
#pragma once

#include <windows.h>

#include <cstdint>

namespace luma::perf {

// How many ticks there are in a second. Windows sets it when the PC boots and
// it doesn't change, so we read it only once (in Init).
inline std::int64_t g_ticksPerSecond = 1;

inline void Init() {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    g_ticksPerSecond = frequency.QuadPart;
}

// "inline" on such short functions lets the compiler copy them into the
// place where they're used, without the cost of a call.
inline std::int64_t Now() {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}

inline double TicksToMs(std::int64_t ticks) {
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(g_ticksPerSecond);
}

inline double TicksToUs(std::int64_t ticks) {
    return static_cast<double>(ticks) * 1'000'000.0 / static_cast<double>(g_ticksPerSecond);
}

}  // namespace luma::perf
