package auth

import (
	"context"
	"crypto/rand"
	"encoding/base64"
	"encoding/hex"
	"errors"
	"net/netip"
	"os"
	"testing"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"
	"github.com/pquerna/otp"
	"github.com/pquerna/otp/totp"

	vcrypto "github.com/jiteshraj/vyuha/gateway/internal/crypto"
	"github.com/jiteshraj/vyuha/gateway/internal/store"
)

// Integration suite: runs against Postgres when VYUHA_TEST_POSTGRES_URL is set (make test-gateway).

const testPassword = "correct horse battery staple"

type fixture struct {
	t     *testing.T
	ctx   context.Context
	pool  *pgxpool.Pool
	svc   *Service
	clock time.Time
	meta  ClientMeta
}

func newFixture(t *testing.T) *fixture {
	t.Helper()
	url := os.Getenv("VYUHA_TEST_POSTGRES_URL")
	if url == "" {
		t.Skip("VYUHA_TEST_POSTGRES_URL not set")
	}
	ctx := context.Background()
	pool, err := store.NewPool(ctx, url)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(pool.Close)
	key := make([]byte, 32)
	_, _ = rand.Read(key)
	env, err := vcrypto.NewEnvelope(base64.StdEncoding.EncodeToString(key))
	if err != nil {
		t.Fatal(err)
	}
	f := &fixture{t: t, ctx: ctx, pool: pool, clock: t0, meta: ClientMeta{UserAgent: "test", IP: netip.MustParseAddr("127.0.0.1")}}
	f.svc, err = NewService(pool, env, newSigner(t), DefaultConfig(), func() time.Time { return f.clock })
	if err != nil {
		t.Fatal(err)
	}
	return f
}

func (f *fixture) advance(d time.Duration) { f.clock = f.clock.Add(d) }

// createUser makes a fresh user and deletes it (and its refresh tokens) after the test.
func (f *fixture) createUser() string {
	f.t.Helper()
	var b [4]byte
	_, _ = rand.Read(b[:])
	username := "t_" + hex.EncodeToString(b[:])
	id, err := f.svc.CreateUser(f.ctx, username, "Test User", store.UserRoleTrader, testPassword)
	if err != nil {
		f.t.Fatal(err)
	}
	f.t.Cleanup(func() { _, _ = f.pool.Exec(context.Background(), "DELETE FROM app_user WHERE id = $1", id) })
	return username
}

func (f *fixture) code(secret string) string {
	f.t.Helper()
	c, err := totp.GenerateCodeCustom(secret, f.clock, totpOpts)
	if err != nil {
		f.t.Fatal(err)
	}
	return c
}

// enroll runs first login + enrollment and returns the TOTP secret and the first session.
func (f *fixture) enroll(username string) (string, Session) {
	f.t.Helper()
	lr, err := f.svc.Login(f.ctx, username, testPassword)
	if err != nil || lr.MFAEnrolled {
		f.t.Fatalf("first login: %+v %v", lr, err)
	}
	e, err := f.svc.BeginEnroll(f.ctx, lr.MFAToken)
	if err != nil {
		f.t.Fatal(err)
	}
	key, err := otp.NewKeyFromURL(e.OtpauthURL)
	if err != nil {
		f.t.Fatal(err)
	}
	sess, err := f.svc.ConfirmEnroll(f.ctx, lr.MFAToken, f.code(key.Secret()), f.meta)
	if err != nil {
		f.t.Fatalf("confirm enroll: %v", err)
	}
	return key.Secret(), sess
}

// login does password + TOTP for an enrolled user.
func (f *fixture) login(username, secret string) (Session, error) {
	f.t.Helper()
	lr, err := f.svc.Login(f.ctx, username, testPassword)
	if err != nil {
		return Session{}, err
	}
	return f.svc.VerifyMFA(f.ctx, lr.MFAToken, f.code(secret), f.meta)
}

func wantErr(t *testing.T, got, want error) {
	t.Helper()
	if !errors.Is(got, want) {
		t.Fatalf("got error %v, want %v", got, want)
	}
}

