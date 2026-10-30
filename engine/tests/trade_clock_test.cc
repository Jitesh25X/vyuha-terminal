#include "vyuha/md/trade_clock.h"

#include <gtest/gtest.h>

#include "md_test_util.h"

namespace {

using vyuha::Nanos;
using vyuha::md::Tick;
using vyuha::md::TradeClock;
using vyuha::testing::ist;
using vyuha::testing::trade;

Tick on_feed(Tick t, std::uint32_t feed) {
  t.feed_id = feed;
  return t;
}

// Kite quote mode: no trade time on the wire, so it is inferred from cumulative day volume rising.
TEST(TradeClock, InfersTradesFromRisingVolumeAndCarriesThemForward) {
  TradeClock clock(16);
  const Nanos t0 = ist(2026, 10, 30, 9, 30);
  const Nanos t1 = ist(2026, 10, 30, 9, 30, 5);
  const Nanos t2 = ist(2026, 10, 30, 9, 31);

  Tick a = on_feed(trade(7, t0, 100, 1'000), 11);
  clock.stamp(a);
  EXPECT_EQ(a.last_trade_ts, 0) << "first tick: no baseline, no trade time";

  Tick b = on_feed(trade(7, t1, 100, 1'000), 11);  // quote refresh, same volume
  clock.stamp(b);
  EXPECT_EQ(b.last_trade_ts, 0);

  Tick c = on_feed(trade(7, t1, 101, 1'075), 11);  // volume rose: a trade at this tick's time
  clock.stamp(c);
  EXPECT_EQ(c.last_trade_ts, t1);

  Tick d = on_feed(trade(7, t2, 101, 1'075), 11);  // no trade since: the time carries forward
  clock.stamp(d);
  EXPECT_EQ(d.last_trade_ts, t1);
  EXPECT_EQ(clock.stats().inferred, 1U);
}

// The same instrument on two feeds (ADR-020) keeps separate baselines: one feed's volume lagging the
// other's must not look like a trade.
TEST(TradeClock, KeepsOneBaselinePerFeed) {
  TradeClock clock(16);
  const Nanos t0 = ist(2026, 10, 30, 9, 30);
  const Nanos t1 = ist(2026, 10, 30, 9, 30, 1);
  Tick k = on_feed(trade(7, t0, 100, 5'000), 11);
  Tick u = on_feed(trade(7, t0, 100, 4'900), 22);  // the other feed is a little behind
  clock.stamp(k);
  clock.stamp(u);
  Tick k2 = on_feed(trade(7, t1, 100, 5'000), 11);
  clock.stamp(k2);
  EXPECT_EQ(k2.last_trade_ts, 0) << "feed 11 saw no new volume";
  Tick u2 = on_feed(trade(7, t1, 100, 5'000), 22);
  clock.stamp(u2);
  EXPECT_EQ(u2.last_trade_ts, t1) << "feed 22's own volume rose";
}

// A broker-provided trade time (Upstox ltt, Kite full mode) wins and becomes the baseline.
TEST(TradeClock, BrokerTradeTimeWins) {
  TradeClock clock(16);
  const Nanos ltt = ist(2026, 10, 30, 9, 29, 58);
  Tick a = on_feed(trade(7, ist(2026, 10, 30, 9, 30), 100, 1'000), 22);
  a.last_trade_ts = ltt;
  clock.stamp(a);
  EXPECT_EQ(a.last_trade_ts, ltt);
  Tick b = on_feed(trade(7, ist(2026, 10, 30, 9, 30, 2), 100, 1'000), 22);  // quote only, no ltt
  clock.stamp(b);
  EXPECT_EQ(b.last_trade_ts, ltt) << "carried forward from the broker's value";
  EXPECT_EQ(clock.stats().inferred, 0U);
}

// Day volume restarts from zero the next session (or after a feed reconnect): re-base, don't invent
// a trade, and don't wait for volume to pass yesterday's total.
TEST(TradeClock, VolumeDropRebasesWithoutATrade) {
  TradeClock clock(16);
  const Nanos y = ist(2026, 10, 29, 15, 29);
  Tick a = on_feed(trade(7, y, 100, 9'000'000), 11);
  clock.stamp(a);
  Tick b = on_feed(trade(7, y + 1, 100, 9'000'100), 11);
  clock.stamp(b);
  ASSERT_EQ(b.last_trade_ts, y + 1);

  const Nanos open = ist(2026, 10, 30, 9, 15, 3);
  Tick c = on_feed(trade(7, open, 100, 500), 11);  // new day: far below yesterday
  clock.stamp(c);
  EXPECT_EQ(c.last_trade_ts, y + 1) << "no trade invented; yesterday's time is old, callers check age";
  Tick d = on_feed(trade(7, open + 1, 100, 650), 11);
  clock.stamp(d);
  EXPECT_EQ(d.last_trade_ts, open + 1) << "rising again from the new base";
}

// Packets without volume (LTP mode, indices) say nothing about trades.
TEST(TradeClock, IgnoresTicksWithoutVolume) {
  TradeClock clock(16);
  Tick a = on_feed(trade(7, ist(2026, 10, 30, 9, 30), 100, 0), 11);
  Tick b = on_feed(trade(7, ist(2026, 10, 30, 9, 30, 1), 101, 0), 11);
  clock.stamp(a);
  clock.stamp(b);
  EXPECT_EQ(b.last_trade_ts, 0);
}

TEST(TradeClock, FullMapLeavesTicksUnstampedAndCounts) {
  TradeClock clock(1);
  Tick a = on_feed(trade(7, ist(2026, 10, 30, 9, 30), 100, 1), 11);
  Tick b = on_feed(trade(8, ist(2026, 10, 30, 9, 30), 100, 1), 11);
  clock.stamp(a);
  clock.stamp(b);
  EXPECT_EQ(clock.stats().full, 1U);
  EXPECT_EQ(b.last_trade_ts, 0);
}

}  // namespace
