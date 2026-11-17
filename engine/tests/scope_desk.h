#pragma once

// The fixture desk plus a scope book, margin and discipline data, every limit loose and everything loaded and
// fresh: the order from order() passes all 22 rules, so a test breaks one thing at a time (M2.4).

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "desk_fixture.h"
#include "vyuha/risk/discipline_table.h"
#include "vyuha/risk/margin_cache.h"
#include "vyuha/risk/scope_book.h"

// NOLINTBEGIN(bugprone-unchecked-optional-access): every test does ASSERT_TRUE(has_value()) first
namespace vyuha::risk {
namespace {

constexpr std::int64_t kFuture = 4;
constexpr std::int64_t kLot = 65;  // the options' and the hedge futures' lot size
constexpr std::int64_t kPutOption = 6;
constexpr std::int64_t kCallFar = 7;
constexpr std::int64_t kHedgeFut = 8;
constexpr std::int64_t kHedgeFutNear = 9;
constexpr std::int64_t kOption2 = 5;
// Same underlying, expiry and kind as kOption, but a lot of 75 where kOption's is 65; and a future of 25.
constexpr std::int64_t kCall75 = 10;
constexpr std::int64_t kHedgeFut25 = 11;
constexpr std::int64_t kFarPut = 12;  // a put expiring with kCallFar
constexpr std::int64_t kRupee = Price::kUnitsPerRupee;

inline constexpr std::array<std::int64_t, 10> kDerivatives{kFuture,     kOption,   kOption2,      kPutOption,
                                                           kCallFar,    kHedgeFut, kHedgeFutNear, kCall75,
                                                           kHedgeFut25, kFarPut};

// The fixture desk with a scope book that holds positions and P&L, every limit loose, and an option and a
// future on the equity.
struct ScopeDesk : Desk {
  std::unique_ptr<ScopeBook> owned_book;
  ScopeBook& sb;
  std::vector<Rule> chain;  // all 22 rules
  std::vector<std::pair<std::int64_t, Price>> marks;
  std::unique_ptr<MarginCache> margin_cache;
  std::unique_ptr<DisciplineTable> discipline_table;

