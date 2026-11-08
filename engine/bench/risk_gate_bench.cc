// The gate framework's own cost: the full 22-rule chain with rules that only pass, so the number is the
// table walk, the stamp and building the ApprovedOrder. M2.3 adds an entry per rule (target: p99 < 20 µs).

#include <array>
#include <cstddef>
#include <utility>

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

}  // namespace
