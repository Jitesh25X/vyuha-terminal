#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "vyuha/md/id_map.h"
#include "vyuha/md/tick.h"

namespace vyuha::md {

struct LastValueStats {
  std::uint64_t inferred{0};    // trades inferred from rising volume
  std::uint64_t feeds_full{0};  // ticks from a feed beyond kFeedsPerInstrument (not inferred)
};

// Latest tick per instrument, plus each feed's trade state for Tick::last_trade_ts (core thread only).
// Fixed capacity, no allocation per update.
//
// last_trade_ts: a broker-provided time (Upstox ltt, Kite full mode) is kept; otherwise a trade is
// inferred when *this feed's* cumulative day volume for the instrument rises between two of its ticks,
// at that tick's exchange time. The last known time carries forward on ticks without a trade. Two feeds
// of one instrument (ADR-020) never compare volumes with each other. A volume drop (next session, feed
// reconnect) re-bases without a trade. The per-feed state lives in the instrument's own entry, next to
// the tick being written, so stamping costs no extra lookup or cache miss (M1.11c).
class LastValueBook {
 public:
  // Feeds tracked per instrument (one per entitled account carrying it, ADR-020). Further feeds still
  // pass broker-provided times through but get no inference (counted in feeds_full).
  static constexpr std::size_t kFeedsPerInstrument = 4;

  explicit LastValueBook(std::size_t max_instruments) : map_(max_instruments) {}

  // Stamps tick.last_trade_ts, then stores the tick. False when the book is full: the tick is neither
  // stored nor stamped (callers count it).
  [[nodiscard]] bool update(Tick& tick) noexcept;
  [[nodiscard]] const Tick* find(InstrumentId id) const noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return map_.size(); }
  [[nodiscard]] const LastValueStats& stats() const noexcept { return stats_; }

 private:
  struct FeedTrade {
    Nanos last_trade{0};
    std::uint64_t volume{0};
    std::uint32_t feed_id{0};
    bool used{false};
  };
  struct Entry {
    std::array<FeedTrade, kFeedsPerInstrument> feeds{};  // first: shares the entry's first cache line
    bool set{false};
    Tick tick;
  };
  void stamp(Entry& e, Tick& tick) noexcept;

  IdMap<Entry> map_;
  LastValueStats stats_;
};

}  // namespace vyuha::md
