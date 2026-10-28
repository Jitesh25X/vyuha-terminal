#include "vyuha/md/bar_builder.h"

#include <gtest/gtest.h>

#include "md_test_util.h"
#include "vyuha/md/last_value.h"
#include "vyuha/md/session.h"

namespace {

using vyuha::Price;
using vyuha::md::BarBuilder;
using vyuha::md::BarInterval;
using vyuha::testing::ist;
using vyuha::testing::RecordingSink;
using vyuha::testing::trade;

constexpr vyuha::md::InstrumentId kRel = 42;

Price rs(std::int64_t rupees) { return Price::from_units(rupees * Price::kUnitsPerRupee); }

TEST(Session, IstDayAndHours) {
  const auto t = ist(2026, 10, 28, 9, 15);
  EXPECT_EQ(vyuha::md::session_open(t), t);
  EXPECT_EQ(vyuha::md::session_close(t), ist(2026, 10, 28, 15, 30));
  EXPECT_EQ(vyuha::md::ist_day_start(ist(2026, 10, 28, 0, 0)), ist(2026, 10, 28, 0, 0));
  EXPECT_EQ(vyuha::md::ist_day_start(ist(2026, 10, 28, 23, 59, 59)), ist(2026, 10, 28, 0, 0));
  // 00:10 IST is still the previous UTC day: the IST day must not follow UTC.
  EXPECT_EQ(vyuha::md::ist_day_start(ist(2026, 10, 29, 0, 10)), ist(2026, 10, 29, 0, 0));
  EXPECT_FALSE(vyuha::md::in_session(ist(2026, 10, 28, 9, 14, 59, 999)));
  EXPECT_TRUE(vyuha::md::in_session(ist(2026, 10, 28, 9, 15)));
  EXPECT_TRUE(vyuha::md::in_session(ist(2026, 10, 28, 15, 29, 59, 999)));
  EXPECT_FALSE(vyuha::md::in_session(ist(2026, 10, 28, 15, 30)));
}

TEST(BarBuilder, OpeningBarStartsAt0915AndExcludesPreOpenPrices) {
  BarBuilder b(BarInterval::k1m, 16);
  RecordingSink sink;
  // Pre-open auction ticks (09:00–09:15): no bar, but their volume counts into the first bar.
  b.on_tick(trade(kRel, ist(2026, 10, 28, 9, 7, 30), 2400, 5'000), sink);
  b.on_tick(trade(kRel, ist(2026, 10, 28, 9, 14, 59, 999), 2401, 5'000), sink);
  EXPECT_EQ(b.forming(kRel), nullptr);
  EXPECT_EQ(b.stats().out_of_session, 2U);

  b.on_tick(trade(kRel, ist(2026, 10, 28, 9, 15), 2410, 5'100), sink);  // exactly 09:15:00.000
  b.on_tick(trade(kRel, ist(2026, 10, 28, 9, 15, 20), 2415, 5'300), sink);
  b.on_tick(trade(kRel, ist(2026, 10, 28, 9, 15, 59, 999), 2405, 5'400), sink);
  const auto* f = b.forming(kRel);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->ts, ist(2026, 10, 28, 9, 15));
  EXPECT_EQ(f->open, rs(2410));  // not the pre-open 2400/2401
  EXPECT_TRUE(sink.bars.empty());

  b.on_tick(trade(kRel, ist(2026, 10, 28, 9, 16), 2420, 5'450), sink);  // next minute closes 09:15
  ASSERT_EQ(sink.bars.size(), 1U);
  const auto& bar = sink.bars[0];
  EXPECT_EQ(bar.ts, ist(2026, 10, 28, 9, 15));
  EXPECT_EQ((std::array{bar.open, bar.high, bar.low, bar.close}),
            (std::array{rs(2410), rs(2415), rs(2405), rs(2405)}));
  EXPECT_EQ(bar.volume, 5'400U);  // whole day so far, including the pre-open auction
  EXPECT_TRUE(bar.final);
  EXPECT_EQ(b.forming(kRel)->ts, ist(2026, 10, 28, 9, 16));
}

TEST(BarBuilder, LastBarIs1529AndClosesAt1530) {
  BarBuilder b(BarInterval::k1m, 16);
  RecordingSink sink;
  b.on_tick(trade(kRel, ist(2026, 10, 28, 15, 28, 10), 2500, 1'000), sink);
  b.on_tick(trade(kRel, ist(2026, 10, 28, 15, 29, 0), 2501, 1'200), sink);
  b.on_tick(trade(kRel, ist(2026, 10, 28, 15, 29, 59, 999), 2503, 1'300), sink);
  ASSERT_EQ(sink.bars.size(), 1U);  // 15:28
  // A 15:30:00 tick belongs to no bar: it closes 15:29 and starts nothing.
  b.on_tick(trade(kRel, ist(2026, 10, 28, 15, 30), 2510, 1'350), sink);
  ASSERT_EQ(sink.bars.size(), 2U);
  EXPECT_EQ(sink.bars[1].ts, ist(2026, 10, 28, 15, 29));
  EXPECT_EQ(sink.bars[1].close, rs(2503));
  EXPECT_EQ(sink.bars[1].volume, 300U);  // 1 300 − 1 000 (end of 15:28); the 15:30 trade is not in it
  EXPECT_EQ(b.forming(kRel), nullptr);
  b.on_tick(trade(kRel, ist(2026, 10, 28, 15, 35), 2520, 1'400), sink);  // post-close
  EXPECT_EQ(sink.bars.size(), 2U);
  EXPECT_EQ(b.stats().out_of_session, 2U);
}

TEST(BarBuilder, ClockClosesTheLastBarWhenNoTickFollows) {
  BarBuilder b(BarInterval::k1m, 16, {}, 2 * vyuha::kNanosPerSecond);
  RecordingSink sink;
  b.on_tick(trade(kRel, ist(2026, 10, 28, 15, 29, 30), 2500, 10), sink);
  b.on_time(ist(2026, 10, 28, 15, 30, 1, 999), sink);  // within grace
  EXPECT_TRUE(sink.bars.empty());
  b.on_time(ist(2026, 10, 28, 15, 30, 2), sink);  // 15:30 + 2 s grace
  ASSERT_EQ(sink.bars.size(), 1U);
  EXPECT_EQ(sink.bars[0].ts, ist(2026, 10, 28, 15, 29));
  EXPECT_TRUE(sink.bars[0].final);
}

TEST(BarBuilder, OneSecondBarsAlignToTheOpen) {
  BarBuilder b(BarInterval::k1s, 16);
  RecordingSink sink;
  b.on_tick(trade(kRel, ist(2026, 10, 28, 9, 15, 0, 400), 100, 1), sink);
  b.on_tick(trade(kRel, ist(2026, 10, 28, 9, 15, 0, 999), 101, 2), sink);
  b.on_tick(trade(kRel, ist(2026, 10, 28, 9, 15, 1, 0), 102, 3), sink);
  ASSERT_EQ(sink.bars.size(), 1U);
  EXPECT_EQ(sink.bars[0].ts, ist(2026, 10, 28, 9, 15, 0));
  EXPECT_EQ(sink.bars[0].close, rs(101));
  EXPECT_EQ(sink.bars[0].volume, 2U);
}

TEST(BarBuilder, GapMinutesProduceNoEmptyBarsAndQuoteTicksDontMovePrices) {
  BarBuilder b(BarInterval::k1m, 16);
  RecordingSink sink;
  b.on_tick(trade(kRel, ist(2026, 10, 28, 10, 0, 5), 300, 10, 7), sink);
  auto quote = trade(kRel, ist(2026, 10, 28, 10, 0, 6), 0, 10, 9);  // no trade (ltp 0)
  b.on_tick(quote, sink);
  b.on_tick(trade(kRel, ist(2026, 10, 28, 10, 7, 0), 310, 25), sink);
  ASSERT_EQ(sink.bars.size(), 1U);
  EXPECT_EQ(sink.bars[0].ts, ist(2026, 10, 28, 10, 0));
  EXPECT_EQ(sink.bars[0].low, rs(300));
  EXPECT_EQ(sink.bars[0].oi, 7U);
  EXPECT_EQ(b.forming(kRel)->ts, ist(2026, 10, 28, 10, 7));
}

TEST(BarBuilder, LateTicksAreDroppedAndCounted) {
  BarBuilder b(BarInterval::k1m, 16);
  RecordingSink sink;
  b.on_tick(trade(kRel, ist(2026, 10, 28, 10, 1, 0), 300, 10), sink);
  b.on_tick(trade(kRel, ist(2026, 10, 28, 10, 0, 59), 1, 11), sink);  // belongs to 10:00, already gone
  EXPECT_EQ(b.stats().late_ticks, 1U);
  EXPECT_EQ(b.forming(kRel)->low, rs(300));
}

TEST(BarBuilder, InstrumentsAreIndependentAndCapacityIsBounded) {
  BarBuilder b(BarInterval::k1m, 2);
  RecordingSink sink;
  b.on_tick(trade(1, ist(2026, 10, 28, 10, 0), 10, 1), sink);
  b.on_tick(trade(2, ist(2026, 10, 28, 10, 0), 20, 1), sink);
  b.on_tick(trade(3, ist(2026, 10, 28, 10, 0), 30, 1), sink);
  EXPECT_EQ(b.stats().capacity_dropped, 1U);
  EXPECT_EQ(b.forming(1)->close, rs(10));
  EXPECT_EQ(b.forming(2)->close, rs(20));
  EXPECT_EQ(b.forming(3), nullptr);
}

TEST(BarBuilder, NewDayStartsVolumeFromZero) {
  BarBuilder b(BarInterval::k1m, 16);
  RecordingSink sink;
  b.on_tick(trade(kRel, ist(2026, 10, 28, 15, 29), 100, 9'000), sink);
  b.on_time(ist(2026, 10, 28, 16, 0), sink);
  b.reset_day();
  b.on_tick(trade(kRel, ist(2026, 10, 29, 9, 15), 101, 50), sink);  // cumulative volume reset by exchange
  b.on_tick(trade(kRel, ist(2026, 10, 29, 9, 16), 102, 80), sink);
  ASSERT_EQ(sink.bars.size(), 2U);
  EXPECT_EQ(sink.bars[1].volume, 50U);
}

TEST(LastValueBook, KeepsLatestTickPerInstrument) {
  vyuha::md::LastValueBook book(4);
  EXPECT_EQ(book.find(7), nullptr);
  ASSERT_TRUE(book.update(trade(7, 1, 100, 1)));
  ASSERT_TRUE(book.update(trade(7, 2, 101, 2)));
  ASSERT_TRUE(book.update(trade(8, 2, 50, 2)));
  EXPECT_EQ(book.find(7)->ltp, rs(101));
  EXPECT_EQ(book.size(), 2U);
  EXPECT_FALSE(book.update(trade(0, 2, 50, 2)));  // id 0 is never stored
}

}  // namespace
