package api

import (
	"context"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"testing"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgtype"
	openapi_types "github.com/oapi-codegen/runtime/types"

	marketdatav1 "github.com/jiteshraj/vyuha/gateway/gen/vyuha/marketdata/v1"
	"github.com/jiteshraj/vyuha/gateway/internal/config"
	"github.com/jiteshraj/vyuha/gateway/internal/quantc"
	"github.com/jiteshraj/vyuha/gateway/internal/store"
	"github.com/jiteshraj/vyuha/gateway/internal/testdb"
	"github.com/jiteshraj/vyuha/gateway/internal/ws"
)

// Chain fixture: underlying 100 (NIFTY index, or a stock), future 200, strikes 24000…24500 step 100,
// CE id = 1000+i, PE id = 2000+i.
type fakeChainStore struct {
	segment store.SegmentCode
	future  bool
}

func numericOf(s string) pgtype.Numeric {
	var n pgtype.Numeric
	_ = n.Scan(s)
	return n
}

func (f fakeChainStore) GetInstrument(_ context.Context, id int64) (store.GetInstrumentRow, error) {
	if id != 100 {
		return store.GetInstrumentRow{}, pgx.ErrNoRows
	}
	return store.GetInstrumentRow{ID: 100, Symbol: "NIFTY", Segment: f.segment}, nil
}
func (f fakeChainStore) ListOptionExpiries(_ context.Context, arg store.ListOptionExpiriesParams) ([]pgtype.Date, error) {
	if arg.UnderlyingID.Int64 != 100 {
		return nil, nil
	}
	return []pgtype.Date{{Time: time.Date(2026, 11, 3, 0, 0, 0, 0, time.UTC), Valid: true}, {Time: time.Date(2026, 11, 10, 0, 0, 0, 0, time.UTC), Valid: true}}, nil
}
func (f fakeChainStore) ListChainContracts(_ context.Context, arg store.ListChainContractsParams) ([]store.ListChainContractsRow, error) {
	if !arg.Expiry.Time.Equal(time.Date(2026, 11, 3, 0, 0, 0, 0, time.UTC)) {
		return nil, nil
	}
	var out []store.ListChainContractsRow
	for i := range 6 {
		k := fmt.Sprint(24000 + 100*i)
		for _, ot := range []store.OptionType{store.OptionTypeCE, store.OptionTypePE} {
			if i == 0 && ot == store.OptionTypePE {
				continue // 24000 lists a call only: one-sided strikes must work
			}
			id := int64(1000 + i)
			if ot == store.OptionTypePE {
				id = int64(2000 + i)
			}
			out = append(out, store.ListChainContractsRow{ID: id, Tradingsymbol: "NIFTY26N03" + k + string(ot), Strike: numericOf(k + ".0000"), // numeric(18,4) renders with scale 4
				OptionType: store.NullOptionType{OptionType: ot, Valid: true}, LotSize: 75, TickSize: numericOf("0.05")})
		}
	}
	return out, nil
}
func (f fakeChainStore) GetSameExpiryFuture(context.Context, store.GetSameExpiryFutureParams) (int64, error) {
	if !f.future {
		return 0, pgx.ErrNoRows
	}
	return 200, nil
}
func (f fakeChainStore) ListUpcomingDividends(context.Context, store.ListUpcomingDividendsParams) ([]store.ListUpcomingDividendsRow, error) {
	return []store.ListUpcomingDividendsRow{{ExDate: pgtype.Date{Time: time.Date(2026, 10, 30, 0, 0, 0, 0, time.UTC), Valid: true}, Amount: numericOf("10")}}, nil
}

type chainEntitlement struct{ accounts []ws.Account }

func (f chainEntitlement) UserAccounts(context.Context, string) ([]ws.Account, error) {
	return f.accounts, nil
}
func (f chainEntitlement) Instruments(_ context.Context, ids []uint64) (map[uint64]ws.InstrumentMap, error) {
	out := map[uint64]ws.InstrumentMap{}
	for _, id := range ids {
		out[id] = ws.InstrumentMap{Kite: true}
	}
	return out, nil
}
func (f chainEntitlement) IsHoliday(context.Context, time.Time) (bool, error) { return false, nil }

// fakeLatest serves ticks only to the account "acct" (stands in for the hub's feed rule).
type fakeLatest struct{ ticks map[uint64]*marketdatav1.Tick }

