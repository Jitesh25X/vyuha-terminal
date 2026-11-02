// Persist before send (ADR-025), and recovery from each status.

#include "vyuha/oms/order_router.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "vyuha/core/clock.h"

namespace vyuha::oms {
namespace {

using enum OrderStatus;
using E = OrderEvent;
using Recovery = OrderRouter::Recovery;
using BrokerState = BrokerOrderView::State;

class FixedClock final : public IClock {
 public:
  [[nodiscard]] Nanos now() const noexcept override { return 1'000; }
};

class LaggingSink final : public IOrderEventSink {
 public:
  // NOLINTNEXTLINE(bugprone-exception-escape): a test's allocation failure may terminate
  [[nodiscard]] bool try_push(const OrderEventRecord& event) noexcept override {
    if (full) {
      return false;
    }
    events.push_back(event);
    return true;
  }
  [[nodiscard]] bool healthy() const noexcept override { return is_healthy; }
  [[nodiscard]] std::uint64_t pushed() const noexcept override { return events.size(); }
  [[nodiscard]] std::uint64_t durable() const noexcept override { return durable_count; }
  std::vector<OrderEventRecord> events;
  std::uint64_t durable_count{0};
  bool full{false};
  bool is_healthy{true};
};

class FakeBroker final : public IOrderSender, public IBrokerOrderBook {
 public:
  // NOLINTNEXTLINE(bugprone-exception-escape): a test's allocation failure may terminate
  [[nodiscard]] bool send(const Order& /*order*/, std::string_view tag) noexcept override {
    if (busy) {
      return false;
    }
    tags.emplace_back(tag);
    return true;
  }
  [[nodiscard]] std::vector<BrokerOrderView> find_by_tag(std::string_view tag) override {
    std::vector<BrokerOrderView> out;
    for (const auto& t : tags) {
      if (t == tag) {
        out.push_back(view);
      }
    }
    return out;
  }
  std::vector<std::string> tags;
  BrokerOrderView view;
  bool busy{false};
};

constexpr std::size_t kTagLimit = 20;
constexpr std::string_view kClient = "0190a3b4-7c2d-7e11-8f00-aabbccddeeff";
std::string tag_of_client() { return broker_tag(kClient, kTagLimit); }

OrderDetails order_of(std::int32_t qty, std::string_view client = kClient) {
  OrderDetails d;
  d.id.bytes.fill(1);
  EXPECT_TRUE(d.client_order_id.assign(client));
  d.qty = qty;
  d.price = Price::from_units(1'000'000);
  return d;
}

struct Rig {
  FixedClock clock;
  LaggingSink sink;
  FakeBroker broker;
  Oms oms{sink, clock, 8};
  OrderRouter router{oms, sink, broker, kTagLimit, 8};

