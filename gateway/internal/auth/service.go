package auth

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"errors"
	"fmt"
	"net/netip"
	"regexp"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
	"github.com/jackc/pgx/v5/pgtype"

	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/audit"
	vcrypto "github.com/Jitesh25X/vyuha-terminal/gateway/internal/crypto"
	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/store"
)

// Config holds token lifetimes and lockout policy (DATA_MODEL.md: 5 failures → 15 min).
type Config struct {
	AccessTTL    time.Duration // ACCESS_TOKEN_TTL, default 15m
	RefreshTTL   time.Duration // REFRESH_TOKEN_TTL, default 168h
	MFATTL       time.Duration // mfa_token lifetime (API.md: 5 min)
	ReauthTTL    time.Duration // reauth_token lifetime (API.md: 300 s)
	MaxFailures  int32
	LockDuration time.Duration

	// Bounds on concurrent credential checks: each holds an argon2 buffer (64 MiB) and, while waiting
	// on the per-user row lock, a pooled DB connection, and a TOTP check briefly needs a second one to commit its
	// audit row. Keep MaxConcurrentChecks + 1 (reauth) at no more than half the pool.
	MaxConcurrentChecks int
	CheckWait           time.Duration // how long to queue for a slot before ErrBusy
	RowLockTimeout      time.Duration // SET LOCAL lock_timeout for the per-user row lock
}

// DefaultConfig matches the spec defaults.
func DefaultConfig() Config {
	return Config{
		AccessTTL: 15 * time.Minute, RefreshTTL: 168 * time.Hour, MFATTL: 5 * time.Minute,
		ReauthTTL: 5 * time.Minute, MaxFailures: 5, LockDuration: 15 * time.Minute,
		MaxConcurrentChecks: 2, CheckWait: 2 * time.Second, RowLockTimeout: 2 * time.Second,
	}
}

// DB is a pgx pool (or test double) that can start transactions.
type DB interface {
	store.DBTX
	Begin(ctx context.Context) (pgx.Tx, error)
}

// User is the public view of an account.
type User struct {
	ID          string
	Username    string
	DisplayName string
	Role        store.UserRole
}

// ClientMeta is recorded with refresh tokens.
type ClientMeta struct {
	UserAgent string
	IP        netip.Addr
}

// Session is the result of a completed login, enrollment or refresh.
type Session struct {
	AccessToken      string
	AccessExpiresIn  time.Duration
	RefreshToken     string // raw value for the httpOnly cookie; only its SHA-256 is stored
	RefreshExpiresAt time.Time
	RefreshMaxAge    time.Duration // cookie Max-Age (the refresh TTL)
	User             User
}

// Service implements the auth flows.
type Service struct {
	db        DB
	q         *store.Queries
	envelope  *vcrypto.Envelope
	signer    *Signer
	now       func() time.Time
	cfg       Config
	dummyHash string        // compared for unknown users so response time doesn't reveal usernames
	checks    chan struct{} // semaphore: anonymous credential checks in flight (login, mfa, enroll)
	// reauthChecks is reserved for signed-in users re-authenticating (kill-switch release, limit edits),
	// so a flood of anonymous logins can't make reauth fail with ErrBusy.
	reauthChecks chan struct{}
}

// NewService builds the auth service. now is injected for tests; pass time.Now in production.
func NewService(db DB, envelope *vcrypto.Envelope, signer *Signer, cfg Config, now func() time.Time) (*Service, error) {
	dummy, err := HashPassword("dummy-password-for-timing")
	if err != nil {
		return nil, err
	}
	// Zero values would silently disable a protection (lock_timeout 0 = wait forever), so use defaults.
	def := DefaultConfig()
	if cfg.MaxConcurrentChecks < 1 {
		cfg.MaxConcurrentChecks = 1
	}
	if cfg.CheckWait <= 0 {
		cfg.CheckWait = def.CheckWait
	}
	if cfg.RowLockTimeout <= 0 {
		cfg.RowLockTimeout = def.RowLockTimeout
	}
	return &Service{
		db: db, q: store.New(db), envelope: envelope, signer: signer,
		now: func() time.Time { return now().UTC() }, cfg: cfg, dummyHash: dummy,
		checks:       make(chan struct{}, cfg.MaxConcurrentChecks),
		reauthChecks: make(chan struct{}, 1),
	}, nil
}

