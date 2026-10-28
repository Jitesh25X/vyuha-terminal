package auth

import (
	"bytes"
	"crypto/subtle"
	"fmt"
	"image/png"
	"time"

	"github.com/pquerna/otp"
	"github.com/pquerna/otp/totp"
)

// RFC 6238 parameters compatible with common authenticator apps; ±1 step tolerates clock drift.
const (
	totpIssuer = "Vyuha"
	totpPeriod = 30
	totpSkew   = 1
)

var totpOpts = totp.ValidateOpts{Period: totpPeriod, Digits: otp.DigitsSix, Algorithm: otp.AlgorithmSHA1}

type enrollment struct {
	secret     string
	otpauthURL string
	qrPNG      []byte
}

func newTOTP(username string) (enrollment, error) {
	key, err := totp.Generate(totp.GenerateOpts{
		Issuer: totpIssuer, AccountName: username, Period: totpPeriod,
		Digits: otp.DigitsSix, Algorithm: otp.AlgorithmSHA1, SecretSize: 20,
	})
	if err != nil {
		return enrollment{}, fmt.Errorf("generate totp: %w", err)
	}
	img, err := key.Image(256, 256)
	if err != nil {
		return enrollment{}, fmt.Errorf("totp qr: %w", err)
	}
	var buf bytes.Buffer
	if err := png.Encode(&buf, img); err != nil {
		return enrollment{}, fmt.Errorf("totp qr png: %w", err)
	}
	return enrollment{secret: key.Secret(), otpauthURL: key.URL(), qrPNG: buf.Bytes()}, nil
}

// matchTOTP returns the time step whose code equals code (within ±totpSkew steps of now).
// The caller must still claim the step once (ClaimTOTPStep) to block replays.
func matchTOTP(secret, code string, now time.Time) (int64, bool) {
	if len(code) != 6 {
		return 0, false
	}
	for _, c := range code {
		if c < '0' || c > '9' {
			return 0, false
		}
	}
	current := now.Unix() / totpPeriod
	for d := int64(-totpSkew); d <= totpSkew; d++ {
		step := current + d
		want, err := totp.GenerateCodeCustom(secret, time.Unix(step*totpPeriod, 0).UTC(), totpOpts)
		if err != nil {
			return 0, false
		}
		if subtle.ConstantTimeCompare([]byte(want), []byte(code)) == 1 {
			return step, true
		}
	}
	return 0, false
}
