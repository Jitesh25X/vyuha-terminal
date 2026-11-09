#pragma once

// The desk the rule tests share: a book where the order from order() passes every rule, so a test breaks
// one thing at a time. Shared by risk_rules_test.cc and scope_rules_test.cc.

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
    for (const Rule& r : scope_rules()) {
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

}  // namespace
}  // namespace vyuha::risk
// NOLINTEND(bugprone-unchecked-optional-access)
