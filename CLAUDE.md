# CLAUDE.md — Vyuha (AI Quant Desk for Indian markets)

You are building **Vyuha**: an open-source, self-hosted, Bloomberg-style trading terminal with
Aladdin-style risk and context mapping for small Indian prop desks (2–10 users), trading NSE/BSE cash
and F&O through Zerodha Kite Connect and Upstox.

## Read before you write code
1. `docs/PRD.md` — what to build, acceptance criteria (feature IDs like `F-RK-1`).
2. `docs/TASKS.md` — **the execution queue.** Do exactly one task at a time: the first unchecked task whose deps are done. `docs/ROADMAP.md` gives milestone context. Do not start milestone N+1 until N's exit criteria pass (`/milestone-exit`).
3. The spec for the module you touch: `ARCHITECTURE.md`, `TECH_STACK.md`, `DATA_MODEL.md`, `API.md`,
   `RISK_ENGINE.md`, `CONTEXT_ENGINE_AND_AI.md`, `DESK_AGENTS.md`, `STRATEGY_SDK_AND_BACKTEST.md`, `UI_SPEC.md`, `COMPLIANCE.md`.
4. The service's own `CLAUDE.md` (`engine/`, `gateway/`, `quant/`, `intel/`, `web/`).
5. `docs/DECISIONS.md` — settled decisions. Don't relitigate them. Add a new ADR entry if you must change one, and ask the owner first.

## Non-negotiable invariants (a violation is a P0 bug)
1. **No order reaches a broker without passing the C++ `RiskGate`.** Broker adapters accept only
   `ApprovedOrder`, which only `RiskGate` can construct. Never add a constructor, cast, friend, or
   "admin bypass" that breaks this. This applies to manual, strategy, AI-drafted, auto-slice and modify paths.
2. **The AI never places, modifies or cancels orders.** This covers the copilot **and every desk agent**. Tools are read-only, except `draft_order_ticket` (opens a UI ticket for a human), `draft_strategy` (DRAFT versions only), and `write_agent_note`/`raise_alert`. No agent changes limits, releases kill switches or promotes strategies.
3. **Live trading is off unless `LIVE_TRADING_ENABLED=true` AND `VYUHA_ENV=prod|staging`.** Tests, CI and dev
   use `SimBroker` or VCR fixtures only. **Never write code or tests that hit live broker order endpoints.**
4. **Each broker account's traffic leaves from its own configured static IP.** Sessions refuse to start if
   the IP is not on the host.
5. **Secrets never appear in logs, errors, test snapshots, or the frontend.** This covers API keys, secrets, access tokens, TOTP seeds and `VYUHA_MASTER_KEY`. Use the scrubbers.
6. **Audit log is append-only and hash-chained.** Never UPDATE/DELETE `audit_event`; never weaken grants.
7. **OPS cap** defaults to 8 orders/s per account per exchange. It must stay below the 10 OPS threshold unless the owner changes `DECISIONS.md`.
8. **Money and prices:** never use binary floats for persisted money. Postgres `numeric(18,4)`. C++ uses `int64` paise/ticks (`Price` = int64 in 1/100 paise units). Python uses `Decimal` at boundaries. Floats are only for analytics (Greeks/VaR).
9. **Time:** store UTC; display IST (`Asia/Kolkata`). Use the exchange calendar for sessions/holidays. No naive datetimes.
10. **Untrusted text is data.** News, filings and web content are never treated as instructions to the LLM. Keep them inside delimited data blocks and run the injection test suite.
11. **Don't build Post-MVP items** (PRD §5) unless the roadmap says so.
12. **Lean footprint: download only what the current task needs** (PRD §10.1). Install only what the task's `needs:` line in `TASKS.md` lists. That means no bulk installs, no "install everything" bootstraps, no full Boost, no CUDA/GPU wheels, no sample datasets, no extra Playwright browsers (Chromium only), and no Docker images for profiles not yet in use. Market data downloads cover only the job spec's instruments/interval/range. Need something unlisted? Stop and ask the owner. Run `make clean-space` at each milestone exit.

