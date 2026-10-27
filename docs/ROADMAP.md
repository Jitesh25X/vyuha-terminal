# Roadmap (build order for Claude Code)

Rules:
- Work through milestones **in order**. A milestone is done only when every exit criterion passes in CI (or on the staging VM where noted) and its exit box in `docs/TASKS.md` is ticked.
- Each task is sized to fit one Claude Code session (≈ one PR). IDs `M<milestone>.<n>`.
- Build mode (ADR-017): **Claude Code writes all code**; the owner reviews. (The former 🧠 hand-written markers were removed.) Previously: The owner writes it, and Claude Code reviews it and writes the tests.
- Effort is in *focused days* and assumes Claude Code writes most code while the owner reviews. Scale it to your weekly hours.
- **Execution queue:** `docs/TASKS.md` holds every task below in order, with its dependencies, its `Needs:` download allow-list and its done check. `/next-task` executes it one task at a time.
- **Lean footprint:** install and download only what the current task's `Needs:` lists (PRD §10.1).

---

## M0 — Foundations (≈ 6–8 days)
| ID | Task | Done when |
|---|---|---|
| M0.1 | Monorepo skeleton per PRD §9; `Makefile` targets; `.editorconfig`; pre-commit (gitleaks, clang-format, ruff, gofmt, prettier) | `make invariants` green; `make test` skips cleanly for unscaffolded services |
| M0.2 | Pin toolchains & versions (fill `TECH_STACK.md §6`); devcontainer/VM setup script | `make bootstrap C=core` on fresh Ubuntu 24.04 succeeds and installs nothing beyond the core list |
| M0.3 | `deploy/docker-compose.dev.yml` with profiles: `core` (Postgres, NATS) now; `md` (ClickHouse), `graph` (Neo4j), `obs` (Grafana/Prometheus/Loki/OTel) started only at their milestones | `make up` (core) healthy; `make migrate` applies Postgres DDL |
| M0.4 | Buf setup; CI `buf lint` + `buf breaking`; codegen wired into each service when it is scaffolded (M0.5 C++, M0.6 Go, M0.9 TS, M1.1 Python) | `buf lint` green; each later scaffold task proves its generated code compiles |
| M0.5 | Engine CMake skeleton (presets: debug, release, asan, tsan), vcpkg manifest, GoogleTest/Benchmark, nanobind hello module | `make test-engine` green; `import vyuha_core` works |
| M0.6 | Gateway skeleton: config, health, OpenAPI codegen, pgx/sqlc, migrations runner, structured logging + OTel | `GET /api/v1/health` 200 |
| M0.7 | Auth: users, argon2id, TOTP enroll/verify, JWT EdDSA, refresh rotation + reuse detection, lockout, `vyuhactl user create` | Auth test suite (incl. reuse detection) green |
| M0.8 | Audit writer + `audit verify` CLI | Tamper test detects modified row |
| M0.9 | Web skeleton: Vite/React/TS strict, design tokens, top bar, status bar, Dockview workspace, command bar with function registry (stub panels), login + MFA screens | Playwright: login → workspace → `HELP` opens via keyboard only |
| M0.10 | CI (GitHub Actions): matrix build/test for all services, sanitizers job, artifact cache | PR checks required on `main` |

**Exit:** login with TOTP works end-to-end in the browser; CI green; DB migrations apply cleanly.

