-- M4.1 (F-RS-1): which date ranges a download has finished, so a re-run requests nothing it already has.
-- A range counts even when the broker returned no bars for it (before listing, holidays).
BEGIN;

CREATE TABLE download_coverage (
  source        broker_kind NOT NULL,
  interval      text NOT NULL,
  instrument_id bigint NOT NULL REFERENCES instrument(id),
  from_date     date NOT NULL,
  to_date       date NOT NULL,
  job_id        uuid NOT NULL REFERENCES download_job(id),
  created_at    timestamptz NOT NULL DEFAULT now(),
  PRIMARY KEY (source, interval, instrument_id, from_date, to_date),
  CHECK (from_date <= to_date)
);

-- Invariant 4: a wildcard or multicast address binds without pinning the source, on every path.
ALTER TABLE broker_account
  ADD CONSTRAINT broker_account_static_ip_one_host CHECK (static_ip IS NULL OR NOT (static_ip <<= '0.0.0.0/32' OR static_ip <<= '::/128' OR static_ip <<= '224.0.0.0/4' OR static_ip <<= 'ff00::/8')),
  ADD CONSTRAINT broker_account_backup_static_ip_one_host CHECK (backup_static_ip IS NULL OR NOT (backup_static_ip <<= '0.0.0.0/32' OR backup_static_ip <<= '::/128' OR backup_static_ip <<= '224.0.0.0/4' OR backup_static_ip <<= 'ff00::/8'));

-- One catalogue row per lake file; a later chunk updates its row.
CREATE UNIQUE INDEX dataset_path_uniq ON dataset(path);

GRANT SELECT, INSERT ON download_coverage TO vyuha_app;
GRANT SELECT ON download_coverage TO vyuha_readonly;

COMMIT;
