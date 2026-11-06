-- The owner's restart rule (ADR-025): an order that is APPROVED but never reached the broker is never sent
-- after a restart. It ends EXPIRED.
ALTER TYPE order_status ADD VALUE IF NOT EXISTS 'EXPIRED';
