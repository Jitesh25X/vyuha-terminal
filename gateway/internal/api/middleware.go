package api

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"errors"
	"log/slog"
	"net/http"
	"net/netip"
	"regexp"
	"strconv"
	"strings"
	"time"

	"github.com/jiteshraj/vyuha/gateway/internal/auth"
	"github.com/jiteshraj/vyuha/gateway/internal/ratelimit"
)

type ctxKey int

const (
	requestIDKey ctxKey = iota
	clientMetaKey
	claimsKey
)

// publicRoutes need no access token; every other route is authenticated (default deny).
var publicRoutes = map[string]bool{
	"GET " + BaseURL + "/health":                   true,
	"POST " + BaseURL + "/auth/login":              true,
	"POST " + BaseURL + "/auth/mfa":                true,
	"POST " + BaseURL + "/auth/mfa/enroll":         true,
	"POST " + BaseURL + "/auth/mfa/enroll/confirm": true,
	"POST " + BaseURL + "/auth/refresh":            true,
	"POST " + BaseURL + "/auth/logout":             true,
}

// ClaimsFrom returns the authenticated user's access-token claims, or nil on public routes.
func ClaimsFrom(ctx context.Context) *auth.Claims {
	c, _ := ctx.Value(claimsKey).(*auth.Claims)
	return c
}

// authenticate requires a valid access JWT (Authorization: Bearer) on non-public routes.
func authenticate(verify func(string) (*auth.Claims, error), next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if publicRoutes[r.Method+" "+r.URL.Path] {
			next.ServeHTTP(w, r)
			return
		}
		tok, ok := strings.CutPrefix(r.Header.Get("Authorization"), "Bearer ")
		var claims *auth.Claims
		if ok && verify != nil {
			if c, err := verify(tok); err == nil {
				claims = c
			}
		}
		if claims == nil {
			w.Header().Set("WWW-Authenticate", `Bearer realm="vyuha"`)
			writeProblem(w, http.StatusUnauthorized, "AUTH_UNAUTHENTICATED", "Authentication required", "")
			return
		}
		next.ServeHTTP(w, r.WithContext(context.WithValue(r.Context(), claimsKey, claims)))
	})
}

// captureClient stores the user agent and client IP (for rate limits, refresh tokens and audit).
// X-Forwarded-For is believed only when the direct peer is a trusted proxy (e.g. Caddy); then the
// client is the right-most address that is not itself a trusted proxy.
func captureClient(trusted []netip.Prefix, next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		meta := auth.ClientMeta{UserAgent: r.UserAgent()}
		meta.IP = clientIP(r, trusted)
		next.ServeHTTP(w, r.WithContext(context.WithValue(r.Context(), clientMetaKey, meta)))
	})
}

func clientIP(r *http.Request, trusted []netip.Prefix) netip.Addr {
	ap, err := netip.ParseAddrPort(r.RemoteAddr)
	if err != nil {
		return netip.Addr{}
	}
	peer := ap.Addr().Unmap()
	if !isTrusted(peer, trusted) {
		return peer
	}
	hops := strings.Split(strings.Join(r.Header.Values("X-Forwarded-For"), ","), ",")
	for i := len(hops) - 1; i >= 0; i-- {
		addr, err := netip.ParseAddr(strings.TrimSpace(hops[i]))
		if err != nil {
			return peer // malformed chain: fall back to the proxy itself
		}
		if addr = addr.Unmap(); !isTrusted(addr, trusted) {
			return addr
		}
	}
	return peer
}

func isTrusted(a netip.Addr, trusted []netip.Prefix) bool {
	for _, p := range trusted {
		if p.Contains(a) {
			return true
		}
	}
	return false
}

// Per-client limits on the unauthenticated auth endpoints (ASVS 2.2.1; bounds audit growth and argon2
// load). Per-account guessing is capped separately by the 5-failure lockout, so the per-IP ceiling can
// allow a desk behind one NAT to log in together at market open.
const (
	credentialBurst = 30 // login, mfa, enroll, confirm: 30 per minute per client
	sessionBurst    = 30 // refresh, logout: 30 per minute per client
	reauthBurst     = 5  // reauth: 5 per minute per signed-in user (keyed by token subject, not IP)
)

var (
	credentialRoutes = map[string]bool{
		"POST " + BaseURL + "/auth/login": true, "POST " + BaseURL + "/auth/mfa": true,
		"POST " + BaseURL + "/auth/mfa/enroll": true, "POST " + BaseURL + "/auth/mfa/enroll/confirm": true,
	}
	sessionRoutes = map[string]bool{"POST " + BaseURL + "/auth/refresh": true, "POST " + BaseURL + "/auth/logout": true}
)

