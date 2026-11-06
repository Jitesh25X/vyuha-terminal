// Package chtest gives integration tests a throwaway ClickHouse database built from
// db/clickhouse/*.sql and dropped when the test ends. Import it from _test.go files only.
package chtest

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"github.com/ClickHouse/clickhouse-go/v2"
	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/testenv"
)

// New returns a connection to a fresh database, its name and a DSN pointing at it. It skips the
// test unless VYUHA_TEST_CLICKHOUSE_URL is set (make up PROFILES="core md").
func New(t testing.TB) (clickhouse.Conn, string, string) {
	t.Helper()
	dsn := os.Getenv("VYUHA_TEST_CLICKHOUSE_URL")
	if dsn == "" {
		testenv.SkipWithoutService(t, "VYUHA_TEST_CLICKHOUSE_URL not set")
	}
	var b [4]byte
	_, _ = rand.Read(b[:])
	db := "vyuha_test_" + hex.EncodeToString(b[:])
	admin := open(t, dsn, "default")
	createSchema(t, admin, db)
	t.Cleanup(func() { _ = admin.Exec(context.Background(), "DROP DATABASE IF EXISTS "+db); _ = admin.Close() })
	conn := open(t, dsn, db)
	t.Cleanup(func() { _ = conn.Close() })
	opts, _ := clickhouse.ParseDSN(dsn)
	u := "clickhouse://" + opts.Auth.Username + ":" + opts.Auth.Password + "@" + strings.Join(opts.Addr, ",") + "/" + db
	return conn, db, u
}

func open(t testing.TB, dsn, db string) clickhouse.Conn {
	t.Helper()
	opts, err := clickhouse.ParseDSN(dsn)
	if err != nil {
		t.Fatal(err)
	}
	opts.Auth.Database = db
	conn, err := clickhouse.Open(opts)
	if err != nil {
		t.Fatal(err)
	}
	return conn
}

// createSchema applies db/clickhouse/*.sql into `db` instead of `vyuha`.
func createSchema(t testing.TB, conn clickhouse.Conn, db string) {
	t.Helper()
	_, file, _, _ := runtime.Caller(0)
	files, _ := filepath.Glob(filepath.Join(filepath.Dir(file), "..", "..", "..", "db", "clickhouse", "*.sql"))
	if len(files) == 0 {
		t.Fatal("no ClickHouse DDL found")
	}
	for _, f := range files {
		raw, err := os.ReadFile(f) //nolint:gosec // G304: repo-relative test fixture
		if err != nil {
			t.Fatal(err)
		}
		sql := strings.ReplaceAll(string(raw), "vyuha.", db+".")
		sql = strings.ReplaceAll(sql, "DATABASE IF NOT EXISTS vyuha", "DATABASE IF NOT EXISTS "+db)
		for _, stmt := range strings.Split(stripComments(sql), ";") {
			if strings.TrimSpace(stmt) == "" {
				continue
			}
			if err := conn.Exec(context.Background(), stmt); err != nil {
				t.Fatalf("%s: %v", f, err)
			}
		}
	}
}

func stripComments(sql string) string {
	var out []string
	for _, line := range strings.Split(sql, "\n") {
		if i := strings.Index(line, "--"); i >= 0 {
			line = line[:i]
		}
		out = append(out, line)
	}
	return strings.Join(out, "\n")
}
