-- F&O ban list (F-MD-1, ADR-019): imported manually from NSE's published daily file. One row per
-- trade date, so "imported with no securities in ban" is distinct from "not imported yet".
BEGIN;

CREATE TABLE fo_ban_import (
  trade_date  date PRIMARY KEY,
  symbols     text[] NOT NULL,                 -- underlying symbols in ban for trade_date (may be empty)
  source_sha256 text NOT NULL,                 -- of the imported file, for audit
  imported_at timestamptz NOT NULL DEFAULT now()
);

GRANT SELECT, INSERT, UPDATE ON fo_ban_import TO vyuha_app;

COMMIT;
