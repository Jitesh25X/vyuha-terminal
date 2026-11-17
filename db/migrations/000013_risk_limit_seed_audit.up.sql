-- F-RK-1 / F-CO-1: 000011 seeded the desk default limits into risk_limit without an audit trail (D-42).
-- Write one audit_event per limit that is still a seed (never edited by a user), so the audit log shows
-- where every limit value came from.
--
-- The hash chain is computed by the audit_chain() trigger (000004): it takes the chain advisory lock,
-- assigns id, reads the previous hash and hashes the row. This statement therefore never sets id,
-- prev_hash or hash. Rows are produced in key order, so the trigger chains them in key order.
-- audit_a_isolation_trg (000005) needs READ COMMITTED; the migrator's transaction uses the server default.
-- Idempotent: a limit that already has a limit.seed event (same entity_id) gets no second one, so a
-- down followed by an up does not duplicate events. Only INSERTs into audit_event; nothing is updated.
INSERT INTO audit_event (actor_id, actor_kind, action, entity, entity_id, payload)
SELECT NULL, 'system', 'limit.seed', 'risk_limit', r.id::text,
       jsonb_build_object(
         'key', r.key,
         'value', r.value::text,
         'scope', 'desk',
         'hard', r.hard,
         'source', 'migration 000011')
FROM risk_limit r
WHERE r.scope = 'desk' AND r.scope_id IS NULL AND r.updated_by IS NULL
  AND NOT EXISTS (
    SELECT 1 FROM audit_event a
    WHERE a.action = 'limit.seed' AND a.entity = 'risk_limit' AND a.entity_id = r.id::text)
ORDER BY r.key;
