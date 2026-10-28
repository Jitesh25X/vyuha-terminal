-- Auth hardening for M0.7 (see docs/TASKS.md notes).
BEGIN;

-- TOTP replay protection (RFC 6238 §5.2, ASVS 2.8.4): the last accepted 30 s time step per user.
-- A code is accepted only if its step is greater than this value.
ALTER TABLE app_user ADD COLUMN totp_last_step bigint;

-- Reuse detection revokes a whole rotation family.
CREATE INDEX refresh_token_family ON refresh_token(family_id);

COMMIT;
