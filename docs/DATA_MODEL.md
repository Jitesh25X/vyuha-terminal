# Data Model

Four stores, each with one job:

| Store | Holds | Authoritative DDL |
|---|---|---|
| PostgreSQL 17 (+pgvector) | Users, accounts, instruments, orders/events/fills, limits, kill switch, strategies, jobs, stories, alerts, copilot logs, audit | `db/migrations/*.sql` |
| ClickHouse | Ticks, depth, bars, risk/P&L time series, latency, eval results | `db/clickhouse/*.sql` |
| Neo4j | Knowledge graph (entities, events, exposures) | `db/neo4j/*.cypher` |
| Parquet lake (`$DATA_LAKE_DIR`) | Downloaded history, backtest artifacts | layout in §5 |

## 1. Conventions
- **IDs:** UUIDv7 for entities created at runtime (time-ordered; engine generates order IDs). `bigserial` for `instrument.id` because it's hot in the engine and must be a compact int.
- **Money/prices:** Postgres `numeric(18,4)` in rupees. Engine and ClickHouse use `Int64` "price units" = rupees × 10,000 (1/100 paise), so tick sizes like ₹0.05 are exact. Conversion lives in one place per language (`price.h`, `price.go`, `price.py`, `price.ts`), each with round-trip tests.
- **Quantity:** always in **units** (shares/contracts), never lots. The UI converts lots ↔ units using `instrument.lot_size` for the day.
- **Time:** `timestamptz` / `DateTime64(…, 'UTC')`. The trading day is computed in IST using the exchange calendar.
- **Soft vs hard deletes:** no hard deletes on trading tables. Instruments are versioned with `valid_from/valid_to`.

## 2. PostgreSQL entities (see migration for constraints)

| Table | Purpose | Key notes |
|---|---|---|
| `app_user` | Users + roles | argon2id, TOTP secret encrypted; lockout after 5 failures for 15 min |
| `refresh_token` | Rotating refresh tokens | Reuse of a rotated token → revoke the whole `family_id` |
| `personal_access_token` | External MCP access | Read-only scopes by default |
| `broker_account` | One row per broker client account | Encrypted key/secret/token; `static_ip` bound by engine; `public_static_ip` unique (no sharing between unrelated accounts); `ops_cap` ≤ 10 |
| `account_user_map` | Who can view/trade which account | One default account per user |
| `instrument` | Unified instrument master | Kite token + Upstox key on one row; option chain index; F&O ban flag; freeze qty |
| `restricted_instrument` | Desk restricted list | Gate rule `R-INSTR-RESTRICTED` |
| `corporate_action` | Price/volume adjustment factors | Raw bars never mutated |
| `exchange_holiday` | Calendar overrides incl. special sessions | Seeded from `exchange_calendars` XBOM (`vyuha-quant holidays seed`); existing rows win |
| `fo_ban_import` | NSE F&O ban list per trade date (ADR-019) | One row per imported day (`symbols` may be empty); drives `instrument.fo_ban` on NFO derivatives |
| `strategy`, `strategy_version`, `strategy_paper_session` | Strategy lifecycle | Live needs approval + paper sessions |
| `order` | Current order state (projection) | Engine is the only writer |
| `order_event` | Every state transition (event log) | `(order_id, seq)` unique; payload has gate stamp |
| `fill` | Executions | Unique `(account_id, broker_trade_id)`; costs breakdown JSON |
| `position_eod` | EOD reconciled snapshot | `reconciled=false` → alert |
| `risk_limit` | Limit catalogue values by scope | Keys in `RISK_ENGINE.md §2` |
| `kill_switch` | Kill switch history/current | One active row per scope |
| `discipline_state` | Loss/win streaks and lockouts | Streaks count **trading days only**; no-trade days don't break a streak |
| `cost_schedule` | Effective-dated charges | Must carry `source_url` + `verified_by` before use in live P&L |
| `dataset`, `download_job` | Data lake catalogue and jobs | Idempotent by (instrument, interval, source, range) |
| `backtest_run` | Backtest jobs and metrics | `sweep_id`, `split` for walk-forward |
| `workspace_layout`, `watchlist`, `alert_rule`, `alert_event` | UI state and alerts | |
| `feed_source`, `story` | Ingestion | `exchange` feeds can't be enabled until `tos_reviewed` |
| `context_alert` | Position-impact alerts | Unique per (story, user) |
| `copilot_session`, `copilot_message` | AI conversations + tool calls | Token usage per message |
| `audit_event` | Hash-chained append-only audit | Trigger assigns id under the chain lock and computes hash v2 (all columns, length-prefixed; `000004`); UPDATE/DELETE raise; `vyuha_app` INSERT-only |

### 2.1 Projections (computed, not stored as truth)
- **Intraday position** per `(account, instrument, product)` = Σ signed fills (today) + previous EOD `net_qty` for carry-forward products (NRML/CNC). Average price uses the weighted-average-cost method; realised P&L is recognised on reducing trades.
- **MTM** = net_qty × (mark − avg_price). The mark is LTP, or the mid when spread ≤ 2 ticks; stale after 3 s.
- **Costs** per fill come from `cost_schedule`, effective on the trade date (see `STRATEGY_SDK_AND_BACKTEST.md §5`).
- **Daily P&L** (for loss limits) = realised + unrealised − costs since the session start for the scope.

