-- name: GetBrokerAccount :one
SELECT * FROM broker_account WHERE id = @id;

-- name: GetUserAccess :one
-- The user's current role and whether they may still act (roles can change after a token was issued).
SELECT role, is_active FROM app_user WHERE id = @id;

-- name: GetAccountMapping :one
SELECT can_view, can_trade FROM account_user_map WHERE account_id = @account_id AND user_id = @user_id;

-- name: StoreBrokerAccessToken :exec
UPDATE broker_account
SET access_token_enc = @access_token_enc, token_expires_at = @token_expires_at, status = @status, updated_at = now()
WHERE id = @id;

-- name: SetBrokerAccountStatus :exec
UPDATE broker_account SET status = @status, updated_at = now() WHERE id = @id;

-- name: CreateBrokerAccount :one
INSERT INTO broker_account (broker, client_code, holder_name, api_key_enc, api_secret_enc, static_ip,
                            public_static_ip, backup_static_ip, data_entitled, ops_cap)
VALUES (@broker, @client_code, @holder_name, @api_key_enc, @api_secret_enc, @static_ip,
        @public_static_ip, @backup_static_ip, @data_entitled, @ops_cap)
RETURNING id;

-- name: SetBrokerAccountSecrets :exec
-- Secrets are sealed with the row id as associated data, so they are written after the INSERT.
UPDATE broker_account SET api_key_enc = @api_key_enc, api_secret_enc = @api_secret_enc, updated_at = now()
WHERE id = @id;

-- name: FindBrokerAccount :one
SELECT id FROM broker_account WHERE broker = @broker AND client_code = @client_code;

-- name: UpsertAccountUser :exec
INSERT INTO account_user_map (account_id, user_id, can_view, can_trade, is_default)
VALUES (@account_id, @user_id, true, @can_trade, @is_default)
ON CONFLICT (account_id, user_id) DO UPDATE SET can_view = true, can_trade = EXCLUDED.can_trade,
                                                is_default = EXCLUDED.is_default;

-- name: ListUserAccounts :many
-- Accounts a user may view, with whether they carry a market-data entitlement (ADR-009).
SELECT a.id, a.broker, a.data_entitled, a.client_code
FROM account_user_map m
JOIN broker_account a ON a.id = m.account_id
JOIN app_user u ON u.id = m.user_id
WHERE m.user_id = @user_id AND m.can_view AND a.status <> 'disabled' AND u.is_active
ORDER BY a.id;

-- name: ListInstrumentBrokerMaps :many
SELECT id, kite_token, upstox_key FROM instrument WHERE id = ANY(@ids::bigint[]) AND valid_to IS NULL;

-- name: IsExchangeHoliday :one
SELECT EXISTS (SELECT 1 FROM exchange_holiday WHERE exchange = 'NSE' AND day = @day) AS holiday;

-- name: ListMappedBrokerAccounts :many
SELECT a.id, a.broker, a.client_code, a.holder_name, a.status, a.data_entitled, a.token_expires_at, m.can_trade
FROM account_user_map m JOIN broker_account a ON a.id = m.account_id
WHERE m.user_id = @user_id AND m.can_view
ORDER BY a.broker, a.client_code;

-- name: ListAllBrokerAccounts :many
SELECT id, broker, client_code, holder_name, status, data_entitled, token_expires_at
FROM broker_account
ORDER BY broker, client_code;
