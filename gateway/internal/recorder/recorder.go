// Package recorder persists market data from NATS into ClickHouse (F-MD-4).
//
// The NATS callback only decodes and enqueues into a bounded channel; when it's full the message
// is dropped and counted (recorder_dropped_total), so a slow ClickHouse never pushes back on
// NATS or the engine. A single writer goroutine batches inserts by size or time and retries a
// failed batch with backoff.
package recorder

import (
	"context"
	"log/slog"
	"strings"
	"sync/atomic"
	"time"

	"google.golang.org/protobuf/proto"

	mdv1 "github.com/Jitesh25X/vyuha-terminal/gateway/gen/vyuha/marketdata/v1"
)

// Sink writes batches (ClickHouse in production, a fake in tests).
type Sink interface {
	InsertTicks(ctx context.Context, ticks []*mdv1.Tick) error
	InsertBars(ctx context.Context, bars []*mdv1.Bar) error
}

// Config sizes the queue and batches.
type Config struct {
	QueueSize     int           // bounded queue between NATS and the writer
	BatchSize     int           // rows per insert
	FlushInterval time.Duration // max age of a partial batch
	MaxRetries    int           // attempts per batch before it's counted as lost
	RetryBackoff  time.Duration // first retry delay (doubles)
	InsertTimeout time.Duration // per attempt; independent of shutdown, so a stop never cancels a write
}

// DefaultConfig fits a full NSE session at peak subscription on one recorder.
func DefaultConfig() Config {
	return Config{QueueSize: 200_000, BatchSize: 50_000, FlushInterval: time.Second, MaxRetries: 5,
		RetryBackoff: 200 * time.Millisecond, InsertTimeout: 30 * time.Second}
}

type item struct {
	tick *mdv1.Tick
	bar  *mdv1.Bar
}

// Recorder counts everything it sees; Stats() exposes the counters as metrics.
type Recorder struct {
	cfg    Config
	sink   Sink
	log    *slog.Logger
	queue  chan item
	closed atomic.Bool

	received      atomic.Uint64
	dropped       atomic.Uint64
	decodeErrors  atomic.Uint64
	insertedTicks atomic.Uint64
	insertedBars  atomic.Uint64
	insertErrors  atomic.Uint64
	lostRows      atomic.Uint64
}

// Stats are the recorder's metrics (Prometheus names in WriteMetrics).
type Stats struct {
	Received, Dropped, DecodeErrors, InsertedTicks, InsertedBars, InsertErrors, LostRows uint64
}

// New builds a recorder; call Run to start writing.
func New(cfg Config, sink Sink, log *slog.Logger) *Recorder {
	return &Recorder{cfg: cfg, sink: sink, log: log, queue: make(chan item, cfg.QueueSize)}
}

// Handle is the NATS message callback for md.tick.* / md.bar.* subjects. Never blocks.
func (r *Recorder) Handle(subject string, data []byte) {
	r.received.Add(1)
	var it item
	switch {
	case strings.HasPrefix(subject, "md.tick."):
		t := &mdv1.Tick{}
		if err := proto.Unmarshal(data, t); err != nil {
			r.decodeErrors.Add(1)
			return
		}
		it.tick = t
	case strings.HasPrefix(subject, "md.bar."):
		b := &mdv1.Bar{}
		if err := proto.Unmarshal(data, b); err != nil {
			r.decodeErrors.Add(1)
			return
		}
		if !b.GetFinal() {
			return // only finished bars are stored
		}
		it.bar = b
	default:
		return
	}
	r.Enqueue(it.tick, it.bar)
}

// Enqueue offers one row to the writer; a full queue drops it (counted).
func (r *Recorder) Enqueue(t *mdv1.Tick, b *mdv1.Bar) {
	if r.closed.Load() {
		r.dropped.Add(1)
		return
	}
	select {
	case r.queue <- item{tick: t, bar: b}:
	default:
		r.dropped.Add(1)
	}
}

// AddDropped counts messages lost before they reached Handle (NATS slow-consumer drops).
func (r *Recorder) AddDropped(n uint64) { r.dropped.Add(n) }

// Stats snapshots the counters.
func (r *Recorder) Stats() Stats {
	return Stats{
		Received: r.received.Load(), Dropped: r.dropped.Load(), DecodeErrors: r.decodeErrors.Load(),
		InsertedTicks: r.insertedTicks.Load(), InsertedBars: r.insertedBars.Load(),
		InsertErrors: r.insertErrors.Load(), LostRows: r.lostRows.Load(),
	}
}

// Run writes batches until ctx is done, then drains what's queued. ctx only signals "stop":
// inserts run under their own timeouts, so stopping never cancels a batch already in flight.
func (r *Recorder) Run(ctx context.Context) {
	ticks := make([]*mdv1.Tick, 0, r.cfg.BatchSize)
	bars := make([]*mdv1.Bar, 0, 1024)
	timer := time.NewTicker(r.cfg.FlushInterval)
	defer timer.Stop()
	flush := func() {
		if len(ticks) > 0 {
			r.write(len(ticks), func(c context.Context) error { return r.sink.InsertTicks(c, ticks) }, &r.insertedTicks)
			ticks = ticks[:0]
		}
		if len(bars) > 0 {
			r.write(len(bars), func(c context.Context) error { return r.sink.InsertBars(c, bars) }, &r.insertedBars)
			bars = bars[:0]
		}
	}
	add := func(it item) {
		if it.tick != nil {
			ticks = append(ticks, it.tick)
		} else if it.bar != nil {
			bars = append(bars, it.bar)
		}
		if len(ticks) >= r.cfg.BatchSize {
			flush()
		}
	}
	for {
		select {
		case it := <-r.queue:
			add(it)
		case <-timer.C:
			flush()
		case <-ctx.Done():
			r.closed.Store(true)
			for drained := false; !drained; {
				select {
				case it := <-r.queue:
					add(it)
				default:
					drained = true
				}
			}
			flush()
			return
		}
	}
}

func (r *Recorder) write(rows int, insert func(context.Context) error, ok *atomic.Uint64) {
	delay := r.cfg.RetryBackoff
	timeout := r.cfg.InsertTimeout
	if timeout <= 0 {
		timeout = 30 * time.Second
	}
	for attempt := 1; ; attempt++ {
		actx, cancel := context.WithTimeout(context.Background(), timeout)
		err := insert(actx)
		cancel()
		if err == nil {
			ok.Add(uint64(rows)) //nolint:gosec // G115: rows > 0
			return
		}
		r.insertErrors.Add(1)
		if attempt >= r.cfg.MaxRetries {
			r.lostRows.Add(uint64(rows)) //nolint:gosec // G115: rows > 0
			r.log.Error("recorder batch lost", "rows", rows, "attempts", attempt, "err", err)
			return
		}
		r.log.Warn("recorder insert failed; retrying", "rows", rows, "attempt", attempt, "err", err)
		time.Sleep(delay)
		delay *= 2
	}
}
