#pragma once
// Test-only frame builders using the protoc-generated reference classes for the vendored
// MarketDataFeed.proto: the engine's hand-written decoder is checked against Upstox's own schema.

#include <cstdint>
#include <string>
#include <vector>

#include "MarketDataFeed.pb.h"

namespace vyuha::testing::upstox {

namespace pb = com::upstox::marketdatafeederv3udapi::rpc::proto;

constexpr std::int64_t kTsMs = 1'793'190'900'000;  // 2026-10-28 12:35:00 UTC (18:05 IST)

inline void set_ltpc(pb::LTPC* l, double ltp, std::int64_t ltt_ms, std::int64_t ltq, double cp) {
  l->set_ltp(ltp);
  l->set_ltt(ltt_ms);
  l->set_ltq(ltq);
  l->set_cp(cp);
}

// NIFTY future full feed (30 depth levels as in full_d30) + index feed + LTPC-only equity.
inline std::string live_frame() {
  pb::FeedResponse r;
  r.set_type(pb::live_feed);
  r.set_currentts(kTsMs + 500);
  {
    pb::Feed& f = (*r.mutable_feeds())["NSE_FO|61471"];
    auto* ff = f.mutable_fullfeed()->mutable_marketff();
    set_ltpc(ff->mutable_ltpc(), 24450.05, kTsMs, 75, 24280.0);
    for (int i = 0; i < 30; ++i) {
      auto* q = ff->mutable_marketlevel()->add_bidaskquote();
      q->set_bidq(75 * (i + 1));
      q->set_bidp(24450.0 - 0.05 * i);
      q->set_askq(65 * (i + 1));
      q->set_askp(24450.05 + 0.05 * i);
    }
    auto* intraday = ff->mutable_marketohlc()->add_ohlc();
    intraday->set_interval("I1");
    intraday->set_open(1);
    intraday->set_high(1);
    intraday->set_low(1);
    auto* day = ff->mutable_marketohlc()->add_ohlc();
    day->set_interval("1d");
    day->set_open(24300.0);
    day->set_high(24500.0);
    day->set_low(24250.0);
    ff->set_atp(24400.0);
    ff->set_vtt(1'234'500);
    ff->set_oi(9'876'500.0);
    f.set_requestmode(pb::full_d30);
  }
  {
    pb::Feed& f = (*r.mutable_feeds())["NSE_INDEX|Nifty 50"];
    auto* idx = f.mutable_fullfeed()->mutable_indexff();
    set_ltpc(idx->mutable_ltpc(), 24400.15, kTsMs, 0, 24280.0);
    auto* day = idx->mutable_marketohlc()->add_ohlc();
    day->set_interval("1d");
    day->set_open(24300.0);
    day->set_high(24500.0);
    day->set_low(24200.0);
  }
  {
    pb::Feed& f = (*r.mutable_feeds())["NSE_EQ|INE002A01018"];
    set_ltpc(f.mutable_ltpc(), 2450.05, 0, 10, 2438.0);  // ltt 0 → currentTs is used
  }
  return r.SerializeAsString();
}

inline std::string market_info_frame() {
  pb::FeedResponse r;
  r.set_type(pb::market_info);
  r.set_currentts(kTsMs);
  (*r.mutable_marketinfo()->mutable_segmentstatus())["NSE_FO"] = pb::NORMAL_OPEN;
  return r.SerializeAsString();
}

inline std::vector<std::uint8_t> bytes(const std::string& s) { return {s.begin(), s.end()}; }

}  // namespace vyuha::testing::upstox
