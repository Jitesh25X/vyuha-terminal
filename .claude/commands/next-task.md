---
description: Pick up the next unchecked roadmap task and implement it end-to-end
argument-hint: "[optional task id, e.g. M1.4]"
---

1. Open `docs/TASKS.md`. Choose task `$ARGUMENTS` if given, otherwise the **first `[ ]` task whose deps are all `[x]`**. Refuse a task whose deps aren't done, or one from a later milestone while the current milestone's exit box is unchecked. Work on exactly one task.
2. Read the task row in `docs/ROADMAP.md`, the PRD feature(s) it maps to, and the relevant spec + service `CLAUDE.md`.
3. Post a short plan: feature IDs, acceptance criteria, files to touch, tests to write, **what you will install/download (must match the task's `needs:` line exactly)**, and any `[DECISION NEEDED]`. If a decision touches money, risk, compliance or broker behaviour, or you need a download not listed, stop and ask.
4. Claude Code writes all code (ADR-017).
5. Tests first → implementation → `make verify` (or the service-level equivalent).
6. If the diff touches risk/OMS/broker/auth/RBAC/MCP/agents, run the `risk-reviewer` subagent and address every P0/P1.
7. Tick the task in `docs/TASKS.md` (`[x]` + `— <date>: <note>`), log any deviation in its notes section, and commit with a Conventional Commit message containing the task ID.
8. Stop. Report what was done and which task is next. Don't start the next task unless the owner says to continue.
