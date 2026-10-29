package ws

import (
	"context"
	"errors"
	"io"
	"log/slog"
	"net/http/httptest"
	"slices"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/coder/websocket"
	"github.com/golang-jwt/jwt/v5"
	"google.golang.org/protobuf/proto"

	enginev1 "github.com/jiteshraj/vyuha/gateway/gen/vyuha/engine/v1"
	marketdatav1 "github.com/jiteshraj/vyuha/gateway/gen/vyuha/marketdata/v1"
	wsv1 "github.com/jiteshraj/vyuha/gateway/gen/vyuha/ws/v1"
	"github.com/jiteshraj/vyuha/gateway/internal/auth"
)

// ── fakes ────────────────────────────────────────────────────────────────────

type fakeDir struct {
	mu       sync.Mutex
	accounts map[string][]Account // user → accounts
	kiteAll  bool                 // every id is Kite-streamable (load tests)
	inst     map[uint64]InstrumentMap
}

func (d *fakeDir) UserAccounts(_ context.Context, user string) ([]Account, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	return slices.Clone(d.accounts[user]), nil
}

func (d *fakeDir) Instruments(_ context.Context, ids []uint64) (map[uint64]InstrumentMap, error) {
	out := map[uint64]InstrumentMap{}
	for _, id := range ids {
		if m, ok := d.inst[id]; ok {
			out[id] = m
		} else if d.kiteAll {
			out[id] = InstrumentMap{Kite: true}
		}
	}
	return out, nil
}

func (d *fakeDir) IsHoliday(context.Context, time.Time) (bool, error) { return false, nil }

type subCall struct {
	account string
	ids     []uint64
	mode    marketdatav1.FeedMode
	remove  bool
}

type fakeEngine struct {
	mu       sync.Mutex
	sessions []*enginev1.SessionInfo
	calls    []subCall
}

func (e *fakeEngine) GetSessions(context.Context) ([]*enginev1.SessionInfo, error) {
	e.mu.Lock()
	defer e.mu.Unlock()
	return slices.Clone(e.sessions), nil
}

func (e *fakeEngine) Subscribe(_ context.Context, ids []uint64, mode marketdatav1.FeedMode, via string) error {
	e.mu.Lock()
	defer e.mu.Unlock()
	e.calls = append(e.calls, subCall{account: via, ids: slices.Sorted(slices.Values(ids)), mode: mode})
	return nil
}

func (e *fakeEngine) Unsubscribe(_ context.Context, ids []uint64, via string) error {
	e.mu.Lock()
	defer e.mu.Unlock()
	e.calls = append(e.calls, subCall{account: via, ids: slices.Sorted(slices.Values(ids)), remove: true})
	return nil
}

func (e *fakeEngine) setFeed(account string, feed uint32) {
	e.mu.Lock()
	defer e.mu.Unlock()
	for i, s := range e.sessions {
		if s.GetAccountId() == account {
			c := proto.Clone(s).(*enginev1.SessionInfo)
			c.FeedId = feed
			e.sessions[i] = c
		}
	}
}

func (e *fakeEngine) callsFor(account string) []subCall {
	e.mu.Lock()
	defer e.mu.Unlock()
	var out []subCall
	for _, c := range e.calls {
		if c.account == account {
			out = append(out, c)
		}
	}
	return out
}

type fakeSource struct {
	ready  chan struct{}
	onTick func(*marketdatav1.Tick)
	onBar  func(*marketdatav1.Bar)
	last   map[uint64]*marketdatav1.Snapshot
}

func (s *fakeSource) Run(ctx context.Context, onTick func(*marketdatav1.Tick), onBar func(*marketdatav1.Bar)) error {
	s.onTick, s.onBar = onTick, onBar
	close(s.ready)
	<-ctx.Done()
	return nil
}

func (s *fakeSource) Last(_ context.Context, ids []uint64) (map[uint64]*marketdatav1.Snapshot, error) {
	out := map[uint64]*marketdatav1.Snapshot{}
	for _, id := range ids {
		if v, ok := s.last[id]; ok {
			out[id] = v
		}
	}
	return out, nil
}

func session(account string, broker string, feed uint32, entitled bool) *enginev1.SessionInfo {
	s := &enginev1.SessionInfo{AccountId: account, State: "connected", FeedId: feed}
	if entitled {
		s.Feeds = []*marketdatav1.FeedStatus{{AccountId: account, State: marketdatav1.FeedStatus_STATE_CONNECTED, Detail: broker}}
	}
	return s
}

