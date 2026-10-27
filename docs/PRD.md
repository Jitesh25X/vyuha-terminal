# PRD: Vyuha — Open-Source AI Quant Desk for Indian Retail Prop Desks

**Version:** 1.1 (adds Module I — Desk Agents, and §10.1 Lean Footprint)
**Status:** Draft — awaiting owner sign-off on §13 Open Decisions
**Author:** Jitesh Raj
**Last Updated:** 2026-10-27
**Target Build Agent:** Claude Code
**Working name:** "Vyuha" (Sanskrit: a battle formation). `[DECISION NEEDED: final product name — default: keep "Vyuha"]`

> Companion specs (the build agent MUST read the relevant one before working on a module):
> `ARCHITECTURE.md` · `TECH_STACK.md` · `DATA_MODEL.md` · `API.md` · `RISK_ENGINE.md` ·
> `CONTEXT_ENGINE_AND_AI.md` · `STRATEGY_SDK_AND_BACKTEST.md` · `UI_SPEC.md` · `COMPLIANCE.md` ·
> `DESK_AGENTS.md` · `ROADMAP.md` · `TASKS.md` (the execution queue) · `DECISIONS.md`

---

## 1. Problem Statement

Small Indian proprietary trading desks (2–10 traders working NSE/BSE cash and F&O) run on a patchwork:
broker web apps (Kite, Upstox Pro) for execution, TradingView for charts, Sensibull/Opstra for option
analytics, Excel for risk and P&L, Telegram/X for news, and ad-hoc Python scripts for backtests.
There is no single screen that shows **what the desk holds, what it is exposed to, what is happening in
the market that affects those exposures, and what each trader is allowed to do** — the job that
Bloomberg Terminal (data + execution) and BlackRock Aladdin (portfolio risk + context) do for
institutions. Those cost lakhs per seat per year and are not built for Indian retail broker APIs.

**Primary user persona:** *Arjun, desk risk manager / owner* — runs a 5-trader desk trading Nifty/BankNifty
options and liquid F&O stocks. Cares about: daily loss limits being enforced *before* a fat-finger order
leaves, net desk Greeks, which positions a breaking news item touches, and an audit trail for every order.

**Secondary persona:** *Meera, options trader* — wants a keyboard-driven terminal: option chain, depth,
fast order ticket with brackets, her P&L and limit headroom, and an AI assistant that answers
"why did my position move?" with evidence.

**Tertiary persona:** *Kabir, quant/analyst* — downloads history, writes Python strategies, backtests with
realistic Indian costs, then promotes a strategy to paper and (after approval) live.

**Pain point:** No pre-trade risk control across traders; risk is computed after the fact in Excel;
news is read manually with no mapping to positions; backtests ignore STT/stamp duty/slippage and use
different code from live trading; no consolidated audit trail now that SEBI's retail algo framework
(fully applicable from 1 Apr 2026) requires static-IP, tagged, traceable API trading.

**Opportunity:** Broker APIs (Kite Connect, Upstox v3) now expose orders, WebSocket ticks, option
Greeks and historical candles at retail prices; LLMs can turn unstructured news/filings into a
knowledge graph; open-source infrastructure (ClickHouse, NATS, Neo4j) makes an institutional-style
stack self-hostable on one Mumbai VM.

---

## 2. Product Overview

**What it is:** A self-hosted, open-source trading terminal and desk OS for Indian markets that unifies
live market data, execution with a non-bypassable pre-trade risk gate, Aladdin-style portfolio risk and
context mapping, research/backtesting, and an AI copilot — in a keyboard-first, Bloomberg-style UI.

**Core value proposition:** One screen where a desk sees positions, risk, limits, market context and
AI explanations together, and where no order — manual, strategy or AI-drafted — reaches a broker
without passing the desk's risk rules and leaving an audit record.

**Product type:** [x] Web App (desktop wrapper post-MVP) [x] Backend services [ ] Mobile

**MVP goal:** A desk admin self-hosts Vyuha, connects Zerodha and Upstox accounts for each trader,
and the desk can trade, risk-manage, research and query the AI copilot end-to-end from the terminal.

**Success criteria (MVP launch) — all must pass on the staging desk (3 users, 1 Kite + 1 Upstox account):**
- [ ] A user logs in with password + TOTP and reaches the terminal workspace in < 3 s.
- [ ] Admin links one Zerodha and one Upstox account; each account's order traffic leaves from its own configured static IP (verified by engine preflight log).
- [ ] Trader opens `NIFTY OC` (option chain) and sees live LTP/OI/IV/Greeks updating; tick-to-screen p95 < 400 ms.
- [ ] Trader places a paper order and a live order (1 lot); both pass the risk gate, appear in the blotter, and the fill updates position and P&L within 1 s of the broker update.
- [ ] An order that breaches any configured limit is rejected by the risk gate with a reason code and never reaches the broker (verified via mock broker in CI and broker order book in staging).
- [ ] Desk kill switch cancels all open orders across all accounts and blocks new ones within 2 s.
- [ ] Risk manager sees desk net Delta/Gamma/Vega/Theta, 1-day 99% historical VaR, and a spot×IV scenario grid, refreshing at ≤ 5 s.
- [ ] Analyst downloads 2 years of 1-minute candles for the Nifty 50 constituents, backtests the reference SMA-cross strategy with Indian cost model, and results match the golden reference within 0.01% P&L.
- [ ] The same strategy file runs unchanged in backtest, paper and live modes.
- [ ] A news item ingested from a configured feed produces a context alert to every trader whose positions are within 2 graph hops, within 60 s, showing the graph path.
- [ ] Copilot answers "What is my exposure to banks if RBI cuts rates?" by calling risk and graph tools and citing them; it can draft — never place — an order ticket.
- [ ] Every order event and admin action is in the hash-chained audit log and exportable as CSV.
- [ ] The three MVP desk agents (Desk Head, Risk Analyst, Macro Analyst) run on their triggers. When a trader's limit use crosses 85%, the Risk Analyst posts a note with a scenario within 60 s, and the Desk Head's pre-open brief cites the other agents' notes.
- [ ] No desk agent can place/modify/cancel orders, change limits or release a kill switch (registry test + red-team eval).
- [ ] A fresh dev setup through M0 uses ≤ 15 GB of disk (toolchains + images + deps). No dataset, model or image is downloaded before the task that needs it (§10.1).

---

## 3. Users & Auth

**User types (single desk per deployment in MVP):**

| Role | Description | Permissions |
|------|-------------|-------------|
| `admin` | Desk owner / operator | Everything; manage users, broker accounts, static-IP bindings, global limits; cannot bypass risk gate |
| `risk_manager` | Oversees risk | View all positions/orders/P&L; set limits for desk/accounts/traders/strategies; trigger any kill switch; approve strategy go-live; square off any position |
| `trader` | Trades mapped accounts | Trade only on mapped broker accounts within limits; view own positions/orders/P&L; run backtests; deploy strategies to paper; request live |
| `analyst` | Research only | Market data (via mapped data entitlement), research, backtests, paper trading; **no live orders** |

**Auth method:** Username + password (argon2id) + mandatory TOTP (RFC 6238). No social login.
**Session management:** Access JWT (EdDSA, 15 min) in memory + rotating refresh token (7 days, httpOnly, Secure, SameSite=Strict cookie, stored hashed in Postgres, revocable). Re-auth (password + TOTP) required for: enabling live trading on a strategy, editing limits, linking broker accounts, desk kill-switch release.
**Broker auth:** Each broker account uses its **own** API app (Kite Connect app / Upstox app) — required because keys are client-specific under the SEBI/NSE framework and Upstox allows one active API app per user. Daily login is user-driven via the broker's OAuth/redirect flow with the broker's own 2FA. **Vyuha must not automate broker TOTP.**

