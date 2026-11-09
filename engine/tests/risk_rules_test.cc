// The pre-trade rules (M2.3): R-KILL to R-OPS, one group per rule, plus the two-phase rule that a rejected
// order leaves no state behind.

#include "vyuha/risk/rules.h"

#include <cstddef>
#include <cstdint>
#include <exception>  // rapidcheck's headers use std::exception_ptr without including it
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include "vyuha/core/clock.h"
#include "vyuha/md/last_value.h"
#include "vyuha/oms/freeze_slicer.h"

// NOLINTBEGIN(bugprone-unchecked-optional-access): every test does ASSERT_TRUE(has_value()) first
namespace vyuha::risk {
namespace {

using oms::OrderDetails;
using oms::OrderType;
using oms::Product;
using oms::Side;
using oms::Source;
using oms::Uuid;

// D-23 and D-24 by type. tests/compile_fail.cc checks the same from the outside, with a control.
static_assert(!std::is_constructible_v<Source, Source::Kind>);
static_assert(!std::is_constructible_v<Source, std::uint8_t>);
static_assert(!std::is_constructible_v<oms::SliceIntent, std::uint64_t>);
static_assert(!std::is_default_constructible_v<KillSwitch>);
static_assert(!std::is_copy_constructible_v<KillSwitch>);
static_assert(!std::is_move_constructible_v<KillSwitch>);
static_assert(std::is_default_constructible_v<GateContext>);

constexpr Nanos kDayStart =
    (Nanos{20'766} * Nanos{1'440} * kNanosPerMinute) - md::kIstOffset;  // Mon 2026-11-09 IST
constexpr Nanos at(int hour, int minute) {
  return kDayStart + (Nanos{(hour * 60) + minute} * kNanosPerMinute);
}
constexpr Nanos kNow = at(10, 0);
constexpr std::int64_t kEquity = 1;
constexpr std::int64_t kOption = 2;
constexpr std::int64_t kBseEquity = 3;
constexpr std::int64_t kFromScale = kLimitScale;
constexpr Price rupees(std::int64_t r) { return Price::from_units(r * Price::kUnitsPerRupee); }

constexpr Uuid id_of(std::uint8_t b) {
  Uuid id;
  id.bytes.fill(b);
  return id;
}
constexpr Uuid kAccount = id_of(1);
constexpr Uuid kUser = id_of(2);
constexpr Uuid kStrategy = id_of(3);
constexpr Uuid kOtherAccount = id_of(4);

// A desk where the order from order() passes: every test then breaks one thing.
struct Desk {
  SimClock clock{kNow};
  InstrumentTable instruments{16};
  md::LastValueBook book{16};
  std::unique_ptr<GateContext> owned{std::make_unique<GateContext>()};
  GateContext& ctx{*owned};

