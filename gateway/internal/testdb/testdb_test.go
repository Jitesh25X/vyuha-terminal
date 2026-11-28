package testdb

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"os"
	"path/filepath"
	"runtime"
	"testing"
	"time"

	"github.com/jackc/pgx/v5"

	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/testenv"
)

// fakeTB records what New does with a test: Fatalf stops the calling goroutine, like the real one.
type fakeTB struct {
	testing.TB // anything New must not call panics on the nil interface
	cleanups   []func()
	failed     bool
}

func (f *fakeTB) Helper()               {}
func (f *fakeTB) Cleanup(fn func())     { f.cleanups = append(f.cleanups, fn) }
func (f *fakeTB) Errorf(string, ...any) { f.failed = true }
func (f *fakeTB) Fatalf(string, ...any) { f.failed = true; runtime.Goexit() }
func (f *fakeTB) runCleanups() {
	for i := len(f.cleanups) - 1; i >= 0; i-- {
		f.cleanups[i]()
	}
}

// A migration that fails after the database exists must still fail the test and drop the database.
func TestNewDropsTheDatabaseWhenMigrationFails(t *testing.T) {
	admin := os.Getenv("VYUHA_TEST_POSTGRES_URL")
	if admin == "" {
		testenv.SkipWithoutService(t, "VYUHA_TEST_POSTGRES_URL not set")
	}
	dir := t.TempDir()
	for name, sql := range map[string]string{
		"000001_broken.up.sql":   "THIS IS NOT SQL;",
		"000001_broken.down.sql": "SELECT 1;",
	} {
		if err := os.WriteFile(filepath.Join(dir, name), []byte(sql), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	var b [6]byte
	_, _ = rand.Read(b[:])
	name := "vyuha_test_leakcheck_" + hex.EncodeToString(b[:])
	oldPath, oldName := migrationsPath, newName
	migrationsPath, newName = func() string { return dir }, func() string { return name }
	t.Cleanup(func() { migrationsPath, newName = oldPath, oldName })

	exists := func() bool {
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		c, err := pgx.Connect(ctx, admin)
		if err != nil {
			t.Fatal(err)
		}
		defer func() { _ = c.Close(ctx) }()
		var found bool
		if err := c.QueryRow(ctx, "SELECT EXISTS (SELECT 1 FROM pg_database WHERE datname = $1)", name).Scan(&found); err != nil {
			t.Fatal(err)
		}
		return found
	}

	fake := &fakeTB{}
	done := make(chan struct{})
	go func() { // Fatalf ends this goroutine, as t.Fatalf would end the test's
		defer close(done)
		New(fake)
	}()
	<-done
	if !fake.failed {
		t.Fatal("a failed migration did not fail the test")
	}
	if len(fake.cleanups) == 0 {
		t.Fatal("no cleanup was registered before the migration ran")
	}
	if !exists() {
		t.Fatal("the database was not created: the test checked nothing")
	}
	fake.runCleanups()
	if exists() {
		t.Fatalf("database %s is left behind after a failed migration", name)
	}
}
