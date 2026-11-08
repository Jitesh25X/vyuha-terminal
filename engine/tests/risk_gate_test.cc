// The RiskGate framework (M2.2): the rule table, the chain order, ApprovedOrder (invariant 1).

#include "vyuha/risk/risk_gate.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>  // rapidcheck's headers use std::exception_ptr without including it
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <rapidcheck.h>

#include "vyuha/core/clock.h"
#include "vyuha/oms/order_router.h"

namespace vyuha::risk {
namespace {

// Invariant 1 by type. tests/compile_fail.cc checks the same from the outside, with a control.
static_assert(!std::is_default_constructible_v<ApprovedOrder>);
static_assert(!std::is_constructible_v<ApprovedOrder, const oms::OrderDetails&>);
static_assert(!std::is_constructible_v<ApprovedOrder, const oms::OrderDetails&, const GateStamp&>);
static_assert(!std::is_constructible_v<ApprovedOrder, const oms::Order&>);
static_assert(!std::is_copy_constructible_v<ApprovedOrder>);
static_assert(!std::is_copy_assignable_v<ApprovedOrder>);
static_assert(std::is_final_v<ApprovedOrder>);
static_assert(!std::is_aggregate_v<ApprovedOrder>);
static_assert(std::is_nothrow_move_constructible_v<ApprovedOrder>);  // the router's queue moves it
// Not trivially copyable, so std::bit_cast can't make one from bytes (risk review, M2.2).
static_assert(!std::is_trivially_copyable_v<ApprovedOrder>);
// An order sender takes the gate's approval and nothing else.
template <typename T>
concept Sendable = requires(oms::IOrderSender& s, const T& t) { s.send(t, std::string_view{}); };
static_assert(Sendable<ApprovedOrder>);
static_assert(!Sendable<oms::Order>);
static_assert(!Sendable<oms::OrderDetails>);
// And the router queues only an approval.
template <typename T>
concept Submittable = requires(oms::OrderRouter& r, T t) { r.submit(oms::OrderHandle{0}, std::move(t)); };
static_assert(Submittable<ApprovedOrder>);
static_assert(!Submittable<oms::OrderDetails>);

constexpr std::size_t kCodes = kRuleCount + 1;  // indexed by the code; 0 unused

// Rules that do what the test says, and log that they ran. Plain functions, as the gate's table needs.
struct Script {
  std::array<Outcome, kCodes> outcome{};
  std::vector<RuleCode> calls;
};
Script& script() {
  static Script s;
  return s;
}

template <std::size_t N>
// NOLINTNEXTLINE(bugprone-exception-escape): a test's allocation failure may terminate
Outcome scripted(const GateContext& /*ctx*/, const oms::OrderDetails& /*order*/,
                 const GateFacts& /*facts*/) noexcept {
  script().calls.push_back(static_cast<RuleCode>(N));
  return script().outcome.at(N);
}

template <std::size_t... I>
constexpr std::array<Rule, kRuleCount> make_full(std::index_sequence<I...> /*seq*/) {
  return {Rule{.code = static_cast<RuleCode>(I + 1), .eval = &scripted<I + 1>}...};
}
constexpr std::array<Rule, kRuleCount> kFull = make_full(std::make_index_sequence<kRuleCount>{});

Rule rule_for(RuleCode code) { return kFull.at(std::to_underlying(code) - 1U); }

void reset() {
  script().outcome.fill({});
  script().calls.clear();
}

// The rejection, or code 0 (R-UNKNOWN) when there's none, which no expectation matches.
RuleResult rejection_of(const GateDecision& d) {
  return d.rejection.value_or(RuleResult{.code = static_cast<RuleCode>(0)});
}

oms::OrderDetails paper_order() {
  oms::OrderDetails d;
  d.id.bytes.fill(1);
  EXPECT_TRUE(d.client_order_id.assign("client-1"));
  d.qty = 50;
  d.price = Price::from_units(1'000'000);
  return d;
}

TEST(RiskGate, CodesMatchTheSpecTable) {
  EXPECT_EQ(code_text(RuleCode::kKill), "R-KILL");
  EXPECT_EQ(code_text(RuleCode::kOrderShape), "R-ORDER-SHAPE");
  EXPECT_EQ(code_text(RuleCode::kOps), "R-OPS");
  EXPECT_EQ(code_text(RuleCode::kOpenOrders), "R-OPEN-ORDERS");
  EXPECT_EQ(code_text(RuleCode::kDiscipline), "R-DISCIPLINE");
  EXPECT_EQ(std::to_underlying(RuleCode::kDiscipline), kRuleCount);
  EXPECT_EQ(code_text(static_cast<RuleCode>(0)), "R-UNKNOWN");
  EXPECT_EQ(code_text(static_cast<RuleCode>(kRuleCount + 1)), "R-UNKNOWN");
}

TEST(RiskGate, ATableMustFollowTheChainOrder) {
  const SimClock clock;
  EXPECT_TRUE(RiskGate::create(kFull, clock).has_value());
  EXPECT_TRUE(RiskGate::create(kFull, clock)->complete());
  EXPECT_FALSE(RiskGate::create({}, clock)->complete());

  const std::array swapped{rule_for(RuleCode::kDup), rule_for(RuleCode::kOrderValue)};
  EXPECT_EQ(RiskGate::create(swapped, clock).error().code, ErrorCode::kInvalidArgument);
  const std::array twice{rule_for(RuleCode::kDup), rule_for(RuleCode::kDup)};
  EXPECT_FALSE(RiskGate::create(twice, clock).has_value());
  const std::array no_fn{Rule{.code = RuleCode::kKill, .eval = nullptr}};
  EXPECT_FALSE(RiskGate::create(no_fn, clock).has_value());
  const std::array outside{Rule{.code = static_cast<RuleCode>(kRuleCount + 1), .eval = &scripted<1>}};
  EXPECT_FALSE(RiskGate::create(outside, clock).has_value());
  const std::array zero{Rule{.code = static_cast<RuleCode>(0), .eval = &scripted<1>}};
  EXPECT_FALSE(RiskGate::create(zero, clock).has_value());
}

TEST(RiskGate, ApprovesExactlyWhatItEvaluatedAndStampsIt) {
  reset();
  const SimClock clock(5'000);
  const RiskGate gate = RiskGate::create(kFull, clock).value();
  const GateContext ctx{.limits = {.version = 42}};
  const oms::OrderDetails order = paper_order();
  const GateDecision d = gate.evaluate(ctx, order);
  ASSERT_TRUE(d.approved.has_value());
  EXPECT_FALSE(d.rejection.has_value());
  if (d.approved.has_value()) {
    EXPECT_TRUE(d.approved->order() == order);
    EXPECT_EQ(d.approved->stamp(), d.stamp);
  }
  EXPECT_EQ(d.stamp.ruleset_version, 42U);
  EXPECT_EQ(d.stamp.ts, 5'000);
  EXPECT_EQ(script().calls.size(), kRuleCount);  // every rule ran, in order
  for (std::size_t i = 0; i < script().calls.size(); ++i) {
    EXPECT_EQ(std::to_underlying(script().calls[i]), i + 1);
  }
}

TEST(RiskGate, TheFirstHardFailureStopsTheChain) {
  reset();
  const SimClock clock;
  const RiskGate gate = RiskGate::create(kFull, clock).value();
  script().outcome.at(std::to_underlying(RuleCode::kOrderValue)) = {
      .verdict = Verdict::kReject, .limit = 500'000, .current = 900'000};
  script().outcome.at(std::to_underlying(RuleCode::kMargin)) = {.verdict = Verdict::kReject};
  const GateDecision d = gate.evaluate({}, paper_order());
  EXPECT_FALSE(d.approved.has_value());
  ASSERT_TRUE(d.rejection.has_value());
  EXPECT_EQ(rejection_of(d).code, RuleCode::kOrderValue);
  EXPECT_EQ(rejection_of(d).limit, 500'000);
  EXPECT_EQ(rejection_of(d).current, 900'000);
  EXPECT_EQ(script().calls.back(), RuleCode::kOrderValue);  // R-DUP onwards never ran
  EXPECT_EQ(script().calls.size(), std::to_underlying(RuleCode::kOrderValue));
}

TEST(RiskGate, ASoftLimitWarnsAndPasses) {
  reset();
  const SimClock clock;
  const RiskGate gate = RiskGate::create(kFull, clock).value();
  script().outcome.at(std::to_underlying(RuleCode::kFatFinger)) = {.verdict = Verdict::kWarn, .limit = 5};
  script().outcome.at(std::to_underlying(RuleCode::kExposure)) = {.verdict = Verdict::kWarn, .limit = 7};
  const GateDecision d = gate.evaluate({}, paper_order());
  EXPECT_TRUE(d.approved.has_value());
  ASSERT_EQ(d.warning_list().size(), 2U);
  EXPECT_EQ(d.warning_list()[0].code, RuleCode::kFatFinger);
  EXPECT_EQ(d.warning_list()[0].limit, 5);
  EXPECT_EQ(d.warning_list()[1].code, RuleCode::kExposure);
}

TEST(RiskGate, AVerdictNoRuleShouldReturnIsARejection) {
  reset();
  const SimClock clock;
  const RiskGate gate = RiskGate::create(kFull, clock).value();
  script().outcome.at(std::to_underlying(RuleCode::kDup)) = {.verdict = static_cast<Verdict>(9)};
  const GateDecision d = gate.evaluate({}, paper_order());
  EXPECT_FALSE(d.approved.has_value());
  ASSERT_TRUE(d.rejection.has_value());
  EXPECT_EQ(rejection_of(d).code, RuleCode::kDup);
  EXPECT_EQ(rejection_of(d).verdict, Verdict::kReject);
}

// A gate missing rules must not approve real orders, however the rules it has vote.
TEST(RiskGate, ALiveOrderNeedsTheWholeChain) {
  reset();
  const SimClock clock;
  oms::OrderDetails live = paper_order();
  live.is_paper = false;

  const RiskGate empty = RiskGate::create({}, clock).value();
  const GateDecision refused = empty.evaluate({}, live);
  EXPECT_FALSE(refused.approved.has_value());
  ASSERT_TRUE(refused.rejection.has_value());
  EXPECT_EQ(rejection_of(refused).code, RuleCode::kLiveDisabled);

  const std::array partial{rule_for(RuleCode::kKill), rule_for(RuleCode::kOrderShape)};
  const RiskGate some = RiskGate::create(partial, clock).value();
  EXPECT_EQ(rejection_of(some.evaluate({}, live)).code, RuleCode::kLiveDisabled);
  EXPECT_TRUE(script().calls.empty());  // refused before any rule ran
  EXPECT_TRUE(some.evaluate({}, paper_order()).approved.has_value());

  const RiskGate full = RiskGate::create(kFull, clock).value();
  EXPECT_TRUE(full.evaluate({}, live).approved.has_value());
}

TEST(RiskGate, OnlyRules14To22AreSkippedForAReducingOrder) {
  for (std::size_t n = 1; n <= kRuleCount; ++n) {
    const auto code = static_cast<RuleCode>(n);
    EXPECT_TRUE(applies(code, true)) << code_text(code);
    EXPECT_EQ(applies(code, false), n < std::to_underlying(RuleCode::kOpenOrders)) << code_text(code);
  }
}

// Until positions exist (M2.6) the gate can't tell an opening order from a reducing one, so it runs every
// rule (fail closed).
TEST(RiskGate, EveryOrderCountsAsOpeningUntilPositionsExist) {
  reset();
  const SimClock clock;
  const std::array opening_only{rule_for(RuleCode::kPosition)};
  const RiskGate gate = RiskGate::create(opening_only, clock).value();
  static_cast<void>(gate.evaluate({}, paper_order()));
  EXPECT_EQ(script().calls, std::vector{RuleCode::kPosition});
}

// RISK_ENGINE §3 as an oracle, over random tables and random votes: the chain runs in order and stops at the
// first hard failure, warnings before it are kept, and the gate is pure given its inputs.
TEST(RiskGate, PropertyTheChainFollowsSection3) {
  const bool ok = rc::check([](const std::vector<bool>& present, const std::vector<std::uint8_t>& votes) {
    reset();
    std::vector<Rule> table;
    for (std::size_t i = 0; i < kRuleCount && i < present.size(); ++i) {
      if (present[i]) {
        table.push_back(kFull.at(i));
      }
    }
    for (std::size_t n = 1; n <= kRuleCount; ++n) {
      const std::uint8_t v = n - 1 < votes.size() ? votes[n - 1] % 3 : 0;
      script().outcome.at(n) = {.verdict = static_cast<Verdict>(v), .limit = static_cast<std::int64_t>(n)};
    }
    const SimClock clock;
    const RiskGate gate = RiskGate::create(table, clock).value();

    std::vector<RuleCode> expect_calls;
    std::vector<RuleCode> expect_warnings;
    std::optional<RuleCode> expect_reject;
    for (const Rule& rule : table) {
      expect_calls.push_back(rule.code);
      const Verdict v = script().outcome.at(std::to_underlying(rule.code)).verdict;
      if (v == Verdict::kReject) {
        expect_reject = rule.code;
        break;
      }
      if (v == Verdict::kWarn) {
        expect_warnings.push_back(rule.code);
      }
    }

    const GateDecision first = gate.evaluate({}, paper_order());
    RC_ASSERT(script().calls == expect_calls);
    RC_ASSERT(first.approved.has_value() == !expect_reject.has_value());
    RC_ASSERT(first.rejection.has_value() == expect_reject.has_value());
    if (expect_reject.has_value()) {
      RC_ASSERT(rejection_of(first).code == *expect_reject);
      RC_ASSERT(rejection_of(first).limit == static_cast<std::int64_t>(std::to_underlying(*expect_reject)));
    }
    RC_ASSERT(first.warning_count == expect_warnings.size());
    for (std::size_t i = 0; i < expect_warnings.size(); ++i) {
      RC_ASSERT(first.warnings.at(i).code == expect_warnings[i]);
    }

    script().calls.clear();
    const GateDecision again = gate.evaluate({}, paper_order());
    RC_ASSERT(script().calls == expect_calls);
    RC_ASSERT(again.approved.has_value() == first.approved.has_value());
    RC_ASSERT(again.warning_count == first.warning_count);
    RC_ASSERT(again.stamp == first.stamp);  // a SimClock that doesn't move: the timing is the same too
  });
  EXPECT_TRUE(ok);
}

}  // namespace
}  // namespace vyuha::risk
