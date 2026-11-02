// Package brokers runs the daily broker login (API.md §7): the login URL with a signed state, the
// redirect callback, the token exchange from the account's static IP, the encrypted token at rest,
// and the engine StartSession. The broker's own 2FA happens on its site; Vyuha never automates it.
package brokers

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"net/http"
	"net/netip"
	"net/url"
	"strings"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgtype"

	commonv1 "github.com/Jitesh25X/vyuha-terminal/gateway/gen/vyuha/common/v1"
	enginev1 "github.com/Jitesh25X/vyuha-terminal/gateway/gen/vyuha/engine/v1"
	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/audit"
	vcrypto "github.com/Jitesh25X/vyuha-terminal/gateway/internal/crypto"
	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/enginec"
	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/rbac"
	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/store"
)

// Broker names match the broker_kind enum and the callback paths.
const (
	Zerodha = "zerodha"
	Upstox  = "upstox"
)

// CallbackPath is where the broker redirects after login, under the public base URL.
func CallbackPath(broker string) string { return "/api/v1/brokers/callback/" + broker }

// Config holds the broker endpoints (overridden in tests with local fakes; never live in tests).
type Config struct {
	// PublicBaseURL is the desk's external origin, e.g. https://desk.example.in. Upstox needs the
	// exact registered redirect_uri: PublicBaseURL + CallbackPath("upstox").
	PublicBaseURL   string
	KiteLoginURL    string
	KiteAPIBase     string
	UpstoxDialogURL string
	UpstoxAPIBase   string
	StateTTL        time.Duration
}

// DefaultConfig points at the brokers' production endpoints (verified 2026-10-29, see README.md).
func DefaultConfig(publicBaseURL string) Config {
	return Config{
		PublicBaseURL:   strings.TrimRight(publicBaseURL, "/"),
		KiteLoginURL:    "https://kite.zerodha.com/connect/login",
		KiteAPIBase:     "https://api.kite.trade",
		UpstoxDialogURL: "https://api.upstox.com/v2/login/authorization/dialog",
		UpstoxAPIBase:   "https://api.upstox.com",
		StateTTL:        10 * time.Minute,
	}
}

// Engine is the part of the engine client the login flow uses.
type Engine interface {
	StartSession(ctx context.Context, req *enginev1.StartSessionRequest) (*enginev1.SessionInfo, error)
	StopSession(ctx context.Context, accountID, reason string) error
}

// Errors from LoginURL.
var (
	ErrNotFound  = errors.New("brokers: account not found")
	ErrForbidden = errors.New("brokers: not allowed")
	ErrNoLogin   = errors.New("brokers: account has no broker login")
)

// LoginError is a failed callback; Code is shown in the terminal redirect (never secret).
type LoginError struct{ Code string }

func (e *LoginError) Error() string { return "brokers: login failed: " + e.Code }

// callbackTimeout bounds the whole callback: token exchange (10 s), DB, engine stop + start.
const callbackTimeout = 30 * time.Second

// Callback failure codes.
const (
	CodeStateInvalid      = "BROKER_LOGIN_STATE_INVALID"
	CodeDenied            = "BROKER_LOGIN_DENIED"
	CodeForbidden         = "BROKER_LOGIN_FORBIDDEN"
	CodeAccountMismatch   = "BROKER_ACCOUNT_MISMATCH"
	CodeExchangeFailed    = "BROKER_TOKEN_EXCHANGE_FAILED"
	CodeSecretsUnreadable = "BROKER_SECRETS_UNREADABLE"
	CodeStaticIP          = "STATIC_IP_NOT_ON_HOST"
	CodeEngineUnavailable = "ENGINE_UNAVAILABLE"
	CodeInternal          = "INTERNAL"
)

// CallbackParams are the redirect's query parameters (all secrets except State and Status).
type CallbackParams struct {
	State        string
	Status       string // Kite: "success" when the user completed login
	RequestToken string // Kite
	Code         string // Upstox
}

