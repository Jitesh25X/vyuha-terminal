package recorder

import (
	"context"
	"errors"
	"io"
	"log/slog"
	"strings"
	"sync"
	"testing"
	"time"

	"google.golang.org/protobuf/proto"

	mdv1 "github.com/jiteshraj/vyuha/gateway/gen/vyuha/marketdata/v1"
)

type fakeSink struct {
	mu       sync.Mutex
	ticks    int
	bars     int
	batches  []int
	failNext int // fail this many inserts first
	block    chan struct{}
}

func (f *fakeSink) InsertTicks(_ context.Context, t []*mdv1.Tick) error {
	if f.block != nil {
		<-f.block
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.failNext > 0 {
		f.failNext--
		return errors.New("clickhouse unavailable")
	}
	f.ticks += len(t)
	f.batches = append(f.batches, len(t))
	return nil
}

func (f *fakeSink) InsertBars(_ context.Context, b []*mdv1.Bar) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.bars += len(b)
	return nil
}

func (f *fakeSink) counts() (int, int, []int) {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.ticks, f.bars, append([]int(nil), f.batches...)
}

func quiet() *slog.Logger { return slog.New(slog.NewTextHandler(io.Discard, nil)) }

func tickBytes(t *testing.T, id uint64) []byte {
	t.Helper()
	b, err := proto.Marshal(&mdv1.Tick{InstrumentId: id, Ltp: 245_005_000, ExchangeTsNs: 1})
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func waitFor(t *testing.T, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for !cond() {
		if time.Now().After(deadline) {
			t.Fatal("timed out")
		}
		time.Sleep(5 * time.Millisecond)
	}
}

// F-MD-4 AC2: backpressure never blocks the publisher side; drops are counted.
func TestFullQueueDropsWithoutBlocking(t *testing.T) {
	sink := &fakeSink{block: make(chan struct{})}
	cfg := Config{QueueSize: 100, BatchSize: 10, FlushInterval: time.Hour, MaxRetries: 1, RetryBackoff: time.Millisecond}
	r := New(cfg, sink, quiet())
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() { r.Run(ctx); close(done) }()

	msg := tickBytes(t, 1)
	start := time.Now()
	for range 10_000 {
		r.Handle("md.tick.1", msg)
	}
	if elapsed := time.Since(start); elapsed > time.Second {
		t.Fatalf("Handle blocked: 10k messages took %v", elapsed)
	}
	s := r.Stats()
	if s.Received != 10_000 || s.Dropped < 10_000-100-10-1 {
		t.Fatalf("want ~9.9k drops counted, got %+v", s)
	}
	var b strings.Builder
	if err := WriteMetrics(&b, s); err != nil || !strings.Contains(b.String(), "recorder_dropped_total ") {
		t.Fatalf("metrics: %v %q", err, b.String())
	}
	close(sink.block)
	cancel()
	<-done
}

func TestBatchesBySizeAndTimeAndDrainsOnShutdown(t *testing.T) {
	sink := &fakeSink{}
	cfg := Config{QueueSize: 1000, BatchSize: 100, FlushInterval: 50 * time.Millisecond, MaxRetries: 3, RetryBackoff: time.Millisecond}
	r := New(cfg, sink, quiet())
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() { r.Run(ctx); close(done) }()

	msg := tickBytes(t, 7)
	for range 250 {
		r.Handle("md.tick.7", msg)
	}
	waitFor(t, func() bool { n, _, _ := sink.counts(); return n == 250 }) // 100 + 100 by size, 50 by time
	_, _, batches := sink.counts()
	if len(batches) != 3 || batches[0] != 100 || batches[1] != 100 || batches[2] != 50 {
		t.Fatalf("batches %v", batches)
	}

	bar, _ := proto.Marshal(&mdv1.Bar{InstrumentId: 7, Interval: "1m", Final: true})
	forming, _ := proto.Marshal(&mdv1.Bar{InstrumentId: 7, Interval: "1m", Final: false})
	r.Handle("md.bar.1m.7", bar)
	r.Handle("md.bar.1m.7", forming)          // not stored
	r.Handle("md.tick.7", []byte{0xff, 0xff}) // not protobuf
	for range 30 {
		r.Handle("md.tick.7", msg)
	}
	cancel()
	<-done // shutdown drains the queue
	ticks, bars, _ := sink.counts()
	s := r.Stats()
	if ticks != 280 || bars != 1 || s.DecodeErrors != 1 || s.InsertedTicks != 280 || s.Dropped != 0 {
		t.Fatalf("ticks=%d bars=%d stats=%+v", ticks, bars, s)
	}
}

func TestRetriesThenCountsLostRows(t *testing.T) {
	sink := &fakeSink{failNext: 2}
	cfg := Config{QueueSize: 100, BatchSize: 5, FlushInterval: time.Hour, MaxRetries: 3, RetryBackoff: time.Millisecond}
	r := New(cfg, sink, quiet())
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() { r.Run(ctx); close(done) }()
	msg := tickBytes(t, 1)
	for range 5 {
		r.Handle("md.tick.1", msg)
	}
	waitFor(t, func() bool { n, _, _ := sink.counts(); return n == 5 }) // third attempt succeeds

	sink.mu.Lock()
	sink.failNext = 10
	sink.mu.Unlock()
	for range 5 {
		r.Handle("md.tick.1", msg)
	}
	waitFor(t, func() bool { return r.Stats().LostRows == 5 })
	cancel()
	<-done
	if s := r.Stats(); s.InsertErrors != 5 || s.InsertedTicks != 5 {
		t.Fatalf("stats %+v", s)
	}
}
