package brokers

import (
	"context"
	"errors"
	"fmt"
	"net/netip"
	"strings"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgtype"

	"github.com/jiteshraj/vyuha/gateway/internal/audit"
	vcrypto "github.com/jiteshraj/vyuha/gateway/internal/crypto"
	"github.com/jiteshraj/vyuha/gateway/internal/store"
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
		*f.dst = &ip
	}
	return p, nil
}

// MapUser lets a user view (and optionally trade) an account; account is an id or broker:client_code.
func MapUser(ctx context.Context, db DB, account, username string, canTrade, isDefault bool) error {
	return pgx.BeginFunc(ctx, db, func(tx pgx.Tx) error {
		q := store.New(tx)
		var aid pgtype.UUID
		if broker, client, ok := strings.Cut(account, ":"); ok {
			id, err := q.FindBrokerAccount(ctx, store.FindBrokerAccountParams{Broker: store.BrokerKind(broker), ClientCode: client})
			if err != nil {
				return fmt.Errorf("brokers: account %s: %w", account, err)
			}
			aid = id
		} else if err := aid.Scan(account); err != nil {
			return fmt.Errorf("brokers: account must be an id or broker:client_code")
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
