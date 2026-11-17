-- Intentionally a no-op. audit_event is append-only (project invariant 6): the limit.seed events written by
-- the up migration stay. Re-applying the up migration will not duplicate them (it skips limits that
-- already have a limit.seed event).
SELECT 1;
