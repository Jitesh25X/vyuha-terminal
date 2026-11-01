-- Login roles for the services (run after migrations; idempotent; rotates passwords on re-run):
--   docker compose exec -T postgres psql -U vyuha -d vyuha -v ON_ERROR_STOP=1 \
--     -v gateway_pw=… -v engine_pw=… -v quant_pw=… -f /opt/vyuha/roles.sql      (./init-env.sh roles does this)
-- Each is a member of vyuha_app (000001): INSERT-only on audit_event, no DDL. The owner role `vyuha`
-- is used for migrations only.
SELECT format('CREATE ROLE %I LOGIN', r) FROM unnest(ARRAY['vyuha_gateway', 'vyuha_engine', 'vyuha_quant']) AS r
WHERE NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = r) \gexec
ALTER ROLE vyuha_gateway PASSWORD :'gateway_pw';
ALTER ROLE vyuha_engine PASSWORD :'engine_pw';
ALTER ROLE vyuha_quant PASSWORD :'quant_pw';
GRANT vyuha_app TO vyuha_gateway, vyuha_engine, vyuha_quant;
ALTER ROLE vyuha_gateway SET statement_timeout = '30s';
ALTER ROLE vyuha_quant SET statement_timeout = '15min';
