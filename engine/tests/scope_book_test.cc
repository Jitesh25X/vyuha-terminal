// The scope book (M2.4a): positions, P&L and exposure per scope, kept current by fills, marks and Greeks.

#include "vyuha/risk/scope_book.h"

#include <cstdint>
#include <limits>
#include <optional>

#include <gtest/gtest.h>

#include "vyuha/core/price.h"

namespace vyuha::risk {
namespace {

using oms::Side;
using oms::Uuid;

constexpr std::int64_t kEquity = 1;
constexpr std::int64_t kOption = 2;
constexpr std::int64_t kFuture = 3;
constexpr std::int64_t kOption2 = 5;
constexpr std::int64_t kOption3 = 6;
constexpr Price rupees(std::int64_t r) { return Price::from_units(r * Price::kUnitsPerRupee); }
constexpr std::int64_t kRupee = Price::kUnitsPerRupee;

constexpr Uuid id_of(std::uint8_t b) {
  Uuid id;
  id.bytes.fill(b);
  return id;
}
constexpr Uuid kAccount = id_of(1);
constexpr Uuid kTrader = id_of(2);
constexpr Uuid kStrategy = id_of(3);
constexpr Uuid kOtherAccount = id_of(4);

struct Book {
  InstrumentTable instruments{16};
  ScopeBook book;

