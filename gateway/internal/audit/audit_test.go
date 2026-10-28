package audit

import (
	"context"
	"errors"
	"net/netip"
	"strconv"
	"strings"
	"sync"
	"testing"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
	"github.com/jackc/pgx/v5/pgxpool"

	"github.com/jiteshraj/vyuha/gateway/internal/store"
	"github.com/jiteshraj/vyuha/gateway/internal/testdb"
)

// The SQL below deliberately attacks audit_event. Each tampering statement disables the
// audit_no_update trigger (or tests audit_immutable / the REVOKEd grants) on the same line,
// which is how scripts/check-invariants.sh tells these tests apart from application code.

func ptr(s string) *string { return &s }

func TestCanonicalFieldBoundaries(t *testing.T) {
	prev := []byte{0}
	a := rowHash(prev, "ts", nil, "user", "auth.login|x", "app_user", nil, ptr("{}"), nil)
	b := rowHash(prev, "ts", nil, "user", "auth.login", "x|app_user", nil, ptr("{}"), nil)
	if string(a) == string(b) {
		t.Fatal("moving text across a field boundary must change the hash")
	}
	if string(rowHash(prev, "ts", nil, "user", "a.b", "e", nil, nil, nil)) == string(rowHash(prev, "ts", nil, "user", "a.b", "e", ptr(""), nil, nil)) {
		t.Fatal("NULL and empty string must hash differently")
	}
}

func TestRecordRejectsBadEvents(t *testing.T) {
	tests := map[string]Event{
		"secret key":         {ActorKind: ActorSystem, Action: "user.create", Entity: "app_user", Payload: map[string]any{"password": "x"}},
		"nested secret":      {ActorKind: ActorSystem, Action: "user.create", Entity: "app_user", Payload: map[string]any{"a": map[string]any{"mfa_token": "x"}}},
		"secret in list":     {ActorKind: ActorSystem, Action: "user.create", Entity: "app_user", Payload: map[string]any{"a": []any{map[string]any{"api_key": "x"}}}},
		"bad action":         {ActorKind: ActorSystem, Action: "Login", Entity: "app_user"},
		"unknown actor kind": {ActorKind: "robot", Action: "user.create", Entity: "app_user"},
		"no entity":          {ActorKind: ActorSystem, Action: "user.create"},
	}
	for name, e := range tests {
		t.Run(name, func(t *testing.T) {
			if _, err := Record(context.Background(), nil, e); !errors.Is(err, ErrInvalidEvent) {
				t.Fatalf("want ErrInvalidEvent, got %v", err)
			}
		})
	}
}

func TestParseAnchor(t *testing.T) {
	good := "42:" + strings.Repeat("ab", 32)
	if a, err := ParseAnchor(good); err != nil || a.ID != 42 || len(a.Hash) != 32 {
		t.Fatalf("ParseAnchor(%q) = %+v, %v", good, a, err)
	}
	for _, bad := range []string{"", "42", "x:" + strings.Repeat("ab", 32), "42:abcd", "42:" + strings.Repeat("zz", 32)} {
		if _, err := ParseAnchor(bad); err == nil {
			t.Errorf("ParseAnchor(%q) accepted", bad)
		}
	}
}

// seed writes a varied set of events, including field text containing '|' and ':', unicode,
// nested payloads, NULL actor/entity id and both IP families.
func seed(t *testing.T, pool *pgxpool.Pool) []int64 {
	t.Helper()
	q := store.New(pool)
	events := []Event{
		{ActorKind: ActorSystem, Action: "user.create", Entity: "app_user", EntityID: "u1", Payload: map[string]any{"username": "meera", "role": "trader"}},
		{ActorID: "6f1c2a3e-0b7d-4e59-9c1a-2d3e4f5a6b7c", ActorKind: ActorUser, Action: "auth.login.success", Entity: "app_user", EntityID: "6f1c2a3e-0b7d-4e59-9c1a-2d3e4f5a6b7c", IP: netip.MustParseAddr("10.1.2.3")},
		{ActorKind: ActorUser, Action: "auth.login.failure", Entity: "app_user", Payload: map[string]any{"username": "उपयोगकर्ता", "step": "password"}, IP: netip.MustParseAddr("2001:db8::1")},
		{ActorKind: ActorEngine, Action: "limit.update", Entity: "risk_limit", EntityID: "a|b:c", Payload: map[string]any{"old": "25000.0000", "new": "30000.0000", "nested": map[string]any{"k": []any{1, "two", nil}}}},
		{ActorKind: ActorCopilot, Action: "copilot.tool_call", Entity: "copilot_session", Payload: map[string]any{"tool": "get_positions", "note": "pipes | and colons : inside"}},
	}
	ids := make([]int64, 0, len(events))
	for _, e := range events {
		id, err := Record(context.Background(), q, e)
		if err != nil {
			t.Fatal(err)
		}
		ids = append(ids, id)
	}
	return ids
}

