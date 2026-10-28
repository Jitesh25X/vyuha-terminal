#pragma once

#include <cstdint>

#include "vyuha/core/clock.h"

namespace vyuha::md {

// IST is UTC+05:30 all year (no daylight saving), so a fixed offset is exact.
inline constexpr Nanos kIstOffset = (5 * 60 + 30) * kNanosPerMinute;

// Regular NSE/BSE cash and F&O continuous session in IST minutes since midnight: [09:15, 15:30).
// Pre-open (09:00–09:15) and post-close ticks update the last-value book but form no bars.
// Special sessions (e.g. Muhurat) arrive later from exchange_holiday.special_session.
struct SessionHours {
  std::int32_t open_minute{(9 * 60) + 15};
  std::int32_t close_minute{(15 * 60) + 30};
};

// Start of the IST calendar day containing `ts`, as UTC ns.
[[nodiscard]] constexpr Nanos ist_day_start(Nanos ts) noexcept {
  constexpr Nanos kDay = Nanos{1'440} * kNanosPerMinute;  // minutes per day
  const Nanos local = ts + kIstOffset;
  Nanos day = local / kDay;
  if (local % kDay < 0) {
    --day;  // floor for pre-1970 instants
  }
  return (day * kDay) - kIstOffset;
}

[[nodiscard]] constexpr Nanos session_open(Nanos ts, SessionHours h = {}) noexcept {
  return ist_day_start(ts) + (h.open_minute * kNanosPerMinute);
}

[[nodiscard]] constexpr Nanos session_close(Nanos ts, SessionHours h = {}) noexcept {
  return ist_day_start(ts) + (h.close_minute * kNanosPerMinute);
}

[[nodiscard]] constexpr bool in_session(Nanos ts, SessionHours h = {}) noexcept {
  return ts >= session_open(ts, h) && ts < session_close(ts, h);
}

}  // namespace vyuha::md
