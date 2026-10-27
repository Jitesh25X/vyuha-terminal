package auth

import (
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha256"
	"crypto/x509"
	"encoding/hex"
	"encoding/pem"
	"errors"
	"fmt"
	"os"
	"time"

	"github.com/golang-jwt/jwt/v5"
)

const issuer = "vyuha"

// Each token kind has its own audience so one can never be used as another (token confusion).
const (
	audAccess = "vyuha-api"
	audMFA    = "vyuha-mfa"
	audReauth = "vyuha-reauth"
)

// Claims are the JWT claims Vyuha issues.
type Claims struct {
	jwt.RegisteredClaims
	Role string `json:"role,omitempty"`
}

// Signer issues and verifies EdDSA (Ed25519) JWTs.
type Signer struct {
	priv ed25519.PrivateKey
	pub  ed25519.PublicKey
	kid  string
}

// NewSigner wraps an Ed25519 private key. The key id is derived from the public key.
func NewSigner(priv ed25519.PrivateKey) *Signer {
	pub, _ := priv.Public().(ed25519.PublicKey)
	sum := sha256.Sum256(pub)
	return &Signer{priv: priv, pub: pub, kid: hex.EncodeToString(sum[:8])}
}

// LoadSigningKey reads a PKCS#8 PEM Ed25519 private key (JWT_SIGNING_KEY_PATH).
func LoadSigningKey(path string) (ed25519.PrivateKey, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("read JWT signing key: %w (create one: make dev-secrets)", err)
	}
	block, _ := pem.Decode(b)
	if block == nil || block.Type != "PRIVATE KEY" {
		return nil, errors.New("JWT signing key: expected a PKCS#8 \"PRIVATE KEY\" PEM block")
	}
	k, err := x509.ParsePKCS8PrivateKey(block.Bytes)
	if err != nil {
		return nil, fmt.Errorf("JWT signing key: %w", err)
	}
	priv, ok := k.(ed25519.PrivateKey)
	if !ok {
		return nil, errors.New("JWT signing key: not an Ed25519 key")
	}
	return priv, nil
}

func (s *Signer) issue(aud, subject, role string, now time.Time, ttl time.Duration) (string, error) {
	var jti [16]byte
	if _, err := rand.Read(jti[:]); err != nil {
		return "", fmt.Errorf("jti: %w", err)
	}
	claims := Claims{
		RegisteredClaims: jwt.RegisteredClaims{
			Issuer:    issuer,
			Subject:   subject,
			Audience:  jwt.ClaimStrings{aud},
			IssuedAt:  jwt.NewNumericDate(now),
			NotBefore: jwt.NewNumericDate(now),
			ExpiresAt: jwt.NewNumericDate(now.Add(ttl)),
			ID:        hex.EncodeToString(jti[:]),
		},
		Role: role,
	}
	t := jwt.NewWithClaims(jwt.SigningMethodEdDSA, claims)
	t.Header["kid"] = s.kid
	signed, err := t.SignedString(s.priv)
	if err != nil {
		return "", fmt.Errorf("sign token: %w", err)
	}
	return signed, nil
}

// parse verifies signature (EdDSA only), issuer, audience and expiry at now.
func (s *Signer) parse(token, aud string, now time.Time) (*Claims, error) {
	claims := &Claims{}
	_, err := jwt.ParseWithClaims(token, claims,
		func(t *jwt.Token) (any, error) {
			if kid, _ := t.Header["kid"].(string); kid != s.kid {
				return nil, errors.New("unknown key id")
			}
			return s.pub, nil
		},
		jwt.WithValidMethods([]string{jwt.SigningMethodEdDSA.Alg()}),
		jwt.WithIssuer(issuer),
		jwt.WithAudience(aud),
		jwt.WithExpirationRequired(),
		jwt.WithIssuedAt(),
		jwt.WithTimeFunc(func() time.Time { return now }),
	)
	if err != nil || claims.Subject == "" {
		return nil, ErrUnauthenticated
	}
	return claims, nil
}
