package auth

import (
	"crypto/ed25519"
	"crypto/rand"
	"encoding/base64"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/golang-jwt/jwt/v5"
	"github.com/pquerna/otp/totp"
)

var t0 = time.Date(2026, 10, 28, 4, 0, 0, 0, time.UTC) // 09:30 IST

func newSigner(t *testing.T) *Signer {
	t.Helper()
	_, priv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	return NewSigner(priv)
}

func TestTokens(t *testing.T) {
	s := newSigner(t)
	access, err := s.issue(audAccess, "user-1", "trader", t0, 15*time.Minute)
	if err != nil {
		t.Fatal(err)
	}
	c, err := s.parse(access, audAccess, t0.Add(time.Minute))
	if err != nil || c.Subject != "user-1" || c.Role != "trader" {
		t.Fatalf("round trip: %+v %v", c, err)
	}

	parts := strings.Split(access, ".")
	sig, _ := base64.RawURLEncoding.DecodeString(parts[2])
	sig[0] ^= 1
	tampered := parts[0] + "." + parts[1] + "." + base64.RawURLEncoding.EncodeToString(sig)

	none := jwt.NewWithClaims(jwt.SigningMethodNone, Claims{RegisteredClaims: jwt.RegisteredClaims{
		Issuer: issuer, Subject: "user-1", Audience: jwt.ClaimStrings{audAccess}, ExpiresAt: jwt.NewNumericDate(t0.Add(time.Hour)),
	}})
	none.Header["kid"] = s.kid
	noneTok, _ := none.SignedString(jwt.UnsafeAllowNoneSignatureType)

	mfa, _ := s.issue(audMFA, "user-1", "", t0, 5*time.Minute)
	otherKey, _ := newSigner(t).issue(audAccess, "user-1", "trader", t0, 15*time.Minute)

	tests := []struct {
		name  string
		token string
		aud   string
		at    time.Time
	}{
		{"expired", access, audAccess, t0.Add(16 * time.Minute)},
		{"tampered signature", tampered, audAccess, t0},
		{"alg none", noneTok, audAccess, t0},
		{"signed by another key", otherKey, audAccess, t0},
		{"mfa token used as access token", mfa, audAccess, t0},
		{"access token used as reauth token", access, audReauth, t0},
		{"garbage", "not.a.jwt", audAccess, t0},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if _, err := s.parse(tt.token, tt.aud, tt.at); !errors.Is(err, ErrUnauthenticated) {
				t.Fatalf("want ErrUnauthenticated, got %v", err)
			}
		})
	}
}

func TestMatchTOTP(t *testing.T) {
	e, err := newTOTP("meera")
	if err != nil {
		t.Fatal(err)
	}
	if !strings.HasPrefix(e.otpauthURL, "otpauth://totp/Vyuha:meera?") || len(e.qrPNG) == 0 {
		t.Fatalf("unexpected enrollment: %q (%d bytes png)", e.otpauthURL, len(e.qrPNG))
	}
	code := func(at time.Time) string {
		c, err := totp.GenerateCodeCustom(e.secret, at, totpOpts)
		if err != nil {
			t.Fatal(err)
		}
		return c
	}
	current := t0.Unix() / totpPeriod
	tests := []struct {
		name     string
		code     string
		wantOK   bool
		wantStep int64
	}{
		{"current step", code(t0), true, current},
		{"previous step (drift)", code(t0.Add(-30 * time.Second)), true, current - 1},
		{"next step (drift)", code(t0.Add(30 * time.Second)), true, current + 1},
		{"two steps old", code(t0.Add(-60 * time.Second)), false, 0},
		{"non-digits", "12a456", false, 0},
		{"wrong length", "12345", false, 0},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			step, ok := matchTOTP(e.secret, tt.code, t0)
			if ok != tt.wantOK || (ok && step != tt.wantStep) {
				t.Fatalf("matchTOTP = (%d, %t), want (%d, %t)", step, ok, tt.wantStep, tt.wantOK)
			}
		})
	}
}

func TestPasswordPolicy(t *testing.T) {
	tests := map[string]bool{
		"short":                  false,
		"exactly-12ch":           true,
		strings.Repeat("a", 128): true,
		strings.Repeat("a", 129): false,
		"ಕನ್ನಡ-ಪಾಸ್ವರ್ಡ್-ಉದ್ದ": true, // counted in runes, not bytes
	}
	for pw, want := range tests {
		if err := ValidatePassword(pw); (err == nil) != want {
			t.Errorf("ValidatePassword(%q) err=%v, want ok=%t", pw, err, want)
		}
	}
	h, err := HashPassword("correct horse battery")
	if err != nil || !strings.HasPrefix(h, "$argon2id$v=19$m=65536,t=2,p=2$") {
		t.Fatalf("hash = %q, %v", h, err)
	}
	if ok, _ := verifyPassword("correct horse battery", h); !ok {
		t.Fatal("correct password rejected")
	}
	if ok, _ := verifyPassword("wrong horse battery", h); ok {
		t.Fatal("wrong password accepted")
	}
}
