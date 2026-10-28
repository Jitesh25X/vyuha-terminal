#!/usr/bin/env bash
# Runs a formatter/scanner for pre-commit if it is installed, otherwise skips with a notice.
# Tools arrive just-in-time with their component's first task (PRD §10.1); CI always runs them.
# Usage: scripts/precommit-tool.sh <tool> [args...] -- [files...]
set -euo pipefail
tool="$1"; shift

case "$tool" in
  prettier) bin="web/node_modules/.bin/prettier" ;;
  *)        bin="$(command -v "$tool" || true)" ;;
esac

if [ -z "$bin" ] || [ ! -x "$bin" ]; then
  echo "· skip $tool (not installed yet)"
  exit 0
fi

args=(); files=()
while [ $# -gt 0 ]; do
  if [ "$1" = "--" ]; then shift; files=("$@"); break; fi
  args+=("$1"); shift
done

# "${a[@]+...}" form: bash 3.2 (macOS) treats an empty array as unbound under `set -u`.
exec "$bin" ${args[@]+"${args[@]}"} ${files[@]+"${files[@]}"}
