// The gate framework's own cost: the full 22-rule chain with rules that only pass, so the number is the
// table walk, the stamp and building the ApprovedOrder. M2.3 adds an entry per rule (target: p99 < 20 µs).
// Run from the release preset: ./build/release/bench/vyuha_bench --benchmark_filter=RiskGate

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#include "vyuha/core/clock.h"
#include "vyuha/risk/risk_gate.h"

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
  const GateContext ctx{};
  vyuha::oms::OrderDetails order;
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
  const GateContext ctx{};
  vyuha::oms::OrderDetails order;
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

}  // namespace
