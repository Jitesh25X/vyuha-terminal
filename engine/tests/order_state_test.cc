// F-EX-2: the transition table, checked against the PRD's list, written out here independently, and property
// tests over random event sequences.

#include "vyuha/oms/order_state.h"

#include <cstdint>
#include <exception>  // rapidcheck's headers use std::exception_ptr without including it
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <rapidcheck.h>

#include "vyuha/core/clock.h"
#include "vyuha/oms/oms.h"

namespace vyuha::oms {
namespace {

using enum OrderStatus;
using E = OrderEvent;

const std::map<std::pair<OrderStatus, OrderEvent>, OrderStatus>& listed() {
  static const std::map<std::pair<OrderStatus, OrderEvent>, OrderStatus> table{
      {{kNew, E::kRiskReject}, kRiskRejected},
      {{kNew, E::kRiskApprove}, kApproved},
      {{kApproved, E::kSend}, kSent},
      {{kSent, E::kBrokerAck}, kOpen},
      {{kSent, E::kBrokerReject}, kBrokerRejected},
      {{kOpen, E::kPartialFill}, kPartiallyFilled},
      {{kPartiallyFilled, E::kPartialFill}, kPartiallyFilled},
      {{kOpen, E::kFill}, kFilled},
      {{kPartiallyFilled, E::kFill}, kFilled},
      {{kOpen, E::kCancelRequest}, kCancelPending},
      {{kPartiallyFilled, E::kCancelRequest}, kCancelPending},
      {{kCancelPending, E::kCancelConfirm}, kCancelled},
      {{kOpen, E::kModifyRequest}, kModifyPending},
      {{kPartiallyFilled, E::kModifyRequest}, kModifyPending},
      {{kModifyPending, E::kModifyConfirm}, kOpen},
      {{kSent, E::kAckTimeout}, kUnknown},
      {{kUnknown, E::kReconcileOpen}, kOpen},
      {{kUnknown, E::kReconcilePartial}, kPartiallyFilled},
      {{kUnknown, E::kReconcileFilled}, kFilled},
      {{kUnknown, E::kReconcileCancelled}, kCancelled},
      {{kUnknown, E::kReconcileRejected}, kBrokerRejected},
  };
  return table;
}

std::vector<OrderStatus> all_statuses() {
  std::vector<OrderStatus> out;
  for (std::uint8_t i = 1; i <= kOrderStatusCount; ++i) {
    out.push_back(static_cast<OrderStatus>(i));
  }
  return out;
}

std::vector<OrderEvent> all_events() {
  std::vector<OrderEvent> out;
  for (std::uint8_t i = 0; i < kOrderEventCount; ++i) {
    out.push_back(static_cast<OrderEvent>(i));
  }
  return out;
}

class FixedClock final : public IClock {
 public:
  [[nodiscard]] Nanos now() const noexcept override { return 1'000; }
};

class RecordingSink final : public IOrderEventSink {
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
  std::vector<OrderEventRecord> events;
  bool full{false};
  bool is_healthy{true};
};

OrderDetails order_of(std::int32_t qty) {
  OrderDetails d;
  d.id.bytes.fill(1);
  EXPECT_TRUE(d.client_order_id.assign("client-1"));
  EXPECT_TRUE(d.tag.assign("manual"));
  d.qty = qty;
  d.price = Price::from_units(1'000'000);
  return d;
}

// The details that make an event valid for this order, so properties reach deep states.
EventDetails valid_details(const Order& o, OrderEvent event) {
  EventDetails d;
  const std::int32_t left = o.details.qty - o.filled_qty;
  d.fill_price = Price::from_units(10'000);
  if (event == E::kFill) {
    d.fill_qty = left;
  } else if (event == E::kPartialFill) {
    d.fill_qty = left > 1 ? 1 : 0;
  } else if (event == E::kReconcilePartial) {
    d.broker_filled_qty = o.filled_qty + (left > 1 ? 1 : 0);
  } else if (event == E::kReconcileCancelled) {
    d.broker_filled_qty = o.filled_qty;
  }
  if (event == E::kRiskReject || event == E::kBrokerReject || event == E::kReconcileRejected) {
    d.reason_code = "R-TEST";
  }
  return d;
}

// An event the PRD lists for this status, if any: random walks would rarely get past NEW otherwise.
std::optional<OrderEvent> listed_event(OrderStatus from, std::uint8_t pick) {
  std::vector<OrderEvent> legal;
  for (const OrderEvent event : all_events()) {
    if (listed().contains({from, event})) {
      legal.push_back(event);
    }
  }
  return legal.empty() ? std::nullopt : std::optional{legal[pick % legal.size()]};
}

TEST(OrderState, TableIsExactlyThePrdList) {
  for (const OrderStatus from : all_statuses()) {
    for (const OrderEvent event : all_events()) {
      const auto it = listed().find({from, event});
      const auto got = next_status(from, event);
      if (it == listed().end()) {
        EXPECT_FALSE(got.has_value()) << to_string(from) << " event " << static_cast<int>(event);
      } else {
        EXPECT_EQ(got, it->second) << to_string(from) << " event " << static_cast<int>(event);
      }
    }
  }
}

TEST(OrderState, TerminalStatesHaveNoWayOut) {
  for (const OrderStatus from : all_statuses()) {
    if (is_terminal(from)) {
      for (const OrderEvent event : all_events()) {
        EXPECT_FALSE(next_status(from, event).has_value());
      }
    }
  }
}

TEST(OrderState, StatusNamesRoundTrip) {
  for (const OrderStatus s : all_statuses()) {
    EXPECT_EQ(parse_status(to_string(s)), s);
  }
  EXPECT_FALSE(parse_status("nonsense").has_value());
}

TEST(OmsProperty, OnlyListedTransitionsHappenAndEveryOneIsRecorded) {
  std::set<OrderStatus> reached;
  std::uint64_t refused_illegal = 0;
  const bool ok = rc::check(
      "random event sequences",
      [&](const std::vector<std::pair<std::uint8_t, std::uint8_t>>& raw, std::uint8_t qty_raw) {
        const FixedClock clock;
        RecordingSink sink;
        Oms oms(sink, clock, 1);
        const auto handle = oms.create(order_of(1 + (qty_raw % 5)));
        RC_ASSERT(handle.has_value());
        std::uint64_t expected_illegal = 0;
        bool was_unknown = false;
        for (const auto& [pick, arbitrary] : raw) {
          const Order before = oms.order(*handle);
          // Mostly a listed event, to walk deep; one in four anything at all.
          const auto wanted = pick % 4 != 0 ? listed_event(before.status, arbitrary) : std::nullopt;
          const OrderEvent event = wanted.value_or(static_cast<OrderEvent>(arbitrary % kOrderEventCount));
          const std::size_t events_before = sink.events.size();
          const EventDetails details = valid_details(before, event);
          const auto result = oms.apply(*handle, event, details);
          const Order& after = oms.order(*handle);
          const auto listing = listed().find({before.status, event});

          if (listing == listed().end()) {  // the oracle is the PRD's list, not the code under test
            ++expected_illegal;
            ++refused_illegal;
            RC_ASSERT(!result.has_value());
            RC_ASSERT(result.error().code == ErrorCode::kIllegalTransition);
          } else {
            // Listed events succeed, bar the partial fill or reconcile that one lot left can't represent.
            const bool unrepresentable = (event == E::kPartialFill && details.fill_qty == 0) ||
                                         (event == E::kReconcilePartial && details.broker_filled_qty == 0);
            RC_ASSERT(result.has_value() != unrepresentable);
            if (result.has_value()) {
              RC_ASSERT(after.status == listing->second);
            }
          }
          if (result.has_value()) {
            RC_ASSERT(sink.events.size() == events_before + 1);
            const OrderEventRecord& e = sink.events.back();
            RC_ASSERT(e.from == before.status);
            RC_ASSERT(e.to == after.status);
            RC_ASSERT(e.seq == before.seq + 1);
            RC_ASSERT(after.seq == e.seq);
            RC_ASSERT(e.filled_qty == after.filled_qty);
          } else {  // a refused event changes nothing and records nothing
            RC_ASSERT(sink.events.size() == events_before);
            RC_ASSERT(after.status == before.status);
            RC_ASSERT(after.seq == before.seq);
            RC_ASSERT(after.filled_qty == before.filled_qty);
          }
          RC_ASSERT(after.filled_qty >= before.filled_qty);
          RC_ASSERT(after.filled_qty <= after.details.qty);
          RC_ASSERT(after.status != kFilled || after.filled_qty == after.details.qty);
          was_unknown = was_unknown || before.status == kUnknown;
          RC_ASSERT(!was_unknown || (after.status != kApproved && after.status != kSent));  // never re-sent
          reached.insert(after.status);
        }
        RC_ASSERT(oms.illegal_transitions() == expected_illegal);
      });
  EXPECT_TRUE(ok);
  EXPECT_GT(refused_illegal, 0U);
  for (const OrderStatus s : all_statuses()) {
    if (s != kNew) {
      EXPECT_TRUE(reached.contains(s)) << "the walks never reached " << to_string(s);
    }
  }
}

TEST(OmsProperty, AFullSinkLeavesTheOrderUnchangedInEveryState) {
  const bool ok = rc::check("sink full", [](const std::vector<std::uint8_t>& walk, std::uint8_t raw) {
    const FixedClock clock;
    RecordingSink sink;
    Oms oms(sink, clock, 1);
    const auto handle = oms.create(order_of(3));
    RC_ASSERT(handle.has_value());
    for (const std::uint8_t pick : walk) {
      const auto event = listed_event(oms.order(*handle).status, pick);
      if (event.has_value()) {
        if (!oms.apply(*handle, *event, valid_details(oms.order(*handle), *event)).has_value()) {
          continue;  // a partial fill that one lot left can't represent
        }
      }
    }
    const Order before = oms.order(*handle);
    sink.full = true;
    const auto event = static_cast<OrderEvent>(raw % kOrderEventCount);
    const auto result = oms.apply(*handle, event, valid_details(before, event));
    const bool one_left = before.details.qty - before.filled_qty == 1;
    const bool needs_two = event == E::kPartialFill || event == E::kReconcilePartial;
    const bool would_apply = listed().contains({before.status, event}) && (!one_left || !needs_two);
    RC_ASSERT(!result.has_value());
    if (would_apply) {
      RC_ASSERT(result.error().code == ErrorCode::kCapacityExceeded);
    }
    RC_ASSERT(oms.order(*handle).status == before.status);
    RC_ASSERT(oms.order(*handle).seq == before.seq);
  });
  EXPECT_TRUE(ok);
}

struct Walked {
  FixedClock clock;
  RecordingSink sink;
  Oms oms{sink, clock, 4};
  OrderHandle handle{0};
};

void walk_to_open(Walked& w, std::int32_t qty) {
  w.handle = w.oms.create(order_of(qty)).value();
  ASSERT_TRUE(w.oms.apply(w.handle, E::kRiskApprove));
  ASSERT_TRUE(w.oms.apply(w.handle, E::kSend));
  ASSERT_TRUE(w.oms.apply(w.handle, E::kBrokerAck, {.broker_order_id = "B1"}));
}

TEST(Oms, FirstEventInsertsTheOrder) {
  Walked w;
  w.handle = w.oms.create(order_of(10)).value();
  ASSERT_EQ(w.sink.events.size(), 1U);
  EXPECT_FALSE(w.sink.events[0].from.has_value());
  EXPECT_EQ(w.sink.events[0].to, kNew);
  EXPECT_EQ(w.sink.events[0].seq, 1);
  EXPECT_EQ(w.sink.events[0].details.qty, 10);
}

TEST(Oms, RefusesOrdersPostgresWouldRefuse) {
  Walked w;
  EXPECT_FALSE(w.oms.create(order_of(0)));
  OrderDetails no_price = order_of(1);
  no_price.price.reset();
  EXPECT_FALSE(w.oms.create(no_price));
  OrderDetails no_trigger = order_of(1);
  no_trigger.order_type = OrderType::kSl;
  EXPECT_FALSE(w.oms.create(no_trigger));
  EXPECT_TRUE(w.sink.events.empty());
}

TEST(Oms, PoolFullIsAnErrorAndSinkFullLeavesNoOrder) {
  const FixedClock clock;
  RecordingSink sink;
  Oms oms(sink, clock, 1);
  sink.full = true;
  EXPECT_EQ(oms.create(order_of(1)).error().code, ErrorCode::kCapacityExceeded);
  EXPECT_EQ(oms.size(), 0U);
  sink.full = false;
  ASSERT_TRUE(oms.create(order_of(1)));
  EXPECT_EQ(oms.create(order_of(1)).error().code, ErrorCode::kCapacityExceeded);
}

TEST(Oms, FillsAddUpAndAnOverfillIsRefused) {
  Walked w;
  walk_to_open(w, 10);
  const Price px = Price::from_units(10'000);
  EXPECT_FALSE(
      w.oms.apply(w.handle, E::kPartialFill, {.fill_qty = 10, .fill_price = px}));  // that would complete it
  EXPECT_FALSE(w.oms.apply(w.handle, E::kFill, {.fill_qty = 11, .fill_price = px}));
  EXPECT_FALSE(w.oms.apply(w.handle, E::kFill, {.fill_qty = 9, .fill_price = px}));
  EXPECT_FALSE(w.oms.apply(w.handle, E::kPartialFill, {.fill_qty = 0, .fill_price = px}));
  EXPECT_FALSE(w.oms.apply(w.handle, E::kPartialFill, {.fill_qty = -1, .fill_price = px}));
  EXPECT_FALSE(w.oms.apply(w.handle, E::kPartialFill, {.fill_qty = 1}));  // no price
  EXPECT_EQ(w.oms.illegal_transitions(), 0U);
  EXPECT_EQ(w.oms.refused_fills(), 6U);
  EXPECT_EQ(w.oms.apply(w.handle, E::kPartialFill, {.fill_qty = 3, .fill_price = px}), kPartiallyFilled);
  EXPECT_EQ(w.oms.apply(w.handle, E::kPartialFill, {.fill_qty = 3, .fill_price = px}), kPartiallyFilled);
  EXPECT_EQ(w.oms.apply(w.handle, E::kFill, {.fill_qty = 4, .fill_price = px}), kFilled);
  EXPECT_EQ(w.oms.order(w.handle).filled_qty, 10);
  EXPECT_EQ(w.oms.order(w.handle).broker_order_id.view(), "B1");
}

TEST(Oms, UnknownIsNeverSentAgain) {
  Walked w;
  w.handle = w.oms.create(order_of(1)).value();
  ASSERT_TRUE(w.oms.apply(w.handle, E::kRiskApprove));
  ASSERT_TRUE(w.oms.apply(w.handle, E::kSend));
  EXPECT_EQ(w.oms.apply(w.handle, E::kAckTimeout), kUnknown);
  EXPECT_EQ(w.oms.apply(w.handle, E::kSend).error().code, ErrorCode::kIllegalTransition);
  EXPECT_EQ(w.oms.illegal_transitions(), 1U);
  EXPECT_EQ(w.oms.apply(w.handle, E::kReconcilePartial, {.broker_filled_qty = 2}).error().code,
            ErrorCode::kInvalidArgument);  // more than the order
  EXPECT_EQ(w.oms.apply(w.handle, E::kReconcileFilled), kFilled);
  EXPECT_EQ(w.oms.order(w.handle).filled_qty, 1);
}

TEST(Oms, ARestoredOrderContinuesItsSeqAndIsNeverSent) {
  Walked w;
  RecoveredOrder r;
  r.details = order_of(5);
  r.status = kApproved;
  r.seq = 2;
  const auto handle = w.oms.restore(r);
  ASSERT_TRUE(handle);
  EXPECT_TRUE(w.sink.events.empty());  // restoring records nothing
  EXPECT_TRUE(w.oms.order(*handle).needs_reconcile);
  // The SENT event may have been lost in the crash, so the broker may already have this order.
  EXPECT_EQ(w.oms.apply(*handle, E::kSend).error().code, ErrorCode::kIllegalTransition);

  RecoveredOrder open = r;
  open.status = kOpen;
  open.seq = 4;
  open.filled_qty = 1;
  open.broker_order_id = "B9";
  const auto h2 = w.oms.restore(open);
  ASSERT_TRUE(h2);
  EXPECT_EQ(w.oms.apply(*h2, E::kModifyRequest).error().code, ErrorCode::kIllegalTransition);
  ASSERT_TRUE(w.oms.confirm_reconciled(*h2));
  EXPECT_EQ(w.oms.apply(*h2, E::kModifyRequest), kModifyPending);
  EXPECT_EQ(w.oms.apply(*h2, E::kModifyConfirm), kOpen);
  EXPECT_EQ(w.oms.apply(*h2, E::kFill, {.fill_qty = 4, .fill_price = Price::from_units(10'000)}), kFilled);
  EXPECT_EQ(w.sink.events.back().seq, 7);
  EXPECT_EQ(w.oms.order(*h2).broker_order_id.view(), "B9");

  RecoveredOrder terminal = r;
  terminal.status = kFilled;
  EXPECT_FALSE(w.oms.restore(terminal));
}

TEST(Oms, NothingIsSentWhileEventsArentReachingStorage) {
  Walked w;
  w.handle = w.oms.create(order_of(1)).value();
  ASSERT_TRUE(w.oms.apply(w.handle, E::kRiskApprove));
  w.sink.is_healthy = false;
  EXPECT_EQ(w.oms.apply(w.handle, E::kSend).error().code, ErrorCode::kOrderLogUnavailable);
  EXPECT_EQ(w.oms.order(w.handle).status, kApproved);
  EXPECT_EQ(w.oms.illegal_transitions(), 0U);
  w.sink.is_healthy = true;
  EXPECT_EQ(w.oms.apply(w.handle, E::kSend), kSent);
}

TEST(Oms, ReconcileCancelledTakesTheBrokersFilledQty) {
  Walked w;
  w.handle = w.oms.create(order_of(5)).value();
  ASSERT_TRUE(w.oms.apply(w.handle, E::kRiskApprove));
  ASSERT_TRUE(w.oms.apply(w.handle, E::kSend));
  ASSERT_TRUE(w.oms.apply(w.handle, E::kAckTimeout));
  EXPECT_FALSE(w.oms.apply(w.handle, E::kReconcileCancelled, {.broker_filled_qty = 5}));  // that's FILLED
  EXPECT_EQ(w.oms.apply(w.handle, E::kReconcileCancelled, {.broker_filled_qty = 2}), kCancelled);
  EXPECT_EQ(w.oms.order(w.handle).filled_qty, 2);
}

TEST(Oms, RejectionsNeedAReasonAndPricesMustFitTheColumn) {
  Walked w;
  w.handle = w.oms.create(order_of(1)).value();
  EXPECT_FALSE(w.oms.apply(w.handle, E::kRiskReject));
  EXPECT_EQ(w.oms.apply(w.handle, E::kRiskReject, {.reason_code = "R-KILL"}), kRiskRejected);
  OrderDetails huge = order_of(1);
  huge.price = Price::from_units(1'000'000'000'000'000'000);
  EXPECT_FALSE(w.oms.create(huge));
  OrderDetails negative = order_of(1);
  negative.market_protection_millipct = -500;
  EXPECT_FALSE(w.oms.create(negative));
}

TEST(Uuid, RoundTrips) {
  const std::string text = "0190a3b4-7c2d-7e11-8f00-aabbccddeeff";
  const auto id = Uuid::parse(text);
  ASSERT_TRUE(id.has_value());
  if (id.has_value()) {
    EXPECT_EQ(id->to_string(), text);
  }
  EXPECT_FALSE(Uuid::parse("not-a-uuid").has_value());
  EXPECT_FALSE(Uuid::parse("0190a3b4-7c2d-7e11-8f00-aabbccddeefg").has_value());
  EXPECT_FALSE(Uuid::parse("0190a3b4x7c2d-7e11-8f00-aabbccddeeff").has_value());
}

}  // namespace
}  // namespace vyuha::oms
