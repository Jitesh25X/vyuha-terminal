package api

import (
	"context"
	"errors"
	"math"
	"net/http"
	"strconv"
	"sync"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgtype"
	openapi_types "github.com/oapi-codegen/runtime/types"

	marketdatav1 "github.com/jiteshraj/vyuha/gateway/gen/vyuha/marketdata/v1"
	"github.com/jiteshraj/vyuha/gateway/internal/quantc"
	"github.com/jiteshraj/vyuha/gateway/internal/store"
	"github.com/jiteshraj/vyuha/gateway/internal/ws"
)

// ChainStore is the part of store.Queries the option-chain routes use.
type ChainStore interface {
	GetInstrument(ctx context.Context, id int64) (store.GetInstrumentRow, error)
	ListOptionExpiries(ctx context.Context, arg store.ListOptionExpiriesParams) ([]pgtype.Date, error)
	ListChainContracts(ctx context.Context, arg store.ListChainContractsParams) ([]store.ListChainContractsRow, error)
	GetSameExpiryFuture(ctx context.Context, arg store.GetSameExpiryFutureParams) (int64, error)
	ListUpcomingDividends(ctx context.Context, arg store.ListUpcomingDividendsParams) ([]store.ListUpcomingDividendsRow, error)
}

// LatestTicks reads latest values under the hub's entitlement rule (ws.Hub.Latest).
type LatestTicks interface {
	Latest(ctx context.Context, accounts map[string]bool, ids []uint64) map[uint64]ws.LatestValue
}

// OIBaseline reads each instrument's last recorded OI before a time (candles.Reader.LastOI).
type OIBaseline interface {
	LastOI(ctx context.Context, ids []uint64, before time.Time) (map[uint64]uint64, error)
}

// ChainAnalytics computes IV/Greeks/PCR/max pain (quantc.Client).
type ChainAnalytics interface {
	OptionChain(ctx context.Context, req *quantc.ChainRequest) (*quantc.ChainResult, error)
}

// OptionChains wires GET /optionchain/{underlying_id} and /instruments/{id}/expiries.
type OptionChains struct {
	Store       ChainStore
	Entitlement ws.Directory
	Latest      LatestTicks
	OI          OIBaseline
	Quant       ChainAnalytics
	Now         func() time.Time

	// ΔOI baselines don't change during a session: cached per IST day (absent ids cached too, so an
	// instrument without history isn't re-queried every poll).
	oiMu   sync.Mutex
	oiDay  string
	oiBase map[uint64]oiEntry
}

type oiEntry struct {
	oi uint64
	ok bool
}

// maxOIBaseline bounds the per-day baseline cache (a full NIFTY chain across expiries is ~2,000).
const maxOIBaseline = 50_000

// WithOptionChains enables the option-chain routes.
func (s *Server) WithOptionChains(oc *OptionChains) *Server {
	if oc.Now == nil {
		oc.Now = time.Now
	}
	s.oc = oc
	return s
}

var istZone = time.FixedZone("IST", 5*3600+30*60)

// GetExpiries lists the active option expiries of an underlying from today (IST).
func (s *Server) GetExpiries(ctx context.Context, req GetExpiriesRequestObject) (GetExpiriesResponseObject, error) {
	if s.oc == nil {
		return nil, errors.New("option chain routes not configured")
	}
	ctx, cancel := context.WithTimeout(ctx, dbTimeout)
	defer cancel()
	today := s.oc.Now().In(istZone)
	days, err := s.oc.Store.ListOptionExpiries(ctx, store.ListOptionExpiriesParams{
		UnderlyingID: pgtype.Int8{Int64: req.Id, Valid: true},
		FromDay:      pgtype.Date{Time: time.Date(today.Year(), today.Month(), today.Day(), 0, 0, 0, 0, time.UTC), Valid: true},
	})
	if err != nil {
		return nil, err
	}
	out := make(GetExpiries200JSONResponse, 0, len(days))
	for _, d := range days {
		out = append(out, openapi_types.Date{Time: d.Time})
	}
	return out, nil
}

// contractPair is one strike's call and put.
type contractPair struct {
	strike  string
	k       float64 // for ATM selection only (display analytics, not money)
	ce, pe  *store.ListChainContractsRow
	tickStr string
}

