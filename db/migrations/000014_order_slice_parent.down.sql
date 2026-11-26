-- Order history is retained 5+ years and never rewritten: once a child order points at its parent, dropping
-- the link would lose what was asked for. Refuse.
DO $$
BEGIN
  IF EXISTS (SELECT 1 FROM "order" WHERE slice_parent_id IS NOT NULL) THEN
    RAISE EXCEPTION 'sliced orders exist: this migration cannot be reversed';
  END IF;
END $$;
ALTER TABLE "order" DROP COLUMN slice_parent_id;  -- its index goes with it
DROP TABLE order_slice_parent;
