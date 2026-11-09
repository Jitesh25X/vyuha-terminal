#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "vyuha/core/clock.h"

namespace vyuha::oms {

inline constexpr std::uint8_t kDefaultOpsCap = 8;
inline constexpr std::uint8_t kMaxOpsCap = 10;       // the DB's CHECK; the threshold in COMPLIANCE.md
inline constexpr std::size_t kOpsVenues = 2;         // NSE, BSE
inline constexpr std::uint8_t kOpsVenueUnknown = 2;  // an instrument not in the master: charged to both

// The OPS token bucket, for both R-OPS and the send queue. A token comes back exactly one second after it was
// used, so there are never more than cap orders in any rolling second. A bucket that refills at cap per
// second from a full start would allow nearly 2 x cap in one second, which is over the 10 OPS threshold at
// the default cap of 8. The check doesn't change anything; take records the order. A clock that stepped back
// gives no token. A cancel path calls clamp_future first, so a step back holds a kill up for at most a
// second. NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index): both indexes are reduced modulo the
// array size
class OpsWindow {
 public:
  [[nodiscard]] constexpr bool has_token(Nanos now, std::uint8_t cap) const noexcept {
    if (cap == 0 || cap > kMaxOpsCap) {
      return false;
    }
    const Nanos cap_th_latest = used_[(next_ + kMaxOpsCap - cap) % kMaxOpsCap];
    if (cap_th_latest == kNever) {
      return true;
    }
    return now - cap_th_latest >= kNanosPerSecond;
  }
  // Treats anything recorded later than now as recorded now: the clock stepped back. The cap still holds.
  constexpr void clamp_future(Nanos now) noexcept {
    for (Nanos& used : used_) {
      used = std::min(used, now);
    }
  }
  constexpr void take(Nanos now) noexcept {
    used_[next_] = now;
    next_ = static_cast<std::uint8_t>((next_ + 1U) % kMaxOpsCap);
  }
  constexpr bool operator==(const OpsWindow&) const noexcept = default;

 private:
  static constexpr Nanos kNever = INT64_MIN;
  static constexpr std::array<Nanos, kMaxOpsCap> kUnused{kNever, kNever, kNever, kNever, kNever,
                                                         kNever, kNever, kNever, kNever, kNever};
  std::array<Nanos, kMaxOpsCap> used_{kUnused};
  std::uint8_t next_{0};
};
// NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)

}  // namespace vyuha::oms
