package brokers

import (
	"context"
	"errors"
	"fmt"
	"net/netip"
	"strings"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgtype"

	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/audit"
	vcrypto "github.com/Jitesh25X/vyuha-terminal/gateway/internal/crypto"
	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/store"
)

// MaxOpsCap: invariant 7, the per-account OPS cap stays below the exchange's 10 OPS threshold.
const MaxOpsCap = 9

// DB starts transactions (a pgx pool).
type DB interface {
	Begin(ctx context.Context) (pgx.Tx, error)
}

// NewAccount is a broker account to register (vyuhactl account create). APIKey/APISecret are secrets.
type NewAccount struct {
	Broker       string // zerodha | upstox | sim
	ClientCode   string
	HolderName   string
	APIKey       string
	APISecret    string
	StaticIP     string // host-local address the account's traffic binds to (required unless sim)
	PublicIP     string // registered with the broker (optional)
	BackupIP     string
	DataEntitled bool
	OpsCap       int
}

// CreateAccount stores an account with its API key and secret sealed (AES-256-GCM, bound to the row)
// and audits it, in one transaction.
func CreateAccount(ctx context.Context, db DB, env *vcrypto.Envelope, a NewAccount) (string, error) {
	p, err := a.params()
	if err != nil {
		return "", err
	}
	var id pgtype.UUID
	err = pgx.BeginFunc(ctx, db, func(tx pgx.Tx) error {
		q := store.New(tx)
		if id, err = q.CreateBrokerAccount(ctx, p); err != nil {
			return fmt.Errorf("brokers: create account: %w", err)
		}
		if a.Broker != string(store.BrokerKindSim) {
			key, secret, err := SealSecrets(env, id, a.APIKey, a.APISecret)
			if err != nil {
				return err
			}
			if err := q.SetBrokerAccountSecrets(ctx, store.SetBrokerAccountSecretsParams{ID: id, ApiKeyEnc: key, ApiSecretEnc: secret}); err != nil {
				return fmt.Errorf("brokers: store secrets: %w", err)
			}
		}
		_, err := audit.Record(ctx, q, audit.Event{
			ActorKind: audit.ActorSystem, Action: "broker_account.create", Entity: "broker_account", EntityID: id.String(),
			Payload: map[string]any{
				"broker": a.Broker, "client_code": a.ClientCode, "static_ip": a.StaticIP,
				"data_entitled": a.DataEntitled, "ops_cap": p.OpsCap,
			},
		})
		return err
	})
	if err != nil {
		return "", err
	}
	return id.String(), nil
}

func (a NewAccount) params() (store.CreateBrokerAccountParams, error) {
	p := store.CreateBrokerAccountParams{
		Broker: store.BrokerKind(a.Broker), ClientCode: strings.TrimSpace(a.ClientCode), HolderName: strings.TrimSpace(a.HolderName),
		DataEntitled: a.DataEntitled, OpsCap: 8,
	}
	if a.OpsCap != 0 {
		if a.OpsCap < 1 || a.OpsCap > MaxOpsCap {
			return p, fmt.Errorf("ops cap must be 1..%d (below the 10 OPS threshold)", MaxOpsCap)
		}
		p.OpsCap = int32(a.OpsCap) //nolint:gosec // G115: bounded to 1..MaxOpsCap just above
	}
	switch {
	case p.Broker != store.BrokerKindZerodha && p.Broker != store.BrokerKindUpstox && p.Broker != store.BrokerKindSim:
		return p, errors.New("broker must be zerodha, upstox or sim")
	case p.ClientCode == "" || p.HolderName == "":
		return p, errors.New("client code and holder name are required")
	}
	if p.Broker == store.BrokerKindSim {
		return p, nil
	}
	if a.APIKey == "" || a.APISecret == "" {
		return p, errors.New("API key and secret are required for a broker account")
	}
	// Placeholders satisfy the table's CHECK until the sealed values (bound to the new id) replace them.
	p.ApiKeyEnc, p.ApiSecretEnc = []byte{0}, []byte{0}
	for _, f := range []struct {
		name     string
		raw      string
		dst      **netip.Addr
		required bool
	}{
		{"static IP", a.StaticIP, &p.StaticIp, true},
		{"public IP", a.PublicIP, &p.PublicStaticIp, false},
		{"backup IP", a.BackupIP, &p.BackupStaticIp, false},
	} {
		if f.raw == "" {
			if f.required {
				return p, errors.New("static IP is required for a broker account (invariant 4)")
			}
			continue
		}
		ip, err := netip.ParseAddr(f.raw)
		if err != nil {
			return p, fmt.Errorf("%s %q is not an IP address", f.name, f.raw)
		}
		if !pinsOneHost(ip) {
			return p, fmt.Errorf("%s %q must name one address of this host", f.name, f.raw)
		}
		*f.dst = &ip
	}
	return p, nil
}

