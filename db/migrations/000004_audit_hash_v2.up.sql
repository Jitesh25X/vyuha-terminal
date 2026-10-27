-- Audit hash chain v2 (M0.8). Fixes three defects in 000001's audit_chain():
--  1. actor_id and actor_kind were outside the hash (a `--` comment swallowed that line), so the actor
--     of an audited action could be rewritten undetected.
--  2. Fields were joined with '|', so text could move between fields (action "a|b", entity "c" vs
--     action "a", entity "b|c") without changing the hash.
--  3. id came from the column default, evaluated before the advisory lock, so concurrent inserts could
--     chain in a different order than their ids and fail verification.
-- Canonical form (also implemented in gateway/internal/audit/verify.go — change both together):
--   hash = sha256(prev_hash || utf8(F(ts) F(actor_id) F(actor_kind) F(action) F(entity) F(entity_id) F(payload) F(ip)))
--   F(NULL) = '-'   F(v) = octet_length(v) || ':' || v
--   ts = to_char(ts AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.US"Z"'), payload = payload::text, ip = host(ip)
BEGIN;

DO $$ BEGIN
  IF EXISTS (SELECT 1 FROM audit_event) THEN
    RAISE EXCEPTION 'audit_event has rows; changing the hash format would make them unverifiable';
  END IF;
END $$;

CREATE OR REPLACE FUNCTION audit_field(v text) RETURNS text LANGUAGE sql IMMUTABLE AS $$
  SELECT CASE WHEN v IS NULL THEN '-' ELSE octet_length(v)::text || ':' || v END
$$;

-- ids are assigned by the trigger after taking the chain lock, so id order = chain order.
ALTER TABLE audit_event ALTER COLUMN id DROP DEFAULT;

CREATE OR REPLACE FUNCTION audit_chain() RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE last_hash bytea;
BEGIN
  PERFORM pg_advisory_xact_lock(727274);           -- 'audit': one writer at a time until commit
  NEW.id := nextval('audit_event_id_seq');          -- overrides any caller-supplied id
  SELECT hash INTO last_hash FROM audit_event ORDER BY id DESC LIMIT 1;
  NEW.prev_hash := coalesce(last_hash, '\x00'::bytea);
  NEW.hash := digest(NEW.prev_hash || convert_to(
      audit_field(to_char(NEW.ts AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.US"Z"')) ||
      audit_field(NEW.actor_id::text) ||
      audit_field(NEW.actor_kind) ||
      audit_field(NEW.action) ||
      audit_field(NEW.entity) ||
      audit_field(NEW.entity_id) ||
      audit_field(NEW.payload::text) ||
      audit_field(host(NEW.ip)),
      'UTF8'), 'sha256');
  RETURN NEW;
END $$;

COMMENT ON FUNCTION audit_chain() IS 'Audit hash chain v2; canonical form documented in db/migrations/000004_audit_hash_v2.up.sql';

COMMIT;