  ScopeDesk() : owned_book(std::make_unique<ScopeBook>(instruments, 512)), sb(*owned_book) {
    ctx.scopes = &sb;
    sb.set_positions_loaded(true);
    sb.set_pnl_loaded(true);
    InstrumentRef option = *instruments.find(kOption);
    option.underlying_id = kEquity;
    option.option_kind = OptionKind::kCall;
    EXPECT_TRUE(instruments.put(kOption, option));
    // A put and a longer-dated call on the same underlying, and two futures of a full lot, one expiring
    // before the option and one well after, for the cover checks (D-31).
    InstrumentRef put = option;
    put.option_kind = OptionKind::kPut;
    EXPECT_TRUE(instruments.put(kPutOption, put));
    InstrumentRef far_call = option;
    far_call.expires_at = option.expires_at + (Nanos{7} * Nanos{1'440} * kNanosPerMinute);
    EXPECT_TRUE(instruments.put(kCallFar, far_call));
    InstrumentRef hedge{.lot_size = 65,
                        .tick_size = Price::from_units(500),
                        .exchange = Exchange::kNfo,
                        .segment = Segment::kFut,
                        .expires_at = option.expires_at + (Nanos{20} * Nanos{1'440} * kNanosPerMinute),
                        .underlying_id = kEquity};
    EXPECT_TRUE(instruments.put(kHedgeFut, hedge));
    hedge.expires_at = option.expires_at - (Nanos{1} * Nanos{1'440} * kNanosPerMinute);
    EXPECT_TRUE(instruments.put(kHedgeFutNear, hedge));
    // A live order needs a known circuit band (D-22), and a price to be checked against.
    EXPECT_TRUE(instruments.put(kEquity, {.lot_size = 1,
                                          .tick_size = Price::from_units(500),
                                          .lower_circuit = rupees(90),
                                          .upper_circuit = rupees(110)}));
    EXPECT_TRUE(instruments.put(kFuture, {.lot_size = 1,
                                          .tick_size = Price::from_units(500),
                                          .exchange = Exchange::kNfo,
                                          .segment = Segment::kFut,
                                          .underlying_id = kEquity}));
    EXPECT_TRUE(instruments.put(kOption2, option));
    InstrumentRef call75 = option;
    call75.lot_size = 75;
    EXPECT_TRUE(instruments.put(kCall75, call75));
    InstrumentRef fut25 = hedge;
    fut25.lot_size = 25;
    fut25.expires_at = option.expires_at + (Nanos{20} * Nanos{1'440} * kNanosPerMinute);
    EXPECT_TRUE(instruments.put(kHedgeFut25, fut25));
    InstrumentRef far_put = far_call;
    far_put.option_kind = OptionKind::kPut;
    EXPECT_TRUE(instruments.put(kFarPut, far_put));
    for (const std::int64_t id : kDerivatives) {
      InstrumentRef banded = *instruments.find(id);
      banded.lower_circuit = rupees(1);
      banded.upper_circuit = rupees(1'000);
      EXPECT_TRUE(instruments.put(id, banded));
      tick(id, rupees(100));
      mark(id, rupees(100));  // a live order needs a fresh mark for its own instrument (D-27)
    }
    tick(kFuture, rupees(100));
    mark(kEquity, rupees(100));
    limit(LimitKey::kMaxOpenOrders, 50 * kFromScale);
    limit(LimitKey::kMaxPositionLots, 1'000 * kFromScale);
    limit(LimitKey::kMaxPositionNotional, rupees(1'000'000).units());
    limit(LimitKey::kMaxGrossExposure, rupees(1'000'000).units());
    limit(LimitKey::kMaxNetDeltaInr, rupees(1'000'000).units());
    limit(LimitKey::kMaxNetVegaInr, rupees(1'000'000).units());
    limit(LimitKey::kMaxDailyLoss, rupees(50'000).units());
    limit(LimitKey::kMaxDrawdownFromPeak, rupees(75'000).units());
    limit(LimitKey::kMaxShortOptionLots, 1'000 * kFromScale);
    limit(LimitKey::kMarginBufferPct, 10 * kFromScale);
    margin_cache = std::make_unique<MarginCache>();
    discipline_table = std::make_unique<DisciplineTable>();
    ctx.margin = margin_cache.get();
    ctx.discipline = discipline_table.get();
    discipline_table->set_loaded(true);
    refresh_margin();
    chain.assign(risk::all_rules().begin(), risk::all_rules().end());
  }

  // A mark stamped now. The desk remembers it, so restamp_marks() can say it arrived again.
  void mark(std::int64_t id, Price price) {
    ASSERT_TRUE(sb.set_mark(id, price, clock.monotonic()));
    for (auto& [known, at] : marks) {
      if (known == id) {
        at = price;
        return;
      }
    }
    marks.emplace_back(id, price);
  }
  // Every mark the desk has been given, as if it had just arrived: a test that moves the clock on and means
  // the marks to be fresh calls this (a mark is stale after 3 s, owner 2026-11-15).
  void restamp_marks() {
    for (const auto& [id, price] : marks) {
      ASSERT_TRUE(sb.set_mark(id, price, clock.monotonic()));
    }
  }
  [[nodiscard]] GateDecision run_all(const OrderDetails& o) { return run(o, chain); }
  // Margin the broker reports: plenty free, and a per-lot figure for the future and both options, all fresh.
  void refresh_margin(std::int64_t available = 1'000'000'000'000'000) {
    ASSERT_TRUE(margin_cache->set_available(kAccount, available, clock.monotonic()));
    for (const std::int64_t id : kDerivatives) {
      ASSERT_TRUE(margin_cache->set_per_lot(id, rupees(100).units(), clock.monotonic()));
    }
  }
  // Fresh Greeks for every option the desk has.
  void fresh_greeks_all() {
    for (const std::int64_t id : {kOption, kOption2, kPutOption, kCallFar}) {
      ASSERT_TRUE(sb.set_greeks(id, {.delta = 50 * kRupee, .vega = 2 * kRupee, .as_of = clock.monotonic()}));
    }
  }
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

[[nodiscard, maybe_unused]] bool names(const Outcome& out, DataSource source) {
  return out.verdict == Verdict::kReject && out.reason == RuleReason::kDataMissing && out.source == source;
}

}  // namespace
}  // namespace vyuha::risk
// NOLINTEND(bugprone-unchecked-optional-access)
