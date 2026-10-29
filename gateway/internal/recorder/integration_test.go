package recorder

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"

	"github.com/ClickHouse/clickhouse-go/v2"
	"github.com/nats-io/nats.go"
	"google.golang.org/protobuf/proto"

	mdv1 "github.com/jiteshraj/vyuha/gateway/gen/vyuha/marketdata/v1"
)

// F-MD-4 AC1 end to end on the dev stack (`make up PROFILES="core md"`): every published tick
// lands in ClickHouse. Uses a throwaway ClickHouse database built from db/clickhouse/*.sql.
func TestEveryPublishedTickIsRecorded(t *testing.T) {
	natsURL, chURL := os.Getenv("VYUHA_TEST_NATS_URL"), os.Getenv("VYUHA_TEST_CLICKHOUSE_URL")
	if natsURL == "" || chURL == "" {
		t.Skip("VYUHA_TEST_NATS_URL / VYUHA_TEST_CLICKHOUSE_URL not set")
	}
	ctx := context.Background()
	var b [4]byte
	_, _ = rand.Read(b[:])
	db := "vyuha_rtest_" + hex.EncodeToString(b[:])
	admin := openCH(t, chURL)
	createSchema(t, admin, db)
	t.Cleanup(func() { _ = admin.Exec(context.Background(), "DROP DATABASE IF EXISTS "+db); _ = admin.Close() })

	opts, err := clickhouse.ParseDSN(chURL)
	if err != nil {
		t.Fatal(err)
	}
	opts.Auth.Database = db
	conn, err := clickhouse.Open(opts)
	if err != nil {
		t.Fatal(err)
	}
	sink := &ClickHouse{conn: conn, database: db}
	t.Cleanup(func() { _ = sink.Close() })

	rec := New(Config{QueueSize: 200_000, BatchSize: 10_000, FlushInterval: 200 * time.Millisecond, MaxRetries: 5,
		RetryBackoff: 50 * time.Millisecond}, sink, quiet())
	runCtx, cancel := context.WithCancel(ctx)
	done := make(chan struct{})
	go func() { rec.Run(runCtx); close(done) }()

	// A unique subject root keeps other publishers on the dev NATS out of this test's count.
	nc, err := nats.Connect(natsURL)
	if err != nil {
		t.Fatal(err)
	}
	defer nc.Close()
	root := "md.tick." + hex.EncodeToString(b[:]) + "."
	sub, err := nc.Subscribe(root+">", func(m *nats.Msg) { rec.Handle("md.tick."+strings.TrimPrefix(m.Subject, root), m.Data) })
	if err != nil {
		t.Fatal(err)
	}
	if err := sub.SetPendingLimits(-1, -1); err != nil {
		t.Fatal(err)
	}
	if err := nc.Flush(); err != nil {
		t.Fatal(err)
	}

	const n = 50_000
	pub, err := nats.Connect(natsURL)
	if err != nil {
		t.Fatal(err)
	}
	defer pub.Close()
	base := time.Date(2026, 10, 28, 3, 45, 0, 0, time.UTC).UnixNano()
	for i := range n {
		data, _ := proto.Marshal(&mdv1.Tick{InstrumentId: uint64(i%100 + 1), ExchangeTsNs: base + int64(i)*1_000_000,
			RecvTsNs: base + int64(i)*1_000_000 + 500, Ltp: 245_005_000 + int64(i), Volume: uint64(i)})
		if err := pub.Publish(root+"1", data); err != nil {
			t.Fatal(err)
		}
	}
	if err := pub.Flush(); err != nil {
		t.Fatal(err)
	}
	waitFor(t, func() bool { return rec.Stats().Received == n })
	cancel()
	<-done

	var rows uint64
	if err := conn.QueryRow(ctx, "SELECT count() FROM "+db+".ticks").Scan(&rows); err != nil {
		t.Fatal(err)
	}
	s := rec.Stats()
	if rows != n || s.InsertedTicks != n || s.Dropped != 0 || s.LostRows != 0 || s.InsertErrors != 0 {
		t.Fatalf("published %d, recorded %d rows (stats %+v)", n, rows, s)
	}
	var ltp int64
	if err := conn.QueryRow(ctx, "SELECT ltp FROM "+db+".ticks ORDER BY exchange_ts DESC LIMIT 1").Scan(&ltp); err != nil {
		t.Fatal(err)
	}
	if ltp != 245_005_000+n-1 {
		t.Fatalf("last ltp %d", ltp)
	}
}

func openCH(t *testing.T, dsn string) clickhouse.Conn {
	t.Helper()
	opts, err := clickhouse.ParseDSN(dsn)
	if err != nil {
		t.Fatal(err)
	}
	opts.Auth.Database = "default"
	conn, err := clickhouse.Open(opts)
	if err != nil {
		t.Fatal(err)
	}
	return conn
}

// createSchema applies db/clickhouse/*.sql into `db` instead of `vyuha`.
func createSchema(t *testing.T, conn clickhouse.Conn, db string) {
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
