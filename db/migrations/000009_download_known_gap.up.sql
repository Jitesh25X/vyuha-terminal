-- A day an instrument really has no bars (suspended, or no trades), recorded by a person, so its download chunk
-- can complete while a real gap anywhere else still fails. Owner, 2026-11-03: nothing clears automatically.
BEGIN;
CREATE TABLE download_known_gap (
  instrument_id bigint NOT NULL REFERENCES instrument(id),
  day           date NOT NULL,
  reason        text NOT NULL CHECK (length(btrim(reason)) > 0),
  recorded_by   uuid NOT NULL REFERENCES app_user(id),
  recorded_at   timestamptz NOT NULL DEFAULT now(),
  PRIMARY KEY (instrument_id, day)
);
GRANT SELECT, INSERT ON download_known_gap TO vyuha_app;
GRANT SELECT ON download_known_gap TO vyuha_readonly;
COMMIT;
