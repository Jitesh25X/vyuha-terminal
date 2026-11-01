# TASKS — execution queue

This file is the **single source of truth for what to build next**. Claude Code works through it **one task at a time, top to bottom**, using `/next-task`.

## How to run a task (Claude Code follows this exactly)
1. Take the **first task whose box is `[ ]`** and whose `deps` are all `[x]`. Don't skip ahead, and don't work on two tasks at once.
2. Read the task's row in `ROADMAP.md`, the PRD feature(s) it implements, and the relevant spec + service `CLAUDE.md`.
3. **Downloads: install only what the task's `needs` line lists.** If you believe something else is required, stop and ask the owner. Add it to the `needs` line only after approval (PRD §10.1).
4. Tests first for logic touching money, risk, OMS, pricing, costs, RBAC or agents. Then implement.
5. Run the `done` check plus `make verify`. If the task touches risk/OMS/broker/auth/RBAC/MCP/agents, run the `risk-reviewer` subagent and fix every P0/P1.
6. Mark the box `[x]`, append the date and a short note (`— 2026-11-02: done; PR #12; deviation: …`), and commit (Conventional Commit with the task ID).
7. At the end of a milestone, run `/milestone-exit` before starting the next milestone's first task.
8. Claude Code writes all code (ADR-017). The owner reviews each task's result before saying "continue".

Legend: `deps` = tasks that must be `[x]` first · `needs` = the **only** things you may install or download for this task · `done` = completion check.

Current milestone: **M1**


## M0 — Foundations

