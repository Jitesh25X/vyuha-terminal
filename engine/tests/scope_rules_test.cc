// Rules 14 to 17, 19 and 20 (M2.4a): judged against a scope's own totals. One group per rule, then the data
// each needs (D-27, D-28) and which orders count as opening (D-29).

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "desk_fixture.h"
#include "scope_desk.h"
#include "vyuha/risk/scope_book.h"

// NOLINTBEGIN(bugprone-unchecked-optional-access): every test does ASSERT_TRUE(has_value()) first
namespace vyuha::risk {
namespace {

TEST(ScopeRules, TheTableIsRules14To17And19And20InOrder) {
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
  EXPECT_FALSE(RiskGate::create(pre_trade_rules(), clock).value().complete());
  EXPECT_TRUE(RiskGate::create(d.chain, clock).value().complete());  // 18, 21 and 22 are in M2.4b
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
  d.mark(kFuture, rupees(100));
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
  d.mark(kOption, rupees(10));
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
  d.mark(kOption, rupees(10));
  ASSERT_TRUE(
      d.sb.set_greeks(kOption, {.delta = 50 * kRupee, .vega = 2 * kRupee, .as_of = d.clock.monotonic()}));
  d.clock.advance(20 * kNanosPerSecond);  // past the 15 s default
  d.restamp_marks();                      // the marks are young: it is the Greeks that are old
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
  d.mark(kOption, rupees(10));
  d.fresh_greeks();
  d.ctx.stale_before = d.clock.monotonic();
  d.clock.advance(1);
  d.restamp_marks();
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
  d.mark(kOption, rupees(10));
  ASSERT_TRUE(
      d.sb.set_greeks(kOption, {.delta = 50 * kRupee, .vega = 2 * kRupee, .as_of = d.clock.monotonic()}));
  d.clock.advance(600 * kNanosPerSecond);
  d.restamp_marks();
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
  d.mark(kOption, rupees(10));
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
  d.mark(kEquity, rupees(91));  // -90
  EXPECT_PASS(d, kLossDaily, d.order());
  d.mark(kEquity, rupees(90));  // -100: reached
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
  d.mark(kEquity, rupees(100));
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
  d.mark(kEquity, rupees(150));
  EXPECT_PASS(d, kLossDaily, d.order());
}

TEST(RLossDaily, EachScopeIsJudgedOnItsOwnLoss) {
  ScopeDesk d;
  d.limit(LimitKey::kMaxDailyLoss, rupees(100).units(), LimitScope::kTrader, true, kUser);
  d.fill(Side::kBuy, 10, rupees(100), kEquity, {.account = kOtherAccount, .trader = id_of(7)});
  d.mark(kEquity, rupees(80));            // another trader loses 200 on it
  EXPECT_PASS(d, kLossDaily, d.order());  // this trader has none
  d.fill(Side::kBuy, 10, rupees(100));
  EXPECT_REJECT(d, kLossDaily, d.order());
}

TEST(RDrawdown, ThePeakMinusNowAtTheLimitBlocks) {
  ScopeDesk d;
  d.limit(LimitKey::kMaxDrawdownFromPeak, rupees(50).units(), LimitScope::kDesk, true);
  d.clear_limits(LimitKey::kMaxDrawdownFromPeak);
  d.limit(LimitKey::kMaxDrawdownFromPeak, rupees(50).units());
  d.fill(Side::kBuy, 10, rupees(100));
  d.mark(kEquity, rupees(110));  // peak +100
  d.mark(kEquity, rupees(106));  // down 40
  EXPECT_PASS(d, kDrawdown, d.order());
  d.mark(kEquity, rupees(105));  // down 50
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
  d.mark(kEquity, rupees(200));  // +1,000 carried in
  d.sb.begin_session();
  EXPECT_PASS(d, kLossDaily, d.order());
  d.mark(kEquity, rupees(190));  // -100 today, +900 in all
  EXPECT_REJECT(d, kLossDaily, d.order());
}

TEST(RLossDaily, AGainBeforeTheSessionDoesntCountAgainstTheDrawdownEither) {
  ScopeDesk d;
  d.clear_limits(LimitKey::kMaxDrawdownFromPeak);
  d.limit(LimitKey::kMaxDrawdownFromPeak, rupees(50).units());
  d.fill(Side::kBuy, 10, rupees(100));
  d.mark(kEquity, rupees(200));
  d.sb.begin_session();
  d.mark(kEquity, rupees(197));  // down 30 from today's peak of nothing
  EXPECT_PASS(d, kDrawdown, d.order());
  d.mark(kEquity, rupees(195));  // down 50
  EXPECT_REJECT(d, kDrawdown, d.order());
}

TEST(RDrawdown, ANewDayStartsFromTheCurrentPnl) {
  ScopeDesk d;
  d.clear_limits(LimitKey::kMaxDrawdownFromPeak);
  d.limit(LimitKey::kMaxDrawdownFromPeak, rupees(50).units());
  d.fill(Side::kBuy, 10, rupees(100));
  d.mark(kEquity, rupees(110));
  d.mark(kEquity, rupees(100));
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
  d.mark(kFuture, rupees(100));
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
    d.mark(kOption, rupees(10));
    EXPECT_TRUE(names(d.check(RuleCode::kGreeks, d.option_order(Side::kBuy, 65, true)), DataSource::kGreeks));
    EXPECT_TRUE(d.run_all(d.option_order(Side::kSell, 65, false)).approved.has_value());  // reducing, paper
  }
}

TEST(DataMissing, TheReasonTextNamesTheSource) {
  RuleResult result{
      .code = RuleCode::kGreeks, .verdict = Verdict::kReject, .reason = RuleReason::kDataMissing};
  const std::array<std::pair<DataSource, std::string_view>, 6> names_of{{
      {DataSource::kPositions, "R-DATA-MISSING:positions"},
      {DataSource::kPnl, "R-DATA-MISSING:pnl"},
      {DataSource::kMargin, "R-DATA-MISSING:margin"},
      {DataSource::kGreeks, "R-DATA-MISSING:greeks"},
      {DataSource::kMark, "R-DATA-MISSING:mark"},
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

// ---- marks have an age (owner, 2026-11-15) ----

// The five rules that read marks, each on its own.
constexpr std::array<RuleCode, 5> kMarkRules{RuleCode::kPosition, RuleCode::kExposure, RuleCode::kGreeks,
                                             RuleCode::kLossDaily, RuleCode::kDrawdown};

TEST(MarkAge, ALiveOpeningOrderIsRefusedWhenAHeldMarkIsOverThreeSecondsOldAndPassesAtExactlyThree) {
  for (const RuleCode code : kMarkRules) {
    ScopeDesk d;
    d.fill(Side::kBuy, 10, rupees(100));  // held; its mark arrived now
    d.clock.advance(3 * kNanosPerSecond);
    EXPECT_FALSE(names(d.check(code, d.live_order()), DataSource::kMark))
        << static_cast<int>(code);  // at 3 s
    EXPECT_EQ(d.verdict(code, d.live_order()), Verdict::kPass) << static_cast<int>(code);
    d.clock.advance(1);
    const Outcome out = d.check(code, d.live_order());
    EXPECT_TRUE(names(out, DataSource::kMark)) << static_cast<int>(code);  // 3 s and 1 ns
    d.restamp_marks();
    EXPECT_EQ(d.verdict(code, d.live_order()), Verdict::kPass) << static_cast<int>(code);
  }
}

TEST(MarkAge, TheMaximumAgeIsTheLtpsThreeSeconds) {
  EXPECT_EQ(kMarkMaxAge, kLtpMaxAge);
  EXPECT_EQ(kMarkMaxAge, 3 * kNanosPerSecond);
}

TEST(MarkAge, AMarkFromBeforeAPauseIsStaleHoweverYoungItLooks) {
  for (const RuleCode code : kMarkRules) {
    ScopeDesk d;
    d.fill(Side::kBuy, 10, rupees(100));
    d.ctx.stale_before = d.clock.monotonic();  // the mark arrived at the pause: not after it
    EXPECT_TRUE(names(d.check(code, d.live_order()), DataSource::kMark)) << static_cast<int>(code);
    d.clock.advance(1);
    d.restamp_marks();
    EXPECT_EQ(d.verdict(code, d.live_order()), Verdict::kPass) << static_cast<int>(code);
  }
}

TEST(MarkAge, AMarkStampedInTheFutureIsStale) {
  for (const RuleCode code : kMarkRules) {
    ScopeDesk d;
    d.fill(Side::kBuy, 10, rupees(100));
    ASSERT_TRUE(d.sb.set_mark(kEquity, rupees(100), d.clock.monotonic() + 1));
    EXPECT_TRUE(names(d.check(code, d.live_order()), DataSource::kMark)) << static_cast<int>(code);
  }
}

TEST(MarkAge, APaperOrderGoesOnTheMarksThereAre) {
  for (const RuleCode code : kMarkRules) {
    ScopeDesk d;
    d.fill(Side::kBuy, 10, rupees(100));
    d.clock.advance(60 * kNanosPerSecond);
    EXPECT_EQ(d.verdict(code, d.order()), Verdict::kPass) << static_cast<int>(code);
    d.ctx.stale_before = d.clock.monotonic();
    EXPECT_EQ(d.verdict(code, d.order()), Verdict::kPass) << static_cast<int>(code);
  }
}

TEST(MarkAge, WithNothingHeldNoMarkIsNeeded) {
  for (const RuleCode code : kMarkRules) {
    ScopeDesk d;
    d.clock.advance(600 * kNanosPerSecond);  // the one mark there is, for the equity, is ten minutes old
    EXPECT_EQ(d.verdict(code, d.live_order()), Verdict::kPass) << static_cast<int>(code);
  }
}

TEST(MarkAge, AStaleMarkOnAPositionHeldOnlyByAnotherAccountStillBlocks) {
  for (const RuleCode code : kMarkRules) {
    ScopeDesk d;
    d.fill(Side::kBuy, 5, rupees(100), kFuture, {.account = kOtherAccount});
    d.mark(kFuture, rupees(100));
    d.clock.advance(4 * kNanosPerSecond);
    ASSERT_TRUE(d.sb.set_mark(kEquity, rupees(100), d.clock.monotonic()));  // this order's own mark is fresh
    EXPECT_TRUE(names(d.check(code, d.live_order()), DataSource::kMark)) << static_cast<int>(code);
  }
}

TEST(MarkAge, AnInstrumentThatWentFlatInEveryScopeNoLongerCounts) {
  for (const RuleCode code : kMarkRules) {
    ScopeDesk d;
    d.fill(Side::kBuy, 5, rupees(100), kFuture);
    d.fill(Side::kBuy, 5, rupees(100), kFuture, {.account = kOtherAccount});
    d.mark(kFuture, rupees(100));
    d.clock.advance(4 * kNanosPerSecond);
    ASSERT_TRUE(d.sb.set_mark(kEquity, rupees(100), d.clock.monotonic()));
    EXPECT_TRUE(names(d.check(code, d.live_order()), DataSource::kMark)) << static_cast<int>(code);
    d.fill(Side::kSell, 5, rupees(100), kFuture);
    EXPECT_TRUE(names(d.check(code, d.live_order()), DataSource::kMark))
        << static_cast<int>(code);  // the other holds
    d.fill(Side::kSell, 5, rupees(100), kFuture, {.account = kOtherAccount});
    EXPECT_EQ(d.verdict(code, d.live_order()), Verdict::kPass) << static_cast<int>(code);
  }
}

TEST(MarkAge, AHeldPositionWithNoMarkAtAllIsRefusedByRules15To17AsMissingAndByRules19And20AsMissingPnl) {
  for (const RuleCode code : {RuleCode::kPosition, RuleCode::kExposure, RuleCode::kGreeks}) {
    ScopeDesk d;
    d.fill(Side::kBuy, 5, rupees(100), kFuture);  // nothing has marked the future
    EXPECT_TRUE(names(d.check(code, d.live_order()), DataSource::kMark)) << static_cast<int>(code);
  }
  for (const RuleCode code : {RuleCode::kLossDaily, RuleCode::kDrawdown}) {
    ScopeDesk d;
    d.fill(Side::kBuy, 5, rupees(100), kFuture);
    EXPECT_TRUE(names(d.check(code, d.live_order()), DataSource::kPnl))
        << static_cast<int>(code);  // as built
    EXPECT_TRUE(names(d.check(code, d.order()), DataSource::kPnl)) << static_cast<int>(code);
  }
}

TEST(MarkAge, NotLoadedPositionsAndPnlAreNamedBeforeAStaleMark) {
  for (const RuleCode code : {RuleCode::kPosition, RuleCode::kExposure, RuleCode::kGreeks}) {
    ScopeDesk d;
    d.fill(Side::kBuy, 10, rupees(100));
    d.clock.advance(10 * kNanosPerSecond);
    d.sb.set_positions_loaded(false);
    EXPECT_TRUE(names(d.check(code, d.live_order()), DataSource::kPositions)) << static_cast<int>(code);
  }
  for (const RuleCode code : {RuleCode::kLossDaily, RuleCode::kDrawdown}) {
    ScopeDesk d;
    d.fill(Side::kBuy, 10, rupees(100));
    d.clock.advance(10 * kNanosPerSecond);
    d.sb.set_pnl_loaded(false);
    EXPECT_TRUE(names(d.check(code, d.live_order()), DataSource::kPnl)) << static_cast<int>(code);
  }
}

TEST(MarkAge, RulesThatDontReadMarksAreNotRefusedForOne) {
  ScopeDesk d;
  d.fill(Side::kBuy, 10, rupees(100));
  d.clock.advance(10 * kNanosPerSecond);
  EXPECT_PASS(d, kOpenOrders, d.live_order());
  EXPECT_PASS(d, kShortOpt, d.live_order());
  EXPECT_PASS(d, kDiscipline, d.live_order());
}

TEST(MarkAge, TheReasonTextIsRDataMissingMark) {
  ScopeDesk d;
  d.fill(Side::kBuy, 10, rupees(100));
  d.clock.advance(4 * kNanosPerSecond);
  d.tick(kEquity, rupees(100), d.clock.monotonic());
  d.refresh_margin();
  const GateDecision refused = d.run_all(d.live_order());
  ASSERT_TRUE(refused.rejection.has_value());
  EXPECT_EQ(refused.rejection->code, RuleCode::kPosition);
  EXPECT_EQ(refused.rejection->reason, RuleReason::kDataMissing);
  EXPECT_EQ(refused.rejection->source, DataSource::kMark);
  EXPECT_EQ(reason_text(refused.rejection.value()), "R-DATA-MISSING:mark");
}

// Only an opening order reaches rules 14 to 22, so a reducing one needs no mark, and may be the way out.
TEST(MarkAge, AReducingLiveOrderPassesTheWholeChainWithStaleMarks) {
  ScopeDesk d;
  d.fill(Side::kBuy, 10, rupees(100));
  d.clock.advance(60 * kNanosPerSecond);
  d.tick(kEquity, rupees(100), d.clock.monotonic());
  d.refresh_margin();
  OrderDetails closing = d.live_order();
  closing.side = Side::kSell;
  closing.qty = 4;
  const GateDecision out = d.run_all(closing);
  EXPECT_TRUE(out.approved.has_value());
  OrderDetails opening = d.live_order();
  opening.qty = 1;
  const GateDecision refused = d.run_all(opening);  // while an opening one is refused for the same marks
  ASSERT_TRUE(refused.rejection.has_value());
  EXPECT_EQ(reason_text(refused.rejection.value()), "R-DATA-MISSING:mark");
}

// ---- Greeks of held options, by the kind of order (D-28; owner, 2026-11-15) ----

TEST(GreeksByOrder, AShareOrAFutureOrderIsNotRefusedForStaleGreeksOfHeldOptions) {
  ScopeDesk d;
  d.fill(Side::kSell, 65, rupees(10), kOption);
  d.mark(kOption, rupees(10));
  d.fresh_greeks();
  d.clock.advance(20 * kNanosPerSecond);  // past the 15 s default
  d.restamp_marks();
  OrderDetails future = d.live_order();
  future.instrument_id = kFuture;
  future.qty = 5;
  for (const RuleCode code : {RuleCode::kPosition, RuleCode::kExposure, RuleCode::kGreeks}) {
    EXPECT_EQ(d.verdict(code, d.live_order()), Verdict::kPass) << static_cast<int>(code);
    EXPECT_EQ(d.verdict(code, future), Verdict::kPass) << static_cast<int>(code);
    // while an option order is
    EXPECT_TRUE(names(d.check(code, d.option_order(Side::kSell, 65, true)), DataSource::kGreeks))
        << static_cast<int>(code);
  }
}

TEST(GreeksByOrder, AnyLiveOrderIsRefusedWhileAHeldOptionHasNoGreeksAtAll) {
  ScopeDesk d;
  d.fill(Side::kSell, 65, rupees(10), kOption);
  d.mark(kOption, rupees(10));  // a mark, but no Greeks have ever arrived
  OrderDetails future = d.live_order();
  future.instrument_id = kFuture;
  future.qty = 5;
  for (const RuleCode code : {RuleCode::kPosition, RuleCode::kExposure, RuleCode::kGreeks}) {
    EXPECT_TRUE(names(d.check(code, d.live_order()), DataSource::kGreeks)) << static_cast<int>(code);
    EXPECT_TRUE(names(d.check(code, future), DataSource::kGreeks)) << static_cast<int>(code);
    EXPECT_EQ(d.verdict(code, d.order()), Verdict::kPass) << static_cast<int>(code);  // paper skips Greeks
  }
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