// ── harness ──────────────────────────────────────────────────────────────────

type harness struct {
	t      *testing.T
	hub    *Hub
	dir    *fakeDir
	engine *fakeEngine
	src    *fakeSource
	url    string
	expiry map[string]time.Time
	mu     sync.Mutex
}

func newHarness(t *testing.T, cfg Config, dir *fakeDir, engine *fakeEngine) *harness {
	t.Helper()
	h := &harness{t: t, dir: dir, engine: engine, src: &fakeSource{ready: make(chan struct{}), last: map[uint64]*marketdatav1.Snapshot{}}, expiry: map[string]time.Time{}}
	verify := func(token string) (*auth.Claims, error) {
		user, role, ok := strings.Cut(token, ":")
		if !ok {
			return nil, errors.New("bad token")
		}
		h.mu.Lock()
		exp, set := h.expiry[user]
		h.mu.Unlock()
		if !set {
			exp = time.Now().Add(time.Hour)
		}
		return &auth.Claims{RegisteredClaims: jwt.RegisteredClaims{Subject: user, ExpiresAt: jwt.NewNumericDate(exp)}, Role: role}, nil
	}
	h.hub = New(cfg, verify, dir, engine, h.src, slog.New(slog.NewTextHandler(io.Discard, nil)), time.Now)
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() { h.hub.Run(ctx); close(done) }()
	srv := httptest.NewServer(h.hub)
	t.Cleanup(func() { srv.Close(); cancel(); <-done })
	h.url = "ws" + strings.TrimPrefix(srv.URL, "http")
	<-h.src.ready
	return h
}

// wsClient reads on a background goroutine: coder/websocket closes the connection when a Read's
// context ends, so per-read timeouts are done on the channel instead.
type wsClient struct {
	t       *testing.T
	conn    *websocket.Conn
	req     uint32
	frames  chan *wsv1.ServerMsg
	closed  chan struct{}
	readErr error
	started bool
}

func (c *wsClient) start() {
	if c.started {
		return
	}
	c.started = true
	go func() {
		defer close(c.closed)
		for {
			_, data, err := c.conn.Read(context.Background())
			if err != nil {
				c.readErr = err
				return
			}
			var m wsv1.ServerMsg
			if proto.Unmarshal(data, &m) == nil {
				c.frames <- &m
			}
		}
	}()
}

// closeStatus waits for the server to close the connection.
func (c *wsClient) closeStatus() websocket.StatusCode {
	c.start()
	for {
		select {
		case <-c.frames:
		case <-c.closed:
			return websocket.CloseStatus(c.readErr)
		case <-time.After(5 * time.Second):
			c.t.Fatal("connection not closed")
		}
	}
}

func (h *harness) dial(token string) *wsClient { return h.dialBuffered(token, 1024) }

// dialBuffered: the background reader stops reading the socket once `frames` unread frames are
// buffered, which is how a stalled browser looks to the server.
func (h *harness) dialBuffered(token string, frames int) *wsClient {
	h.t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	conn, resp, err := websocket.Dial(ctx, h.url, nil)
	if resp != nil && resp.Body != nil {
		_ = resp.Body.Close()
	}
	if err != nil {
		h.t.Fatalf("dial: %v", err)
	}
	conn.SetReadLimit(-1)
	h.t.Cleanup(func() { _ = conn.CloseNow() })
	c := &wsClient{t: h.t, conn: conn, frames: make(chan *wsv1.ServerMsg, frames), closed: make(chan struct{})}
	if token != "" {
		c.write(&wsv1.ClientMsg{Body: &wsv1.ClientMsg_Hello{Hello: &wsv1.ClientHello{AccessToken: token}}})
		if m := c.next(func(m *wsv1.ServerMsg) bool { return m.GetWelcome() != nil }); m == nil {
			h.t.Fatal("no Welcome")
		}
	}
	return c
}

func (c *wsClient) write(m *wsv1.ClientMsg) {
	c.t.Helper()
	b, _ := proto.Marshal(m)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := c.conn.Write(ctx, websocket.MessageBinary, b); err != nil {
		c.t.Fatalf("write: %v", err)
	}
}

func (c *wsClient) subscribe(ch wsv1.Channel, conflateMS uint32, ids ...uint64) uint32 {
	c.req++
	keys := make([]string, len(ids))
	for i, id := range ids {
		keys[i] = strconv.FormatUint(id, 10)
	}
	c.write(&wsv1.ClientMsg{ReqId: c.req, Body: &wsv1.ClientMsg_Subscribe{Subscribe: &wsv1.Subscribe{Channel: ch, Keys: keys, ConflateMs: conflateMS}}})
	return c.req
}

