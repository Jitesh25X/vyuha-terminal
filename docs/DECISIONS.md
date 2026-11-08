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

### ADR-016 — Private repo until licensed — Superseded by ADR-021 (2026-10-31) — 2026-10-28
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

### ADR-021 — Public without license — Accepted (owner) — 2026-10-31
**Context:** ADR-016 kept the repo private until a license was chosen. GitHub Actions minutes on a private repo are limited, and the engine's CI (vcpkg + three sanitizer presets) is long; public repos get free CI and free secret scanning.
**Decision:** Supersedes ADR-016. The repository is **public without a license**: all rights reserved, nothing granted beyond GitHub's Terms of Service. `DISCLAIMER.md` and the README say so, plus "not investment advice" and "not affiliated with Bloomberg, BlackRock, Zerodha or Upstox". A license (AGPL-3.0 still recommended, D-3) is added **before accepting outside contributions**. Before going public the full history was audited (gitleaks over every branch + targeted greps for secrets, personal data, trade data and broker responses); GitHub secret scanning and push protection are enabled once it is public.
**Consequences:** Outside pull requests are not accepted until the license lands. Anything committed is world-readable: secrets stay in `.env` / `deploy/secrets/` (gitignored), fixtures stay synthetic, and no real trade data or broker responses are committed.

### ADR-022 — Staging/production: Docker Compose with host networking for Vyuha's services — Accepted (owner chose Compose) — 2026-10-31
**Context:** M1.12 needs Vyuha's services running on the staging VM. ARCHITECTURE §9 planned Docker Compose with only the engine on the host network. Since M1.8 the gateway accepts only loopback addresses for the engine gRPC (it carries broker secrets, no TLS yet) and for the quant API (unauthenticated). From a bridge container the host is not loopback. The owner chose Compose over native systemd services.
**Decision:** `deploy/staging/` — images for engine (GCC 14 + vcpkg at the manifest baseline), gateway/recorder/vyuhactl (static Go, distroless), quant (uv-locked), and Caddy with the built web bundle; base images pinned by digest. Postgres, NATS and ClickHouse run on the compose bridge with ports published on 127.0.0.1 only. The engine, gateway, recorder, quant and Caddy run with `network_mode: host`, so every internal hop stays on loopback and the engine binds account IPs directly; Caddy is the only public listener. Each service gets only its own secrets (no shared env file); services log in to Postgres as `vyuha_gateway` / `vyuha_engine` / `vyuha_quant` (members of `vyuha_app`, INSERT-only on `audit_event`); the owner role runs migrations only. Scheduled jobs (instrument sync 08:00 IST, ban check 08:45 IST) are host systemd timers running `docker compose run quant …`. `LIVE_TRADING_ENABLED` is pinned to `false` in the compose file.
**Consequences:** Host networking means service ports must stay bound to 127.0.0.1 (they are). Images are built on the VM (`make staging-build`); the engine image's first build takes about an hour. ClickHouse still uses one user for writer and reader (least privilege there later). Revisit the bridge layout when mTLS lands for the engine and quant APIs.

### ADR-023 — Build order while the staging VM is pending — Accepted (owner) — 2026-11-02
**Context:** The staging VM details aren't available yet. M1.12 and M3 need the VM. ROADMAP says to work through milestones in order, which would stop all work at M1.12.
**Decision:** Supersedes the "milestones in order" rule until the VM details arrive. M1.12 and M3 are parked as *blocked: waiting on VM*. New order: **M4.1 next, out of order** (Upstox historical downloader; it has no M2/M3 dependency) → all of **M2** (paper OMS, risk gate, SimBroker, kill switch, order ticket) → **M4** (cost model, SDK, backtester, reports). M4.7's live strategy hosting is split off as blocked on M3; paper and backtest go ahead. M1.12 and M3 resume when the owner sends the VM details. M4.1's first job is limited to Nifty 50 constituents + NIFTY/BANKNIFTY index, 1-minute from Jan 2022 and daily from 2015, and nothing more (invariant 12).
**Consequences:** The M1, M2 and M4 exit boxes stay open: their criteria need staging or live trading (M1: live data on staging; M2: a paper day on staging; M4: the same strategy live), so tasks depend on the last task of a milestone, not its exit box. M5 still waits for M4 exit. Invariants are unchanged: the downloader binds the account's address like every other broker call.

