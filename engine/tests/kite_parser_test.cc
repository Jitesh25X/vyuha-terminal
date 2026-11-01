#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "vyuha/broker/kite/ticker_parser.h"
#include "vyuha/broker/kite/ticker_shards.h"
#include "vyuha/core/price.h"
#include "vyuha/util/backoff.h"
#include "vyuha/util/net.h"
#include "vyuha/util/secret.h"

namespace {

namespace kite = vyuha::broker::kite;
using vyuha::Price;
using vyuha::md::Tick;

// Tokens in fixtures/vcr/kite/expected.json (gen_frames.py).
constexpr std::uint32_t kNfoFut = (61471U << 8U) | 2U;
constexpr std::uint32_t kNseEq = (2885U << 8U) | 1U;
constexpr std::uint32_t kNifty = 256265;
constexpr std::uint32_t kCds = (1234U << 8U) | 3U;
constexpr vyuha::Nanos kTs = 1'793'190'900LL * vyuha::kNanosPerSecond;
constexpr vyuha::Nanos kRecv = 42;

std::vector<std::uint8_t> fixture(const char* name) {
  const auto path = std::filesystem::path(VYUHA_FIXTURES_DIR) / "vcr" / "kite" / name;
  std::ifstream f(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}

kite::TokenMap tokens() {
  return kite::TokenMap({{kNfoFut, 101}, {kNseEq, 202}, {kNifty, 303}, {kCds, 404}});
}

std::vector<Tick> parse(const std::vector<std::uint8_t>& frame, kite::ParseStats& stats) {
  std::vector<Tick> out;
  kite::parse_frame(frame, tokens(), kRecv, stats, [&out](const Tick& t) { out.push_back(t); });
  return out;
}

Price paise(std::int64_t p) { return Price::from_paise(p); }

TEST(KiteParser, FullQuoteIndexAndUnsupportedSegments) {
  kite::ParseStats stats;
  const auto ticks = parse(fixture("full_mixed.bin"), stats);
  ASSERT_EQ(ticks.size(), 3U);
  EXPECT_EQ(stats.packets, 4U);
  EXPECT_EQ(stats.unsupported_segment, 1U);  // currency (cds) packet skipped

  const Tick& fut = ticks[0];  // 184-byte full packet
  EXPECT_EQ(fut.instrument_id, 101U);
  EXPECT_EQ(fut.source, vyuha::md::Source::kZerodha);
  EXPECT_EQ(fut.ltp, paise(2'445'005));
  EXPECT_EQ(fut.ltp.to_string(), "24450.0500");
  EXPECT_EQ((std::array{fut.ltq, fut.bid_qty, fut.ask_qty}), (std::array<std::uint32_t, 3>{75, 75, 65}));
  EXPECT_EQ(fut.volume, 1'234'500U);
  EXPECT_EQ(fut.oi, 9'876'500U);
  EXPECT_EQ(fut.exchange_ts, kTs);
  EXPECT_EQ(fut.last_trade_ts, kTs - vyuha::kNanosPerSecond);  // bytes 44–48: last-traded timestamp
  EXPECT_EQ(fut.recv_ts, kRecv);
  EXPECT_EQ((std::array{fut.open, fut.high, fut.low, fut.prev_close}),
            (std::array{paise(2'430'000), paise(2'450'000), paise(2'425'000), paise(2'428'000)}));
  EXPECT_EQ(fut.bid, paise(2'445'000));
  EXPECT_EQ(fut.ask, paise(2'445'005));
  EXPECT_EQ(fut.bids[4].price, paise(2'444'980));
  EXPECT_EQ(fut.bids[4].orders, 5U);
  EXPECT_EQ(fut.asks[4].qty, 325U);

  const Tick& idx = ticks[1];  // 32-byte index full: high/low/open/close order differs
  EXPECT_EQ(idx.instrument_id, 303U);
  EXPECT_EQ(
      (std::array{idx.ltp, idx.high, idx.low, idx.open, idx.prev_close}),
      (std::array{paise(2'440'015), paise(2'450'000), paise(2'420'000), paise(2'430'000), paise(2'428'000)}));
  EXPECT_EQ(idx.exchange_ts, kTs);
  EXPECT_EQ(idx.last_trade_ts, 0);  // indices don't trade

  const Tick& eq = ticks[2];  // 44-byte quote: no exchange time, so recv time stands in
  EXPECT_EQ(eq.ltp.to_string(), "2450.0500");
  EXPECT_EQ(eq.volume, 5'000'000U);
  EXPECT_EQ(eq.exchange_ts, kRecv);
  EXPECT_EQ(eq.last_trade_ts, 0);  // quote mode has no trade time: the core's TradeClock infers it
  EXPECT_EQ(eq.bid.units(), 0);
}

TEST(KiteParser, LtpHeartbeatAndUnknownTokens) {
  kite::ParseStats stats;
  EXPECT_EQ(parse(fixture("ltp_only.bin"), stats).size(), 2U);
  EXPECT_TRUE(parse(fixture("heartbeat.bin"), stats).empty());
  EXPECT_EQ(stats.heartbeats, 1U);

  kite::ParseStats s2;
  std::vector<Tick> out;
  kite::parse_frame(fixture("ltp_only.bin"), kite::TokenMap({{kNseEq, 7}}), kRecv, s2,
                    [&out](const Tick& t) { out.push_back(t); });
  EXPECT_EQ(out.size(), 1U);
  EXPECT_EQ(s2.unknown_token, 1U);
}

TEST(KiteParser, TruncatedAndInconsistentFramesNeverOverread) {
  const auto full = fixture("full_mixed.bin");
  // Every prefix of a valid frame: no crash (ASan) and nothing emitted past the cut.
  for (std::size_t cut = 0; cut < full.size(); ++cut) {
    kite::ParseStats stats;
    const std::vector<std::uint8_t> prefix(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(cut));
    const auto ticks = parse(prefix, stats);
    EXPECT_LE(ticks.size(), 3U);
    if (cut >= 2 && cut < full.size()) {
      EXPECT_GE(stats.malformed + stats.heartbeats, 1U) << cut;
    }
  }
  // Index layout on a tradable token and an odd packet length are rejected.
  Tick t;
  const std::vector<std::uint8_t> idx_layout_on_eq = {0x00, 0x0B, 0x45, 0x01, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0,
                                                      0,    0,    0,    0,    0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  EXPECT_EQ(kite::parse_packet(idx_layout_on_eq, tokens(), kRecv, t), kite::PacketResult::kMalformed);
  const std::vector<std::uint8_t> odd(12, 0);
  EXPECT_EQ(kite::parse_packet(odd, tokens(), kRecv, t), kite::PacketResult::kMalformed);
}

// F-MD-2 AC1: never exceed 3 000 tokens per connection or 3 connections per key.
TEST(KiteShards, LimitTable) {
  struct Case {
    std::size_t tokens;
    std::size_t shards;
    std::size_t last;
  };
  for (const Case c : {Case{1, 1, 1}, Case{3'000, 1, 3'000}, Case{3'001, 2, 1}, Case{6'500, 3, 500},
                       Case{9'000, 3, 3'000}}) {
    std::vector<kite::Subscription> subs;
    for (std::uint32_t i = 0; i < c.tokens; ++i) {
      subs.push_back({(i + 1) << 8U | 2U, kite::Mode::kFull});
    }
    const auto plan = kite::plan_shards(subs);
    ASSERT_TRUE(plan.has_value()) << c.tokens;
    ASSERT_EQ(plan->size(), c.shards) << c.tokens;
    for (const auto& s : *plan) {
      EXPECT_LE(s.subs.size(), 3'000U);
    }
    EXPECT_EQ(plan->back().subs.size(), c.last);
  }
  std::vector<kite::Subscription> too_many(9'001);
  for (std::uint32_t i = 0; i < too_many.size(); ++i) {
    too_many[i].token = i + 1;
  }
  const auto rejected = kite::plan_shards(too_many);
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(rejected.error().code, vyuha::ErrorCode::kCapacityExceeded);
}

TEST(KiteShards, DeduplicatesKeepingRichestModeAndBuildsMessages) {
  const std::vector<kite::Subscription> subs = {
      {408065, kite::Mode::kLtp}, {884737, kite::Mode::kQuote}, {408065, kite::Mode::kFull}};
  const auto plan = kite::plan_shards(subs);
  ASSERT_TRUE(plan.has_value());
  ASSERT_EQ(plan->at(0).subs.size(), 2U);
  EXPECT_EQ(kite::subscribe_messages(plan->at(0)),
            (std::vector<std::string>{R"({"a":"subscribe","v":[408065,884737]})",
                                      R"({"a":"mode","v":["quote",[884737]]})",
                                      R"({"a":"mode","v":["full",[408065]]})"}));
}

TEST(Backoff, DoublesToCapWithEqualJitter) {
  vyuha::Backoff b(vyuha::BackoffPolicy{}, 7);
  const vyuha::Nanos ms = 1'000'000;
  for (const vyuha::Nanos ceiling :
       {500 * ms, 1'000 * ms, 2'000 * ms, 4'000 * ms, 8'000 * ms, 16'000 * ms, 30'000 * ms, 30'000 * ms}) {
    const vyuha::Nanos d = b.next();
    EXPECT_GE(d, ceiling / 2);
    EXPECT_LE(d, ceiling);
  }
  b.reset();
  EXPECT_LE(b.next(), 500 * ms);
  for (int i = 0; i < 100; ++i) {
    EXPECT_LE(b.next(), 30'000 * ms);  // no overflow after many attempts
  }
}

TEST(SecretRef, HoldsAndMoves) {
  vyuha::SecretRef s("tok3n");
  EXPECT_EQ(s.reveal(), "tok3n");
  const vyuha::SecretRef moved = std::move(s);
  EXPECT_EQ(moved.reveal(), "tok3n");
  EXPECT_TRUE(s.empty());  // NOLINT(bugprone-use-after-move): moved-from state is specified
}

TEST(StaticIp, PreflightRequiresAddressOnHost) {
  EXPECT_TRUE(vyuha::net::check_ip_on_host("127.0.0.1").has_value());
  const auto missing = vyuha::net::check_ip_on_host("203.0.113.7");  // TEST-NET-3, never local
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(missing.error().code, vyuha::ErrorCode::kStaticIpNotOnHost);
  EXPECT_EQ(vyuha::net::check_ip_on_host("not-an-ip").error().code, vyuha::ErrorCode::kInvalidArgument);
}

}  // namespace