// read returns the next frame, or nil after d.
func (c *wsClient) read(d time.Duration) *wsv1.ServerMsg {
	c.start()
	select {
	case m := <-c.frames:
		return m
	case <-c.closed:
		select {
		case m := <-c.frames:
			return m
		default:
			return nil
		}
	case <-time.After(d):
		return nil
	}
}

// next skips frames until match (or fails after 5 s).
func (c *wsClient) next(match func(*wsv1.ServerMsg) bool) *wsv1.ServerMsg {
	c.t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if m := c.read(time.Until(deadline)); m != nil && match(m) {
			return m
		}
	}
	c.t.Fatal("expected frame not received")
	return nil
}

func isUpdate(m *wsv1.ServerMsg) bool {
	return m.GetUpdates() != nil && m.GetUpdates().GetChannel() == wsv1.Channel_CHANNEL_QUOTE
}

func waitFor(t *testing.T, what string, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for !cond() {
		if time.Now().After(deadline) {
			t.Fatalf("timed out waiting for %s", what)
		}
		time.Sleep(10 * time.Millisecond)
	}
}

func tick(id uint64, feed uint32, ltp int64) *marketdatav1.Tick {
	return &marketdatav1.Tick{InstrumentId: id, FeedId: feed, Ltp: ltp, PrevClose: 1_000_000, RecvTsNs: time.Now().UnixNano()}
}

func fastConfig() Config {
	cfg := DefaultConfig()
	cfg.SessionPoll = 20 * time.Millisecond
	cfg.HelloTimeout = 300 * time.Millisecond
	return cfg
}

// ── tests ────────────────────────────────────────────────────────────────────

