-- F-RK-1: loss-streak lockouts keep separate streaks for paper and live. A live lockout blocks live only, a
-- paper one paper only, and paper losses never count toward the live streak.
-- Nothing has written this table yet (the EOD job is not built), so there are no rows to split.
ALTER TABLE discipline_state ADD COLUMN is_paper boolean NOT NULL DEFAULT false;

DO $$
DECLARE pk text;
BEGIN
  SELECT conname INTO STRICT pk FROM pg_constraint
  WHERE conrelid = 'discipline_state'::regclass AND contype = 'p';
  EXECUTE format('ALTER TABLE discipline_state DROP CONSTRAINT %I', pk);
END $$;
ALTER TABLE discipline_state ADD PRIMARY KEY (user_id, is_paper);

-- Every writer must name the mode: a writer that forgets is_paper fails instead of writing paper losses
-- into the live streak. (The default only served to add the column.)
ALTER TABLE discipline_state ALTER COLUMN is_paper DROP DEFAULT;
