# Decision Log (ADR-lite)

Format: `ADR-NNN — Title — Status (Accepted | Proposed | Superseded by ADR-x) — Date`. Context → Decision → Consequences.
Claude Code: you may add **Proposed** entries. Only the owner moves them to Accepted.

---

### ADR-001 — Fresh product, separate from earlier projects — Accepted — 2026-10-27
**Decision:** Vyuha is a new codebase. It doesn't reuse code from the owner's earlier trading projects.
**Consequences:** Ideas can be ported, but every module meets this repo's specs and tests.

### ADR-002 — Users and scope — Accepted — 2026-10-27
**Decision:** Single small desk (2–10 users) per deployment. Roles admin / risk_manager / trader / analyst. Brokers: Zerodha + Upstox. Instruments: NSE/BSE equities + index and stock F&O.
**Consequences:** No multi-tenancy. Commodities, currency and others are post-MVP.

### ADR-003 — Web first, desktop later — Accepted — 2026-10-27
**Decision:** React web terminal for MVP; Electron wrapper post-MVP.

### ADR-004 — MVP modules — Accepted — 2026-10-27
**Decision:** Market data + charts; execution + OMS; portfolio risk analytics; AI copilot + research; historical data download, strategy building and backtesting.

### ADR-005 — Context mapping = knowledge graph + unified risk model — Accepted — 2026-10-27
**Decision:** Both. Neo4j graph for news/events → positions, plus a factor-based desk-wide risk model.

### ADR-006 — Data budget — Accepted (default) — 2026-10-27
**Decision:** Free broker feeds by default, behind pluggable `IMarketDataSource`; paid vendors post-MVP.
**Note:** Owner answered "continue"; confirm at M1.

### ADR-007 — Polyglot stack: C++ engine, Go gateway, Python quant/AI, TS UI — Accepted — 2026-10-27
**Context:** Owner asked for the "best" stack, similar to Bloomberg's.
**Decision:** See `TECH_STACK.md`.
**Consequences:** Four languages. Contracts via Protobuf keep them aligned.

### ADR-008 — One static egress IP per broker account — Accepted — 2026-10-27
**Context:** NSE rules allow sharing a static IP only among family members.
**Decision:** The engine binds each account's sockets to its own IP. The DB enforces unique public IPs.

### ADR-009 — Market data entitlement per mapped account — Accepted (default) — 2026-10-27
**Context:** Possible redistribution concerns with broker API data.
**Decision:** Users only see live data from accounts they're mapped to with `data_entitled=true`.

### ADR-010 — AI is read-only + drafts — Accepted — 2026-10-27
**Decision:** No AI tool places, modifies or cancels orders. `draft_order_ticket` needs a human submit.

### ADR-011 — Desk OPS cap 8/s per account per exchange — Accepted — 2026-10-27
**Decision:** Stay under the 10 OPS registration threshold. DB CHECK caps at 10.

### ADR-012 — No object store in MVP — Accepted — 2026-10-27
**Context:** MinIO community images were discontinued in 2025; single-host deployment.
**Decision:** Parquet on local NVMe; S3-compatible storage is optional later.

---

### ADR-013 — Runtime desk agents — Accepted — 2026-10-27
**Decision:** Add AI "desk staff" agents that mirror firm roles (`DESK_AGENTS.md`). MVP: Desk Head, Risk Analyst, Macro Analyst (M8). Agents analyse/draft/alert only. No autonomous trading, limit changes or strategy promotion.

### ADR-014 — Lean footprint, just-in-time downloads — Accepted — 2026-10-27
**Decision:** Each task in `TASKS.md` declares a `needs:` allow-list; nothing else is installed or downloaded. Compose profiles start services only when their milestone needs them. CPU-only ML wheels; one embedding model; market data only per job spec. `make clean-space` at each milestone exit.

---

## Open (owner to decide) — mirrored from PRD §13
| # | Topic | Default in code until decided |
|---|---|---|
| D-1 | Product name | "Vyuha" |
| D-2 | Legal/account structure, operator empanelment | One account per holder; live limited to owner's account until resolved |
| D-3 | License | AGPL-3.0 (LICENSE file not added until decided) |
| D-5 | Build mode / hand-written modules | Claude Code writes; 🧠 modules in ROADMAP for owner |
| D-6 | LLM provider & privacy | Anthropic API; aggregated data only |
| D-7 | Exchange announcements / ban list source | Disabled scraping; official daily files where terms allow |
| D-8 | MARKET order handling | Allowed with market protection 2% |
| D-9 | Hosting provider | Any Mumbai-region VM with multiple static IPs |
| D-10 | Auto-flatten | Off |
| D-11 | Cost schedule values | Seed + verify at M4 |
| D-12 | Min reward:risk without bracket | Allowed with warning |
| D-13 | Macro data source for SENSITIVE_TO | Manual CSV / owner downloads |
| D-14 | Desk-agent AI budget (per agent / desk monthly) | Per-agent caps in YAML; desk cap set before M8 |
| D-15 | Agents beyond MVP three | Research + Quant as M8 stretch |
