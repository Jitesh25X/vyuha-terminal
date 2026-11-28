#!/usr/bin/env bash
# Grep-level guards for CLAUDE.md invariants. Cheap, fast, runs in CI and `make verify`.
# Every check reads only files git tracks (`git ls-files | xargs grep`, see tg), so ignored files (local config,
# secrets, build output, caches) are never opened. A new file counts once it is added (`git add -N` is enough).
set -euo pipefail
cd "$(dirname "$0")/.."
# Without a git checkout every check would find nothing and pass: refuse to run instead.
if ! git rev-parse --git-dir >/dev/null 2>&1; then
  echo "✖ check-invariants: not inside a git checkout, so no tracked files to scan" >&2
  exit 2
fi
if [ -z "$(git ls-files | head -n 1)" ]; then
  echo "✖ check-invariants: git tracks no files, so there is nothing to scan" >&2
  exit 2
fi
fail=0
broken="$(mktemp)"
trap 'rm -f "$broken"' EXIT
label=""
check() { # $1 description, $2 command producing offending lines
  label="$1"  # read by tg (same shell, copied into the command substitution) to name the check in errors
  local out; out="$(eval "$2" || true)"
  if [ -s "$broken" ]; then echo "✖ $1: could not scan (see above)" >&2; exit 2; fi
  if [ -n "$out" ]; then echo "✖ $1"; echo "$out" | sed 's/^/    /'; fail=1; else echo "✔ $1"; fi
}
# tg GREP_OPTIONS... -- PATHSPEC...: grep (the system grep, so \b etc. mean what they always did) over the tracked
# files that match the git pathspecs, printing path:line:text.
tg() {
  local opts=() list errs rc=0
  while [ "$1" != "--" ]; do opts+=("$1"); shift; done
  shift
  # A failing `git ls-files` must not look like "nothing found". tg runs inside check's command substitution,
  # so it can't exit the script itself: it marks $broken, and check exits 2 on seeing it.
  list="$(mktemp)"
  if ! git ls-files -z -- "$@" >"$list" 2>/dev/null; then
    rm -f "$list"
    echo "✖ check-invariants: git ls-files failed for: $*" >&2
    echo x >"$broken"
    return 0
  fi
  # xargs can't tell grep's "no match" (1) from its errors (2+): BSD folds every child exit 1-125 into 1, GNU into 123.
  # So the sh wrapper maps grep 0/1 to 0 and anything else to 255, which both xargs stop on and report as non-zero
  # (BSD 1, GNU 124). Any non-zero xargs status is therefore a real error. grep's stderr goes to a file, shown on error.
  # An empty list runs nothing (-r), which is "no match".
  errs="$(mktemp)"
  xargs -0 -r sh -c 'grep -H "$@"; r=$?; [ "$r" -le 1 ] || exit 255' _ "${opts[@]}" <"$list" 2>"$errs" || rc=$?
  rm -f "$list"
  if [ "$rc" -ne 0 ]; then
    {
      echo "✖ check-invariants: grep failed (xargs rc $rc) in check \"$label\" for: $*"
      sed 's/^/    grep: /' "$errs"
    } >&2
    echo "$label" >"$broken"
  fi
  rm -f "$errs"
  return 0
}

check "no live broker order endpoints in tests/fixtures" \
  "tg -InE '(api\.kite\.trade|api(-hft)?\.upstox\.com).*/(orders|gtt)' -- engine/tests quant/tests intel/tests gateway web/e2e | grep -v '/fixtures/vcr/'"

check "LIVE_TRADING_ENABLED never true in committed configs" \
  "tg -InE 'LIVE_TRADING_ENABLED=(1|true|TRUE|yes)' -- . ':!*.md' ':!*check-invariants.sh' ':!*guard-bash.sh' ':!*cases.tsv'"

check "engine: no system_clock::now outside WallClock" \
  "tg -In 'system_clock::now' -- engine/src engine/include | grep -v 'wall_clock'"

check "engine: no double/float prices in oms/risk/broker" \
  "tg -InE '\b(double|float)\b[^;]*(price|Price|ltp|qty)' -- engine/src/oms engine/src/risk engine/src/broker"

# Invariant 1. The type system stops a constructor call (tests/compile_fail.cc); these catch what it can't.
check "engine: ApprovedOrder's only friend is RiskGate" \
  "tg -nE '^[[:space:]]*friend[[:space:]]' -- engine/include/vyuha/risk/approved_order.h | grep -v 'friend class RiskGate;'"

# D-23 and D-24: the kill-generated source only from the kill-switch code, a slice intent only from the slicer.
check "engine: Source's only friend is KillSwitch and SliceIntent's only friend is FreezeSlicer" \
  "tg -nE '^[[:space:]]*friend[[:space:]]' -- engine/include/vyuha/oms/oms.h | grep -vE 'friend class (risk::KillSwitch|FreezeSlicer);|friend bool operator=='"
