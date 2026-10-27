package api

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"log/slog"
	"net/http"
	"regexp"
	"time"
)

type ctxKey int

const requestIDKey ctxKey = iota

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
				if v == http.ErrAbortHandler {
					panic(v)
				}
				logger.ErrorContext(r.Context(), "handler panic", "panic", v, "path", r.URL.Path, "request_id", RequestID(r.Context()))
				writeProblem(w, http.StatusInternalServerError, "internal", "Internal server error", "")
			}
		}()
		next.ServeHTTP(w, r)
	})
}