### ADR-024 — A broker fill is a fact — Accepted (owner) — 2026-11-03
**Context:** F-EX-2 listed no transition for several real broker races, so the OMS refused them: a fill during a cancel would not update the order (D-17).
**Decision:** The OMS never refuses or drops a broker fill. Added: `CANCEL_PENDING` → `PARTIALLY_FILLED`/`FILLED` (the fill raced the cancel) and → `OPEN` (cancel rejected); `MODIFY_PENDING` → `PARTIALLY_FILLED`/`FILLED`/`OPEN` (a fill during the modify, or the modify rejected); `OPEN`/`PARTIALLY_FILLED` → `CANCELLED` (broker- or exchange-initiated, `filled_qty` kept); `OPEN` → `BROKER_REJECTED` (post-ack RMS reject); `UNKNOWN` → any real broker state, via reconcile only, still never re-sending. `SENT` → `PARTIALLY_FILLED`/`FILLED` (a fill can beat the ack) and → `CANCELLED` (an IOC cancelled before the ack). A late ack after a fill or cancel is a recorded no-op (`LATE_ACK`): not an illegal transition and not an alert. Only real mismatches raise `RECON_MISMATCH`. A fill on an order in a terminal state is still recorded: `filled_qty` and positions update and a `RECON_MISMATCH` alert is raised.
**Consequences:** A fill past the order's quantity is capped on `order.filled_qty` (the column's CHECK) and kept in full on the `order_event`, with `RECON_MISMATCH`. A fill in a status the list doesn't cover (`UNKNOWN`, or before `SENT`) follows the same rule: recorded and alerted, status unchanged. Positions arrive with M2.6, which must read these events. Only a fill of zero or negative shares is refused, as garbage. A trade already counted on that order (same `broker_trade_id`, also after a restart) isn't a new fact: it's recorded as `DUPLICATE_FILL` with no shares and changes nothing. A trade that can't be remembered (id over 40 characters, or the table is full) is counted and raises `RECON_MISMATCH`. A modify changes the order on the broker's confirmation, and `order_event` keeps what it was: with one out, `qty` is the higher of old and new, so a fill against either counts and the order doesn't read `FILLED` with shares still live. An answer that arrives after a fill moved the order on is applied, and the status stays unless the order is now complete.

### ADR-025 — Persist before send; client_order_id is the broker tag — Accepted (owner) — 2026-11-03
**Context:** Order events were written behind the core thread, so a crash could lose an order the broker already had, and a client retry could send a duplicate (D-18).
**Decision:** Write-ahead: `NEW` and `APPROVED` (with `client_order_id`) are durable in Postgres before the adapter sends anything. Postgres stays off the core thread: the router polls the writer's durable count and sends once it passes the order's event. `SENT` is recorded after the adapter has the order, so after a crash `SENT` means handed over and `APPROVED` means maybe. `client_order_id` goes in the broker's order tag, so reconcile finds any order the broker has by its tag. On restart nothing is sent (**owner, 2026-11-03**, replacing "one it lacks gets its first send"): an `APPROVED` order the broker has is adopted; one it lacks becomes `EXPIRED` (terminal, reason `EXPIRED_ON_RESTART`), with an `order.expire` audit event and a critical `ORDER_EXPIRED_ON_RESTART` alert to admins in the same transaction, because a stale order surfacing after a crash must never fire; a `SENT` or `UNKNOWN` order is never sent again; two orders with one tag raise `RECON_MISMATCH` and stay blocked.
**Duplicates:** one bad request must never stop all sends, or cancels. `Oms::intake` is idempotent on `client_order_id`: the same body returns the existing order, a different one is `ORDER_DUP`. A duplicate that still reaches the writer (an order from an earlier run) fails alone: a data error (SQLSTATE 22, 23) is retried event by event, the bad order is quarantined with a critical `ORDER_EVENT_REFUSED` alert, and it is reported to the router before its events count as durable, so it never goes out. A live order's event whose broker id clashes is kept, without the id. `OrderRouter::cancel_all` doesn't consult the order log at all.
**Postgres outage (owner, 2026-11-03):** persist-before-send wins over "a trader can always get flat". While events aren't durable no new order goes out, including position-reducing and kill-switch flatten orders, so a crash can't leave an unrecorded order at the broker. Cancels still go, and the broker's own terminal is the fallback for flattening.
**Consequences:** About 1 ms per order, against the broker's 10–100 ms. The strategy tag stays in `order.tag` but no longer goes to the broker (F-EX-3 changed). The tag keeps a client id's letters and digits and its last characters up to the broker's limit, because a UUIDv7 starts with a shared timestamp. **Residual risk:** an order in flight at the moment of the crash that the broker's book doesn't show yet would look absent and be marked `EXPIRED` while live at the broker. It can't cause a second send, but it could trade unmanaged, and restart doesn't reload terminal orders. So M3's live recovery must wait for the book to settle before trusting "not there", and its periodic reconcile must look for `EXPIRED` tags at the broker, raise `RECON_MISMATCH` and cancel. Until then a fill on an expired order is still recorded and alerted (ADR-024).

