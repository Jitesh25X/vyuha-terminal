-- Postgres can't drop an enum value, so EXPIRED stays in the type, unused. Rows that use it are order history
-- (retained 5+ years, never rewritten), and an engine from before this migration can't read them: refuse.
DO $$
BEGIN
  IF EXISTS (SELECT 1 FROM "order" WHERE status::text = 'EXPIRED') THEN
    RAISE EXCEPTION 'orders with status EXPIRED exist: this migration cannot be reversed';
  END IF;
END $$;
