// MdPublisher against a real NATS server (`make up`): VYUHA_TEST_NATS_URL, else skipped.
// Every tick handed to the publisher reaches subscribers (count = published_ticks), bars too, and
// the KV bucket holds the latest snapshot. A full ring drops (counted) instead of blocking.

#include "vyuha/bus/md_publisher.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include <gtest/gtest.h>
#include <nats/nats.h>

#include "vyuha/core/clock.h"

namespace {

std::string nats_url() {
  const char* u = std::getenv("VYUHA_TEST_NATS_URL");  // NOLINT(concurrency-mt-unsafe): test setup
  return u != nullptr ? u : "";
}

vyuha::md::Tick tick(std::uint64_t id, std::int64_t paise) {
  vyuha::md::Tick t;
  t.instrument_id = id;
  t.ltp = vyuha::Price::from_paise(paise);
  t.prev_close = vyuha::Price::from_paise(10'000);
  t.exchange_ts = 1'793'190'900'000'000'000;  // 2026-10-28 12:35 UTC (inside the ticks TTL)
  return t;
}

TEST(MdPublisher, PublishesEveryTickBarAndLatestSnapshot) {
  const std::string url = nats_url();
  if (url.empty()) {
    GTEST_SKIP() << "VYUHA_TEST_NATS_URL not set";
  }
  natsConnection* nc = nullptr;
  ASSERT_EQ(natsConnection_ConnectTo(&nc, url.c_str()), NATS_OK);
  // Unique ids per run so concurrent runs/old KV entries don't interfere.
  const auto base =
      static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count() % 1'000'000) *
          1000 +
      1;
  natsSubscription* ticks_sub = nullptr;
  natsSubscription* bars_sub = nullptr;
  ASSERT_EQ(natsConnection_SubscribeSync(&ticks_sub, nc, "md.tick.>"), NATS_OK);
  ASSERT_EQ(natsConnection_SubscribeSync(&bars_sub, nc, "md.bar.1m.>"), NATS_OK);
  ASSERT_EQ(natsSubscription_SetPendingLimits(ticks_sub, -1, -1), NATS_OK);
  ASSERT_EQ(natsConnection_Flush(nc), NATS_OK);

  vyuha::SimClock clock(1'000'000'000);
  vyuha::bus::MdPublisher pub(vyuha::bus::PublisherConfig{.url = url, .kv_bucket = "md_last_test"}, clock);
  ASSERT_TRUE(pub.start().has_value());
  constexpr int kTicks = 20'000;
  for (int i = 0; i < kTicks; ++i) {
    pub.on_tick(tick(base + static_cast<std::uint64_t>(i % 10), 10'000 + i));
    if (i % 1'000 == 999) {
      clock.advance(vyuha::kNanosPerSecond);  // lets KV snapshots through the throttle
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }
  vyuha::md::Bar bar;
  bar.instrument_id = base;
  bar.final = true;
  pub.on_bar(bar);
  pub.stop();  // drains the ring and flushes
  const auto st = pub.stats();
  EXPECT_EQ(st.dropped, 0U);
  EXPECT_EQ(st.published_ticks, static_cast<std::uint64_t>(kTicks));
  EXPECT_EQ(st.published_bars, 1U);
  EXPECT_GT(st.kv_puts, 0U);

  int got = 0;
  natsMsg* msg = nullptr;
  while (natsSubscription_NextMsg(&msg, ticks_sub, 500) == NATS_OK) {
    const std::string subj = natsMsg_GetSubject(msg);
    got += subj.starts_with("md.tick.") ? 1 : 0;
    natsMsg_Destroy(msg);
  }
  EXPECT_EQ(got, kTicks);
  ASSERT_EQ(natsSubscription_NextMsg(&msg, bars_sub, 1000), NATS_OK);
  EXPECT_EQ(std::string(natsMsg_GetSubject(msg)), "md.bar.1m." + std::to_string(base));
  natsMsg_Destroy(msg);

  jsCtx* js = nullptr;
  kvStore* kv = nullptr;
  kvEntry* e = nullptr;
  ASSERT_EQ(natsConnection_JetStream(&js, nc, nullptr), NATS_OK);
  ASSERT_EQ(js_KeyValue(&kv, js, "md_last_test"), NATS_OK);
  ASSERT_EQ(kvStore_Get(&e, kv, std::to_string(base).c_str()), NATS_OK);
  EXPECT_GT(kvEntry_ValueLen(e), 0);
  kvEntry_Destroy(e);
  kvStore_Destroy(kv);
  EXPECT_EQ(js_DeleteKeyValue(js, "md_last_test"), NATS_OK);
  jsCtx_Destroy(js);
  natsSubscription_Destroy(ticks_sub);
  natsSubscription_Destroy(bars_sub);
  natsConnection_Destroy(nc);
}

TEST(MdPublisher, FullRingDropsAndCountsInsteadOfBlocking) {
  const vyuha::SimClock clock(1);
  vyuha::bus::MdPublisher pub(vyuha::bus::PublisherConfig{.url = "nats://127.0.0.1:1"},
                              clock);  // never started
  for (int i = 0; i < 40'000; ++i) {
    pub.on_tick(tick(1, 100));
  }
  const auto st = pub.stats();
  EXPECT_EQ(st.enqueued_ticks, 32'768U);  // ring capacity
  EXPECT_EQ(st.dropped, 40'000U - 32'768U);
  EXPECT_FALSE(
      vyuha::bus::MdPublisher(vyuha::bus::PublisherConfig{.url = "nats://127.0.0.1:1"}, clock).start());
}

}  // namespace
