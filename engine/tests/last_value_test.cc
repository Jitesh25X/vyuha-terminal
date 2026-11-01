#include "vyuha/md/last_value.h"

#include <gtest/gtest.h>

#include "md_test_util.h"

namespace {

using vyuha::Nanos;
using vyuha::md::LastValueBook;
using vyuha::md::Tick;
using vyuha::testing::ist;
using vyuha::testing::trade;

Tick on_feed(Tick t, std::uint32_t feed) {
  t.feed_id = feed;
  return t;
}

// The book stamps last_trade_ts on update; the tests read the stamped tick back.
void stamp(LastValueBook& book, Tick& t) { (void)book.update(t); }

// Kite quote mode: no trade time on the wire, so it is inferred from cumulative day volume rising.
TEST(LastValueTrades, InfersTradesFromRisingVolumeAndCarriesThemForward) {
  LastValueBook book(16);
  const Nanos t0 = ist(2026, 10, 30, 9, 30);
  const Nanos t1 = ist(2026, 10, 30, 9, 30, 5);
  const Nanos t2 = ist(2026, 10, 30, 9, 31);

  Tick a = on_feed(trade(7, t0, 100, 1'000), 11);
  stamp(book, a);
  EXPECT_EQ(a.last_trade_ts, 0) << "first tick: no baseline, no trade time";

  Tick b = on_feed(trade(7, t1, 100, 1'000), 11);  // quote refresh, same volume
  stamp(book, b);
  EXPECT_EQ(b.last_trade_ts, 0);

  Tick c = on_feed(trade(7, t1, 101, 1'075), 11);  // volume rose: a trade at this tick's time
  stamp(book, c);
  EXPECT_EQ(c.last_trade_ts, t1);

  Tick d = on_feed(trade(7, t2, 101, 1'075), 11);  // no trade since: the time carries forward
  stamp(book, d);
  EXPECT_EQ(d.last_trade_ts, t1);
  EXPECT_EQ(book.stats().inferred, 1U);
}

// The same instrument on two feeds (ADR-020) keeps separate baselines: one feed's volume lagging the
// other's must not look like a trade.
TEST(LastValueTrades, KeepsOneBaselinePerFeed) {
  LastValueBook book(16);
  const Nanos t0 = ist(2026, 10, 30, 9, 30);
  const Nanos t1 = ist(2026, 10, 30, 9, 30, 1);
  Tick k = on_feed(trade(7, t0, 100, 5'000), 11);
  Tick u = on_feed(trade(7, t0, 100, 4'900), 22);  // the other feed is a little behind
  stamp(book, k);
  stamp(book, u);
  Tick k2 = on_feed(trade(7, t1, 100, 5'000), 11);
  stamp(book, k2);
  EXPECT_EQ(k2.last_trade_ts, 0) << "feed 11 saw no new volume";
  Tick u2 = on_feed(trade(7, t1, 100, 5'000), 22);
  stamp(book, u2);
  EXPECT_EQ(u2.last_trade_ts, t1) << "feed 22's own volume rose";
}

// A broker-provided trade time (Upstox ltt, Kite full mode) wins and becomes the baseline.
TEST(LastValueTrades, BrokerTradeTimeWins) {
  LastValueBook book(16);
  const Nanos ltt = ist(2026, 10, 30, 9, 29, 58);
  Tick a = on_feed(trade(7, ist(2026, 10, 30, 9, 30), 100, 1'000), 22);
  a.last_trade_ts = ltt;
  stamp(book, a);
  EXPECT_EQ(a.last_trade_ts, ltt);
  Tick b = on_feed(trade(7, ist(2026, 10, 30, 9, 30, 2), 100, 1'000), 22);  // quote only, no ltt
  stamp(book, b);
  EXPECT_EQ(b.last_trade_ts, ltt) << "carried forward from the broker's value";
  EXPECT_EQ(book.stats().inferred, 0U);
}

// Day volume restarts from zero the next session (or after a feed reconnect): re-base, don't invent
// a trade, and don't wait for volume to pass yesterday's total.
TEST(LastValueTrades, VolumeDropRebasesWithoutATrade) {
  LastValueBook book(16);
  const Nanos y = ist(2026, 10, 29, 15, 29);
  Tick a = on_feed(trade(7, y, 100, 9'000'000), 11);
  stamp(book, a);
  Tick b = on_feed(trade(7, y + 1, 100, 9'000'100), 11);
  stamp(book, b);
  ASSERT_EQ(b.last_trade_ts, y + 1);

  const Nanos open = ist(2026, 10, 30, 9, 15, 3);
  Tick c = on_feed(trade(7, open, 100, 500), 11);  // new day: far below yesterday
  stamp(book, c);
  EXPECT_EQ(c.last_trade_ts, y + 1) << "no trade invented; yesterday's time is old, callers check age";
  Tick d = on_feed(trade(7, open + 1, 100, 650), 11);
  stamp(book, d);
  EXPECT_EQ(d.last_trade_ts, open + 1) << "rising again from the new base";
}

// Packets without volume (LTP mode, indices) say nothing about trades.
TEST(LastValueTrades, IgnoresTicksWithoutVolume) {
  LastValueBook book(16);
  Tick a = on_feed(trade(7, ist(2026, 10, 30, 9, 30), 100, 0), 11);
  Tick b = on_feed(trade(7, ist(2026, 10, 30, 9, 30, 1), 101, 0), 11);
  stamp(book, a);
  stamp(book, b);
  EXPECT_EQ(b.last_trade_ts, 0);
}

TEST(LastValueTrades, FullBookLeavesTicksUnstamped) {
  LastValueBook book(1);
  Tick a = on_feed(trade(7, ist(2026, 10, 30, 9, 30), 100, 1), 11);
  Tick b = on_feed(trade(8, ist(2026, 10, 30, 9, 30), 100, 1), 11);
  EXPECT_TRUE(book.update(a));
  EXPECT_FALSE(book.update(b));  // the core counts it (book_full)
  EXPECT_EQ(b.last_trade_ts, 0);
}

// Beyond kFeedsPerInstrument feeds of one instrument: no inference for the extra feed, but a
// broker-provided time still passes through untouched.
TEST(LastValueTrades, FeedsBeyondTheSlotsAreCountedNotInferred) {
  LastValueBook book(16);
  const Nanos t0 = ist(2026, 10, 30, 9, 30);
  for (std::uint32_t f = 1; f <= LastValueBook::kFeedsPerInstrument; ++f) {
    Tick t = on_feed(trade(7, t0, 100, 1'000), f);
    stamp(book, t);
  }
  Tick extra = on_feed(trade(7, t0 + 1, 100, 2'000), 99);
  stamp(book, extra);
  EXPECT_EQ(extra.last_trade_ts, 0);
  EXPECT_EQ(book.stats().feeds_full, 1U);
  Tick with_ltt = on_feed(trade(7, t0 + 2, 100, 3'000), 99);
  with_ltt.last_trade_ts = t0;
  stamp(book, with_ltt);
  EXPECT_EQ(with_ltt.last_trade_ts, t0);
  Tick known = on_feed(trade(7, t0 + 3, 100, 1'500), 2);  // the tracked feeds still work
  stamp(book, known);
  EXPECT_EQ(known.last_trade_ts, t0 + 3);
}

}  // namespace
