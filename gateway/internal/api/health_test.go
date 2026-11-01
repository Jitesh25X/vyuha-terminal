package api

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/jackc/pgx/v5/pgtype"

	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/config"
)

type fakeDB struct{ err error }

func (f fakeDB) DatabaseNow(context.Context) (pgtype.Timestamptz, error) {
	return pgtype.Timestamptz{Time: time.Now().UTC(), Valid: f.err == nil}, f.err
}

func newTestHandler(db DB) (http.Handler, *bytes.Buffer) {
	var logs bytes.Buffer
	logger := slog.New(slog.NewJSONHandler(&logs, nil))
	return NewHandler(NewServer("0.0.1", config.EnvDev, db, nil, logger), logger), &logs
}

func TestHealth(t *testing.T) {
	tests := []struct {
		name       string
		db         DB
		wantCode   int
		wantStatus HealthStatus
		wantCheck  CheckStatus
	}{
		{name: "postgres up", db: fakeDB{}, wantCode: http.StatusOK, wantStatus: HealthStatusOk, wantCheck: CheckStatusOk},
		{
			name:     "postgres down",
			db:       fakeDB{err: errors.New("dial tcp 10.0.0.5:5432: password authentication failed for user vyuha")},
			wantCode: http.StatusServiceUnavailable, wantStatus: HealthStatusDegraded, wantCheck: CheckStatusFail,
		},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			h, _ := newTestHandler(tt.db)
			rec := httptest.NewRecorder()
			h.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/api/v1/health", nil))

			if rec.Code != tt.wantCode {
				t.Fatalf("status = %d, want %d", rec.Code, tt.wantCode)
			}
			if ct := rec.Header().Get("Content-Type"); ct != "application/json" {
				t.Fatalf("content-type = %q", ct)
			}
			var got Health
			if err := json.Unmarshal(rec.Body.Bytes(), &got); err != nil {
				t.Fatal(err)
			}
			if got.Status != tt.wantStatus || got.Version != "0.0.1" || got.Env != HealthEnvDev {
				t.Fatalf("unexpected body: %+v", got)
			}
			pg, ok := got.Checks["postgres"]
			if !ok || pg.Status != tt.wantCheck || pg.LatencyMs == nil {
				t.Fatalf("unexpected postgres check: %+v", got.Checks)
			}
			if body := rec.Body.String(); strings.Contains(body, "10.0.0.5") || strings.Contains(body, "password") {
				t.Fatalf("driver error leaked to client: %s", body)
			}
		})
	}
}

// Non-public routes are default-deny: without a valid bearer token even unknown paths get 401.
func TestUnauthenticatedRouteIsProblemJSON(t *testing.T) {
	h, _ := newTestHandler(fakeDB{})
	for _, req := range []*http.Request{
		httptest.NewRequest(http.MethodGet, "/api/v1/nope", nil),
		httptest.NewRequest(http.MethodPost, "/api/v1/auth/reauth", strings.NewReader(`{}`)),
		func() *http.Request {
			r := httptest.NewRequest(http.MethodGet, "/api/v1/nope", nil)
			r.Header.Set("Authorization", "Bearer forged.token.value")
			return r
		}(),
	} {
		rec := httptest.NewRecorder()
		h.ServeHTTP(rec, req)
		if rec.Code != http.StatusUnauthorized || rec.Header().Get("Content-Type") != "application/problem+json" {
			t.Fatalf("%s %s: got %d %q", req.Method, req.URL.Path, rec.Code, rec.Header().Get("Content-Type"))
		}
		var p Problem
		if err := json.Unmarshal(rec.Body.Bytes(), &p); err != nil {
			t.Fatal(err)
		}
		if p.Type != problemTypeBase+"auth-unauthenticated" || p.Code == nil || *p.Code != "AUTH_UNAUTHENTICATED" {
			t.Fatalf("unexpected problem: %+v", p)
		}
	}
}

func TestRequestID(t *testing.T) {
	tests := []struct {
		name     string
		incoming string
		wantSame bool
	}{
		{name: "propagates well-formed id", incoming: "abc-123", wantSame: true},
		{name: "replaces missing id", incoming: ""},
		{name: "replaces malformed id", incoming: "bad id\nwith newline"},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			h, _ := newTestHandler(fakeDB{})
			req := httptest.NewRequest(http.MethodGet, "/api/v1/health", nil)
			if tt.incoming != "" {
				req.Header.Set("X-Request-Id", tt.incoming)
			}
			rec := httptest.NewRecorder()
			h.ServeHTTP(rec, req)
			got := rec.Header().Get("X-Request-Id")
			if tt.wantSame && got != tt.incoming {
				t.Fatalf("request id = %q, want %q", got, tt.incoming)
			}
			if !tt.wantSame && (got == tt.incoming || len(got) != 32) {
				t.Fatalf("expected a generated 32-hex id, got %q", got)
			}
		})
	}
}

func TestRequestLogOmitsQueryAndHeaders(t *testing.T) {
	h, logs := newTestHandler(fakeDB{})
	req := httptest.NewRequest(http.MethodGet, "/api/v1/health?request_token=SECRET-QUERY", nil)
	req.Header.Set("Authorization", "Bearer SECRET-HEADER")
	h.ServeHTTP(httptest.NewRecorder(), req)

	out := logs.String()
	if !strings.Contains(out, `"path":"/api/v1/health"`) || !strings.Contains(out, `"status":200`) {
		t.Fatalf("request line missing: %s", out)
	}
	if strings.Contains(out, "SECRET") {
		t.Fatalf("secret leaked into logs: %s", out)
	}
}

func TestPanicBecomesProblem(t *testing.T) {
	var logs bytes.Buffer
	logger := slog.New(slog.NewJSONHandler(&logs, nil))
	h := recoverPanics(logger, http.HandlerFunc(func(http.ResponseWriter, *http.Request) { panic("boom") }))
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/api/v1/x", nil))
	if rec.Code != http.StatusInternalServerError || rec.Header().Get("Content-Type") != "application/problem+json" {
		t.Fatalf("got %d %q", rec.Code, rec.Header().Get("Content-Type"))
	}
	if !strings.Contains(logs.String(), "handler panic") {
		t.Fatalf("panic not logged: %s", logs.String())
	}
}
