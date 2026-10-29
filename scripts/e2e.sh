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
feed_addr="127.0.0.1:${VYUHA_E2E_FAKEFEED_PORT:-18091}"

if ! "${compose[@]}" ps --status running -q postgres 2>/dev/null | grep -q .; then
  echo "✖ e2e: postgres is not running (make up)" >&2
  exit 1
fi

db="vyuha_e2e_$$"
tmp="$(mktemp -d)"
gw_pid=""
feed_pid=""
psql_admin() { "${compose[@]}" exec -T postgres psql -U vyuha -d postgres -qc "$1" >/dev/null; }
cleanup() {
  if [ -n "$gw_pid" ]; then kill "$gw_pid" 2>/dev/null || true; wait "$gw_pid" 2>/dev/null || true; fi
  if [ -n "$feed_pid" ]; then kill "$feed_pid" 2>/dev/null || true; wait "$feed_pid" 2>/dev/null || true; fi
  psql_admin "DROP DATABASE IF EXISTS \"$db\" WITH (FORCE)" || true
  rm -rf "$tmp"
}
trap cleanup EXIT

psql_admin "CREATE DATABASE \"$db\""
export VYUHA_ENV=dev POSTGRES_URL="$pg_base/$db?sslmode=disable"

# Generated protobuf code is gitignored: (re)generate when missing or older than a .proto file.
stale() { [ ! -f "$1" ] || [ -n "$(find "$root/proto" -name '*.proto' -newer "$1")" ]; }
if stale "$root/gateway/gen/.stamp" || stale "$root/web/src/gen/.stamp"; then
  make -C "$root" --no-print-directory proto >/dev/null
fi
cd "$root/gateway"
go build -o "$tmp/gateway" ./cmd/gateway
go build -o "$tmp/vyuhactl" ./cmd/vyuhactl
go build -o "$tmp/fakefeed" ./cmd/fakefeed # perf test data source (speaks the /ws protocol)
"$tmp/fakefeed" -addr "$feed_addr" -hz 4 >"$tmp/fakefeed.log" 2>&1 &
feed_pid=$!
"$tmp/vyuhactl" migrate up >/dev/null
"$tmp/vyuhactl" secrets init -env-file "$tmp/env" -env-example "$root/.env.example" -jwt-key "$tmp/jwt.pem" >/dev/null

user="e2e_trader"
password="$(openssl rand -hex 16)"
printf '%s\n' "$password" | "$tmp/vyuhactl" user create -username "$user" -display-name "E2E Trader" -role trader >/dev/null
# Market panels (keyboard) and the perf test each enroll their own user (TOTP codes are single-use).
for u in e2e_md e2e_perf e2e_oc e2e_mov; do
  printf '%s\n' "$password" | "$tmp/vyuhactl" user create -username "$u" -display-name "E2E ${u#e2e_}" -role trader >/dev/null
done
# A few instruments for the market panels to resolve (no broker account is mapped: no entitlement).
"${compose[@]}" exec -T postgres psql -U vyuha -d "$db" -qc "INSERT INTO instrument (id, exchange, segment, symbol, tradingsymbol, name, tick_size, lot_size, valid_from, kite_token, upstox_key) VALUES
  (1001, 'NSE', 'EQ', 'RELIANCE', 'RELIANCE', 'Reliance Industries', 0.10, 1, '2026-01-01', 738561, 'NSE_EQ|INE002A01018'),
  (1002, 'NSE', 'EQ', 'INFY', 'INFY', 'Infosys', 0.10, 1, '2026-01-01', 408065, 'NSE_EQ|INE009A01021')" >/dev/null
# OC/OI: a NIFTY index with one expiry a week out (3 strikes × CE/PE) and its same-expiry future.
"${compose[@]}" exec -T postgres psql -U vyuha -d "$db" -qc "
  INSERT INTO instrument (id, exchange, segment, symbol, tradingsymbol, name, tick_size, lot_size, valid_from, kite_token) VALUES
    (1100, 'NSE', 'IDX', 'NIFTY', 'NIFTY 50', 'Nifty 50', 0.05, 1, '2026-01-01', 256265);
  INSERT INTO instrument (id, exchange, segment, symbol, tradingsymbol, underlying_id, expiry, strike, option_type, tick_size, lot_size, valid_from, kite_token)
  SELECT 1200 + n, 'NFO', 'OPT', 'NIFTY', 'NIFTYE2E' || k || ot, 1100, CURRENT_DATE + 7, k, ot::option_type, 0.05, 75, '2026-01-01', 900000 + n
  FROM (VALUES (1, 24200, 'CE'), (2, 24200, 'PE'), (3, 24300, 'CE'), (4, 24300, 'PE'), (5, 24400, 'CE'), (6, 24400, 'PE')) AS v(n, k, ot);
  INSERT INTO instrument (id, exchange, segment, symbol, tradingsymbol, underlying_id, expiry, tick_size, lot_size, valid_from, kite_token) VALUES
    (1210, 'NFO', 'FUT', 'NIFTY', 'NIFTYE2EFUT', 1100, CURRENT_DATE + 7, 0.10, 75, '2026-01-01', 900010)" >/dev/null

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
  VYUHA_E2E_MD_USER=e2e_md VYUHA_E2E_PERF_USER=e2e_perf VYUHA_E2E_OC_USER=e2e_oc VYUHA_E2E_MOV_USER=e2e_mov VYUHA_E2E_FAKEFEED="$feed_addr" \
  pnpm exec playwright test "$@" || status=$?
if [ $status -ne 0 ]; then
  echo "── gateway log ──" >&2
  cat "$tmp/gateway.log" >&2
fi
exit $status