// GetOptionChain returns one expiry's chain around ATM with analytics from quant (F-MD-3).
func (s *Server) GetOptionChain(ctx context.Context, req GetOptionChainRequestObject) (GetOptionChainResponseObject, error) {
	if s.oc == nil {
		return nil, errors.New("option chain routes not configured")
	}
	oc := s.oc
	notFound := func(title string) GetOptionChainResponseObject {
		return GetOptionChain404ApplicationProblemPlusJSONResponse{NotFoundApplicationProblemPlusJSONResponse(
			problem(http.StatusNotFound, "NOT_FOUND", title, ""))}
	}
	now := oc.Now()
	today := now.In(istZone)
	todayDate := time.Date(today.Year(), today.Month(), today.Day(), 0, 0, 0, 0, time.UTC)
	uid := pgtype.Int8{Int64: req.UnderlyingId, Valid: true}

	dctx, cancel := context.WithTimeout(ctx, dbTimeout)
	defer cancel()
	under, err := oc.Store.GetInstrument(dctx, req.UnderlyingId)
	if errors.Is(err, pgx.ErrNoRows) {
		return notFound("Instrument not found"), nil
	} else if err != nil {
		return nil, err
	}
	expiry := todayDate
	if req.Params.Expiry != nil {
		expiry = req.Params.Expiry.Time
	} else {
		days, err := oc.Store.ListOptionExpiries(dctx, store.ListOptionExpiriesParams{UnderlyingID: uid, FromDay: pgtype.Date{Time: todayDate, Valid: true}})
		if err != nil {
			return nil, err
		}
		if len(days) == 0 {
			return notFound("No listed options for " + under.Symbol), nil
		}
		expiry = days[0].Time
	}
	exp := pgtype.Date{Time: expiry, Valid: true}
	contracts, err := oc.Store.ListChainContracts(dctx, store.ListChainContractsParams{UnderlyingID: uid, Expiry: exp})
	if err != nil {
		return nil, err
	}
	if len(contracts) == 0 {
		return notFound("No listed options for " + under.Symbol + " on " + expiry.Format(time.DateOnly)), nil
	}
	var futureID int64
	if id, err := oc.Store.GetSameExpiryFuture(dctx, store.GetSameExpiryFutureParams{UnderlyingID: uid, Expiry: exp}); err == nil {
		futureID = id
	} else if !errors.Is(err, pgx.ErrNoRows) {
		return nil, err
	}

	// Entitlement (ADR-009): the user needs a data-entitled account whose broker streams these options.
	accounts, err := oc.Entitlement.UserAccounts(dctx, ClaimsFrom(ctx).Subject)
	if err != nil {
		return nil, err
	}
	pairs := pairContracts(contracts)
	data := ws.DataAccounts(accounts)

	// Window: ±N strikes around the strike nearest the underlying (future when the spot is unknown).
	n := 20
	if req.Params.Strikes != nil {
		n = min(max(*req.Params.Strikes, 1), 50)
	}
	headIDs := []uint64{uint64(req.UnderlyingId)} //nolint:gosec // G115: ids are positive bigserials
	if futureID != 0 {
		headIDs = append(headIDs, uint64(futureID)) //nolint:gosec // G115: positive bigserial
	}
	head := oc.Latest.Latest(ctx, data, headIDs)
	spot, fut := ltpOf(head[uint64(req.UnderlyingId)].Tick), int64(0) //nolint:gosec // G115: positive bigserial
	if futureID != 0 {
		fut = ltpOf(head[uint64(futureID)].Tick) //nolint:gosec // G115: positive bigserial
	}
	ref := spot
	if ref == 0 {
		ref = fut
	}
	lo, hi := window(pairs, ref, n)
	win := pairs[lo:hi] // rows shown; PCR and max pain use every pair

	ids := make([]uint64, 0, len(contracts))
	contractIDs := make([]int64, 0, len(contracts))
	for _, c := range contracts {
		ids = append(ids, uint64(c.ID)) //nolint:gosec // G115: positive bigserial
		contractIDs = append(contractIDs, c.ID)
	}
	maps, err := oc.Entitlement.Instruments(dctx, ids)
	if err != nil {
		return nil, err
	}
	entitled := map[uint64]bool{}
	for _, id := range ids {
		if len(ws.EntitledVia(accounts, maps[id])) > 0 {
			entitled[id] = true
		}
	}
	if len(entitled) == 0 {
		return GetOptionChain403ApplicationProblemPlusJSONResponse{ForbiddenApplicationProblemPlusJSONResponse(
			problem(http.StatusForbidden, "MD_NOT_ENTITLED", ws.NotEntitledMessage, ""))}, nil
	}
	latest := oc.Latest.Latest(ctx, data, ids)
	shown := map[uint64]bool{} // ΔOI baselines are only needed for the rows shown
	for _, p := range win {
		for _, c := range []*store.ListChainContractsRow{p.ce, p.pe} {
			if c != nil && entitled[uint64(c.ID)] { //nolint:gosec // G115: positive bigserial
				shown[uint64(c.ID)] = true //nolint:gosec // G115: positive bigserial
			}
		}
	}
	open := time.Date(today.Year(), today.Month(), today.Day(), 9, 15, 0, 0, istZone)
	base := oc.baselines(ctx, s, shown, open)

	var dividends []quantc.Dividend
	if under.Segment == store.SegmentCodeEQ {
		rows, err := oc.Store.ListUpcomingDividends(dctx, store.ListUpcomingDividendsParams{
			InstrumentID: req.UnderlyingId, After: pgtype.Date{Time: todayDate, Valid: true}, Until: exp,
		})
		if err != nil {
			return nil, err
		}
		for _, d := range rows {
			dividends = append(dividends, quantc.Dividend{ExDate: d.ExDate.Time.Format(time.DateOnly), Amount: numeric(d.Amount)})
		}
	}

	out := OptionChain{
		Analytics: OptionChainAnalyticsUnavailable, AsOf: now.UTC(), Expiry: openapi_types.Date{Time: expiry},
		LotSize: int(contracts[0].LotSize), Rows: make([]OptionChainRow, len(win)), ContractIds: contractIDs,
	}
	out.Underlying.InstrumentId = req.UnderlyingId
	out.Underlying.Ltp = priceStr(spot)
	if futureID != 0 {
		out.Underlying.FutureId = &futureID
		out.Underlying.FutLtp = priceStr(fut)
	}
	qreq := &quantc.ChainRequest{
		Kind: "index", Now: now.UTC(), Expiry: expiry.Format(time.DateOnly), TickSize: win[0].tickStr,
		Spot: priceStr(spot), Future: priceStr(fut), Dividends: dividends, Strikes: make([]quantc.Strike, len(win)),
		OI: openInterest(pairs, latest),
	}
	if under.Segment != store.SegmentCodeIDX {
		qreq.Kind, qreq.Future = "stock", nil
	}
	for i, p := range win {
		out.Rows[i].Strike = p.strike
		qreq.Strikes[i].Strike = p.strike
		if p.ce != nil {
			out.Rows[i].Ce, qreq.Strikes[i].CE = side(p.ce, latest, base)
		}
		if p.pe != nil {
			out.Rows[i].Pe, qreq.Strikes[i].PE = side(p.pe, latest, base)
		}
	}

	res, err := oc.Quant.OptionChain(ctx, qreq)
	if err != nil {
		s.logger.WarnContext(ctx, "option chain analytics unavailable", "error", err, "request_id", RequestID(ctx))
		return GetOptionChain200JSONResponse(out), nil
	}
	out.Analytics = OptionChainAnalyticsOk
	model := OptionChainModel(res.Model)
	out.Model, out.Rate, out.TYears = &model, &res.Rate, &res.TYears
	out.Forward, out.Pcr, out.AtmStrike, out.MaxPain = finite(res.UnderlyingPrice), finite(res.PCR), res.ATMStrike, res.MaxPain
	out.OiStrikes = &res.OIStrikes
	if res.RateAsOf != nil {
		if d, err := time.Parse(time.DateOnly, *res.RateAsOf); err == nil {
			out.RateAsOf = &openapi_types.Date{Time: d}
		}
	}
	if res.ForwardSource != nil {
		fs := OptionChainForwardSource(*res.ForwardSource)
		out.ForwardSource = &fs
	}
	for i, r := range res.Rows {
		applyAnalytics(out.Rows[i].Ce, r.CE)
		applyAnalytics(out.Rows[i].Pe, r.PE)
	}
	return GetOptionChain200JSONResponse(out), nil
}

