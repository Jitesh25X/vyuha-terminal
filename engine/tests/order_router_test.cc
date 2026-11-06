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
  [[nodiscard]] bool try_pop_refused(Uuid& order_id) noexcept override {
    if (refused.empty()) {
      return false;
    }
    order_id = refused.back();
    refused.pop_back();
    return true;
  }
  std::vector<Uuid> refused;
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
  // NOLINTNEXTLINE(bugprone-exception-escape): a test's allocation failure may terminate
  [[nodiscard]] bool cancel(const Order& order) noexcept override {
    if (busy) {
      return false;
    }
    cancels.push_back(order.details.id);
    return true;
  }
  [[nodiscard]] std::expected<std::vector<BrokerOrderView>, Error> find_by_tag(
      std::string_view tag) override {
    if (book_down) {
      return std::unexpected(Error{ErrorCode::kInvalidArgument, "test: book down"});
    }
    std::vector<BrokerOrderView> out;
    for (const auto& t : tags) {
      if (t == tag) {
        out.push_back(view);
      }
    }
    return out;
  }
  std::vector<std::string> tags;
  std::vector<Uuid> cancels;
  BrokerOrderView view;
  bool busy{false};
  bool book_down{false};
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

TEST(OrderRouter, AnOrderStorageRefusedIsNeverSentThoughItsEventsCountAsDurable) {
  Rig r;
  const OrderHandle h = r.approved();
  ASSERT_TRUE(r.router.submit(h));
  r.sink.refused.push_back(r.oms.order(h).details.id);
  r.sink.durable_count = 2;
  EXPECT_EQ(r.router.poll(), 0U);
  EXPECT_TRUE(r.broker.tags.empty());
  EXPECT_EQ(r.oms.order(h).status, kRiskRejected);
  EXPECT_EQ(r.oms.refused_by_storage(), 1U);
  EXPECT_EQ(r.router.pending(), 0U);
}

// The owner's rule: one bad request never stops a kill-switch cancel.
TEST(OrderRouter, ADuplicateThenCancelAllStillGoesThrough) {
  Rig r;
  const OrderHandle live = r.approved();
  ASSERT_TRUE(r.router.submit(live));
  r.sink.durable_count = 2;
  ASSERT_EQ(r.router.poll(), 1U);
  ASSERT_TRUE(r.oms.apply(live, E::kBrokerAck));

  OrderDetails other = order_of(99);  // the same client_order_id, a different body
  other.id.bytes.fill(2);
  EXPECT_EQ(r.oms.intake(other).error().code, ErrorCode::kOrderDup);

  const auto out = r.router.cancel_all();
  EXPECT_EQ(out.asked, 1U);
  EXPECT_EQ(out.failed, 0U);
  ASSERT_EQ(r.broker.cancels.size(), 1U);
  EXPECT_EQ(r.broker.cancels[0], r.oms.order(live).details.id);
  EXPECT_EQ(r.oms.order(live).status, kCancelPending);
}

TEST(OrderRouter, CancelAllIgnoresTheStateOfTheOrderLog) {
  Rig r;
  const OrderHandle live = r.approved();
  ASSERT_TRUE(r.router.submit(live));
  r.sink.durable_count = 2;
  ASSERT_EQ(r.router.poll(), 1U);
  ASSERT_TRUE(r.oms.apply(live, E::kBrokerAck));
  const OrderHandle queued = r.oms.create(order_of(5, "queued-order")).value();
  ASSERT_TRUE(r.oms.apply(queued, E::kRiskApprove));
  ASSERT_TRUE(r.router.submit(queued));
  const OrderHandle restored = r.restored(kPartiallyFilled, 1);

  r.sink.is_healthy = false;  // Postgres down, the ring full, and durable() far behind
  r.sink.full = true;
  const auto out = r.router.cancel_all();
  EXPECT_EQ(out.asked, 2U);  // the live order and the restored one, reconciled or not
  EXPECT_EQ(r.broker.cancels.size(), 2U);
  EXPECT_EQ(r.oms.order(restored).status, kPartiallyFilled);  // the ring is full; the broker has been asked

  r.sink.is_healthy = true;
  r.sink.full = false;
  r.sink.durable_count = 1'000;
  EXPECT_EQ(r.router.poll(), 0U);
  EXPECT_EQ(r.broker.tags.size(), 1U);  // the queued order never goes out
  EXPECT_EQ(r.router.pending(), 0U);
}