func mustVerify(t *testing.T, pool *pgxpool.Pool, anchors ...Anchor) Result {
	t.Helper()
	r, err := Verify(context.Background(), pool, anchors...)
	if err != nil {
		t.Fatal(err)
	}
	return r
}

func exec(t *testing.T, pool *pgxpool.Pool, sql string, args ...any) {
	t.Helper()
	if _, err := pool.Exec(context.Background(), sql, args...); err != nil {
		t.Fatalf("%s: %v", sql, err)
	}
}

func TestChainVerifies(t *testing.T) {
	_, pool := testdb.New(t)
	ids := seed(t, pool)
	r := mustVerify(t, pool)
	if !r.OK || r.RowsChecked != int64(len(ids)) || r.HeadID != ids[len(ids)-1] || len(r.HeadHash) != 32 {
		t.Fatalf("unexpected result: %+v", r)
	}
	for i := 1; i < len(ids); i++ {
		if ids[i] != ids[i-1]+1 {
			t.Fatalf("ids not consecutive: %v", ids)
		}
	}
	if r := mustVerify(t, pool, Anchor{ID: r.HeadID, Hash: r.HeadHash}); !r.OK {
		t.Fatalf("anchored verify failed: %+v", r)
	}
}

// Ids are assigned under the chain lock, so concurrent writers still chain in id order.
func TestConcurrentWritersKeepChainOrder(t *testing.T) {
	_, pool := testdb.New(t)
	q := store.New(pool)
	var wg sync.WaitGroup
	errs := make(chan error, 50)
	for i := 0; i < 50; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			_, err := Record(context.Background(), q, Event{ActorKind: ActorSystem, Action: "test.concurrent", Entity: "test"})
			errs <- err
		}()
	}
	wg.Wait()
	close(errs)
	for err := range errs {
		if err != nil {
			t.Fatal(err)
		}
	}
	if r := mustVerify(t, pool); !r.OK || r.RowsChecked != 50 {
		t.Fatalf("unexpected result: %+v", r)
	}
}

func TestTamperDetected(t *testing.T) {
	const off = "ALTER TABLE audit_event DISABLE TRIGGER audit_no_update; "
	tests := []struct {
		name   string
		tamper func(t *testing.T, pool *pgxpool.Pool, ids []int64) int64 // returns the id expected to be reported
		reason string
	}{
		{"payload modified", func(t *testing.T, p *pgxpool.Pool, ids []int64) int64 {
			exec(t, p, off+`UPDATE audit_event SET payload = '{"old": "25000.0000", "new": "99999.0000"}' WHERE id = `+itoa(ids[3])+`; -- audit_no_update off`)
			return ids[3]
		}, "hash mismatch"},
		{"actor kind rewritten (v1 hash missed this)", func(t *testing.T, p *pgxpool.Pool, ids []int64) int64 {
			exec(t, p, off+`UPDATE audit_event SET actor_kind = 'system' WHERE id = `+itoa(ids[1])+`; -- audit_no_update off`)
			return ids[1]
		}, "hash mismatch"},
		{"actor id rewritten (v1 hash missed this)", func(t *testing.T, p *pgxpool.Pool, ids []int64) int64 {
			exec(t, p, off+`UPDATE audit_event SET actor_id = '00000000-0000-4000-8000-000000000000' WHERE id = `+itoa(ids[1])+`; -- audit_no_update off`)
			return ids[1]
		}, "hash mismatch"},
		{"timestamp moved", func(t *testing.T, p *pgxpool.Pool, ids []int64) int64 {
			exec(t, p, off+`UPDATE audit_event SET ts = ts - interval '1 hour' WHERE id = `+itoa(ids[0])+`; -- audit_no_update off`)
			return ids[0]
		}, "hash mismatch"},
		{"ip changed", func(t *testing.T, p *pgxpool.Pool, ids []int64) int64 {
			exec(t, p, off+`UPDATE audit_event SET ip = '10.9.9.9' WHERE id = `+itoa(ids[1])+`; -- audit_no_update off`)
			return ids[1]
		}, "hash mismatch"},
		{"text shifted across fields (v1 '|' join missed this)", func(t *testing.T, p *pgxpool.Pool, ids []int64) int64 {
			exec(t, p, off+`UPDATE audit_event SET action = 'limit', entity = 'update|risk_limit' WHERE id = `+itoa(ids[3])+`; -- audit_no_update off`)
			return ids[3]
		}, "hash mismatch"},
		{"middle row deleted", func(t *testing.T, p *pgxpool.Pool, ids []int64) int64 {
			exec(t, p, off+`DELETE FROM audit_event WHERE id = `+itoa(ids[2])+`; -- audit_no_update off`)
			return ids[3]
		}, "chain broken"},
		{"forged row inserted with chain trigger off", func(t *testing.T, p *pgxpool.Pool, ids []int64) int64 {
			exec(t, p, `ALTER TABLE audit_event DISABLE TRIGGER audit_chain_trg; INSERT INTO audit_event (id, actor_kind, action, entity, payload, prev_hash, hash)
				SELECT 999, 'system', 'limit.update', 'risk_limit', '{}', hash, sha256('forged') FROM audit_event ORDER BY id DESC LIMIT 1`)
			return 999
		}, "hash mismatch"},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			_, pool := testdb.New(t)
			ids := seed(t, pool)
			if r := mustVerify(t, pool); !r.OK {
				t.Fatalf("clean chain failed: %+v", r)
			}
			wantID := tt.tamper(t, pool, ids)
			r := mustVerify(t, pool)
			if r.OK || r.FirstBadID != wantID || !strings.HasPrefix(r.Reason, tt.reason) {
				t.Fatalf("got %+v, want first bad id %d (%s)", r, wantID, tt.reason)
			}
		})
	}
}