var usernameRe = regexp.MustCompile(`^[a-z0-9_.]{3,32}$`)

// Actor identifies who performs an administrative action, for the audit log.
type Actor struct {
	ID   string // empty for system actors
	Kind audit.ActorKind
}

// SystemActor is used by vyuhactl.
var SystemActor = Actor{Kind: audit.ActorSystem}

// CreateUser adds an account (vyuhactl user create) and audits it atomically. TOTP is enrolled at first login.
func (s *Service) CreateUser(ctx context.Context, by Actor, username, displayName string, role store.UserRole, password string) (string, error) {
	if !usernameRe.MatchString(username) {
		return "", fmt.Errorf("%w: username must match %s", ErrInvalidInput, usernameRe)
	}
	if n := len([]rune(displayName)); n < 1 || n > 100 {
		return "", fmt.Errorf("%w: display name must be 1–100 characters", ErrInvalidInput)
	}
	switch role {
	case store.UserRoleAdmin, store.UserRoleRiskManager, store.UserRoleTrader, store.UserRoleAnalyst:
	default:
		return "", fmt.Errorf("%w: unknown role %q", ErrInvalidInput, role)
	}
	hash, err := HashPassword(password)
	if err != nil {
		return "", err
	}
	var id pgtype.UUID
	err = pgx.BeginFunc(ctx, s.db, func(tx pgx.Tx) error {
		q := s.q.WithTx(tx)
		var err error
		id, err = q.CreateUser(ctx, store.CreateUserParams{Username: username, DisplayName: displayName, Role: role, PasswordHash: hash})
		if err != nil {
			return err
		}
		_, err = audit.Record(ctx, q, audit.Event{
			ActorID: by.ID, ActorKind: by.Kind, Action: "user.create", Entity: "app_user", EntityID: id.String(),
			Payload: map[string]any{"username": username, "display_name": displayName, "role": string(role)},
		})
		return err
	})
	if err != nil {
		var pgErr *pgconn.PgError
		if errors.As(err, &pgErr) && pgErr.Code == "23505" {
			return "", ErrUserExists
		}
		return "", fmt.Errorf("create user: %w", err)
	}
	return id.String(), nil
}

// LoginResult is step one of login: an mfa_token for the TOTP step.
type LoginResult struct {
	MFAToken    string
	MFAEnrolled bool
}

// Login checks the password and returns a short-lived mfa_token. Failures count towards lockout.
// Failure counters reset only after the TOTP step succeeds, so a known password can't be used
// to keep resetting the counter while guessing TOTP codes.
func (s *Service) Login(ctx context.Context, username, password string, meta ClientMeta) (LoginResult, error) {
	release, err := s.acquireCheck(ctx)
	if err != nil {
		return LoginResult{}, err
	}
	defer release()
	now := s.now()
	u, err := s.q.GetUserByUsername(ctx, username)
	if errors.Is(err, pgx.ErrNoRows) {
		_, _ = verifyPassword(password, s.dummyHash)
		return LoginResult{}, s.auditRejected(ctx, "", "unknown_user", meta)
	}
	if err != nil {
		return LoginResult{}, fmt.Errorf("load user: %w", err)
	}
	if !u.IsActive {
		_, _ = verifyPassword(password, s.dummyHash)
		return LoginResult{}, s.auditRejected(ctx, u.ID.String(), "inactive", meta)
	}
	// Password success does not reset the counter (see above), hence resetOnSuccess=false.
	u, err = s.attempt(ctx, u.ID, now, attemptOpts{step: "password", failAction: "auth.login.failure", meta: meta},
		func(_ *store.Queries, locked store.AppUser) (bool, error) {
			return verifyPassword(password, locked.PasswordHash)
		})
	if errors.Is(err, ErrUnauthenticated) {
		return LoginResult{}, ErrInvalidCredentials // deactivated or deleted between the two reads
	}
	if err != nil {
		return LoginResult{}, err
	}
	tok, err := s.signer.issue(audMFA, u.ID.String(), "", now, s.cfg.MFATTL)
	if err != nil {
		return LoginResult{}, err
	}
	return LoginResult{MFAToken: tok, MFAEnrolled: u.TotpEnrolled}, nil
}

