#pragma once

#include <chrono>

// Host wall-clock source for the devices that run on host time (RTC, CYCLES,
// GP timer ticks: TEMPORARY host-time behaviour). Null means
// system_clock (called directly, so the default path stays inlined); tests
// install a function to drive host time deterministically.
using HostClockFn = std::chrono::system_clock::time_point (*)();

inline std::chrono::system_clock::time_point HostNow(HostClockFn clock) {
    return clock ? clock() : std::chrono::system_clock::now();
}
