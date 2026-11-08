// D-19 (ADR-027): an order-log outage reaches NATS `sys.alert` with no database anywhere. Needs a real NATS
// server (VYUHA_TEST_NATS_URL, else skipped); the Postgres URL points at a closed port. Runs in every preset,
// so it compares wire bytes: md_encode_reference_test checks the encoder against the generated classes.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include <nats/nats.h>
#include "require_services.h"

#include "vyuha/bus/md_encode.h"
#include "vyuha/bus/system_alert_publisher.h"
#include "vyuha/core/clock.h"
#include "vyuha/oms/oms.h"
#include "vyuha/oms/pg_order_store.h"

namespace {

using namespace std::chrono_literals;
using vyuha::SimClock;
using vyuha::bus::SystemAlertConfig;
using vyuha::bus::SystemAlertPublisher;

constexpr const char* kNoDatabase = "postgresql://vyuha@127.0.0.1:1/none?connect_timeout=1";
constexpr vyuha::Nanos kStart = 1'793'190'900'000'000'000;

std::string nats_url() {
  const char* u = std::getenv("VYUHA_TEST_NATS_URL");  // NOLINT(concurrency-mt-unsafe): test setup
  return u != nullptr ? u : "";
}

class Listener {
 public:
  Listener(const std::string& url, const std::string& subject) {
    EXPECT_EQ(natsConnection_ConnectTo(&nc_, url.c_str()), NATS_OK);
    EXPECT_EQ(natsConnection_SubscribeSync(&sub_, nc_, subject.c_str()), NATS_OK);
    EXPECT_EQ(natsConnection_Flush(nc_), NATS_OK);
  }
  Listener(const Listener&) = delete;
  Listener& operator=(const Listener&) = delete;
  Listener(Listener&&) = delete;
  Listener& operator=(Listener&&) = delete;
  ~Listener() {
    natsSubscription_Destroy(sub_);
    natsConnection_Destroy(nc_);
  }

  // Empty if nothing arrives in time, so the comparison fails.
  [[nodiscard]] std::string next() {
    std::string bytes;
    natsMsg* msg = nullptr;
    if (natsSubscription_NextMsg(&msg, sub_, 10'000) == NATS_OK) {
      bytes.assign(natsMsg_GetData(msg), static_cast<std::size_t>(natsMsg_GetDataLength(msg)));
      natsMsg_Destroy(msg);
    }
    return bytes;
  }

 private:
  natsConnection* nc_{nullptr};
  natsSubscription* sub_{nullptr};
};

std::string wire(bool active) {
  std::array<std::uint8_t, vyuha::bus::kMaxSystemAlertBytes> buf{};
  const std::size_t n = vyuha::bus::encode_system_alert({.code = "ORDER_LOG_OUTAGE",
                                                         .severity = "critical",
                                                         .banner = "ORDER LOG DOWN: new orders blocked",
                                                         .active = active,
                                                         .since_ms = kStart / 1'000'000},
                                                        buf);
  return {buf.begin(), std::next(buf.begin(), static_cast<std::ptrdiff_t>(n))};
}

template <typename F>
bool eventually(F done) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!done() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(5ms);
  }
  return done();
}

vyuha::oms::OrderDetails an_order() {
  vyuha::oms::OrderDetails d{.is_paper = vyuha::oms::kPaperOrder};
  d.id.bytes.fill(7);
  EXPECT_TRUE(d.client_order_id.assign("outage-signal"));
  d.qty = 1;
  d.price = vyuha::Price::from_units(1'000'000);
  return d;
}

TEST(OutageSignal, AnOrderLogWithNoDatabaseRaisesTheAlertAndKeepsRepeatingIt) {
  const std::string url = nats_url();
  if (url.empty()) {
    VYUHA_SKIP_WITHOUT_SERVICE("VYUHA_TEST_NATS_URL not set");
  }
  const std::string subject = "sys.alert.test.repeat";  // tests run in parallel
  Listener listener(url, subject);
  const SimClock clock{kStart};
  SystemAlertPublisher publisher(SystemAlertConfig{.url = url, .subject = subject, .repeat = 100ms}, clock);
  ASSERT_TRUE(publisher.start());
  {
    vyuha::oms::PgOrderEventWriter writer(kNoDatabase, &publisher);
    vyuha::oms::Oms oms(writer, clock, 1);
    ASSERT_TRUE(oms.create(an_order()));

    EXPECT_EQ(listener.next(), wire(true));
    EXPECT_FALSE(writer.healthy());
    EXPECT_EQ(listener.next(), wire(true));  // repeated: a gateway that restarts mid-outage still hears it
  }
}

TEST(OutageSignal, TheEndOfAnOutageClearsItWithTheSameIdentity) {
  const std::string url = nats_url();
  if (url.empty()) {
    VYUHA_SKIP_WITHOUT_SERVICE("VYUHA_TEST_NATS_URL not set");
  }
  const std::string subject = "sys.alert.test.clear";  // tests run in parallel
  Listener listener(url, subject);
  const SimClock clock{kStart};
  SystemAlertPublisher publisher(SystemAlertConfig{.url = url, .subject = subject, .repeat = 10s}, clock);
  ASSERT_TRUE(publisher.start());
  publisher.outage_started();
  publisher.outage_started();  // the same outage: not a second alert
  publisher.outage_ended();
  EXPECT_EQ(listener.next(), wire(true));
  EXPECT_EQ(listener.next(), wire(false));
  EXPECT_TRUE(eventually([&] { return publisher.published() == 2U; }));
}

// NATS down too (D-19): the engine must start and the signal must not block the writer thread.
TEST(OutageSignal, NatsDownNeitherFailsStartNorBlocks) {
  const SimClock clock{kStart};
  SystemAlertPublisher publisher(
      SystemAlertConfig{.url = "nats://127.0.0.1:1", .subject = "sys.alert.test.down", .repeat = 10s}, clock);
  ASSERT_TRUE(publisher.start());
  const auto begin = std::chrono::steady_clock::now();
  publisher.outage_started();
  publisher.outage_ended();
  EXPECT_LT(std::chrono::steady_clock::now() - begin, 100ms);
  // Counted, and not buffered to replay when NATS comes back.
  EXPECT_TRUE(eventually([&] { return publisher.publish_errors() == 2U; }));
  EXPECT_EQ(publisher.published(), 0U);
}

TEST(OutageSignal, APublisherCannotBeRestarted) {
  const SimClock clock{kStart};
  SystemAlertPublisher publisher(
      SystemAlertConfig{.url = "nats://127.0.0.1:1", .subject = "sys.alert.test.restart"}, clock);
  ASSERT_TRUE(publisher.start());
  publisher.stop();
  EXPECT_FALSE(publisher.start());
}

}  // namespace