// pinsOneHost reports whether binding to ip pins the source address (invariant 4). A wildcard or multicast
// address doesn't, including its IPv4-mapped form: ::ffff:0.0.0.0 binds and lets the OS choose. The same
// rule is on the database (migration 000007).
func pinsOneHost(ip netip.Addr) bool {
	ip = ip.Unmap().WithZone("")
	return ip.IsValid() && !ip.IsUnspecified() && !ip.IsMulticast()
}

// ErrNoLiveToken means the account has no unexpired access token: the user must do the daily login.
var ErrNoLiveToken = errors.New("brokers: no live access token: do the daily broker login first")

// OpenAccessToken decrypts an account's current access token for a local job (vyuhactl download run)
// and audits it; account is an id or broker:client_code. It returns the account id and the token, which
// the caller must zero after use (see gateway/CLAUDE.md).
func OpenAccessToken(ctx context.Context, db store.DBTX, env *vcrypto.Envelope, account, requestedBy, purpose string, now time.Time) (string, []byte, error) {
	q := store.New(db)
	aid, err := accountID(ctx, q, account)
	if err != nil {
		return "", nil, err
	}
	acct, err := q.GetBrokerAccount(ctx, aid)
	if err != nil {
		return "", nil, fmt.Errorf("brokers: account %s: %w", account, err)
	}
	if acct.Status == store.AccountStatusDisabled {
		return "", nil, fmt.Errorf("brokers: account %s is disabled", account)
	}
	if acct.AccessTokenEnc == nil || !acct.TokenExpiresAt.Valid || !now.Before(acct.TokenExpiresAt.Time) {
		return "", nil, ErrNoLiveToken
	}
	token, err := env.Open(acct.AccessTokenEnc, ad("access_token_enc", acct.ID))
	if err != nil {
		return "", nil, fmt.Errorf("brokers: access token: %w", err)
	}
	if _, err := audit.Record(ctx, q, audit.Event{
		ActorKind: audit.ActorSystem, Action: "broker.token.opened", Entity: "broker_account", EntityID: acct.ID.String(),
		Payload: map[string]any{"requested_by": requestedBy, "purpose": purpose},
	}); err != nil {
		clear(token)
		return "", nil, fmt.Errorf("brokers: audit token use: %w", err)
	}
	return acct.ID.String(), token, nil
}

func accountID(ctx context.Context, q *store.Queries, account string) (pgtype.UUID, error) {
	var aid pgtype.UUID
	if broker, client, ok := strings.Cut(account, ":"); ok {
		id, err := q.FindBrokerAccount(ctx, store.FindBrokerAccountParams{Broker: store.BrokerKind(broker), ClientCode: client})
		if err != nil {
			return aid, fmt.Errorf("brokers: account %s: %w", account, err)
		}
		return id, nil
	}
	if err := aid.Scan(account); err != nil {
		return aid, fmt.Errorf("brokers: account must be an id or broker:client_code")
	}
	return aid, nil
}

// MapUser lets a user view (and optionally trade) an account; account is an id or broker:client_code.
func MapUser(ctx context.Context, db DB, account, username string, canTrade, isDefault bool) error {
	return pgx.BeginFunc(ctx, db, func(tx pgx.Tx) error {
		q := store.New(tx)
		aid, err := accountID(ctx, q, account)
		if err != nil {
			return err
		}
		u, err := q.GetUserByUsername(ctx, username)
		if err != nil {
			return fmt.Errorf("brokers: user %s: %w", username, err)
		}
		if err := q.UpsertAccountUser(ctx, store.UpsertAccountUserParams{AccountID: aid, UserID: u.ID, CanTrade: canTrade, IsDefault: isDefault}); err != nil {
			return fmt.Errorf("brokers: map user: %w", err)
		}
		_, err = audit.Record(ctx, q, audit.Event{
			ActorKind: audit.ActorSystem, Action: "account_user_map.set", Entity: "broker_account", EntityID: aid.String(),
			Payload: map[string]any{"user_id": u.ID.String(), "can_view": true, "can_trade": canTrade, "is_default": isDefault},
		})
		return err
	})
}
