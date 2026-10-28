#!/usr/bin/env bash
# Playwright e2e (Chromium) against a throwaway gateway + Postgres database.
# Never touches the dev database or .env: a fresh database, an ephemeral JWT key and master key in a
# temp dir, and a gateway on its own port. No broker is involved (invariant 3).
#   scripts/e2e.sh [playwright args…]      (via `make e2e`; needs `make up`)
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
compose=(docker compose -f "$root/deploy/docker-compose.dev.yml")
pg_base="${VYUHA_E2E_POSTGRES:-postgres://vyuha:vyuha@localhost:5432}"
gw_addr="127.0.0.1:${VYUHA_E2E_GATEWAY_PORT:-18080}"
web_port="${VYUHA_WEB_PORT:-5174}"

if ! "${compose[@]}" ps --status running -q postgres 2>/dev/null | grep -q .; then
  echo "✖ e2e: postgres is not running (make up)" >&2
  exit 1
fi

db="vyuha_e2e_$$"
tmp="$(mktemp -d)"
gw_pid=""
psql_admin() { "${compose[@]}" exec -T postgres psql -U vyuha -d postgres -qc "$1" >/dev/null; }
cleanup() {
  if [ -n "$gw_pid" ]; then kill "$gw_pid" 2>/dev/null || true; wait "$gw_pid" 2>/dev/null || true; fi
  psql_admin "DROP DATABASE IF EXISTS \"$db\" WITH (FORCE)" || true
  rm -rf "$tmp"
}
trap cleanup EXIT

psql_admin "CREATE DATABASE \"$db\""
export VYUHA_ENV=dev POSTGRES_URL="$pg_base/$db?sslmode=disable"

make -C "$root" --no-print-directory proto >/dev/null # Go protobuf code is gitignored
cd "$root/gateway"
go build -o "$tmp/gateway" ./cmd/gateway
go build -o "$tmp/vyuhactl" ./cmd/vyuhactl
"$tmp/vyuhactl" migrate up >/dev/null
"$tmp/vyuhactl" secrets init -env-file "$tmp/env" -env-example "$root/.env.example" -jwt-key "$tmp/jwt.pem" >/dev/null

user="e2e_trader"
password="$(openssl rand -hex 16)"
printf '%s\n' "$password" | "$tmp/vyuhactl" user create -username "$user" -display-name "E2E Trader" -role trader >/dev/null

VYUHA_MASTER_KEY="$(sed -n 's/^VYUHA_MASTER_KEY=//p' "$tmp/env")" \
  JWT_SIGNING_KEY_PATH="$tmp/jwt.pem" GATEWAY_HTTP_ADDR="$gw_addr" LOG_LEVEL=warn OTEL_EXPORTER_OTLP_ENDPOINT= \
  "$tmp/gateway" >"$tmp/gateway.log" 2>&1 &
gw_pid=$!
for _ in $(seq 50); do
  curl -fs "http://$gw_addr/api/v1/health" >/dev/null && break
  kill -0 "$gw_pid" 2>/dev/null || break
  sleep 0.2
done
if ! curl -fs "http://$gw_addr/api/v1/health" >/dev/null; then
  echo "✖ e2e: gateway did not become healthy" >&2
  cat "$tmp/gateway.log" >&2
  exit 1
fi

cd "$root/web"
status=0
VYUHA_GATEWAY_URL="http://$gw_addr" VYUHA_WEB_PORT="$web_port" VYUHA_E2E_USER="$user" VYUHA_E2E_PASSWORD="$password" \
  pnpm exec playwright test "$@" || status=$?
if [ $status -ne 0 ]; then
  echo "── gateway log ──" >&2
  cat "$tmp/gateway.log" >&2
fi
exit $status