func wantLocked(t *testing.T, err error) *LockedError {
	t.Helper()
	var le *LockedError
	if !errors.As(err, &le) {
		t.Fatalf("want LockedError, got %v", err)
	}
	return le
}

func TestCreateUserValidation(t *testing.T) {
	f := newFixture(t)
	existing := f.createUser()
	tests := []struct {
		name, username, display string
		role                    store.UserRole
		password                string
		want                    error
	}{
		{"bad username", "Bad Name", "X", store.UserRoleTrader, testPassword, ErrInvalidInput},
		{"short password", "t_shortpw", "X", store.UserRoleTrader, "short", ErrInvalidInput},
		{"unknown role", "t_badrole", "X", store.UserRole("superuser"), testPassword, ErrInvalidInput},
		{"empty display name", "t_nodisp", "", store.UserRoleTrader, testPassword, ErrInvalidInput},
		{"duplicate", existing, "X", store.UserRoleTrader, testPassword, ErrUserExists},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			_, err := f.svc.CreateUser(f.ctx, tt.username, tt.display, tt.role, tt.password)
			wantErr(t, err, tt.want)
		})
	}
}

func TestEnrollmentAndLogin(t *testing.T) {
	f := newFixture(t)
	username := f.createUser()

	lr, err := f.svc.Login(f.ctx, username, testPassword)
	if err != nil {
		t.Fatal(err)
	}
	_, err = f.svc.VerifyMFA(f.ctx, lr.MFAToken, "123456", f.meta)
	wantErr(t, err, ErrMFANotEnrolled)
	_, err = f.svc.ConfirmEnroll(f.ctx, lr.MFAToken, "123456", f.meta)
	wantErr(t, err, ErrEnrollNotStarted)

	secret, sess := f.enroll(username)
	if sess.AccessToken == "" || sess.RefreshToken == "" || sess.User.Username != username || sess.User.Role != store.UserRoleTrader {
		t.Fatalf("unexpected session: %+v", sess.User)
	}
	claims, err := f.svc.VerifyAccess(sess.AccessToken)
	if err != nil || claims.Subject != sess.User.ID || claims.Role != "trader" {
		t.Fatalf("access token: %+v %v", claims, err)
	}

	lr, _ = f.svc.Login(f.ctx, username, testPassword)
	if !lr.MFAEnrolled {
		t.Fatal("expected mfa_enrolled after confirmation")
	}
	_, err = f.svc.BeginEnroll(f.ctx, lr.MFAToken)
	wantErr(t, err, ErrMFAAlreadyEnrolled)
	_, err = f.svc.VerifyAccess(lr.MFAToken)
	wantErr(t, err, ErrUnauthenticated)

	f.advance(30 * time.Second)
	if _, err := f.login(username, secret); err != nil {
		t.Fatalf("second login: %v", err)
	}
}

func TestTOTPReplayRejected(t *testing.T) {
	f := newFixture(t)
	username := f.createUser()
	secret, _ := f.enroll(username) // claims the current step

	_, err := f.login(username, secret)
	wantErr(t, err, ErrInvalidCredentials) // same 30 s step: replay

	f.advance(30 * time.Second)
	if _, err := f.login(username, secret); err != nil {
		t.Fatalf("next step should work: %v", err)
	}
	// An older (still in-window) code is rejected once a newer step was used.
	lr, _ := f.svc.Login(f.ctx, username, testPassword)
	old, _ := totp.GenerateCodeCustom(secret, f.clock.Add(-30*time.Second), totpOpts)
	_, err = f.svc.VerifyMFA(f.ctx, lr.MFAToken, old, f.meta)
	wantErr(t, err, ErrInvalidCredentials)
}

