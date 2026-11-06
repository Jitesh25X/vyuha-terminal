#!/usr/bin/env bash
# PreToolUse guard for Bash. Exit 2 = block (stderr is shown to Claude).
# Blocks: direct calls to live broker order endpoints, enabling live trading, printing secrets, destroying audit_event.
# Every rule has cases in .claude/hooks/tests/cases.tsv, run by scripts/check-invariants.sh (CI and `make verify`).
# A blocked command means stop and tell the owner; never route around this hook (CLAUDE.md working rules).
set -euo pipefail

input="$(cat)"
cmd="$(printf '%s' "$input" | jq -r '.tool_input.command // empty' 2>/dev/null || true)"
[ -z "$cmd" ] && exit 0

block() { echo "BLOCKED by .claude/hooks/guard-bash.sh: $1" >&2; exit 2; }

# 1) Live broker order endpoints must never be called from the agent's shell.
if printf '%s' "$cmd" | grep -Eiq '(api\.kite\.trade|api(-hft)?\.upstox\.com)[^ ]*/(orders|order|gtt|margins/orders)'; then
  block "direct calls to live broker order endpoints are forbidden (invariant #3). Use SimBroker or VCR fixtures."
fi

# 2) Never flip live trading on from the agent.
if printf '%s' "$cmd" | grep -Eq 'LIVE_TRADING_ENABLED=(1|true|TRUE|yes)'; then
  block "enabling LIVE_TRADING_ENABLED is an owner-only action."
fi

# 3) Don't dump secrets.
# A reader runs as a command word: at the start, or after whitespace, ; & | ( or a backtick, and followed by whitespace.
# So "detail", "cats" or "s.tail" are not readers.
readers='(^|[[:space:];&|(`])(cat|less|more|head|tail|bat|nl|strings|xxd|od)[[:space:]]'
# An env file is a whole path component:
#   .env or .env.<suffix>, e.g. .env, ./.env, deploy/.env.staging, ".env.local"
#   <name>.env with no suffix, e.g. prod.env, deploy/staging.env
# The component must start after a boundary (start, whitespace, / ' " = < or a backtick) and end at one
# (end, whitespace, ' " ; | & ) > or a backtick). So s.env.Open, os.Getenv and config.env.go don't match.
# .env.example is allowed (it holds no secrets): it is removed before matching.
b_open='(^|[[:space:]/'"'"'"=<`])'
b_close='([[:space:]'"'"'";|&)>`]|$)'
envfile="${b_open}(\\.env(\\.[A-Za-z0-9_-]+)?|[A-Za-z0-9_-][A-Za-z0-9_.-]*\\.env)${b_close}"
scrubbed="$(printf '%s' "$cmd" | sed -E 's/\.env\.example([^A-Za-z0-9_.-]|$)/\1/g')"
if printf '%s' "$cmd" | grep -Eq "$readers" && printf '%s' "$scrubbed" | grep -Eq "$envfile"; then
  block "reading .env files is not allowed; use .env.example."
fi
if printf '%s' "$cmd" | grep -Eq '(printenv|env)[[:space:]]*(\||$).*' && printf '%s' "$cmd" | grep -Eqi 'master_key|secret|token|api_key'; then
  block "printing secret environment variables is not allowed."
fi

# 4) Never drop the audit table or run destructive SQL against non-dev DBs.
if printf '%s' "$cmd" | grep -Eiq '(drop|truncate|delete[[:space:]]+from)[[:space:]]+(table[[:space:]]+)?audit_event'; then
  block "audit_event is append-only."
fi

exit 0
