---
description: Scaffold a new terminal function panel (mnemonic) in web/
argument-hint: "<MNEMONIC> <short description>"
---

Create terminal function `$ARGUMENTS` using the `terminal-ui` subagent conventions:
1. Confirm the mnemonic exists in `docs/UI_SPEC.md §3`. If it doesn't, propose adding it there first and ask the owner.
2. Add the registry entry, `panels/<MNEMONIC>/index.tsx` (lazy), its store selectors and the required REST/WS hooks.
3. Implement all panel states (loading, empty, error, stale, market closed, not entitled).
4. Add help text and a keyboard-only Playwright test, plus a perf test if the panel streams data.