// VerifyMFA completes login for an enrolled user and starts a new session (refresh family).
func (s *Service) VerifyMFA(ctx context.Context, mfaToken, code string, meta ClientMeta) (Session, error) {
	release, err := s.acquireCheck(ctx)
	if err != nil {
		return Session{}, err
	}
	defer release()
	now := s.now()
	id, err := s.subjectFromToken(mfaToken, audMFA, now)
	if err != nil {
		return Session{}, err
	}
	opts := attemptOpts{reset: true, step: "totp", failAction: "auth.login.failure", successAction: "auth.login.success", meta: meta}
	u, err := s.attempt(ctx, id, now, opts, func(q *store.Queries, u store.AppUser) (bool, error) {
		if !u.TotpEnrolled {
			return false, ErrMFANotEnrolled
		}
		return s.totpOK(ctx, q, u, code, now)
	})
	if err != nil {
		return Session{}, err
	}
	return s.startSession(ctx, u, meta, now)
}

// Enrollment is what the user scans into an authenticator app.
type Enrollment struct {
	OtpauthURL string
	QRPNG      []byte
}

// BeginEnroll generates (or replaces an unconfirmed) TOTP secret for a not-yet-enrolled user.
func (s *Service) BeginEnroll(ctx context.Context, mfaToken string, meta ClientMeta) (Enrollment, error) {
	now := s.now()
	u, err := s.userFromToken(ctx, mfaToken, audMFA, now)
	if err != nil {
		return Enrollment{}, err
	}
	if u.TotpEnrolled {
		return Enrollment{}, ErrMFAAlreadyEnrolled
	}
	e, err := newTOTP(u.Username)
	if err != nil {
		return Enrollment{}, err
	}
	sealed, err := s.envelope.Seal([]byte(e.secret), totpAD(u.ID))
	if err != nil {
		return Enrollment{}, err
	}
	err = pgx.BeginFunc(ctx, s.db, func(tx pgx.Tx) error {
		q := s.q.WithTx(tx)
		n, err := q.SetPendingTOTPSecret(ctx, store.SetPendingTOTPSecretParams{ID: u.ID, SecretEnc: sealed, Now: ts(now)})
		if err != nil {
			return fmt.Errorf("store totp secret: %w", err)
		}
		if n == 0 {
			return ErrMFAAlreadyEnrolled
		}
		return s.auditUser(ctx, q, u.ID.String(), "auth.mfa.enroll_begin", nil, meta)
	})
	if err != nil {
		return Enrollment{}, err
	}
	return Enrollment{OtpauthURL: e.otpauthURL, QRPNG: e.qrPNG}, nil
}

