#!/usr/bin/env bash
# Grep-level guards for CLAUDE.md invariants. Cheap, fast, runs in CI and `make verify`.
set -euo pipefail
cd "$(dirname "$0")/.."
fail=0
check() { # $1 description, $2 command producing offending lines
  local out; out="$(eval "$2" || true)"
  if [ -n "$out" ]; then echo "✖ $1"; echo "$out" | sed 's/^/    /'; fail=1; else echo "✔ $1"; fi
}

check "no live broker order endpoints in tests/fixtures" \
  "grep -RInE '(api\.kite\.trade|api(-hft)?\.upstox\.com).*/(orders|gtt)' --include='*' engine/tests quant/tests intel/tests gateway web/e2e 2>/dev/null | grep -v '/fixtures/vcr/'"

check "LIVE_TRADING_ENABLED never true in committed configs" \
  "grep -RInE 'LIVE_TRADING_ENABLED=(1|true|TRUE|yes)' --exclude-dir=.git --exclude-dir=node_modules --exclude=check-invariants.sh --exclude=guard-bash.sh --exclude=cases.tsv --exclude='*.md' ."

check "engine: no system_clock::now outside WallClock" \
  "grep -RIn 'system_clock::now' engine/src engine/include 2>/dev/null | grep -v 'wall_clock'"

check "engine: no double/float prices in oms/risk/broker" \
  "grep -RInE '\b(double|float)\b[^;]*(price|Price|ltp|qty)' engine/src/oms engine/src/risk engine/src/broker 2>/dev/null"

# Invariant 1. The type system stops a constructor call (tests/compile_fail.cc); these catch what it can't.
check "engine: ApprovedOrder's only friend is RiskGate" \
  "grep -nE '^[[:space:]]*friend[[:space:]]' engine/include/vyuha/risk/approved_order.h | grep -v 'friend class RiskGate;'"

# compile_fail.cc holds such lines on purpose (ctest compile_fail.* requires each to fail to compile). Only a line
# in that file that also carries the marker is skipped, so a real violation elsewhere in it is still caught.
check "engine: no cast or byte copy makes an ApprovedOrder" \
  "grep -RInE '(reinterpret_cast|bit_cast|const_cast|memcpy|memmove|start_lifetime_as)[^;]*ApprovedOrder' engine/src engine/include engine/tests engine/bindings engine/bench 2>/dev/null | grep -vE '^engine/tests/compile_fail\.cc:[0-9]+:.*// invariant-allow: compile-fail fixture\$'"

check "audit_event never updated/deleted/truncated" \
  "grep -RInEi '(update|delete from|truncate)[[:space:]]+(table[[:space:]]+)?audit_event' --include='*.go' --include='*.py' --include='*.sql' --include='*.cc' gateway quant intel engine db 2>/dev/null | grep -v 'audit_no_update\|audit_immutable\|REVOKE'"

check "MCP tool registry has no mutating tools" \
  "grep -RInE 'def (place|modify|cancel)_order|set_limit|release_kill' intel/src 2>/dev/null"

check "desk agents: no mutating tools in agent definitions" \
  "grep -RInE '(place|modify|cancel)_order|set_limit|release_kill|set_kill|transition_strategy' intel/agents 2>/dev/null"

check "lean: no GPU/CUDA wheels or full Boost requested" \
  "grep -RInE 'nvidia-|cuda|\"boost\"[[:space:]]*[,\]]' --include=pyproject.toml --include=vcpkg.json --include=uv.lock . 2>/dev/null"

# The Bash guard hook must still block (and allow) every case on record, so a hook edit can't silently drop one.
check "guard hook: every case in .claude/hooks/tests/cases.tsv" \
  "out=\$(.claude/hooks/tests/run_cases.sh 2>&1) || echo \"\$out\""

exit $fail