// tradedAt: contract 1003 traded 20 s before chainNow (as the hub infers from volume).
func (f fakeLatest) Latest(_ context.Context, accounts map[string]bool, ids []uint64) map[uint64]ws.LatestValue {
	out := map[uint64]ws.LatestValue{}
	if !accounts["acct"] {
		return out
	}
	for _, id := range ids {
		if t := f.ticks[id]; t != nil {
			v := ws.LatestValue{Tick: t}
			if id == 1003 {
				v.TradedAt = chainNow.Add(-20 * time.Second)
			}
			out[id] = v
		}
	}
	return out
}

type fakeOI struct {
	calls  int
	before time.Time
}

func (f *fakeOI) LastOI(_ context.Context, ids []uint64, before time.Time) (map[uint64]uint64, error) {
	f.calls++
	f.before = before
	out := map[uint64]uint64{}
	for _, id := range ids {
		if id != 2005 { // no history for one contract
			out[id] = 1000
		}
	}
	return out, nil
}

type fakeQuant struct {
	req  *quantc.ChainRequest
	err  error
	resp func(*quantc.ChainRequest) *quantc.ChainResult
}

func (f *fakeQuant) OptionChain(_ context.Context, req *quantc.ChainRequest) (*quantc.ChainResult, error) {
	f.req = req
	if f.err != nil {
		return nil, f.err
	}
	return f.resp(req), nil
}

func ptr[T any](v T) *T { return &v }

func quantEcho(req *quantc.ChainRequest) *quantc.ChainResult {
	res := &quantc.ChainResult{Model: "black76", UnderlyingPrice: ptr(24310.0), ForwardSource: ptr("future"), Rate: 0.055,
		RateAsOf: ptr("2026-10-07"), TYears: 0.02, ATMStrike: ptr("24300"), PCR: ptr(0.8), MaxPain: ptr("24200"), OIStrikes: len(req.OI)}
	for _, s := range req.Strikes {
		res.Rows = append(res.Rows, quantc.Row{Strike: s.Strike, CE: &quantc.Side{IV: ptr(0.12), IVSource: ptr("ltp"), Delta: ptr(0.5)}, PE: &quantc.Side{}})
	}
	return res
}

// 2026-10-29 11:00 IST
var chainNow = time.Date(2026, 10, 29, 5, 30, 0, 0, time.UTC)

func chainServer(segment store.SegmentCode, accounts []ws.Account, q *fakeQuant, oi *fakeOI) *Server {
	ticks := map[uint64]*marketdatav1.Tick{
		100:  {InstrumentId: 100, Ltp: 24_290_0000}, // spot 24,290 → ATM 24300
		200:  {InstrumentId: 200, Ltp: 24_320_0000},
		1003: {InstrumentId: 1003, Ltp: 180_5000, Bid: 180_0000, Ask: 181_0000, PrevClose: 150_0000, Volume: 12_000, Oi: 1500},
		2005: {InstrumentId: 2005, Ltp: 210_0000, Oi: 300},
		1000: {InstrumentId: 1000, Ltp: 400_0000, Oi: 9000}, // 24000 CE: outside a ±1 window, still in PCR
	}
	return NewServer("0", config.EnvDev, fakeDB{}, nil, slog.New(slog.NewTextHandler(io.Discard, nil))).
		WithOptionChains(&OptionChains{
			Store: fakeChainStore{segment: segment, future: true}, Entitlement: chainEntitlement{accounts},
			Latest: fakeLatest{ticks}, OI: oi, Quant: q, Now: func() time.Time { return chainNow },
		})
}

var traderAccounts = []ws.Account{{ID: "acct", Broker: "zerodha", DataEntitled: true}}

