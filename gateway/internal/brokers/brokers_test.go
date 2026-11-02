package brokers

import (
	"context"
	"errors"
	"net/http"
	"net/http/httptest"
	"net/netip"
	"strings"
	"testing"
	"time"
)

func TestStateIsSignedSingleUseAndExpires(t *testing.T) {
	now := time.Date(2026, 10, 29, 3, 0, 0, 0, time.UTC)
	clock := func() time.Time { return now }
	s, err := newStateSigner(10*time.Minute, clock)
	if err != nil {
		t.Fatal(err)
	}
	raw, err := s.issue("acct-1", "user-1", Zerodha)
	if err != nil {
		t.Fatal(err)
	}
	other, _ := newStateSigner(10*time.Minute, clock)
	payload, sig, _ := strings.Cut(raw, ".")
	tests := []struct {
		name   string
		signer *stateSigner
		raw    string
		broker string
	}{
		{"other key", other, raw, Zerodha},
		{"wrong broker", s, raw, Upstox},
		{"tampered payload", s, payload + "x." + sig, Zerodha},
		{"no signature", s, payload, Zerodha},
		{"empty", s, "", Zerodha},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if _, err := tt.signer.consume(tt.raw, tt.broker); !errors.Is(err, ErrBadState) {
				t.Fatalf("err = %v, want ErrBadState", err)
			}
		})
	}
	st, err := s.consume(raw, Zerodha)
	if err != nil || st.Account != "acct-1" || st.User != "user-1" {
		t.Fatalf("consume = %+v, %v", st, err)
	}
	if _, err := s.consume(raw, Zerodha); !errors.Is(err, ErrBadState) {
		t.Fatal("a state must be accepted only once")
	}
	late, _ := s.issue("acct-1", "user-1", Upstox)
	now = now.Add(10 * time.Minute)
	if _, err := s.consume(late, Upstox); !errors.Is(err, ErrBadState) {
		t.Fatal("expired state accepted")
	}
}

func TestTokenExpiry(t *testing.T) {
	at := func(s string) time.Time {
		v, err := time.ParseInLocation("2006-01-02 15:04", s, ist)
		if err != nil {
			t.Fatal(err)
		}
		return v
	}
	tests := []struct {
		broker, issued, want string
	}{
		{Zerodha, "2026-10-29 08:30", "2026-10-30 06:00"},
		{Zerodha, "2026-10-29 23:59", "2026-10-30 06:00"},
		{Zerodha, "2026-10-30 02:00", "2026-10-30 06:00"}, // before the cut-off: that same morning
		{Zerodha, "2026-10-30 06:00", "2026-10-31 06:00"},
		{Upstox, "2026-10-29 08:30", "2026-10-30 03:30"},
		{Upstox, "2026-10-30 03:29", "2026-10-30 03:30"},
	}
	for _, tt := range tests {
		if got := tokenExpiry(tt.broker, at(tt.issued)); !got.Equal(at(tt.want)) {
			t.Errorf("%s issued %s: expires %s, want %s IST", tt.broker, tt.issued, got.In(ist), tt.want)
		}
	}
}

func TestKiteChecksum(t *testing.T) {
	// Kite Connect v3: SHA-256 hex of api_key + request_token + api_secret (reference: shasum -a 256).
	const want = "ff6a6d3d60c9d974df906ba6f787ac38300cfa68b41801b486ea1007e52e8942"
	if got := kiteChecksum("api_key", "request_token", "api_secret"); got != want {
		t.Fatalf("checksum = %s, want %s", got, want)
	}
}

func TestExchangeKite(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_ = r.ParseForm()
		switch {
		case r.Method != http.MethodPost || r.URL.Path != "/session/token" || r.Header.Get("X-Kite-Version") != "3":
			http.Error(w, "bad request shape", http.StatusBadRequest)
		case r.PostForm.Get("checksum") != kiteChecksum("key1", "rt-1", "secret1") || r.PostForm.Get("api_key") != "key1":
			w.WriteHeader(http.StatusForbidden)
			_, _ = w.Write([]byte(`{"status":"error","error_type":"TokenException","message":"Token is invalid or has expired."}`))
		default:
			_, _ = w.Write([]byte(`{"status":"success","data":{"user_id":"AB1234","access_token":"at-xyz","public_token":"p"}}`))
		}
	}))
	defer srv.Close()
	c := boundClient(netip.MustParseAddr("127.0.0.1"))
	g, err := exchangeKite(context.Background(), c, srv.URL, "key1", "secret1", "rt-1")
	if err != nil || g.UserID != "AB1234" || g.AccessToken != "at-xyz" {
		t.Fatalf("grant = %+v, %v", g, err)
	}
	if _, err := exchangeKite(context.Background(), c, srv.URL, "key1", "wrong", "rt-1"); !errors.Is(err, ErrExchange) {
		t.Fatalf("bad checksum: err = %v", err)
	}
	if err != nil && strings.Contains(err.Error(), "rt-1") {
		t.Fatal("error leaks the request token")
	}
}

