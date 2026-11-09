#pragma once

#include <time.h>  // NOLINT(modernize-deprecated-headers): clock_gettime is POSIX, and <ctime> needn't declare it

namespace vyuha {

// The clock WallClock::monotonic reads, one that keeps counting while the host is suspended (D-34). Linux:
// CLOCK_BOOTTIME. macOS: CLOCK_MONOTONIC, which its man page says continues to increment while the system
// sleeps (CLOCK_UPTIME_RAW doesn't). libstdc++'s steady_clock is CLOCK_MONOTONIC on Linux, which stops during
// suspend, so it isn't used. A hypervisor pause of the whole guest is not a guest suspend, and no guest clock
// counts it (D-36).
#if defined(__linux__)
inline constexpr clockid_t kMonotonicClockId = CLOCK_BOOTTIME;
#else
inline constexpr clockid_t kMonotonicClockId = CLOCK_MONOTONIC;
#endif

}  // namespace vyuha
