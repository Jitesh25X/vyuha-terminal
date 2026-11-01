#include "vyuha/core/core_loop.h"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <stop_token>
#include <thread>

#include <gtest/gtest.h>

#include "md_test_util.h"
#include "vyuha/util/log.h"

// Counting global allocator for this test binary: proves the core loop's steady state allocates
// nothing (ARCHITECTURE.md §4 "no heap allocation per tick").
namespace {
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): the allocator hook's counter
std::atomic<std::size_t> g_allocs{0};
}  // namespace

// NOLINTBEGIN(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory,misc-new-delete-overloads)
void* operator new(std::size_t n) {
  g_allocs.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n == 0 ? 1 : n)) {
    return p;
  }
  throw std::bad_alloc();
}
// The nothrow forms must be replaced too, or a library's nothrow new (e.g. std::stable_sort's
// temporary buffer) allocates through the sanitizer and is freed here (alloc-dealloc mismatch).
void* operator new(std::size_t n, const std::nothrow_t& /*tag*/) noexcept {
  g_allocs.fetch_add(1, std::memory_order_relaxed);
  return std::malloc(n == 0 ? 1 : n);
}
void operator delete(void* p, const std::nothrow_t& /*tag*/) noexcept { std::free(p); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t /*n*/) noexcept { std::free(p); }
// NOLINTEND(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory,misc-new-delete-overloads)

