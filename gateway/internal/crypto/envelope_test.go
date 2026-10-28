package crypto

import (
	"bytes"
	"crypto/rand"
	"encoding/base64"
	"errors"
	"testing"
)

func newKey(t *testing.T) string {
	t.Helper()
	k := make([]byte, 32)
	if _, err := rand.Read(k); err != nil {
		t.Fatal(err)
	}
	return base64.StdEncoding.EncodeToString(k)
}

func TestEnvelope(t *testing.T) {
	key := newKey(t)
	e, err := NewEnvelope(key)
	if err != nil {
		t.Fatal(err)
	}
	ad := []byte("app_user.totp_secret_enc:user-1")
	sealed, err := e.Seal([]byte("JBSWY3DPEHPK3PXP"), ad)
	if err != nil {
		t.Fatal(err)
	}
	if bytes.Contains(sealed, []byte("JBSWY3DPEHPK3PXP")) {
		t.Fatal("plaintext visible in ciphertext")
	}
	again, _ := e.Seal([]byte("JBSWY3DPEHPK3PXP"), ad)
	if bytes.Equal(sealed, again) {
		t.Fatal("nonce reuse: identical ciphertexts")
	}
	pt, err := e.Open(sealed, ad)
	if err != nil || string(pt) != "JBSWY3DPEHPK3PXP" {
		t.Fatalf("round trip: %q %v", pt, err)
	}

	other, _ := NewEnvelope(newKey(t))
	tampered := append([]byte(nil), sealed...)
	tampered[len(tampered)-1] ^= 1
	tests := map[string]func() ([]byte, error){
		"other user's associated data": func() ([]byte, error) { return e.Open(sealed, []byte("app_user.totp_secret_enc:user-2")) },
		"wrong key":                    func() ([]byte, error) { return other.Open(sealed, ad) },
		"tampered ciphertext":          func() ([]byte, error) { return e.Open(tampered, ad) },
		"truncated":                    func() ([]byte, error) { return e.Open(sealed[:10], ad) },
	}
	for name, open := range tests {
		if _, err := open(); !errors.Is(err, ErrDecrypt) {
			t.Errorf("%s: want ErrDecrypt, got %v", name, err)
		}
	}
}

func TestNewEnvelopeRejectsBadKeys(t *testing.T) {
	for _, k := range []string{"", "not base64!", base64.StdEncoding.EncodeToString(make([]byte, 16))} {
		if _, err := NewEnvelope(k); err == nil {
			t.Errorf("key %q accepted", k)
		}
	}
}
