#!/usr/bin/env bash
# Schema drift check (D-57): builds a throwaway database from db/migrations on the dev Postgres and compares it
# with the dev database: schema_migrations (version, dirty), then a normalised schema-only pg_dump.
#   make migrate-check                                 compares the database "vyuha"
#   VYUHA_MIGRATE_CHECK_DB=name make migrate-check     compares another database on the same server
# The compared database is only read (every session on it is read-only). The throwaway one is always dropped.
# Roles are cluster-wide and not compared; neither are object owners or database-level grants.
# Needs `make up`. Reads no env file but env.example: pg_dump and psql run inside the Postgres container.
# Exit: 0 no drift, 1 drift (only from the comparison below), 2 the check could not run (any other failure).
set -Eeuo pipefail
trap 'echo "✖ migrate-check: could not run (failed command at line $LINENO)" >&2; exit 2' ERR

root="$(cd "$(dirname "$0")/.." && pwd)"
compose=(docker compose -f "$root/deploy/docker-compose.dev.yml" --env-file "$root/env.example")
target="${VYUHA_MIGRATE_CHECK_DB:-vyuha}"
if ! [[ "$target" =~ ^[a-z0-9_]+$ ]]; then
  echo "✖ migrate-check: VYUHA_MIGRATE_CHECK_DB must be a database name ([a-z0-9_]+)" >&2
  exit 2
fi
if ! "${compose[@]}" ps --status running -q postgres 2>/dev/null | grep -q .; then
  echo "✖ migrate-check: postgres is not running (make up)" >&2
  exit 2
fi

tmp="$(mktemp -d)"
fresh="vyuha_migcheck_$$_$RANDOM"
psql_admin() { "${compose[@]}" exec -T postgres psql -X -v ON_ERROR_STOP=1 -U vyuha -d postgres -qc "$1" >/dev/null; }
cleanup() {
  psql_admin "DROP DATABASE IF EXISTS \"$fresh\" WITH (FORCE)" || echo "✖ migrate-check: could not drop $fresh" >&2
  rm -rf "$tmp"
}
trap cleanup EXIT

# Every session on the compared database is read-only.
ro() { "${compose[@]}" exec -T -e PGOPTIONS='-c default_transaction_read_only=on' postgres "$@"; }
# Prints "version N, dirty=B", "no schema_migrations table" or "no schema_migrations row"; fails if psql does.
version_of() {
  local has v
  has="$(ro psql -X -v ON_ERROR_STOP=1 -U vyuha -d "$1" -tAc "SELECT to_regclass('public.schema_migrations') IS NOT NULL")" || return 1
  if [ "$has" != t ]; then echo "no schema_migrations table"; return 0; fi
  v="$(ro psql -X -v ON_ERROR_STOP=1 -U vyuha -d "$1" -tAc \
    "SELECT 'version ' || version || ', dirty=' || dirty::text FROM schema_migrations")" || return 1
  echo "${v:-no schema_migrations row}"
}

# The compared database must exist: say so plainly, before anything is created.
found="$(ro psql -X -v ON_ERROR_STOP=1 -U vyuha -d postgres -tAc "SELECT count(*) FROM pg_database WHERE datname = '$target'")"
if [ "$found" != 1 ]; then
  echo "✖ migrate-check: database \"$target\" does not exist on the dev Postgres (VYUHA_MIGRATE_CHECK_DB)" >&2
  exit 2
fi

# One line per statement (function bodies keep their lines), statements sorted. CREATE TABLE becomes one line per
# column or inline constraint, so column order (which a down/up can change) doesn't count. Enum order does.
normalise() {
  awk '
    function squeeze(s) { gsub(/^[ \t]+|[ \t]+$/, "", s); return s }
    function emit() { if (stmt != "") print stmt; stmt = "" }
    # Tracks whether the line leaves us inside a dollar-quoted body.
    function quotes(s,   q) {
      while (match(s, /\$[A-Za-z_]*\$/)) {
        q = substr(s, RSTART, RLENGTH); s = substr(s, RSTART + RLENGTH)
        if (tag == "") { tag = q; body = 1 } else if (tag == q) tag = ""
      }
    }
    tag == "" && stmt == "" && table == "" && (/^--/ || /^[ \t]*$/ || /^\\(un)?restrict / || /^SET / ||
      /^SELECT pg_catalog\.set_config/) { next }
    table == "" && stmt == "" && /^CREATE (UNLOGGED |FOREIGN )?TABLE .*\($/ {
      table = $0; sub(/[ \t]*\($/, "", table); next
    }
    table != "" {
      if ($0 ~ /^\)/) {
        if (member != "") print table " | " member
        print table " ) " $0
        table = ""; member = ""; next
      }
      line = squeeze($0)
      member = member == "" ? line : member " " line
      if (member ~ /,$/) { sub(/,$/, "", member); print table " | " member; member = "" }
      next
    }
    {
      quotes($0)
      if (stmt == "") stmt = $0
      else if (body) stmt = stmt "\001" $0
      else stmt = stmt " " squeeze($0)
      if (tag == "" && $0 ~ /;[ \t]*$/) { emit(); body = 0 }
    }
    END { emit() }
  ' | LC_ALL=C sort | tr '\001' '\n'
}

dump() { # $1 database, $2 output file
  ro pg_dump --schema-only --no-owner -U vyuha -d "$1" >"$tmp/raw.sql"
  normalise <"$tmp/raw.sql" >"$2"
  if ! [ -s "$2" ]; then echo "✖ migrate-check: the dump of $1 is empty" >&2; return 1; fi
}

psql_admin "CREATE DATABASE \"$fresh\""
(cd "$root/gateway" && VYUHA_ENV=dev LIVE_TRADING_ENABLED=false \
  POSTGRES_URL="postgres://vyuha:vyuha@localhost:5432/$fresh?sslmode=disable" \
  go run ./cmd/vyuhactl migrate up -dir ../db/migrations >/dev/null)

drift=0
want="$(version_of "$fresh")"
got="$(version_of "$target")"
if [ "$want" != "$got" ]; then
  echo "✖ migrate-check: schema_migrations differs: db/migrations gives \"$want\", database $target has \"$got\""
  drift=1
fi

dump "$fresh" "$tmp/fresh.sql"
dump "$target" "$tmp/target.sql"
diff_rc=0
diff -U1 --label "db/migrations (fresh database)" --label "database $target" \
  "$tmp/fresh.sql" "$tmp/target.sql" >"$tmp/diff" || diff_rc=$?
if [ "$diff_rc" -eq 1 ]; then # diff's own failure is 2: that is "could not run", handled below
  echo "✖ migrate-check: schema differs (- only in db/migrations, + only in database $target)"
  cat "$tmp/diff"
  drift=1
elif [ "$diff_rc" -ne 0 ]; then
  echo "✖ migrate-check: could not run (diff failed with $diff_rc)" >&2
  exit 2
fi

if [ $drift -ne 0 ]; then
  echo "hint: if $target is behind, run \`make migrate\`; if it ran a draft migration, roll that one back and re-apply it."
  exit 1
fi
echo "✔ migrate-check: database $target matches db/migrations ($want)"
