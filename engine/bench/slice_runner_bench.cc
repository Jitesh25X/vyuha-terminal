// The freeze slicer on the core thread (F-EX-3, M2.5b): the parent check, a poll that is only waiting for an
// OPS token (what every core loop pays while a parent is being paced), and a whole parent of ten children
// started, gated and submitted. Rules 1 to 13, paper. Percentiles are counters; each sample includes one
// steady_clock read pair. Compare within one machine only.
// Run from the release preset: ./build/release/bench/vyuha_bench --benchmark_filter='SliceRunner|GateCheck'

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <benchmark/benchmark.h>

#include "vyuha/core/clock.h"
#include "vyuha/md/session.h"
#include "vyuha/oms/slice_runner.h"
#include "vyuha/risk/rules.h"

namespace {

using vyuha::Nanos;
using vyuha::Price;
using vyuha::oms::OrderDetails;
using vyuha::risk::LimitKey;

constexpr Nanos kNow = (Nanos{20'766} * Nanos{1'440} * vyuha::kNanosPerMinute) - vyuha::md::kIstOffset +
                       (Nanos{600} * vyuha::kNanosPerMinute);
constexpr std::int64_t kInstrument = 42;
constexpr std::int32_t kLot = 75;

class NullSink final : public vyuha::oms::IOrderEventSink {
 public:
  [[nodiscard]] bool try_push(const vyuha::oms::OrderEventRecord& /*event*/) noexcept override {
    ++count_;
    return true;
  }
  [[nodiscard]] std::uint64_t pushed() const noexcept override { return count_; }
  [[nodiscard]] std::uint64_t durable() const noexcept override { return count_; }

 private:
  std::uint64_t count_{0};
};

class NullSender final : public vyuha::oms::IOrderSender {
 public:
  [[nodiscard]] bool send(const vyuha::risk::ApprovedOrder& /*order*/,
                          std::string_view /*tag*/) noexcept override {
    return true;
  }
  [[nodiscard]] bool cancel(const vyuha::oms::Order& /*order*/) noexcept override { return true; }
};

class CountingIds final : public vyuha::oms::IOrderIdSource {
 public:
  // NOLINTNEXTLINE(bugprone-exception-escape): the indices are in range, so at() cannot throw
  [[nodiscard]] vyuha::oms::Uuid next() noexcept override {
    vyuha::oms::Uuid id;
    ++n_;
    for (std::size_t i = 0; i < 8; ++i) {
      id.bytes.at(15 - i) = static_cast<std::uint8_t>(n_ >> (8 * i));
    }
    return id;
  }

