// F-EX-2 (ADR-024): the transition table, checked against the PRD's list, written out here independently, and
// property tests over random event sequences.

#include "vyuha/oms/order_state.h"

#include <algorithm>
#include <cstdint>
#include <exception>  // rapidcheck's headers use std::exception_ptr without including it
#include <map>
#include <optional>
#include <set>
#include <string>
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
      {{kCancelPending, E::kPartialFill}, kPartiallyFilled},
      {{kCancelPending, E::kFill}, kFilled},
      {{kCancelPending, E::kCancelReject}, kOpen},
      {{kModifyPending, E::kPartialFill}, kPartiallyFilled},
      {{kModifyPending, E::kFill}, kFilled},
      {{kModifyPending, E::kModifyReject}, kOpen},
      {{kOpen, E::kBrokerCancel}, kCancelled},
      {{kPartiallyFilled, E::kBrokerCancel}, kCancelled},
      {{kOpen, E::kBrokerReject}, kBrokerRejected},
      {{kOpen, E::kCancelRequest}, kCancelPending},
      {{kPartiallyFilled, E::kCancelRequest}, kCancelPending},
      {{kCancelPending, E::kCancelConfirm}, kCancelled},
      {{kOpen, E::kModifyRequest}, kModifyPending},
      {{kPartiallyFilled, E::kModifyRequest}, kModifyPending},
      {{kModifyPending, E::kModifyConfirm}, kOpen},
      {{kSent, E::kAckTimeout}, kUnknown},
      {{kSent, E::kPartialFill}, kPartiallyFilled},
      {{kSent, E::kFill}, kFilled},
      {{kSent, E::kBrokerCancel}, kCancelled},
      // A late ack: recorded, and the status stays.
      {{kPartiallyFilled, E::kBrokerAck}, kPartiallyFilled},
      {{kFilled, E::kBrokerAck}, kFilled},
      {{kCancelled, E::kBrokerAck}, kCancelled},
      {{kCancelPending, E::kBrokerAck}, kCancelPending},
      {{kModifyPending, E::kBrokerAck}, kModifyPending},
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
  [[nodiscard]] std::uint64_t pushed() const noexcept override { return events.size(); }
  [[nodiscard]] std::uint64_t durable() const noexcept override { return events.size(); }
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
    d.fill_qty = left > 0 ? left : 1;  // on a filled order, a late fill
  } else if (event == E::kPartialFill) {
    d.fill_qty = 1;
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

// A reconcile whose broker total can't be true of that state: "partly filled" with none or all of it, or
// "cancelled" with all of it. Oms refuses these, and recovery raises a mismatch.
bool contradictory_total(OrderEvent event, const EventDetails& d, std::int32_t qty) {
  return (event == E::kReconcilePartial && (d.broker_filled_qty <= 0 || d.broker_filled_qty >= qty)) ||
         (event == E::kReconcileCancelled && d.broker_filled_qty >= qty);
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
        const auto next = next_status(from, event);
        EXPECT_TRUE(!next.has_value() || next == from)
            << to_string(from) << " event " << static_cast<int>(event);
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

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one property, and its oracle is the point
TEST(OmsProperty, OnlyListedTransitionsHappenAndEveryOneIsRecorded) {
  std::uint64_t refused_illegal = 0;
  std::uint64_t late_fills = 0;
  const bool ok = rc::check(
      "random event sequences",
      [&](const std::vector<std::pair<std::uint8_t, std::uint8_t>>& raw, std::uint8_t qty_raw) {
        const FixedClock clock;
        RecordingSink sink;
        Oms oms(sink, clock, 1);
        const auto handle = oms.create(order_of(1 + (qty_raw % 5)));
        RC_ASSERT(handle.has_value());
        std::uint64_t expected_illegal = 0;
        std::uint64_t expected_mismatches = 0;
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
          const bool is_fill = event == E::kPartialFill || event == E::kFill;
          if (is_fill) {
            // A fill is a fact: always recorded. Which fill it is comes from the totals, and the list says
            // what it does.
            const std::int32_t total = before.filled_qty + details.fill_qty;
            const OrderEvent actual = total >= before.details.qty ? E::kFill : E::kPartialFill;
            const auto listing = listed().find({before.status, actual});
            const bool mismatch = listing == listed().end() || total > before.details.qty;
            RC_ASSERT(result.has_value());
            RC_ASSERT(after.status == (listing != listed().end() ? listing->second : before.status));
            RC_ASSERT(after.filled_qty == std::min(total, before.details.qty));
            RC_ASSERT(sink.events.back().fill_qty == details.fill_qty);
            RC_ASSERT((sink.events.back().reason_code.view() == kReconMismatch) == mismatch);
            expected_mismatches += mismatch ? 1 : 0;
            late_fills += mismatch ? 1 : 0;
          } else if (const auto listing = listed().find({before.status, event}); listing == listed().end()) {
            // The oracle is the PRD's list, not the code under test.
            ++expected_illegal;
            ++refused_illegal;
            RC_ASSERT(!result.has_value());
            RC_ASSERT(result.error().code == ErrorCode::kIllegalTransition);
          } else {
            // Listed events succeed, bar a broker total the order can't hold.
            const bool unrepresentable = contradictory_total(event, details, before.details.qty);
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
        }
        RC_ASSERT(oms.illegal_transitions() == expected_illegal);
        RC_ASSERT(oms.recon_mismatches() == expected_mismatches);
      });
  EXPECT_TRUE(ok);
  EXPECT_GT(refused_illegal, 0U);
  EXPECT_GT(late_fills, 0U);
}

// The walks above are random, so this proves deterministically that the real Oms can reach every status.
TEST(Oms, EveryStatusIsReachableThroughListedEvents) {
  std::set<OrderStatus> reached{kNew};
  std::vector<std::vector<OrderEvent>> walks{{}};
  for (std::size_t next = 0; next < walks.size(); ++next) {
    const std::vector<OrderEvent> walk = walks[next];
    const FixedClock clock;
    RecordingSink sink;
    Oms oms(sink, clock, 1);
    const OrderHandle handle = oms.create(order_of(3)).value();
    for (const OrderEvent event : walk) {
      ASSERT_TRUE(oms.apply(handle, event, valid_details(oms.order(handle), event)).has_value());
    }
    for (const OrderEvent event : all_events()) {
      const auto listing = listed().find({oms.order(handle).status, event});
      if (listing != listed().end() && reached.insert(listing->second).second) {
        std::vector<OrderEvent> longer = walk;
        longer.push_back(event);
        walks.push_back(std::move(longer));
      }
    }
  }
  EXPECT_EQ(reached.size(), std::size_t{kOrderStatusCount});
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
    const EventDetails details = valid_details(before, event);
    const bool is_fill = event == E::kPartialFill || event == E::kFill;
    const bool would_apply = is_fill || (listed().contains({before.status, event}) &&
                                         !contradictory_total(event, details, before.details.qty));
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
  OrderDetails second = order_of(1);
  ASSERT_TRUE(second.client_order_id.assign("client-2"));
  EXPECT_EQ(oms.create(second).error().code, ErrorCode::kCapacityExceeded);
}

TEST(Oms, FillsAddUpWhicheverEventTheBrokerUpdateCameAs) {
  Walked w;
  walk_to_open(w, 10);
  const Price px = Price::from_units(10'000);
  EXPECT_FALSE(
      w.oms.apply(w.handle, E::kPartialFill, {.fill_qty = 0, .fill_price = px}));  // no shares: garbage
  EXPECT_FALSE(w.oms.apply(w.handle, E::kPartialFill, {.fill_qty = -1, .fill_price = px}));
  EXPECT_EQ(w.oms.illegal_transitions(), 0U);
  EXPECT_EQ(w.oms.apply(w.handle, E::kFill, {.fill_qty = 3, .fill_price = px}), kPartiallyFilled);
  EXPECT_EQ(w.oms.apply(w.handle, E::kPartialFill, {.fill_qty = 3, .fill_price = px}), kPartiallyFilled);
  EXPECT_EQ(w.oms.apply(w.handle, E::kPartialFill, {.fill_qty = 4, .fill_price = px}), kFilled);
  EXPECT_EQ(w.oms.order(w.handle).filled_qty, 10);
  EXPECT_EQ(w.oms.order(w.handle).broker_order_id.view(), "B1");
  EXPECT_EQ(w.oms.recon_mismatches(), 0U);
}

TEST(Oms, AFillRacingACancelOrModifyIsAccepted) {
  const Price px = Price::from_units(10'000);
  Walked a;
  walk_to_open(a, 10);
  ASSERT_TRUE(a.oms.apply(a.handle, E::kCancelRequest));
  EXPECT_EQ(a.oms.apply(a.handle, E::kPartialFill, {.fill_qty = 4, .fill_price = px}), kPartiallyFilled);
  ASSERT_TRUE(a.oms.apply(a.handle, E::kCancelRequest));
  EXPECT_EQ(a.oms.apply(a.handle, E::kFill, {.fill_qty = 6, .fill_price = px}), kFilled);
  EXPECT_EQ(a.oms.recon_mismatches(), 0U);

  Walked b;
  walk_to_open(b, 10);
  ASSERT_TRUE(b.oms.apply(b.handle, E::kModifyRequest));
  EXPECT_EQ(b.oms.apply(b.handle, E::kPartialFill, {.fill_qty = 4, .fill_price = px}), kPartiallyFilled);
  ASSERT_TRUE(b.oms.apply(b.handle, E::kModifyRequest));
  EXPECT_EQ(b.oms.apply(b.handle, E::kModifyReject), kOpen);
  EXPECT_EQ(b.oms.order(b.handle).filled_qty, 4);  // kept, though the status says OPEN
  ASSERT_TRUE(b.oms.apply(b.handle, E::kCancelRequest));
  EXPECT_EQ(b.oms.apply(b.handle, E::kCancelReject), kOpen);
}

TEST(Oms, BrokerInitiatedEndsKeepFilledQty) {
  const Price px = Price::from_units(10'000);
  Walked a;
  walk_to_open(a, 10);
  ASSERT_TRUE(a.oms.apply(a.handle, E::kPartialFill, {.fill_qty = 4, .fill_price = px}));
  EXPECT_EQ(a.oms.apply(a.handle, E::kBrokerCancel, {.reason_code = "IOC_REMAINDER"}), kCancelled);
  EXPECT_EQ(a.oms.order(a.handle).filled_qty, 4);

  Walked b;
  walk_to_open(b, 10);
  EXPECT_EQ(b.oms.apply(b.handle, E::kBrokerReject, {.reason_code = "RMS"}), kBrokerRejected);
}

TEST(Oms, ALateFillOnATerminalOrderIsRecordedAndRaisesAMismatch) {
  const Price px = Price::from_units(10'000);
  Walked w;
  walk_to_open(w, 10);
  ASSERT_TRUE(w.oms.apply(w.handle, E::kBrokerCancel));
  const std::size_t before = w.sink.events.size();
  EXPECT_EQ(w.oms.apply(w.handle, E::kPartialFill, {.fill_qty = 2, .fill_price = px}), kCancelled);
  EXPECT_EQ(w.oms.order(w.handle).filled_qty, 2);
  EXPECT_EQ(w.oms.recon_mismatches(), 1U);
  ASSERT_EQ(w.sink.events.size(), before + 1);
  EXPECT_EQ(w.sink.events.back().reason_code.view(), kReconMismatch);
  EXPECT_EQ(w.sink.events.back().from, kCancelled);
  EXPECT_EQ(w.sink.events.back().to, kCancelled);
  EXPECT_EQ(w.sink.events.back().fill_qty, 2);
}

TEST(Oms, AnOverfillIsRecordedInFullButCappedOnTheOrder) {
  const Price px = Price::from_units(10'000);
  Walked w;
  walk_to_open(w, 10);
  EXPECT_EQ(w.oms.apply(w.handle, E::kFill, {.fill_qty = 15, .fill_price = px}), kFilled);
  EXPECT_EQ(w.oms.order(w.handle).filled_qty, 10);  // the column's CHECK; the event keeps the real number
  EXPECT_EQ(w.sink.events.back().fill_qty, 15);
  EXPECT_EQ(w.sink.events.back().reason_code.view(), kReconMismatch);
  EXPECT_EQ(w.oms.apply(w.handle, E::kFill, {.fill_qty = 1, .fill_price = px}),
            kFilled);  // and again, after FILLED
  EXPECT_EQ(w.oms.recon_mismatches(), 2U);
}

TEST(Oms, AFillInAStateWithNoFillTransitionKeepsTheStatus) {
  const Price px = Price::from_units(10'000);
  Walked w;
  w.handle = w.oms.create(order_of(10)).value();
  ASSERT_TRUE(w.oms.apply(w.handle, E::kRiskApprove));
  ASSERT_TRUE(w.oms.apply(w.handle, E::kSend));
  ASSERT_TRUE(w.oms.apply(w.handle, E::kAckTimeout));
  EXPECT_EQ(w.oms.apply(w.handle, E::kPartialFill, {.fill_qty = 2, .fill_price = px}), kUnknown);
  EXPECT_EQ(w.oms.order(w.handle).filled_qty, 2);
  EXPECT_EQ(w.oms.recon_mismatches(), 1U);
  EXPECT_EQ(w.oms.illegal_transitions(), 0U);
}

TEST(Oms, AFillCanBeatTheAckAndTheLateAckIsARecordedNoOp) {
  const Price px = Price::from_units(10'000);
  Walked w;
  w.handle = w.oms.create(order_of(10)).value();
  ASSERT_TRUE(w.oms.apply(w.handle, E::kRiskApprove));
  ASSERT_TRUE(w.oms.apply(w.handle, E::kSend));
  EXPECT_EQ(w.oms.apply(w.handle, E::kPartialFill, {.fill_qty = 4, .fill_price = px}), kPartiallyFilled);
  const std::size_t before = w.sink.events.size();
  EXPECT_EQ(w.oms.apply(w.handle, E::kBrokerAck, {.broker_order_id = "B1"}), kPartiallyFilled);
  ASSERT_EQ(w.sink.events.size(), before + 1);
  EXPECT_EQ(w.sink.events.back().reason_code.view(), kLateAck);
  EXPECT_EQ(w.sink.events.back().from, kPartiallyFilled);
  EXPECT_EQ(w.sink.events.back().to, kPartiallyFilled);
  EXPECT_EQ(w.oms.order(w.handle).broker_order_id.view(), "B1");  // the ack's one contribution
  EXPECT_EQ(w.oms.order(w.handle).filled_qty, 4);
  EXPECT_EQ(w.oms.recon_mismatches(), 0U);
  EXPECT_EQ(w.oms.illegal_transitions(), 0U);
}

TEST(Oms, ACompleteFillOrCancelBeforeTheAckAlsoTakesALateAck) {
  const Price px = Price::from_units(10'000);
  Walked filled;
  filled.handle = filled.oms.create(order_of(10)).value();
  ASSERT_TRUE(filled.oms.apply(filled.handle, E::kRiskApprove));
  ASSERT_TRUE(filled.oms.apply(filled.handle, E::kSend));
  EXPECT_EQ(filled.oms.apply(filled.handle, E::kFill, {.fill_qty = 10, .fill_price = px}), kFilled);
  EXPECT_EQ(filled.oms.apply(filled.handle, E::kBrokerAck), kFilled);
  EXPECT_EQ(filled.oms.recon_mismatches(), 0U);
  EXPECT_EQ(filled.oms.illegal_transitions(), 0U);

  Walked cancelled;
  cancelled.handle = cancelled.oms.create(order_of(10)).value();
  ASSERT_TRUE(cancelled.oms.apply(cancelled.handle, E::kRiskApprove));
  ASSERT_TRUE(cancelled.oms.apply(cancelled.handle, E::kSend));
  EXPECT_EQ(cancelled.oms.apply(cancelled.handle, E::kBrokerCancel, {.reason_code = "IOC"}), kCancelled);
  EXPECT_EQ(cancelled.oms.apply(cancelled.handle, E::kBrokerAck), kCancelled);
  EXPECT_EQ(cancelled.oms.recon_mismatches(), 0U);
  EXPECT_EQ(cancelled.oms.illegal_transitions(), 0U);
}

TEST(Oms, AnAckOnAnOpenOrderIsStillIllegal) {
  Walked w;
  walk_to_open(w, 10);  // it has had its ack
  EXPECT_EQ(w.oms.apply(w.handle, E::kBrokerAck).error().code, ErrorCode::kIllegalTransition);
}

TEST(OmsIntake, TheSameRequestReturnsTheExistingOrderAndRecordsNothing) {
  Walked w;
  const auto first = w.oms.intake(order_of(10));
  ASSERT_TRUE(first.has_value());
  EXPECT_FALSE(first->existing);
  const std::size_t events = w.sink.events.size();
  OrderDetails again = order_of(10);
  again.id.bytes.fill(9);  // a retry gets a new engine id and a new timestamp; those aren't the request
  again.created_at = 123;
  const auto second = w.oms.intake(again);
  ASSERT_TRUE(second.has_value());
  EXPECT_TRUE(second->existing);
  EXPECT_EQ(second->handle, first->handle);
  EXPECT_EQ(w.sink.events.size(), events);
  EXPECT_EQ(w.oms.size(), 1U);
}

TEST(OmsIntake, ADifferentBodyIsOrderDup) {
  Walked w;
  ASSERT_TRUE(w.oms.intake(order_of(10)).has_value());
  const std::size_t events = w.sink.events.size();
  const auto changed = [](auto&& edit) {
    OrderDetails d = order_of(10);
    edit(d);
    return d;
  };
  for (const OrderDetails& d :
       {changed([](OrderDetails& o) { o.qty = 11; }), changed([](OrderDetails& o) { o.side = Side::kSell; }),
        changed([](OrderDetails& o) { o.price = Price::from_units(1); }),
        changed([](OrderDetails& o) { o.instrument_id = 7; }),
        changed([](OrderDetails& o) { o.is_paper = false; })}) {
    const auto dup = w.oms.intake(d);
    ASSERT_FALSE(dup.has_value());
    EXPECT_EQ(dup.error().code, ErrorCode::kOrderDup);
  }
  EXPECT_EQ(w.sink.events.size(), events);
  EXPECT_EQ(w.oms.size(), 1U);
}

TEST(OmsIntake, ARestoredOrderIsADuplicateToo) {
  Walked w;
  RecoveredOrder r;
  r.details = order_of(10);
  r.status = kOpen;
  r.seq = 3;
  const auto h = w.oms.restore(r);
  ASSERT_TRUE(h.has_value());
  const auto again = w.oms.intake(order_of(10));
  ASSERT_TRUE(again.has_value());
  EXPECT_TRUE(again->existing);
  EXPECT_EQ(again->handle, *h);
}

TEST(OmsIntake, ManyClientIdsDontCollideInTheIndex) {
  const FixedClock clock;
  RecordingSink sink;
  Oms oms(sink, clock, 64);
  for (int i = 0; i < 64; ++i) {
    OrderDetails d = order_of(1);
    ASSERT_TRUE(d.client_order_id.assign("client-" + std::to_string(i)));
    const auto taken = oms.intake(d);
    ASSERT_TRUE(taken.has_value());
    EXPECT_FALSE(taken->existing);
    EXPECT_TRUE(oms.intake(d).value_or(Intake{}).existing);
  }
}

TEST(OmsRefuse, OnlyAnUnsentOrderEndsRejected) {
  Walked w;
  w.handle = w.oms.create(order_of(10)).value();
  ASSERT_TRUE(w.oms.apply(w.handle, E::kRiskApprove));
  w.oms.refuse(w.oms.order(w.handle).details.id);
  EXPECT_EQ(w.oms.order(w.handle).status, kRiskRejected);
  EXPECT_FALSE(w.oms.can_send(w.handle));

  Walked live;
  walk_to_open(live, 10);
  live.oms.refuse(live.oms.order(live.handle).details.id);
  EXPECT_EQ(live.oms.order(live.handle).status, kOpen);  // it's at the broker: facts still matter
  EXPECT_EQ(live.oms.refused_by_storage(), 1U);
}

TEST(Oms, AZeroPriceFillIsStillAFillAndRaisesAMismatch) {
  Walked w;
  walk_to_open(w, 10);
  EXPECT_EQ(w.oms.apply(w.handle, E::kPartialFill, {.fill_qty = 2}), kPartiallyFilled);
  EXPECT_EQ(w.oms.recon_mismatches(), 1U);
}

TEST(Oms, AFullSinkRefusesAFillSoTheCallerRetries) {
  Walked w;
  walk_to_open(w, 10);
  w.sink.full = true;
  const EventDetails fill{.fill_qty = 2, .fill_price = Price::from_units(10'000)};
  EXPECT_EQ(w.oms.apply(w.handle, E::kPartialFill, fill).error().code, ErrorCode::kCapacityExceeded);
  EXPECT_EQ(w.oms.order(w.handle).filled_qty, 0);
  EXPECT_EQ(w.oms.recon_mismatches(), 0U);
  w.sink.full = false;
  EXPECT_EQ(w.oms.apply(w.handle, E::kPartialFill, fill), kPartiallyFilled);
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
