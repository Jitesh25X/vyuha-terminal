// An external test package: testdb imports store, so package store itself can't import testdb.
package store_test

import (
	"context"
	"testing"
	"time"

	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/store"
	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/testdb"
)

// Runs against a real Postgres when VYUHA_TEST_POSTGRES_URL is set (make test-gateway sets it when the dev
// stack is up). That URL is only used to create a throwaway database: testdb migrates it up from empty and drops
// it afterwards. No test migrates the dev database (D-57; scripts/check-invariants.sh enforces it).
func TestStoreIntegration(t *testing.T) {
	url, pool := testdb.New(t)

	v, dirty, err := store.MigrationVersion("../../../db/migrations", url)
	if err != nil || dirty || v < 2 {
		t.Fatalf("version = %d dirty=%t err=%v", v, dirty, err)
	}

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	now, err := store.New(pool).DatabaseNow(ctx)
	if err != nil {
		t.Fatalf("DatabaseNow: %v", err)
	}
	if skew := time.Since(now.Time); skew > 5*time.Second || skew < -5*time.Second {
		t.Fatalf("database clock skew %v", skew)
	}
}
