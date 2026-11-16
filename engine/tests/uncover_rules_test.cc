// Rule 18 beyond the opening sell (M2.4b, D-38, D-39 (10) and (11)): cover compared in units, an order that
// would take cover away from a short is refused, and a flip is opening as a whole with its closing part
// reported.

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "desk_fixture.h"
#include "scope_desk.h"

// NOLINTBEGIN(bugprone-unchecked-optional-access): every test does ASSERT_TRUE(has_value()) first
namespace vyuha::risk {
namespace {

constexpr Nanos kDay = Nanos{1'440} * kNanosPerMinute;

[[nodiscard]] bool no_cover(const Outcome& out) {
  return out.verdict == Verdict::kReject && out.reason == RuleReason::kNoCover;
}
[[nodiscard]] bool uncovers(const Outcome& out) {
  return out.verdict == Verdict::kReject && out.reason == RuleReason::kUncover;
}
[[nodiscard]] bool refused_as_uncovering(const GateDecision& out) {
  return !out.approved.has_value() && out.rejection.has_value() &&
         out.rejection->code == RuleCode::kShortOpt && out.rejection->reason == RuleReason::kUncover &&
         reason_text(out.rejection.value()) == "R-SHORT-OPT-UNCOVER";
}

// A desk that holds the book a test sets up: every position is filled at 100 and marked at 100 (so P&L is 0
// and no mark is missing), with fresh Greeks, so any order of any kind can reach the rules after 18.
struct Hedge : ScopeDesk {
  Hedge() { fresh_greeks_all(); }
  void hold(Side side, std::int64_t qty, std::int64_t id, const Attribution& by = who()) {
    fill(side, qty, rupees(100), id, by);
    mark(id, rupees(100));
  }
  // A contract like `from`, with its own lot size and, if given, expiry. Marked and ticked like the rest.
  void clone(std::int64_t id, std::int64_t from, std::int32_t lot,
             std::optional<Nanos> expiry = std::nullopt) {
    InstrumentRef r = *instruments.find(from);
    r.lot_size = lot;
    if (expiry.has_value()) {
      r.expires_at = *expiry;
    }
    ASSERT_TRUE(instruments.put(id, r));
    tick(id, rupees(100));
    ASSERT_TRUE(margin_cache->set_per_lot(id, rupees(100).units(), clock.monotonic()));
  }
  [[nodiscard]] OrderDetails in(std::int64_t id, Side side, std::int64_t qty, bool live = false) const {
    OrderDetails o = live ? live_order() : order();
    o.instrument_id = id;
    o.side = side;
    o.qty = static_cast<std::int32_t>(qty);
    o.price = rupees(100);
    return o;
  }
  // Rule 18 alone for an order that reduces (the gate counts it so): what the gate asks it.
  [[nodiscard]] Outcome rule18(const OrderDetails& o) const { return check_reducing(RuleCode::kShortOpt, o); }
  [[nodiscard]] Nanos expiry_of(std::int64_t id) const { return instruments.find(id)->expires_at; }
};

// ---- cover is compared in units, whatever the lot sizes ----

TEST(CoverUnits, OneLotOfTheSmallerContractDoesntCoverOneLotOfTheLargerAndTwoDo) {
  Hedge d;
  d.clone(500, kOption, 50);  // a call of the same underlying, expiry and kind, with a lot of 50
  d.hold(Side::kBuy, 50, 500);
  const Outcome one = d.check(RuleCode::kShortOpt, d.in(kCall75, Side::kSell, 75));
  EXPECT_TRUE(no_cover(one));
  EXPECT_EQ(one.limit, 50);  // units of cover, units needed
  EXPECT_EQ(one.current, 75);
  EXPECT_FALSE(d.run_all(d.in(kCall75, Side::kSell, 75)).approved.has_value());
  d.hold(Side::kBuy, 50, 500);  // two lots of the 50: 100 units
  EXPECT_EQ(d.verdict(RuleCode::kShortOpt, d.in(kCall75, Side::kSell, 75)), Verdict::kPass);
  EXPECT_TRUE(d.run_all(d.in(kCall75, Side::kSell, 75)).approved.has_value());
  const Outcome two = d.check(RuleCode::kShortOpt, d.in(kCall75, Side::kSell, 150));
  EXPECT_TRUE(no_cover(two));
  EXPECT_EQ(two.limit, 100);
  EXPECT_EQ(two.current, 150);
}

TEST(CoverUnits, ALongOfTheLargerContractCoversASmallerShortWithUnitsToSpare) {
  Hedge d;
  d.hold(Side::kBuy, 75, kCall75);
  EXPECT_EQ(d.verdict(RuleCode::kShortOpt, d.in(kOption, Side::kSell, 65)), Verdict::kPass);  // 65 of 75
  EXPECT_TRUE(no_cover(d.check(RuleCode::kShortOpt, d.in(kOption, Side::kSell, 130))));       // 130 of 75
}

TEST(CoverUnits, AFutureOfLot25CoversAShortCallUnitForUnit) {
  Hedge d;
  d.hold(Side::kBuy, 75, kHedgeFut25);
  EXPECT_EQ(d.verdict(RuleCode::kShortOpt, d.in(kCall75, Side::kSell, 75)), Verdict::kPass);  // 3 lots: 75
  Hedge e;
  e.hold(Side::kBuy, 50, kHedgeFut25);
  const Outcome out = e.check(RuleCode::kShortOpt, e.in(kCall75, Side::kSell, 75));  // 2 lots: 50
  EXPECT_TRUE(no_cover(out));
  EXPECT_EQ(out.limit, 50);
  EXPECT_EQ(out.current, 75);
}

TEST(CoverUnits, ShortsHeldInOtherLotSizesCountInUnitsToo) {
  Hedge d;
  d.hold(Side::kBuy, 130, kOption);  // 130 units of cover
  d.hold(Side::kSell, 75, kCall75);  // one lot of 75 short, covered
  EXPECT_EQ(d.verdict(RuleCode::kShortOpt, d.in(kOption2, Side::kSell, 55)), Verdict::kPass);  // 130 of 130
  const Outcome out = d.check(RuleCode::kShortOpt, d.in(kOption2, Side::kSell, 65));
  EXPECT_TRUE(no_cover(out));
  EXPECT_EQ(out.limit, 130);
  EXPECT_EQ(out.current, 140);
}

TEST(CoverUnits, TheShortOptionLotsLimitStaysInLots) {
  Hedge d;
  d.limit(LimitKey::kNakedShortOptions, 1 * kFromScale);
  d.clear_limits(LimitKey::kMaxShortOptionLots);
  d.limit(LimitKey::kMaxShortOptionLots, 2 * kFromScale);
  d.hold(Side::kSell, 75, kCall75);  // one lot
  EXPECT_EQ(d.verdict(RuleCode::kShortOpt, d.in(kOption, Side::kSell, 65)),
            Verdict::kPass);  // the second lot
  const Outcome out = d.check(RuleCode::kShortOpt, d.in(kOption, Side::kSell, 130));
  EXPECT_EQ(out.verdict, Verdict::kReject);
  EXPECT_FALSE(no_cover(out));
  EXPECT_EQ(out.limit, 2 * kFromScale);
  EXPECT_EQ(out.current, 3 * kFromScale);
}

// ---- ScopeBook::uncover, directly ----

TEST(Uncover, SellingTheLongThatCoversAShortLeavesItBareWithNeedAndHave) {
  Hedge d;
  d.hold(Side::kBuy, 130, kOption);
  d.hold(Side::kSell, 130, kOption2);  // covered by the long: same underlying, expiry and kind
  const ScopeBook::Uncover left = d.sb.uncover(kAccount, kOption, Side::kSell, 65);
  EXPECT_TRUE(left.uncovers);
  EXPECT_EQ(left.need, 65);  // short units left wanting
  EXPECT_EQ(left.have, 0);   // futures left for them
  EXPECT_FALSE(d.sb.uncover(kAccount, kOption, Side::kSell, 0).uncovers);
  EXPECT_FALSE(d.sb.uncover(kAccount, kOption, Side::kBuy, 65).uncovers);  // a buy takes nothing away
}

TEST(Uncover, AFutureLeftOverCoversWhatTheLongNoLongerDoes) {
  Hedge d;
  d.hold(Side::kBuy, 130, kOption);
  d.hold(Side::kSell, 130, kOption2);
  d.hold(Side::kBuy, 65, kHedgeFut);  // a long future that expires after the options
  EXPECT_FALSE(d.sb.uncover(kAccount, kOption, Side::kSell, 65).uncovers);  // 65 bare, 65 of future
  const ScopeBook::Uncover left = d.sb.uncover(kAccount, kOption, Side::kSell, 130);
  EXPECT_TRUE(left.uncovers);
  EXPECT_EQ(left.need, 130);
  EXPECT_EQ(left.have, 65);
}

TEST(Uncover, SellingTheHedgeFutureReportsWhatItLeavesBehind) {
  Hedge d;
  d.hold(Side::kBuy, 130, kHedgeFut);
  d.hold(Side::kSell, 130, kOption);
  EXPECT_FALSE(d.sb.uncover(kAccount, kHedgeFut, Side::kSell, 0).uncovers);
  const ScopeBook::Uncover left = d.sb.uncover(kAccount, kHedgeFut, Side::kSell, 65);
  EXPECT_TRUE(left.uncovers);
  EXPECT_EQ(left.need, 130);
  EXPECT_EQ(left.have, 65);
  EXPECT_FALSE(d.sb.uncover(kAccount, kHedgeFut, Side::kBuy, 65).uncovers);  // buying more takes nothing away
}

TEST(Uncover, ANothingHeldAShareAnotherAccountOrAPlainLongIsNotUncovering) {
  Hedge d;
  EXPECT_FALSE(d.sb.uncover(kAccount, kOption, Side::kSell, 65).uncovers);  // no position at all
  d.hold(Side::kBuy, 130, kOption);
  EXPECT_FALSE(d.sb.uncover(kAccount, kOption, Side::kSell, 130).uncovers);  // no short to be left bare
  EXPECT_FALSE(d.sb.uncover(kAccount, kEquity, Side::kSell, 10).uncovers);   // a share
  d.hold(Side::kSell, 130, kOption2, {.account = kOtherAccount});            // another account's short
  EXPECT_FALSE(d.sb.uncover(kAccount, kOption, Side::kSell, 130).uncovers);
  EXPECT_FALSE(d.sb.uncover(kOtherAccount, kOption2, Side::kBuy, 1).uncovers);
}

TEST(Uncover, ASoldLongOptionCountsOnlyUpToWhatItHolds) {
  Hedge d;
  d.hold(Side::kBuy, 65, kOption);
  d.hold(Side::kSell, 65, kOption2);
  const ScopeBook::Uncover left = d.sb.uncover(kAccount, kOption, Side::kSell, 1'000);
  EXPECT_TRUE(left.uncovers);
  EXPECT_EQ(left.need, 65);  // it can leave only the 65 it holds bare: the rest would be opening
}

TEST(Uncover, AShortFutureCoversPutsAndABuyBackUncoversThem) {
  Hedge d;
  d.hold(Side::kSell, 130, kHedgeFut);
  d.hold(Side::kSell, 130, kPutOption);
  EXPECT_TRUE(d.sb.uncover(kAccount, kHedgeFut, Side::kBuy, 65).uncovers);
  EXPECT_FALSE(d.sb.uncover(kAccount, kHedgeFut, Side::kSell, 65).uncovers);  // more short: more cover
  Hedge calls;                                                                // short calls are no concern
  calls.hold(Side::kSell, 130, kHedgeFut);
  calls.hold(Side::kSell, 130, kOption);
  EXPECT_FALSE(calls.sb.uncover(kAccount, kHedgeFut, Side::kBuy, 130).uncovers);
}

TEST(Uncover, WorkingOrdersOnTheClosingSideOfTheHedgeAlreadyReduceWhatItGives) {
  Hedge d;
  d.hold(Side::kBuy, 195, kHedgeFut);
  d.hold(Side::kSell, 130, kOption);
  EXPECT_FALSE(d.sb.uncover(kAccount, kHedgeFut, Side::kSell, 65).uncovers);  // 195 - 65 = 130: still enough
  ASSERT_TRUE(d.sb.add_working(d.who(), kHedgeFut, Side::kSell, 65));
  EXPECT_TRUE(d.sb.uncover(kAccount, kHedgeFut, Side::kSell, 65).uncovers);  // 130 given, 65 more taken
  ASSERT_TRUE(d.sb.add_working(d.who(), kHedgeFut, Side::kSell, -65));
  EXPECT_FALSE(d.sb.uncover(kAccount, kHedgeFut, Side::kSell, 65).uncovers);
}

TEST(Uncover, MoreExpiriesThanAreTrackedWithAShortIsUncoveringAndWithoutOneIsNot) {
  Hedge d;
  const InstrumentRef base = *d.instruments.find(kOption);
  for (std::int64_t i = 0; i < 17; ++i) {
    InstrumentRef r = base;
    r.expires_at = base.expires_at + (Nanos{i + 1} * kDay);
    ASSERT_TRUE(d.instruments.put(100 + i, r));
    d.hold(Side::kBuy, kLot, 100 + i);
  }
  EXPECT_FALSE(d.sb.uncover(kAccount, 100, Side::kSell, kLot).uncovers);  // no short: nothing to leave bare
  d.hold(Side::kSell, kLot, kOption2);
  EXPECT_TRUE(d.sb.uncover(kAccount, 100, Side::kSell, kLot).uncovers);  // fail closed
}

TEST(Uncover, ShortsOfALaterExpiryThatWereBareAlreadyDontBlockAnUnrelatedSale) {
  Hedge d;
  d.hold(Side::kSell, 65, kCallFar);      // a far short with no cover at all
  d.hold(Side::kBuy, 65, kHedgeFutNear);  // a future that expires before it: covers nothing of it
  d.hold(Side::kBuy, 65, kPutOption);     // a long put: unrelated to short calls
  d.hold(Side::kBuy, 65, kOption);        // a near long call, with no near short to cover
  EXPECT_FALSE(d.sb.uncover(kAccount, kHedgeFutNear, Side::kSell, 65).uncovers);
  EXPECT_FALSE(d.sb.uncover(kAccount, kPutOption, Side::kSell, 65).uncovers);
  EXPECT_FALSE(d.sb.uncover(kAccount, kOption, Side::kSell, 65).uncovers);
  // but selling one more of what the far short does use is worse
  d.clone(502, kCallFar, kLot);  // another far call, which does cover the far short
  d.hold(Side::kBuy, 65, 502);
  EXPECT_TRUE(d.sb.uncover(kAccount, 502, Side::kSell, 65).uncovers);
}

TEST(Uncover, TheNearFutureIsUsedUpByTheNearShortSoSellingTheFarLongsStillLeavesEverythingCovered) {
  Hedge d;
  d.clone(501, kHedgeFut, 5, d.expiry_of(kOption));  // a future of the near expiry, lot 5
  d.clone(502, kCallFar, 5);                         // a far call, lot 5
  d.hold(Side::kBuy, 10, 501);                       // near future, 10 units
  d.hold(Side::kSell, 10, kOption);                  // near short, 10 units: uses all of it
  d.hold(Side::kBuy, 5, 502);                        // far longs, 5
  d.hold(Side::kSell, 5, kCallFar);                  // far shorts, 5
  d.hold(Side::kBuy, 5, kHedgeFut);                  // far future, 5
  EXPECT_FALSE(d.sb.uncover(kAccount, 502, Side::kSell, 5).uncovers);
  EXPECT_EQ(d.rule18(d.in(502, Side::kSell, 5)).verdict, Verdict::kPass);
  EXPECT_TRUE(d.run_all(d.in(502, Side::kSell, 5)).approved.has_value());
  // Without the far future the far shorts have only their own longs, so selling them is refused.
  Hedge e;
  e.clone(501, kHedgeFut, 5, e.expiry_of(kOption));
  e.clone(502, kCallFar, 5);
  e.hold(Side::kBuy, 10, 501);
  e.hold(Side::kSell, 10, kOption);
  e.hold(Side::kBuy, 5, 502);
  e.hold(Side::kSell, 5, kCallFar);
  EXPECT_TRUE(e.sb.uncover(kAccount, 502, Side::kSell, 5).uncovers);
  // Selling the whole near future does leave the near short short of cover: the far future alone isn't
  // enough.
  EXPECT_FALSE(d.sb.uncover(kAccount, 501, Side::kSell, 5).uncovers);  // the far future takes up the 5
  EXPECT_TRUE(d.sb.uncover(kAccount, 501, Side::kSell, 10).uncovers);
}

// ---- rule 18: R-SHORT-OPT-UNCOVER ----

TEST(RShortOptUncover, SellingTheCoveringLongCallWithAShortCallOfTheSameExpiryIsRefused) {
  for (const bool live : {false, true}) {
    Hedge d;
    d.hold(Side::kBuy, 130, kOption);
    d.hold(Side::kSell, 130, kOption2);
    const OrderDetails sell = d.in(kOption, Side::kSell, 65, live);
    EXPECT_TRUE(uncovers(d.rule18(sell)));
    EXPECT_EQ(d.rule18(sell).limit, 0);     // futures left
    EXPECT_EQ(d.rule18(sell).current, 65);  // short units left wanting
    EXPECT_TRUE(refused_as_uncovering(d.run_all(sell))) << live;
  }
}

TEST(RShortOptUncover, ThePartialThatLeavesLongUnitsEqualToShortUnitsPassesAndOneLotMoreIsRefused) {
  for (const bool live : {false, true}) {
    Hedge d;
    d.hold(Side::kBuy, 195, kOption);
    d.hold(Side::kSell, 130, kOption2);
    EXPECT_EQ(d.rule18(d.in(kOption, Side::kSell, 65, live)).verdict, Verdict::kPass);  // 130 left for 130
    EXPECT_TRUE(d.run_all(d.in(kOption, Side::kSell, 65, live)).approved.has_value()) << live;
    EXPECT_TRUE(uncovers(d.rule18(d.in(kOption, Side::kSell, 130, live))));  // 65 left for 130
    EXPECT_TRUE(refused_as_uncovering(d.run_all(d.in(kOption, Side::kSell, 130, live)))) << live;
  }
}

TEST(RShortOptUncover, SellingTheHedgeLongFutureWithAShortCallIsRefusedAndAPartialThatLeavesEnoughPasses) {
  for (const bool live : {false, true}) {
    Hedge d;
    d.hold(Side::kBuy, 195, kHedgeFut);
    d.hold(Side::kSell, 130, kOption);
    EXPECT_EQ(d.rule18(d.in(kHedgeFut, Side::kSell, 65, live)).verdict, Verdict::kPass);
    EXPECT_TRUE(d.run_all(d.in(kHedgeFut, Side::kSell, 65, live)).approved.has_value()) << live;
    EXPECT_TRUE(uncovers(d.rule18(d.in(kHedgeFut, Side::kSell, 130, live))));
    EXPECT_TRUE(refused_as_uncovering(d.run_all(d.in(kHedgeFut, Side::kSell, 130, live)))) << live;
    EXPECT_TRUE(refused_as_uncovering(d.run_all(d.in(kHedgeFut, Side::kSell, 195, live)))) << live;
  }
}

TEST(RShortOptUncover, BuyingBackTheShortFutureWithAShortPutIsRefusedAndAPartialThatLeavesEnoughPasses) {
  for (const bool live : {false, true}) {
    Hedge d;
    d.hold(Side::kSell, 195, kHedgeFut);
    d.hold(Side::kSell, 130, kPutOption);
    EXPECT_TRUE(d.run_all(d.in(kHedgeFut, Side::kBuy, 65, live)).approved.has_value()) << live;
    EXPECT_TRUE(refused_as_uncovering(d.run_all(d.in(kHedgeFut, Side::kBuy, 130, live)))) << live;
    EXPECT_TRUE(refused_as_uncovering(d.run_all(d.in(kHedgeFut, Side::kBuy, 195, live)))) << live;
  }
}

TEST(RShortOptUncover, GettingFlatMeansTheShortFirstAndThenTheHedge) {
  for (const bool live : {false, true}) {
    Hedge d;
    d.hold(Side::kBuy, 130, kHedgeFut);
    d.hold(Side::kSell, 130, kOption);
    EXPECT_TRUE(
        refused_as_uncovering(d.run_all(d.in(kHedgeFut, Side::kSell, 130, live))));  // hedge first: no
    const GateDecision close_short = d.run_all(d.in(kOption, Side::kBuy, 130, live));
    ASSERT_TRUE(close_short.approved.has_value()) << live;  // the short first
    d.fill(Side::kBuy, 130, rupees(100), kOption);
    EXPECT_TRUE(d.run_all(d.in(kHedgeFut, Side::kSell, 130, live)).approved.has_value())
        << live;  // then the hedge
  }
}

TEST(RShortOptUncover, ALongOptionWithNoShortsAndALongPutBesideShortCallsSellFreely) {
  for (const bool live : {false, true}) {
    Hedge d;
    d.hold(Side::kBuy, 130, kOption);
    EXPECT_TRUE(d.run_all(d.in(kOption, Side::kSell, 130, live)).approved.has_value()) << live;
    Hedge e;
    e.hold(Side::kBuy, 130, kPutOption);
    e.hold(Side::kBuy, 130, kHedgeFut);  // cover for the short calls, so they are not bare to begin with
    e.hold(Side::kSell, 130, kOption);
    EXPECT_TRUE(e.run_all(e.in(kPutOption, Side::kSell, 130, live)).approved.has_value()) << live;
  }
}

TEST(RShortOptUncover, WhenNakedShortsAreAllowedTheOrderIsNotAskedForCover) {
  Hedge d;
  d.limit(LimitKey::kNakedShortOptions, 1 * kFromScale);
  d.hold(Side::kBuy, 130, kOption);
  d.hold(Side::kSell, 130, kOption2);
  EXPECT_EQ(d.rule18(d.in(kOption, Side::kSell, 130)).verdict, Verdict::kPass);
  EXPECT_TRUE(d.run_all(d.in(kOption, Side::kSell, 130)).approved.has_value());
  Hedge e;  // a zero row: the strictest wins, so not allowed
  e.limit(LimitKey::kNakedShortOptions, 1 * kFromScale);
  e.limit(LimitKey::kNakedShortOptions, 0, LimitScope::kAccount, true, kAccount);
  e.hold(Side::kBuy, 130, kOption);
  e.hold(Side::kSell, 130, kOption2);
  EXPECT_TRUE(refused_as_uncovering(e.run_all(e.in(kOption, Side::kSell, 130))));
}

TEST(RShortOptUncover, WorkingSellsOnTheHedgeCountAgainstWhatItGives) {
  Hedge d;
  d.hold(Side::kBuy, 195, kHedgeFut);
  d.hold(Side::kSell, 130, kOption);
  EXPECT_EQ(d.rule18(d.in(kHedgeFut, Side::kSell, 65)).verdict, Verdict::kPass);  // 130 left for 130
  ASSERT_TRUE(d.sb.add_working(d.who(), kHedgeFut, Side::kSell, 65));
  EXPECT_TRUE(refused_as_uncovering(d.run_all(d.in(kHedgeFut, Side::kSell, 65))));  // 130 gone with it
}

TEST(RShortOptUncover, AShortAlreadyBareDoesNotBlockSellingWhatItDoesntNeed) {
  Hedge d;
  d.hold(Side::kSell, 65, kCallFar);      // bare from the start
  d.hold(Side::kBuy, 65, kHedgeFutNear);  // expires before it: no cover for it
  d.hold(Side::kBuy, 65, kPutOption);
  EXPECT_TRUE(d.run_all(d.in(kHedgeFutNear, Side::kSell, 65)).approved.has_value());
  EXPECT_TRUE(d.run_all(d.in(kPutOption, Side::kSell, 65)).approved.has_value());
}

TEST(RShortOptUncover, MoreExpiriesThanAreTrackedRefusesWithAShortAndPassesWithout) {
  for (const bool with_short : {true, false}) {
    Hedge d;
    const InstrumentRef base = *d.instruments.find(kOption);
    for (std::int64_t i = 0; i < 17; ++i) {
      InstrumentRef r = base;
      r.expires_at = base.expires_at + (Nanos{i + 1} * kDay);
      ASSERT_TRUE(d.instruments.put(100 + i, r));
      d.tick(100 + i, rupees(100));
      d.hold(Side::kBuy, kLot, 100 + i);
    }
    if (with_short) {
      d.hold(Side::kSell, kLot, kOption2);
    }
    const GateDecision out = d.run_all(d.in(100, Side::kSell, kLot));
    EXPECT_EQ(refused_as_uncovering(out), with_short);
    EXPECT_EQ(out.approved.has_value(), !with_short);
  }
}

TEST(RShortOptUncover, AnotherAccountsShortsAreIgnored) {
  Hedge d;
  d.hold(Side::kBuy, 130, kOption);
  d.hold(Side::kSell, 130, kOption2, {.account = kOtherAccount});
  EXPECT_TRUE(d.run_all(d.in(kOption, Side::kSell, 130)).approved.has_value());
}

TEST(RShortOptUncover, AFlipOfTheHedgeFutureIsRefusedAsUncoveringWhenTheOtherRulesAreLoose) {
  for (const bool live : {false, true}) {
    Hedge d;
    d.hold(Side::kBuy, 65, kHedgeFut);
    d.hold(Side::kSell, 65, kOption);
    const GateDecision out = d.run_all(d.in(kHedgeFut, Side::kSell, 130, live));  // sells through flat
    EXPECT_TRUE(refused_as_uncovering(out)) << live;
    EXPECT_EQ(out.flip_close_qty, 65);
  }
}

TEST(RShortOptUncover, AFlipOfTheCoveringLongOptionIsAnOpeningShortWithNoCover) {
  for (const bool live : {false, true}) {
    Hedge d;
    d.hold(Side::kBuy, 65, kOption);
    d.hold(Side::kSell, 65, kOption2);
    const GateDecision out = d.run_all(d.in(kOption, Side::kSell, 130, live));
    ASSERT_TRUE(out.rejection.has_value()) << live;
    EXPECT_EQ(out.rejection->code, RuleCode::kShortOpt);
    EXPECT_EQ(out.rejection->reason, RuleReason::kNoCover);
    EXPECT_EQ(reason_text(out.rejection.value()), "R-SHORT-OPT:no-cover");
  }
}

TEST(RShortOptUncover, OnlyRule18SeesAReducingOrderAndSharesAndOptionBuysPassIt) {
  Hedge d;
  d.hold(Side::kBuy, 130, kOption);
  d.hold(Side::kSell, 130, kOption2);
  d.hold(Side::kBuy, 10, kEquity);
  EXPECT_EQ(d.rule18(d.in(kEquity, Side::kSell, 10)).verdict, Verdict::kPass);
  EXPECT_EQ(d.rule18(d.in(kOption2, Side::kBuy, 65)).verdict, Verdict::kPass);  // closing the short itself
  EXPECT_EQ(d.rule18(d.in(kOption, Side::kBuy, 65)).verdict, Verdict::kPass);
  const OrderDetails tight = d.in(kOption, Side::kSell, 65);
  d.clear_limits(LimitKey::kMaxOpenOrders);
  d.limit(LimitKey::kMaxOpenOrders, 0);
  d.clear_limits(LimitKey::kMaxDailyLoss);
  d.limit(LimitKey::kMaxDailyLoss, 0);
  EXPECT_TRUE(refused_as_uncovering(d.run_all(tight)));  // rules 14 and 19 would refuse an opening order
}

TEST(RShortOptUncover, WithNoPositionsLoadedAnOptionSellOrAnyFutureOrderNamesThemAndOthersPass) {
  Hedge d;
  d.sb.set_positions_loaded(false);
  EXPECT_TRUE(names(d.rule18(d.in(kOption, Side::kSell, 65)), DataSource::kPositions));
  EXPECT_TRUE(names(d.rule18(d.in(kHedgeFut, Side::kBuy, 65)), DataSource::kPositions));
  EXPECT_TRUE(names(d.rule18(d.in(kHedgeFut, Side::kSell, 65)), DataSource::kPositions));
  EXPECT_EQ(d.rule18(d.in(kOption, Side::kBuy, 65)).verdict, Verdict::kPass);
  EXPECT_EQ(d.rule18(d.in(kEquity, Side::kSell, 10)).verdict, Verdict::kPass);
  EXPECT_EQ(d.rule18(d.in(kEquity, Side::kBuy, 10)).verdict, Verdict::kPass);
  const GateDecision out = d.run_all(d.in(kHedgeFut, Side::kSell, 65));
  ASSERT_TRUE(out.rejection.has_value());
  EXPECT_EQ(reason_text(out.rejection.value()), "R-DATA-MISSING:positions");
}

// ---- flips (D-38): opening as a whole order, with the closing part reported ----

// Every limit rules 14 to 22 read at its tightest, so any opening order is refused.
void tighten_all(ScopeDesk& d) {
  for (const LimitKey key :
       {LimitKey::kMaxOpenOrders, LimitKey::kMaxPositionLots, LimitKey::kMaxPositionNotional,
        LimitKey::kMaxGrossExposure, LimitKey::kMaxNetDeltaInr, LimitKey::kMaxNetVegaInr,
        LimitKey::kMaxDailyLoss, LimitKey::kMaxDrawdownFromPeak, LimitKey::kMaxShortOptionLots}) {
    d.clear_limits(key);
    d.limit(key, 0);
  }
}

TEST(Flip, ALongSoldThroughFlatIsRefusedAndReportsWhatOfItOnlyCloses) {
  for (const bool live : {false, true}) {
    Hedge d;
    d.hold(Side::kBuy, 10, kEquity);
    tighten_all(d);
    const GateDecision flip = d.run_all(d.in(kEquity, Side::kSell, 14, live));
    ASSERT_TRUE(flip.rejection.has_value()) << live;
    EXPECT_GE(flip.rejection->code, RuleCode::kOpenOrders);
    EXPECT_EQ(flip.flip_close_qty, 10);
    const GateDecision close = d.run_all(d.in(kEquity, Side::kSell, 10, live));  // the pure close
    EXPECT_TRUE(close.approved.has_value()) << live;
    EXPECT_EQ(close.flip_close_qty, 0);
    const GateDecision part = d.run_all(d.in(kEquity, Side::kSell, 4, live));
    EXPECT_TRUE(part.approved.has_value()) << live;
    EXPECT_EQ(part.flip_close_qty, 0);
  }
}

TEST(Flip, ARefusedFlipAndAnApprovedOneReportTheSameCloseQuantity) {
  Hedge d;  // loose limits: the flip goes through
  d.hold(Side::kBuy, 10, kEquity);
  const GateDecision flip = d.run_all(d.in(kEquity, Side::kSell, 14));
  EXPECT_TRUE(flip.approved.has_value());
  EXPECT_EQ(flip.flip_close_qty, 10);
}

TEST(Flip, WorkingOrdersOnTheSameSideAreAlreadyClosingAndNeverMoreThanTheNetIsReported) {
  Hedge d;
  d.hold(Side::kBuy, 10, kEquity);
  tighten_all(d);
  ASSERT_TRUE(d.sb.add_working(d.who(), kEquity, Side::kSell, 3));
  EXPECT_EQ(d.run_all(d.in(kEquity, Side::kSell, 14)).flip_close_qty, 7);
  EXPECT_TRUE(d.run_all(d.in(kEquity, Side::kSell, 7)).approved.has_value());  // 3 and 7 close it
  ASSERT_TRUE(d.sb.add_working(d.who(), kEquity, Side::kSell, 7));             // 10 working in all
  const GateDecision flip = d.run_all(d.in(kEquity, Side::kSell, 1));
  EXPECT_TRUE(flip.rejection.has_value());
  EXPECT_EQ(flip.flip_close_qty, 0);
  ASSERT_TRUE(d.sb.add_working(d.who(), kEquity, Side::kSell, 5));  // more than the net
  EXPECT_EQ(d.run_all(d.in(kEquity, Side::kSell, 1)).flip_close_qty, 0);
}

TEST(Flip, AnOrderThatIsNotAFlipReportsNothing) {
  Hedge d;
  d.hold(Side::kBuy, 10, kEquity);
  tighten_all(d);
  const GateDecision more = d.run_all(d.in(kEquity, Side::kBuy, 5));  // opening, refused, no flip
  EXPECT_TRUE(more.rejection.has_value());
  EXPECT_EQ(more.flip_close_qty, 0);
  EXPECT_EQ(d.run_all(d.in(kEquity, Side::kSell, 4)).flip_close_qty, 0);
  Hedge flat;  // nothing held: a sell opens a short and is no flip
  tighten_all(flat);
  EXPECT_EQ(flat.run_all(flat.in(kEquity, Side::kSell, 4)).flip_close_qty, 0);
}

TEST(Flip, AShortBoughtThroughFlatMirrorsIt) {
  Hedge d;
  d.hold(Side::kSell, 10, kEquity);
  tighten_all(d);
  const GateDecision flip = d.run_all(d.in(kEquity, Side::kBuy, 14));
  ASSERT_TRUE(flip.rejection.has_value());
  EXPECT_EQ(flip.flip_close_qty, 10);
  EXPECT_TRUE(d.run_all(d.in(kEquity, Side::kBuy, 10)).approved.has_value());
  ASSERT_TRUE(d.sb.add_working(d.who(), kEquity, Side::kBuy, 3));
  EXPECT_EQ(d.run_all(d.in(kEquity, Side::kBuy, 14)).flip_close_qty, 7);
  ASSERT_TRUE(d.sb.add_working(d.who(), kEquity, Side::kBuy, 20));
  EXPECT_EQ(d.run_all(d.in(kEquity, Side::kBuy, 14)).flip_close_qty, 0);
}

TEST(Flip, TheAccountsPositionDecidesNotAnotherAccounts) {
  Hedge d;
  d.hold(Side::kBuy, 10, kEquity, {.account = kOtherAccount});
  tighten_all(d);
  EXPECT_EQ(d.run_all(d.in(kEquity, Side::kSell, 14)).flip_close_qty, 0);  // this account holds none
}

TEST(Flip, WithNoPositionsLoadedNothingIsKnownSoNothingIsReported) {
  Hedge d;
  d.hold(Side::kBuy, 10, kEquity);
  tighten_all(d);
  d.sb.set_positions_loaded(false);
  const GateDecision out = d.run_all(d.in(kEquity, Side::kSell, 14));
  ASSERT_TRUE(out.rejection.has_value());
  EXPECT_EQ(reason_text(out.rejection.value()), "R-DATA-MISSING:positions");
  EXPECT_EQ(out.flip_close_qty, 0);
}

}  // namespace
}  // namespace vyuha::risk
// NOLINTEND(bugprone-unchecked-optional-access)