// pairContracts groups contracts (ordered by strike, CE before PE) into one pair per strike.
func pairContracts(rows []store.ListChainContractsRow) []contractPair {
	var out []contractPair
	for i := range rows {
		r := &rows[i]
		k := numeric(r.Strike)
		if len(out) == 0 || out[len(out)-1].strike != k {
			f, _ := strconv.ParseFloat(k, 64)
			out = append(out, contractPair{strike: k, k: f, tickStr: numeric(r.TickSize)})
		}
		p := &out[len(out)-1]
		if r.OptionType.OptionType == store.OptionTypeCE {
			p.ce = r
		} else {
			p.pe = r
		}
	}
	return out
}

// window returns [lo, hi) covering n strikes either side of the strike nearest ref (price units); the
// middle strike when ref is 0 (no price seen yet). Ties go to the lower strike.
func window(pairs []contractPair, ref int64, n int) (int, int) {
	atm := len(pairs) / 2
	if ref > 0 {
		x := float64(ref) / 10_000
		best := math.Inf(1)
		for i, p := range pairs {
			if d := math.Abs(p.k - x); d < best {
				best, atm = d, i
			}
		}
	}
	return max(atm-n, 0), min(atm+n+1, len(pairs))
}

// baselines returns ΔOI baselines for ids from the per-day cache, querying ClickHouse for misses. A
// query failure leaves those ids without ΔOI (not cached, retried next poll).
func (oc *OptionChains) baselines(ctx context.Context, s *Server, ids map[uint64]bool, open time.Time) map[uint64]uint64 {
	day := open.Format(time.DateOnly)
	out := make(map[uint64]uint64, len(ids))
	var missing []uint64
	oc.oiMu.Lock()
	if oc.oiDay != day || len(oc.oiBase) > maxOIBaseline {
		oc.oiDay, oc.oiBase = day, map[uint64]oiEntry{}
	}
	for id := range ids {
		if e, ok := oc.oiBase[id]; !ok {
			missing = append(missing, id)
		} else if e.ok {
			out[id] = e.oi
		}
	}
	oc.oiMu.Unlock()
	if len(missing) == 0 || oc.OI == nil {
		return out
	}
	got, err := oc.OI.LastOI(ctx, missing, open)
	if err != nil {
		s.logger.WarnContext(ctx, "oi baseline query failed", "error", err, "request_id", RequestID(ctx))
		return out
	}
	oc.oiMu.Lock()
	defer oc.oiMu.Unlock()
	if oc.oiDay != day {
		return out
	}
	for _, id := range missing {
		v, ok := got[id]
		oc.oiBase[id] = oiEntry{oi: v, ok: ok}
		if ok {
			out[id] = v
		}
	}
	return out
}

