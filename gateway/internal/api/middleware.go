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
	"strings"
	"time"

	"github.com/jiteshraj/vyuha/gateway/internal/auth"
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

// captureClient stores the user agent and peer IP for refresh-token records.
// The peer is the direct connection (Caddy in production); X-Forwarded-For is not trusted yet.
func captureClient(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		meta := auth.ClientMeta{UserAgent: r.UserAgent()}
		if ap, err := netip.ParseAddrPort(r.RemoteAddr); err == nil {
			meta.IP = ap.Addr().Unmap()
		}
		next.ServeHTTP(w, r.WithContext(context.WithValue(r.Context(), clientMetaKey, meta)))
	})
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
