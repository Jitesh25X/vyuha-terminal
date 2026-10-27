---
description: Scaffold a new broker adapter (post-MVP brokers) following the adapter checklist
argument-hint: "<broker-name, e.g. dhan>"
---

Scaffold a broker adapter for `$ARGUMENTS`. This is post-MVP: confirm with the owner that it's on the roadmap before writing code.

Checklist:
1. Research the broker's current official API docs: auth flow, order endpoints, market data WS (format, limits), historical data limits, static-IP registration, OPS limits, tag field length, market protection parameter. Write `engine/src/broker/$ARGUMENTS/README.md` with URLs and today's date.
2. Add the `broker_kind` enum value via a **new migration**, plus the proto `Broker` enum value (additive only; `buf breaking` must pass).
3. Implement `IBrokerAdapter` and `IMarketDataSource` in `engine/src/broker/$ARGUMENTS/`, with source-IP binding and preflight.
4. Add instrument-master mapping in `quant/src/vyuha_quant/data/` and historical limits in `data/limits.py`.
5. Tests: VCR contract tests from documented example payloads (no live calls), parser fuzz target, reconnect/backoff tests, OPS bucket test.
6. Gateway: login URL + callback route; admin form fields.
7. Run the `risk-reviewer` and `compliance-auditor` subagents.
