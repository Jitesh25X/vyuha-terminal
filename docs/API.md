# API Contract (Gateway)

- Base URL: `https://<host>/api/v1`. JSON (`application/json`). Authoritative machine spec: `gateway/api/openapi.yaml`. Generate it from this document at M0 and keep both in sync; the OpenAPI file wins on conflict.
- Auth: `Authorization: Bearer <access_jwt>` unless noted. The refresh token lives in the `vyuha_rt` httpOnly cookie (path `/api/v1/auth`).
- Every mutating request accepts an `Idempotency-Key` header (UUID). Orders use `client_order_id` instead.
- Errors: RFC 9457 `application/problem+json`:
```json
{ "type": "https://vyuha.dev/errors/risk-rejected", "title": "Order rejected by risk gate",
  "status": 422, "detail": "Daily loss limit ₹25,000.00 reached (current loss ₹25,310.50)",
  "code": "R-LOSS-DAILY", "reasons": [{"code":"R-LOSS-DAILY","limit":"25000.00","current":"25310.50"}] }
```
- Common status codes: `400` validation, `401` unauthenticated, `403` RBAC/entitlement, `404`, `409` conflict/idempotency mismatch, `422` business rule (risk), `423` locked (kill switch / account awaiting login), `429` rate limited, `503` engine unavailable.
- Money/prices in JSON are **strings** with up to 4 decimals (`"24310.5000"`). Quantities are integers in units.
- Timestamps: RFC 3339 UTC.
- RBAC scoping: `scoped` means traders see their mapped accounts and own strategies, while risk_manager/admin see everything.

## 0. System
### `GET /health` — no auth → `{ status: "ok"|"degraded", version, env, checks: { postgres: { status: "ok"|"fail", latency_ms, error? } } }`. 200 when all checks pass, 503 otherwise; `error` is a generic reason, never a driver message.

## 1. Auth
### `POST /auth/login` — no auth
Req `{ "username": "meera", "password": "…" }` → 200 `{ "mfa_token": "…", "mfa_enrolled": true }` (the mfa_token is valid for 5 minutes).
Errors: 401 `AUTH_INVALID` (generic message), 423 `AUTH_LOCKED` (`locked_until`).

### `POST /auth/mfa` — mfa_token in body
Req `{ "mfa_token": "…", "totp": "123456" }` → 200 `{ "access_token": "…", "expires_in": 900, "user": {id, username, display_name, role} }` + Set-Cookie refresh.
### `POST /auth/mfa/enroll` (first login) `{ mfa_token }` → `{ "otpauth_url": "…", "qr_png_base64": "…" }`; `POST /auth/mfa/enroll/confirm { mfa_token, totp }` → same response as `/auth/mfa` (session + cookie). 409 `AUTH_MFA_ALREADY_ENROLLED` / `AUTH_MFA_ENROLL_NOT_STARTED`; `/auth/mfa` for a not-enrolled user → 409 `AUTH_MFA_NOT_ENROLLED`.
### `POST /auth/refresh` — cookie → new access token + `user`, rotated cookie. Reuse detected → 401 `AUTH_REFRESH_REUSED` + family revoked; invalid/expired → 401 `AUTH_REFRESH_INVALID`; both clear the cookie. Clients must serialise refresh calls (one in flight per browser), or the second call trips reuse detection.
### `POST /auth/logout` — revoke the current session (refresh family) → 204, cookie cleared.
- Lockout: 5 consecutive failures (wrong password **or** wrong/replayed TOTP, including on `/auth/reauth`) → 423 `AUTH_LOCKED` with `locked_until` for 15 min. The counter resets only after a full password + TOTP success. Each TOTP code is accepted once (replay → `AUTH_INVALID`).
- Every non-public route requires `Authorization: Bearer`; otherwise 401 `AUTH_UNAUTHENTICATED`.
- Rate limits: login / mfa / enroll / confirm share 30 per minute per client; refresh / logout 30 per minute per client; reauth 5 per minute per signed-in user → 429 `RATE_LIMITED` with `Retry-After`. A client is its IPv4 address or IPv6 /64. The client IP is the direct peer unless the peer is in `TRUSTED_PROXIES` (default loopback; required in staging/prod), in which case the right-most untrusted `X-Forwarded-For` hop is used.
- Anonymous credential checks (argon2 + row lock) run at most N at a time (N = a quarter of the DB pool); reauth has its own reserved slot. A request that can't get a slot within 2 s, or whose user row stays locked over 2 s, gets 429 `AUTH_BUSY` with `Retry-After: 2` and is not counted as a failure.
### `POST /auth/reauth` — `{ password, totp }` → `{ "reauth_token": "…", "expires_in": 300 }`. Required header `X-Reauth` for sensitive endpoints (marked 🔐).

