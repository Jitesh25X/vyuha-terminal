# API Contract (Gateway)

- Base URL: `https://<host>/api/v1`. JSON (`application/json`). Authoritative machine spec: `gateway/api/openapi.yaml`. Generate it from this document at M0 and keep both in sync; the OpenAPI file wins on conflict.
- Auth: `Authorization: Bearer <access_jwt>` unless noted. The refresh token lives in the `vyuha_rt` httpOnly cookie (path `/api/v1/auth`).
- Every mutating request accepts an `Idempotency-Key` header (UUID). Orders use `client_order_id` instead.
- Errors: RFC 9457 `application/problem+json`:
```json
{ "type": "https://vyuha.dev/errors/risk-rejected", "title": "Order rejected by risk gate",
  "status": 422, "detail": "Daily loss limit ₹25,000.00 would be breached (current ₹24,310.50)",
  "code": "R-LOSS-DAILY", "reasons": [{"code":"R-LOSS-DAILY","limit":"25000.00","current":"24310.50"}] }
```
- Common status codes: `400` validation, `401` unauthenticated, `403` RBAC/entitlement, `404`, `409` conflict/idempotency mismatch, `422` business rule (risk), `423` locked (kill switch / account awaiting login), `429` rate limited, `503` engine unavailable.
- Money/prices in JSON are **strings** with up to 4 decimals (`"24310.5000"`). Quantities are integers in units.
- Timestamps: RFC 3339 UTC.
- RBAC scoping: `scoped` means traders see their mapped accounts and own strategies, while risk_manager/admin see everything.

## 1. Auth
### `POST /auth/login` — no auth
Req `{ "username": "meera", "password": "…" }` → 200 `{ "mfa_token": "…", "mfa_enrolled": true }` (the mfa_token is valid for 5 minutes).
Errors: 401 `AUTH_INVALID` (generic message), 423 `AUTH_LOCKED` (`locked_until`).

### `POST /auth/mfa` — mfa_token in body
Req `{ "mfa_token": "…", "totp": "123456" }` → 200 `{ "access_token": "…", "expires_in": 900, "user": {id, username, display_name, role} }` + Set-Cookie refresh.
### `POST /auth/mfa/enroll` (first login) → `{ "otpauth_url": "…", "qr_png_base64": "…" }`; `POST /auth/mfa/enroll/confirm {totp}`.
### `POST /auth/refresh` — cookie → new access token, rotated cookie. Reuse detected → 401 + family revoked.
### `POST /auth/logout` — revoke current refresh token.
### `POST /auth/reauth` — `{ password, totp }` → `{ "reauth_token": "…", "expires_in": 300 }`. Required header `X-Reauth` for sensitive endpoints (marked 🔐).

## 2. Instruments & market data
### `GET /instruments/search?q=relia&segment=EQ&limit=20` → `[{ id, exchange, segment, tradingsymbol, name, expiry, strike, option_type, lot_size, tick_size }]`
### `GET /instruments/{id}` → full instrument incl. `freeze_qty`, `fo_ban`, `underlying_id`.
### `GET /instruments/{id}/expiries` → `["2026-10-28", …]`
### `GET /marketdata/snapshot?ids=1,2,3` (entitled) → `[{ instrument_id, ltp, change_pct, bid, ask, volume, oi, ts, stale }]`
### `GET /marketdata/candles?instrument_id=&interval=1m&from=&to=&adjusted=true` (entitled) → `{ "bars": [[ts, o, h, l, c, v, oi], …] }`. Max 50,000 bars per call; 400 if exceeded.
### `GET /optionchain/{underlying_id}?expiry=2026-10-28&strikes=20` (entitled)
→ `{ underlying: {ltp, fut_ltp}, expiry, pcr, max_pain, atm_strike, rows: [{ strike, ce: {instrument_id, ltp, oi, oi_chg, iv, delta, gamma, theta, vega, bid, ask, volume}, pe: {…} }] }`

## 3. Orders
### `POST /orders/preview` — trader+
Req: same as create. → 200 `{ "gate": {"pass": false, "reasons": [...], "warnings": [...]}, "margin": {"required": "…", "available": "…", "source": "broker|cache"}, "notional": "…", "reward_risk": "2.10", "slices": 3, "costs_estimate": {...} }`

