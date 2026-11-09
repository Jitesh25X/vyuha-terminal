#pragma once

#include "vyuha/core/clock.h"

namespace vyuha {

// How far the wall clock must have moved past the monotonic clock, between two looks, to count as a pause.
inline constexpr Nanos kPauseGap = 2 * kNanosPerSecond;

// Spots a host or hypervisor pause (D-36). A paused guest's clocks stop, and the wall clock is set again from
// the host when it resumes, so the wall clock jumps ahead of the monotonic one. A forward NTP step looks the
// same, and that's accepted: all this triggers is a fail-safe action (the send queue expires, cached data
// goes stale and has to be refreshed). A wall-clock step never loosens a decision (D-26), and a step back is
// never a pause. Core thread only; each user keeps its own.
class PauseWatch {
 public:
  // True when, since the last call, the wall clock moved more than kPauseGap further than the monotonic one.
  // The first call only records the clocks.
  [[nodiscard]] bool observe(Nanos wall, Nanos monotonic) noexcept {
    const bool paused = seen_ && (wall - wall_) - (monotonic - monotonic_) > kPauseGap;
    seen_ = true;
    wall_ = wall;
    monotonic_ = monotonic;
    return paused;
  }
  [[nodiscard]] bool observe(const IClock& clock) noexcept { return observe(clock.now(), clock.monotonic()); }

 private:
  bool seen_{false};
  Nanos wall_{0};
  Nanos monotonic_{0};
};

}  // namespace vyuha
