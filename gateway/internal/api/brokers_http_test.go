package api

import (
	"bytes"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"testing"
	"time"

	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/brokers"
	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/config"
)

// The callbacks are reachable without a bearer token, always redirect to the terminal, never cache,
// and never log their (one-time token) query strings.
func TestBrokerCallbackRedirects(t *testing.T) {
	var logs bytes.Buffer
	logger := slog.New(slog.NewJSONHandler(&logs, nil))
	svc, err := brokers.NewService(nil, nil, nil, brokers.DefaultConfig("https://desk.example"), logger, time.Now, nil)
	if err != nil {
		t.Fatal(err)
	}
	h := NewHandler(NewServer("0.0.1", config.EnvDev, fakeDB{}, nil, logger).WithBrokers(svc), logger)

	for _, path := range []string{
		"/api/v1/brokers/callback/zerodha?request_token=SECRET-RT&status=success&state=forged",
		"/api/v1/brokers/callback/upstox?code=SECRET-CODE&state=forged",
	} {
		rec := httptest.NewRecorder()
		h.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, path, nil))
		if rec.Code != http.StatusFound {
			t.Fatalf("%s: status %d", path, rec.Code)
		}
		loc, err := url.Parse(rec.Header().Get("Location"))
		if err != nil || loc.Path != "/terminal" || loc.Query().Get("broker_login") != "error" || loc.Query().Get("code") != brokers.CodeStateInvalid {
			t.Fatalf("%s: Location %q", path, rec.Header().Get("Location"))
		}
		if strings.Contains(rec.Header().Get("Location"), "SECRET") {
			t.Fatal("redirect carries the broker token")
		}
		if rec.Header().Get("Cache-Control") != "no-store" || rec.Header().Get("Referrer-Policy") != "no-referrer" {
			t.Fatalf("headers %v", rec.Header())
		}
	}
	if strings.Contains(logs.String(), "SECRET") {
		t.Fatalf("logs carry a broker token: %s", logs.String())
	}

	// The login URL route is authenticated.
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/api/v1/brokers/0b6f8d4e-2c1a-4f5e-9a7b-3c2d1e0f9a8b/login-url", nil))
	if rec.Code != http.StatusUnauthorized {
		t.Fatalf("login-url without token: %d", rec.Code)
	}
}
