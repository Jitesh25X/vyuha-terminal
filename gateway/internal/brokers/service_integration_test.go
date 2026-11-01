package brokers

import (
	"bytes"
	"context"
	"crypto/rand"
	"encoding/base64"
	"errors"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"net/netip"
	"net/url"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/jackc/pgx/v5/pgtype"
	"github.com/jackc/pgx/v5/pgxpool"

	enginev1 "github.com/Jitesh25X/vyuha-terminal/gateway/gen/vyuha/engine/v1"
	vcrypto "github.com/Jitesh25X/vyuha-terminal/gateway/internal/crypto"
	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/enginec"
	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/store"
	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/testdb"
)

type fakeEngine struct {
	mu      sync.Mutex
	started []*enginev1.StartSessionRequest
	stopped []string
	refuse  string
}

func (e *fakeEngine) StartSession(_ context.Context, req *enginev1.StartSessionRequest) (*enginev1.SessionInfo, error) {
	e.mu.Lock()
	defer e.mu.Unlock()
	cp := &enginev1.StartSessionRequest{
		AccountId: req.AccountId, Broker: req.Broker, ApiKey: req.ApiKey, ApiSecret: req.ApiSecret,
		AccessToken: req.AccessToken, SourceIp: req.SourceIp, OpsCap: req.OpsCap, DataEntitled: req.DataEntitled,
	}
	e.started = append(e.started, cp)
	if e.refuse != "" {
		return nil, &enginec.RefusedError{Code: e.refuse}
	}
	return &enginev1.SessionInfo{AccountId: req.AccountId, State: "connected", FeedId: uint32(len(e.started))}, nil
}

func (e *fakeEngine) StopSession(_ context.Context, id, _ string) error {
	e.mu.Lock()
	defer e.mu.Unlock()
	e.stopped = append(e.stopped, id)
	return nil
}

type fixture struct {
	t        *testing.T
	pool     *pgxpool.Pool
	q        *store.Queries
	env      *vcrypto.Envelope
	engine   *fakeEngine
	svc      *Service
	observed []*enginev1.SessionInfo
	kiteUser string // user_id the fake Kite returns
	now      time.Time
}

func newFixture(t *testing.T) *fixture {
	t.Helper()
	_, pool := testdb.New(t)
	key := make([]byte, 32)
	_, _ = rand.Read(key)
	env, err := vcrypto.NewEnvelope(base64.StdEncoding.EncodeToString(key))
	if err != nil {
		t.Fatal(err)
	}
	f := &fixture{t: t, pool: pool, q: store.New(pool), env: env, engine: &fakeEngine{}, kiteUser: "ZK1001",
		now: time.Date(2026, 10, 29, 3, 0, 0, 0, time.UTC)}

	kite := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_ = r.ParseForm()
		if r.PostForm.Get("checksum") != kiteChecksum("kite-key", r.PostForm.Get("request_token"), "kite-secret") {
			w.WriteHeader(http.StatusForbidden)
			return
		}
		_, _ = io.WriteString(w, `{"status":"success","data":{"user_id":"`+f.kiteUser+`","access_token":"kite-at-`+r.PostForm.Get("request_token")+`"}}`)
	}))
	upstox := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_ = r.ParseForm()
		if r.PostForm.Get("client_secret") != "up-secret" || r.PostForm.Get("redirect_uri") != "https://desk.example/api/v1/brokers/callback/upstox" {
			w.WriteHeader(http.StatusBadRequest)
			return
		}
		_, _ = io.WriteString(w, `{"user_id":"UP2002","access_token":"up-at"}`)
	}))
	t.Cleanup(func() { kite.Close(); upstox.Close() })

	cfg := DefaultConfig("https://desk.example/")
	cfg.KiteAPIBase, cfg.UpstoxAPIBase = kite.URL, upstox.URL
	f.svc, err = NewService(f.q, env, f.engine, cfg, slog.New(slog.NewTextHandler(io.Discard, nil)),
		func() time.Time { return f.now }, func(i *enginev1.SessionInfo) { f.observed = append(f.observed, i) })
	if err != nil {
		t.Fatal(err)
	}
	return f
}

