-- Vyuha initial schema (PostgreSQL 17 + pgvector)
-- Single source of truth for OLTP. Never edit after it has been applied anywhere; add a new migration.
-- Money/prices: numeric(18,4). Timestamps: timestamptz (UTC).

BEGIN;

CREATE EXTENSION IF NOT EXISTS pgcrypto;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_trgm;

-- ───────────────────────── Enums ─────────────────────────
CREATE TYPE user_role        AS ENUM ('admin','risk_manager','trader','analyst');
CREATE TYPE broker_kind      AS ENUM ('zerodha','upstox','sim');
CREATE TYPE account_status   AS ENUM ('disconnected','awaiting_login','connected','error','disabled');
CREATE TYPE exchange_code    AS ENUM ('NSE','BSE','NFO','BFO');
CREATE TYPE segment_code     AS ENUM ('EQ','FUT','OPT','IDX');
CREATE TYPE option_type      AS ENUM ('CE','PE');
CREATE TYPE order_side       AS ENUM ('BUY','SELL');
CREATE TYPE order_type       AS ENUM ('LIMIT','MARKET','SL','SL-M');
CREATE TYPE product_type     AS ENUM ('CNC','MIS','NRML');
CREATE TYPE validity_type    AS ENUM ('DAY','IOC');
CREATE TYPE order_source     AS ENUM ('manual','strategy','ai_draft','system');
CREATE TYPE order_status     AS ENUM ('NEW','RISK_REJECTED','APPROVED','SENT','OPEN','PARTIALLY_FILLED',
                                      'FILLED','CANCEL_PENDING','CANCELLED','MODIFY_PENDING',
                                      'BROKER_REJECTED','UNKNOWN');
CREATE TYPE limit_scope      AS ENUM ('desk','account','trader','strategy','instrument');
CREATE TYPE strategy_state   AS ENUM ('DRAFT','BACKTESTED','PAPER','PENDING_APPROVAL','LIVE','PAUSED','RETIRED');
CREATE TYPE job_status       AS ENUM ('queued','running','done','failed','cancelled');