namespace {

using vyuha::CoreConfig;
using vyuha::CoreLoop;
using vyuha::SimClock;
using vyuha::TickRing;
using vyuha::md::BarInterval;
using vyuha::testing::ist;
using vyuha::testing::NullSink;
using vyuha::testing::RecordingSink;
using vyuha::testing::trade;

TEST(CoreLoop, SequencesTicksUpdatesBookAndBuildsBars) {
  SimClock clock(ist(2026, 10, 28, 9, 15));
  RecordingSink sink;
  CoreLoop core(CoreConfig{.max_instruments = 64}, clock, sink);
  auto kite = std::make_unique<TickRing>();
  auto upstox = std::make_unique<TickRing>();
  ASSERT_TRUE(core.add_input(*kite));
  ASSERT_TRUE(core.add_input(*upstox));

  ASSERT_TRUE(kite->try_push(trade(1, ist(2026, 10, 28, 9, 15, 1), 100, 10)));
  ASSERT_TRUE(upstox->try_push(trade(2, ist(2026, 10, 28, 9, 15, 2), 200, 5)));
  ASSERT_TRUE(kite->try_push(trade(1, ist(2026, 10, 28, 9, 15, 30), 105, 20)));
  EXPECT_EQ(core.poll(), 3U);

  ASSERT_EQ(sink.ticks.size(), 3U);
  EXPECT_EQ(sink.ticks[0].seq, 1U);  // round-robin: kite, upstox, kite
  EXPECT_EQ(sink.ticks[1].instrument_id, 2U);
  EXPECT_EQ(sink.ticks[2].seq, 3U);
  EXPECT_EQ(core.book().find(1)->ltp, vyuha::Price::from_units(105 * vyuha::Price::kUnitsPerRupee));
  EXPECT_NE(core.bars(BarInterval::k1m).forming(1), nullptr);

  // The clock (not a tick) closes 09:15 once 09:16 + grace has passed.
  clock.set(ist(2026, 10, 28, 9, 16, 2));
  EXPECT_EQ(core.poll(), 0U);
  std::size_t minute_bars = 0;
  for (const auto& b : sink.bars) {
    minute_bars += b.interval == BarInterval::k1m ? 1 : 0;
  }
  EXPECT_EQ(minute_bars, 2U);
  EXPECT_EQ(core.stats().ticks, 3U);
}

// Kite quote-mode ticks carry no trade time: the core stamps one when this feed's volume rises and
// forwards it (to NATS / md_last) on every later tick of that feed.
TEST(CoreLoop, StampsInferredLastTradeTime) {
  const SimClock clock(ist(2026, 10, 30, 9, 30));
  RecordingSink sink;
  CoreLoop core(CoreConfig{.max_instruments = 64}, clock, sink);
  auto kite = std::make_unique<TickRing>();
  ASSERT_TRUE(core.add_input(*kite));
  const auto t1 = ist(2026, 10, 30, 9, 30, 1);
  for (auto t : {trade(1, ist(2026, 10, 30, 9, 30), 100, 10), trade(1, t1, 101, 25),
                 trade(1, ist(2026, 10, 30, 9, 30, 9), 101, 25)}) {
    t.feed_id = 5;
    ASSERT_TRUE(kite->try_push(t));
  }
  EXPECT_EQ(core.poll(), 3U);
  ASSERT_EQ(sink.ticks.size(), 3U);
  EXPECT_EQ(sink.ticks[0].last_trade_ts, 0);
  EXPECT_EQ(sink.ticks[1].last_trade_ts, t1);
  EXPECT_EQ(sink.ticks[2].last_trade_ts, t1);
  ASSERT_NE(core.book().find(1), nullptr);
  EXPECT_EQ(core.book().find(1)->last_trade_ts, t1);
  EXPECT_EQ(core.book().stats().inferred, 1U);
}

TEST(CoreLoop, RejectsInvalidTicks) {
  const SimClock clock(ist(2026, 10, 28, 10, 0));
  RecordingSink sink;
  CoreLoop core(CoreConfig{.max_instruments = 8}, clock, sink);
  auto ring = std::make_unique<TickRing>();
  ASSERT_TRUE(core.add_input(*ring));
  ASSERT_TRUE(ring->try_push(trade(0, ist(2026, 10, 28, 10, 0), 100, 1)));  // no instrument
  ASSERT_TRUE(ring->try_push(trade(3, ist(2026, 10, 28, 10, 0), -1, 1)));   // negative price
  EXPECT_EQ(core.poll(), 2U);
  EXPECT_EQ(core.stats().invalid, 2U);
  EXPECT_TRUE(sink.ticks.empty());
}

TEST(CoreLoop, RollsOverToTheNextTradingDay) {
  SimClock clock(ist(2026, 10, 28, 15, 0));
  RecordingSink sink;
  CoreLoop core(CoreConfig{.max_instruments = 8}, clock, sink);
  auto ring = std::make_unique<TickRing>();
  ASSERT_TRUE(core.add_input(*ring));
  ASSERT_TRUE(ring->try_push(trade(5, ist(2026, 10, 28, 15, 29, 30), 100, 900)));
  core.poll();
  clock.set(ist(2026, 10, 29, 9, 0));
  core.poll();  // closes yesterday's bars, then resets for the new day
  EXPECT_EQ(core.stats().day_rollovers, 1U);
  ASSERT_TRUE(ring->try_push(trade(5, ist(2026, 10, 29, 9, 15), 101, 40)));
  ASSERT_TRUE(ring->try_push(trade(5, ist(2026, 10, 29, 9, 16), 102, 70)));
  core.poll();
  EXPECT_EQ(sink.bars.back().interval, BarInterval::k1m);
  EXPECT_EQ(sink.bars.back().volume, 40U);  // today's first bar, not 40 − 900
}

TEST(CoreLoop, SteadyStateDoesNotAllocate) {
  SimClock clock(ist(2026, 10, 28, 9, 15));
  NullSink sink;
  CoreLoop core(CoreConfig{.max_instruments = 1024}, clock, sink);
  auto ring = std::make_unique<TickRing>();
  ASSERT_TRUE(core.add_input(*ring));
  std::uint64_t vol = 0;
  const auto feed = [&](int minute) {
    for (int i = 0; i < 2000; ++i) {
      const auto id = static_cast<vyuha::md::InstrumentId>((i % 500) + 1);
      ASSERT_TRUE(ring->try_push(trade(id, ist(2026, 10, 28, 9, 15 + minute, i % 60), 100 + (i % 7), ++vol)));
    }
  };
  feed(0);  // warm up: instruments enter the fixed tables
  while (core.poll() > 0) {
  }
  feed(1);
  const std::size_t before = g_allocs.load();
  while (core.poll() > 0) {
  }
  clock.set(ist(2026, 10, 28, 9, 20));
  core.poll();  // timer path (bar closes) too
  EXPECT_EQ(g_allocs.load() - before, 0U);
  EXPECT_GT(sink.bars, 0U);
}

struct AtomicCountSink final : vyuha::md::IMdSink {
  std::atomic<std::uint64_t> ticks{0};
  void on_tick(const vyuha::md::Tick& /*t*/) noexcept override {
    ticks.fetch_add(1, std::memory_order_release);
  }
  void on_bar(const vyuha::md::Bar& /*b*/) noexcept override {}
};

TEST(CoreLoop, RunsOnItsOwnThreadUntilStopped) {
  vyuha::log::init();  // Quill backend + sink; run() logs start/stop
  ASSERT_NE(vyuha::log::engine(), nullptr);
  const vyuha::WallClock clock;
  AtomicCountSink sink;
  CoreLoop core(CoreConfig{.max_instruments = 16}, clock, sink);
  auto ring = std::make_unique<TickRing>();
  ASSERT_TRUE(core.add_input(*ring));
  std::jthread core_thread([&core](const std::stop_token& st) { core.run(st); });
  for (std::uint64_t i = 1; i <= 10'000; ++i) {
    while (!ring->try_push(trade(1 + (i % 3), ist(2026, 10, 28, 10, 0), 100, i))) {
      std::this_thread::yield();
    }
  }
  while (sink.ticks.load(std::memory_order_acquire) < 10'000) {
    std::this_thread::yield();
  }
  core_thread.request_stop();
  core_thread.join();
  EXPECT_EQ(core.stats().ticks, 10'000U);  // safe after join
}

}  // namespace
