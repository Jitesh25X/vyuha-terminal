// The gate framework's own cost: the full 22-rule chain with rules that only pass, so the number is the
// table walk, the stamp and building the ApprovedOrder. M2.3 adds an entry per rule (target: p99 < 20 µs).
// Run from the release preset: ./build/release/bench/vyuha_bench --benchmark_filter=RiskGate

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#include "vyuha/core/clock.h"
#include "vyuha/md/session.h"
#include "vyuha/risk/risk_gate.h"
#include "vyuha/risk/rules.h"

namespace {

using vyuha::risk::GateContext;
using vyuha::risk::GateFacts;
using vyuha::risk::Outcome;
using vyuha::risk::Rule;
using vyuha::risk::RuleCode;

Outcome pass(const GateContext& /*ctx*/, const vyuha::oms::OrderDetails& order,
             const GateFacts& /*facts*/) noexcept {
  return {.current = order.qty};
}

template <std::size_t... I>
constexpr std::array<Rule, vyuha::risk::kRuleCount> all_pass(std::index_sequence<I...> /*seq*/) {
  return {Rule{.code = static_cast<RuleCode>(I + 1), .eval = &pass}...};
}
constexpr auto kChain = all_pass(std::make_index_sequence<vyuha::risk::kRuleCount>{});

void BM_RiskGateFullChainPasses(benchmark::State& state) {
  const vyuha::WallClock clock;
  const auto gate = vyuha::risk::RiskGate::create(kChain, clock).value();
  GateContext ctx{};
  vyuha::oms::OrderDetails order{.is_paper = vyuha::oms::kPaperOrder};
  order.qty = 50;
  order.price = vyuha::Price::from_units(1'000'000);
  for (auto _ : state) {
    auto decision = gate.evaluate(ctx, order);
    benchmark::DoNotOptimize(decision);
  }
}
BENCHMARK(BM_RiskGateFullChainPasses);

// The NFR is a percentile (PRD §10: gate p99 < 20 µs), so this times every evaluation and reports p50, p99
// and p99.9 as counters. Each sample includes one steady_clock read pair (tens of ns), so it overstates a
// little. Local numbers are for comparison within one machine; the official ones come from the staging VM.
void BM_RiskGateFullChainPercentiles(benchmark::State& state) {
  const vyuha::WallClock clock;
  const auto gate = vyuha::risk::RiskGate::create(kChain, clock).value();
  GateContext ctx{};
  vyuha::oms::OrderDetails order{.is_paper = vyuha::oms::kPaperOrder};
  order.qty = 50;
  order.price = vyuha::Price::from_units(1'000'000);
  std::vector<std::int64_t> samples;
  samples.reserve(static_cast<std::size_t>(state.max_iterations));
  for (auto _ : state) {
    const auto start = std::chrono::steady_clock::now();
    auto decision = gate.evaluate(ctx, order);
    benchmark::DoNotOptimize(decision);
    const auto end = std::chrono::steady_clock::now();
    samples.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
  }
  std::ranges::sort(samples);
  const auto at = [&](double q) {
    const auto i = static_cast<std::size_t>(q * static_cast<double>(samples.size() - 1));
    return static_cast<double>(samples.at(i));
  };
  state.counters["p50_ns"] = at(0.50);
  state.counters["p99_ns"] = at(0.99);
  state.counters["p999_ns"] = at(0.999);
  state.counters["max_ns"] = static_cast<double>(samples.back());
}
BENCHMARK(BM_RiskGateFullChainPercentiles)->Iterations(1'000'000);

// The real rules from here on. Every lookup hits, so nothing is cut short before the rule being timed.
struct BenchDesk {
  static constexpr vyuha::Nanos kNow = (vyuha::Nanos{20'766} * vyuha::Nanos{1'440} * vyuha::kNanosPerMinute) -
                                       vyuha::md::kIstOffset + (vyuha::Nanos{600} * vyuha::kNanosPerMinute);
  static constexpr std::int64_t kScale = vyuha::risk::kLimitScale;

