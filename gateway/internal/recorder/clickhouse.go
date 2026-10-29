package recorder

import (
	"context"
	"fmt"
	"time"

	"github.com/ClickHouse/clickhouse-go/v2"
	"github.com/ClickHouse/clickhouse-go/v2/lib/driver"

	commonv1 "github.com/jiteshraj/vyuha/gateway/gen/vyuha/common/v1"
	mdv1 "github.com/jiteshraj/vyuha/gateway/gen/vyuha/marketdata/v1"
)

// ClickHouse writes into the vyuha.ticks / vyuha.bars tables (db/clickhouse/001_init.sql).
type ClickHouse struct {
	conn     driver.Conn
	database string
}

// OpenClickHouse connects using a clickhouse:// DSN (CLICKHOUSE_URL). The database is taken
// from the DSN path.
func OpenClickHouse(ctx context.Context, dsn string) (*ClickHouse, error) {
	opts, err := clickhouse.ParseDSN(dsn)
	if err != nil {
		return nil, fmt.Errorf("clickhouse: bad DSN") // the DSN may hold a password: not echoed
	}
	conn, err := clickhouse.Open(opts)
	if err != nil {
		return nil, fmt.Errorf("clickhouse: open: %w", err)
	}
	if err := conn.Ping(ctx); err != nil {
		_ = conn.Close()
		return nil, fmt.Errorf("clickhouse: ping: %w", err)
	}
	db := opts.Auth.Database
	if db == "" {
		db = "vyuha"
	}
	return &ClickHouse{conn: conn, database: db}, nil
}

// Close releases the connection pool.
func (c *ClickHouse) Close() error { return c.conn.Close() }

func ns(t int64) time.Time { return time.Unix(0, t).UTC() }

func source(b commonv1.Broker) string {
	switch b {
	case commonv1.Broker_BROKER_ZERODHA:
		return "zerodha"
	case commonv1.Broker_BROKER_UPSTOX:
		return "upstox"
	case commonv1.Broker_BROKER_SIM:
		return "sim"
	default:
		return "unknown"
	}
}

// InsertTicks writes one batch.
func (c *ClickHouse) InsertTicks(ctx context.Context, ticks []*mdv1.Tick) error {
	batch, err := c.conn.PrepareBatch(ctx, "INSERT INTO "+c.database+".ticks "+
		"(instrument_id, exchange_ts, recv_ts, source, ltp, ltq, volume, oi, bid, ask, bid_qty, ask_qty, avg_price)")
	if err != nil {
		return fmt.Errorf("clickhouse: prepare ticks: %w", err)
	}
	for _, t := range ticks {
		if err := batch.Append(t.GetInstrumentId(), ns(t.GetExchangeTsNs()), ns(t.GetRecvTsNs()), source(t.GetSource()),
			t.GetLtp(), t.GetLtq(), t.GetVolume(), t.GetOi(), t.GetBid(), t.GetAsk(), t.GetBidQty(), t.GetAskQty(),
			t.GetAvgPrice()); err != nil {
			_ = batch.Abort()
			return fmt.Errorf("clickhouse: append tick: %w", err)
		}
	}
	if err := batch.Send(); err != nil {
		return fmt.Errorf("clickhouse: send ticks: %w", err)
	}
	return nil
}

// InsertBars writes engine-built bars (source 'engine'; version = write time, ms).
func (c *ClickHouse) InsertBars(ctx context.Context, bars []*mdv1.Bar) error {
	batch, err := c.conn.PrepareBatch(ctx, "INSERT INTO "+c.database+".bars "+
		"(instrument_id, interval, ts, open, high, low, close, volume, oi, source, version)")
	if err != nil {
		return fmt.Errorf("clickhouse: prepare bars: %w", err)
	}
	version := uint64(time.Now().UnixMilli()) //nolint:gosec // G115: positive wall-clock ms
	for _, b := range bars {
		if err := batch.Append(b.GetInstrumentId(), b.GetInterval(), ns(b.GetTsNs()), b.GetOpen(), b.GetHigh(),
			b.GetLow(), b.GetClose(), b.GetVolume(), b.GetOi(), "engine", version); err != nil {
			_ = batch.Abort()
			return fmt.Errorf("clickhouse: append bar: %w", err)
		}
	}
	if err := batch.Send(); err != nil {
		return fmt.Errorf("clickhouse: send bars: %w", err)
	}
	return nil
}
