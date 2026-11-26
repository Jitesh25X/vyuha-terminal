-- Values the up migration cleared (0 or less) are not restored.
ALTER TABLE instrument
  DROP CONSTRAINT instrument_freeze_qty_positive,
  DROP CONSTRAINT instrument_freeze_provenance_needs_value,
  DROP COLUMN freeze_qty_from_id,
  DROP COLUMN freeze_qty_as_of;
