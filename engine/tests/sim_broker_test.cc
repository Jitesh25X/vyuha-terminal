// F-EX-4: the SimBroker. Fills against live ticks, through the same Oms and OrderRouter as a live adapter.
// Prices are written in paise (P(10000) is Rs 100.00) and the instrument's tick is Rs 0.05 (5 paise).

#include "vyuha/sim/sim_broker.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>  // rapidcheck's headers use std::exception_ptr without including it
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include "test_gate.h"
#include "vyuha/core/clock.h"
#include "vyuha/oms/order_router.h"
#include "vyuha/risk/risk_gate.h"
#include "vyuha/risk/rules.h"

// NOLINTBEGIN(bugprone-unchecked-optional-access): every test does ASSERT_TRUE(has_value()) first
namespace vyuha::sim {
namespace {

using oms::OrderDetails;
using oms::OrderEvent;
using oms::OrderStatus;
using oms::OrderType;
using oms::Side;
using oms::Validity;
using BrokerState = oms::BrokerOrderView::State;

constexpr std::int64_t kInstr = 1;
constexpr std::int64_t kOther = 2;
constexpr std::int64_t kNoTick = 3;    // an instrument whose tick size is unknown (0)
constexpr std::int64_t kHugeTick = 4;  // a tick size so large that slippage overflows
constexpr Price P(std::int64_t paise) { return Price::from_paise(paise); }
constexpr Nanos kStart = 1'000'000'000'000;

// An approval from a gate whose 22 rules all pass, for the one test that needs a live order approved. Real
// code can't get one without the full chain, and the SimBroker must refuse it anyway.
risk::Outcome pass_rule(const risk::GateContext& /*ctx*/, const OrderDetails& /*order*/,
                        const risk::GateFacts& /*facts*/) noexcept {
  return {};
}
risk::ApprovedOrder approve_live(const OrderDetails& details, const IClock& clock) {
  std::array<risk::Rule, risk::kRuleCount> chain{};
  for (std::size_t i = 0; i < chain.size(); ++i) {
    chain.at(i) = risk::Rule{.code = static_cast<risk::RuleCode>(i + 1), .eval = &pass_rule};
  }
  const risk::RiskGate gate = risk::RiskGate::create(chain, clock).value();
  const auto ctx = std::make_unique<risk::GateContext>();
  risk::GateDecision decision = gate.evaluate(*ctx, details);
  return std::move(decision.approved).value();
}

struct Spec {
  Side side{Side::kBuy};
  OrderType type{OrderType::kLimit};
  std::int32_t qty{10};
  std::optional<Price> price{};    // NOLINT(readability-redundant-member-init): see OrderDetails
  std::optional<Price> trigger{};  // NOLINT(readability-redundant-member-init)
  Validity validity{Validity::kDay};
  std::int64_t instrument{kInstr};
  bool paper{true};
};

Spec limit(Side side, std::int64_t paise) {
  return {.side = side, .type = OrderType::kLimit, .price = P(paise)};
}
Spec market(Side side) { return {.side = side, .type = OrderType::kMarket}; }
Spec stop_market(Side side, std::int64_t trigger) {
  return {.side = side, .type = OrderType::kSlM, .trigger = P(trigger)};
}
Spec stop_limit(Side side, std::int64_t trigger, std::int64_t paise) {
  return {.side = side, .type = OrderType::kSl, .price = P(paise), .trigger = P(trigger)};
}

std::string cid_of(int n) { return "c" + std::to_string(n); }
std::string tag_of(int n) { return "t" + std::to_string(n); }

OrderDetails details_of(int n, const Spec& s) {
  OrderDetails d{.is_paper = s.paper ? oms::kPaperOrder : oms::kLiveOrder};
  d.id.bytes.fill(static_cast<std::uint8_t>(n));
  d.id.bytes.at(1) = static_cast<std::uint8_t>(n >> 8);
  EXPECT_TRUE(d.client_order_id.assign(cid_of(n)));
  d.instrument_id = s.instrument;
  d.side = s.side;
  d.qty = s.qty;
  d.order_type = s.type;
  d.validity = s.validity;
  d.price = s.price;
  d.trigger_price = s.trigger;
  return d;
}

oms::Order order_for(int n, const Spec& s = {}) { return oms::Order{.details = details_of(n, s)}; }

class SimRig {
 public:
  explicit SimRig(const SimConfig& config = {}) : sim_(make(config)) {}
  SimRig(const SimRig&) = delete;
  SimRig& operator=(const SimRig&) = delete;
  SimRig(SimRig&&) = delete;
  SimRig& operator=(SimRig&&) = delete;
  ~SimRig() = default;

  // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes): a test rig; tests move the clock
  // and read the book directly.
  SimClock clock{kStart};
  risk::InstrumentTable instruments{16};
  md::LastValueBook book{16};
  // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

  SimBroker& sim() { return *sim_; }
  [[nodiscard]] const SimBroker& sim() const { return *sim_; }

  // The book only: what send sees as the last tick, with nothing for the resting orders to react to.
  void quote(std::int64_t ltp, std::int64_t bid = 0, std::int64_t ask = 0, std::int64_t id = kInstr) {
    md::Tick t = tick_of(ltp, bid, ask, id);
    ASSERT_TRUE(book.update(t));
  }
  // A live tick: into the book, then to the SimBroker.
  bool tick(std::int64_t ltp, std::int64_t bid = 0, std::int64_t ask = 0, std::int64_t id = kInstr) {
    md::Tick t = tick_of(ltp, bid, ask, id);
    EXPECT_TRUE(book.update(t));
    return sim().on_tick(t);
  }
  // Orders go through the gate's approval, as every order does.
  bool send(int n, const Spec& s) {
    const risk::ApprovedOrder approved =
        s.paper ? testing::approve(details_of(n, s), clock) : approve_live(details_of(n, s), clock);
    return sim().send(approved, tag_of(n));
  }
  std::vector<SimEvent> drain() {
    std::vector<SimEvent> out;
    SimEvent e;
    while (sim().pop_event(e)) {
      out.push_back(e);
    }
    return out;
  }

