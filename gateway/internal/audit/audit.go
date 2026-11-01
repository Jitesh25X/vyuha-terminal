// Package audit writes and verifies the append-only, hash-chained audit_event log (PRD F-CO-1).
// It only ever INSERTs; the chain is computed by the audit_chain() trigger.
package audit

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/netip"
	"regexp"

	"github.com/jackc/pgx/v5/pgtype"

	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/store"
)

// ActorKind matches the audit_event.actor_kind CHECK constraint.
type ActorKind string

const (
	ActorUser     ActorKind = "user"
	ActorSystem   ActorKind = "system"
	ActorEngine   ActorKind = "engine"
	ActorStrategy ActorKind = "strategy"
	ActorCopilot  ActorKind = "copilot"
)

// Event is one audit record. Empty ActorID / EntityID are stored as NULL.
// Payload must never contain secrets: keys that look like credentials are rejected.
type Event struct {
	ActorID   string
	ActorKind ActorKind
	Action    string // dotted lower_snake, e.g. auth.login.success
	Entity    string // e.g. app_user
	EntityID  string
	Payload   map[string]any
	IP        netip.Addr
}

var (
	actionRe     = regexp.MustCompile(`^[a-z_]+(\.[a-z_]+)+$`)
	secretKeyRe  = regexp.MustCompile(`(?i)(password|passwd|secret|token|totp|otp|api_?key|master_?key|private)`)
	emptyPayload = map[string]any{}
)

// ErrInvalidEvent is returned for malformed events (bad action, unknown actor kind, secret-like keys).
var ErrInvalidEvent = errors.New("audit: invalid event")

// Record inserts e using q (pass a transaction-bound store.Queries to make the audit row atomic
// with the change it describes) and returns the assigned id.
func Record(ctx context.Context, q *store.Queries, e Event) (int64, error) {
	if !actionRe.MatchString(e.Action) || e.Entity == "" {
		return 0, fmt.Errorf("%w: action %q / entity %q", ErrInvalidEvent, e.Action, e.Entity)
	}
	switch e.ActorKind {
	case ActorUser, ActorSystem, ActorEngine, ActorStrategy, ActorCopilot:
	default:
		return 0, fmt.Errorf("%w: actor kind %q", ErrInvalidEvent, e.ActorKind)
	}
	payload := e.Payload
	if payload == nil {
		payload = emptyPayload
	}
	if err := checkKeys(payload); err != nil {
		return 0, err
	}
	body, err := json.Marshal(payload)
	if err != nil {
		return 0, fmt.Errorf("%w: payload: %w", ErrInvalidEvent, err)
	}

	params := store.InsertAuditEventParams{
		ActorKind: string(e.ActorKind), Action: e.Action, Entity: e.Entity, Payload: body,
		EntityID: pgtype.Text{String: e.EntityID, Valid: e.EntityID != ""},
	}
	if e.ActorID != "" {
		if err := params.ActorID.Scan(e.ActorID); err != nil {
			return 0, fmt.Errorf("%w: actor id: %w", ErrInvalidEvent, err)
		}
	}
	if e.IP.IsValid() {
		ip := e.IP
		params.Ip = &ip
	}
	id, err := q.InsertAuditEvent(ctx, params)
	if err != nil {
		return 0, fmt.Errorf("audit insert: %w", err)
	}
	return id, nil
}

// checkKeys rejects credential-looking keys anywhere in the payload (invariant #5).
func checkKeys(v any) error {
	switch t := v.(type) {
	case map[string]any:
		for k, child := range t {
			if secretKeyRe.MatchString(k) {
				return fmt.Errorf("%w: payload key %q looks like a secret", ErrInvalidEvent, k)
			}
			if err := checkKeys(child); err != nil {
				return err
			}
		}
	case []any:
		for _, child := range t {
			if err := checkKeys(child); err != nil {
				return err
			}
		}
	}
	return nil
}