func (f *fixture) exec(sql string, args ...any) {
	f.t.Helper()
	if _, err := f.pool.Exec(context.Background(), sql, args...); err != nil {
		f.t.Fatalf("%s: %v", sql, err)
	}
}

func (f *fixture) user(name, role string) string {
	f.t.Helper()
	var id string
	err := f.pool.QueryRow(context.Background(),
		`INSERT INTO app_user (username, display_name, role, password_hash) VALUES ($1, $1, $2, 'x') RETURNING id::text`,
		name, role).Scan(&id)
	if err != nil {
		f.t.Fatal(err)
	}
	return id
}

func (f *fixture) account(broker, client, apiKey, apiSecret, staticIP string) string {
	f.t.Helper()
	ctx := context.Background()
	p := store.CreateBrokerAccountParams{
		Broker: store.BrokerKind(broker), ClientCode: client, HolderName: "Desk " + client,
		ApiKeyEnc: []byte{0}, ApiSecretEnc: []byte{0}, DataEntitled: true, OpsCap: 8,
	}
	if staticIP != "" {
		ip := netip.MustParseAddr(staticIP)
		p.StaticIp = &ip
	}
	id, err := f.q.CreateBrokerAccount(ctx, p)
	if err != nil {
		f.t.Fatal(err)
	}
	k, s, err := SealSecrets(f.env, id, apiKey, apiSecret)
	if err != nil {
		f.t.Fatal(err)
	}
	if err := f.q.SetBrokerAccountSecrets(ctx, store.SetBrokerAccountSecretsParams{ID: id, ApiKeyEnc: k, ApiSecretEnc: s}); err != nil {
		f.t.Fatal(err)
	}
	return id.String()
}

func (f *fixture) mapUser(account, user string, trade bool) {
	f.exec(`INSERT INTO account_user_map (account_id, user_id, can_view, can_trade) VALUES ($1, $2, true, $3)`, account, user, trade)
}

func stateOf(t *testing.T, loginURL string) (url.Values, string) {
	t.Helper()
	u, err := url.Parse(loginURL)
	if err != nil {
		t.Fatal(err)
	}
	q := u.Query()
	if rp := q.Get("redirect_params"); rp != "" { // Kite
		inner, err := url.ParseQuery(rp)
		if err != nil {
			t.Fatal(err)
		}
		return q, inner.Get("state")
	}
	return q, q.Get("state")
}

func (f *fixture) auditActions(account string) []string {
	f.t.Helper()
	rows, err := f.pool.Query(context.Background(),
		`SELECT action || ':' || coalesce(payload->>'reason', '') FROM audit_event WHERE entity = 'broker_account' AND entity_id = $1 ORDER BY id`, account)
	if err != nil {
		f.t.Fatal(err)
	}
	defer rows.Close()
	var out []string
	for rows.Next() {
		var s string
		_ = rows.Scan(&s)
		out = append(out, s)
	}
	return out
}

