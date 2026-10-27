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

## Rules
- `uv run pytest`, `uv run mypy --strict src sdk`, `uv run ruff check`.
- Polars first. NumPy/Numba for numeric kernels. No pandas in library code (allowed in notebooks only).
- `Decimal` for money at I/O boundaries. `float64` is fine inside analytics. Convert prices via `vyuha_quant.price`.
- Pricing functions are pure and vectorised, with docstrings stating formula, units and conventions. Test vectors are checked against QuantLib (test-only dependency).
- Backtests must be deterministic. All randomness flows from `config.seed`.
- Broker historical limits live in `data/limits.py` with a source URL + verified date per entry.
- The strategy host never imports broker code. It talks to the engine only through the provided socket.
- Cost rates come only from `cost_schedule`. Never hard-code a tax/fee rate.
