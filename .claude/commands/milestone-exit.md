---
description: Check whether the current milestone's exit criteria are met
---

1. Read the current milestone section in `docs/TASKS.md` and its exit criteria.
2. For each task and exit criterion, find the evidence (tests, benchmark output, staging logs) and mark it ✅ / ❌ with file:line or command output.
3. Run `/verify`.
4. Run the `compliance-auditor` subagent if the milestone is M3 or later.
5. Run `make clean-space` and report disk usage per component. Flag anything large that no task's `needs:` justifies.
6. If everything passes, tick the milestone's exit box in `docs/TASKS.md` and update `Current milestone:`. Otherwise list the blocking items in priority order.