func TestLoginFailuresAndLockout(t *testing.T) {
	f := newFixture(t)
	username := f.createUser()
	secret, _ := f.enroll(username)

	_, err := f.svc.Login(f.ctx, "t_nobody_here", testPassword)
	wantErr(t, err, ErrInvalidCredentials)

	for i := 1; i <= 4; i++ {
		_, err := f.svc.Login(f.ctx, username, "wrong password!!")
		wantErr(t, err, ErrInvalidCredentials)
	}
	le := wantLocked(t, func() error { _, err := f.svc.Login(f.ctx, username, "wrong password!!"); return err }())
	if want := f.clock.Add(15 * time.Minute); !le.Until.Equal(want) {
		t.Fatalf("locked until %v, want %v", le.Until, want)
	}
	// Correct password is refused while locked, and doesn't extend the lock.
	wantLocked(t, func() error { _, err := f.svc.Login(f.ctx, username, testPassword); return err }())

	f.advance(15*time.Minute + time.Second)
	// After expiry a single failure starts a fresh count instead of re-locking.
	_, err = f.svc.Login(f.ctx, username, "wrong password!!")
	wantErr(t, err, ErrInvalidCredentials)
	f.advance(30 * time.Second)
	if _, err := f.login(username, secret); err != nil {
		t.Fatalf("login after lock expiry: %v", err)
	}
}

func TestWrongTOTPCountsTowardsLockout(t *testing.T) {
	f := newFixture(t)
	username := f.createUser()
	secret, _ := f.enroll(username)
	f.advance(30 * time.Second)

	lr, err := f.svc.Login(f.ctx, username, testPassword)
	if err != nil {
		t.Fatal(err)
	}
	for i := 1; i <= 4; i++ {
		_, err := f.svc.VerifyMFA(f.ctx, lr.MFAToken, "000000", f.meta)
		wantErr(t, err, ErrInvalidCredentials)
	}
	// A correct password in between must not reset the counter.
	if _, err := f.svc.Login(f.ctx, username, testPassword); err != nil {
		t.Fatal(err)
	}
	_, err = f.svc.VerifyMFA(f.ctx, lr.MFAToken, "000000", f.meta)
	wantLocked(t, err)
	_, err = f.svc.VerifyMFA(f.ctx, lr.MFAToken, f.code(secret), f.meta)
	wantLocked(t, err) // even the right code is refused while locked
}

func TestRefreshRotationAndReuseDetection(t *testing.T) {
	f := newFixture(t)
	username := f.createUser()
	_, sess := f.enroll(username)

	f.advance(time.Minute)
	r1, err := f.svc.Refresh(f.ctx, sess.RefreshToken, f.meta)
	if err != nil {
		t.Fatal(err)
	}
	if r1.RefreshToken == sess.RefreshToken || r1.AccessToken == "" {
		t.Fatal("refresh did not rotate")
	}
	r2, err := f.svc.Refresh(f.ctx, r1.RefreshToken, f.meta)
	if err != nil {
		t.Fatal(err)
	}

	// Replaying the first (already rotated) token = theft signal: the whole family dies.
	_, err = f.svc.Refresh(f.ctx, sess.RefreshToken, f.meta)
	wantErr(t, err, ErrRefreshReused)
	_, err = f.svc.Refresh(f.ctx, r2.RefreshToken, f.meta)
	wantErr(t, err, ErrRefreshReused) // the legitimate latest token is revoked too

	var live int
	if err := f.pool.QueryRow(f.ctx, `SELECT count(*) FROM refresh_token rt JOIN app_user u ON u.id = rt.user_id
		WHERE u.username = $1 AND rt.revoked_at IS NULL`, username).Scan(&live); err != nil {
		t.Fatal(err)
	}
	if live != 0 {
		t.Fatalf("%d refresh tokens still live after reuse", live)
	}
}

func TestReuseRevokesOnlyThatSession(t *testing.T) {
	f := newFixture(t)
	username := f.createUser()
	secret, desktop := f.enroll(username)
	f.advance(30 * time.Second)
	laptop, err := f.login(username, secret)
	if err != nil {
		t.Fatal(err)
	}

	if _, err := f.svc.Refresh(f.ctx, desktop.RefreshToken, f.meta); err != nil {
		t.Fatal(err)
	}
	_, err = f.svc.Refresh(f.ctx, desktop.RefreshToken, f.meta)
	wantErr(t, err, ErrRefreshReused)

	if _, err := f.svc.Refresh(f.ctx, laptop.RefreshToken, f.meta); err != nil {
		t.Fatalf("other session should survive: %v", err)
	}
}

