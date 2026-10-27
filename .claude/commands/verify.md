---
description: Run the full verification suite and summarise failures by service
---

Run `make verify`. If a target is missing because the milestone hasn't built it yet, skip it and say so.

Then summarise:
- per service: lint / unit / integration / e2e status
- benchmarks vs NFR targets in `docs/PRD.md §10` (gate p99 < 20 µs, normalize→publish p99 < 200 µs, etc.)
- invariant checks: `grep` for forbidden patterns (raw `double` prices in engine/src outside analytics, `system_clock::now` outside WallClock, live broker URLs in tests, `LIVE_TRADING_ENABLED=true` in configs)

For each failure, give the file:line and the smallest fix. Don't tick anything in TASKS.md unless everything relevant passes.
