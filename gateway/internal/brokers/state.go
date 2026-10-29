package brokers

import (
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"sync"
	"time"
)

// ErrBadState is any state that is malformed, forged, expired, replayed or for another broker.
var ErrBadState = errors.New("brokers: invalid login state")

// loginState travels through the broker's login page and back. The callback has no bearer token
// (it is a top-level redirect), so the signed state is what identifies the user who started it.
type loginState struct {
	Account string `json:"a"`
	User    string `json:"u"`
	Broker  string `json:"b"`
	Expires int64  `json:"e"` // unix seconds
	Nonce   string `json:"n"`
}

// stateSigner issues HMAC-SHA256 states and accepts each one once. The key lives in memory only:
// a gateway restart invalidates logins in progress (the user clicks login again).
type stateSigner struct {
	key []byte
	ttl time.Duration
	now func() time.Time

	mu   sync.Mutex
	used map[string]int64 // nonce → expiry, pruned as they lapse
}

func newStateSigner(ttl time.Duration, now func() time.Time) (*stateSigner, error) {
	key := make([]byte, 32)
	if _, err := rand.Read(key); err != nil {
		return nil, fmt.Errorf("brokers: state key: %w", err)
	}
	return &stateSigner{key: key, ttl: ttl, now: now, used: map[string]int64{}}, nil
}

func (s *stateSigner) issue(account, user, broker string) (string, error) {
	var n [16]byte
	if _, err := rand.Read(n[:]); err != nil {
		return "", fmt.Errorf("brokers: state nonce: %w", err)
	}
	body, err := json.Marshal(loginState{
		Account: account, User: user, Broker: broker,
		Expires: s.now().Add(s.ttl).Unix(), Nonce: hex.EncodeToString(n[:]),
	})
	if err != nil {
		return "", fmt.Errorf("brokers: state: %w", err)
	}
	payload := base64.RawURLEncoding.EncodeToString(body)
	return payload + "." + base64.RawURLEncoding.EncodeToString(s.mac(payload)), nil
}

// consume verifies a state for broker and marks it used.
func (s *stateSigner) consume(raw, broker string) (loginState, error) {
	payload, sig, ok := strings.Cut(raw, ".")
	if !ok {
		return loginState{}, ErrBadState
	}
	got, err := base64.RawURLEncoding.DecodeString(sig)
	if err != nil || !hmac.Equal(got, s.mac(payload)) {
		return loginState{}, ErrBadState
	}
	body, err := base64.RawURLEncoding.DecodeString(payload)
	if err != nil {
		return loginState{}, ErrBadState
	}
	var st loginState
	if err := json.Unmarshal(body, &st); err != nil || st.Nonce == "" || st.Broker != broker {
		return loginState{}, ErrBadState
	}
	now := s.now().Unix()
	if now >= st.Expires {
		return loginState{}, ErrBadState
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	for n, exp := range s.used {
		if now >= exp {
			delete(s.used, n)
		}
	}
	if _, replay := s.used[st.Nonce]; replay {
		return loginState{}, ErrBadState
	}
	s.used[st.Nonce] = st.Expires
	return st, nil
}

func (s *stateSigner) mac(payload string) []byte {
	m := hmac.New(sha256.New, s.key)
	m.Write([]byte("vyuha broker login v1\x00" + payload))
	return m.Sum(nil)
}
