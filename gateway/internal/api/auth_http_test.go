package api

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/cookiejar"
	"net/http/httptest"
	"net/url"
	"strings"
	"testing"
	"time"

	"github.com/pquerna/otp"
	"github.com/pquerna/otp/totp"

	"github.com/jiteshraj/vyuha/gateway/internal/auth"
	"github.com/jiteshraj/vyuha/gateway/internal/config"
	vcrypto "github.com/jiteshraj/vyuha/gateway/internal/crypto"
	"github.com/jiteshraj/vyuha/gateway/internal/store"
	"github.com/jiteshraj/vyuha/gateway/internal/testdb"
)

// End-to-end over HTTP with a real cookie jar: login → enroll → confirm → refresh → reuse → reauth → logout.
// Runs when VYUHA_TEST_POSTGRES_URL is set.
func TestAuthFlowHTTP(t *testing.T) {
	ctx := context.Background()
	_, pool := testdb.New(t) // fresh migrated database, dropped after the test
	key := make([]byte, 32)
	_, _ = rand.Read(key)
	env, _ := vcrypto.NewEnvelope(base64.StdEncoding.EncodeToString(key))
	_, priv, _ := ed25519.GenerateKey(rand.Reader)
	clock := time.Now().UTC()
	svc, err := auth.NewService(pool, env, auth.NewSigner(priv), auth.DefaultConfig(), func() time.Time { return clock })
	if err != nil {
		t.Fatal(err)
	}
	var b [4]byte
	_, _ = rand.Read(b[:])
	username := "t_http_" + hex.EncodeToString(b[:])
	const password = "correct horse battery staple"
	if _, err := svc.CreateUser(ctx, auth.SystemActor, username, "HTTP Test", store.UserRoleAnalyst, password); err != nil {
		t.Fatal(err)
	}

	var logs bytes.Buffer
	logger := slog.New(slog.NewJSONHandler(&logs, nil))
	// httptest.NewTLSServer: the refresh cookie is Secure, so the jar only returns it over https.
	srv := httptest.NewTLSServer(NewHandler(NewServer("0.0.1", config.EnvDev, store.New(pool), svc, logger), logger))
	defer srv.Close()
	jar, _ := cookiejar.New(nil)
	client := srv.Client()
	client.Jar = jar

	call := func(method, path, bearer string, body any, want int) map[string]any {
		t.Helper()
		var rdr io.Reader
		if body != nil {
			b, _ := json.Marshal(body)
			rdr = bytes.NewReader(b)
		}
		req, _ := http.NewRequest(method, srv.URL+"/api/v1"+path, rdr)
		if body != nil {
			req.Header.Set("Content-Type", "application/json")
		}
		if bearer != "" {
			req.Header.Set("Authorization", "Bearer "+bearer)
		}
		resp, err := client.Do(req)
		if err != nil {
			t.Fatal(err)
		}
		defer resp.Body.Close()
		raw, _ := io.ReadAll(resp.Body)
		if resp.StatusCode != want {
			t.Fatalf("%s %s: status %d, want %d: %s", method, path, resp.StatusCode, want, raw)
		}
		out := map[string]any{}
		if len(raw) > 0 {
			_ = json.Unmarshal(raw, &out)
		}
		return out
	}
	rtCookie := func() string {
		u, _ := url.Parse(srv.URL + "/api/v1/auth/refresh")
		for _, c := range jar.Cookies(u) {
			if c.Name == "vyuha_rt" {
				return c.Value
			}
		}
		return ""
	}

	// Wrong password → 401 AUTH_INVALID with a generic message.
	p := call("POST", "/auth/login", "", map[string]string{"username": username, "password": "wrong password!!"}, 401)
	if p["code"] != "AUTH_INVALID" {
		t.Fatalf("unexpected problem %v", p)
	}

	lr := call("POST", "/auth/login", "", map[string]string{"username": username, "password": password}, 200)
	if lr["mfa_enrolled"] != false {
		t.Fatalf("expected first login, got %v", lr)
	}
	mfa := lr["mfa_token"].(string)
	call("POST", "/auth/mfa", "", map[string]string{"mfa_token": mfa, "totp": "123456"}, 409)

	enr := call("POST", "/auth/mfa/enroll", "", map[string]string{"mfa_token": mfa}, 200)
	if !strings.HasPrefix(enr["otpauth_url"].(string), "otpauth://totp/") || enr["qr_png_base64"] == "" {
		t.Fatalf("bad enrollment %v", enr)
	}
	k, _ := otp.NewKeyFromURL(enr["otpauth_url"].(string))
	code := func() string {
		c, _ := totp.GenerateCodeCustom(k.Secret(), clock, totp.ValidateOpts{Period: 30, Digits: otp.DigitsSix, Algorithm: otp.AlgorithmSHA1})
		return c
	}
	sess := call("POST", "/auth/mfa/enroll/confirm", "", map[string]string{"mfa_token": mfa, "totp": code()}, 200)
	user := sess["user"].(map[string]any)
	if user["username"] != username || user["role"] != "analyst" || sess["expires_in"].(float64) != 900 {
		t.Fatalf("unexpected session %v", sess)
	}
	access := sess["access_token"].(string)
	first := rtCookie()
	if first == "" {
		t.Fatal("no refresh cookie set")
	}

	// Rotation.
	r1 := call("POST", "/auth/refresh", "", nil, 200)
	if rtCookie() == first || r1["access_token"] == "" {
		t.Fatal("refresh cookie not rotated")
	}
	// Reuse of the first cookie → 401 AUTH_REFRESH_REUSED, and the current cookie is dead too.
	latest := rtCookie()
	u, _ := url.Parse(srv.URL + "/api/v1/auth")
	jar.SetCookies(u, []*http.Cookie{{Name: "vyuha_rt", Value: first, Path: "/api/v1/auth", Secure: true}})
	if p := call("POST", "/auth/refresh", "", nil, 401); p["code"] != "AUTH_REFRESH_REUSED" {
		t.Fatalf("unexpected problem %v", p)
	}
	jar.SetCookies(u, []*http.Cookie{{Name: "vyuha_rt", Value: latest, Path: "/api/v1/auth", Secure: true}})
	call("POST", "/auth/refresh", "", nil, 401)

	// Access token from the revoked session still verifies until expiry (stateless, 15 min); reauth needs it.
	call("POST", "/auth/reauth", "", map[string]string{"password": password, "totp": "000000"}, 401)
	clock = clock.Add(30 * time.Second)
	re := call("POST", "/auth/reauth", access, map[string]string{"password": password, "totp": code()}, 200)
	if re["reauth_token"] == "" || re["expires_in"].(float64) != 300 {
		t.Fatalf("unexpected reauth %v", re)
	}

	// New login, then logout clears the session.
	clock = clock.Add(30 * time.Second)
	lr = call("POST", "/auth/login", "", map[string]string{"username": username, "password": password}, 200)
	call("POST", "/auth/mfa", "", map[string]string{"mfa_token": lr["mfa_token"].(string), "totp": code()}, 200)
	call("POST", "/auth/logout", "", nil, 204)
	if rtCookie() != "" {
		t.Fatal("logout did not clear the cookie")
	}
	call("POST", "/auth/refresh", "", nil, 401)

	if strings.Contains(logs.String(), password) || strings.Contains(logs.String(), first) {
		t.Fatal("password or refresh token leaked into logs")
	}
}
