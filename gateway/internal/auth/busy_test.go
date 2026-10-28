package auth

import (
	"context"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

func TestBusyWhenCheckSlotsFull(t *testing.T) {
	f := newFixtureWith(t, func(c *Config) { c.MaxConcurrentChecks = 1; c.CheckWait = 20 * time.Millisecond })
	username := f.createUser()
	release, err := f.svc.acquireCheck(f.ctx) // occupy the only slot
	if err != nil {
		t.Fatal(err)
	}
	_, err = f.svc.Login(f.ctx, username, testPassword, f.meta)
	wantErr(t, err, ErrBusy)
	release()
	if _, err := f.svc.Login(f.ctx, username, testPassword, f.meta); err != nil {
		t.Fatalf("after release: %v", err)
	}
}

// A user row held locked (e.g. by a stuck request) makes attempts fail fast with ErrBusy instead
// of holding a pooled connection indefinitely; nothing is counted as a failure.
func TestBusyWhenRowLockHeld(t *testing.T) {
	f := newFixtureWith(t, func(c *Config) { c.RowLockTimeout = 200 * time.Millisecond })
	username := f.createUser()
	tx, err := f.pool.Begin(f.ctx)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := tx.Exec(f.ctx, "SELECT 1 FROM app_user WHERE username = $1 FOR UPDATE", username); err != nil {
		t.Fatal(err)
	}
	start := time.Now()
	_, err = f.svc.Login(f.ctx, username, "wrong password!!", f.meta)
	wantErr(t, err, ErrBusy)
	if d := time.Since(start); d > 2*time.Second {
		t.Fatalf("waited %v; lock_timeout not applied", d)
	}
	_ = tx.Rollback(f.ctx)
	if n := f.evaluatedGuesses(username); n != 0 {
		t.Fatalf("busy attempt was counted as a failure (%d)", n)
	}
}

// M0.8a done check: a burst of logins for one user never holds more DB connections than the cap.
func TestLoginBurstBoundedByCheckCap(t *testing.T) {
	const capN = 2
	f := newFixtureWith(t, func(c *Config) { c.MaxConcurrentChecks = capN; c.CheckWait = 30 * time.Second })
	username := f.createUser()

	var peak atomic.Int32
	stop := make(chan struct{})
	sampled := make(chan struct{})
	go func() {
		defer close(sampled)
		for {
			select {
			case <-stop:
				return
			default:
				if n := f.pool.Stat().AcquiredConns(); n > peak.Load() {
					peak.Store(n)
				}
				time.Sleep(200 * time.Microsecond)
			}
		}
	}()
	var wg sync.WaitGroup
	for i := 0; i < 20; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			_, _ = f.svc.Login(context.Background(), username, "wrong password!!", f.meta)
		}()
	}
	wg.Wait()
	close(stop)
	<-sampled
	if p := peak.Load(); p > capN || p == 0 {
		t.Fatalf("peak acquired connections %d, want 1..%d", p, capN)
	}
	if n := f.evaluatedGuesses(username); n != 5 {
		t.Fatalf("%d guesses evaluated, want 5", n)
	}
}

// Signed-in users re-authenticating (kill-switch release, limit edits) have a reserved slot.
func TestReauthHasReservedSlot(t *testing.T) {
	f := newFixtureWith(t, func(c *Config) { c.MaxConcurrentChecks = 1; c.CheckWait = 20 * time.Millisecond })
	username := f.createUser()
	secret, sess := f.enroll(username)
	f.advance(30 * time.Second)
	release, err := f.svc.acquireCheck(f.ctx) // anonymous logins have filled every slot
	if err != nil {
		t.Fatal(err)
	}
	defer release()
	_, err = f.svc.Login(f.ctx, username, testPassword, f.meta)
	wantErr(t, err, ErrBusy)
	if _, _, err := f.svc.Reauth(f.ctx, sess.User.ID, testPassword, f.code(secret), f.meta); err != nil {
		t.Fatalf("reauth starved by anonymous checks: %v", err)
	}
}

func TestZeroConfigUsesDefaults(t *testing.T) {
	f := newFixtureWith(t, func(c *Config) { c.CheckWait, c.RowLockTimeout, c.MaxConcurrentChecks = 0, -1, 0 })
	d := DefaultConfig()
	if f.svc.cfg.CheckWait != d.CheckWait || f.svc.cfg.RowLockTimeout != d.RowLockTimeout || cap(f.svc.checks) != 1 {
		t.Fatalf("zero values not normalised: %+v cap=%d", f.svc.cfg, cap(f.svc.checks))
	}
}
