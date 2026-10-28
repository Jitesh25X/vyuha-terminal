#include "vyuha/core/version.h"

#include <benchmark/benchmark.h>

namespace {

// Placeholder so the benchmark harness builds; real entries (gate, normalizer, rings) land with their tasks.
void BM_Version(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(vyuha::version());
  }
}
BENCHMARK(BM_Version);

}  // namespace
