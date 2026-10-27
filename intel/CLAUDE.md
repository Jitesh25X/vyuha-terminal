# intel/ — Python (context engine, copilot, MCP server)

Read `docs/CONTEXT_ENGINE_AND_AI.md` and the root `CLAUDE.md` first.

## Layout
```
src/vyuha_intel/
  ingest/       EventSource implementations, scheduler, dedupe/cluster
  extract/      LLM extraction, schema.json, prompts/
  resolve/      alias tables, trigram + embedding resolution
  graph/        Neo4j client, Cypher templates (parameterised only), seed loaders
  context/      HOLDS sync, impact query, alert scoring
  copilot/      orchestrator (tool loop), summarisers, budgets, SSE
  mcp_server/   FastMCP server, tool registry, RBAC
  briefs/       pre-open / post-close briefs
  llm/          LLMClient protocol, AnthropicClient, OpenAICompatClient
  agents/       desk agent runner, loader, triggers, budgets (see docs/DESK_AGENTS.md)
agents/         desk agent YAML definitions + prompts/
evals/          golden sets (YAML) + runners
tests/
```

## Rules
- Model IDs come from env (`LLM_MODEL_FAST`, `LLM_MODEL_SMART`). Never hard-code them.
- Untrusted content always goes inside `<document …>` blocks with the "untrusted data" system instruction. Never concatenate it into instructions.
- Cypher/SQL come only from templates in `graph/queries/*.cypher` and `copilot/sql/*.sql` with parameters. The LLM never writes queries.
- The tool registry is the single list in `mcp_server/registry.py`. `tests/test_registry.py` asserts the exact set. No tool may mutate trading state.
- Use the `vyuha_readonly` Postgres role, the Neo4j read-only user and the ClickHouse read-only profile for tools.
- Unit tests mock the LLM (recorded responses). Real-LLM evals run only in the nightly eval job with a budget cap.
- Never send broker client codes, holder names or user emails to the LLM.
- Desk agents: definitions are data (YAML). Every tool in a definition must be in the registry and non-mutating; the loader rejects anything else. Only `desk_head` may call `ask_agent`, depth 1.
- Install `sentence-transformers` with the CPU-only torch wheel (`--index-url https://download.pytorch.org/whl/cpu`). Download bge-m3 once into `$MODELS_DIR`; never download other models.