func TestOptionChainWindowMarketFieldsAndAnalytics(t *testing.T) {
	q, oi := &fakeQuant{resp: quantEcho}, &fakeOI{}
	s := chainServer(store.SegmentCodeIDX, traderAccounts, q, oi)
	res, err := s.GetOptionChain(asUser("u1"), GetOptionChainRequestObject{UnderlyingId: 100, Params: GetOptionChainParams{Strikes: ptr(1)}})
	if err != nil {
		t.Fatal(err)
	}
	oc, ok := res.(GetOptionChain200JSONResponse)
	if !ok {
		t.Fatalf("res = %#v", res)
	}
	// Default expiry = nearest; ±1 strike around 24300.
	if oc.Expiry.Format(time.DateOnly) != "2026-11-03" || len(oc.Rows) != 3 || oc.Rows[0].Strike != "24200.0000" || oc.Rows[2].Strike != "24400.0000" {
		t.Fatalf("expiry %v rows %+v", oc.Expiry, oc.Rows)
	}
	if oc.Underlying.Ltp == nil || *oc.Underlying.Ltp != "24290.0000" || oc.Underlying.FutureId == nil || *oc.Underlying.FutLtp != "24320.0000" || oc.LotSize != 75 {
		t.Fatalf("underlying %+v lot %d", oc.Underlying, oc.LotSize)
	}
	ce := oc.Rows[1].Ce // 24300 CE = 1003
	if ce == nil || ce.InstrumentId != 1003 || *ce.Ltp != "180.5000" || *ce.Bid != "180.0000" || *ce.PrevClose != "150.0000" ||
		*ce.Oi != 1500 || *ce.OiChg != 500 || *ce.Volume != 12_000 || *ce.Iv != 0.12 || *ce.IvSource != "ltp" || *ce.Delta != 0.5 {
		t.Fatalf("24300 CE %+v", ce)
	}
	if pe := oc.Rows[2].Pe; pe == nil || pe.Ltp != nil || pe.Oi != nil || pe.Iv != nil { // 24400 PE (2004): no tick
		t.Fatalf("24400 PE %+v", pe)
	}
	if oc.Analytics != OptionChainAnalyticsOk || *oc.Pcr != 0.8 || *oc.MaxPain != "24200" || *oc.AtmStrike != "24300" || *oc.ForwardSource != "future" ||
		oc.RateAsOf.Format(time.DateOnly) != "2026-10-07" || *oc.OiStrikes != 3 {
		t.Fatalf("analytics %+v", oc)
	}
	// The terminal subscribes the whole expiry so PCR and max pain see every strike.
	if len(oc.ContractIds) != 11 {
		t.Fatalf("contract ids %v", oc.ContractIds)
	}
	// Quant got the user's prices for exactly the window, as an index chain on the future.
	if q.req.Kind != "index" || *q.req.Spot != "24290.0000" || *q.req.Future != "24320.0000" || q.req.Expiry != "2026-11-03" ||
		q.req.TickSize != "0.05" || len(q.req.Strikes) != 3 || *q.req.Strikes[1].CE.LTP != "180.5000" || q.req.Strikes[1].CE.OI != 1500 {
		t.Fatalf("quant request %+v", q.req)
	}
	// Trade time only where the hub saw one; OI for PCR/max pain covers strikes outside the window.
	if at := q.req.Strikes[1].CE.TradedAt; at == nil || !at.Equal(chainNow.Add(-20*time.Second)) || q.req.Strikes[1].PE.TradedAt != nil {
		t.Fatalf("traded_at %v / %v", at, q.req.Strikes[1].PE.TradedAt)
	}
	wantOI := []quantc.OIStrike{{Strike: "24000.0000", CEOI: 9000}, {Strike: "24300.0000", CEOI: 1500}, {Strike: "24500.0000", PEOI: 300}}
	if fmt.Sprint(q.req.OI) != fmt.Sprint(wantOI) {
		t.Fatalf("oi %v, want %v", q.req.OI, wantOI)
	}
	// ΔOI baseline = last recorded OI before today's 09:15 IST open; cached for the day.
	if want := time.Date(2026, 10, 29, 3, 45, 0, 0, time.UTC); !oi.before.Equal(want) {
		t.Fatalf("baseline before %v, want %v", oi.before, want)
	}
	if _, err := s.GetOptionChain(asUser("u1"), GetOptionChainRequestObject{UnderlyingId: 100, Params: GetOptionChainParams{Strikes: ptr(1)}}); err != nil || oi.calls != 1 {
		t.Fatalf("second poll: %v, baseline queries %d", err, oi.calls)
	}
	// Contract without history: OI shown, no ΔOI.
	res, _ = s.GetOptionChain(asUser("u1"), GetOptionChainRequestObject{UnderlyingId: 100, Params: GetOptionChainParams{Strikes: ptr(5)}})
	wide := res.(GetOptionChain200JSONResponse)
	if pe := wide.Rows[5].Pe; pe == nil || *pe.Oi != 300 || pe.OiChg != nil {
		t.Fatalf("24500 PE %+v", pe)
	}
	if wide.Rows[0].Ce == nil || wide.Rows[0].Pe != nil {
		t.Fatalf("24000 is call-only: %+v", wide.Rows[0])
	}
}

