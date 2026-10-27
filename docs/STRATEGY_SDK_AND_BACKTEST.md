# Strategy SDK, Data Downloads & Backtesting

## 1. Historical data downloads (`quant/src/vyuha_quant/data`)
- Job spec: `{account_id, instruments | index, interval, from, to, include_expired}`. The source broker comes from the account. The download uses that account's API session (data entitlement required).
- **Chunking:** each broker caps the date range per request by interval. Keep the caps in `data/limits.py` as a table, each entry with a source URL and verification date. **Verify against current official docs at implementation time.** Known at writing: Upstox V3 historical candles provide minute/hour data from Jan 2022 and day/week/month data from Jan 2000. Kite caps the days per request per interval.
- **Rate limiting:** `aiolimiter` per account, using the documented per-second/minute limits minus 20% headroom. Back off on HTTP 429 (exponential, max 5 tries).
- **Resumability:** progress cursor saved in `download_job.progress` after each chunk. Restart continues from the cursor.
- **Expired F&O:** Upstox expired-instruments endpoints enumerate expiries and contracts and fetch their candles. Store under `options_expired/`.
- **Validation per chunk:** monotonic timestamps; no bars outside the session (09:15–15:30 IST, plus special sessions from `exchange_holiday`); OHLC sanity (low ≤ open/close ≤ high); duplicates dropped. Gaps are logged, not filled.
- **Corporate actions:** raw prices are stored. Adjusted series = raw × cumulative `price_factor` for dates before each `ex_date`, applied on read (`adjusted=true`).
- Output: Parquet (layout in `DATA_MODEL.md §5`) plus a `dataset` row, and optionally loaded into ClickHouse `bars` for fast charting.

## 2. Strategy SDK (`quant/sdk/vyuha/strategy`)
```python
from decimal import Decimal
from vyuha.strategy import Strategy, Bar, Tick, OrderUpdate, Fill, Context, Param, indicators as ta

class SmaCross(Strategy):
    """Reference strategy: SMA crossover on 1-minute bars, one position per symbol."""
    params = {
        "fast": Param(int, default=10, min=2, max=200),
        "slow": Param(int, default=30, min=5, max=400),
        "lots": Param(int, default=1, min=1, max=10),
        "stop_pct": Param(float, default=0.5, min=0.1, max=5.0),
    }
    interval = "1m"

    def on_start(self, ctx: Context) -> None:
        for iid in ctx.universe:
            ctx.subscribe_bars(iid, self.interval)
        self.fast = {i: ta.SMA(self.p.fast) for i in ctx.universe}
        self.slow = {i: ta.SMA(self.p.slow) for i in ctx.universe}

    def on_bar(self, ctx: Context, bar: Bar) -> None:
        f = self.fast[bar.instrument_id].update(bar.close)
        s = self.slow[bar.instrument_id].update(bar.close)
        if f is None or s is None:
            return
        pos = ctx.position(bar.instrument_id)
        lot = ctx.instrument(bar.instrument_id).lot_size
        if f > s and pos.net_qty <= 0:
            ctx.buy(bar.instrument_id, qty=self.p.lots * lot - pos.net_qty, order_type="MARKET",
                    bracket=ctx.bracket_pct(stop_pct=self.p.stop_pct))
        elif f < s and pos.net_qty > 0:
            ctx.sell(bar.instrument_id, qty=pos.net_qty, order_type="MARKET")

    def on_fill(self, ctx: Context, fill: Fill) -> None:
        ctx.log.info("filled", fill=fill)
```

### 2.1 Lifecycle callbacks
`on_start(ctx)`, `on_bar(ctx, bar)`, `on_tick(ctx, tick)`, `on_order(ctx, update)`, `on_fill(ctx, fill)`, `on_timer(ctx, name)`, `on_session(ctx, event)` (pre_open/open/close), `on_stop(ctx)`.

### 2.2 Context API (identical in all modes)
- **Data:** `subscribe_bars`, `subscribe_ticks`, `history(iid, interval, n)`, `quote(iid)`, `option_chain(underlying, expiry)`, `instrument(iid)`, `universe`, `now()` (IST-aware; simulated in backtest).
- **Orders:** `buy`, `sell`, `order(intent)`, `modify`, `cancel`, `cancel_all`, `bracket_pct`, `bracket_abs`. Each returns an `OrderHandle` with a status future.
- **State:** `position(iid)`, `positions()`, `pnl()`, `cash()`, `open_orders()`.
- **Utility:** `schedule(name, at|every)`, `log` (structured), `metrics.gauge/counter`, `params` (`self.p`).
- **Indicators:** `ta.SMA, EMA, RSI, ATR, BBands, MACD, VWAP, SuperTrend, Donchian`. These are incremental O(1)-update classes, with vectorised Polars equivalents for research (`ta.vec.*`).

### 2.3 Sandbox
Strategy code runs in a separate process (`strategy_host`) with:
- an import allow-list (`vyuha.strategy`, `math`, `statistics`, `decimal`, `datetime`, `collections`, `itertools`, `functools`, `dataclasses`, `typing`, `numpy`, `polars`, `numba`); everything else raises `ImportError`
- no network: the process only reaches the engine via a local Unix socket provided by the host
- CPU-time and RSS limits (rlimits)
- a code hash stored in `strategy_version.code_sha256`, with the running hash checked at start.

