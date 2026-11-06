#!/usr/bin/env bash
# Runs every case in cases.tsv ("block" or "allow", a tab, then a command) through the guard hook as
# PreToolUse JSON and checks the result. The commands are never executed. Exits 1 on any mismatch.
# Usage: run_cases.sh [hook] [cases]   (defaults: ../guard-bash.sh and ./cases.tsv)
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
hook="${1:-$here/../guard-bash.sh}"
cases="${2:-$here/cases.tsv}"
pass=0
fail=0
while IFS=$'\t' read -r want cmd; do
  [ -z "$want" ] && continue
  jq -n --arg c "$cmd" '{tool_input:{command:$c}}' | bash "$hook" >/dev/null 2>&1
  rc=$?
  got=allow
  [ "$rc" -eq 2 ] && got=block
  if [ "$got" = "$want" ]; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    printf 'MISMATCH want=%s got=%s  %s\n' "$want" "$got" "$cmd"
  fi
done <"$cases"
echo "hook cases: pass=$pass fail=$fail"
[ "$fail" -eq 0 ]