func TestOptionChainDegradesWhenQuantIsDown(t *testing.T) {
	s := chainServer(store.SegmentCodeIDX, traderAccounts, &fakeQuant{err: errors.New("connection refused")}, &fakeOI{})
	res, err := s.GetOptionChain(asUser("u1"), GetOptionChainRequestObject{UnderlyingId: 100})
	if err != nil {
		t.Fatal(err)
	}
	oc := res.(GetOptionChain200JSONResponse)
	if oc.Analytics != OptionChainAnalyticsUnavailable || oc.Pcr != nil || len(oc.Rows) != 6 || *oc.Rows[3].Ce.Ltp != "180.5000" || oc.Rows[3].Ce.Iv != nil {
		t.Fatalf("degraded chain %+v", oc)
	}
}

func TestOptionChainNeedsEntitlement(t *testing.T) {
	for name, accounts := range map[string][]ws.Account{
		"no accounts":              nil,
		"account not entitled":     {{ID: "acct", Broker: "zerodha"}},
		"upstox for kite-only ids": {{ID: "acct", Broker: "upstox", DataEntitled: true}},
		"sim account":              {{ID: "acct", Broker: "sim", DataEntitled: true}},
	} {
		t.Run(name, func(t *testing.T) {
			q := &fakeQuant{resp: quantEcho}
			res, err := chainServer(store.SegmentCodeIDX, accounts, q, &fakeOI{}).
				GetOptionChain(asUser("u1"), GetOptionChainRequestObject{UnderlyingId: 100})
			if err != nil {
				t.Fatal(err)
			}
			if _, denied := res.(GetOptionChain403ApplicationProblemPlusJSONResponse); !denied || q.req != nil {
				t.Fatalf("res = %#v, quant called: %v", res, q.req != nil)
			}
		})
	}
}

func TestOptionChainNotFoundAndStockRouting(t *testing.T) {
	s := chainServer(store.SegmentCodeIDX, traderAccounts, &fakeQuant{resp: quantEcho}, &fakeOI{})
	for _, req := range []GetOptionChainRequestObject{
		{UnderlyingId: 999},
		{UnderlyingId: 100, Params: GetOptionChainParams{Expiry: &openapi_types.Date{Time: time.Date(2026, 12, 1, 0, 0, 0, 0, time.UTC)}}},
	} {
		if res, _ := s.GetOptionChain(asUser("u1"), req); res == nil {
			t.Fatal("no response")
		} else if _, nf := res.(GetOptionChain404ApplicationProblemPlusJSONResponse); !nf {
			t.Fatalf("%+v: %#v", req, res)
		}
	}

	q := &fakeQuant{resp: quantEcho}
	if _, err := chainServer(store.SegmentCodeEQ, traderAccounts, q, &fakeOI{}).
		GetOptionChain(asUser("u1"), GetOptionChainRequestObject{UnderlyingId: 100}); err != nil {
		t.Fatal(err)
	}
	if q.req.Kind != "stock" || q.req.Future != nil || len(q.req.Dividends) != 1 || q.req.Dividends[0] != (quantc.Dividend{ExDate: "2026-10-30", Amount: "10"}) {
		t.Fatalf("stock request %+v", q.req)
	}
}

func TestChainWindow(t *testing.T) {
	pairs := make([]contractPair, 11)
	for i := range pairs {
		pairs[i].k = float64(24000 + 50*i)
	}
	for _, tc := range []struct {
		ref    int64
		n      int
		lo, hi int
	}{
		{0, 2, 3, 8},             // no price yet: around the middle strike (24250)
		{24_260_0000, 1, 4, 7},   // nearest 24250 (index 5)
		{24_275_0000, 1, 4, 7},   // tie 24250/24300 → lower
		{23_000_0000, 3, 0, 4},   // clamped at the bottom
		{25_000_0000, 3, 7, 11},  // clamped at the top
		{24_250_0000, 50, 0, 11}, // window wider than the chain
	} {
		if lo, hi := window(pairs, tc.ref, tc.n); lo != tc.lo || hi != tc.hi {
			t.Errorf("window(%d, %d) = [%d, %d), want [%d, %d)", tc.ref, tc.n, lo, hi, tc.lo, tc.hi)
		}
	}
}

func TestGetExpiries(t *testing.T) {
	s := chainServer(store.SegmentCodeIDX, traderAccounts, &fakeQuant{resp: quantEcho}, &fakeOI{})
	res, err := s.GetExpiries(asUser("u1"), GetExpiriesRequestObject{Id: 100})
	if err != nil {
		t.Fatal(err)
	}
	if days := res.(GetExpiries200JSONResponse); len(days) != 2 || days[0].Format(time.DateOnly) != "2026-11-03" {
		t.Fatalf("expiries %v", days)
	}
}