### `POST /orders` — trader+ (analyst: paper only)
Req:
```json
{ "client_order_id": "01JD…", "account_id": "uuid|paper", "instrument_id": 123, "side": "BUY",
  "qty": 75, "order_type": "LIMIT", "product": "NRML", "validity": "DAY",
  "price": "120.5000", "trigger_price": null, "market_protection_pct": null,
  "bracket": { "stop_trigger": "100.0000", "stop_price": "99.5000", "target_price": "160.0000" },
  "source": "manual", "ai_draft_id": null }
```
→ 202 `{ "order_id": "uuid", "status": "APPROVED|RISK_REJECTED", "children": ["uuid", …] }`
Errors: 409 `ORDER_DUP` (same client_order_id, different body), 422 `R-*`, 423 `KILL_ACTIVE` / `ACCOUNT_NOT_CONNECTED`, 403 `ACCOUNT_NOT_MAPPED` / `LIVE_DISABLED`.
### `POST /orders/{id}/modify` `{ qty?, price?, trigger_price? }` → 202 (re-runs gate).
### `POST /orders/{id}/cancel` → 202.
### `POST /orders/cancel-all` `{ account_id? , strategy_id? }` — trader for own scope, risk_manager any.
### `GET /orders?account_id=&status=&from=&to=&cursor=` (scoped) → paginated.
### `GET /orders/{id}` → order + events + fills.
### `GET /positions?group_by=account|trader|strategy` (scoped) → `[{ account_id, instrument_id, product, net_qty, avg_price, ltp, mtm, realized, costs, delta, gamma, theta, vega }]`
### `GET /pnl?scope=trader&id=&day=` → `{ realized, unrealized, costs, net, series: [[ts, net], …] }`

## 4. Risk
### `GET /risk/limits?scope=&scope_id=` (scoped read) / `PUT /risk/limits` 🔐 risk_manager+
PUT body `[{ scope, scope_id, key, value, hard }]` → 200 with old/new (audited).
### `GET /risk/summary?scope=desk|account|trader|strategy&id=` → Greeks, gross/net, beta-delta, sector exposure, VaR, limit utilisation `[{key, limit, current, pct}]`.
### `POST /risk/scenario` `{ scope, id, spot_shocks_pct: [-10..10], iv_shocks_pct: [-30..30], days_forward: 0 }` → grid of P&L.
### `GET /risk/var?scope=&id=&method=historical|parametric&confidence=0.99&horizon_days=1`
### `GET /risk/stress?scope=&id=` → named scenarios with P&L.
### `GET /risk/factors?scope=&id=` → exposures + contribution to VaR.
### `POST /risk/kill` risk_manager+ (trader may kill own scope) `{ scope, scope_id, actions: ["block","cancel"], reason }` → 200 state.
### `POST /risk/kill/release` 🔐 risk_manager+ `{ scope, scope_id, reason }`.
### `GET /risk/kill` → active switches.

## 5. Research
### `POST /data/jobs` analyst+ `{ account_id, instruments: [ids] | {index: "NIFTY 50"}, interval, from, to, include_expired: false }` → 202 `{ job_id }`
### `GET /data/jobs/{id}` → status + progress. `POST /data/jobs/{id}/cancel`.
### `GET /data/datasets?instrument_id=&interval=`
### `GET /strategies` / `POST /strategies` `{ name, code, params_schema, default_params }` / `GET /strategies/{id}` / `PUT /strategies/{id}/code` (new version)
### `POST /strategies/{id}/backtests` `{ version_id?, params, config: { from, to, interval, universe, fill_model, slippage_ticks, capital, cost_profile, seed } , sweep?: { param_grid }, walk_forward?: { train_days, test_days } }` → 202 `{ run_ids }`
### `GET /backtests/{run_id}` → status, metrics; `GET /backtests/{run_id}/equity`, `/trades.csv`
### `POST /strategies/{id}/transition` `{ to: "PAPER"|"PENDING_APPROVAL"|"LIVE"|"PAUSED"|"RETIRED", account_id?, limits? }` — LIVE requires risk_manager 🔐 and preconditions (PRD F-RS-4); 422 `STRAT_PRECONDITION` with missing items.

