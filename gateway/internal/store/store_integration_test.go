package store

import (
	"context"
	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/testenv"
	"os"
	"testing"
	"time"
)

// Runs against a real Postgres when VYUHA_TEST_POSTGRES_URL is set (make test-gateway sets it
// when the dev stack is up). Migrations must already be applied by the same runner; Up is a no-op.
func TestStoreIntegration(t *testing.T) {
	url := os.Getenv("VYUHA_TEST_POSTGRES_URL")
	if url == "" {
		testenv.SkipWithoutService(t, "VYUHA_TEST_POSTGRES_URL not set")
	}
	const dir = "../../../db/migrations"

	v, err := MigrateUp(dir, url)
	if err != nil {
		t.Fatalf("MigrateUp: %v", err)
	}
	gotV, dirty, err := MigrationVersion(dir, url)
	if err != nil || dirty || gotV != v || v < 2 {
		t.Fatalf("version = %d dirty=%t err=%v (after up: %d)", gotV, dirty, err, v)
	}

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	pool, err := NewPool(ctx, url)
	if err != nil {
		t.Fatal(err)
	}
	defer pool.Close()
	now, err := New(pool).DatabaseNow(ctx)
	if err != nil {
		t.Fatalf("DatabaseNow: %v", err)
	}
	if skew := time.Since(now.Time); skew > 5*time.Second || skew < -5*time.Second {
		t.Fatalf("database clock skew %v", skew)
	}
}

func TestPgx5URL(t *testing.T) {
	tests := map[string]string{
		"postgres://u:p@h:5432/d?sslmode=disable": "pgx5://u:p@h:5432/d?sslmode=disable",
		"postgresql://u@h/d":                      "pgx5://u@h/d",
		"pgx5://u@h/d":                            "pgx5://u@h/d",
	}
	for in, want := range tests {
		if got := pgx5URL(in); got != want {
			t.Errorf("pgx5URL(%q) = %q, want %q", in, got, want)
		}
	}
}