func TestChainQueries(t *testing.T) {
	_, pool := testdb.New(t)
	ctx := context.Background()
	if _, err := pool.Exec(ctx, `INSERT INTO instrument (id, exchange, segment, symbol, tradingsymbol, tick_size, lot_size, valid_from, underlying_id, expiry, strike, option_type, valid_to) VALUES
		(1, 'NSE', 'IDX', 'NIFTY', 'NIFTY 50', 0.05, 1, '2026-01-01', NULL, NULL, NULL, NULL, NULL),
		(2, 'NSE', 'EQ', 'TCS', 'TCS', 0.05, 1, '2026-01-01', NULL, NULL, NULL, NULL, NULL),
		(10, 'NFO', 'OPT', 'NIFTY', 'N03_24300PE', 0.05, 75, '2026-01-01', 1, '2026-11-03', 24300, 'PE', NULL),
		(11, 'NFO', 'OPT', 'NIFTY', 'N03_24300CE', 0.05, 75, '2026-01-01', 1, '2026-11-03', 24300, 'CE', NULL),
		(12, 'NFO', 'OPT', 'NIFTY', 'N03_24200CE', 0.05, 75, '2026-01-01', 1, '2026-11-03', 24200, 'CE', NULL),
		(13, 'NFO', 'OPT', 'NIFTY', 'N03_24100CE', 0.05, 75, '2026-01-01', 1, '2026-11-03', 24100, 'CE', '2026-10-20'),
		(14, 'NFO', 'OPT', 'NIFTY', 'N10_24300CE', 0.05, 75, '2026-01-01', 1, '2026-11-10', 24300, 'CE', NULL),
		(15, 'NFO', 'OPT', 'NIFTY', 'O27_24300CE', 0.05, 75, '2026-01-01', 1, '2026-10-27', 24300, 'CE', NULL),
		(20, 'NFO', 'FUT', 'NIFTY', 'N03FUT', 0.10, 75, '2026-01-01', 1, '2026-11-03', NULL, NULL, NULL);
		INSERT INTO corporate_action (instrument_id, ex_date, kind, amount, price_factor, volume_factor, source) VALUES
		(2, '2026-10-29', 'dividend', 5, 1, 1, 'test'), (2, '2026-11-01', 'dividend', 10, 1, 1, 'test'),
		(2, '2026-11-02', 'split', NULL, 0.5, 2, 'test'), (2, '2026-12-01', 'dividend', 99, 1, 1, 'test')`); err != nil {
		t.Fatal(err)
	}
	q := store.New(pool)
	day := func(s string) pgtype.Date {
		d, _ := time.Parse(time.DateOnly, s)
		return pgtype.Date{Time: d, Valid: true}
	}
	nifty := pgtype.Int8{Int64: 1, Valid: true}

	exp, err := q.ListOptionExpiries(ctx, store.ListOptionExpiriesParams{UnderlyingID: nifty, FromDay: day("2026-10-29")})
	if err != nil || len(exp) != 2 || exp[0] != day("2026-11-03") || exp[1] != day("2026-11-10") {
		t.Fatalf("expiries %v, %v (the 27th has passed)", exp, err)
	}
	rows, err := q.ListChainContracts(ctx, store.ListChainContractsParams{UnderlyingID: nifty, Expiry: day("2026-11-03")})
	if err != nil {
		t.Fatal(err)
	}
	var got []int64
	for _, r := range rows {
		got = append(got, r.ID)
	}
	if fmt.Sprint(got) != "[12 11 10]" { // strike ascending, CE before PE; delisted 13 excluded
		t.Fatalf("chain ids %v", got)
	}
	if id, err := q.GetSameExpiryFuture(ctx, store.GetSameExpiryFutureParams{UnderlyingID: nifty, Expiry: day("2026-11-03")}); err != nil || id != 20 {
		t.Fatalf("future %d, %v", id, err)
	}
	if _, err := q.GetSameExpiryFuture(ctx, store.GetSameExpiryFutureParams{UnderlyingID: nifty, Expiry: day("2026-11-10")}); !errors.Is(err, pgx.ErrNoRows) {
		t.Fatalf("weekly expiry has no future: %v", err)
	}
	divs, err := q.ListUpcomingDividends(ctx, store.ListUpcomingDividendsParams{InstrumentID: 2, After: day("2026-10-29"), Until: day("2026-11-26")})
	if err != nil || len(divs) != 1 || divs[0].ExDate != day("2026-11-01") || numeric(divs[0].Amount) != "10.0000" {
		t.Fatalf("dividends %+v, %v (ex today, splits and after-expiry excluded)", divs, err)
	}
}