// The chain alone can't see the newest rows disappear; an external anchor (vyuhactl audit head) can.
func TestNewestRowDeletionNeedsAnchor(t *testing.T) {
	_, pool := testdb.New(t)
	ids := seed(t, pool)
	head := mustVerify(t, pool)
	anchor := Anchor{ID: head.HeadID, Hash: head.HeadHash}

	exec(t, pool, "ALTER TABLE audit_event DISABLE TRIGGER audit_no_update; DELETE FROM audit_event WHERE id = "+itoa(ids[len(ids)-1])+"; -- audit_no_update off")
	if r := mustVerify(t, pool); !r.OK {
		t.Fatalf("without an anchor the truncated chain is still self-consistent: %+v", r)
	}
	r := mustVerify(t, pool, anchor)
	if r.OK || r.FirstBadID != anchor.ID || !strings.Contains(r.Reason, "anchored row missing") {
		t.Fatalf("anchor did not catch tail deletion: %+v", r)
	}
}

func TestAppendOnlyTrigger(t *testing.T) {
	_, pool := testdb.New(t)
	ids := seed(t, pool)
	for _, sql := range []string{
		"UPDATE audit_event SET action = 'x.y' WHERE id = $1 -- must be stopped by audit_immutable",
		"DELETE FROM audit_event WHERE id = $1 -- must be stopped by audit_immutable",
	} {
		_, err := pool.Exec(context.Background(), sql, ids[0])
		if err == nil || !strings.Contains(err.Error(), "append-only") {
			t.Fatalf("%q: want append-only error, got %v", sql, err)
		}
	}
}

// Snapshot isolation could make the trigger miss the latest row and fork the chain; it is refused.
func TestInsertRequiresReadCommitted(t *testing.T) {
	_, pool := testdb.New(t)
	seed(t, pool)
	ctx := context.Background()
	for _, level := range []pgx.TxIsoLevel{pgx.RepeatableRead, pgx.Serializable} {
		tx, err := pool.BeginTx(ctx, pgx.TxOptions{IsoLevel: level})
		if err != nil {
			t.Fatal(err)
		}
		_, err = Record(ctx, store.New(tx), Event{ActorKind: ActorSystem, Action: "test.isolation", Entity: "test"})
		_ = tx.Rollback(ctx)
		if err == nil || !strings.Contains(err.Error(), "READ COMMITTED") {
			t.Fatalf("%s: want isolation error, got %v", level, err)
		}
	}
	if r := mustVerify(t, pool); !r.OK {
		t.Fatalf("chain damaged: %+v", r)
	}
}

// PRD F-CO-1: the service role (vyuha_app) may insert audit rows but never update, delete or truncate them.
func TestServiceRoleIsInsertOnly(t *testing.T) {
	_, pool := testdb.New(t)
	seed(t, pool)
	ctx := context.Background()
	conn, err := pool.Acquire(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Release()
	tx, err := conn.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	if _, err := tx.Exec(ctx, "SET LOCAL ROLE vyuha_app"); err != nil {
		t.Fatal(err)
	}
	if _, err := Record(ctx, store.New(tx), Event{ActorKind: ActorSystem, Action: "test.insert", Entity: "test"}); err != nil {
		t.Fatalf("vyuha_app INSERT: %v", err)
	}
	for _, sql := range []string{
		"SAVEPOINT s; UPDATE audit_event SET action = 'x.y' -- REVOKEd from vyuha_app",
		"SAVEPOINT s; DELETE FROM audit_event -- REVOKEd from vyuha_app",
		"SAVEPOINT s; TRUNCATE audit_event -- REVOKEd from vyuha_app",
	} {
		_, err := tx.Exec(ctx, sql)
		var pgErr *pgconn.PgError
		if !errors.As(err, &pgErr) || pgErr.Code != "42501" {
			t.Fatalf("%q: want permission denied (42501), got %v", sql, err)
		}
		if _, err := tx.Exec(ctx, "ROLLBACK TO SAVEPOINT s"); err != nil {
			t.Fatal(err)
		}
	}
}

func itoa(n int64) string { return strconv.FormatInt(n, 10) }