func ltpOf(t *marketdatav1.Tick) int64 {
	if t == nil {
		return 0
	}
	return t.GetLtp()
}

// priceStr renders a positive price in units as a decimal string; nil for "no price".
func priceStr(v int64) *string {
	if v <= 0 {
		return nil
	}
	s := units(v)
	return &s
}

// openInterest lists every strike of the expiry with OI from the user's latest ticks (PCR, max pain).
func openInterest(pairs []contractPair, latest map[uint64]ws.LatestValue) []quantc.OIStrike {
	oi := func(c *store.ListChainContractsRow) uint64 {
		if c == nil {
			return 0
		}
		return latest[uint64(c.ID)].Tick.GetOi() //nolint:gosec // G115: positive bigserial
	}
	out := make([]quantc.OIStrike, 0, len(pairs))
	for _, p := range pairs {
		if ce, pe := oi(p.ce), oi(p.pe); ce > 0 || pe > 0 {
			out = append(out, quantc.OIStrike{Strike: p.strike, CEOI: ce, PEOI: pe})
		}
	}
	return out
}

// side builds the API contract and the quant quote from the user's latest tick, if any.
func side(c *store.ListChainContractsRow, latest map[uint64]ws.LatestValue, base map[uint64]uint64) (*OptionSide, *quantc.SideQuote) {
	id := uint64(c.ID) //nolint:gosec // G115: positive bigserial
	o := &OptionSide{InstrumentId: c.ID, Tradingsymbol: c.Tradingsymbol}
	q := &quantc.SideQuote{}
	v := latest[id]
	t := v.Tick
	if t == nil {
		return o, q
	}
	if !v.TradedAt.IsZero() {
		at := v.TradedAt.UTC()
		q.TradedAt = &at
	}
	o.Ltp, o.Bid, o.Ask, o.PrevClose = priceStr(t.GetLtp()), priceStr(t.GetBid()), priceStr(t.GetAsk()), priceStr(t.GetPrevClose())
	q.LTP, q.Bid, q.Ask, q.OI = o.Ltp, o.Bid, o.Ask, t.GetOi()
	vol, oi := clampInt64(t.GetVolume()), clampInt64(t.GetOi())
	o.Volume, o.Oi = &vol, &oi
	if b, ok := base[id]; ok {
		chg := oi - clampInt64(b)
		o.OiChg = &chg
	}
	return o, q
}

func clampInt64(v uint64) int64 {
	if v > math.MaxInt64 {
		return math.MaxInt64
	}
	return int64(v)
}

func finite(v *float64) *float64 {
	if v == nil || math.IsNaN(*v) || math.IsInf(*v, 0) {
		return nil
	}
	return v
}

func applyAnalytics(o *OptionSide, a *quantc.Side) {
	if o == nil || a == nil {
		return
	}
	o.Iv, o.Delta, o.Gamma, o.Theta, o.Vega = finite(a.IV), finite(a.Delta), finite(a.Gamma), finite(a.Theta), finite(a.Vega)
	if a.IVSource != nil && o.Iv != nil {
		src := OptionSideIvSource(*a.IVSource)
		o.IvSource = &src
	}
}