func TestExchangeUpstox(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_ = r.ParseForm()
		f := r.PostForm
		if r.URL.Path != "/v2/login/authorization/token" || f.Get("grant_type") != "authorization_code" ||
			f.Get("client_id") != "key2" || f.Get("client_secret") != "secret2" || f.Get("code") != "c-1" ||
			f.Get("redirect_uri") != "https://desk.example/api/v1/brokers/callback/upstox" {
			w.WriteHeader(http.StatusBadRequest)
			_, _ = w.Write([]byte(`{"status":"error","errors":[{"errorCode":"UDAPI100057"}]}`))
			return
		}
		_, _ = w.Write([]byte(`{"user_id":"UX99","access_token":"at-up","extended_token":"ext"}`))
	}))
	defer srv.Close()
	c := boundClient(netip.MustParseAddr("127.0.0.1"))
	g, err := exchangeUpstox(context.Background(), c, srv.URL, "key2", "secret2", "c-1", "https://desk.example/api/v1/brokers/callback/upstox")
	if err != nil || g.UserID != "UX99" || g.AccessToken != "at-up" {
		t.Fatalf("grant = %+v, %v", g, err)
	}
	if _, err := exchangeUpstox(context.Background(), c, srv.URL, "key2", "secret2", "used", "x"); !errors.Is(err, ErrExchange) {
		t.Fatalf("err = %v", err)
	}
}

// Invariant 4: the exchange binds to the account's static IP; an address not on this host fails
// before anything is sent.
func TestExchangeRefusesStaticIPNotOnHost(t *testing.T) {
	hit := false
	srv := httptest.NewServer(http.HandlerFunc(func(http.ResponseWriter, *http.Request) { hit = true }))
	defer srv.Close()
	// TEST-NET-3 is never local; the others would bind and let the OS choose the source.
	for _, ip := range []string{"203.0.113.7", "0.0.0.0", "::", "::ffff:0.0.0.0", "::ffff:224.0.0.1"} {
		c := boundClient(netip.MustParseAddr(ip))
		_, err := exchangeKite(context.Background(), c, srv.URL, "k", "s", "rt")
		if !errors.Is(err, ErrSourceIP) || hit {
			t.Fatalf("%s: err = %v, request reached server = %v", ip, err, hit)
		}
	}
}

func TestNewAccountRefusesAddressesThatPinNothing(t *testing.T) {
	account := func() NewAccount {
		return NewAccount{Broker: "upstox", ClientCode: "UP1", HolderName: "X", StaticIP: "10.0.0.5", APIKey: "k", APISecret: "s"}
	}
	fields := map[string]func(*NewAccount, string){
		"static": func(a *NewAccount, ip string) { a.StaticIP = ip },
		"public": func(a *NewAccount, ip string) { a.PublicIP = ip },
		"backup": func(a *NewAccount, ip string) { a.BackupIP = ip },
	}
	for name, set := range fields {
		for _, ip := range []string{"0.0.0.0", "::", "224.0.0.1", "ff02::1", "::ffff:0.0.0.0", "::ffff:224.0.0.1", "::%lo0"} {
			a := account()
			set(&a, ip)
			if _, err := a.params(); err == nil {
				t.Errorf("%s IP %s accepted", name, ip)
			}
		}
		for _, ip := range []string{"10.0.0.6", "2001:db8::1", "::ffff:10.0.0.6"} {
			a := account()
			set(&a, ip)
			if _, err := a.params(); err != nil {
				t.Errorf("%s IP %s refused: %v", name, ip, err)
			}
		}
	}
}