### 2.4 Modes
| Mode | Clock | Data | Broker | Risk gate |
|---|---|---|---|---|
| `backtest` | Simulated (event-driven) | Parquet/ClickHouse bars or recorded ticks | `SimBroker` (in-process `vyuha_core`) | Same C++ gate, via nanobind, with backtest limits |
| `paper` | Wall clock | Live ticks | `SimBroker` in engine | Engine gate |
| `live` | Wall clock | Live ticks | Kite/Upstox adapters | Engine gate + strategy limits |

## 3. Backtest engine
- Event queue ordered by `(ts, priority, seq)` where priority is `market data < timers < order events`. Deterministic: same inputs + seed → identical outputs (CI checks a byte hash of `trades.parquet`).
- **Bar fill models:**
  - `next_open` (default): orders created in `on_bar(t)` fill at bar t+1's open ± slippage.
  - `intrabar_ohlc`: LIMIT/SL orders are checked against bar t+1's high/low. The path assumption is O→H→L→C if close ≥ open, else O→L→H→C. When stop and target are both touched in one bar, the stop is assumed to hit first (conservative).
  - `close` (opt-in, flagged in report): fill at the same bar's close.
- **Tick fill model:** LIMIT fills when the trade price crosses through the limit. At touch, it fills with probability `queue_fill_prob` (default 0.3, seeded RNG). MARKET fills at opposite best + `slippage_ticks`.
- **Partial fills:** optional `max_participation` (default 10% of bar volume). The excess carries to later bars.
- **Session rules:** MIS auto-square-off at the configured time (default 15:20 IST) at the bar close. F&O expiry: open positions settle at the expiry close/settlement price (cash-settled index; stock F&O physical-settlement flag is reported as a warning).
- **Lot sizes** are as of the trade date (from the versioned instrument master). Strike/expiry universe: use only contracts that existed on that date (no survivorship bias for expired options).
- **Look-ahead guards:** data access in `on_bar(t)` is limited to ≤ t. `history()` never returns the forming bar. Indicator classes can't peek.

## 4. Sweeps & walk-forward
- Grid sweep over `params` (max 500 combos per job). Jobs run in parallel via NATS work-queue workers.
- Walk-forward: rolling `train_days`/`test_days`. Pick the best params on train by the chosen objective (default Sharpe), evaluate on test, and stitch the out-of-sample equity curve.
- The report flags overfitting risk: number of combinations tried, and in-sample vs out-of-sample Sharpe decay. Deflated Sharpe ratio is post-MVP.

## 5. Indian cost model (`quant/src/vyuha_quant/costs`)
Components are computed per fill from `cost_schedule` (effective-dated):

| Component | Basis | Applies to |
|---|---|---|
| Brokerage | Per order: flat (capped) or % turnover, by broker/segment/product | all |
| STT / CTT | % of turnover (equity: by delivery vs intraday and side; futures: sell side; options: premium on sell, intrinsic on exercise) | all |
| Exchange transaction charges | % of turnover (premium for options), by exchange/segment | all |
| SEBI turnover fee | per ₹ crore of turnover | all |
| Stamp duty | % of buy-side turnover, by segment/product | buy |
| GST | % of (brokerage + exchange txn + SEBI fee) | all |
| IPFT (NSE) | % of turnover where applicable | per schedule |
| DP charges | flat per scrip per day on delivery sells | CNC sells |

- **Rates are data, not code.** Seed `cost_schedule` at M4 from the brokers' published charges pages and the exchange circulars in force on the seed date, and record `source_url` and `verified_by`. Rates change (e.g. STT revisions in Union Budgets), so each row has `effective_from/effective_to`, and a backtest uses the rates in force on each trade date.
- Unit tests: one hand-calculated contract note per segment (EQ delivery, EQ intraday, futures, options buy, options sell) must match to ₹0.01.
- Backtest report shows total costs and a per-component breakdown, plus "costs as % of gross P&L".

## 6. Metrics (definitions in `quant/src/vyuha_quant/backtest/metrics.py`)
CAGR; annualised volatility (√252 × daily stdev); Sharpe (excess over `RISK_FREE_RATE`, daily, annualised); Sortino (downside deviation); max drawdown (₹ and %) and duration; Calmar; win rate; average win/loss; profit factor; expectancy in ₹ and in R (R = initial risk per trade from bracket stop, when present); exposure % (time in market); turnover; number of trades; average holding time; longest losing streak (in trading days); cost breakdown; monthly returns table; best/worst day.

## 7. Reference strategies (shipped, used as golden tests)
- `sma_cross` (equity, 1m bars): golden results committed under `quant/tests/golden/`.
- `orb_breakout` (opening-range breakout on index futures, 5m bars).
- `short_straddle_with_stop` (weekly index options; uses expired-options data; per-leg stop %).

Golden tests fail on any metric drift > 1e-9 unless the PR updates the golden file and explains why.
