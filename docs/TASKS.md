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
8. Tasks marked 🧠 are hand-written by the owner. Claude Code only writes the interface, tests and benchmark, then stops.

Legend: `deps` = tasks that must be `[x]` first · `needs` = the **only** things you may install or download for this task · `done` = completion check.

Current milestone: **M0**


## M0 — Foundations

- [x] **M0.1** Monorepo skeleton per PRD §9; `Makefile` targets; `.editorconfig`; pre-commit (gitleaks, clang-format, ruff, gofmt, prettier) — 2026-10-28: done; skeleton dirs per PRD §9 (.gitkeep), .editorconfig, pre-commit via local system hooks (skip until each tool is installed), `make lint` skips buf until M0.4
  - deps: —
  - needs: nothing (text files only; write `.pre-commit-config.yaml` but don't install hooks until the tools arrive in their own tasks)
  - done: `make invariants` green; `make test` skips cleanly for unscaffolded services
- [ ] **M0.2** Pin toolchains & versions (fill `TECH_STACK.md §6`); devcontainer/VM setup script
  - deps: M0.1
  - needs: nothing yet. Write `scripts/bootstrap.sh <component>` (core|cpp|go|py|web) but run only `core` (git, jq, docker present?)
  - done: `make bootstrap C=core` on fresh Ubuntu 24.04 succeeds and installs nothing beyond the core list
- [ ] **M0.3** `deploy/docker-compose.dev.yml` with profiles: `core` (Postgres, NATS) now; `md` (ClickHouse), `graph` (Neo4j), `obs` (Grafana/Prometheus/Loki/OTel) started only at their milestones
  - deps: M0.2
  - needs: Docker images: `pgvector/pgvector:pg17`, `nats:2.10-alpine` only (profile `core`)
  - done: `make up` (core) healthy; `make migrate` applies Postgres DDL
- [ ] **M0.4** Buf setup; CI `buf lint` + `buf breaking`; codegen wired into each service when it is scaffolded (M0.5 C++, M0.6 Go, M0.9 TS, M1.1 Python)
  - deps: M0.3
  - needs: `buf` CLI (single binary). No language plugins until each service exists (remote plugins, nothing installed locally)
  - done: `buf lint` green; each later scaffold task proves its generated code compiles
- [ ] **M0.5** Engine CMake skeleton (presets: debug, release, asan, tsan), vcpkg manifest, GoogleTest/Benchmark, nanobind hello module
  - deps: M0.4
  - needs: `bootstrap.sh cpp`: compiler, CMake, Ninja, vcpkg (shallow clone). vcpkg ports: `gtest`, `benchmark`, `nanobind` only
  - done: `make test-engine` green; `import vyuha_core` works
- [ ] **M0.6** Gateway skeleton: config, health, OpenAPI codegen, pgx/sqlc, migrations runner, structured logging + OTel
  - deps: M0.5
  - needs: `bootstrap.sh go`: Go toolchain. Modules: pgx, sqlc (binary), oapi-codegen, golang-migrate, OTel SDK
  - done: `GET /api/v1/health` 200
- [ ] **M0.7** Auth: users, argon2id, TOTP enroll/verify, JWT EdDSA, refresh rotation + reuse detection, lockout, `vyuhactl user create`
  - deps: M0.6
  - needs: Go modules: argon2id, pquerna/otp, golang-jwt
  - done: Auth test suite (incl. reuse detection) green
- [ ] **M0.8** Audit writer + `audit verify` CLI
  - deps: M0.7
  - needs: nothing new
  - done: Tamper test detects modified row
- [ ] **M0.9** Web skeleton: Vite/React/TS strict, design tokens, top bar, status bar, Dockview workspace, command bar with function registry (stub panels), login + MFA screens
  - deps: M0.8
  - needs: `bootstrap.sh web`: Node LTS + pnpm. npm: react, react-dom, vite, typescript, dockview, cmdk, zustand, @tanstack/react-query, tailwindcss, tinykeys, vitest, @playwright/test (+ Chromium only: `playwright install chromium`)
  - done: Playwright: login → workspace → `HELP` opens via keyboard only
- [ ] **M0.10** CI (GitHub Actions): matrix build/test for all services, sanitizers job, artifact cache
  - deps: M0.9
  - needs: nothing locally (CI caches keyed by lockfiles)
  - done: PR checks required on `main`

**M0 exit:** login with TOTP works end-to-end in the browser; CI green; DB migrations apply cleanly.
- [ ] M0 exit verified (`/milestone-exit`)


## M1 — Instruments & market data

- [ ] **M1.1** Instrument master sync (quant job): Kite + Upstox files → unified `instrument` with mapping, freeze qty, lot size; holiday calendar seed
  - deps: M0 exit
  - needs: `bootstrap.sh py`: Python + uv. quant group `core`: polars, httpx, pydantic, sqlalchemy, asyncpg, exchange_calendars. Broker instrument files are fetched at runtime (not committed)
  - done: F-MD-1 ACs
- [ ] **M1.2** 🧠 `SpscRing<T,N>` + benchmarks
  - deps: M1.1
  - needs: nothing new
  - done: p99 push/pop < 50 ns; TSan clean
- [ ] **M1.3** Engine core loop, canonical `Tick`, `Price` int64 type, LastValue book, bar builder
  - deps: M1.2
  - needs: vcpkg: `quill`, `fmt`
  - done: Unit tests incl. bar boundary at 09:15/15:30 IST
- [ ] **M1.4** Kite Ticker adapter (binary parser, sharding ≤ 3,000/conn, ≤ 3 conns/key, reconnect) with recorded-frame fixtures
  - deps: M1.3
  - needs: vcpkg: `boost-asio`, `boost-beast`, `openssl`, `simdjson`; libFuzzer comes with Clang
  - done: Parser fuzz test (libFuzzer) 10 min clean; F-MD-2 ACs (fixture-based)
- [ ] **M1.5** Upstox Market Data Feed V3 adapter (authorize → WSS, protobuf decode, modes, limits table)
  - deps: M1.4
  - needs: vcpkg: `protobuf`
  - done: Same as above
- [ ] **M1.6** Per-account source-IP binding + preflight; session manager; gRPC `StartSession/GetSessions/Subscribe`
  - deps: M1.5
  - needs: vcpkg: `grpc`
  - done: `STATIC_IP_NOT_ON_HOST` test
- [ ] **M1.7** NATS publisher + KV `md_last`; recorder → ClickHouse
  - deps: M1.6
  - needs: vcpkg: `cnats`, `libpqxx`; Docker image `clickhouse/clickhouse-server:24.8` (profile `md`); Go: clickhouse-go, nats.go
  - done: F-MD-4 ACs; `make up PROFILES="core md" && make migrate-ch` works
- [ ] **M1.8** Gateway: broker login URL/callback flows, encrypted secrets, WS hub with entitlement + conflation
  - deps: M1.7
  - needs: Go: coder/websocket, grpc
  - done: Entitlement test; slow-consumer test
- [ ] **M1.9** Web: WS worker, stores, `Q`, `WL`, `CH`, `DOM`, `TS`, status bar feed indicators
  - deps: M1.8
  - needs: npm: lightweight-charts, ag-grid-community, ag-grid-react, @bufbuild/protobuf
  - done: Perf test (F-MD-2)
- [ ] **M1.10** Option chain service (quant: IV/Greeks, PCR, max pain) + `OC`, `OI` panels
  - deps: M1.9
  - needs: Python: numpy, scipy, nats-py, fastapi, uvicorn; npm: echarts
  - done: F-MD-3 ACs
- [ ] **M1.11** `MOV`, `HM` panels
  - deps: M1.10
  - needs: nothing new
  - done: Render with fixture data
- [ ] **M1.12** **Staging:** VM with 2 static IPs, real Kite + Upstox sessions streaming Nifty 50 + NIFTY options
  - deps: M1.11
  - needs: Staging VM only; record only Nifty 50 + NIFTY options near expiries
  - done: Tick→screen p95 < 400 ms measured

**M1 exit:** live data from both brokers on staging; benchmarks meet NFRs.
- [ ] M1 exit verified (`/milestone-exit`)


## M2 — Paper OMS & risk gate

- [ ] **M2.1** OMS state machine + `order_event` writer thread + recovery from Postgres
  - deps: M1 exit
  - needs: vcpkg: `rapidcheck`
  - done: Property tests: only legal transitions
- [ ] **M2.2** 🧠 RiskGate framework (`ApprovedOrder` construct-only-by-gate, rule table, GateContext)
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
  - done: Playwright: keyboard-only paper trade flow

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
  - needs: nothing new
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
  - done: Counts match instrument master; `make up PROFILES="core md graph" && make migrate-neo4j` works
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
