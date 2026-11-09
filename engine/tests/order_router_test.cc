// Persist before send (ADR-025), and recovery from each status.

#include "vyuha/oms/order_router.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "test_gate.h"
#include "vyuha/core/clock.h"
#include "vyuha/core/latency_histogram.h"
#include "vyuha/risk/risk_gate.h"

namespace vyuha::oms {
namespace {

// For tests whose rules keep no state.
risk::GateContext& no_state() {
  static risk::GateContext ctx;
  ctx = {};
  return ctx;
}

using enum OrderStatus;
using E = OrderEvent;
using Recovery = OrderRouter::Recovery;
using BrokerState = BrokerOrderView::State;
using risk::Outcome;

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
  // NOLINTNEXTLINE(bugprone-exception-escape): a test sink; reserve would only move the throw
  void note_outage_refusal(OutageRefusal kind) noexcept override { outage_refusals.push_back(kind); }
  std::vector<Uuid> refused;
  std::vector<OutageRefusal> outage_refusals;
  std::vector<OrderEventRecord> events;
  std::uint64_t durable_count{0};
  bool full{false};
  bool is_healthy{true};
};

class FakeBroker final : public IOrderSender, public IBrokerOrderBook {
 public:
  // NOLINTNEXTLINE(bugprone-exception-escape): a test's allocation failure may terminate
  [[nodiscard]] bool send(const risk::ApprovedOrder& order, std::string_view tag) noexcept override {
    if (busy) {
      return false;
    }
    tags.emplace_back(tag);
    sent_qty.push_back(order.order().qty);
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
  std::vector<std::int32_t> sent_qty;
  std::vector<Uuid> cancels;
  BrokerOrderView view;
  bool busy{false};
  bool book_down{false};
};

constexpr std::size_t kTagLimit = 20;
constexpr std::string_view kClient = "0190a3b4-7c2d-7e11-8f00-aabbccddeeff";
std::string tag_of_client() { return broker_tag(kClient, kTagLimit); }

OrderDetails order_of(std::int32_t qty, std::string_view client = kClient) {
  OrderDetails d{.is_paper = kPaperOrder};
  d.id.bytes.fill(1);
  for (std::size_t i = 0; i < client.size(); ++i) {  // one id per client id: intake refuses a reused id
    d.id.bytes.at(i % d.id.bytes.size()) ^= static_cast<std::uint8_t>(client[i]);
  }
  EXPECT_TRUE(d.client_order_id.assign(client));
  d.qty = qty;
  d.price = Price::from_units(1'000'000);
  return d;
}

struct Rig {
  SimClock clock{1'000};
  LaggingSink sink;
  FakeBroker broker;
  Oms oms{sink, clock, 8};
  OrderRouter router{
      oms, sink, broker, Venue::kPaper, {.tag_limit = kTagLimit, .capacity = 8}, testing::open_limits()};

  OrderHandle fresh(std::int32_t qty = 10) { return oms.create(order_of(qty)).value(); }
  // Through the gate, as every order is.
  std::expected<void, Error> submit(OrderHandle h) {
    return router.submit(h, testing::approve(oms.order(h).details, clock));
  }
  OrderHandle restored(OrderStatus status, std::int32_t filled = 0, std::int32_t qty = 10) {
    RecoveredOrder r{.details = order_of(qty)};
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
  const OrderHandle h = r.fresh();
  ASSERT_TRUE(r.submit(h));
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
  const OrderHandle h = r.fresh();
  ASSERT_TRUE(r.submit(h));
  r.sink.durable_count = 2;
  r.sink.is_healthy = false;
  EXPECT_EQ(r.router.poll(), 0U);
  EXPECT_TRUE(r.broker.tags.empty());
  r.sink.is_healthy = true;
  EXPECT_EQ(r.router.poll(), 1U);
}

TEST(OrderRouter, ABusyAdapterIsRetried) {
  Rig r;
  const OrderHandle h = r.fresh();
  ASSERT_TRUE(r.submit(h));
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
  const OrderHandle h = r.fresh();
  ASSERT_TRUE(r.submit(h));
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
  const OrderHandle a = r.fresh();
  const OrderHandle b = r.oms.create(order_of(5, "second-order")).value();
  ASSERT_TRUE(r.submit(a));
  ASSERT_TRUE(r.submit(b));
  r.sink.durable_count = 4;
  EXPECT_EQ(r.router.poll(), 2U);
  ASSERT_EQ(r.broker.tags.size(), 2U);
  EXPECT_EQ(r.broker.tags[1], "secondorder");
}

TEST(OrderRouter, AnOrderThatMovedOnWhileWaitingIsNotSent) {
  Rig r;
  const OrderHandle h = r.oms.create(order_of(1)).value();
  ASSERT_TRUE(r.submit(h));
  EXPECT_FALSE(r.router.submit(99, testing::approve(r.oms.order(h).details, r.clock)).has_value());
  r.sink.durable_count = 2;
  ASSERT_TRUE(r.oms.record_sent(h));  // something else moved it
  EXPECT_EQ(r.router.poll(), 0U);
  EXPECT_TRUE(r.broker.tags.empty());
  EXPECT_EQ(r.router.pending(), 0U);
}

TEST(OrderRouter, AnOrderStorageRefusedIsNeverSentThoughItsEventsCountAsDurable) {
  Rig r;
  const OrderHandle h = r.fresh();
  ASSERT_TRUE(r.submit(h));
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
  const OrderHandle live = r.fresh();
  ASSERT_TRUE(r.submit(live));
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
  const OrderHandle live = r.fresh();
  ASSERT_TRUE(r.submit(live));
  r.sink.durable_count = 2;
  ASSERT_EQ(r.router.poll(), 1U);
  ASSERT_TRUE(r.oms.apply(live, E::kBrokerAck));
  const OrderHandle queued = r.oms.create(order_of(5, "queued-order")).value();
  ASSERT_TRUE(r.submit(queued));
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
  EXPECT_FALSE(r.submit(h).has_value());  // nor can anyone queue it by hand
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
  EXPECT_FALSE(r.submit(h).has_value());

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
  EXPECT_FALSE(r.submit(h).has_value());
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
  const OrderHandle sent = r.fresh();
  ASSERT_TRUE(r.submit(sent));
  r.sink.durable_count = r.sink.events.size();
  ASSERT_EQ(r.router.poll(), 1U);

  OrderDetails queued_details = order_of(10, "client-queued");
  queued_details.id.bytes.fill(2);
  const OrderHandle queued = r.oms.create(queued_details).value();
  ASSERT_TRUE(r.submit(queued));
  ASSERT_TRUE(r.router.cancel_all().failed == 0U);
  EXPECT_EQ(r.router.pending(), 0U);

  OrderDetails later_details = order_of(10, "client-later");
  later_details.id.bytes.fill(3);
  const OrderHandle later = r.oms.create(later_details).value();
  ASSERT_TRUE(r.submit(later));
  r.sink.durable_count = r.sink.events.size();
  EXPECT_EQ(r.router.poll(), 1U);
  EXPECT_EQ(r.oms.order(later).status, kSent);
  EXPECT_EQ(r.oms.order(queued).status, kExpired);  // ended by the cancel, and never went out
  EXPECT_EQ(r.router.pending(), 0U);
}

TEST(OrderRouter, CancelAllAsksForAnOrderHandedOverButNotYetRecorded) {
  Rig r;
  const OrderHandle h = r.fresh();
  ASSERT_TRUE(r.submit(h));
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
  ASSERT_TRUE(r.submit(queued));
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
  RecoveredOrder other{.details = order_of(10, "0190a3b4-7c2d-7e11-8f00-000000000099")};
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

// Invariant 1: what reaches the adapter is what the gate passed, and the APPROVED event says which limit set.
TEST(OrderRouter, SendsWhatTheGateApprovedAndRecordsItsStamp) {
  Rig r;
  const OrderHandle h = r.fresh(7);
  ASSERT_TRUE(r.submit(h));
  ASSERT_EQ(r.sink.events.size(), 2U);
  EXPECT_EQ(r.sink.events[1].to, kApproved);
  EXPECT_TRUE(r.sink.events[1].gate.has_value());
  EXPECT_FALSE(r.sink.events[0].gate.has_value());  // NEW isn't the gate's
  r.sink.durable_count = 2;
  ASSERT_EQ(r.router.poll(), 1U);
  EXPECT_EQ(r.broker.sent_qty, std::vector<std::int32_t>{7});
}

TEST(OrderRouter, AnApprovalForAnotherOrderIsRefused) {
  Rig r;
  const OrderHandle h = r.fresh(10);
  OrderDetails bigger = r.oms.order(h).details;
  bigger.qty = 1'000;  // same id, different body: not what the order is
  EXPECT_EQ(r.router.submit(h, testing::approve(bigger, r.clock)).error().code, ErrorCode::kInvalidArgument);
  OrderDetails other = order_of(10, "other-client");
  other.id.bytes.fill(9);
  EXPECT_EQ(r.router.submit(h, testing::approve(other, r.clock)).error().code, ErrorCode::kInvalidArgument);
  EXPECT_EQ(r.oms.order(h).status, kNew);
  EXPECT_EQ(r.sink.events.size(), 1U);  // nothing recorded
  EXPECT_EQ(r.router.pending(), 0U);
}

TEST(OrderRouter, AFullSinkAtSubmitLeavesTheOrderNew) {
  Rig r;
  const OrderHandle h = r.fresh();
  r.sink.full = true;
  EXPECT_EQ(r.submit(h).error().code, ErrorCode::kCapacityExceeded);
  EXPECT_EQ(r.oms.order(h).status, kNew);
  EXPECT_EQ(r.router.pending(), 0U);
  r.sink.full = false;
  EXPECT_TRUE(r.submit(h).has_value());  // a fresh evaluation goes through
}

// Oms::apply can mark an order APPROVED, but only the router's queue reaches the adapter, and it takes only
// the gate's approval of a NEW order.
TEST(OrderRouter, AnOrderMarkedApprovedWithoutTheGateIsNeverSent) {
  Rig r;
  const OrderHandle h = r.fresh();
  ASSERT_TRUE(r.oms.apply(h, E::kRiskApprove));
  EXPECT_FALSE(r.submit(h).has_value());
  r.sink.durable_count = r.sink.events.size();
  EXPECT_EQ(r.router.poll(), 0U);
  EXPECT_TRUE(r.broker.tags.empty());
}

Outcome pass_rule(const risk::GateContext& /*ctx*/, const OrderDetails& /*order*/,
                  const risk::GateFacts& /*facts*/) noexcept {
  return {};
}
template <std::size_t... I>
constexpr std::array<risk::Rule, risk::kRuleCount> all_pass(std::index_sequence<I...> /*seq*/) {
  return {risk::Rule{.code = static_cast<risk::RuleCode>(I + 1), .eval = &pass_rule}...};
}
constexpr auto kAllPass = all_pass(std::make_index_sequence<risk::kRuleCount>{});

// A paper flag left at its default can't reach a live sender, and a live approval can't go to paper.
TEST(OrderRouter, PaperAndLiveDontMix) {
  Rig r;
  OrderRouter live{
      r.oms, r.sink, r.broker, Venue::kLive, {.tag_limit = kTagLimit, .capacity = 8}, testing::open_limits()};
  const OrderHandle paper = r.fresh();
  EXPECT_EQ(live.submit(paper, testing::approve(r.oms.order(paper).details, r.clock)).error().code,
            ErrorCode::kInvalidArgument);
  EXPECT_EQ(r.oms.order(paper).status, kNew);

  OrderDetails real = order_of(10, "live-client");
  real.id.bytes.fill(4);
  real.is_paper = kLiveOrder;
  const OrderHandle h = r.oms.create(real).value();
  const auto gate = risk::RiskGate::create(kAllPass, r.clock).value();
  auto decision = gate.evaluate(no_state(), real);
  ASSERT_TRUE(decision.approved.has_value());
  EXPECT_EQ(r.router.submit(h, std::move(decision.approved).value()).error().code,  // NOLINT
            ErrorCode::kInvalidArgument);                                           // the paper router
  auto again = gate.evaluate(no_state(), real);
  ASSERT_TRUE(again.approved.has_value());
  EXPECT_TRUE(live.submit(h, std::move(again.approved).value()).has_value());  // NOLINT
}

// Evaluated and submitted in one pass: an approval held back may predate a kill switch or a limit cut. A
// stale one ends the order (owner, 2026-11-08): RISK_REJECTED with R-APPROVAL-STALE and the stamp, no retry.
TEST(OrderRouter, AStaleApprovalRejectsTheOrderForGood) {
  Rig r;  // the OMS clock reads 1'000
  const OrderHandle h = r.fresh();
  const SimClock too_old(1'000 - kDefaultApprovalMaxAge - 1);
  EXPECT_EQ(r.router.submit(h, testing::approve(r.oms.order(h).details, too_old)).error().code,
            ErrorCode::kApprovalStale);
  EXPECT_EQ(r.oms.order(h).status, kRiskRejected);
  ASSERT_EQ(r.sink.events.size(), 2U);
  EXPECT_EQ(r.sink.events[1].to, kRiskRejected);
  EXPECT_EQ(r.sink.events[1].reason_code.view(), kApprovalStale);
  ASSERT_TRUE(r.sink.events[1].gate.has_value());
  EXPECT_EQ(r.sink.events[1].gate.value_or(risk::GateStamp{}).ts, 1'000 - kDefaultApprovalMaxAge - 1);
  EXPECT_EQ(r.router.stale_approvals(), 1U);
  EXPECT_EQ(r.router.pending(), 0U);

  // A fresh approval can't revive it: it's no longer NEW.
  EXPECT_EQ(r.submit(h).error().code, ErrorCode::kInvalidArgument);
  r.sink.durable_count = r.sink.events.size();
  EXPECT_EQ(r.router.poll(), 0U);
  EXPECT_TRUE(r.broker.tags.empty());
}

TEST(OrderRouter, AnApprovalFromTheFutureIsStaleToo) {
  Rig r;
  const OrderHandle h = r.fresh();
  const SimClock ahead(2'000);
  EXPECT_EQ(r.router.submit(h, testing::approve(r.oms.order(h).details, ahead)).error().code,
            ErrorCode::kApprovalStale);
  EXPECT_EQ(r.oms.order(h).status, kRiskRejected);
}

TEST(OrderRouter, AnApprovalExactlyAtTheLimitGoes) {
  Rig r;
  const OrderHandle h = r.fresh();
  const SimClock at_limit(1'000 - kDefaultApprovalMaxAge);
  EXPECT_TRUE(r.router.submit(h, testing::approve(r.oms.order(h).details, at_limit)).has_value());
  EXPECT_EQ(r.oms.order(h).status, kApproved);
}

TEST(OrderRouter, AStaleRejectionThatCantBeRecordedLeavesTheOrderNew) {
  Rig r;
  const OrderHandle h = r.fresh();
  r.sink.full = true;
  const SimClock too_old(1'000 - kDefaultApprovalMaxAge - 1);
  EXPECT_EQ(r.router.submit(h, testing::approve(r.oms.order(h).details, too_old)).error().code,
            ErrorCode::kCapacityExceeded);
  EXPECT_EQ(r.oms.order(h).status, kNew);
  EXPECT_EQ(r.router.stale_approvals(), 0U);
}

TEST(OrderRouter, TheApprovalAgeLimitIsConfigurable) {
  Rig r;
  OrderRouter tight{r.oms,
                    r.sink,
                    r.broker,
                    Venue::kPaper,
                    {.tag_limit = kTagLimit, .capacity = 8, .approval_max_age = 500},
                    testing::open_limits()};
  const OrderHandle h = r.fresh();
  const SimClock six_hundred_ns_ago(400);  // well inside the default, outside this router's 500 ns
  EXPECT_EQ(tight.submit(h, testing::approve(r.oms.order(h).details, six_hundred_ns_ago)).error().code,
            ErrorCode::kApprovalStale);
}

TEST(OrderRouter, RecordsApprovalToSendTime) {
  Rig r;
  const OrderHandle h = r.fresh();
  const SimClock approved_at(1'000 - 30'000);  // 30 µs before the OMS clock's 1'000
  ASSERT_TRUE(r.router.submit(h, testing::approve(r.oms.order(h).details, approved_at)));
  EXPECT_EQ(r.router.approval_to_send().count(), 0U);  // not sent yet
  r.sink.durable_count = r.sink.events.size();
  ASSERT_EQ(r.router.poll(), 1U);
  const LatencyHistogram& m = r.router.approval_to_send();
  EXPECT_EQ(m.count(), 1U);
  EXPECT_EQ(m.sum(), 30'000);
  EXPECT_EQ(m.max(), 30'000);
  EXPECT_EQ(m.bucket(1), 1U);  // (10 µs, 50 µs]
}

TEST(ApprovalMaxAge, ParsesWholeMillisecondsInRange) {
  EXPECT_EQ(parse_approval_max_age_ms("").value_or(0), kDefaultApprovalMaxAge);
  EXPECT_EQ(parse_approval_max_age_ms("100").value_or(0), 100'000'000);
  EXPECT_EQ(parse_approval_max_age_ms("1").value_or(0), 1'000'000);
  EXPECT_EQ(parse_approval_max_age_ms("10000").value_or(0), 10'000'000'000);
  for (const std::string_view bad :
       {"0", "-5", "10001", "100ms", " 100", "1.5", "abc", "99999999999999999999"}) {
    EXPECT_FALSE(parse_approval_max_age_ms(bad).has_value()) << bad;
  }
}

TEST(OrderRouter, AMovedFromApprovalApprovesNothing) {
  Rig r;
  const OrderHandle h = r.fresh();
  risk::ApprovedOrder first = testing::approve(r.oms.order(h).details, r.clock);
  risk::ApprovedOrder taken = std::move(first);
  // NOLINTNEXTLINE(bugprone-use-after-move,hicpp-invalid-access-moved): that's the test
  EXPECT_TRUE(first.spent());
  EXPECT_FALSE(taken.spent());
  // NOLINTNEXTLINE(bugprone-use-after-move,hicpp-invalid-access-moved): that's the test
  EXPECT_EQ(first.order().qty, 0);  // blanked: nothing sendable left even for a sender that skips spent()
  // NOLINTNEXTLINE(bugprone-use-after-move,hicpp-invalid-access-moved): that's the test
  EXPECT_TRUE(first.order().client_order_id.view().empty());
  // NOLINTNEXTLINE(bugprone-use-after-move,hicpp-invalid-access-moved): that's the test
  EXPECT_EQ(r.router.submit(h, std::move(first)).error().code, ErrorCode::kInvalidArgument);
  EXPECT_TRUE(r.router.submit(h, std::move(taken)).has_value());
}

// One id is one order (M2.1b review: intake must refuse an id reused with another client id).
TEST(OrderRouter, IntakeRefusesAnOrderIdAlreadyUsed) {
  Rig r;
  const OrderHandle h = r.fresh();
  OrderDetails reused = order_of(10, "another-client");
  reused.id = r.oms.order(h).details.id;
  EXPECT_EQ(r.oms.create(reused).error().code, ErrorCode::kOrderDup);
  EXPECT_EQ(r.oms.size(), 1U);
}

// ---- ADR-027: while the order log isn't durable, nothing that adds risk goes out ----

// An OPEN order: sent and acknowledged.
OrderHandle open_order(Rig& r, std::int32_t qty = 10) {
  const OrderHandle h = r.fresh(qty);
  EXPECT_TRUE(r.submit(h));
  r.sink.durable_count = r.sink.events.size();
  EXPECT_EQ(r.router.poll(), 1U);
  EXPECT_TRUE(r.oms.apply(h, E::kBrokerAck, {.broker_order_id = "B1"}));
  return h;
}

TEST(OutageRules, ANewOrderIsRefusedAndNothingIsRecorded) {
  Rig r;
  r.sink.is_healthy = false;
  EXPECT_EQ(r.oms.create(order_of(10)).error().code, ErrorCode::kOrderLogUnavailable);
  EXPECT_EQ(r.oms.size(), 0U);
  EXPECT_TRUE(r.sink.events.empty());
  EXPECT_EQ(r.sink.outage_refusals, std::vector{OutageRefusal::kNewOrder});
  EXPECT_EQ(r.oms.outage_refusals(), 1U);
  r.sink.is_healthy = true;
  EXPECT_TRUE(r.oms.create(order_of(10)).has_value());  // back: taken in again
}

TEST(OutageRules, TheSameRequestAgainStillReturnsTheExistingOrder) {
  Rig r;
  const OrderHandle h = r.fresh();
  r.sink.is_healthy = false;
  const auto again = r.oms.intake(order_of(10));  // a retry: nothing new is created
  ASSERT_TRUE(again.has_value());
  EXPECT_TRUE(again.value_or(Intake{}).existing);
  EXPECT_EQ(again.value_or(Intake{}).handle, h);
  EXPECT_TRUE(r.sink.outage_refusals.empty());
}

TEST(OutageRules, AModifyThatRaisesQtyIsRefused) {
  Rig r;
  const OrderHandle h = open_order(r, 10);
  r.sink.is_healthy = false;
  const auto before = r.sink.events.size();
  EXPECT_EQ(r.oms.apply(h, E::kModifyRequest, {.modified_qty = 11}).error().code,
            ErrorCode::kOrderLogUnavailable);
  EXPECT_EQ(r.oms.order(h).status, kOpen);
  EXPECT_EQ(r.oms.order(h).details.qty, 10);
  EXPECT_EQ(r.sink.events.size(), before);
  EXPECT_EQ(r.sink.outage_refusals, std::vector{OutageRefusal::kModifyQtyUp});
}

// Until positions exist (M2.6) every price or trigger change counts as opening: fail closed.
TEST(OutageRules, APriceOrTriggerModifyIsRefused) {
  Rig r;
  const OrderHandle h = open_order(r, 10);
  r.sink.is_healthy = false;
  EXPECT_EQ(r.oms.apply(h, E::kModifyRequest, {.modified_price = Price::from_units(990'000)}).error().code,
            ErrorCode::kOrderLogUnavailable);
  EXPECT_EQ(r.oms.apply(h, E::kModifyRequest, {.modified_trigger = Price::from_units(990'000)}).error().code,
            ErrorCode::kOrderLogUnavailable);
  // A lower qty with a new price still changes the price: refused.
  EXPECT_EQ(
      r.oms.apply(h, E::kModifyRequest, {.modified_qty = 5, .modified_price = Price::from_units(990'000)})
          .error()
          .code,
      ErrorCode::kOrderLogUnavailable);
  EXPECT_EQ(r.oms.order(h).status, kOpen);
  EXPECT_EQ(r.sink.outage_refusals.size(), 3U);
}

TEST(OutageRules, AModifyThatOnlyLowersQtyGoes) {
  Rig r;
  const OrderHandle h = open_order(r, 10);
  r.sink.is_healthy = false;
  EXPECT_EQ(r.oms.apply(h, E::kModifyRequest, {.modified_qty = 4}), kModifyPending);
  EXPECT_TRUE(r.sink.outage_refusals.empty());
}

TEST(OutageRules, ACancelRequestGoes) {
  Rig r;
  const OrderHandle h = open_order(r);
  r.sink.is_healthy = false;
  EXPECT_EQ(r.oms.apply(h, E::kCancelRequest), kCancelPending);
}

TEST(OutageRules, CancelAllGoesAndAsksTheBroker) {
  Rig r;
  const OrderHandle h = open_order(r);
  r.sink.is_healthy = false;
  const auto out = r.router.cancel_all();
  EXPECT_EQ(out.asked, 1U);
  EXPECT_EQ(out.failed, 0U);
  EXPECT_EQ(r.broker.cancels.size(), 1U);
  EXPECT_EQ(r.oms.order(h).status, kCancelPending);
}

TEST(OutageRules, TheBrokersCancelAndItsConfirmAreRecorded) {
  Rig r;
  const OrderHandle a = open_order(r);
  r.sink.is_healthy = false;
  ASSERT_EQ(r.oms.apply(a, E::kCancelRequest), kCancelPending);
  EXPECT_EQ(r.oms.apply(a, E::kCancelConfirm), kCancelled);  // a broker fact, outage or not
}

// ---- ADR-027: a queued order has an age limit, from its gate stamp ----

TEST(QueueAge, AnOrderPastTheLimitIsExpiredNotSent) {
  Rig r;
  const OrderHandle h = r.fresh();
  ASSERT_TRUE(r.submit(h));  // stamped at 1'000
  r.clock.advance(kDefaultQueueMaxAge + 1);
  r.sink.durable_count = r.sink.events.size();
  EXPECT_EQ(r.router.poll(), 0U);
  EXPECT_TRUE(r.broker.tags.empty());
  EXPECT_EQ(r.oms.order(h).status, kExpired);
  EXPECT_EQ(r.sink.events.back().reason_code.view(), kExpiredInQueue);
  EXPECT_EQ(r.router.expired_in_queue(), 1U);
  EXPECT_EQ(r.router.pending(), 0U);
}

TEST(QueueAge, AnOrderExactlyAtTheLimitIsSent) {
  Rig r;
  const OrderHandle h = r.fresh();
  ASSERT_TRUE(r.submit(h));
  r.clock.advance(kDefaultQueueMaxAge);
  r.sink.durable_count = r.sink.events.size();
  EXPECT_EQ(r.router.poll(), 1U);
  EXPECT_EQ(r.oms.order(h).status, kSent);
  EXPECT_EQ(r.router.expired_in_queue(), 0U);
}

// A clock stepped back gives a negative age: expired, never sent (fail closed, as submit() does).
TEST(QueueAge, AClockSteppedBackExpiresTheOrder) {
  Rig r;
  const OrderHandle h = r.fresh();
  ASSERT_TRUE(r.submit(h));  // stamped at 1'000
  r.clock.set(500);
  r.sink.durable_count = r.sink.events.size();
  EXPECT_EQ(r.router.poll(), 0U);
  EXPECT_TRUE(r.broker.tags.empty());
  EXPECT_EQ(r.oms.order(h).status, kExpired);
  EXPECT_EQ(r.router.expired_in_queue(), 1U);
}

// The point of the limit: orders queued before an outage don't fire when it ends.
TEST(QueueAge, OrdersQueuedThroughAnOutageExpireWhenItEnds) {
  Rig r;
  const OrderHandle a = r.fresh();
  const OrderDetails second = order_of(5, "second-order");
  const OrderHandle b = r.oms.create(second).value();
  ASSERT_TRUE(r.submit(a));
  ASSERT_TRUE(r.submit(b));
  r.sink.is_healthy = false;
  r.sink.durable_count = r.sink.events.size();
  EXPECT_EQ(r.router.poll(), 0U);  // down: nothing goes
  r.clock.advance(6 * kNanosPerSecond);
  r.sink.is_healthy = true;
  EXPECT_EQ(r.router.poll(), 0U);
  EXPECT_TRUE(r.broker.tags.empty());
  EXPECT_EQ(r.oms.order(a).status, kExpired);
  EXPECT_EQ(r.oms.order(b).status, kExpired);
}

TEST(QueueAge, AnExpiryThatCantBeRecordedHoldsTheQueueAndNeverSends) {
  Rig r;
  const OrderHandle h = r.fresh();
  ASSERT_TRUE(r.submit(h));
  r.clock.advance(kDefaultQueueMaxAge + 1);
  r.sink.durable_count = r.sink.events.size();
  r.sink.full = true;
  EXPECT_EQ(r.router.poll(), 0U);
  EXPECT_EQ(r.oms.order(h).status, kApproved);
  EXPECT_EQ(r.router.pending(), 1U);
  r.sink.full = false;
  EXPECT_EQ(r.router.poll(), 0U);
  EXPECT_EQ(r.oms.order(h).status, kExpired);
  EXPECT_TRUE(r.broker.tags.empty());
}

TEST(QueueAge, TheLimitIsConfigurable) {
  Rig r;
  OrderRouter tight{r.oms,
                    r.sink,
                    r.broker,
                    Venue::kPaper,
                    {.tag_limit = kTagLimit, .capacity = 8, .queue_max_age = kNanosPerSecond},
                    testing::open_limits()};
  const OrderHandle h = r.fresh();
  ASSERT_TRUE(tight.submit(h, testing::approve(r.oms.order(h).details, r.clock)));
  r.clock.advance(kNanosPerSecond + 1);
  r.sink.durable_count = r.sink.events.size();
  EXPECT_EQ(tight.poll(), 0U);
  EXPECT_EQ(r.oms.order(h).status, kExpired);
}

TEST(QueueMaxAge, ParsesWholeMillisecondsInRange) {
  EXPECT_EQ(parse_queue_max_age_ms("").value_or(0), kDefaultQueueMaxAge);
  EXPECT_EQ(parse_queue_max_age_ms("5000").value_or(0), 5 * kNanosPerSecond);
  EXPECT_EQ(parse_queue_max_age_ms("60000").value_or(0), 60 * kNanosPerSecond);
  for (const std::string_view bad : {"0", "60001", "-1", "5s", "1e3"}) {
    EXPECT_FALSE(parse_queue_max_age_ms(bad).has_value()) << bad;
  }
}

}  // namespace
}  // namespace vyuha::oms

namespace vyuha::oms {
namespace {

// ---- The send budget (D-20) ----

// A router with room for a burst, on a cap of 8.
struct PacedRig {
  SimClock clock{1'000};
  LaggingSink sink;
  FakeBroker broker;
  testing::FixedSendLimits limits{8};
  Oms oms{sink, clock, 256};
  OrderRouter router{oms, sink, broker, Venue::kPaper, {.tag_limit = kTagLimit, .capacity = 256}, limits};

  OrderHandle add(int n, std::uint8_t account = 7) {
    OrderDetails d = order_of(10, "client-" + std::to_string(n));
    d.account_id.bytes.fill(account);
    return oms.create(d).value();
  }
  void queue(OrderHandle h) { ASSERT_TRUE(router.submit(h, testing::approve(oms.order(h).details, clock))); }
  void mark_durable() { sink.durable_count = sink.events.size(); }
  // An order the broker holds.
  OrderHandle open_order(int n) {
    const OrderHandle h = add(n);
    queue(h);
    mark_durable();
    EXPECT_EQ(router.poll(), 1U);
    EXPECT_TRUE(oms.apply(h, OrderEvent::kBrokerAck).has_value());
    return h;
  }
};

TEST(SendBudget, NoMoreThanTheCapLeaveInASecond) {
  PacedRig r;
  for (int i = 0; i < 20; ++i) {
    r.queue(r.add(i));
  }
  r.mark_durable();
  EXPECT_EQ(r.router.poll(), 8U);
  EXPECT_EQ(r.router.poll(), 0U);
  EXPECT_EQ(r.router.pending(), 12U);
  r.clock.advance(kNanosPerSecond - 1);
  EXPECT_EQ(r.router.poll(), 0U);  // a token comes back one full second after it was used
  r.clock.advance(1);
  EXPECT_EQ(r.router.poll(), 8U);
}

TEST(SendBudget, OneAccountsBacklogDoesNotHoldUpAnother) {
  PacedRig r;
  for (int i = 0; i < 12; ++i) {
    r.queue(r.add(i, 7));
  }
  r.queue(r.add(100, 9));
  r.mark_durable();
  EXPECT_EQ(r.router.poll(), 9U);  // 8 of the first account's, and the other account's one
}

TEST(SendBudget, AHeldOrderStillExpiresAtTheQueueLimit) {
  PacedRig r;
  std::vector<OrderHandle> all;
  for (int i = 0; i < 9; ++i) {
    all.push_back(r.add(i));
    r.queue(all.back());
  }
  r.mark_durable();
  EXPECT_EQ(r.router.poll(), 8U);
  r.clock.advance(kDefaultQueueMaxAge + 1);
  EXPECT_EQ(r.router.poll(), 0U);
  EXPECT_EQ(r.oms.order(all.back()).status, OrderStatus::kExpired);
  EXPECT_EQ(r.router.expired_in_queue(), 1U);
}

TEST(SendBudget, ANewOrderWithNoCapIsHeldAndNeverSent) {
  const SimClock clock{1'000};
  LaggingSink sink;
  FakeBroker broker;
  const testing::FixedSendLimits none{0};
  Oms oms{sink, clock, 8};
  OrderRouter router{oms, sink, broker, Venue::kPaper, {.tag_limit = kTagLimit, .capacity = 8}, none};
  const OrderHandle h = oms.create(order_of(10)).value();
  ASSERT_TRUE(router.submit(h, testing::approve(oms.order(h).details, clock)));
  sink.durable_count = sink.events.size();
  EXPECT_EQ(router.poll(), 0U);
  EXPECT_TRUE(broker.tags.empty());
}

TEST(SendBudget, ACancelGoesAheadOfNewOrdersAndUsesAToken) {
  PacedRig r;
  const OrderHandle live = r.open_order(0);  // 1 token used
  r.clock.advance(kNanosPerSecond);
  for (int i = 1; i <= 8; ++i) {
    r.queue(r.add(i));  // these would take all 8 tokens
  }
  r.mark_durable();
  EXPECT_TRUE(r.router.cancel(live));
  ASSERT_EQ(r.broker.cancels.size(), 1U);  // first, though the new orders were queued before it
  EXPECT_EQ(r.router.poll(), 7U);          // the cancel used one of the 8
  EXPECT_EQ(r.router.pending(), 1U);
}

TEST(SendBudget, AKillBurstIsPacedAndEveryOrderIsCancelled) {
  PacedRig r;
  for (int i = 0; i < 20; ++i) {
    r.clock.advance(kNanosPerSecond);
    (void)r.open_order(i);
  }
  r.clock.advance(kNanosPerSecond);
  const auto first = r.router.cancel_all();
  EXPECT_EQ(first.asked, 8U);
  EXPECT_EQ(first.queued, 12U);
  EXPECT_EQ(r.broker.cancels.size(), 8U);
  r.router.poll();
  EXPECT_EQ(r.broker.cancels.size(), 8U);  // the second has no tokens left
  r.clock.advance(kNanosPerSecond);
  r.router.poll();
  EXPECT_EQ(r.broker.cancels.size(), 16U);
  r.clock.advance(kNanosPerSecond);
  r.router.poll();
  EXPECT_EQ(r.broker.cancels.size(), 20U);
  EXPECT_EQ(r.router.cancels_queued(), 0U);
  // Each order was asked once.
  std::vector<Uuid> ids = r.broker.cancels;
  std::ranges::sort(ids);
  EXPECT_EQ(std::ranges::unique(ids).begin(), ids.end());
}

TEST(SendBudget, ACancelTheAdapterRefusedUsesNoTokenAndIsRetried) {
  PacedRig r;
  const OrderHandle live = r.open_order(0);
  r.clock.advance(kNanosPerSecond);
  r.broker.busy = true;
  const auto out = r.router.cancel_all();
  EXPECT_EQ(out.failed, 1U);
  EXPECT_EQ(r.router.cancels_queued(), 1U);
  r.broker.busy = false;
  r.router.poll();
  ASSERT_EQ(r.broker.cancels.size(), 1U);
  EXPECT_EQ(r.broker.cancels[0], r.oms.order(live).details.id);
  EXPECT_EQ(r.router.cancels_queued(), 0U);
}

TEST(SendBudget, ACancelStillGoesForAnAccountWithNoCapOnFile) {
  const SimClock clock{1'000};
  LaggingSink sink;
  FakeBroker broker;
  const testing::FixedSendLimits limits{8};
  Oms oms{sink, clock, 8};
  OrderRouter opening{oms, sink, broker, Venue::kPaper, {.tag_limit = kTagLimit, .capacity = 8}, limits};
  const OrderHandle h = oms.create(order_of(10)).value();
  ASSERT_TRUE(opening.submit(h, testing::approve(oms.order(h).details, clock)));
  sink.durable_count = sink.events.size();
  ASSERT_EQ(opening.poll(), 1U);
  ASSERT_TRUE(oms.apply(h, OrderEvent::kBrokerAck).has_value());

  const testing::FixedSendLimits none{0};
  OrderRouter router{oms, sink, broker, Venue::kPaper, {.tag_limit = kTagLimit, .capacity = 8}, none};
  EXPECT_EQ(router.cancel_all().asked, 1U);
  EXPECT_EQ(broker.cancels.size(), 1U);
}

TEST(SendBudget, ACancelForAnOrderTheBrokerCantHoldIsRefused) {
  PacedRig r;
  EXPECT_FALSE(r.router.cancel(r.add(0)));  // NEW: never approved
  EXPECT_FALSE(r.router.cancel(99));        // no such order
}

// An instrument not in the master could be on either exchange: instrument 99 here.
class VenueLimits final : public ISendLimits {
 public:
  [[nodiscard]] std::uint8_t ops_cap(const Uuid& /*account*/) const noexcept override { return 8; }
  [[nodiscard]] std::uint8_t ops_venue(std::int64_t instrument_id) const noexcept override {
    return instrument_id == 99 ? kOpsVenueUnknown : 0;
  }
};

TEST(SendBudget, ACancelForAnUnknownInstrumentUsesEveryExchangesBudget) {
  const SimClock clock{1'000};
  LaggingSink sink;
  FakeBroker broker;
  const VenueLimits limits;
  Oms oms{sink, clock, 32};
  OrderRouter router{oms, sink, broker, Venue::kPaper, {.tag_limit = kTagLimit, .capacity = 32}, limits};
  for (int i = 0; i < 16; ++i) {
    OrderDetails d = order_of(10, "client-" + std::to_string(i));
    d.instrument_id = i < 8 ? 1 : 99;
    RecoveredOrder restored{.details = d};
    restored.status = kOpen;
    restored.seq = 3;
    ASSERT_TRUE(oms.restore(restored).has_value());
  }
  const auto out = router.cancel_all();
  EXPECT_EQ(out.asked, 8U);  // not 16: those 8 could all be on the one exchange
  EXPECT_EQ(out.queued, 8U);
}

TEST(SendBudget, AccountsBeyondTheTableShareABudget) {
  PacedRig r;
  for (int i = 0; i < 140; ++i) {
    r.queue(r.add(i, static_cast<std::uint8_t>(i)));
  }
  r.mark_durable();
  EXPECT_EQ(r.router.poll(), 135U);  // 127 accounts with their own bucket, then 8 from the shared last one
}

TEST(SendBudget, RepeatedCancelAllReachesEveryOrderWhenTheBrokerIsSilent) {
  PacedRig r;
  for (int i = 0; i < 20; ++i) {
    r.clock.advance(kNanosPerSecond);
    (void)r.open_order(i);
  }
  for (int second = 0; second < 3; ++second) {
    r.clock.advance(kNanosPerSecond);
    (void)r.router.cancel_all();
  }
  std::vector<Uuid> ids = r.broker.cancels;
  std::ranges::sort(ids);
  const auto [end, last] = std::ranges::unique(ids);
  ids.erase(end, last);
  EXPECT_EQ(ids.size(), 20U);
}

TEST(SendBudget, CancellingAnOrderStillWaitingForATokenMeansItNeverGoes) {
  PacedRig r;
  std::vector<OrderHandle> all;
  for (int i = 0; i < 9; ++i) {
    all.push_back(r.add(i));
    r.queue(all.back());
  }
  r.mark_durable();
  EXPECT_EQ(r.router.poll(), 8U);
  EXPECT_TRUE(r.router.cancel(all.back()));
  EXPECT_EQ(r.oms.order(all.back()).status, OrderStatus::kExpired);
  EXPECT_EQ(r.sink.events.back().reason_code.view(), kCancelledInQueue);
  r.clock.advance(kNanosPerSecond);
  EXPECT_EQ(r.router.poll(), 0U);
  EXPECT_EQ(r.broker.tags.size(), 8U);
  EXPECT_EQ(r.router.pending(), 0U);
}

TEST(SendBudget, CancelAllEndsTheOrdersItDrops) {
  PacedRig r;
  const OrderHandle h = r.add(0);
  r.queue(h);
  (void)r.router.cancel_all();
  EXPECT_EQ(r.oms.order(h).status, OrderStatus::kExpired);
  EXPECT_EQ(r.sink.events.back().reason_code.view(), kCancelledInQueue);
}

TEST(SendBudget, AClockThatSteppedBackHoldsCancelsUpForASecondAtMost) {
  PacedRig r;
  for (int i = 0; i < 12; ++i) {
    r.clock.advance(kNanosPerSecond);
    (void)r.open_order(i);
  }
  r.clock.advance(kNanosPerSecond);
  EXPECT_EQ(r.router.cancel_all().asked, 8U);
  r.clock.set(1'000);  // an hour's step back looks the same: every stamp is now in the future
  r.router.poll();
  EXPECT_EQ(r.broker.cancels.size(), 8U);  // the cap still holds
  r.clock.advance(kNanosPerSecond);
  r.router.poll();
  EXPECT_EQ(r.broker.cancels.size(), 12U);  // and it doesn't wait for the clock to catch up
}

// The clock moves between reads, as a real one does. A frozen clock hides a stamp that looks like the future.
class AdvancingClock final : public IClock {
 public:
  [[nodiscard]] Nanos now() const noexcept override { return now_.fetch_add(1, std::memory_order_relaxed); }

 private:
  mutable std::atomic<Nanos> now_{1'000};
};

TEST(SendBudget, ACancelBurstIsPacedOnAClockThatMovesBetweenReads) {
  const AdvancingClock clock;
  LaggingSink sink;
  FakeBroker broker;
  const testing::FixedSendLimits limits{8};
  Oms oms{sink, clock, 64};
  OrderRouter router{oms, sink, broker, Venue::kPaper, {.tag_limit = kTagLimit, .capacity = 8}, limits};
  for (int i = 0; i < 40; ++i) {
    RecoveredOrder restored{.details = order_of(10, "client-" + std::to_string(i))};
    restored.status = kOpen;
    restored.seq = 3;
    ASSERT_TRUE(oms.restore(restored).has_value());
  }
  const auto out = router.cancel_all();
  EXPECT_EQ(out.asked, 8U);
  EXPECT_EQ(out.queued, 32U);
  router.poll();
  EXPECT_EQ(broker.cancels.size(), 8U);
}

TEST(SendBudget, CancellingAnOrderStorageRefusedRecordsNothingIllegal) {
  PacedRig r;
  const OrderHandle h = r.add(0);
  r.queue(h);
  r.oms.refuse(r.oms.order(h).details.id);
  ASSERT_EQ(r.oms.order(h).status, OrderStatus::kRiskRejected);
  EXPECT_FALSE(r.router.cancel(h));  // nothing was left to cancel
  EXPECT_EQ(r.oms.illegal_transitions(), 0U);
  EXPECT_EQ(r.oms.order(h).status, OrderStatus::kRiskRejected);
}

TEST(SendBudget, ATokenIsStampedWhenTheOrderLeavesNotWhenThePollBegan) {
  class SlowBroker final : public IOrderSender {
   public:
    explicit SlowBroker(SimClock& clock) noexcept : clock_(&clock) {}
    [[nodiscard]] bool send(const risk::ApprovedOrder& /*order*/,
                            std::string_view /*tag*/) noexcept override {
      clock_->advance(kNanosPerSecond / 2);
      return true;
    }
    [[nodiscard]] bool cancel(const Order& /*order*/) noexcept override { return true; }

   private:
    SimClock* clock_;
  };
  SimClock clock{1'000};
  LaggingSink sink;
  SlowBroker broker(clock);
  const testing::FixedSendLimits limits{1};
  Oms oms{sink, clock, 8};
  OrderRouter router{oms, sink, broker, Venue::kPaper, {.tag_limit = kTagLimit, .capacity = 8}, limits};
  const OrderHandle h = oms.create(order_of(10)).value();
  ASSERT_TRUE(router.submit(h, testing::approve(oms.order(h).details, clock)));
  const OrderHandle second = oms.create(order_of(10, "client-2")).value();
  ASSERT_TRUE(router.submit(second, testing::approve(oms.order(second).details, clock)));
  sink.durable_count = sink.events.size();
  EXPECT_EQ(router.poll(), 1U);  // it left at +0.5 s, so its token is back at +1.5 s
  clock.advance(kNanosPerSecond - 1);
  EXPECT_EQ(router.poll(), 0U);
  clock.advance(1);
  EXPECT_EQ(router.poll(), 1U);
}

TEST(OpsWindow, HoldsTheCapInEveryRollingSecond) {
  OpsWindow w;
  Nanos now = 1'000;
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(w.has_token(now, 8));
    w.take(now);
    now += kNanosPerSecond / 10;  // 8 orders in the first 0.7 s
  }
  EXPECT_FALSE(w.has_token(now, 8));
  EXPECT_FALSE(w.has_token(1'000 + kNanosPerSecond - 1, 8));
  EXPECT_TRUE(w.has_token(1'000 + kNanosPerSecond, 8));
}

TEST(OpsWindow, RefusesAZeroOrOverLimitCapAndAClockThatSteppedBack) {
  OpsWindow w;
  EXPECT_FALSE(w.has_token(1, 0));
  EXPECT_FALSE(w.has_token(1, kMaxOpsCap + 1));
  w.take(5'000);
  EXPECT_FALSE(w.has_token(4'000, 1));
}

TEST(OpsWindow, ClampingTreatsAFutureStampAsNowAndKeepsTheCap) {
  OpsWindow w;
  w.take(5'000);
  w.clamp_future(4'000);
  EXPECT_FALSE(w.has_token(4'000, 1));
  EXPECT_TRUE(w.has_token(4'000 + kNanosPerSecond, 1));
}

TEST(OpsWindow, ALowerCapAppliesToOrdersAlreadyInTheWindow) {
  OpsWindow w;
  for (int i = 0; i < 5; ++i) {
    w.take(1'000);
  }
  EXPECT_TRUE(w.has_token(1'000, 6));
  EXPECT_FALSE(w.has_token(1'000, 5));
  EXPECT_FALSE(w.has_token(1'000, 2));
}

}  // namespace
}  // namespace vyuha::oms
