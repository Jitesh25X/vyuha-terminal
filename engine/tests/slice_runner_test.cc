// F-EX-3 / F-RK-1, M2.5b: the freeze-quantity slicer and its runner. D-45 (a refused child stops the rest),
// D-48 (the parent link), D-49 (the parent check), D-50 (pacing), D-51 (no freeze quantity), D-52 (children
// strictly below the freeze quantity). Every child goes through the real 22-rule gate, the real Oms and the
// real OrderRouter. Freeze quantities here are made-up test data: production reads them from the instrument
// master only.

#include "vyuha/oms/slice_runner.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>  // rapidcheck's headers use std::exception_ptr without including it
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include "scope_desk.h"
#include "vyuha/oms/freeze_slicer.h"
#include "vyuha/oms/order_router.h"
#include "vyuha/sim/sim_broker.h"

// NOLINTBEGIN(bugprone-unchecked-optional-access): every test does ASSERT_TRUE(has_value()) first
namespace vyuha::risk {
namespace {

using oms::FreezeSlicer;
using oms::Oms;
using oms::OrderEvent;
using oms::OrderEventRecord;
using oms::OrderHandle;
using oms::OrderRouter;
using oms::OrderStatus;
using oms::SliceRunner;
using oms::SliceRunnerConfig;
using oms::SliceStart;
using oms::SliceStop;
using oms::SliceView;
using Kind = SliceStart::Kind;

// A check-only evaluation can't hand out an approval: its result has nowhere to put one.
template <class T>
concept HasApproved = requires(T t) { t.approved; };
static_assert(HasApproved<GateDecision>);
static_assert(!HasApproved<GateCheck>);

constexpr std::int64_t kSliced = 20;
constexpr Nanos kMillis = 1'000'000;

class SeqIds final : public oms::IOrderIdSource {
 public:
  // NOLINTNEXTLINE(bugprone-exception-escape): the indices are in range, so at() cannot throw
  [[nodiscard]] Uuid next() noexcept override {
    ++drawn;
    Uuid id;
    id.bytes.fill(0xC0);
    id.bytes.at(14) = static_cast<std::uint8_t>(drawn >> 8);
    id.bytes.at(15) = static_cast<std::uint8_t>(drawn);
    return id;
  }
  std::uint32_t drawn{0};
};

// An order log a test can fill, break, slow down or make refuse an order.
class Sink final : public oms::IOrderEventSink {
 public:
  // NOLINTNEXTLINE(bugprone-exception-escape): a test's allocation failure may terminate
  [[nodiscard]] bool try_push(const OrderEventRecord& event) noexcept override {
    if (room == 0) {
      return false;
    }
    --room;
    events.push_back(event);
    if (slow_clock != nullptr) {
      slow_clock->advance(slow_by);
    }
    return true;
  }
  [[nodiscard]] bool healthy() const noexcept override { return up; }
  [[nodiscard]] std::uint64_t pushed() const noexcept override { return events.size(); }
  [[nodiscard]] std::uint64_t durable() const noexcept override {
    return durable_up_to.value_or(events.size());
  }
  [[nodiscard]] bool try_pop_refused(Uuid& order_id) noexcept override {
    if (refused.empty()) {
      return false;
    }
    order_id = refused.back();
    refused.pop_back();
    return true;
  }
  void note_outage_refusal(oms::OutageRefusal /*kind*/) noexcept override { ++outage_notes; }

  std::vector<OrderEventRecord> events;
  std::vector<Uuid> refused;
  std::optional<std::uint64_t> durable_up_to;  // set: the writer is behind
  std::size_t room{1'000'000};
  bool up{true};
  int outage_notes{0};
  SimClock* slow_clock{nullptr};
  Nanos slow_by{0};
};

class Broker final : public oms::IOrderSender {
 public:
  // NOLINTNEXTLINE(bugprone-exception-escape): a test's allocation failure may terminate
  [[nodiscard]] bool send(const ApprovedOrder& approved, std::string_view /*tag*/) noexcept override {
    sent.emplace_back(approved.order().client_order_id.view());
    sent_qty.push_back(approved.order().qty);
    return true;
  }
  // NOLINTNEXTLINE(bugprone-exception-escape)
  [[nodiscard]] bool cancel(const oms::Order& order) noexcept override {
    cancelled.emplace_back(order.details.client_order_id.view());
    return true;
  }
  std::vector<std::string> sent;
  std::vector<std::int32_t> sent_qty;
  std::vector<std::string> cancelled;
};

struct Setup {
  std::int32_t lot{1};
  std::int32_t freeze{11};  // largest child 10
  std::size_t oms_capacity{256};
  std::size_t router_capacity{256};
  SliceRunnerConfig config{.max_parents = 4, .max_children = 100};
};

struct SliceDesk : ScopeDesk {
  Sink sink;
  Broker broker;
  SeqIds ids;
  RiskGate gate;
  std::unique_ptr<Oms> oms;
  GateSendLimits limits;
  std::unique_ptr<OrderRouter> router;
  std::unique_ptr<SliceRunner> runner;
  std::int32_t lot;