## 2. Instruments & market data
### `GET /instruments/search?q=relia&segment=EQ&limit=20` → `[{ id, exchange, segment, tradingsymbol, name, expiry, strike, option_type, lot_size, tick_size }]` (M1.9: prefix on trading symbol or underlying symbol, active contracts only; exact symbol first, then cash/index, nearer expiries; `segment` also accepts an exchange NSE/BSE/NFO/BFO; limit ≤ 50)
### `GET /instruments/{id}` → full instrument incl. `freeze_qty`, `fo_ban`, `underlying_id`.
### `GET /instruments/{id}/expiries` → `["2026-10-28", …]` (M1.10: active option expiries from today IST, nearest first)
### `GET /marketdata/snapshot?ids=1,2,3` (entitled) → `[{ instrument_id, ltp, change_pct, bid, ask, volume, oi, ts, stale }]`
### `GET /marketdata/candles?instrument_id=&interval=1m&from=&to=&adjusted=true` (entitled) → `{ "bars": [[ts, o, h, l, c, v, oi], …] }`. Max 50,000 bars per call; 400 if exceeded. (M1.9: intervals 1m/5m/15m/1h/1d built from recorded 1-minute bars — intraday buckets UTC-aligned, 1d by IST day; ts RFC 3339 UTC, prices decimal strings; entitlement = same rule as the WebSocket hub, 403 `MD_NOT_ENTITLED`; 503 `MD_UNAVAILABLE` when ClickHouse is down; `adjusted` not implemented yet.)
### `GET /indices/{id}/constituents` → `{ index_id, as_of, constituents: [{ instrument_id, tradingsymbol, name, sector, weight, avg_volume_20d, future_id, future_prev_oi }] }` (M1.11: reference data for `MOV`/`HM`, no prices; optional fields absent when unknown; 404 `NO_CONSTITUENTS` until the graph seed provides a source, M6.1)
### `GET /optionchain/{underlying_id}?expiry=2026-10-28&strikes=20` (entitled)
→ `{ underlying: {ltp, fut_ltp}, expiry, pcr, max_pain, atm_strike, rows: [{ strike, ce: {instrument_id, ltp, oi, oi_chg, iv, delta, gamma, theta, vega, bid, ask, volume}, pe: {…} }] }` (M1.10: rows = `strikes` ± N around ATM, 1–50; `expiry` defaults to the nearest; also `lot_size`, `analytics: ok|unavailable`, `model`, `forward`, `forward_source`, `rate`, `rate_as_of`, `t_years`, `as_of`, `contract_ids`, `oi_strikes`, per side `tradingsymbol`/`prev_close`/`iv_source` (mid|ltp); absent fields = unknown, e.g. `oi_chg` without a recorded baseline. Values only from the user's own feeds (ADR-020); the client subscribes `contract_ids` on the WebSocket to make the engine carry the whole expiry. PCR/max pain over every strike of the expiry with OI. IV from a tight mid (spread ≤ `IV_MID_MAX_SPREAD`), else an LTP that traded within `IV_LTP_MAX_AGE_S`, else absent. 403 `MD_NOT_ENTITLED`, 404 no listed options.)

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
### `GET /brokers/accounts` → `[{ id, broker, client_code, holder_name, status, data_entitled, token_expires_at?, can_login }]` — mapped (can_view) accounts; admin and risk_manager see all. `status` reads `awaiting_login` once the stored token has expired; `can_login` follows the login-url rule. No secrets.
### `GET /brokers/{account_id}/login-url` (admin, or a trader mapped with `can_trade`) → `{ url }` (Kite login URL with api_key; Upstox OAuth authorise URL with state). 403 `FORBIDDEN`, 404 `NOT_FOUND`, 409 `BROKER_NO_LOGIN` (sim account). The URL carries a signed, single-use `state` valid for 10 minutes (Kite returns it via `redirect_params`).
### `GET /brokers/callback/zerodha?request_token=&status=&state=` / `GET /brokers/callback/upstox?code=&state=` — no bearer token (the state identifies the user; permissions are re-checked). The gateway exchanges the token **from the account's static IP**, checks the broker's `user_id` equals the account's `client_code`, stores the access token encrypted (`access_token_enc`, expiry 06:00 IST Kite / 03:30 IST Upstox), restarts the engine session (`StopSession` + `StartSession`) and audits `broker.login.success|failure`. Always redirects: `/terminal?broker_login=ok&account=<id>` or `/terminal?broker_login=error&code=<CODE>` with `BROKER_LOGIN_STATE_INVALID`, `BROKER_LOGIN_DENIED`, `BROKER_LOGIN_FORBIDDEN`, `BROKER_ACCOUNT_MISMATCH`, `BROKER_TOKEN_EXCHANGE_FAILED`, `BROKER_SECRETS_UNREADABLE`, `STATIC_IP_NOT_ON_HOST`, `ENGINE_UNAVAILABLE`, `INTERNAL` (or an engine refusal code). `Cache-Control: no-store`, `Referrer-Policy: no-referrer`; rate-limited per client like `/auth/refresh`.
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
- Entitlement (ADR-009, ADR-020): quote/depth/bar keys are filtered per user. A key is entitled when the user is mapped (`can_view`) to a `data_entitled` account whose broker can stream the instrument; ticks are delivered only from that user's own feeds (`Tick.feed_id`). Non-entitled keys return `Error MD_NOT_ENTITLED` (message: the PRD text followed by `[id,id]`) and are not subscribed. Mappings are re-read every 30 s and on `TokenRefresh`; keys that lose entitlement are dropped with the same error.
- Implemented in M1.8: channels `quote`, `depth`, `bar.1m`, `system` (status bar: market state, the user's visible feeds, engine health, data latency; pushed every second). Other channels return `Error BAD_REQUEST` until their tasks. Implemented in M2.2c: `alerts` (`Alert` frames, system alerts for admins only) and `SystemStatus.banners` (sticky status bar text, for every user). If the gateway can't load a user's accounts at hello (Postgres down), the connection still opens: market-data subscriptions get `Error UNAVAILABLE`, and the server closes with 1013 once the accounts load, so the client reconnects and replays them. `Subscribe` replies with a `Snapshot` carrying the request's `req_id`. Access-token expiry without `TokenRefresh` → `Error AUTH_EXPIRED` and close 4401.

## 9. Engine gRPC (`proto/vyuha/engine/v1/engine.proto`)
Internal only; reachable from the gateway and quant (strategy host) on a private network with mTLS in prod.
`SubmitIntent`, `PreviewIntent`, `ModifyOrder`, `CancelOrder`, `CancelAll`, `SetKill`, `ReleaseKill`, `StartSession`, `StopSession`, `GetSessions`, `ReloadLimits`, `GetPositions`, `Reconcile`, `Health`.

## 10. MCP server (intel) — `https://<host>/mcp` (Streamable HTTP)
Auth: internal service token (copilot) or user PAT (read scope). Tools: see `CONTEXT_ENGINE_AND_AI.md §4`. RBAC is enforced per caller on every tool.