 private:
  [[nodiscard]] md::Tick tick_of(std::int64_t ltp, std::int64_t bid, std::int64_t ask,
                                 std::int64_t id) const {
    md::Tick t;
    t.instrument_id = static_cast<md::InstrumentId>(id);
    t.ltp = P(ltp);
    t.bid = P(bid);
    t.ask = P(ask);
    t.recv_mono = clock.monotonic();
    t.recv_ts = clock.now();
    return t;
  }
  std::expected<SimBroker, Error> make(const SimConfig& config) {
    EXPECT_TRUE(instruments.put(kInstr, {.lot_size = 1, .tick_size = P(5)}));
    EXPECT_TRUE(instruments.put(kOther, {.lot_size = 1, .tick_size = P(5)}));
    EXPECT_TRUE(instruments.put(kNoTick, {.lot_size = 1, .tick_size = P(0)}));
    EXPECT_TRUE(instruments.put(
        kHugeTick,
        {.lot_size = 1, .tick_size = Price::from_units(std::numeric_limits<std::int64_t>::max() / 1000)}));
    return SimBroker::create(instruments, book, clock, config);
  }
  std::expected<SimBroker, Error> sim_;
};

std::vector<OrderEvent> kinds(const std::vector<SimEvent>& ev) {
  std::vector<OrderEvent> out;
  out.reserve(ev.size());
  for (const SimEvent& e : ev) {
    out.push_back(e.kind);
  }
  return out;
}

using E = OrderEvent;
std::vector<OrderEvent> ack_only() { return {E::kBrokerAck}; }
std::vector<OrderEvent> ack_fill() { return {E::kBrokerAck, E::kFill}; }

// ---- the acceptance criterion and the limit rules ----

TEST(SimBroker, BuyLimitAt100FillsAt100WhenLtpPrints99_95) {  // F-EX-4 AC
  SimRig r;
  ASSERT_TRUE(r.send(1, limit(Side::kBuy, 10000)));
  EXPECT_EQ(kinds(r.drain()), ack_only());
  EXPECT_TRUE(r.tick(9995));
  const auto ev = r.drain();
  ASSERT_EQ(ev.size(), 1U);
  EXPECT_EQ(ev[0].kind, E::kFill);
  EXPECT_EQ(ev[0].fill_price.units(), P(10000).units());
  EXPECT_EQ(ev[0].fill_qty, 10);
  EXPECT_EQ(ev[0].client_order_id.view(), "c1");
  EXPECT_EQ(ev[0].broker_trade_id.view(), "SIMT-1");
  EXPECT_EQ(r.sim().working(), 0U);
}

TEST(SimBroker, SellLimitMirrors) {
  SimRig r;
  ASSERT_TRUE(r.send(1, limit(Side::kSell, 10000)));
  r.drain();
  r.tick(10000);  // a touch, and touch_fill_permille is 0
  EXPECT_TRUE(r.drain().empty());
  r.tick(10005);
  const auto ev = r.drain();
  ASSERT_EQ(ev.size(), 1U);
  EXPECT_EQ(ev[0].fill_price.units(), P(10000).units());  // at the limit price, not the LTP
}

TEST(SimBroker, ALimitAwayFromTheMarketRests) {
  SimRig r;
  ASSERT_TRUE(r.send(1, limit(Side::kBuy, 10000)));
  r.drain();
  r.tick(10005, 10000, 10010);
  r.tick(10100, 10095, 10105);
  EXPECT_TRUE(r.drain().empty());
  EXPECT_EQ(r.sim().working(), 1U);
}

TEST(SimBroker, ATouchNeverFillsWhenThePermilleIsZero) {
  SimRig r;
  ASSERT_TRUE(r.send(1, limit(Side::kBuy, 10000)));
  r.drain();
  for (int i = 0; i < 50; ++i) {
    r.tick(10000);
  }
  EXPECT_TRUE(r.drain().empty());
}

TEST(SimBroker, ATouchAlwaysFillsWhenThePermilleIs1000) {
  SimRig r({.touch_fill_permille = 1000});
  ASSERT_TRUE(r.send(1, limit(Side::kBuy, 10000)));
  r.drain();
  r.tick(10000);
  const auto ev = r.drain();
  ASSERT_EQ(ev.size(), 1U);
  EXPECT_EQ(ev[0].fill_price.units(), P(10000).units());
}

// One touch evaluation is one draw: each send against a last tick at the limit is one. The expected outcome
// of draw i comes from the generator itself.
std::vector<bool> touch_fills(std::uint64_t seed, std::uint32_t permille, int orders) {
  SimRig r({.touch_fill_permille = permille, .seed = seed});
  r.quote(10000);
  std::vector<bool> filled;
  for (int n = 1; n <= orders; ++n) {
    EXPECT_TRUE(r.send(n, limit(Side::kBuy, 10000)));
    const auto ev = r.drain();
    filled.push_back(ev.size() == 2 && ev[1].kind == E::kFill);
  }
  return filled;
}

TEST(SimBroker, TouchDrawsAreReproducibleForASeed) {
  constexpr int kOrders = 40;
  SimRng rng(99);
  std::vector<bool> expected;
  expected.reserve(kOrders);
  for (int i = 0; i < kOrders; ++i) {
    expected.push_back(rng.hit(500));
  }
  const auto first = touch_fills(99, 500, kOrders);
  const auto second = touch_fills(99, 500, kOrders);
  EXPECT_EQ(first, expected);
  EXPECT_EQ(second, expected);
  EXPECT_NE(std::ranges::count(first, true), 0);
  EXPECT_NE(std::ranges::count(first, false), 0);
  EXPECT_NE(touch_fills(100, 500, kOrders), first);  // another seed, another stream
}

// ---- market orders ----

TEST(SimBroker, MarketOrdersFillAtTheOppositeBestPlusSlippage) {
  for (const std::int32_t slip : {0, 1, 3}) {
    SimRig r({.slippage_ticks = slip});
    ASSERT_TRUE(r.send(1, market(Side::kBuy)));
    ASSERT_TRUE(r.send(2, market(Side::kSell)));
    r.drain();
    r.tick(10005, 10000, 10010);
    const auto ev = r.drain();
    ASSERT_EQ(ev.size(), 2U) << slip;
    EXPECT_EQ(ev[0].fill_price.units(), P(10010 + (5 * slip)).units()) << slip;  // BUY: ask + slip
    EXPECT_EQ(ev[1].fill_price.units(), P(10000 - (5 * slip)).units()) << slip;  // SELL: bid - slip
  }
}

TEST(SimBroker, AMarketOrderUsesTheLtpWhenTheOppositeSideIsEmpty) {
  SimRig r({.slippage_ticks = 2});
  ASSERT_TRUE(r.send(1, market(Side::kBuy)));
  ASSERT_TRUE(r.send(2, market(Side::kSell)));
  r.drain();
  r.tick(10005, 10000, 0);  // no ask: BUY falls back; the bid is there for the SELL
  auto ev = r.drain();
  ASSERT_EQ(ev.size(), 2U);
  EXPECT_EQ(ev[0].fill_price.units(), P(10015).units());
  EXPECT_EQ(ev[1].fill_price.units(), P(9990).units());

  SimRig q({.slippage_ticks = 2});
  ASSERT_TRUE(q.send(1, market(Side::kSell)));
  q.drain();
  q.tick(10005, 0, 10010);  // no bid
  ev = q.drain();
  ASSERT_EQ(ev.size(), 1U);
  EXPECT_EQ(ev[0].fill_price.units(), P(9995).units());
}

TEST(SimBroker, AMarketOrderWithNoTickWaitsForTheFirstOne) {
  SimRig r;
  ASSERT_TRUE(r.send(1, market(Side::kBuy)));
  EXPECT_EQ(kinds(r.drain()), ack_only());  // not rejected, not filled
  EXPECT_EQ(r.sim().working(), 1U);
  r.tick(10005, 10000, 10010);
  const auto ev = r.drain();
  ASSERT_EQ(ev.size(), 1U);
  EXPECT_EQ(ev[0].fill_price.units(), P(10015).units());
}

TEST(SimBroker, AMarketOrderFillsOnSendAgainstAFreshLastTick) {
  SimRig r;
  r.quote(10005, 10000, 10010);
  ASSERT_TRUE(r.send(1, market(Side::kBuy)));
  const auto ev = r.drain();
  ASSERT_EQ(kinds(ev), ack_fill());
  EXPECT_EQ(ev[1].fill_price.units(), P(10015).units());
}

TEST(SimBroker, ATickOlderThanThreeSecondsCountsAsNone) {
  SimRig r;
  r.quote(10005, 10000, 10010);
  r.clock.advance(risk::kLtpMaxAge + 1);
  ASSERT_TRUE(r.send(1, market(Side::kBuy)));
  EXPECT_EQ(kinds(r.drain()), ack_only());
  EXPECT_EQ(r.sim().working(), 1U);
}

TEST(SimBroker, ATickExactlyThreeSecondsOldIsUsable) {
  SimRig r;
  r.quote(10005, 10000, 10010);
  r.clock.advance(risk::kLtpMaxAge);
  ASSERT_TRUE(r.send(1, market(Side::kBuy)));
  EXPECT_EQ(kinds(r.drain()), ack_fill());
}

TEST(SimBroker, ATickStampedInTheFutureIsNotUsable) {
  SimRig r;
  md::Tick t;
  t.instrument_id = kInstr;
  t.ltp = P(10005);
  t.ask = P(10010);
  t.recv_mono = r.clock.monotonic() + kNanosPerSecond;  // a negative age
  ASSERT_TRUE(r.book.update(t));
  ASSERT_TRUE(r.send(1, market(Side::kBuy)));
  EXPECT_EQ(kinds(r.drain()), ack_only());
}

TEST(SimBroker, ATickWithNoLtpIsIgnored) {
  SimRig r;
  ASSERT_TRUE(r.send(1, market(Side::kBuy)));
  r.drain();
  EXPECT_TRUE(r.tick(0, 10000, 10010));
  EXPECT_TRUE(r.drain().empty());
}

// ---- marketable limits ----

TEST(SimBroker, AMarketableLimitFillsAtTheOppositeBestNotItsPrice) {
  SimRig r;
  ASSERT_TRUE(r.send(1, limit(Side::kBuy, 10050)));
  ASSERT_TRUE(r.send(2, limit(Side::kSell, 9950)));
  r.drain();
  r.tick(10005, 10000, 10010);
  const auto ev = r.drain();
  ASSERT_EQ(ev.size(), 2U);
  EXPECT_EQ(ev[0].fill_price.units(), P(10010).units());  // the ask: price improvement, no slippage
  EXPECT_EQ(ev[1].fill_price.units(), P(10000).units());  // the bid
}

TEST(SimBroker, AMarketableLimitFillsOnSend) {
  SimRig r;
  r.quote(10005, 10000, 10010);
  ASSERT_TRUE(r.send(1, limit(Side::kBuy, 10050)));
  const auto ev = r.drain();
  ASSERT_EQ(kinds(ev), ack_fill());
  EXPECT_EQ(ev[1].fill_price.units(), P(10010).units());
}

TEST(SimBroker, ALimitTradedThroughOnSendFillsAtItsPrice) {
  SimRig r;
  r.quote(9995);  // no quote sides
  ASSERT_TRUE(r.send(1, limit(Side::kBuy, 10000)));
  const auto ev = r.drain();
  ASSERT_EQ(kinds(ev), ack_fill());
  EXPECT_EQ(ev[1].fill_price.units(), P(10000).units());
}

// ---- stops ----

TEST(SimBroker, StopMarketBuyTriggersAtItsTriggerExactly) {
  SimRig r;
  ASSERT_TRUE(r.send(1, stop_market(Side::kBuy, 10100)));
  r.drain();
  r.tick(10095, 10090, 10100);
  EXPECT_TRUE(r.drain().empty());
  r.tick(10100, 10095, 10105);  // LTP == trigger
  const auto ev = r.drain();
  ASSERT_EQ(ev.size(), 1U);
  EXPECT_EQ(ev[0].fill_price.units(), P(10110).units());  // a market order: ask + 1 tick
}

TEST(SimBroker, StopMarketSellTriggersAtItsTriggerExactly) {
  SimRig r;
  ASSERT_TRUE(r.send(1, stop_market(Side::kSell, 9900)));
  r.drain();
  r.tick(9905, 9900, 9910);
  EXPECT_TRUE(r.drain().empty());
  r.tick(9900, 9895, 9905);
  const auto ev = r.drain();
  ASSERT_EQ(ev.size(), 1U);
  EXPECT_EQ(ev[0].fill_price.units(), P(9890).units());  // bid - 1 tick
}

TEST(SimBroker, StopLimitBecomesAMarketableLimitWhenTriggered) {
  SimRig r;
  ASSERT_TRUE(r.send(1, stop_limit(Side::kBuy, 10100, 10150)));
  ASSERT_TRUE(r.send(2, stop_limit(Side::kSell, 9900, 9850)));
  r.drain();
  r.tick(10000, 9995, 10005);
  EXPECT_TRUE(r.drain().empty());
  r.tick(10100, 10095, 10110);  // BUY triggers; limit 101.50 >= ask: fills at the ask
  auto ev = r.drain();
  ASSERT_EQ(ev.size(), 1U);
  EXPECT_EQ(ev[0].client_order_id.view(), "c1");
  EXPECT_EQ(ev[0].fill_price.units(), P(10110).units());
  r.tick(9900, 9890, 9905);  // SELL triggers; limit 98.50 <= bid: fills at the bid
  ev = r.drain();
  ASSERT_EQ(ev.size(), 1U);
  EXPECT_EQ(ev[0].client_order_id.view(), "c2");
  EXPECT_EQ(ev[0].fill_price.units(), P(9890).units());
}

TEST(SimBroker, AStopAlreadyThroughItsTriggerOnSendTriggersAtOnce) {
  SimRig r;
  r.quote(10200, 10195, 10205);
  ASSERT_TRUE(r.send(1, stop_market(Side::kBuy, 10100)));
  auto ev = r.drain();
  ASSERT_EQ(kinds(ev), ack_fill());
  EXPECT_EQ(ev[1].fill_price.units(), P(10210).units());

  r.quote(9800, 9795, 9805);
  ASSERT_TRUE(r.send(2, stop_market(Side::kSell, 9900)));
  ev = r.drain();
  ASSERT_EQ(kinds(ev), ack_fill());
  EXPECT_EQ(ev[1].fill_price.units(), P(9790).units());
}

TEST(SimBroker, ATriggeredStopLimitThatCannotFillRestsAsALimit) {
  SimRig r;
  ASSERT_TRUE(r.send(1, stop_limit(Side::kBuy, 10100, 10100)));
  r.drain();
  r.tick(10150, 10145, 10155);  // triggers, but the limit (101.00) is below the market
  EXPECT_TRUE(r.drain().empty());
  EXPECT_EQ(r.sim().working(), 1U);
  r.tick(10090, 10085, 10095);  // below the trigger again; the order is a limit now and trades through
  const auto ev = r.drain();
  ASSERT_EQ(ev.size(), 1U);
  EXPECT_EQ(ev[0].fill_price.units(), P(10095).units());  // marketable: the ask, which is under 101.00
}

TEST(SimBroker, ATriggeredStopMarketWithNoQuoteStillFillsOnTheLtp) {
  SimRig r;
  ASSERT_TRUE(r.send(1, stop_market(Side::kBuy, 10100)));
  r.drain();
  r.tick(10100);
  const auto ev = r.drain();
  ASSERT_EQ(ev.size(), 1U);
  EXPECT_EQ(ev[0].fill_price.units(), P(10105).units());
}

// ---- validity ----

TEST(SimBroker, AnIocRemainderIsCancelled) {
  SimRig r;
  r.quote(10100, 10095, 10105);
  Spec s = limit(Side::kBuy, 10000);
  s.validity = Validity::kIoc;
  ASSERT_TRUE(r.send(1, s));
  auto ev = r.drain();
  ASSERT_EQ(ev.size(), 2U);
  EXPECT_EQ(ev[0].kind, E::kBrokerAck);
  EXPECT_EQ(ev[1].kind, E::kBrokerCancel);
  EXPECT_EQ(ev[1].reason_code, "IOC_REMAINDER");
  EXPECT_EQ(r.sim().working(), 0U);
  r.tick(9900);  // it is gone: nothing fills later
  EXPECT_TRUE(r.drain().empty());

  s = market(Side::kBuy);  // an IOC market with no usable quote has nothing to trade against
  s.validity = Validity::kIoc;
  SimRig q;
  ASSERT_TRUE(q.send(2, s));
  ev = q.drain();
  ASSERT_EQ(ev.size(), 2U);
  EXPECT_EQ(ev[1].kind, E::kBrokerCancel);
}

TEST(SimBroker, AnIocThatFillsIsNotCancelled) {
  SimRig r;
  r.quote(10005, 10000, 10010);
  Spec s = limit(Side::kBuy, 10050);
  s.validity = Validity::kIoc;
  ASSERT_TRUE(r.send(1, s));
  EXPECT_EQ(kinds(r.drain()), ack_fill());
}

TEST(SimBroker, ADayOrderRests) {
  SimRig r;
  r.quote(10100, 10095, 10105);
  ASSERT_TRUE(r.send(1, limit(Side::kBuy, 10000)));
  EXPECT_EQ(kinds(r.drain()), ack_only());
  EXPECT_EQ(r.sim().working(), 1U);
  r.clock.advance(3600 * kNanosPerSecond);
  EXPECT_EQ(r.sim().working(), 1U);
}

// ---- send ----

TEST(SimBroker, ALiveOrderIsRefusedWithNothingCreated) {
  SimRig r;
  Spec s = limit(Side::kBuy, 10000);
  s.paper = false;
  EXPECT_FALSE(r.send(1, s));
  EXPECT_TRUE(r.drain().empty());
  EXPECT_EQ(r.sim().working(), 0U);
  EXPECT_EQ(r.sim().stats().refused_live, 1U);
  EXPECT_TRUE(r.sim().find_by_tag(tag_of(1)).value().empty());
}

TEST(SimBroker, ASpentApprovalIsRefused) {
  SimRig r;
  risk::ApprovedOrder approved = testing::approve(details_of(1, limit(Side::kBuy, 10000)), r.clock);
  const risk::ApprovedOrder moved = std::move(approved);
  ASSERT_TRUE(approved.spent());  // NOLINT(bugprone-use-after-move): spent() is how a moved-from one is told
  EXPECT_FALSE(r.sim().send(approved, "t1"));
  EXPECT_TRUE(r.drain().empty());
  EXPECT_EQ(r.sim().stats().refused_spent, 1U);
  EXPECT_TRUE(r.sim().send(moved, "t1"));  // the approval it was moved to still works
}

TEST(SimBroker, AFullOrderTableRefusesAndCreatesNothing) {
  SimRig r({.max_orders = 2});
  ASSERT_TRUE(r.send(1, limit(Side::kBuy, 10000)));
  ASSERT_TRUE(r.send(2, limit(Side::kBuy, 10000)));
  r.drain();
  EXPECT_FALSE(r.send(3, limit(Side::kBuy, 10000)));
  EXPECT_TRUE(r.drain().empty());
  EXPECT_EQ(r.sim().working(), 2U);
  EXPECT_EQ(r.sim().stats().refused_full, 1U);
  EXPECT_TRUE(r.sim().find_by_tag(tag_of(3)).value().empty());
  ASSERT_TRUE(r.sim().cancel(order_for(1)));  // room again
  r.drain();
  EXPECT_TRUE(r.send(3, limit(Side::kBuy, 10000)));
}

TEST(SimBroker, AnUnknownInstrumentIsRefused) {
  SimRig r;
  Spec s = limit(Side::kBuy, 10000);
  s.instrument = 99;
  EXPECT_FALSE(r.send(1, s));
  EXPECT_TRUE(r.drain().empty());
  EXPECT_EQ(r.sim().working(), 0U);
}

TEST(SimBroker, ATypeThatNeedsTheTickSizeIsRefusedWithoutOne) {
  SimRig r;
  Spec s = market(Side::kBuy);
  s.instrument = kNoTick;
  EXPECT_FALSE(r.send(1, s));
  s = stop_market(Side::kBuy, 10100);
  s.instrument = kNoTick;
  EXPECT_FALSE(r.send(2, s));
  s = limit(Side::kBuy, 10000);  // a limit needs no tick size
  s.instrument = kNoTick;
  EXPECT_TRUE(r.send(3, s));
}

TEST(SimBroker, ADuplicateTagIsIdempotent) {
  SimRig r;
  const risk::ApprovedOrder approved = testing::approve(details_of(1, limit(Side::kBuy, 10000)), r.clock);
  ASSERT_TRUE(r.sim().send(approved, "t1"));
  ASSERT_TRUE(r.sim().send(approved, "t1"));
  EXPECT_EQ(kinds(r.drain()), ack_only());  // one order, one ack
  EXPECT_EQ(r.sim().working(), 1U);
  EXPECT_EQ(r.sim().find_by_tag("t1").value().size(), 1U);
}

TEST(SimBroker, ATagHeldByAnotherOrderIsRefused) {
  SimRig r;
  const risk::ApprovedOrder first = testing::approve(details_of(1, limit(Side::kBuy, 10000)), r.clock);
  const risk::ApprovedOrder second = testing::approve(details_of(2, limit(Side::kBuy, 10000)), r.clock);
  ASSERT_TRUE(r.sim().send(first, "same"));
  EXPECT_FALSE(r.sim().send(second, "same"));  // not "already handed over": it is a different order
  EXPECT_EQ(r.sim().working(), 1U);
  EXPECT_EQ(r.sim().stats().refused_tag, 1U);
}

TEST(SimBroker, AnEmptyOrOverlongTagIsRefused) {
  SimRig r;
  const risk::ApprovedOrder approved = testing::approve(details_of(1, limit(Side::kBuy, 10000)), r.clock);
  EXPECT_FALSE(r.sim().send(approved, ""));
  EXPECT_FALSE(r.sim().send(approved, std::string(41, 'x')));
  EXPECT_TRUE(r.drain().empty());
}

TEST(SimBroker, MalformedOrdersAreRejectedByAnEventNeverAcked) {
  Spec no_qty = limit(Side::kBuy, 10000);
  no_qty.qty = 0;
  Spec neg_qty = limit(Side::kBuy, 10000);
  neg_qty.qty = -5;
  const Spec limit_no_price = {.type = OrderType::kLimit};
  const Spec sl_no_price = {.type = OrderType::kSl, .trigger = P(10100)};
  const Spec sl_no_trigger = {.type = OrderType::kSl, .price = P(10100)};
  const Spec slm_no_trigger = {.type = OrderType::kSlM};
  const Spec zero_price = limit(Side::kBuy, 0);
  int n = 0;
  for (const Spec& s :
       {no_qty, neg_qty, limit_no_price, sl_no_price, sl_no_trigger, slm_no_trigger, zero_price}) {
    SimRig r;
    r.quote(10005, 10000, 10010);
    ++n;
    ASSERT_TRUE(r.send(n, s)) << n;
    const auto ev = r.drain();
    ASSERT_EQ(ev.size(), 1U) << n;
    EXPECT_EQ(ev[0].kind, E::kBrokerReject) << n;
    EXPECT_EQ(ev[0].reason_code, "SIM_INVALID") << n;
    EXPECT_TRUE(ev[0].broker_order_id.empty()) << n;
    EXPECT_EQ(r.sim().working(), 0U);
    const auto view = r.sim().find_by_tag(tag_of(n)).value();
    ASSERT_EQ(view.size(), 1U) << n;
    EXPECT_EQ(view[0].state, BrokerState::kRejected) << n;
    EXPECT_TRUE(r.send(n, s)) << n;  // a repeat is idempotent: no second reject
    EXPECT_TRUE(r.drain().empty()) << n;
  }
}

TEST(SimBroker, BrokerOrderIdsAreNumberedPerInstance) {
  SimRig r;
  ASSERT_TRUE(r.send(1, limit(Side::kBuy, 10000)));
  ASSERT_TRUE(r.send(2, limit(Side::kBuy, 10000)));
  const auto ev = r.drain();
  ASSERT_EQ(ev.size(), 2U);
  EXPECT_EQ(ev[0].broker_order_id.view(), "SIM-1");
  EXPECT_EQ(ev[1].broker_order_id.view(), "SIM-2");
  SimRig q;
  ASSERT_TRUE(q.send(1, limit(Side::kBuy, 10000)));
  EXPECT_EQ(q.drain()[0].broker_order_id.view(), "SIM-1");
}

// ---- cancel ----

TEST(SimBroker, CancelOfAWorkingOrderConfirms) {
  SimRig r;
  ASSERT_TRUE(r.send(1, limit(Side::kBuy, 10000)));
  r.drain();
  EXPECT_TRUE(r.sim().cancel(order_for(1)));
  const auto ev = r.drain();
  ASSERT_EQ(ev.size(), 1U);
  EXPECT_EQ(ev[0].kind, E::kCancelConfirm);
  EXPECT_EQ(ev[0].broker_order_id.view(), "SIM-1");
  EXPECT_EQ(r.sim().working(), 0U);
  r.tick(9900);  // gone
  EXPECT_TRUE(r.drain().empty());
}

TEST(SimBroker, CancelOfAFilledOrderIsRefusedByAnEventAndReturnsTrue) {
  SimRig r;
  ASSERT_TRUE(r.send(1, limit(Side::kBuy, 10000)));
  r.tick(9995);
  r.drain();
  EXPECT_TRUE(r.sim().cancel(order_for(1)));
  const auto ev = r.drain();
  ASSERT_EQ(ev.size(), 1U);
  EXPECT_EQ(ev[0].kind, E::kCancelReject);
  EXPECT_EQ(ev[0].reason_code, "SIM_TOO_LATE");
}

TEST(SimBroker, CancelOfAnUnknownOrderIsFalse) {
  SimRig r;
  EXPECT_FALSE(r.sim().cancel(order_for(7)));
  EXPECT_TRUE(r.drain().empty());
  EXPECT_EQ(r.sim().stats().cancel_unknown, 1U);
}

TEST(SimBroker, ARepeatedCancelIsHarmless) {
  SimRig r;
  ASSERT_TRUE(r.send(1, limit(Side::kBuy, 10000)));
  r.drain();
  EXPECT_TRUE(r.sim().cancel(order_for(1)));
  EXPECT_TRUE(r.sim().cancel(order_for(1)));
  EXPECT_TRUE(r.sim().cancel(order_for(1)));
  const auto ev = r.drain();
  ASSERT_EQ(ev.size(), 3U);
  EXPECT_EQ(ev[0].kind, E::kCancelConfirm);
  EXPECT_EQ(ev[1].kind, E::kCancelConfirm);  // cancelled already: confirmed again
  EXPECT_EQ(ev[2].kind, E::kCancelConfirm);
  EXPECT_EQ(r.sim().working(), 0U);
}

TEST(SimBroker, CancelFindsAnOrderTheOmsHasNoBrokerIdFor) {
  SimRig r;
  ASSERT_TRUE(r.send(1, limit(Side::kBuy, 10000)));  // handed over, the ack not seen yet
  const oms::Order order = order_for(1);
  ASSERT_TRUE(order.broker_order_id.empty());
  EXPECT_TRUE(r.sim().cancel(order));
  r.drain();
  EXPECT_EQ(r.sim().working(), 0U);
}

TEST(SimBroker, CancelWithAnotherOrdersBrokerIdIsFalse) {
  SimRig r;
  ASSERT_TRUE(r.send(1, limit(Side::kBuy, 10000)));
  r.drain();
  oms::Order order = order_for(1);
  ASSERT_TRUE(order.broker_order_id.assign("SIM-999"));
  EXPECT_FALSE(r.sim().cancel(order));
  EXPECT_EQ(r.sim().working(), 1U);
  ASSERT_TRUE(order.broker_order_id.assign("SIM-1"));
  EXPECT_TRUE(r.sim().cancel(order));
}

// ---- the event queue ----

TEST(SimBroker, AFullEventQueueNeverLosesAFill) {
  SimRig r({.event_capacity = 4});
  ASSERT_TRUE(r.send(1, limit(Side::kBuy, 10000)));
  ASSERT_TRUE(r.send(2, limit(Side::kBuy, 10000)));
  r.drain();
  Spec other = limit(Side::kBuy, 5000);
  other.instrument = kOther;
  ASSERT_TRUE(r.send(3, other));
  ASSERT_TRUE(r.send(4, other));
  ASSERT_TRUE(r.send(5, other));   // three events queued, one place left
  EXPECT_FALSE(r.send(6, other));  // a send needs two: nothing is created
  EXPECT_EQ(r.sim().working(), 5U);
  EXPECT_TRUE(r.sim().find_by_tag(tag_of(6)).value().empty());
  EXPECT_EQ(r.sim().stats().queue_full, 1U);

  EXPECT_FALSE(r.tick(9995));  // order 1 fills into the last place; order 2 has to wait
  EXPECT_EQ(r.sim().stats().queue_full, 2U);
  EXPECT_EQ(r.sim().working(), 4U);
  auto ev = r.drain();
  ASSERT_EQ(ev.size(), 4U);
  EXPECT_EQ(ev[3].kind, E::kFill);  // delivered
  EXPECT_EQ(ev[3].client_order_id.view(), "c1");

  EXPECT_TRUE(r.tick(9994));  // the next call finds room and order 2 fills
  ev = r.drain();
  ASSERT_EQ(ev.size(), 1U);
  EXPECT_EQ(ev[0].client_order_id.view(), "c2");
  EXPECT_EQ(r.sim().stats().fills, 2U);
}

TEST(SimBroker, ACancelWithAFullQueueIsRetriedNotLost) {
  SimRig r({.event_capacity = 4});
  ASSERT_TRUE(r.send(1, limit(Side::kBuy, 10000)));
  ASSERT_TRUE(r.send(2, limit(Side::kBuy, 10000)));
  Spec other = limit(Side::kBuy, 5000);
  other.instrument = kOther;
  ASSERT_TRUE(r.send(3, other));               // three acks queued, one place left
  EXPECT_TRUE(r.sim().cancel(order_for(1)));   // takes the last place
  EXPECT_FALSE(r.sim().cancel(order_for(2)));  // nowhere to say it: the caller retries
  EXPECT_EQ(r.sim().working(), 2U);
  r.drain();
  EXPECT_TRUE(r.sim().cancel(order_for(2)));
  EXPECT_EQ(r.sim().working(), 1U);
}

// ---- find_by_tag ----

TEST(SimBroker, FindByTagReportsWorkingFilledAndCancelledOrders) {
  SimRig r;
  ASSERT_TRUE(r.send(1, limit(Side::kBuy, 10000)));
  ASSERT_TRUE(r.send(2, stop_limit(Side::kSell, 9900, 9850)));
  ASSERT_TRUE(r.send(3, limit(Side::kBuy, 10000)));
  ASSERT_TRUE(r.send(4, limit(Side::kBuy, 9000)));
  r.drain();
  r.tick(9995);  // fills 1 and 3 at 100.00
  ASSERT_TRUE(r.sim().cancel(order_for(4)));

  const auto open = r.sim().find_by_tag(tag_of(2)).value();
  ASSERT_EQ(open.size(), 1U);
  EXPECT_EQ(open[0].state, BrokerState::kOpen);
  EXPECT_EQ(open[0].filled_qty, 0);
  EXPECT_EQ(open[0].qty, 10);
  EXPECT_EQ(open[0].price->units(), P(9850).units());
  EXPECT_EQ(open[0].trigger_price->units(), P(9900).units());
  EXPECT_EQ(open[0].broker_order_id, "SIM-2");

  const auto filled = r.sim().find_by_tag(tag_of(1)).value();
  ASSERT_EQ(filled.size(), 1U);
  EXPECT_EQ(filled[0].state, BrokerState::kFilled);
  EXPECT_EQ(filled[0].filled_qty, 10);
  EXPECT_EQ(filled[0].average_price.units(), P(10000).units());

  const auto cancelled = r.sim().find_by_tag(tag_of(4)).value();
  ASSERT_EQ(cancelled.size(), 1U);
  EXPECT_EQ(cancelled[0].state, BrokerState::kCancelled);
  EXPECT_TRUE(r.sim().find_by_tag("nope").value().empty());
}

TEST(SimBroker, TheHistoryIsARingAndEvictsTheOldest) {
  SimRig r({.history = 2});
  for (int n = 1; n <= 3; ++n) {
    ASSERT_TRUE(r.send(n, limit(Side::kBuy, 10000)));
    ASSERT_TRUE(r.sim().cancel(order_for(n)));
  }
  EXPECT_TRUE(r.sim().find_by_tag(tag_of(1)).value().empty());  // evicted
  EXPECT_EQ(r.sim().find_by_tag(tag_of(2)).value().size(), 1U);
  EXPECT_EQ(r.sim().find_by_tag(tag_of(3)).value().size(), 1U);
  EXPECT_FALSE(r.sim().cancel(order_for(1)));        // unknown now
  EXPECT_TRUE(r.sim().cancel(order_for(2)));         // still in history: too late, but known
  EXPECT_TRUE(r.send(1, limit(Side::kBuy, 10000)));  // the tag is free again
}

// ---- boundaries and odd ticks ----

TEST(SimBroker, MarketableLimitBoundariesAtEquality) {
  SimRig r;
  ASSERT_TRUE(r.send(1, limit(Side::kBuy, 10010)));   // == ask: fills at the ask
  ASSERT_TRUE(r.send(2, limit(Side::kBuy, 10005)));   // one tick under the ask, LTP above it: rests
  ASSERT_TRUE(r.send(3, limit(Side::kSell, 10000)));  // == bid: fills at the bid
  ASSERT_TRUE(r.send(4, limit(Side::kSell, 10005)));  // one tick over the bid, LTP not above it: rests
  r.drain();
  r.tick(10005, 10000, 10010);  // LTP at the one-tick limits: a touch, and permille is 0
  const auto ev = r.drain();
  ASSERT_EQ(ev.size(), 2U);
  EXPECT_EQ(ev[0].client_order_id.view(), "c1");
  EXPECT_EQ(ev[0].fill_price.units(), P(10010).units());
  EXPECT_EQ(ev[1].client_order_id.view(), "c3");
  EXPECT_EQ(ev[1].fill_price.units(), P(10000).units());
  EXPECT_EQ(r.sim().working(), 2U);
}

TEST(SimBroker, ACrossedTickNeverFillsALimitWorseThanItsPrice) {
  SimRig r;
  ASSERT_TRUE(r.send(1, limit(Side::kBuy, 10050)));
  ASSERT_TRUE(r.send(2, limit(Side::kSell, 9950)));
  ASSERT_TRUE(r.send(3, limit(Side::kBuy, 10005)));
  ASSERT_TRUE(r.send(4, limit(Side::kSell, 10025)));
  r.drain();
  r.tick(10015, 10020, 10010);  // bid above ask
  const auto ev = r.drain();
  for (const SimEvent& e : ev) {
    const int n = std::stoi(std::string(e.client_order_id.view()).substr(1));
    if (n == 1 || n == 3) {
      EXPECT_LE(e.fill_price.units(), n == 1 ? P(10050).units() : P(10005).units());
    } else {
      EXPECT_GE(e.fill_price.units(), n == 2 ? P(9950).units() : P(10025).units());
    }
  }
  ASSERT_GE(ev.size(), 2U);
  EXPECT_EQ(ev[0].fill_price.units(), P(10010).units());  // BUY 100.50: the ask
  EXPECT_EQ(ev[1].fill_price.units(), P(10020).units());  // SELL 99.50: the bid
}

TEST(SimBroker, ASellTouchFillsAtPermille1000) {
  SimRig r({.touch_fill_permille = 1000});
  ASSERT_TRUE(r.send(1, limit(Side::kSell, 10000)));
  r.drain();
  r.tick(10000);
  const auto ev = r.drain();
  ASSERT_EQ(ev.size(), 1U);
  EXPECT_EQ(ev[0].fill_price.units(), P(10000).units());
}

TEST(SimBroker, ASellMarketWhoseSlippedPriceIsNotPositiveWaits) {
  SimRig r;
  ASSERT_TRUE(r.send(1, market(Side::kSell)));
  r.drain();
  r.tick(5, 5, 10);  // bid 0.05 less one tick of 0.05 is 0
  EXPECT_TRUE(r.drain().empty());
  EXPECT_EQ(r.sim().working(), 1U);
  r.tick(100, 50, 100);  // a real price later
  const auto ev = r.drain();
  ASSERT_EQ(ev.size(), 1U);
  EXPECT_EQ(ev[0].fill_price.units(), P(45).units());
}

TEST(SimBroker, SlippageThatOverflowsIsNoFill) {
  SimRig r({.slippage_ticks = 10'000});
  Spec s = market(Side::kBuy);
  s.instrument = kHugeTick;
  ASSERT_TRUE(r.send(1, s));  // tick size * 10,000 overflows int64
  r.drain();
  r.tick(10000, 9995, 10005, kHugeTick);
  EXPECT_TRUE(r.drain().empty());
  EXPECT_EQ(r.sim().working(), 1U);

  SimRig q;  // an ask next to the top of the range: the addition would wrap
  ASSERT_TRUE(q.send(1, market(Side::kBuy)));
  q.drain();
  md::Tick t;
  t.instrument_id = kInstr;
  t.ltp = P(10000);
  t.ask = Price::from_units(std::numeric_limits<std::int64_t>::max() - 10);
  t.recv_mono = q.clock.monotonic();
  EXPECT_TRUE(q.sim().on_tick(t));
  EXPECT_TRUE(q.drain().empty());
}

// ---- config ----

TEST(SimBroker, ConfigRangesAreValidated) {
  const risk::InstrumentTable instruments{4};
  const md::LastValueBook book{4};
  const SimClock clock;
  const auto code = [&](const SimConfig& c) {
    const auto made = SimBroker::create(instruments, book, clock, c);
    return made.has_value() ? ErrorCode{} : made.error().code;
  };
  EXPECT_TRUE(SimBroker::create(instruments, book, clock).has_value());
  EXPECT_EQ(code({.max_orders = 0}), ErrorCode::kInvalidArgument);
  EXPECT_EQ(code({.history = 0}), ErrorCode::kInvalidArgument);
  EXPECT_EQ(code({.slippage_ticks = -1}), ErrorCode::kInvalidArgument);
  EXPECT_EQ(code({.slippage_ticks = 10'001}), ErrorCode::kInvalidArgument);
  EXPECT_EQ(code({.touch_fill_permille = 1001}), ErrorCode::kInvalidArgument);
  EXPECT_EQ(code({.event_capacity = 3}), ErrorCode::kInvalidArgument);
  EXPECT_EQ(code({.max_orders = 5'000'000}), ErrorCode::kInvalidArgument);
  EXPECT_EQ(code({.slippage_ticks = 0, .touch_fill_permille = 1000}), ErrorCode{});
}

// ---- end to end: Oms, OrderRouter (Venue::kPaper) and the pump ----

class RecordingSink final : public oms::IOrderEventSink {
 public:
  // NOLINTNEXTLINE(bugprone-exception-escape): a test's allocation failure may terminate
  [[nodiscard]] bool try_push(const oms::OrderEventRecord& event) noexcept override {
    if (full) {
      return false;
    }
    events.push_back(event);
    return true;
  }
  [[nodiscard]] std::uint64_t pushed() const noexcept override { return events.size(); }
  [[nodiscard]] std::uint64_t durable() const noexcept override { return events.size(); }
  std::vector<oms::OrderEventRecord> events;
  bool full{false};
};

struct EndToEnd {
  SimRig rig;
  RecordingSink sink;
  oms::Oms oms{sink, rig.clock, 16};
  oms::OrderRouter router{
      oms, sink, rig.sim(), oms::Venue::kPaper, {.tag_limit = 20, .capacity = 16}, testing::open_limits()};

  oms::OrderHandle place(int n, const Spec& s) {
    const oms::OrderHandle h = oms.create(details_of(n, s)).value();
    EXPECT_TRUE(router.submit(h, testing::approve(oms.order(h).details, rig.clock)).has_value());
    return h;
  }
  // Sends it and takes the ack in.
  oms::OrderHandle place_open(int n, const Spec& s) {
    const oms::OrderHandle h = place(n, s);
    EXPECT_EQ(router.poll(), 1U);
    EXPECT_EQ(apply_events(rig.sim(), oms), 1U);
    EXPECT_EQ(oms.order(h).status, OrderStatus::kOpen);
    return h;
  }
};

TEST(SimBrokerEndToEnd, APaperOrderGoesNewApprovedSentOpenFilled) {
  EndToEnd e;
  const oms::OrderHandle h = e.place(1, limit(Side::kBuy, 10000));
  EXPECT_EQ(e.oms.order(h).status, OrderStatus::kApproved);
  EXPECT_EQ(e.router.poll(), 1U);
  EXPECT_EQ(e.oms.order(h).status, OrderStatus::kSent);
  EXPECT_EQ(apply_events(e.rig.sim(), e.oms), 1U);
  EXPECT_EQ(e.oms.order(h).status, OrderStatus::kOpen);
  EXPECT_EQ(e.oms.order(h).broker_order_id.view(), "SIM-1");

  e.rig.tick(9995);
  EXPECT_EQ(apply_events(e.rig.sim(), e.oms), 1U);
  const oms::Order& order = e.oms.order(h);
  EXPECT_EQ(order.status, OrderStatus::kFilled);
  EXPECT_EQ(order.filled_qty, 10);
  const oms::OrderEventRecord& last = e.sink.events.back();
  EXPECT_EQ(last.to, OrderStatus::kFilled);
  EXPECT_EQ(last.fill_price.units(), P(10000).units());
  EXPECT_EQ(last.fill_qty, 10);
  EXPECT_EQ(last.broker_trade_id.view(), "SIMT-1");
  EXPECT_EQ(last.broker_order_id.view(), "SIM-1");
  EXPECT_EQ(e.oms.illegal_transitions(), 0U);
  EXPECT_EQ(e.rig.sim().stats().apply_errors, 0U);
}

TEST(SimBrokerEndToEnd, AMarketableOrderIsFilledByTheTimeThePumpRuns) {
  EndToEnd e;
  e.rig.quote(10005, 10000, 10010);
  const oms::OrderHandle h = e.place(1, limit(Side::kBuy, 10050));
  EXPECT_EQ(e.router.poll(), 1U);
  EXPECT_EQ(apply_events(e.rig.sim(), e.oms), 2U);  // ack, fill
  EXPECT_EQ(e.oms.order(h).status, OrderStatus::kFilled);
  EXPECT_EQ(e.sink.events.back().fill_price.units(), P(10010).units());
  EXPECT_EQ(e.oms.illegal_transitions(), 0U);
}

TEST(SimBrokerEndToEnd, ACancelRoundTrip) {
  EndToEnd e;
  const oms::OrderHandle h = e.place_open(1, limit(Side::kBuy, 10000));
  EXPECT_TRUE(e.router.cancel(h));
  EXPECT_EQ(e.oms.order(h).status, OrderStatus::kCancelPending);
  EXPECT_EQ(apply_events(e.rig.sim(), e.oms), 1U);
  EXPECT_EQ(e.oms.order(h).status, OrderStatus::kCancelled);
  EXPECT_EQ(e.rig.sim().working(), 0U);
  EXPECT_EQ(e.oms.illegal_transitions(), 0U);
}

TEST(SimBrokerEndToEnd, AFillThatArrivesWhileCancelPendingIsRecordedNotDropped) {
  EndToEnd e;
  const oms::OrderHandle h = e.place_open(1, limit(Side::kBuy, 10000));
  e.rig.tick(9995);                 // the broker fills it ...
  EXPECT_TRUE(e.router.cancel(h));  // ... while the OMS, not knowing yet, asks to cancel
  EXPECT_EQ(e.oms.order(h).status, OrderStatus::kCancelPending);
  EXPECT_EQ(apply_events(e.rig.sim(), e.oms), 1U);  // the fill; the cancel's "too late" is a stale race
  const oms::Order& order = e.oms.order(h);
  EXPECT_EQ(order.status, OrderStatus::kFilled);
  EXPECT_EQ(order.filled_qty, 10);
  EXPECT_EQ(e.rig.sim().stats().stale_cancel_rejects, 1U);
  EXPECT_EQ(e.oms.illegal_transitions(), 0U);
  EXPECT_EQ(e.oms.recon_mismatches(), 0U);
}

TEST(SimBrokerEndToEnd, AnIocRemainderIsCancelledInTheOms) {
  EndToEnd e;
  e.rig.quote(10100, 10095, 10105);
  Spec s = limit(Side::kBuy, 10000);
  s.validity = Validity::kIoc;
  const oms::OrderHandle h = e.place(1, s);
  EXPECT_EQ(e.router.poll(), 1U);
  EXPECT_EQ(apply_events(e.rig.sim(), e.oms), 2U);
  EXPECT_EQ(e.oms.order(h).status, OrderStatus::kCancelled);
  EXPECT_EQ(e.sink.events.back().reason_code.view(), "IOC_REMAINDER");
  EXPECT_EQ(e.oms.illegal_transitions(), 0U);
}

TEST(SimBrokerEndToEnd, AMalformedOrderEndsBrokerRejected) {
  EndToEnd e;
  // The OMS already refuses a missing price, qty <= 0 or a missing trigger, so what is left for the sim to
  // reject is a price of zero.
  const Spec s = limit(Side::kBuy, 0);
  const oms::OrderHandle h = e.place(1, s);
  EXPECT_EQ(e.router.poll(), 1U);
  EXPECT_EQ(apply_events(e.rig.sim(), e.oms), 1U);
  EXPECT_EQ(e.oms.order(h).status, OrderStatus::kBrokerRejected);
  EXPECT_EQ(e.sink.events.back().reason_code.view(), "SIM_INVALID");
  EXPECT_EQ(e.oms.illegal_transitions(), 0U);
}

TEST(SimBrokerEndToEnd, AnEventForAnUnknownOrderIsCountedNotApplied) {
  EndToEnd e;
  ASSERT_TRUE(e.rig.send(5, limit(Side::kBuy, 10000)));  // the OMS never heard of it
  EXPECT_EQ(apply_events(e.rig.sim(), e.oms), 0U);
  EXPECT_EQ(e.rig.sim().stats().unknown_orders, 1U);
  EXPECT_EQ(e.rig.sim().events_pending(), 0U);
}

TEST(SimBrokerEndToEnd, AFullSinkLeavesTheFillQueuedForTheNextCall) {
  EndToEnd e;
  const oms::OrderHandle h = e.place_open(1, limit(Side::kBuy, 10000));
  e.rig.tick(9995);
  e.sink.full = true;
  EXPECT_EQ(apply_events(e.rig.sim(), e.oms), 0U);
  EXPECT_EQ(e.rig.sim().events_pending(), 1U);  // not dropped
  EXPECT_EQ(e.rig.sim().stats().apply_retries, 1U);
  EXPECT_EQ(e.oms.order(h).status, OrderStatus::kOpen);
  e.sink.full = false;
  EXPECT_EQ(apply_events(e.rig.sim(), e.oms), 1U);
  EXPECT_EQ(e.oms.order(h).status, OrderStatus::kFilled);
  EXPECT_EQ(e.rig.sim().stats().apply_errors, 0U);
}

TEST(SimBrokerEndToEnd, ACancelBeforeTheAckIsPumpedCancels) {
  EndToEnd e;
  const oms::OrderHandle h = e.place(1, limit(Side::kBuy, 10000));
  EXPECT_EQ(e.router.poll(), 1U);
  EXPECT_EQ(e.oms.order(h).status, OrderStatus::kSent);
  EXPECT_TRUE(e.router.cancel(h));  // SENT: the router records no CANCEL_REQUEST
  EXPECT_EQ(e.oms.order(h).status, OrderStatus::kSent);
  EXPECT_EQ(apply_events(e.rig.sim(), e.oms), 2U);  // ack, then the confirm as kBrokerCancel
  EXPECT_EQ(e.oms.order(h).status, OrderStatus::kCancelled);
  EXPECT_EQ(e.oms.illegal_transitions(), 0U);
  EXPECT_EQ(e.rig.sim().stats().apply_errors, 0U);
  EXPECT_EQ(e.rig.sim().working(), 0U);
}

TEST(SimBrokerEndToEnd, CancelAllBeforeTheAckIsPumpedCancels) {
  EndToEnd e;
  const oms::OrderHandle a = e.place(1, limit(Side::kBuy, 10000));
  const oms::OrderHandle b = e.place(2, limit(Side::kSell, 11000));
  EXPECT_EQ(e.router.poll(), 2U);
  const auto result = e.router.cancel_all();
  EXPECT_EQ(result.asked, 2U);
  EXPECT_EQ(apply_events(e.rig.sim(), e.oms), 4U);
  EXPECT_EQ(e.oms.order(a).status, OrderStatus::kCancelled);
  EXPECT_EQ(e.oms.order(b).status, OrderStatus::kCancelled);
  EXPECT_EQ(e.oms.illegal_transitions(), 0U);
  EXPECT_EQ(e.rig.sim().stats().apply_errors, 0U);
}

TEST(SimBrokerEndToEnd, ASecondCancelAfterTheFirstIsHarmless) {
  EndToEnd e;
  const oms::OrderHandle h = e.place(1, limit(Side::kBuy, 10000));
  EXPECT_EQ(e.router.poll(), 1U);
  EXPECT_TRUE(e.router.cancel(h));
  EXPECT_TRUE(e.router.cancel(h));  // asked again before anything is pumped
  EXPECT_EQ(apply_events(e.rig.sim(), e.oms), 2U);
  EXPECT_EQ(e.oms.order(h).status, OrderStatus::kCancelled);
  EXPECT_EQ(e.rig.sim().stats().stale_cancel_confirms, 1U);
  // And once the OMS has it cancelled, the sim itself still answers a cancel, which the pump ignores.
  EXPECT_TRUE(e.rig.sim().cancel(e.oms.order(h)));
  EXPECT_EQ(apply_events(e.rig.sim(), e.oms), 0U);
  EXPECT_EQ(e.rig.sim().stats().stale_cancel_confirms, 2U);
  EXPECT_EQ(e.oms.illegal_transitions(), 0U);
  EXPECT_EQ(e.rig.sim().stats().apply_errors, 0U);
}

TEST(SimBrokerEndToEnd, ASecondCancelWhileCancelPendingClosesCleanly) {
  EndToEnd e;
  const oms::OrderHandle h = e.place_open(1, limit(Side::kBuy, 10000));
  EXPECT_TRUE(e.router.cancel(h));
  EXPECT_EQ(e.oms.order(h).status, OrderStatus::kCancelPending);
  EXPECT_TRUE(e.router.cancel(h));
  EXPECT_EQ(apply_events(e.rig.sim(), e.oms), 1U);
  EXPECT_EQ(e.oms.order(h).status, OrderStatus::kCancelled);
  EXPECT_EQ(e.oms.illegal_transitions(), 0U);
  EXPECT_EQ(e.rig.sim().stats().apply_errors, 0U);
}

TEST(SimBrokerEndToEnd, AnAckWhileTheOmsStillHasItApprovedWaitsForSent) {
  EndToEnd e;
  e.rig.quote(10005, 10000, 10010);
  const oms::OrderHandle h = e.place(1, limit(Side::kBuy, 10050));  // fills on send: ack, fill
  e.sink.full = true;                                               // record_sent can't be written
  EXPECT_EQ(e.router.poll(), 1U);                                   // the adapter has it
  EXPECT_EQ(e.oms.order(h).status, OrderStatus::kApproved);
  EXPECT_EQ(apply_events(e.rig.sim(), e.oms), 0U);  // held back
  EXPECT_EQ(e.rig.sim().events_pending(), 2U);
  EXPECT_EQ(e.rig.sim().stats().apply_retries, 1U);
  EXPECT_EQ(e.oms.illegal_transitions(), 0U);
  e.sink.full = false;
  EXPECT_EQ(e.router.poll(), 0U);  // records SENT
  EXPECT_EQ(e.oms.order(h).status, OrderStatus::kSent);
  EXPECT_EQ(apply_events(e.rig.sim(), e.oms), 2U);
  EXPECT_EQ(e.oms.order(h).status, OrderStatus::kFilled);
  EXPECT_EQ(e.oms.illegal_transitions(), 0U);
  EXPECT_EQ(e.rig.sim().stats().apply_errors, 0U);
}

TEST(SimBrokerEndToEnd, AnEventUnderAnotherOrderIdIsCountedNotApplied) {
  EndToEnd e;
  const oms::OrderHandle h = e.place(1, limit(Side::kBuy, 10000));
  EXPECT_EQ(e.router.poll(), 1U);
  OrderDetails other = details_of(1, limit(Side::kBuy, 10000));
  other.id.bytes.at(5) = 77;  // same client_order_id, another order id
  const risk::ApprovedOrder approved = testing::approve(other, e.rig.clock);
  ASSERT_TRUE(e.rig.sim().send(approved, "other-tag"));
  EXPECT_EQ(apply_events(e.rig.sim(), e.oms), 1U);  // the real order's ack; the other's is not applied
  EXPECT_EQ(e.rig.sim().stats().mismatched_orders, 1U);
  EXPECT_EQ(e.oms.order(h).status, OrderStatus::kOpen);
  EXPECT_EQ(e.oms.illegal_transitions(), 0U);
}

// ---- properties ----

struct Op {
  enum class Kind : std::uint8_t { kOrder, kTick, kCancel };
  Kind kind{Kind::kTick};
  Spec spec{};
  std::int64_t ltp{10000};
  bool bid{true};
  bool ask{true};
  std::size_t cancel_of{0};
};

struct Row {
  OrderEvent kind{};
  std::string cid;
  std::int32_t fill_qty{0};
  std::int64_t fill_price{0};
  std::string trade;
  std::string broker_id;
  std::string reason;
  bool operator==(const Row&) const = default;
};

struct Run {
  std::vector<Row> rows;
  std::vector<Spec> specs;  // by order number - 1
};

Run run_ops(const std::vector<Op>& ops, std::uint64_t seed, std::uint32_t permille) {
  SimRig r({.max_orders = 256,
            .history = 256,
            .slippage_ticks = 1,
            .touch_fill_permille = permille,
            .seed = seed,
            .event_capacity = 1024});
  Run out;
  const auto collect = [&] {
    for (const SimEvent& e : r.drain()) {
      out.rows.push_back({e.kind, std::string(e.client_order_id.view()), e.fill_qty, e.fill_price.units(),
                          std::string(e.broker_trade_id.view()), std::string(e.broker_order_id.view()),
                          std::string(e.reason_code)});
    }
  };
  int n = 0;
  for (const Op& op : ops) {
    switch (op.kind) {
      case Op::Kind::kOrder:
        out.specs.push_back(op.spec);
        EXPECT_TRUE(r.send(++n, op.spec));
        break;
      case Op::Kind::kTick:
        r.tick(op.ltp, op.bid ? op.ltp - 5 : 0, op.ask ? op.ltp + 5 : 0);
        break;
      case Op::Kind::kCancel:
        if (n > 0) {
          (void)r.sim().cancel(order_for(static_cast<int>(op.cancel_of % static_cast<std::size_t>(n)) + 1));
        }
        break;
    }
    collect();
  }
  return out;
}

Op gen_op() {
  Op op;
  const int pick = *rc::gen::inRange(0, 10);
  op.kind = Op::Kind::kCancel;
  if (pick < 4) {
    op.kind = Op::Kind::kOrder;
  } else if (pick < 9) {
    op.kind = Op::Kind::kTick;
  }
  const auto level = [] { return static_cast<std::int64_t>(*rc::gen::inRange(9900, 10101) / 5 * 5); };
  op.ltp = level();
  op.bid = *rc::gen::arbitrary<bool>();
  op.ask = *rc::gen::arbitrary<bool>();
  op.cancel_of = *rc::gen::inRange<std::size_t>(0, 64);
  op.spec.side = *rc::gen::arbitrary<bool>() ? Side::kBuy : Side::kSell;
  op.spec.type = static_cast<OrderType>(*rc::gen::inRange(0, 4));
  op.spec.validity = *rc::gen::inRange(0, 5) == 0 ? Validity::kIoc : Validity::kDay;
  op.spec.qty = *rc::gen::inRange(0, 51);  // 0 is malformed
  if (*rc::gen::inRange(0, 12) != 0) {
    op.spec.price = P(level());
  }
  if (*rc::gen::inRange(0, 12) != 0) {
    op.spec.trigger = P(level());
  }
  return op;
}

// What every run must satisfy, whatever the capacities: fills respect the order's price and quantity, an
// order is acked or rejected and never both, and ends at most once. A repeated cancel of a cancelled order
// confirms again (idempotent), which is not a second end.
void check_rows(const std::vector<Row>& rows, const std::vector<Spec>& specs) {
  std::map<std::string, int> terminal;
  std::map<std::string, bool> acked;
  std::map<std::string, bool> rejected;
  std::map<std::string, bool> cancelled;
  std::map<std::string, bool> filled;
  for (const Row& row : rows) {
    const int n = std::stoi(row.cid.substr(1));
    const Spec& s = specs.at(static_cast<std::size_t>(n - 1));
    bool ends = false;
    switch (row.kind) {
      case OrderEvent::kBrokerAck:
        RC_ASSERT(!rejected[row.cid]);
        acked[row.cid] = true;
        break;
      case OrderEvent::kBrokerReject:
        RC_ASSERT(!acked[row.cid]);
        rejected[row.cid] = true;
        ends = true;
        break;
      case OrderEvent::kFill: {
        RC_ASSERT(!cancelled[row.cid]);  // no fill after a cancel confirm
        RC_ASSERT(row.fill_qty > 0 && row.fill_qty <= s.qty);
        RC_ASSERT(row.fill_price > 0);
        if (s.price.has_value() && (s.type == OrderType::kLimit || s.type == OrderType::kSl)) {
          const bool buy = s.side == Side::kBuy;
          const bool within = buy ? row.fill_price <= s.price->units() : row.fill_price >= s.price->units();
          RC_ASSERT(within);
        }
        filled[row.cid] = true;
        ends = true;
        break;
      }
      case OrderEvent::kCancelConfirm:
        RC_ASSERT(!filled[row.cid] && !rejected[row.cid]);
        ends = !cancelled[row.cid];
        cancelled[row.cid] = true;
        break;
      case OrderEvent::kBrokerCancel:
        cancelled[row.cid] = true;
        ends = true;
        break;
      default:
        break;
    }
    if (ends) {
      RC_ASSERT(++terminal[row.cid] == 1);  // at most one terminal event
    }
  }
}

RC_GTEST_PROP(SimBrokerProperties, FillsRespectLimitsAndEachOrderEndsOnce, ()) {
  const auto ops = *rc::gen::container<std::vector<Op>>(rc::gen::exec(gen_op));
  const auto seed = *rc::gen::arbitrary<std::uint64_t>();
  const auto permille = static_cast<std::uint32_t>(*rc::gen::inRange(0, 1001));
  const Run run = run_ops(ops, seed, permille);
  check_rows(run.rows, run.specs);
  // The same seed and inputs give the same events.
  RC_ASSERT(run_ops(ops, seed, permille).rows == run.rows);
}

// ---- an independent model of resting orders ----
// Worked out here from the order and the tick alone, in paise, never by calling SimBroker code. At permille 0
// a touch never fills and at 1000 it always does, so the expected fills are exact. One tick is 5 paise and
// the slippage is one tick.

struct MTick {
  std::int64_t ltp{0};
  std::int64_t bid{0};  // 0: no quote on that side
  std::int64_t ask{0};
};

struct MOrder {
  int n{0};
  Side side{Side::kBuy};
  OrderType type{OrderType::kLimit};
  std::int64_t price{0};
  std::int64_t trigger{0};
  bool triggered{false};
  bool alive{true};
};

using Fills = std::vector<std::pair<std::string, std::int64_t>>;  // client_order_id, price units

// Whether the order's market or limit leg is live on this tick: a stop arms once and stays armed.
bool leg_live(MOrder& o, const MTick& t) {
  if (o.type != OrderType::kSl && o.type != OrderType::kSlM) {
    return true;
  }
  const bool buy = o.side == Side::kBuy;
  o.triggered = o.triggered || (buy ? t.ltp >= o.trigger : t.ltp <= o.trigger);
  return o.triggered;
}

std::optional<std::int64_t> model_market(const MOrder& o, const MTick& t) {
  const bool buy = o.side == Side::kBuy;
  std::int64_t base = buy ? t.ask : t.bid;
  if (base <= 0) {
    base = t.ltp;
  }
  const std::int64_t price = buy ? base + 5 : base - 5;
  return price > 0 ? std::optional{price} : std::nullopt;
}

std::optional<std::int64_t> model_limit(const MOrder& o, const MTick& t, bool touch_fills) {
  const bool buy = o.side == Side::kBuy;
  const std::int64_t best = buy ? t.ask : t.bid;
  if (best > 0 && (buy ? best <= o.price : best >= o.price)) {
    return best;
  }
  const bool through = buy ? t.ltp < o.price : t.ltp > o.price;
  if (through || (t.ltp == o.price && touch_fills)) {
    return o.price;
  }
  return std::nullopt;
}

std::optional<std::int64_t> model_price(MOrder& o, const MTick& t, bool touch_fills) {
  if (!leg_live(o, t)) {
    return std::nullopt;
  }
  const bool market_leg = o.type == OrderType::kMarket || o.type == OrderType::kSlM;
  return market_leg ? model_market(o, t) : model_limit(o, t, touch_fills);
}

struct MOp {
  bool is_order{false};
  Side side{Side::kBuy};
  OrderType type{OrderType::kLimit};
  std::int32_t qty{1};
  std::int64_t price{10000};
  std::int64_t trigger{10000};
  MTick tick{};
};

std::int64_t gen_level() { return static_cast<std::int64_t>(*rc::gen::inRange(9950, 10051) / 5 * 5); }

MOp gen_mop() {
  MOp op;
  op.is_order = *rc::gen::inRange(0, 10) < 4;
  op.side = *rc::gen::arbitrary<bool>() ? Side::kBuy : Side::kSell;
  op.type = static_cast<OrderType>(*rc::gen::inRange(0, 4));
  op.qty = *rc::gen::inRange(1, 51);
  op.price = gen_level();
  op.trigger = gen_level();
  op.tick.ltp = gen_level();
  // Quotes may be missing, and may cross: the bid can sit above the ask.
  op.tick.bid = *rc::gen::inRange(0, 4) == 0 ? 0 : gen_level();
  op.tick.ask = *rc::gen::inRange(0, 4) == 0 ? 0 : gen_level();
  return op;
}

class Model {
 public:
  explicit Model(bool touch_fills) : touch_fills_(touch_fills) {}

  // A new order is evaluated once against the last tick, as send does.
  Fills order(int n, const MOp& op) {
    MOrder o{.n = n, .side = op.side, .type = op.type, .price = op.price, .trigger = op.trigger};
    Fills out;
    if (last_.has_value()) {
      collect(o, *last_, out);
    }
    orders_.push_back(o);
    return out;
  }
  // A tick evaluates every resting order, in the order they were sent.
  Fills tick(const MTick& t) {
    last_ = t;
    Fills out;
    for (MOrder& o : orders_) {
      collect(o, t, out);
    }
    return out;
  }

 private:
  void collect(MOrder& o, const MTick& t, Fills& out) const {
    if (!o.alive) {
      return;
    }
    if (const auto price = model_price(o, t, touch_fills_); price.has_value()) {
      o.alive = false;
      out.emplace_back(cid_of(o.n), *price * 100);
    }
  }
  bool touch_fills_;
  std::vector<MOrder> orders_;
  std::optional<MTick> last_;
};

Spec spec_of(const MOp& op) {
  Spec s{.side = op.side, .type = op.type, .qty = op.qty};
  if (op.type == OrderType::kLimit || op.type == OrderType::kSl) {
    s.price = P(op.price);
  }
  if (op.type == OrderType::kSl || op.type == OrderType::kSlM) {
    s.trigger = P(op.trigger);
  }
  return s;
}

Fills fills_of(const std::vector<SimEvent>& events) {
  Fills out;
  for (const SimEvent& e : events) {
    if (e.kind == OrderEvent::kFill) {
      out.emplace_back(std::string(e.client_order_id.view()), e.fill_price.units());
    }
  }
  return out;
}

void check_model(const std::vector<MOp>& ops, bool touch_fills) {
  SimRig r({.max_orders = 256,
            .history = 256,
            .slippage_ticks = 1,
            .touch_fill_permille = touch_fills ? 1000U : 0U,
            .event_capacity = 1024});
  Model model(touch_fills);
  int n = 0;
  std::size_t expected_fills = 0;
  for (const MOp& op : ops) {
    Fills expected;
    if (op.is_order) {
      RC_ASSERT(r.send(++n, spec_of(op)));
      expected = model.order(n, op);
    } else {
      r.tick(op.tick.ltp, op.tick.bid, op.tick.ask);
      expected = model.tick(op.tick);
    }
    RC_ASSERT(fills_of(r.drain()) == expected);
    expected_fills += expected.size();
  }
  RC_ASSERT(r.sim().stats().fills == expected_fills);
}

RC_GTEST_PROP(SimBrokerProperties, MatchesAnIndependentModelOfRestingOrders, ()) {
  const auto ops = *rc::gen::container<std::vector<MOp>>(rc::gen::exec(gen_mop));
  check_model(ops, *rc::gen::arbitrary<bool>());
}

// ---- tiny capacities ----
// The drain is interleaved at random so the queue is often full. A caller retries after draining, as the core
// loop does: every fill is delivered and every order ends at most once.

class TinyDrive {
 public:
  TinyDrive() : rig_({.max_orders = 128, .history = 2, .event_capacity = 4}) {}

  void step(const Op& op) {
    switch (op.kind) {
      case Op::Kind::kOrder:
        order(op);
        break;
      case Op::Kind::kTick:
        tick(op);
        break;
      case Op::Kind::kCancel:
        cancel(op);
        break;
    }
    collect(static_cast<std::size_t>(*rc::gen::inRange(0, 4)));  // a partial drain keeps the queue near full
  }

  void finish() const {
    check_rows(out_.rows, out_.specs);
    std::size_t fills = 0;
    std::map<std::string, bool> acked;
    std::map<std::string, bool> ended;
    for (const Row& row : out_.rows) {
      fills += row.kind == OrderEvent::kFill ? 1U : 0U;
      acked[row.cid] = acked[row.cid] || row.kind == OrderEvent::kBrokerAck;
      ended[row.cid] = ended[row.cid] || row.kind == OrderEvent::kFill ||
                       row.kind == OrderEvent::kBrokerCancel || row.kind == OrderEvent::kCancelConfirm;
    }
    RC_ASSERT(fills == rig_.sim().stats().fills);  // no fill lost
    std::size_t still_working = 0;
    for (const auto& [cid, was_acked] : acked) {
      still_working += (was_acked && !ended[cid]) ? 1U : 0U;
    }
    RC_ASSERT(still_working == rig_.sim().working());
  }

  void collect(std::size_t most) {
    SimEvent e;
    for (std::size_t i = 0; i < most && rig_.sim().pop_event(e); ++i) {
      out_.rows.push_back({e.kind, std::string(e.client_order_id.view()), e.fill_qty, e.fill_price.units(),
                           std::string(e.broker_trade_id.view()), std::string(e.broker_order_id.view()),
                           std::string(e.reason_code)});
    }
  }

 private:
  static constexpr std::size_t kAll = 1'000;

  void order(const Op& op) {
    out_.specs.push_back(op.spec);
    ++n_;
    if (!rig_.send(n_, op.spec)) {  // the queue was full
      collect(kAll);
      RC_ASSERT(rig_.send(n_, op.spec));
    }
  }
  void tick(const Op& op) {
    for (int tries = 0; !rig_.tick(op.ltp, op.bid ? op.ltp - 5 : 0, op.ask ? op.ltp + 5 : 0); ++tries) {
      RC_ASSERT(tries < 200);  // each retry frees the whole queue
      collect(kAll);
    }
  }
  void cancel(const Op& op) {
    if (n_ == 0) {
      return;
    }
    const oms::Order target = order_for(static_cast<int>(op.cancel_of % static_cast<std::size_t>(n_)) + 1);
    if (!rig_.sim().cancel(target)) {
      collect(kAll);
      (void)rig_.sim().cancel(target);  // unknown stays false; a full queue now has room
    }
  }

  SimRig rig_;
  Run out_;
  int n_{0};
};

RC_GTEST_PROP(SimBrokerProperties, ATinyQueueAndHistoryLoseNoFillAndEndOrdersOnce, ()) {
  const auto ops = *rc::gen::container<std::vector<Op>>(rc::gen::exec(gen_op));
  TinyDrive drive;
  for (const Op& op : ops) {
    drive.step(op);
  }
  drive.collect(1'000);
  drive.finish();
}

}  // namespace
}  // namespace vyuha::sim
// NOLINTEND(bugprone-unchecked-optional-access)
