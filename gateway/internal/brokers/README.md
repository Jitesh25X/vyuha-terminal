# Broker login (gateway)

Daily broker login for each account (API.md §7, PRD "Broker auth"). The user logs in on the broker's
own site with the broker's 2FA; Vyuha never automates broker TOTP.

## Flow
1. `GET /api/v1/brokers/{account_id}/login-url` — admin, or a trader mapped with `can_trade`. Returns the
   broker URL with a signed single-use `state` (HMAC-SHA256, 10 min, in-memory key: a gateway restart
   invalidates logins in progress).
2. The broker redirects the browser to `<PUBLIC_BASE_URL>/api/v1/brokers/callback/<broker>`. The state
   identifies the user; the account mapping and role are re-checked.
3. The token exchange leaves from the account's `static_ip` (invariant 4; `EADDRNOTAVAIL` →
   `STATIC_IP_NOT_ON_HOST`, nothing sent). Environment proxies are ignored.
4. The broker's `user_id` must equal the account's `client_code` (`BROKER_ACCOUNT_MISMATCH` otherwise, token
   discarded).
5. The access token is sealed (AES-256-GCM, associated data `broker_account.access_token_enc:<id>`) with its
   expiry; the engine session is restarted with the decrypted key, secret and token; the WebSocket hub learns
   the new feed id at once; `broker.login.success|failure` is audited (no secrets in the payload).

API keys and secrets are written with `vyuhactl account create` (read from the terminal or stdin, never flags)
and sealed the same way (`api_key_enc` / `api_secret_enc`). They are decrypted only to build the login URL
(key) and for the exchange and `StartSession` (key, secret, token), and are never returned by the API.

## Broker docs (verified 2026-10-29)
| Broker | Doc | What we use |
|---|---|---|
| Zerodha Kite Connect v3 | https://kite.trade/docs/connect/v3/user/ | Login `https://kite.zerodha.com/connect/login?v=3&api_key=…&redirect_params=<urlencoded>`; redirect carries `request_token` (+ `status`, `action`) and the `redirect_params`; `POST https://api.kite.trade/session/token` form `api_key, request_token, checksum=SHA-256(api_key+request_token+api_secret)`, header `X-Kite-Version: 3`; response `data.user_id`, `data.access_token`. Token expires 06:00 IST next day (regulatory). `request_token` expires within minutes. The redirect URL is fixed in the Kite developer console. |
| Upstox API v2 auth | https://upstox.com/developer/api-documentation/authentication/ , https://upstox.com/developer/api-documentation/get-token/ | Dialog `https://api.upstox.com/v2/login/authorization/dialog?response_type=code&client_id=…&redirect_uri=…&state=…`; redirect carries `code`, `state`; `POST https://api.upstox.com/v2/login/authorization/token` form `code, client_id, client_secret, redirect_uri, grant_type=authorization_code`, `Accept: application/json`; response `user_id`, `access_token` (also `extended_token`, unused). Code is single-use whether or not the exchange succeeds. Token lasts until 03:30 IST the following day. `redirect_uri` must match the app's registered URL exactly. |

## Tests
No test touches a broker: `brokers_test.go` and `service_integration_test.go` use local fake servers
bound to 127.0.0.1 (the static-IP refusal uses TEST-NET-3 203.0.113.7).
