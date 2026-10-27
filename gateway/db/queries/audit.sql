-- name: InsertAuditEvent :one
-- INSERT only: id, prev_hash and hash are set by the audit_chain() trigger (000004).
INSERT INTO audit_event (actor_id, actor_kind, action, entity, entity_id, payload, ip)
VALUES (@actor_id, @actor_kind, @action, @entity, @entity_id, @payload, @ip)
RETURNING id;