// F-MD-2 AC5 / ADR-009: live ticks reach a user only through an account they are mapped to that has a
// data entitlement — and only from that account's own feed.
func TestEntitlement(t *testing.T) {
	dir := &fakeDir{
		accounts: map[string][]Account{
			"alice": {{ID: "acct-z", Broker: "zerodha", DataEntitled: true}},
			"bob":   {{ID: "acct-n", Broker: "zerodha", DataEntitled: false}}, // mapped, no entitlement
			"carol": {{ID: "acct-x", Broker: "zerodha", DataEntitled: true}},
			"dan":   nil, // not mapped at all
		},
		inst: map[uint64]InstrumentMap{101: {Kite: true, Upstox: true}, 102: {Upstox: true}},
	}
	engine := &fakeEngine{sessions: []*enginev1.SessionInfo{
		session("acct-z", "zerodha", 11, true), session("acct-n", "zerodha", 22, false), session("acct-x", "zerodha", 33, true),
	}}
	h := newHarness(t, fastConfig(), dir, engine)

	alice, bob, carol, dan := h.dial("alice:trader"), h.dial("bob:trader"), h.dial("carol:analyst"), h.dial("dan:admin")
	req := alice.subscribe(wsv1.Channel_CHANNEL_QUOTE, 50, 101, 102)
	if m := alice.next(func(m *wsv1.ServerMsg) bool { return m.GetSnapshot() != nil }); m.GetReqId() != req {
		t.Fatalf("snapshot req_id = %d, want %d", m.GetReqId(), req)
	}
	e := alice.next(func(m *wsv1.ServerMsg) bool { return m.GetError() != nil }).GetError()
	if e.GetCode() != "MD_NOT_ENTITLED" || !strings.HasPrefix(e.GetMessage(), NotEntitledMessage) || !strings.Contains(e.GetMessage(), "[102]") {
		t.Fatalf("alice error = %v; 102 is Upstox-only and alice has only a Kite account", e)
	}
	for _, c := range []*wsClient{bob, dan} {
		c.subscribe(wsv1.Channel_CHANNEL_QUOTE, 50, 101)
		if e := c.next(func(m *wsv1.ServerMsg) bool { return m.GetError() != nil }).GetError(); e.GetCode() != "MD_NOT_ENTITLED" {
			t.Fatalf("error = %v, want MD_NOT_ENTITLED", e)
		}
	}
	carol.subscribe(wsv1.Channel_CHANNEL_QUOTE, 50, 101)
	carol.next(func(m *wsv1.ServerMsg) bool { return m.GetSnapshot() != nil })

	// Each entitled user's interest is registered through their own account.
	waitFor(t, "engine subscriptions", func() bool {
		return len(engine.callsFor("acct-z")) == 1 && len(engine.callsFor("acct-x")) == 1
	})
	if c := engine.callsFor("acct-z")[0]; !slices.Equal(c.ids, []uint64{101}) || c.mode != marketdatav1.FeedMode_FEED_MODE_QUOTE {
		t.Fatalf("engine call = %+v", c)
	}
	if len(engine.callsFor("acct-n")) != 0 {
		t.Fatal("the non-entitled account must not be asked to stream")
	}

	// A tick from alice's feed reaches alice only; one from carol's feed reaches carol only.
	h.src.onTick(tick(101, 11, 2_450_000))
	h.src.onTick(tick(101, 33, 2_451_000))
	h.src.onTick(tick(101, 99, 2_452_000)) // feed the hub doesn't know: delivered to nobody
	for _, tc := range []struct {
		c    *wsClient
		want int64
	}{{alice, 2_450_000}, {carol, 2_451_000}} {
		u := tc.c.next(isUpdate).GetUpdates()
		if len(u.GetItems()) != 1 || u.GetItems()[0].GetQuote().GetTick().GetLtp() != tc.want {
			t.Fatalf("update = %v, want ltp %d only", u, tc.want)
		}
		if q := u.GetItems()[0].GetQuote(); q.GetChangePct() < 144 || q.GetChangePct() > 146 {
			t.Fatalf("change_pct = %v", q.GetChangePct())
		}
	}
	for _, c := range []*wsClient{alice, bob, carol, dan} {
		if m := c.read(300 * time.Millisecond); m != nil && m.GetUpdates() != nil {
			t.Fatalf("unexpected extra update %v", m)
		}
	}
	if h.hub.Stats().UnknownFeed != 1 {
		t.Fatalf("unknown feed ticks = %d", h.hub.Stats().UnknownFeed)
	}

	// Unmapping alice mid-session removes her subscription at the next entitlement refresh.
	dir.mu.Lock()
	dir.accounts["alice"] = nil
	dir.mu.Unlock()
	alice.req++
	alice.write(&wsv1.ClientMsg{ReqId: alice.req, Body: &wsv1.ClientMsg_TokenRefresh{TokenRefresh: &wsv1.TokenRefresh{AccessToken: "alice:trader"}}})
	if e := alice.next(func(m *wsv1.ServerMsg) bool { return m.GetError() != nil }).GetError(); e.GetCode() != "MD_NOT_ENTITLED" {
		t.Fatalf("after unmapping: %v", e)
	}
	waitFor(t, "engine unsubscribe", func() bool {
		calls := engine.callsFor("acct-z")
		return len(calls) == 2 && calls[1].remove
	})
	h.src.onTick(tick(101, 11, 2_460_000))
	if m := alice.read(300 * time.Millisecond); m != nil && m.GetUpdates() != nil {
		t.Fatal("unmapped user still receives ticks")
	}
}

func TestSnapshotOnlyFromTheUsersOwnFeed(t *testing.T) {
	dir := &fakeDir{
		accounts: map[string][]Account{"alice": {{ID: "acct-z", Broker: "zerodha", DataEntitled: true}}},
		inst:     map[uint64]InstrumentMap{101: {Kite: true}, 103: {Kite: true}},
	}
	engine := &fakeEngine{sessions: []*enginev1.SessionInfo{session("acct-z", "zerodha", 11, true), session("acct-x", "zerodha", 33, true)}}
	h := newHarness(t, fastConfig(), dir, engine)
	h.src.last[101] = &marketdatav1.Snapshot{Tick: tick(101, 11, 2_000_000)}
	h.src.last[103] = &marketdatav1.Snapshot{Tick: tick(103, 33, 3_000_000)} // someone else's feed
	waitFor(t, "first poll", func() bool { h.hub.mu.Lock(); defer h.hub.mu.Unlock(); return len(h.hub.feeds) == 2 })

	alice := h.dial("alice:trader")
	alice.subscribe(wsv1.Channel_CHANNEL_QUOTE, 0, 101, 103)
	items := alice.next(func(m *wsv1.ServerMsg) bool { return m.GetSnapshot() != nil }).GetSnapshot().GetItems()
	if len(items) != 1 || items[0].GetQuote().GetTick().GetInstrumentId() != 101 {
		t.Fatalf("snapshot = %v, want only 101 (103's md_last came from another account's feed)", items)
	}
}