TEST(OrderRouter, ABusyAdapterIsReportedSoCancelAllCanBeRepeated) {
  Rig r;
  const OrderHandle h = r.restored(kOpen);
  r.broker.busy = true;
  const auto out = r.router.cancel_all();
  EXPECT_EQ(out.asked, 1U);
  EXPECT_EQ(out.failed, 1U);
  EXPECT_EQ(r.oms.order(h).status, kOpen);
  r.broker.busy = false;
  EXPECT_EQ(r.router.cancel_all().failed, 0U);
  EXPECT_EQ(r.oms.order(h).status, kCancelPending);
}

// The restart rule: a stale order surfacing after a crash must never fire.
TEST(OrderRouterRecovery, ApprovedAndNotAtTheBrokerExpiresAndIsNeverSent) {
  Rig r;
  const OrderHandle h = r.restored(kApproved);
  EXPECT_EQ(r.router.recover(h, r.broker), Recovery::kResolved);
  EXPECT_EQ(r.oms.order(h).status, kExpired);
  EXPECT_EQ(r.sink.events.back().from, kApproved);
  EXPECT_EQ(r.sink.events.back().to, kExpired);
  EXPECT_EQ(r.sink.events.back().reason_code.view(), kExpiredOnRestart);

  r.sink.durable_count = r.sink.events.size();
  EXPECT_FALSE(r.oms.can_send(h));
  EXPECT_FALSE(r.router.submit(h).has_value());  // nor can anyone queue it by hand
  EXPECT_EQ(r.router.pending(), 0U);
  EXPECT_EQ(r.router.poll(), 0U);
  EXPECT_TRUE(r.broker.tags.empty());
}

TEST(OrderRouterRecovery, ExpiryThatCantBeRecordedLeavesTheOrderBlocked) {
  Rig r;
  const OrderHandle h = r.restored(kApproved);
  r.sink.full = true;
  EXPECT_EQ(r.router.recover(h, r.broker), Recovery::kUnresolved);
  EXPECT_EQ(r.oms.order(h).status, kApproved);
  EXPECT_TRUE(r.oms.order(h).needs_reconcile);
  EXPECT_FALSE(r.oms.can_send(h));
  EXPECT_FALSE(r.router.submit(h).has_value());

  r.sink.full = false;  // the next pass finishes the job
  EXPECT_EQ(r.router.recover(h, r.broker), Recovery::kResolved);
  EXPECT_EQ(r.oms.order(h).status, kExpired);
  EXPECT_EQ(r.router.poll(), 0U);
  EXPECT_TRUE(r.broker.tags.empty());
}

TEST(OrderRouterRecovery, AdoptionThatCantBeRecordedLeavesTheOrderBlocked) {
  Rig r;
  r.broker.tags = {tag_of_client()};
  const OrderHandle h = r.restored(kApproved);
  r.sink.full = true;
  EXPECT_EQ(r.router.recover(h, r.broker), Recovery::kUnresolved);
  EXPECT_EQ(r.oms.order(h).status, kApproved);
  EXPECT_FALSE(r.oms.can_send(h));  // the broker has it: this must not look sendable
  EXPECT_FALSE(r.router.submit(h).has_value());
  EXPECT_EQ(r.broker.tags.size(), 1U);
}

