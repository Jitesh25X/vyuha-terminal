#include <chrono>

#include "vyuha/core/clock.h"

namespace vyuha {

// The only place the engine reads the system clock (scripts/check-invariants.sh).
Nanos WallClock::now() const noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace vyuha