  vyuha::SimClock clock{kNow};
  vyuha::risk::InstrumentTable instruments{70'000};
  vyuha::md::LastValueBook book{70'000};
  std::unique_ptr<GateContext> ctx{std::make_unique<GateContext>()};
  vyuha::oms::Uuid account;
  vyuha::oms::Uuid user;

  BenchDesk() {
    using vyuha::risk::LimitKey;
    account.bytes.fill(1);
    user.bytes.fill(2);
    ctx->instruments = &instruments;
    ctx->book = &book;
    ctx->calendar.set_covered_until(kNow + (vyuha::Nanos{1'440} * vyuha::kNanosPerMinute));
    for (std::int64_t id = 1; id <= 65'000; ++id) {
      static_cast<void>(instruments.put(id, {.lot_size = 1,
                                             .tick_size = vyuha::Price::from_units(500),
                                             .exchange = vyuha::risk::Exchange::kNse}));
      vyuha::md::Tick tick;
      tick.instrument_id = static_cast<vyuha::md::InstrumentId>(id);
      tick.ltp = vyuha::Price::from_units(1'000'000);
      tick.recv_ts = kNow;
      tick.recv_mono = kNow;
      static_cast<void>(book.update(tick));
    }
    static_cast<void>(ctx->accounts.put({.id = account,
                                         .session_connected = true,
                                         .token_expires_at = kNow + vyuha::kNanosPerSecond,
                                         .ops_cap = 10}));
    static_cast<void>(
        ctx->entitlements.put({.user = user, .account = account, .can_trade = true, .can_live = true}));
    const auto limit = [this](LimitKey key, std::int64_t value) {
      static_cast<void>(ctx->limits.add(key, {.value = value}));
    };
    limit(LimitKey::kMaxOrderValue, 500'000 * kScale);
    limit(LimitKey::kMaxOrderQtyLots, 100'000 * kScale);
    limit(LimitKey::kFatFingerPct, 5 * kScale);
    limit(LimitKey::kFatFingerPctOpt, 30 * kScale);
    limit(LimitKey::kDupWindowMs, 1'500 * kScale);
    limit(LimitKey::kAllowMarketOrders, 1 * kScale);
    limit(LimitKey::kAllowMisAfter, 1510 * kScale);
    limit(LimitKey::kTradingWindowStart, 915 * kScale);
    limit(LimitKey::kTradingWindowEnd, 1530 * kScale);
  }

  [[nodiscard]] vyuha::oms::OrderDetails order() const {
    vyuha::oms::OrderDetails o{.is_paper = vyuha::oms::kPaperOrder};
    o.account_id = account;
    o.user_id = user;
    o.instrument_id = 42;
    o.qty = 10;
    o.price = vyuha::Price::from_units(1'000'000);
    return o;
  }

  // Leaves room for one more, with every entry live: the longest scan R-DUP can make.
  void fill_duplicate_table() {
    for (std::size_t i = 0; i + 1 < vyuha::risk::DupTable::kCapacity; ++i) {
      ctx->dup.entries.at(i) = vyuha::risk::DupEntry{.account = account,
                                                     .instrument_id = 42,
                                                     .qty = 1'000'000 + static_cast<std::int32_t>(i),
                                                     .ts = kNow,
                                                     .keep = 3'600 * vyuha::kNanosPerSecond};
    }
    ctx->dup.count = vyuha::risk::DupTable::kCapacity - 1;
  }
};

void record_percentiles(benchmark::State& state, std::vector<std::int64_t>& samples) {
  std::ranges::sort(samples);
  const auto at = [&](double q) {
    return static_cast<double>(
        samples.at(static_cast<std::size_t>(q * static_cast<double>(samples.size() - 1))));
  };
  state.counters["p50_ns"] = at(0.50);
  state.counters["p99_ns"] = at(0.99);
  state.counters["p999_ns"] = at(0.999);
  state.counters["max_ns"] = static_cast<double>(samples.back());
}

// One real rule, on its own: the entry each rule ships with. An order that keeps passing would fill the
// stateful rules' tables, so each iteration starts them empty.
void BM_RiskRule(benchmark::State& state, RuleCode code) {
  BenchDesk desk;
  const vyuha::risk::Rule* found = nullptr;
  for (const Rule& r : vyuha::risk::pre_trade_rules()) {
    if (r.code == code) {
      found = &r;
    }
  }
  const std::array<Rule, 1> one{*found};
  const auto gate = vyuha::risk::RiskGate::create(one, desk.clock).value();
  const vyuha::oms::OrderDetails order = desk.order();
  for (auto _ : state) {
    desk.ctx->dup.count = 0;
    desk.ctx->accounts.find(desk.account)->ops = {};  // the budget is not what is being timed
    auto decision = gate.evaluate(*desk.ctx, order);
    benchmark::DoNotOptimize(decision);
  }
}
BENCHMARK_CAPTURE(BM_RiskRule, kill, RuleCode::kKill);
BENCHMARK_CAPTURE(BM_RiskRule, live_disabled, RuleCode::kLiveDisabled);
BENCHMARK_CAPTURE(BM_RiskRule, entitle, RuleCode::kEntitle);
BENCHMARK_CAPTURE(BM_RiskRule, session, RuleCode::kSession);
BENCHMARK_CAPTURE(BM_RiskRule, mkt_hours, RuleCode::kMktHours);
BENCHMARK_CAPTURE(BM_RiskRule, instr_ban, RuleCode::kInstrBan);
BENCHMARK_CAPTURE(BM_RiskRule, instr_restricted, RuleCode::kInstrRestricted);
BENCHMARK_CAPTURE(BM_RiskRule, instr_expired, RuleCode::kInstrExpired);
BENCHMARK_CAPTURE(BM_RiskRule, order_shape, RuleCode::kOrderShape);
BENCHMARK_CAPTURE(BM_RiskRule, fat_finger, RuleCode::kFatFinger);
BENCHMARK_CAPTURE(BM_RiskRule, order_value, RuleCode::kOrderValue);
BENCHMARK_CAPTURE(BM_RiskRule, dup, RuleCode::kDup);
BENCHMARK_CAPTURE(BM_RiskRule, ops, RuleCode::kOps);

// A rule on a stop with a known circuit band, which takes the branches a plain LIMIT order skips (M2.3b).
void BM_RiskRuleStop(benchmark::State& state, RuleCode code, vyuha::oms::OrderType type) {
  BenchDesk desk;
  vyuha::risk::InstrumentRef banded = *desk.instruments.find(42);
  banded.lower_circuit = vyuha::Price::from_units(900'000);
  banded.upper_circuit = vyuha::Price::from_units(1'100'000);
  static_cast<void>(desk.instruments.put(42, banded));
  const Rule* found = nullptr;
  for (const Rule& r : vyuha::risk::pre_trade_rules()) {
    if (r.code == code) {
      found = &r;
    }
  }
  const std::array<Rule, 1> one{*found};
  const auto gate = vyuha::risk::RiskGate::create(one, desk.clock).value();
  vyuha::oms::OrderDetails order = desk.order();
  order.order_type = type;
  order.price =
      type == vyuha::oms::OrderType::kSl ? std::optional{vyuha::Price::from_units(1'020'000)} : std::nullopt;
  order.trigger_price = vyuha::Price::from_units(1'010'000);
  for (auto _ : state) {
    auto decision = gate.evaluate(*desk.ctx, order);
    benchmark::DoNotOptimize(decision);
  }
}
BENCHMARK_CAPTURE(BM_RiskRuleStop, fat_finger_stop_market, RuleCode::kFatFinger, vyuha::oms::OrderType::kSlM);
BENCHMARK_CAPTURE(BM_RiskRuleStop, order_shape_stop_limit, RuleCode::kOrderShape, vyuha::oms::OrderType::kSl);

// The 13 real rules, approving every order, followed by 9 rules that only pass in M2.4's place.
void BM_RiskGateRealRulesPercentiles(benchmark::State& state, bool full_duplicate_table) {
  BenchDesk desk;
  std::vector<Rule> rules(vyuha::risk::pre_trade_rules().begin(), vyuha::risk::pre_trade_rules().end());
  for (std::size_t i = rules.size(); i < vyuha::risk::kRuleCount; ++i) {
    rules.push_back(kChain.at(i));
  }
  const auto gate = vyuha::risk::RiskGate::create(rules, desk.clock).value();
  const vyuha::oms::OrderDetails order = desk.order();
  std::vector<std::int64_t> samples;
  samples.reserve(static_cast<std::size_t>(state.max_iterations));
  for (auto _ : state) {
    if (full_duplicate_table) {
      desk.fill_duplicate_table();
    } else {
      desk.ctx->dup.count = 0;
    }
    desk.ctx->accounts.find(desk.account)->ops = {};  // the budget is not what is being timed
    const auto start = std::chrono::steady_clock::now();
    auto decision = gate.evaluate(*desk.ctx, order);
    benchmark::DoNotOptimize(decision);
    const auto end = std::chrono::steady_clock::now();
    samples.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    if (decision.rejection.has_value()) {
      state.SkipWithError("the bench order was rejected, so this isn't timing the whole chain");
      break;
    }
  }
  if (!samples.empty() && !state.skipped()) {
    record_percentiles(state, samples);
  }
}
BENCHMARK_CAPTURE(BM_RiskGateRealRulesPercentiles, empty_duplicate_table, false)->Iterations(200'000);
// The worst the chain can do: R-DUP scans a nearly full table, then evicts from it and adds to it.
BENCHMARK_CAPTURE(BM_RiskGateRealRulesPercentiles, full_duplicate_table, true)->Iterations(20'000);

}  // namespace
