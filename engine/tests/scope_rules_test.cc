// Rules 14 to 17, 19 and 20 (M2.4a): judged against a scope's own totals. One group per rule, then the data
// each needs (D-27, D-28) and which orders count as opening (D-29).

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "desk_fixture.h"
#include "vyuha/risk/scope_book.h"

// NOLINTBEGIN(bugprone-unchecked-optional-access): every test does ASSERT_TRUE(has_value()) first
namespace vyuha::risk {
namespace {

constexpr std::int64_t kFuture = 4;
constexpr std::int64_t kOption2 = 5;
constexpr std::int64_t kRupee = Price::kUnitsPerRupee;

// The fixture desk with a scope book that holds positions and P&L, every limit loose, and an option and a
// future on the equity.
struct ScopeDesk : Desk {
  std::unique_ptr<ScopeBook> owned_book;
  ScopeBook& sb;
  std::vector<Rule> all_rules;

  ScopeDesk() : owned_book(std::make_unique<ScopeBook>(instruments, 512)), sb(*owned_book) {
    ctx.scopes = &sb;
    sb.set_positions_loaded(true);
    sb.set_pnl_loaded(true);
    InstrumentRef option = *instruments.find(kOption);
    option.underlying_id = kEquity;
    EXPECT_TRUE(instruments.put(kOption, option));
    EXPECT_TRUE(instruments.put(kFuture, {.lot_size = 1,
                                          .tick_size = Price::from_units(500),
                                          .exchange = Exchange::kNfo,
                                          .segment = Segment::kFut,
                                          .underlying_id = kEquity}));
    EXPECT_TRUE(instruments.put(kOption2, option));
    tick(kFuture, rupees(100));
    EXPECT_TRUE(sb.set_mark(kEquity, rupees(100)));
    limit(LimitKey::kMaxOpenOrders, 50 * kFromScale);
    limit(LimitKey::kMaxPositionLots, 1'000 * kFromScale);
    limit(LimitKey::kMaxPositionNotional, rupees(1'000'000).units());
    limit(LimitKey::kMaxGrossExposure, rupees(1'000'000).units());
    limit(LimitKey::kMaxNetDeltaInr, rupees(1'000'000).units());
    limit(LimitKey::kMaxNetVegaInr, rupees(1'000'000).units());
    limit(LimitKey::kMaxDailyLoss, rupees(50'000).units());
    limit(LimitKey::kMaxDrawdownFromPeak, rupees(75'000).units());
    all_rules.assign(pre_trade_rules().begin(), pre_trade_rules().end());
    all_rules.insert(all_rules.end(), scope_rules().begin(), scope_rules().end());
  }

  [[nodiscard]] GateDecision run_all(const OrderDetails& o) { return run(o, all_rules); }
  [[nodiscard]] static Attribution who() { return {.account = kAccount, .trader = kUser}; }
  void fill(oms::Side side, std::int64_t qty, Price price, std::int64_t instrument = kEquity,
            const Attribution& by = who()) {
    ASSERT_TRUE(
        sb.apply_fill({.who = by, .instrument_id = instrument, .side = side, .qty = qty, .price = price}));
  }
  [[nodiscard]] OrderDetails sell(std::int32_t qty) const {
    OrderDetails o = order();
    o.side = Side::kSell;
    o.qty = qty;
    return o;
  }
  [[nodiscard]] OrderDetails buy(std::int32_t qty) const {
    OrderDetails o = order();
    o.qty = qty;
    return o;
  }
  // An option order with fresh Greeks for the option: delta 50 and vega 2 rupees per unit.
  void fresh_greeks() {
    ASSERT_TRUE(
        sb.set_greeks(kOption, {.delta = 50 * kRupee, .vega = 2 * kRupee, .as_of = clock.monotonic()}));
  }
  [[nodiscard]] OrderDetails option_order(Side side, std::int32_t qty, bool live) const {
    OrderDetails o = live ? live_order() : order();
    o.instrument_id = kOption;
    o.side = side;
    o.qty = qty;
    o.price = rupees(100);
    return o;
  }
};

[[nodiscard]] bool names(const Outcome& out, DataSource source) {
  return out.verdict == Verdict::kReject && out.reason == RuleReason::kDataMissing && out.source == source;
}

TEST(ScopeRules, TheTableIsRules14To17And19And20InOrderAndLeavesTheGateIncomplete) {
  const auto rules = scope_rules();
  ASSERT_EQ(rules.size(), 6U);
  std::vector<RuleCode> codes;
  for (const Rule& r : rules) {
    codes.push_back(r.code);
  }
  const std::vector<RuleCode> expected{RuleCode::kOpenOrders, RuleCode::kPosition,  RuleCode::kExposure,
                                       RuleCode::kGreeks,     RuleCode::kLossDaily, RuleCode::kDrawdown};
  EXPECT_EQ(codes, expected);
  ScopeDesk d;
  const SimClock clock{1};
  EXPECT_TRUE(RiskGate::create(d.all_rules, clock).has_value());
  EXPECT_FALSE(RiskGate::create(d.all_rules, clock).value().complete());  // 18, 21 and 22 come in M2.4b
}

// ---- R-OPEN-ORDERS ----

TEST(ROpenOrders, TheOrderThatReachesTheLimitPassesAndTheNextIsRefused) {
  ScopeDesk d;
  d.clear_limits(LimitKey::kMaxOpenOrders);
  d.limit(LimitKey::kMaxOpenOrders, 3 * kFromScale);
  ASSERT_TRUE(d.sb.add_open_orders(d.who(), 2));
  EXPECT_PASS(d, kOpenOrders, d.order());  // the third
  ASSERT_TRUE(d.sb.add_open_orders(d.who(), 1));
  const Outcome out = d.check(RuleCode::kOpenOrders, d.order());
  EXPECT_EQ(out.verdict, Verdict::kReject);
  EXPECT_EQ(out.limit, 3 * kFromScale);
  EXPECT_EQ(out.current, 4 * kFromScale);
}

TEST(ROpenOrders, EachRowIsJudgedAgainstItsOwnScope) {
  ScopeDesk d;
  d.limit(LimitKey::kMaxOpenOrders, 2 * kFromScale, LimitScope::kAccount, true, kAccount);
  ASSERT_TRUE(d.sb.add_open_orders(d.who(), 2));
  ASSERT_TRUE(d.sb.add_open_orders({.account = kOtherAccount}, 10));  // the desk's 12 is under its 50
  EXPECT_REJECT(d, kOpenOrders, d.order());                           // the account's own 3rd is not
  OrderDetails other = d.order();
  other.account_id = kOtherAccount;
  EXPECT_PASS(d, kOpenOrders, other);  // the account row isn't this account's
}

TEST(ROpenOrders, ATraderRowCountsTheTradersOrdersAcrossAccounts) {
  ScopeDesk d;
  d.limit(LimitKey::kMaxOpenOrders, 3 * kFromScale, LimitScope::kTrader, true, kUser);
  ASSERT_TRUE(d.sb.add_open_orders({.account = kAccount, .trader = kUser}, 2));
  ASSERT_TRUE(d.sb.add_open_orders({.account = kOtherAccount, .trader = kUser}, 1));
  EXPECT_REJECT(d, kOpenOrders, d.order());
}

TEST(ROpenOrders, AStrategyRowDoesntApplyToAnOrderWithNoStrategy) {
  ScopeDesk d;
  d.limit(LimitKey::kMaxOpenOrders, 0, LimitScope::kStrategy, true, kStrategy);
  EXPECT_PASS(d, kOpenOrders, d.order());
  OrderDetails tagged = d.order();
  tagged.strategy_id = kStrategy;
  EXPECT_REJECT(d, kOpenOrders, tagged);
}

TEST(ROpenOrders, AnAccountRowAndAStrategyRowAreMeasuredAtTheirOwnScopeNotTheDesk) {
  ScopeDesk d;
  ASSERT_TRUE(d.sb.add_open_orders({.account = kOtherAccount}, 10));  // the desk has 10, this account none
  d.limit(LimitKey::kMaxOpenOrders, 3 * kFromScale, LimitScope::kAccount, true, kAccount);
  EXPECT_PASS(d, kOpenOrders, d.order());
  d.limit(LimitKey::kMaxOpenOrders, 3 * kFromScale, LimitScope::kStrategy, true, kStrategy);
  OrderDetails tagged = d.order();
  tagged.strategy_id = kStrategy;
  EXPECT_PASS(d, kOpenOrders, tagged);
}

TEST(ROpenOrders, AHardBreachWinsOverASoftOneWhicheverIsLoadedFirst) {
  ScopeDesk d;
  d.clear_limits(LimitKey::kMaxOpenOrders);
  d.limit(LimitKey::kMaxOpenOrders, 0, LimitScope::kDesk, false);
  d.limit(LimitKey::kMaxOpenOrders, 0, LimitScope::kDesk, true);
  EXPECT_REJECT(d, kOpenOrders, d.order());
}

TEST(ROpenOrders, AnInstrumentRowIsIgnoredAndIfItIsTheOnlyRowTheOrderIsRefused) {
  ScopeDesk d;
  d.limit(LimitKey::kMaxOpenOrders, 0, LimitScope::kInstrument, true, {}, kEquity);
  EXPECT_PASS(d, kOpenOrders, d.order());  // the desk's 50 is the only row that counts
  d.clear_limits(LimitKey::kMaxOpenOrders);
  d.limit(LimitKey::kMaxOpenOrders, 100 * kFromScale, LimitScope::kInstrument, true, {}, kEquity);
  EXPECT_REJECT(d, kOpenOrders, d.order());
}

TEST(ROpenOrders, ASoftLimitWarnsAndNoLimitAtAllRefuses) {
  ScopeDesk d;
  d.limit(LimitKey::kMaxOpenOrders, 0, LimitScope::kDesk, false);
  EXPECT_WARN(d, kOpenOrders, d.order());
  d.clear_limits(LimitKey::kMaxOpenOrders);
  EXPECT_REJECT(d, kOpenOrders, d.order());
}

// ---- R-POSITION ----

TEST(RPosition, TheLimitIsOnTheProjectedNetInLots) {
  ScopeDesk d;
  d.limit(LimitKey::kMaxPositionLots, 20 * kFromScale);
  d.fill(Side::kBuy, 15, rupees(100));
  EXPECT_PASS(d, kPosition, d.buy(5));  // 20
  const Outcome out = d.check(RuleCode::kPosition, d.buy(6));
  EXPECT_EQ(out.verdict, Verdict::kReject);
  EXPECT_EQ(out.limit, 20 * kFromScale);
  EXPECT_EQ(out.current, 21 * kFromScale);
  EXPECT_PASS(d, kPosition, d.sell(30));  // net -15
  EXPECT_REJECT(d, kPosition, d.sell(36));
}

TEST(RPosition, WorkingOrdersOnTheSameSideCountAndTheOppositeSideDoesNot) {
  ScopeDesk d;
  d.limit(LimitKey::kMaxPositionLots, 20 * kFromScale);
  d.fill(Side::kBuy, 10, rupees(100));
  ASSERT_TRUE(d.sb.add_working(d.who(), kEquity, Side::kBuy, 6));
  ASSERT_TRUE(d.sb.add_working(d.who(), kEquity, Side::kSell, 100));
  EXPECT_PASS(d, kPosition, d.buy(4));
  EXPECT_REJECT(d, kPosition, d.buy(5));
}

TEST(RPosition, LotsAreCountedInTheInstrumentsOwnLots) {
  ScopeDesk d;
  d.limit(LimitKey::kMaxPositionLots, 2 * kFromScale);
  d.fresh_greeks();
  d.fill(Side::kBuy, 130, rupees(10), kOption);
  EXPECT_REJECT(d, kPosition, d.option_order(Side::kBuy, 65, false));  // 3 lots
  EXPECT_PASS(d, kPosition, d.option_order(Side::kSell, 65, false));
}

TEST(RPosition, AnInstrumentRowIsMeasuredAcrossTheDesk) {
  ScopeDesk d;
  d.limit(LimitKey::kMaxPositionLots, 20 * kFromScale, LimitScope::kInstrument, true, {}, kEquity);
  d.fill(Side::kBuy, 15, rupees(100), kEquity, {.account = kOtherAccount});
  EXPECT_PASS(d, kPosition, d.buy(5));
  EXPECT_REJECT(d, kPosition, d.buy(6));  // this account holds none: the desk's 15 and 6 are 21
}

TEST(RPosition, EachScopeHasItsOwnLimitAndTheTraderIsAggregatedAcrossAccounts) {
  ScopeDesk d;
  d.limit(LimitKey::kMaxPositionLots, 12 * kFromScale, LimitScope::kTrader, true, kUser);
  d.fill(Side::kBuy, 8, rupees(100), kEquity, {.account = kOtherAccount, .trader = kUser});
  EXPECT_PASS(d, kPosition, d.buy(4));
  EXPECT_REJECT(d, kPosition, d.buy(5));  // 8 on the other account and 5 here are the trader's 13
}

TEST(RPosition, NotionalIsPerUnderlyingAcrossItsFuturesAndShares) {
  ScopeDesk d;
  d.limit(LimitKey::kMaxPositionNotional, rupees(2'000).units());
  d.fill(Side::kBuy, 10, rupees(100), kEquity);
  d.fill(Side::kBuy, 8, rupees(100), kFuture);
  ASSERT_TRUE(d.sb.set_mark(kFuture, rupees(100)));
  EXPECT_PASS(d, kPosition, d.buy(2));  // 1800 held and 200 more
  const Outcome out = d.check(RuleCode::kPosition, d.buy(3));
  EXPECT_EQ(out.verdict, Verdict::kReject);
  EXPECT_EQ(out.current, rupees(2'100).units());
}

TEST(RPosition, AnOptionCountsAtDeltaNotionalFromItsGreeks) {
  ScopeDesk d;
  d.limit(LimitKey::kMaxPositionNotional, rupees(3'000).units());
  d.fresh_greeks();                                                  // 50 rupees of delta a unit
  EXPECT_PASS(d, kPosition, d.option_order(Side::kSell, 59, true));  // 2,950
  EXPECT_REJECT(d, kPosition, d.option_order(Side::kSell, 61, true));
}

TEST(RPosition, AFuturesOrderIsCountedAgainstItsUnderlyingsOtherPositions) {
  ScopeDesk d;
  d.limit(LimitKey::kMaxPositionNotional, rupees(1'500).units());
  d.fill(Side::kBuy, 10, rupees(100), kEquity);  // 1,000 on the underlying
  OrderDetails future = d.buy(5);
  future.instrument_id = kFuture;
  EXPECT_PASS(d, kPosition, future);  // 1,500
  future.qty = 6;
  EXPECT_REJECT(d, kPosition, future);
}

TEST(RPosition, WithNoPositionsLoadedItNamesThemAndNoLimitRefuses) {
  ScopeDesk d;
  d.sb.set_positions_loaded(false);
  EXPECT_TRUE(names(d.check(RuleCode::kPosition, d.order()), DataSource::kPositions));
  d.sb.set_positions_loaded(true);
  d.clear_limits(LimitKey::kMaxPositionLots);
  EXPECT_REJECT(d, kPosition, d.order());
}

// ---- R-EXPOSURE ----

TEST(RExposure, GrossPlusTheOrderMustStayUnderTheLimit) {
  ScopeDesk d;
  d.limit(LimitKey::kMaxGrossExposure, rupees(1'500).units());
  d.fill(Side::kBuy, 10, rupees(100));
  EXPECT_PASS(d, kExposure, d.buy(5));
  const Outcome out = d.check(RuleCode::kExposure, d.buy(6));
  EXPECT_EQ(out.verdict, Verdict::kReject);
  EXPECT_EQ(out.current, rupees(1'600).units());
}

TEST(RExposure, ASellAddsToGrossToo) {
  ScopeDesk d;
  d.limit(LimitKey::kMaxGrossExposure, rupees(1'500).units());
  d.fill(Side::kBuy, 10, rupees(100));
  EXPECT_REJECT(d, kExposure, d.sell(6));  // an upper bound: worst case
}

TEST(RExposure, AnOrderIsValuedAtTheWorstOfItsPriceItsTriggerAndTheLtp) {
  ScopeDesk d;
  d.limit(LimitKey::kMaxGrossExposure, rupees(1'050).units());
  OrderDetails stop = d.buy(10);
  stop.order_type = OrderType::kSlM;
  stop.price.reset();
  stop.trigger_price = rupees(104);
  EXPECT_PASS(d, kExposure, stop);  // 1,040
  stop.trigger_price = rupees(106);
  EXPECT_REJECT(d, kExposure, stop);
  OrderDetails low_sell = d.sell(10);
  low_sell.price = rupees(90);  // fills at 90 or better, so up to the LTP
  EXPECT_PASS(d, kExposure, low_sell);
  d.clear_limits(LimitKey::kMaxGrossExposure);
  d.limit(LimitKey::kMaxGrossExposure, rupees(950).units());
  EXPECT_REJECT(d, kExposure, low_sell);  // 1,000 at the LTP, not 900
  OrderDetails market = d.buy(10);
  market.order_type = OrderType::kMarket;
  market.price.reset();
  EXPECT_REJECT(d, kExposure, market);  // valued at the LTP
}

TEST(RExposure, EachScopeIsMeasuredSeparately) {
  ScopeDesk d;
  d.limit(LimitKey::kMaxGrossExposure, rupees(1'200).units(), LimitScope::kTrader, true, kUser);
  d.fill(Side::kBuy, 10, rupees(100), kEquity, {.account = kOtherAccount});  // not this trader's
  EXPECT_PASS(d, kExposure, d.buy(12));
  EXPECT_REJECT(d, kExposure, d.buy(13));
}

// ---- R-GREEKS ----

TEST(RGreeks, ProjectedNetDeltaIsInRupeesAndAnOrderThatReducesItPasses) {
  ScopeDesk d;
  d.limit(LimitKey::kMaxNetDeltaInr, rupees(1'500).units());
  d.fill(Side::kBuy, 10, rupees(100));  // delta 1,000
  EXPECT_PASS(d, kGreeks, d.buy(5));
  EXPECT_REJECT(d, kGreeks, d.buy(6));
  EXPECT_PASS(d, kGreeks, d.sell(10));  // flat
}

TEST(RGreeks, ANetShortCountsByItsSizeNotItsSign) {
  ScopeDesk d;
  d.limit(LimitKey::kMaxNetDeltaInr, rupees(1'500).units());
  d.fill(Side::kSell, 10, rupees(100));
  EXPECT_PASS(d, kGreeks, d.sell(5));
  EXPECT_REJECT(d, kGreeks, d.sell(6));
}

TEST(RGreeks, VegaIsCheckedForOptionOrders) {
  ScopeDesk d;
  d.fresh_greeks();  // 2 rupees of vega a unit
  d.limit(LimitKey::kMaxNetVegaInr, rupees(100).units());
  EXPECT_PASS(d, kGreeks, d.option_order(Side::kSell, 50, true));
  const Outcome out = d.check(RuleCode::kGreeks, d.option_order(Side::kSell, 51, true));
  EXPECT_EQ(out.verdict, Verdict::kReject);
  EXPECT_EQ(out.limit, rupees(100).units());
  EXPECT_EQ(out.current, rupees(102).units());
}

TEST(RGreeks, NetDeltaIncludesTheOptionsHeld) {
  ScopeDesk d;
  d.fresh_greeks();
  d.fill(Side::kSell, 65, rupees(10), kOption);  // delta -3,250
  d.limit(LimitKey::kMaxNetDeltaInr, rupees(4'000).units());
  EXPECT_PASS(d, kGreeks, d.option_order(Side::kSell, 14, true));
  EXPECT_REJECT(d, kGreeks, d.option_order(Side::kSell, 16, true));
}

TEST(RGreeks, ALiveOptionOrderNeedsFreshGreeksAndNamesThemWhenItHasNone) {
  ScopeDesk d;
  EXPECT_TRUE(names(d.check(RuleCode::kGreeks, d.option_order(Side::kSell, 10, true)), DataSource::kGreeks));
  d.fresh_greeks();
  EXPECT_PASS(d, kGreeks, d.option_order(Side::kSell, 10, true));
}

TEST(RGreeks, GreeksAreStaleAfterTheConfiguredAge) {
  ScopeDesk d;
  d.ctx.greeks_max_age = 2 * kNanosPerSecond;
  d.fresh_greeks();
  d.clock.advance(2 * kNanosPerSecond);
  EXPECT_PASS(d, kGreeks, d.option_order(Side::kSell, 10, true));  // at the age
  d.clock.advance(1);
  EXPECT_TRUE(names(d.check(RuleCode::kGreeks, d.option_order(Side::kSell, 10, true)), DataSource::kGreeks));
  d.clock.set(d.clock.now());
  d.fresh_greeks();
  EXPECT_PASS(d, kGreeks, d.option_order(Side::kSell, 10, true));
}

TEST(RGreeks, AHeldOptionWithStaleGreeksBlocksALiveOptionOrderButNotAnEquityOne) {
  ScopeDesk d;
  d.fill(Side::kSell, 65, rupees(10), kOption);
  ASSERT_TRUE(
      d.sb.set_greeks(kOption, {.delta = 50 * kRupee, .vega = 2 * kRupee, .as_of = d.clock.monotonic()}));
  d.clock.advance(20 * kNanosPerSecond);  // past the 15 s default
  ASSERT_TRUE(
      d.sb.set_greeks(kOption2, {.delta = 50 * kRupee, .vega = 2 * kRupee, .as_of = d.clock.monotonic()}));
  OrderDetails second = d.option_order(Side::kSell, 65, true);
  second.instrument_id = kOption2;
  EXPECT_TRUE(names(d.check(RuleCode::kGreeks, second), DataSource::kGreeks));  // the held one is old
  EXPECT_PASS(d, kGreeks, d.live_order());  // an equity order goes on the last Greeks known
  ASSERT_TRUE(
      d.sb.set_greeks(kOption, {.delta = 50 * kRupee, .vega = 2 * kRupee, .as_of = d.clock.monotonic()}));
  EXPECT_PASS(d, kGreeks, second);
}

TEST(RGreeks, HeldGreeksThatPredateAPauseBlockALiveOptionOrder) {
  ScopeDesk d;
  d.fill(Side::kSell, 65, rupees(10), kOption);
  d.fresh_greeks();
  d.ctx.stale_before = d.clock.monotonic();
  d.clock.advance(1);
  ASSERT_TRUE(d.sb.set_greeks(kOption2, {.delta = kRupee, .vega = kRupee, .as_of = d.clock.monotonic()}));
  OrderDetails second = d.option_order(Side::kSell, 65, true);
  second.instrument_id = kOption2;
  EXPECT_TRUE(names(d.check(RuleCode::kGreeks, second), DataSource::kGreeks));
}

// The desk is flat, but this account is long and another is short: its totals still carry the old Greeks.
TEST(RGreeks, OffsettingAccountsDontHideAStaleHeldOption) {
  ScopeDesk d;
  d.fill(Side::kBuy, 65, rupees(10), kOption);
  d.fill(Side::kSell, 65, rupees(10), kOption, {.account = kOtherAccount});
  ASSERT_TRUE(
      d.sb.set_greeks(kOption, {.delta = 50 * kRupee, .vega = 2 * kRupee, .as_of = d.clock.monotonic()}));
  d.clock.advance(600 * kNanosPerSecond);
  ASSERT_TRUE(
      d.sb.set_greeks(kOption2, {.delta = 50 * kRupee, .vega = 2 * kRupee, .as_of = d.clock.monotonic()}));
  OrderDetails second = d.option_order(Side::kSell, 65, true);
  second.instrument_id = kOption2;
  EXPECT_TRUE(names(d.check(RuleCode::kGreeks, second), DataSource::kGreeks));
}

TEST(RGreeks, GreeksFromTheFutureAreNotFresh) {
  ScopeDesk d;
  ASSERT_TRUE(d.sb.set_greeks(kOption, {.delta = kRupee, .vega = kRupee, .as_of = d.clock.monotonic() + 1}));
  EXPECT_TRUE(names(d.check(RuleCode::kGreeks, d.option_order(Side::kSell, 10, true)), DataSource::kGreeks));
}

TEST(RGreeks, GreeksThatPredateAPauseAreStaleHoweverYoungTheyLook) {
  ScopeDesk d;
  d.fresh_greeks();
  d.ctx.stale_before = d.clock.monotonic();
  EXPECT_TRUE(names(d.check(RuleCode::kGreeks, d.option_order(Side::kSell, 10, true)), DataSource::kGreeks));
  d.clock.advance(1);
  d.fresh_greeks();
  EXPECT_PASS(d, kGreeks, d.option_order(Side::kSell, 10, true));
}

TEST(RGreeks, APaperOptionOrderSkipsGreeksAndCountsNothingForThem) {
  ScopeDesk d;
  d.limit(LimitKey::kMaxNetVegaInr, 0);
  EXPECT_PASS(d, kGreeks, d.option_order(Side::kSell, 10, false));
}

TEST(RGreeks, AnOptionHeldWithoutGreeksMakesTheAggregatesUntrustworthyForLiveOrders) {
  ScopeDesk d;
  ASSERT_TRUE(d.sb.set_mark(kOption, rupees(10)));
  d.fill(Side::kSell, 65, rupees(10), kOption);  // no Greeks have arrived for it
  EXPECT_TRUE(names(d.check(RuleCode::kGreeks, d.live_order()), DataSource::kGreeks));
  EXPECT_TRUE(names(d.check(RuleCode::kExposure, d.live_order()), DataSource::kGreeks));
  EXPECT_TRUE(names(d.check(RuleCode::kPosition, d.live_order()), DataSource::kGreeks));
  EXPECT_PASS(d, kGreeks, d.order());  // a paper order skips Greeks
  d.fresh_greeks();
  EXPECT_PASS(d, kGreeks, d.live_order());
}

// ---- R-LOSS-DAILY and R-DRAWDOWN (D-30) ----

TEST(RLossDaily, ALossThatHasReachedTheLimitBlocksAndOneUnderItDoesNot) {
  ScopeDesk d;
  d.limit(LimitKey::kMaxDailyLoss, rupees(100).units(), LimitScope::kDesk, true);
  d.clear_limits(LimitKey::kMaxDailyLoss);
  d.limit(LimitKey::kMaxDailyLoss, rupees(100).units());
  d.fill(Side::kBuy, 10, rupees(100));
  ASSERT_TRUE(d.sb.set_mark(kEquity, rupees(91)));  // -90
  EXPECT_PASS(d, kLossDaily, d.order());
  ASSERT_TRUE(d.sb.set_mark(kEquity, rupees(90)));  // -100: reached
  const Outcome out = d.check(RuleCode::kLossDaily, d.order());
  EXPECT_EQ(out.verdict, Verdict::kReject);
  EXPECT_EQ(out.limit, rupees(100).units());
  EXPECT_EQ(out.current, rupees(100).units());
}

TEST(RLossDaily, PnlIsRealisedPlusMarkToMarketMinusCosts) {
  ScopeDesk d;
  d.clear_limits(LimitKey::kMaxDailyLoss);
  d.limit(LimitKey::kMaxDailyLoss, rupees(100).units());
  ASSERT_TRUE(d.sb.apply_fill({.who = d.who(),
                               .instrument_id = kEquity,
                               .side = Side::kBuy,
                               .qty = 10,
                               .price = rupees(100),
                               .cost = rupees(60).units()}));
  ASSERT_TRUE(d.sb.set_mark(kEquity, rupees(100)));
  EXPECT_PASS(d, kLossDaily, d.order());  // -60 of costs
  d.fill(Side::kSell, 10, rupees(96));    // -40 realised: -100 in all
  EXPECT_REJECT(d, kLossDaily, d.order());
}

TEST(RLossDaily, AProfitAndAnEmptyBookPass) {
  ScopeDesk d;
  d.clear_limits(LimitKey::kMaxDailyLoss);
  d.limit(LimitKey::kMaxDailyLoss, rupees(100).units());
  EXPECT_PASS(d, kLossDaily, d.order());
  d.fill(Side::kBuy, 10, rupees(100));
  ASSERT_TRUE(d.sb.set_mark(kEquity, rupees(150)));
  EXPECT_PASS(d, kLossDaily, d.order());
}

TEST(RLossDaily, EachScopeIsJudgedOnItsOwnLoss) {
  ScopeDesk d;
  d.limit(LimitKey::kMaxDailyLoss, rupees(100).units(), LimitScope::kTrader, true, kUser);
  d.fill(Side::kBuy, 10, rupees(100), kEquity, {.account = kOtherAccount, .trader = id_of(7)});
  ASSERT_TRUE(d.sb.set_mark(kEquity, rupees(80)));  // another trader loses 200 on it
  EXPECT_PASS(d, kLossDaily, d.order());            // this trader has none
  d.fill(Side::kBuy, 10, rupees(100));
  EXPECT_REJECT(d, kLossDaily, d.order());
}

TEST(RDrawdown, ThePeakMinusNowAtTheLimitBlocks) {
  ScopeDesk d;
  d.limit(LimitKey::kMaxDrawdownFromPeak, rupees(50).units(), LimitScope::kDesk, true);
  d.clear_limits(LimitKey::kMaxDrawdownFromPeak);
  d.limit(LimitKey::kMaxDrawdownFromPeak, rupees(50).units());
  d.fill(Side::kBuy, 10, rupees(100));
  ASSERT_TRUE(d.sb.set_mark(kEquity, rupees(110)));  // peak +100
  ASSERT_TRUE(d.sb.set_mark(kEquity, rupees(106)));  // down 40
  EXPECT_PASS(d, kDrawdown, d.order());
  ASSERT_TRUE(d.sb.set_mark(kEquity, rupees(105)));  // down 50
  const Outcome out = d.check(RuleCode::kDrawdown, d.order());
  EXPECT_EQ(out.verdict, Verdict::kReject);
  EXPECT_EQ(out.current, rupees(50).units());
}

// D-30: "daily" is what happened since the session began, not since the book was built.
TEST(RLossDaily, YesterdaysProfitDoesNotHideTodaysLoss) {
  ScopeDesk d;
  d.clear_limits(LimitKey::kMaxDailyLoss);
  d.limit(LimitKey::kMaxDailyLoss, rupees(100).units());
  d.fill(Side::kBuy, 10, rupees(100));
  ASSERT_TRUE(d.sb.set_mark(kEquity, rupees(200)));  // +1,000 carried in
  d.sb.begin_session();
  EXPECT_PASS(d, kLossDaily, d.order());
  ASSERT_TRUE(d.sb.set_mark(kEquity, rupees(190)));  // -100 today, +900 in all
  EXPECT_REJECT(d, kLossDaily, d.order());
}

TEST(RLossDaily, AGainBeforeTheSessionDoesntCountAgainstTheDrawdownEither) {
  ScopeDesk d;
  d.clear_limits(LimitKey::kMaxDrawdownFromPeak);
  d.limit(LimitKey::kMaxDrawdownFromPeak, rupees(50).units());
  d.fill(Side::kBuy, 10, rupees(100));
  ASSERT_TRUE(d.sb.set_mark(kEquity, rupees(200)));
  d.sb.begin_session();
  ASSERT_TRUE(d.sb.set_mark(kEquity, rupees(197)));  // down 30 from today's peak of nothing
  EXPECT_PASS(d, kDrawdown, d.order());
  ASSERT_TRUE(d.sb.set_mark(kEquity, rupees(195)));  // down 50
  EXPECT_REJECT(d, kDrawdown, d.order());
}

TEST(RDrawdown, ANewDayStartsFromTheCurrentPnl) {
  ScopeDesk d;
  d.clear_limits(LimitKey::kMaxDrawdownFromPeak);
  d.limit(LimitKey::kMaxDrawdownFromPeak, rupees(50).units());
  d.fill(Side::kBuy, 10, rupees(100));
  ASSERT_TRUE(d.sb.set_mark(kEquity, rupees(110)));
  ASSERT_TRUE(d.sb.set_mark(kEquity, rupees(100)));
  EXPECT_REJECT(d, kDrawdown, d.order());  // down 100 from the peak
  d.sb.begin_session();
  EXPECT_PASS(d, kDrawdown, d.order());
}

// ---- the data each rule needs (D-27) ----

TEST(DataMissing, WithNoScopeBookEveryRuleNamesThePositions) {
  ScopeDesk d;
  d.ctx.scopes = nullptr;
  for (const RuleCode code :
       {RuleCode::kOpenOrders, RuleCode::kPosition, RuleCode::kExposure, RuleCode::kGreeks}) {
    EXPECT_TRUE(names(d.check(code, d.live_order()), DataSource::kPositions)) << static_cast<int>(code);
    EXPECT_TRUE(names(d.check(code, d.order()), DataSource::kPositions))
        << static_cast<int>(code);  // paper too
  }
}

TEST(DataMissing, AnUnhealthyBookRefusesEveryRuleThatNeedsIt) {
  ScopeDesk d;
  EXPECT_FALSE(d.sb.add_working(d.who(), kEquity, Side::kSell, -1));  // more taken off than was put on
  ASSERT_FALSE(d.sb.healthy());
  EXPECT_TRUE(names(d.check(RuleCode::kOpenOrders, d.order()), DataSource::kPositions));
  EXPECT_TRUE(names(d.check(RuleCode::kLossDaily, d.order()), DataSource::kPnl));
  const GateDecision out = d.run_all(d.sell(1));  // opening as far as the gate can tell
  ASSERT_TRUE(out.rejection.has_value());
  EXPECT_EQ(reason_text(out.rejection.value()), "R-DATA-MISSING:positions");
}

TEST(DataMissing, PnlNotLoadedOrAPositionWithNoMarkNamesThePnlForPaperAndLive) {
  ScopeDesk d;
  d.sb.set_pnl_loaded(false);
  for (const RuleCode code : {RuleCode::kLossDaily, RuleCode::kDrawdown}) {
    EXPECT_TRUE(names(d.check(code, d.live_order()), DataSource::kPnl));
    EXPECT_TRUE(names(d.check(code, d.order()), DataSource::kPnl));
  }
  d.sb.set_pnl_loaded(true);
  EXPECT_PASS(d, kLossDaily, d.order());
  d.fill(Side::kBuy, 5, rupees(100), kFuture);  // no mark for the future
  EXPECT_TRUE(names(d.check(RuleCode::kLossDaily, d.order()), DataSource::kPnl));
  ASSERT_TRUE(d.sb.set_mark(kFuture, rupees(100)));
  EXPECT_PASS(d, kLossDaily, d.order());
}

// For each source: a live opening order is refused naming it, and a reducing order still passes the gate.
TEST(DataMissing, ALiveOpeningOrderIsRefusedNamingTheSourceAndAReducingOrderStillPasses) {
  {
    ScopeDesk d;  // positions
    d.sb.set_positions_loaded(false);
    const Outcome out = d.check(RuleCode::kOpenOrders, d.live_order());
    EXPECT_TRUE(names(out, DataSource::kPositions));
    // With none loaded nothing can be known to reduce, so every order is opening (D-27): the exception for
    // reducing orders can't apply here.
    const GateDecision refused = d.run_all(d.sell(1));
    ASSERT_TRUE(refused.rejection.has_value());
    EXPECT_EQ(reason_text(refused.rejection.value()), "R-DATA-MISSING:positions");
  }
  {
    ScopeDesk d;  // pnl
    d.fill(Side::kBuy, 10, rupees(100));
    d.sb.set_pnl_loaded(false);
    EXPECT_TRUE(names(d.check(RuleCode::kLossDaily, d.live_order()), DataSource::kPnl));
    const GateDecision refused = d.run_all(d.buy(1));
    ASSERT_TRUE(refused.rejection.has_value());
    EXPECT_EQ(reason_text(refused.rejection.value()), "R-DATA-MISSING:pnl");
    EXPECT_TRUE(d.run_all(d.sell(4)).approved.has_value());  // reducing
  }
  {
    ScopeDesk d;  // Greeks
    d.fill(Side::kBuy, 130, rupees(10), kOption);
    EXPECT_TRUE(names(d.check(RuleCode::kGreeks, d.option_order(Side::kBuy, 65, true)), DataSource::kGreeks));
    EXPECT_TRUE(d.run_all(d.option_order(Side::kSell, 65, false)).approved.has_value());  // reducing, paper
  }
}

TEST(DataMissing, TheReasonTextNamesTheSource) {
  RuleResult result{
      .code = RuleCode::kGreeks, .verdict = Verdict::kReject, .reason = RuleReason::kDataMissing};
  const std::array<std::pair<DataSource, std::string_view>, 5> names_of{{
      {DataSource::kPositions, "R-DATA-MISSING:positions"},
      {DataSource::kPnl, "R-DATA-MISSING:pnl"},
      {DataSource::kMargin, "R-DATA-MISSING:margin"},
      {DataSource::kGreeks, "R-DATA-MISSING:greeks"},
      {DataSource::kNone, "R-DATA-MISSING"},
  }};
  for (const auto& [source, text] : names_of) {
    result.source = source;
    EXPECT_EQ(reason_text(result), text);
  }
}

// ---- which orders are opening (D-29) ----

// With a limit of 0 open orders every opening order is refused by rule 14, so an approval means reducing.
struct Opening : ScopeDesk {
  Opening() {
    clear_limits(LimitKey::kMaxOpenOrders);
    limit(LimitKey::kMaxOpenOrders, 0);
  }
  [[nodiscard]] bool opening(const OrderDetails& o) {
    const GateDecision out = run_all(o);
    if (out.approved.has_value()) {
      return false;
    }
    EXPECT_TRUE(out.rejection.has_value());
    EXPECT_EQ(out.rejection.value().code, RuleCode::kOpenOrders);
    return true;
  }
};

TEST(Opening, ABuyOnALongAndAnyOrderOnAFlatBookIsOpening) {
  Opening d;
  EXPECT_TRUE(d.opening(d.buy(1)));
  EXPECT_TRUE(d.opening(d.sell(2)));
  d.fill(Side::kBuy, 10, rupees(100));
  EXPECT_TRUE(d.opening(d.buy(3)));
}

TEST(Opening, ASellAgainstALongReducesUntilItIsFlatAndAnythingPastFlatIsOpening) {
  Opening d;
  d.fill(Side::kBuy, 4, rupees(100));
  EXPECT_FALSE(d.opening(d.sell(2)));
  EXPECT_FALSE(d.opening(d.sell(4)));  // closes it
  EXPECT_TRUE(d.opening(d.sell(5)));   // lands at -1: a flip, opening as a whole order however small
  EXPECT_TRUE(d.opening(d.sell(8)));   // lands at -4: as large as the long was
  EXPECT_TRUE(d.opening(d.sell(9)));   // lands at -5
}

TEST(Opening, ABuyAgainstAShortFlipsTheSameWay) {
  Opening d;
  d.fill(Side::kSell, 4, rupees(100));
  EXPECT_FALSE(d.opening(d.buy(3)));
  EXPECT_FALSE(d.opening(d.buy(4)));
  EXPECT_TRUE(d.opening(d.buy(5)));
  EXPECT_TRUE(d.opening(d.buy(9)));
}

TEST(Opening, WorkingExitsCountTowardsTheReduction) {
  Opening d;
  d.fill(Side::kBuy, 10, rupees(100));
  ASSERT_TRUE(d.sb.add_working(d.who(), kEquity, Side::kSell, 6));
  EXPECT_FALSE(d.opening(d.sell(4)));  // 6 and 4 close it
  EXPECT_FALSE(d.opening(d.sell(3)));  // 9 leaves 1 long
  EXPECT_TRUE(d.opening(d.sell(6)));   // 12 would leave -2: past flat
  EXPECT_TRUE(d.opening(d.sell(14)));  // 20 would leave -10
  EXPECT_TRUE(d.opening(d.sell(15)));  // 21 would leave -11
}

TEST(Opening, AStackOfExitsThatWouldFlipThePositionStopsBeingExempt) {
  Opening d;
  d.fill(Side::kBuy, 10, rupees(100));
  ASSERT_TRUE(d.sb.add_working(d.who(), kEquity, Side::kSell, 8));
  EXPECT_FALSE(d.opening(d.sell(2)));  // 10 closes it
  EXPECT_TRUE(d.opening(d.sell(3)));   // 11 would leave -1
  EXPECT_TRUE(d.opening(d.sell(8)));   // 16 would leave -6
  EXPECT_TRUE(d.opening(d.sell(14)));  // -12
}

TEST(Opening, OnlyTheSameSideWorkingOrdersAreCounted) {
  Opening d;
  d.fill(Side::kSell, 10, rupees(100));
  ASSERT_TRUE(d.sb.add_working(d.who(), kEquity, Side::kSell, 50));  // more shorting working
  EXPECT_FALSE(d.opening(d.buy(4)));                                 // a buy reduces the short
  EXPECT_TRUE(d.opening(d.sell(1)));
}

TEST(Opening, TheAccountsPositionDecidesNotTheTraders) {
  Opening d;
  d.fill(Side::kBuy, 10, rupees(100), kEquity, {.account = kAccount});  // reconciled: no trader
  EXPECT_FALSE(d.opening(d.sell(4)));
  d.fill(Side::kBuy, 5, rupees(100), kEquity, {.account = kOtherAccount, .trader = kUser});
  EXPECT_FALSE(d.opening(d.sell(5)));  // the other account's 5 are not this one's to sell
}

TEST(Opening, ARejectedOrderLeavesNoWorkingState) {
  Opening d;
  EXPECT_TRUE(d.opening(d.buy(1)));
  EXPECT_EQ(d.sb.position(LimitScope::kAccount, kAccount, kEquity), nullptr);  // the gate only reads the book
}

// ---- RISKS_GREEKS_MAX_AGE_MS ----

TEST(GreeksMaxAge, ParsesWholeMillisecondsInRange) {
  EXPECT_EQ(parse_greeks_max_age_ms("").value_or(0), kDefaultGreeksMaxAge);
  EXPECT_EQ(parse_greeks_max_age_ms("15000").value_or(0), 15 * kNanosPerSecond);
  EXPECT_EQ(parse_greeks_max_age_ms("1").value_or(0), 1'000'000);
  EXPECT_EQ(parse_greeks_max_age_ms("300000").value_or(0), 300 * kNanosPerSecond);
  for (const std::string_view bad :
       {"0", "-5", "300001", "15s", " 15", "1.5", "abc", "99999999999999999999"}) {
    EXPECT_FALSE(parse_greeks_max_age_ms(bad).has_value()) << bad;
  }
}

TEST(GreeksMaxAge, TheDefaultIsFifteenSeconds) {
  EXPECT_EQ(kDefaultGreeksMaxAge, 15 * kNanosPerSecond);
  EXPECT_EQ(GateContext{}.greeks_max_age, kDefaultGreeksMaxAge);
}

}  // namespace
}  // namespace vyuha::risk
// NOLINTEND(bugprone-unchecked-optional-access)
