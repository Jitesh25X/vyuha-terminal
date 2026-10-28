#pragma once

#include <cstddef>
#include <cstdint>

#include "vyuha/core/clock.h"
#include "vyuha/md/id_map.h"
#include "vyuha/md/session.h"
#include "vyuha/md/tick.h"

namespace vyuha::md {

struct BarBuilderStats {
  std::uint64_t bars{0};
  std::uint64_t late_ticks{0};        // older than the instrument's forming bar
  std::uint64_t out_of_session{0};    // pre-open / post-close ticks (no bar)
  std::uint64_t capacity_dropped{0};  // instrument table full
};

// Builds OHLCV bars of one interval from trades (ticks with ltp > 0), bucketed by exchange time
// and aligned to the session open, inside [open, close) IST only.
//
// - A bar is emitted (final) when a tick of a later bucket arrives, or by on_time() once the
//   clock passes the bar's end + `grace` (covers instruments that stop trading and the close).
// - Volume = change in the exchange's cumulative day volume since the previous bar of the day;
//   the first bar of the day therefore includes pre-open (auction) volume.
// - OI = last OI seen in the bar.
class BarBuilder {
 public:
  BarBuilder(BarInterval interval, std::size_t max_instruments, SessionHours hours = {},
             Nanos grace = 2 * kNanosPerSecond)
      : interval_(interval), hours_(hours), grace_(grace), map_(max_instruments) {}

  void on_tick(const Tick& tick, IMdSink& sink) noexcept;
  // Finalizes bars whose end + grace is at or before `now`.
  void on_time(Nanos now, IMdSink& sink) noexcept;
  // New trading day: forget forming bars and volume baselines (call after on_time closed them).
  void reset_day() noexcept;

  // The bar still forming for `id`, if any (not final).
  [[nodiscard]] const Bar* forming(InstrumentId id) const noexcept;
  [[nodiscard]] const BarBuilderStats& stats() const noexcept { return stats_; }
  [[nodiscard]] BarInterval interval() const noexcept { return interval_; }

 private:
  struct State {
    Bar bar;
    bool active{false};
    std::uint64_t cum_volume{0};       // latest cumulative day volume seen
    std::uint64_t prev_bar_volume{0};  // cumulative day volume at the previous bar's last tick
  };

  void finalize(State& s, IMdSink& sink) noexcept;

  BarInterval interval_;
  SessionHours hours_;
  Nanos grace_;
  IdMap<State> map_;
  BarBuilderStats stats_;
};

}  // namespace vyuha::md