## M1 — Instruments & market data (≈ 12–15 days)
| ID | Task | Done when |
|---|---|---|
| M1.1 | Instrument master sync (quant job): Kite + Upstox files → unified `instrument` with mapping, freeze qty, lot size; holiday calendar seed | F-MD-1 ACs |
| M1.2 | `SpscRing<T,N>` + benchmarks | p99 push/pop < 50 ns; TSan clean |
| M1.3 | Engine core loop, canonical `Tick`, `Price` int64 type, LastValue book, bar builder | Unit tests incl. bar boundary at 09:15/15:30 IST |
| M1.4 | Kite Ticker adapter (binary parser, sharding ≤ 3,000/conn, ≤ 3 conns/key, reconnect) with recorded-frame fixtures | Parser fuzz test (libFuzzer) 10 min clean; F-MD-2 ACs (fixture-based) |
| M1.5 | Upstox Market Data Feed V3 adapter (authorize → WSS, protobuf decode, modes, limits table) | Same as above |
| M1.6 | Per-account source-IP binding + preflight; session manager; gRPC `StartSession/GetSessions/Subscribe` | `STATIC_IP_NOT_ON_HOST` test |
| M1.7 | NATS publisher + KV `md_last`; recorder → ClickHouse | F-MD-4 ACs |
| M1.8 | Gateway: broker login URL/callback flows, encrypted secrets, WS hub with entitlement + conflation | Entitlement test; slow-consumer test |
| M1.9 | Web: WS worker, stores, `Q`, `WL`, `CH`, `DOM`, `TS`, status bar feed indicators | Perf test (F-MD-2) |
| M1.10 | Option chain service (quant: IV/Greeks, PCR, max pain) + `OC`, `OI` panels | F-MD-3 ACs |
| M1.11 | `MOV`, `HM` panels | Render with fixture data |
| M1.12 | **Staging:** VM with 2 static IPs, real Kite + Upstox sessions streaming Nifty 50 + NIFTY options | Tick→screen p95 < 400 ms measured |

**Exit:** live data from both brokers on staging; benchmarks meet NFRs.

## M2 — Paper OMS & risk gate (≈ 12–15 days)
| ID | Task | Done when |
|---|---|---|
| M2.1 | OMS state machine + `order_event` writer thread + recovery from Postgres | Property tests: only legal transitions |
| M2.2 | RiskGate framework (`ApprovedOrder` construct-only-by-gate, rule table, GateContext) | Compile-time test that adapters can't receive raw intents |
| M2.3 | Rules R-KILL…R-ORDER-SHAPE, R-FAT-FINGER, R-ORDER-VALUE, R-DUP, R-OPS | Per-rule unit tests; bench p99 < 20 µs |
| M2.4 | Rules R-OPEN-ORDERS…R-DISCIPLINE with incremental scope aggregation | Property tests (RISK_ENGINE §3) |
| M2.5 | SimBroker (paper) on live ticks; brackets/OCO; freeze-qty slicer | F-EX-4 ACs |
| M2.6 | Position/MTM projector + costs (temporary flat schedule until M4) | P&L unit tests |
| M2.7 | Kill switch (scopes, actions, auto triggers) + gRPC | F-RK-2 ACs (paper) |
| M2.8 | Gateway: orders/positions/pnl/limits/kill APIs + WS channels | API tests |
| M2.9 | Web: `ORD` ticket with live preview, `BLT`, `POS`, `PNL`, `LIM`, `KILL`, hotkeys | Playwright: keyboard-only paper trade flow |

**Exit:** a full paper trading day on staging with ≥ 50 orders, zero illegal transitions, and every gate rule exercised by tests.

