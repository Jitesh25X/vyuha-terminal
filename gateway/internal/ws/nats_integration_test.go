package ws

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"os"
	"testing"
	"time"

	"github.com/nats-io/nats.go"
	"github.com/nats-io/nats.go/jetstream"
	"google.golang.org/protobuf/proto"

	marketdatav1 "github.com/Jitesh25X/vyuha-terminal/gateway/gen/vyuha/marketdata/v1"
)

// NATSSource against the dev NATS (make up): the subjects and KV layout the engine publishes.
func TestNATSSource(t *testing.T) {
	natsURL := os.Getenv("VYUHA_TEST_NATS_URL")
	if natsURL == "" {
		t.Skip("VYUHA_TEST_NATS_URL not set")
	}
	nc, err := nats.Connect(natsURL)
	if err != nil {
		t.Fatal(err)
	}
	defer nc.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()

	var b [4]byte
	_, _ = rand.Read(b[:])
	bucket := "md_last_test_" + hex.EncodeToString(b[:]) // never the real md_last
	js, err := jetstream.New(nc)
	if err != nil {
		t.Fatal(err)
	}
	kv, err := js.CreateKeyValue(ctx, jetstream.KeyValueConfig{Bucket: bucket, History: 1, Storage: jetstream.MemoryStorage})
	if err != nil {
		t.Fatal(err)
	}
	defer func() { _ = js.DeleteKeyValue(context.Background(), bucket) }()
	snap, _ := proto.Marshal(&marketdatav1.Snapshot{Tick: &marketdatav1.Tick{InstrumentId: 900001, Ltp: 42, FeedId: 3}})
	// The engine writes the bucket by publishing to its stream subject; do the same.
	if err := nc.Publish("$KV."+bucket+".900001", snap); err != nil {
		t.Fatal(err)
	}
	if err := nc.Flush(); err != nil {
		t.Fatal(err)
	}
	waitFor(t, "kv value", func() bool { _, err := kv.Get(ctx, "900001"); return err == nil })

	src := NewNATSSource(nc, bucket)
	last, err := src.Last(ctx, []uint64{900001, 900002})
	if err != nil || len(last) != 1 || last[900001].GetTick().GetFeedId() != 3 {
		t.Fatalf("last = %v, %v", last, err)
	}

	// Buffered and never reassigned: the callbacks run on NATS goroutines.
	ticks := make(chan *marketdatav1.Tick, 1024)
	bars := make(chan *marketdatav1.Bar, 1024)
	runCtx, stop := context.WithCancel(ctx)
	defer stop()
	go func() {
		_ = src.Run(runCtx, func(t *marketdatav1.Tick) { ticks <- t }, func(b *marketdatav1.Bar) { bars <- b })
	}()
	tb, _ := proto.Marshal(&marketdatav1.Tick{InstrumentId: 900001, Ltp: 43, FeedId: 3})
	bb, _ := proto.Marshal(&marketdatav1.Bar{InstrumentId: 900001, Interval: "1m", Close: 43})
	deadline := time.After(5 * time.Second)
	gotTick, gotBar := false, false
	for !gotTick || !gotBar {
		_ = nc.Publish("md.tick.900001", tb) // repeat until the subscriptions are live
		_ = nc.Publish("md.bar.1m.900001", bb)
		select {
		case tk := <-ticks:
			if tk.GetLtp() != 43 || tk.GetFeedId() != 3 {
				t.Fatalf("tick %v", tk)
			}
			gotTick = true
		case br := <-bars:
			if br.GetClose() != 43 {
				t.Fatalf("bar %v", br)
			}
			gotBar = true
		case <-time.After(50 * time.Millisecond):
		case <-deadline:
			t.Fatal("no tick/bar received")
		}
	}
}