// The broker's book can lag: a late fill on an expired order is a fact, so it's recorded and raised, not
// lost.
TEST(OrderRouterRecovery, AFillOnAnExpiredOrderIsRecordedAndRaised) {
  Rig r;
  const OrderHandle h = r.restored(kApproved);
  ASSERT_EQ(r.router.recover(h, r.broker), Recovery::kResolved);
  ASSERT_TRUE(r.oms.apply(h, E::kPartialFill, {.fill_qty = 2, .fill_price = Price::from_units(10'000)}));
  EXPECT_EQ(r.oms.order(h).status, kExpired);
  EXPECT_EQ(r.oms.order(h).filled_qty, 2);
  EXPECT_EQ(r.sink.events.back().reason_code.view(), kReconMismatch);
}

// A failed read isn't "not at the broker": expiring on it would disown a live order at every restart.
TEST(OrderRouterRecovery, ABookThatCantBeReadChangesNothing) {
  for (const OrderStatus status : {kApproved, kSent, kUnknown, kOpen}) {
    Rig r;
    r.broker.book_down = true;
    const OrderHandle h = r.restored(status);
    const std::size_t events = r.sink.events.size();
    EXPECT_EQ(r.router.recover(h, r.broker), Recovery::kUnresolved);
    EXPECT_EQ(r.oms.order(h).status, status);
    EXPECT_EQ(r.sink.events.size(), events);
    EXPECT_TRUE(r.oms.order(h).needs_reconcile);
    EXPECT_FALSE(r.oms.can_send(h));
  }
}

// A book that can't be true of this order must never settle it (positions read these events).
TEST(OrderRouterRecovery, ABookThatContradictsTheOrderStaysBlocked) {
  struct Case {
    const char* name;
    BrokerState state;
    std::int32_t filled;
    std::int32_t broker_qty;
  };
  for (const Case& c : {Case{"filled, under our qty", BrokerState::kFilled, 5, 0},
                        Case{"filled, past our qty", BrokerState::kFilled, 15, 0},
                        Case{"another qty", BrokerState::kOpen, 0, 15},
                        Case{"open with all of it filled", BrokerState::kOpen, 10, 0},
                        Case{"rejected, but it traded", BrokerState::kRejected, 2, 0},
                        Case{"partly filled, past our qty", BrokerState::kPartiallyFilled, 12, 0}}) {
    for (const OrderStatus status : {kApproved, kSent, kUnknown}) {
      Rig r;
      r.broker.tags = {tag_of_client()};
      r.broker.view = {.state = c.state,
                       .filled_qty = c.filled,
                       .average_price = Price::from_units(10'000),
                       .qty = c.broker_qty};
      const OrderHandle h = r.restored(status);
      EXPECT_EQ(r.router.recover(h, r.broker), Recovery::kUnresolved) << c.name;
      EXPECT_EQ(r.oms.order(h).filled_qty, 0) << c.name;
      EXPECT_EQ(r.oms.recon_mismatches(), 1U) << c.name;
      EXPECT_TRUE(r.oms.order(h).needs_reconcile) << c.name;
      EXPECT_EQ(r.oms.order(h).status, kUnknown) << c.name;  // handed over, never sent again
    }
  }
}

TEST(OrderRouterRecovery, OpenWithFillsIsPartlyFilled) {
  Rig r;
  r.broker.tags = {tag_of_client()};
  r.broker.view = {.state = BrokerState::kOpen, .filled_qty = 5, .average_price = Price::from_units(10'000)};
  const OrderHandle h = r.restored(kSent);
  EXPECT_EQ(r.router.recover(h, r.broker), Recovery::kResolved);
  EXPECT_EQ(r.oms.order(h).status, kPartiallyFilled);
  EXPECT_EQ(r.oms.order(h).filled_qty, 5);
  EXPECT_EQ(r.sink.events.back().fill_qty, 5);
}

// The kill switch's flatten orders queue behind whatever went before, so nothing here may wedge the queue.
TEST(OrderRouter, NewOrdersStillGoOutAfterCancelAll) {
  Rig r;
  const OrderHandle sent = r.approved();
  ASSERT_TRUE(r.router.submit(sent));
  r.sink.durable_count = r.sink.events.size();
  ASSERT_EQ(r.router.poll(), 1U);

  OrderDetails queued_details = order_of(10, "client-queued");
  queued_details.id.bytes.fill(2);
  const OrderHandle queued = r.oms.create(queued_details).value();
  ASSERT_TRUE(r.oms.apply(queued, E::kRiskApprove));
  ASSERT_TRUE(r.router.submit(queued));
  ASSERT_TRUE(r.router.cancel_all().failed == 0U);
  EXPECT_EQ(r.router.pending(), 0U);

  OrderDetails later_details = order_of(10, "client-later");
  later_details.id.bytes.fill(3);
  const OrderHandle later = r.oms.create(later_details).value();
  ASSERT_TRUE(r.oms.apply(later, E::kRiskApprove));
  ASSERT_TRUE(r.router.submit(later));
  r.sink.durable_count = r.sink.events.size();
  EXPECT_EQ(r.router.poll(), 1U);
  EXPECT_EQ(r.oms.order(later).status, kSent);
  EXPECT_EQ(r.oms.order(queued).status, kApproved);  // never went out
  EXPECT_EQ(r.router.pending(), 0U);
}

TEST(OrderRouter, CancelAllAsksForAnOrderHandedOverButNotYetRecorded) {
  Rig r;
  const OrderHandle h = r.approved();
  ASSERT_TRUE(r.router.submit(h));
  r.sink.durable_count = r.sink.events.size();
  r.sink.full = true;  // the adapter takes it, and SENT can't be recorded
  ASSERT_EQ(r.router.poll(), 1U);
  ASSERT_EQ(r.oms.order(h).status, kApproved);
  const auto out = r.router.cancel_all();
  EXPECT_EQ(out.asked, 1U);
  EXPECT_EQ(r.broker.cancels.size(), 1U);
  EXPECT_EQ(r.router.poll(), 0U);  // never sent twice
  EXPECT_EQ(r.broker.tags.size(), 1U);
}

TEST(OrderRouter, CancelAllAsksForARestoredApprovedOrderButNotAQueuedOne) {
  Rig r;
  const OrderHandle restored = r.restored(kApproved);
  OrderDetails queued_details = order_of(10, "client-queued");
  queued_details.id.bytes.fill(2);
  const OrderHandle queued = r.oms.create(queued_details).value();
  ASSERT_TRUE(r.oms.apply(queued, E::kRiskApprove));
  ASSERT_TRUE(r.router.submit(queued));
  const auto out = r.router.cancel_all();
  EXPECT_EQ(out.asked, 1U);
  ASSERT_EQ(r.broker.cancels.size(), 1U);
  EXPECT_EQ(r.broker.cancels.front(), r.oms.order(restored).details.id);
}

// The confirm was lost in the crash, and the broker's book says what the order now is.
TEST(OrderRouterRecovery, AModifyInFlightTakesTheBrokersValues) {
  Rig r;
  r.broker.tags = {tag_of_client()};
  r.broker.view = {
      .state = BrokerState::kOpen, .broker_order_id = "B1", .qty = 5, .price = Price::from_units(2'000'000)};
  const OrderHandle h = r.restored(kModifyPending);
  EXPECT_EQ(r.router.recover(h, r.broker), Recovery::kResolved);
  EXPECT_EQ(r.oms.order(h).status, kOpen);
  EXPECT_EQ(r.oms.order(h).details.qty, 5);
  EXPECT_EQ(r.oms.order(h).details.price, Price::from_units(2'000'000));
}

TEST(OrderRouterRecovery, ABrokerQtyWeDontKnowAboutStaysBlocked) {
  Rig r;
  r.broker.tags = {tag_of_client()};
  r.broker.view = {.state = BrokerState::kOpen, .broker_order_id = "B1", .qty = 20};
  const OrderHandle h = r.restored(kOpen);
  EXPECT_EQ(r.router.recover(h, r.broker), Recovery::kUnresolved);
  EXPECT_EQ(r.oms.recon_mismatches(), 1U);
  EXPECT_TRUE(r.oms.order(h).needs_reconcile);
}

TEST(OrderRouterRecovery, ANeverSentOrderAmongOthersExpiresAlone) {
  Rig r;
  const OrderHandle had = r.restored(kApproved);
  RecoveredOrder other;
  other.details = order_of(10, "0190a3b4-7c2d-7e11-8f00-000000000099");
  other.details.id.bytes.fill(2);
  other.status = kApproved;
  other.seq = 2;
  const OrderHandle lacked = r.oms.restore(other).value();
  r.broker.tags = {tag_of_client()};
  r.broker.view = {.state = BrokerState::kOpen, .broker_order_id = "B1"};
  EXPECT_EQ(r.router.recover(had, r.broker), Recovery::kResolved);
  EXPECT_EQ(r.router.recover(lacked, r.broker), Recovery::kResolved);
  EXPECT_EQ(r.oms.order(had).status, kOpen);
  EXPECT_EQ(r.oms.order(lacked).status, kExpired);
  r.sink.durable_count = r.sink.events.size();
  EXPECT_EQ(r.router.poll(), 0U);
  EXPECT_EQ(r.broker.tags.size(), 1U);
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