func TestKiteLoginStoresEncryptedTokenAndStartsSession(t *testing.T) {
	f := newFixture(t)
	trader := f.user("meera", "trader")
	acct := f.account("zerodha", "ZK1001", "kite-key", "kite-secret", "127.0.0.1")
	f.mapUser(acct, trader, true)
	ctx := context.Background()

	loginURL, err := f.svc.LoginURL(ctx, trader, acct)
	if err != nil {
		t.Fatal(err)
	}
	q, state := stateOf(t, loginURL)
	if !strings.HasPrefix(loginURL, "https://kite.zerodha.com/connect/login?") || q.Get("v") != "3" || q.Get("api_key") != "kite-key" || state == "" {
		t.Fatalf("login url %s", loginURL)
	}
	if strings.Contains(loginURL, "kite-secret") {
		t.Fatal("login URL leaks the API secret")
	}

	got, err := f.svc.Callback(ctx, Zerodha, CallbackParams{State: state, Status: "success", RequestToken: "rt1"}, netip.MustParseAddr("10.0.0.5"))
	if err != nil || got != acct {
		t.Fatalf("callback = %q, %v", got, err)
	}
	a := f.mustAccount(acct)
	if a.Status != store.AccountStatusConnected || !a.TokenExpiresAt.Valid ||
		!a.TokenExpiresAt.Time.Equal(time.Date(2026, 10, 30, 0, 30, 0, 0, time.UTC)) { // 06:00 IST
		t.Fatalf("account after login: status %s expires %v", a.Status, a.TokenExpiresAt)
	}
	if bytes.Contains(a.AccessTokenEnc, []byte("kite-at-rt1")) {
		t.Fatal("access token stored in plaintext")
	}
	if pt, err := f.env.Open(a.AccessTokenEnc, []byte("broker_account.access_token_enc:"+acct)); err != nil || string(pt) != "kite-at-rt1" {
		t.Fatalf("sealed token = %q, %v", pt, err)
	}

	if len(f.engine.stopped) != 1 || len(f.engine.started) != 1 {
		t.Fatalf("engine stop/start = %v / %d", f.engine.stopped, len(f.engine.started))
	}
	s := f.engine.started[0]
	if s.AccountId != acct || s.ApiKey != "kite-key" || s.ApiSecret != "kite-secret" || s.AccessToken != "kite-at-rt1" ||
		s.SourceIp != "127.0.0.1" || s.OpsCap != 8 || !s.DataEntitled {
		t.Fatalf("StartSession = %+v", s)
	}
	if len(f.observed) != 1 || f.observed[0].GetFeedId() != 1 {
		t.Fatalf("hub not told about the new session: %v", f.observed)
	}
	if acts := f.auditActions(acct); len(acts) != 1 || acts[0] != "broker.login.success:" {
		t.Fatalf("audit = %v", acts)
	}
	var payload string
	_ = f.pool.QueryRow(ctx, `SELECT payload::text FROM audit_event WHERE entity_id = $1`, acct).Scan(&payload)
	if strings.Contains(payload, "kite-at") || strings.Contains(payload, "rt1") || strings.Contains(payload, "secret") {
		t.Fatalf("audit payload carries a secret: %s", payload)
	}

	// The state is single-use.
	if _, err := f.svc.Callback(ctx, Zerodha, CallbackParams{State: state, Status: "success", RequestToken: "rt2"}, netip.Addr{}); code(err) != CodeStateInvalid {
		t.Fatalf("replay: %v", err)
	}

	// A browser that disconnects mid-login doesn't leave a session without its audit row.
	u2, err := f.svc.LoginURL(ctx, trader, acct)
	if err != nil {
		t.Fatal(err)
	}
	_, state2 := stateOf(t, u2)
	gone, cancel := context.WithCancel(ctx)
	cancel()
	if _, err := f.svc.Callback(gone, Zerodha, CallbackParams{State: state2, Status: "success", RequestToken: "rt3"}, netip.Addr{}); err != nil {
		t.Fatalf("cancelled request: %v", err)
	}
	if acts := f.auditActions(acct); len(acts) != 2 || acts[1] != "broker.login.success:" {
		t.Fatalf("audit after cancelled request = %v", acts)
	}
}

func TestUpstoxLogin(t *testing.T) {
	f := newFixture(t)
	admin := f.user("ops", "admin") // admin: no mapping needed
	acct := f.account("upstox", "UP2002", "up-key", "up-secret", "127.0.0.1")
	ctx := context.Background()
	loginURL, err := f.svc.LoginURL(ctx, admin, acct)
	if err != nil {
		t.Fatal(err)
	}
	q, state := stateOf(t, loginURL)
	if !strings.HasPrefix(loginURL, "https://api.upstox.com/v2/login/authorization/dialog?") || q.Get("response_type") != "code" ||
		q.Get("client_id") != "up-key" || q.Get("redirect_uri") != "https://desk.example/api/v1/brokers/callback/upstox" {
		t.Fatalf("login url %s", loginURL)
	}
	if _, err := f.svc.Callback(ctx, Upstox, CallbackParams{State: state, Code: "c1"}, netip.Addr{}); err != nil {
		t.Fatal(err)
	}
	if a := f.mustAccount(acct); !a.TokenExpiresAt.Time.Equal(time.Date(2026, 10, 29, 22, 0, 0, 0, time.UTC)) { // 03:30 IST
		t.Fatalf("expiry %v", a.TokenExpiresAt.Time)
	}
}

