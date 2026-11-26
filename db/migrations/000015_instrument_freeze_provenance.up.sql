-- D-52: where a freeze quantity came from. The sync never overwrites a value with nothing, so it records the
-- file date the value was read from, and for an inherited value the contract it was copied from.
-- Existing rows keep a NULL date: it is not known.
ALTER TABLE instrument
  ADD COLUMN freeze_qty_as_of   date,
  ADD COLUMN freeze_qty_from_id bigint REFERENCES instrument(id),
  ADD CONSTRAINT instrument_freeze_provenance_needs_value
    CHECK (freeze_qty IS NOT NULL OR (freeze_qty_as_of IS NULL AND freeze_qty_from_id IS NULL));

-- A freeze quantity is positive. Rows written before the parser refused other values may hold 0 or less:
-- they have no usable value, so clear them before adding the constraint.
UPDATE instrument SET freeze_qty = NULL, freeze_qty_as_of = NULL, freeze_qty_from_id = NULL WHERE freeze_qty <= 0;
ALTER TABLE instrument
  ADD CONSTRAINT instrument_freeze_qty_positive CHECK (freeze_qty IS NULL OR freeze_qty > 0);
