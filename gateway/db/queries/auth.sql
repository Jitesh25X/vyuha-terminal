-- name: CreateUser :one
INSERT INTO app_user (username, display_name, role, password_hash)
VALUES ($1, $2, $3, $4)
RETURNING id;

-- name: GetUserByUsername :one
SELECT * FROM app_user WHERE username = $1;

-- name: GetUserByID :one
SELECT * FROM app_user WHERE id = $1;

-- name: GetUserForUpdate :one
-- Serialises password/TOTP attempts per user so the lockout holds under concurrent requests.
SELECT * FROM app_user WHERE id = $1 FOR UPDATE;

-- name: RecordLoginFailure :one
-- A failure after an expired lock starts a new count. Reaching max_failures locks until lock_until.
UPDATE app_user SET
  failed_logins = CASE WHEN locked_until IS NOT NULL AND locked_until <= @now::timestamptz
                       THEN 1 ELSE failed_logins + 1 END,
  locked_until  = CASE
                    WHEN locked_until IS NOT NULL AND locked_until > @now::timestamptz THEN locked_until
                    WHEN (CASE WHEN locked_until IS NOT NULL AND locked_until <= @now::timestamptz
                               THEN 1 ELSE failed_logins + 1 END) >= @max_failures::int
                      THEN @lock_until::timestamptz
                    WHEN locked_until IS NOT NULL AND locked_until <= @now::timestamptz THEN NULL
                    ELSE locked_until
                  END,
  updated_at    = @now::timestamptz
WHERE id = @id
RETURNING failed_logins, locked_until;

-- name: ResetLoginFailures :exec
UPDATE app_user SET failed_logins = 0, locked_until = NULL, updated_at = @now::timestamptz
WHERE id = @id;

-- name: SetPendingTOTPSecret :execrows
-- Only while not enrolled; re-running enrollment replaces an unconfirmed secret.
UPDATE app_user SET totp_secret_enc = @secret_enc, totp_last_step = NULL, updated_at = @now::timestamptz
WHERE id = @id AND NOT totp_enrolled;

-- name: ConfirmTOTP :execrows
-- Only the secret the code was verified against may become the enrolled one.
UPDATE app_user SET totp_enrolled = true, updated_at = @now::timestamptz
WHERE id = @id AND NOT totp_enrolled AND totp_secret_enc = @verified_secret_enc;

-- name: ClaimTOTPStep :execrows
-- Atomically accepts a TOTP time step once; 0 rows means the code was already used (replay).
UPDATE app_user SET totp_last_step = @step
WHERE id = @id AND (totp_last_step IS NULL OR totp_last_step < @step);

-- name: InsertRefreshToken :exec
INSERT INTO refresh_token (user_id, token_hash, family_id, expires_at, user_agent, ip, created_at)
VALUES (@user_id, @token_hash, @family_id, @expires_at, @user_agent, @ip, @now::timestamptz);

-- name: GetRefreshTokenForUpdate :one
SELECT * FROM refresh_token WHERE token_hash = @token_hash FOR UPDATE;

-- name: RevokeRefreshToken :exec
UPDATE refresh_token SET revoked_at = @now::timestamptz WHERE id = @id AND revoked_at IS NULL;

-- name: RevokeRefreshFamily :execrows
UPDATE refresh_token SET revoked_at = @now::timestamptz WHERE family_id = @family_id AND revoked_at IS NULL;