func TestHelloIsRequired(t *testing.T) {
	h := newHarness(t, fastConfig(), &fakeDir{}, &fakeEngine{})
	for name, send := range map[string]func(*wsClient){
		"no hello": func(*wsClient) {},
		"bad token": func(c *wsClient) {
			c.write(&wsv1.ClientMsg{Body: &wsv1.ClientMsg_Hello{Hello: &wsv1.ClientHello{AccessToken: "garbage"}}})
		},
		"subscribe first": func(c *wsClient) {
			c.subscribe(wsv1.Channel_CHANNEL_QUOTE, 0, 1)
		},
	} {
		t.Run(name, func(t *testing.T) {
			c := h.dial("")
			send(c)
			if got := c.closeStatus(); got != 4401 {
				t.Fatalf("close status = %d, want 4401", got)
			}
		})
	}
}

func TestExpiredTokenClosesWith4401(t *testing.T) {
	h := newHarness(t, fastConfig(), &fakeDir{}, &fakeEngine{})
	h.expiry["alice"] = time.Now().Add(1500 * time.Millisecond)
	alice := h.dial("alice:trader")
	if e := alice.next(func(m *wsv1.ServerMsg) bool { return m.GetError() != nil }).GetError(); e.GetCode() != "AUTH_EXPIRED" {
		t.Fatalf("error = %v", e)
	}
	if got := alice.closeStatus(); got != 4401 {
		t.Fatalf("close status = %d, want 4401", got)
	}
}

func TestConflationSendsLatestValuePerInterval(t *testing.T) {
	dir := &fakeDir{accounts: map[string][]Account{"alice": {{ID: "acct-z", Broker: "zerodha", DataEntitled: true}}}, kiteAll: true}
	engine := &fakeEngine{sessions: []*enginev1.SessionInfo{session("acct-z", "zerodha", 11, true)}}
	h := newHarness(t, fastConfig(), dir, engine)
	alice := h.dial("alice:trader")
	alice.subscribe(wsv1.Channel_CHANNEL_QUOTE, 200, 101)
	alice.next(func(m *wsv1.ServerMsg) bool { return m.GetSnapshot() != nil })
	waitFor(t, "feed map", func() bool { h.hub.mu.Lock(); defer h.hub.mu.Unlock(); return h.hub.feeds[11] == "acct-z" })

	for i := range 100 {
		h.src.onTick(tick(101, 11, int64(1_000_000+i)))
	}
	u := alice.next(isUpdate).GetUpdates()
	if len(u.GetItems()) != 1 || u.GetItems()[0].GetQuote().GetTick().GetLtp() != 1_000_099 {
		t.Fatalf("conflated update = %v, want one item with the last ltp", u)
	}
	if m := alice.read(400 * time.Millisecond); m != nil && isUpdate(m) {
		t.Fatalf("no change, but another update was sent: %v", m)
	}

	alice.subscribe(wsv1.Channel_CHANNEL_QUOTE, 20, 101) // outside 50..1000
	if e := alice.next(func(m *wsv1.ServerMsg) bool { return m.GetError() != nil }).GetError(); e.GetCode() != "BAD_REQUEST" {
		t.Fatalf("error = %v", e)
	}
	keys := make([]uint64, 51)
	for i := range keys {
		keys[i] = uint64(1000 + i)
	}
	alice.subscribe(wsv1.Channel_CHANNEL_DEPTH, 0, keys...) // depth: 50 keys per connection
	if e := alice.next(func(m *wsv1.ServerMsg) bool { return m.GetError() != nil }).GetError(); !strings.Contains(e.GetMessage(), "limit 50") {
		t.Fatalf("error = %v", e)
	}
}

