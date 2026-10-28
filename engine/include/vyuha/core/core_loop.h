#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stop_token>

#include "vyuha/core/clock.h"
#include "vyuha/core/spsc_ring.h"
#include "vyuha/md/bar_builder.h"
#include "vyuha/md/last_value.h"
#include "vyuha/md/session.h"
#include "vyuha/md/tick.h"

namespace vyuha {

// One ring per IO decoder thread (ARCHITECTURE.md §4). 16k ticks ≈ 4.6 MB each.
using TickRing = SpscRing<md::Tick, 16384>;

struct CoreConfig {
  std::size_t max_instruments{50'000};
  md::SessionHours hours{};
  Nanos bar_grace{2 * kNanosPerSecond};
  Nanos timer_period{100'000'000};  // bar close checks
  std::size_t poll_budget{4096};    // ticks per poll() before timers run
};

struct CoreStats {
  std::uint64_t ticks{0};
  std::uint64_t invalid{0};  // instrument id 0 or negative prices
  std::uint64_t book_full{0};
  std::uint64_t day_rollovers{0};
};

// The single-writer core thread (market-data half; OMS/risk join in M2–M3). Drains the input
// rings, validates and sequences ticks, keeps the last-value book and 1s/1m bars, and forwards
// to the sink. No locks and no allocation after construction.
class CoreLoop {
 public:
  static constexpr std::size_t kMaxInputs = 8;

  CoreLoop(const CoreConfig& cfg, const IClock& clock, md::IMdSink& sink);

  // Before run(): each ring has exactly one producer (an IO decoder thread). False when full.
  [[nodiscard]] bool add_input(TickRing& ring) noexcept;

  // One iteration: up to poll_budget ticks round-robin across inputs, then timers. Returns ticks
  // processed. Backtests and tests call this directly with a SimClock.
  std::size_t poll() noexcept;
  // Polls until stop is requested; yields when idle. Logs start/stop (Quill) if logging is set up.
  void run(const std::stop_token& stop);

  [[nodiscard]] const md::LastValueBook& book() const noexcept { return book_; }
  [[nodiscard]] const md::BarBuilder& bars(md::BarInterval i) const noexcept {
    return i == md::BarInterval::k1s ? bars_1s_ : bars_1m_;
  }
  [[nodiscard]] const CoreStats& stats() const noexcept { return stats_; }

 private:
  void on_tick(md::Tick& tick) noexcept;
  void on_timer(Nanos now) noexcept;

  CoreConfig cfg_;
  const IClock* clock_;  // not owned; outlives the loop
  md::IMdSink* sink_;    // not owned; outlives the loop
  std::array<TickRing*, kMaxInputs> inputs_{};
  std::size_t n_inputs_{0};
  std::size_t next_input_{0};
  md::LastValueBook book_;
  md::BarBuilder bars_1s_;
  md::BarBuilder bars_1m_;
  CoreStats stats_;
  std::uint64_t seq_{0};
  Nanos next_timer_{0};
  Nanos day_{0};
};

}  // namespace vyuha