// Service implements the login flow.
type Service struct {
	q       *store.Queries
	env     *vcrypto.Envelope
	engine  Engine
	states  *stateSigner
	cfg     Config
	now     func() time.Time
	client  func(netip.Addr) *http.Client
	logger  *slog.Logger
	observe func(*enginev1.SessionInfo)
}

// NewService builds the login flow. observe (optional) is told about each session the engine starts,
// so the WebSocket hub learns its feed id at once.
func NewService(q *store.Queries, env *vcrypto.Envelope, engine Engine, cfg Config, logger *slog.Logger,
	now func() time.Time, observe func(*enginev1.SessionInfo),
) (*Service, error) {
	if cfg.PublicBaseURL == "" {
		return nil, errors.New("brokers: PUBLIC_BASE_URL is required for broker login redirects")
	}
	states, err := newStateSigner(cfg.StateTTL, now)
	if err != nil {
		return nil, err
	}
	if observe == nil {
		observe = func(*enginev1.SessionInfo) {}
	}
	return &Service{
		q: q, env: env, engine: engine, states: states, cfg: cfg, now: now,
		client: boundClient, logger: logger, observe: observe,
	}, nil
}

// LoginURL returns the broker login URL for an account the user may log in.
func (s *Service) LoginURL(ctx context.Context, userID, accountID string) (string, error) {
	acct, err := s.authorise(ctx, userID, accountID)
	if err != nil {
		return "", err
	}
	if acct.Broker != store.BrokerKindZerodha && acct.Broker != store.BrokerKindUpstox {
		return "", ErrNoLogin
	}
	apiKey, err := s.open(acct.ApiKeyEnc, "api_key_enc", acct.ID)
	if err != nil {
		return "", fmt.Errorf("brokers: api key: %w", err)
	}
	state, err := s.states.issue(acct.ID.String(), userID, string(acct.Broker))
	if err != nil {
		return "", err
	}
	if acct.Broker == store.BrokerKindZerodha {
		// Kite returns redirect_params (URL-encoded) unchanged to the app's registered redirect URL.
		q := url.Values{"v": {"3"}, "api_key": {string(apiKey)}, "redirect_params": {url.Values{"state": {state}}.Encode()}}
		return s.cfg.KiteLoginURL + "?" + q.Encode(), nil
	}
	q := url.Values{
		"response_type": {"code"}, "client_id": {string(apiKey)},
		"redirect_uri": {s.redirectURI(Upstox)}, "state": {state},
	}
	return s.cfg.UpstoxDialogURL + "?" + q.Encode(), nil
}

// Callback completes a login: it returns the account id, or a *LoginError.
func (s *Service) Callback(ctx context.Context, broker string, p CallbackParams, ip netip.Addr) (string, error) {
	// Once started, the login must finish and be audited even if the browser goes away: a stored
	// token or a started session without its audit row is worse than a slow redirect.
	ctx, cancel := context.WithTimeout(context.WithoutCancel(ctx), callbackTimeout)
	defer cancel()
	st, err := s.states.consume(p.State, broker)
	if err != nil {
		// No audit row: anyone can hit this URL, and an unsigned request names no real user.
		s.logger.WarnContext(ctx, "broker login callback with invalid state", "broker", broker)
		return "", &LoginError{Code: CodeStateInvalid}
	}
	info, code := s.complete(ctx, st, p)
	ev := audit.Event{
		ActorID: st.User, ActorKind: audit.ActorUser, Entity: "broker_account", EntityID: st.Account, IP: ip,
		Action: "broker.login.success", Payload: map[string]any{"broker": broker},
	}
	if code != "" {
		ev.Action, ev.Payload["reason"] = "broker.login.failure", code
	} else {
		ev.Payload["session_state"], ev.Payload["feed_id"] = info.GetState(), info.GetFeedId()
	}
	// The audit row gets its own deadline: a slow exchange + engine restart must not use it up.
	actx, acancel := context.WithTimeout(context.WithoutCancel(ctx), 5*time.Second)
	defer acancel()
	if _, err := audit.Record(actx, s.q, ev); err != nil {
		s.logger.ErrorContext(ctx, "broker login audit failed", "account", st.Account, "error", err)
		if code == "" {
			code = CodeInternal
		}
	}
	if code != "" {
		s.logger.WarnContext(ctx, "broker login failed", "account", st.Account, "broker", broker, "code", code)
		return st.Account, &LoginError{Code: code}
	}
	s.logger.InfoContext(ctx, "broker login complete", "account", st.Account, "broker", broker, "state", info.GetState())
	return st.Account, nil
}