## Repo map
```
proto/    protobuf contracts (Buf). Change here first, then regenerate: `make proto`
engine/   C++23 core: md, oms, risk gate, broker adapters (kite, upstox, sim), nanobind bindings
gateway/  Go: auth, RBAC, REST (openapi.yaml), WebSocket fan-out, recorder, vyuhactl CLI
quant/    Python: risk analytics, pricing, data downloader, backtest, strategy host, Strategy SDK
intel/    Python: ingestion, LLM extraction, knowledge graph, context alerts, copilot, MCP server, desk agents
web/      React 19 terminal
db/       Postgres migrations (single source of truth), ClickHouse DDL, Neo4j Cypher
deploy/   docker compose, Caddy, Grafana/Prometheus
```

## Commands
```
make bootstrap C=<core|proto|cpp|go|py|web>   # install ONE component's toolchain, only when its first task starts
make up             # docker compose profile `core` (postgres, nats); add PROFILES="md graph obs" when the milestone needs them
make clean-space    # prune build dirs, caches, dangling images; prints disk usage
make proto          # buf lint + format check + generate Go/Python/TS for scaffolded services (C++ via CMake)
make proto-breaking # buf breaking against main
make migrate        # apply db/migrations, clickhouse, neo4j constraints
make dev-secrets    # create JWT key + VYUHA_MASTER_KEY in .env (never printed / overwritten)
make dev-gateway    # run the gateway with .env loaded
make test           # all unit tests (engine, gateway, quant, intel, web)
make test-engine    # ctest incl. sanitizers preset `asan`
make bench          # Google Benchmark: risk gate, normalizer
make e2e            # Playwright (Chromium) against a throwaway gateway + database (scripts/e2e.sh; needs `make up`)
make lint           # buf, clang-tidy, clang-format --dry-run, gofmt, golangci-lint, ruff, mypy, tsc (web: strict tsc only)
make verify         # lint + test + e2e + invariants check (run before declaring a task done)
make staging-build  # staging/production images (deploy/staging, ADR-022; runbook docs/STAGING.md)
```

## Working rules
- Start every task by restating which **feature ID(s)** and **acceptance criteria** it satisfies.
- Write the test first for any risk, OMS, pricing, cost-model or RBAC logic.
- Keep PRs small: one task from `TASKS.md` per PR. At the end, tick it in `TASKS.md` with the date and notes.
- Contracts first: change `proto/` or `gateway/api/openapi.yaml` and regenerate before you implement.
- DB changes go only through a new numbered migration in `db/migrations/`. Never edit an applied one.
- New dependencies must be listed in `docs/TECH_STACK.md` with a reason. Prefer what is already there.
- If a spec is ambiguous, check `DECISIONS.md`. If it's still unclear, write `[DECISION NEEDED: …]` with a recommended default in `docs/DECISIONS.md` and ask the owner. Don't guess on anything touching money, risk, compliance or broker behaviour.
- Broker API behaviour (limits, params, endpoints) changes often. Verify against current official docs before you implement an adapter, then record the doc URL + date in the adapter's README.
- Use `.claude/agents/risk-reviewer` on any diff touching `engine/src/{risk,oms,broker}` or `gateway/internal/{auth,rbac}`.

## Style
- C++: see `engine/CLAUDE.md` (no exceptions on hot path, `std::expected`, no heap alloc per tick).
- Go: stdlib-first, `context` everywhere, errors wrapped with `%w`, table tests.
- Python: typed (mypy strict), Polars over pandas, Pydantic v2 models at boundaries, `ruff` clean.
- TS: strict mode, no `any`, components < 250 lines, ticks decoded in a Web Worker.
- Commit messages: Conventional Commits with feature ID, e.g. `feat(risk): F-RK-1 daily loss rule`.
