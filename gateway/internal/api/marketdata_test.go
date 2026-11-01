package api

import (
	"context"
	"io"
	"log/slog"
	"math"
	"testing"
	"time"

	"github.com/golang-jwt/jwt/v5"

	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/auth"
	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/candles"
	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/config"
	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/store"
	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/testdb"
	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/ws"
)

func TestUnits(t *testing.T) {
	for v, want := range map[int64]string{
		0: "0.0000", 1: "0.0001", 24_505_000: "2450.5000", -50: "-0.0050", -10_000: "-1.0000",
		math.MaxInt64: "922337203685477.5807", math.MinInt64: "-922337203685477.5808",
	} {
		if got := units(v); got != want {
			t.Errorf("units(%d) = %s, want %s", v, got, want)
		}
	}
}

type fakeCandles struct{ calls int }

func (f *fakeCandles) Bars(context.Context, uint64, string, time.Time, time.Time) ([]candles.Bar, error) {
	f.calls++
	return []candles.Bar{{TS: time.Date(2026, 10, 29, 3, 45, 0, 0, time.UTC), Open: 24_500_000, High: 24_600_000, Low: 24_400_000, Close: 24_550_000, Volume: 9, OI: 1}}, nil
}

type fakeEntitlement struct{ accounts []ws.Account }

func (f fakeEntitlement) UserAccounts(context.Context, string) ([]ws.Account, error) {
	return f.accounts, nil
}
func (f fakeEntitlement) Instruments(_ context.Context, ids []uint64) (map[uint64]ws.InstrumentMap, error) {
	return map[uint64]ws.InstrumentMap{ids[0]: {Upstox: true}}, nil // streamable through Upstox only
}
func (f fakeEntitlement) IsHoliday(context.Context, time.Time) (bool, error) { return false, nil }

func asUser(id string) context.Context {
	return context.WithValue(context.Background(), claimsKey, &auth.Claims{RegisteredClaims: jwt.RegisteredClaims{Subject: id}})
}

func TestCandlesNeedAnEntitledAccountForTheInstrument(t *testing.T) {
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	from := time.Date(2026, 10, 29, 0, 0, 0, 0, time.UTC)
	req := GetCandlesRequestObject{Params: GetCandlesParams{InstrumentId: 7, Interval: "1m", From: from, To: from.Add(24 * time.Hour)}}
	for name, tc := range map[string]struct {
		accounts []ws.Account
		ok       bool
	}{
		"kite account, upstox-only instrument": {[]ws.Account{{ID: "a", Broker: "zerodha", DataEntitled: true}}, false},
		"upstox account without entitlement":   {[]ws.Account{{ID: "b", Broker: "upstox"}}, false},
		"entitled upstox account":              {[]ws.Account{{ID: "c", Broker: "upstox", DataEntitled: true}}, true},
	} {
		t.Run(name, func(t *testing.T) {
			fc := &fakeCandles{}
			s := NewServer("0", config.EnvDev, fakeDB{}, nil, logger).WithMarketData(&MarketData{Candles: fc, Entitlement: fakeEntitlement{tc.accounts}})
			res, err := s.GetCandles(asUser("u1"), req)
			if err != nil {
				t.Fatal(err)
			}
			if !tc.ok {
				if _, denied := res.(GetCandles403ApplicationProblemPlusJSONResponse); !denied || fc.calls != 0 {
					t.Fatalf("res = %#v, reader calls %d", res, fc.calls)
				}
				return
			}
			bars := res.(GetCandles200JSONResponse).Bars
			if len(bars) != 1 || bars[0][0] != "2026-10-29T03:45:00Z" || bars[0][4] != "2455.0000" {
				t.Fatalf("bars = %v", bars)
			}
		})
	}
	s := NewServer("0", config.EnvDev, fakeDB{}, nil, logger).WithMarketData(&MarketData{Candles: &fakeCandles{}, Entitlement: fakeEntitlement{}})
	bad := req
	bad.Params.To = from
	if res, _ := s.GetCandles(asUser("u1"), bad); res == nil {
		t.Fatal("no response")
	} else if _, ok := res.(GetCandles400ApplicationProblemPlusJSONResponse); !ok {
		t.Fatalf("empty range: %#v", res)
	}
}