// complete runs everything after the state check; it returns a failure code or "".
func (s *Service) complete(ctx context.Context, st loginState, p CallbackParams) (*enginev1.SessionInfo, string) {
	secret := p.RequestToken
	if st.Broker == Upstox {
		secret = p.Code
	}
	if secret == "" || (st.Broker == Zerodha && p.Status != "success") {
		return nil, CodeDenied
	}
	acct, err := s.authorise(ctx, st.User, st.Account)
	switch {
	case errors.Is(err, ErrForbidden), errors.Is(err, ErrNotFound):
		return nil, CodeForbidden
	case err != nil:
		return nil, CodeInternal
	}
	if string(acct.Broker) != st.Broker || acct.StaticIp == nil {
		return nil, CodeForbidden
	}
	apiKey, err1 := s.open(acct.ApiKeyEnc, "api_key_enc", acct.ID)
	apiSecret, err2 := s.open(acct.ApiSecretEnc, "api_secret_enc", acct.ID)
	if err1 != nil || err2 != nil {
		return nil, CodeSecretsUnreadable
	}

	client := s.client(*acct.StaticIp)
	var g grant
	if st.Broker == Zerodha {
		g, err = exchangeKite(ctx, client, s.cfg.KiteAPIBase, string(apiKey), string(apiSecret), secret)
	} else {
		g, err = exchangeUpstox(ctx, client, s.cfg.UpstoxAPIBase, string(apiKey), string(apiSecret), secret, s.redirectURI(Upstox))
	}
	switch {
	case errors.Is(err, ErrSourceIP):
		return nil, CodeStaticIP
	case err != nil:
		return nil, CodeExchangeFailed
	}
	// The token must belong to this account's client: someone mapped to account A could otherwise
	// log in to the broker as client B and attach B's session to A.
	if !strings.EqualFold(strings.TrimSpace(g.UserID), acct.ClientCode) {
		return nil, CodeAccountMismatch
	}

	sealed, err := s.env.Seal([]byte(g.AccessToken), ad("access_token_enc", acct.ID))
	if err != nil {
		return nil, CodeInternal
	}
	expires := tokenExpiry(st.Broker, s.now())
	if err := s.q.StoreBrokerAccessToken(ctx, store.StoreBrokerAccessTokenParams{
		ID: acct.ID, AccessTokenEnc: sealed, TokenExpiresAt: pgtype.Timestamptz{Time: expires, Valid: true},
		Status: store.AccountStatusAwaitingLogin,
	}); err != nil {
		return nil, CodeInternal
	}

	info, code := s.startSession(ctx, acct, string(apiKey), string(apiSecret), g.AccessToken)
	status := store.AccountStatusError
	if code == "" {
		status = accountStatus(info.GetState())
		s.observe(info)
	}
	if err := s.q.SetBrokerAccountStatus(ctx, store.SetBrokerAccountStatusParams{ID: acct.ID, Status: status}); err != nil && code == "" {
		code = CodeInternal
	}
	return info, code
}