`[DECISION NEEDED: legal/account structure of the desk — see §13 #2. Default implemented: one broker account = one account holder; traders are mapped to accounts; each account has its own static egress IP.]`

---

## 4. MVP Feature Set

Features are grouped into 8 modules. IDs (e.g. `F-MD-1`) are referenced in `ROADMAP.md` and tests.

### Module A — Terminal Shell (UI)

#### F-UI-1: Command line & function mnemonics
**Description:** Bloomberg-style command bar. Syntax: `[SECURITY] [SEGMENT] FUNCTION` then Enter. Examples: `RELIANCE EQ CH`, `NIFTY OC`, `BANKNIFTY FUT DOM`, `POS`, `RISK`, `AI`. Full list in `UI_SPEC.md §3`.
**User flow:**
1. User presses `` ` `` (backtick) or clicks the command bar.
2. User types; system shows fuzzy matches for securities (instrument master) and functions, ≤ 50 ms per keystroke.
3. User presses Enter.
4. System opens the function panel in the focused panel slot (or a new tab with `Shift+Enter`) bound to that security.
**Acceptance criteria:**
- [ ] Given instrument master loaded, when user types `relia`, then `RELIANCE NSE EQ` is in the top 3 suggestions within 50 ms.
- [ ] Given `NIFTY OC` + Enter, then an option-chain panel for the nearest NIFTY expiry opens.
- [ ] Given an unknown function `XYZ`, then the bar shows `Unknown function XYZ — type HELP` and nothing opens.
- [ ] Every function in `UI_SPEC.md §3` is reachable by keyboard only (Playwright test without mouse).
**UI components:** `CommandBar`, `SuggestionList`, `FunctionRegistry`.

#### F-UI-2: Docking workspace, security linking, multi-monitor
**Description:** Panels dock, tab, split and float (Dockview). Panels join a **link group** (colour: amber/cyan/magenta/green); changing the security in one linked panel updates all panels in that group. Panels can pop out to separate browser windows (multi-monitor); layouts save per user.
**Acceptance criteria:**
- [ ] Given CH and DOM in the amber link group, when user changes CH to `INFY`, then DOM shows INFY within 200 ms.
- [ ] Given a popped-out window, when the main window changes the linked security, then the popped window updates (BroadcastChannel).
- [ ] Given a saved layout, when the user logs in on another browser, then the same layout restores.
- [ ] Edge: if a popped-out window is closed, its panel returns to the main workspace on next load.
**UI components:** `Workspace`, `PanelFrame`, `LinkGroupBadge`, `LayoutManager`.

#### F-UI-3: System status bar
**Description:** Always-visible bottom bar: market session state (pre-open/open/closed), per-broker-account connection state, data latency (ms), engine health, kill-switch state, user/role, IST clock.
**Acceptance criteria:**
- [ ] Given a broker WebSocket disconnect, then that account's indicator turns red within 2 s and an alert toast shows `Upstox <acct> market data disconnected — reconnecting`.
- [ ] Given desk kill switch active, then a red `KILL SWITCH ACTIVE` banner is shown on every panel.

### Module B — Market Data

#### F-MD-1: Instrument master
**Description:** Daily (08:00 IST) sync of Kite instruments dump and Upstox instruments file into a unified `instrument` table with a canonical ID and per-broker mapping; includes lot size, tick size, expiry, strike, option type, freeze quantity, F&O ban flag.
**Acceptance criteria:**
- [ ] Given both brokers' files downloaded, then every NFO/BFO contract present in both maps to one `instrument_id` with both `kite_token` and `upstox_key`.
- [ ] Given a contract present in only one broker, then it is stored with the other mapping NULL and flagged `partial_mapping=true`.
- [ ] Given the sync fails, then yesterday's master stays active and an admin alert is raised; trading is not blocked.
- [ ] The NSE F&O ban list is ingested daily and `instrument.fo_ban` updated before 09:00 IST. `[DECISION NEEDED: ban-list source — see §13 #7]`

#### F-MD-2: Live streaming (Kite + Upstox)
**Description:** C++ engine connects to Kite Ticker (binary) and Upstox Market Data Feed V3 (protobuf), normalizes to canonical `Tick`/`Depth`, publishes on NATS, and the gateway fans out to browsers with per-panel conflation (default 250 ms).
**Acceptance criteria:**
- [ ] Given subscriptions exceed a connection's limit (Kite: 3,000 instruments/connection, 3 connections/key; Upstox: per-mode limits per docs), then the engine shards across connections and never exceeds limits (unit test with limit table).
- [ ] Given a socket drop, then the engine reconnects with exponential backoff (0.5 s → 30 s cap, jitter) and resubscribes all instruments; gap is logged with start/end timestamps.
- [ ] Engine normalize → NATS publish latency p99 < 200 µs at 20,000 ticks/s (benchmark in CI).
- [ ] Browser receives conflated updates; at 1,000 visible rows updating 4 Hz the UI holds ≥ 55 fps (Playwright perf trace).
- [ ] Entitlement: a user receives live ticks only via a broker account they are mapped to that has a data entitlement; otherwise the panel shows `No market-data entitlement — ask admin to map a data-enabled account`.

#### F-MD-3: Core market panels
Functions: `Q` quote, `CH` chart (candles, volume, indicators, drawing), `DOM` depth (5 levels; 30 where Upstox `full_d30` is available), `WL` watchlist, `OC` option chain (LTP, OI, ΔOI, IV, Greeks, PCR, max-pain), `OI` OI-by-strike & build-up, `MOV` gainers/losers/volume shockers for an index, `HM` sector heatmap, `TS` time & sales (from tick stream).
**Acceptance criteria (each):**
- [ ] `CH` loads 1 year of daily or 30 days of 1-min bars in < 1.5 s from ClickHouse and appends live bars from ticks.
- [ ] `OC` computes IV (Black-76 off the synthetic/near futures for index options) and Greeks server-side; values within 1% of reference implementation tests.
- [ ] `OC` row click with `B`/`S` key opens a pre-filled order ticket for that contract.
- [ ] Empty state: if market closed, panels show last session's close and a `MARKET CLOSED` tag.

#### F-MD-4: Recording
**Description:** All subscribed ticks persist to ClickHouse (`ticks`), plus 1-s and 1-min bars built in-engine.
**Acceptance criteria:**
- [ ] Given 6.25 hours of a full session, then row count in `ticks` equals engine-published count ± 0.01%.
- [ ] Recorder backpressure never blocks the engine hot path (bounded queue; drops counted in metric `recorder_dropped_total` and alerted if > 0).

### Module C — Execution & OMS

#### F-EX-1: Order ticket
**Description:** Keyboard order ticket (`B`/`S` hotkeys, or `ORD` function). Fields: account, instrument, side, qty (lots for F&O), order type (LIMIT, MARKET, SL, SL-M), product (CNC/MIS/NRML), validity (DAY/IOC), price, trigger, optional bracket (stop-loss + target). Shows pre-trade preview: notional, broker margin estimate, risk-gate dry-run result, limit headroom, reward:risk (if bracket).
**User flow:**
1. Trader presses `B` on a selected instrument.
2. System opens ticket pre-filled (account = trader's default, qty = 1 lot, LIMIT at LTP).
3. Trader edits and presses `Ctrl+Enter`.
4. System runs risk gate; if pass, routes to broker; ticket closes; blotter shows `PENDING → OPEN`.
5. If fail, ticket stays open with red reason list (e.g. `R-LOSS-DAILY: daily loss limit ₹25,000 would be breached`).
**Acceptance criteria:**
- [ ] Given qty above exchange freeze quantity, then the engine auto-slices into child orders ≤ freeze qty, each passing the gate, and shows them as one parent in the blotter.
- [ ] Given MARKET/SL-M, then the adapter sends the broker's market-protection parameter as configured (`[DECISION NEEDED: §13 #8]`).
- [ ] Given a bracket, then the protective stop is placed **at the broker** immediately after entry fill (survives engine crash); target is a resting LIMIT; when one fills, the other is cancelled (OCO) within 1 s.
- [ ] Given double `Ctrl+Enter` within 500 ms, then only one order is sent (idempotency key).
- [ ] Given `analyst` role, then the ticket's account selector shows only `PAPER`.

#### F-EX-2: OMS state machine
States and transitions (canonical, all transitions recorded as `order_event`):
```
NEW → RiskCheck → RISK_REJECTED (terminal)
NEW → RiskCheck → APPROVED → Send → SENT
SENT → BrokerAck → OPEN | BROKER_REJECTED (terminal)
OPEN → PartialFill → PARTIALLY_FILLED
OPEN | PARTIALLY_FILLED → Fill(complete) → FILLED (terminal)
OPEN | PARTIALLY_FILLED → CancelReq → CANCEL_PENDING → CANCELLED (terminal)
OPEN | PARTIALLY_FILLED → ModifyReq → (risk check) → MODIFY_PENDING → OPEN
SENT → Timeout(5s, no ack) → UNKNOWN → Reconcile → (actual broker state)
```
**Acceptance criteria:**
- [ ] Every transition not listed above is rejected and logged as `OMS_ILLEGAL_TRANSITION` (property-based test).
- [ ] Given `UNKNOWN`, then reconciliation queries the broker order book within 2 s and resolves state; no duplicate re-send ever happens for an `UNKNOWN` order.
- [ ] Every 60 s and on reconnect, engine reconciles open orders and positions against broker; mismatches create a `RECON_MISMATCH` alert with diff.

#### F-EX-3: Broker adapters (Zerodha Kite Connect, Upstox v3)
**Description:** C++ adapters implementing `IBrokerAdapter` (`ARCHITECTURE.md §5`): login token handling, place/modify/cancel, order-update stream, positions/holdings/margins, historical candles. Each account's HTTP and WS sockets bind to that account's configured source IP.
**Acceptance criteria:**
- [ ] Given account `A` bound to IP `x.x.x.x` that is not present on the host, then the engine refuses to start `A`'s session with `STATIC_IP_NOT_ON_HOST` and alerts admin.
- [ ] Per-account order rate is capped by a token bucket at `min(broker_limit, desk_ops_cap)`, default desk cap **8 orders/s per account per exchange** (below the 10 OPS registration threshold).
- [ ] Every order carries the strategy tag (`manual`, `strat:<id>`, `ai-draft`) in the broker's tag field (truncated to broker max length) and in `order.tag`.
- [ ] Adapter contract tests run against recorded broker responses (VCR fixtures) — no live calls in CI.
- [ ] Access tokens are held only in engine memory and encrypted in Postgres; never logged (log scrubber test).

#### F-EX-4: Paper trading (Sim broker)
**Description:** `SimBroker` adapter fills against live ticks: LIMIT fills when LTP trades through price (or at touch with configurable queue-position probability); MARKET fills at opposite best + configured slippage ticks. Same OMS/risk path as live.
**Acceptance criteria:**
- [ ] Given a BUY LIMIT at 100 and LTP prints 99.95, then the order fills at 100.
- [ ] Paper P&L includes the full Indian cost model (`STRATEGY_SDK_AND_BACKTEST.md §5`).
- [ ] Paper and live orders are visually distinct (`PAPER` tag, cyan) in every panel.

#### F-EX-5: Blotter, positions, P&L
Functions: `BLT` (orders + trades), `POS` (net positions by account/trader/strategy with MTM, realized, Greeks), `PNL` (intraday P&L curve per trader/desk).
**Acceptance criteria:**
- [ ] Positions derive from fills (event-sourced); EOD snapshot reconciles with broker positions/holdings; mismatch → alert.
- [ ] MTM updates on every conflated tick; P&L equals Σ(fills) + MTM − costs (unit-tested).

### Module D — Risk (pre-trade gate + Aladdin-style analytics)
Full rule catalogue, limit hierarchy and formulas: `RISK_ENGINE.md`.

#### F-RK-1: Non-bypassable pre-trade risk gate
**Description:** In-process C++ gate. Every order intent (manual, strategy, AI-drafted, auto-slice child, modify) passes the ordered rule chain; first hard failure rejects. Rules include: kill-switch state, role/account entitlement, market session, instrument allowed (F&O ban, desk restricted list), price band / fat-finger vs LTP and circuit limits, max order qty/value, freeze-qty slicing, position limits (qty/notional/lots), gross/net exposure, daily loss limit (realized + MTM), max open orders, OPS throttle, duplicate-order window, margin headroom (cached broker margin), Greeks limits (net delta/vega per trader), short-option controls, discipline rules (loss-streak lockout, minimum reward:risk on brackets).
**Acceptance criteria:**
- [ ] Gate evaluation p99 < 20 µs (Google Benchmark, 50 rules, 1,000 positions).
- [ ] There is no code path from any intent source to `IBrokerAdapter::place` that skips the gate (enforced by type: adapters accept only `ApprovedOrder`, constructible only by `RiskGate` — compile-time test).
- [ ] Each rejection has a stable reason code (`R-*`) and human message; reason codes listed in `RISK_ENGINE.md §3`.
- [ ] Limit changes take effect for the next intent (no restart) and are audited with old/new values.

#### F-RK-2: Kill switch
**Description:** Scopes: desk, account, trader, strategy. Actions: (1) block new intents, (2) cancel all open orders, (3) optional flatten (square-off with marketable LIMIT orders at protection band). Triggers: manual (`Ctrl+Shift+K` + confirm, or `KILL` function), automatic (daily loss limit, desk drawdown, order-reject storm > N/min, broker-disconnect > N s with open positions — configurable).
**Acceptance criteria:**
- [ ] Desk kill: all open orders on all accounts cancelled and new intents blocked within 2 s (staging test with 20 open orders).
- [ ] Release requires `risk_manager`/`admin` re-auth and a reason; audited.
- [ ] Flatten is never automatic by default; it requires explicit config per scope. `[DECISION NEEDED: §13 #10]`

#### F-RK-3: Portfolio risk analytics
**Description:** Python `quant` service computes per-trader, per-account, per-strategy and desk-level: Greeks (Black-76 index options; Black-Scholes with dividend yield for stock options), beta-adjusted delta (to NIFTY), sector/industry exposure, concentration, scenario grid (spot −10%…+10% × IV −30%…+30%), 1-day 95/99% historical VaR (250-day window) and parametric VaR, named historical stress scenarios, limit utilization. Functions: `RISK`, `SCN`, `VAR`, `EXP`.
**Acceptance criteria:**
- [ ] Desk risk refresh ≤ 5 s for 500 positions.
- [ ] Greeks match reference test vectors within 1e-4 (relative).
- [ ] Historical VaR reproduces a hand-computed fixture exactly.
- [ ] Scenario grid cell P&L = full revaluation (not delta-gamma approximation) for options.

#### F-RK-4: Unified desk risk model (Aladdin-style)
**Description:** One risk model across all accounts and brokers: positions mapped to factors (market beta, sector, size, momentum, rates-sensitivity tag from the knowledge graph), so the desk sees *what it is really exposed to*, not just per-account P&L. Factor returns estimated from Nifty 500 universe daily data (cross-sectional regression, 1-yr window). Function: `FAC`.
**Acceptance criteria:**
- [ ] Factor exposures sum consistently (desk = Σ traders) to 1e-6.
- [ ] Factor model rebuild nightly < 10 min on the reference VM.
- [ ] `FAC` shows top-5 factor exposures and their contribution to VaR.

### Module E — Research: Data, Strategies, Backtests
Full spec: `STRATEGY_SDK_AND_BACKTEST.md`.

#### F-RS-1: Historical data download manager
**Description:** `DL` function and API to schedule downloads (instrument set × interval × date range × source broker). Rate-limit aware, resumable, deduplicated; writes partitioned Parquet to the local data lake and registers datasets in ClickHouse. Supports expired F&O contracts via Upstox expired-instruments APIs.
**Acceptance criteria:**
- [ ] Given a job for 50 symbols × 2 years × 1-min, then it completes without exceeding broker rate limits (no HTTP 429 in logs) and is resumable after a kill -9.
- [ ] Re-running the same job downloads 0 new bars (idempotent).
- [ ] Corporate-action adjustment factors stored separately; raw data never mutated; adjusted view computed on read.

#### F-RS-2: Strategy SDK (Python)
**Description:** One `Strategy` base class with `on_start/on_bar/on_tick/on_order/on_fill/on_timer/on_stop`; context API for data, indicators, orders, positions, params, logging. Same code runs in `backtest`, `paper`, `live` modes.
**Acceptance criteria:**
- [ ] The reference strategies (`sma_cross`, `orb_breakout`, `short_straddle_with_stop`) run unchanged in all three modes.
- [ ] Strategies cannot import network or broker modules (sandbox import allow-list; test asserts `ImportError`).
- [ ] Strategy orders pass through the same risk gate (backtest uses the same C++ gate via nanobind).

#### F-RS-3: Backtest engine
**Description:** Event-driven backtester using the C++ core (`vyuha_core`) in-process via nanobind with a deterministic clock; bar- and tick-level fill models; Indian cost model with effective-dated rates; parameter sweeps; walk-forward splits. Function: `BT`.
**Acceptance criteria:**
- [ ] Same inputs + seed → byte-identical results.
- [ ] 2 years × 50 symbols × 1-min bars SMA-cross completes < 60 s on the reference VM.
- [ ] Report: CAGR, Sharpe, Sortino, max DD, Calmar, win rate, profit factor, expectancy (R), exposure, turnover, total costs by component, equity curve, drawdown chart, trade list (CSV export).
- [ ] Look-ahead guard: a strategy reading a bar's close inside `on_bar` cannot place an order that fills on that same bar's close unless `fill_model=close` is explicitly set (test).

#### F-RS-4: Strategy lifecycle
States: `DRAFT → BACKTESTED → PAPER → PENDING_APPROVAL → LIVE ↔ PAUSED → RETIRED`. Live requires `risk_manager` approval with per-strategy limits set.
**Acceptance criteria:**
- [ ] A strategy cannot enter `LIVE` without ≥ 1 completed backtest, ≥ 5 paper trading sessions (configurable), and approval record.
- [ ] Editing a live strategy's code creates a new version in `PAPER`; the live version keeps running until replaced by approval.

### Module F — Context Engine (Aladdin-style knowledge graph)
Full spec: `CONTEXT_ENGINE_AND_AI.md`.

#### F-CX-1: Ingestion
Sources (pluggable `EventSource`): configurable RSS/Atom feeds (news), SEBI and RBI RSS, broker corporate-action data, manual upload (CSV/PDF). Exchange announcement adapters ship **disabled** pending ToS review (`§13 #7`).
**Acceptance criteria:**
- [ ] Ingest is idempotent by URL/GUID hash; duplicate stories across feeds clustered (embedding cosine > 0.92) into one `story`.
- [ ] Source content is stored as data and never interpreted as instructions (prompt-injection test suite passes).

#### F-CX-2: Extraction & entity resolution
LLM extracts entities (companies, people, sectors, commodities, macro variables), event type (from a fixed taxonomy), sentiment (−1…1), magnitude, and time horizon; entities are resolved to `instrument`/`company` nodes using symbol/ISIN/alias tables + embeddings (bge-m3).
**Acceptance criteria:**
- [ ] On the labelled eval set (200 Indian market headlines, `intel/evals/`), entity-resolution F1 ≥ 0.90 and event-type accuracy ≥ 0.85.
- [ ] Extraction output validated against a JSON schema; invalid output retried once then quarantined.

#### F-CX-3: Knowledge graph
Neo4j graph: `Company, Instrument, Sector, Industry, Index, Person, Event, Story, MacroVariable, Commodity, Trader, Strategy, Account` with edges `ISSUES, IN_SECTOR, CONSTITUENT_OF{weight}, MENTIONS{sentiment}, AFFECTS{direction,confidence}, SENSITIVE_TO{beta}, SUPPLIER_OF, COMPETITOR_OF, HOLDS{qty,delta}` — schema in `DATA_MODEL.md §4`.
**Acceptance criteria:**
- [ ] Seed load builds Nifty 500 companies, sectors, index constituents and weights.
- [ ] `HOLDS` edges sync from positions within 10 s of a fill.

#### F-CX-4: Context mapping & alerts
For each new event, traverse ≤ 2 hops from affected entities to `HOLDS` edges; score impact = |exposure| × edge-weight × confidence × magnitude; alert affected traders/risk manager above threshold. Function: `CTX` (graph view centred on a security or position, with "why" path), `N` (news filtered to my exposures).
**Acceptance criteria:**
- [ ] Given a story affecting `HDFCBANK` and a trader long `BANKNIFTY` futures, then the trader receives an alert with path `Story → HDFCBANK → CONSTITUENT_OF(BANKNIFTY, w) → HOLDS` within 60 s of ingestion.
- [ ] Alerts deduplicate per (story, trader) and respect quiet-hours / threshold settings.

### Module G — AI Copilot

#### F-AI-1: Copilot panel (`AI`)
**Description:** Chat panel aware of the focused security, link group and user role. Uses tool calling over the Vyuha MCP server: `get_positions, get_orders, get_risk_summary, run_scenario, get_var, get_quote, get_candles, get_option_chain, search_news, graph_paths, explain_pnl, run_backtest, get_backtest_report, draft_order_ticket, get_limits`.
**Acceptance criteria:**
- [ ] Every answer that cites a number shows which tool call produced it (expandable trace).
- [ ] `draft_order_ticket` opens a pre-filled ticket in the UI; the user must press `Ctrl+Enter`; there is **no** tool that places, modifies or cancels orders (test enumerates tool list).
- [ ] Tools enforce the caller's RBAC (trader cannot read other traders' positions through the copilot).
- [ ] Copilot eval suite (`intel/evals/copilot_golden.yaml`, 50 questions) ≥ 85% pass before release.
- [ ] Each conversation and tool call is persisted and audited.

#### F-AI-2: Daily briefs
Pre-open (08:45 IST) and post-close (16:00 IST) briefs per trader: overnight events touching positions, F&O ban changes, results calendar for held names, risk headroom, yesterday's P&L attribution.
**Acceptance criteria:**
- [ ] Brief generated ≤ 2 min after schedule; every claim links to its source story/tool output.

#### F-AI-3: External MCP access
The same MCP server can be connected from Claude Desktop/Claude Code by a user with a personal access token, scoped read-only.
**Acceptance criteria:**
- [ ] PAT scopes default to read-only; `draft_order_ticket` unavailable over external MCP.

### Module H — Compliance, Audit, Admin
Full spec: `COMPLIANCE.md`.

#### F-CO-1: Audit log
Append-only, hash-chained `audit_event` (each row stores `sha256(prev_hash || canonical_json(payload))`); covers auth, admin, limits, orders, kill switch, strategy lifecycle, copilot tool calls. Function `AUD` + CSV export. Retention default 5 years.
**Acceptance criteria:**
- [ ] `vyuha audit verify` detects any modified/deleted row (test tampers a row).
- [ ] DB role used by services has INSERT-only on `audit_event` (no UPDATE/DELETE grants).

#### F-CO-2: Algo-framework controls
Static-IP binding per account, OPS cap, strategy tagging, daily session expiry handling, restricted-list, F&O-ban enforcement, order-to-trade ratio monitor.
**Acceptance criteria:** covered by F-EX-3 and F-RK-1 tests plus a compliance report (`COMP` function) listing per-account OPS max, rejected orders, tag coverage = 100%.

#### F-CO-3: Admin console
Users, roles, broker accounts (API key/secret encrypted), static-IP bindings, account↔user mapping, data entitlements, limits, cost schedules, feeds.
**Acceptance criteria:**
- [ ] Secrets are write-only in UI (never re-displayed); stored AES-256-GCM encrypted with a key from `VYUHA_MASTER_KEY`.

### Module I — Desk Agents ("AI desk staff")
Full spec: `DESK_AGENTS.md`. These are runtime AI agents that mirror trading-firm roles. They **analyse, draft, monitor and alert**. Humans and the risk gate make every trading decision.

#### F-AG-1: Agent runtime
**Description:** Agent definitions in YAML (role prompt, model, allowed tools, triggers, audience scope, budget) are loaded by an agent runner in `intel`. The runner executes bounded tool loops on NATS event triggers and IST schedules (exchange-holiday aware). Each run is audited and stored (`agent_run`) and produces notes/alerts (`agent_note`).
**User flow:**
1. A trigger fires (e.g. `risk.util` ≥ 70% for trader Meera).
2. The runner checks cooldown and budget, then starts `risk_analyst` with the trigger payload as data.
3. The agent calls allowed tools (`get_risk_summary`, `run_scenario`, …), then writes a note.
4. The note appears in `DESK` and as an alert to Meera and the risk manager, with a tool-trace expander.
**Acceptance criteria:**
- [ ] Given a definition listing a tool outside the MCP registry or a mutating tool, when the runner loads it, then the load fails with `AGENT_TOOL_FORBIDDEN` (test).
- [ ] Given the same trigger key within `cooldown_s`, then the second run is `skipped_cooldown`.
- [ ] Given the agent's daily token budget is exhausted, then further runs are `skipped_budget`, the agent shows `BUDGET HIT`, and the admin gets an alert.
- [ ] Given an exchange holiday, then scheduled runs don't fire.
- [ ] Every run stores trigger, prompt hash, tool calls, tokens and estimated cost. p95 run time ≤ 90 s (hard timeout).
- [ ] An admin disabling an agent takes effect within 5 s, with no restart.

#### F-AG-2: MVP roster — Desk Head, Risk Analyst, Macro Analyst
- `desk_head`: produces the pre-open (08:45 IST) and post-close (16:00 IST) briefs (replacing F-AI-2's generator) and routes `@desk` questions. It is the only agent allowed to call other agents (depth 1).
- `risk_analyst`: pre-breach warnings at 70/85/95% utilisation, post-fill risk notes, 15-minute intraday checks, and kill-switch **requests** (a critical alert with one-click action for a risk manager).
- `macro_analyst`: event notes for macro/policy/FX/commodity/global events, with knowledge-graph impact paths and a suggested `SCN` scenario.
**Acceptance criteria:**
- [ ] Each MVP agent passes its eval suite (`intel/evals/agents/*.yaml`) at ≥ 85%, with ≤ 10% false alerts on "stay silent" fixtures and 100% on injection fixtures.
- [ ] Every number in a note is backed by a `refs` entry pointing to a tool result or story.
- [ ] A trader only sees notes whose `audience_scope` covers them. A risk manager sees all.

#### F-AG-3: `DESK` panel and `@agent` mentions
**Description:** The `DESK` function shows the roster (status, runs today, tokens/cost) and an activity feed filterable by agent, security and trader. In `AI`, `@risk_analyst <question>` asks that agent directly, and `@desk` routes to the Desk Head.
**Acceptance criteria:**
- [ ] A keyboard-only Playwright test opens `DESK`, filters by agent, and expands a tool trace.
- [ ] 👍/👎 feedback on a note is stored and appears in the eval dashboard.

#### F-AG-4 (stretch in M8): Research Analyst and Quant Researcher
- `research_analyst`: company notes on results/corporate events for held and watchlist names.
- `quant_researcher`: writes `DRAFT` strategy versions and runs backtests/paper, with an overfitting report. It can never move a strategy past `PAPER`.
**Acceptance criteria:**
- [ ] `quant_researcher` output is always a new `strategy_version` on a strategy in `DRAFT`. An attempt to transition state returns `AGENT_ACTION_FORBIDDEN`.

---

## 5. Out of Scope (Post-MVP)

| Feature | Rationale for deferral |
|---------|------------------------|
| Brokers beyond Zerodha & Upstox (Angel One, Dhan, Fyers, Shoonya…) | Adapter interface ready; add after core is stable |
| MCX commodities, currency derivatives, crypto, US stocks | Not chosen for MVP; instrument model is asset-agnostic |
| Native desktop app (Electron) | Web first; wrapper later |
| Paid data vendors (TrueData, Global Datafeeds) adapters | Free broker feeds first; interface `IMarketDataSource` ready |
| ML signal pipeline, feature store, model registry (MLflow), ONNX inference in strategies | Research-heavy; after backtester is trusted |
| Monte Carlo VaR, full multi-factor (value/quality) model needing fundamentals | Needs fundamentals data source |
| Supply-chain/competitor edges extracted from annual reports | Needs document pipeline + eval |
| Multi-desk / multi-tenant, SSO (OIDC) | Single desk per deployment |
| Exchange registration support for > 10 OPS algos | Desk capped below threshold |
| Broker-native GTT management UI | Brackets handled by engine + resting broker stop |
| Mobile app, Excel add-in | Not core |
| Desk agents: Execution Assistant, Compliance Officer, Ops Reconciler, Trading Coach | Roster spec ready in `DESK_AGENTS.md`; add after the MVP agents prove useful and affordable |
| Any agent that trades, changes limits or promotes strategies autonomously | Never in scope: keeps algos white-box and human-approved |
| Chat/IB messaging between traders | Use existing tools |
| Kubernetes deployment | Single-VM Docker Compose suffices for ≤ 10 users |

---

## 6. Data Models (summary — full DDL in `DATA_MODEL.md`)

```typescript
type User = { id: uuid; username: string /*unique, 3-32*/; display_name: string; role: 'admin'|'risk_manager'|'trader'|'analyst'; password_hash: string /*argon2id*/; totp_secret_enc: bytes; is_active: boolean; created_at: timestamptz; };
type BrokerAccount = { id: uuid; broker: 'zerodha'|'upstox'|'sim'; client_code: string; holder_name: string; api_key_enc: bytes; api_secret_enc: bytes; static_ip: inet|null /*required unless sim*/; backup_static_ip: inet|null; data_entitled: boolean; status: 'disconnected'|'awaiting_login'|'connected'|'error'; created_at: timestamptz; };
type AccountUserMap = { account_id: uuid; user_id: uuid; can_trade: boolean; can_view: boolean; is_default: boolean; };
type Instrument = { id: bigint; exchange: 'NSE'|'BSE'|'NFO'|'BFO'; segment: 'EQ'|'FUT'|'OPT'|'IDX'; symbol: string; tradingsymbol: string; isin: string|null; underlying_id: bigint|null; expiry: date|null; strike: numeric|null; option_type: 'CE'|'PE'|null; lot_size: int; tick_size: numeric; freeze_qty: int|null; fo_ban: boolean; kite_token: bigint|null; upstox_key: string|null; valid_from: date; valid_to: date|null; };
type Order = { id: uuid; parent_id: uuid|null; client_order_id: string /*unique idempotency*/; account_id: uuid; user_id: uuid; strategy_id: uuid|null; source: 'manual'|'strategy'|'ai_draft'|'system'; instrument_id: bigint; side: 'BUY'|'SELL'; qty: int; filled_qty: int; order_type: 'LIMIT'|'MARKET'|'SL'|'SL-M'; product: 'CNC'|'MIS'|'NRML'; validity: 'DAY'|'IOC'; price: numeric|null; trigger_price: numeric|null; status: OrderStatus; broker_order_id: string|null; tag: string; is_paper: boolean; created_at: timestamptz; updated_at: timestamptz; };
type OrderEvent = { id: bigint; order_id: uuid; seq: int; from_status: string; to_status: string; reason_code: string|null; payload: jsonb; ts: timestamptz; };
type Fill = { id: uuid; order_id: uuid; broker_trade_id: string; qty: int; price: numeric; ts: timestamptz; };
type RiskLimit = { id: uuid; scope: 'desk'|'account'|'trader'|'strategy'|'instrument'; scope_id: uuid|null; key: string /*e.g. max_daily_loss*/; value: numeric; hard: boolean; updated_by: uuid; updated_at: timestamptz; };
type KillSwitch = { scope: string; scope_id: uuid|null; active: boolean; actions: string[]; reason: string; set_by: uuid; set_at: timestamptz; };
type Strategy = { id: uuid; name: string; owner_id: uuid; state: StrategyState; current_version_id: uuid; };
type StrategyVersion = { id: uuid; strategy_id: uuid; code_sha256: string; code: text; params_schema: jsonb; created_at: timestamptz; };
type BacktestRun = { id: uuid; strategy_version_id: uuid; params: jsonb; dataset_ids: uuid[]; seed: int; status: 'queued'|'running'|'done'|'failed'; metrics: jsonb; created_by: uuid; created_at: timestamptz; };
type Story = { id: uuid; source: string; url: string; title: string; body: text; published_at: timestamptz; embedding: vector(1024); extraction: jsonb; };
type ContextAlert = { id: uuid; story_id: uuid; user_id: uuid; score: numeric; path: jsonb; acked_at: timestamptz|null; };
type AuditEvent = { id: bigint; ts: timestamptz; actor_id: uuid|null; action: string; entity: string; entity_id: string; payload: jsonb; prev_hash: bytea; hash: bytea; };
type Agent = { id: string /*e.g. risk_analyst*/; title: string; definition: jsonb; prompt_sha256: string; enabled: boolean; disabled_reason: string|null; updated_at: timestamptz; };
type AgentRun = { id: uuid; agent_id: string; parent_run_id: uuid|null; trigger_kind: 'event'|'schedule'|'user'|'agent'; trigger_key: string; trigger_payload: jsonb; status: 'running'|'done'|'no_note'|'failed'|'skipped_budget'|'skipped_cooldown'; tool_calls: jsonb; input_tokens: int; output_tokens: int; cost_usd_est: numeric; started_at: timestamptz; finished_at: timestamptz|null; };
type AgentNote = { id: uuid; run_id: uuid; agent_id: string; kind: 'note'|'alert'|'brief'|'strategy_draft'|'ticket_draft'; severity: 'info'|'warn'|'critical'; audience_scope: string; instrument_ids: bigint[]; title: string; body_md: text; refs: jsonb; embedding: vector(1024); feedback: -1|1|null; created_at: timestamptz; };
```
(DDL: `db/migrations/000002_desk_agents.up.sql`.)

**Relationships:** User ↔ BrokerAccount (many-to-many via AccountUserMap); Order → Account, User, Strategy?, Instrument; Order 1→N OrderEvent, Fill; Strategy 1→N StrategyVersion 1→N BacktestRun; Story 1→N ContextAlert; Agent 1→N AgentRun 1→N AgentNote; AgentRun → parent AgentRun (desk_head delegation).

---

## 7. API / Backend Contract (summary — full spec in `API.md`)

Gateway base: `https://<host>/api/v1`, JSON, bearer JWT. Real-time: `wss://<host>/ws` (protobuf frames, schema `proto/vyuha/ws/v1/ws.proto`).

| Method & Path | Auth | Purpose |
|---|---|---|
| `POST /auth/login` | none | password → `{mfa_required, mfa_token}` |
| `POST /auth/mfa` | mfa_token | TOTP → access token + refresh cookie |
| `POST /auth/refresh`, `POST /auth/logout` | cookie | rotate / revoke |
| `GET /instruments/search?q=` | any | fuzzy instrument search |
| `GET /marketdata/candles` | any+entitled | historical bars from ClickHouse |
| `GET /optionchain/{underlying}?expiry=` | any+entitled | snapshot chain with IV/Greeks |
| `POST /orders` | trader+ | create intent (idempotent on `client_order_id`) |
| `POST /orders/{id}/modify`, `POST /orders/{id}/cancel` | trader+ | |
| `POST /orders/preview` | trader+ | risk-gate dry run + margin estimate |
| `GET /orders`, `GET /positions`, `GET /pnl` | scoped | |
| `GET/PUT /risk/limits` | risk_manager+ for PUT | |
| `POST /risk/kill`, `POST /risk/kill/release` | risk_manager+ (re-auth for release) | |
| `GET /risk/summary`, `POST /risk/scenario`, `GET /risk/var`, `GET /risk/factors` | scoped | |
| `POST /data/jobs`, `GET /data/jobs/{id}` | analyst+ | download jobs |
| `CRUD /strategies`, `POST /strategies/{id}/backtests`, `POST /strategies/{id}/transition` | analyst+/risk_manager for LIVE | |
| `GET /news`, `GET /context/graph?entity=`, `GET /context/alerts` | scoped | |
| `POST /copilot/sessions/{id}/messages` (SSE stream) | any | |
| `CRUD /admin/users`, `/admin/accounts`, `/admin/feeds`, `/admin/costs` | admin | |
| `GET /audit?from=&to=`, `GET /audit/export.csv` | risk_manager+ | |
| `GET /agents`, `GET /agents/{id}/runs`, `GET /agent-notes?agent=&instrument_id=&cursor=` | scoped | Desk agents roster, runs, feed |
| `POST /agents/{id}/ask` (SSE) | any (audience-scoped) | Ask one agent directly (`@agent`) |
| `PUT /admin/agents/{id}` 🔐 | admin | Enable/disable, budget, triggers |
| `POST /agent-notes/{id}/feedback` | any | 👍/👎 |
| `GET /brokers/{account_id}/login-url`, `GET /brokers/callback/{broker}` | trader (own acct) | daily broker login |

Errors: RFC 9457 problem+json `{type, title, status, detail, code}`; risk rejects → `422` with `code: "R-…"`.

---

## 8. Tech Stack (summary — exact versions and rationale in `TECH_STACK.md`)

| Layer | Technology |
|---|---|
| Hot path engine (MD, OMS, risk gate, broker adapters, sim) | **C++23**, GCC 14 / Clang 18+, CMake + Ninja, vcpkg; Boost.Asio/Beast, OpenSSL 3, simdjson, protobuf, gRPC, nats.c, Quill logging, nanobind (Python bindings), GoogleTest, Google Benchmark |
| Gateway (auth, REST, WebSocket fan-out, recorder) | **Go** (latest stable), stdlib `net/http`, `coder/websocket`, `pgx` + `sqlc`, `oapi-codegen`, `golang-jwt`, `pquerna/otp` |
| Quant & AI services | **Python 3.13**, uv, FastAPI, Pydantic v2, Polars, NumPy, Numba, DuckDB, SciPy, Anthropic SDK, MCP Python SDK, sentence-transformers (bge-m3), neo4j driver |
| Frontend terminal | **React 19 + TypeScript + Vite**, Dockview, AG Grid Community, TradingView Lightweight Charts v5, Apache ECharts, cmdk, Zustand, TanStack Query, `@bufbuild/protobuf`, Tailwind CSS v4 |
| Messaging | **NATS JetStream** (streams + KV last-value cache); gRPC for engine control |
| OLTP DB | **PostgreSQL 17** + pgvector |
| Time-series / analytics | **ClickHouse** |
| Knowledge graph | **Neo4j 5 Community** |
| Data lake | Parquet on local disk (`/data/lake`), queried via DuckDB/Polars and ClickHouse `file()` |
| LLM | Anthropic Claude via API (provider-abstracted; OpenAI-compatible local endpoint optional) |
| Observability | OpenTelemetry → Prometheus + Grafana + Loki |
| Deployment | Docker Compose on one Mumbai-region VM with one static public IP per broker account; Caddy (TLS); Tailscale for access |
| CI | GitHub Actions; Buf for proto lint/breaking; sanitizers (ASan/UBSan/TSan) |
| Desktop (post-MVP) | Electron (Chromium-based, same approach as Bloomberg's Chromium/Node runtime) |

**Environment variables required:** see `.env.example` (authoritative). Key: `VYUHA_MASTER_KEY`, `POSTGRES_URL`, `CLICKHOUSE_URL`, `NATS_URL`, `NEO4J_URI/USER/PASSWORD`, `ANTHROPIC_API_KEY`, `LLM_MODEL_FAST`, `LLM_MODEL_SMART`, `JWT_SIGNING_KEY_PATH`, `PUBLIC_BASE_URL`, `DATA_LAKE_DIR`, `ENGINE_GRPC_ADDR`, `VYUHA_ENV` (`dev|staging|prod`), `LIVE_TRADING_ENABLED` (default `false`).

---

## 9. Project Structure

```
vyuha/
├── CLAUDE.md                     # agent rules (read first)
├── README.md
├── .env.example
├── Makefile                      # make bootstrap C=… | up PROFILES=… | test | lint | verify | clean-space
├── .claude/                      # agents, commands, settings, hooks
├── docs/                         # this PRD + specs + ADRs
├── proto/vyuha/                  # protobuf contracts (Buf)
│   ├── common/v1/  marketdata/v1/  orders/v1/  risk/v1/  engine/v1/  ws/v1/
├── engine/                       # C++23 core (CLAUDE.md inside)
│   ├── CMakeLists.txt  vcpkg.json
│   ├── include/vyuha/{core,md,oms,risk,broker,sim,util}/
│   ├── src/{core,md,oms,risk,broker/{kite,upstox,sim},bus,app}/
│   ├── bindings/python/          # nanobind module vyuha_core
│   ├── tests/  bench/  fixtures/vcr/
├── gateway/                      # Go (CLAUDE.md inside)
│   ├── cmd/{gateway,recorder,vyuhactl}/
│   ├── internal/{auth,rbac,api,ws,enginec,audit,store,crypto}/
│   ├── api/openapi.yaml
│   └── db/queries/*.sql          # sqlc
├── db/
│   ├── migrations/               # Postgres SQL (golang-migrate) — single source of truth
│   ├── clickhouse/               # ClickHouse DDL
│   └── neo4j/                    # constraints + seed Cypher
├── quant/                        # Python: risk analytics, data downloader, backtest, strategy host
│   ├── pyproject.toml
│   ├── src/vyuha_quant/{api,risk,pricing,data,backtest,costs,strategy_host,factors}/
│   ├── sdk/vyuha/strategy/       # public Strategy SDK
│   ├── strategies/examples/
│   └── tests/
├── intel/                        # Python: ingestion, extraction, KG, context mapping, copilot, MCP server
│   ├── pyproject.toml
│   ├── src/vyuha_intel/{ingest,extract,resolve,graph,context,copilot,mcp_server,briefs,agents}/
│   ├── agents/                   # desk agent YAML definitions + prompts/
│   ├── evals/                    # incl. evals/agents/
│   └── tests/
├── web/                          # React terminal (CLAUDE.md inside)
│   ├── src/{app,shell,commands,panels/<FUNC>/,components,lib/{ws,proto,api},stores,theme}/
│   └── e2e/
└── deploy/
    ├── docker-compose.yml  docker-compose.dev.yml
    ├── caddy/Caddyfile  grafana/  prometheus/
    └── netplan-static-ips.example.yaml
```

---

## 10. Non-Functional Requirements

| Requirement | Target |
|---|---|
| Risk gate evaluation | p99 < 20 µs |
| Engine tick normalize → bus publish | p99 < 200 µs @ 20k ticks/s |
| Order intent → broker HTTP request written | p99 < 1 ms (broker RTT excluded; typically 10–100 ms, outside our control) |
| Tick → browser paint | p95 < 400 ms with 250 ms conflation |
| UI frame rate | ≥ 55 fps with 1,000 updating grid rows |
| Terminal initial load (LCP) | < 2.0 s on LAN |
| REST p95 (non-analytics) | < 100 ms |
| Desk risk refresh | ≤ 5 s for 500 positions |
| Kill switch (desk) | all cancels sent < 2 s |
| Availability in market hours (09:00–15:45 IST) | 99.9% per month for engine + gateway |
| Recovery | engine restart restores OMS state from Postgres + broker reconcile < 30 s |
| Security | OWASP ASVS L2 for gateway; secrets encrypted at rest; no secrets in logs; TOTP mandatory |
| Data retention | audit & orders 5 years; ticks 1 year hot in ClickHouse, then Parquet |
| Browser support | Chrome/Edge latest 2 (Chromium only for MVP), desktop ≥ 1366×768 |
| Accessibility | All functions keyboard-reachable; WCAG AA contrast for text on dark theme |
| Time | All timestamps stored UTC; displayed IST; exchange calendar aware (NSE holidays) |
| Desk agents | Run p95 ≤ 90 s; per-agent daily token caps; desk monthly AI cap enforced |
| Disk footprint (dev) | ≤ 15 GB after M0; ≤ 40 GB at MVP excluding market data; market data grows only with explicitly requested universes |

### 10.1 Lean footprint (just-in-time downloads)
The build must install and download **only what the current task needs**, when it needs it:
- **Per-task allow-list:** each task in `TASKS.md` lists its `Needs:` (toolchains, packages, images, data, models). Nothing outside that list is installed or downloaded.
- **Toolchains just-in-time:** C++ toolchain/vcpkg only from the first engine task, Go from the first gateway task, Node/pnpm from the first web task, Python/uv for quant/intel. `scripts/bootstrap.sh <component>` installs one component at a time.
- **Docker:** compose **profiles** (`core`, `graph`, `obs`). Only `core` (Postgres, NATS) starts at first; ClickHouse is added at M1, Neo4j (`graph`) at M6, Grafana/Prometheus/Loki/OTel (`obs`) at M3. Use slim/alpine images. Pull images only when their profile is first needed.
- **Dependencies:** no "kitchen-sink" installs. vcpkg uses minimal features (e.g. Boost components individually: `boost-asio`, `boost-beast`, not all of Boost). Python deps go in optional groups per service (`uv sync --group <x>`). Web deps are added per panel as needed. ML/heavy libs (`sentence-transformers`, `torch`) are installed only at M6, **CPU-only wheels**.
- **Models:** the embedding model (bge-m3, ~2 GB) is downloaded only at the first task that needs embeddings (M6.2), into `$MODELS_DIR`, once. No other ML models are downloaded.
- **Market data:** never bulk-download "everything". Default recording universe = instruments currently on screen + held + strategy universes. Historical downloads cover only the instruments, interval and date range in the job spec. Test fixtures are small and committed (< 5 MB total per service), generated from synthetic data where possible.
- **No mirrors:** shallow clones, no vendored third-party source trees, no sample datasets, no unused SDKs (e.g. don't install CUDA, Android or other toolchains).
- **Housekeeping:** `make clean-space` removes build dirs, caches (`uv cache prune`, `pnpm store prune`, `go clean -cache`, vcpkg buildtrees, dangling Docker images) and prints disk usage per component. CI caches only lockfile-keyed dependencies.

---

## 11. Design & UX Guidelines (full spec in `UI_SPEC.md`)

**Visual style:** Dense, dark, keyboard-first financial terminal, inspired by (not copying) Bloomberg — no Bloomberg names, logos or trademarked function codes.
**Color palette:** background `#000000`; panel `#0A0A0A`; grid lines `#1E1E1E`; primary text amber `#FFB000`; secondary text `#C8C8C8`; labels `#8A8A8A`; up `#00D26A`; down `#FF3B3B`; accent/link cyan `#3AB8FF`; warning `#FFD400`; paper-trade tag cyan `#00E5FF`; kill-switch red `#FF1744`.
**Typography:** `JetBrains Mono` for numbers/grids (tabular figures), `IBM Plex Sans Condensed` for labels; base 12 px, grid row 18 px, headers 13 px.
**Key UX principles:**
- Every action has a keyboard path; mouse is optional.
- Numbers never shift width (tabular mono); price changes flash 150 ms (green up / red down).
- Dangerous actions (live order, kill release, limit change) need an explicit confirm and show the account + live/paper state in red/cyan.
- Never hide a risk rejection: show reason code + limit + current value.
- Latency is visible: status bar shows data age; stale (> 3 s) values dim to 50% opacity.

---

## 12. Third-Party Integrations

| Service | Purpose | Auth method | SDK/API used |
|---|---|---|---|
| Zerodha Kite Connect | Orders, portfolio, ticks, historical | API key + daily `request_token` → `access_token` | REST v3 + Kite Ticker WS (binary), native C++ client |
| Upstox Developer API | Orders, portfolio, ticks (V3 protobuf), historical, expired instruments, option Greeks | OAuth2 code → access token (daily) | REST v2/v3 + Market Data Feed V3 + Portfolio Stream Feed, native C++ client |
| Anthropic API | Copilot, extraction, briefs | API key | `anthropic` Python SDK |
| News / regulator feeds | Context ingestion | none (public RSS) | `feedparser`/`httpx` |
| Tailscale (recommended) | Private access to the terminal | Tailscale auth | — |

---

## 13. Open Questions & Decisions

| # | Question | Recommended default | Owner | Due |
|---|----------|---------------------|-------|-----|
| 1 | Product name | Keep "Vyuha" | Jitesh | Before M0 |
| 2 | **Legal structure of the desk** — whose accounts, who may place orders on them, profit sharing; whether running Vyuha for others makes the operator an "algo provider" needing exchange empanelment | One account per account-holder trader; each account on its own static IP; desk = shared tooling + oversight. **Get a SEBI-practice lawyer's opinion before going live with other people's accounts.** | Jitesh + lawyer | Before M3 live pilot |
| 3 | Open-source license | **AGPL-3.0** (keeps hosted forks open; same choice as OpenBB/OpenAlgo) — alternative Apache-2.0 for max adoption | Jitesh | Before first public commit |
| 4 | Market-data sharing across desk | Per-account entitlement (no redistribution); licensed vendor adapter post-MVP | Jitesh | Before M1 |
| 5 | Build mode | Claude Code writes; Jitesh reviews every PR, hand-writes selected C++ learning modules (risk gate rules, SPSC ring) | Jitesh | Before M0 |
| 6 | LLM provider & data privacy | Anthropic API; positions sent to LLM only as aggregated numbers + tickers; local model option via OpenAI-compatible endpoint | Jitesh | Before M6 |
| 7 | Exchange announcements / F&O ban list source | Download the official daily files published by the exchange where its terms allow; announcement scraping disabled by default until ToS reviewed | Jitesh | Before M1 |
| 8 | MARKET/SL-M handling | Allow, always send broker market-protection param (default 2%); desk can disable MARKET orders | Jitesh | Before M3 |
| 9 | Hosting | One Mumbai-region VM (8 vCPU/32 GB/500 GB NVMe) with N elastic static IPs; provider = Jitesh's choice (AWS ap-south-1 / GCP asia-south1 / Azure Central India) | Jitesh | Before M3 |
| 10 | Auto-flatten on loss-limit breach | Off by default; block + cancel + alert; per-scope opt-in | Risk manager | Before M2 |
| 11 | Cost schedule values (STT, exchange txn, SEBI fee, stamp duty, GST, brokerage) | Seed from broker/NSE published rates at build time with `effective_from`; verify, never hard-code | Agent + Jitesh | M4 |
| 12 | Data budget | Free broker feeds; vendor adapters later | Jitesh | Confirmed "continue" → default |
| 13 | Desk-agent AI budget | Per agent: ≤ 60 runs/day and a token cap set in YAML; desk monthly cap set by admin before M8 go-live | Jitesh | Before M8 |
| 14 | Which agents beyond the MVP three | Research Analyst + Quant Researcher as M8 stretch; others post-MVP | Jitesh | M8 |

---

## 14. Hand-off Checklist

- [x] Problem statement is understood
- [x] All MVP features are enumerated with acceptance criteria
- [x] Data models are defined (full DDL in `DATA_MODEL.md`)
- [x] API contracts are specified (`API.md`, `proto/`)
- [x] Tech stack is fully prescribed (`TECH_STACK.md`)
- [x] Environment variables are listed (`.env.example`)
- [x] Project structure is defined
- [x] Out-of-scope items are clearly separated
- [ ] All `[DECISION NEEDED]` items are resolved or assigned — **#2, #3, #5, #7 must be resolved by Jitesh; others have defaults**
- [x] Non-functional requirements are set
- [x] No features in scope are underspecified

**Build confidence score:** **Medium-High.** Technical scope is fully specified and sequenced in `ROADMAP.md`. Medium because (a) the desk's legal/account structure (#2) can change how accounts and IPs are modelled, and (b) MVP breadth is large for a solo builder — follow the milestone order strictly and do not start a milestone until the previous exit criteria pass.