### ADR-026 — A day with no bars clears only by an audited known-gap record — Accepted (owner) — 2026-11-03
**Context:** A trading day with no bars is a gap, so the job exits 3 and retries it. A day the broker will never serve for one instrument (a suspension, or a data hole at the broker) would then fail every run.
**Decision:** That day clears only through a record for that instrument and day in `download_known_gap` (migration 000009). An active admin adds it with `vyuha-quant download known-gap --symbol --day --reason --user`. The reason is required, and each record writes a `data.known_gap.add` audit event. Records are insert-only: there is no update or delete grant for the app role. A recorded day is counted in `days_known_gap`, not as covered data.
**Consequences:** One call per day, so a long suspension needs many calls. The CLI trusts `--user`, so anyone with the database URL can name an admin. A record has no interval, so it covers all intervals. There is no revoke. These are open P2s, listed in the TASKS M4.1 review log.

### ADR-027 — Outage rules and the queue age limit — Accepted (owner) — 2026-11-07, extended 2026-11-08
**Context:** ADR-025 stops sends while order events aren't durable, but the OMS still took in new orders and risk-adding modifies during an outage, and anything queued then fired when the outage ended, however long it lasted (M2.1b review). Separately, `is_paper` defaulted to true, so an order built without saying paper or live quietly became paper (M2.2 review).
**Decision:**
- **While the order log isn't durable** (`healthy()` false), nothing that adds risk is taken in. Refused with `ORDER_LOG_UNAVAILABLE` (`kOrderLogUnavailable`), recording nothing: new orders (`Oms::intake`; a retry of a request already taken in still returns the existing order), a modify that raises qty, and any price or trigger modify. Until positions exist (M2.6) the OMS can't tell an opening order from a reducing one, so every price or trigger change counts as opening (fail closed); revisit at M2.6. A modify that only lowers qty goes. Cancels, `cancel_all` and broker facts (acks, fills, cancel confirms) always go through.
- **The alert:** an alert row can't be written while Postgres is down. So each refusal returns its code immediately and logs an error line. The writer counts refusals by kind and, after the first batch that commits once Postgres is back, writes one critical `ORDER_LOG_OUTAGE` alert to active admins. If that write fails it is retried about once a second while the writer is idle, and a report still unwritten at stop goes to the log with its counts. It carries the outage's length and the counts (`refused_new_orders`, `refused_qty_raises`, `refused_price_modifies`). A refusal counted just as the outage ends goes in the next report. *(Proposed by the agent at M2.2b; owner to confirm the delivery, since the spec said "an alert" without saying when.)*
- **Queue age limit:** a queued order (`APPROVED`, not yet sent) whose turn comes more than `ORDER_QUEUE_MAX_AGE_MS` (default 5,000, range 1–60,000) after its gate stamp is never sent. It becomes `EXPIRED` with reason `EXPIRED_IN_QUEUE`, audited (`order.expire`) and alerted (`ORDER_EXPIRED_IN_QUEUE`, critical) in the status's transaction, as the restart rule's expiry is. If the expiry can't be recorded (a full ring) the queue waits; the order is still never sent.
- **`is_paper` is required** (owner, 2026-11-08): `OrderDetails::is_paper` is a `PaperFlag` with no default and an explicit constructor. Leaving it out, or passing a bare `bool`, doesn't compile (ctest `compile_fail.PAPER_MISSING`, `PAPER_FROM_BOOL`). An order event's `details` (set on the first event only) became `std::optional<OrderDetails>`.

**Consequences:** During an outage a trader can cancel and reduce qty but not add risk or move a price, and an order queued before it can fire at most 5 s after it was approved. A long outage therefore drops queued orders; they are visible as `EXPIRED_IN_QUEUE`, and none fires late. The outage alert arrives only after recovery; the immediate signal is the error code to the caller and the engine's log.

---

## Open (owner to decide) — mirrored from PRD §13
| # | Topic | Default in code until decided |
|---|---|---|
| D-1 | Product name | "Vyuha" |
| D-2 | Legal/account structure, operator empanelment | **Deferred** (ADR-015): personal use only; resolve with a lawyer before onboarding anyone else |
| D-3 | License | **Deferred** (ADR-021): repo public without a license (all rights reserved); pick one before accepting outside contributions (AGPL-3.0 recommended) |
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
| D-16 | Decrypt sites | **Resolved** (owner, 2026-11-03): five permitted sites, listed in `gateway/CLAUDE.md`, all audited; a test fails on a new one |
| D-17 | Extra OMS transitions | **Resolved** (ADR-024) |
| D-18 | Persist before send | **Resolved** (ADR-025) |
| D-19 | Outage alert delivery (ADR-027) | Proposed: one `ORDER_LOG_OUTAGE` alert after recovery with counts; owner to confirm |
