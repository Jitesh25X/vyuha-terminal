#!/usr/bin/env bash
# Applies pending db/migrations/*.up.sql to the dev Postgres container with psql (no host tools needed).
# State is kept in golang-migrate's own table, schema_migrations(version bigint, dirty bool), with the
# same dirty-flag protocol, so the gateway's golang-migrate runner (M0.6) can take over seamlessly.
# Usage: scripts/migrate-pg.sh            apply all pending migrations
#        scripts/migrate-pg.sh status     print the current version
set -euo pipefail
cd "$(dirname "$0")/.."

psql() { docker compose -f deploy/docker-compose.dev.yml exec -T -e PGOPTIONS='-c client_min_messages=warning' postgres psql -U vyuha -d vyuha -v ON_ERROR_STOP=1 -qAt "$@"; }
die() { echo "✖ $*" >&2; exit 1; }

psql -c 'CREATE TABLE IF NOT EXISTS schema_migrations (version bigint NOT NULL PRIMARY KEY, dirty boolean NOT NULL)' \
  || die "cannot reach Postgres (run: make up)"

state="$(psql -c 'SELECT version, dirty FROM schema_migrations LIMIT 1')"
current="${state%%|*}"
[ -n "$current" ] || current=0
if [ "${state#*|}" = t ]; then
  die "database is dirty at version $current: fix it by hand, then: UPDATE schema_migrations SET dirty = false"
fi

if [ "${1:-up}" = status ]; then
  echo "schema version: $current"
  exit 0
fi

applied=0
for f in db/migrations/*.up.sql; do
  name="$(basename "$f")"
  version=$((10#${name%%_*}))
  [ "$version" -gt "$current" ] || continue
  echo "· applying $name"
  psql -c "BEGIN; DELETE FROM schema_migrations; INSERT INTO schema_migrations VALUES ($version, true); COMMIT;"
  psql < "$f" || die "$name failed; schema_migrations left dirty at $version"
  psql -c "UPDATE schema_migrations SET dirty = false"
  current=$version
  applied=$((applied + 1))
done
echo "✔ schema version $current ($applied applied)"