- [x] **M0.1** Monorepo skeleton per PRD §9; `Makefile` targets; `.editorconfig`; pre-commit (gitleaks, clang-format, ruff, gofmt, prettier) — 2026-10-28: done; skeleton dirs per PRD §9 (.gitkeep), .editorconfig, pre-commit via local system hooks (skip until each tool is installed), `make lint` skips buf until M0.4
  - deps: —
  - needs: nothing (text files only; write `.pre-commit-config.yaml` but don't install hooks until the tools arrive in their own tasks)
  - done: `make invariants` green; `make test` skips cleanly for unscaffolded services
- [x] **M0.2** Pin toolchains & versions (fill `TECH_STACK.md §6`); devcontainer/VM setup script — 2026-10-28: done; `scripts/bootstrap.sh` auto-detects macOS (Homebrew) / Ubuntu 24.04 (apt), pins Go/Node/uv with sha256, `--check` mode; macOS core run installed nothing; devcontainer not added
  - deps: M0.1
  - needs: nothing yet. Write `scripts/bootstrap.sh <component>` (core|cpp|go|py|web) but run only `core` (git, jq, docker present?)
  - done: `make bootstrap C=core` works on macOS (dev machine) and installs nothing beyond the core list; Ubuntu 24.04 verification is deferred to M1.12 on the staging VM.
- [x] **M0.3** `deploy/docker-compose.dev.yml` with profiles: `core` (Postgres, NATS) now; `md` (ClickHouse), `graph` (Neo4j), `obs` (Grafana/Prometheus/Loki/OTel) started only at their milestones — 2026-10-28: done; core digests pinned, NATS JetStream healthcheck, `make up` healthy, `make migrate` → schema version 2 (37 tables), idempotent; audit hash bug found (see notes)
  - deps: M0.2
  - needs: Docker images: `pgvector/pgvector:pg17`, `nats:2.10-alpine` only (profile `core`)
  - done: `make up` (core) healthy; `make migrate` applies Postgres DDL
- [x] **M0.4** Buf setup; CI `buf lint` + `buf breaking`; codegen wired into each service when it is scaffolded (M0.5 C++, M0.6 Go, M0.9 TS, M1.1 Python) — 2026-10-28: done; buf 1.75.0 via `bootstrap.sh proto`; `buf lint` + format green; `make proto-breaking` vs main green; `.github/workflows/proto.yml`; per-language `buf.gen.<lang>.yaml`, `make proto` generates only for scaffolded services; negative tests (bad field name, deleted field) caught
  - deps: M0.3
  - needs: `buf` CLI (single binary). No language plugins until each service exists (remote plugins, nothing installed locally)
  - done: `buf lint` green; each later scaffold task proves its generated code compiles
- [x] **M0.5** Engine CMake skeleton (presets: debug, release, asan, tsan), vcpkg manifest, GoogleTest/Benchmark, nanobind hello module — 2026-10-28: done; `make test-engine` green (debug 3/3 incl. `python.import_vyuha_core`, asan 2/2), tsan green, `make bench` runs, clang-format + clang-tidy clean; vcpkg baseline 19c33061; nanobind via overlay port (owner choice)
  - deps: M0.4
  - needs: `bootstrap.sh cpp`: compiler, CMake, Ninja, vcpkg (shallow clone). vcpkg ports: `gtest`, `benchmark`, `nanobind` only
  - done: `make test-engine` green; `import vyuha_core` works
- [x] **M0.6** Gateway skeleton: config, health, OpenAPI codegen, pgx/sqlc, migrations runner, structured logging + OTel — 2026-10-28: done; `GET /api/v1/health` 200 on the real binary (503 + generic reason with Postgres stopped); `vyuhactl migrate` (golang-migrate) took over schema v2 with no re-apply; Go protobuf codegen compiles; unit + Postgres integration tests green
  - deps: M0.5
  - needs: `bootstrap.sh go`: Go toolchain. Modules: pgx, sqlc (binary), oapi-codegen, golang-migrate, OTel SDK
  - done: `GET /api/v1/health` 200
- [x] **M0.7** Auth: users, argon2id, TOTP enroll/verify, JWT EdDSA, refresh rotation + reuse detection, lockout, `vyuhactl user create` — 2026-10-28: done; auth suite green with `-race` (unit: tokens/alg-none/audience confusion, TOTP window, password policy; integration: enrollment, replay, lockout incl. 20 concurrent guesses → exactly 5 evaluated, refresh rotation + reuse revokes family, expiry, logout, reauth; HTTP e2e over a real cookie jar); mutation-checked; risk-reviewer BLOCK (2×P1) → fixed → APPROVE
  - deps: M0.6
  - needs: Go modules: argon2id, pquerna/otp, golang-jwt
  - done: Auth test suite (incl. reuse detection) green
- [x] **M0.8** Audit writer + `audit verify` CLI — 2026-10-28: done; tamper suite green (payload/actor kind/actor id/ts/ip modified, text shifted across fields, middle row deleted, forged row, newest row deleted via anchor), append-only trigger, `vyuha_app` INSERT-only (42501), 50 concurrent writers, READ COMMITTED guard; auth events audited in-transaction; risk-reviewer BLOCK (P1: password-as-username stored) → fixed → APPROVE
  - deps: M0.7
  - needs: nothing new
  - done: Tamper test detects modified row
- [x] **M0.8a** Auth hardening (added 2026-10-28, owner-approved): per-IP rate limits on `/auth/*` (429 + `Retry-After`, trusted-proxy aware client IP), bounded concurrent credential checks + row-lock timeout (risk-reviewer P2s from M0.7/M0.8)
  - deps: M0.8
  - needs: nothing new (Go stdlib)
  - done: limiter + middleware tests (burst, refill, per-IP, X-Forwarded-For only from trusted proxies); a burst of logins for one user can't take more than the attempt cap of DB connections; risk-reviewer APPROVE — 2026-10-28: done; 20-login burst peaks at 2 pooled connections with cap 2 (8 without — mutation-checked); risk-reviewer BLOCK (P1: empty TRUSTED_PROXIES made the limit global behind Caddy) → fixed → APPROVE
- [x] **M0.9** Web skeleton: Vite/React/TS strict, design tokens, top bar, status bar, Dockview workspace, command bar with function registry (stub panels), login + MFA screens — 2026-10-28: done; `make e2e` green (Chromium, keyboard only: first login → TOTP enrollment via QR → workspace → `` ` `` + `HELP` Enter → help panel; `HELP CH`; `F1`; reload restores the session from the refresh cookie; enrolled sign-in rejects a bad code, then signs in; sign-out revokes); vitest 31 green (command grammar, IST clock, refresh single-flight — mutation-checked); `tsc` strict clean; 36 MVP functions registered (HELP real, others stub)
  - deps: M0.8a
  - needs: `bootstrap.sh web`: Node LTS + pnpm. npm: react, react-dom, vite, typescript, dockview, cmdk, zustand, @tanstack/react-query, tailwindcss, tinykeys, vitest, @playwright/test (+ Chromium only: `playwright install chromium`); owner-approved 2026-10-28: @types/react, @types/react-dom, @tailwindcss/vite
  - done: Playwright: login → workspace → `HELP` opens via keyboard only
- [x] **M0.10** CI (GitHub Actions): matrix build/test for all services, sanitizers job, artifact cache — 2026-10-28: done; `.github/workflows/ci.yml` green on PR #1 (engine debug + clang-tidy/format, asan, tsan; gateway `-race` with Postgres + golangci-lint + codegen drift; web tsc/vitest/build/e2e; invariants + gitleaks; `ci-ok` aggregate); caches for tools, vcpkg binaries, Go, pnpm, Playwright; actions pinned by SHA. **Not enforced as required** (private repo on GitHub Free — owner choice, see deviations)
  - deps: M0.9
  - needs: nothing locally (CI caches keyed by lockfiles)
  - done: PR checks required on `main`

**M0 exit:** login with TOTP works end-to-end in the browser; CI green; DB migrations apply cleanly.
- [x] M0 exit verified (`/milestone-exit`) — 2026-10-28: TOTP login end-to-end in Chromium (`make e2e`: enroll via QR → workspace → HELP; enrolled sign-in; reload restore; sign-out); CI green on PR #1 (all jobs + `ci-ok`; advisory, not required — M0.10 deviation); migrations 000001–000005 apply to a fresh DB and re-apply as a no-op (v5, not dirty; dev DB v5 clean); `make verify` green; `make clean-space` run (repo 213M → 162M; Docker build cache 10.7 GB pruned; free disk 24 → 34 GiB)


## M1 — Instruments & market data

- [x] **M1.1** Instrument master sync (quant job): Kite + Upstox files → unified `instrument` with mapping, freeze qty, lot size; holiday calendar seed — 2026-10-28: done; F-MD-1 ACs covered by `quant/tests` (26 green, mutation-checked): AC1 both-broker contracts → one row with `kite_token` + `upstox_key`; AC2 one-broker contracts → `partial_mapping`; AC3 failed sync (bad file, DB error, conflicts, mass-close guard) rolls back, keeps yesterday's master, critical admin alert; AC4 ban list via manual import (ADR-019) + `ban check` alert. Real run 2026-10-28: 65,967 instruments, 63,133 matched, 0 conflicts, 2,834 partial, every derivative linked to its underlying; 6 s first sync, 2.8 s idempotent re-run; holidays seeded (XBOM to 2026-12-31)
  - deps: M0 exit
  - needs: `bootstrap.sh py`: Python + uv. quant group `core`: polars, httpx, pydantic, sqlalchemy, asyncpg, exchange_calendars. Broker instrument files are fetched at runtime (not committed)
  - done: F-MD-1 ACs
- [x] **M1.2** `SpscRing<T,N>` + benchmarks — 2026-10-28: done; `include/vyuha/core/spsc_ring.h` (wait-free, cached indices, 128 B padding); `make bench` on Apple M2 (release), p99 ns push/pop: single-thread 3.9/3.6 (u64), 4.2/3.6 (64 B tick); two cores streaming 3.9/3.9 (u64, 375 M items/s), **29.3/11.1** (64 B tick, 64 M items/s); TSan + ASan/UBSan clean; mutation (relaxed publish) → TSan data race
  - deps: M1.1
  - needs: nothing new
  - done: p99 push/pop < 50 ns; TSan clean
- [x] **M1.3** Engine core loop, canonical `Tick`, `Price` int64 type, LastValue book, bar builder — 2026-10-28: done; `Price` (int64, ₹1 = 10 000 units, exact parse/format + round-trip tests), `Tick`/`Bar` (trivially copyable, mirror the proto), `IClock` (WallClock/SimClock), fixed-capacity `IdMap`, `LastValueBook`, 1s/1m `BarBuilder`, `CoreLoop` (round-robin over SPSC rings, seq stamping, validation, timers, IST day rollover), Quill logging; 37 tests incl. 09:15 open / 15:29 last bar / 15:30 close (mutation: inclusive close → 2 failures) and a counting-allocator test proving zero heap allocations in steady state; ASan/UBSan + TSan clean; `BM_CorePollTicks` 15–19 M ticks/s on Apple M2
  - deps: M1.2
  - needs: vcpkg: `quill`, `fmt`
  - done: Unit tests incl. bar boundary at 09:15/15:30 IST
- [x] **M1.4** Kite Ticker adapter (binary parser, sharding ≤ 3,000/conn, ≤ 3 conns/key, reconnect) with recorded-frame fixtures — 2026-10-29: done; parser for all 5 packet layouts (LTP 8, index 28/32, quote 44, full 184 with 5-level depth), big-endian, paise → `Price`; libFuzzer 10 min clean (60.8 M inputs, macOS UBSan + libc++ hardening; mutation: removing the frame-length check crashes in < 30 s); F-MD-2 AC1 shard limit table (1/3 000/3 001/6 500/9 000 tokens, 9 001 rejected); AC2 reconnect against a local WebSocket server: drop → backoff (0.5 s→30 s, equal jitter) → reconnect from the bound source IP → identical resubscription → gap logged with start/end; static-IP preflight refuses a non-local IP; 47 tests, ASan/UBSan + TSan clean, reconnect test stable over 20 repeats
  - deps: M1.3
  - needs: vcpkg: `boost-asio`, `boost-beast`, `openssl`, `simdjson`; libFuzzer comes with Clang
  - done: Parser fuzz test (libFuzzer) 10 min clean; F-MD-2 ACs (fixture-based)
- [x] **M1.5** Upstox Market Data Feed V3 adapter (authorize → WSS, protobuf decode, modes, limits table) — 2026-10-29: done; authorize (bearer) → single-use `wss://` URL → WSS from the bound source IP, binary JSON requests, zero-allocation protobuf wire decoder for all feed types (LTPC, full market/index, first-level+greeks; market_info), modes + Standard/Plus limits table; F-MD-2 AC1 limits table test, AC2 local authorize + WS servers: re-authorizes per connection, single-use codes respected, identical resubscription, reconnect + gap log; 10-min libFuzzer clean with UB fatal (Upstox 152.7 M inputs, Kite re-run 98.7 M); decoder checked against `protoc` reference classes; 58 tests (55 under ASan/TSan) green, reconnect tests stable ×10 under TSan
  - deps: M1.4
  - needs: vcpkg: `protobuf`
  - done: Same as above
- [x] **M1.6** Per-account source-IP binding + preflight; session manager; gRPC `StartSession/GetSessions/Subscribe` — 2026-10-29: done; `SessionManager` (per-account sessions, preflight of source + backup IP, states connected/awaiting_login/error, per-requester subscription interest, feeds bound to the account IP, ring pool) + gRPC `EngineService` (StartSession, StopSession, GetSessions, Subscribe, Unsubscribe, Health) + `vyuha_engine` binary; STATIC_IP_NOT_ON_HOST tested in-process and over gRPC (FAILED_PRECONDITION, session listed as `error` with the code, admin-alert log line); ops_cap > 10 rejected (invariant 7); 65 tests (60 under ASan/TSan); binary smoke-tested (listens, clean SIGTERM shutdown)
  - deps: M1.5
  - needs: vcpkg: `grpc`
  - done: `STATIC_IP_NOT_ON_HOST` test
- [x] **M1.7** NATS publisher + KV `md_last`; recorder → ClickHouse — 2026-10-29: done; engine `MdPublisher` (core → bounded SPSC ring → publisher thread; protobuf hand-encoded, checked against generated classes) publishes `md.tick.<id>`, `md.bar.<1s|1m>.<id>` and KV `md_last`; Go `recorder` (`gateway/cmd/recorder`): NATS → bounded queue → batched ClickHouse inserts with retries, `/metrics` incl. `recorder_dropped_total`; engine loads the instrument master from Postgres (libpqxx; 65,967 instruments on the dev DB). F-MD-4 AC1: 50,000 published = 50,000 rows (recorder integration test) and C++ engine → Go recorder → ClickHouse end to end 20,000 = 20,000; AC2: `Handle` never blocks (10,000 msgs into a stalled 100-slot queue in < 1 s, drops counted; mutation: blocking send hangs the test), engine publisher ring drops+counts instead of blocking. `make up PROFILES="core md" && make migrate-ch` works (idempotent)
  - deps: M1.6
  - needs: vcpkg: `cnats`, `libpqxx`; Docker image `clickhouse/clickhouse-server:24.8` (profile `md`); Go: clickhouse-go, nats.go
  - done: F-MD-4 ACs; `make up PROFILES="core md" && make migrate-ch` works
- [x] **M1.8** Gateway: broker login URL/callback flows, encrypted secrets, WS hub with entitlement + conflation — 2026-10-29: done; `GET /brokers/{id}/login-url` + Kite/Upstox callbacks (signed single-use state, token exchange bound to the account's static IP, client-code match, access token sealed with expiry, engine StopSession+StartSession, audited); `vyuhactl account create|map` (API key/secret sealed, never from flags); `/ws` hub: ClientHello/4401, quote/depth/bar.1m/system channels, per-group conflation, 4 MB outbound cap → SLOW_CONSUMER + snapshot-only mode, token expiry → AUTH_EXPIRED/4401, engine interest reconciled per account (re-sent after a session restart). **Entitlement test** (`TestEntitlement`): users without a mapping or with a non-entitled account get `MD_NOT_ENTITLED`; each entitled user receives ticks only from their own account's feed (engine `feed_id`, ADR-020); unmapping mid-session drops the subscription and the engine interest. **Slow-consumer test** (`TestSlowConsumer`): a stalled client's buffer stays ≤ the cap, it gets SLOW_CONSUMER then full snapshots then updates again, while another client on the same instruments keeps receiving (2,000 keys every 10 ms). Mutation-checked (feed filter, entitlement grant, buffer cap, client-code check, RBAC). Gateway + engine binaries smoke-tested together
  - deps: M1.7
  - needs: Go: coder/websocket, grpc
  - done: Entitlement test; slow-consumer test
- [x] **M1.9** Web: WS worker, stores, `Q`, `WL`, `CH`, `DOM`, `TS`, status bar feed indicators — 2026-10-29: done; market-data Web Worker (socket, protobuf decode, latest-per-key merge, one diff per animation frame, reconnect with backoff, 4401 → token refresh + restart), ref-counted subscriptions (released 10 s after the last holder), zustand md store; panels `Q`, `DOM`, `TS`, `CH` (history from ClickHouse via `GET /marketdata/candles` + live 1-minute bars aggregated to 5m/15m/1h/1d), `WL` (AG Grid, async transactions, cell flash); status bar with market state, per-account feed dots, data latency, engine/stream state and the F-UI-3 disconnect toast; gateway `GET /instruments/search`, `/instruments/{id}`, `/marketdata/candles`. **Perf test (F-MD-2 AC4):** 1,000 watchlist rows × 4 Hz for 60 s over a real WebSocket → 59.9 fps, 0 long tasks (`e2e/perf.spec.ts`, data from `gateway/cmd/fakefeed`). Keyboard-only e2e for Q/DOM/TS/CH/WL against the real gateway hub (entitlement message for an unmapped user, unknown symbol, WL add/delete)
  - deps: M1.8
  - needs: npm: lightweight-charts, ag-grid-community, ag-grid-react, @bufbuild/protobuf
  - done: Perf test (F-MD-2)
- [x] **M1.10** Option chain service (quant: IV/Greeks, PCR, max pain) + `OC`, `OI` panels — 2026-10-29: done; quant `pricing/` (generalized Black-Scholes: Black-76 on the forward, BSM on spot with escrowed dividends; analytic Greeks; Brent IV on [0.001, 5] with bisection fallback), `optionchain/` (forward = same-expiry future, else put-call-parity synthetic from ATM mids → LTPs; PCR, max pain, ATM) behind an internal FastAPI app (`vyuha-quant serve`); gateway `GET /optionchain/{underlying_id}` + `GET /instruments/{id}/expiries`; web `OC` (calls | strike | puts, ITM shading, ATM outline, Greeks toggle, keyboard rows) and `OI` (ECharts OI/ΔOI by strike, build-up table, session PCR line). **F-MD-3 AC "IV/Greeks within 1 % of reference":** recovered within 1 % of a known smile from tick-rounded premiums; kernels match Hull/Haug published vectors to 4 dp and finite differences to 1e-6 (QuantLib cross-check at M5.1). F-MD-3 "OC row + B/S opens a pre-filled ticket" is deferred to M2.9 (`ORD`), added to its done check
  - deps: M1.9
  - needs: Python: numpy, scipy, fastapi, uvicorn; npm: echarts (nats-py moved to M5.2, 2026-10-29)
  - done: F-MD-3 ACs
- [x] **M1.11** `MOV`, `HM` panels — 2026-10-29: done; contract `GET /indices/{id}/constituents` (reference data: members, weight, sector, optional `avg_volume_20d`, near-month `future_id` + `future_prev_oi`), answering 404 `NO_CONSTITUENTS` until a source exists (M6.1, owner decision); `MOV` (gainers, losers, volume shockers vs 20-day average, OI gainers on near-month futures) and `HM` (ECharts treemap grouped by sector, sized by weight, diverging colour clamped at ±3 %, sector table) rank/colour live WebSocket quotes sampled once a second. **Done check:** keyboard-only Playwright test renders fixture constituents with live quotes from the fake feed
  - deps: M1.10
  - needs: nothing new
  - done: Render with fixture data
- [x] **M1.11b** Engine last-trade time: `marketdata.v1.Tick.last_trade_ts_ns` (additive; `buf breaking` must pass). Upstox fills it from the broker's last-trade time (`ltt`); Kite uses full mode's last-traded timestamp where present, else the engine infers it from cumulative volume increasing (the engine sees every tick, the gateway only conflated ones). Stored in KV `md_last` so it survives restarts; the gateway reads the field instead of inferring (replaces M1.10's hub volume-delta inference)
  - deps: M1.11
  - needs: nothing new
  - done: Unit tests for both brokers; gateway-restart test showing LTP-based IVs keep showing — 2026-10-30: done; `TradeClock` (core thread) stamps/infers per (instrument, feed); Kite full mode reads bytes 44–48 (verified against pykiteconnect 2026-10-30), Upstox `ltt`; hub's volume-delta inference removed. Restart test `TestOptionChainLTPIVSurvivesGatewayRestart` (fresh hub, real NATS KV) passed against a running `vyuha-quant serve`
- [ ] **M1.12** **Staging:** VM with 2 static IPs, real Kite + Upstox sessions streaming Nifty 50 + NIFTY options
  - deps: M1.11b
  - needs: Staging VM only; record only Nifty 50 + NIFTY options near expiries
  - done: Tick→screen p95 < 400 ms measured; `make bootstrap C=core` passes on the staging Ubuntu VM

**M1 exit:** live data from both brokers on staging; benchmarks meet NFRs.
- [ ] M1 exit verified (`/milestone-exit`)


## M2 — Paper OMS & risk gate

- [ ] **M2.1** OMS state machine + `order_event` writer thread + recovery from Postgres
  - deps: M1 exit
  - needs: vcpkg: `rapidcheck`
  - done: Property tests: only legal transitions
- [ ] **M2.2** RiskGate framework (`ApprovedOrder` construct-only-by-gate, rule table, GateContext)
  - deps: M2.1
  - needs: nothing new
  - done: Compile-time test that adapters can't receive raw intents
- [ ] **M2.3** Rules R-KILL…R-ORDER-SHAPE, R-FAT-FINGER, R-ORDER-VALUE, R-DUP, R-OPS
  - deps: M2.2
  - needs: nothing new
  - done: Per-rule unit tests; bench p99 < 20 µs
- [ ] **M2.4** Rules R-OPEN-ORDERS…R-DISCIPLINE with incremental scope aggregation
  - deps: M2.3
  - needs: nothing new
  - done: Property tests (RISK_ENGINE §3)
- [ ] **M2.5** SimBroker (paper) on live ticks; brackets/OCO; freeze-qty slicer
  - deps: M2.4
  - needs: nothing new
  - done: F-EX-4 ACs
- [ ] **M2.6** Position/MTM projector + costs (temporary flat schedule until M4)
  - deps: M2.5
  - needs: nothing new
  - done: P&L unit tests
- [ ] **M2.7** Kill switch (scopes, actions, auto triggers) + gRPC
  - deps: M2.6
  - needs: nothing new
  - done: F-RK-2 ACs (paper)
- [ ] **M2.8** Gateway: orders/positions/pnl/limits/kill APIs + WS channels
  - deps: M2.7
  - needs: nothing new
  - done: API tests
- [ ] **M2.9** Web: `ORD` ticket with live preview, `BLT`, `POS`, `PNL`, `LIM`, `KILL`, hotkeys
  - deps: M2.8
  - needs: nothing new (reuse grid/chart libs)
  - done: Playwright: keyboard-only paper trade flow; `OC` row + `B`/`S` opens a pre-filled ticket for that contract (F-MD-3 AC, deferred from M1.10)

**M2 exit:** a full paper trading day on staging with ≥ 50 orders, zero illegal transitions, and every gate rule exercised by tests.
- [ ] M2 exit verified (`/milestone-exit`)


## M3 — Live execution

- [ ] **M3.1** Kite order adapter (place/modify/cancel, tags, market protection, order-update stream, positions/margins) with VCR fixtures
  - deps: M2 exit
  - needs: nothing new (recorded fixtures built from documented example payloads)
  - done: Contract tests
- [ ] **M3.2** Upstox order adapter (same) + Portfolio Stream Feed
  - deps: M3.1
  - needs: nothing new
  - done: Contract tests
- [ ] **M3.3** OPS token bucket per account per exchange; reject storm detection
  - deps: M3.2
  - needs: nothing new
  - done: Burst test
- [ ] **M3.4** Reconciliation (60 s, on reconnect, EOD) + `UNKNOWN` resolution
  - deps: M3.3
  - needs: nothing new
  - done: Fault-injection tests (timeouts, dup updates, out-of-order)
- [ ] **M3.5** Margin cache + broker margin preview for large orders
  - deps: M3.4
  - needs: nothing new
  - done: Unit tests
- [ ] **M3.6** `LIVE_TRADING_ENABLED` guard, live/paper UI distinctions, re-auth flows
  - deps: M3.5
  - needs: nothing new
  - done: E2E
- [ ] **M3.7** Compliance report `COMP`; `AUD` viewer
  - deps: M3.6
  - needs: nothing new
  - done: F-CO-1/2 ACs
- [ ] **M3.8** Backups + restore drill; Grafana dashboards; alerting (engine down, feed down, recon mismatch)
  - deps: M3.7
  - needs: Docker images: Prometheus, Grafana, Loki, OTel collector (profile `obs`)
  - done: Restore drill documented
- [ ] **M3.9** **Staging live pilot (owner's own account only)**: 1-lot orders, all brackets, kill switch drill
  - deps: M3.8
  - needs: nothing new
  - done: PRD §2 live-order criterion passes; owner sign-off

**M3 exit:** owner's account trades live through Vyuha for 5 sessions with no recon mismatches. The legal question (PRD §13 #2) must be answered before any other person's account is connected.
- [ ] M3 exit verified (`/milestone-exit`)


## M4 — Data, strategies & backtesting

- [ ] **M4.1** Download manager (Kite + Upstox historical, expired options), Parquet lake, dataset catalogue, `DL` panel
  - deps: M3 exit
  - needs: Python: aiolimiter, duckdb, pyarrow. **Data:** only the instruments/interval/range in the job spec (start with Nifty 50, 1-min, 2 years)
  - done: F-RS-1 ACs
- [ ] **M4.2** Cost schedule seeding (verified, with sources) + cost engine + contract-note tests
  - deps: M4.1
  - needs: nothing new
  - done: 5 contract-note tests pass to ₹0.01
- [ ] **M4.3** nanobind bindings for core (clock, SimBroker, gate, OMS)
  - deps: M4.2
  - needs: nothing new (nanobind already present)
  - done: Python tests drive core deterministically
- [ ] **M4.4** Strategy SDK + indicators + sandboxed strategy host
  - deps: M4.3
  - needs: Python: numba (only if a benchmark shows it is needed)
  - done: F-RS-2 ACs
- [ ] **M4.5** Backtest engine (fill models, sessions, expiry handling, look-ahead guards)
  - deps: M4.4
  - needs: nothing new
  - done: Golden tests for 3 reference strategies
- [ ] **M4.6** Metrics + report + artifacts; sweeps; walk-forward
  - deps: M4.5
  - needs: nothing new
  - done: F-RS-3 ACs
- [ ] **M4.7** Strategy lifecycle + approval; paper/live strategy hosting via engine
  - deps: M4.6
  - needs: nothing new
  - done: F-RS-4 ACs
- [ ] **M4.8** Web: `STRAT` (Monaco), `BT` report, `PAY` payoff
  - deps: M4.7
  - needs: npm: @monaco-editor/react (Monaco loaded lazily)
  - done: E2E

**M4 exit:** the same strategy file runs in backtest, paper and live (owner's account, 1 lot) with matching behaviour.
- [ ] M4 exit verified (`/milestone-exit`)


## M5 — Portfolio risk analytics

- [ ] **M5.1** Pricing library (Black-76/BSM, IV solver, rupee Greeks) + QuantLib-checked test vectors
  - deps: M4 exit
  - needs: Python (test group only): QuantLib
  - done: 1e-6 vs QuantLib
- [ ] **M5.2** Risk worker: per-scope Greeks/exposures streaming
  - deps: M5.1
  - needs: Python: nats-py (moved from M1.10)
  - done: F-RK-3 refresh ≤ 5 s
- [ ] **M5.3** Scenario grid + named stress (owner picks dates)
  - deps: M5.2
  - needs: nothing new
  - done: Full revaluation tests
- [ ] **M5.4** Historical + parametric VaR, component VaR
  - deps: M5.3
  - needs: nothing new
  - done: Fixture reproduces hand calc
- [ ] **M5.5** Factor model nightly build + exposures
  - deps: M5.4
  - needs: **Data:** Nifty 500 daily bars only (no intraday)
  - done: F-RK-4 ACs
- [ ] **M5.6** Post-trade auto triggers wired to kill switch
  - deps: M5.5
  - needs: nothing new
  - done: Integration test
- [ ] **M5.7** Web: `RISK`, `SCN`, `VAR`, `STR`, `FAC`, `EXP`
  - deps: M5.6
  - needs: nothing new
  - done: E2E

**M5 exit:** desk Greeks, VaR, scenarios and factor exposures refresh within NFR targets on staging; auto triggers proven in integration tests.
- [ ] M5 exit verified (`/milestone-exit`)


## M6 — Knowledge graph & context mapping

- [ ] **M6.1** Graph seed (companies, instruments, sectors, indices + weights, macro nodes)
  - deps: M5 exit
  - needs: Docker image `neo4j:5.26-community` (profile `graph`); Python: neo4j driver. **Data:** index constituent files (small CSVs)
  - done: Counts match instrument master; `make up PROFILES="core md graph" && make migrate-neo4j` works; a `ConstituentSource` serves `GET /indices/{id}/constituents` (weights, sectors, `avg_volume_20d` from recorded daily volumes, near-month future + previous-close OI via `candles.LastOI`), so `MOV`/`HM` show live data (added at M1.11)
- [ ] **M6.2** Feed ingestion (RSS, SEBI, RBI, manual) + dedupe/cluster
  - deps: M6.1
  - needs: Python intel group: feedparser, trafilatura, sentence-transformers + **CPU-only torch wheel**; **model:** BAAI/bge-m3 once into `$MODELS_DIR`
  - done: F-CX-1 ACs
- [ ] **M6.3** Build labelled eval sets (entity resolution, event types, injection)
  - deps: M6.2
  - needs: nothing new (hand-labelled YAML, ≤ 1 MB)
  - done: Sets reviewed by owner
- [ ] **M6.4** Extraction (LLM fast) + schema validation + quarantine
  - deps: M6.3
  - needs: Python: anthropic
  - done: F-CX-2 ACs
- [ ] **M6.5** Entity resolution (alias, trigram, embeddings)
  - deps: M6.4
  - needs: nothing new
  - done: F1 ≥ 0.90
- [ ] **M6.6** HOLDS sync + impact query + context alerts
  - deps: M6.5
  - needs: nothing new
  - done: F-CX-4 ACs (≤ 60 s)
- [ ] **M6.7** `SENSITIVE_TO` estimation job (quant)
  - deps: M6.6
  - needs: nothing new
  - done: Betas stored with t-stats
- [ ] **M6.8** Web: `N`, `CTX` (Cytoscape), `CAL`, alert toasts
  - deps: M6.7
  - needs: npm: cytoscape
  - done: E2E

**M6 exit:** a real news item creates a correct context alert with graph path within 60 s on staging; extraction/resolution evals meet targets.
- [ ] M6 exit verified (`/milestone-exit`)


## M7 — AI copilot

- [ ] **M7.1** MCP server with tool registry, RBAC, summarisers, read-only DB roles
  - deps: M6 exit
  - needs: Python: mcp
  - done: Registry test (exact tool list)
- [ ] **M7.2** Copilot orchestrator (Anthropic SDK tool loop, streaming SSE, traces, budgets)
  - deps: M7.1
  - needs: nothing new
  - done: Unit tests with mocked LLM
- [ ] **M7.3** `draft_order_ticket` → `ui.open_ticket` flow
  - deps: M7.2
  - needs: nothing new
  - done: E2E: draft opens ticket; nothing sent without Ctrl+Enter
- [ ] **M7.4** Golden set (50) + injection suite; nightly eval job
  - deps: M7.3
  - needs: nothing new
  - done: ≥ 85% pass; 100% injection pass
- [ ] **M7.5** Daily briefs
  - deps: M7.4
  - needs: nothing new
  - done: F-AI-2 ACs
- [ ] **M7.6** External MCP via PAT (read-only)
  - deps: M7.5
  - needs: nothing new
  - done: F-AI-3 ACs
- [ ] **M7.7** Web: `AI` panel with tool traces, brief viewer
  - deps: M7.6
  - needs: nothing new
  - done: E2E

**M7 exit:** copilot golden set ≥ 85%, injection suite 100%, and draft tickets never submit without a human.
- [ ] M7 exit verified (`/milestone-exit`)


## M8 — Desk agents

- [ ] **M8.1** Agent tables (migration 000002, applied with the others); agent YAML schema + loader with forbidden-tool validation
  - deps: M7 exit
  - needs: Python: pyyaml (if not already present)
  - done: F-AG-1 load test (`AGENT_TOOL_FORBIDDEN`)
- [ ] **M8.2** Agent runner: NATS triggers, IST schedules (holiday-aware), cooldown/dedupe, budgets, timeouts, audit
  - deps: M8.1
  - needs: Python: apscheduler
  - done: F-AG-1 ACs
- [ ] **M8.3** Agent write tools (`write_agent_note`, `raise_alert`, `search_agent_notes`) + note embeddings + audience scoping
  - deps: M8.2
  - needs: nothing new
  - done: RBAC tests
- [ ] **M8.4** `risk_analyst` (prompt, triggers, evals incl. silent cases)
  - deps: M8.3
  - needs: nothing new
  - done: ≥ 85% eval, ≤ 10% false alerts
- [ ] **M8.5** `macro_analyst` (prompt, triggers, evals)
  - deps: M8.4
  - needs: nothing new
  - done: same
- [ ] **M8.6** `desk_head` (briefs move from F-AI-2 generator; `ask_agent` depth-1 delegation; `@desk` routing)
  - deps: M8.5
  - needs: nothing new
  - done: same + delegation test
- [ ] **M8.7** Web: `DESK` panel, `@agent` mentions in `AI`, note feedback
  - deps: M8.6
  - needs: nothing new
  - done: F-AG-3 ACs
- [ ] **M8.8** Red-team: agents attempt forbidden actions via prompt injection
  - deps: M8.7
  - needs: nothing new
  - done: 100% blocked
- [ ] **M8.9** (stretch) `research_analyst`, `quant_researcher` (DRAFT-only)
  - deps: M8.8
  - needs: nothing new
  - done: F-AG-4 ACs

**M8 exit:** the three MVP agents run for 5 paper-trading days within budget, with owner-reviewed note quality.
- [ ] M8 exit verified (`/milestone-exit`)


## M9 — Hardening & launch

- [ ] **M9.1** Load test: 10 users, 9,000 instruments, 4 accounts, 2 hours replay
  - deps: M8 exit
  - needs: Replay uses recorded staging data; no new downloads
  - done: All NFRs met
- [ ] **M9.2** Chaos: kill engine mid-session, drop broker WS, Postgres restart, NATS restart
  - deps: M9.1
  - needs: nothing new
  - done: Recovery < 30 s; no duplicate orders
- [ ] **M9.3** Security review (OWASP ASVS L2 checklist), dependency audit, secrets scan
  - deps: M9.2
  - needs: Scanners run in CI (gitleaks, osv-scanner), not installed locally
  - done: No high findings open
- [ ] **M9.4** Docs: install guide, operator runbook, user guide per role, disclaimer
  - deps: M9.3
  - needs: nothing new
  - done: Reviewed
- [ ] **M9.5** 2-week paper burn-in with the full desk + 1-week limited live pilot (low limits)
  - deps: M9.4
  - needs: nothing new
  - done: PRD §2 checklist all ticked
- [ ] **M9.6** Public open-source release (license, NOTICE, SECURITY.md, CONTRIBUTING.md)
  - deps: M9.5
  - needs: nothing new
  - done: Tagged v0.1.0

**M9 exit:** every PRD §2 success criterion is ticked and v0.1.0 is tagged.
- [ ] M9 exit verified (`/milestone-exit`)


## Notes / deviations log
- (append here: date — task — what changed vs spec and why)
- 2026-10-28 — M0.1 — pre-commit uses `local`/`system` hooks through `scripts/precommit-tool.sh` instead of upstream hook repos, so pre-commit never downloads its own tool environments (PRD §10.1). gitleaks is enforced in CI (M0.10); locally it runs only once installed.
- 2026-10-28 — M0.2 — done check changed from "fresh Ubuntu 24.04" to the macOS dev machine (owner develops on macOS; no `ubuntu:24.04` image download). The script still supports both OSes; Ubuntu verification moved to M1.12 on the staging VM. Devcontainer not added (task `needs:` only asks for `bootstrap.sh`).
- 2026-10-28 — M0.3 — `make migrate` uses `scripts/migrate-pg.sh` (psql inside the postgres container) instead of the golang-migrate CLI, which is not on M0.3's `needs:` list (it arrives with Go in M0.6). It uses golang-migrate's own `schema_migrations(version, dirty)` table and dirty-flag protocol, so the M0.6 runner takes over without re-applying. `make up` falls back to `.env.example` when `.env` does not exist (dev compose has no secrets).
- 2026-10-28 — M0.3 — **[DECISION NEEDED: audit hash chain]** In `000001_init.up.sql` `audit_chain()`, the `-- session-TZ independent` comment shares a line with `coalesce(NEW.actor_id::text,'') || '|' || NEW.actor_kind || '|' ||`, so actor_id and actor_kind are excluded from the hash (confirmed on the dev DB in a rolled-back transaction). Invariant #6 is weakened: rewriting who did an action is not detected. Recommended: fix in place in 000001 (not yet applied outside this disposable dev DB; owner must lift the settings.json edit deny), then `make down` + drop volume + `make migrate`; otherwise add migration 000003 with `CREATE OR REPLACE FUNCTION audit_chain()`. Must be settled before M0.8 (audit writer + tamper test, which must cover actor fields).
- 2026-10-28 — M0.4 — `bootstrap.sh` gained a `proto` component (buf only, pinned + sha256) so buf installs like the other toolchains; `proto/buf.gen.yaml` split into `buf.gen.{go,python,ts}.yaml` so `make proto` generates only for services that exist; `buf format -w` applied to all protos (comment alignment only, `git diff -w` empty) and `make lint` now checks format; `make lint` requires buf from here on. Remote plugin versions get pinned when each service is scaffolded.
- 2026-10-28 — M0.5 — nanobind comes from an overlay port (`engine/vcpkg-overlays/nanobind`) identical to the official one minus its `python3` dependency, which would have built CPython + OpenSSL/zlib/libffi/sqlite from source (owner chose this option). `robin-map` (header-only) is pulled in transitively. `vyuha_core` builds against `find_package(Python 3.13)`: on the dev Mac the python.org 3.13 framework; override with `-DPython_EXECUTABLE` (uv's 3.13 from M1.1). The Python module is built in debug/release only; asan/tsan presets turn it off because a sanitized extension cannot load into an uninstrumented interpreter. On macOS, clang-tidy 18 cannot parse the macOS 27 SDK's newer libc++, so `make lint` points it at LLVM 18's own libc++ headers (`TIDY_EXTRA`), and clang-tidy is scoped to engine sources so nanobind's compiled-in sources aren't linted. Makefile selects GCC 14 on Linux (Ubuntu 24.04 defaults to 13).
- 2026-10-28 — M0.6 — `make migrate` now runs `vyuhactl migrate up` (golang-migrate library, pgx/v5 driver); `scripts/migrate-pg.sh` removed after confirming the takeover (version 2, not dirty, nothing re-applied). `make gen` added (proto + oapi-codegen + sqlc). sqlc binary installed by `bootstrap.sh go`; oapi-codegen is a `tool` directive in go.mod. `api/openapi.yaml` holds only `/health` + shared `Problem` schema; other endpoints are added contract-first by the task that implements them (API.md said to generate the whole file at M0). Health check added to API.md §0. `go mod tidy` downloads golang-migrate's own test deps (dktest, docker client, lib/pq) into the module cache for go.sum; `go list -deps ./cmd/...` confirms none are linked. Store integration tests run against the dev Postgres when it is up (testcontainers-go isn't on M0.6's `needs:`). **golangci-lint is not on M0.6's `needs:`**: `make lint` falls back to gofmt + go vet until the owner approves adding it.
- 2026-10-28 — M0.7 — Migration 000003 adds `app_user.totp_last_step` (TOTP replay protection, RFC 6238 §5.2 / ASVS 2.8.4) and an index on `refresh_token(family_id)`; if the audit-hash fix goes into a new migration it becomes 000004. Choices where the spec was silent: argon2id m=64 MiB t=2 p=2; password 12–128 chars; TOTP SHA1/6/30 s ±1 step; wrong **and** replayed TOTP codes count towards the 5-failure lockout, as do reauth failures; the counter resets only after a full password + TOTP success; every attempt runs under `SELECT … FOR UPDATE` on the user row so the lockout holds under concurrency; lockout blocks new logins but does not end existing sessions (risk-reviewer P1); `/auth/mfa/enroll` and `/confirm` take the `mfa_token` in the body and confirm returns a session; `/auth/refresh` also returns `user`; every non-public route is default-deny bearer auth. `vyuhactl secrets init` (`make dev-secrets`) creates the JWT key and `VYUHA_MASTER_KEY` (0600, never printed or overwritten); `make dev-gateway` runs the gateway with `.env`. `.env.example` OTLP endpoint now empty until the obs profile (M3.8). Makefile sets `GOTOOLCHAIN=local` after an ad-hoc `go run` with the older system Go auto-downloaded a duplicate 1.27.3 toolchain (243 MB, removed). Known, accepted: audit events for auth actions arrive with the audit writer (M0.8); 423 + `locked_until` reveals that a username exists (mandated by API.md); access JWTs stay valid until expiry (≤ 15 min) after logout; the web client must serialise refresh calls (M0.9). **Open P2 (risk-reviewer), fix before M0.9:** a burst of logins for one known username queues on the row lock while each holds a pooled DB connection, which can starve other requests for seconds; fix with a small attempt semaphore or `SET LOCAL lock_timeout` mapped to 429 (needs a 429 in the contract), together with per-IP rate limiting.
- 2026-10-28 — M0.8 — Audit hash bug fixed by option B (owner continued without choosing; ADR-018): `000004_audit_hash_v2` hashes every column length-prefixed and assigns ids under the chain lock; `000005_audit_read_committed` refuses audit inserts outside READ COMMITTED (snapshot isolation could fork the chain). `vyuhactl audit verify [-anchor id:hash]` recomputes the chain in Go independently of the DB function; `vyuhactl audit head` prints the anchor to keep off-database (a chain alone can't see its newest rows deleted). Auth events (user.create, login success/failure, mfa enroll begin/enrolled, reauth success/failure, refresh reuse, session revoked, logout) are written in the same transaction as the change; routine refresh rotation isn't audited. Unknown-user login failures store no attempted username (risk-reviewer P1: passwords typed into the username box would be kept forever). Payload keys that look like secrets are rejected. Integration tests now use `internal/testdb` (fresh migrated DB per test, force-dropped) — the M0.7 HTTP test had leaked 4 users into the dev DB (removed). `scripts/check-invariants.sh` now also flags `TRUNCATE audit_event`. Dev services still connect as the `vyuha` superuser; INSERT-only is proven for `vyuha_app` via `SET ROLE` — deploy must create login roles in `vyuha_app` (M1.12/M3.8). No `POST /audit/verify` endpoint yet (needs RBAC, M3.7). **Open P2 (risk-reviewer), with the rate-limiting work before M0.9:** unauthenticated unknown-user logins each add an audit row (unbounded growth).
- 2026-10-28 — tooling — Owner approved golangci-lint (2.15.0, pinned + sha256 in `bootstrap.sh go`); `make lint` now requires it. First run: 14 findings fixed (unchecked Close on the JWT key write, errors.Is for ErrAbortHandler, %w wrapping, constant stty args with context, usage text out of error strings) or suppressed with a reason (operator-supplied CLI/config paths; dev-only compose credentials).
- 2026-10-28 — M0.8a — Closes the M0.7/M0.8 P2s. Per-client limits (IPv4 address / IPv6 /64): login, mfa, enroll, confirm 30/min; refresh, logout 30/min; reauth 5/min per signed-in user → 429 `RATE_LIMITED` + `Retry-After`. Anonymous credential checks capped at max(1, pool/4) concurrent with a 2 s wait; reauth has a reserved slot; per-user row lock has a 2 s `lock_timeout`; both give 429 `AUTH_BUSY` without counting a failure. `TRUSTED_PROXIES` (new): defaults to loopback in dev, required in staging/prod ("none" = no proxy) — without it every request behind Caddy shared one bucket (risk-reviewer P1). Accepted: limits are per gateway process (one gateway in MVP); a many-IP attacker can still add unknown-user audit rows at ≤ 30/min per IP; BeginEnroll is rate-limited but outside the check cap.
- 2026-10-28 — M0.9 — (1) Resolved versions were newer majors than the docs assumed: TypeScript 7.0 (native compiler), Vite 8 (Rolldown/Oxc, so no React plugin is needed for JSX). (2) Dockview 8 moved its React components to `dockview-react`, which is not on the approved list: the workspace uses dockview-core's DOM API (`createDockview`) and React portals instead of adding a package. (3) Not installed (owner choice): ESLint, Prettier, Testing Library/jsdom, `@vitejs/plugin-react`, `@types/node`. `make lint` for web = strict `tsc --noEmit`; the prettier pre-commit hook was removed; vitest runs in the node environment (logic only, UI covered by Playwright); config files declare `process` in `web/config.d.ts`. (4) Deferred: `openapi-typescript` (REST types hand-mirrored in `src/lib/api/types.ts`) and TS protobuf codegen (`make proto` runs `buf.gen.ts.yaml` once `@bufbuild/protobuf` arrives at M1.9). (5) `make e2e` = `scripts/e2e.sh`: fresh database, ephemeral JWT key + master key in a temp dir, gateway on 127.0.0.1:18080, Vite on 5174; dropped afterwards; the dev DB and `.env` are never touched. (6) Refresh is serialized per browser (shared promise + Web Lock `vyuha-auth-refresh`) as API.md §1 requires; tokens stay in memory. The enrollment screen shows the QR only (no plain-text seed) — invariant 5. (7) Playwright's `install chromium` also fetched its headless shell (~200 MB, same browser); no other browsers. (8) Not yet: Alt+Enter pop-out, Ctrl+W/Ctrl+Tab, link groups, layout save (later UI tasks).
- 2026-10-28 — M0.10 — Done check "PR checks required on `main`" not met by owner choice: the repo is private on GitHub Free, where branch protection/rulesets are unavailable; `ci-ok` (single aggregate check) is advisory until the repo goes public or the plan changes — then require `ci-ok` on `main`. CI runs on Ubuntu 24.04 only (no macOS job; macOS is the dev machine). quant/intel jobs arrive with their scaffolds (M1.1, M6.x). Fixes found by the first CI run: CMake module scanning disabled (`CMAKE_CXX_SCAN_FOR_MODULES OFF`; GCC scan flags broke clang-tidy), Linux clang-tidy defines `__cpp_concepts=202002L` so clang 18 sees libstdc++ 14's `std::expected`, `internal/testdb` migrates under an advisory lock (parallel packages raced creating the cluster-wide `vyuha_app` role on a fresh Postgres), `.gitleaksignore` for one reviewed fake test key. Makefile gained per-service `lint-*` targets and `ENGINE_PRESETS`.
- 2026-10-28 — M1.1 — (1) D-7 resolved by owner as ADR-019: manual import of NSE's `fo_secban.csv` (the owner first picked an automated fetch, conditional on NSE terms; NSE circular NSE/WEB/45032 prohibits scripted downloads, so the owner switched). New migration `000006_fo_ban_import`. (2) Matching key: exchange + exchange-issued token (both files carry it; verified 100% agreement on terms for 63,133 contracts); indices by name (tokens differ). Undocumented Upstox units verified against Kite: `tick_size` in paise, `expiry` epoch ms at 23:59:59 IST — documented in `upstox.py`. (3) Contracts keep their `instrument.id` while listed (lot/tick/freeze/mapping changes update in place); delisted/expired rows get `valid_to` = sync date (exclusive); same symbol with a different contract identity is closed and re-inserted. (4) Guards: refuse when >0.1% of matched rows conflict or the sync would close >40% of active instruments (`--allow-large-change` after review). (5) Every sync and ban import writes a `system` audit event in the same transaction. (6) Not on the `needs:` line but required by the Makefile/quant rules: pytest, mypy, ruff (dev group). `sqlalchemy` from `needs:` not installed yet (asyncpg COPY fits this job; add when a read model needs it). `exchange_calendars` pulls pandas transitively (not imported by our code). (7) The 08:00 IST schedule (sync) and pre-09:00 `ban check` are CLI jobs today; no scheduler exists yet — wire them as systemd timers at M1.12 (staging) or with the job runner when it lands. (8) XBOM in exchange-calendars 4.13.2 ends 2026-12-31: `holidays seed` raises a warn alert when coverage is < 90 days out; 2027 holidays need a library update or desk rows. (9) `make lint-quant` checks `src` only until `sdk/` has code. CI gained a `quant` job. (10) `.gitignore` `data/` (meant for the repo-root data lake) also hid `quant/src/vyuha_quant/data/` from git and ruff; anchored to `/data/`. `make lint-engine` now configures the debug preset when `compile_commands.json` is missing (after `clean-space`).
- 2026-10-28 — M1.2 — "p99 push/pop" is measured per operation over batches of 128 (a single op is a few ns, below macOS's ~42 ns clock tick), timed only while the ring has room/data, so it is the op cost under cross-core contention, not time waiting for the other side. p999 for 64-byte ticks across cores is 76 ns (pop) / 130 ns (push): cache-line transfer outliers, outside the done check. Slots hold default-constructed `T` and are filled by move-assignment (no placement new): elements must be nothrow default-constructible/move-assignable, which fits POD ticks/intents; popped slots stay moved-from until reused. Benchmarks are not run in CI (shared runners are too noisy for ns thresholds); `make bench` is the gate. `.clang-tidy`: cognitive-complexity ignores macros (GoogleTest assertions). Also fixed an M1.1 slip: `make proto` had started generating Python protobuf code (unpinned remote plugins) into the gitignored `quant/src/vyuha_proto/` as soon as `quant/` existed, though quant has no protobuf runtime; generation now waits for `protobuf` in `quant/uv.lock` (as TS waits for `@bufbuild/protobuf`), and mypy excludes that directory.
- 2026-10-28 — M1.3 — Bar rules where the spec was silent: bars are bucketed by **exchange** timestamp, aligned to the session open, only inside [09:15, 15:30) IST; pre-open/post-close ticks update the last-value book but form no bar; a 15:30:00 tick belongs to no bar; minutes without trades produce no bar (no flat fill); a bar is final when a later bucket's tick arrives or the clock passes its end + 2 s grace; late ticks for an already-closed bar are dropped and counted. Bar volume = change in cumulative day volume since the previous bar, so the 09:15 bar includes pre-open auction volume. Only final bars are emitted (forming bar via `BarBuilder::forming()`); Muhurat/special sessions come later from `exchange_holiday`. Depth is 5 levels in `Tick` (Upstox 30-level arrives with its adapter). `fmt` from `needs:` not added: Quill 13.1 bundles its own fmt. Core-thread CPU pinning (`ENGINE_CORE_CPU`) belongs to the engine app (macOS can't pin). On macOS clang-tidy now passes `-fexperimental-library` (LLVM 18 libc++ keeps `std::jthread`/`stop_token` experimental).
- 2026-10-29 — M1.4 — (1) Fixtures are **synthesized** from the documented layout (`engine/fixtures/vcr/kite/gen_frames.py`), not recorded: no live Kite session is used (invariant 3); replace with recorded, scrubbed frames once a session exists. (2) Byte order isn't in Kite's docs: big-endian per Zerodha's pykiteconnect. (3) libFuzzer + ASan from Homebrew LLVM 18 hangs at startup on this macOS (even an empty target), so `make fuzz-kite` on macOS uses UBSan + libc++ debug hardening (bounds-checked `span`/`array`); a new CI `fuzz` job runs ASan + UBSan on Ubuntu (60 s on PRs, 600 s on main). (4) Quote/LTP packets carry no exchange time: `exchange_ts = recv_ts` for them (bars from quote mode use receive time). (5) The adapter pushes into the core's `TickRing` directly (one IO thread per adapter = one producer) instead of ARCHITECTURE §4's `std::function` `on_tick` callback (hot-path rule). (6) Text frames: `order` postbacks counted for the OMS (M3); `error`/`message` logged by kind only. (7) TLS trust store: vcpkg OpenSSL has none, so the system bundle is loaded (`/etc/ssl/cert.pem` on macOS, `/etc/ssl/certs/ca-certificates.crt` on Ubuntu) or `ca_file`. Live TLS to Kite is not exercised by tests. (8) New: `SecretRef` (mlock + zeroise), `net::check_ip_on_host` (STATIC_IP_NOT_ON_HOST), `Backoff`. The adapter README carries the doc URLs, verified date and limits table. (9) CI found two M1.3 issues macOS hid: a clang-tidy widening warning in the core-loop bench (fixed in M1.4) and an ASan alloc/dealloc mismatch on Linux in the counting-allocator test (GCC's `std::stable_sort` uses nothrow `new`, which the test hadn't replaced); nothrow forms now replaced too.
- 2026-10-29 — M1.5 — (1) **Protobuf is decoded by hand**, not with generated code: `protoc` C++ parsing allocates per frame (map keys become `std::string`s), which breaks the no-allocation IO-decode rule, and would tie the fuzz build to vcpkg. `feed_parser.cc` walks the wire format with string_views. The `protobuf` port from `needs:` is used only by `vyuha_reference_tests`, which builds frames with `protoc`-generated classes for the vendored schema and checks the decoder against them (not built under sanitizers: vcpkg's abseil/protobuf aren't instrumented and abseil's layouts differ under ASan). (2) Upstox doesn't document price/time units: prices are `double` rupees (rounded to the nearest 1/10 000 rupee → `Price`; NaN/inf/negative drops the feed), timestamps epoch ms. (3) Limits are enforced per user across connections (the docs don't say per connection; stricter reading) and one connection is used; `full_d30` only on the Plus plan, best 5 levels kept. (4) Every connection re-authorizes (the URL's `code` is single-use); neither token nor URL is logged. (5) The Kite connection code was refactored into a broker-neutral `FeedClient` (IO thread, source-IP bind, TLS, backoff, gap log) + per-broker `FeedProtocol`; M1.4 tests pass unchanged. (6) UBSan found a signed overflow on corrupted timestamps (ms → ns) via a mutated-frame test; fixed, and fuzz builds now use `-fno-sanitize-recover=all` (the M1.4 macOS fuzz ran with recoverable UBSan; its log has no UB reports). (7) CI fuzz job fixed: clang 18 can't use GCC 14's `<expected>`, so Linux fuzz builds use libc++. (8) Test fixture servers no longer close Asio acceptors from another thread (TSan race in the tests, not the adapters). (9) `make invariants` flagged `double` prices in broker code: the decoder now reads each wire price straight into `Price` via a single audited core helper, `Price::from_wire_rupees` (tested in `price_test.cc`); no floating-point price is held in broker code. The vendored `.proto` moved to `engine/third_party/upstox/`. The 10-minute fuzz numbers above predate this refactor; a 60 s re-fuzz of both parsers after it was clean.
- 2026-10-29 — M1.6 — (1) Subscription changes are applied by **restarting the account's feed**, debounced to once per second: simple and correct, but each change causes a short logged gap (and an Upstox re-authorization). Adding/removing instruments on live connections is a follow-up before M1.9 opens panels at scale. (2) No instrument resolves yet: `InstrumentDirectory` is an interface with a static implementation; the Postgres-backed one arrives with libpqxx in M1.7. (3) "Alerts admin" on STATIC_IP_NOT_ON_HOST is an `ADMIN ALERT` error log line plus the refused session listed as `error` (visible via GetSessions); writing `alert_event` rows needs the gateway/NATS path (M1.7/M1.8). (4) gRPC, protobuf codegen and the engine binary build only without sanitizers (vcpkg's gRPC/abseil aren't instrumented); SessionManager and everything it drives are tested under ASan/TSan. (5) Not yet: egress public-IP check (`IP_ECHO_URL`, optional per ARCHITECTURE §5), mTLS on gRPC (listens on 127.0.0.1 by default), core-thread CPU pinning, using `ops_cap` (OMS, M3). (6) The gRPC request's secret fields are overwritten after being copied into `SecretRef`. (7) gRPC is a 37-minute first build on the M2 (cached afterwards); the CI engine timeout was raised to 150 min for first builds.
- 2026-10-29 — M1.7 — (1) NATS subjects are `md.tick.<instrument_id>` and `md.bar.<interval>.<instrument_id>` (ARCHITECTURE §6 says `md.tick.<exch>.<id>`; ticks don't carry the exchange yet — add it when the gateway needs per-exchange wildcards). (2) The core is the only market-data producer today, so core → publisher is an SPSC ring, not the MPSC ring in ARCHITECTURE §4 (needed once OMS events share the publisher). (3) KV `md_last` is written by publishing to the stream subject `$KV.md_last.<id>` (no ack round trip), at most every 250 ms per instrument. (4) "Alerted if > 0": the recorder logs an error every 10 s while `recorder_dropped_total` grows and exports it on `/metrics`; the Prometheus alert rule lands with the obs profile (M3.8). NATS slow-consumer drops are added to the same counter. (5) **Security fix:** every dev compose port was published on all interfaces (Postgres/NATS/ClickHouse reachable from the LAN with well-known dev credentials since M0.3); all now bind 127.0.0.1. ClickHouse 24.8 pinned by digest; it no longer allows the password-less `default` user over the network, so dev uses `vyuha`/`vyuha` (`.env.example`, `make migrate-ch`). (6) Recorder bug found by its integration test: stopping the recorder cancelled an in-flight insert and counted delivered rows as lost; inserts now run under their own timeout. (7) libpqxx (Postgres instrument directory) and the engine binary are non-sanitizer builds like gRPC; the publisher (cnats, C) runs in every preset. (8) Fixture timestamps `1793190900` were labelled "09:25 IST"; they are 18:05 IST (comments fixed; no test depended on market hours). (9) Not yet: the recorder doesn't write `depth`; no deploy unit/compose service for the recorder or engine (M1.12). (10) CI: PR #7's three engine jobs each built gRPC/abseil/protobuf from source in parallel and ran out of runner disk (~90 min in, nothing cached). A new `engine-deps` job builds the vcpkg manifest once (with `--clean-after-build`, preinstalled SDKs removed) and saves the binary cache; the preset jobs restore it and fail fast on a miss.
- 2026-10-29 — M1.8 — (1) **Owner decision ADR-020:** strict ADR-009 via a per-session `feed_id` the engine stamps on every tick (proto `Tick.feed_id`, `SessionInfo.feed_id`; engine change in this task); 1-minute bars stay per instrument (see ADR-020 consequences). (2) The token exchange runs in the gateway, not the engine, with its HTTP client bound to the account's `static_ip` (gateway and engine share the host). (3) Login state is HMAC-signed with an in-memory key: a gateway restart (or a second gateway instance) invalidates logins in progress. (4) RBAC for broker login: admin, or trader with `can_trade` (API.md said "mapped trader"; view-only mappings and analysts/risk managers can't start a trading session). (5) Account management is CLI-only (`vyuhactl account create|map`); `CRUD /admin/accounts` arrives with the admin UI. A rejected token (client-code mismatch) is discarded but not revoked at the broker. (6) WS channels beyond quote/depth/bar.1m/system return BAD_REQUEST until their tasks; MD_NOT_ENTITLED's message is the PRD text plus `[ids]`. (7) Snapshot-only mode lasts ≥ 5 s and ends at the first flush with an empty queue; frames larger than the cap are dropped. A frame that can't be written in 15 s closes the connection. (8) Feed status comes from polling engine GetSessions every 1 s (engine reports connecting/connected only; RECONNECTING/DOWN appear when the engine is unreachable). Market state uses IST times and `exchange_holiday` (NSE); special sessions (Muhurat) are not modelled. (9) Ticks from a feed id not yet seen are dropped and counted (`UnknownFeed`) until the next poll or the login callback reports the session. (10) Not yet: the Vite dev proxy for `/ws` (M1.9), a `/metrics` endpoint for hub counters (M3.8), mTLS to the engine. (11) Worst-case encoded tick (all fields maximal, incl. `feed_id`) is now tested to fit the 512-byte buffer. (12) Risk review (risk-reviewer checklist, run as a general-purpose agent because the risk-reviewer agent type wasn't registered in this session): BLOCK with 2×P1 → fixed: feed ids could repeat after an engine restart (now randomly seeded per process, tested), and bars were delivered without an entitlement check (now only via the user's own carrying, connected account; tested). P2s fixed: callback detached from the request context with a 30 s budget (tested), 10 s deadline for StartSession/StopSession, instrument cache bounded and unknown ids not cached, idle conflation groups not created for denied subscribes, subscribe/refresh race re-checked under the client lock, cached latest values cleared when an account is unmapped, inactive users lose entitlement (tested), ENGINE_GRPC_ADDR must be loopback until mTLS. API key in the login URL recorded as an accepted exception in ADR-020. Re-review: APPROVE; its P2s fixed too (audit write on its own 5 s deadline, bar test race, bar test for a restarted feed). Not done: a per-user connection cap; a deterministic guard against a (~2⁻³¹) feed-id collision across restarts (`started_at_ns` in SessionInfo, reject older `md_last` ticks).
- 2026-10-29 — M1.9 — (1) Not on any task but needed by the panels: gateway `GET /instruments/search`, `GET /instruments/{id}` and `GET /marketdata/candles` (ClickHouse reader, new `CLICKHOUSE_URL` gateway setting; candles use the hub's entitlement rule, and history is desk-recorded bars like ADR-020's bars). Proto: `FeedStatus.label` (client code for the status bar). (2) Watchlists live in the browser (localStorage, per user and name) until the `watchlist` table gets an API; layouts are still not saved. (3) `TS` lists the trades seen at each 50 ms conflation tick, not every exchange print (the hub conflates by design). (4) `Q` has no circuit band / 52-week range yet (no data source); `CH` has no indicators, drawings or OI overlay yet; `DOM` click-to-ticket waits for `ORD` (M3); `adjusted` candles not implemented. (5) Dockview keeps inactive tabs mounted, so a hidden tab stays subscribed; the 10 s release applies when a panel closes or is replaced. (6) Perf: the first 60 s run had one 59 ms long task; copying two 1,000-key records per frame in the store was the main garbage source, so quotes/depth now update in place with a version counter (then 0 long tasks over repeated runs). The test's 1,000 rows all update via grid transactions; AG Grid renders the rows in view (row virtualisation). (7) Data for the perf test comes from `gateway/cmd/fakefeed` (dev-only Go tool speaking the /ws protocol, started by `scripts/e2e.sh`), not a Node server: `@types/node` isn't an approved dependency. Dev builds read a WebSocket URL override from localStorage (`vyuha.dev.wsUrl`); production builds ignore it. (8) Bugs found by the new tests: ClickHouse aggregation aliased the bucket as `ts`, shadowing the column argMin/argMax order by (wrong open/close); the worker lost a system subscription made before the socket opened. (9) Buf's remote plugins rate-limit unauthenticated use: generated code (gitignored) is now regenerated only when a `.proto` is newer than a stamp written after each successful `buf generate` (`make web-gen`, `scripts/e2e.sh`). (10) The disconnect toast (F-UI-3) is implemented but not exercised end to end (no feed transitions in e2e). (11) CI fix (separate PR #8, waiting for the owner to merge — the merge was denied by the permission policy): engine-deps job built vcpkg with GCC 13 while the preset jobs use GCC 14, so the cache never matched; plus a data race in `TestNATSSource`. The same commits are on this branch. (12) `gtest_discover_tests` timed out (5 s default) listing a freshly linked test binary on a busy Mac; discovery timeout raised to 60 s.
- 2026-10-29 — M1.10 — (1) **Data path (ADR-020):** the gateway gathers latest values under the hub's feed rule (new `ws.Hub.Latest`, shared with WebSocket snapshots; `ws.DataAccounts` is the single "which accounts' feeds may this user see" rule) and sends only those prices to quant, which computes and holds no market data. quant has no protobuf runtime, so it can't read `md_last` itself, and it must not anyway (entitlement). (2) The REST endpoint doesn't register engine interest: the panel subscribes every contract of the expiry (`contract_ids`) plus the underlying/future on the WebSocket (1 s conflation) so the engine carries them, then polls the chain every 1 s while the market is open (15 s otherwise). (3) gateway → quant is loopback HTTP without auth (like the engine gRPC channel until mTLS): `QUANT_URL` must be a loopback http origin; quant binds `QUANT_HTTP_ADDR` (default 127.0.0.1:8090), API docs disabled. When quant is down the chain is still served with `analytics: unavailable` (ARCHITECTURE §8). (4) `RISK_FREE_RATE` 0.055 = RBI repo 5.50 % since the MPC decision of 2026-10-07 (verified 2026-10-29), with `RISK_FREE_RATE_AS_OF` (required whenever the rate is set); the OC footer shows the rate and its date so a stale rate is visible. (5) Other conventions: ATM = strike nearest spot (ties lower); Greeks per unit w.r.t. the model's underlying (forward for Black-76), theta ₹/calendar day, vega ₹/vol point; IV is also NaN (`—`) when the premium carries no time value (deep ITM near expiry: σ not identifiable) or exceeds the no-arbitrage bound. (6) **Owner decision — PCR and max pain on the full expiry:** both use every strike of the expiry with OI (`oi_strikes`), independent of the displayed window (± `strikes`, default 20, max 50); the header and footer say "PCR (all strikes)". (7) **Owner decision — IV source:** the mid when both sides are quoted and the spread is ≤ `IV_MID_MAX_SPREAD` (default 10 %) of the mid; else the LTP if the contract traded within `IV_LTP_MAX_AGE_S` (default 60 s), marked **L** in OC; else no IV (`—`), so wide-spread illiquid strikes don't show fitted-to-stale IVs. Kite quote mode (what chain contracts stream in) carries no last-trade time, so the gateway infers it: the hub records when a feed's cumulative day volume rises between two of its ticks. Conservative by design — unknown after a gateway restart or for values read from `md_last` until the next trade is seen. Owner accepted this for M1.10 (fails safe) and moved it to the engine as **M1.11b** (`Tick.last_trade_ts_ns`, kept in `md_last`), because the gateway sees only conflated data and loses the state on restart. (8) **Owner decision — ΔOI baseline:** OI − last recorded OI before today's 09:15 IST (ClickHouse 1-minute bars, 10-day lookback, cached per IST day); with no recorded baseline (first day, gap) ΔOI is absent and shows `—`, never 0, and the build-up table skips that side. (9) PCR history is in-browser for the session; it moves to ClickHouse when the risk worker lands (M5.2). (10) **Deferred:** `OC` B/S ticket → M2.9 (done check updated); QuantLib cross-check → M5.1 (TODO in `tests/test_pricing.py`); `nats-py` → M5.2 (needs lines moved); no deploy unit for `vyuha-quant serve` yet (M1.12, with the engine's). (11) Tests: the e2e user has no data account, so the real gateway path ends at 403 MD_NOT_ENTITLED; the populated chain is rendered from a stubbed `/optionchain` response (no engine in e2e). Chain SQL runs against Postgres (`TestChainQueries`), the ΔOI query against ClickHouse (`TestLastOIIsPreviousSessionClose`). (12) Chart colours for calls/puts are tokens `--series-ce`/`--series-pe` (not up/down, which mean price direction), validated for the dark panel (lightness band, CVD ΔE 23.5, contrast ≥ 3:1). (13) Tooling: `.nvmrc` pins Node 22.23.4 (same as `engines.node`); `bootstrap.sh web` warns when the shell's own node isn't that version (make targets put the bootstrap toolchain first; a plain `pnpm` uses the shell's). The temporary uv cache used during install (`~/.cache/uv` wasn't writable from the agent sandbox) was deleted; `make clean-space` ran, but its `uv cache prune` of `~/.cache/uv` failed on the same permission (owner can run it from a normal shell).
- 2026-10-29 — M1.11 — (1) **Owner decision — constituents:** index membership, weights and sectors exist nowhere yet (no Postgres table; `instrument.sector` is never filled — the broker files carry none; the graph seed with NSE constituent CSVs is M6.1, manual import per ADR-019). So M1.11 defines the contract and builds the panels against it; the gateway answers 404 `NO_CONSTITUENTS` (no source configured) and the panels say so. M6.1's done check now includes wiring a `ConstituentSource`. (2) Live values come from the existing quote stream (constituents + near-month futures subscribed, 250 ms conflation); `useQuoteSnapshot` samples the md store once a second so MOV/HM re-render at most once a second (UI rule: no per-tick React state). (3) Definitions: gainers/losers by `change_pct` (> 0 / < 0), volume shocker = today's volume ÷ `avg_volume_20d`, OI gainer = near-month future OI change vs `future_prev_oi` (> 0); each list says why it's empty (no quotes yet / no baseline). Sector % = weight-averaged change of priced members (equal weights when the data has none; tiles are then equal-sized and the legend says so). Heatmap colours: diverging `--down` → neutral `--border` → `--up`, clamped at ±3 %; tile ink black/white by luminance. (4) The e2e streams quotes from `gateway/cmd/fakefeed` (dev WebSocket override, as the perf test does) with stubbed constituents; the fake feed never prices below the previous close, so Losers is empty there (covered by unit tests, as are OI gainers — the fake feed sends no OI). (5) Reference data, so no market-data entitlement check on the constituents endpoint; quotes keep the hub's rules. (6) M1.11b (engine last-trade time) added before M1.12 at the owner's request.
- 2026-10-30 — M1.11b — (1) `Tick.last_trade_ts_ns` = 27 (additive; `buf breaking` against main passes). Engine `Tick::last_trade_ts`: Upstox from `ltt` (every mode), Kite from full-mode bytes 44–48; otherwise `md::TradeClock` in the core loop infers it when *that feed's* cumulative day volume rises (fixed-capacity map keyed by (feed, instrument), 2 × `max_instruments`; ADR-020 feeds never compare with each other), carries the last time forward on every later tick, and re-bases on a volume drop (next session, reconnect) without inventing a trade. Broker times win. The value rides in `md_last`, so it survives gateway restarts; the hub now reads it instead of inferring (M1.10's per-gateway inference removed). (2) Done check: Kite/Upstox parser tests, 6 `TradeClock` unit tests, a core-loop stamping test, encoder round-trip incl. the worst-case 512-byte tick; `TestOptionChainLTPIVSurvivesGatewayRestart` (real NATS KV, fresh hub) passes with a fake quant and, with `VYUHA_TEST_QUANT_URL`, against a running `vyuha-quant serve` (CE IV from LTP after restart; PE without a known trade has none). (3) Verified locally only (owner: no uploads): engine debug 80/80 and ASan+UBSan 70/70, `make lint-engine`, 60 s fuzz of both parsers (~37M / 17M runs, clean), gateway 14/14 with integrations, quant 66, web 50, e2e 6/6 (60 fps), invariants. Linux/GCC (CI) not run. (4) **Cost:** `BM_CorePollTicks` 18.0M → 16.6M ticks/s at 100 instruments (−8 %), 13.8M → 7.4M at 5,000 (−46 %, ≈ 73 → 134 ns/tick): the extra map is a cache miss at large instrument counts. Far inside the 50 µs p99 normalize budget; if it matters later, keep the per-feed trade state in the last-value book's entry (already in cache) instead of a separate map. (5) Fake feed and fixtures unchanged (the Kite fixture already carried a last-trade time at offset 44).
