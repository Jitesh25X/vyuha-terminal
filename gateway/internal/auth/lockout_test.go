package auth

import (
	"errors"
	"sync"
	"testing"
	"time"

	"github.com/pquerna/otp"
)

// countOutcomes fires n attempts concurrently and tallies invalid vs locked results.
func countOutcomes(t *testing.T, n int, try func() error) (invalid, locked int) {
	t.Helper()
	var (
		mu sync.Mutex
		wg sync.WaitGroup
	)
	for i := 0; i < n; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			err := try()
			var le *LockedError
			mu.Lock()
			defer mu.Unlock()
			switch {
			case errors.Is(err, ErrInvalidCredentials):
				invalid++
			case errors.As(err, &le):
				locked++
			default:
				t.Errorf("unexpected result: %v", err)
			}
		}()
	}
	wg.Wait()
	return invalid, locked
}

// evaluatedGuesses is failed_logins: every guess that was actually checked is counted, and guesses
// rejected by the lock are not. Response codes alone can't show the race (late failures report 423 too).
func (f *fixture) evaluatedGuesses(username string) int {
	f.t.Helper()
	var n int
	if err := f.pool.QueryRow(f.ctx, "SELECT failed_logins FROM app_user WHERE username = $1", username).Scan(&n); err != nil {
		f.t.Fatal(err)
	}
	return n
}

// At most MaxFailures guesses may be evaluated per lock window, however many arrive at once.
func TestLockoutHoldsUnderConcurrentPasswordGuesses(t *testing.T) {
	f := newFixture(t)
	username := f.createUser()
	invalid, locked := countOutcomes(t, 20, func() error {
		_, err := f.svc.Login(f.ctx, username, "wrong password!!")
		return err
	})
	if invalid != 4 || locked != 16 {
		t.Fatalf("invalid=%d locked=%d, want 4/16", invalid, locked)
	}
	if n := f.evaluatedGuesses(username); n != 5 {
		t.Fatalf("%d guesses evaluated, want 5", n)
	}
}

func TestLockoutHoldsUnderConcurrentTOTPGuesses(t *testing.T) {
	f := newFixture(t)
	username := f.createUser()
	f.enroll(username)
	f.advance(30 * time.Second)
	lr, err := f.svc.Login(f.ctx, username, testPassword)
	if err != nil {
		t.Fatal(err)
	}
	invalid, locked := countOutcomes(t, 20, func() error {
		_, err := f.svc.VerifyMFA(f.ctx, lr.MFAToken, "000000", f.meta)
		return err
	})
	if invalid != 4 || locked != 16 {
		t.Fatalf("invalid=%d locked=%d, want 4/16", invalid, locked)
	}
	if n := f.evaluatedGuesses(username); n != 5 {
		t.Fatalf("%d TOTP guesses evaluated, want 5", n)
	}
}

// Lockout blocks new logins only: an attacker failing logins must not end the victim's session.
func TestLockoutDoesNotEndExistingSessions(t *testing.T) {
	f := newFixture(t)
	username := f.createUser()
	_, sess := f.enroll(username)
	for i := 0; i < 5; i++ {
		_, _ = f.svc.Login(f.ctx, username, "wrong password!!")
	}
	_, err := f.svc.Login(f.ctx, username, testPassword)
	wantLocked(t, err)
	r, err := f.svc.Refresh(f.ctx, sess.RefreshToken, f.meta)
	if err != nil {
		t.Fatalf("existing session ended by lockout: %v", err)
	}
	if _, err := f.svc.Refresh(f.ctx, r.RefreshToken, f.meta); err != nil {
		t.Fatalf("rotation after lockout: %v", err)
	}
}

func TestInactiveUserLoginIsGeneric(t *testing.T) {
	f := newFixture(t)
	username := f.createUser()
	if _, err := f.pool.Exec(f.ctx, "UPDATE app_user SET is_active = false WHERE username = $1", username); err != nil {
		t.Fatal(err)
	}
	_, err := f.svc.Login(f.ctx, username, testPassword)
	wantErr(t, err, ErrInvalidCredentials)
}

func TestWrongEnrollmentCodesCountTowardsLockout(t *testing.T) {
	f := newFixture(t)
	username := f.createUser()
	lr, err := f.svc.Login(f.ctx, username, testPassword)
	if err != nil {
		t.Fatal(err)
	}
	e, err := f.svc.BeginEnroll(f.ctx, lr.MFAToken)
	if err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 4; i++ {
		_, err := f.svc.ConfirmEnroll(f.ctx, lr.MFAToken, "000000", f.meta)
		wantErr(t, err, ErrInvalidCredentials)
	}
	_, err = f.svc.ConfirmEnroll(f.ctx, lr.MFAToken, "000000", f.meta)
	wantLocked(t, err)
	key, _ := otp.NewKeyFromURL(e.OtpauthURL)
	_, err = f.svc.ConfirmEnroll(f.ctx, lr.MFAToken, f.code(key.Secret()), f.meta)
	wantLocked(t, err)
}

// A secret replaced by a second BeginEnroll can't be confirmed with a code from the first one.
func TestConfirmUsesLatestSecretOnly(t *testing.T) {
	f := newFixture(t)
	username := f.createUser()
	lr, _ := f.svc.Login(f.ctx, username, testPassword)
	first, err := f.svc.BeginEnroll(f.ctx, lr.MFAToken)
	if err != nil {
		t.Fatal(err)
	}
	second, err := f.svc.BeginEnroll(f.ctx, lr.MFAToken)
	if err != nil {
		t.Fatal(err)
	}
	k1, _ := otp.NewKeyFromURL(first.OtpauthURL)
	k2, _ := otp.NewKeyFromURL(second.OtpauthURL)
	_, err = f.svc.ConfirmEnroll(f.ctx, lr.MFAToken, f.code(k1.Secret()), f.meta)
	wantErr(t, err, ErrInvalidCredentials)
	if _, err := f.svc.ConfirmEnroll(f.ctx, lr.MFAToken, f.code(k2.Secret()), f.meta); err != nil {
		t.Fatalf("confirm with the current secret: %v", err)
	}
}

func TestReauthFailuresCountAndSuccessResets(t *testing.T) {
	f := newFixture(t)
	username := f.createUser()
	secret, sess := f.enroll(username)
	f.advance(30 * time.Second)

	for i := 0; i < 3; i++ {
		_, _, err := f.svc.Reauth(f.ctx, sess.User.ID, testPassword, "000000") // wrong TOTP
		wantErr(t, err, ErrInvalidCredentials)
	}
	if _, _, err := f.svc.Reauth(f.ctx, sess.User.ID, testPassword, f.code(secret)); err != nil {
		t.Fatal(err)
	}
	var failures int
	if err := f.pool.QueryRow(f.ctx, "SELECT failed_logins FROM app_user WHERE username = $1", username).Scan(&failures); err != nil || failures != 0 {
		t.Fatalf("failed_logins=%d (err %v), want reset to 0", failures, err)
	}
	f.advance(30 * time.Second)
	for i := 0; i < 4; i++ {
		_, _, err := f.svc.Reauth(f.ctx, sess.User.ID, "wrong password!!", f.code(secret))
		wantErr(t, err, ErrInvalidCredentials)
	}
	_, _, err := f.svc.Reauth(f.ctx, sess.User.ID, "wrong password!!", f.code(secret))
	wantLocked(t, err)
}
