#pragma once

#include <cstddef>
#include <cstdint>

#include "vyuha/md/id_map.h"
#include "vyuha/md/tick.h"

namespace vyuha::md {

struct TradeClockStats {
  std::uint64_t inferred{0};  // trades inferred from rising volume
  std::uint64_t full{0};      // ticks left unstamped because the map was full
};

// Stamps Tick::last_trade_ts (core thread only). A broker-provided time (Upstox ltt, Kite full mode)
// is kept; otherwise a trade is inferred when this feed's cumulative day volume for the instrument
// rises between two of its ticks, at that tick's exchange time. The last known time carries forward
// on ticks without a trade. Per (instrument, feed): two feeds of one instrument (ADR-020) never
// compare volumes with each other. A volume drop (next session, feed reconnect) re-bases without a
// trade. Fixed capacity, no allocation per tick.
class TradeClock {
 public:
  explicit TradeClock(std::size_t max_entries) : map_(max_entries) {}

  void stamp(Tick& tick) noexcept;
  [[nodiscard]] const TradeClockStats& stats() const noexcept { return stats_; }

 private:
  struct Entry {
    std::uint64_t volume{0};
    Nanos last_trade{0};
    bool seen{false};
  };
  // (feed, instrument) packed into one IdMap key: instrument ids are bigserials far below 2^40.
  static constexpr unsigned kFeedShift = 40;
  IdMap<Entry> map_;
  TradeClockStats stats_;
};

}  // namespace vyuha::md
