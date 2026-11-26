// F-EX-4: the SimBroker's hot paths. on_tick with 1,000 working orders on other instruments and 100 on the
// ticked one (none fills, so it is the visit cost), and send of a marketable order (ack and fill, with the
// events drained). Times every call and reports p50/p99/p99.9 as counters, like the gate's benchmark. Local
// numbers are for comparison within one machine; the official ones come from the staging VM.
// Run from the release preset: ./build/release/bench/vyuha_bench --benchmark_filter=SimBroker

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#include "vyuha/core/clock.h"
#include "vyuha/md/last_value.h"
#include "vyuha/oms/order_router.h"
#include "vyuha/risk/gate_context.h"
#include "vyuha/risk/risk_gate.h"
#include "vyuha/sim/sim_broker.h"

namespace {

using vyuha::Price;
using vyuha::oms::OrderDetails;

constexpr std::int64_t kTicked = 1;

struct SimDesk {
  vyuha::SimClock clock{1'000'000'000'000};
  vyuha::risk::InstrumentTable instruments{2048};
  vyuha::md::LastValueBook book{2048};
  std::expected<vyuha::sim::SimBroker, vyuha::Error> made;
  vyuha::sim::SimBroker& sim() { return *made; }

  SimDesk() : made(make()) {}
  SimDesk(const SimDesk&) = delete;
  SimDesk& operator=(const SimDesk&) = delete;
  SimDesk(SimDesk&&) = delete;
  SimDesk& operator=(SimDesk&&) = delete;
  ~SimDesk() = default;

  [[nodiscard]] vyuha::md::Tick tick(std::int64_t id, std::int64_t ltp_paise) const {
    vyuha::md::Tick t;
    t.instrument_id = static_cast<vyuha::md::InstrumentId>(id);
    t.ltp = Price::from_paise(ltp_paise);
    t.bid = Price::from_paise(ltp_paise - 5);
    t.ask = Price::from_paise(ltp_paise + 5);
    t.recv_mono = clock.monotonic();
    return t;
  }

  static OrderDetails order(int n, std::int64_t instrument, vyuha::oms::OrderType type, std::int64_t paise) {
    OrderDetails d{.is_paper = vyuha::oms::kPaperOrder};
    d.id.bytes.fill(static_cast<std::uint8_t>(n));
    const std::string cid = "c" + std::to_string(n);
    const bool ok = d.client_order_id.assign(cid);
    (void)ok;
    d.instrument_id = instrument;
    d.qty = 10;
    d.order_type = type;
    if (type == vyuha::oms::OrderType::kLimit) {
      d.price = Price::from_paise(paise);
    }
    return d;
  }

