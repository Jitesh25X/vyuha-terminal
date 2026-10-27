# Desk Agents ("AI desk staff")

Runtime AI agents that mirror the roles of a trading firm. They live in the `intel` service and use the
same read-only MCP tools as the copilot (`CONTEXT_ENGINE_AND_AI.md §4`).

**Prime rule:** agents analyse, draft, monitor and alert. They never place, modify or cancel orders, change limits, release a kill switch, or promote a strategy to LIVE. Those actions stay with humans and the risk gate. This keeps every algo white-box and human-approved (`COMPLIANCE.md`).

> These are **runtime** agents inside Vyuha. They are different from the **build-time** Claude Code subagents in `.claude/agents/`, which write the software.

## 1. Roster
| Agent ID | Firm role | MVP? | Triggers | Allowed tools (subset of MCP registry) | May do autonomously | Human needed for |
|---|---|---|---|---|---|---|
| `desk_head` | Desk head / PM, the orchestrator | ✅ M8 | Schedule 08:45 & 16:00 IST; user question routed to "Desk" | all read tools; can **invoke other agents** | Briefs, focus list, route questions, summarise other agents' notes | — |
| `risk_analyst` | Risk analyst | ✅ M8 | Fill; limit utilisation crosses 70/85/95%; VaR change > 20%; context alert with score ≥ threshold; every 15 min intraday | `get_positions, get_risk_summary, run_scenario, get_var, get_factor_exposure, get_limits, graph_paths` | Risk notes, pre-breach warnings, **request** kill switch (creates a high-priority alert for risk_manager) | Setting/releasing kill, changing limits |
| `macro_analyst` | Macro/news analyst | ✅ M8 | New Event of type monetary/fiscal/inflation/fx/commodity/global/geopolitical; schedule 08:30 IST | `search_news, graph_paths, get_events_calendar, get_quote, get_candles, run_scenario` | Event notes with impact paths and suggested scenarios | — |
| `research_analyst` | Equity research analyst | M8 stretch | Results/corporate events for held or watchlist names | `search_news, get_events_calendar, get_candles, get_quote, graph_paths` | Company notes (what happened, numbers vs expectations if available, risks) | — |
| `quant_researcher` | Quant researcher | M8 stretch | User request; weekly schedule | `get_candles, run_backtest, get_backtest_report` + **draft strategy** (writes a `DRAFT` strategy version only) | Draft strategies, run backtests/paper, overfitting report | PAPER→LIVE promotion (risk_manager) |
| `execution_assistant` | Execution trader | Post-MVP | User request; large order preview | `get_quote, get_option_chain, get_limits, draft_order_ticket` | Fill in tickets (slicing/timing suggestions) | Submitting any order |
| `compliance_officer` | Compliance | Post-MVP | EOD 15:50 IST | `compliance report` read tool | Daily compliance note, flags | Any rule change |
| `ops_reconciler` | Middle office / ops | Post-MVP | Recon mismatch; EOD | positions/orders/fills read tools | Mismatch investigation notes | Any correction |
| `trading_coach` | Performance coach | Post-MVP | Weekly; after a losing streak | journal/P&L/discipline read tools | Personal review notes (visible only to that trader + risk_manager) | — |

## 2. Agent definition (data, not code)
Agents are defined in YAML under `intel/agents/*.yaml` and loaded at start (validated by a Pydantic schema):
```yaml
id: risk_analyst
title: Risk Analyst
role_prompt_file: prompts/risk_analyst.md     # versioned; hash stored on every run
model: ${LLM_MODEL_SMART}                     # or ${LLM_MODEL_FAST}
tools: [get_positions, get_risk_summary, run_scenario, get_var, get_factor_exposure, get_limits, graph_paths]
can_invoke_agents: []                          # only desk_head may invoke others
triggers:
  - { kind: event, subject: "risk.util", when: "pct >= 0.70", cooldown_s: 900 }
  - { kind: event, subject: "oms.fill.>", debounce_s: 30 }
  - { kind: event, subject: "ctx.alert.>", when: "score >= 25000" }
  - { kind: schedule, cron_ist: "*/15 9-15 * * 1-5" }
audience: { roles: [risk_manager, trader], scope: own }   # traders see notes about their own scope
budget: { max_runs_per_day: 60, max_tokens_per_day: 1500000, max_tool_calls_per_run: 12 }
outputs: [note, alert]                          # note | alert | brief | strategy_draft | ticket_draft
enabled: true
```

## 3. Runtime
- **Agent runner** (`intel/src/vyuha_intel/agents/`) subscribes to trigger subjects on NATS and runs scheduled jobs (APScheduler, IST calendar-aware, no runs on exchange holidays).
- A run = bounded tool loop (same orchestrator as the copilot) with: a role prompt, a trigger payload as data, scoped RBAC (the agent acts with a **service identity limited to its audience scope**), a max tool-call count, and a wall-clock timeout of 90 s.
- **Inter-agent calls:** only `desk_head` may call `ask_agent(agent_id, question)`. Depth max 1 (no chains). Each call is a child run linked by `parent_run_id`.
- **Dedupe & cooldown:** identical trigger keys within the cooldown are skipped. A run with nothing material to say emits `no_note` (stored, not shown).
- **Shared memory:** agent notes are stored in Postgres (`agent_note`) with embeddings. Agents can `search_agent_notes` to build on earlier work instead of repeating it.
- **Outputs:**
  - `note`: shows in the `DESK` panel activity feed and the relevant security's context.
  - `alert`: shows as a toast plus the alert inbox (`severity` set by the agent within its allowed range).
  - `brief`: the existing daily briefs are now produced by `desk_head`.
  - `strategy_draft`: a `DRAFT` strategy version, which the owner reviews.
  - `ticket_draft`: an order ticket the human must submit (post-MVP agent only).
- **Every run is audited:** trigger, prompt hash, tool calls, tokens, cost estimate and output ids.

## 4. Safety
- Agent tool lists are a subset of the MCP registry, and a unit test asserts no agent lists a mutating tool. The only write tools that exist are `write_agent_note`, `raise_alert`, `draft_strategy` (DRAFT only) and `draft_order_ticket` (human-submitted).
- Trigger payloads from news and filings are untrusted data (same injection rules and test suite).
- Kill-switch requests from `risk_analyst` create a `critical` alert with one-click "Set kill" for a risk manager. The agent cannot set it.
- Budgets: per-agent daily token and run caps, plus a desk monthly cap. Hitting a cap disables that agent until the next day and alerts the admin.
- Each agent can be disabled by an admin instantly (`enabled: false` via the admin API, hot-reloaded).

## 5. UI
- New function **`DESK`**: a roster with status (idle/running/disabled/budget-hit), today's runs, tokens and cost, and an activity feed of notes and alerts filterable by agent/security/trader.
- In the `AI` panel, `@risk_analyst …` asks a specific agent. A plain question goes to the copilot (or `@desk` routes to `desk_head`).
- Every note shows: the agent, trigger, time, a tool-trace expander, and 👍/👎 feedback (stored for evals).

## 6. Evals
- `intel/evals/agents/<agent_id>.yaml`: scenario fixtures (positions + events) → expected tool calls, required facts, forbidden content, and "should stay silent" cases (to avoid alert noise).
- Release gates: each MVP agent ≥ 85% pass; false-alert rate ≤ 10% on the silent cases; 100% pass on injection cases.
