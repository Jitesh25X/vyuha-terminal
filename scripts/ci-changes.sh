#!/usr/bin/env bash
# CI path filter: which service jobs a change needs. Writes engine/gateway/quant/web=true|false to
# $GITHUB_OUTPUT (or stdout). Plain git, no third-party action.
#   scripts/ci-changes.sh BASE HEAD     (BASE empty / all zeros / unknown → everything runs)
# Shared inputs (Makefile, scripts, workflows, dev compose, .env.example) run everything; docs-only
# changes run only the hygiene job.
set -euo pipefail

base="${1:-}"
head="${2:-HEAD}"
out="${GITHUB_OUTPUT:-/dev/stdout}"

all() {
  for k in engine gateway quant web; do echo "$k=true" >>"$out"; done
  echo "· ci-changes: $1 → all jobs" >&2
  exit 0
}

[ -n "$base" ] && [ "$base" != "0000000000000000000000000000000000000000" ] || all "no base"
git cat-file -e "$base^{commit}" 2>/dev/null || all "base $base not in this clone"
files="$(git diff --name-only "$base" "$head")"
[ -n "$files" ] || all "empty diff"

match() { grep -Eq "$1" <<<"$files" && echo true || echo false; }

if [ "$(match '^(Makefile|scripts/|\.github/workflows/|deploy/docker-compose\.dev\.yml|\.env\.example)')" = true ]; then
  all "shared build/CI inputs changed"
fi
{
  echo "engine=$(match '^(engine|proto)/')"
  echo "gateway=$(match '^(gateway|proto|db)/')"
  echo "quant=$(match '^(quant|db)/')"
  echo "web=$(match '^(web|gateway|proto|db)/')" # the e2e builds and runs the gateway
} >>"$out"
echo "· ci-changes: $(wc -l <<<"$files" | tr -d ' ') files changed" >&2
