#!/usr/bin/env bash
# Tests scripts/check-invariants.sh in a throwaway git repo (removed on exit). No dependencies beyond bash and git.
# The copied script needs a few repo paths; this test creates them as minimal tracked files, so the script itself
# has no "skip a check when its input is missing" mode that could hide a real violation in the real repo.
set -uo pipefail
src="$(cd "$(dirname "$0")/.." && pwd)/check-invariants.sh"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
failures=0

new_repo() { # $1 name: a clean repo with the script and the files the checks look at
  local r="$work/$1"
  mkdir -p "$r/scripts" "$r/.claude/hooks/tests" "$r/engine/include/vyuha/risk" "$r/engine/include/vyuha/oms" "$r/engine/src/oms"
  cp "$src" "$r/scripts/check-invariants.sh"
  printf '#!/bin/sh\nexit 0\n' >"$r/.claude/hooks/tests/run_cases.sh"
  chmod +x "$r/.claude/hooks/tests/run_cases.sh"
  printf 'class ApprovedOrder { friend class RiskGate; };\n' >"$r/engine/include/vyuha/risk/approved_order.h"
  printf 'struct Source {};\n' >"$r/engine/include/vyuha/oms/oms.h"
  printf 'int ok = 1;\n' >"$r/engine/src/oms/ok.cc"
  git -C "$r" init -q
  git -C "$r" add -A
  echo "$r"
}

expect() { # $1 case name, $2 wanted exit code, $3 text the output must contain (or empty), then: repo, then env assignments
  local name="$1" want="$2" text="$3" repo="$4" out rc
  shift 4
  out="$(cd "$repo" && env "$@" bash scripts/check-invariants.sh 2>&1)"; rc=$?
  if [ "$rc" -ne "$want" ]; then
    echo "FAIL $name: exit $rc, wanted $want"; echo "$out" | sed 's/^/    /'; failures=$((failures + 1)); return
  fi
  if [ -n "$text" ] && ! printf '%s' "$out" | grep -qF -- "$text"; then
    echo "FAIL $name: output lacks: $text"; echo "$out" | sed 's/^/    /'; failures=$((failures + 1)); return
  fi
  echo "ok   $name (exit $rc)"
}

# (a) a clean tree passes
r="$(new_repo clean)"
expect "clean tree passes" 0 "" "$r" X=1

# (b) a pattern grep rejects (unbalanced parenthesis under -E) must fail the run with exit 2, naming the check
expect "broken pattern fails with exit 2" 2 "test-only extra pattern" "$r" 'VYUHA_INVARIANTS_TEST_EXTRA_PATTERN=('

# the seam alone, with a valid pattern that matches nothing, still passes
expect "valid extra pattern with no match passes" 0 "" "$r" 'VYUHA_INVARIANTS_TEST_EXTRA_PATTERN=no_such_text_anywhere'

# (c) a tracked file that is gone from the work tree makes grep fail: exit 2, and grep's own message is shown
r="$(new_repo unreadable)"
printf 'hello\n' >"$r/notes.txt"
git -C "$r" add notes.txt
rm "$r/notes.txt"
expect "tracked file that cannot be read fails with exit 2" 2 "No such file" "$r" X=1

# (d) a real violation still fails with exit 1 (the old behaviour is kept)
r="$(new_repo violation)"
printf 'double price = 1;\n' >"$r/engine/src/oms/bad.cc"
git -C "$r" add engine/src/oms/bad.cc
expect "real violation fails with exit 1" 1 "no double/float prices" "$r" X=1

if [ "$failures" -ne 0 ]; then echo "✖ $failures case(s) failed"; exit 1; fi
echo "✔ check-invariants tests passed"