func TestLoginPermissions(t *testing.T) {
	f := newFixture(t)
	acct := f.account("zerodha", "ZK1001", "kite-key", "kite-secret", "127.0.0.1")
	viewer, analyst, stranger, risk := f.user("view", "trader"), f.user("ana", "analyst"), f.user("nobody", "trader"), f.user("risk", "risk_manager")
	f.mapUser(acct, viewer, false)
	f.mapUser(acct, analyst, true)
	f.mapUser(acct, risk, true)
	inactive := f.user("gone", "trader")
	f.mapUser(acct, inactive, true)
	f.exec(`UPDATE app_user SET is_active = false WHERE id = $1`, inactive)
	for name, user := range map[string]string{"view-only trader": viewer, "analyst": analyst, "unmapped trader": stranger, "risk manager": risk, "inactive": inactive} {
		if _, err := f.svc.LoginURL(context.Background(), user, acct); !errors.Is(err, ErrForbidden) {
			t.Errorf("%s: err = %v, want ErrForbidden", name, err)
		}
	}
	admin := f.user("ops", "admin")
	if _, err := f.svc.LoginURL(context.Background(), admin, "00000000-0000-4000-8000-000000000000"); !errors.Is(err, ErrNotFound) {
		t.Errorf("unknown account: %v", err)
	}
	sim := f.account("sim", "PAPER", "", "", "")
	if _, err := f.svc.LoginURL(context.Background(), admin, sim); !errors.Is(err, ErrNoLogin) {
		t.Errorf("sim account: %v", err)
	}
}

func TestCallbackFailures(t *testing.T) {
	ctx := context.Background()
	setup := func(t *testing.T) (*fixture, string, string) {
		f := newFixture(t)
		trader := f.user("meera", "trader")
		acct := f.account("zerodha", "ZK1001", "kite-key", "kite-secret", "127.0.0.1")
		f.mapUser(acct, trader, true)
		u, err := f.svc.LoginURL(ctx, trader, acct)
		if err != nil {
			t.Fatal(err)
		}
		_, state := stateOf(t, u)
		return f, acct, state
	}

	t.Run("user cancelled at the broker", func(t *testing.T) {
		f, acct, state := setup(t)
		_, err := f.svc.Callback(ctx, Zerodha, CallbackParams{State: state, Status: "cancelled"}, netip.Addr{})
		if code(err) != CodeDenied || len(f.engine.started) != 0 {
			t.Fatalf("err = %v", err)
		}
		if acts := f.auditActions(acct); len(acts) != 1 || acts[0] != "broker.login.failure:"+CodeDenied {
			t.Fatalf("audit = %v", acts)
		}
	})
	t.Run("logged in to a different client at the broker", func(t *testing.T) {
		f, acct, state := setup(t)
		f.kiteUser = "OTHER9"
		_, err := f.svc.Callback(ctx, Zerodha, CallbackParams{State: state, Status: "success", RequestToken: "rt"}, netip.Addr{})
		if code(err) != CodeAccountMismatch || len(f.engine.started) != 0 {
			t.Fatalf("err = %v", err)
		}
		if a := f.mustAccount(acct); a.AccessTokenEnc != nil {
			t.Fatal("another client's token was stored")
		}
	})
	t.Run("engine refuses the static IP", func(t *testing.T) {
		f, acct, state := setup(t)
		f.engine.refuse = CodeStaticIP
		_, err := f.svc.Callback(ctx, Zerodha, CallbackParams{State: state, Status: "success", RequestToken: "rt"}, netip.Addr{})
		if code(err) != CodeStaticIP {
			t.Fatalf("err = %v", err)
		}
		if a := f.mustAccount(acct); a.Status != store.AccountStatusError {
			t.Fatalf("status = %s", a.Status)
		}
	})
	t.Run("mapping removed between login URL and callback", func(t *testing.T) {
		f, acct, state := setup(t)
		f.exec(`DELETE FROM account_user_map WHERE account_id = $1`, acct)
		if _, err := f.svc.Callback(ctx, Zerodha, CallbackParams{State: state, Status: "success", RequestToken: "rt"}, netip.Addr{}); code(err) != CodeForbidden {
			t.Fatalf("err = %v", err)
		}
	})
	t.Run("forged state is not audited", func(t *testing.T) {
		f, acct, _ := setup(t)
		if _, err := f.svc.Callback(ctx, Zerodha, CallbackParams{State: "forged.sig", Status: "success", RequestToken: "rt"}, netip.Addr{}); code(err) != CodeStateInvalid {
			t.Fatalf("err = %v", err)
		}
		if acts := f.auditActions(acct); len(acts) != 0 {
			t.Fatalf("audit = %v", acts)
		}
	})
}

