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

### ADR-015 — Personal use first — Accepted — 2026-10-28
**Decision:** For now Vyuha is developed and used **only by the owner, on the owner's own broker accounts**. Multi-user/desk features (roles, account mapping, per-account static IPs) stay in the design and get built as specified. But no other person's account is connected and no other user trades live until the legal/account-structure question (D-2) is answered with a lawyer.
**Consequences:** M3 live pilot and M9 burn-in run with the owner only. The D-2 lawyer check moves from "before M3" to "before onboarding anyone else".

### ADR-016 — Private repo until licensed — Accepted — 2026-10-28
**Decision:** Keep the repository **private**. Choose and add a license (LICENSE, NOTICE, DISCLAIMER) **before** making it public on GitHub. AGPL-3.0 remains the recommendation. No LICENSE file until then.

### ADR-017 — Build mode: Claude Code writes all code — Accepted — 2026-10-28
**Decision:** Claude Code implements every task, including the former hand-written learning modules (M1.2 SPSC ring, M2.2 RiskGate framework). The owner reviews each task before continuing.

### ADR-018 — Audit hash chain v2 via a new migration — Accepted (default; owner may revisit) — 2026-10-28
**Context:** `000001_init.up.sql`'s `audit_chain()` left `actor_id`/`actor_kind` out of the hash (a `--` comment swallowed that line), joined fields with `|` (text could shift between fields without changing the hash) and assigned ids before taking the chain lock (concurrent inserts could chain out of id order). `000001` is protected from edits in `.claude/settings.json`; the owner was offered (A) fixing 000001 in place or (B) a new migration, and continued without choosing.
**Decision:** (B) `000004_audit_hash_v2` replaces `audit_chain()`: every column incl. `ip` is hashed, each field length-prefixed (`len:value`, NULL = `-`), ids assigned inside the trigger after the advisory lock. It refuses to run if `audit_event` already has rows. `gateway/internal/audit/verify.go` recomputes the same canonical form independently in Go. A chain cannot reveal deletion of its newest rows, so `vyuhactl audit head` prints an `id:hash` anchor to keep outside the database and `audit verify -anchor` checks it.
**Consequences:** 000001 still contains the v1 function text (superseded). If the owner later prefers (A), fold 000004 into 000001 before any non-dev database exists.

### ADR-019 — F&O ban list: manual import of NSE's daily file (D-7) — Accepted (owner) — 2026-10-28
**Context:** F-MD-1 needs the NSE F&O ban list daily before 09:00 IST. The owner first chose an automated daily fetch of NSE's published `fo_secban.csv`, on condition that NSE's terms allow it. NSE circular NSE/WEB/45032 (2020-07-16) states that using scripts to download information or scraping data from the NSE website is not permitted and points to NSE's paid data subscription; NSE is also reported to block cloud IP ranges.
**Decision:** No scripted access to NSE. An operator downloads "Securities in Ban Period" (`fo_secban.csv`, currently `archives.nseindia.com/content/fo/fo_secban.csv`) in a browser and runs `vyuha-quant ban import FILE`. The file's own trade date must match the day imported. Each import is stored in `fo_ban_import` (migration 000006) and audited. `vyuha-quant ban check` (scheduled before 09:00 IST) raises an admin alert when today's list is missing; until then the latest earlier list stays applied. Ban flags apply to NFO futures/options of the listed underlyings only; cash and BFO are unaffected. Announcement scraping stays disabled.
**Consequences:** Ban flags depend on a daily human step; a missed import is visible (alert + stale-list date) but does not block trading. Revisit with a licensed feed (NSE data subscription or a broker API exposing the list).

### ADR-020 — Entitlement routing by feed id (ADR-009 strict) — Accepted (owner) — 2026-10-29
**Context:** ADR-009 / COMPLIANCE.md §4: a feed obtained through one person's broker account is shown only to users mapped to that account. The engine published `md.tick.<id>` without saying which account's feed produced a tick, so the gateway could not tell two traders' feeds apart when both stream the same instrument. Options were tagging ticks by feed, one feed per instrument (deny everyone else), or desk-wide entitlement.
**Decision:** The engine gives every session a `feed_id` (unique per start; the counter starts at a random point in [1, 2³¹] each process so ids aren't reused across restarts while old ticks sit in `md_last`) and stamps it on each tick (`marketdata.v1.Tick.feed_id`, `SessionInfo.feed_id`). The gateway maps feed ids to accounts from `GetSessions` and delivers a tick only to users mapped (`can_view`) to that account when it is `data_entitled`. A user's subscription registers engine interest through one of their own entitled accounts, so two users with their own accounts each get the instrument from their own feed. Snapshots from `md_last` are used only when their tick came from one of the user's feeds.
**Consequences:** The same instrument may stream on several feeds at once (more broker subscriptions). 1-minute bars are still built per instrument by the engine from whichever feeds carry it; a bar goes to a user only when one of their own entitled accounts is carrying that instrument on a connected feed, but such a bar can include ticks from a colleague's feed of the same instrument. The broker login URL shown to the user contains the account's API key (`api_key` / `client_id`): public by design in both brokers' OAuth flows, an accepted exception to invariant 5 (the API secret and tokens never leave the server). Ticks from a feed the gateway hasn't yet seen are dropped (counted) until the next 1 s poll or the login callback reports it.

---

## Open (owner to decide) — mirrored from PRD §13
| # | Topic | Default in code until decided |
|---|---|---|
| D-1 | Product name | "Vyuha" |
| D-2 | Legal/account structure, operator empanelment | **Deferred** (ADR-015): personal use only; resolve with a lawyer before onboarding anyone else |
| D-3 | License | **Deferred** (ADR-016): repo private; pick before going public (AGPL-3.0 recommended) |
| D-5 | Build mode | **Resolved** (ADR-017): Claude Code writes all |
| D-6 | LLM provider & privacy | Anthropic API; aggregated data only |
| D-7 | Exchange announcements / ban list source | **Resolved** (ADR-019): ban list imported manually from NSE's daily file; no scripted NSE access; announcement scraping disabled |
| D-8 | MARKET order handling | Allowed with market protection 2% |
| D-9 | Hosting provider | Any Mumbai-region VM with multiple static IPs |
| D-10 | Auto-flatten | Off |
| D-11 | Cost schedule values | Seed + verify at M4 |
| D-12 | Min reward:risk without bracket | Allowed with warning |
| D-13 | Macro data source for SENSITIVE_TO | Manual CSV / owner downloads |
| D-14 | Desk-agent AI budget (per agent / desk monthly) | Per-agent caps in YAML; desk cap set before M8 |
| D-15 | Agents beyond MVP three | Research + Quant as M8 stretch |