  OrderHandle approved(std::int32_t qty = 10) {
    const OrderHandle h = oms.create(order_of(qty)).value();
    EXPECT_TRUE(oms.apply(h, E::kRiskApprove).has_value());
    return h;
  }
  OrderHandle restored(OrderStatus status, std::int32_t filled = 0, std::int32_t qty = 10) {
    RecoveredOrder r;
    r.details = order_of(qty);
    r.status = status;
    r.seq = 3;
    r.filled_qty = filled;
    return oms.restore(r).value();
  }
};

TEST(BrokerTag, KeepsLettersAndDigitsAndTheTail) {
  EXPECT_EQ(broker_tag("0190a3b4-7c2d-7e11-8f00-aabbccddeeff", 20), "7e118f00aabbccddeeff");
  EXPECT_EQ(broker_tag("abc-1", 20), "abc1");
  EXPECT_EQ(broker_tag("abcdef", 3), "def");
}

TEST(OrderRouter, NothingIsSentUntilApprovedIsDurable) {
  Rig r;
  const OrderHandle h = r.approved();
  ASSERT_TRUE(r.router.submit(h));
  EXPECT_EQ(r.sink.events.size(), 2U);  // NEW, APPROVED

  r.sink.durable_count = 1;  // NEW only
  EXPECT_EQ(r.router.poll(), 0U);
  EXPECT_TRUE(r.broker.tags.empty());
  EXPECT_EQ(r.oms.order(h).status, kApproved);

  r.sink.durable_count = 2;
  EXPECT_EQ(r.router.poll(), 1U);
  ASSERT_EQ(r.broker.tags.size(), 1U);
  EXPECT_EQ(r.broker.tags[0], tag_of_client());
  EXPECT_EQ(r.oms.order(h).status, kSent);
  EXPECT_EQ(r.router.pending(), 0U);

  r.sink.durable_count = 3;
  EXPECT_EQ(r.router.poll(), 0U);  // and only once
  EXPECT_EQ(r.broker.tags.size(), 1U);
}

TEST(OrderRouter, SendsNothingWhileTheWriterIsUnhealthy) {
  Rig r;
  const OrderHandle h = r.approved();
  ASSERT_TRUE(r.router.submit(h));
  r.sink.durable_count = 2;
  r.sink.is_healthy = false;
  EXPECT_EQ(r.router.poll(), 0U);
  EXPECT_TRUE(r.broker.tags.empty());
  r.sink.is_healthy = true;
  EXPECT_EQ(r.router.poll(), 1U);
}

TEST(OrderRouter, ABusyAdapterIsRetried) {
  Rig r;
  const OrderHandle h = r.approved();
  ASSERT_TRUE(r.router.submit(h));
  r.sink.durable_count = 2;
  r.broker.busy = true;
  EXPECT_EQ(r.router.poll(), 0U);
  EXPECT_EQ(r.oms.order(h).status, kApproved);
  r.broker.busy = false;
  EXPECT_EQ(r.router.poll(), 1U);
  EXPECT_EQ(r.oms.order(h).status, kSent);
}

TEST(OrderRouter, AFullSinkAfterSendingNeverSendsAgain) {
  Rig r;
  const OrderHandle h = r.approved();
  ASSERT_TRUE(r.router.submit(h));
  r.sink.durable_count = 2;
  r.sink.full = true;
  EXPECT_EQ(r.router.poll(), 1U);
  EXPECT_EQ(r.oms.order(h).status, kApproved);  // the adapter has it; recording waits
  EXPECT_EQ(r.router.pending(), 1U);
  EXPECT_EQ(r.router.poll(), 0U);
  r.sink.full = false;
  EXPECT_EQ(r.router.poll(), 0U);
  EXPECT_EQ(r.oms.order(h).status, kSent);
  EXPECT_EQ(r.broker.tags.size(), 1U);
}

TEST(OrderRouter, OrdersGoOutInOrder) {
  Rig r;
  const OrderHandle a = r.approved();
  const OrderHandle b = r.oms.create(order_of(5, "second-order")).value();
  ASSERT_TRUE(r.oms.apply(b, E::kRiskApprove));
  ASSERT_TRUE(r.router.submit(a));
  ASSERT_TRUE(r.router.submit(b));
  r.sink.durable_count = 4;
  EXPECT_EQ(r.router.poll(), 2U);
  ASSERT_EQ(r.broker.tags.size(), 2U);
  EXPECT_EQ(r.broker.tags[1], "secondorder");
}

TEST(OrderRouter, AnOrderThatMovedOnWhileWaitingIsNotSent) {
  Rig r;
  const OrderHandle h = r.oms.create(order_of(1)).value();
  ASSERT_TRUE(r.oms.apply(h, E::kRiskApprove));
  ASSERT_TRUE(r.router.submit(h));
  EXPECT_FALSE(r.router.submit(99).has_value());
  r.sink.durable_count = 2;
  ASSERT_TRUE(r.oms.record_sent(h));  // something else moved it
  EXPECT_EQ(r.router.poll(), 0U);
  EXPECT_TRUE(r.broker.tags.empty());
  EXPECT_EQ(r.router.pending(), 0U);
}

TEST(OrderRouterRecovery, ApprovedAndNotAtTheBrokerIsSentOnce) {
  Rig r;
  const OrderHandle h = r.restored(kApproved);
  EXPECT_EQ(r.router.recover(h, r.broker), Recovery::kQueued);
  EXPECT_EQ(r.router.poll(), 1U);
  EXPECT_EQ(r.broker.tags.size(), 1U);
  EXPECT_EQ(r.oms.order(h).status, kSent);
}

TEST(OrderRouterRecovery, ApprovedAndAtTheBrokerIsAdoptedNotSent) {
  Rig r;
  r.broker.tags = {tag_of_client()};
  r.broker.view = {.state = BrokerState::kOpen, .broker_order_id = "B1"};
  const OrderHandle h = r.restored(kApproved);
  EXPECT_EQ(r.router.recover(h, r.broker), Recovery::kResolved);
  EXPECT_EQ(r.router.poll(), 0U);
  EXPECT_EQ(r.broker.tags.size(), 1U);
  EXPECT_EQ(r.oms.order(h).status, kOpen);
  EXPECT_EQ(r.oms.order(h).broker_order_id.view(), "B1");
  EXPECT_FALSE(r.oms.order(h).needs_reconcile);
}

TEST(OrderRouterRecovery, SentAndNotAtTheBrokerIsNeverResent) {
  Rig r;
  const OrderHandle h = r.restored(kSent);
  EXPECT_EQ(r.router.recover(h, r.broker), Recovery::kResolved);
  EXPECT_EQ(r.router.poll(), 0U);
  EXPECT_TRUE(r.broker.tags.empty());
  EXPECT_EQ(r.oms.order(h).status, kBrokerRejected);
  EXPECT_EQ(r.sink.events.back().reason_code.view(), kReconMismatch);
}

TEST(OrderRouterRecovery, UnknownTakesTheBrokersState) {
  for (const auto& [state, filled, want] :
       {std::tuple{BrokerState::kOpen, 0, kOpen},
        std::tuple{BrokerState::kPartiallyFilled, 4, kPartiallyFilled},
        std::tuple{BrokerState::kFilled, 10, kFilled}, std::tuple{BrokerState::kCancelled, 4, kCancelled},
        std::tuple{BrokerState::kRejected, 0, kBrokerRejected}}) {
    Rig r;
    r.broker.tags = {tag_of_client()};
    r.broker.view = {.state = state, .filled_qty = filled, .average_price = Price::from_units(10'000)};
    const OrderHandle h = r.restored(kUnknown);
    EXPECT_EQ(r.router.recover(h, r.broker), Recovery::kResolved);
    EXPECT_EQ(r.oms.order(h).status, want);
    EXPECT_EQ(r.oms.order(h).filled_qty, filled);
    EXPECT_EQ(r.sink.events.back().fill_qty, filled);  // for the position projector
    EXPECT_EQ(r.router.poll(), 0U);
    EXPECT_EQ(r.broker.tags.size(), 1U);
  }
}

TEST(OrderRouterRecovery, NewNeverReachedTheBrokerAndIsRejected) {
  Rig r;
  const OrderHandle h = r.restored(kNew);
  EXPECT_EQ(r.router.recover(h, r.broker), Recovery::kResolved);
  EXPECT_EQ(r.oms.order(h).status, kRiskRejected);
  EXPECT_TRUE(r.broker.tags.empty());
}

TEST(OrderRouterRecovery, TwoOrdersWithOneTagAreNeverGuessedAt) {
  Rig r;
  r.broker.tags = {tag_of_client(), tag_of_client()};
  const OrderHandle h = r.restored(kApproved);
  EXPECT_EQ(r.router.recover(h, r.broker), Recovery::kUnresolved);
  EXPECT_EQ(r.router.poll(), 0U);
  EXPECT_EQ(r.broker.tags.size(), 2U);
  EXPECT_EQ(r.oms.recon_mismatches(), 1U);
  EXPECT_TRUE(r.oms.order(h).needs_reconcile);
}

TEST(OrderRouterRecovery, AnOpenOrderCatchesUpOnWhatItMissed) {
  Rig r;
  r.broker.tags = {tag_of_client()};
  r.broker.view = {
      .state = BrokerState::kCancelled, .filled_qty = 6, .average_price = Price::from_units(10'000)};
  const OrderHandle h = r.restored(kPartiallyFilled, 2);
  EXPECT_EQ(r.router.recover(h, r.broker), Recovery::kResolved);
  EXPECT_EQ(r.oms.order(h).filled_qty, 6);
  EXPECT_EQ(r.oms.order(h).status, kCancelled);
  EXPECT_EQ(r.oms.recon_mismatches(), 0U);
}

TEST(OrderRouterRecovery, PendingStatesResolve) {
  {
    Rig r;
    r.broker.tags = {tag_of_client()};
    r.broker.view = {.state = BrokerState::kOpen};
    const OrderHandle h = r.restored(kCancelPending);
    EXPECT_EQ(r.router.recover(h, r.broker), Recovery::kResolved);
    EXPECT_EQ(r.oms.order(h).status, kOpen);
  }
  {
    Rig r;
    r.broker.tags = {tag_of_client()};
    r.broker.view = {.state = BrokerState::kCancelled};
    const OrderHandle h = r.restored(kCancelPending);
    EXPECT_EQ(r.router.recover(h, r.broker), Recovery::kResolved);
    EXPECT_EQ(r.oms.order(h).status, kCancelled);
  }
  {
    Rig r;
    r.broker.tags = {tag_of_client()};
    r.broker.view = {.state = BrokerState::kCancelled};
    const OrderHandle h = r.restored(kModifyPending);
    EXPECT_EQ(r.router.recover(h, r.broker), Recovery::kResolved);
    EXPECT_EQ(r.oms.order(h).status, kCancelled);
  }
}

TEST(OrderRouterRecovery, AnOpenOrderTheBrokerLacksIsAMismatch) {
  Rig r;
  const OrderHandle h = r.restored(kOpen);
  EXPECT_EQ(r.router.recover(h, r.broker), Recovery::kUnresolved);
  EXPECT_EQ(r.oms.recon_mismatches(), 1U);
  EXPECT_EQ(r.oms.order(h).status, kOpen);
  EXPECT_TRUE(r.oms.order(h).needs_reconcile);
}

}  // namespace
}  // namespace vyuha::oms