// API.md §8: a consumer that stops reading is held to a bounded outbound buffer, told SLOW_CONSUMER,
// and served snapshots only; fan-out to everyone else is not slowed down.
func TestSlowConsumer(t *testing.T) {
	cfg := fastConfig()
	cfg.MaxQueueBytes = 512 << 10
	cfg.SlowRecovery = 300 * time.Millisecond
	cfg.WriteTimeout = time.Minute
	dir := &fakeDir{
		accounts: map[string][]Account{
			"fast": {{ID: "acct-z", Broker: "zerodha", DataEntitled: true}},
			"slow": {{ID: "acct-z", Broker: "zerodha", DataEntitled: true}},
		},
		kiteAll: true,
	}
	engine := &fakeEngine{sessions: []*enginev1.SessionInfo{session("acct-z", "zerodha", 11, true)}}
	h := newHarness(t, cfg, dir, engine)
	waitFor(t, "feed map", func() bool { h.hub.mu.Lock(); defer h.hub.mu.Unlock(); return h.hub.feeds[11] == "acct-z" })

	ids := make([]uint64, 2000)
	for i := range ids {
		ids[i] = uint64(i + 1)
	}
	fast, slow := h.dial("fast:trader"), h.dialBuffered("slow:trader", 1)
	for _, c := range []*wsClient{fast, slow} {
		c.subscribe(wsv1.Channel_CHANNEL_QUOTE, 50, ids...)
		c.next(func(m *wsv1.ServerMsg) bool { return m.GetSnapshot() != nil })
	}
	var slowClient *client
	h.hub.mu.Lock()
	for c := range h.hub.clients {
		if c.userID == "slow" {
			slowClient = c
		}
	}
	h.hub.mu.Unlock()

	// The fast client reads continuously and records how stale each update is.
	var (
		mu        sync.Mutex
		updates   int
		worstLagM int64
	)
	stopFast := make(chan struct{})
	fastDone := make(chan struct{})
	go func() {
		defer close(fastDone)
		for {
			select {
			case <-stopFast:
				return
			default:
			}
			if m := fast.read(time.Second); m != nil && isUpdate(m) {
				lag := time.Now().UnixMilli() - m.GetUpdates().GetServerTsMs()
				mu.Lock()
				updates++
				worstLagM = max(worstLagM, lag)
				mu.Unlock()
			}
		}
	}()

	// Publish every instrument every 10 ms until the slow client overflows; it never reads.
	stopPub := make(chan struct{})
	pubDone := make(chan struct{})
	go func() {
		defer close(pubDone)
		for n := int64(0); ; n++ {
			select {
			case <-stopPub:
				return
			case <-time.After(10 * time.Millisecond):
			}
			for _, id := range ids {
				h.src.onTick(tick(id, 11, 1_000_000+n))
			}
		}
	}()
	deadline := time.Now().Add(30 * time.Second)
	var maxQueued int
	for h.hub.Stats().SlowConsumers == 0 {
		if time.Now().After(deadline) {
			t.Fatal("slow consumer never detected")
		}
		slowClient.mu.Lock()
		maxQueued = max(maxQueued, slowClient.queued)
		slowClient.mu.Unlock()
		time.Sleep(5 * time.Millisecond)
	}
	// Keep publishing a little while in snapshot-only mode; the buffer must stay bounded.
	for range 50 {
		slowClient.mu.Lock()
		maxQueued = max(maxQueued, slowClient.queued)
		slowClient.mu.Unlock()
		time.Sleep(10 * time.Millisecond)
	}
	if maxQueued > cfg.MaxQueueBytes {
		t.Fatalf("slow client's buffer reached %d bytes, limit %d", maxQueued, cfg.MaxQueueBytes)
	}
	mu.Lock()
	gotUpdates, lag := updates, worstLagM
	mu.Unlock()
	if gotUpdates < 10 || lag > 2000 {
		t.Fatalf("fast client starved while the slow one stalled: %d updates, worst lag %d ms", gotUpdates, lag)
	}

	// The slow client resumes reading: it is told SLOW_CONSUMER, then gets full snapshots, then
	// incremental updates again once it keeps up.
	sawError, sawSnapshot, sawUpdateAfter := false, false, false
	readDeadline := time.Now().Add(20 * time.Second)
	for !sawUpdateAfter && time.Now().Before(readDeadline) {
		m := slow.read(5 * time.Second)
		if m == nil {
			break
		}
		switch {
		case m.GetError().GetCode() == "SLOW_CONSUMER":
			sawError = true
		case sawError && m.GetSnapshot() != nil && m.GetSnapshot().GetChannel() == wsv1.Channel_CHANNEL_QUOTE:
			if len(m.GetSnapshot().GetItems()) != len(ids) {
				t.Fatalf("snapshot has %d items, want %d", len(m.GetSnapshot().GetItems()), len(ids))
			}
			sawSnapshot = true
		case sawSnapshot && isUpdate(m):
			sawUpdateAfter = true
		}
	}
	close(stopPub)
	<-pubDone
	close(stopFast)
	<-fastDone
	if !sawError || !sawSnapshot || !sawUpdateAfter {
		t.Fatalf("slow client saw error=%v snapshot=%v update-after-recovery=%v", sawError, sawSnapshot, sawUpdateAfter)
	}
	if h.hub.Stats().DroppedFrames == 0 {
		t.Fatal("expected dropped frames to be counted")
	}
}

