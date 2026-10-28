-- audit_chain() reads the previous hash after taking the chain lock. Under READ COMMITTED each
-- statement sees rows committed before it, so it always finds the true last row. Under
-- REPEATABLE READ / SERIALIZABLE the transaction snapshot may predate rows committed meanwhile,
-- two rows would share a prev_hash and verification would report a broken chain. Refuse that.
BEGIN;

CREATE OR REPLACE FUNCTION audit_require_read_committed() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  IF current_setting('transaction_isolation') <> 'read committed' THEN
    RAISE EXCEPTION 'audit_event inserts require READ COMMITTED isolation (got %)', current_setting('transaction_isolation');
  END IF;
  RETURN NEW;
END $$;

-- Named to sort before audit_chain_trg: BEFORE triggers fire in name order, so the check runs first.
CREATE TRIGGER audit_a_isolation_trg BEFORE INSERT ON audit_event
  FOR EACH ROW EXECUTE FUNCTION audit_require_read_committed();

COMMIT;
