-- Dev only. Never run in prod (audit retention).
BEGIN;
DROP SCHEMA public CASCADE;
CREATE SCHEMA public;
COMMIT;
