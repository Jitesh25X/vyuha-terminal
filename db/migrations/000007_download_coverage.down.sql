BEGIN;
ALTER TABLE broker_account
  DROP CONSTRAINT IF EXISTS broker_account_static_ip_one_host,
  DROP CONSTRAINT IF EXISTS broker_account_backup_static_ip_one_host;
DROP INDEX IF EXISTS dataset_path_uniq;
DROP TABLE IF EXISTS download_coverage;
COMMIT;
