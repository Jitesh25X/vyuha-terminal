-- F-RK-1: desk-level default limits. The risk gate refuses an order when a limit key has no row, so a fresh
-- install with no rows would block every order. These are SEED VALUES FOR A STAGING DESK (docs/RISK_ENGINE.md §2):
-- the owner sets real values in the admin console before going live.

-- NULL updated_by means the row was seeded by a migration and never edited by a user (a fresh install has
-- no users when migrations run).
ALTER TABLE risk_limit ALTER COLUMN updated_by DROP NOT NULL;
-- Only a desk row may be unattributed: every other scope still needs a user.
ALTER TABLE risk_limit ADD CONSTRAINT risk_limit_updated_by_seed_only
  CHECK (updated_by IS NOT NULL OR (scope = 'desk' AND scope_id IS NULL));

-- UNIQUE (scope, scope_id, key) does not catch a NULL scope_id, so ON CONFLICT would not protect an existing
-- desk row: guard each insert with NOT EXISTS. An existing value is never overwritten or duplicated.
INSERT INTO risk_limit (scope, scope_id, key, value, hard, updated_by)
SELECT 'desk', NULL, d.key, d.value, true, NULL
FROM (VALUES
  ('max_order_value',          500000::numeric),
  ('max_order_qty_lots',       20),
  ('max_position_lots',        50),
  ('max_position_notional',    2500000),
  ('max_gross_exposure',       10000000),
  ('max_net_delta_inr',        5000000),
  ('max_net_vega_inr',         50000),
  ('max_short_option_lots',    20),
  ('naked_short_options',      0),
  ('max_daily_loss',           50000),
  ('max_drawdown_from_peak',   75000),
  ('max_open_orders',          50),
  ('ops_cap',                  8),      -- stays below the 10 OPS threshold (project invariant 7)
  ('fat_finger_pct',           5),
  ('fat_finger_pct_opt',       30),
  ('dup_window_ms',            1500),
  ('margin_buffer_pct',        10),
  ('min_reward_risk',          0),
  ('loss_streak_lockout_days', 0),
  ('lockout_length_days',      1),
  ('allow_market_orders',      1),
  ('allow_mis_after',          1510),   -- HHMM IST
  ('trading_window_start',     915),    -- HHMM IST
  ('trading_window_end',       1530)    -- HHMM IST
) AS d(key, value)
WHERE NOT EXISTS (
  SELECT 1 FROM risk_limit r WHERE r.scope = 'desk' AND r.scope_id IS NULL AND r.key = d.key
);