// startSession replaces any running session (yesterday's token has expired) with a new one.
func (s *Service) startSession(ctx context.Context, acct store.BrokerAccount, apiKey, apiSecret, token string) (*enginev1.SessionInfo, string) {
	id := acct.ID.String()
	if err := s.engine.StopSession(ctx, id, "daily broker login"); err != nil {
		return nil, engineCode(err)
	}
	req := &enginev1.StartSessionRequest{
		AccountId: id, Broker: brokerEnum(acct.Broker), ApiKey: apiKey, ApiSecret: apiSecret, AccessToken: token,
		SourceIp: acct.StaticIp.String(), OpsCap: uint32(max(acct.OpsCap, 0)), DataEntitled: acct.DataEntitled,
	}
	if acct.BackupStaticIp != nil {
		req.BackupSourceIp = acct.BackupStaticIp.String()
	}
	if acct.PublicStaticIp != nil {
		req.PublicIp = acct.PublicStaticIp.String()
	}
	info, err := s.engine.StartSession(ctx, req)
	req.ApiSecret, req.AccessToken = "", "" // drop our references; the engine zeroises its copies
	if err != nil {
		return nil, engineCode(err)
	}
	return info, ""
}

// authorise loads the account and checks that the user may log it in (role re-read from the DB).
func (s *Service) authorise(ctx context.Context, userID, accountID string) (store.BrokerAccount, error) {
	var aid, uid pgtype.UUID
	if aid.Scan(accountID) != nil {
		return store.BrokerAccount{}, ErrNotFound
	}
	if uid.Scan(userID) != nil {
		return store.BrokerAccount{}, ErrForbidden
	}
	acct, err := s.q.GetBrokerAccount(ctx, aid)
	if errors.Is(err, pgx.ErrNoRows) {
		return store.BrokerAccount{}, ErrNotFound
	} else if err != nil {
		return store.BrokerAccount{}, fmt.Errorf("brokers: load account: %w", err)
	}
	user, err := s.q.GetUserAccess(ctx, uid)
	if errors.Is(err, pgx.ErrNoRows) || (err == nil && !user.IsActive) {
		return store.BrokerAccount{}, ErrForbidden
	} else if err != nil {
		return store.BrokerAccount{}, fmt.Errorf("brokers: load user: %w", err)
	}
	var m *rbac.Mapping
	row, err := s.q.GetAccountMapping(ctx, store.GetAccountMappingParams{AccountID: aid, UserID: uid})
	switch {
	case err == nil:
		m = &rbac.Mapping{CanView: row.CanView, CanTrade: row.CanTrade}
	case !errors.Is(err, pgx.ErrNoRows):
		return store.BrokerAccount{}, fmt.Errorf("brokers: load mapping: %w", err)
	}
	if !rbac.CanBrokerLogin(string(user.Role), m) {
		return store.BrokerAccount{}, ErrForbidden
	}
	return acct, nil
}

func (s *Service) open(sealed []byte, column string, id pgtype.UUID) ([]byte, error) {
	if len(sealed) == 0 {
		return nil, vcrypto.ErrDecrypt
	}
	return s.env.Open(sealed, ad(column, id))
}

func (s *Service) redirectURI(broker string) string {
	return s.cfg.PublicBaseURL + CallbackPath(broker)
}

// ad binds a broker_account ciphertext to its column and row (see crypto.Envelope).
func ad(column string, id pgtype.UUID) []byte {
	return []byte("broker_account." + column + ":" + id.String())
}

// SealSecrets encrypts an account's API key and secret for storage (vyuhactl account create).
func SealSecrets(env *vcrypto.Envelope, id pgtype.UUID, apiKey, apiSecret string) (key, secret []byte, err error) {
	if key, err = env.Seal([]byte(apiKey), ad("api_key_enc", id)); err != nil {
		return nil, nil, err
	}
	if secret, err = env.Seal([]byte(apiSecret), ad("api_secret_enc", id)); err != nil {
		return nil, nil, err
	}
	return key, secret, nil
}

