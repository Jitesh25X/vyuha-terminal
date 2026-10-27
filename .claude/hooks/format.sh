#!/usr/bin/env bash
# PostToolUse formatter: formats the single file that was just edited. Never fails the edit.
set -uo pipefail

input="$(cat)"
f="$(printf '%s' "$input" | jq -r '.tool_input.file_path // empty' 2>/dev/null || true)"
if [ -z "$f" ] || [ ! -f "$f" ]; then exit 0; fi

case "$f" in
  *.cc|*.cpp|*.h|*.hpp)      command -v clang-format >/dev/null && clang-format -i "$f" ;;
  *.go)                      command -v gofmt >/dev/null && gofmt -w "$f" ;;
  *.py)                      command -v ruff >/dev/null && ruff format -q "$f" && ruff check -q --fix "$f" ;;
  *.ts|*.tsx|*.css|*.json|*.yaml|*.yml|*.md)
                             [ -x "$CLAUDE_PROJECT_DIR/web/node_modules/.bin/prettier" ] && \
                               "$CLAUDE_PROJECT_DIR/web/node_modules/.bin/prettier" --log-level silent -w "$f" ;;
  *.proto)                   command -v buf >/dev/null && (cd "$CLAUDE_PROJECT_DIR/proto" && buf format -w) ;;
esac
exit 0