 private:
  std::expected<vyuha::sim::SimBroker, vyuha::Error> make() {
    for (std::int64_t id = 1; id <= 1001; ++id) {
      (void)instruments.put(id, {.lot_size = 1, .tick_size = Price::from_paise(5)});
    }
    return vyuha::sim::SimBroker::create(instruments, book, clock);
  }
};

void report(benchmark::State& state, std::vector<std::int64_t>& samples) {
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

vyuha::risk::ApprovedOrder approve(const OrderDetails& d, const vyuha::IClock& clock) {
  const auto gate = vyuha::risk::RiskGate::create({}, clock).value();
  const auto ctx = std::make_unique<vyuha::risk::GateContext>();
  auto decision = gate.evaluate(*ctx, d);
  if (!decision.approved.has_value()) {
    std::abort();  // a bench setup error
  }
  return std::move(*decision.approved);
}

void BM_SimBrokerOnTick(benchmark::State& state) {
  SimDesk desk;
  // Resting BUY limits at 50.00 on 10 other instruments (1,000 orders) and on the ticked one (100): the
  // market stays at 100.00, so nothing fills and every tick visits all 100 of the ticked instrument's.
  int n = 0;
  for (int i = 0; i < 1000; ++i) {
    const auto approved =
        approve(SimDesk::order(++n, 2 + (i % 10), vyuha::oms::OrderType::kLimit, 5000), desk.clock);
    bool ok = desk.sim().send(approved, "t" + std::to_string(n));
    benchmark::DoNotOptimize(ok);
  }
  for (int i = 0; i < 100; ++i) {
    const auto approved =
        approve(SimDesk::order(++n, kTicked, vyuha::oms::OrderType::kLimit, 5000), desk.clock);
    bool ok = desk.sim().send(approved, "t" + std::to_string(n));
    benchmark::DoNotOptimize(ok);
  }
  vyuha::sim::SimEvent e;
  while (desk.sim().pop_event(e)) {
  }
  const vyuha::md::Tick tick = desk.tick(kTicked, 10000);
  std::vector<std::int64_t> samples;
  samples.reserve(static_cast<std::size_t>(state.max_iterations));
  for (auto _ : state) {
    const auto start = std::chrono::steady_clock::now();
    bool done = desk.sim().on_tick(tick);
    benchmark::DoNotOptimize(done);
    const auto end = std::chrono::steady_clock::now();
    samples.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
  }
  report(state, samples);
}
BENCHMARK(BM_SimBrokerOnTick)->Iterations(1'000'000);

void BM_SimBrokerSendMarketable(benchmark::State& state) {
  SimDesk desk;
  const vyuha::md::Tick tick = desk.tick(kTicked, 10000);
  vyuha::md::Tick stored = tick;
  bool ok = desk.book.update(stored);
  benchmark::DoNotOptimize(ok);
  // A pool of approvals, each its own client_order_id and tag, reused only after the history ring (4,096) has
  // forgotten the earlier use: approving costs more than the send being timed, so it is done up front.
  constexpr std::size_t kPool = 8192;
  std::vector<vyuha::risk::ApprovedOrder> pool;
  std::vector<std::string> tags;
  pool.reserve(kPool);
  tags.reserve(kPool);
  for (std::size_t i = 0; i < kPool; ++i) {
    pool.push_back(approve(
        SimDesk::order(static_cast<int>(i + 1), kTicked, vyuha::oms::OrderType::kMarket, 0), desk.clock));
    tags.push_back("t" + std::to_string(i));
  }
  std::vector<std::int64_t> samples;
  samples.reserve(static_cast<std::size_t>(state.max_iterations));
  vyuha::sim::SimEvent e;
  std::size_t i = 0;
  for (auto _ : state) {
    const std::size_t k = i++ % kPool;
    const auto start = std::chrono::steady_clock::now();
    bool sent = desk.sim().send(pool[k], tags[k]);
    benchmark::DoNotOptimize(sent);
    const auto end = std::chrono::steady_clock::now();
    samples.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    while (desk.sim().pop_event(e)) {  // ack and fill
    }
  }
  report(state, samples);
}
BENCHMARK(BM_SimBrokerSendMarketable)->Iterations(1'000'000);

// k fills among n orders on one instrument: 1,000 resting BUY limits all trade through on one tick. The
// orders are put back, untimed, before each iteration. This is what the doubly linked instrument chain is
// for.
void BM_SimBrokerOnTickFills1000(benchmark::State& state) {
  SimDesk desk;
  constexpr std::size_t kOrders = 1000;
  std::vector<vyuha::risk::ApprovedOrder> pool;
  pool.reserve(kOrders);
  for (std::size_t i = 0; i < kOrders; ++i) {
    pool.push_back(approve(
        SimDesk::order(static_cast<int>(i + 1), kTicked, vyuha::oms::OrderType::kLimit, 10000), desk.clock));
  }
  const vyuha::md::Tick through = desk.tick(kTicked, 9900);  // below every limit: all fill, at 100.00
  std::vector<std::int64_t> samples;
  samples.reserve(static_cast<std::size_t>(state.max_iterations));
  vyuha::sim::SimEvent e;
  std::size_t round = 0;
  for (auto _ : state) {
    for (std::size_t i = 0; i < kOrders; ++i) {
      bool ok = desk.sim().send(pool[i], "r" + std::to_string(round) + "-" + std::to_string(i));
      benchmark::DoNotOptimize(ok);
    }
    ++round;
    while (desk.sim().pop_event(e)) {
    }
    const auto start = std::chrono::steady_clock::now();
    bool done = desk.sim().on_tick(through);
    benchmark::DoNotOptimize(done);
    const auto end = std::chrono::steady_clock::now();
    samples.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    while (desk.sim().pop_event(e)) {
    }
  }
  report(state, samples);
}
BENCHMARK(BM_SimBrokerOnTickFills1000)->Iterations(2000);

}  // namespace
