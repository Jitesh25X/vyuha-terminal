# quant/ — Python (risk analytics, data, backtest, strategy host)

Read `docs/RISK_ENGINE.md §6`, `docs/STRATEGY_SDK_AND_BACKTEST.md` and the root `CLAUDE.md` first.

## Layout
```
src/vyuha_quant/
  api/            FastAPI app (internal endpoints used by gateway: optionchain, risk, scenario, var, factors)
  pricing/        Black-76, BSM, IV solver, rupee Greeks (pure functions, vectorised)
  risk/           risk worker (NATS), exposures, VaR, scenarios, stress
  factors/        nightly factor model
  data/           instrument master sync, download manager, broker historical clients, limits table
  costs/          cost engine over cost_schedule
  backtest/       engine driver over vyuha_core, fill models, metrics, sweeps, walk-forward
  strategy_host/  sandboxed process running strategies in paper/live
sdk/vyuha/strategy/   public Strategy SDK (stable API; semver)
strategies/examples/  sma_cross, orb_breakout, short_straddle_with_stop
tests/ (+ tests/golden/)
```

## Jobs (`uv run vyuha-quant …`, JSON report on stdout, JSON logs on stderr)
- `instruments sync [--date D] [--from-dir DIR] [--allow-large-change]` — F-MD-1 master sync (08:00 IST)
- `ban import FILE [--date D]` / `ban check` — NSE F&O ban list, manual import only (ADR-019; never fetch from NSE)
- `holidays seed` — exchange_holiday from exchange_calendars XBOM (existing rows win)
- `download plan --spec F` — what a job would fetch (requests, minutes); no token, no broker request
- `download run --spec F --account ID --user NAME` — run or resume a job; the access token comes on stdin, so start it with `vyuhactl download run`, which passes it no `VYUHA_MASTER_KEY` or other `.env` secrets. Dev-only overrides for tests: `UPSTOX_API_BASE`, `DOWNLOAD_PACE_S`
- `serve` — internal API for the gateway on `QUANT_HTTP_ADDR` (default 127.0.0.1:8090, loopback, no auth): `POST /internal/optionchain`, `GET /health`. Settings: `RISK_FREE_RATE` (decimal, RBI repo rate) + `RISK_FREE_RATE_AS_OF`, `IV_MID_MAX_SPREAD` (0.10), `IV_LTP_MAX_AGE_S` (60)

## Rules
- `uv run pytest`, `uv run mypy --strict src sdk`, `uv run ruff check`.
- Polars first. NumPy/Numba for numeric kernels. No pandas in library code (allowed in notebooks only).
- `Decimal` for money at I/O boundaries. `float64` is fine inside analytics. Convert prices via `vyuha_quant.price`.
- Pricing functions are pure and vectorised, with docstrings stating formula, units and conventions. Test vectors are checked against QuantLib (test-only dependency).
- Backtests must be deterministic. All randomness flows from `config.seed`.
- Broker historical limits live in `data/limits.py` with a source URL + verified date per entry.
- The strategy host never imports broker code. It talks to the engine only through the provided socket.
- Cost rates come only from `cost_schedule`. Never hard-code a tax/fee rate.
