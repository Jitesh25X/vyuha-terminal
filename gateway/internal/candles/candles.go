// Package candles reads historical bars from ClickHouse (vyuha.bars, written by the recorder and the
// historical downloader) for GET /marketdata/candles.
package candles

import (
	"context"
	"errors"
	"fmt"
	"time"

	"github.com/ClickHouse/clickhouse-go/v2"
	"github.com/ClickHouse/clickhouse-go/v2/lib/driver"
)

// MaxBars is the most bars one call returns (API.md §2).
const MaxBars = 50_000

// queryTimeout bounds one candle query.
const queryTimeout = 5 * time.Second

// ErrTooMany: the range holds more than MaxBars bars at this interval.
var ErrTooMany = errors.New("candles: too many bars")

// Bar is one OHLCV bar; prices are int64 units (1 rupee = 10,000), like the engine's Price.
type Bar struct {
	TS                     time.Time
	Open, High, Low, Close int64
	Volume, OI             uint64
}

// buckets maps API intervals to ClickHouse bucket expressions over 1-minute bars. Daily bars bucket
// by the IST calendar day (NSE sessions sit inside one IST day).
var buckets = map[string]string{
	"1m":  "ts",
	"5m":  "toStartOfInterval(ts, INTERVAL 5 MINUTE)",
	"15m": "toStartOfInterval(ts, INTERVAL 15 MINUTE)",
	"1h":  "toStartOfInterval(ts, INTERVAL 1 HOUR)",
	"1d":  "toDateTime(toStartOfDay(ts, 'Asia/Kolkata'), 'UTC')",
}

// width is the bar length, used to refuse ranges that would exceed MaxBars before querying.
var width = map[string]time.Duration{
	"1m": time.Minute, "5m": 5 * time.Minute, "15m": 15 * time.Minute, "1h": time.Hour, "1d": 24 * time.Hour,
}

// Reader queries ClickHouse. The connection is opened lazily, so the gateway starts without it.
type Reader struct {
	conn     driver.Conn
	database string
}

// Open prepares a reader from a clickhouse:// DSN (CLICKHOUSE_URL); it does not connect yet.
func Open(dsn string) (*Reader, error) {
	opts, err := clickhouse.ParseDSN(dsn)
	if err != nil {
		return nil, errors.New("candles: bad CLICKHOUSE_URL") // the DSN may hold a password: not echoed
	}
	conn, err := clickhouse.Open(opts)
	if err != nil {
		return nil, fmt.Errorf("candles: open: %w", err)
	}
	db := opts.Auth.Database
	if db == "" {
		db = "vyuha"
	}
	return &Reader{conn: conn, database: db}, nil
}

// Close releases the pool.
func (r *Reader) Close() error { return r.conn.Close() }

// Bars returns bars for [from, to) at interval, oldest first.
func (r *Reader) Bars(ctx context.Context, instrumentID uint64, interval string, from, to time.Time) ([]Bar, error) {
	bucket, ok := buckets[interval]
	if !ok {
		return nil, fmt.Errorf("candles: interval %q", interval)
	}
	if !to.After(from) {
		return nil, errors.New("candles: empty range")
	}
	// Markets trade ~6¼ h a day, so a calendar range overestimates; bound it loosely (×4) and let
	// LIMIT catch the rest.
	if to.Sub(from)/width[interval] > MaxBars*4 {
		return nil, ErrTooMany
	}
	ctx, cancel := context.WithTimeout(ctx, queryTimeout)
	defer cancel()
	// FINAL: the table is a ReplacingMergeTree (forming bars are rewritten until final).
	// The outer columns must not be aliased "ts": the alias would shadow the inner ts that
	// argMin/argMax order by (every row of a bucket would tie).
	q := `SELECT bucket, argMin(open, ts), max(high), min(low), argMax(close, ts), sum(volume), argMax(oi, ts)
	      FROM (SELECT ts, open, high, low, close, volume, oi, ` + bucket + ` AS bucket
	            FROM ` + r.database + `.bars FINAL
	            WHERE instrument_id = ? AND interval = '1m' AND ts >= ? AND ts < ?)
	      GROUP BY bucket ORDER BY bucket LIMIT ?`
	rows, err := r.conn.Query(ctx, q, instrumentID, from.UTC(), to.UTC(), MaxBars+1)
	if err != nil {
		return nil, fmt.Errorf("candles: query: %w", err)
	}
	defer func() { _ = rows.Close() }()
	var out []Bar
	for rows.Next() {
		var b Bar
		if err := rows.Scan(&b.TS, &b.Open, &b.High, &b.Low, &b.Close, &b.Volume, &b.OI); err != nil {
			return nil, fmt.Errorf("candles: scan: %w", err)
		}
		b.TS = b.TS.UTC()
		out = append(out, b)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("candles: rows: %w", err)
	}
	if len(out) > MaxBars {
		return nil, ErrTooMany
	}
	return out, nil
}
