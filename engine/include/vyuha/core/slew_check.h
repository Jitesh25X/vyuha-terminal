#pragma once

#include <string_view>

namespace vyuha {

// The fastest NTP slew the engine accepts, in ppm (D-36). Above it a clock-second can be noticeably shorter
// than a real second during a correction, and the 1 s OPS window with it. chrony's default is 83,333.
inline constexpr double kMaxSlewPpm = 500.0;
inline constexpr double kChronyDefaultSlewPpm = 83'333.333;

struct SlewCheck {
  bool ok{false};  // a usable maxslewrate, above 0 and at most kMaxSlewPpm, on every line, none unreadable
  bool configured{false};  // there was a usable maxslewrate line
  bool unreadable{false};  // a maxslewrate line that isn't `maxslewrate <number>`
  double ppm{0.0};         // the largest maxslewrate found, or chrony's default when there is none
  // makestep with a negative limit on every such line (`makestep 1 -1`): chrony steps the clock whenever it
  // is off by more than the threshold, however often. A pause is then corrected by a step at chrony's next
  // measurement, which PauseWatch sees; slewed away over minutes, it isn't seen at all.
  bool steps{false};
};

// Reads chrony.conf text. Directives are matched without regard to case; comments start with #, !, ; or %.
// With no maxslewrate chrony uses its default, which is too high, so that isn't ok. Anything but one number
// after it, or a number that isn't above 0, is not ok either.
[[nodiscard]] SlewCheck check_chrony_slew(std::string_view conf) noexcept;

}  // namespace vyuha