// ConfirmEnroll verifies the first code from the new authenticator, marks TOTP enrolled and logs in.
func (s *Service) ConfirmEnroll(ctx context.Context, mfaToken, code string, meta ClientMeta) (Session, error) {
	release, err := s.acquireCheck(ctx)
	if err != nil {
		return Session{}, err
	}
	defer release()
	now := s.now()
	id, err := s.subjectFromToken(mfaToken, audMFA, now)
	if err != nil {
		return Session{}, err
	}
	opts := attemptOpts{reset: true, step: "enroll_confirm", failAction: "auth.login.failure", successAction: "auth.mfa.enrolled", meta: meta}
	u, err := s.attempt(ctx, id, now, opts, func(q *store.Queries, u store.AppUser) (bool, error) {
		if u.TotpEnrolled {
			return false, ErrMFAAlreadyEnrolled
		}
		if u.TotpSecretEnc == nil {
			return false, ErrEnrollNotStarted
		}
		ok, err := s.totpOK(ctx, q, u, code, now)
		if err != nil || !ok {
			return ok, err
		}
		// Same transaction and row lock: the verified secret is the one that becomes enrolled.
		n, err := q.ConfirmTOTP(ctx, store.ConfirmTOTPParams{ID: u.ID, Now: ts(now), VerifiedSecretEnc: u.TotpSecretEnc})
		if err != nil {
			return false, fmt.Errorf("confirm totp: %w", err)
		}
		if n == 0 {
			return false, ErrMFAAlreadyEnrolled
		}
		return true, nil
	})
	if err != nil {
		return Session{}, err
	}
	return s.startSession(ctx, u, meta, now)
}

// Refresh rotates a refresh token. Presenting an already-revoked token revokes its whole family.
func (s *Service) Refresh(ctx context.Context, rawRefresh string, meta ClientMeta) (Session, error) {
	now := s.now()
	hash, ok := hashRefresh(rawRefresh)
	if !ok {
		return Session{}, ErrRefreshInvalid
	}
	var (
		sess   Session
		reused bool // revoked token presented: family revoked, report reuse
		ended  bool // user can no longer hold a session: family revoked, report invalid
	)
	err := pgx.BeginFunc(ctx, s.db, func(tx pgx.Tx) error {
		q := s.q.WithTx(tx)
		rt, err := q.GetRefreshTokenForUpdate(ctx, hash)
		if errors.Is(err, pgx.ErrNoRows) {
			return ErrRefreshInvalid
		}
		if err != nil {
			return fmt.Errorf("load refresh token: %w", err)
		}
		if rt.RevokedAt.Valid {
			// Reuse: commit the family revocation, then report it.
			n, err := q.RevokeRefreshFamily(ctx, store.RevokeRefreshFamilyParams{FamilyID: rt.FamilyID, Now: ts(now)})
			if err != nil {
				return fmt.Errorf("revoke family: %w", err)
			}
			reused = true
			return s.auditUser(ctx, q, rt.UserID.String(), "auth.refresh.reuse",
				map[string]any{"family_id": rt.FamilyID.String(), "revoked_count": n}, meta)
		}
		if !rt.ExpiresAt.Time.After(now) {
			return ErrRefreshInvalid
		}
		u, err := q.GetUserByID(ctx, rt.UserID)
		if err != nil {
			return fmt.Errorf("load user: %w", err)
		}
		// Lockout gates new logins only; it must not end existing sessions, or anyone who knows a
		// username could log a trader or risk manager out by failing five logins.
		if !u.IsActive || !u.TotpEnrolled {
			if _, err := q.RevokeRefreshFamily(ctx, store.RevokeRefreshFamilyParams{FamilyID: rt.FamilyID, Now: ts(now)}); err != nil {
				return fmt.Errorf("revoke family: %w", err)
			}
			ended = true
			return s.auditUser(ctx, q, u.ID.String(), "auth.session.revoked",
				map[string]any{"family_id": rt.FamilyID.String(), "reason": "user inactive or not enrolled"}, meta)
		}
		if err := q.RevokeRefreshToken(ctx, store.RevokeRefreshTokenParams{ID: rt.ID, Now: ts(now)}); err != nil {
			return fmt.Errorf("revoke refresh token: %w", err)
		}
		sess, err = s.issueSession(ctx, q, u, rt.FamilyID, meta, now)
		return err
	})
	// Revocations above are committed before the failure is reported.
	switch {
	case err != nil:
		return Session{}, err
	case reused:
		return Session{}, ErrRefreshReused
	case ended:
		return Session{}, ErrRefreshInvalid
	}
	return sess, nil
}

