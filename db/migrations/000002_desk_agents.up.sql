-- Desk agents (runtime AI staff). See docs/DESK_AGENTS.md.
BEGIN;

CREATE TABLE agent (
  id             text PRIMARY KEY CHECK (id ~ '^[a-z_]{3,40}$'),
  title          text NOT NULL,
  definition     jsonb NOT NULL,                  -- parsed YAML (tools, triggers, budget, audience)
  prompt_sha256  text NOT NULL,
  enabled        boolean NOT NULL DEFAULT true,
  disabled_reason text,
  updated_at     timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE agent_run (
  id              uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  agent_id        text NOT NULL REFERENCES agent(id),
  parent_run_id   uuid REFERENCES agent_run(id),
  trigger_kind    text NOT NULL CHECK (trigger_kind IN ('event','schedule','user','agent')),
  trigger_key     text NOT NULL,                  -- dedupe key
  trigger_payload jsonb NOT NULL,
  requested_by    uuid REFERENCES app_user(id),   -- for user-triggered runs
  status          text NOT NULL CHECK (status IN ('running','done','no_note','failed','skipped_budget','skipped_cooldown')),
  tool_calls      jsonb NOT NULL DEFAULT '[]'::jsonb,
  input_tokens    int NOT NULL DEFAULT 0,
  output_tokens   int NOT NULL DEFAULT 0,
  cost_usd_est    numeric(10,4) NOT NULL DEFAULT 0,
  error           text,
  started_at      timestamptz NOT NULL DEFAULT now(),
  finished_at     timestamptz
);
CREATE INDEX agent_run_agent_day ON agent_run(agent_id, started_at DESC);
CREATE INDEX agent_run_trigger ON agent_run(agent_id, trigger_key, started_at DESC);

CREATE TABLE agent_note (
  id             uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  run_id         uuid NOT NULL REFERENCES agent_run(id),
  agent_id       text NOT NULL REFERENCES agent(id),
  kind           text NOT NULL CHECK (kind IN ('note','alert','brief','strategy_draft','ticket_draft')),
  severity       text NOT NULL DEFAULT 'info' CHECK (severity IN ('info','warn','critical')),
  audience_scope text NOT NULL,                   -- 'desk' | 'trader:<uuid>' | 'account:<uuid>'
  instrument_ids bigint[] NOT NULL DEFAULT '{}',
  title          text NOT NULL,
  body_md        text NOT NULL,
  refs           jsonb NOT NULL DEFAULT '[]'::jsonb, -- tool-result refs / story ids backing each claim
  embedding      vector(1024),
  feedback       smallint,                        -- -1 / +1
  created_at     timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX agent_note_created ON agent_note(created_at DESC);
CREATE INDEX agent_note_instr ON agent_note USING gin (instrument_ids);
CREATE INDEX agent_note_embedding ON agent_note USING hnsw (embedding vector_cosine_ops);

GRANT SELECT, INSERT, UPDATE ON agent, agent_run, agent_note TO vyuha_app;
GRANT SELECT ON agent, agent_run, agent_note TO vyuha_readonly;

COMMIT;