func TestEngineInterestFollowsSubscriptionsAndFeedRestarts(t *testing.T) {
	dir := &fakeDir{accounts: map[string][]Account{"alice": {{ID: "acct-z", Broker: "zerodha", DataEntitled: true}}}, kiteAll: true}
	engine := &fakeEngine{sessions: []*enginev1.SessionInfo{session("acct-z", "zerodha", 11, true)}}
	h := newHarness(t, fastConfig(), dir, engine)
	alice := h.dial("alice:trader")

	alice.subscribe(wsv1.Channel_CHANNEL_QUOTE, 0, 101)
	alice.next(func(m *wsv1.ServerMsg) bool { return m.GetSnapshot() != nil })
	waitFor(t, "quote subscribe", func() bool { return len(engine.callsFor("acct-z")) == 1 })
	alice.subscribe(wsv1.Channel_CHANNEL_DEPTH, 0, 101)
	alice.next(func(m *wsv1.ServerMsg) bool { return m.GetSnapshot() != nil })
	waitFor(t, "upgrade to full", func() bool {
		c := engine.callsFor("acct-z")
		return len(c) == 2 && c[1].mode == marketdatav1.FeedMode_FEED_MODE_FULL
	})

	// The account's session restarts (daily login): new feed id, the engine lost the interest.
	engine.setFeed("acct-z", 12)
	waitFor(t, "resubscribe after restart", func() bool {
		c := engine.callsFor("acct-z")
		return len(c) == 3 && c[2].mode == marketdatav1.FeedMode_FEED_MODE_FULL && slices.Equal(c[2].ids, []uint64{101})
	})

	// Closing the connection releases everything.
	_ = alice.conn.Close(websocket.StatusNormalClosure, "")
	waitFor(t, "unsubscribe on close", func() bool {
		c := engine.callsFor("acct-z")
		return len(c) == 4 && c[3].remove
	})
}

func TestSystemChannelShowsOnlyVisibleAccounts(t *testing.T) {
	dir := &fakeDir{accounts: map[string][]Account{
		"alice": {{ID: "acct-z", Broker: "zerodha", DataEntitled: true, Label: "ZR1234"}},
		"root":  nil,
	}}
	engine := &fakeEngine{sessions: []*enginev1.SessionInfo{session("acct-z", "zerodha", 11, true), session("acct-x", "upstox", 33, true)}}
	h := newHarness(t, fastConfig(), dir, engine)
	waitFor(t, "first poll", func() bool { h.hub.mu.Lock(); defer h.hub.mu.Unlock(); return len(h.hub.sessions) == 2 })
	for user, want := range map[string]int{"alice:trader": 1, "root:admin": 2} {
		c := h.dial(user)
		c.subscribe(wsv1.Channel_CHANNEL_SYSTEM, 0)
		st := c.next(func(m *wsv1.ServerMsg) bool { return m.GetSnapshot() != nil }).GetSnapshot().GetItems()[0].GetSystem()
		if len(st.GetFeeds()) != want || !st.GetEngineOk() || st.GetMarketState() == "" {
			t.Fatalf("%s: status = %v, want %d feeds", user, st, want)
		}
		for _, f := range st.GetFeeds() {
			if wantLabel := map[string]string{"acct-z": "ZR1234"}[f.GetAccountId()]; user == "alice:trader" && f.GetLabel() != wantLabel {
				t.Fatalf("label = %q, want %q", f.GetLabel(), wantLabel)
			}
		}
		if u := c.next(func(m *wsv1.ServerMsg) bool { return m.GetUpdates() != nil }).GetUpdates(); u.GetChannel() != wsv1.Channel_CHANNEL_SYSTEM {
			t.Fatalf("%s: periodic update = %v", user, u)
		}
	}
}

func TestMarketState(t *testing.T) {
	cases := []struct {
		at   string // IST
		want string
	}{
		{"2026-10-29 08:59", "closed"},
		{"2026-10-29 09:00", "pre_open"},
		{"2026-10-29 09:15", "open"},
		{"2026-10-29 15:29", "open"},
		{"2026-10-29 15:30", "closed"},
		{"2026-10-31 11:00", "closed"}, // Saturday
	}
	for _, tc := range cases {
		at, _ := time.ParseInLocation("2006-01-02 15:04", tc.at, ist)
		h := New(DefaultConfig(), nil, &fakeDir{}, nil, nil, slog.New(slog.NewTextHandler(io.Discard, nil)), func() time.Time { return at })
		if got := h.computeMarketState(context.Background()); got != tc.want {
			t.Errorf("%s: %s, want %s", tc.at, got, tc.want)
		}
	}
}