func TestSearchInstruments(t *testing.T) {
	_, pool := testdb.New(t)
	ctx := context.Background()
	if _, err := pool.Exec(ctx, `INSERT INTO instrument (id, exchange, segment, symbol, tradingsymbol, name, tick_size, lot_size, valid_from, expiry, valid_to) VALUES
		(1, 'NSE', 'EQ', 'RELIANCE', 'RELIANCE', 'Reliance Industries', 0.10, 1, '2026-10-01', NULL, NULL),
		(2, 'NFO', 'FUT', 'RELIANCE', 'RELIANCE26NOVFUT', NULL, 0.10, 500, '2026-10-01', '2026-11-26', NULL),
		(3, 'NFO', 'FUT', 'RELIANCE', 'RELIANCE26OCTFUT', NULL, 0.10, 500, '2026-10-01', '2026-10-28', '2026-10-29'),
		(4, 'BSE', 'EQ', 'RELAXO', 'RELAXO', NULL, 0.05, 1, '2026-10-01', NULL, NULL),
		(5, 'NSE', 'EQ', 'M&M', 'M&M', NULL, 0.05, 1, '2026-10-01', NULL, NULL),
		(6, 'NSE', 'EQ', 'MAMAEARTH', 'MAMAEARTH', NULL, 0.05, 1, '2026-10-01', NULL, NULL)`); err != nil {
		t.Fatal(err)
	}
	s := NewServer("0", config.EnvDev, fakeDB{}, nil, slog.New(slog.NewTextHandler(io.Discard, nil))).
		WithMarketData(&MarketData{Instruments: store.New(pool)})
	search := func(q string, seg *SearchInstrumentsParamsSegment) []int64 {
		t.Helper()
		res, err := s.SearchInstruments(ctx, SearchInstrumentsRequestObject{Params: SearchInstrumentsParams{Q: q, Segment: seg}})
		if err != nil {
			t.Fatal(err)
		}
		var ids []int64
		for _, i := range res.(SearchInstruments200JSONResponse) {
			ids = append(ids, i.Id)
		}
		return ids
	}
	eq, nse := SearchInstrumentsParamsSegment("EQ"), SearchInstrumentsParamsSegment("NSE")
	tests := []struct {
		q    string
		seg  *SearchInstrumentsParamsSegment
		want []int64
	}{
		{"reliance", nil, []int64{1, 2}}, // exact symbol first; the expired future is not listed
		{"REL", nil, []int64{4, 1, 2}},   // cash before derivatives, shorter symbol first
		{"rel", &eq, []int64{4, 1}},
		{"rel", &nse, []int64{1}},
		{"M&", nil, []int64{5}},
		{"M%", nil, nil}, // LIKE wildcards are literal
		{"_A", nil, nil},
	}
	for _, tt := range tests {
		if got := search(tt.q, tt.seg); !equalIDs(got, tt.want) {
			t.Errorf("search %q (%v) = %v, want %v", tt.q, tt.seg, got, tt.want)
		}
	}
	res, err := s.GetInstrument(ctx, GetInstrumentRequestObject{Id: 3})
	if err != nil {
		t.Fatal(err)
	}
	inst := res.(GetInstrument200JSONResponse)
	if inst.ValidTo == nil || inst.TickSize != "0.1000" || inst.LotSize != 500 || inst.Expiry == nil {
		t.Fatalf("instrument 3 = %+v", inst)
	}
	if res, _ := s.GetInstrument(ctx, GetInstrumentRequestObject{Id: 99}); res == nil {
		t.Fatal("no response")
	} else if _, ok := res.(GetInstrument404ApplicationProblemPlusJSONResponse); !ok {
		t.Fatalf("unknown id: %#v", res)
	}
}

func equalIDs(a, b []int64) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}