-- ───────────────────────── Identity ─────────────────────────
CREATE TABLE app_user (
  id              uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  username        text NOT NULL UNIQUE CHECK (username ~ '^[a-z0-9_.]{3,32}$'),
  display_name    text NOT NULL CHECK (length(display_name) BETWEEN 1 AND 100),
  role            user_role NOT NULL,
  password_hash   text NOT NULL,                 -- argon2id PHC string
  totp_secret_enc bytea,                         -- AES-256-GCM(nonce||ct||tag); NULL until enrolled
  totp_enrolled   boolean NOT NULL DEFAULT false,
  is_active       boolean NOT NULL DEFAULT true,
  failed_logins   int NOT NULL DEFAULT 0,
  locked_until    timestamptz,
  created_at      timestamptz NOT NULL DEFAULT now(),
  updated_at      timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE refresh_token (
  id          uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  user_id     uuid NOT NULL REFERENCES app_user(id) ON DELETE CASCADE,
  token_hash  bytea NOT NULL UNIQUE,             -- sha256 of opaque token
  family_id   uuid NOT NULL,                     -- rotation family; reuse → revoke family
  expires_at  timestamptz NOT NULL,
  revoked_at  timestamptz,
  user_agent  text,
  ip          inet,
  created_at  timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX ON refresh_token(user_id);

CREATE TABLE personal_access_token (          -- external MCP access (read-only scopes)
  id          uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  user_id     uuid NOT NULL REFERENCES app_user(id) ON DELETE CASCADE,
  name        text NOT NULL,
  token_hash  bytea NOT NULL UNIQUE,
  scopes      text[] NOT NULL DEFAULT ARRAY['read'],
  expires_at  timestamptz NOT NULL,
  revoked_at  timestamptz,
  created_at  timestamptz NOT NULL DEFAULT now()
);

-- ───────────────────────── Broker accounts ─────────────────────────
CREATE TABLE broker_account (
  id                 uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  broker             broker_kind NOT NULL,
  client_code        text NOT NULL,
  holder_name        text NOT NULL,
  api_key_enc        bytea,                      -- NULL only for sim
  api_secret_enc     bytea,
  access_token_enc   bytea,                      -- daily; cleared at expiry
  token_expires_at   timestamptz,
  static_ip          inet,                       -- host-local address to bind (required unless sim)
  public_static_ip   inet,                       -- registered with broker (display + egress check)
  backup_static_ip   inet,
  data_entitled      boolean NOT NULL DEFAULT false,
  status             account_status NOT NULL DEFAULT 'disconnected',
  ops_cap            int NOT NULL DEFAULT 8 CHECK (ops_cap BETWEEN 1 AND 10),
  created_at         timestamptz NOT NULL DEFAULT now(),
  updated_at         timestamptz NOT NULL DEFAULT now(),
  UNIQUE (broker, client_code),
  CHECK (broker = 'sim' OR (static_ip IS NOT NULL AND api_key_enc IS NOT NULL AND api_secret_enc IS NOT NULL))
);
-- Unrelated accounts must not share an egress IP (NSE: shareable only among family members).
-- If a family exception is approved, record it in account_ip_exception and the check is done in app logic.
CREATE UNIQUE INDEX broker_account_public_ip_uniq ON broker_account(public_static_ip)
  WHERE public_static_ip IS NOT NULL;

CREATE TABLE account_user_map (
  account_id  uuid NOT NULL REFERENCES broker_account(id) ON DELETE CASCADE,
  user_id     uuid NOT NULL REFERENCES app_user(id) ON DELETE CASCADE,
  can_trade   boolean NOT NULL DEFAULT false,
  can_view    boolean NOT NULL DEFAULT true,
  is_default  boolean NOT NULL DEFAULT false,
  PRIMARY KEY (account_id, user_id)
);
CREATE UNIQUE INDEX account_user_default_uniq ON account_user_map(user_id) WHERE is_default;

-- ───────────────────────── Instruments ─────────────────────────
CREATE TABLE instrument (
  id               bigserial PRIMARY KEY,
  exchange         exchange_code NOT NULL,
  segment          segment_code NOT NULL,
  symbol           text NOT NULL,                -- underlying symbol, e.g. NIFTY, RELIANCE
  tradingsymbol    text NOT NULL,                -- exchange trading symbol
  name             text,
  isin             text,
  underlying_id    bigint REFERENCES instrument(id),
  expiry           date,
  strike           numeric(18,4),
  option_type      option_type,
  lot_size         int NOT NULL DEFAULT 1 CHECK (lot_size > 0),
  tick_size        numeric(18,4) NOT NULL CHECK (tick_size > 0),
  freeze_qty       int,
  fo_ban           boolean NOT NULL DEFAULT false,
  kite_token       bigint,
  upstox_key       text,
  partial_mapping  boolean NOT NULL DEFAULT false,
  sector           text,
  industry         text,
  valid_from       date NOT NULL,
  valid_to         date,
  UNIQUE (exchange, tradingsymbol, valid_from),
  CHECK ((segment = 'OPT') = (option_type IS NOT NULL AND strike IS NOT NULL)),
  CHECK (segment NOT IN ('FUT','OPT') OR expiry IS NOT NULL)
);
CREATE INDEX instrument_trgm ON instrument USING gin (tradingsymbol gin_trgm_ops);
CREATE INDEX instrument_chain ON instrument(underlying_id, expiry, strike, option_type) WHERE segment = 'OPT';
CREATE UNIQUE INDEX instrument_kite_uniq ON instrument(kite_token) WHERE kite_token IS NOT NULL AND valid_to IS NULL;
CREATE UNIQUE INDEX instrument_upstox_uniq ON instrument(upstox_key) WHERE upstox_key IS NOT NULL AND valid_to IS NULL;

CREATE TABLE restricted_instrument (          -- desk restricted list
  instrument_id bigint PRIMARY KEY REFERENCES instrument(id),
  reason        text NOT NULL,
  added_by      uuid NOT NULL REFERENCES app_user(id),
  added_at      timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE corporate_action (
  id             uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  instrument_id  bigint NOT NULL REFERENCES instrument(id),
  ex_date        date NOT NULL,
  kind           text NOT NULL CHECK (kind IN ('split','bonus','dividend','rights','merger','other')),
  ratio_num      numeric(18,6),
  ratio_den      numeric(18,6),
  amount         numeric(18,4),
  price_factor   numeric(18,10) NOT NULL,          -- multiply prices before ex_date
  volume_factor  numeric(18,10) NOT NULL,
  source         text NOT NULL,
  UNIQUE (instrument_id, ex_date, kind)
);

CREATE TABLE exchange_holiday (
  exchange   exchange_code NOT NULL,
  day        date NOT NULL,
  description text,
  special_session jsonb,                            -- e.g. Muhurat trading windows
  PRIMARY KEY (exchange, day)
);

-- ───────────────────────── Strategies ─────────────────────────
CREATE TABLE strategy (
  id                  uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  name                text NOT NULL UNIQUE CHECK (name ~ '^[a-z0-9_]{3,48}$'),
  owner_id            uuid NOT NULL REFERENCES app_user(id),
  state               strategy_state NOT NULL DEFAULT 'DRAFT',
  current_version_id  uuid,
  live_account_id     uuid REFERENCES broker_account(id),
  approved_by         uuid REFERENCES app_user(id),
  approved_at         timestamptz,
  created_at          timestamptz NOT NULL DEFAULT now(),
  updated_at          timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE strategy_version (
  id             uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  strategy_id    uuid NOT NULL REFERENCES strategy(id) ON DELETE CASCADE,
  version        int NOT NULL,
  code           text NOT NULL,
  code_sha256    text NOT NULL,
  params_schema  jsonb NOT NULL DEFAULT '{}'::jsonb,
  default_params jsonb NOT NULL DEFAULT '{}'::jsonb,
  created_by     uuid NOT NULL REFERENCES app_user(id),
  created_at     timestamptz NOT NULL DEFAULT now(),
  UNIQUE (strategy_id, version)
);
ALTER TABLE strategy ADD CONSTRAINT strategy_current_version_fk
  FOREIGN KEY (current_version_id) REFERENCES strategy_version(id) DEFERRABLE INITIALLY DEFERRED;

CREATE TABLE strategy_paper_session (
  id           uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  strategy_version_id uuid NOT NULL REFERENCES strategy_version(id),
  trading_day  date NOT NULL,
  orders       int NOT NULL DEFAULT 0,
  pnl          numeric(18,4) NOT NULL DEFAULT 0,
  completed    boolean NOT NULL DEFAULT false,
  UNIQUE (strategy_version_id, trading_day)
);

-- ───────────────────────── Orders (event-sourced) ─────────────────────────
CREATE TABLE "order" (
  id               uuid PRIMARY KEY,                -- generated by engine (UUIDv7)
  parent_id        uuid REFERENCES "order"(id),
  client_order_id  text NOT NULL UNIQUE,            -- idempotency key from client
  account_id       uuid NOT NULL REFERENCES broker_account(id),
  user_id          uuid NOT NULL REFERENCES app_user(id),
  strategy_id      uuid REFERENCES strategy(id),
  source           order_source NOT NULL,
  instrument_id    bigint NOT NULL REFERENCES instrument(id),
  side             order_side NOT NULL,
  qty              int NOT NULL CHECK (qty > 0),
  filled_qty       int NOT NULL DEFAULT 0 CHECK (filled_qty >= 0),
  avg_fill_price   numeric(18,4),
  order_type       order_type NOT NULL,
  product          product_type NOT NULL,
  validity         validity_type NOT NULL DEFAULT 'DAY',
  price            numeric(18,4),
  trigger_price    numeric(18,4),
  market_protection_pct numeric(6,3),
  bracket          jsonb,                           -- {stop_trigger, stop_price, target_price}
  status           order_status NOT NULL,
  broker_order_id  text,
  tag              text NOT NULL,
  is_paper         boolean NOT NULL,
  reject_code      text,
  reject_message   text,
  created_at       timestamptz NOT NULL,
  updated_at       timestamptz NOT NULL,
  CHECK (filled_qty <= qty),
  CHECK (order_type NOT IN ('LIMIT','SL') OR price IS NOT NULL),
  CHECK (order_type NOT IN ('SL','SL-M') OR trigger_price IS NOT NULL)
);
CREATE INDEX order_account_day ON "order"(account_id, created_at DESC);
CREATE INDEX order_open ON "order"(status) WHERE status IN ('SENT','OPEN','PARTIALLY_FILLED','CANCEL_PENDING','MODIFY_PENDING','UNKNOWN');
CREATE UNIQUE INDEX order_broker_id_uniq ON "order"(account_id, broker_order_id) WHERE broker_order_id IS NOT NULL;

CREATE TABLE order_event (
  id           bigserial PRIMARY KEY,
  order_id     uuid NOT NULL REFERENCES "order"(id),
  seq          int NOT NULL,
  from_status  order_status,
  to_status    order_status NOT NULL,
  reason_code  text,
  payload      jsonb NOT NULL DEFAULT '{}'::jsonb,   -- includes gate stamp (ruleset version, eval_ns)
  ts           timestamptz NOT NULL,
  UNIQUE (order_id, seq)
);

CREATE TABLE fill (
  id               uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  order_id         uuid NOT NULL REFERENCES "order"(id),
  account_id       uuid NOT NULL REFERENCES broker_account(id),
  instrument_id    bigint NOT NULL REFERENCES instrument(id),
  side             order_side NOT NULL,
  broker_trade_id  text NOT NULL,
  qty              int NOT NULL CHECK (qty > 0),
  price            numeric(18,4) NOT NULL,
  costs            jsonb NOT NULL DEFAULT '{}'::jsonb,   -- computed cost breakdown
  is_paper         boolean NOT NULL,
  ts               timestamptz NOT NULL,
  UNIQUE (account_id, broker_trade_id)
);
CREATE INDEX fill_account_ts ON fill(account_id, ts DESC);

CREATE TABLE position_eod (                      -- EOD snapshot after broker reconcile
  trading_day     date NOT NULL,
  account_id      uuid NOT NULL REFERENCES broker_account(id),
  instrument_id   bigint NOT NULL REFERENCES instrument(id),
  product         product_type NOT NULL,
  net_qty         int NOT NULL,
  avg_price       numeric(18,4) NOT NULL,
  close_price     numeric(18,4) NOT NULL,
  realized_pnl    numeric(18,4) NOT NULL,
  unrealized_pnl  numeric(18,4) NOT NULL,
  costs           numeric(18,4) NOT NULL,
  broker_net_qty  int NOT NULL,
  reconciled      boolean NOT NULL,
  PRIMARY KEY (trading_day, account_id, instrument_id, product)
);

-- ───────────────────────── Risk ─────────────────────────
CREATE TABLE risk_limit (
  id          uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  scope       limit_scope NOT NULL,
  scope_id    text,                               -- uuid or instrument id as text; NULL for desk
  key         text NOT NULL,                      -- see RISK_ENGINE.md §2 catalogue
  value       numeric(20,4) NOT NULL,
  hard        boolean NOT NULL DEFAULT true,       -- soft = warn only
  updated_by  uuid NOT NULL REFERENCES app_user(id),
  updated_at  timestamptz NOT NULL DEFAULT now(),
  UNIQUE (scope, scope_id, key)
);

CREATE TABLE kill_switch (
  id          uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  scope       limit_scope NOT NULL,
  scope_id    text,
  active      boolean NOT NULL,
  actions     text[] NOT NULL,                    -- {'block','cancel','flatten'}
  trigger     text NOT NULL,                      -- manual | auto:<rule>
  reason      text NOT NULL,
  set_by      uuid REFERENCES app_user(id),
  set_at      timestamptz NOT NULL DEFAULT now(),
  released_by uuid REFERENCES app_user(id),
  released_at timestamptz
);
CREATE UNIQUE INDEX kill_switch_active_uniq ON kill_switch(scope, coalesce(scope_id,'')) WHERE active;

CREATE TABLE discipline_state (                 -- loss-streak lockouts etc.
  user_id         uuid PRIMARY KEY REFERENCES app_user(id),
  loss_streak     int NOT NULL DEFAULT 0,          -- counts trading days only
  win_streak      int NOT NULL DEFAULT 0,
  locked_until    date,
  last_trading_day date,
  updated_at      timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE cost_schedule (                    -- effective-dated, verified rates
  id              uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  broker          broker_kind NOT NULL,
  exchange        exchange_code NOT NULL,
  segment         segment_code NOT NULL,
  product         product_type,                    -- NULL = any
  component       text NOT NULL CHECK (component IN ('brokerage','stt','exchange_txn','sebi_fee','stamp_duty','gst','ipft','dp_charge')),
  basis           text NOT NULL CHECK (basis IN ('pct_turnover','pct_premium','flat_per_order','flat_per_scrip_day','pct_of_components','per_crore')),
  side            text NOT NULL CHECK (side IN ('buy','sell','both')),
  rate            numeric(18,10) NOT NULL,
  cap             numeric(18,4),
  effective_from  date NOT NULL,
  effective_to    date,
  source_url      text NOT NULL,
  verified_by     uuid REFERENCES app_user(id),
  verified_at     timestamptz
);

-- ───────────────────────── Research ─────────────────────────
CREATE TABLE dataset (
  id           uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  kind         text NOT NULL CHECK (kind IN ('bars','ticks','option_chain','fundamentals')),
  interval     text,                               -- '1minute','day',...
  source       broker_kind NOT NULL,
  instrument_id bigint REFERENCES instrument(id),
  from_ts      timestamptz NOT NULL,
  to_ts        timestamptz NOT NULL,
  path         text NOT NULL,                      -- relative to DATA_LAKE_DIR
  row_count    bigint NOT NULL,
  sha256       text NOT NULL,
  created_at   timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX dataset_lookup ON dataset(instrument_id, interval, source);

CREATE TABLE download_job (
  id           uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  requested_by uuid NOT NULL REFERENCES app_user(id),
  account_id   uuid NOT NULL REFERENCES broker_account(id),
  spec         jsonb NOT NULL,                     -- {instruments, interval, from, to, include_expired}
  status       job_status NOT NULL DEFAULT 'queued',
  progress     jsonb NOT NULL DEFAULT '{}'::jsonb, -- {done, total, last_cursor}
  error        text,
  created_at   timestamptz NOT NULL DEFAULT now(),
  updated_at   timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE backtest_run (
  id                  uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  strategy_version_id uuid NOT NULL REFERENCES strategy_version(id),
  params              jsonb NOT NULL,
  config              jsonb NOT NULL,               -- fill model, slippage, costs, capital, calendar
  dataset_ids         uuid[] NOT NULL,
  seed                int NOT NULL,
  sweep_id            uuid,                         -- groups parameter-sweep runs
  split               text,                         -- 'in_sample'|'out_of_sample'|NULL
  status              job_status NOT NULL DEFAULT 'queued',
  metrics             jsonb,
  artifacts_path      text,                         -- equity curve, trades parquet
  error               text,
  created_by          uuid NOT NULL REFERENCES app_user(id),
  created_at          timestamptz NOT NULL DEFAULT now(),
  finished_at         timestamptz
);

-- ───────────────────────── UI ─────────────────────────
CREATE TABLE workspace_layout (
  user_id    uuid NOT NULL REFERENCES app_user(id) ON DELETE CASCADE,
  name       text NOT NULL,
  layout     jsonb NOT NULL,
  is_default boolean NOT NULL DEFAULT false,
  updated_at timestamptz NOT NULL DEFAULT now(),
  PRIMARY KEY (user_id, name)
);

CREATE TABLE watchlist (
  id        uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  user_id   uuid NOT NULL REFERENCES app_user(id) ON DELETE CASCADE,
  name      text NOT NULL,
  instrument_ids bigint[] NOT NULL DEFAULT '{}',
  UNIQUE (user_id, name)
);

CREATE TABLE alert_rule (
  id         uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  user_id    uuid NOT NULL REFERENCES app_user(id) ON DELETE CASCADE,
  kind       text NOT NULL CHECK (kind IN ('price','pct_change','oi_change','iv','pnl','limit_util','context')),
  spec       jsonb NOT NULL,
  active     boolean NOT NULL DEFAULT true,
  created_at timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE alert_event (
  id         bigserial PRIMARY KEY,
  rule_id    uuid REFERENCES alert_rule(id) ON DELETE SET NULL,
  user_id    uuid NOT NULL REFERENCES app_user(id),
  severity   text NOT NULL CHECK (severity IN ('info','warn','critical')),
  title      text NOT NULL,
  body       jsonb NOT NULL,
  acked_at   timestamptz,
  ts         timestamptz NOT NULL DEFAULT now()
);

-- ───────────────────────── Context / AI ─────────────────────────
CREATE TABLE feed_source (
  id         uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  name       text NOT NULL UNIQUE,
  kind       text NOT NULL CHECK (kind IN ('rss','regulator_rss','exchange','manual')),
  url        text,
  enabled    boolean NOT NULL DEFAULT true,
  poll_secs  int NOT NULL DEFAULT 60 CHECK (poll_secs >= 30),
  tos_reviewed boolean NOT NULL DEFAULT false,
  created_at timestamptz NOT NULL DEFAULT now(),
  CHECK (kind <> 'exchange' OR tos_reviewed OR NOT enabled)
);

CREATE TABLE story (
  id            uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  source_id     uuid NOT NULL REFERENCES feed_source(id),
  guid_hash     bytea NOT NULL UNIQUE,
  cluster_id    uuid,                              -- duplicate cluster representative
  url           text,
  title         text NOT NULL,
  body          text,
  lang          text,
  published_at  timestamptz NOT NULL,
  ingested_at   timestamptz NOT NULL DEFAULT now(),
  embedding     vector(1024),
  extraction    jsonb,                             -- validated extraction JSON
  extraction_status text NOT NULL DEFAULT 'pending' CHECK (extraction_status IN ('pending','ok','quarantined'))
);
CREATE INDEX story_published ON story(published_at DESC);
CREATE INDEX story_embedding ON story USING hnsw (embedding vector_cosine_ops);

CREATE TABLE context_alert (
  id         uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  story_id   uuid NOT NULL REFERENCES story(id),
  user_id    uuid NOT NULL REFERENCES app_user(id),
  score      numeric(12,4) NOT NULL,
  path       jsonb NOT NULL,                       -- [{node,label,edge,weight}...]
  summary    text NOT NULL,
  acked_at   timestamptz,
  created_at timestamptz NOT NULL DEFAULT now(),
  UNIQUE (story_id, user_id)
);

CREATE TABLE copilot_session (
  id         uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  user_id    uuid NOT NULL REFERENCES app_user(id),
  title      text,
  context    jsonb NOT NULL DEFAULT '{}'::jsonb,   -- focused security, link group
  created_at timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE copilot_message (
  id          bigserial PRIMARY KEY,
  session_id  uuid NOT NULL REFERENCES copilot_session(id) ON DELETE CASCADE,
  role        text NOT NULL CHECK (role IN ('user','assistant','tool')),
  content     jsonb NOT NULL,
  tool_name   text,
  model       text,
  input_tokens  int,
  output_tokens int,
  ts          timestamptz NOT NULL DEFAULT now()
);

-- ───────────────────────── Audit (append-only, hash-chained) ─────────────────────────
CREATE TABLE audit_event (
  id         bigserial PRIMARY KEY,
  ts         timestamptz NOT NULL DEFAULT now(),
  actor_id   uuid,
  actor_kind text NOT NULL CHECK (actor_kind IN ('user','system','engine','strategy','copilot')),
  action     text NOT NULL,                        -- e.g. auth.login, limit.update, order.submit
  entity     text NOT NULL,
  entity_id  text,
  payload    jsonb NOT NULL,
  ip         inet,
  prev_hash  bytea NOT NULL,
  hash       bytea NOT NULL UNIQUE
);
CREATE INDEX audit_ts ON audit_event(ts);

-- Hash chain computed in DB so every writer is consistent. Serialised via advisory lock.
CREATE OR REPLACE FUNCTION audit_chain() RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE last_hash bytea;
BEGIN
  PERFORM pg_advisory_xact_lock(727274);           -- 'audit'
  SELECT hash INTO last_hash FROM audit_event ORDER BY id DESC LIMIT 1;
  NEW.prev_hash := coalesce(last_hash, '\x00'::bytea);
  NEW.hash := digest(NEW.prev_hash || convert_to(
      to_char(NEW.ts AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.US"Z"') || '|' ||  -- session-TZ independent coalesce(NEW.actor_id::text,'') || '|' || NEW.actor_kind || '|' ||
      NEW.action || '|' || NEW.entity || '|' || coalesce(NEW.entity_id,'') || '|' || NEW.payload::text,
      'UTF8'), 'sha256');
  RETURN NEW;
END $$;
CREATE TRIGGER audit_chain_trg BEFORE INSERT ON audit_event FOR EACH ROW EXECUTE FUNCTION audit_chain();

CREATE OR REPLACE FUNCTION audit_immutable() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN RAISE EXCEPTION 'audit_event is append-only'; END $$;
CREATE TRIGGER audit_no_update BEFORE UPDATE OR DELETE ON audit_event FOR EACH ROW EXECUTE FUNCTION audit_immutable();

-- ───────────────────────── Roles & grants ─────────────────────────
-- Service roles are created by deploy scripts with passwords; grants here.
DO $$ BEGIN
  IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'vyuha_app') THEN CREATE ROLE vyuha_app NOLOGIN; END IF;
  IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'vyuha_readonly') THEN CREATE ROLE vyuha_readonly NOLOGIN; END IF;
END $$;
GRANT SELECT, INSERT, UPDATE, DELETE ON ALL TABLES IN SCHEMA public TO vyuha_app;
REVOKE UPDATE, DELETE, TRUNCATE ON audit_event FROM vyuha_app;
GRANT USAGE ON ALL SEQUENCES IN SCHEMA public TO vyuha_app;
GRANT SELECT ON ALL TABLES IN SCHEMA public TO vyuha_readonly;          -- copilot/MCP read role
REVOKE SELECT ON app_user, refresh_token, personal_access_token, broker_account FROM vyuha_readonly;

COMMIT;