  explicit Book(std::size_t capacity = 256) : book(instruments, capacity) {
    EXPECT_TRUE(instruments.put(kEquity, {.lot_size = 1, .tick_size = Price::from_units(500)}));
    EXPECT_TRUE(instruments.put(kFuture, {.lot_size = 1,
                                          .tick_size = Price::from_units(500),
                                          .exchange = Exchange::kNfo,
                                          .segment = Segment::kFut,
                                          .underlying_id = kEquity}));
    EXPECT_TRUE(instruments.put(kOption3, {.lot_size = 65,
                                           .tick_size = Price::from_units(500),
                                           .exchange = Exchange::kNfo,
                                           .segment = Segment::kOpt,
                                           .underlying_id = kEquity}));
    EXPECT_TRUE(instruments.put(kOption2, {.lot_size = 65,
                                           .tick_size = Price::from_units(500),
                                           .exchange = Exchange::kNfo,
                                           .segment = Segment::kOpt,
                                           .underlying_id = kEquity}));
    EXPECT_TRUE(instruments.put(kOption, {.lot_size = 65,
                                          .tick_size = Price::from_units(500),
                                          .exchange = Exchange::kNfo,
                                          .segment = Segment::kOpt,
                                          .underlying_id = kEquity}));
  }
  static Attribution all() { return {.account = kAccount, .trader = kTrader, .strategy = kStrategy}; }
  bool fill(Side side, std::int64_t qty, Price price, std::int64_t instrument = kEquity,
            const Attribution& who = all(), std::int64_t cost = 0) {
    return book.apply_fill(
        {.who = who, .instrument_id = instrument, .side = side, .qty = qty, .price = price, .cost = cost});
  }
  [[nodiscard]] const Position* at(LimitScope kind, const Uuid& id, std::int64_t instrument = kEquity) const {
    return book.position(kind, id, instrument);
  }
};

TEST(ScopeBook, AFillOpensAPositionAtEveryScopeItIsCountedAt) {
  Book b;
  ASSERT_TRUE(b.fill(Side::kBuy, 10, rupees(100)));
  for (const auto& [kind, id] :
       {std::pair{LimitScope::kDesk, Uuid{}}, std::pair{LimitScope::kAccount, kAccount},
        std::pair{LimitScope::kTrader, kTrader}, std::pair{LimitScope::kStrategy, kStrategy}}) {
    const Position* p = b.at(kind, id);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->net_qty, 10);
    EXPECT_EQ(p->avg_price, rupees(100));
  }
  EXPECT_EQ(b.at(LimitScope::kAccount, kOtherAccount), nullptr);
}

// D-35: an order placed in the broker's own app has no trader or strategy.
TEST(ScopeBook, AReconciledFillCountsOnlyAtAccountAndDeskScope) {
  Book b;
  ASSERT_TRUE(b.fill(Side::kBuy, 10, rupees(100), kEquity, {.account = kAccount}));
  EXPECT_NE(b.at(LimitScope::kDesk, Uuid{}), nullptr);
  EXPECT_NE(b.at(LimitScope::kAccount, kAccount), nullptr);
  EXPECT_EQ(b.at(LimitScope::kTrader, kTrader), nullptr);
  EXPECT_EQ(b.at(LimitScope::kStrategy, kStrategy), nullptr);
  EXPECT_EQ(b.book.agg(LimitScope::kTrader, kTrader), nullptr);
}

TEST(ScopeBook, TheDeskAddsUpEveryAccount) {
  Book b;
  ASSERT_TRUE(b.fill(Side::kBuy, 10, rupees(100)));
  ASSERT_TRUE(b.fill(Side::kBuy, 5, rupees(100), kEquity, {.account = kOtherAccount}));
  EXPECT_EQ(b.at(LimitScope::kDesk, Uuid{})->net_qty, 15);
  EXPECT_EQ(b.at(LimitScope::kAccount, kAccount)->net_qty, 10);
  EXPECT_EQ(b.at(LimitScope::kAccount, kOtherAccount)->net_qty, 5);
}

TEST(ScopeBook, TheAveragePriceIsWeightedByQuantity) {
  Book b;
  ASSERT_TRUE(b.fill(Side::kBuy, 10, rupees(100)));
  ASSERT_TRUE(b.fill(Side::kBuy, 30, rupees(120)));
  EXPECT_EQ(b.at(LimitScope::kAccount, kAccount)->net_qty, 40);
  EXPECT_EQ(b.at(LimitScope::kAccount, kAccount)->avg_price, rupees(115));
}

TEST(ScopeBook, ReducingRealisesTheDifferenceAndKeepsTheAverage) {
  Book b;
  ASSERT_TRUE(b.fill(Side::kBuy, 10, rupees(100)));
  ASSERT_TRUE(b.fill(Side::kSell, 4, rupees(110)));
  const Position* p = b.at(LimitScope::kAccount, kAccount);
  EXPECT_EQ(p->net_qty, 6);
  EXPECT_EQ(p->avg_price, rupees(100));
  EXPECT_EQ(p->realised, Wide{4} * 10 * kRupee);
  EXPECT_EQ(b.book.agg(LimitScope::kDesk, Uuid{})->realised, Wide{4} * 10 * kRupee);
}

TEST(ScopeBook, AFlipRealisesTheClosedPartAndReopensTheRestAtTheFillPrice) {
  Book b;
  ASSERT_TRUE(b.fill(Side::kBuy, 10, rupees(100)));
  ASSERT_TRUE(b.fill(Side::kSell, 25, rupees(110)));
  const Position* p = b.at(LimitScope::kAccount, kAccount);
  EXPECT_EQ(p->net_qty, -15);
  EXPECT_EQ(p->avg_price, rupees(110));
  EXPECT_EQ(p->realised, Wide{10} * 10 * kRupee);
}

TEST(ScopeBook, AShortGainsWhenThePriceFalls) {
  Book b;
  ASSERT_TRUE(b.fill(Side::kSell, 10, rupees(100)));
  ASSERT_TRUE(b.fill(Side::kBuy, 10, rupees(90)));
  const Position* p = b.at(LimitScope::kAccount, kAccount);
  EXPECT_EQ(p->net_qty, 0);
  EXPECT_EQ(p->avg_price, Price{});
  EXPECT_EQ(p->realised, Wide{10} * 10 * kRupee);
}

TEST(ScopeBook, AMarkMovesUnrealisedAtEveryScopeWithoutWalkingThePositions) {
  Book b;
  ASSERT_TRUE(b.fill(Side::kBuy, 10, rupees(100)));
  ASSERT_TRUE(b.book.set_mark(kEquity, rupees(105)));
  EXPECT_EQ(b.book.agg(LimitScope::kTrader, kTrader)->unrealised, Wide{10} * 5 * kRupee);
  EXPECT_EQ(b.book.agg(LimitScope::kDesk, Uuid{})->unrealised, Wide{10} * 5 * kRupee);
  ASSERT_TRUE(b.book.set_mark(kEquity, rupees(95)));
  EXPECT_EQ(b.book.agg(LimitScope::kDesk, Uuid{})->unrealised, Wide{-10} * 5 * kRupee);
  // Not added twice: the same mark again changes nothing.
  ASSERT_TRUE(b.book.set_mark(kEquity, rupees(95)));
  EXPECT_EQ(b.book.agg(LimitScope::kDesk, Uuid{})->unrealised, Wide{-10} * 5 * kRupee);
}

TEST(ScopeBook, AMarkThatArrivedBeforeThePositionIsUsedByIt) {
  Book b;
  ASSERT_TRUE(b.book.set_mark(kEquity, rupees(105)));
  ASSERT_TRUE(b.fill(Side::kBuy, 10, rupees(100)));
  EXPECT_EQ(b.book.agg(LimitScope::kAccount, kAccount)->unrealised, Wide{10} * 5 * kRupee);
}

TEST(ScopeBook, PnlIsRealisedPlusUnrealisedMinusCosts) {
  Book b;
  ASSERT_TRUE(b.book.set_mark(kEquity, rupees(100)));
  ASSERT_TRUE(b.fill(Side::kBuy, 10, rupees(100), kEquity, Book::all(), 50 * kRupee));
  ASSERT_TRUE(b.fill(Side::kSell, 10, rupees(103), kEquity, Book::all(), 40 * kRupee));
  EXPECT_EQ(b.book.agg(LimitScope::kAccount, kAccount)->pnl(), Wide{10} * 3 * kRupee - Wide{90} * kRupee);
}

TEST(ScopeBook, ThePeakIsTheBestDayPnlAndABeginSessionStartsTheDayAtNothing) {
  Book b;
  ASSERT_TRUE(b.fill(Side::kBuy, 10, rupees(100)));
  ASSERT_TRUE(b.book.set_mark(kEquity, rupees(110)));
  ASSERT_TRUE(b.book.set_mark(kEquity, rupees(104)));
  const ScopeAgg* agg = b.book.agg(LimitScope::kDesk, Uuid{});
  EXPECT_EQ(agg->peak_pnl, Wide{10} * 10 * kRupee);
  EXPECT_EQ(agg->pnl(), Wide{10} * 4 * kRupee);
  b.book.begin_session();
  EXPECT_EQ(agg->peak_pnl, 0);
  EXPECT_EQ(agg->day_pnl(), 0);
  EXPECT_EQ(agg->pnl(), Wide{10} * 4 * kRupee);  // what was carried over is still there, but isn't today's
  ASSERT_TRUE(b.book.set_mark(kEquity, rupees(102)));
  EXPECT_EQ(agg->day_pnl(), Wide{-10} * 2 * kRupee);
  EXPECT_EQ(agg->peak_pnl, 0);
}

TEST(ScopeBook, RepeatingAMarkChangesNeitherGrossNorTheUnderlyingNotional) {
  Book b;
  ASSERT_TRUE(b.fill(Side::kBuy, 10, rupees(100)));
  ASSERT_TRUE(b.book.set_mark(kEquity, rupees(105)));
  const Wide gross = b.book.agg(LimitScope::kDesk, Uuid{})->gross;
  const Wide underlying = b.book.underlying_notional(LimitScope::kDesk, Uuid{}, kEquity);
  EXPECT_EQ(gross, Wide{10} * 105 * kRupee);
  ASSERT_TRUE(b.book.set_mark(kEquity, rupees(105)));
  ASSERT_TRUE(b.book.set_mark(kEquity, rupees(105)));
  EXPECT_EQ(b.book.agg(LimitScope::kDesk, Uuid{})->gross, gross);
  EXPECT_EQ(b.book.underlying_notional(LimitScope::kDesk, Uuid{}, kEquity), underlying);
}

TEST(ScopeBook, AFlatPositionNoLongerNeedsAMark) {
  Book b;
  ASSERT_TRUE(b.fill(Side::kBuy, 10, rupees(100)));
  EXPECT_EQ(b.book.positions_without_mark(), 4);
  ASSERT_TRUE(b.fill(Side::kSell, 10, rupees(100)));
  EXPECT_EQ(b.book.positions_without_mark(), 0);
}

// D-28: a held option's Greeks are in the totals, so they have to be fresh too.
TEST(ScopeBook, HeldGreeksAreFreshOnlyIfEveryOptionHeldHasRecentOnes) {
  Book b;
  EXPECT_TRUE(b.book.held_greeks_fresh(1'000, 100, 0));  // none held
  ASSERT_TRUE(b.fill(Side::kSell, 65, rupees(10), kOption));
  EXPECT_FALSE(b.book.held_greeks_fresh(1'000, 100, 0));  // none have arrived
  ASSERT_TRUE(b.book.set_greeks(kOption, {.delta = 50 * kRupee, .vega = kRupee, .as_of = 1'000}));
  EXPECT_TRUE(b.book.held_greeks_fresh(1'000, 100, 0));
  EXPECT_TRUE(b.book.held_greeks_fresh(1'100, 100, 0));       // at the age
  EXPECT_FALSE(b.book.held_greeks_fresh(1'101, 100, 0));      // past it
  EXPECT_FALSE(b.book.held_greeks_fresh(999, 100, 0));        // from the future
  EXPECT_FALSE(b.book.held_greeks_fresh(1'000, 100, 1'000));  // at or before a pause
  EXPECT_TRUE(b.book.held_greeks_fresh(1'000, 100, 999));
}

TEST(ScopeBook, OnlyAnOptionStillHeldHasItsGreeksChecked) {
  Book b;
  ASSERT_TRUE(b.fill(Side::kSell, 65, rupees(10), kOption));
  ASSERT_TRUE(b.fill(Side::kSell, 65, rupees(10), kOption2));
  ASSERT_TRUE(b.book.set_greeks(kOption, {.delta = kRupee, .vega = kRupee, .as_of = 100}));
  ASSERT_TRUE(b.book.set_greeks(kOption2, {.delta = kRupee, .vega = kRupee, .as_of = 1'000}));
  EXPECT_FALSE(b.book.held_greeks_fresh(1'000, 100, 0));     // the first is old
  ASSERT_TRUE(b.fill(Side::kBuy, 65, rupees(10), kOption));  // flat: no longer held
  EXPECT_TRUE(b.book.held_greeks_fresh(1'000, 100, 0));
  ASSERT_TRUE(b.fill(Side::kBuy, 65, rupees(10), kOption2));
  ASSERT_TRUE(b.fill(Side::kSell, 65, rupees(10), kOption));  // held again, still old
  EXPECT_FALSE(b.book.held_greeks_fresh(1'000, 100, 0));
}

// Offsetting positions leave the desk flat, but each side still has the option's Greeks in its totals.
TEST(ScopeBook, AnOptionIsHeldWhileAnyScopeHoldsItEvenIfTheDeskIsFlat) {
  Book b;
  ASSERT_TRUE(b.fill(Side::kBuy, 65, rupees(10), kOption, {.account = kAccount}));
  ASSERT_TRUE(b.fill(Side::kSell, 65, rupees(10), kOption, {.account = kOtherAccount}));
  EXPECT_EQ(b.at(LimitScope::kDesk, Uuid{}, kOption)->net_qty, 0);
  EXPECT_FALSE(b.book.held_greeks_fresh(1'000, 100, 0));  // no Greeks yet: it is held at two scopes
  ASSERT_TRUE(b.book.set_greeks(kOption, {.delta = kRupee, .vega = kRupee, .as_of = 100}));
  EXPECT_FALSE(b.book.held_greeks_fresh(1'000, 100, 0));                             // and they are old
  ASSERT_TRUE(b.fill(Side::kSell, 65, rupees(10), kOption, {.account = kAccount}));  // this side is flat
  EXPECT_FALSE(b.book.held_greeks_fresh(1'000, 100, 0));  // the other still holds it
  ASSERT_TRUE(b.fill(Side::kBuy, 65, rupees(10), kOption, {.account = kOtherAccount}));
  EXPECT_TRUE(b.book.held_greeks_fresh(1'000, 100, 0));  // nobody does
}

TEST(ScopeBook, RemovingAHeldOptionThatIsNotTheLastKeepsTheOthersChecked) {
  Book b;
  for (const std::int64_t id : {kOption, kOption2, kOption3}) {
    ASSERT_TRUE(b.fill(Side::kSell, 65, rupees(10), id));
    ASSERT_TRUE(b.book.set_greeks(id, {.delta = kRupee, .vega = kRupee, .as_of = 1'000}));
  }
  EXPECT_TRUE(b.book.held_greeks_fresh(1'000, 100, 0));
  ASSERT_TRUE(
      b.book.set_greeks(kOption3, {.delta = kRupee, .vega = kRupee, .as_of = 10}));  // the last goes old
  EXPECT_FALSE(b.book.held_greeks_fresh(1'000, 100, 0));
  ASSERT_TRUE(b.fill(Side::kBuy, 65, rupees(10), kOption));  // the first, not the last, goes flat
  EXPECT_FALSE(b.book.held_greeks_fresh(1'000, 100, 0));     // the old one moved into its place
  ASSERT_TRUE(b.fill(Side::kBuy, 65, rupees(10), kOption3));
  EXPECT_TRUE(b.book.held_greeks_fresh(1'000, 100, 0));
  ASSERT_TRUE(b.fill(Side::kSell, 65, rupees(10), kOption3));  // held again, and old
  EXPECT_FALSE(b.book.held_greeks_fresh(1'000, 100, 0));
  ASSERT_TRUE(b.fill(Side::kBuy, 65, rupees(10), kOption2));  // the last one that was added goes flat
  ASSERT_TRUE(b.fill(Side::kSell, 65, rupees(10), kOption2));
  ASSERT_TRUE(b.book.set_greeks(kOption3, {.delta = kRupee, .vega = kRupee, .as_of = 1'000}));
  EXPECT_TRUE(b.book.held_greeks_fresh(1'000, 100, 0));
}

TEST(ScopeBook, AnOptionClosedAndHeldAgainIsCheckedAgain) {
  Book b;
  ASSERT_TRUE(b.fill(Side::kSell, 65, rupees(10), kOption));
  ASSERT_TRUE(b.book.set_greeks(kOption, {.delta = kRupee, .vega = kRupee, .as_of = 10}));
  EXPECT_FALSE(b.book.held_greeks_fresh(1'000, 100, 0));
  ASSERT_TRUE(b.fill(Side::kBuy, 65, rupees(10), kOption));  // closed: the only entry, and the last, goes
  EXPECT_TRUE(b.book.held_greeks_fresh(1'000, 100, 0));
  ASSERT_TRUE(b.fill(Side::kSell, 65, rupees(10), kOption));  // held again, with the same old Greeks
  EXPECT_FALSE(b.book.held_greeks_fresh(1'000, 100, 0));
}

TEST(ScopeBook, TheDataOnlyShareOfTheInstrumentTableIsExactlyHalf) {
  Book b;
  std::int64_t accepted = 0;
  for (std::int64_t id = 100'000; id < 110'000; ++id) {
    accepted += b.book.set_mark(id, rupees(100)) ? 1 : 0;
  }
  EXPECT_EQ(accepted, 4'096);  // half of 8,192
}

TEST(ScopeBook, AMarkForAnInstrumentNobodyHoldsIsDroppedOnceHalfTheTableIsUsedAndIsNotAnError) {
  Book b;
  std::int64_t refused = 0;
  for (std::int64_t id = 10'000; id < 15'000; ++id) {
    refused += b.book.set_mark(id, rupees(100)) ? 0 : 1;
  }
  EXPECT_GT(refused, 0);
  EXPECT_TRUE(b.book.healthy());
  EXPECT_TRUE(b.fill(Side::kBuy, 1, rupees(100)));  // the other half is kept for what is held
  EXPECT_TRUE(b.book.set_mark(kEquity, rupees(101)));
}

TEST(ScopeBook, OverflowOrClippingMakesTheBookUnhealthy) {
  constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
  Book a;
  EXPECT_TRUE(a.book.add_open_orders(Book::all(), kMax));
  EXPECT_FALSE(a.book.add_open_orders(Book::all(), 1));
  EXPECT_FALSE(a.book.healthy());
  Book w;
  EXPECT_TRUE(w.book.add_working(Book::all(), kEquity, Side::kBuy, kMax));
  EXPECT_FALSE(w.book.add_working(Book::all(), kEquity, Side::kBuy, 1));
  EXPECT_FALSE(w.book.healthy());
  Book f;
  EXPECT_TRUE(f.fill(Side::kBuy, kMax, rupees(1)));
  EXPECT_TRUE(f.book.healthy());
  EXPECT_TRUE(f.fill(Side::kBuy, kMax, rupees(1)));  // the position can't be counted any more
  EXPECT_FALSE(f.book.healthy());
}

TEST(ScopeBook, PnlNeedsEveryOpenPositionToHaveAMark) {
  Book b;
  b.book.set_pnl_loaded(true);
  ASSERT_TRUE(b.fill(Side::kBuy, 10, rupees(100)));
  EXPECT_EQ(b.book.positions_without_mark(), 4);  // desk, account, trader, strategy
  EXPECT_FALSE(b.book.pnl_loaded());
  ASSERT_TRUE(b.book.set_mark(kEquity, rupees(100)));
  EXPECT_EQ(b.book.positions_without_mark(), 0);
  EXPECT_TRUE(b.book.pnl_loaded());
  ASSERT_TRUE(b.fill(Side::kSell, 10, rupees(100)));  // flat again: nothing to mark
  EXPECT_TRUE(b.book.pnl_loaded());
}

TEST(ScopeBook, WorkingOrdersAndTheOpenOrderCountAreKeptPerScope) {
  Book b;
  ASSERT_TRUE(b.book.add_working(Book::all(), kEquity, Side::kBuy, 7));
  ASSERT_TRUE(b.book.add_open_orders(Book::all(), 1));
  EXPECT_EQ(b.at(LimitScope::kTrader, kTrader)->working_buy, 7);
  EXPECT_EQ(b.at(LimitScope::kTrader, kTrader)->working_sell, 0);
  EXPECT_EQ(b.book.agg(LimitScope::kStrategy, kStrategy)->open_orders, 1);
  ASSERT_TRUE(b.book.add_working(Book::all(), kEquity, Side::kBuy, -7));
  ASSERT_TRUE(b.book.add_open_orders(Book::all(), -1));
  EXPECT_EQ(b.at(LimitScope::kDesk, Uuid{})->working_buy, 0);
  EXPECT_TRUE(b.book.healthy());
}

TEST(ScopeBook, TakingOffMoreThanWasPutOnMakesTheBookUnhealthy) {
  Book b;
  b.book.set_positions_loaded(true);
  EXPECT_TRUE(b.book.positions_loaded());
  EXPECT_FALSE(b.book.add_working(Book::all(), kEquity, Side::kSell, -1));
  EXPECT_FALSE(b.book.healthy());
  EXPECT_FALSE(b.book.positions_loaded());
  Book c;
  EXPECT_FALSE(c.book.add_open_orders(Book::all(), -1));
  EXPECT_FALSE(c.book.healthy());
}

TEST(ScopeBook, ExposureAndTheUnderlyingNotionalAddUpAcrossInstruments) {
  Book b;
  ASSERT_TRUE(b.book.set_mark(kEquity, rupees(100)));
  ASSERT_TRUE(b.book.set_mark(kFuture, rupees(101)));
  ASSERT_TRUE(b.fill(Side::kBuy, 10, rupees(100), kEquity));
  ASSERT_TRUE(b.fill(Side::kSell, 5, rupees(101), kFuture));
  EXPECT_EQ(b.book.agg(LimitScope::kAccount, kAccount)->gross, Wide{1000 + 505} * kRupee);
  EXPECT_EQ(b.book.agg(LimitScope::kAccount, kAccount)->delta, Wide{1000 - 505} * kRupee);
  EXPECT_EQ(b.book.underlying_notional(LimitScope::kAccount, kAccount, kEquity), Wide{1000 + 505} * kRupee);
  EXPECT_EQ(b.book.underlying_notional(LimitScope::kAccount, kAccount, 99), 0);
}

TEST(ScopeBook, AnOptionCountsForItsDeltaNotionalAndVegaOnceItHasGreeks) {
  Book b;
  ASSERT_TRUE(b.book.set_mark(kOption, rupees(10)));
  ASSERT_TRUE(b.fill(Side::kSell, 65, rupees(10), kOption));
  EXPECT_EQ(b.book.options_without_greeks(), 4);
  EXPECT_EQ(b.book.agg(LimitScope::kAccount, kAccount)->delta, 0);
  ASSERT_TRUE(b.book.set_greeks(kOption, {.delta = 50 * kRupee, .vega = 2 * kRupee, .as_of = 5}));
  EXPECT_EQ(b.book.options_without_greeks(), 0);
  EXPECT_EQ(b.book.agg(LimitScope::kAccount, kAccount)->delta, Wide{-65} * 50 * kRupee);
  EXPECT_EQ(b.book.agg(LimitScope::kAccount, kAccount)->vega, Wide{-65} * 2 * kRupee);
  EXPECT_EQ(b.book.agg(LimitScope::kAccount, kAccount)->gross, Wide{65} * 50 * kRupee);  // delta-notional
  ASSERT_NE(b.book.greeks(kOption), nullptr);
  EXPECT_EQ(b.book.greeks(kOption)->as_of, 5);
  EXPECT_EQ(b.book.greeks(kEquity), nullptr);
  // New Greeks replace the old contribution, not add to it.
  ASSERT_TRUE(b.book.set_greeks(kOption, {.delta = 40 * kRupee, .vega = 2 * kRupee, .as_of = 6}));
  EXPECT_EQ(b.book.agg(LimitScope::kAccount, kAccount)->delta, Wide{-65} * 40 * kRupee);
}

TEST(ScopeBook, AFullTableMakesTheBookUnhealthyAndRefusesFurtherFills) {
  Book b(4);  // four positions: one fill at four scopes fills it
  b.book.set_positions_loaded(true);
  ASSERT_TRUE(b.fill(Side::kBuy, 1, rupees(100)));
  EXPECT_FALSE(b.fill(Side::kBuy, 1, rupees(100), kFuture));
  EXPECT_FALSE(b.book.healthy());
  EXPECT_FALSE(b.book.positions_loaded());
  EXPECT_FALSE(b.book.pnl_loaded());
}

TEST(ScopeBook, UnusableFillsAreRefusedAndLeaveNothing) {
  Book b;
  EXPECT_FALSE(b.fill(Side::kBuy, 0, rupees(100)));
  EXPECT_FALSE(b.fill(Side::kBuy, 5, Price{}));
  EXPECT_FALSE(b.fill(Side::kBuy, 5, rupees(100), 99));  // not in the instrument master
  EXPECT_EQ(b.book.agg(LimitScope::kDesk, Uuid{}), nullptr);
  EXPECT_TRUE(b.book.healthy());
}

TEST(ScopeBook, ManyScopesAndInstrumentsDontCollide) {
  Book b(4'096);
  for (std::uint8_t a = 1; a < 100; ++a) {
    ASSERT_TRUE(b.fill(Side::kBuy, a, rupees(100), kEquity, {.account = id_of(a)}));
  }
  for (std::uint8_t a = 1; a < 100; ++a) {
    ASSERT_NE(b.at(LimitScope::kAccount, id_of(a)), nullptr);
    EXPECT_EQ(b.at(LimitScope::kAccount, id_of(a))->net_qty, a);
  }
  EXPECT_EQ(b.at(LimitScope::kDesk, Uuid{})->net_qty, 99 * 100 / 2);
}

}  // namespace
}  // namespace vyuha::risk
