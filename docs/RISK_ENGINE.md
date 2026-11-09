# Risk Engine

There are two halves:
1. **Pre-trade gate** (C++, in-process in `engine`): synchronous and deterministic, and every order passes through it.
2. **Portfolio analytics** (Python `quant`): asynchronous. It covers Greeks, VaR, scenarios, factors and stress tests, and drives post-trade alerts and auto kill triggers.

The gate never waits on analytics. It uses engine-local state only: positions, marks, limits, a margin cache, kill state and discipline state.

## 1. Limit hierarchy
Scopes: `desk ⊃ account ⊃ trader ⊃ strategy`, plus `instrument` overrides.
For each check, the **most restrictive** applicable limit wins. A trader on two accounts is checked per account *and* at trader level (aggregated across accounts).
Limits are versioned (`LimitSet.version`). Changes hot-reload via gRPC `ReloadLimits`, and every `GateStamp` records the version used.
`hard=true` rejects the order; `hard=false` passes it with a warning (shown in the ticket and logged).

## 2. Limit catalogue (keys for `risk_limit.key`)
| Key | Unit | Default (desk) | Meaning |
|---|---|---|---|
| `max_order_value` | ₹ | 5,00,000 | Notional per order (premium × qty for options) |
| `max_order_qty_lots` | lots | 20 | Per order, before freeze slicing |
| `max_position_lots` | lots | 50 | Abs net per instrument |
| `max_position_notional` | ₹ | 25,00,000 | Abs net notional per underlying (futures at price, options at delta-notional) |
| `max_gross_exposure` | ₹ | 1,00,00,000 | Σ abs notional |
| `max_net_delta_inr` | ₹ | 50,00,000 | Rupee delta |
| `max_net_vega_inr` | ₹/vol-pt | 50,000 | |
| `max_short_option_lots` | lots | 20 | Σ short option lots |
| `naked_short_options` | bool | 0 (disallowed) | 1 allows short options without a covering long within the same underlying/expiry |
| `max_daily_loss` | ₹ | 50,000 | Realised + MTM − costs since session start |
| `max_drawdown_from_peak` | ₹ | 75,000 | Intraday peak P&L − current |
| `max_open_orders` | count | 50 | |
| `ops_cap` | orders/s | 8 | Per account per exchange (hard ceiling 10, from SEBI/NSE threshold) |
| `fat_finger_pct` | % | 5 | Limit price vs LTP deviation, EQ and FUT |
| `fat_finger_pct_opt` | % | 30 | The same, for options |
| `dup_window_ms` | ms | 1,500 | Same account/instrument/side/qty/price within window → reject |
| `margin_buffer_pct` | % | 10 | Required margin × (1 + buffer) ≤ available |
| `min_reward_risk` | ratio | 0 (off) | When > 0, bracket orders need (target−entry)/(entry−stop) ≥ value |
| `loss_streak_lockout_days` | trading days | 0 (off) | After N consecutive losing trading days, block new opening orders for `lockout_length_days` |
| `lockout_length_days` | trading days | 1 | |
| `allow_market_orders` | bool | 1 | |
| `allow_mis_after` | HHMM IST, stored as a number such as 1510 | 15:10 | Block new MIS opening orders after this time |
| `trading_window_start` / `_end` | HHMM IST, stored as a number such as 915 | 09:15 / 15:30 | Outside window: only closing orders (and AMO post-MVP) |

Desk defaults are **seed values for a staging desk**. The owner sets real values in the admin console before going live.