// Logout revokes the session (refresh family) of the presented token. Unknown tokens are ignored.
func (s *Service) Logout(ctx context.Context, rawRefresh string, meta ClientMeta) error {
	hash, ok := hashRefresh(rawRefresh)
	if !ok {
		return nil
	}
	return pgx.BeginFunc(ctx, s.db, func(tx pgx.Tx) error {
		q := s.q.WithTx(tx)
		rt, err := q.GetRefreshTokenForUpdate(ctx, hash)
		if errors.Is(err, pgx.ErrNoRows) {
			return nil
		}
		if err != nil {
			return fmt.Errorf("load refresh token: %w", err)
		}
		n, err := q.RevokeRefreshFamily(ctx, store.RevokeRefreshFamilyParams{FamilyID: rt.FamilyID, Now: ts(s.now())})
		if err != nil {
			return fmt.Errorf("revoke family: %w", err)
		}
		if n == 0 {
			return nil // already logged out or revoked; nothing changed
		}
		return s.auditUser(ctx, q, rt.UserID.String(), "auth.logout", map[string]any{"family_id": rt.FamilyID.String()}, meta)
	})
}

// Reauth re-checks password + TOTP for sensitive actions and returns a short-lived reauth_token.
func (s *Service) Reauth(ctx context.Context, userID, password, code string, meta ClientMeta) (string, time.Duration, error) {
	release, err := s.acquire(ctx, s.reauthChecks)
	if err != nil {
		return "", 0, err
	}
	defer release()
	now := s.now()
	var id pgtype.UUID
	if err := id.Scan(userID); err != nil {
		return "", 0, ErrUnauthenticated
	}
	opts := attemptOpts{reset: true, step: "reauth", failAction: "auth.reauth.failure", successAction: "auth.reauth.success", meta: meta}
	u, err := s.attempt(ctx, id, now, opts, func(q *store.Queries, u store.AppUser) (bool, error) {
		if !u.TotpEnrolled {
			return false, ErrMFANotEnrolled
		}
		ok, err := verifyPassword(password, u.PasswordHash)
		if err != nil || !ok {
			return false, err
		}
		return s.totpOK(ctx, q, u, code, now)
	})
	if err != nil {
		return "", 0, err
	}
	tok, err := s.signer.issue(audReauth, u.ID.String(), "", now, s.cfg.ReauthTTL)
	return tok, s.cfg.ReauthTTL, err
}

// VerifyAccess validates an access JWT (Authorization: Bearer).
func (s *Service) VerifyAccess(token string) (*Claims, error) {
	return s.signer.parse(token, audAccess, s.now())
}

// VerifyReauth validates an X-Reauth token and checks it belongs to userID.
func (s *Service) VerifyReauth(token, userID string) error {
	c, err := s.signer.parse(token, audReauth, s.now())
	if err != nil || c.Subject != userID {
		return ErrUnauthenticated
	}
	return nil
}

// ── helpers ──────────────────────────────────────────────────────────────────

func (s *Service) userFromToken(ctx context.Context, token, aud string, now time.Time) (store.AppUser, error) {
	id, err := s.subjectFromToken(token, aud, now)
	if err != nil {
		return store.AppUser{}, err
	}
	return s.loadActiveUser(ctx, id.String(), now)
}

func (s *Service) subjectFromToken(token, aud string, now time.Time) (pgtype.UUID, error) {
	c, err := s.signer.parse(token, aud, now)
	if err != nil {
		return pgtype.UUID{}, err
	}
	var id pgtype.UUID
	if err := id.Scan(c.Subject); err != nil {
		return pgtype.UUID{}, ErrUnauthenticated
	}
	return id, nil
}

