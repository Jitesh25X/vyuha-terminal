// Package auth implements login (argon2id + mandatory TOTP), EdDSA JWTs, rotating refresh tokens
// with reuse detection, lockout and re-authentication. See docs/PRD.md §3 and docs/API.md §1.
package auth

import (
	"errors"
	"fmt"
	"time"
)

var (
	// ErrInvalidCredentials covers unknown user, wrong password, wrong/replayed TOTP and inactive user alike.
	ErrInvalidCredentials = errors.New("auth: invalid credentials")
	ErrUnauthenticated    = errors.New("auth: missing or invalid token")
	ErrMFANotEnrolled     = errors.New("auth: TOTP not enrolled")
	ErrMFAAlreadyEnrolled = errors.New("auth: TOTP already enrolled")
	ErrEnrollNotStarted   = errors.New("auth: TOTP enrollment not started")
	ErrRefreshInvalid     = errors.New("auth: refresh token invalid or expired")
	ErrRefreshReused      = errors.New("auth: refresh token reuse detected; session revoked")
	ErrUserExists         = errors.New("auth: username already exists")
	ErrInvalidInput       = errors.New("auth: invalid input")
	// ErrBusy: too many credential checks in progress, or the user row stayed locked past LockTimeout.
	ErrBusy = errors.New("auth: busy; retry shortly")
)

// LockedError is returned while an account is locked after repeated failures.
type LockedError struct{ Until time.Time }

func (e *LockedError) Error() string {
	return fmt.Sprintf("auth: account locked until %s", e.Until.UTC().Format(time.RFC3339))
}