func (f *fixture) mustAccount(id string) store.BrokerAccount {
	f.t.Helper()
	var uid pgtype.UUID
	_ = uid.Scan(id)
	a, err := f.q.GetBrokerAccount(context.Background(), uid)
	if err != nil {
		f.t.Fatal(err)
	}
	return a
}

func code(err error) string {
	var le *LoginError
	if errors.As(err, &le) {
		return le.Code
	}
	return ""
}

func TestCreateAccountSealsSecretsAndMapsUsers(t *testing.T) {
	f := newFixture(t)
	ctx := context.Background()
	trader := f.user("meera", "trader")
	id, err := CreateAccount(ctx, f.pool, f.env, NewAccount{
		Broker: "zerodha", ClientCode: "ZK1001", HolderName: "Meera", APIKey: "kite-key", APISecret: "kite-secret",
		StaticIP: "127.0.0.1", DataEntitled: true,
	})
	if err != nil {
		t.Fatal(err)
	}
	a := f.mustAccount(id)
	if bytes.Contains(a.ApiSecretEnc, []byte("kite-secret")) || bytes.Contains(a.ApiKeyEnc, []byte("kite-key")) || a.OpsCap != 8 {
		t.Fatalf("account row: %+v", a)
	}
	if err := MapUser(ctx, f.pool, "zerodha:ZK1001", "meera", true, true); err != nil {
		t.Fatal(err)
	}
	// The stored secrets work end to end: the login flow decrypts them for the exchange.
	u, err := f.svc.LoginURL(ctx, trader, id)
	if err != nil {
		t.Fatal(err)
	}
	_, state := stateOf(t, u)
	if _, err := f.svc.Callback(ctx, Zerodha, CallbackParams{State: state, Status: "success", RequestToken: "rt"}, netip.Addr{}); err != nil {
		t.Fatal(err)
	}
	var payloads string
	_ = f.pool.QueryRow(ctx, `SELECT string_agg(payload::text, ' ') FROM audit_event WHERE action IN ('broker_account.create', 'account_user_map.set')`).Scan(&payloads)
	if !strings.Contains(payloads, "ZK1001") || strings.Contains(payloads, "kite-") {
		t.Fatalf("audit payloads: %s", payloads)
	}

	for name, bad := range map[string]NewAccount{
		"ops cap at the threshold": {Broker: "zerodha", ClientCode: "A", HolderName: "A", APIKey: "k", APISecret: "s", StaticIP: "127.0.0.1", OpsCap: 10},
		"no static IP":             {Broker: "upstox", ClientCode: "B", HolderName: "B", APIKey: "k", APISecret: "s"},
		"no secret":                {Broker: "upstox", ClientCode: "C", HolderName: "C", APIKey: "k", StaticIP: "127.0.0.1"},
		"unknown broker":           {Broker: "angel", ClientCode: "D", HolderName: "D"},
	} {
		if _, err := CreateAccount(ctx, f.pool, f.env, bad); err == nil {
			t.Errorf("%s: accepted", name)
		}
	}
	if _, err := CreateAccount(ctx, f.pool, f.env, NewAccount{Broker: "sim", ClientCode: "PAPER", HolderName: "Paper"}); err != nil {
		t.Errorf("sim account: %v", err)
	}
}
