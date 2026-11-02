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
- Secrets from the DB may be decrypted for the engine's `StartSession` and for `vyuhactl download run`, and nowhere else (owner, 2026-11-02). They are never returned in API responses.
- `vyuhactl download run` must: pass the token to the Python downloader via stdin or an inherited pipe, never argv or an env var (both are visible to other processes); zero the buffer after use; audit each decrypt (`broker.token.opened`); never log it. Tests: `cmd/vyuhactl/download_test.go`, `test_token_is_not_in_argv_env_or_logs` in quant. Python can't zero a `str`, so the downloader's copy lasts until the job exits.
- **Older than this rule and not covered by it** (`[DECISION NEEDED]`, `docs/DECISIONS.md` D-16): TOTP check at user login (`internal/auth/service.go`), API key for the broker login URL and API key + secret for the token exchange (`internal/brokers/service.go`). Don't add to this list.
- WS: one goroutine per connection for writes, bounded buffer (4 MB). Conflation ticker per subscription group. Drop to snapshot mode on slow consumers.
- Use `context.Context` with deadlines on every outbound call (engine gRPC 2 s, DB 1 s default).
- Tests: table-driven; `testcontainers-go` for Postgres/NATS integration tests; no engine needed (use a fake gRPC server).