## 3. Gate rule chain (evaluation order; first hard failure stops)
| # | Code | Rule | Applies to |
|---|---|---|---|
| 1 | `R-KILL` | Any active kill switch in scope with `block` | all intents except kill-generated cancels/flatten |
| 2 | `R-LIVE-DISABLED` | Live order when `LIVE_TRADING_ENABLED=false` or strategy not LIVE | live |
| 3 | `R-ENTITLE` | User mapped to account with `can_trade`; role allows live | all |
| 4 | `R-SESSION` | Account session connected; token valid | live |
| 5 | `R-MKT-HOURS` | Within trading window / allowed session for segment | opening orders |
| 6 | `R-INSTR-BAN` | Instrument in F&O ban: only position-reducing orders allowed | opening |
| 7 | `R-INSTR-RESTRICTED` | Desk restricted list | all |
| 8 | `R-INSTR-EXPIRED` | Contract expired / invalid lot multiple | all |
| 9 | `R-ORDER-SHAPE` | Qty multiple of lot; price on tick grid; required fields per type; MARKET allowed | all |
| 10 | `R-FAT-FINGER` | Price within `fat_finger_pct` of a fresh LTP (at most 3 s old) and within the circuit band. A live order needs a known band (D-22) | priced orders |
| 11 | `R-ORDER-VALUE` | `max_order_value`, `max_order_qty_lots` | all |
| 12 | `R-DUP` | Duplicate within `dup_window_ms`: same account, instrument, side, qty, price and trigger. Slices of one intent are exempt from each other (D-24). A full table rejects with `R-DUP-TABLE-FULL` and alerts | all |
| 13 | `R-OPS` | Token at `ops_cap`, per account per exchange. No free token rejects at once (D-21) | all |
| 14 | `R-OPEN-ORDERS` | `max_open_orders` | opening |
| 15 | `R-POSITION` | Post-trade projected position within `max_position_lots/notional` | opening |
| 16 | `R-EXPOSURE` | Projected gross/net exposure within limits | opening |
| 17 | `R-GREEKS` | Projected net delta/vega within limits | opening |
| 18 | `R-SHORT-OPT` | Short option lots / naked short rule | opening sells of options |
| 19 | `R-LOSS-DAILY` | Current daily P&L > −`max_daily_loss` (opening orders blocked once breached; closing allowed) | opening |
| 20 | `R-DRAWDOWN` | Intraday drawdown | opening |
| 21 | `R-MARGIN` | Margin cache: required × (1+buffer) ≤ available; for orders > ₹10 lakh notional, preview calls the broker margin API (async, before submit) | opening |
| 22 | `R-DISCIPLINE` | Loss-streak lockout active; bracket reward:risk below `min_reward_risk` | opening |

"Opening" = increases absolute position. Position-reducing orders skip rules 14–22, so limits never stop a trader getting flat. They still pass 1–13, except rule 1 when the action is a kill-generated cancel or flatten, and rule 2 for a flatten, which runs under a system identity so that it works after a trader's mapping is gone (M2.7). The exception is a Postgres outage: no new order goes out while events aren't durable, including reducing and flatten orders, while cancels still go (ADR-025). Nothing that adds risk is taken in then either: new orders, qty-raising modifies and price or trigger modifies are refused with `ORDER_LOG_UNAVAILABLE`. A queued order whose turn comes more than `ORDER_QUEUE_MAX_AGE_MS` (5 s) after its gate stamp is `EXPIRED_IN_QUEUE`, never sent (ADR-027).

**Projected state** uses current net position + open orders on the same side (worst case) + this intent.

