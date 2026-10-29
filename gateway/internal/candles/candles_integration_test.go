package candles

import (
	"context"
	"errors"
	"testing"
	"time"

	"github.com/jiteshraj/vyuha/gateway/internal/chtest"
)

func TestBarsAggregatesOneMinuteBars(t *testing.T) {
	conn, _, dsn := chtest.New(t)
	ctx := context.Background()
	r, err := Open(dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer func() { _ = r.Close() }()

	// 09:15–09:24 IST on two days (03:45 UTC), close = 100 + minute; one forming bar rewritten later.
	batch, err := conn.PrepareBatch(ctx, "INSERT INTO "+r.database+".bars (instrument_id, interval, ts, open, high, low, close, volume, oi, source, version)")
	if err != nil {
		t.Fatal(err)
	}
	for _, day := range []int{28, 29} {
		start := time.Date(2026, 10, day, 3, 45, 0, 0, time.UTC)
		for m := range 10 {
			px := int64(100+m) * 10_000
			if err := batch.Append(uint64(7), "1m", start.Add(time.Duration(m)*time.Minute), px, px+5_000, px-5_000, px, uint64(10), uint64(m), "engine", uint64(1)); err != nil {
				t.Fatal(err)
			}
		}
	}
	// A later version of the last minute of day 29 (the bar was still forming): FINAL keeps this one.
	if err := batch.Append(uint64(7), "1m", time.Date(2026, 10, 29, 3, 54, 0, 0, time.UTC), int64(109*10_000), int64(120*10_000), int64(108*10_000), int64(119*10_000), uint64(30), uint64(9), "engine", uint64(2)); err != nil {
		t.Fatal(err)
	}
	if err := batch.Send(); err != nil {
		t.Fatal(err)
	}

	from, to := time.Date(2026, 10, 28, 0, 0, 0, 0, time.UTC), time.Date(2026, 10, 30, 0, 0, 0, 0, time.UTC)
	one, err := r.Bars(ctx, 7, "1m", from, to)
	if err != nil || len(one) != 20 || one[19].Close != 119*10_000 || one[19].Volume != 30 {
		t.Fatalf("1m: %d bars, last %+v, %v", len(one), one[len(one)-1], err)
	}
	five, err := r.Bars(ctx, 7, "5m", from, to)
	if err != nil || len(five) != 4 {
		t.Fatalf("5m: %+v, %v", five, err)
	}
	if b := five[0]; b.Open != 100*10_000 || b.Close != 104*10_000 || b.High != 1_045_000 || b.Low != 995_000 || b.Volume != 50 || b.OI != 4 {
		t.Fatalf("first 5m bar %+v", b)
	}
	daily, err := r.Bars(ctx, 7, "1d", from, to)
	if err != nil || len(daily) != 2 {
		t.Fatalf("1d: %+v, %v", daily, err)
	}
	// Day buckets start at IST midnight (18:30 UTC the day before).
	if want := time.Date(2026, 10, 28, 18, 30, 0, 0, time.UTC); !daily[1].TS.Equal(want) || daily[1].High != 120*10_000 || daily[1].Volume != 120 {
		t.Fatalf("second day %+v", daily[1])
	}
	if _, err := r.Bars(ctx, 7, "1m", from, from.AddDate(1, 0, 0)); !errors.Is(err, ErrTooMany) {
		t.Fatalf("a year of 1-minute bars must be refused: %v", err)
	}
	if got, err := r.Bars(ctx, 8, "1d", from, to); err != nil || len(got) != 0 {
		t.Fatalf("other instrument: %v, %v", got, err)
	}
}
