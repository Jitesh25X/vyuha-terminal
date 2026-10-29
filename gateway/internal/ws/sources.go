package ws

import (
	"context"
	"errors"
	"fmt"
	"strconv"
	"sync"
	"time"

	"github.com/jackc/pgx/v5/pgtype"
	"github.com/nats-io/nats.go"
	"github.com/nats-io/nats.go/jetstream"
	"google.golang.org/protobuf/proto"

	marketdatav1 "github.com/jiteshraj/vyuha/gateway/gen/vyuha/marketdata/v1"
	"github.com/jiteshraj/vyuha/gateway/internal/store"
)

// ── NATS market data ─────────────────────────────────────────────────────────

// NATSSource reads the engine's md.tick.<id> / md.bar.1m.<id> subjects and the md_last KV bucket.
// The engine publishes only instruments someone subscribed, so one wildcard subscription per kind
// is enough; the hub filters by its own interest.
type NATSSource struct {
	nc     *nats.Conn
	bucket string

	mu sync.Mutex
	kv jetstream.KeyValue
}

// NewNATSSource wraps a connection; bucket is the KV bucket name (md_last).
func NewNATSSource(nc *nats.Conn, bucket string) *NATSSource {
	return &NATSSource{nc: nc, bucket: bucket}
}

// Run implements Source.
func (s *NATSSource) Run(ctx context.Context, onTick func(*marketdatav1.Tick), onBar func(*marketdatav1.Bar)) error {
	ticks, err := s.nc.Subscribe("md.tick.*", func(m *nats.Msg) {
		t := &marketdatav1.Tick{}
		if proto.Unmarshal(m.Data, t) == nil {
			onTick(t)
		}
	})
	if err != nil {
		return fmt.Errorf("ws: subscribe ticks: %w", err)
	}
	defer func() { _ = ticks.Unsubscribe() }()
	bars, err := s.nc.Subscribe("md.bar.1m.*", func(m *nats.Msg) {
		b := &marketdatav1.Bar{}
		if proto.Unmarshal(m.Data, b) == nil {
			onBar(b)
		}
	})
	if err != nil {
		return fmt.Errorf("ws: subscribe bars: %w", err)
	}
	defer func() { _ = bars.Unsubscribe() }()
	// Ticks are conflated downstream, so a large pending buffer only adds latency; NATS drops (and
	// counts) beyond it rather than growing without bound.
	for _, sub := range []*nats.Subscription{ticks, bars} {
		if err := sub.SetPendingLimits(200_000, 64<<20); err != nil {
			return fmt.Errorf("ws: pending limits: %w", err)
		}
	}
	<-ctx.Done()
	return nil
}

// Last implements Source: md_last snapshots for ids (missing keys are skipped).
func (s *NATSSource) Last(ctx context.Context, ids []uint64) (map[uint64]*marketdatav1.Snapshot, error) {
	kv, err := s.keyValue(ctx)
	if err != nil {
		return nil, err
	}
	out := make(map[uint64]*marketdatav1.Snapshot, len(ids))
	var (
		mu  sync.Mutex
		wg  sync.WaitGroup
		sem = make(chan struct{}, 16)
	)
	for _, id := range ids {
		wg.Add(1)
		sem <- struct{}{}
		go func() {
			defer func() { <-sem; wg.Done() }()
			e, err := kv.Get(ctx, strconv.FormatUint(id, 10))
			if err != nil {
				return // not found, or the deadline passed: no snapshot for this key
			}
			snap := &marketdatav1.Snapshot{}
			if proto.Unmarshal(e.Value(), snap) == nil {
				mu.Lock()
				out[id] = snap
				mu.Unlock()
			}
		}()
	}
	wg.Wait()
	return out, nil
}

func (s *NATSSource) keyValue(ctx context.Context) (jetstream.KeyValue, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.kv != nil {
		return s.kv, nil
	}
	js, err := jetstream.New(s.nc)
	if err != nil {
		return nil, fmt.Errorf("ws: jetstream: %w", err)
	}
	kv, err := js.KeyValue(ctx, s.bucket)
	if err != nil {
		return nil, fmt.Errorf("ws: kv %s: %w", s.bucket, err) // created by the engine publisher
	}
	s.kv = kv
	return kv, nil
}

// ── Postgres directory ───────────────────────────────────────────────────────

// PGDirectory reads account mappings, instrument broker mappings and holidays.
type PGDirectory struct{ q *store.Queries }

// NewPGDirectory wraps sqlc queries.
func NewPGDirectory(q *store.Queries) *PGDirectory { return &PGDirectory{q: q} }

// UserAccounts implements Directory.
func (d *PGDirectory) UserAccounts(ctx context.Context, userID string) ([]Account, error) {
	var uid pgtype.UUID
	if err := uid.Scan(userID); err != nil {
		return nil, errors.New("ws: bad user id")
	}
	rows, err := d.q.ListUserAccounts(ctx, uid)
	if err != nil {
		return nil, fmt.Errorf("ws: user accounts: %w", err)
	}
	out := make([]Account, len(rows))
	for i, r := range rows {
		out[i] = Account{ID: r.ID.String(), Broker: string(r.Broker), DataEntitled: r.DataEntitled}
	}
	return out, nil
}

// Instruments implements Directory (active instruments only).
func (d *PGDirectory) Instruments(ctx context.Context, ids []uint64) (map[uint64]InstrumentMap, error) {
	in := make([]int64, 0, len(ids))
	for _, id := range ids {
		if id <= 1<<62 {
			in = append(in, int64(id))
		}
	}
	rows, err := d.q.ListInstrumentBrokerMaps(ctx, in)
	if err != nil {
		return nil, fmt.Errorf("ws: instruments: %w", err)
	}
	out := make(map[uint64]InstrumentMap, len(rows))
	for _, r := range rows {
		if r.ID <= 0 {
			continue // bigserial ids are positive
		}
		out[uint64(r.ID)] = InstrumentMap{Kite: r.KiteToken.Valid, Upstox: r.UpstoxKey.Valid && r.UpstoxKey.String != ""}
	}
	return out, nil
}

// IsHoliday implements Directory (NSE calendar).
func (d *PGDirectory) IsHoliday(ctx context.Context, day time.Time) (bool, error) {
	return d.q.IsExchangeHoliday(ctx, pgtype.Date{Time: time.Date(day.Year(), day.Month(), day.Day(), 0, 0, 0, 0, time.UTC), Valid: true})
}