// attempt runs one credential check (password and/or TOTP) under SELECT … FOR UPDATE on the user
// row, so attempts for a user are serialised and the 5-failure lockout holds under concurrency:
// lock check → verify → record failure or (if resetOnSuccess) reset, all in one transaction.
// check returns ok=false for a wrong credential (counted) or an error for anything else (not
// counted, rolled back). Failures are committed before ErrInvalidCredentials / LockedError returns.
// attemptOpts: reset the failure counter on success; audit actions for failure/success (in the same tx).
type attemptOpts struct {
	reset         bool
	step          string // password | totp | enroll_confirm | reauth
	failAction    string
	successAction string // empty: no success event (e.g. the password step alone)
	meta          ClientMeta
}

func (s *Service) attempt(ctx context.Context, id pgtype.UUID, now time.Time, opts attemptOpts,
	check func(q *store.Queries, u store.AppUser) (bool, error),
) (store.AppUser, error) {
	var (
		user   store.AppUser
		failed error
	)
	err := pgx.BeginFunc(ctx, s.db, func(tx pgx.Tx) error {
		// SET doesn't take bind parameters; the value is an integer from config.
		if _, err := tx.Exec(ctx, fmt.Sprintf("SET LOCAL lock_timeout = %d", s.cfg.RowLockTimeout.Milliseconds())); err != nil {
			return fmt.Errorf("set lock_timeout: %w", err)
		}
		q := s.q.WithTx(tx)
		u, err := q.GetUserForUpdate(ctx, id)
		if errors.Is(err, pgx.ErrNoRows) {
			return ErrUnauthenticated
		}
		var pgErr *pgconn.PgError
		if errors.As(err, &pgErr) && pgErr.Code == "55P03" { // lock_not_available
			return ErrBusy
		}
		if err != nil {
			return fmt.Errorf("lock user: %w", err)
		}
		if !u.IsActive {
			return ErrUnauthenticated
		}
		if err := lockedAt(u, now); err != nil {
			return err
		}
		ok, err := check(q, u)
		if err != nil {
			return err
		}
		if !ok {
			failed, err = s.recordFailure(ctx, q, u.ID, now)
			if err != nil {
				return err
			}
			payload := map[string]any{"step": opts.step}
			var le *LockedError
			if errors.As(failed, &le) {
				payload["locked_until"] = le.Until.UTC().Format(time.RFC3339)
			}
			return s.auditUser(ctx, q, u.ID.String(), opts.failAction, payload, opts.meta)
		}
		if opts.reset {
			if err := q.ResetLoginFailures(ctx, store.ResetLoginFailuresParams{ID: u.ID, Now: ts(now)}); err != nil {
				return fmt.Errorf("reset failures: %w", err)
			}
		}
		if opts.successAction != "" {
			if err := s.auditUser(ctx, q, u.ID.String(), opts.successAction, map[string]any{"step": opts.step}, opts.meta); err != nil {
				return err
			}
		}
		user = u
		return nil
	})
	if err != nil {
		return store.AppUser{}, err
	}
	if failed != nil {
		return store.AppUser{}, failed
	}
	return user, nil
}

func (s *Service) loadActiveUser(ctx context.Context, userID string, now time.Time) (store.AppUser, error) {
	var id pgtype.UUID
	if err := id.Scan(userID); err != nil {
		return store.AppUser{}, ErrUnauthenticated
	}
	u, err := s.q.GetUserByID(ctx, id)
	if errors.Is(err, pgx.ErrNoRows) {
		return store.AppUser{}, ErrUnauthenticated
	}
	if err != nil {
		return store.AppUser{}, fmt.Errorf("load user: %w", err)
	}
	if !u.IsActive {
		return store.AppUser{}, ErrUnauthenticated
	}
	if err := lockedAt(u, now); err != nil {
		return store.AppUser{}, err
	}
	return u, nil
}

