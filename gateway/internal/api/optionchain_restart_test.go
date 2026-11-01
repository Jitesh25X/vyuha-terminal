package api

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"io"
	"log/slog"
	"os"
	"strconv"
	"testing"
	"time"

	"github.com/nats-io/nats.go"
	"github.com/nats-io/nats.go/jetstream"
	"google.golang.org/protobuf/proto"

	enginev1 "github.com/Jitesh25X/vyuha-terminal/gateway/gen/vyuha/engine/v1"
	marketdatav1 "github.com/Jitesh25X/vyuha-terminal/gateway/gen/vyuha/marketdata/v1"
	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/config"
	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/quantc"
	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/store"
	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/ws"
)

// M1.11b done check: after a gateway restart (fresh hub, empty cache) the engine's last trade time
// comes back from md_last, so an option whose spread is too wide for the mid still gets an IV from its
// recent LTP. Needs the dev NATS (make up). With VYUHA_TEST_QUANT_URL set (a running
// `vyuha-quant serve`) the real quant service computes the IV; otherwise a fake checks what it was sent.
func TestOptionChainLTPIVSurvivesGatewayRestart(t *testing.T) {
	natsURL := os.Getenv("VYUHA_TEST_NATS_URL")
	if natsURL == "" {
		t.Skip("VYUHA_TEST_NATS_URL not set")
	}
	nc, err := nats.Connect(natsURL)
	if err != nil {
		t.Fatal(err)
	}
	defer nc.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	var b [4]byte
	_, _ = rand.Read(b[:])
	bucket := "md_last_test_" + hex.EncodeToString(b[:]) // never the real md_last
	js, err := jetstream.New(nc)
	if err != nil {
		t.Fatal(err)
	}
	kv, err := js.CreateKeyValue(ctx, jetstream.KeyValueConfig{Bucket: bucket, History: 1, Storage: jetstream.MemoryStorage})
	if err != nil {
		t.Fatal(err)
	}
	defer func() { _ = js.DeleteKeyValue(context.Background(), bucket) }()

	// What the engine left in md_last before the restart (feed 3 = the trader's account "acct").
	traded := chainNow.Add(-10 * time.Second)
	for _, tk := range []*marketdatav1.Tick{
		{InstrumentId: 100, Ltp: 24_290_0000, FeedId: 3},
		{InstrumentId: 200, Ltp: 24_320_0000, FeedId: 3},
		// 24300 CE: a one-sided quote (no mid) that traded 10 s ago at ₹180.50.
		{InstrumentId: 1003, Ltp: 180_5000, Bid: 175_0000, Oi: 1500, Volume: 12_000, FeedId: 3, LastTradeTsNs: traded.UnixNano()},
		// 24300 PE: same, but the engine never saw it trade (unknown): no IV.
		{InstrumentId: 2003, Ltp: 150_0000, Oi: 900, Volume: 0, FeedId: 3},
	} {
		v, _ := proto.Marshal(&marketdatav1.Snapshot{Tick: tk})
		if _, err := kv.Put(ctx, strconv.FormatUint(tk.GetInstrumentId(), 10), v); err != nil {
			t.Fatal(err)
		}
	}

	// The restarted gateway: a new hub with nothing cached; it learns feed 3 → "acct" from the engine.
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	hub := ws.New(ws.DefaultConfig(), nil, nil, nil, ws.NewNATSSource(nc, bucket), logger, time.Now)
	hub.ObserveSession(&enginev1.SessionInfo{AccountId: "acct", FeedId: 3, State: "connected"})

	var quant ChainAnalytics
	rec := &fakeQuant{resp: quantEcho}
	realQuant := os.Getenv("VYUHA_TEST_QUANT_URL")
	if realQuant != "" {
		quant = quantc.New(realQuant)
	} else {
		quant = rec
	}
	s := NewServer("0", config.EnvDev, fakeDB{}, nil, logger).WithOptionChains(&OptionChains{
		Store: fakeChainStore{segment: store.SegmentCodeIDX, future: true}, Entitlement: chainEntitlement{traderAccounts},
		Latest: hub, OI: &fakeOI{}, Quant: quant, Now: func() time.Time { return chainNow },
	})
	res, err := s.GetOptionChain(asUser("u1"), GetOptionChainRequestObject{UnderlyingId: 100, Params: GetOptionChainParams{Strikes: ptr(1)}})
	if err != nil {
		t.Fatal(err)
	}
	oc, ok := res.(GetOptionChain200JSONResponse)
	if !ok || len(oc.Rows) != 3 || oc.Rows[1].Strike != "24300.0000" {
		t.Fatalf("chain %#v", res)
	}
	ce, pe := oc.Rows[1].Ce, oc.Rows[1].Pe
	if ce == nil || ce.Ltp == nil || *ce.Ltp != "180.5000" || pe == nil || pe.Ltp == nil {
		t.Fatalf("market fields not restored from md_last: ce %+v pe %+v", ce, pe)
	}

	if realQuant == "" {
		at := rec.req.Strikes[1].CE.TradedAt
		if at == nil || !at.Equal(traded) || rec.req.Strikes[1].PE.TradedAt != nil {
			t.Fatalf("traded_at after restart: ce %v pe %v (want ce %v, pe none)", at, rec.req.Strikes[1].PE.TradedAt, traded)
		}
		return
	}
	// Real quant: the CE's IV comes from its LTP (marked), the PE without a known trade has none.
	if oc.Analytics != OptionChainAnalyticsOk || ce.Iv == nil || ce.IvSource == nil || *ce.IvSource != OptionSideIvSourceLtp {
		t.Fatalf("CE after restart: analytics %s iv %v source %v", oc.Analytics, ce.Iv, ce.IvSource)
	}
	if *ce.Iv < 0.01 || *ce.Iv > 1 {
		t.Fatalf("CE IV %v not plausible", *ce.Iv)
	}
	if pe.Iv != nil {
		t.Fatalf("PE without a known trade must have no IV, got %v", *pe.Iv)
	}
}