  explicit SliceDesk(const Setup& s = {})
      : gate(RiskGate::create(all_rules(), clock).value()), limits(ctx), lot(s.lot) {
    instrument(s.lot, s.freeze);
    tick(kSliced, rupees(100));
    mark(kSliced, rupees(100));
    // Room for the large parents these tests use.
    relimit(LimitKey::kMaxOrderValue, rupees(100'000'000).units());
    relimit(LimitKey::kMaxOrderQtyLots, 100'000 * kFromScale);
    relimit(LimitKey::kMaxPositionLots, 100'000 * kFromScale);
    relimit(LimitKey::kMaxPositionNotional, rupees(100'000'000).units());
    relimit(LimitKey::kMaxGrossExposure, rupees(100'000'000).units());
    relimit(LimitKey::kMaxOpenOrders, 1'000 * kFromScale);
    oms = std::make_unique<Oms>(sink, clock, s.oms_capacity);
    router = std::make_unique<OrderRouter>(*oms, sink, broker, oms::Venue::kPaper,
                                           oms::RouterConfig{.tag_limit = 20, .capacity = s.router_capacity},
                                           limits);
    runner = std::make_unique<SliceRunner>(gate, ctx, *oms, *router, sink, ids, s.config);
  }

  // A future on the desk's share: only F&O is sliced.
  void instrument(std::int32_t lot_size, std::int32_t freeze) {
    ASSERT_TRUE(instruments.put(kSliced, {.lot_size = lot_size,
                                          .tick_size = Price::from_units(500),
                                          .exchange = Exchange::kNfo,
                                          .segment = Segment::kFut,
                                          .underlying_id = kEquity,
                                          .lower_circuit = rupees(90),
                                          .upper_circuit = rupees(110),
                                          .freeze_qty = freeze}));
  }
  void relimit(LimitKey key, std::int64_t value) {
    clear_limits(key);
    limit(key, value);
  }
  void ops_cap(std::uint8_t cap) { ctx.accounts.find(kAccount)->ops_cap = cap; }

  [[nodiscard]] OrderDetails parent(std::int32_t qty, std::string_view cid = "P1",
                                    std::uint8_t id = 40) const {
    OrderDetails o = order();
    o.id = id_of(id);
    EXPECT_TRUE(o.client_order_id.assign(cid));
    o.instrument_id = kSliced;
    o.qty = qty;
    return o;
  }
  [[nodiscard]] SliceStart start(const OrderDetails& p) const {
    const auto out = runner->start(p);
    EXPECT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().detail);
    return out.value_or(SliceStart{});
  }
  [[nodiscard]] SliceView view(std::uint8_t id = 40) const {
    const auto v = runner->view(id_of(id));
    EXPECT_TRUE(v.has_value());
    return v.value_or(SliceView{});
  }
  [[nodiscard]] static OrderDetails at_price(OrderDetails o, std::int64_t price) {
    o.price = rupees(price);
    return o;
  }
  // A second later, with the prices fresh again: the OPS tokens are back.
  void next_second() {
    clock.advance(kNanosPerSecond);
    tick(kSliced, rupees(100), clock.monotonic());
    restamp_marks();
  }
  [[nodiscard]] std::optional<OrderHandle> child(std::string_view cid) const { return oms->find_handle(cid); }
  [[nodiscard]] OrderStatus status(std::string_view cid) const {
    const auto h = child(cid);
    EXPECT_TRUE(h.has_value()) << cid;
    return h.has_value() ? oms->order(*h).status : OrderStatus::kNew;
  }
  // The reason on the last event of the order with this client id.
  [[nodiscard]] std::string last_reason(std::string_view cid) const {
    const auto h = child(cid);
    if (!h.has_value()) {
      return "no such order";
    }
    const Uuid id = oms->order(*h).details.id;
    for (const OrderEventRecord& event : std::views::reverse(sink.events)) {
      if (event.order_id == id) {
        return std::string(event.reason_code.view());
      }
    }
    return "no event";
  }
  // The OPS buckets, duplicate entries: what an approval uses up.
  [[nodiscard]] std::pair<std::array<oms::OpsWindow, oms::kOpsVenues>, std::size_t> used_state() const {
    return {std::as_const(ctx).accounts.find(kAccount)->ops, ctx.dup.count};
  }
};

std::vector<std::int32_t> child_sizes(std::int32_t qty, std::int32_t lot, std::int32_t freeze) {
  const std::int32_t largest = FreezeSlicer::largest_child(freeze, lot);
  OrderDetails p{.is_paper = oms::kPaperOrder};
  EXPECT_TRUE(p.client_order_id.assign("P"));
  p.qty = qty;
  SeqIds ids;
  std::vector<OrderDetails> out(100, p);
  const std::size_t n = FreezeSlicer::slice(p, largest, ids, out);
  std::vector<std::int32_t> sizes;
  for (const OrderDetails& child : std::span(out).first(n)) {
    sizes.push_back(child.qty);
  }
  return sizes;
}

using Sizes = std::vector<std::int32_t>;

// What the runner does with a qty on an instrument: the children it sends, or empty if it isn't sliced.
Sizes runner_sizes(std::int32_t qty, std::int32_t lot, std::int32_t freeze) {
  SliceDesk d({.lot = lot, .freeze = freeze});
  const SliceStart out = d.start(d.parent(qty));
  if (out.kind != Kind::kStarted) {
    EXPECT_EQ(out.kind, Kind::kNotSliced);
    EXPECT_TRUE(d.sink.events.empty());
    return {};
  }
  static_cast<void>(d.runner->poll());
  static_cast<void>(d.router->poll());
  return d.broker.sent_qty;
}

// ---- child sizes (D-52) ----

TEST(FreezeSlicerSizes, AFreezeValueThatIsAnExactLotMultiple) {
  EXPECT_EQ(FreezeSlicer::largest_child(1800, 75), 1725);
  EXPECT_EQ(runner_sizes(1725, 75, 1800), Sizes{});  // not sliced
  EXPECT_EQ(runner_sizes(1800, 75, 1800), (Sizes{1725, 75}));
  EXPECT_EQ(runner_sizes(5400, 75, 1800), (Sizes{1725, 1725, 1725, 225}));
}

TEST(FreezeSlicerSizes, AFreezeValueThatIsNotALotMultiple) {
  EXPECT_EQ(FreezeSlicer::largest_child(1800, 65), 1755);
  EXPECT_EQ(runner_sizes(1755, 65, 1800), Sizes{});
  EXPECT_EQ(runner_sizes(3510, 65, 1800), (Sizes{1755, 1755}));
  EXPECT_EQ(runner_sizes(1820, 65, 1800), (Sizes{1755, 65}));
  EXPECT_EQ(FreezeSlicer::largest_child(1801, 75), 1800);
  EXPECT_EQ(runner_sizes(1800, 75, 1801), Sizes{});
  EXPECT_EQ(runner_sizes(1875, 75, 1801), (Sizes{1800, 75}));
}

TEST(FreezeSlicerSizes, FreezeOneAboveALotGivesOneLotChildren) {
  EXPECT_EQ(FreezeSlicer::largest_child(76, 75), 75);
  EXPECT_EQ(runner_sizes(225, 75, 76), (Sizes{75, 75, 75}));
}

TEST(FreezeSlicerSizes, NoWholeLotBelowTheFreezeValueRefusesTheParent) {
  for (const std::int32_t freeze : {75, 74, 1}) {
    EXPECT_EQ(FreezeSlicer::largest_child(freeze, 75), 0) << freeze;
    for (const std::int32_t qty : {150, 750}) {
      SliceDesk d({.lot = 75, .freeze = freeze});
      const auto out = d.runner->start(d.parent(qty));
      ASSERT_FALSE(out.has_value()) << freeze;
      EXPECT_EQ(out.error().code, ErrorCode::kInvalidArgument);
      EXPECT_TRUE(d.sink.events.empty());
      EXPECT_EQ(d.runner->poll(), 0U);
    }
  }
}

// One lot is never a parent, even on bad master data, so a trader can always get out (D-54).
TEST(FreezeSlicerSizes, OneLotOnBadMasterDataTakesTheNormalPathAndPassesTheGate) {
  for (const std::int32_t freeze : {75, 74, 1}) {
    for (const Side side : {Side::kBuy, Side::kSell}) {
      for (const bool reducing : {false, true}) {
        SliceDesk d({.lot = 75, .freeze = freeze});
        if (reducing) {  // a position this order closes a lot of
          d.fill(side == Side::kBuy ? Side::kSell : Side::kBuy, 225, rupees(100), kSliced);
        }
        OrderDetails o = d.parent(75);
        o.side = side;
        EXPECT_EQ(d.start(o).kind, Kind::kNotSliced) << freeze;
        EXPECT_TRUE(d.sink.events.empty());
        EXPECT_EQ(d.ids.drawn, 0U);
        const GateDecision decided = d.gate.evaluate(d.ctx, o);
        EXPECT_TRUE(decided.approved.has_value())
            << freeze << ' ' << reason_text(decided.rejection.value_or(RuleResult{}));
        o.qty = 150;
        const auto two = d.runner->start(o);
        ASSERT_FALSE(two.has_value()) << freeze;
        EXPECT_EQ(two.error().code, ErrorCode::kInvalidArgument);
        const GateDecision refused = d.gate.evaluate(d.ctx, o);
        ASSERT_TRUE(refused.rejection.has_value());
        EXPECT_EQ(reason_text(*refused.rejection), "R-FREEZE-EXCEEDED");
      }
    }
  }
}

TEST(FreezeSlicerSizes, AnUnknownInstrumentGoesTheNormalPath) {
  const SliceDesk d;
  OrderDetails unknown = d.parent(500);
  unknown.instrument_id = 999;
  EXPECT_EQ(d.start(unknown).kind, Kind::kNotSliced);
}

TEST(FreezeSlicerSizes, AnOptionIsSlicedAsAFutureIs) {
  SliceDesk d;
  InstrumentRef i = *d.instruments.find(kSliced);
  i.segment = Segment::kOpt;
  i.option_kind = OptionKind::kCall;
  ASSERT_TRUE(d.instruments.put(kSliced, i));
  d.fresh_greeks_all();
  ASSERT_TRUE(d.sb.set_greeks(kSliced, {.delta = 0, .vega = 0, .as_of = d.clock.monotonic()}));
  EXPECT_EQ(d.start(d.parent(30)).kind, Kind::kStarted);
  EXPECT_EQ(d.view().children, 3U);
}

// Equities are not sliced for now (D-51, D-52): the normal path, never a freeze refusal, whatever the value.
TEST(FreezeSlicerSizes, AnEquityIsNeverSlicedOrRefusedForItsFreezeValue) {
  // Unknown, below the order, no whole lot below it, exactly one lot, and nonsense.
  for (const std::int32_t freeze : {0, 100, 50, 75, -5}) {
    for (const bool live : {false, true}) {
      SliceDesk d;
      InstrumentRef i = *d.instruments.find(kSliced);
      i.segment = Segment::kEq;
      i.exchange = Exchange::kNse;
      i.lot_size = 75;
      i.freeze_qty = freeze;
      ASSERT_TRUE(d.instruments.put(kSliced, i));
      ASSERT_TRUE(d.margin_cache->set_per_lot(kSliced, rupees(100).units(), d.clock.monotonic()));
      OrderDetails o = d.parent(750);
      o.is_paper = live ? oms::kLiveOrder : oms::kPaperOrder;
      const auto out = d.runner->start(o);
      ASSERT_TRUE(out.has_value()) << freeze;
      EXPECT_EQ(out->kind, Kind::kNotSliced) << freeze;
      EXPECT_TRUE(d.sink.events.empty());
      EXPECT_EQ(d.ids.drawn, 0U);
      // And the normal path's gate passes it whole.
      const GateDecision decided = d.gate.evaluate(d.ctx, o);
      EXPECT_TRUE(decided.approved.has_value())
          << freeze << ' ' << reason_text(decided.rejection.value_or(RuleResult{}));
    }
  }
}

RC_GTEST_PROP(FreezeSlicerProperty, ChildrenAreWholeLotsBelowTheFreezeValueAndSumToTheParent, ()) {
  const auto lot = *rc::gen::inRange(1, 200);
  const auto freeze = *rc::gen::inRange(lot + 1, lot * 40);
  const std::int32_t largest = FreezeSlicer::largest_child(freeze, lot);
  RC_ASSERT(largest >= lot);
  RC_ASSERT(largest < freeze);
  RC_ASSERT(largest % lot == 0);
  RC_ASSERT(largest + lot >= freeze);  // no larger whole lot fits below
  const auto lots = *rc::gen::inRange(1, (largest / lot) * 50);
  const std::int32_t qty = lots * lot;
  const Sizes sizes = child_sizes(qty, lot, freeze);
  RC_ASSERT(sizes.size() == FreezeSlicer::child_count(qty, largest));
  std::int64_t sum = 0;
  for (std::size_t i = 0; i < sizes.size(); ++i) {
    RC_ASSERT(sizes[i] > 0);
    RC_ASSERT(sizes[i] < freeze);
    RC_ASSERT(sizes[i] % lot == 0);
    RC_ASSERT(i + 1 == sizes.size() || sizes[i] == largest);
    sum += sizes[i];
  }
  RC_ASSERT(sum == qty);
}

// ---- an F&O contract with no freeze quantity (D-51) ----

constexpr std::int32_t kFnoLot = 65;

void make_fno(SliceDesk& d, Segment segment, std::int32_t freeze = 0) {
  InstrumentRef i = *d.instruments.find(kSliced);
  i.lot_size = kFnoLot;
  i.freeze_qty = freeze;
  i.segment = segment;
  i.exchange = Exchange::kNfo;
  ASSERT_TRUE(d.instruments.put(kSliced, i));
}

TEST(FreezeUnknown, MoreThanOneLotIsRefusedAndOneLotPasses) {
  for (const Segment segment : {Segment::kFut, Segment::kOpt}) {
    for (const bool live : {false, true}) {
      for (const Side side : {Side::kBuy, Side::kSell}) {
        SliceDesk d;
        make_fno(d, segment);
        OrderDetails o = d.parent(kFnoLot);
        o.is_paper = live ? oms::kLiveOrder : oms::kPaperOrder;
        o.side = side;
        EXPECT_EQ(d.check(RuleCode::kOrderShape, o).verdict, Verdict::kPass);
        for (const std::int32_t lots : {2, 3, 50}) {
          o.qty = lots * kFnoLot;
          const Outcome out = d.check(RuleCode::kOrderShape, o);
          EXPECT_EQ(out.verdict, Verdict::kReject);
          EXPECT_EQ(out.reason, RuleReason::kFreezeUnknown);
          EXPECT_EQ(out.limit, kFnoLot);
          EXPECT_EQ(out.current, o.qty);
          const RuleResult result{
              .code = RuleCode::kOrderShape, .verdict = out.verdict, .reason = out.reason};
          EXPECT_EQ(reason_text(result), "R-FREEZE-UNKNOWN");
        }
      }
    }
  }
}

TEST(FreezeUnknown, ANegativeFreezeValueIsUnknownToo) {
  SliceDesk d;
  make_fno(d, Segment::kFut, -5);
  EXPECT_EQ(d.check(RuleCode::kOrderShape, d.parent(kFnoLot)).verdict, Verdict::kPass);
  const Outcome two = d.check(RuleCode::kOrderShape, d.parent(2 * kFnoLot));
  EXPECT_EQ(two.reason, RuleReason::kFreezeUnknown);
  EXPECT_EQ(two.limit, kFnoLot);
  const SliceStart out = d.start(d.parent(2 * kFnoLot));
  EXPECT_EQ(out.kind, Kind::kRefused);
  EXPECT_EQ(reason_text(out.check.rejection.value_or(RuleResult{})), "R-FREEZE-UNKNOWN");
  EXPECT_EQ(d.start(d.parent(kFnoLot)).kind, Kind::kNotSliced);
  EXPECT_TRUE(d.sink.events.empty());
}

TEST(FreezeUnknown, AReducingCloseGoesOneLotAtATime) {
  for (const bool live : {false, true}) {
    SliceDesk d;
    make_fno(d, Segment::kFut);
    d.mark(kSliced, rupees(100));
    d.fill(Side::kBuy, std::int64_t{3} * kFnoLot, rupees(100), kSliced);
    OrderDetails close = d.parent(2 * kFnoLot);
    close.is_paper = live ? oms::kLiveOrder : oms::kPaperOrder;
    close.side = Side::kSell;
    const GateDecision two = d.gate.evaluate(d.ctx, close);
    ASSERT_TRUE(two.rejection.has_value());
    EXPECT_EQ(two.rejection->code, RuleCode::kOrderShape);
    EXPECT_EQ(reason_text(*two.rejection), "R-FREEZE-UNKNOWN");
    close.qty = kFnoLot;
    const GateDecision one = d.gate.evaluate(d.ctx, close);
    EXPECT_TRUE(one.approved.has_value()) << reason_text(one.rejection.value_or(RuleResult{}));
  }
}

TEST(FreezeUnknown, AKnownFreezeValueOrAnEquityIsNotAffected) {
  SliceDesk known;
  make_fno(known, Segment::kFut, 1'800);
  EXPECT_EQ(known.check(RuleCode::kOrderShape, known.parent(2 * kFnoLot)).verdict, Verdict::kPass);
  SliceDesk equity;
  make_fno(equity, Segment::kEq);
  EXPECT_EQ(equity.check(RuleCode::kOrderShape, equity.parent(50 * kFnoLot)).verdict, Verdict::kPass);
  EXPECT_EQ(equity.start(equity.parent(50 * kFnoLot)).kind, Kind::kNotSliced);
}

TEST(FreezeUnknown, TheRunnerRefusesTheParentByItsCheckAndSendsNothing) {
  for (const Segment segment : {Segment::kFut, Segment::kOpt}) {
    SliceDesk d;
    make_fno(d, segment);
    const auto before = d.used_state();
    const SliceStart out = d.start(d.parent(2 * kFnoLot));
    EXPECT_EQ(out.kind, Kind::kRefused);
    ASSERT_TRUE(out.check.rejection.has_value());
    EXPECT_EQ(out.check.rejection->code, RuleCode::kOrderShape);
    EXPECT_EQ(reason_text(*out.check.rejection), "R-FREEZE-UNKNOWN");
    EXPECT_EQ(d.runner->poll(), 0U);
    EXPECT_TRUE(d.sink.events.empty());
    EXPECT_EQ(d.used_state(), before);
    EXPECT_FALSE(d.runner->view(id_of(40)).has_value());
    EXPECT_EQ(d.start(d.parent(kFnoLot)).kind, Kind::kNotSliced);  // one lot: the normal path
  }
}

// ---- the gate refuses a single order above the largest child (D-54) ----

// What R-ORDER-SHAPE says about `qty` on a future with this lot and freeze value, for every kind of order.
void expect_most(std::int32_t lot, std::int32_t freeze, std::int32_t most) {
  const SliceDesk d({.lot = lot, .freeze = freeze});
  for (const bool live : {false, true}) {
    for (const Side side : {Side::kBuy, Side::kSell}) {
      for (const bool opening : {true, false}) {
        OrderDetails o = d.parent(most);
        o.is_paper = live ? oms::kLiveOrder : oms::kPaperOrder;
        o.side = side;
        EXPECT_EQ(d.check_at(RuleCode::kOrderShape, o, d.clock.monotonic(), opening).verdict, Verdict::kPass)
            << freeze << '/' << lot;
        o.qty = most + lot;
        const Outcome over = d.check_at(RuleCode::kOrderShape, o, d.clock.monotonic(), opening);
        EXPECT_EQ(over.verdict, Verdict::kReject) << freeze << '/' << lot;
        EXPECT_EQ(over.reason, RuleReason::kFreezeExceeded);
        EXPECT_EQ(over.limit, most);
        EXPECT_EQ(over.current, most + lot);
      }
    }
  }
}

TEST(FreezeExceeded, TheLimitIsTheLargestChildAndNeverBelowOneLot) {
  expect_most(75, 1800, 1725);  // an exact multiple
  expect_most(65, 1800, 1755);  // not a multiple
  expect_most(75, 1801, 1800);
  expect_most(75, 76, 75);
  expect_most(75, 75, 75);  // bad master data: one lot still goes
  expect_most(75, 74, 75);
  expect_most(75, 1, 75);
  const RuleResult result{.code = RuleCode::kOrderShape, .reason = RuleReason::kFreezeExceeded};
  EXPECT_EQ(reason_text(result), "R-FREEZE-EXCEEDED");
}

TEST(FreezeExceeded, AReducingCloseIsRefusedTooAndOneWithinTheLimitGoes) {
  SliceDesk d({.lot = 65, .freeze = 1800});
  d.fill(Side::kBuy, 5'200, rupees(100), kSliced);
  OrderDetails close = d.parent(1'820);
  close.side = Side::kSell;
  const GateDecision over = d.gate.evaluate(d.ctx, close);
  ASSERT_TRUE(over.rejection.has_value());
  EXPECT_EQ(reason_text(*over.rejection), "R-FREEZE-EXCEEDED");
  close.qty = 1'755;
  const GateDecision within = d.gate.evaluate(d.ctx, close);
  EXPECT_TRUE(within.approved.has_value()) << reason_text(within.rejection.value_or(RuleResult{}));
}

TEST(FreezeExceeded, TheParentCheckPassesWhereEvaluateRefuses) {
  SliceDesk d;  // largest child 10
  const OrderDetails o = d.parent(50);
  EXPECT_TRUE(d.gate.check(d.ctx, o).passed());
  const GateDecision decided = d.gate.evaluate(d.ctx, o);
  ASSERT_TRUE(decided.rejection.has_value());
  EXPECT_EQ(decided.rejection->code, RuleCode::kOrderShape);
  EXPECT_EQ(reason_text(*decided.rejection), "R-FREEZE-EXCEEDED");
  // The exemption is that reason alone: the rest of R-ORDER-SHAPE still applies to a parent.
  EXPECT_FALSE(d.gate.check(d.ctx, d.at_price(o, 0)).passed());
}

// A master reload that lowers the freeze value under a held parent: its next child is refused.
TEST(FreezeExceeded, ALoweredFreezeValueStopsAHeldParent) {
  SliceDesk d;
  ASSERT_EQ(d.start(d.parent(30)).kind, Kind::kStarted);
  d.instrument(1, 6);
  EXPECT_EQ(d.runner->poll(), 0U);
  EXPECT_EQ(d.view().stopped, SliceStop::kRefused);
  EXPECT_EQ(d.last_reason("P1-001"), "R-FREEZE-EXCEEDED");
}

RC_GTEST_PROP(FreezeExceededProperty, TheGateRefusesExactlyTheOrdersAboveTheLargestChildOrOneLot, ()) {
  const auto lot = *rc::gen::inRange(1, 100);
  const auto freeze = *rc::gen::inRange(1, 4'000);
  const auto lots = *rc::gen::inRange(1, 200);
  const SliceDesk d({.lot = lot, .freeze = freeze});
  const std::int32_t qty = lot * lots;
  const bool over = qty > std::max(FreezeSlicer::largest_child(freeze, lot), lot);
  const Outcome out = d.check(RuleCode::kOrderShape, d.parent(qty));
  RC_ASSERT((out.verdict == Verdict::kReject) == over);
}

// ---- what a child is ----

TEST(FreezeSlicerChildren, EachHasItsOwnIdAndClientIdAndTheLinkAndKeepsTheBracketParent) {
  OrderDetails p{.is_paper = oms::kPaperOrder};
  p.id = id_of(40);
  p.parent_id = id_of(41);  // a bracket's, to be left alone (D-48)
  ASSERT_TRUE(p.client_order_id.assign("abc"));
  p.qty = 25;
  SeqIds ids;
  std::vector<OrderDetails> out(3, p);
  ASSERT_EQ(FreezeSlicer::slice(p, 10, ids, out), 3U);
  const std::array<std::string_view, 3> cids{"abc-001", "abc-002", "abc-003"};
  for (std::size_t i = 0; i < out.size(); ++i) {
    EXPECT_EQ(out[i].client_order_id.view(), cids.at(i));
    EXPECT_EQ(out[i].parent_id, id_of(41));
    ASSERT_TRUE(out[i].slice_parent.has_value());
    EXPECT_EQ(out[i].slice_parent->id, id_of(40));
    EXPECT_EQ(out[i].slice_parent->qty, 25);
    EXPECT_EQ(out[i].slice_parent->client_order_id.view(), "abc");
    EXPECT_NE(out[i].id, p.id);
    EXPECT_EQ(out[i].slice, out[0].slice);
    EXPECT_TRUE(out[i].slice.sliced());
  }
  EXPECT_NE(out[0].id, out[1].id);
  EXPECT_NE(out[1].id, out[2].id);
}

TEST(FreezeSlicerChildren, AParentWithNoRoomForTheSuffixOrAlreadyASliceIsRefused) {
  OrderDetails p{.is_paper = oms::kPaperOrder};
  p.qty = 25;
  SeqIds ids;
  std::vector<OrderDetails> out(3, p);
  EXPECT_EQ(FreezeSlicer::slice(p, 10, ids, out), 0U);  // no client_order_id
  ASSERT_TRUE(p.client_order_id.assign(std::string(60, 'x')));
  ASSERT_EQ(FreezeSlicer::slice(p, 10, ids, out), 3U);
  EXPECT_EQ(out[2].client_order_id.view(), std::string(60, 'x') + "-003");
  const OrderDetails child = out[0];
  EXPECT_EQ(FreezeSlicer::slice(child, 1, ids, out), 0U);  // a slice isn't sliced again
  ASSERT_TRUE(p.client_order_id.assign(std::string(61, 'x')));
  const std::uint32_t drawn = ids.drawn;
  EXPECT_EQ(FreezeSlicer::slice(p, 10, ids, out), 0U);
  EXPECT_EQ(ids.drawn, drawn);

  SliceDesk d;
  const auto long_id = d.runner->start(d.parent(50, std::string(61, 'x')));
  ASSERT_FALSE(long_id.has_value());
  EXPECT_EQ(long_id.error().code, ErrorCode::kInvalidArgument);
  const auto again = d.runner->start(child);
  ASSERT_FALSE(again.has_value());
  EXPECT_EQ(again.error().code, ErrorCode::kInvalidArgument);
}

TEST(FreezeSlicerChildren, TheParentLinkIsPartOfTheRequest) {
  SliceDesk d;
  std::vector<OrderDetails> cut(1, d.parent(10));
  ASSERT_EQ(FreezeSlicer::slice(d.parent(10), 10, d.ids, cut), 1U);
  const OrderDetails o = cut[0];
  ASSERT_TRUE(d.oms->intake(o).has_value());
  ASSERT_TRUE(d.oms->intake(o)->existing);
  OrderDetails other = o;
  other.slice_parent->qty = 31;
  EXPECT_EQ(d.oms->intake(other).error().code, ErrorCode::kOrderDup);
  other = o;
  other.slice_parent->id = id_of(9);
  EXPECT_EQ(d.oms->intake(other).error().code, ErrorCode::kOrderDup);
}

// Only the slicer sets the link and the intent, and it sets both: one without the other is refused.
TEST(FreezeSlicerChildren, IntakeRefusesALinkWithoutAnIntentOrTheReverse) {
  SliceDesk d;
  std::vector<OrderDetails> cut(1, d.parent(10));
  ASSERT_EQ(FreezeSlicer::slice(d.parent(10), 10, d.ids, cut), 1U);
  OrderDetails no_intent = cut[0];
  no_intent.slice = {};
  OrderDetails no_link = cut[0];
  no_link.slice_parent.reset();
  for (const OrderDetails& bad : {no_intent, no_link}) {
    const auto taken = d.oms->intake(bad);
    ASSERT_FALSE(taken.has_value());
    EXPECT_EQ(taken.error().code, ErrorCode::kInvalidArgument);
  }
  EXPECT_TRUE(d.sink.events.empty());
  EXPECT_TRUE(d.oms->intake(cut[0]).has_value());
  // Recovery puts a child back with its link and no intent, through restore().
  EXPECT_TRUE(d.oms->restore({.details = no_intent, .status = OrderStatus::kSent, .seq = 3}).has_value());
}

// ---- the gate's check-only evaluation (D-49) ----

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): a rule is a plain function pointer
std::uint32_t g_evaluated = 0;  // bit n: rule n ran
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::uint32_t g_committed = 0;

template <std::size_t N>
Outcome probe_eval(const GateContext& /*ctx*/, const OrderDetails& /*o*/, const GateFacts& /*f*/) noexcept {
  g_evaluated |= 1U << N;
  return {};
}
template <std::size_t N>
void probe_commit(GateContext& /*ctx*/, const OrderDetails& /*o*/, const GateFacts& /*f*/) noexcept {
  g_committed |= 1U << N;
}
template <std::size_t... I>
constexpr std::array<Rule, kRuleCount> probes(std::index_sequence<I...> /*seq*/) {
  return {Rule{.code = static_cast<RuleCode>(I + 1),
               .eval = &probe_eval<I + 1>,
               .on_approved = &probe_commit<I + 1>}...};
}

TEST(GateCheckOnly, RunsEveryRuleButOpsAndCommitsNothing) {
  const auto rules = probes(std::make_index_sequence<kRuleCount>{});
  SliceDesk d;
  const RiskGate gate = RiskGate::create(rules, d.clock).value();
  g_evaluated = 0;
  g_committed = 0;
  static_cast<void>(gate.evaluate(d.ctx, d.parent(10)));
  const std::uint32_t all = g_evaluated;
  EXPECT_EQ(g_committed, all);
  g_evaluated = 0;
  g_committed = 0;
  EXPECT_TRUE(gate.check(d.ctx, d.parent(10)).passed());
  EXPECT_EQ(g_evaluated, all & ~(1U << static_cast<unsigned>(RuleCode::kOps)));
  EXPECT_EQ(g_committed, 0U);
}

TEST(GateCheckOnly, RefusesAsEvaluateDoesForAKillAPositionAndMargin) {
  struct Case {
    RuleCode code;
    void (*arrange)(SliceDesk&);
    bool live;
  };
  const std::array<Case, 3> cases{
      {{RuleCode::kKill, [](SliceDesk& d) { EXPECT_TRUE(d.ctx.kill.block(KillSwitch::Scope::kDesk)); },
        false},
       {RuleCode::kPosition, [](SliceDesk& d) { d.relimit(LimitKey::kMaxPositionLots, 5 * kFromScale); },
        false},
       {RuleCode::kMargin,
        [](SliceDesk& d) { ASSERT_TRUE(d.margin_cache->set_available(kAccount, 1, d.clock.monotonic())); },
        true}}};
  for (const Case& c : cases) {
    SliceDesk d({.freeze = kAmpleFreeze});  // so evaluate judges the whole size too
    ASSERT_TRUE(d.margin_cache->set_per_lot(kSliced, rupees(100).units(), d.clock.monotonic()));
    c.arrange(d);
    OrderDetails o = d.parent(50);
    o.is_paper = c.live ? oms::kLiveOrder : oms::kPaperOrder;
    const GateCheck checked = d.gate.check(d.ctx, o);
    const GateDecision decided = d.gate.evaluate(d.ctx, o);
    ASSERT_TRUE(checked.rejection.has_value()) << code_text(c.code);
    ASSERT_TRUE(decided.rejection.has_value()) << code_text(c.code);
    EXPECT_EQ(checked.rejection->code, c.code);
    EXPECT_EQ(decided.rejection->code, c.code);
    EXPECT_EQ(checked.rejection->limit, decided.rejection->limit);
    EXPECT_EQ(checked.rejection->current, decided.rejection->current);
  }
}

TEST(GateCheckOnly, APassTakesNoTokenDuplicateEntryOrMargin) {
  SliceDesk d({.freeze = kAmpleFreeze});
  ASSERT_TRUE(d.margin_cache->set_per_lot(kSliced, rupees(100).units(), d.clock.monotonic()));
  OrderDetails o = d.parent(50);
  o.is_paper = oms::kLiveOrder;
  const auto before = d.used_state();
  const GateCheck checked = d.gate.check(d.ctx, o);
  ASSERT_TRUE(checked.passed()) << reason_text(checked.rejection.value_or(RuleResult{}));
  EXPECT_EQ(d.used_state(), before);
  EXPECT_TRUE(d.margin_cache->available(kAccount)->reserved == Wide{0});
  // The control: evaluate does take all three.
  ASSERT_TRUE(d.gate.evaluate(d.ctx, o).approved.has_value());
  EXPECT_NE(d.used_state().first, before.first);
  EXPECT_EQ(d.used_state().second, before.second + 1);
  EXPECT_TRUE(d.margin_cache->available(kAccount)->reserved > Wide{0});
}

TEST(GateCheckOnly, DoesNotRunOpsEvenWithNoTokenFree) {
  SliceDesk d({.freeze = kAmpleFreeze});
  d.ops_cap(0);
  const OrderDetails o = d.parent(50);
  EXPECT_TRUE(d.gate.check(d.ctx, o).passed());
  const GateDecision decided = d.gate.evaluate(d.ctx, o);
  ASSERT_TRUE(decided.rejection.has_value());
  EXPECT_EQ(decided.rejection->code, RuleCode::kOps);
}

TEST(GateCheckOnly, RefusesALiveOrderOnAnIncompleteGateAndSeesAPause) {
  SliceDesk d;
  const RiskGate partial = RiskGate::create(pre_trade_rules(), d.clock).value();
  OrderDetails o = d.parent(50);
  o.is_paper = oms::kLiveOrder;
  const GateCheck checked = partial.check(d.ctx, o);
  ASSERT_TRUE(checked.rejection.has_value());
  EXPECT_EQ(checked.rejection->code, RuleCode::kLiveDisabled);
  d.clock.step_wall(10 * kNanosPerSecond);
  EXPECT_EQ(d.ctx.stale_before, 0);
  static_cast<void>(d.gate.check(d.ctx, o));
  EXPECT_EQ(d.ctx.stale_before, d.clock.monotonic());
}

// ---- the parent check at start ----

TEST(SliceRunnerStart, ARefusedParentSendsRecordsAndTakesNothing) {
  for (const LimitKey key : {LimitKey::kMaxOrderValue, LimitKey::kMaxOrderQtyLots}) {
    SliceDesk d;
    // Each child (10) passes; the full 60 does not.
    d.relimit(key, key == LimitKey::kMaxOrderValue ? rupees(2'000).units() : 20 * kFromScale);
    const auto before = d.used_state();
    const SliceStart out = d.start(d.parent(60));
    EXPECT_EQ(out.kind, Kind::kRefused);
    ASSERT_TRUE(out.check.rejection.has_value());
    EXPECT_EQ(out.check.rejection->code, RuleCode::kOrderValue);
    EXPECT_EQ(d.runner->poll(), 0U);
    EXPECT_EQ(d.router->poll(), 0U);
    EXPECT_TRUE(d.sink.events.empty());
    EXPECT_EQ(d.oms->size(), 0U);
    EXPECT_TRUE(d.broker.sent.empty());
    EXPECT_EQ(d.used_state(), before);
    EXPECT_EQ(d.ids.drawn, 0U);
    EXPECT_FALSE(d.runner->view(id_of(40)).has_value());
  }
}

TEST(SliceRunnerStart, IsIdempotentOnTheParentsClientId) {
  SliceDesk d;
  const OrderDetails p = d.parent(30);
  EXPECT_EQ(d.start(p).kind, Kind::kStarted);
  EXPECT_EQ(d.start(p).kind, Kind::kExisting);
  EXPECT_EQ(d.runner->poll(), 3U);
  EXPECT_EQ(d.start(p).kind, Kind::kExisting);
  EXPECT_EQ(d.runner->poll(), 0U);
  EXPECT_EQ(d.oms->size(), 3U);
  EXPECT_EQ(d.ids.drawn, 3U);
  OrderDetails other = p;
  other.qty = 40;
  const auto dup = d.runner->start(other);
  ASSERT_FALSE(dup.has_value());
  EXPECT_EQ(dup.error().code, ErrorCode::kOrderDup);
  // The same parent id under another client id.
  const auto reused = d.runner->start(d.parent(30, "P2", 40));
  ASSERT_FALSE(reused.has_value());
  EXPECT_EQ(reused.error().code, ErrorCode::kOrderDup);
}

TEST(SliceRunnerStart, AClientIdTheOmsHoldsGoesTheNormalPath) {
  SliceDesk d;
  ASSERT_TRUE(d.oms->create(d.parent(5)).has_value());
  EXPECT_EQ(d.start(d.parent(30)).kind, Kind::kNotSliced);  // intake there says ORDER_DUP
}

TEST(SliceRunnerStart, TooManyChildrenOrNoRoomForAParentIsRefused) {
  SliceDesk d({.config = {.max_parents = 1, .max_children = 3}});
  const auto many = d.runner->start(d.parent(31));
  ASSERT_FALSE(many.has_value());
  EXPECT_EQ(many.error().code, ErrorCode::kCapacityExceeded);
  EXPECT_TRUE(d.sink.events.empty());

  EXPECT_EQ(d.start(d.parent(30)).kind, Kind::kStarted);
  const auto full = d.runner->start(d.parent(20, "P2", 50));
  ASSERT_FALSE(full.has_value());
  EXPECT_EQ(full.error().code, ErrorCode::kCapacityExceeded);

  // Sent but still working: the parent is kept, so it can still be cancelled.
  EXPECT_EQ(d.runner->poll(), 3U);
  EXPECT_EQ(d.router->poll(), 3U);
  EXPECT_FALSE(d.runner->start(d.parent(20, "P2", 50)).has_value());

  // Every child ended: the slot is given up, and the evicted parent can't be started a second time.
  for (OrderHandle h = 0; h < 3; ++h) {
    ASSERT_TRUE(d.oms->apply(h, OrderEvent::kBrokerAck, {.broker_order_id = "B"}).has_value());
    ASSERT_TRUE(d.oms->apply(h, OrderEvent::kBrokerCancel).has_value());
  }
  d.next_second();
  EXPECT_EQ(d.start(d.parent(20, "P2", 50)).kind, Kind::kStarted);
  EXPECT_FALSE(d.runner->view(id_of(40)).has_value());
  d.runner->drop_all();
  const auto again = d.runner->start(d.parent(30));
  ASSERT_FALSE(again.has_value());
  EXPECT_EQ(again.error().code, ErrorCode::kOrderDup);
}

TEST(SliceRunnerStart, AnUnhealthyOrderLogRefusesAndIsCounted) {
  SliceDesk d;
  d.sink.up = false;
  const auto out = d.runner->start(d.parent(30));
  ASSERT_FALSE(out.has_value());
  EXPECT_EQ(out.error().code, ErrorCode::kOrderLogUnavailable);
  EXPECT_EQ(d.sink.outage_notes, 1);
}

// ---- pacing (D-50) ----

TEST(SliceRunnerPacing, SendsAsManyAsThereAreTokensAndNeverTwice) {
  SliceDesk d;  // cap 8
  ASSERT_EQ(d.start(d.parent(200)).kind, Kind::kStarted);
  EXPECT_EQ(d.runner->poll(), 8U);
  EXPECT_EQ(d.router->poll(), 8U);
  const auto before = d.used_state();
  const std::size_t events = d.sink.events.size();
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(d.runner->poll(), 0U);  // waiting is not a refusal
  }
  EXPECT_EQ(d.used_state(), before);  // and it uses nothing up
  EXPECT_EQ(d.sink.events.size(), events);
  EXPECT_EQ(d.oms->size(), 8U);
  EXPECT_EQ(d.view().stopped, SliceStop::kNone);
  EXPECT_EQ(d.view().unsent_qty, 120);

  d.next_second();
  EXPECT_EQ(d.runner->poll(), 8U);
  EXPECT_EQ(d.router->poll(), 8U);
  d.next_second();
  EXPECT_EQ(d.runner->poll(), 4U);
  EXPECT_EQ(d.router->poll(), 4U);
  d.next_second();
  EXPECT_EQ(d.runner->poll(), 0U);

  const SliceView v = d.view();
  EXPECT_EQ(v.requested, 200);
  EXPECT_EQ(v.children, 20U);
  EXPECT_EQ(v.children_sent, 20U);
  EXPECT_EQ(v.unsent_qty, 0);
  EXPECT_EQ(v.stopped, SliceStop::kNone);
  EXPECT_EQ(d.oms->size(), 20U);
  ASSERT_EQ(d.broker.sent.size(), 20U);
  std::vector<std::string> unique = d.broker.sent;
  std::ranges::sort(unique);
  EXPECT_EQ(std::ranges::unique(unique).begin(), unique.end());
  EXPECT_EQ(d.broker.sent.front(), "P1-001");
  EXPECT_EQ(d.broker.sent.back(), "P1-020");
  EXPECT_EQ(d.oms->illegal_transitions(), 0U);
}

TEST(SliceRunnerPacing, ACapOfZeroIsARefusal) {
  SliceDesk d;
  ASSERT_EQ(d.start(d.parent(30)).kind, Kind::kStarted);
  d.ops_cap(0);
  EXPECT_EQ(d.runner->poll(), 0U);
  const SliceView v = d.view();
  EXPECT_EQ(v.stopped, SliceStop::kRefused);
  ASSERT_TRUE(v.rejection.has_value());
  EXPECT_EQ(v.rejection->code, RuleCode::kOps);
  EXPECT_EQ(d.status("P1-001"), OrderStatus::kRiskRejected);
  EXPECT_EQ(d.last_reason("P1-001"), "R-OPS");
  EXPECT_EQ(d.oms->size(), 1U);
}

TEST(SliceRunnerPacing, AFullRouterQueueWaitsWithoutGating) {
  SliceDesk d({.router_capacity = 2});
  ASSERT_EQ(d.start(d.parent(50)).kind, Kind::kStarted);
  EXPECT_EQ(d.runner->poll(), 2U);
  const auto before = d.used_state();
  EXPECT_EQ(d.runner->poll(), 0U);
  EXPECT_EQ(d.used_state(), before);
  EXPECT_EQ(d.router->poll(), 2U);
  EXPECT_EQ(d.runner->poll(), 2U);
  EXPECT_EQ(d.view().stopped, SliceStop::kNone);
}

TEST(SliceRunnerPacing, ParentsGoInStartOrderAndShareTheTokens) {
  SliceDesk d;
  ASSERT_EQ(d.start(d.parent(60)).kind, Kind::kStarted);
  ASSERT_EQ(d.start(d.at_price(d.parent(61, "P2", 50), 101)).kind, Kind::kStarted);
  EXPECT_EQ(d.runner->poll(), 8U);
  EXPECT_EQ(d.view(40).children_sent, 6U);
  EXPECT_EQ(d.view(50).children_sent, 2U);
}

// ---- refusals and stops (D-45) ----

TEST(SliceRunnerRefusal, ChildFourOfSixStopsTheRestAndIsRecorded) {
  SliceDesk d;
  d.relimit(LimitKey::kMaxPositionLots, 100 * kFromScale);
  d.ops_cap(1);  // one child a second, so the book can be fed between children, as M2.6 will
  ASSERT_EQ(d.start(d.parent(60)).kind, Kind::kStarted);
  for (int i = 0; i < 3; ++i) {
    ASSERT_EQ(d.runner->poll(), 1U) << i;
    ASSERT_EQ(d.router->poll(), 1U);
    ASSERT_TRUE(d.sb.add_working(d.who(), kSliced, Side::kBuy, 10));
    d.next_second();
  }
  ASSERT_TRUE(d.sb.add_working(d.who(), kSliced, Side::kBuy, 65));  // someone else's order
  EXPECT_EQ(d.runner->poll(), 0U);
  d.next_second();
  EXPECT_EQ(d.runner->poll(), 0U);
  EXPECT_EQ(d.router->poll(), 0U);

  const SliceView v = d.view();
  EXPECT_EQ(v.stopped, SliceStop::kRefused);
  ASSERT_TRUE(v.rejection.has_value());
  EXPECT_EQ(v.rejection->code, RuleCode::kPosition);
  EXPECT_EQ(v.children_sent, 3U);
  EXPECT_EQ(v.unsent_qty, 30);
  for (const std::string_view cid : {"P1-001", "P1-002", "P1-003"}) {
    EXPECT_EQ(d.status(cid), OrderStatus::kSent);
  }
  EXPECT_EQ(d.status("P1-004"), OrderStatus::kRiskRejected);
  EXPECT_EQ(d.last_reason("P1-004"), "R-POSITION");
  EXPECT_TRUE(d.sink.events.back().gate.has_value());
  EXPECT_FALSE(d.child("P1-005").has_value());
  EXPECT_EQ(d.oms->size(), 4U);
  EXPECT_TRUE(d.broker.cancelled.empty());
  EXPECT_EQ(d.oms->illegal_transitions(), 0U);
}

TEST(SliceRunnerRefusal, AKillPartWayStopsTheRest) {
  SliceDesk d;
  ASSERT_EQ(d.start(d.parent(200)).kind, Kind::kStarted);
  ASSERT_EQ(d.runner->poll(), 8U);
  ASSERT_TRUE(d.ctx.kill.block(KillSwitch::Scope::kDesk));
  EXPECT_EQ(d.runner->poll(), 0U);  // refused even though no token is free
  EXPECT_EQ(d.view().stopped, SliceStop::kRefused);
  EXPECT_EQ(d.view().rejection->code, RuleCode::kKill);
  EXPECT_EQ(d.last_reason("P1-009"), "R-KILL");
  d.ctx.kill.release(KillSwitch::Scope::kDesk);
  d.next_second();
  EXPECT_EQ(d.runner->poll(), 0U);
  EXPECT_EQ(d.oms->size(), 9U);
}

TEST(SliceRunnerRefusal, TheRecordWaitsForAFullSinkAndTheChildIsNeverGatedAgain) {
  SliceDesk d;
  ASSERT_EQ(d.start(d.parent(30)).kind, Kind::kStarted);
  ASSERT_TRUE(d.ctx.kill.block(KillSwitch::Scope::kDesk));
  d.sink.room = 0;
  EXPECT_EQ(d.runner->poll(), 0U);
  EXPECT_EQ(d.view().stopped, SliceStop::kRefused);
  d.ctx.kill.release(KillSwitch::Scope::kDesk);  // it would pass now, and must not get the chance
  d.sink.room = 1;                               // NEW goes in, the rejection doesn't
  EXPECT_EQ(d.runner->poll(), 0U);
  EXPECT_EQ(d.status("P1-001"), OrderStatus::kNew);
  d.sink.room = 1'000;
  EXPECT_EQ(d.runner->poll(), 0U);
  EXPECT_EQ(d.router->poll(), 0U);
  EXPECT_EQ(d.status("P1-001"), OrderStatus::kRiskRejected);
  EXPECT_EQ(d.last_reason("P1-001"), "R-KILL");
  EXPECT_EQ(d.oms->size(), 1U);
  EXPECT_TRUE(d.broker.sent.empty());
  EXPECT_EQ(d.runner->unrecorded(), 0U);
}

TEST(SliceRunnerStops, AnUnhealthyOrderLogPartWayStopsTheRest) {
  SliceDesk d;
  ASSERT_EQ(d.start(d.parent(200)).kind, Kind::kStarted);
  ASSERT_EQ(d.runner->poll(), 8U);
  d.sink.up = false;
  d.next_second();
  const auto before = d.used_state();
  EXPECT_EQ(d.runner->poll(), 0U);
  EXPECT_EQ(d.view().stopped, SliceStop::kOrderLog);
  EXPECT_EQ(d.used_state(), before);
  EXPECT_EQ(d.sink.outage_notes, 1);
  d.sink.up = true;
  d.next_second();
  EXPECT_EQ(d.runner->poll(), 0U);
  EXPECT_EQ(d.oms->size(), 8U);
}

TEST(SliceRunnerStops, AFullOrderPoolStopsTheRest) {
  SliceDesk d({.oms_capacity = 3});
  ASSERT_EQ(d.start(d.parent(50)).kind, Kind::kStarted);
  EXPECT_EQ(d.runner->poll(), 3U);
  EXPECT_EQ(d.view().stopped, SliceStop::kPoolFull);
  EXPECT_EQ(d.view().unsent_qty, 20);
}

TEST(SliceRunnerStops, SomeoneElsesOrderUnderAChildsIdStopsTheRest) {
  SliceDesk d;
  ASSERT_EQ(d.start(d.parent(30)).kind, Kind::kStarted);
  const OrderDetails squatter = d.parent(5, "P1-002", 60);
  ASSERT_TRUE(d.oms->create(squatter).has_value());
  EXPECT_EQ(d.runner->poll(), 1U);
  EXPECT_EQ(d.view().stopped, SliceStop::kDuplicate);
  EXPECT_EQ(d.status("P1-002"), OrderStatus::kNew);  // not ours to touch
  EXPECT_EQ(d.router->poll(), 1U);
}

TEST(SliceRunnerStops, AChildTheOrderLogRefusesStopsTheRest) {
  SliceDesk d;
  d.ops_cap(2);
  ASSERT_EQ(d.start(d.parent(50)).kind, Kind::kStarted);
  ASSERT_EQ(d.runner->poll(), 2U);
  d.sink.refused.push_back(d.oms->order(*d.child("P1-001")).details.id);
  EXPECT_EQ(d.router->poll(), 1U);  // the refused one never goes out
  d.next_second();
  EXPECT_EQ(d.runner->poll(), 0U);
  EXPECT_EQ(d.view().stopped, SliceStop::kStorageRefused);
  EXPECT_EQ(d.oms->size(), 2U);
}

TEST(SliceRunnerStops, ATakenChildTheOrderLogRefusesIsNeverSubmitted) {
  SliceDesk d;
  ASSERT_EQ(d.start(d.parent(30)).kind, Kind::kStarted);
  d.sink.room = 1;  // NEW goes in, APPROVED doesn't
  EXPECT_EQ(d.runner->poll(), 0U);
  d.sink.refused.push_back(d.oms->order(*d.child("P1-001")).details.id);
  EXPECT_EQ(d.router->poll(), 0U);
  d.sink.room = 1'000;
  const auto before = d.used_state();
  EXPECT_EQ(d.runner->poll(), 0U);
  EXPECT_EQ(d.view().stopped, SliceStop::kStorageRefused);
  EXPECT_EQ(d.used_state(), before);  // not gated again
  EXPECT_EQ(d.router->pending(), 0U);
  EXPECT_EQ(d.oms->size(), 1U);
}

// The clock steps while the first child is being taken in: the second is never gated.
TEST(SliceRunnerStops, APauseInsideAPollStopsBeforeTheNextChild) {
  class SteppingSink final : public oms::IOrderEventSink {
   public:
    explicit SteppingSink(SimClock& clock) : clock_(&clock) {}
    // NOLINTNEXTLINE(bugprone-exception-escape): a test's allocation failure may terminate
    [[nodiscard]] bool try_push(const OrderEventRecord& event) noexcept override {
      events_.push_back(event);
      if (event.to == OrderStatus::kApproved) {
        clock_->step_wall(10 * kNanosPerSecond);
      }
      return true;
    }
    [[nodiscard]] std::uint64_t pushed() const noexcept override { return events_.size(); }
    [[nodiscard]] std::uint64_t durable() const noexcept override { return events_.size(); }

   private:
    SimClock* clock_;
    std::vector<OrderEventRecord> events_;
  };
  SliceDesk d;
  SteppingSink sink(d.clock);
  Oms oms(sink, d.clock, 16);
  OrderRouter router(oms, sink, d.broker, oms::Venue::kPaper, {.tag_limit = 20, .capacity = 16}, d.limits);
  SliceRunner runner(d.gate, d.ctx, oms, router, sink, d.ids);
  ASSERT_TRUE(runner.start(d.parent(30)).has_value());
  EXPECT_EQ(runner.poll(), 1U);
  EXPECT_EQ(runner.view(id_of(40))->stopped, SliceStop::kPaused);
  EXPECT_EQ(oms.size(), 1U);
}

TEST(SliceRunnerStops, AStaleApprovalStopsTheRest) {
  SliceDesk d;
  ASSERT_EQ(d.start(d.parent(30)).kind, Kind::kStarted);
  d.sink.slow_clock = &d.clock;
  d.sink.slow_by = 200 * kMillis;  // the NEW event takes longer than an approval lives
  EXPECT_EQ(d.runner->poll(), 0U);
  EXPECT_EQ(d.view().stopped, SliceStop::kStaleApproval);
  EXPECT_EQ(d.status("P1-001"), OrderStatus::kRiskRejected);
  EXPECT_EQ(d.last_reason("P1-001"), "R-APPROVAL-STALE");
  EXPECT_EQ(d.router->poll(), 0U);
  EXPECT_EQ(d.oms->size(), 1U);
}

TEST(SliceRunnerStops, APausePartWayDropsTheRemainder) {  // D-36
  SliceDesk d;
  ASSERT_EQ(d.start(d.parent(200)).kind, Kind::kStarted);
  ASSERT_EQ(d.runner->poll(), 8U);
  d.clock.step_wall(10 * kNanosPerSecond);
  EXPECT_EQ(d.runner->poll(), 0U);
  EXPECT_EQ(d.view().stopped, SliceStop::kPaused);
  d.next_second();
  EXPECT_EQ(d.runner->poll(), 0U);
  EXPECT_EQ(d.oms->size(), 8U);
}

TEST(SliceRunnerStops, APauseBeforeTheFirstPollOrSeenAtStartDropsTheParent) {
  SliceDesk d;
  ASSERT_EQ(d.start(d.parent(30)).kind, Kind::kStarted);
  d.clock.step_wall(10 * kNanosPerSecond);
  EXPECT_EQ(d.runner->poll(), 0U);
  EXPECT_EQ(d.view().stopped, SliceStop::kPaused);
  EXPECT_EQ(d.oms->size(), 0U);

  d.next_second();
  // The gate hasn't looked since the step: this check sees it, and wants a price newer than the pause.
  EXPECT_EQ(d.start(d.parent(30, "P2", 50)).kind, Kind::kRefused);
  d.next_second();
  ASSERT_EQ(d.start(d.parent(30, "P2", 50)).kind, Kind::kStarted);
  d.clock.step_wall(10 * kNanosPerSecond);
  // The next start sees it, and the gate's check then wants a price newer than the pause.
  const SliceStart stale = d.start(d.parent(30, "P3", 60));
  EXPECT_EQ(stale.kind, Kind::kRefused);
  EXPECT_EQ(d.view(50).stopped, SliceStop::kPaused);
  d.next_second();
  ASSERT_EQ(d.start(d.parent(30, "P3", 60)).kind, Kind::kStarted);
  EXPECT_EQ(d.view(60).stopped, SliceStop::kNone);
}

// ---- a child that fails after the gate (D-53) ----

// A parent of five children going one a second, the first of them already at the broker.
void one_at_the_broker(SliceDesk& d) {
  d.ops_cap(1);
  ASSERT_EQ(d.start(d.parent(50)).kind, Kind::kStarted);
  ASSERT_EQ(d.runner->poll(), 1U);
  ASSERT_EQ(d.router->poll(), 1U);
}

// Seconds pass and both are polled: nothing more may go.
void expect_nothing_more(SliceDesk& d, SliceStop why) {
  for (int second = 0; second < 6; ++second) {
    d.next_second();
    EXPECT_EQ(d.runner->poll(), 0U);
    static_cast<void>(d.router->poll());
  }
  EXPECT_EQ(d.view().stopped, why);
  EXPECT_EQ(d.view().children_sent, 1U);
  EXPECT_EQ(d.view().unsent_qty, 40);
  EXPECT_EQ(d.oms->size(), 1U);
  EXPECT_TRUE(d.broker.cancelled.empty());
  EXPECT_EQ(d.oms->illegal_transitions(), 0U);
}

TEST(SliceRunnerChildFailure, ABrokerRejectionOfTheFirstChildMeansNoLaterChildGoes) {
  SliceDesk d;
  one_at_the_broker(d);
  ASSERT_TRUE(d.oms->apply(0, OrderEvent::kBrokerReject, {.reason_code = "RMS"}).has_value());
  expect_nothing_more(d, SliceStop::kChildBrokerRejected);
  EXPECT_EQ(d.broker.sent, (std::vector<std::string>{"P1-001"}));
}

TEST(SliceRunnerChildFailure, AnExpiryInTheSendQueueStopsTheRest) {
  SliceDesk d;
  d.ops_cap(1);
  ASSERT_EQ(d.start(d.parent(50)).kind, Kind::kStarted);
  d.sink.durable_up_to = 0;  // the writer is behind
  ASSERT_EQ(d.runner->poll(), 1U);
  EXPECT_EQ(d.router->poll(), 0U);
  d.clock.advance(6 * kNanosPerSecond);  // past the queue age limit
  d.tick(kSliced, rupees(100), d.clock.monotonic());
  d.restamp_marks();
  d.sink.durable_up_to.reset();
  EXPECT_EQ(d.router->poll(), 0U);
  ASSERT_EQ(d.status("P1-001"), OrderStatus::kExpired);
  expect_nothing_more(d, SliceStop::kChildExpired);
  EXPECT_TRUE(d.broker.sent.empty());
}

// A pause drops the remainder itself (D-36), before the expired child is looked at.
TEST(SliceRunnerChildFailure, AnExpiryAfterAPauseStopsTheRest) {
  SliceDesk d;
  d.ops_cap(1);
  ASSERT_EQ(d.start(d.parent(50)).kind, Kind::kStarted);
  ASSERT_EQ(d.runner->poll(), 1U);
  d.clock.step_wall(10 * kNanosPerSecond);
  EXPECT_EQ(d.router->poll(), 0U);
  ASSERT_EQ(d.status("P1-001"), OrderStatus::kExpired);
  expect_nothing_more(d, SliceStop::kPaused);
}

TEST(SliceRunnerChildFailure, AnUnknownChildStopsTheRestEvenIfItTurnsOutOpen) {
  SliceDesk d;
  one_at_the_broker(d);
  ASSERT_TRUE(d.oms->apply(0, OrderEvent::kAckTimeout).has_value());
  d.next_second();
  EXPECT_EQ(d.runner->poll(), 0U);
  EXPECT_EQ(d.view().stopped, SliceStop::kChildUnknown);
  ASSERT_TRUE(d.oms->apply(0, OrderEvent::kReconcileOpen).has_value());
  expect_nothing_more(d, SliceStop::kChildUnknown);
}

// Reconciled between two polls: the runner never saw UNKNOWN, and still stops.
TEST(SliceRunnerChildFailure, AnUnknownChildResolvedBeforeTheNextPollStillStopsTheRest) {
  SliceDesk d;
  one_at_the_broker(d);
  ASSERT_TRUE(d.oms->apply(0, OrderEvent::kAckTimeout).has_value());
  ASSERT_TRUE(d.oms->apply(0, OrderEvent::kReconcileOpen).has_value());
  expect_nothing_more(d, SliceStop::kChildUnknown);
}

TEST(SliceRunnerChildFailure, PartlyFilledFilledAndCancelledChildrenDoNotStopTheRest) {
  SliceDesk d;
  one_at_the_broker(d);
  ASSERT_TRUE(d.oms->apply(0, OrderEvent::kBrokerAck, {.broker_order_id = "B0"}).has_value());
  const oms::EventDetails some{.fill_qty = 4, .fill_price = rupees(100)};
  ASSERT_TRUE(d.oms->apply(0, OrderEvent::kPartialFill, some).has_value());
  d.next_second();
  ASSERT_EQ(d.runner->poll(), 1U);  // with the first PARTIALLY_FILLED
  ASSERT_EQ(d.router->poll(), 1U);
  ASSERT_TRUE(d.oms->apply(1, OrderEvent::kBrokerAck, {.broker_order_id = "B1"}).has_value());
  ASSERT_TRUE(d.oms->apply(1, OrderEvent::kFill, {.fill_qty = 10, .fill_price = rupees(100)}).has_value());
  d.next_second();
  ASSERT_EQ(d.runner->poll(), 1U);  // FILLED
  ASSERT_EQ(d.router->poll(), 1U);
  ASSERT_TRUE(d.oms->apply(2, OrderEvent::kBrokerCancel, {.reason_code = "IOC_REMAINDER"}).has_value());
  d.next_second();
  EXPECT_EQ(d.runner->poll(), 1U);  // CANCELLED
  EXPECT_EQ(d.view().stopped, SliceStop::kNone);
}

// A taken child waiting on a full sink is ended when an earlier one fails, and never submitted.
TEST(SliceRunnerChildFailure, ATakenChildIsEndedNotSubmitted) {
  SliceDesk d;
  one_at_the_broker(d);
  d.next_second();
  d.sink.room = 1;  // NEW goes in, APPROVED doesn't
  ASSERT_EQ(d.runner->poll(), 0U);
  d.sink.room = 1'000;
  ASSERT_TRUE(d.oms->apply(0, OrderEvent::kBrokerReject, {.reason_code = "RMS"}).has_value());
  d.next_second();
  EXPECT_EQ(d.runner->poll(), 0U);
  EXPECT_EQ(d.view().stopped, SliceStop::kChildBrokerRejected);
  EXPECT_EQ(d.status("P1-002"), OrderStatus::kRiskRejected);
  EXPECT_EQ(d.last_reason("P1-002"), "SLICE_DROPPED");
  EXPECT_EQ(d.router->poll(), 0U);
}

// ---- cancel, drop_all and the retries ----

TEST(SliceRunnerCancel, DropsTheRemainderAndCancelsEveryWorkingChild) {
  SliceDesk d;
  ASSERT_EQ(d.start(d.parent(200)).kind, Kind::kStarted);
  ASSERT_EQ(d.runner->poll(), 8U);
  ASSERT_EQ(d.router->poll(), 8U);
  for (OrderHandle h = 0; h < 8; ++h) {
    ASSERT_TRUE(d.oms->apply(h, OrderEvent::kBrokerAck, {.broker_order_id = "B"}).has_value());
  }
  d.next_second();
  EXPECT_FALSE(d.runner->cancel(id_of(99)));
  EXPECT_TRUE(d.runner->cancel(id_of(40)));
  EXPECT_EQ(d.broker.cancelled.size(), 8U);
  EXPECT_EQ(d.view().stopped, SliceStop::kCancelled);
  EXPECT_EQ(d.view().unsent_qty, 120);
  EXPECT_EQ(d.runner->poll(), 0U);
  EXPECT_EQ(d.oms->size(), 8U);
  EXPECT_EQ(d.oms->illegal_transitions(), 0U);
}

TEST(SliceRunnerCancel, ChildrenStillQueuedNeverGoOut) {
  SliceDesk d;
  ASSERT_EQ(d.start(d.parent(30)).kind, Kind::kStarted);
  ASSERT_EQ(d.runner->poll(), 3U);
  EXPECT_TRUE(d.runner->cancel(id_of(40)));
  EXPECT_EQ(d.router->poll(), 0U);
  for (const std::string_view cid : {"P1-001", "P1-002", "P1-003"}) {
    EXPECT_EQ(d.status(cid), OrderStatus::kExpired);
  }
}

TEST(SliceRunnerCancel, DropAllStopsEveryParentAndCancelsNothing) {
  SliceDesk d;
  d.ops_cap(1);
  ASSERT_EQ(d.start(d.parent(30)).kind, Kind::kStarted);
  ASSERT_EQ(d.start(d.at_price(d.parent(31, "P2", 50), 101)).kind, Kind::kStarted);
  ASSERT_EQ(d.runner->poll(), 1U);
  d.runner->drop_all();
  d.next_second();
  EXPECT_EQ(d.runner->poll(), 0U);
  EXPECT_EQ(d.view(40).stopped, SliceStop::kDropped);
  EXPECT_EQ(d.view(50).stopped, SliceStop::kDropped);
  EXPECT_EQ(d.oms->size(), 1U);
  EXPECT_TRUE(d.broker.cancelled.empty());
}

TEST(SliceRunnerRetry, AFullSinkAtIntakeIsRetriedAndSentOnce) {
  SliceDesk d;
  ASSERT_EQ(d.start(d.parent(30)).kind, Kind::kStarted);
  d.sink.room = 0;
  EXPECT_EQ(d.runner->poll(), 0U);
  EXPECT_EQ(d.view().stopped, SliceStop::kNone);
  EXPECT_EQ(d.oms->size(), 0U);
  d.sink.room = 1'000;
  EXPECT_EQ(d.runner->poll(), 3U);
  EXPECT_EQ(d.router->poll(), 3U);
  EXPECT_EQ(d.oms->size(), 3U);
  EXPECT_EQ(d.broker.sent, (std::vector<std::string>{"P1-001", "P1-002", "P1-003"}));
}

TEST(SliceRunnerRetry, AFullSinkAtSubmitKeepsTheTakenChildAndSendsItOnce) {
  SliceDesk d;
  ASSERT_EQ(d.start(d.parent(30)).kind, Kind::kStarted);
  d.sink.room = 1;  // NEW goes in, APPROVED doesn't
  EXPECT_EQ(d.runner->poll(), 0U);
  EXPECT_EQ(d.status("P1-001"), OrderStatus::kNew);
  EXPECT_EQ(d.view().children_sent, 0U);
  EXPECT_EQ(d.router->pending(), 0U);
  d.sink.room = 1'000;
  EXPECT_EQ(d.runner->poll(), 3U);
  EXPECT_EQ(d.router->poll(), 3U);
  EXPECT_EQ(d.oms->size(), 3U);
  EXPECT_EQ(d.broker.sent, (std::vector<std::string>{"P1-001", "P1-002", "P1-003"}));
  EXPECT_EQ(d.oms->illegal_transitions(), 0U);
}

TEST(SliceRunnerRetry, DroppingATakenChildEndsItsNewOrder) {
  SliceDesk d;
  ASSERT_EQ(d.start(d.parent(30)).kind, Kind::kStarted);
  d.sink.room = 1;
  EXPECT_EQ(d.runner->poll(), 0U);
  d.runner->drop_all();  // the sink is still full
  EXPECT_EQ(d.status("P1-001"), OrderStatus::kNew);
  d.sink.room = 1'000;
  EXPECT_EQ(d.runner->poll(), 0U);
  EXPECT_EQ(d.status("P1-001"), OrderStatus::kRiskRejected);
  EXPECT_EQ(d.last_reason("P1-001"), "SLICE_DROPPED");
  EXPECT_EQ(d.router->poll(), 0U);
}

// ---- R-DUP (D-24) ----

TEST(SliceRunnerDup, SiblingsPassAndAnIdenticalParentIsRefusedAtItsFirstChild) {
  SliceDesk d;
  ASSERT_EQ(d.start(d.parent(30)).kind, Kind::kStarted);
  ASSERT_EQ(d.runner->poll(), 3U);
  EXPECT_EQ(d.view().stopped, SliceStop::kNone);
  ASSERT_EQ(d.start(d.parent(30, "P2", 50)).kind, Kind::kStarted);
  EXPECT_EQ(d.runner->poll(), 0U);
  EXPECT_EQ(d.view(50).stopped, SliceStop::kRefused);
  EXPECT_EQ(d.view(50).rejection->code, RuleCode::kDup);
  EXPECT_EQ(d.status("P2-001"), OrderStatus::kRiskRejected);
  EXPECT_FALSE(d.child("P2-002").has_value());
  EXPECT_EQ(d.router->poll(), 3U);
}

// R-DUP compares the children, not the parents: another parent whose children look the same (account,
// instrument, side, size, price) is refused inside the window even though its full size differs.
TEST(SliceRunnerDup, ADifferentSizedParentWithTheSameChildShapeIsRefusedToo) {
  SliceDesk d;
  ASSERT_EQ(d.start(d.parent(30)).kind, Kind::kStarted);
  ASSERT_EQ(d.runner->poll(), 3U);
  ASSERT_EQ(d.start(d.parent(40, "P2", 50)).kind, Kind::kStarted);
  EXPECT_EQ(d.runner->poll(), 0U);
  EXPECT_EQ(d.view(50).rejection->code, RuleCode::kDup);
}

// ---- no double send, whatever happens ----

RC_GTEST_PROP(SliceRunnerProperty, EveryChildGoesAtMostOnceAndNeverMoreThanTheParent, ()) {
  SliceDesk d;
  d.ops_cap(static_cast<std::uint8_t>(*rc::gen::inRange(1, 9)));
  const std::int32_t qty = *rc::gen::inRange(11, 300);
  RC_ASSERT(d.start(d.parent(qty)).kind == Kind::kStarted);
  const auto steps = *rc::gen::container<std::vector<int>>(rc::gen::inRange(0, 8));
  for (const int step : steps) {
    switch (step) {
      case 0:
        d.next_second();
        break;
      case 1:
        d.sink.room = static_cast<std::size_t>(*rc::gen::inRange(0, 6));
        break;
      case 2:
        d.sink.room = 1'000'000;
        break;
      case 3:
        static_cast<void>(d.router->poll());
        break;
      case 4:
        static_cast<void>(d.start(d.parent(qty)));
        break;
      default:
        static_cast<void>(d.runner->poll());
        break;
    }
  }
  d.sink.room = 1'000'000;
  for (int i = 0; i < 40; ++i) {
    d.next_second();
    static_cast<void>(d.runner->poll());
    static_cast<void>(d.router->poll());
  }
  std::vector<std::string> sent = d.broker.sent;
  std::ranges::sort(sent);
  RC_ASSERT(std::ranges::adjacent_find(sent) == sent.end());
  std::int64_t total = 0;
  for (const std::int32_t each : d.broker.sent_qty) {
    total += each;
  }
  // What the runner handed on either reached the broker or expired in the router's queue (ADR-027).
  std::int64_t expired = 0;
  for (OrderHandle h = 0; h < d.oms->size(); ++h) {
    if (d.oms->order(h).status == OrderStatus::kExpired) {
      expired += d.oms->order(h).details.qty;
    }
  }
  const SliceView v = d.view();
  RC_ASSERT(total <= qty);
  RC_ASSERT(total + expired == qty - v.unsent_qty);
  RC_ASSERT(v.stopped != SliceStop::kNone || v.unsent_qty == 0);
  RC_ASSERT(d.oms->illegal_transitions() == 0U);
}

// ---- end to end with the SimBroker ----

TEST(SliceRunnerEndToEnd, ASlicedPaperParentFillsCompletely) {
  SliceDesk d;
  auto sim = sim::SimBroker::create(d.instruments, d.book, d.clock);
  ASSERT_TRUE(sim.has_value());
  OrderRouter router(*d.oms, d.sink, *sim, oms::Venue::kPaper, {.tag_limit = 20, .capacity = 64}, d.limits);
  SliceRunner runner(d.gate, d.ctx, *d.oms, router, d.sink, d.ids);
  const auto started = runner.start(d.parent(200));
  ASSERT_TRUE(started.has_value());
  ASSERT_EQ(started->kind, Kind::kStarted);
  for (int second = 0; second < 5; ++second) {
    static_cast<void>(runner.poll());
    static_cast<void>(router.poll());
    static_cast<void>(sim::apply_events(*sim, *d.oms));
    d.tick(kSliced, rupees(99), d.clock.monotonic());  // trades through the buy limit at 100
    static_cast<void>(sim->on_tick(*d.book.find(static_cast<md::InstrumentId>(kSliced))));
    static_cast<void>(sim::apply_events(*sim, *d.oms));
    d.next_second();
  }
  ASSERT_EQ(d.oms->size(), 20U);
  std::int64_t filled = 0;
  for (OrderHandle h = 0; h < d.oms->size(); ++h) {
    EXPECT_EQ(d.oms->order(h).status, OrderStatus::kFilled);
    filled += d.oms->order(h).filled_qty;
  }
  EXPECT_EQ(filled, 200);
  EXPECT_EQ(runner.view(id_of(40))->unsent_qty, 0);
  EXPECT_EQ(d.oms->illegal_transitions(), 0U);
  EXPECT_EQ(sim->stats().apply_errors, 0U);
}

}  // namespace
}  // namespace vyuha::risk
// NOLINTEND(bugprone-unchecked-optional-access)
