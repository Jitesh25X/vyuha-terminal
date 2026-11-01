package recorder

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/nats-io/nats.go"
	"google.golang.org/protobuf/proto"

	mdv1 "github.com/Jitesh25X/vyuha-terminal/gateway/gen/vyuha/marketdata/v1"
	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/chtest"
)

// F-MD-4 AC1 end to end on the dev stack (`make up PROFILES="core md"`): every published tick
// lands in ClickHouse. Uses a throwaway ClickHouse database built from db/clickhouse/*.sql.
func TestEveryPublishedTickIsRecorded(t *testing.T) {
	natsURL, chURL := os.Getenv("VYUHA_TEST_NATS_URL"), os.Getenv("VYUHA_TEST_CLICKHOUSE_URL")
	if natsURL == "" || chURL == "" {
		t.Skip("VYUHA_TEST_NATS_URL / VYUHA_TEST_CLICKHOUSE_URL not set")
	}
	ctx := context.Background()
	conn, db, _ := chtest.New(t)
	var b [4]byte
	_, _ = rand.Read(b[:])
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
