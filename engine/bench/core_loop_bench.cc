#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

#include <benchmark/benchmark.h>

#include "vyuha/core/clock.h"
#include "vyuha/core/core_loop.h"
#include "vyuha/md/session.h"
#include "vyuha/md/tick.h"

// Core-thread cost per tick: ring pop + validation + last-value book + 1s/1m bars + sink.

namespace {

struct CountingSink final : vyuha::md::IMdSink {
  std::uint64_t n{0};
  void on_tick(const vyuha::md::Tick& /*t*/) noexcept override { ++n; }
  void on_bar(const vyuha::md::Bar& /*b*/) noexcept override { ++n; }
};

void BM_CorePollTicks(benchmark::State& state) {
  const auto instruments = static_cast<std::uint64_t>(state.range(0));
  // 2026-10-28 10:00 IST, inside the session.
  const vyuha::Nanos t0 = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::sys_days{std::chrono::year{2026} / 10 / 28}.time_since_epoch())
                              .count() +
                          (10 * 60 * vyuha::kNanosPerMinute) - vyuha::md::kIstOffset;
  vyuha::SimClock clock(t0);
  CountingSink sink;
  vyuha::CoreLoop core(vyuha::CoreConfig{.max_instruments = 50'000}, clock, sink);
  auto ring = std::make_unique<vyuha::TickRing>();
  (void)core.add_input(*ring);
  constexpr std::size_t kBatch = 4096;
  std::uint64_t seq = 0;
  for (auto _ : state) {
    for (std::size_t i = 0; i < kBatch; ++i, ++seq) {
      vyuha::md::Tick t;
      t.instrument_id = 1 + (seq % instruments);
      t.exchange_ts = t0 + static_cast<vyuha::Nanos>(seq * 1'000'000);  // 1 ms apart
      t.ltp = vyuha::Price::from_paise(250'000 + static_cast<std::int64_t>(seq % 100));
      t.volume = seq;
      (void)ring->try_push(t);
    }
    clock.set(t0 + static_cast<vyuha::Nanos>(seq * 1'000'000));
    while (core.poll() > 0) {
    }
  }
  benchmark::DoNotOptimize(sink.n);
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * kBatch));
}
BENCHMARK(BM_CorePollTicks)->Arg(100)->Arg(5'000);

}  // namespace