// totpOK verifies code against the user's secret and claims its time step once (inside attempt's
// transaction). Wrong and replayed codes both return false, so both count towards lockout.
func (s *Service) totpOK(ctx context.Context, q *store.Queries, u store.AppUser, code string, now time.Time) (bool, error) {
	if u.TotpSecretEnc == nil {
		return false, ErrMFANotEnrolled
	}
	secret, err := s.envelope.Open(u.TotpSecretEnc, totpAD(u.ID))
	if err != nil {
		return false, fmt.Errorf("open totp secret: %w", err)
	}
	defer clear(secret)
	userID := u.ID.String()
	// On the pool, not attempt's transaction: the row must be committed before the seed is used, or a later
	// error would roll it back and leave a decrypt with no audit row (D-16). It's a second connection while
	// attempt holds one, so the time limit fails this closed if the pool is ever too small (see Config).
	auditCtx, cancel := context.WithTimeout(ctx, s.cfg.RowLockTimeout)
	defer cancel()
	if _, err := audit.Record(auditCtx, s.q, audit.Event{
		ActorID: userID, ActorKind: audit.ActorUser, Action: "auth.totp.opened", Entity: "app_user", EntityID: userID,
		Payload: map[string]any{"purpose": "login_totp_verification"},
	}); err != nil {
		return false, fmt.Errorf("audit totp use: %w", err)
	}
	step, ok := matchTOTP(string(secret), code, now)
	if !ok {
		return false, nil
	}
	n, err := q.ClaimTOTPStep(ctx, store.ClaimTOTPStepParams{ID: u.ID, Step: pgtype.Int8{Int64: step, Valid: true}})
	if err != nil {
		return false, fmt.Errorf("claim totp step: %w", err)
	}
	return n == 1, nil
}

// recordFailure counts a failed attempt. The first result is the outcome to report
// (ErrInvalidCredentials, or LockedError if this failure locked the account); the second is a DB error.
func (s *Service) recordFailure(ctx context.Context, q *store.Queries, id pgtype.UUID, now time.Time) (error, error) {
	row, err := q.RecordLoginFailure(ctx, store.RecordLoginFailureParams{
		ID: id, Now: ts(now), MaxFailures: s.cfg.MaxFailures, LockUntil: ts(now.Add(s.cfg.LockDuration)),
	})
	if err != nil {
		return nil, fmt.Errorf("record failure: %w", err)
	}
	if row.LockedUntil.Valid && row.LockedUntil.Time.After(now) {
		return &LockedError{Until: row.LockedUntil.Time}, nil
	}
	return ErrInvalidCredentials, nil
}

// startSession opens a new refresh family; the failure counter was already reset by attempt.
func (s *Service) startSession(ctx context.Context, u store.AppUser, meta ClientMeta, now time.Time) (Session, error) {
	family, err := newUUID()
	if err != nil {
		return Session{}, err
	}
	return s.issueSession(ctx, s.q, u, family, meta, now)
}

func (s *Service) issueSession(ctx context.Context, q *store.Queries, u store.AppUser, family pgtype.UUID, meta ClientMeta, now time.Time) (Session, error) {
	access, err := s.signer.issue(audAccess, u.ID.String(), string(u.Role), now, s.cfg.AccessTTL)
	if err != nil {
		return Session{}, err
	}
	var raw [32]byte
	if _, err := rand.Read(raw[:]); err != nil {
		return Session{}, fmt.Errorf("refresh token: %w", err)
	}
	rawB64 := base64.RawURLEncoding.EncodeToString(raw[:])
	hash := sha256.Sum256(raw[:])
	expires := now.Add(s.cfg.RefreshTTL)
	params := store.InsertRefreshTokenParams{
		UserID: u.ID, TokenHash: hash[:], FamilyID: family, ExpiresAt: ts(expires), Now: ts(now),
		UserAgent: pgtype.Text{String: truncate(meta.UserAgent, 512), Valid: meta.UserAgent != ""},
	}
	if meta.IP.IsValid() {
		ip := meta.IP
		params.Ip = &ip
	}
	if err := q.InsertRefreshToken(ctx, params); err != nil {
		return Session{}, fmt.Errorf("store refresh token: %w", err)
	}
	return Session{
		AccessToken: access, AccessExpiresIn: s.cfg.AccessTTL, RefreshToken: rawB64, RefreshExpiresAt: expires,
		RefreshMaxAge: s.cfg.RefreshTTL,
		User:          User{ID: u.ID.String(), Username: u.Username, DisplayName: u.DisplayName, Role: u.Role},
	}, nil
}

