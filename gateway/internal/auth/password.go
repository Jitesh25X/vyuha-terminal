package auth

import (
	"fmt"
	"unicode/utf8"

	"github.com/alexedwards/argon2id"
)

// Password policy (ASVS 2.1.1/2.1.2): 12–128 characters, no composition rules.
const (
	MinPasswordLen = 12
	MaxPasswordLen = 128
)

// argonParams: 64 MiB, 2 passes, 2 lanes (stronger than the OWASP 46 MiB / t=1 floor; ~100 ms per hash).
var argonParams = &argon2id.Params{Memory: 64 * 1024, Iterations: 2, Parallelism: 2, SaltLength: 16, KeyLength: 32}

// ValidatePassword enforces the length policy.
func ValidatePassword(pw string) error {
	if n := utf8.RuneCountInString(pw); n < MinPasswordLen || n > MaxPasswordLen {
		return fmt.Errorf("%w: password must be %d–%d characters", ErrInvalidInput, MinPasswordLen, MaxPasswordLen)
	}
	return nil
}

// HashPassword returns an argon2id PHC string.
func HashPassword(pw string) (string, error) {
	if err := ValidatePassword(pw); err != nil {
		return "", err
	}
	h, err := argon2id.CreateHash(pw, argonParams)
	if err != nil {
		return "", fmt.Errorf("hash password: %w", err)
	}
	return h, nil
}

// verifyPassword reports whether pw matches hash. Over-long input is rejected without hashing.
func verifyPassword(pw, hash string) (bool, error) {
	if utf8.RuneCountInString(pw) > MaxPasswordLen {
		return false, nil
	}
	ok, err := argon2id.ComparePasswordAndHash(pw, hash)
	if err != nil {
		return false, fmt.Errorf("verify password: %w", err)
	}
	return ok, nil
}
