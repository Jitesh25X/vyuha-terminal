# engine/ — C++23 core

Read `docs/ARCHITECTURE.md §4–5`, `docs/RISK_ENGINE.md` and the root `CLAUDE.md` invariants first.

## Layout
```
include/vyuha/{core,md,oms,risk,broker,sim,bus,util}/   public headers
src/{core,md,oms,risk,broker/kite,broker/upstox,sim,bus,app}/
bindings/python/      nanobind module `vyuha_core` (clock, SimBroker, RiskGate, OMS for backtests)
tests/                GoogleTest (+ RapidCheck property tests)
bench/                Google Benchmark (gate, normalizer, rings)
fixtures/vcr/         recorded broker HTTP/WS fixtures (scrubbed of tokens)
fuzz/                 libFuzzer targets for broker parsers
```

## Build
```
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
cmake --preset asan  ...   # ASan+UBSan
cmake --preset tsan  ...   # ThreadSanitizer
cmake --preset release && ./build/release/bench/vyuha_bench
```

## Rules
- C++23, `-Wall -Wextra -Wpedantic -Werror`, clang-tidy profile in `.clang-tidy`.
- **Hot path** (core thread, IO decode): no exceptions (`-fno-exceptions` is not global, but hot-path code must be `noexcept`), no heap allocation per tick/intent (use pools), no locks, no logging with formatting on the core thread (Quill's async backend only), no `std::function` in inner loops.
- Errors: `std::expected<T, Error>`. `Error` = `{ErrorCode code; std::string_view detail}`.
- Money: `vyuha::Price` (strong typedef over int64, 1 rupee = 10000 units). Never `double` for prices outside analytics.
- Time: `vyuha::Nanos` (int64 since epoch, UTC). The clock is injected via `IClock` (wall or simulated). Never call `std::chrono::system_clock::now()` directly outside `WallClock`.
- `risk::ApprovedOrder` can only be constructed by `RiskGate`. Adapters accept only `ApprovedOrder`/`ApprovedModify`. Don't weaken this.
- Every broker adapter has: a README with doc URLs + verified date + limits table; VCR contract tests; a fuzz target for its parser; and zero live network calls in tests.
- Sockets for an account bind to `SessionConfig::source_ip` **before** connect. Preflight checks run on `start()`.
- Secrets: `SecretRef` holds secrets in locked memory (`mlock`) and zeroises on destruction. Never stream secrets to logs. The `scrub()` helper is applied to all HTTP logs.
- Threads: IO threads (Asio), the core thread (pinned via `ENGINE_CORE_CPU`), NATS publisher, Postgres event writer, gRPC server. Only the core thread mutates trading state.
- Every gate rule ships with: unit tests for pass/fail/edge, a benchmark entry, and a stable `R-*` code added to `RISK_ENGINE.md §3`.