// The gateway's http.Server has a ReadTimeout; a WebSocket must outlive it. net/http clears the
// deadlines on hijack (server.go hijackLocked); this guards against that changing.
func TestConnectionOutlivesServerReadTimeout(t *testing.T) {
	h := newHarness(t, fastConfig(), &fakeDir{}, &fakeEngine{})
	srv := httptest.NewUnstartedServer(h.hub)
	srv.Config.ReadTimeout, srv.Config.WriteTimeout = 300*time.Millisecond, 300*time.Millisecond
	srv.Start()
	defer srv.Close()
	h.url = "ws" + strings.TrimPrefix(srv.URL, "http")
	c := h.dial("alice:trader")
	time.Sleep(time.Second)
	c.write(&wsv1.ClientMsg{ReqId: 7, Body: &wsv1.ClientMsg_Ping{Ping: &wsv1.Ping{ClientTsMs: 1}}})
	if p := c.next(func(m *wsv1.ServerMsg) bool { return m.GetPong() != nil }); p.GetReqId() != 7 || p.GetPong().GetClientTsMs() != 1 {
		t.Fatalf("pong = %v", p)
	}
}

// ADR-020: bars carry no feed id, so they go only to users whose own entitled account is carrying
// the instrument on a connected feed — not to a user whose account hasn't logged in today.
func TestBarsOnlyThroughTheUsersCarryingAccount(t *testing.T) {
	dir := &fakeDir{accounts: map[string][]Account{
		"alice": {{ID: "acct-z", Broker: "zerodha", DataEntitled: true}},
		"dan":   {{ID: "acct-d", Broker: "zerodha", DataEntitled: true}}, // no session today
	}, kiteAll: true}
	engine := &fakeEngine{sessions: []*enginev1.SessionInfo{session("acct-z", "zerodha", 11, true)}}
	h := newHarness(t, fastConfig(), dir, engine)
	waitFor(t, "feed map", func() bool { h.hub.mu.Lock(); defer h.hub.mu.Unlock(); return h.hub.feeds[11] == "acct-z" })
	alice, dan := h.dial("alice:trader"), h.dial("dan:trader")
	for _, c := range []*wsClient{alice, dan} {
		c.subscribe(wsv1.Channel_CHANNEL_BAR_1M, 50, 101)
		c.next(func(m *wsv1.ServerMsg) bool { return m.GetSnapshot() != nil })
	}
	waitFor(t, "engine carries 101 on acct-z", func() bool {
		h.hub.mu.Lock()
		defer h.hub.mu.Unlock()
		ap := h.hub.applied["acct-z"]
		if ap == nil {
			return false
		}
		_, ok := ap.ids[101]
		return ok
	})
	h.src.onBar(&marketdatav1.Bar{InstrumentId: 101, Interval: "1m", TsNs: 1, Close: 5, Final: true})
	h.src.onBar(&marketdatav1.Bar{InstrumentId: 101, Interval: "1m", TsNs: 2, Close: 6})
	u := alice.next(func(m *wsv1.ServerMsg) bool { return m.GetUpdates().GetChannel() == wsv1.Channel_CHANNEL_BAR_1M }).GetUpdates()
	if len(u.GetItems()) != 2 || u.GetItems()[0].GetBar().GetClose() != 5 || u.GetItems()[1].GetBar().GetClose() != 6 {
		t.Fatalf("bars = %v (final bar must not be conflated away)", u)
	}
	if m := dan.read(300 * time.Millisecond); m != nil && m.GetUpdates() != nil {
		t.Fatalf("dan got bars carried only by alice's account: %v", m)
	}

	// alice's session restarts (new feed id): until the hub has re-sent her interest to the new
	// session, no feed of hers carries 101, so a bar published in that window is not hers to see.
	h.hub.mu.Lock()
	h.hub.sessions["acct-z"] = session("acct-z", "zerodha", 12, true) // seen by the hub, not yet reconciled
	h.hub.mu.Unlock()
	h.hub.onBar(&marketdatav1.Bar{InstrumentId: 101, Interval: "1m", TsNs: 3, Close: 7})
	if m := alice.read(300 * time.Millisecond); m != nil && m.GetUpdates().GetChannel() == wsv1.Channel_CHANNEL_BAR_1M {
		t.Fatalf("bar delivered through a feed that no longer carries the instrument: %v", m)
	}
}