## 6. Context & AI
### `GET /news?mine=true&q=&from=&cursor=` → stories with extraction summary and `affects_me` flag.
### `GET /context/graph?entity=instrument:123|company:INE002A01018&depth=2` → `{ nodes: [...], edges: [...] }` (≤ 300 nodes).
### `GET /context/alerts?unacked=true` / `POST /context/alerts/{id}/ack`
### `POST /copilot/sessions` `{ context: { instrument_id?, link_group? } }` → `{ session_id }`
### `POST /copilot/sessions/{id}/messages` `{ text }` → `text/event-stream` events: `delta {text}`, `tool_call {name, args}`, `tool_result {name, summary, ref}`, `ticket_draft {draft_id, order}`, `done {usage}`, `error {code}`.
### `GET /copilot/sessions/{id}` → messages incl. tool traces.
### `GET /briefs?day=&kind=preopen|postclose`

## 7. Brokers & admin
### `GET /brokers/{account_id}/login-url` (mapped trader or admin) → `{ url }` (Kite login URL with api_key; Upstox OAuth authorise URL with state).
### `GET /brokers/callback/zerodha?request_token=&status=&state=` / `GET /brokers/callback/upstox?code=&state=` → exchanges the token (gateway → engine gRPC `StartSession`) → redirects to `/terminal?broker_login=ok`.
### Admin (admin only, 🔐 for mutations): `CRUD /admin/users`, `POST /admin/users/{id}/reset-mfa`, `CRUD /admin/accounts` (secrets write-only), `PUT /admin/accounts/{id}/users`, `CRUD /admin/feeds`, `CRUD /admin/costs`, `CRUD /admin/restricted`, `GET /admin/health`.
### `GET /audit?from=&to=&action=&actor=&cursor=` risk_manager+; `GET /audit/export.csv`; `POST /audit/verify` → `{ ok, rows_checked, first_bad_id }`.
### `GET /compliance/report?day=` → per-account max OPS, order counts, rejects by code, tag coverage %, order-to-trade ratio.

## 8. WebSocket `wss://<host>/ws`
- Auth: first frame `ClientHello { access_token }` within 5 s, or the server closes with 4401.
- Frames: binary protobuf `vyuha.ws.v1.ClientMsg` / `ServerMsg` (see `proto/vyuha/ws/v1/ws.proto`).
- Client → server: `Subscribe { channel, keys[], conflate_ms }`, `Unsubscribe`, `Ping`, `TokenRefresh`.
- Channels: `quote` (keys = instrument ids), `depth`, `bar.1m`, `orders` (own scope), `positions`, `pnl`, `risk.summary`, `alerts`, `ctx`, `kill`, `system`, `ui` (server-pushed UI commands such as `open_ticket`).
- Server → client: `Snapshot` (on subscribe), `Update` batch per conflation tick, `Alert`, `UiCommand`, `Error {code}`, `Pong`.
- Limits: 2,000 instrument keys per connection on `quote`, 50 on `depth`; `conflate_ms` 50–1000 (default 250). Slow consumers: if the outbound buffer is above 4 MB, drop to snapshot-only mode and send `Error SLOW_CONSUMER`.
- Entitlement: quote/depth/bar keys are filtered per user. Non-entitled keys return `Error MD_NOT_ENTITLED` and are not subscribed.

## 9. Engine gRPC (`proto/vyuha/engine/v1/engine.proto`)
Internal only; reachable from the gateway and quant (strategy host) on a private network with mTLS in prod.
`SubmitIntent`, `PreviewIntent`, `ModifyOrder`, `CancelOrder`, `CancelAll`, `SetKill`, `ReleaseKill`, `StartSession`, `StopSession`, `GetSessions`, `ReloadLimits`, `GetPositions`, `Reconcile`, `Health`.

## 10. MCP server (intel) — `https://<host>/mcp` (Streamable HTTP)
Auth: internal service token (copilot) or user PAT (read scope). Tools: see `CONTEXT_ENGINE_AND_AI.md §4`. RBAC is enforced per caller on every tool.