 private:
  std::uint64_t n_{0};
};

struct Desk {
  vyuha::SimClock clock{kNow};
  vyuha::risk::InstrumentTable instruments{64};
  vyuha::md::LastValueBook book{64};
  std::unique_ptr<vyuha::risk::GateContext> ctx{std::make_unique<vyuha::risk::GateContext>()};
  vyuha::oms::Uuid account;
  vyuha::oms::Uuid user;
  vyuha::risk::RiskGate gate{vyuha::risk::RiskGate::create(vyuha::risk::pre_trade_rules(), clock).value()};
  NullSink sink;
  NullSender sender;
  CountingIds ids;
  vyuha::risk::GateSendLimits limits{*ctx};
  vyuha::oms::Oms oms{sink, clock, 16'384};
  vyuha::oms::OrderRouter router{
      oms, sink, sender, vyuha::oms::Venue::kPaper, {.tag_limit = 20, .capacity = 256}, limits};
  vyuha::oms::SliceRunner runner{gate, *ctx, oms, router, sink, ids};

  Desk() {
    account.bytes.fill(1);
    user.bytes.fill(2);
    ctx->instruments = &instruments;
    ctx->book = &book;
    ctx->calendar.set_covered_until(kNow + (Nanos{30} * Nanos{1'440} * vyuha::kNanosPerMinute));
    static_cast<void>(instruments.put(kInstrument, {.lot_size = kLot,
                                                    .tick_size = Price::from_units(500),
                                                    .exchange = vyuha::risk::Exchange::kNfo,
                                                    .segment = vyuha::risk::Segment::kFut,
                                                    .freeze_qty = kLot + 1}));
    static_cast<void>(ctx->accounts.put({.id = account,
                                         .session_connected = true,
                                         .token_expires_at = kNow + vyuha::kNanosPerSecond,
                                         .ops_cap = 10}));
    static_cast<void>(
        ctx->entitlements.put({.user = user, .account = account, .can_trade = true, .can_live = true}));
    const auto limit = [this](LimitKey key, std::int64_t value) {
      static_cast<void>(ctx->limits.add(key, {.value = value * vyuha::risk::kLimitScale}));
    };
    limit(LimitKey::kMaxOrderValue, 5'000'000);
    limit(LimitKey::kMaxOrderQtyLots, 100'000);
    limit(LimitKey::kFatFingerPct, 5);
    limit(LimitKey::kDupWindowMs, 1'500);
    limit(LimitKey::kAllowMarketOrders, 1);
    limit(LimitKey::kAllowMisAfter, 1510);
    limit(LimitKey::kTradingWindowStart, 915);
    limit(LimitKey::kTradingWindowEnd, 1530);
    tick();
  }

  void tick() {
    vyuha::md::Tick t;
    t.instrument_id = static_cast<vyuha::md::InstrumentId>(kInstrument);
    t.ltp = Price::from_units(1'000'000);
    t.recv_ts = clock.now();
    t.recv_mono = clock.monotonic();
    static_cast<void>(book.update(t));
  }

  // Parent n: its own id and client_order_id.
  [[nodiscard]] OrderDetails parent(std::uint32_t n, std::int32_t children) {
    OrderDetails o{.is_paper = vyuha::oms::kPaperOrder};
    o.id = ids.next();
    std::array<char, 9> cid{'p'};
    for (std::size_t i = 0; i < 8; ++i) {
      cid.at(8 - i) = static_cast<char>('0' + ((n >> (3 * i)) & 7U));
    }
    static_cast<void>(o.client_order_id.assign({cid.data(), cid.size()}));
    o.account_id = account;
    o.user_id = user;
    o.instrument_id = kInstrument;
    o.qty = children * kLot;
    o.price = Price::from_units(1'000'000);
    return o;
  }
};

void report(benchmark::State& state, std::vector<std::int64_t>& samples) {
  if (samples.empty()) {
    return;
  }
  std::ranges::sort(samples);
  const auto at = [&](double q) {
    return static_cast<double>(
        samples[static_cast<std::size_t>(q * static_cast<double>(samples.size() - 1))]);
  };
  state.counters["p50_ns"] = at(0.50);
  state.counters["p99_ns"] = at(0.99);
}

template <class F>
void timed(std::vector<std::int64_t>& samples, F work) {
  const auto start = std::chrono::steady_clock::now();
  work();
  const auto end = std::chrono::steady_clock::now();
  samples.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
}

void BM_GateCheckParent(benchmark::State& state) {
  Desk d;
  const OrderDetails o = d.parent(0, 10);
  std::vector<std::int64_t> samples;
  samples.reserve(static_cast<std::size_t>(state.max_iterations));
  for (auto _ : state) {
    timed(samples, [&] {
      auto checked = d.gate.check(*d.ctx, o);
      benchmark::DoNotOptimize(checked);
    });
  }
  report(state, samples);
}
BENCHMARK(BM_GateCheckParent)->Iterations(200'000);

// Ten children go at once (the cap); the rest of the parent waits, and the clock stands still.
void BM_SliceRunnerPollWaiting(benchmark::State& state) {
  Desk d;
  if (!d.runner.start(d.parent(0, 20)).has_value() || d.runner.poll() != 10) {
    state.SkipWithError("the parent did not start");
    return;
  }
  static_cast<void>(d.router.poll());
  std::vector<std::int64_t> samples;
  samples.reserve(static_cast<std::size_t>(state.max_iterations));
  for (auto _ : state) {
    timed(samples, [&] {
      auto sent = d.runner.poll();
      benchmark::DoNotOptimize(sent);
    });
  }
  report(state, samples);
}
BENCHMARK(BM_SliceRunnerPollWaiting)->Iterations(200'000);

// start() plus the poll that gates, takes in and submits all ten children.
void BM_SliceRunnerTenChildren(benchmark::State& state) {
  Desk d;
  std::vector<std::int64_t> samples;
  samples.reserve(static_cast<std::size_t>(state.max_iterations));
  std::uint32_t n = 0;
  for (auto _ : state) {
    d.clock.advance(2 * vyuha::kNanosPerSecond);  // the tokens are back and the duplicate window is over
    d.tick();
    const OrderDetails o = d.parent(++n, 10);
    std::size_t sent = 0;
    timed(samples, [&] {
      auto started = d.runner.start(o);
      benchmark::DoNotOptimize(started);
      sent = d.runner.poll();
    });
    if (sent != 10) {
      state.SkipWithError("not all children went");
      break;
    }
    static_cast<void>(d.router.poll());
    // The broker is done with them, so the parent's slot can be reused.
    for (auto h = static_cast<vyuha::oms::OrderHandle>(d.oms.size() - 10); h < d.oms.size(); ++h) {
      auto acked = d.oms.apply(h, vyuha::oms::OrderEvent::kBrokerAck, {.broker_order_id = "B"});
      auto ended = d.oms.apply(h, vyuha::oms::OrderEvent::kBrokerCancel);
      benchmark::DoNotOptimize(acked);
      benchmark::DoNotOptimize(ended);
    }
  }
  report(state, samples);
}
BENCHMARK(BM_SliceRunnerTenChildren)->Iterations(1'000);

}  // namespace
