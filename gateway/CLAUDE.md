# gateway/ — Go

Read `docs/API.md`, `docs/ARCHITECTURE.md §6` and the root `CLAUDE.md` first.

## Layout
```
cmd/gateway/      HTTP + WS server
cmd/recorder/     NATS md.> → ClickHouse
cmd/vyuhactl/     admin CLI (user create, audit verify, kill, sessions)
internal/auth     password, TOTP, JWT, refresh rotation, reauth
internal/rbac     role + account-mapping checks (one function per permission; table-tested)
internal/api      handlers generated from api/openapi.yaml (oapi-codegen strict server)
internal/ws       hub, per-client conflation, entitlement filter, slow-consumer handling
internal/enginec  gRPC client to engine
internal/audit    audit writer (INSERT only)
internal/store    sqlc-generated queries (db/queries/*.sql)
internal/crypto   AES-256-GCM envelope for secrets (key from VYUHA_MASTER_KEY)
api/openapi.yaml  REST contract (source of truth for codegen)
```

## Rules
- Contract first: edit `api/openapi.yaml`, run `make gen`, then implement.
- Every handler: authN → RBAC (`rbac.Can(user, action, resource)`) → validate → call → audit (for mutations) → problem+json errors.
- Never log request bodies on `/auth/*`, `/admin/accounts*` or `/brokers/*`. The log middleware has a deny-list.
- **Permitted decrypt sites (owner, 2026-11-03). These five and nowhere else** (`internal/crypto/sites_test.go` fails on a new one):
  1. Engine `StartSession`: API key and secret, opened in `brokers.Service.complete`.
  2. `vyuhactl download run`: the access token, `brokers.OpenAccessToken`.
  3. Login TOTP verification: `auth.Service.totpOK`. Reauth and enrolment confirmation use the same function.
  4. Building the broker login URL: API key, `brokers.Service.LoginURL`.
  5. The broker token exchange: API key and secret, `brokers.Service.complete`.
- Conditions on all five: never logged; never in argv or env (`vyuhactl` uses stdin; the Python job can't zero its `str`, so its copy lasts until it exits); audited on each decrypt (`broker.token.opened`, `broker.secret.opened`, `auth.totp.opened`); zeroed after use where the language allows (Go `[]byte` is, a Go `string` isn't, so the copies that protobuf, form bodies and URLs need last until the garbage collector takes them). Never returned in API responses, except the API key inside the login URL, which the broker's login page requires.
- WS: one goroutine per connection for writes, bounded buffer (4 MB). Conflation ticker per subscription group. Drop to snapshot mode on slow consumers.
- Use `context.Context` with deadlines on every outbound call (engine gRPC 2 s, DB 1 s default).
- Tests: table-driven; `testcontainers-go` for Postgres/NATS integration tests; no engine needed (use a fake gRPC server).
