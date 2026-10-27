// Package crypto encrypts secrets at rest with AES-256-GCM under VYUHA_MASTER_KEY.
package crypto

import (
	"crypto/aes"
	"crypto/cipher"
	"crypto/rand"
	"encoding/base64"
	"errors"
	"fmt"
)

// Envelope seals and opens secrets. Ciphertext layout: nonce(12) || ciphertext || tag(16).
// The associated data binds a ciphertext to its row and column, so it cannot be moved to another user.
type Envelope struct{ aead cipher.AEAD }

// ErrDecrypt is returned for any failure to open a ciphertext; details are deliberately not exposed.
var ErrDecrypt = errors.New("crypto: decryption failed")

// NewEnvelope builds an envelope from a base64-encoded 32-byte key (VYUHA_MASTER_KEY).
func NewEnvelope(masterKeyB64 string) (*Envelope, error) {
	key, err := base64.StdEncoding.DecodeString(masterKeyB64)
	if err != nil || len(key) != 32 {
		return nil, errors.New("crypto: VYUHA_MASTER_KEY must be 32 bytes, base64-encoded (vyuhactl secrets init)")
	}
	block, err := aes.NewCipher(key)
	if err != nil {
		return nil, fmt.Errorf("crypto: %w", err)
	}
	aead, err := cipher.NewGCM(block)
	if err != nil {
		return nil, fmt.Errorf("crypto: %w", err)
	}
	return &Envelope{aead: aead}, nil
}

// Seal encrypts plaintext bound to associatedData (e.g. "app_user.totp_secret_enc:<user id>").
func (e *Envelope) Seal(plaintext, associatedData []byte) ([]byte, error) {
	nonce := make([]byte, e.aead.NonceSize())
	if _, err := rand.Read(nonce); err != nil {
		return nil, fmt.Errorf("crypto: nonce: %w", err)
	}
	return e.aead.Seal(nonce, nonce, plaintext, associatedData), nil
}

// Open decrypts a Seal output; it fails if the key, ciphertext or associated data differ.
func (e *Envelope) Open(sealed, associatedData []byte) ([]byte, error) {
	n := e.aead.NonceSize()
	if len(sealed) < n+e.aead.Overhead() {
		return nil, ErrDecrypt
	}
	pt, err := e.aead.Open(nil, sealed[:n], sealed[n:], associatedData)
	if err != nil {
		return nil, ErrDecrypt
	}
	return pt, nil
}