// rateLimitAuth applies the per-IP limits; it must run after captureClient.
func rateLimitAuth(creds, sessions *ratelimit.Limiter, next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		route := r.Method + " " + r.URL.Path
		var l *ratelimit.Limiter
		switch {
		case credentialRoutes[route]:
			l = creds
		case sessionRoutes[route]:
			l = sessions
		default:
			next.ServeHTTP(w, r)
			return
		}
		if ok, wait := l.Allow(clientKey(clientMeta(r.Context()).IP)); !ok {
			writeRateLimited(w, wait)
			return
		}
		next.ServeHTTP(w, r)
	})
}

// clientKey buckets IPv4 clients by address and IPv6 clients by /64, since one IPv6 client
// usually controls a whole /64 and could otherwise rotate addresses to dodge the limit.
func clientKey(ip netip.Addr) string {
	switch {
	case !ip.IsValid():
		return "unknown"
	case ip.Is6():
		return netip.PrefixFrom(ip, 64).Masked().String()
	}
	return ip.String()
}

func writeRateLimited(w http.ResponseWriter, wait time.Duration) {
	w.Header().Set("Retry-After", strconv.Itoa(retryAfterSeconds(wait)))
	writeProblem(w, http.StatusTooManyRequests, "RATE_LIMITED", "Too many requests", "Slow down and retry later.")
}

func retryAfterSeconds(d time.Duration) int {
	s := int((d + time.Second - 1) / time.Second)
	if s < 1 {
		return 1
	}
	return s
}

func clientMeta(ctx context.Context) auth.ClientMeta {
	m, _ := ctx.Value(clientMetaKey).(auth.ClientMeta)
	return m
}

var validRequestID = regexp.MustCompile(`^[A-Za-z0-9._-]{1,64}$`)

// RequestID returns the request id stored by the requestID middleware, or "".
func RequestID(ctx context.Context) string {
	id, _ := ctx.Value(requestIDKey).(string)
	return id
}

// requestID propagates a well-formed X-Request-Id or generates one, and echoes it in the response.
func requestID(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		id := r.Header.Get("X-Request-Id")
		if !validRequestID.MatchString(id) {
			var b [16]byte
			_, _ = rand.Read(b[:]) // crypto/rand.Read never fails on supported platforms
			id = hex.EncodeToString(b[:])
		}
		w.Header().Set("X-Request-Id", id)
		next.ServeHTTP(w, r.WithContext(context.WithValue(r.Context(), requestIDKey, id)))
	})
}

type statusRecorder struct {
	http.ResponseWriter
	status int
}

func (s *statusRecorder) WriteHeader(code int) {
	if s.status == 0 {
		s.status = code
	}
	s.ResponseWriter.WriteHeader(code)
}

func (s *statusRecorder) Write(b []byte) (int, error) {
	if s.status == 0 {
		s.status = http.StatusOK
	}
	return s.ResponseWriter.Write(b)
}

func (s *statusRecorder) Unwrap() http.ResponseWriter { return s.ResponseWriter }

// logRequests logs one line per request. It never logs bodies, headers or query strings:
// tokens travel in Authorization headers, cookies and broker-callback query parameters.
func logRequests(logger *slog.Logger, next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		rec := &statusRecorder{ResponseWriter: w}
		next.ServeHTTP(rec, r)
		if rec.status == 0 {
			rec.status = http.StatusOK
		}
		logger.LogAttrs(r.Context(), slog.LevelInfo, "http request",
			slog.String("method", r.Method),
			slog.String("path", r.URL.Path),
			slog.Int("status", rec.status),
			slog.Int64("duration_us", time.Since(start).Microseconds()),
			slog.String("request_id", RequestID(r.Context())),
		)
	})
}

// recoverPanics turns a handler panic into a 500 problem response.
func recoverPanics(logger *slog.Logger, next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		defer func() {
			if v := recover(); v != nil {
				if err, ok := v.(error); ok && errors.Is(err, http.ErrAbortHandler) {
					panic(v)
				}
				logger.ErrorContext(r.Context(), "handler panic", "panic", v, "path", r.URL.Path, "request_id", RequestID(r.Context()))
				writeProblem(w, http.StatusInternalServerError, "INTERNAL", "Internal server error", "")
			}
		}()
		next.ServeHTTP(w, r)
	})
}
