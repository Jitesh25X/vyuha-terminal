BEGIN;
DROP INDEX IF EXISTS refresh_token_family;
ALTER TABLE app_user DROP COLUMN IF EXISTS totp_last_step;
COMMIT;