check "engine: only the kill-switch code names the kill-generated source" \
  "tg -In 'Source::kKillFlatten' -- engine/src engine/include engine/bindings engine/bench | grep -vE '^engine/include/vyuha/(oms/oms\.h|risk/kill_switch\.h):'"
check "engine: no cast or byte copy makes a Source or SliceIntent" \
  "tg -InE '(reinterpret_cast|bit_cast|const_cast|memcpy|memmove|start_lifetime_as)[^;]*(\bSource\b|SliceIntent)' -- engine/src engine/include engine/tests engine/bindings engine/bench"

# compile_fail.cc holds such lines on purpose (ctest compile_fail.* requires each to fail to compile). Only a line
# in that file that also carries the marker is skipped, so a real violation elsewhere in it is still caught.
check "engine: no cast or byte copy makes an ApprovedOrder" \
  "tg -InE '(reinterpret_cast|bit_cast|const_cast|memcpy|memmove|start_lifetime_as)[^;]*ApprovedOrder' -- engine/src engine/include engine/tests engine/bindings engine/bench | grep -vE '^engine/tests/compile_fail\.cc:[0-9]+:.*// invariant-allow: compile-fail fixture\$'"

check "audit_event never updated/deleted/truncated" \
  "tg -InEi '(update|delete from|truncate)[[:space:]]+(table[[:space:]]+)?audit_event' -- '*.go' '*.py' '*.sql' '*.cc' | grep -E '^(gateway|quant|intel|engine|db)/' | grep -v 'audit_no_update\|audit_immutable\|REVOKE'"

check "MCP tool registry has no mutating tools" \
  "tg -InE 'def (place|modify|cancel)_order|set_limit|release_kill' -- intel/src"

check "desk agents: no mutating tools in agent definitions" \
  "tg -InE '(place|modify|cancel)_order|set_limit|release_kill|set_kill|transition_strategy' -- intel/agents"

check "lean: no GPU/CUDA wheels or full Boost requested" \
  "tg -InE 'nvidia-|cuda|\"boost\"[[:space:]]*[,\]]' -- ':(glob)**/pyproject.toml' ':(glob)**/vcpkg.json' ':(glob)**/uv.lock'"

# D-57: tests never migrate or write the dev database. Only the throwaway helpers (gateway/internal/testdb and its
# own test, quant/tests/conftest.py, engine/tests/pg_temp_db.h) read the server URL; they create a database and drop it.
# A test may still apply one migration file to a database that one of those helpers gave it.
test_paths=('gateway/*_test.go' 'quant/tests/*.py' 'intel/tests/*.py' 'engine/tests/*.cc' 'engine/tests/*.h'
  'web/e2e/*.ts' 'web/src/*.test.ts' 'web/src/*.test.tsx')
tests_run_migrations() {
  tg -InE '\b(MigrateUp|runMigrate|newMigrate)\(|\bmigrate\.New\(|\bmigrate[^a-z]+(up|down|force)\b' -- "${test_paths[@]}"
}
tests_reach_the_server() {
  tg -InE '(Getenv|LookupEnv|getenv|environ\.get)\(.(VYUHA_TEST_)?POSTGRES_URL|environ\[.(VYUHA_TEST_)?POSTGRES_URL|process\.env(\.|\[.)(VYUHA_TEST_)?POSTGRES_URL|config\.Load\(os\.Getenv\)|(localhost|127\.0\.0\.1):5432' \
    -- "${test_paths[@]}" ':!gateway/internal/testdb/testdb.go' ':!gateway/internal/testdb/testdb_test.go' ':!quant/tests/conftest.py' ':!engine/tests/pg_temp_db.h'
}
check "tests: migrations run only through the throwaway helpers" tests_run_migrations
check "tests: only the throwaway helpers reach the Postgres server named by the environment" tests_reach_the_server

# The Bash guard hook must still block (and allow) every case on record, so a hook edit can't silently drop one.
check "guard hook: every case in .claude/hooks/tests/cases.tsv" \
  "out=\$(.claude/hooks/tests/run_cases.sh 2>&1) || echo \"\$out\""

# TEST-ONLY seam, used by scripts/tests/check-invariants-test.sh and nothing else: when set, it ADDS one more check
# that greps every tracked file for this pattern. It can't remove, skip or loosen any check above (they have run).
if [ -n "${VYUHA_INVARIANTS_TEST_EXTRA_PATTERN:-}" ]; then
  check "test-only extra pattern" "tg -InE \"\$VYUHA_INVARIANTS_TEST_EXTRA_PATTERN\" -- ."
fi

exit $fail
