BEGIN;
DROP TRIGGER IF EXISTS audit_a_isolation_trg ON audit_event;
DROP FUNCTION IF EXISTS audit_require_read_committed();
COMMIT;
