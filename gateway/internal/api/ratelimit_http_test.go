package api

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"net/netip"
	"strconv"
	"testing"
	"time"

	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/ratelimit"
)

func limitedHandler(trusted []netip.Prefix) http.Handler {
	ok := http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) { w.WriteHeader(http.StatusNoContent) })
	creds := ratelimit.New(credentialBurst, time.Minute, time.Now)
	sessions := ratelimit.New(sessionBurst, time.Minute, time.Now)
	return captureClient(trusted, rateLimitAuth(creds, sessions, ok))
}

func send(h http.Handler, method, path, remote string, xff ...string) *httptest.ResponseRecorder {
	r := httptest.NewRequest(method, path, nil)
	r.RemoteAddr = remote
	for _, v := range xff {
		r.Header.Add("X-Forwarded-For", v)
	}
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, r)
	return rec
}

func TestAuthRateLimit(t *testing.T) {
	h := limitedHandler(nil)
	for i := 0; i < credentialBurst; i++ {
		if rec := send(h, "POST", "/api/v1/auth/login", "198.51.100.7:4000"); rec.Code != http.StatusNoContent {
			t.Fatalf("request %d: %d", i+1, rec.Code)
		}
	}
	rec := send(h, "POST", "/api/v1/auth/mfa", "198.51.100.7:4001") // same IP, other credential route: same bucket
	wantRetry := strconv.Itoa(60 / credentialBurst)                 // one token per (60 s / burst)
	if rec.Code != http.StatusTooManyRequests || rec.Header().Get("Retry-After") != wantRetry {
		t.Fatalf("got %d Retry-After=%q, want 429 / %s", rec.Code, rec.Header().Get("Retry-After"), wantRetry)
	}
	var p Problem
	if err := json.Unmarshal(rec.Body.Bytes(), &p); err != nil || p.Code == nil || *p.Code != "RATE_LIMITED" {
		t.Fatalf("problem: %+v %v", p, err)
	}
	if rec := send(h, "POST", "/api/v1/auth/login", "198.51.100.8:4000"); rec.Code != http.StatusNoContent {
		t.Fatalf("other IP limited: %d", rec.Code)
	}
	if rec := send(h, "POST", "/api/v1/auth/refresh", "198.51.100.7:4000"); rec.Code != http.StatusNoContent {
		t.Fatalf("session bucket must be separate: %d", rec.Code)
	}
	for i := 0; i < 50; i++ {
		if rec := send(h, "GET", "/api/v1/health", "198.51.100.7:4000"); rec.Code != http.StatusNoContent {
			t.Fatalf("non-auth route limited: %d", rec.Code)
		}
	}
}

// Default deployment: Caddy on loopback forwards for many clients; each must get its own bucket.
func TestLoopbackProxyKeysByClient(t *testing.T) {
	loopback := []netip.Prefix{netip.MustParsePrefix("127.0.0.1/32"), netip.MustParsePrefix("::1/128")}
	h := limitedHandler(loopback)
	for i := 0; i < credentialBurst; i++ {
		send(h, "POST", "/api/v1/auth/login", "127.0.0.1:5000", "203.0.113.1")
	}
	if rec := send(h, "POST", "/api/v1/auth/login", "127.0.0.1:5000", "203.0.113.1"); rec.Code != http.StatusTooManyRequests {
		t.Fatalf("client 203.0.113.1 should be limited: %d", rec.Code)
	}
	if rec := send(h, "POST", "/api/v1/auth/login", "127.0.0.1:5000", "203.0.113.2"); rec.Code != http.StatusNoContent {
		t.Fatalf("another client behind the same proxy was limited: %d", rec.Code)
	}
}

func TestIPv6KeyedBySlash64(t *testing.T) {
	h := limitedHandler(nil)
	for i := 0; i < credentialBurst; i++ {
		send(h, "POST", "/api/v1/auth/login", "[2001:db8:1:2::"+strconv.Itoa(i+1)+"]:443")
	}
	if rec := send(h, "POST", "/api/v1/auth/login", "[2001:db8:1:2::ffff]:443"); rec.Code != http.StatusTooManyRequests {
		t.Fatalf("rotating within one /64 bypassed the limit: %d", rec.Code)
	}
	if rec := send(h, "POST", "/api/v1/auth/login", "[2001:db8:1:3::1]:443"); rec.Code != http.StatusNoContent {
		t.Fatalf("different /64 limited: %d", rec.Code)
	}
}

// Reauth is not in the per-IP bucket (it is limited per signed-in user in the handler).
func TestReauthNotInIPBucket(t *testing.T) {
	h := limitedHandler(nil)
	for i := 0; i < credentialBurst; i++ {
		send(h, "POST", "/api/v1/auth/login", "198.51.100.7:4000")
	}
	if rec := send(h, "POST", "/api/v1/auth/reauth", "198.51.100.7:4000"); rec.Code != http.StatusNoContent {
		t.Fatalf("reauth blocked by anonymous logins from the same IP: %d", rec.Code)
	}
}

// A client can't dodge the limit by sending its own X-Forwarded-For.
func TestForgedForwardedForIgnored(t *testing.T) {
	h := limitedHandler(nil)
	for i := 0; i < credentialBurst; i++ {
		send(h, "POST", "/api/v1/auth/login", "198.51.100.7:4000", "203.0.113.1")
	}
	if rec := send(h, "POST", "/api/v1/auth/login", "198.51.100.7:4000", "203.0.113.99"); rec.Code != http.StatusTooManyRequests {
		t.Fatalf("forged XFF bypassed the limit: %d", rec.Code)
	}
}

func TestClientIP(t *testing.T) {
	caddy := []netip.Prefix{netip.MustParsePrefix("172.18.0.0/16")}
	tests := []struct {
		name, remote string
		xff          []string
		trusted      []netip.Prefix
		want         string
	}{
		{"direct client", "198.51.100.7:1", nil, nil, "198.51.100.7"},
		{"untrusted peer's XFF ignored", "198.51.100.7:1", []string{"203.0.113.1"}, caddy, "198.51.100.7"},
		{"behind trusted proxy", "172.18.0.5:1", []string{"203.0.113.1"}, caddy, "203.0.113.1"},
		{"right-most untrusted hop wins", "172.18.0.5:1", []string{"10.9.9.9, 203.0.113.1"}, caddy, "203.0.113.1"},
		{"skips trusted hops", "172.18.0.5:1", []string{"203.0.113.1, 172.18.0.9"}, caddy, "203.0.113.1"},
		{"multiple headers", "172.18.0.5:1", []string{"203.0.113.1", "172.18.0.9"}, caddy, "203.0.113.1"},
		{"malformed chain falls back to proxy", "172.18.0.5:1", []string{"not-an-ip"}, caddy, "172.18.0.5"},
		{"trusted proxy without XFF", "172.18.0.5:1", nil, caddy, "172.18.0.5"},
		{"ipv4-mapped peer", "[::ffff:198.51.100.7]:1", nil, nil, "198.51.100.7"},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			r := httptest.NewRequest("GET", "/", nil)
			r.RemoteAddr = tt.remote
			for _, v := range tt.xff {
				r.Header.Add("X-Forwarded-For", v)
			}
			if got := clientIP(r, tt.trusted).String(); got != tt.want {
				t.Fatalf("clientIP = %s, want %s", got, tt.want)
			}
		})
	}
}