  Desk() {
    ctx.instruments = &instruments;
    ctx.book = &book;
    ctx.live_trading_enabled = true;
    ctx.calendar.set_covered_until(at(0, 0) + (Nanos{30} * Nanos{1'440} * kNanosPerMinute));
    EXPECT_TRUE(instruments.put(kEquity, {.lot_size = 1, .tick_size = Price::from_units(500)}));
    EXPECT_TRUE(
        instruments.put(kOption, {.lot_size = 65,
                                  .tick_size = Price::from_units(500),
                                  .exchange = Exchange::kNfo,
                                  .segment = Segment::kOpt,
                                  .expires_at = at(15, 30) + (Nanos{2} * Nanos{1'440} * kNanosPerMinute)}));
    EXPECT_TRUE(instruments.put(
        kBseEquity, {.lot_size = 1, .tick_size = Price::from_units(500), .exchange = Exchange::kBse}));
    tick(kEquity, rupees(100));
    tick(kOption, rupees(100));
    tick(kBseEquity, rupees(100));
    EXPECT_NE(ctx.accounts.put(
                  {.id = kAccount, .session_connected = true, .token_expires_at = at(23, 0), .ops_cap = 8}),
              nullptr);
    EXPECT_TRUE(
        ctx.entitlements.put({.user = kUser, .account = kAccount, .can_trade = true, .can_live = true}));
    limit(LimitKey::kMaxOrderValue, 500'000 * kFromScale);
    limit(LimitKey::kMaxOrderQtyLots, 20 * kFromScale);
    limit(LimitKey::kFatFingerPct, 5 * kFromScale);
    limit(LimitKey::kFatFingerPctOpt, 30 * kFromScale);
    limit(LimitKey::kDupWindowMs, 1'500 * kFromScale);
    limit(LimitKey::kAllowMarketOrders, 1 * kFromScale);
    limit(LimitKey::kAllowMisAfter, 1510 * kFromScale);
    limit(LimitKey::kTradingWindowStart, 915 * kFromScale);
    limit(LimitKey::kTradingWindowEnd, 1530 * kFromScale);
  }

  void limit(LimitKey key, std::int64_t value, LimitScope scope = LimitScope::kDesk, bool hard = true,
             Uuid id = {}, std::int64_t instrument = 0) {
    ASSERT_TRUE(ctx.limits.add(
        key, {.scope = scope, .id = id, .instrument_id = instrument, .value = value, .hard = hard}));
  }
  void clear_limits(LimitKey key) { ctx.limits.counts.at(static_cast<std::size_t>(key)) = 0; }

  void tick(std::int64_t id, Price ltp, Nanos recv = kNow) {
    md::Tick t;
    t.instrument_id = static_cast<md::InstrumentId>(id);
    t.ltp = ltp;
    t.recv_ts = recv;
    t.recv_mono = recv;
    ASSERT_TRUE(book.update(t));
  }

  [[nodiscard]] OrderDetails order() const {
    OrderDetails o{.is_paper = oms::kPaperOrder};
    o.account_id = kAccount;
    o.user_id = kUser;
    o.instrument_id = kEquity;
    o.qty = 10;
    o.price = rupees(100);
    o.created_at = clock.now();
    return o;
  }
  [[nodiscard]] OrderDetails live_order() const {
    OrderDetails o = order();
    o.is_paper = oms::kLiveOrder;
    return o;
  }

  [[nodiscard]] GateDecision run(const OrderDetails& o, std::span<const Rule> rules = pre_trade_rules()) {
    const RiskGate gate = RiskGate::create(rules, clock).value();
    return gate.evaluate(ctx, o);
  }

  // Runs one rule on its own, so a live order doesn't stop at the "incomplete chain" refusal.
  [[nodiscard]] Outcome check(RuleCode code, const OrderDetails& o) const {
    return check_at(code, o, clock.monotonic());
  }
  [[nodiscard]] Outcome check_at(RuleCode code, const OrderDetails& o, Nanos monotonic) const {
    const Rule* found = nullptr;
    for (const Rule& r : pre_trade_rules()) {
      if (r.code == code) {
        found = &r;
      }
    }
    EXPECT_NE(found, nullptr);
    const GateFacts facts{.now = clock.now(),
                          .monotonic = monotonic,
                          .opening = true,
                          .instrument = instruments.find(o.instrument_id),
                          .account = std::as_const(ctx).accounts.find(o.account_id)};
    return found->eval(ctx, o, facts);
  }
  [[nodiscard]] Verdict verdict(RuleCode code, const OrderDetails& o) const { return check(code, o).verdict; }
};

// NOLINTBEGIN(cppcoreguidelines-macro-usage): as macros, a failure reports the test's own line
#define EXPECT_PASS(desk, code, o) EXPECT_EQ((desk).verdict(RuleCode::code, (o)), Verdict::kPass)
#define EXPECT_REJECT(desk, code, o) EXPECT_EQ((desk).verdict(RuleCode::code, (o)), Verdict::kReject)
#define EXPECT_WARN(desk, code, o) EXPECT_EQ((desk).verdict(RuleCode::code, (o)), Verdict::kWarn)
// NOLINTEND(cppcoreguidelines-macro-usage)

TEST(Rules, TheTableIsTheFirstThirteenInOrderAndLeavesTheGateIncomplete) {
  ASSERT_EQ(pre_trade_rules().size(), 13U);
  for (std::size_t i = 0; i < 13; ++i) {
    EXPECT_EQ(std::to_underlying(pre_trade_rules()[i].code), i + 1);
  }
  const SimClock clock{1};
  EXPECT_FALSE(RiskGate::create(pre_trade_rules(), clock).value().complete());
}

TEST(Rules, AWellFormedOrderPassesTheWholeChain) {
  Desk d;
  const GateDecision out = d.run(d.order());
  EXPECT_FALSE(out.rejection.has_value());
  EXPECT_TRUE(out.approved.has_value());
  EXPECT_EQ(out.warning_count, 0U);
}

TEST(Rules, ALiveOrderIsStillRefusedUntilTheChainIsComplete) {
  Desk d;
  const GateDecision out = d.run(d.live_order());
  ASSERT_TRUE(out.rejection.has_value());
  EXPECT_EQ(out.rejection.value().code, RuleCode::kLiveDisabled);
}

// ---- R-KILL ----

TEST(RKill, EachScopeBlocksItsOrders) {
  Desk d;
  OrderDetails o = d.order();
  o.strategy_id = kStrategy;
  EXPECT_PASS(d, kKill, o);
  for (const auto& [scope, id] :
       {std::pair{KillSwitch::Scope::kDesk, Uuid{}}, std::pair{KillSwitch::Scope::kAccount, kAccount},
        std::pair{KillSwitch::Scope::kTrader, kUser}, std::pair{KillSwitch::Scope::kStrategy, kStrategy}}) {
    d.ctx.kill.block(scope, id);
    EXPECT_REJECT(d, kKill, o);
    d.ctx.kill.release(scope, id);
    EXPECT_PASS(d, kKill, o);
  }
}

TEST(RKill, AnotherScopesKillDoesNotBlock) {
  Desk d;
  d.ctx.kill.block(KillSwitch::Scope::kAccount, kOtherAccount);
  EXPECT_PASS(d, kKill, d.order());
}

TEST(RKill, OnlyAnOrderMarkedByTheKillSwitchIsExempt) {
  Desk d;
  d.ctx.kill.block(KillSwitch::Scope::kAccount, kAccount);
  const std::optional<OrderDetails> flatten = d.ctx.kill.kill_generated(d.order());
  ASSERT_TRUE(flatten.has_value());
  EXPECT_TRUE(flatten.value().source.kill_generated());
  EXPECT_PASS(d, kKill, flatten.value());
  for (const Source source : {Source::kManual, Source::kStrategy, Source::kAiDraft, Source::kSystem}) {
    OrderDetails o = d.order();
    o.source = source;
    EXPECT_REJECT(d, kKill, o);
  }
}

TEST(RKill, TheKillMarkCantBeHadWhileNothingCoversTheOrder) {
  Desk d;
  EXPECT_FALSE(d.ctx.kill.kill_generated(d.order()).has_value());
  d.ctx.kill.block(KillSwitch::Scope::kAccount, kOtherAccount);
  EXPECT_FALSE(d.ctx.kill.kill_generated(d.order()).has_value());
}

TEST(RKill, AFullKillListBlocksTheWholeDesk) {
  Desk d;
  for (std::size_t i = 0; i < KillSwitch::kMaxPerScope; ++i) {
    Uuid id;
    id.bytes.at(0) = static_cast<std::uint8_t>(100 + (i % 100));
    id.bytes.at(1) = static_cast<std::uint8_t>(i);
    ASSERT_TRUE(d.ctx.kill.block(KillSwitch::Scope::kStrategy, id));
  }
  EXPECT_PASS(d, kKill, d.order());
  EXPECT_FALSE(d.ctx.kill.block(KillSwitch::Scope::kStrategy, kStrategy));
  EXPECT_REJECT(d, kKill, d.order());
}

// ---- R-LIVE-DISABLED ----

TEST(RLiveDisabled, PaperPassesEvenWithLiveOff) {
  Desk d;
  d.ctx.live_trading_enabled = false;
  EXPECT_PASS(d, kLiveDisabled, d.order());
}

TEST(RLiveDisabled, LiveOffRefusesALiveOrder) {
  Desk d;
  EXPECT_PASS(d, kLiveDisabled, d.live_order());
  d.ctx.live_trading_enabled = false;
  EXPECT_REJECT(d, kLiveDisabled, d.live_order());
}

TEST(RLiveDisabled, ALiveOrderFromAStrategyNotLiveIsRefused) {
  Desk d;
  OrderDetails o = d.live_order();
  o.source = Source::kStrategy;
  o.strategy_id = kStrategy;
  EXPECT_REJECT(d, kLiveDisabled, o);
  ASSERT_TRUE(d.ctx.live_strategies.put(kStrategy));
  EXPECT_PASS(d, kLiveDisabled, o);
}

TEST(RLiveDisabled, AStrategyTakenOffLiveIsRefusedAgain) {
  Desk d;
  OrderDetails o = d.live_order();
  o.strategy_id = kStrategy;
  ASSERT_TRUE(d.ctx.live_strategies.put(kStrategy));
  EXPECT_PASS(d, kLiveDisabled, o);
  d.ctx.live_strategies.remove(kStrategy);
  EXPECT_REJECT(d, kLiveDisabled, o);
}

TEST(RLiveDisabled, AStrategyOrderNamingNoStrategyIsRefused) {
  const Desk d;
  OrderDetails o = d.live_order();
  o.source = Source::kStrategy;
  EXPECT_REJECT(d, kLiveDisabled, o);
}

TEST(RLiveDisabled, TheSettingMustBeExactlyTrueInAProperEnvironment) {
  EXPECT_TRUE(live_trading_allowed("true", "prod"));
  EXPECT_TRUE(live_trading_allowed("true", "staging"));
  EXPECT_FALSE(live_trading_allowed("true", "dev"));
  EXPECT_FALSE(live_trading_allowed("true", ""));
  EXPECT_FALSE(live_trading_allowed("TRUE", "prod"));
  EXPECT_FALSE(live_trading_allowed("1", "prod"));
  EXPECT_FALSE(live_trading_allowed("", "prod"));
  EXPECT_FALSE(live_trading_allowed("false", "prod"));
}

// ---- R-ENTITLE ----

TEST(REntitle, AUserNotMappedToTheAccountIsRefused) {
  const Desk d;
  OrderDetails o = d.order();
  o.user_id = id_of(9);
  EXPECT_REJECT(d, kEntitle, o);
}

TEST(REntitle, CanTradeIsRequiredEvenForPaper) {
  Desk d;
  OrderDetails o = d.order();
  o.user_id = id_of(9);
  ASSERT_TRUE(
      d.ctx.entitlements.put({.user = o.user_id, .account = kAccount, .can_trade = false, .can_live = true}));
  EXPECT_REJECT(d, kEntitle, o);
}

TEST(REntitle, ARoleThatCantGoLiveMayStillPaperTrade) {
  Desk d;
  OrderDetails o = d.order();
  o.user_id = id_of(9);
  ASSERT_TRUE(
      d.ctx.entitlements.put({.user = o.user_id, .account = kAccount, .can_trade = true, .can_live = false}));
  EXPECT_PASS(d, kEntitle, o);
  o.is_paper = oms::kLiveOrder;
  EXPECT_REJECT(d, kEntitle, o);
}

TEST(REntitle, ARevocationReplacesTheEarlierRow) {
  Desk d;
  EXPECT_PASS(d, kEntitle, d.order());
  ASSERT_TRUE(d.ctx.entitlements.put({.user = kUser, .account = kAccount, .can_trade = false}));
  EXPECT_REJECT(d, kEntitle, d.order());
}

// ---- R-SESSION ----

TEST(RSession, PaperNeedsNoSession) {
  Desk d;
  ASSERT_NE(d.ctx.accounts.put({.id = kAccount, .session_connected = false}), nullptr);
  EXPECT_PASS(d, kSession, d.order());
}

TEST(RSession, ALiveOrderNeedsAConnectedSessionAndAValidToken) {
  Desk d;
  EXPECT_PASS(d, kSession, d.live_order());
  ASSERT_NE(d.ctx.accounts.put({.id = kAccount, .session_connected = false, .token_expires_at = at(23, 0)}),
            nullptr);
  EXPECT_REJECT(d, kSession, d.live_order());
  ASSERT_NE(d.ctx.accounts.put({.id = kAccount, .session_connected = true, .token_expires_at = kNow}),
            nullptr);
  EXPECT_REJECT(d, kSession, d.live_order());
}

TEST(RSession, AnAccountNotLoadedIsRefused) {
  const Desk d;
  OrderDetails o = d.live_order();
  o.account_id = kOtherAccount;
  EXPECT_REJECT(d, kSession, o);
}

// ---- R-MKT-HOURS ----

TEST(RMktHours, TheSessionEdges) {
  Desk d;
  const OrderDetails o = [&] {
    OrderDetails x = d.order();
    x.product = Product::kCnc;
    return x;
  }();
  d.clock.set(at(9, 14));
  EXPECT_REJECT(d, kMktHours, o);
  d.clock.set(at(9, 15));
  EXPECT_PASS(d, kMktHours, o);
  d.clock.set(at(15, 29));
  EXPECT_PASS(d, kMktHours, o);
  d.clock.set(at(15, 30));
  EXPECT_REJECT(d, kMktHours, o);
}

TEST(RMktHours, AWeekendIsClosed) {
  Desk d;
  d.clock.set(kNow + (Nanos{5} * Nanos{1'440} * kNanosPerMinute));  // Saturday
  EXPECT_REJECT(d, kMktHours, d.order());
  d.clock.set(kNow + (Nanos{6} * Nanos{1'440} * kNanosPerMinute));  // Sunday
  EXPECT_REJECT(d, kMktHours, d.order());
}

TEST(RMktHours, AHolidayIsClosedForThatExchangeOnly) {
  Desk d;
  ASSERT_TRUE(d.ctx.calendar.add({.exchange = Exchange::kNse, .day_start = at(0, 0)}));
  EXPECT_REJECT(d, kMktHours, d.order());
  OrderDetails bse = d.order();
  bse.instrument_id = kBseEquity;
  EXPECT_PASS(d, kMktHours, bse);
}

TEST(RMktHours, ASpecialSessionIsOpenForItsOwnHoursOnly) {
  Desk d;
  ASSERT_TRUE(d.ctx.calendar.add(
      {.exchange = Exchange::kNse, .day_start = at(0, 0), .open_minute = 18 * 60, .close_minute = 19 * 60}));
  OrderDetails o = d.order();
  o.product = Product::kCnc;
  EXPECT_REJECT(d, kMktHours, o);
  d.clock.set(at(18, 30));
  EXPECT_PASS(d, kMktHours, o);  // outside the desk's regular window, inside the special session
}

TEST(RMktHours, BeyondTheCalendarIsRefused) {
  Desk d;
  d.ctx.calendar.set_covered_until(at(0, 0) - 1);
  EXPECT_REJECT(d, kMktHours, d.order());
}

TEST(RMktHours, ACalendarNeverLoadedIsRefused) {
  Desk d;
  d.ctx.calendar = {};
  EXPECT_REJECT(d, kMktHours, d.order());
}

TEST(RMktHours, TheDeskWindowNarrowsTheSession) {
  Desk d;
  d.limit(LimitKey::kTradingWindowStart, 1000 * kFromScale, LimitScope::kAccount, true, kAccount);
  d.clock.set(at(9, 59));
  EXPECT_REJECT(d, kMktHours, d.order());
  d.clock.set(at(10, 0));
  EXPECT_PASS(d, kMktHours, d.order());
}

TEST(RMktHours, ASoftWindowWarns) {
  Desk d;
  d.limit(LimitKey::kTradingWindowEnd, 930 * kFromScale, LimitScope::kDesk, false);
  EXPECT_WARN(d, kMktHours, d.order());
}

TEST(RMktHours, ASoftWarningDoesNotHideALaterHardCheck) {
  Desk d;
  d.limit(LimitKey::kTradingWindowEnd, 1500 * kFromScale, LimitScope::kDesk, false);
  d.clock.set(at(15, 20));
  OrderDetails mis = d.order();
  mis.product = Product::kMis;
  EXPECT_REJECT(d, kMktHours, mis);  // past the soft window end and the hard MIS cutoff
  OrderDetails cnc = d.order();
  cnc.product = Product::kCnc;
  EXPECT_WARN(d, kMktHours, cnc);
}

TEST(RMktHours, ASoftStartDoesNotHideAHardEnd) {
  Desk d;
  d.limit(LimitKey::kTradingWindowStart, 1000 * kFromScale, LimitScope::kDesk, false);
  d.limit(LimitKey::kTradingWindowEnd, 930 * kFromScale);
  d.clock.set(at(9, 45));
  EXPECT_REJECT(d, kMktHours, d.order());
}

TEST(RMktHours, NoNewMisAfterTheCutoffButCncStillGoes) {
  Desk d;
  d.clock.set(at(15, 10));
  OrderDetails mis = d.order();
  mis.product = Product::kMis;
  EXPECT_REJECT(d, kMktHours, mis);
  OrderDetails cnc = d.order();
  cnc.product = Product::kCnc;
  EXPECT_PASS(d, kMktHours, cnc);
  d.clock.set(at(15, 9));
  EXPECT_PASS(d, kMktHours, mis);
}

TEST(RMktHours, AnImpossibleTimeInALimitIsRefused) {
  Desk d;
  d.clear_limits(LimitKey::kAllowMisAfter);
  d.limit(LimitKey::kAllowMisAfter, 1575 * kFromScale);  // 15:75
  EXPECT_REJECT(d, kMktHours, d.order());
}

TEST(RMktHours, AMissingWindowLimitIsRefused) {
  Desk d;
  d.clear_limits(LimitKey::kTradingWindowEnd);
  EXPECT_REJECT(d, kMktHours, d.order());
}

// ---- R-INSTR-BAN, R-INSTR-RESTRICTED, R-INSTR-EXPIRED ----

TEST(RInstr, ABannedInstrumentIsRefused) {
  Desk d;
  EXPECT_PASS(d, kInstrBan, d.order());
  ASSERT_TRUE(
      d.instruments.put(kEquity, {.lot_size = 1, .tick_size = Price::from_units(500), .fo_ban = true}));
  EXPECT_REJECT(d, kInstrBan, d.order());
}

TEST(RInstr, ARestrictedInstrumentIsRefused) {
  Desk d;
  EXPECT_PASS(d, kInstrRestricted, d.order());
  ASSERT_TRUE(
      d.instruments.put(kEquity, {.lot_size = 1, .tick_size = Price::from_units(500), .restricted = true}));
  EXPECT_REJECT(d, kInstrRestricted, d.order());
}

TEST(RInstr, AnUnknownInstrumentIsRefusedHere) {
  Desk d;
  OrderDetails o = d.order();
  o.instrument_id = 999;
  EXPECT_REJECT(d, kInstrExpired, o);
  o.instrument_id = 0;
  EXPECT_REJECT(d, kInstrExpired, o);
  o.instrument_id = -1;
  EXPECT_REJECT(d, kInstrExpired, o);
  o.instrument_id = 999;
  const GateDecision out = d.run(o);
  ASSERT_TRUE(out.rejection.has_value());
  EXPECT_EQ(out.rejection.value().code, RuleCode::kInstrExpired);
}

TEST(RInstr, ExpiryIsAtTheClose) {
  Desk d;
  OrderDetails o = d.order();
  o.instrument_id = kOption;
  o.qty = 65;
  const Nanos close = at(15, 30) + (Nanos{2} * Nanos{1'440} * kNanosPerMinute);
  d.clock.set(close - 1);
  EXPECT_PASS(d, kInstrExpired, o);
  d.clock.set(close);
  EXPECT_REJECT(d, kInstrExpired, o);
}

TEST(RInstr, AMasterRowThatCantBeCheckedAgainstIsRefused) {
  Desk d;
  ASSERT_TRUE(d.instruments.put(kEquity, {.lot_size = 0, .tick_size = Price::from_units(500)}));
  EXPECT_REJECT(d, kInstrExpired, d.order());
  ASSERT_TRUE(d.instruments.put(kEquity, {.lot_size = 1, .tick_size = Price{}}));
  EXPECT_REJECT(d, kInstrExpired, d.order());
}

// ---- R-ORDER-SHAPE ----

TEST(ROrderShape, QtyMustBeAPositiveMultipleOfTheLot) {
  const Desk d;
  OrderDetails o = d.order();
  o.instrument_id = kOption;
  o.qty = 130;
  EXPECT_PASS(d, kOrderShape, o);
  o.qty = 100;
  EXPECT_REJECT(d, kOrderShape, o);
  o.qty = 0;
  EXPECT_REJECT(d, kOrderShape, o);
  o.qty = -65;
  EXPECT_REJECT(d, kOrderShape, o);
}

TEST(ROrderShape, PriceAndTriggerMustBeOnTheTickGrid) {
  const Desk d;
  OrderDetails o = d.order();
  o.price = Price::from_units(1'000'500);
  EXPECT_PASS(d, kOrderShape, o);
  o.price = Price::from_units(1'000'501);
  EXPECT_REJECT(d, kOrderShape, o);
  o.price = Price{};
  EXPECT_REJECT(d, kOrderShape, o);
  o.price = Price::from_units(-500);
  EXPECT_REJECT(d, kOrderShape, o);
  o = d.order();
  o.order_type = OrderType::kSl;
  o.trigger_price = Price::from_units(1'000'001);
  EXPECT_REJECT(d, kOrderShape, o);
}

struct ShapeCase {
  OrderType type;
  bool price;
  bool trigger;
  Verdict expected;
};

TEST(ROrderShape, RequiredFieldsPerType) {
  for (const ShapeCase c : {ShapeCase{OrderType::kLimit, true, false, Verdict::kPass},
                            ShapeCase{OrderType::kLimit, false, false, Verdict::kReject},
                            ShapeCase{OrderType::kLimit, true, true, Verdict::kReject},
                            ShapeCase{OrderType::kMarket, false, false, Verdict::kPass},
                            ShapeCase{OrderType::kMarket, true, false, Verdict::kReject},
                            ShapeCase{OrderType::kMarket, false, true, Verdict::kReject},
                            ShapeCase{OrderType::kSl, true, true, Verdict::kPass},
                            ShapeCase{OrderType::kSl, true, false, Verdict::kReject},
                            ShapeCase{OrderType::kSl, false, true, Verdict::kReject},
                            ShapeCase{OrderType::kSlM, false, true, Verdict::kPass},
                            ShapeCase{OrderType::kSlM, true, true, Verdict::kReject},
                            ShapeCase{OrderType::kSlM, false, false, Verdict::kReject}}) {
    const Desk d;
    OrderDetails o = d.order();
    o.order_type = c.type;
    o.price = c.price ? std::optional{rupees(100)} : std::nullopt;
    o.trigger_price = c.trigger ? std::optional{rupees(99)} : std::nullopt;
    EXPECT_EQ(d.verdict(RuleCode::kOrderShape, o), c.expected) << static_cast<int>(c.type);
  }
}

TEST(ROrderShape, ABuyStopLimitNeedsALimitAtOrAboveItsTrigger) {
  const Desk d;
  OrderDetails o = d.order();
  o.order_type = OrderType::kSl;
  o.trigger_price = rupees(101);
  o.price = rupees(101);
  EXPECT_PASS(d, kOrderShape, o);
  o.price = rupees(102);
  EXPECT_PASS(d, kOrderShape, o);
  o.price = Price::from_units(1'009'500);
  EXPECT_REJECT(d, kOrderShape, o);
}

TEST(ROrderShape, ASellStopLimitNeedsALimitAtOrBelowItsTrigger) {
  const Desk d;
  OrderDetails o = d.order();
  o.side = oms::Side::kSell;
  o.order_type = OrderType::kSl;
  o.trigger_price = rupees(99);
  o.price = rupees(99);
  EXPECT_PASS(d, kOrderShape, o);
  o.price = rupees(98);
  EXPECT_PASS(d, kOrderShape, o);
  o.price = Price::from_units(990'500);
  EXPECT_REJECT(d, kOrderShape, o);
}

TEST(ROrderShape, AStopLimitRefusalReportsTheTriggerAndThePrice) {
  const Desk d;
  OrderDetails o = d.order();
  o.order_type = OrderType::kSl;
  o.trigger_price = rupees(101);
  o.price = rupees(100);
  const Outcome out = d.check(RuleCode::kOrderShape, o);
  EXPECT_EQ(out.limit, rupees(101).units());
  EXPECT_EQ(out.current, rupees(100).units());
}

TEST(ROrderShape, TheTickGridIsCheckedBeforeTheStopLimitRule) {
  const Desk d;
  OrderDetails o = d.order();
  o.order_type = OrderType::kSl;
  o.trigger_price = rupees(101);
  o.price = Price::from_units(1'000'300);  // off the grid and short of the trigger
  const Outcome out = d.check(RuleCode::kOrderShape, o);
  EXPECT_EQ(out.verdict, Verdict::kReject);
  EXPECT_EQ(out.limit, 500);
  EXPECT_EQ(out.current, 1'000'300);
}

TEST(ROrderShape, MarketOrdersCanBeTurnedOff) {
  Desk d;
  OrderDetails o = d.order();
  o.order_type = OrderType::kMarket;
  o.price.reset();
  EXPECT_PASS(d, kOrderShape, o);
  d.limit(LimitKey::kAllowMarketOrders, 0, LimitScope::kAccount, true, kAccount);
  EXPECT_REJECT(d, kOrderShape, o);
}

TEST(ROrderShape, ALimitOrderIsntAffectedByTheMarketSetting) {
  Desk d;
  d.limit(LimitKey::kAllowMarketOrders, 0);
  EXPECT_PASS(d, kOrderShape, d.order());
}

// ---- R-FAT-FINGER ----

TEST(RFatFinger, TheEdgesOfThePercent) {
  const Desk d;
  OrderDetails o = d.order();
  o.price = rupees(105);
  EXPECT_PASS(d, kFatFinger, o);
  o.price = rupees(95);
  EXPECT_PASS(d, kFatFinger, o);
  o.price = Price::from_units(1'050'500);
  EXPECT_REJECT(d, kFatFinger, o);
  o.price = Price::from_units(949'500);
  EXPECT_REJECT(d, kFatFinger, o);
}

TEST(RFatFinger, OptionsUseTheirOwnPercent) {
  const Desk d;
  OrderDetails o = d.order();
  o.instrument_id = kOption;
  o.qty = 65;
  o.price = rupees(130);
  EXPECT_PASS(d, kFatFinger, o);
  o.price = rupees(131);
  EXPECT_REJECT(d, kFatFinger, o);
}

TEST(RFatFinger, TheStrictestApplicableScopeWins) {
  Desk d;
  d.limit(LimitKey::kFatFingerPct, 2 * kFromScale, LimitScope::kInstrument, true, {}, kEquity);
  OrderDetails o = d.order();
  o.price = rupees(103);
  EXPECT_REJECT(d, kFatFinger, o);
  o.instrument_id = kBseEquity;  // the override is for the other instrument
  EXPECT_PASS(d, kFatFinger, o);
}

TEST(RFatFinger, ALooserScopeCantWidenTheDesksLimit) {
  Desk d;
  d.limit(LimitKey::kFatFingerPct, 50 * kFromScale, LimitScope::kAccount, true, kAccount);
  OrderDetails o = d.order();
  o.price = rupees(110);
  EXPECT_REJECT(d, kFatFinger, o);
}

TEST(RFatFinger, ASoftLimitWarns) {
  Desk d;
  d.limit(LimitKey::kFatFingerPct, 2 * kFromScale, LimitScope::kDesk, false);
  OrderDetails o = d.order();
  o.price = rupees(103);
  EXPECT_WARN(d, kFatFinger, o);
}

TEST(RFatFinger, NoLtpRefusesAPricedOrder) {
  Desk d;
  OrderDetails o = d.order();
  o.instrument_id = 7;
  ASSERT_TRUE(d.instruments.put(7, {.lot_size = 1, .tick_size = Price::from_units(500)}));
  EXPECT_REJECT(d, kFatFinger, o);
  d.tick(7, Price{});  // a tick with no trade
  EXPECT_REJECT(d, kFatFinger, o);
}

TEST(RFatFinger, AnLtpOlderThanThreeSecondsIsStale) {
  Desk d;
  d.tick(kEquity, rupees(100), kNow - (3 * kNanosPerSecond));
  EXPECT_PASS(d, kFatFinger, d.order());
  d.tick(kEquity, rupees(100), kNow - (3 * kNanosPerSecond) - 1);
  EXPECT_REJECT(d, kFatFinger, d.order());
}

TEST(RFatFinger, AnLtpFarInTheFutureIsStaleToo) {
  Desk d;
  d.tick(kEquity, rupees(100), kNow + (3 * kNanosPerSecond) + 1);
  EXPECT_REJECT(d, kFatFinger, d.order());
}

// D-26: an LTP's age comes from the monotonic receive stamp, whatever the wall clock does.
TEST(RFatFinger, AWallClockStepInEitherDirectionDoesntChangeAnLtpsAge) {
  for (const Nanos step : {Nanos{-10} * kNanosPerSecond, Nanos{10} * kNanosPerSecond}) {
    Desk d;
    d.clock.step_wall(step);
    EXPECT_PASS(d, kFatFinger, d.order());
    d.clock.advance(kLtpMaxAge + 1);
    d.clock.step_wall(step);
    EXPECT_REJECT(d, kFatFinger, d.order());
  }
}

TEST(RFatFinger, ATickWithNoReceiveStampIsStaleOnAClockNearZero) {
  Desk d;
  d.tick(kEquity, rupees(100), 0);
  EXPECT_EQ(d.check_at(RuleCode::kFatFinger, d.order(), 1).verdict, Verdict::kReject);
}

TEST(RFatFinger, ATickAheadOfTheGatesReadingIsRefused) {
  Desk d;
  d.tick(kEquity, rupees(100), kNow + 1);
  EXPECT_REJECT(d, kFatFinger, d.order());
}

TEST(RFatFinger, ASuspendMakesAnLtpStale) {  // D-34
  Desk d;
  EXPECT_PASS(d, kFatFinger, d.order());
  d.clock.suspend(kLtpMaxAge + 1);
  EXPECT_REJECT(d, kFatFinger, d.order());
}

TEST(RFatFinger, TheWallClockReceiveTimeIsNotUsed) {
  Desk d;
  md::Tick t;
  t.instrument_id = static_cast<md::InstrumentId>(kEquity);
  t.ltp = rupees(100);
  t.recv_ts = d.clock.now();                           // says fresh
  t.recv_mono = d.clock.monotonic() - kLtpMaxAge - 1;  // says stale
  ASSERT_TRUE(d.book.update(t));
  EXPECT_REJECT(d, kFatFinger, d.order());
}

TEST(RFatFinger, TheExchangeTimeIsNotUsed) {
  Desk d;
  md::Tick t;
  t.instrument_id = static_cast<md::InstrumentId>(kEquity);
  t.ltp = rupees(100);
  t.exchange_ts = d.clock.now() - (Nanos{60} * kNanosPerSecond);  // an old exchange time
  t.recv_ts = d.clock.now();
  t.recv_mono = d.clock.monotonic();
  ASSERT_TRUE(d.book.update(t));
  EXPECT_PASS(d, kFatFinger, d.order());
}

TEST(RFatFinger, AGateWithNoBookRefuses) {
  Desk d;
  d.ctx.book = nullptr;
  EXPECT_REJECT(d, kFatFinger, d.order());
}

TEST(RFatFinger, AMarketOrderHasNoPriceToCheck) {
  Desk d;
  d.ctx.book = nullptr;
  OrderDetails o = d.order();
  o.order_type = OrderType::kMarket;
  o.price.reset();
  EXPECT_PASS(d, kFatFinger, o);
}

// A stop-market order on the equity, whose LTP is 100 and fat-finger percent 5.
[[nodiscard]] OrderDetails stop_market(const Desk& d, oms::Side side, Price trigger) {
  OrderDetails o = d.order();
  o.side = side;
  o.order_type = OrderType::kSlM;
  o.price.reset();
  o.trigger_price = trigger;
  return o;
}

TEST(RTriggerSide, ABuyStopMustTriggerAboveTheLtp) {
  const Desk d;
  EXPECT_PASS(d, kFatFinger, stop_market(d, oms::Side::kBuy, rupees(101)));
  for (const Price wrong : {rupees(100), rupees(99)}) {
    const Outcome out = d.check(RuleCode::kFatFinger, stop_market(d, oms::Side::kBuy, wrong));
    EXPECT_EQ(out.verdict, Verdict::kReject);
    EXPECT_EQ(out.reason, RuleReason::kTriggerSide);
  }
}

TEST(RTriggerSide, ASellStopMustTriggerBelowTheLtp) {
  const Desk d;
  EXPECT_PASS(d, kFatFinger, stop_market(d, oms::Side::kSell, rupees(99)));
  for (const Price wrong : {rupees(100), rupees(101)}) {
    const Outcome out = d.check(RuleCode::kFatFinger, stop_market(d, oms::Side::kSell, wrong));
    EXPECT_EQ(out.verdict, Verdict::kReject);
    EXPECT_EQ(out.reason, RuleReason::kTriggerSide);
  }
}

TEST(RTriggerSide, AStopLimitIsCheckedToo) {
  const Desk d;
  OrderDetails o = stop_market(d, oms::Side::kBuy, rupees(99));
  o.order_type = OrderType::kSl;
  o.price = rupees(102);
  const Outcome out = d.check(RuleCode::kFatFinger, o);
  EXPECT_EQ(out.verdict, Verdict::kReject);
  EXPECT_EQ(out.reason, RuleReason::kTriggerSide);
  o.trigger_price = rupees(101);
  EXPECT_PASS(d, kFatFinger, o);
}

TEST(RTriggerSide, TheOrderIsRefusedWithItsOwnReason) {
  Desk d;
  const GateDecision out = d.run(stop_market(d, oms::Side::kBuy, rupees(99)));
  ASSERT_TRUE(out.rejection.has_value());
  EXPECT_EQ(out.rejection.value().code, RuleCode::kFatFinger);
  EXPECT_EQ(reason_text(out.rejection.value()), "R-TRIGGER-SIDE");
}

TEST(RTriggerSide, AnOrdinaryFatFingerKeepsItsReason) {
  Desk d;
  OrderDetails o = d.order();
  o.price = rupees(120);
  const GateDecision out = d.run(o);
  ASSERT_TRUE(out.rejection.has_value());
  EXPECT_EQ(reason_text(out.rejection.value()), "R-FAT-FINGER");
}

// The equity with a known band, inside which the 5% fat-finger band reaches further.
void set_band(Desk& d, Price lower, Price upper) {
  ASSERT_TRUE(d.instruments.put(
      kEquity,
      {.lot_size = 1, .tick_size = Price::from_units(500), .lower_circuit = lower, .upper_circuit = upper}));
}

TEST(RTriggerBand, ABuyStopAboveTheUpperCircuitIsRefused) {
  Desk d;
  set_band(d, rupees(98), rupees(102));
  const Outcome out = d.check(RuleCode::kFatFinger, stop_market(d, oms::Side::kBuy, rupees(104)));
  EXPECT_EQ(out.verdict, Verdict::kReject);
  EXPECT_EQ(out.reason, RuleReason::kTriggerBand);
  EXPECT_EQ(out.limit, rupees(102).units());
  EXPECT_EQ(out.current, rupees(104).units());
  EXPECT_PASS(d, kFatFinger, stop_market(d, oms::Side::kBuy, rupees(102)));  // at the limit is inside
}

TEST(RTriggerBand, ASellStopBelowTheLowerCircuitIsRefused) {
  Desk d;
  set_band(d, rupees(98), rupees(102));
  const Outcome out = d.check(RuleCode::kFatFinger, stop_market(d, oms::Side::kSell, rupees(96)));
  EXPECT_EQ(out.verdict, Verdict::kReject);
  EXPECT_EQ(out.reason, RuleReason::kTriggerBand);
  EXPECT_EQ(out.limit, rupees(98).units());
  EXPECT_EQ(out.current, rupees(96).units());
  EXPECT_PASS(d, kFatFinger, stop_market(d, oms::Side::kSell, rupees(98)));
}

TEST(RTriggerBand, AStopLimitIsCheckedToo) {
  Desk d;
  set_band(d, rupees(98), rupees(102));
  OrderDetails o = stop_market(d, oms::Side::kBuy, rupees(103));
  o.order_type = OrderType::kSl;
  o.price = rupees(102);
  const Outcome out = d.check(RuleCode::kFatFinger, o);
  EXPECT_EQ(out.verdict, Verdict::kReject);
  EXPECT_EQ(out.reason, RuleReason::kTriggerBand);
}

TEST(RTriggerBand, TheWrongSideIsReportedFirst) {
  Desk d;
  set_band(d, rupees(98), rupees(102));
  const Outcome out = d.check(RuleCode::kFatFinger, stop_market(d, oms::Side::kBuy, rupees(96)));
  EXPECT_EQ(out.reason, RuleReason::kTriggerSide);
}

TEST(RTriggerBand, OutsideBothBandsIsReportedAsTheCircuit) {
  Desk d;
  set_band(d, rupees(98), rupees(102));
  const Outcome out = d.check(RuleCode::kFatFinger, stop_market(d, oms::Side::kBuy, rupees(110)));
  EXPECT_EQ(out.reason, RuleReason::kTriggerBand);
}

TEST(RTriggerBand, OneBoundIsNoBand) {
  Desk d;
  ASSERT_TRUE(d.instruments.put(
      kEquity, {.lot_size = 1, .tick_size = Price::from_units(500), .upper_circuit = rupees(102)}));
  OrderDetails buy = stop_market(d, oms::Side::kBuy, rupees(104));
  EXPECT_PASS(d, kFatFinger, buy);
  buy.is_paper = oms::kLiveOrder;
  const Outcome live = d.check(RuleCode::kFatFinger, buy);
  EXPECT_EQ(live.verdict, Verdict::kReject);
  EXPECT_EQ(live.reason, RuleReason::kRule);
  ASSERT_TRUE(d.instruments.put(
      kEquity, {.lot_size = 1, .tick_size = Price::from_units(500), .lower_circuit = rupees(98)}));
  EXPECT_PASS(d, kFatFinger, stop_market(d, oms::Side::kSell, rupees(96)));
}

TEST(RTriggerBand, NoKnownBandMeansNoCheckOnAPaperOrder) {
  const Desk d;
  EXPECT_PASS(d, kFatFinger, stop_market(d, oms::Side::kBuy, rupees(104)));
}

TEST(RTriggerBand, TheOrderIsRefusedWithItsOwnReason) {
  Desk d;
  set_band(d, rupees(98), rupees(102));
  const GateDecision out = d.run(stop_market(d, oms::Side::kBuy, rupees(104)));
  ASSERT_TRUE(out.rejection.has_value());
  EXPECT_EQ(out.rejection.value().code, RuleCode::kFatFinger);
  EXPECT_EQ(reason_text(out.rejection.value()), "R-TRIGGER-BAND");
}

TEST(RFatFingerTrigger, TheEdgesOfThePercent) {
  const Desk d;
  EXPECT_PASS(d, kFatFinger, stop_market(d, oms::Side::kBuy, rupees(105)));
  EXPECT_PASS(d, kFatFinger, stop_market(d, oms::Side::kSell, rupees(95)));
  for (const auto& [side, trigger] : {std::pair{oms::Side::kBuy, Price::from_units(1'050'500)},
                                      std::pair{oms::Side::kSell, Price::from_units(949'500)}}) {
    const Outcome out = d.check(RuleCode::kFatFinger, stop_market(d, side, trigger));
    EXPECT_EQ(out.verdict, Verdict::kReject);
    EXPECT_EQ(out.reason, RuleReason::kRule);
  }
}

TEST(RFatFingerTrigger, OptionsUseTheirOwnPercent) {
  const Desk d;
  OrderDetails o = stop_market(d, oms::Side::kBuy, rupees(125));
  o.instrument_id = kOption;
  EXPECT_PASS(d, kFatFinger, o);
  o.trigger_price = rupees(131);
  EXPECT_REJECT(d, kFatFinger, o);
}

TEST(RFatFingerTrigger, ASoftLimitWarns) {
  Desk d;
  d.limit(LimitKey::kFatFingerPct, 2 * kFromScale, LimitScope::kDesk, false);
  EXPECT_WARN(d, kFatFinger, stop_market(d, oms::Side::kBuy, rupees(103)));
}

TEST(RFatFingerTrigger, AWarningOnThePriceCantHideABadTrigger) {
  Desk d;
  d.limit(LimitKey::kFatFingerPct, 2 * kFromScale, LimitScope::kDesk, false);
  OrderDetails o = stop_market(d, oms::Side::kBuy, rupees(106));
  o.order_type = OrderType::kSl;
  o.price = rupees(103);  // warns
  EXPECT_REJECT(d, kFatFinger, o);
}

TEST(RFatFingerTrigger, ATriggerWarningCantHideABadPrice) {
  Desk d;
  d.limit(LimitKey::kFatFingerPct, 2 * kFromScale, LimitScope::kDesk, false);
  OrderDetails o = stop_market(d, oms::Side::kBuy, rupees(103));
  o.order_type = OrderType::kSl;
  o.price = rupees(120);
  const Outcome out = d.check(RuleCode::kFatFinger, o);
  EXPECT_EQ(out.verdict, Verdict::kReject);
  EXPECT_EQ(out.reason, RuleReason::kRule);
}

TEST(RFatFingerTrigger, ATriggerWarningSurvivesAPassingPrice) {
  Desk d;
  d.limit(LimitKey::kFatFingerPct, 2 * kFromScale, LimitScope::kDesk, false);
  OrderDetails o = stop_market(d, oms::Side::kBuy, rupees(103));
  o.order_type = OrderType::kSl;
  o.price = rupees(101);
  EXPECT_WARN(d, kFatFinger, o);
}

TEST(RFatFingerTrigger, ABadPriceIsRefusedThoughTheTriggerIsFine) {
  const Desk d;
  OrderDetails o = stop_market(d, oms::Side::kBuy, rupees(101));
  o.order_type = OrderType::kSl;
  o.price = rupees(120);
  EXPECT_REJECT(d, kFatFinger, o);
}

TEST(RFatFingerTrigger, NoLtpRefusesAStop) {
  Desk d;
  d.ctx.book = nullptr;
  EXPECT_REJECT(d, kFatFinger, stop_market(d, oms::Side::kBuy, rupees(101)));
}

TEST(RFatFingerTrigger, AStaleLtpRefusesAStop) {
  Desk d;
  d.clock.advance(kLtpMaxAge);
  EXPECT_PASS(d, kFatFinger, stop_market(d, oms::Side::kBuy, rupees(101)));
  d.clock.advance(1);
  EXPECT_REJECT(d, kFatFinger, stop_market(d, oms::Side::kBuy, rupees(101)));
}

TEST(RFatFingerTrigger, AStopWithNoTriggerIsRefused) {
  const Desk d;
  OrderDetails o = stop_market(d, oms::Side::kBuy, rupees(101));
  o.trigger_price.reset();
  EXPECT_REJECT(d, kFatFinger, o);
}

TEST(RFatFinger, ALiveOrderOfAnyTypeNeedsABand) {
  Desk d;
  OrderDetails market = d.live_order();
  market.order_type = OrderType::kMarket;
  market.price.reset();
  EXPECT_REJECT(d, kFatFinger, market);
  OrderDetails stop = market;
  stop.order_type = OrderType::kSlM;
  stop.trigger_price = rupees(101);
  EXPECT_REJECT(d, kFatFinger, stop);
  ASSERT_TRUE(d.instruments.put(kEquity, {.lot_size = 1,
                                          .tick_size = Price::from_units(500),
                                          .lower_circuit = rupees(90),
                                          .upper_circuit = rupees(110)}));
  EXPECT_PASS(d, kFatFinger, market);
  EXPECT_PASS(d, kFatFinger, stop);
}

TEST(RFatFinger, AKnownCircuitBandIsEnforced) {
  Desk d;
  ASSERT_TRUE(d.instruments.put(kEquity, {.lot_size = 1,
                                          .tick_size = Price::from_units(500),
                                          .lower_circuit = rupees(98),
                                          .upper_circuit = rupees(102)}));
  OrderDetails o = d.order();
  o.price = rupees(102);
  EXPECT_PASS(d, kFatFinger, o);
  o.price = rupees(98);
  EXPECT_PASS(d, kFatFinger, o);
  o.price = Price::from_units(1'020'500);  // inside the 5 %, outside the band
  EXPECT_REJECT(d, kFatFinger, o);
  o.price = Price::from_units(979'500);
  EXPECT_REJECT(d, kFatFinger, o);
}

TEST(RFatFinger, ALiveOrderNeedsACircuitBandButPaperDoesNot) {
  Desk d;
  EXPECT_PASS(d, kFatFinger, d.order());
  EXPECT_REJECT(d, kFatFinger, d.live_order());
  ASSERT_TRUE(d.instruments.put(kEquity, {.lot_size = 1,
                                          .tick_size = Price::from_units(500),
                                          .lower_circuit = rupees(90),
                                          .upper_circuit = rupees(110)}));
  EXPECT_PASS(d, kFatFinger, d.live_order());
}

TEST(RFatFinger, AnEnormousPriceIsRefusedWithoutOverflowing) {
  const Desk d;
  OrderDetails o = d.order();
  o.price = Price::from_units(std::numeric_limits<std::int64_t>::max() - 500);
  EXPECT_REJECT(d, kFatFinger, o);
}

TEST(RFatFinger, AMissingLimitIsRefused) {
  Desk d;
  d.clear_limits(LimitKey::kFatFingerPct);
  EXPECT_REJECT(d, kFatFinger, d.order());
}

// ---- R-ORDER-VALUE ----

TEST(ROrderValue, AMarketOrdersLtpIsAgedOnTheMonotonicClock) {
  for (const Nanos step : {Nanos{-10} * kNanosPerSecond, Nanos{10} * kNanosPerSecond}) {
    Desk d;
    OrderDetails o = d.order();
    o.order_type = OrderType::kMarket;
    o.price.reset();
    d.clock.step_wall(step);
    EXPECT_PASS(d, kOrderValue, o);
    d.clock.advance(kLtpMaxAge + 1);
    d.clock.step_wall(step);
    EXPECT_REJECT(d, kOrderValue, o);
  }
}

TEST(ROrderValue, TheNotionalLimit) {
  Desk d;
  OrderDetails o = d.order();
  o.qty = 5'000;  // ₹5,00,000 exactly
  d.clear_limits(LimitKey::kMaxOrderQtyLots);
  d.limit(LimitKey::kMaxOrderQtyLots, 100'000 * kFromScale);
  EXPECT_PASS(d, kOrderValue, o);
  o.qty = 5'001;
  EXPECT_REJECT(d, kOrderValue, o);
}

TEST(ROrderValue, TheLotLimitCountsLots) {
  const Desk d;
  OrderDetails o = d.order();
  o.instrument_id = kOption;
  o.qty = 65 * 20;
  o.price = rupees(10);
  EXPECT_PASS(d, kOrderValue, o);
  o.qty = 65 * 21;
  EXPECT_REJECT(d, kOrderValue, o);
}

TEST(ROrderValue, AMarketOrderIsValuedAtTheLtp) {
  Desk d;
  d.clear_limits(LimitKey::kMaxOrderQtyLots);
  d.limit(LimitKey::kMaxOrderQtyLots, 100'000 * kFromScale);
  OrderDetails o = d.order();
  o.order_type = OrderType::kMarket;
  o.price.reset();
  o.qty = 5'000;
  EXPECT_PASS(d, kOrderValue, o);
  d.tick(kEquity, rupees(101));
  EXPECT_REJECT(d, kOrderValue, o);
}

TEST(ROrderValue, AStopMarketIsValuedAtTheHigherOfItsTriggerAndTheLtp) {
  Desk d;
  d.clear_limits(LimitKey::kMaxOrderQtyLots);
  d.limit(LimitKey::kMaxOrderQtyLots, 100'000 * kFromScale);
  OrderDetails o = d.order();
  o.order_type = OrderType::kSlM;
  o.price.reset();
  o.qty = 5'000;
  o.trigger_price = rupees(100);
  EXPECT_PASS(d, kOrderValue, o);
  o.trigger_price = rupees(101);  // above the LTP: the trigger decides
  EXPECT_REJECT(d, kOrderValue, o);
  o.trigger_price = rupees(1);  // a low trigger can't understate it
  d.tick(kEquity, rupees(101));
  EXPECT_REJECT(d, kOrderValue, o);
}

TEST(ROrderValue, AStopMarketWithNoFreshLtpCantBeValued) {
  Desk d;
  OrderDetails o = d.order();
  o.order_type = OrderType::kSlM;
  o.price.reset();
  o.trigger_price = rupees(99);
  EXPECT_PASS(d, kOrderValue, o);
  d.tick(kEquity, rupees(100), kNow - (4 * kNanosPerSecond));
  EXPECT_REJECT(d, kOrderValue, o);
  d.ctx.book = nullptr;
  EXPECT_REJECT(d, kOrderValue, o);
}

TEST(ROrderValue, AMarketOrderWithNoFreshLtpCantBeValued) {
  Desk d;
  OrderDetails o = d.order();
  o.order_type = OrderType::kMarket;
  o.price.reset();
  d.tick(kEquity, rupees(100), kNow - (4 * kNanosPerSecond));
  EXPECT_REJECT(d, kOrderValue, o);
}

TEST(ROrderValue, ASoftLimitWarnsAndAHardOneOutranksIt) {
  Desk d;
  d.limit(LimitKey::kMaxOrderValue, 500 * kFromScale, LimitScope::kDesk, false);
  EXPECT_WARN(d, kOrderValue, d.order());
  d.limit(LimitKey::kMaxOrderQtyLots, 5 * kFromScale);
  EXPECT_REJECT(d, kOrderValue, d.order());
}

TEST(ROrderValue, AHugeOrderIsRefusedWithoutOverflowing) {
  const Desk d;
  OrderDetails o = d.order();
  o.qty = std::numeric_limits<std::int32_t>::max();
  o.price = Price::from_units(std::numeric_limits<std::int64_t>::max() - 500);
  EXPECT_REJECT(d, kOrderValue, o);
}

TEST(ROrderValue, EachMissingLimitIsRefused) {
  for (const LimitKey key : {LimitKey::kMaxOrderValue, LimitKey::kMaxOrderQtyLots}) {
    Desk d;
    d.clear_limits(key);
    EXPECT_REJECT(d, kOrderValue, d.order());
  }
}

// ---- R-DUP ----

TEST(RDup, TheSameOrderInsideTheWindowIsRefused) {
  Desk d;
  ASSERT_TRUE(d.run(d.order()).approved.has_value());
  d.clock.advance((1'500 * kNanosPerSecond / 1'000) - 1);
  const GateDecision again = d.run(d.order());
  ASSERT_TRUE(again.rejection.has_value());
  EXPECT_EQ(again.rejection.value().code, RuleCode::kDup);
  EXPECT_EQ(reason_text(again.rejection.value()), "R-DUP");
}

TEST(RDup, TheWindowEndsAtItsLength) {
  Desk d;
  ASSERT_TRUE(d.run(d.order()).approved.has_value());
  d.clock.advance(1'500 * kNanosPerSecond / 1'000);
  EXPECT_TRUE(d.run(d.order()).approved.has_value());
}

TEST(RDup, EachFieldOfTheKeyMakesItADifferentOrder) {
  Desk d;
  ASSERT_TRUE(d.run(d.order()).approved.has_value());
  OrderDetails side = d.order();
  side.side = Side::kSell;
  EXPECT_PASS(d, kDup, side);
  OrderDetails qty = d.order();
  qty.qty = 11;
  EXPECT_PASS(d, kDup, qty);
  OrderDetails price = d.order();
  price.price = rupees(101);
  EXPECT_PASS(d, kDup, price);
  OrderDetails instrument = d.order();
  instrument.instrument_id = kBseEquity;
  EXPECT_PASS(d, kDup, instrument);
  OrderDetails account = d.order();
  account.account_id = kOtherAccount;
  EXPECT_PASS(d, kDup, account);
  EXPECT_REJECT(d, kDup, d.order());
}

TEST(RDup, ADifferentTriggerIsADifferentOrder) {
  Desk d;
  OrderDetails first = d.order();
  first.order_type = OrderType::kSlM;
  first.price.reset();
  first.trigger_price = rupees(101);
  ASSERT_TRUE(d.run(first).approved.has_value());
  EXPECT_REJECT(d, kDup, first);
  OrderDetails other = first;
  other.trigger_price = rupees(102);
  EXPECT_PASS(d, kDup, other);
}

TEST(RDup, ACallerSuppliedParentIdEarnsNoExemption) {
  Desk d;
  OrderDetails first = d.order();
  first.parent_id = id_of(5);
  ASSERT_TRUE(d.run(first).approved.has_value());
  OrderDetails second = d.order();
  second.parent_id = id_of(5);  // the same parent, but the slicer didn't make these
  EXPECT_REJECT(d, kDup, second);
}

TEST(RDup, ChildrenOfOneSliceAreNotDuplicatesOfEachOther) {
  Desk d;
  OrderDetails parent = d.order();
  parent.qty = 30;
  std::vector<OrderDetails> children(3, parent);
  ASSERT_EQ(oms::FreezeSlicer::slice(parent, 10, children), 3U);
  for (const OrderDetails& child : children) {
    EXPECT_EQ(child.qty, 10);
    const GateDecision out = d.run(child);
    EXPECT_TRUE(out.approved.has_value());
  }
}

TEST(RDup, TwoSeparateSlicesAreStillDuplicatesOfEachOther) {
  Desk d;
  OrderDetails parent = d.order();
  parent.qty = 20;
  std::vector<OrderDetails> first(2, parent);
  std::vector<OrderDetails> second(2, parent);
  ASSERT_EQ(oms::FreezeSlicer::slice(parent, 10, first), 2U);
  ASSERT_EQ(oms::FreezeSlicer::slice(parent, 10, second), 2U);
  ASSERT_TRUE(d.run(first[0]).approved.has_value());
  EXPECT_REJECT(d, kDup, second[0]);
}

TEST(RDup, AnOrdinaryOrderIsStillADuplicateOfASliceChild) {
  Desk d;
  OrderDetails parent = d.order();
  parent.qty = 20;
  std::vector<OrderDetails> children(2, parent);
  ASSERT_EQ(oms::FreezeSlicer::slice(parent, 10, children), 2U);
  ASSERT_TRUE(d.run(children[0]).approved.has_value());
  EXPECT_REJECT(d, kDup, d.order());
}

TEST(RDup, ASoftWindowWarnsInsteadOfRefusing) {
  Desk d;
  d.clear_limits(LimitKey::kDupWindowMs);
  d.limit(LimitKey::kDupWindowMs, 1'500 * kFromScale, LimitScope::kDesk, false);
  ASSERT_TRUE(d.run(d.order()).approved.has_value());
  EXPECT_WARN(d, kDup, d.order());
}

TEST(RDup, AZeroWindowChecksNothingAndKeepsNothing) {
  Desk d;
  d.clear_limits(LimitKey::kDupWindowMs);
  d.limit(LimitKey::kDupWindowMs, 0);
  ASSERT_TRUE(d.run(d.order()).approved.has_value());
  EXPECT_EQ(d.ctx.dup.count, 0U);
  EXPECT_PASS(d, kDup, d.order());
}

TEST(RDup, AWallClockStepInEitherDirectionKeepsTheWindow) {
  for (const Nanos step : {Nanos{-10} * kNanosPerSecond, Nanos{10} * kNanosPerSecond}) {
    Desk d;
    d.clock.step_wall(step);
    ASSERT_TRUE(d.run(d.order()).approved.has_value());
    d.clock.step_wall(step);
    EXPECT_REJECT(d, kDup, d.order());
    d.clock.advance((Nanos{1'500} * kNanosPerSecond / 1'000) - 1);
    EXPECT_REJECT(d, kDup, d.order());
    d.clock.advance(1);
    EXPECT_PASS(d, kDup, d.order());
  }
}

// Another order's commit prunes the table, which must not evict a live entry.
TEST(RDup, AForwardClockStepDoesntPruneALiveEntry) {
  Desk d;
  ASSERT_TRUE(d.run(d.order()).approved.has_value());
  d.clock.step_wall(Nanos{19} * kNanosPerSecond / 10);  // past the 1.5 s window, inside the pause gap
  OrderDetails other = d.order();
  other.qty = 11;
  ASSERT_TRUE(d.run(other).approved.has_value());
  EXPECT_REJECT(d, kDup, d.order());
}

TEST(RDup, ASuspendLongerThanTheWindowEndsIt) {  // D-34
  Desk d;
  ASSERT_TRUE(d.run(d.order()).approved.has_value());
  d.clock.suspend((Nanos{1'500} * kNanosPerSecond / 1'000) + 1);
  EXPECT_PASS(d, kDup, d.order());
}

TEST(GateStampClocks, EvaluationTimeIsMeasuredOnTheMonotonicClock) {
  // The wall clock jumps on every read and the monotonic one doesn't move.
  class JumpingWall final : public IClock {
   public:
    [[nodiscard]] Nanos now() const noexcept override {
      return kNow + (7 * kNanosPerSecond) + (reads_++ * kNanosPerSecond);
    }
    [[nodiscard]] Nanos monotonic() const noexcept override { return kNow; }

   private:
    mutable Nanos reads_{0};
  };
  Desk d;
  const JumpingWall clock;
  const RiskGate gate = RiskGate::create(pre_trade_rules(), clock).value();
  EXPECT_EQ(gate.evaluate(d.ctx, d.order()).stamp.eval_ns, 0);
}

TEST(GateStampClocks, SessionHoursFollowTheWallClock) {
  Desk d;
  d.clock.step_wall(at(9, 14) - kNow);  // the monotonic clock still reads inside the session
  const GateDecision out = d.run(d.order());
  ASSERT_TRUE(out.rejection.has_value());
  EXPECT_EQ(out.rejection.value().code, RuleCode::kMktHours);
}

TEST(GateStampClocks, TheStampCarriesBothClocks) {
  Desk d;
  d.clock.step_wall(Nanos{10} * kNanosPerSecond);
  const GateDecision out = d.run(d.order());
  ASSERT_TRUE(out.approved.has_value());
  EXPECT_EQ(out.stamp.ts, d.clock.now());
  EXPECT_EQ(out.stamp.monotonic, d.clock.monotonic());
}

TEST(RDup, ExpiredEntriesLeaveTheTableOnTheNextApproval) {
  Desk d;
  ASSERT_TRUE(d.run(d.order()).approved.has_value());
  EXPECT_EQ(d.ctx.dup.count, 1U);
  d.clock.advance(2 * kNanosPerSecond);
  OrderDetails other = d.order();
  other.qty = 11;
  ASSERT_TRUE(d.run(other).approved.has_value());
  EXPECT_EQ(d.ctx.dup.count, 1U);
}

void fill_dup_table(Desk& d) {
  for (std::size_t i = 0; i < DupTable::kCapacity; ++i) {
    d.ctx.dup.entries.at(i) = DupEntry{
        .instrument_id = 5'000 + static_cast<std::int64_t>(i), .ts = kNow, .keep = 60 * kNanosPerSecond};
  }
  d.ctx.dup.count = DupTable::kCapacity;
}

TEST(RDup, AFullTableRefusesAndAsksForAnAlert) {
  Desk d;
  fill_dup_table(d);
  const GateDecision out = d.run(d.order());
  ASSERT_TRUE(out.rejection.has_value());
  EXPECT_EQ(out.rejection.value().code, RuleCode::kDup);
  EXPECT_TRUE(out.rejection.value().alert);
  EXPECT_EQ(reason_text(out.rejection.value()), oms::kDupTableFull);
  EXPECT_EQ(d.ctx.dup.count, DupTable::kCapacity);
}

TEST(RDup, ATableFullOfExpiredEntriesIsNotFull) {
  Desk d;
  fill_dup_table(d);
  d.clock.advance(61 * kNanosPerSecond);
  d.tick(kEquity, rupees(100), d.clock.monotonic());
  const GateDecision out = d.run(d.order());
  ASSERT_TRUE(out.approved.has_value());
  EXPECT_EQ(d.ctx.dup.count, 1U);
}

TEST(RDup, ATableWithOneSlotLeftStillTakesAnOrder) {
  Desk d;
  fill_dup_table(d);
  d.ctx.dup.count = DupTable::kCapacity - 1;
  ASSERT_TRUE(d.run(d.order()).approved.has_value());
  EXPECT_EQ(d.ctx.dup.count, DupTable::kCapacity);
}

TEST(RDup, AMissingWindowLimitIsRefused) {
  Desk d;
  d.clear_limits(LimitKey::kDupWindowMs);
  EXPECT_REJECT(d, kDup, d.order());
}

// ---- R-OPS ----

// Orders that differ, so R-DUP doesn't step in.
OrderDetails nth(const Desk& d, int n) {
  OrderDetails o = d.order();
  o.qty = 1 + n;
  return o;
}

TEST(ROps, TheCapthOrderPassesAndTheNextIsRefusedAtOnce) {
  Desk d;
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(d.run(nth(d, i)).approved.has_value()) << i;
  }
  const GateDecision over = d.run(nth(d, 8));
  ASSERT_TRUE(over.rejection.has_value());
  EXPECT_EQ(over.rejection.value().code, RuleCode::kOps);
}

TEST(ROps, ATokenComesBackAfterAFullSecond) {
  Desk d;
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(d.run(nth(d, i)).approved.has_value());
  }
  d.clock.advance(kNanosPerSecond - 1);
  EXPECT_REJECT(d, kOps, nth(d, 8));
  d.clock.advance(1);
  EXPECT_PASS(d, kOps, nth(d, 8));
}

TEST(ROps, ATenSecondForwardClockStepReleasesNoToken) {
  Desk d;
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(d.run(nth(d, i)).approved.has_value());
  }
  d.clock.step_wall(10 * kNanosPerSecond);
  EXPECT_REJECT(d, kOps, nth(d, 8));
  d.clock.advance(kNanosPerSecond);
  EXPECT_PASS(d, kOps, nth(d, 8));
}

// Through the gate, with the wall clock stepped before the tokens are taken: the only way to catch the gate
// reading or stamping the wrong clock.
TEST(ROps, TheGateUsesTheMonotonicClockAcrossSteps) {
  Desk d;
  d.clock.step_wall(10 * kNanosPerSecond);
  d.tick(kEquity, rupees(100), d.clock.monotonic());
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(d.run(nth(d, i)).approved.has_value()) << i;
  }
  d.clock.step_wall(Nanos{3} * kNanosPerSecond / 2);  // past the 1 s window, inside the pause gap
  d.tick(kEquity, rupees(100), d.clock.monotonic());
  const GateDecision stepped = d.run(nth(d, 8));
  ASSERT_TRUE(stepped.rejection.has_value());
  EXPECT_EQ(stepped.rejection.value().code, RuleCode::kOps);
  d.clock.advance(kNanosPerSecond);
  d.tick(kEquity, rupees(100), d.clock.monotonic());
  EXPECT_TRUE(d.run(nth(d, 8)).approved.has_value());
}

TEST(ROps, AForwardClockStepDoesntClearASoftCapWarning) {
  Desk d;
  d.limit(LimitKey::kOpsCap, 1 * kFromScale, LimitScope::kDesk, false);
  ASSERT_TRUE(d.run(nth(d, 0)).approved.has_value());
  d.clock.step_wall(10 * kNanosPerSecond);
  EXPECT_WARN(d, kOps, nth(d, 1));
}

TEST(ROps, ASuspendReturnsTheTokens) {  // D-34
  Desk d;
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(d.run(nth(d, i)).approved.has_value());
  }
  EXPECT_REJECT(d, kOps, nth(d, 8));
  d.clock.suspend(kNanosPerSecond);
  EXPECT_PASS(d, kOps, nth(d, 8));
}

// D-36. The gate's first evaluation only records the clocks.
TEST(PauseCheck, AForwardStepOverTwoSecondsMakesEveryLtpStaleUntilANewTickArrives) {
  Desk d;
  ASSERT_TRUE(d.run(nth(d, 0)).approved.has_value());
  d.clock.step_wall(kPauseGap + 1);
  const GateDecision stale = d.run(nth(d, 1));
  ASSERT_TRUE(stale.rejection.has_value());
  EXPECT_EQ(stale.rejection.value().code,
            RuleCode::kFatFinger);  // the tick looks young but predates the pause
  d.clock.advance(1);
  d.tick(kEquity, rupees(100), d.clock.monotonic());
  EXPECT_TRUE(d.run(nth(d, 1)).approved.has_value());
}

TEST(PauseCheck, ABackwardStepOrOneAtTheGapLeavesTheDataFresh) {
  for (const Nanos step : {Nanos{-10} * kNanosPerSecond, kPauseGap}) {
    Desk d;
    ASSERT_TRUE(d.run(nth(d, 0)).approved.has_value());
    d.clock.step_wall(step);
    EXPECT_TRUE(d.run(nth(d, 1)).approved.has_value());
  }
}

TEST(PauseCheck, TheWatermarkIsTheMonotonicReadingAtTheLook) {
  Desk d;
  ASSERT_TRUE(d.run(nth(d, 0)).approved.has_value());
  EXPECT_EQ(d.ctx.stale_before, 0);
  d.clock.step_wall(kPauseGap + 1);
  (void)d.run(nth(d, 1));
  EXPECT_EQ(d.ctx.stale_before, d.clock.monotonic());
  EXPECT_FALSE(d.ctx.fresh(d.clock.monotonic()));
  EXPECT_TRUE(d.ctx.fresh(d.clock.monotonic() + 1));
}

TEST(ROps, AWallClockStepBackHoldsNoTokenBack) {
  Desk d;
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(d.run(nth(d, i)).approved.has_value());
  }
  d.clock.advance(kNanosPerSecond);
  d.clock.step_wall(-3'600 * kNanosPerSecond);
  EXPECT_PASS(d, kOps, nth(d, 8));
}

TEST(ROps, EachExchangeHasItsOwnBudgetAndNfoSharesNses) {
  Desk d;
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(d.run(nth(d, i)).approved.has_value());
  }
  OrderDetails bse = nth(d, 20);
  bse.instrument_id = kBseEquity;
  EXPECT_PASS(d, kOps, bse);
  OrderDetails nfo = nth(d, 20);
  nfo.instrument_id = kOption;
  EXPECT_REJECT(d, kOps, nfo);  // NFO is NSE: one exchange, one budget
}

TEST(ROps, EachAccountHasItsOwnBudget) {
  Desk d;
  ASSERT_NE(d.ctx.accounts.put({.id = kOtherAccount, .ops_cap = 8}), nullptr);
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(d.run(nth(d, i)).approved.has_value());
  }
  OrderDetails other = nth(d, 20);
  other.account_id = kOtherAccount;
  EXPECT_PASS(d, kOps, other);
}

TEST(ROps, TheCapIsTheStrictestOfTheAccountAndItsLimits) {
  Desk d;
  EXPECT_EQ(effective_ops_cap(d.ctx, kAccount), 8);
  d.limit(LimitKey::kOpsCap, 3 * kFromScale, LimitScope::kAccount, true, kAccount);
  EXPECT_EQ(effective_ops_cap(d.ctx, kAccount), 3);
  d.limit(LimitKey::kOpsCap, 2 * kFromScale);
  EXPECT_EQ(effective_ops_cap(d.ctx, kAccount), 2);
}

TEST(ROps, ATraderScopedLimitDoesNotChangeAnAccountsBucket) {
  Desk d;
  d.limit(LimitKey::kOpsCap, 1 * kFromScale, LimitScope::kTrader, true, kUser);
  EXPECT_EQ(effective_ops_cap(d.ctx, kAccount), 8);
}

TEST(ROps, TheCapIsNeverAboveTen) {
  Desk d;
  ASSERT_NE(d.ctx.accounts.put({.id = kAccount, .ops_cap = 200}), nullptr);
  EXPECT_EQ(effective_ops_cap(d.ctx, kAccount), oms::kMaxOpsCap);
}

TEST(ROps, AnUnknownAccountOrAZeroCapIsRefused) {
  Desk d;
  OrderDetails o = d.order();
  o.account_id = kOtherAccount;
  EXPECT_REJECT(d, kOps, o);
  ASSERT_NE(d.ctx.accounts.put({.id = kAccount, .ops_cap = 0}), nullptr);
  EXPECT_REJECT(d, kOps, d.order());
}

TEST(ROps, RefreshingAnAccountKeepsItsBudget) {
  Desk d;
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(d.run(nth(d, i)).approved.has_value());
  }
  ASSERT_NE(d.ctx.accounts.put({.id = kAccount, .session_connected = true, .ops_cap = 8}), nullptr);
  EXPECT_REJECT(d, kOps, nth(d, 8));
}

TEST(ROps, ASoftCapWarns) {
  Desk d;
  d.limit(LimitKey::kOpsCap, 1 * kFromScale, LimitScope::kDesk, false);
  EXPECT_PASS(d, kOps, nth(d, 0));
  ASSERT_TRUE(d.run(nth(d, 0)).approved.has_value());
  EXPECT_WARN(d, kOps, nth(d, 1));
}

// ---- Two-phase: a rejected order leaves no state ----

Outcome rejects(const GateContext& /*ctx*/, const OrderDetails& /*o*/, const GateFacts& /*f*/) noexcept {
  return {.verdict = Verdict::kReject};
}

struct Snapshot {
  std::size_t dup_count{0};
  oms::OpsWindow nse;
  oms::OpsWindow bse;
};
Snapshot snapshot(const Desk& d) {
  const AccountState* a = std::as_const(d.ctx).accounts.find(kAccount);
  return {d.ctx.dup.count, a->ops.at(0), a->ops.at(1)};
}
void expect_unchanged(const Desk& d, const Snapshot& before) {
  const Snapshot now = snapshot(d);
  EXPECT_EQ(now.dup_count, before.dup_count);
  EXPECT_EQ(now.nse, before.nse);
  EXPECT_EQ(now.bse, before.bse);
}

TEST(TwoPhaseRules, AnOrderRejectedAfterBothStatefulRulesLeavesThemUnchanged) {
  Desk d;
  std::vector<Rule> rules(pre_trade_rules().begin(), pre_trade_rules().end());
  rules.push_back({.code = RuleCode::kDiscipline, .eval = &rejects});
  const Snapshot before = snapshot(d);
  const GateDecision out = d.run(d.order(), rules);
  ASSERT_TRUE(out.rejection.has_value());
  EXPECT_EQ(out.rejection.value().code, RuleCode::kDiscipline);
  expect_unchanged(d, before);
  // And the same order can go through now: it never took the duplicate slot or a token.
  EXPECT_TRUE(d.run(d.order()).approved.has_value());
}

TEST(TwoPhaseRules, AnOrderRejectedByROpsLeavesNoDuplicateEntry) {
  Desk d;
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(d.run(nth(d, i)).approved.has_value());
  }
  const Snapshot before = snapshot(d);
  const GateDecision out = d.run(nth(d, 12));  // 13 lots: under the lot limit, so R-OPS is what stops it
  ASSERT_TRUE(out.rejection.has_value());
  EXPECT_EQ(out.rejection.value().code, RuleCode::kOps);
  expect_unchanged(d, before);
}

TEST(TwoPhaseRules, AnOrderRejectedByRDupUsesNoToken) {
  Desk d;
  ASSERT_TRUE(d.run(d.order()).approved.has_value());
  const Snapshot before = snapshot(d);
  ASSERT_TRUE(d.run(d.order()).rejection.has_value());
  expect_unchanged(d, before);
}

TEST(TwoPhaseRules, AnOrderRejectedEarlierInTheChainTouchesNothing) {
  Desk d;
  d.ctx.kill.block(KillSwitch::Scope::kDesk);
  const Snapshot before = snapshot(d);
  ASSERT_TRUE(d.run(d.order()).rejection.has_value());
  expect_unchanged(d, before);
}

TEST(TwoPhaseRules, AnApprovedOrderTakesExactlyOneTokenAndOneEntry) {
  Desk d;
  ASSERT_TRUE(d.run(d.order()).approved.has_value());
  EXPECT_EQ(d.ctx.dup.count, 1U);
  const AccountState* a = std::as_const(d.ctx).accounts.find(kAccount);
  EXPECT_TRUE(a->ops.at(0).has_token(kNow, 2));
  EXPECT_FALSE(a->ops.at(0).has_token(kNow, 1));
}

// ---- Failing closed, and the §3 properties these rules can be held to ----

TEST(RulesFailClosed, AnEmptyContextRefusesEverything) {
  const SimClock clock{kNow};
  const RiskGate gate = RiskGate::create(pre_trade_rules(), clock).value();
  const auto ctx = std::make_unique<GateContext>();
  OrderDetails o{.is_paper = oms::kPaperOrder};
  o.qty = 1;
  o.price = rupees(1);
  o.instrument_id = kEquity;
  EXPECT_TRUE(gate.evaluate(*ctx, o).rejection.has_value());
}

TEST(FreezeSlicerBasics, SplitsWithTheRemainderLast) {
  OrderDetails parent{.is_paper = oms::kPaperOrder};
  parent.id = id_of(8);
  parent.qty = 25;
  std::vector<OrderDetails> out(3, parent);
  ASSERT_EQ(oms::FreezeSlicer::slice(parent, 10, out), 3U);
  EXPECT_EQ(out[0].qty, 10);
  EXPECT_EQ(out[1].qty, 10);
  EXPECT_EQ(out[2].qty, 5);
  EXPECT_EQ(out[2].parent_id, parent.id);
  EXPECT_TRUE(out[0].slice.sliced());
  EXPECT_EQ(out[0].slice, out[2].slice);
}

TEST(FreezeSlicerBasics, ANewIntentEachTime) {
  OrderDetails parent{.is_paper = oms::kPaperOrder};
  parent.qty = 10;
  std::vector<OrderDetails> a(1, parent);
  std::vector<OrderDetails> b(1, parent);
  ASSERT_EQ(oms::FreezeSlicer::slice(parent, 10, a), 1U);
  ASSERT_EQ(oms::FreezeSlicer::slice(parent, 10, b), 1U);
  EXPECT_FALSE(a[0].slice == b[0].slice);
}

TEST(FreezeSlicerBasics, BadInputsWriteNothing) {
  OrderDetails parent{.is_paper = oms::kPaperOrder};
  parent.qty = 25;
  std::vector<OrderDetails> out(2, parent);
  EXPECT_EQ(oms::FreezeSlicer::slice(parent, 10, out), 0U);  // needs 3
  EXPECT_EQ(oms::FreezeSlicer::slice(parent, 0, out), 0U);
  parent.qty = 0;
  EXPECT_EQ(oms::FreezeSlicer::slice(parent, 10, out), 0U);
  EXPECT_FALSE(out[0].slice.sliced());
}

TEST(GateSendLimitsTest, GivesTheCapAndTheExchangeBudget) {
  const Desk d;
  const GateSendLimits limits(d.ctx);
  EXPECT_EQ(limits.ops_cap(kAccount), 8);
  EXPECT_EQ(limits.ops_cap(kOtherAccount), 0);
  EXPECT_EQ(limits.ops_venue(kEquity), 0);
  EXPECT_EQ(limits.ops_venue(kOption), 0);  // NFO counts with NSE
  EXPECT_EQ(limits.ops_venue(kBseEquity), 1);
  EXPECT_EQ(limits.ops_venue(999), oms::kOpsVenueUnknown);
}

// §3 property (b), for the rules that exist: with every limit at its loosest, every well-formed order passes.
RC_GTEST_PROP(RulesProperties, WithEveryLimitAtItsLoosestAWellFormedOrderPasses, ()) {
  Desk d;
  const auto loosest = [&](LimitKey key, std::int64_t value) {
    d.clear_limits(key);
    d.limit(key, value);
  };
  constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
  loosest(LimitKey::kMaxOrderValue, kMax);
  loosest(LimitKey::kMaxOrderQtyLots, kMax);
  loosest(LimitKey::kFatFingerPct, kMax);
  loosest(LimitKey::kFatFingerPctOpt, kMax);
  loosest(LimitKey::kDupWindowMs, 0);

  const bool option = *rc::gen::arbitrary<bool>();
  OrderDetails o = d.order();
  o.instrument_id = option ? kOption : kEquity;
  o.qty = (option ? 65 : 1) * *rc::gen::inRange(1, 50'000);
  o.side = *rc::gen::element(Side::kBuy, Side::kSell);
  o.product = *rc::gen::element(Product::kCnc, Product::kNrml);
  o.price = Price::from_units(500 * *rc::gen::inRange<std::int64_t>(1, 1'000'000'000));
  const GateDecision out = d.run(o);
  RC_ASSERT(!out.rejection.has_value());
}

// §3 property (c): same context and order, same answer. Run twice from the same state, with nothing
// committed.
RC_GTEST_PROP(RulesProperties, TheChecksArePureGivenTheContext, ()) {
  const Desk d;
  OrderDetails o = d.order();
  o.qty = *rc::gen::inRange(-5, 1'000);
  o.price = Price::from_units(*rc::gen::inRange<std::int64_t>(-1'000, 2'000'000));
  o.instrument_id = *rc::gen::element(kEquity, kOption, kBseEquity, std::int64_t{999});
  o.order_type = *rc::gen::element(OrderType::kLimit, OrderType::kMarket, OrderType::kSl, OrderType::kSlM);
  for (const Rule& rule : pre_trade_rules()) {
    const Outcome first = d.check(rule.code, o);
    const Outcome second = d.check(rule.code, o);
    RC_ASSERT(first.verdict == second.verdict);
    RC_ASSERT(first.limit == second.limit);
    RC_ASSERT(first.current == second.current);
  }
}

// Whatever arrives, a rejection leaves no state behind, and an approval takes one entry and one token.
RC_GTEST_PROP(RulesProperties, ARejectionNeverChangesTheStatefulRules, ()) {
  Desk d;
  const int orders = *rc::gen::inRange(1, 30);
  for (int i = 0; i < orders; ++i) {
    OrderDetails o = d.order();
    o.qty = *rc::gen::inRange(1, 6);
    o.price = *rc::gen::element(rupees(100), rupees(101), rupees(500));
    const Snapshot before = snapshot(d);
    const GateDecision out = d.run(o);
    if (out.rejection.has_value()) {
      expect_unchanged(d, before);
    } else {
      RC_ASSERT(d.ctx.dup.count == before.dup_count + 1);
    }
  }
}

}  // namespace
}  // namespace vyuha::risk
// NOLINTEND(bugprone-unchecked-optional-access)
