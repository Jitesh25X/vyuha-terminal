package auth

import (
	"strings"
	"testing"
	"time"

	"github.com/jiteshraj/vyuha/gateway/internal/audit"
)

// Every auth mutation leaves an audit row in order, the chain verifies, and nothing secret is stored.
func TestAuthEventsAreAudited(t *testing.T) {
	f := newFixture(t)
	username := f.createUser()                                                   // user.create
	_, _ = f.svc.Login(f.ctx, "t_ghost_user", testPassword, f.meta)              // auth.login.failure (unknown_user)
	_, _ = f.svc.Login(f.ctx, "correcthorsebatterystaple", testPassword, f.meta) // a password in the username box
	secret, sess := f.enroll(username)                                           // enroll_begin, enrolled
	f.advance(30 * time.Second)
	_, _ = f.svc.Login(f.ctx, username, "wrong password!!", f.meta) // auth.login.failure (password)
	s2, err := f.login(username, secret)                            // auth.login.success
	if err != nil {
		t.Fatal(err)
	}
	f.advance(30 * time.Second)
	if _, _, err := f.svc.Reauth(f.ctx, sess.User.ID, testPassword, f.code(secret), f.meta); err != nil { // auth.reauth.success
		t.Fatal(err)
	}
	r1, _ := f.svc.Refresh(f.ctx, sess.RefreshToken, f.meta)             // routine rotation: not audited
	_, _ = f.svc.Refresh(f.ctx, sess.RefreshToken, f.meta)               // auth.refresh.reuse
	if err := f.svc.Logout(f.ctx, s2.RefreshToken, f.meta); err != nil { // auth.logout
		t.Fatal(err)
	}
	_ = f.svc.Logout(f.ctx, s2.RefreshToken, f.meta) // already revoked: no second event

	rows, err := f.pool.Query(f.ctx, `SELECT action, coalesce(payload->>'reason', payload->>'step', ''), coalesce(host(ip), '') FROM audit_event ORDER BY id`)
	if err != nil {
		t.Fatal(err)
	}
	var got []string
	for rows.Next() {
		var action, detail, ip string
		if err := rows.Scan(&action, &detail, &ip); err != nil {
			t.Fatal(err)
		}
		if action != "user.create" && ip != "127.0.0.1" {
			t.Errorf("%s: ip = %q, want 127.0.0.1", action, ip)
		}
		got = append(got, action+":"+detail)
	}
	want := []string{
		"user.create:",
		"auth.login.failure:unknown_user",
		"auth.login.failure:unknown_user",
		"auth.mfa.enroll_begin:",
		"auth.mfa.enrolled:enroll_confirm",
		"auth.login.failure:password",
		"auth.login.success:totp",
		"auth.reauth.success:reauth",
		"auth.refresh.reuse:",
		"auth.logout:",
	}
	if strings.Join(got, "\n") != strings.Join(want, "\n") {
		t.Fatalf("audit actions:\n%s\nwant:\n%s", strings.Join(got, "\n"), strings.Join(want, "\n"))
	}

	res, err := audit.Verify(f.ctx, f.pool)
	if err != nil || !res.OK || res.RowsChecked != int64(len(want)) {
		t.Fatalf("verify: %+v %v", res, err)
	}

	var dump string
	if err := f.pool.QueryRow(f.ctx, `SELECT string_agg(payload::text || coalesce(entity_id, ''), ' ') FROM audit_event`).Scan(&dump); err != nil {
		t.Fatal(err)
	}
	for _, secretText := range []string{testPassword, "correcthorsebatterystaple", "t_ghost_user", secret, sess.RefreshToken, r1.RefreshToken, s2.RefreshToken, sess.AccessToken} {
		if secretText != "" && strings.Contains(dump, secretText) {
			t.Fatalf("secret material in audit log: %q", secretText)
		}
	}
}
