#include <time.h>  // NOLINT(modernize-deprecated-headers): clock_gettime is POSIX, and <ctime> needn't declare it

#include <chrono>
#include <cstdlib>

#include "vyuha/core/clock.h"
#include "vyuha/core/monotonic_source.h"

namespace vyuha {

// The only place the engine reads the system clock (scripts/check-invariants.sh).
Nanos WallClock::now() const noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

Nanos WallClock::monotonic() const noexcept {
  timespec ts{};
  if (clock_gettime(kMonotonicClockId, &ts) != 0) {
    // Can't happen for a valid id. No stand-in value is safe for every user (0 would release OPS tokens and
    // end an R-DUP entry early), so the engine stops: no order goes out without a clock.
    std::abort();
  }
  return (static_cast<Nanos>(ts.tv_sec) * kNanosPerSecond) + static_cast<Nanos>(ts.tv_nsec);
}

}  // namespace vyuha