func TestRefreshExpiryLogoutAndGarbage(t *testing.T) {
	f := newFixture(t)
	username := f.createUser()
	secret, sess := f.enroll(username)

	for _, raw := range []string{"", "not-base64!", base64.RawURLEncoding.EncodeToString(make([]byte, 32))} {
		_, err := f.svc.Refresh(f.ctx, raw, f.meta)
		wantErr(t, err, ErrRefreshInvalid)
	}

	f.advance(168*time.Hour + time.Second)
	_, err := f.svc.Refresh(f.ctx, sess.RefreshToken, f.meta)
	wantErr(t, err, ErrRefreshInvalid)

	s2, err := f.login(username, secret)
	if err != nil {
		t.Fatal(err)
	}
	if err := f.svc.Logout(f.ctx, s2.RefreshToken); err != nil {
		t.Fatal(err)
	}
	if _, err := f.svc.Refresh(f.ctx, s2.RefreshToken, f.meta); err == nil {
		t.Fatal("refresh after logout succeeded")
	}
	if err := f.svc.Logout(f.ctx, "unknown"); err != nil {
		t.Fatalf("logout with unknown token: %v", err)
	}
}

func TestRefreshEndsWhenUserDisabled(t *testing.T) {
	f := newFixture(t)
	username := f.createUser()
	_, sess := f.enroll(username)
	if _, err := f.pool.Exec(f.ctx, "UPDATE app_user SET is_active = false WHERE username = $1", username); err != nil {
		t.Fatal(err)
	}
	_, err := f.svc.Refresh(f.ctx, sess.RefreshToken, f.meta)
	wantErr(t, err, ErrRefreshInvalid)
	var revoked bool
	if err := f.pool.QueryRow(f.ctx, `SELECT bool_and(rt.revoked_at IS NOT NULL) FROM refresh_token rt
		JOIN app_user u ON u.id = rt.user_id WHERE u.username = $1`, username).Scan(&revoked); err != nil || !revoked {
		t.Fatalf("family not revoked for disabled user (revoked=%t, err=%v)", revoked, err)
	}
}

func TestReauth(t *testing.T) {
	f := newFixture(t)
	username := f.createUser()
	secret, sess := f.enroll(username)
	f.advance(30 * time.Second)

	_, _, err := f.svc.Reauth(f.ctx, sess.User.ID, "wrong password!!", f.code(secret))
	wantErr(t, err, ErrInvalidCredentials)

	tok, ttl, err := f.svc.Reauth(f.ctx, sess.User.ID, testPassword, f.code(secret))
	if err != nil || ttl != 5*time.Minute {
		t.Fatalf("reauth: %v (ttl %v)", err, ttl)
	}
	if err := f.svc.VerifyReauth(tok, sess.User.ID); err != nil {
		t.Fatalf("verify reauth: %v", err)
	}
	wantErr(t, f.svc.VerifyReauth(tok, "00000000-0000-0000-0000-000000000000"), ErrUnauthenticated)
	wantErr(t, f.svc.VerifyReauth(sess.AccessToken, sess.User.ID), ErrUnauthenticated)
	_, err = f.svc.VerifyAccess(tok)
	wantErr(t, err, ErrUnauthenticated)

	f.advance(5*time.Minute + time.Second)
	wantErr(t, f.svc.VerifyReauth(tok, sess.User.ID), ErrUnauthenticated)
}

func TestTOTPSecretEncryptedAtRest(t *testing.T) {
	f := newFixture(t)
	username := f.createUser()
	secret, _ := f.enroll(username)
	var enc []byte
	if err := f.pool.QueryRow(f.ctx, "SELECT totp_secret_enc FROM app_user WHERE username = $1", username).Scan(&enc); err != nil {
		t.Fatal(err)
	}
	if len(enc) == 0 || string(enc) == secret || containsBytes(enc, []byte(secret)) {
		t.Fatal("TOTP secret stored in clear")
	}
}

func containsBytes(haystack, needle []byte) bool {
	for i := 0; i+len(needle) <= len(haystack); i++ {
		if string(haystack[i:i+len(needle)]) == string(needle) {
			return true
		}
	}
	return false
}
