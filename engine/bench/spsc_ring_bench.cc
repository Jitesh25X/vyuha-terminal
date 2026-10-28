#include "vyuha/core/spsc_ring.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <benchmark/benchmark.h>

// M1.2 done check: p99 push/pop < 50 ns. Per-operation times are measured as batches of kBatch
// operations (a single push is ~ns, below the macOS clock's ~42 ns tick), timed only while the
// ring has room (producer) or data (consumer), so waiting for the other side is excluded and what
// remains is the operation cost under real cross-core contention. Counters report ns per op.

namespace {

using Clock = std::chrono::steady_clock;  // monotonic interval timing only (never wall time)
constexpr std::size_t kBatch = 128;
constexpr std::size_t kRing = 4096;

// About the size of a normalized tick.
struct Tick64 {
  std::array<std::uint64_t, 8> words{};
};

template <class T>
T make(std::uint64_t seq) noexcept {
  if constexpr (std::is_same_v<T, Tick64>) {
    T t;
    t.words[0] = seq;
    return t;
  } else {
    return static_cast<T>(seq);
  }
}

double percentile(std::vector<double>& v, double p) {
  if (v.empty()) {
    return 0;
  }
  const auto idx = static_cast<std::size_t>(p * static_cast<double>(v.size() - 1));
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(idx), v.end());
  return v[idx];
}

double ns_per_op(Clock::time_point start, Clock::time_point end) {
  return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()) /
         static_cast<double>(kBatch);
}

void report(benchmark::State& state, const char* name, std::vector<double>& samples) {
  const std::string prefix(name);
  state.counters[prefix + "_p50_ns"] = percentile(samples, 0.50);
  state.counters[prefix + "_p99_ns"] = percentile(samples, 0.99);
  state.counters[prefix + "_p999_ns"] = percentile(samples, 0.999);
}

// Uncontended baseline: one thread pushes a batch then pops it.
template <class T>
void BM_SpscSingleThread(benchmark::State& state) {
  auto ring = std::make_unique<vyuha::SpscRing<T, kRing>>();
  std::vector<double> push_ns;
  std::vector<double> pop_ns;
  std::uint64_t seq = 0;
  T out{};
  for (auto _ : state) {
    const auto t0 = Clock::now();
    for (std::size_t i = 0; i < kBatch; ++i) {
      benchmark::DoNotOptimize(ring->try_push(make<T>(seq++)));
    }
    const auto t1 = Clock::now();
    for (std::size_t i = 0; i < kBatch; ++i) {
      benchmark::DoNotOptimize(ring->try_pop(out));
    }
    const auto t2 = Clock::now();
    push_ns.push_back(ns_per_op(t0, t1));
    pop_ns.push_back(ns_per_op(t1, t2));
  }
  benchmark::DoNotOptimize(out);
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * kBatch));
  report(state, "push", push_ns);
  report(state, "pop", pop_ns);
}

// Producer (benchmark thread) and consumer on separate cores, streaming continuously.
template <class T>
void BM_SpscTwoThreads(benchmark::State& state) {
  auto ring = std::make_unique<vyuha::SpscRing<T, kRing>>();
  std::atomic<bool> stop{false};
  std::vector<double> pop_ns;
  pop_ns.reserve(1U << 20U);

  std::thread consumer([&] {
    T out{};
    while (!stop.load(std::memory_order_relaxed)) {
      if (ring->size_approx() < kBatch) {
        continue;  // untimed: wait until a full batch is available
      }
      const auto t0 = Clock::now();
      for (std::size_t i = 0; i < kBatch; ++i) {
        while (!ring->try_pop(out)) {
        }
      }
      const auto t1 = Clock::now();
      if (pop_ns.size() < pop_ns.capacity()) {
        pop_ns.push_back(ns_per_op(t0, t1));
      }
    }
    while (ring->try_pop(out)) {  // drain
    }
    benchmark::DoNotOptimize(out);
  });

  std::vector<double> push_ns;
  std::uint64_t seq = 0;
  for (auto _ : state) {
    while (kRing - ring->size_approx() < kBatch) {
      // untimed by our counters: wait for room for a whole batch
    }
    const auto t0 = Clock::now();
    for (std::size_t i = 0; i < kBatch; ++i) {
      while (!ring->try_push(make<T>(seq))) {
      }
      ++seq;
    }
    const auto t1 = Clock::now();
    push_ns.push_back(ns_per_op(t0, t1));
  }
  stop.store(true, std::memory_order_relaxed);
  consumer.join();
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * kBatch));
  report(state, "push", push_ns);
  report(state, "pop", pop_ns);
}

BENCHMARK(BM_SpscSingleThread<std::uint64_t>);
BENCHMARK(BM_SpscSingleThread<Tick64>);
BENCHMARK(BM_SpscTwoThreads<std::uint64_t>)->UseRealTime();
BENCHMARK(BM_SpscTwoThreads<Tick64>)->UseRealTime();

}  // namespace
