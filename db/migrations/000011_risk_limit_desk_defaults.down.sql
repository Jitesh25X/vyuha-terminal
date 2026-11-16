-- Remove only rows no user has touched; an edited row carries an updated_by and stays.
DELETE FROM risk_limit WHERE scope = 'desk' AND scope_id IS NULL AND updated_by IS NULL;
ALTER TABLE risk_limit DROP CONSTRAINT risk_limit_updated_by_seed_only;
-- Fails if any other row with a NULL updated_by exists (none should: only this migration writes them).
ALTER TABLE risk_limit ALTER COLUMN updated_by SET NOT NULL;
