-- Dev only. Restores the column default; the v1 function stays replaced (it was defective).
BEGIN;
ALTER TABLE audit_event ALTER COLUMN id SET DEFAULT nextval('audit_event_id_seq');
COMMIT;
