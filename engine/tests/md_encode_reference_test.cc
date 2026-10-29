// The engine's hand-written protobuf encoders vs protoc-generated vyuha.marketdata.v1 classes.

#include <array>
#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

#include "vyuha/bus/md_encode.h"
#include "vyuha/marketdata/v1/marketdata.pb.h"

namespace {

namespace pb = vyuha::marketdata::v1;
using vyuha::Price;

vyuha::md::Tick sample_tick() {
  vyuha::md::Tick t;
  t.instrument_id = 13'050'370;
  t.exchange_ts = 1'793'190'900'000'000'000;
  t.recv_ts = 1'793'190'900'000'123'456;
  t.seq = 42;
  t.feed_id = 3;
  t.source = vyuha::md::Source::kUpstox;
  t.ltp = *Price::parse("24450.05");
  t.ltq = 75;
  t.volume = 1'234'500;
  t.oi = 9'876'500;
  t.open = *Price::parse("24300");
  t.high = *Price::parse("24500");
  t.low = *Price::parse("24250");
  t.prev_close = *Price::parse("24280");
  t.avg_price = *Price::parse("24400");
  t.bid = *Price::parse("24450");
  t.ask = t.ltp;
  t.bid_qty = 75;
  t.ask_qty = 65;
  for (std::size_t i = 0; i < 5; ++i) {
    t.bids.at(i) = {Price::from_units(t.bid.units() - static_cast<std::int64_t>(i) * 500),
                    static_cast<std::uint32_t>(75 * (i + 1)), static_cast<std::uint32_t>(i + 1)};
    t.asks.at(i) = {Price::from_units(t.ask.units() + static_cast<std::int64_t>(i) * 500),
                    static_cast<std::uint32_t>(65 * (i + 1)), static_cast<std::uint32_t>(i + 2)};
  }
  return t;
}

void expect_same(const vyuha::md::Tick& t, const pb::Tick& p) {
  EXPECT_EQ(p.instrument_id(), t.instrument_id);
  EXPECT_EQ(p.exchange_ts_ns(), t.exchange_ts);
  EXPECT_EQ(p.recv_ts_ns(), t.recv_ts);
  EXPECT_EQ(p.source(), vyuha::common::v1::BROKER_UPSTOX);
  EXPECT_EQ(p.ltp(), t.ltp.units());
  EXPECT_EQ(p.ltq(), t.ltq);
  EXPECT_EQ(p.volume(), t.volume);
  EXPECT_EQ(p.oi(), t.oi);
  EXPECT_EQ(p.open(), t.open.units());
  EXPECT_EQ(p.prev_close(), t.prev_close.units());
  EXPECT_EQ(p.avg_price(), t.avg_price.units());
  EXPECT_EQ(p.bid(), t.bid.units());
  EXPECT_EQ(p.ask_qty(), t.ask_qty);
  EXPECT_EQ(p.seq(), t.seq);
  EXPECT_EQ(p.feed_id(), t.feed_id);
  ASSERT_EQ(p.bids_size(), 5);
  ASSERT_EQ(p.asks_size(), 5);
  EXPECT_EQ(p.bids(4).price(), t.bids[4].price.units());
  EXPECT_EQ(p.asks(4).qty(), t.asks[4].qty);
  EXPECT_EQ(p.asks(4).orders(), t.asks[4].orders);
}

TEST(MdEncodeReference, TickRoundTripsThroughGeneratedClasses) {
  std::array<std::uint8_t, vyuha::bus::kMaxTickBytes> buf{};
  const auto t = sample_tick();
  const std::size_t n = vyuha::bus::encode_tick(t, buf);
  ASSERT_GT(n, 0U);
  pb::Tick p;
  ASSERT_TRUE(p.ParseFromArray(buf.data(), static_cast<int>(n)));
  expect_same(t, p);
}

TEST(MdEncodeReference, NegativeAndExtremeValues) {
  auto t = sample_tick();
  t.ltp = Price::from_units(std::numeric_limits<std::int64_t>::min());  // never valid, must still encode
  t.exchange_ts = -1;
  std::array<std::uint8_t, vyuha::bus::kMaxTickBytes> buf{};
  const std::size_t n = vyuha::bus::encode_tick(t, buf);
  pb::Tick p;
  ASSERT_TRUE(p.ParseFromArray(buf.data(), static_cast<int>(n)));
  EXPECT_EQ(p.ltp(), std::numeric_limits<std::int64_t>::min());
  EXPECT_EQ(p.exchange_ts_ns(), -1);
  std::array<std::uint8_t, 10> tiny{};
  EXPECT_EQ(vyuha::bus::encode_tick(t, tiny), 0U);  // too small: refused, never truncated
}

TEST(MdEncodeReference, WorstCaseTickFitsTheBuffer) {
  constexpr auto kMin = std::numeric_limits<std::int64_t>::min();  // negative int64: 10-byte varints
  constexpr auto kU32 = std::numeric_limits<std::uint32_t>::max();
  constexpr auto kU64 = std::numeric_limits<std::uint64_t>::max();
  vyuha::md::Tick t;
  t.instrument_id = kU64;
  t.exchange_ts = t.recv_ts = kMin;
  t.seq = kU64;
  t.source = vyuha::md::Source::kUpstox;
  t.feed_id = kU32;
  for (Price* p : {&t.ltp, &t.open, &t.high, &t.low, &t.prev_close, &t.avg_price, &t.bid, &t.ask}) {
    *p = Price::from_units(kMin);
  }
  t.ltq = t.bid_qty = t.ask_qty = kU32;
  t.volume = t.oi = kU64;
  for (auto* side : {&t.bids, &t.asks}) {
    for (auto& l : *side) {
      l = {.price = Price::from_units(kMin), .qty = kU32, .orders = kU32};
    }
  }
  std::array<std::uint8_t, vyuha::bus::kMaxTickBytes> buf{};
  const std::size_t n = vyuha::bus::encode_tick(t, buf);
  ASSERT_GT(n, 0U);
  pb::Tick p;
  ASSERT_TRUE(p.ParseFromArray(buf.data(), static_cast<int>(n)));
  EXPECT_EQ(p.feed_id(), kU32);
  std::array<std::uint8_t, vyuha::bus::kMaxSnapshotBytes> sbuf{};
  EXPECT_GT(vyuha::bus::encode_snapshot(t, true, sbuf), 0U);
}

TEST(MdEncodeReference, BarAndSnapshot) {
  const vyuha::md::Bar b{.instrument_id = 7,
                         .interval = vyuha::md::BarInterval::k1m,
                         .ts = 1'793'190'900'000'000'000,
                         .open = *Price::parse("100"),
                         .high = *Price::parse("101.5"),
                         .low = *Price::parse("99.95"),
                         .close = *Price::parse("101"),
                         .volume = 5'000,
                         .oi = 12,
                         .final = true};
  std::array<std::uint8_t, vyuha::bus::kMaxSnapshotBytes> buf{};
  std::size_t n = vyuha::bus::encode_bar(b, buf);
  pb::Bar pbar;
  ASSERT_TRUE(pbar.ParseFromArray(buf.data(), static_cast<int>(n)));
  EXPECT_EQ(pbar.interval(), "1m");
  EXPECT_EQ(pbar.low(), b.low.units());
  EXPECT_EQ(pbar.volume(), 5'000U);
  EXPECT_TRUE(pbar.final());

  const auto t = sample_tick();
  n = vyuha::bus::encode_snapshot(t, true, buf);
  pb::Snapshot snap;
  ASSERT_TRUE(snap.ParseFromArray(buf.data(), static_cast<int>(n)));
  expect_same(t, snap.tick());
  EXPECT_NEAR(snap.change_pct(), 100.0 * (24450.05 - 24280.0) / 24280.0, 1e-9);
  EXPECT_TRUE(snap.stale());
}

}  // namespace
