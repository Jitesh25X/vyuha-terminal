package api

import (
	"context"
	"io"
	"log/slog"
	"testing"
	"time"

	openapi_types "github.com/oapi-codegen/runtime/types"

	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/config"
)

type fakeConstituents map[int64]*IndexConstituents

func (f fakeConstituents) Constituents(_ context.Context, id int64) (*IndexConstituents, error) {
	return f[id], nil
}

func TestIndexConstituents(t *testing.T) {
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	req := GetIndexConstituentsRequestObject{Id: 1100}

	// No source configured (until M6.1): a clear 404, never an empty list that looks like real data.
	s := NewServer("0", config.EnvDev, fakeDB{}, nil, logger)
	res, err := s.GetIndexConstituents(context.Background(), req)
	if err != nil {
		t.Fatal(err)
	}
	if nf, ok := res.(GetIndexConstituents404ApplicationProblemPlusJSONResponse); !ok || *nf.Code != "NO_CONSTITUENTS" || *nf.Detail == "" {
		t.Fatalf("no source: %#v", res)
	}

	w := 11.2
	nifty := &IndexConstituents{IndexId: 1100, AsOf: openapi_types.Date{Time: time.Date(2026, 10, 1, 0, 0, 0, 0, time.UTC)},
		Constituents: []Constituent{{InstrumentId: 1001, Tradingsymbol: "RELIANCE", Weight: &w}}}
	s = s.WithConstituents(fakeConstituents{1100: nifty})
	res, err = s.GetIndexConstituents(context.Background(), req)
	if err != nil {
		t.Fatal(err)
	}
	if got, ok := res.(GetIndexConstituents200JSONResponse); !ok || len(got.Constituents) != 1 || *got.Constituents[0].Weight != 11.2 {
		t.Fatalf("with source: %#v", res)
	}
	res, _ = s.GetIndexConstituents(context.Background(), GetIndexConstituentsRequestObject{Id: 7})
	if _, ok := res.(GetIndexConstituents404ApplicationProblemPlusJSONResponse); !ok {
		t.Fatalf("unknown index: %#v", res)
	}
}
