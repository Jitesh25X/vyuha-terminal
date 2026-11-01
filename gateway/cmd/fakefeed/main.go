// Command fakefeed stands in for the gateway's /ws in the web perf test (scripts/e2e.sh, e2e/perf.spec.ts):
// it speaks the real protobuf protocol, accepts any ClientHello, and pushes one UpdateBatch with every
// subscribed quote key at a fixed rate. Dev/e2e only; never deployed.
package main

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"strconv"
	"sync"
	"sync/atomic"
	"time"

	"github.com/coder/websocket"
	"google.golang.org/protobuf/proto"

	commonv1 "github.com/jiteshraj/vyuha/gateway/gen/vyuha/common/v1"
	mdv1 "github.com/jiteshraj/vyuha/gateway/gen/vyuha/marketdata/v1"
	wsv1 "github.com/jiteshraj/vyuha/gateway/gen/vyuha/ws/v1"
)

type feed struct {
	hz         float64
	batches    atomic.Int64
	subscribed atomic.Int64
}

func main() {
	addr := flag.String("addr", "127.0.0.1:18091", "listen address")
	hz := flag.Float64("hz", 4, "updates per second for every subscribed key")
	flag.Parse()
	f := &feed{hz: *hz}
	mux := http.NewServeMux()
	mux.HandleFunc("/ws", f.serve)
	mux.HandleFunc("/stats", func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(map[string]int64{"batches": f.batches.Load(), "subscribed": f.subscribed.Load()})
	})
	srv := &http.Server{Addr: *addr, Handler: mux, ReadHeaderTimeout: 5 * time.Second}
	slog.Info("fakefeed listening", "addr", *addr, "hz", *hz)
	if err := srv.ListenAndServe(); err != nil {
		fmt.Fprintln(os.Stderr, "fakefeed:", err)
		os.Exit(1)
	}
}

func (f *feed) serve(w http.ResponseWriter, r *http.Request) {
	// The page is served by Vite on another loopback port.
	conn, err := websocket.Accept(w, r, &websocket.AcceptOptions{OriginPatterns: []string{"127.0.0.1:*", "localhost:*"}})
	if err != nil {
		return
	}
	conn.SetReadLimit(1 << 20)
	ctx, cancel := context.WithCancel(r.Context())
	defer cancel()
	var mu sync.Mutex
	ids := map[uint64]bool{}
	send := func(m *wsv1.ServerMsg) error {
		b, err := proto.Marshal(m)
		if err != nil {
			return err
		}
		return conn.Write(ctx, websocket.MessageBinary, b)
	}
	go func() {
		t := time.NewTicker(time.Duration(float64(time.Second) / f.hz))
		defer t.Stop()
		for seq := int64(1); ; seq++ {
			select {
			case <-ctx.Done():
				return
			case <-t.C:
			}
			mu.Lock()
			now := time.Now()
			items := make([]*wsv1.Item, 0, len(ids))
			for id := range ids {
				ltp := 1_000_000 + int64((id*7919+uint64(seq)*104_729)%20_000)*5 //nolint:gosec // G115: small test values
				items = append(items, &wsv1.Item{Value: &wsv1.Item_Quote{Quote: &mdv1.Snapshot{
					Tick: &mdv1.Tick{
						InstrumentId: id, Ltp: ltp, Ltq: uint32(1 + seq%50), PrevClose: 1_000_000, //nolint:gosec // G115: small
						Bid: ltp - 500, Ask: ltp + 500, Volume: uint64(seq*100) + id, //nolint:gosec // G115: small
						ExchangeTsNs: now.UnixNano(), RecvTsNs: now.UnixNano(), FeedId: 1,
					},
					ChangePct: float64(ltp-1_000_000) / 10_000,
				}}})
			}
			mu.Unlock()
			if len(items) == 0 {
				continue
			}
			if send(&wsv1.ServerMsg{Body: &wsv1.ServerMsg_Updates{Updates: &wsv1.UpdateBatch{
				Channel: wsv1.Channel_CHANNEL_QUOTE, Items: items, ServerTsMs: now.UnixMilli(),
			}}}) != nil {
				cancel()
				return
			}
			f.batches.Add(1)
		}
	}()
	for {
		_, data, err := conn.Read(ctx)
		if err != nil {
			return
		}
		var m wsv1.ClientMsg
		if proto.Unmarshal(data, &m) != nil {
			continue
		}
		switch b := m.GetBody().(type) {
		case *wsv1.ClientMsg_Ping:
			_ = send(&wsv1.ServerMsg{ReqId: m.GetReqId(), Body: &wsv1.ServerMsg_Pong{Pong: &wsv1.Pong{
				ClientTsMs: b.Ping.GetClientTsMs(), ServerTsMs: time.Now().UnixMilli(),
			}}})
		case *wsv1.ClientMsg_Hello:
			_ = send(&wsv1.ServerMsg{Body: &wsv1.ServerMsg_Welcome{Welcome: &wsv1.Welcome{UserId: "perf", Role: "trader", ServerTsMs: time.Now().UnixMilli()}}})
		case *wsv1.ClientMsg_Subscribe:
			var items []*wsv1.Item
			if b.Subscribe.GetChannel() == wsv1.Channel_CHANNEL_SYSTEM {
				mu.Lock()
				n := uint32(len(ids)) //nolint:gosec // G115: bounded by the 2,000-key limit
				mu.Unlock()
				items = []*wsv1.Item{{Value: &wsv1.Item_System{System: &wsv1.SystemStatus{
					MarketState: "open", EngineOk: true, DataLatencyMs: 3,
					Feeds: []*mdv1.FeedStatus{{AccountId: "perf-acct", Broker: commonv1.Broker_BROKER_UPSTOX,
						State: mdv1.FeedStatus_STATE_CONNECTED, Label: "PERF01", Subscribed: n}},
				}}}}
			} else if b.Subscribe.GetChannel() == wsv1.Channel_CHANNEL_QUOTE {
				mu.Lock()
				for _, k := range b.Subscribe.GetKeys() {
					if id, err := strconv.ParseUint(k, 10, 64); err == nil {
						ids[id] = true
					}
				}
				f.subscribed.Store(int64(len(ids)))
				mu.Unlock()
			}
			_ = send(&wsv1.ServerMsg{ReqId: m.GetReqId(), Body: &wsv1.ServerMsg_Snapshot{Snapshot: &wsv1.Snapshot{Channel: b.Subscribe.GetChannel(), Items: items}}})
		}
	}
}