// acquireCheck takes an anonymous credential-check slot, waiting at most CheckWait. Call release.
func (s *Service) acquireCheck(ctx context.Context) (func(), error) { return s.acquire(ctx, s.checks) }

func (s *Service) acquire(ctx context.Context, sem chan struct{}) (func(), error) {
	timer := time.NewTimer(s.cfg.CheckWait)
	defer timer.Stop()
	select {
	case sem <- struct{}{}:
		return func() { <-sem }, nil
	case <-timer.C:
		return nil, ErrBusy
	case <-ctx.Done():
		return nil, ctx.Err()
	}
}

// auditUser records an auth event about (and by) a user, using q so it commits with the change.
func (s *Service) auditUser(ctx context.Context, q *store.Queries, userID, action string, payload map[string]any, meta ClientMeta) error {
	_, err := audit.Record(ctx, q, audit.Event{
		ActorID: userID, ActorKind: audit.ActorUser, Action: action, Entity: "app_user", EntityID: userID,
		Payload: payload, IP: meta.IP,
	})
	return err
}

// auditRejected records a login for an unknown or inactive user and returns ErrInvalidCredentials.
// The attempted username is never stored for unknown users: people type passwords into the username
// box, many passwords look like valid usernames, and audit rows can never be removed. An inactive
// user is identified by their account id instead.
func (s *Service) auditRejected(ctx context.Context, userID, reason string, meta ClientMeta) error {
	_, err := audit.Record(ctx, s.q, audit.Event{
		ActorID: userID, ActorKind: audit.ActorUser, Action: "auth.login.failure", Entity: "app_user", EntityID: userID,
		Payload: map[string]any{"step": "password", "reason": reason}, IP: meta.IP,
	})
	if err != nil {
		return err
	}
	return ErrInvalidCredentials
}

func lockedAt(u store.AppUser, now time.Time) error {
	if u.LockedUntil.Valid && u.LockedUntil.Time.After(now) {
		return &LockedError{Until: u.LockedUntil.Time}
	}
	return nil
}

// hashRefresh decodes a raw refresh token (32 bytes, base64url) and returns its SHA-256.
func hashRefresh(raw string) ([]byte, bool) {
	b, err := base64.RawURLEncoding.DecodeString(raw)
	if err != nil || len(b) != 32 {
		return nil, false
	}
	sum := sha256.Sum256(b)
	return sum[:], true
}

func totpAD(id pgtype.UUID) []byte { return []byte("app_user.totp_secret_enc:" + id.String()) }

func ts(t time.Time) pgtype.Timestamptz { return pgtype.Timestamptz{Time: t, Valid: true} }

func newUUID() (pgtype.UUID, error) {
	var b [16]byte
	if _, err := rand.Read(b[:]); err != nil {
		return pgtype.UUID{}, fmt.Errorf("uuid: %w", err)
	}
	b[6] = (b[6] & 0x0f) | 0x40 // version 4
	b[8] = (b[8] & 0x3f) | 0x80 // RFC 4122 variant
	return pgtype.UUID{Bytes: b, Valid: true}, nil
}

func truncate(s string, n int) string {
	if len(s) <= n {
		return s
	}
	return s[:n]
}
