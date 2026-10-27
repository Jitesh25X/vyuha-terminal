// Package testdb gives integration tests a fresh, fully migrated Postgres database that is dropped
// when the test ends, so tests never leave rows (in particular append-only audit rows) in the dev DB.
// Import it from _test.go files only.
package testdb

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"net/url"
	"os"
	"path/filepath"
	"runtime"
	"testing"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"

	"github.com/jiteshraj/vyuha/gateway/internal/store"
)

// New returns the URL and a pool for a new database migrated to the latest version.
// It skips the test unless VYUHA_TEST_POSTGRES_URL (an account allowed to CREATE DATABASE) is set.
func New(t testing.TB) (string, *pgxpool.Pool) {
	t.Helper()
	admin := os.Getenv("VYUHA_TEST_POSTGRES_URL")
	if admin == "" {
		t.Skip("VYUHA_TEST_POSTGRES_URL not set")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	var b [6]byte
	_, _ = rand.Read(b[:])
	name := "vyuha_test_" + hex.EncodeToString(b[:])
	conn, err := pgx.Connect(ctx, admin)
	if err != nil {
		t.Fatalf("testdb: connect: %v", err)
	}
	defer conn.Close(ctx)
	if _, err := conn.Exec(ctx, "CREATE DATABASE "+pgx.Identifier{name}.Sanitize()); err != nil {
		t.Fatalf("testdb: create: %v", err)
	}

	u, err := url.Parse(admin)
	if err != nil {
		t.Fatalf("testdb: %v", err)
	}
	u.Path = "/" + name
	dbURL := u.String()
	if _, err := store.MigrateUp(migrationsDir(), dbURL); err != nil {
		t.Fatalf("testdb: migrate: %v", err)
	}
	pool, err := store.NewPool(ctx, dbURL)
	if err != nil {
		t.Fatalf("testdb: pool: %v", err)
	}
	// Registered after the caller's earlier cleanups, so it runs before them (LIFO): close, then drop.
	t.Cleanup(func() {
		pool.Close()
		ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
		defer cancel()
		c, err := pgx.Connect(ctx, admin)
		if err != nil {
			t.Errorf("testdb: drop connect: %v", err)
			return
		}
		defer c.Close(ctx)
		if _, err := c.Exec(ctx, "DROP DATABASE "+pgx.Identifier{name}.Sanitize()+" WITH (FORCE)"); err != nil {
			t.Errorf("testdb: drop: %v", err)
		}
	})
	return dbURL, pool
}

func migrationsDir() string {
	_, file, _, _ := runtime.Caller(0)
	return filepath.Join(filepath.Dir(file), "..", "..", "..", "db", "migrations")
}
