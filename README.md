# Vyuha — open-source AI quant desk for Indian markets

A self-hosted, keyboard-first trading terminal for small Indian prop desks (2–10 people):
live NSE/BSE cash + F&O data, execution through **Zerodha Kite Connect** and **Upstox**, a
non-bypassable pre-trade risk gate, Aladdin-style portfolio risk + knowledge-graph context mapping,
research/backtesting with Indian costs, and an AI copilot that explains — but never places — trades.

> Status: **specification stage**. Build follows `docs/ROADMAP.md` using Claude Code.
> Not investment advice. You are responsible for complying with SEBI/exchange rules and your broker's terms — see `docs/COMPLIANCE.md`.

## Start here
| File | What it is |
|---|---|
| `CLAUDE.md` | Rules for the Claude Code build agent (read first) |
| `docs/PRD.md` | Product requirements, features, acceptance criteria |
| `docs/ROADMAP.md` | Milestones M0–M9 overview |
| `docs/TASKS.md` | **Execution queue**: every task in order, with deps, download allow-list and done check |
| `docs/DESK_AGENTS.md` | Runtime AI desk staff (Desk Head, Risk Analyst, Macro Analyst, …) |
| `docs/ARCHITECTURE.md` | Services, data flows, threading, static-IP binding |
| `docs/TECH_STACK.md` | Exact stack + rationale (Bloomberg/Aladdin mapping) |
| `docs/DATA_MODEL.md` | Postgres / ClickHouse / Neo4j / Parquet models |
| `docs/API.md` | REST + WebSocket + gRPC + MCP contracts |
| `docs/RISK_ENGINE.md` | Limits, gate rules, kill switch, Greeks/VaR/scenarios/factors |
| `docs/CONTEXT_ENGINE_AND_AI.md` | News → graph → position alerts; copilot tools & evals |
| `docs/STRATEGY_SDK_AND_BACKTEST.md` | Data downloads, Strategy SDK, backtester, cost model |
| `docs/UI_SPEC.md` | Terminal functions, keyboard map, design tokens |
| `docs/COMPLIANCE.md` | Regulatory notes → engineering controls; open legal questions |
| `docs/DECISIONS.md` | Decision log + open owner decisions |

## Using with Claude Code
```bash
cd vyuha
claude                      # CLAUDE.md, .claude/agents, .claude/commands load automatically
/next-task                  # does exactly ONE task: the next unchecked one in docs/TASKS.md, then stops
/verify                     # full checks
/milestone-exit             # checks exit criteria, cleans disk, advances the milestone
```
Subagents: `cpp-engine`, `quant-research`, `terminal-ui`, `risk-reviewer`, `compliance-auditor`.

Downloads are just-in-time: each task may install only what its `needs:` line lists (PRD §10.1).

## Stack at a glance
C++23 engine · Go gateway · Python 3.13 quant & AI · React 19 terminal · NATS JetStream · PostgreSQL 17 + pgvector · ClickHouse · Neo4j · Parquet lake · Anthropic Claude via MCP tools · Docker Compose on one Mumbai VM.

## License
To be decided (`docs/DECISIONS.md` D-3; recommended AGPL-3.0).
