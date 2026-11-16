-- Paper streaks are dropped on the way down: the old key allows one row per user.
DELETE FROM discipline_state WHERE is_paper;

DO $$
DECLARE pk text;
BEGIN
  SELECT conname INTO STRICT pk FROM pg_constraint
  WHERE conrelid = 'discipline_state'::regclass AND contype = 'p';
  EXECUTE format('ALTER TABLE discipline_state DROP CONSTRAINT %I', pk);
END $$;
ALTER TABLE discipline_state ADD PRIMARY KEY (user_id);
ALTER TABLE discipline_state DROP COLUMN is_paper;