## 3. ClickHouse
See `db/clickhouse/001_init.sql`. Tables: `ticks` (1-year TTL), `depth` (30 days), `bars` (ReplacingMergeTree, engine-built + downloaded), `risk_metrics`, `pnl_ts`, `engine_latency`, `eval_results`; view `bars_inr`.
Sizing guide: ~9,000 instruments × ~1.5 ticks/s × 22,500 s/day ≈ 300 M rows/day at peak subscription. At ~10–15 bytes/row compressed that's ~3–5 GB/day. In MVP, subscribe only to what users and strategies need; the default recording universe is configurable.

## 4. Knowledge graph (Neo4j)

### 4.1 Nodes
| Label | Key | Properties |
|---|---|---|
| `Company` | `isin` | name, aliases[], aliases_text, nse_symbol, bse_code, mcap_bucket |
| `Instrument` | `instrument_id` | tradingsymbol, segment, expiry, strike, option_type |
| `Sector`, `Industry` | `name` | classification source |
| `Index` | `symbol` | name (NIFTY 50, NIFTY BANK, …) |
| `Person` | `person_id` | name, aliases[], roles[] |
| `Event` | `event_id` | type (taxonomy §4.3), date, magnitude, horizon, source_story_id |
| `Story` | `story_id` | title, url, published_at, sentiment |
| `MacroVariable` | `code` | e.g. `IN.REPO`, `IN.CPI`, `USDINR`, `BRENT`, `US10Y` |
| `Commodity` | `code` | |
| `Trader` | `user_id` | display_name |
| `Account` | `account_id` | broker |
| `Strategy` | `strategy_id` | name |

### 4.2 Relationships
| Type | From → To | Properties |
|---|---|---|
| `ISSUES` | Company → Instrument | |
| `UNDERLYING_OF` | Instrument → Instrument | (stock/index → future/option) |
| `IN_SECTOR`, `IN_INDUSTRY` | Company → Sector/Industry | |
| `CONSTITUENT_OF` | Company → Index | weight, as_of |
| `MENTIONS` | Story → Company/Person/Sector/MacroVariable/Commodity | sentiment, relevance |
| `REPORTS` | Story → Event | |
| `AFFECTS` | Event → Company/Sector/Index/MacroVariable | direction (+1/−1/0), confidence 0–1 |
| `SENSITIVE_TO` | Company/Sector → MacroVariable/Commodity | beta, method, as_of (estimated by quant nightly) |
| `SUPPLIER_OF`, `COMPETITOR_OF`, `SUBSIDIARY_OF` | Company → Company | source, confidence (post-MVP extraction; seedable manually) |
| `HOLDS` | Account → Instrument | net_qty, delta_inr, as_of (synced from positions) |
| `TRADES` | Trader → Account | |
| `RUNS` | Strategy → Account | |

### 4.3 Event taxonomy (fixed enum; extraction must choose one)
`earnings_result, guidance, dividend, split_bonus, buyback, rights_issue, merger_acquisition, management_change, promoter_pledge, block_bulk_deal, insider_trade, rating_change, regulatory_action, litigation, order_win, capacity_expansion, product_launch, monetary_policy, fiscal_policy_budget, inflation_data, gdp_data, fx_move, commodity_move, global_market_move, geopolitical, index_rebalance, fo_ban, circuit_limit_change, market_structure_change, other`

### 4.4 Impact query (canonical)
```cypher
// $entity_keys: entities affected by the new event; returns exposure paths ≤ 2 hops to HOLDS
MATCH (e:Event {event_id: $event_id})-[a:AFFECTS]->(x)
MATCH p = (x)-[:ISSUES|UNDERLYING_OF|CONSTITUENT_OF|IN_SECTOR|SENSITIVE_TO*0..2]-(i:Instrument)<-[h:HOLDS]-(acc:Account)<-[:TRADES]-(t:Trader)
WITH t, acc, i, h, a, p, e,
     reduce(w = 1.0, r IN relationships(p) | w * coalesce(r.weight, r.beta, 1.0)) AS path_weight
RETURN t.user_id AS user_id, acc.account_id AS account_id, i.instrument_id AS instrument_id,
       abs(h.delta_inr) * path_weight * a.confidence * e.magnitude AS score,
       [n IN nodes(p) | coalesce(n.name, n.tradingsymbol)] AS path
ORDER BY score DESC
```

## 5. Parquet lake layout
```
$DATA_LAKE_DIR/
  bars/source=<zerodha|upstox>/interval=<1minute|day|...>/instrument_id=<id>/year=<yyyy>/part-0.parquet
  options_expired/source=upstox/underlying=<NIFTY>/expiry=<yyyy-mm-dd>/interval=1minute/part-0.parquet
  backtests/<run_id>/{equity.parquet, trades.parquet, orders.parquet, report.json}
  factors/<yyyy-mm-dd>/{exposures.parquet, returns.parquet, cov.parquet}
```
Parquet schema for bars: `ts: timestamp[us, UTC], open/high/low/close: int64 (price units), volume: int64, oi: int64`. Write with zstd compression and row groups of 128 MB. Every write registers a `dataset` row with a sha256.

## 6. Retention
| Data | Retention |
|---|---|
| `audit_event`, `order`, `order_event`, `fill` | ≥ 5 years |
| ClickHouse `ticks` | 1 year (export to Parquet before TTL if needed) |
| `depth` | 30 days |
| `story` | 2 years |
| `copilot_message` | 1 year (configurable) |
| Backtest artifacts | Until deleted by owner |
