// Decoder vs Upstox's own schema (protoc-generated classes). Built only without sanitizers:
// vcpkg's abseil/protobuf are not sanitizer-instrumented and abseil's layouts differ under ASan.

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include <gtest/gtest.h>

#include "upstox_test_frames.h"
#include "vyuha/broker/upstox/feed_parser.h"

namespace {

namespace up = vyuha::broker::upstox;
namespace tf = vyuha::testing::upstox;
using vyuha::md::Tick;

up::KeyMap keys() {
  return up::KeyMap({{"NSE_FO|61471", 101}, {"NSE_INDEX|Nifty 50", 303}, {"NSE_EQ|INE002A01018", 202}});
}

std::vector<Tick> decode(const std::vector<std::uint8_t>& frame, up::DecodeStats& stats) {
  std::vector<Tick> out;
  up::decode_frame(frame, keys(), 7, stats, [&out](const Tick& t) { out.push_back(t); });
  return out;
}

std::filesystem::path fixture_dir() { return std::filesystem::path(VYUHA_FIXTURES_DIR) / "vcr" / "upstox"; }

std::vector<std::uint8_t> read(const char* name) {
  std::ifstream f(fixture_dir() / name, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}

TEST(UpstoxReference, FixturesMatchTheSchemaEncoding) {
  // Regenerate: VYUHA_WRITE_FIXTURES=1 ./vyuha_reference_tests (getenv is fine: single-threaded here)
  if (std::getenv("VYUHA_WRITE_FIXTURES") != nullptr) {  // NOLINT(concurrency-mt-unsafe)
    const auto dir = fixture_dir();
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "live_feed.bin", std::ios::binary) << tf::live_frame();
    std::ofstream(dir / "market_info.bin", std::ios::binary) << tf::market_info_frame();
  }
  // The committed bytes parse with the reference classes, and our decoder reads a fresh reference
  // encoding the same way it reads the fixture (map order may differ: compare per instrument).
  tf::pb::FeedResponse parsed;
  const auto fixture = read("live_feed.bin");
  ASSERT_TRUE(parsed.ParseFromArray(fixture.data(), static_cast<int>(fixture.size())));
  EXPECT_EQ(parsed.feeds_size(), 3);
  up::DecodeStats a;
  up::DecodeStats b;
  auto from_fixture = decode(fixture, a);
  auto from_reference = decode(tf::bytes(tf::live_frame()), b);
  ASSERT_EQ(from_fixture.size(), 3U);
  ASSERT_EQ(from_reference.size(), 3U);
  const auto by_id = [](std::vector<Tick>& v) {
    std::ranges::sort(v, {}, &Tick::instrument_id);
    return v;
  };
  by_id(from_fixture);
  by_id(from_reference);
  for (std::size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(from_fixture[i].ltp, from_reference[i].ltp);
    EXPECT_EQ(from_fixture[i].volume, from_reference[i].volume);
    EXPECT_EQ(from_fixture[i].bids[4].price, from_reference[i].bids[4].price);
  }
  // Values agree with the reference parse of the same bytes.
  const auto& fut = parsed.feeds().at("NSE_FO|61471").fullfeed().marketff();
  EXPECT_EQ(from_fixture[0].ltp.units(), std::llround(fut.ltpc().ltp() * 10'000));
  EXPECT_EQ(from_fixture[0].volume, static_cast<std::uint64_t>(fut.vtt()));
}

TEST(UpstoxReference, BadPricesAndUnknownKeysAreDropped) {
  tf::pb::FeedResponse r;
  r.set_type(tf::pb::live_feed);
  tf::set_ltpc((*r.mutable_feeds())["NSE_FO|99999"].mutable_ltpc(), 10.0, 0, 1, 9.0);  // not subscribed
  tf::set_ltpc((*r.mutable_feeds())["NSE_FO|61471"].mutable_ltpc(), std::nan(""), 0, 1, 9.0);  // NaN price
  tf::set_ltpc((*r.mutable_feeds())["NSE_EQ|INE002A01018"].mutable_ltpc(), -1.0, 0, 1, 9.0);   // negative
  up::DecodeStats stats;
  EXPECT_TRUE(decode(tf::bytes(r.SerializeAsString()), stats).empty());
  EXPECT_EQ(stats.unknown_key, 1U);
  EXPECT_EQ(stats.bad_price, 2U);
}

}  // namespace
