// Rules 18, 21 and 22 (M2.4b): short options and their cover (D-31), margin (D-32), and the loss-streak
// lockout (RISK_ENGINE §5), with the margin cache and the discipline table they read.

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "desk_fixture.h"
#include "scope_desk.h"
#include "vyuha/risk/discipline_table.h"
#include "vyuha/risk/margin_cache.h"

// NOLINTBEGIN(bugprone-unchecked-optional-access): every test does ASSERT_TRUE(has_value()) first
namespace vyuha::risk {
namespace {

[[nodiscard]] bool no_cover(const Outcome& out) {
  return out.verdict == Verdict::kReject && out.reason == RuleReason::kNoCover;
}

// ---- the tables ----

TEST(MarginCache, ARefreshReplacesTheFigureAndClearsWhatWasReserved) {
  MarginCache cache(4, 16);
  EXPECT_EQ(cache.available(kAccount), nullptr);
  ASSERT_TRUE(cache.set_available(kAccount, 1'000, 10));
  ASSERT_TRUE(cache.reserve(kAccount, 300));
  EXPECT_EQ(cache.available(kAccount)->amount, 1'000);
  EXPECT_EQ(cache.available(kAccount)->reserved, 300);
  ASSERT_TRUE(cache.reserve(kAccount, 200));
  EXPECT_EQ(cache.available(kAccount)->reserved, 500);
  ASSERT_TRUE(cache.set_available(kAccount, 800, 20));
  EXPECT_EQ(cache.available(kAccount)->amount, 800);
  EXPECT_EQ(cache.available(kAccount)->reserved, 0);
  EXPECT_EQ(cache.available(kAccount)->as_of, 20);
}

TEST(MarginCache, ReservingAgainstAnUnknownAccountIsRefused) {
  MarginCache cache(4, 16);
  EXPECT_FALSE(cache.reserve(kAccount, 1));
}

TEST(MarginCache, AccountsAreKeptApartAndATableThatIsFullRefusesANewOne) {
  MarginCache cache(2, 16);
  ASSERT_TRUE(cache.set_available(kAccount, 1, 1));
  ASSERT_TRUE(cache.set_available(kOtherAccount, 2, 1));
  EXPECT_FALSE(cache.set_available(id_of(77), 3, 1));
  EXPECT_TRUE(cache.set_available(kAccount, 5, 2));  // an update still works
  EXPECT_EQ(cache.available(kAccount)->amount, 5);
  EXPECT_EQ(cache.available(kOtherAccount)->amount, 2);
  EXPECT_EQ(cache.available(id_of(77)), nullptr);
}

TEST(MarginCache, PerLotFiguresAreKeptPerInstrumentAndUpdatedInPlace) {
  MarginCache cache(2, 8);
  EXPECT_EQ(cache.per_lot(5), nullptr);
  ASSERT_TRUE(cache.set_per_lot(5, 100, 1));
  ASSERT_TRUE(cache.set_per_lot(6, 200, 1));
  ASSERT_TRUE(cache.set_per_lot(5, 150, 2));
  EXPECT_EQ(cache.per_lot(5)->amount, 150);
  EXPECT_EQ(cache.per_lot(5)->as_of, 2);
  EXPECT_EQ(cache.per_lot(6)->amount, 200);
  for (std::int64_t id = 100; id < 106; ++id) {
    EXPECT_TRUE(cache.set_per_lot(id, 1, 1));
  }
  EXPECT_FALSE(cache.set_per_lot(999, 1, 1));  // full
  EXPECT_TRUE(cache.set_per_lot(5, 1, 3));
}

TEST(DisciplineTable, ALockoutIsPerUserAndModeAndItsUpdateReplacesIt) {
  DisciplineTable table(2);
  EXPECT_FALSE(table.loaded());
  table.set_loaded(true);
  EXPECT_TRUE(table.loaded());
  EXPECT_EQ(table.locked_until(kUser, oms::kLiveOrder), std::nullopt);
  ASSERT_TRUE(table.lock(kUser, oms::kLiveOrder, 500));
  ASSERT_TRUE(table.lock(kUser, oms::kLiveOrder, 900));
  EXPECT_EQ(table.locked_until(kUser, oms::kLiveOrder), 900);
  EXPECT_EQ(table.locked_until(kUser, oms::kPaperOrder), std::nullopt);  // the other mode is its own row
  ASSERT_TRUE(table.lock(kUser, oms::kPaperOrder, 700));
  EXPECT_EQ(table.locked_until(kUser, oms::kPaperOrder), 700);
  EXPECT_EQ(table.locked_until(kUser, oms::kLiveOrder), 900);
  EXPECT_EQ(table.locked_until(kOtherAccount, oms::kLiveOrder), std::nullopt);
}

TEST(DisciplineTable, ClearingOneModeLeavesTheOtherAndRoomIsTwoRowsPerUser) {
  DisciplineTable table(1);  // one user: a paper row and a live row
  ASSERT_TRUE(table.lock(id_of(1), oms::kLiveOrder, 1));
  ASSERT_TRUE(table.lock(id_of(1), oms::kPaperOrder, 2));
  EXPECT_FALSE(table.lock(id_of(2), oms::kLiveOrder, 3));  // full
  EXPECT_FALSE(table.lock(id_of(2), oms::kPaperOrder, 3));
  EXPECT_TRUE(table.lock(id_of(1), oms::kLiveOrder, 9));  // an update still works when full
  table.clear(id_of(1), oms::kLiveOrder);
  EXPECT_EQ(table.locked_until(id_of(1), oms::kLiveOrder), std::nullopt);
  EXPECT_EQ(table.locked_until(id_of(1), oms::kPaperOrder), 2);
  EXPECT_TRUE(table.lock(id_of(2), oms::kLiveOrder, 3));  // the freed row
  table.clear(id_of(99), oms::kLiveOrder);                // not there: nothing happens
  EXPECT_EQ(table.locked_until(id_of(2), oms::kLiveOrder), 3);
  table.clear(id_of(1), oms::kPaperOrder);
  EXPECT_EQ(table.locked_until(id_of(1), oms::kPaperOrder), std::nullopt);
}

TEST(DisciplineTable, TheSameUserBothModesFillsATwoUserTableAsTwoRows) {
  DisciplineTable table(2);
  for (const auto mode : {oms::kLiveOrder, oms::kPaperOrder}) {
    ASSERT_TRUE(table.lock(id_of(1), mode, 1));
    ASSERT_TRUE(table.lock(id_of(2), mode, 1));
  }
  EXPECT_FALSE(table.lock(id_of(3), oms::kPaperOrder, 1));
}

TEST(MarginMaxAge, ParsesWholeMillisecondsInRange) {
  EXPECT_EQ(parse_margin_max_age_ms("").value_or(0), kDefaultMarginMaxAge);
  EXPECT_EQ(kDefaultMarginMaxAge, 30 * kNanosPerSecond);
  EXPECT_EQ(parse_margin_max_age_ms("1").value_or(0), 1'000'000);
  EXPECT_EQ(parse_margin_max_age_ms("300000").value_or(0), 300 * kNanosPerSecond);
  for (const std::string_view bad : {"0", "-5", "300001", "30s", " 30", "1.5", "abc"}) {
    EXPECT_FALSE(parse_margin_max_age_ms(bad).has_value()) << bad;
  }
  EXPECT_EQ(GateContext{}.margin_max_age, kDefaultMarginMaxAge);
}

TEST(AllRules, TheTableIsTheTwentyTwoInOrderAndTheGateIsComplete) {
  const auto rules = all_rules();
  ASSERT_EQ(rules.size(), kRuleCount);
  for (std::size_t i = 0; i < kRuleCount; ++i) {
    EXPECT_EQ(std::to_underlying(rules[i].code), i + 1);
    EXPECT_NE(rules[i].eval, nullptr);
  }
  const SimClock clock{1};
  EXPECT_TRUE(RiskGate::create(rules, clock).value().complete());
  // Only R-DUP, R-OPS and R-MARGIN keep state.
  for (const Rule& r : rules) {
    const bool stateful = r.code == RuleCode::kDup || r.code == RuleCode::kOps || r.code == RuleCode::kMargin;
    EXPECT_EQ(r.on_approved != nullptr, stateful) << static_cast<int>(r.code);
  }
}

// ---- the book's short-option aggregates and the cover view ----

TEST(ShortLots, ShortOptionLotsAndWorkingSellsAreKeptPerScopeAtTheLimitScale) {
  ScopeDesk d;
  d.fill(Side::kSell, 2 * kLot, rupees(10), kOption);
  ASSERT_TRUE(d.sb.add_working(d.who(), kOption, Side::kSell, kLot));
  for (const auto& [kind, id] :
       {std::pair{LimitScope::kDesk, Uuid{}}, std::pair{LimitScope::kAccount, kAccount},
        std::pair{LimitScope::kTrader, kUser}}) {
    EXPECT_EQ(d.sb.agg(kind, id)->short_lots, Wide{2} * kFromScale) << static_cast<int>(kind);
    EXPECT_EQ(d.sb.agg(kind, id)->working_short_lots, Wide{1} * kFromScale);
  }
  d.fill(Side::kBuy, kLot, rupees(10), kOption);  // one lot closed
  EXPECT_EQ(d.sb.agg(LimitScope::kAccount, kAccount)->short_lots, Wide{1} * kFromScale);
  ASSERT_TRUE(d.sb.add_working(d.who(), kOption, Side::kSell, -kLot));
  EXPECT_EQ(d.sb.agg(LimitScope::kAccount, kAccount)->working_short_lots, 0);
}

TEST(ShortLots, ALongOptionAndAWorkingBuyAddNothing) {
  ScopeDesk d;
  d.fill(Side::kBuy, 2 * kLot, rupees(10), kOption);
  ASSERT_TRUE(d.sb.add_working(d.who(), kOption, Side::kBuy, kLot));
  EXPECT_EQ(d.sb.agg(LimitScope::kAccount, kAccount)->short_lots, 0);
  EXPECT_EQ(d.sb.agg(LimitScope::kAccount, kAccount)->working_short_lots, 0);
}

TEST(ShortLots, ARepeatedMarkOrGreeksDoesntDriftTheCount) {
  ScopeDesk d;
  d.fill(Side::kSell, 2 * kLot, rupees(10), kOption);
  for (int i = 0; i < 3; ++i) {
    d.mark(kOption, rupees(10 + i));
    d.fresh_greeks();
  }
  EXPECT_EQ(d.sb.agg(LimitScope::kAccount, kAccount)->short_lots, Wide{2} * kFromScale);
}

TEST(CoverView, CountsLongsShortsAndFuturesOfTheKindAndExpiryAskedFor) {
  ScopeDesk d;
  const Nanos expiry = d.instruments.find(kOption)->expires_at;
  d.fill(Side::kBuy, 3 * kLot, rupees(10), kOption);         // long calls
  d.fill(Side::kSell, 1 * kLot, rupees(10), kOption2);       // short call, same expiry, other strike
  d.fill(Side::kBuy, 5 * kLot, rupees(10), kPutOption);      // puts don't count for calls
  d.fill(Side::kBuy, 2 * kLot, rupees(10), kHedgeFut);       // long future: covers calls
  d.fill(Side::kSell, 4 * kLot, rupees(10), kHedgeFutNear);  // expires before: covers nothing
  ASSERT_TRUE(d.sb.add_working(d.who(), kOption2, Side::kSell, kLot));
  const ScopeBook::CoverView view = d.sb.cover_view(kAccount, kEquity, OptionKind::kCall, expiry);
  EXPECT_EQ(view.long_units, Wide{3} * kLot);
  EXPECT_EQ(view.short_units, Wide{2} * kLot);  // one held and one working
  EXPECT_EQ(view.future_units, Wide{2} * kLot);
  EXPECT_EQ(view.others_uncovered, 0);
  EXPECT_FALSE(view.overflowed);
  const ScopeBook::CoverView puts = d.sb.cover_view(kAccount, kEquity, OptionKind::kPut, expiry);
  EXPECT_EQ(puts.long_units, Wide{5} * kLot);
  EXPECT_EQ(puts.future_units, 0);  // a long future doesn't cover puts, and the near short doesn't reach
}

TEST(CoverView, AShortFutureCoversPutsAndOnlyThose) {
  ScopeDesk d;
  const Nanos expiry = d.instruments.find(kOption)->expires_at;
  d.fill(Side::kSell, 2 * kLot, rupees(10), kHedgeFut);
  EXPECT_EQ(d.sb.cover_view(kAccount, kEquity, OptionKind::kPut, expiry).future_units, Wide{2} * kLot);
  EXPECT_EQ(d.sb.cover_view(kAccount, kEquity, OptionKind::kCall, expiry).future_units, 0);
}

TEST(CoverView, OtherExpiriesShortsThatTheirOwnLongsDontCoverAreCountedAside) {
  ScopeDesk d;
  const Nanos expiry = d.instruments.find(kOption)->expires_at;
  d.fill(Side::kSell, 3 * kLot, rupees(10), kCallFar);  // far expiry: 3 short
  d.fill(Side::kBuy, 1 * kLot, rupees(10), kCallFar);   // 1 long of it: 2 uncovered
  const ScopeBook::CoverView view = d.sb.cover_view(kAccount, kEquity, OptionKind::kCall, expiry);
  EXPECT_EQ(view.long_units, 0);
  EXPECT_EQ(view.short_units, 0);
  EXPECT_EQ(view.others_uncovered, Wide{2} * kLot);
}

TEST(CoverView, OnlyTheAccountsOwnPositionsCountAndAnUnknownUnderlyingIsEmpty) {
  ScopeDesk d;
  const Nanos expiry = d.instruments.find(kOption)->expires_at;
  d.fill(Side::kBuy, 2 * kLot, rupees(10), kOption, {.account = kOtherAccount});
  EXPECT_EQ(d.sb.cover_view(kAccount, kEquity, OptionKind::kCall, expiry).long_units, 0);
  EXPECT_EQ(d.sb.cover_view(kOtherAccount, kEquity, OptionKind::kCall, expiry).long_units, Wide{2} * kLot);
  EXPECT_EQ(d.sb.cover_view(kAccount, 12'345, OptionKind::kCall, expiry).long_units, 0);
}

TEST(CoverView, MoreExpiriesThanAreTrackedIsReportedAsOverflow) {
  ScopeDesk d;
  const InstrumentRef base = *d.instruments.find(kOption);
  for (std::int64_t i = 0; i < 17; ++i) {
    InstrumentRef r = base;
    r.expires_at = base.expires_at + (Nanos{i + 1} * Nanos{1'440} * kNanosPerMinute);
    ASSERT_TRUE(d.instruments.put(100 + i, r));
  }
  for (std::int64_t i = 0; i < 17; ++i) {
    d.fill(Side::kBuy, kLot, rupees(10), 100 + i);
  }
  EXPECT_TRUE(d.sb.cover_view(kAccount, kEquity, OptionKind::kCall, base.expires_at).overflowed);
}

// ---- R-SHORT-OPT ----

// Rule 18 judges an opening sell of an option, and any order that could take cover away from a short (D-39
// (10)): a sell of an option or any future order. A share order or an option buy is not its business.
TEST(RShortOpt, SharesAndBoughtOptionsAreNotJudgedWhateverIsLoaded) {
  ScopeDesk d;
  d.sb.set_positions_loaded(false);  // nothing is judged, so nothing is missing
  EXPECT_PASS(d, kShortOpt, d.option_order(Side::kBuy, kLot, false));
  EXPECT_PASS(d, kShortOpt, d.sell(5));  // an equity sell
  EXPECT_PASS(d, kShortOpt, d.buy(5));
}

TEST(RShortOpt, AnOptionSellOrAnyFutureOrderNamesThePositionsWhenTheyAreMissing) {
  ScopeDesk d;
  d.sb.set_positions_loaded(false);
  for (const Side side : {Side::kBuy, Side::kSell}) {
    OrderDetails future = d.sell(5);
    future.side = side;
    future.instrument_id = kHedgeFut;
    EXPECT_TRUE(names(d.check(RuleCode::kShortOpt, future), DataSource::kPositions));
    EXPECT_TRUE(names(d.check_reducing(RuleCode::kShortOpt, future), DataSource::kPositions));
  }
  EXPECT_TRUE(names(d.check_reducing(RuleCode::kShortOpt, d.option_order(Side::kSell, kLot, false)),
                    DataSource::kPositions));
}

TEST(RShortOpt, AnInstrumentTheMasterDoesntKnowIsRefused) {
  const ScopeDesk d;
  OrderDetails unknown = d.order();
  unknown.instrument_id = 987'654;
  EXPECT_EQ(d.check(RuleCode::kShortOpt, unknown).verdict, Verdict::kReject);
  EXPECT_EQ(d.check_reducing(RuleCode::kShortOpt, unknown).verdict, Verdict::kReject);
}

TEST(RShortOpt, ASoldOptionNamesThePositionsWhenTheyAreMissing) {
  ScopeDesk d;
  d.sb.set_positions_loaded(false);
  EXPECT_TRUE(
      names(d.check(RuleCode::kShortOpt, d.option_order(Side::kSell, kLot, false)), DataSource::kPositions));
}

TEST(RShortOpt, TheLotsLimitCountsHeldShortsWorkingSellsAndTheOrder) {
  ScopeDesk d;
  d.limit(LimitKey::kNakedShortOptions, 1 * kFromScale);  // cover isn't what is tested here
  d.clear_limits(LimitKey::kMaxShortOptionLots);
  d.limit(LimitKey::kMaxShortOptionLots, 3 * kFromScale);
  d.fill(Side::kSell, 1 * kLot, rupees(10), kOption);
  ASSERT_TRUE(d.sb.add_working(d.who(), kOption2, Side::kSell, kLot));
  EXPECT_PASS(d, kShortOpt, d.option_order(Side::kSell, kLot, false));  // the third lot
  const Outcome out = d.check(RuleCode::kShortOpt, d.option_order(Side::kSell, 2 * kLot, false));
  EXPECT_EQ(out.verdict, Verdict::kReject);
  EXPECT_EQ(out.limit, 3 * kFromScale);
  EXPECT_EQ(out.current, 4 * kFromScale);
  EXPECT_FALSE(no_cover(out));
}

TEST(RShortOpt, EachScopeRowIsMeasuredAtItsOwnScope) {
  ScopeDesk d;
  d.limit(LimitKey::kNakedShortOptions, 1 * kFromScale);
  d.limit(LimitKey::kMaxShortOptionLots, 2 * kFromScale, LimitScope::kTrader, true, kUser);
  d.fill(Side::kSell, 5 * kLot, rupees(10), kOption, {.account = kOtherAccount, .trader = id_of(7)});
  EXPECT_PASS(d, kShortOpt,
              d.option_order(Side::kSell, 2 * kLot, false));  // another trader's shorts don't count
  EXPECT_REJECT(d, kShortOpt, d.option_order(Side::kSell, 3 * kLot, false));
}

TEST(RShortOpt, ASoftLimitWarnsAndNoRowAtAllRefuses) {
  ScopeDesk d;
  d.limit(LimitKey::kNakedShortOptions, 1 * kFromScale);
  d.clear_limits(LimitKey::kMaxShortOptionLots);
  EXPECT_REJECT(d, kShortOpt, d.option_order(Side::kSell, kLot, false));
  d.limit(LimitKey::kMaxShortOptionLots, 0, LimitScope::kDesk, false);
  EXPECT_WARN(d, kShortOpt, d.option_order(Side::kSell, kLot, false));
}

TEST(RShortOpt, WithNothingToCoverItAShortIsRefusedByDefaultWithItsOwnReason) {
  ScopeDesk d;  // no naked row: the default is not allowed
  const Outcome out = d.check(RuleCode::kShortOpt, d.option_order(Side::kSell, kLot, false));
  EXPECT_TRUE(no_cover(out));
  EXPECT_EQ(out.limit, 0);  // units of cover, and units needed
  EXPECT_EQ(out.current, kLot);
  const GateDecision refused = d.run_all(d.option_order(Side::kSell, kLot, false));
  ASSERT_TRUE(refused.rejection.has_value());
  EXPECT_EQ(refused.rejection.value().code, RuleCode::kShortOpt);
  EXPECT_EQ(reason_text(refused.rejection.value()), "R-SHORT-OPT:no-cover");
}

TEST(RShortOpt, ALongOfTheSameUnderlyingExpiryAndKindCoversLotForLotAtAnyStrike) {
  ScopeDesk d;
  d.fill(Side::kBuy, 2 * kLot, rupees(10), kOption2);  // another strike
  EXPECT_PASS(d, kShortOpt, d.option_order(Side::kSell, 2 * kLot, false));
  const Outcome out = d.check(RuleCode::kShortOpt, d.option_order(Side::kSell, 3 * kLot, false));
  EXPECT_TRUE(no_cover(out));
  EXPECT_EQ(out.limit, 2 * kLot);
  EXPECT_EQ(out.current, 3 * kLot);
}

TEST(RShortOpt, APutADifferentExpiryOrAnotherAccountDoesntCover) {
  ScopeDesk d;
  d.fill(Side::kBuy, 5 * kLot, rupees(10), kPutOption);
  d.fill(Side::kBuy, 5 * kLot, rupees(10), kCallFar);
  d.fill(Side::kBuy, 5 * kLot, rupees(10), kOption, {.account = kOtherAccount});
  EXPECT_TRUE(no_cover(d.check(RuleCode::kShortOpt, d.option_order(Side::kSell, kLot, false))));
}

TEST(RShortOpt, ShortsAlreadyHeldNeedTheirCoverToo) {
  ScopeDesk d;
  d.fill(Side::kBuy, 1 * kLot, rupees(10), kOption2);
  d.fill(Side::kSell, 1 * kLot, rupees(10), kOption);  // uses the one lot
  EXPECT_TRUE(no_cover(d.check(RuleCode::kShortOpt, d.option_order(Side::kSell, kLot, false))));
  d.fill(Side::kBuy, 1 * kLot, rupees(10), kOption2);
  EXPECT_PASS(d, kShortOpt, d.option_order(Side::kSell, kLot, false));
}

TEST(RShortOpt, WorkingSellsNeedCoverAndWorkingBuysAreNoCover) {
  ScopeDesk d;
  d.fill(Side::kBuy, 1 * kLot, rupees(10), kOption2);
  ASSERT_TRUE(d.sb.add_working(d.who(), kOption, Side::kSell, kLot));
  EXPECT_TRUE(no_cover(d.check(RuleCode::kShortOpt, d.option_order(Side::kSell, kLot, false))));
  ScopeDesk e;  // the hedge is only working: buy it first
  ASSERT_TRUE(e.sb.add_working(e.who(), kOption2, Side::kBuy, 5 * kLot));
  EXPECT_TRUE(no_cover(e.check(RuleCode::kShortOpt, e.option_order(Side::kSell, kLot, false))));
}

TEST(RShortOpt, AFutureCoversTheKindItHedgesLotForLotAndNotBeforeItsExpiry) {
  {
    ScopeDesk d;  // a long future covers short calls
    d.fill(Side::kBuy, 2 * kLot, rupees(100), kHedgeFut);
    EXPECT_PASS(d, kShortOpt, d.option_order(Side::kSell, 2 * kLot, false));
    EXPECT_TRUE(no_cover(d.check(RuleCode::kShortOpt, d.option_order(Side::kSell, 3 * kLot, false))));
    OrderDetails put = d.option_order(Side::kSell, kLot, false);
    put.instrument_id = kPutOption;
    EXPECT_TRUE(no_cover(d.check(RuleCode::kShortOpt, put)));  // but not short puts
  }
  {
    ScopeDesk d;  // a short future covers short puts
    d.fill(Side::kSell, 2 * kLot, rupees(100), kHedgeFut);
    OrderDetails put = d.option_order(Side::kSell, 2 * kLot, false);
    put.instrument_id = kPutOption;
    EXPECT_PASS(d, kShortOpt, put);
    EXPECT_TRUE(no_cover(d.check(RuleCode::kShortOpt, d.option_order(Side::kSell, kLot, false))));
  }
  {
    ScopeDesk d;  // a future that expires before the option covers nothing
    d.fill(Side::kBuy, 2 * kLot, rupees(100), kHedgeFutNear);
    EXPECT_TRUE(no_cover(d.check(RuleCode::kShortOpt, d.option_order(Side::kSell, kLot, false))));
  }
}

TEST(RShortOpt, OptionLongsAndFuturesAddUp) {
  ScopeDesk d;
  d.fill(Side::kBuy, 1 * kLot, rupees(10), kOption2);
  d.fill(Side::kBuy, 1 * kLot, rupees(100), kHedgeFut);
  EXPECT_PASS(d, kShortOpt, d.option_order(Side::kSell, 2 * kLot, false));
  EXPECT_TRUE(no_cover(d.check(RuleCode::kShortOpt, d.option_order(Side::kSell, 3 * kLot, false))));
}

TEST(RShortOpt, AFutureIsNotSpentTwiceAcrossExpiries) {
  ScopeDesk d;
  d.fill(Side::kBuy, 1 * kLot, rupees(100), kHedgeFut);
  EXPECT_PASS(d, kShortOpt, d.option_order(Side::kSell, kLot, false));
  d.fill(Side::kSell, 1 * kLot, rupees(10), kCallFar);  // a short call of the far expiry takes the future
  EXPECT_TRUE(no_cover(d.check(RuleCode::kShortOpt, d.option_order(Side::kSell, kLot, false))));
  d.fill(Side::kBuy, 1 * kLot, rupees(10), kCallFar);  // now the far expiry covers itself
  EXPECT_PASS(d, kShortOpt, d.option_order(Side::kSell, kLot, false));
}

TEST(RShortOpt, WhenNakedShortsAreAllowedNoCoverIsNeededButTheLimitStillApplies) {
  ScopeDesk d;
  d.limit(LimitKey::kNakedShortOptions, 1 * kFromScale);
  d.clear_limits(LimitKey::kMaxShortOptionLots);
  d.limit(LimitKey::kMaxShortOptionLots, 2 * kFromScale);
  EXPECT_PASS(d, kShortOpt, d.option_order(Side::kSell, 2 * kLot, false));
  EXPECT_REJECT(d, kShortOpt, d.option_order(Side::kSell, 3 * kLot, false));
}

TEST(RShortOpt, TheStrictestNakedRowWinsAndAZeroRowIsNotAllowed) {
  ScopeDesk d;
  d.limit(LimitKey::kNakedShortOptions, 1 * kFromScale);
  d.limit(LimitKey::kNakedShortOptions, 0, LimitScope::kAccount, true, kAccount);
  EXPECT_TRUE(no_cover(d.check(RuleCode::kShortOpt, d.option_order(Side::kSell, kLot, false))));
  ScopeDesk e;
  e.limit(LimitKey::kNakedShortOptions, 0);
  EXPECT_TRUE(no_cover(e.check(RuleCode::kShortOpt, e.option_order(Side::kSell, kLot, false))));
  ScopeDesk f;  // a soft "allowed" row isn't an allowance
  f.limit(LimitKey::kNakedShortOptions, 1 * kFromScale, LimitScope::kDesk, false);
  EXPECT_TRUE(no_cover(f.check(RuleCode::kShortOpt, f.option_order(Side::kSell, kLot, false))));
}

TEST(RShortOpt, AnOptionWithNoUnderlyingOrKindCantBeCoveredSoItIsRefused) {
  ScopeDesk d;
  InstrumentRef odd = *d.instruments.find(kOption);
  odd.option_kind = OptionKind::kNone;
  ASSERT_TRUE(d.instruments.put(kOption, odd));
  d.fill(Side::kBuy, 5 * kLot, rupees(10), kOption2);
  EXPECT_TRUE(no_cover(d.check(RuleCode::kShortOpt, d.option_order(Side::kSell, kLot, false))));
  odd.option_kind = OptionKind::kCall;
  odd.underlying_id = 0;
  ASSERT_TRUE(d.instruments.put(kOption, odd));
  EXPECT_TRUE(no_cover(d.check(RuleCode::kShortOpt, d.option_order(Side::kSell, kLot, false))));
}

TEST(RShortOpt, MoreExpiriesThanAreTrackedRefusesInsteadOfGuessing) {
  ScopeDesk d;
  const InstrumentRef base = *d.instruments.find(kOption);
  for (std::int64_t i = 0; i < 17; ++i) {
    InstrumentRef r = base;
    r.expires_at = base.expires_at + (Nanos{i + 1} * Nanos{1'440} * kNanosPerMinute);
    ASSERT_TRUE(d.instruments.put(100 + i, r));
    d.fill(Side::kBuy, kLot, rupees(10), 100 + i);
  }
  d.fill(Side::kBuy, 5 * kLot, rupees(10), kOption2);
  EXPECT_TRUE(no_cover(d.check(RuleCode::kShortOpt, d.option_order(Side::kSell, kLot, false))));
}

TEST(RShortOpt, ASellThatClosesALongIsNotOpeningSoTheGateDoesntAskForCover) {
  ScopeDesk d;
  d.fill(Side::kBuy, 2 * kLot, rupees(10), kOption);
  EXPECT_TRUE(d.run_all(d.option_order(Side::kSell, 2 * kLot, false)).approved.has_value());
}

TEST(RShortOpt, AWorkingSellOfTheHedgeFutureRemovesItsCoverAndAWorkingBuyRemovesAPutHedges) {
  {
    ScopeDesk d;
    d.fill(Side::kBuy, 2 * kLot, rupees(100), kHedgeFut);
    ASSERT_TRUE(d.sb.add_working(d.who(), kHedgeFut, Side::kSell, kLot));
    EXPECT_PASS(d, kShortOpt, d.option_order(Side::kSell, kLot, false));  // one lot of the two is left
    EXPECT_TRUE(no_cover(d.check(RuleCode::kShortOpt, d.option_order(Side::kSell, 2 * kLot, false))));
    ASSERT_TRUE(d.sb.add_working(d.who(), kHedgeFut, Side::kSell, kLot));
    EXPECT_TRUE(no_cover(d.check(RuleCode::kShortOpt, d.option_order(Side::kSell, kLot, false))));
    ASSERT_TRUE(d.sb.add_working(d.who(), kHedgeFut, Side::kSell, -2 * kLot));  // cancelled
    EXPECT_PASS(d, kShortOpt, d.option_order(Side::kSell, 2 * kLot, false));
  }
  {
    ScopeDesk d;  // a short future hedges puts; a working buy of it would close it
    d.fill(Side::kSell, 2 * kLot, rupees(100), kHedgeFut);
    ASSERT_TRUE(d.sb.add_working(d.who(), kHedgeFut, Side::kBuy, 2 * kLot));
    OrderDetails put = d.option_order(Side::kSell, kLot, false);
    put.instrument_id = kPutOption;
    EXPECT_TRUE(no_cover(d.check(RuleCode::kShortOpt, put)));
  }
}

TEST(RShortOpt, AFutureExpiringExactlyWithTheOptionCoversIt) {
  ScopeDesk d;
  InstrumentRef same = *d.instruments.find(kHedgeFut);
  same.expires_at = d.instruments.find(kOption)->expires_at;
  ASSERT_TRUE(d.instruments.put(300, same));
  d.fill(Side::kBuy, kLot, rupees(100), 300);
  EXPECT_PASS(d, kShortOpt, d.option_order(Side::kSell, kLot, false));
}

TEST(RShortOpt, WhenTheTableOfExpiriesOverflowsAFutureDoesntCoverWhatItCouldNotSettle) {
  ScopeDesk d;
  const InstrumentRef base = *d.instruments.find(kOption);
  for (std::int64_t i = 0; i < 17; ++i) {
    InstrumentRef r = base;
    r.expires_at = base.expires_at + (Nanos{i + 1} * Nanos{1'440} * kNanosPerMinute);
    ASSERT_TRUE(d.instruments.put(100 + i, r));
    d.fill(Side::kBuy, kLot, rupees(10), 100 + i);
  }
  d.fill(Side::kBuy, 50 * kLot, rupees(100), kHedgeFut);  // plenty of cover, if it could be counted
  EXPECT_TRUE(no_cover(d.check(RuleCode::kShortOpt, d.option_order(Side::kSell, kLot, false))));
}

TEST(RShortOpt, FlatPositionsDontTakeAPlaceInTheTableOfExpiries) {
  ScopeDesk d;
  const InstrumentRef base = *d.instruments.find(kOption);
  for (std::int64_t i = 0; i < 20; ++i) {
    InstrumentRef r = base;
    r.expires_at = base.expires_at + (Nanos{i + 1} * Nanos{1'440} * kNanosPerMinute);
    ASSERT_TRUE(d.instruments.put(100 + i, r));
    d.fill(Side::kBuy, kLot, rupees(10), 100 + i);
    d.fill(Side::kSell, kLot, rupees(10), 100 + i);  // and flat again
  }
  d.fill(Side::kBuy, kLot, rupees(10), kOption2);
  EXPECT_PASS(d, kShortOpt, d.option_order(Side::kSell, kLot, false));
}

TEST(RShortOpt, AnOptionWithNoKindIsNoCoverForEitherKind) {
  ScopeDesk d;
  InstrumentRef odd = *d.instruments.find(kOption);
  odd.option_kind = OptionKind::kNone;
  ASSERT_TRUE(d.instruments.put(301, odd));
  d.fill(Side::kBuy, 5 * kLot, rupees(10), 301);
  d.fill(Side::kSell, 5 * kLot, rupees(100), kHedgeFut);  // a short future, which hedges puts
  OrderDetails put = d.option_order(Side::kSell, kLot, false);
  put.instrument_id = kPutOption;
  EXPECT_PASS(d, kShortOpt, put);  // the future covers it
  put.qty = 6 * kLot;
  EXPECT_TRUE(no_cover(d.check(RuleCode::kShortOpt, put)));  // and the 5 kind-less longs add nothing
}

TEST(RShortOpt, ASoldOptionWithNoKindIsNotTakenForAPutByAShortFuture) {
  ScopeDesk d;
  InstrumentRef odd = *d.instruments.find(kOption);
  odd.option_kind = OptionKind::kNone;
  ASSERT_TRUE(d.instruments.put(301, odd));
  d.fill(Side::kSell, 5 * kLot, rupees(100), kHedgeFut);  // a short future, which would cover a put
  OrderDetails kindless = d.option_order(Side::kSell, kLot, false);
  kindless.instrument_id = 301;
  EXPECT_TRUE(no_cover(d.check(RuleCode::kShortOpt, kindless)));
}

TEST(ShortLots, ShortFuturesAndSharesAddNothingToShortOptionLots) {
  ScopeDesk d;
  d.fill(Side::kSell, 2 * kLot, rupees(100), kHedgeFut);
  d.fill(Side::kSell, 50, rupees(100), kEquity);
  ASSERT_TRUE(d.sb.add_working(d.who(), kHedgeFut, Side::kSell, kLot));
  EXPECT_EQ(d.sb.agg(LimitScope::kAccount, kAccount)->short_lots, 0);
  EXPECT_EQ(d.sb.agg(LimitScope::kAccount, kAccount)->working_short_lots, 0);
}

TEST(ScopeBookLots, ABookRefusesAFutureOrOptionWithNoLotSizeAndBecomesUnhealthy) {
  ScopeDesk d;
  InstrumentRef odd = *d.instruments.find(kOption);
  odd.lot_size = 0;
  ASSERT_TRUE(d.instruments.put(302, odd));
  EXPECT_FALSE(d.sb.apply_fill(
      {.who = d.who(), .instrument_id = 302, .side = Side::kSell, .qty = 65, .price = rupees(10)}));
  EXPECT_FALSE(d.sb.healthy());
  ScopeDesk e;
  ASSERT_TRUE(e.instruments.put(302, odd));
  EXPECT_FALSE(e.sb.add_working(e.who(), 302, Side::kSell, 65));
  EXPECT_FALSE(e.sb.healthy());
}

// ---- R-MARGIN ----

TEST(RMargin, APaperOrderSkipsMarginWhateverIsLoaded) {
  ScopeDesk d;
  d.ctx.margin = nullptr;
  EXPECT_PASS(d, kMargin, d.order());
}

TEST(RMargin, ALiveOrderNeedsTheCacheAndTheAccountsFigure) {
  ScopeDesk d;
  d.ctx.margin = nullptr;
  EXPECT_TRUE(names(d.check(RuleCode::kMargin, d.live_order()), DataSource::kMargin));
  d.ctx.margin = d.margin_cache.get();
  EXPECT_PASS(d, kMargin, d.live_order());
  OrderDetails other = d.live_order();
  other.account_id = kOtherAccount;  // no figure for it
  EXPECT_TRUE(names(d.check(RuleCode::kMargin, other), DataSource::kMargin));
}

TEST(RMargin, AFigureOlderThanTheMaximumAgeOrFromBeforeAPauseIsMissing) {
  ScopeDesk d;
  d.ctx.margin_max_age = 5 * kNanosPerSecond;
  d.refresh_margin();
  d.clock.advance(5 * kNanosPerSecond);
  EXPECT_PASS(d, kMargin, d.live_order());  // at the age
  d.clock.advance(1);
  EXPECT_TRUE(names(d.check(RuleCode::kMargin, d.live_order()), DataSource::kMargin));
  d.refresh_margin();
  EXPECT_PASS(d, kMargin, d.live_order());
  d.ctx.stale_before = d.clock.monotonic();  // a pause since
  EXPECT_TRUE(names(d.check(RuleCode::kMargin, d.live_order()), DataSource::kMargin));
}

TEST(RMargin, AFigureFromTheFutureIsMissing) {
  ScopeDesk d;
  ASSERT_TRUE(d.margin_cache->set_available(kAccount, 1'000'000'000, d.clock.monotonic() + 1));
  EXPECT_TRUE(names(d.check(RuleCode::kMargin, d.live_order()), DataSource::kMargin));
}

TEST(RMargin, SharesAreTakenAtTheirValueWithTheBufferOnTop) {
  ScopeDesk d;
  const OrderDetails order = d.live_order();  // 10 shares at 100: 1,000
  d.refresh_margin(rupees(1'100).units());
  EXPECT_PASS(d, kMargin, order);  // exactly the 10% buffer
  d.refresh_margin(rupees(1'100).units() - 1);
  const Outcome out = d.check(RuleCode::kMargin, order);
  EXPECT_EQ(out.verdict, Verdict::kReject);
  EXPECT_EQ(out.limit, rupees(1'100).units() - 1);
  EXPECT_EQ(out.current, rupees(1'100).units());
}

TEST(RMargin, ABoughtOptionCostsItsPremiumAndASoldOneTheBrokersPerLotFigure) {
  ScopeDesk d;                                                         // the per-lot figure is 100 rupees
  const OrderDetails bought = d.option_order(Side::kBuy, kLot, true);  // 65 x 100 = 6,500 premium
  d.refresh_margin(rupees(7'150).units());                             // with the 10% buffer: 7,150
  EXPECT_PASS(d, kMargin, bought);
  d.refresh_margin(rupees(7'150).units() - 1);
  EXPECT_REJECT(d, kMargin, bought);
  const OrderDetails sold = d.option_order(Side::kSell, 2 * kLot, true);  // 2 lots x 100 = 200
  d.refresh_margin(rupees(220).units());
  EXPECT_PASS(d, kMargin, sold);
  d.refresh_margin(rupees(220).units() - 1);
  EXPECT_REJECT(d, kMargin, sold);
}

TEST(RMargin, AFutureNeedsThePerLotFigureRoundedUp) {
  ScopeDesk d;
  ASSERT_TRUE(d.margin_cache->set_per_lot(kHedgeFut, 1'001, d.clock.monotonic()));
  OrderDetails fut = d.live_order();
  fut.instrument_id = kHedgeFut;
  fut.qty = kLot;
  d.refresh_margin(1'102);  // 1,001 x 1.1 = 1,101.1, rounded up to 1,102
  ASSERT_TRUE(d.margin_cache->set_per_lot(kHedgeFut, 1'001, d.clock.monotonic()));
  EXPECT_PASS(d, kMargin, fut);
  d.refresh_margin(1'101);
  ASSERT_TRUE(d.margin_cache->set_per_lot(kHedgeFut, 1'001, d.clock.monotonic()));
  EXPECT_REJECT(d, kMargin, fut);
}

TEST(RMargin, AFutureOrSoldOptionWithNoFreshPerLotFigureNamesMargin) {
  ScopeDesk d;
  ASSERT_TRUE(d.instruments.put(200, *d.instruments.find(kHedgeFut)));  // a future nothing is cached for
  OrderDetails fut = d.live_order();
  fut.instrument_id = 200;
  fut.qty = kLot;
  EXPECT_TRUE(names(d.check(RuleCode::kMargin, fut), DataSource::kMargin));  // never cached
  ASSERT_TRUE(d.margin_cache->set_per_lot(200, 100, d.clock.monotonic()));
  EXPECT_PASS(d, kMargin, fut);
  d.clock.advance(d.ctx.margin_max_age + 1);
  d.refresh_margin();  // the account's figure is fresh again, the per-lot figure for 200 is not
  EXPECT_TRUE(names(d.check(RuleCode::kMargin, fut), DataSource::kMargin));
  ASSERT_TRUE(d.margin_cache->set_per_lot(200, 100, d.clock.monotonic()));
  const Nanos cached_at = d.clock.monotonic();
  d.clock.advance(1);
  d.refresh_margin();
  EXPECT_PASS(d, kMargin, fut);
  d.ctx.stale_before = cached_at;  // a pause since it was cached, though the account's figure is newer
  EXPECT_TRUE(names(d.check(RuleCode::kMargin, fut), DataSource::kMargin));
}

TEST(RMargin, ALiveOrderOverTenLakhWaitsForThePreviewThatIsNotBuiltYet) {
  const ScopeDesk d;
  OrderDetails big = d.live_order();
  big.qty = 10'000;  // 10 lakh at 100 each: exactly the threshold
  EXPECT_PASS(d, kMargin, big);
  big.qty = 10'001;
  EXPECT_TRUE(names(d.check(RuleCode::kMargin, big), DataSource::kMargin));
}

TEST(RMargin, NoBufferRowRefusesAndTheLargestBufferWins) {
  ScopeDesk d;
  d.refresh_margin(rupees(1'150).units());
  d.limit(LimitKey::kMarginBufferPct, 20 * kFromScale, LimitScope::kAccount, true, kAccount);
  EXPECT_REJECT(d, kMargin, d.live_order());  // 1,000 x 1.2 is 1,200
  d.refresh_margin(rupees(1'200).units());
  EXPECT_PASS(d, kMargin, d.live_order());
  d.clear_limits(LimitKey::kMarginBufferPct);
  const Outcome out = d.check(RuleCode::kMargin, d.live_order());
  EXPECT_EQ(out.verdict, Verdict::kReject);
  EXPECT_EQ(out.reason, RuleReason::kRule);
}

TEST(RMargin, AnApprovedLiveOrderReservesItsBufferedMarginUntilTheNextRefresh) {
  ScopeDesk d;
  d.refresh_margin(rupees(3'000).units());
  const auto live = [&d](std::int32_t qty) {
    OrderDetails o = d.live_order();
    o.qty = qty;
    return o;
  };
  ASSERT_TRUE(d.run_all(live(10)).approved.has_value());  // 1,100 reserved
  EXPECT_EQ(d.margin_cache->available(kAccount)->reserved, rupees(1'100).units());
  ASSERT_TRUE(d.run_all(live(11)).approved.has_value());  // 1,210 more
  EXPECT_EQ(d.margin_cache->available(kAccount)->reserved, rupees(2'310).units());
  const GateDecision refused = d.run_all(live(12));  // needs 1,320 of the 690 left
  ASSERT_TRUE(refused.rejection.has_value());
  EXPECT_EQ(refused.rejection.value().code, RuleCode::kMargin);
  EXPECT_EQ(refused.rejection.value().limit, rupees(690).units());
  EXPECT_EQ(refused.rejection.value().current, rupees(1'320).units());
  d.refresh_margin(rupees(3'000).units());  // the broker's new figure replaces what was reserved
  EXPECT_EQ(d.margin_cache->available(kAccount)->reserved, 0);
  EXPECT_TRUE(d.run_all(live(12)).approved.has_value());
}

TEST(RMargin, AnOrderRefusedByAnotherRuleReservesNothing) {
  ScopeDesk d;
  d.discipline_table->lock(kUser, oms::kLiveOrder, md::ist_day_start(d.clock.now()));
  const GateDecision refused = d.run_all(d.live_order());
  ASSERT_TRUE(refused.rejection.has_value());
  EXPECT_EQ(refused.rejection.value().code, RuleCode::kDiscipline);
  EXPECT_EQ(d.margin_cache->available(kAccount)->reserved, 0);
}

TEST(RMargin, APaperOrderAndAReducingOrderReserveNothing) {
  ScopeDesk d;
  ASSERT_TRUE(d.run_all(d.order()).approved.has_value());  // paper
  EXPECT_EQ(d.margin_cache->available(kAccount)->reserved, 0);
  d.fill(Side::kBuy, 10, rupees(100));
  OrderDetails closing = d.live_order();
  closing.side = Side::kSell;
  closing.qty = 4;
  ASSERT_TRUE(d.run_all(closing).approved.has_value());  // reducing: rules 14 to 22 skipped
  EXPECT_EQ(d.margin_cache->available(kAccount)->reserved, 0);
}

TEST(MarginCache, AFigureThatIsZeroOrNegativeIsRefusedAndTheOldOneStays) {
  MarginCache cache(2, 8);
  ASSERT_TRUE(cache.set_per_lot(5, 100, 1));
  EXPECT_FALSE(cache.set_per_lot(5, 0, 2));
  EXPECT_FALSE(cache.set_per_lot(5, -1, 2));
  EXPECT_FALSE(cache.set_per_lot(6, 0, 2));
  EXPECT_EQ(cache.per_lot(5)->amount, 100);
  EXPECT_EQ(cache.per_lot(6), nullptr);
}

TEST(RMargin, APerLotFigureStampedInTheFutureOrWithNoPriceToCheckTheOrderAgainstIsMissing) {
  ScopeDesk d;
  ASSERT_TRUE(d.instruments.put(200, *d.instruments.find(kHedgeFut)));
  OrderDetails fut = d.live_order();
  fut.instrument_id = 200;
  fut.qty = kLot;
  ASSERT_TRUE(d.margin_cache->set_per_lot(200, 100, d.clock.monotonic() + 1));
  EXPECT_TRUE(names(d.check(RuleCode::kMargin, fut), DataSource::kMargin));  // from the future
  ASSERT_TRUE(d.margin_cache->set_per_lot(200, 100, d.clock.monotonic()));
  fut.order_type = OrderType::kMarket;
  fut.price.reset();
  EXPECT_TRUE(names(d.check(RuleCode::kMargin, fut), DataSource::kMargin));  // no LTP for it: no price at all
  d.tick(200, rupees(100));
  EXPECT_PASS(d, kMargin, fut);
}

TEST(RMargin, ANegativeBufferWouldLoosenTheCheckSoItIsRefused) {
  ScopeDesk d;
  d.clear_limits(LimitKey::kMarginBufferPct);
  d.limit(LimitKey::kMarginBufferPct, -50 * kFromScale);
  d.refresh_margin(rupees(600).units());  // 1,000 needed: passes only if the buffer halved it
  const Outcome out = d.check(RuleCode::kMargin, d.live_order());
  EXPECT_EQ(out.verdict, Verdict::kReject);
  EXPECT_EQ(out.reason, RuleReason::kRule);
}

// D-27: with margin not loaded a live opening order is refused naming it, and a live reducing order still
// goes.
TEST(RMargin, WithNoMarginLoadedALiveOpeningOrderIsRefusedAndAReducingOneStillPasses) {
  ScopeDesk d;
  d.ctx.margin = nullptr;
  d.fill(Side::kBuy, 10, rupees(100));
  OrderDetails reducing = d.live_order();
  reducing.side = Side::kSell;
  reducing.qty = 4;
  EXPECT_TRUE(d.run_all(reducing).approved.has_value());
  OrderDetails opening = d.live_order();
  opening.qty = 1;
  const GateDecision refused = d.run_all(opening);
  ASSERT_TRUE(refused.rejection.has_value());
  EXPECT_EQ(refused.rejection.value().code, RuleCode::kMargin);
  EXPECT_EQ(reason_text(refused.rejection.value()), "R-DATA-MISSING:margin");
}

// ---- R-DISCIPLINE ----

TEST(RDiscipline, AnUnloadedTableRefusesALiveOrderAndLetsAPaperOrderThrough) {
  ScopeDesk d;
  d.discipline_table->set_loaded(false);
  EXPECT_TRUE(names(d.check(RuleCode::kDiscipline, d.live_order()), DataSource::kDiscipline));
  EXPECT_PASS(d, kDiscipline, d.order());
  d.ctx.discipline = nullptr;
  EXPECT_TRUE(names(d.check(RuleCode::kDiscipline, d.live_order()), DataSource::kDiscipline));
  EXPECT_PASS(d, kDiscipline, d.order());
}

TEST(RDiscipline, ALoadedTableWithNoRowForTheUserLetsEveryOrderThrough) {
  const ScopeDesk d;
  EXPECT_PASS(d, kDiscipline, d.live_order());
  EXPECT_PASS(d, kDiscipline, d.order());
}

TEST(RDiscipline, ALockoutCoversEveryDayThroughItsLastAndNotTheNext) {
  ScopeDesk d;
  const Nanos today = md::ist_day_start(d.clock.now());
  const Nanos day = Nanos{1'440} * kNanosPerMinute;
  const auto mode = oms::kPaperOrder;
  ASSERT_TRUE(d.discipline_table->lock(kUser, mode, today + day));  // through tomorrow
  const Outcome out = d.check(RuleCode::kDiscipline, d.order());
  EXPECT_EQ(out.verdict, Verdict::kReject);
  EXPECT_EQ(out.limit, today + day);
  EXPECT_EQ(out.current, today);
  ASSERT_TRUE(d.discipline_table->lock(kUser, mode, today));  // through today: the last day
  EXPECT_REJECT(d, kDiscipline, d.order());
  ASSERT_TRUE(d.discipline_table->lock(kUser, mode, today - day));  // ended yesterday
  EXPECT_PASS(d, kDiscipline, d.order());
  d.clock.advance(day);  // and the lockout through today ends when tomorrow starts
  ASSERT_TRUE(d.discipline_table->lock(kUser, mode, today));
  EXPECT_PASS(d, kDiscipline, d.order());
}

// Paper and live keep separate lockouts (owner, 2026-11-15).
TEST(RDiscipline, ALiveLockoutBlocksLiveOrdersOnlyAndALockoutIsThatUsersOnly) {
  ScopeDesk d;
  ASSERT_TRUE(d.discipline_table->lock(kUser, oms::kLiveOrder, md::ist_day_start(d.clock.now())));
  EXPECT_REJECT(d, kDiscipline, d.live_order());
  EXPECT_PASS(d, kDiscipline, d.order());  // paper goes on
  OrderDetails other = d.live_order();
  other.user_id = id_of(55);
  EXPECT_PASS(d, kDiscipline, other);
}

TEST(RDiscipline, APaperLockoutBlocksPaperOrdersOnly) {
  ScopeDesk d;
  ASSERT_TRUE(d.discipline_table->lock(kUser, oms::kPaperOrder, md::ist_day_start(d.clock.now())));
  EXPECT_REJECT(d, kDiscipline, d.order());
  EXPECT_PASS(d, kDiscipline, d.live_order());
}

TEST(RDiscipline, WithBothLockedBothAreBlockedAndClearingOneFreesOnlyThatMode) {
  ScopeDesk d;
  const Nanos today = md::ist_day_start(d.clock.now());
  ASSERT_TRUE(d.discipline_table->lock(kUser, oms::kPaperOrder, today));
  ASSERT_TRUE(d.discipline_table->lock(kUser, oms::kLiveOrder, today));
  EXPECT_REJECT(d, kDiscipline, d.order());
  EXPECT_REJECT(d, kDiscipline, d.live_order());
  d.discipline_table->clear(kUser, oms::kLiveOrder);
  EXPECT_REJECT(d, kDiscipline, d.order());
  EXPECT_PASS(d, kDiscipline, d.live_order());
}

TEST(RDiscipline, ALockedTraderCanStillReduceBecauseTheRuleOnlyAppliesToOpeningOrders) {
  ScopeDesk d;
  d.fill(Side::kBuy, 10, rupees(100));
  ASSERT_TRUE(d.discipline_table->lock(kUser, oms::kPaperOrder, md::ist_day_start(d.clock.now())));
  EXPECT_TRUE(d.run_all(d.sell(4)).approved.has_value());
  const GateDecision opening = d.run_all(d.buy(1));
  ASSERT_TRUE(opening.rejection.has_value());
  EXPECT_EQ(opening.rejection.value().code, RuleCode::kDiscipline);
  EXPECT_EQ(reason_text(opening.rejection.value()), "R-DISCIPLINE");
}

TEST(DataMissing, TheReasonTextNamesTheDiscipline) {
  const RuleResult result{.code = RuleCode::kDiscipline,
                          .verdict = Verdict::kReject,
                          .reason = RuleReason::kDataMissing,
                          .source = DataSource::kDiscipline};
  EXPECT_EQ(reason_text(result), "R-DATA-MISSING:discipline");
}

}  // namespace
}  // namespace vyuha::risk
// NOLINTEND(bugprone-unchecked-optional-access)