## M3 — Live execution (≈ 10–12 days)
| ID | Task | Done when |
|---|---|---|
| M3.1 | Kite order adapter (place/modify/cancel, tags, market protection, order-update stream, positions/margins) with VCR fixtures | Contract tests |
| M3.2 | Upstox order adapter (same) + Portfolio Stream Feed | Contract tests |
| M3.3 | OPS token bucket per account per exchange; reject storm detection | Burst test |
| M3.4 | Reconciliation (60 s, on reconnect, EOD) + `UNKNOWN` resolution | Fault-injection tests (timeouts, dup updates, out-of-order) |
| M3.5 | Margin cache + broker margin preview for large orders | Unit tests |
| M3.6 | `LIVE_TRADING_ENABLED` guard, live/paper UI distinctions, re-auth flows | E2E |
| M3.7 | Compliance report `COMP`; `AUD` viewer | F-CO-1/2 ACs |
| M3.8 | Backups + restore drill; Grafana dashboards; alerting (engine down, feed down, recon mismatch) | Restore drill documented |
| M3.9 | **Staging live pilot (owner's own account only)**: 1-lot orders, all brackets, kill switch drill | PRD §2 live-order criterion passes; owner sign-off |

**Exit:** owner's account trades live through Vyuha for 5 sessions with no recon mismatches. The legal question (PRD §13 #2) must be answered before any other person's account is connected.

## M4 — Data, strategies & backtesting (≈ 12–15 days)
| ID | Task | Done when |
|---|---|---|
| M4.1 | Download manager (Kite + Upstox historical, expired options), Parquet lake, dataset catalogue, `DL` panel | F-RS-1 ACs |
| M4.2 | Cost schedule seeding (verified, with sources) + cost engine + contract-note tests | 5 contract-note tests pass to ₹0.01 |
| M4.3 | nanobind bindings for core (clock, SimBroker, gate, OMS) | Python tests drive core deterministically |
| M4.4 | Strategy SDK + indicators + sandboxed strategy host | F-RS-2 ACs |
| M4.5 | Backtest engine (fill models, sessions, expiry handling, look-ahead guards) | Golden tests for 3 reference strategies |
| M4.6 | Metrics + report + artifacts; sweeps; walk-forward | F-RS-3 ACs |
| M4.7 | Strategy lifecycle + approval; paper/live strategy hosting via engine | F-RS-4 ACs |
| M4.8 | Web: `STRAT` (Monaco), `BT` report, `PAY` payoff | E2E |

**Exit:** the same strategy file runs in backtest, paper and live (owner's account, 1 lot) with matching behaviour.

## M5 — Portfolio risk analytics (≈ 10–12 days)
| ID | Task | Done when |
|---|---|---|
| M5.1 | Pricing library (Black-76/BSM, IV solver, rupee Greeks) + QuantLib-checked test vectors | 1e-6 vs QuantLib |
| M5.2 | Risk worker: per-scope Greeks/exposures streaming | F-RK-3 refresh ≤ 5 s |
| M5.3 | Scenario grid + named stress (owner picks dates) | Full revaluation tests |
| M5.4 | Historical + parametric VaR, component VaR | Fixture reproduces hand calc |
| M5.5 | Factor model nightly build + exposures | F-RK-4 ACs |
| M5.6 | Post-trade auto triggers wired to kill switch | Integration test |
| M5.7 | Web: `RISK`, `SCN`, `VAR`, `STR`, `FAC`, `EXP` | E2E |

**Exit:** Desk Greeks, VaR, scenarios and factor exposures refresh within NFR targets on staging; auto triggers proven in integration tests.

## M6 — Knowledge graph & context mapping (≈ 12–15 days)
| ID | Task | Done when |
|---|---|---|
| M6.1 | Graph seed (companies, instruments, sectors, indices + weights, macro nodes) | Counts match instrument master |
| M6.2 | Feed ingestion (RSS, SEBI, RBI, manual) + dedupe/cluster | F-CX-1 ACs |
| M6.3 | Build labelled eval sets (entity resolution, event types, injection) | Sets reviewed by owner |
| M6.4 | Extraction (LLM fast) + schema validation + quarantine | F-CX-2 ACs |
| M6.5 | Entity resolution (alias, trigram, embeddings) | F1 ≥ 0.90 |
| M6.6 | HOLDS sync + impact query + context alerts | F-CX-4 ACs (≤ 60 s) |
| M6.7 | `SENSITIVE_TO` estimation job (quant) | Betas stored with t-stats |
| M6.8 | Web: `N`, `CTX` (Cytoscape), `CAL`, alert toasts | E2E |

**Exit:** A real news item creates a correct context alert with graph path within 60 s on staging; extraction/resolution evals meet targets.

## M7 — AI copilot (≈ 10–12 days)
| ID | Task | Done when |
|---|---|---|
| M7.1 | MCP server with tool registry, RBAC, summarisers, read-only DB roles | Registry test (exact tool list) |
| M7.2 | Copilot orchestrator (Anthropic SDK tool loop, streaming SSE, traces, budgets) | Unit tests with mocked LLM |
| M7.3 | `draft_order_ticket` → `ui.open_ticket` flow | E2E: draft opens ticket; nothing sent without Ctrl+Enter |
| M7.4 | Golden set (50) + injection suite; nightly eval job | ≥ 85% pass; 100% injection pass |
| M7.5 | Daily briefs | F-AI-2 ACs |
| M7.6 | External MCP via PAT (read-only) | F-AI-3 ACs |
| M7.7 | Web: `AI` panel with tool traces, brief viewer | E2E |

**Exit:** Copilot golden set ≥ 85%, injection suite 100%, and draft tickets never submit without a human.

## M8 — Desk agents (≈ 8–10 days)
| ID | Task | Done when |
|---|---|---|
| M8.1 | Agent tables (migration 000002, applied with the others); agent YAML schema + loader with forbidden-tool validation | F-AG-1 load test (`AGENT_TOOL_FORBIDDEN`) |
| M8.2 | Agent runner: NATS triggers, IST schedules (holiday-aware), cooldown/dedupe, budgets, timeouts, audit | F-AG-1 ACs |
| M8.3 | Agent write tools (`write_agent_note`, `raise_alert`, `search_agent_notes`) + note embeddings + audience scoping | RBAC tests |
| M8.4 | `risk_analyst` (prompt, triggers, evals incl. silent cases) | ≥ 85% eval, ≤ 10% false alerts |
| M8.5 | `macro_analyst` (prompt, triggers, evals) | same |
| M8.6 | `desk_head` (briefs move from F-AI-2 generator; `ask_agent` depth-1 delegation; `@desk` routing) | same + delegation test |
| M8.7 | Web: `DESK` panel, `@agent` mentions in `AI`, note feedback | F-AG-3 ACs |
| M8.8 | Red-team: agents attempt forbidden actions via prompt injection | 100% blocked |
| M8.9 | (stretch) `research_analyst`, `quant_researcher` (DRAFT-only) | F-AG-4 ACs |

**Exit:** the three MVP agents run for 5 paper-trading days within budget, with owner-reviewed note quality.

## M9 — Hardening & launch (≈ 8–10 days)
| ID | Task | Done when |
|---|---|---|
| M9.1 | Load test: 10 users, 9,000 instruments, 4 accounts, 2 hours replay | All NFRs met |
| M9.2 | Chaos: kill engine mid-session, drop broker WS, Postgres restart, NATS restart | Recovery < 30 s; no duplicate orders |
| M9.3 | Security review (OWASP ASVS L2 checklist), dependency audit, secrets scan | No high findings open |
| M9.4 | Docs: install guide, operator runbook, user guide per role, disclaimer | Reviewed |
| M9.5 | 2-week paper burn-in with the full desk + 1-week limited live pilot (low limits) | PRD §2 checklist all ticked |
| M9.6 | Public open-source release (license, NOTICE, SECURITY.md, CONTRIBUTING.md) | Tagged v0.1.0 |

**Exit:** Every PRD §2 success criterion is ticked and v0.1.0 is tagged.

---

## Post-MVP backlog (ordered)
0. Remaining desk agents: Execution Assistant, Compliance Officer, Ops Reconciler, Trading Coach (`DESK_AGENTS.md`)
1. Electron desktop wrapper (multi-monitor native windows)
2. More brokers (Dhan, Fyers, Angel One, Shoonya) via `IBrokerAdapter`
3. Licensed vendor feed adapter (desk-wide data)
4. ML signal pipeline (features in Polars, models → ONNX → strategy inference), MLflow registry
5. MCX commodities and currency derivatives
6. Monte Carlo VaR; fundamentals-based factors
7. Supply-chain/competitor edges from annual reports
8. Exchange registration workflow for > 10 OPS strategies
9. Multi-desk tenancy + OIDC SSO
