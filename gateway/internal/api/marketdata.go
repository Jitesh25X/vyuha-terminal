package api

import (
	"context"
	"errors"
	"net/http"
	"strconv"
	"strings"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgtype"
	openapi_types "github.com/oapi-codegen/runtime/types"

	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/candles"
	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/store"
	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/ws"
)

// InstrumentStore is the part of store.Queries the instrument routes use.
type InstrumentStore interface {
	SearchInstruments(ctx context.Context, arg store.SearchInstrumentsParams) ([]store.SearchInstrumentsRow, error)
	GetInstrument(ctx context.Context, id int64) (store.GetInstrumentRow, error)
}

// CandleReader reads historical bars (candles.Reader).
type CandleReader interface {
	Bars(ctx context.Context, instrumentID uint64, interval string, from, to time.Time) ([]candles.Bar, error)
}

// MarketData wires the instrument and candle routes.
type MarketData struct {
	Instruments InstrumentStore
	Candles     CandleReader
	Entitlement ws.Directory
}

// WithMarketData enables /instruments/* and /marketdata/candles.
func (s *Server) WithMarketData(md *MarketData) *Server {
	s.md = md
	return s
}

// likeEscaper escapes LIKE wildcards in user input (Postgres' default escape is backslash).
var likeEscaper = strings.NewReplacer(`\`, `\\`, `%`, `\%`, `_`, `\_`)

// SearchInstruments: prefix search over active instruments.
func (s *Server) SearchInstruments(ctx context.Context, req SearchInstrumentsRequestObject) (SearchInstrumentsResponseObject, error) {
	if s.md == nil {
		return nil, errors.New("market data routes not configured")
	}
	p := store.SearchInstrumentsParams{Prefix: likeEscaper.Replace(strings.TrimSpace(req.Params.Q)), Lim: 20}
	if req.Params.Limit != nil {
		p.Lim = int32(min(max(*req.Params.Limit, 1), 50)) //nolint:gosec // G115: clamped to 1..50
	}
	if seg := req.Params.Segment; seg != nil {
		switch v := string(*seg); v {
		case "EQ", "FUT", "OPT", "IDX":
			p.Segment = store.NullSegmentCode{SegmentCode: store.SegmentCode(v), Valid: true}
		default:
			p.Exchange = store.NullExchangeCode{ExchangeCode: store.ExchangeCode(v), Valid: true}
		}
	}
	ctx, cancel := context.WithTimeout(ctx, dbTimeout)
	defer cancel()
	rows, err := s.md.Instruments.SearchInstruments(ctx, p)
	if err != nil {
		return nil, err
	}
	out := make(SearchInstruments200JSONResponse, len(rows))
	for i, r := range rows {
		out[i] = instrumentJSON(store.GetInstrumentRow(r))
	}
	return out, nil
}

// GetInstrument returns one instrument.
func (s *Server) GetInstrument(ctx context.Context, req GetInstrumentRequestObject) (GetInstrumentResponseObject, error) {
	if s.md == nil {
		return nil, errors.New("market data routes not configured")
	}
	ctx, cancel := context.WithTimeout(ctx, dbTimeout)
	defer cancel()
	r, err := s.md.Instruments.GetInstrument(ctx, req.Id)
	if errors.Is(err, pgx.ErrNoRows) {
		return GetInstrument404ApplicationProblemPlusJSONResponse{NotFoundApplicationProblemPlusJSONResponse(
			problem(http.StatusNotFound, "NOT_FOUND", "Instrument not found", ""))}, nil
	} else if err != nil {
		return nil, err
	}
	return GetInstrument200JSONResponse(instrumentJSON(r)), nil
}

// GetCandles returns historical bars for an instrument the user is entitled to.
func (s *Server) GetCandles(ctx context.Context, req GetCandlesRequestObject) (GetCandlesResponseObject, error) {
	if s.md == nil {
		return nil, errors.New("market data routes not configured")
	}
	p := req.Params
	if p.InstrumentId <= 0 || !p.To.After(p.From) {
		return GetCandles400ApplicationProblemPlusJSONResponse(problem(http.StatusBadRequest, "BAD_REQUEST", "Invalid range", "to must be after from")), nil
	}
	id := uint64(p.InstrumentId)
	dctx, cancel := context.WithTimeout(ctx, dbTimeout)
	accounts, err := s.md.Entitlement.UserAccounts(dctx, ClaimsFrom(ctx).Subject)
	var maps map[uint64]ws.InstrumentMap
	if err == nil {
		maps, err = s.md.Entitlement.Instruments(dctx, []uint64{id})
	}
	cancel()
	if err != nil {
		return nil, err
	}
	if len(ws.EntitledVia(accounts, maps[id])) == 0 {
		return GetCandles403ApplicationProblemPlusJSONResponse{ForbiddenApplicationProblemPlusJSONResponse(
			problem(http.StatusForbidden, "MD_NOT_ENTITLED", ws.NotEntitledMessage, ""))}, nil
	}
	bars, err := s.md.Candles.Bars(ctx, id, string(p.Interval), p.From, p.To)
	switch {
	case errors.Is(err, candles.ErrTooMany):
		return GetCandles400ApplicationProblemPlusJSONResponse(problem(http.StatusBadRequest, "BAD_REQUEST", "Too many bars",
			"at most "+strconv.Itoa(candles.MaxBars)+" bars per call; narrow the range or use a larger interval")), nil
	case err != nil:
		s.logger.ErrorContext(ctx, "candles query failed", "error", err, "request_id", RequestID(ctx))
		return GetCandles503ApplicationProblemPlusJSONResponse(problem(http.StatusServiceUnavailable, "MD_UNAVAILABLE", "Market data store unavailable", "")), nil
	}
	out := GetCandles200JSONResponse{Bars: make([][]interface{}, len(bars))}
	for i, b := range bars {
		out.Bars[i] = []interface{}{b.TS.Format(time.RFC3339), units(b.Open), units(b.High), units(b.Low), units(b.Close), b.Volume, b.OI}
	}
	return out, nil
}

// units formats an int64 price in units (1 rupee = 10,000) as a 4-decimal string.
func units(v int64) string {
	sign := ""
	u := uint64(v) //nolint:gosec // G115: magnitude taken below
	if v < 0 {
		sign, u = "-", uint64(-(v+1))+1 //nolint:gosec // G115: two's complement magnitude, safe for MinInt64
	}
	frac := strconv.FormatUint(u%10_000, 10)
	return sign + strconv.FormatUint(u/10_000, 10) + "." + strings.Repeat("0", 4-len(frac)) + frac
}

func instrumentJSON(r store.GetInstrumentRow) Instrument {
	out := Instrument{
		Id: r.ID, Exchange: InstrumentExchange(r.Exchange), Segment: InstrumentSegment(r.Segment),
		Symbol: r.Symbol, Tradingsymbol: r.Tradingsymbol, LotSize: int(r.LotSize), TickSize: numeric(r.TickSize), FoBan: r.FoBan,
	}
	if r.Name.Valid {
		out.Name = &r.Name.String
	}
	if r.UnderlyingID.Valid {
		out.UnderlyingId = &r.UnderlyingID.Int64
	}
	if r.Expiry.Valid {
		out.Expiry = &openapi_types.Date{Time: r.Expiry.Time}
	}
	if r.Strike.Valid {
		s := numeric(r.Strike)
		out.Strike = &s
	}
	if r.OptionType.Valid {
		o := InstrumentOptionType(r.OptionType.OptionType)
		out.OptionType = &o
	}
	if r.FreezeQty.Valid {
		f := int(r.FreezeQty.Int32)
		out.FreezeQty = &f
	}
	if r.ValidTo.Valid {
		out.ValidTo = &openapi_types.Date{Time: r.ValidTo.Time}
	}
	return out
}

// numeric renders a Postgres numeric as its exact decimal string.
func numeric(n pgtype.Numeric) string {
	v, err := n.Value()
	if s, ok := v.(string); ok && err == nil {
		return s
	}
	return ""
}
