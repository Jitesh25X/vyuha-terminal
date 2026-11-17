package audit

import (
	"context"
	"os"
	"path/filepath"
	"testing"

	"github.com/jackc/pgx/v5/pgxpool"

	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/testdb"
)

// Tests of migration 000013 (limit.seed audit events), run against a database that testdb migrated fully.

func seedSQL(t *testing.T) string {
	t.Helper()
	b, err := os.ReadFile(filepath.Join("..", "..", "..", "db", "migrations", "000013_risk_limit_seed_audit.up.sql"))
	if err != nil {
		t.Fatal(err)
	}
	return string(b)
}

func countRows(t *testing.T, pool *pgxpool.Pool, sql string, args ...any) int64 {
	t.Helper()
	var n int64
	if err := pool.QueryRow(context.Background(), sql, args...).Scan(&n); err != nil {
		t.Fatalf("%s: %v", sql, err)
	}
	return n
}

func TestLimitSeedEventsContent(t *testing.T) {
	_, pool := testdb.New(t)
	ctx := context.Background()
	const q = `SELECT count(*) FROM risk_limit r JOIN audit_event a ON a.entity_id = r.id::text AND a.action = 'limit.seed'
		WHERE a.actor_kind = 'system' AND a.actor_id IS NULL AND a.entity = 'risk_limit'
		  AND a.payload->>'key' = r.key AND a.payload->>'value' = r.value::text
		  AND a.payload->>'scope' = 'desk' AND (a.payload->>'hard')::boolean = true
		  AND a.payload->>'source' = 'migration 000011'`
	if n := countRows(t, pool, q); n != 24 {
		t.Fatalf("%d of 24 seeded limits have a fully matching limit.seed event", n)
	}
	if n := countRows(t, pool, `SELECT count(*) FROM risk_limit WHERE scope = 'desk' AND scope_id IS NULL AND updated_by IS NULL`); n != 24 {
		t.Fatalf("%d seeded limits, want 24", n)
	}
	for key, want := range map[string]string{"max_order_value": "500000.0000", "ops_cap": "8.0000"} {
		var got string
		if err := pool.QueryRow(ctx, `SELECT payload->>'value' FROM audit_event WHERE action = 'limit.seed' AND payload->>'key' = $1`, key).Scan(&got); err != nil || got != want {
			t.Fatalf("%s: value %q (%v), want %q", key, got, err, want)
		}
	}
}

func TestLimitSeedRerunAddsNothing(t *testing.T) {
	_, pool := testdb.New(t)
	ctx := context.Background()
	before := countRows(t, pool, `SELECT count(*) FROM audit_event`)
	exec(t, pool, seedSQL(t))
	if after := countRows(t, pool, `SELECT count(*) FROM audit_event`); after != before {
		t.Fatalf("re-running the up SQL added %d rows", after-before)
	}
	if r := mustVerify(t, pool); !r.OK || r.RowsChecked != before {
		t.Fatalf("chain after re-run: %+v", r)
	}
	_ = ctx
}

func TestLimitSeedSkipsEditedAndNonSeedLimits(t *testing.T) {
	_, pool := testdb.New(t)
	ctx := context.Background()
	exec(t, pool, `INSERT INTO app_user (username, display_name, role, password_hash) VALUES ('seed_tester', 'T', 'admin', 'x')`)

	// A user-edited seed row keeps its single event; executing the up SQL again adds none.
	var editedID string
	if err := pool.QueryRow(ctx, `UPDATE risk_limit SET updated_by = (SELECT id FROM app_user WHERE username = 'seed_tester'), value = 600000
		WHERE scope = 'desk' AND scope_id IS NULL AND key = 'max_order_value' RETURNING id::text`).Scan(&editedID); err != nil {
		t.Fatal(err)
	}
	const perEntity = `SELECT count(*) FROM audit_event WHERE action = 'limit.seed' AND entity_id = $1`
	if n := countRows(t, pool, perEntity, editedID); n != 1 {
		t.Fatalf("edited limit has %d limit.seed events before rerun, want 1", n)
	}
	before := countRows(t, pool, `SELECT count(*) FROM audit_event`)

	// A new user-attributed desk limit under a key outside the seed list is not a seed.
	var otherID string
	if err := pool.QueryRow(ctx, `INSERT INTO risk_limit (scope, scope_id, key, value, hard, updated_by)
		VALUES ('desk', NULL, 'test_only_key', 1, true, (SELECT id FROM app_user WHERE username = 'seed_tester')) RETURNING id::text`).Scan(&otherID); err != nil {
		t.Fatal(err)
	}
	exec(t, pool, seedSQL(t))
	if n := countRows(t, pool, perEntity, editedID); n != 1 {
		t.Fatalf("edited limit has %d limit.seed events after rerun, want 1", n)
	}
	if n := countRows(t, pool, perEntity, otherID); n != 0 {
		t.Fatalf("user-attributed limit got %d limit.seed events, want 0", n)
	}
	if after := countRows(t, pool, `SELECT count(*) FROM audit_event`); after != before {
		t.Fatalf("rerun added %d rows", after-before)
	}
	if r := mustVerify(t, pool); !r.OK {
		t.Fatalf("chain: %+v", r)
	}
}