func engineCode(err error) string {
	var refused *enginec.RefusedError
	if errors.As(err, &refused) {
		return refused.Code
	}
	return CodeEngineUnavailable
}

func brokerEnum(b store.BrokerKind) commonv1.Broker {
	switch b {
	case store.BrokerKindZerodha:
		return commonv1.Broker_BROKER_ZERODHA
	case store.BrokerKindUpstox:
		return commonv1.Broker_BROKER_UPSTOX
	case store.BrokerKindSim:
		return commonv1.Broker_BROKER_SIM
	}
	return commonv1.Broker_BROKER_UNSPECIFIED
}

func accountStatus(engineState string) store.AccountStatus {
	switch engineState {
	case "connected":
		return store.AccountStatusConnected
	case "awaiting_login":
		return store.AccountStatusAwaitingLogin
	case "disconnected":
		return store.AccountStatusDisconnected
	}
	return store.AccountStatusError
}

// AccountView is one broker account as the terminal sees it (no secrets).
type AccountView struct {
	ID, Broker, ClientCode, HolderName, Status string
	DataEntitled, CanLogin                     bool
	TokenExpiresAt                             *time.Time
}

// Accounts lists the accounts a user can see: mapped (can_view) ones, or all for admins and risk
// managers. A stored token past its expiry reads as awaiting_login.
func (s *Service) Accounts(ctx context.Context, userID string) ([]AccountView, error) {
	var uid pgtype.UUID
	if uid.Scan(userID) != nil {
		return nil, ErrForbidden
	}
	user, err := s.q.GetUserAccess(ctx, uid)
	if errors.Is(err, pgx.ErrNoRows) || (err == nil && !user.IsActive) {
		return nil, ErrForbidden
	} else if err != nil {
		return nil, fmt.Errorf("brokers: load user: %w", err)
	}
	role := string(user.Role)
	now := s.now()
	view := func(id pgtype.UUID, broker store.BrokerKind, code, holder string, status store.AccountStatus,
		entitled bool, expires pgtype.Timestamptz, m *rbac.Mapping,
	) AccountView {
		v := AccountView{
			ID: id.String(), Broker: string(broker), ClientCode: code, HolderName: holder, Status: string(status),
			DataEntitled: entitled, CanLogin: broker != store.BrokerKindSim && rbac.CanBrokerLogin(role, m),
		}
		if expires.Valid {
			t := expires.Time
			v.TokenExpiresAt = &t
			if !now.Before(t) && status == store.AccountStatusConnected {
				v.Status = string(store.AccountStatusAwaitingLogin)
			}
		}
		return v
	}
	var out []AccountView
	if rbac.SeesAllAccounts(role) {
		rows, err := s.q.ListAllBrokerAccounts(ctx)
		if err != nil {
			return nil, fmt.Errorf("brokers: list accounts: %w", err)
		}
		mapped := map[string]*rbac.Mapping{}
		if mine, err := s.q.ListMappedBrokerAccounts(ctx, uid); err == nil {
			for _, r := range mine {
				mapped[r.ID.String()] = &rbac.Mapping{CanView: true, CanTrade: r.CanTrade}
			}
		}
		for _, r := range rows {
			out = append(out, view(r.ID, r.Broker, r.ClientCode, r.HolderName, r.Status, r.DataEntitled, r.TokenExpiresAt, mapped[r.ID.String()]))
		}
		return out, nil
	}
	rows, err := s.q.ListMappedBrokerAccounts(ctx, uid)
	if err != nil {
		return nil, fmt.Errorf("brokers: list accounts: %w", err)
	}
	for _, r := range rows {
		out = append(out, view(r.ID, r.Broker, r.ClientCode, r.HolderName, r.Status, r.DataEntitled, r.TokenExpiresAt,
			&rbac.Mapping{CanView: true, CanTrade: r.CanTrade}))
	}
	return out, nil
}