**Implementation notes**
- **Rules 1–13** are in `engine/src/risk/rules.cc` (M2.3), and what they read is in `risk/gate_context.h`. Anything not loaded refuses the order: no limit row for a key, no calendar for today, an unknown account or instrument. An unknown instrument is `R-INSTR-EXPIRED`'s to refuse, and so is a master row with no usable lot or tick size. A wrong lot multiple on the order is `R-ORDER-SHAPE`'s.
- **Limits:** the strictest hard limit rejects and the strictest soft limit warns. "Strictest" depends on the key: the longest duplicate window, the latest window start and the earliest window end.
- **The OPS budget:** a token comes back one second after it was used, so no rolling second holds more than the cap. A bucket refilling at `cap`/s from full would allow nearly twice the cap in one second, over the 10 OPS threshold. The gate and the send queue each keep one. NSE and NFO share a budget, as do BSE and BFO: counting a venue's segments together can only be stricter. The budgets run on the wall clock: a step back holds tokens back, which is safe, but a forward step makes recent sends look old and can release one extra cap per step. M2.3b moves them to a monotonic clock (D-25).
- **Send queue (D-20):** `OrderRouter` holds an order with no token, up to the queue age limit. Other accounts' orders go past it. Cancels go first and use tokens too, so cancelling many orders takes `orders / cap` seconds.
- Rules are plain functions, `Outcome (*)(const GateContext&, const OrderDetails&, const GateFacts&) noexcept`, registered in a fixed table of `{RuleCode, fn}` in the order above (`engine/include/vyuha/risk/risk_gate.h`, M2.2). There is no virtual dispatch in the inner loop. `RiskGate::create` refuses a table that's out of order, repeats a code or has an empty slot. The gate attaches the code to each outcome, so a rule can't misreport. A verdict that is neither pass nor warn counts as a reject (fail closed).
- **Incomplete chain:** a gate whose table lacks any of the 22 rules refuses every live order (`is_paper` false) with `R-LIVE-DISABLED`, before any rule runs. Paper orders still go through the rules it has. So until M2.4 finishes the chain, nothing the gate approves can be live.
- **Opening vs reducing:** the gate works this out itself (`GateFacts::opening`); callers don't supply it. Until positions exist (M2.6), every order counts as opening, so every rule runs (fail closed). Property (a) below can only be tested from M2.6.
- **A router refusal after the gate** (owner, 2026-11-09): the gate commits an approved order's OPS token and duplicate entry before `submit`, so an order the router then refuses has still used them. This fails closed, and it stays.
- **Stateful rules are two-phase** (owner, 2026-11-08; built in M2.3). Each stateful rule (`R-DUP`, `R-OPS`) has a pure `check()` and a separate `on_approved()`. The gate runs every `check()` first, and only when the whole chain passes does it call `on_approved()` on each. A rejected order never uses up an OPS token or creates a duplicate-window entry. The core thread is the only writer, so there is no race.
- `ApprovedOrder` (`risk/approved_order.h`) holds exactly the `OrderDetails` the gate evaluated and its `GateStamp`. It is `final` and move-only, with a private constructor whose only friend is `RiskGate`. Its move is user-provided and empties the source (`spent()`), which also makes it not trivially copyable, so `std::bit_cast` can't forge one. `IOrderSender::send` takes nothing else. `OrderRouter::submit(handle, ApprovedOrder)` refuses an approval that:
  - is spent;
  - doesn't match the NEW order field for field;
  - is for the other venue (a router is built `Venue::kPaper` or `Venue::kLive`, so a paper flag left at its default can't reach a live sender);
  - is older than `RISK_APPROVAL_MAX_AGE_MS` (default 100 ms; whole ms, 1–10,000), or dated in the future, by the OMS clock: evaluating and submitting happen in one pass of the core thread. Unlike the other refusals, this one ends the order: RISK_REJECTED with **`R-APPROVAL-STALE`** and the stamp, audited (`order.approval_stale`) and alerted (`APPROVAL_STALE`, warn) in the same transaction, never retried. Approval-to-send time is a histogram (`vyuha_engine_approval_to_send_seconds`).

  It then records `APPROVED` with the stamp in `order_event.payload.gate` (`ruleset_version`, `eval_ns`, `ts_ns`) and queues exactly the approved details. The gate copies its rule table and refuses a temporary clock. `tests/compile_fail.cc` (ctest `compile_fail.*`) checks that every route around this fails to compile; `scripts/check-invariants.sh` checks for extra friends and casts.
- `GateContext` holds pre-aggregated per-scope state, updated incrementally on fills and marks. It never iterates all positions per check.
- Each result records `{code, limit, current, hard}`. Messages are rendered outside the hot path.
- Property tests (RapidCheck): (a) a position-reducing order is never rejected by rules 14–22; (b) if every limit is at max value, every well-formed order passes; (c) the gate is pure given the context (same input → same output).

## 4. Kill switch
| Scope | Who can set | Who can release |
|---|---|---|
| desk | risk_manager, admin, auto | risk_manager, admin (re-auth) |
| account | risk_manager, admin, auto | same |
| trader | trader (self), risk_manager, admin, auto | risk_manager, admin |
| strategy | owner, risk_manager, admin, auto | risk_manager, admin |

**Actions:** `block` (always), `cancel` (default on), `flatten` (opt-in).
Flatten sends marketable LIMIT orders at LTP ± `market_protection_pct` and slices at freeze qty. It retries up to 3 times at 2 s intervals, widening the price by 1 tick-band each time, then alerts for manual action.

**Auto triggers** (configurable per scope):
- `R-LOSS-DAILY` breach → block + cancel
- drawdown breach → block + cancel
- broker-reject storm: > 20 rejects / 60 s per account → block that account
- engine/broker disconnect with open positions > 30 s → alert only (flatten would be unsafe without connectivity)
- Postgres event ring > 80% → desk block

**UI:** `Ctrl+Shift+K` opens the kill dialog. Scope is pre-selected to the user's widest permitted scope, and Enter confirms. The release dialog requires re-auth plus a reason.
Zerodha also offers a segment kill switch in the Kite console. Document it as a second, independent safety layer for account holders.

## 5. Discipline rules
Common retail-desk practice, made enforceable:
- **Loss-streak lockout:** a trading day is "losing" when the day's net P&L (after costs) < 0 for the scope. Streaks count **trading days only**; days with no trades neither break nor extend a streak. After `loss_streak_lockout_days` consecutive losing days, opening orders are blocked for `lockout_length_days` trading days. EOD job updates `discipline_state`.
- **Minimum reward:risk:** when `min_reward_risk` > 0, an opening order *with* a bracket must meet the ratio. `[DECISION NEEDED: should opening orders without a bracket be blocked when min_reward_risk > 0? Default: allowed with a warning]`
- Lockouts are visible in the status bar and in `RISK`.

## 6. Portfolio analytics (quant)

### 6.1 Pricing
- **Index options (NIFTY, BANKNIFTY, FINNIFTY, MIDCPNIFTY, SENSEX, BANKEX):** Black-76 on the forward. Forward = same-expiry futures price if listed; otherwise synthetic forward from ATM put-call parity (mid prices).
- **Stock options:** Black-Scholes-Merton with continuous dividend yield (0 unless a dividend is known in `corporate_action` before expiry → use escrowed dividend).
- Exercise style: European (NSE index and stock options are European).
- Time to expiry: ACT/365 in years, measured to 15:30 IST on expiry day, with a 1-minute floor.
- Rate: configurable `RISK_FREE_RATE` (default from the RBI policy repo rate, set by admin).
- **IV solve:** Brent's method on [0.001, 5.0], tolerance 1e-8, max 100 iterations. Fall back to bisection. Return NaN (shown as `—`) when price < intrinsic − 1 tick.
- **Greeks** per contract are analytic. Rupee Greeks: delta_inr = Δ × S × qty; gamma_inr per 1% = 0.5 × Γ × (0.01 S)² × qty … (precise definitions in `quant/src/vyuha_quant/pricing/README.md`, written at M5 with test vectors).
- Test vectors: compare with QuantLib in tests only (`quantlib` is a test dependency, not runtime) to 1e-6.

### 6.2 Exposures
- Gross/net notional; futures at price × qty; options at delta-notional.
- Beta-delta: Σ delta_inr × β(underlying vs NIFTY, 1-yr daily, Blume-adjusted). Index options use β = 1 for NIFTY and the measured β for other indices.
- Sector exposure via `instrument.sector` (and the index-weighted sector mix for index derivatives).

### 6.3 VaR
- **Historical (default):** 250 trading days of daily returns of each underlying, plus IV changes (ATM IV series where available; else IV held constant, flagged). Full revaluation of each position under each historical day's shocks, then the 1st/5th percentile of P&L. Horizon 1 day.
- **Parametric:** delta-normal using an EWMA covariance (λ = 0.94) of underlying returns, for quick checks.
- Report component VaR by trader and underlying (Euler allocation on the historical scenarios).
- Monte Carlo VaR is post-MVP.

### 6.4 Scenarios & stress
- **Grid:** spot shocks {−10, −7, −5, −3, −2, −1, 0, +1, +2, +3, +5, +7, +10}% × IV shocks {−30, −20, −10, 0, +10, +20, +30}% (relative), days-forward {0, 1, to-expiry}. Full revaluation.
- **Named historical stress:** replay realised underlying moves and ATM-IV changes from specific dates. The seed list is chosen by the owner at M5 (e.g. the COVID crash weeks of March 2020, general-election result day of 4 June 2024). The engine computes the shocks from stored data; never hard-code shock sizes.
- **Custom:** user-defined per-underlying spot/IV shocks; saved per user.

### 6.5 Unified factor model (Aladdin-style)
- Universe: Nifty 500, daily.
- Factors (MVP): market (NIFTY 500 return), size (log mcap), momentum (12-1 month), short-term reversal (1 month), volatility (1-yr realised), sector dummies (NSE industry classification), plus macro sensitivities from the knowledge graph (`SENSITIVE_TO` betas to USDINR, Brent, 10Y G-sec change) estimated by time-series regression.
- Estimation: daily cross-sectional WLS (weights √mcap), 1-year window. Factor covariance by EWMA (half-life 90 days), plus specific variance.
- Outputs per scope: factor exposures, factor VaR contribution, specific risk share.
- Nightly rebuild job (after 18:00 IST). Results go in `$DATA_LAKE_DIR/factors/<date>/`.
- Value/quality factors need fundamentals, so they're post-MVP.

### 6.6 Refresh cadence & outputs
| Computation | Cadence | Output |
|---|---|---|
| Position Greeks/MTM per scope | 1 s | NATS `risk.summary.*`, ClickHouse `risk_metrics` (1 row/10 s persisted) |
| Desk VaR (historical) | 5 s intraday (scenario matrix cached; only positions change) | same |
| Scenario grid | on demand + 30 s for open `SCN` panels | REST/WS |
| Factor exposures | 60 s (exposures), nightly (model) | REST |

## 7. Reconciliation
- Every 60 s and on reconnect: compare engine open orders and positions with the broker. Mismatch → `RECON_MISMATCH` alert with diff. Broker state wins for positions (engine adjusts and emits a `system` order event). Orders in `UNKNOWN` are resolved from the broker order book.
- EOD (15:45 IST): snapshot `position_eod` with `broker_net_qty`; `reconciled=false` rows alert the risk manager.
