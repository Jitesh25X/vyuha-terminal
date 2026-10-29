// Package ws is the browser WebSocket hub (API.md §8): ClientHello auth, per-client conflation,
// market-data entitlement (ADR-009) and slow-consumer handling.
//
// Entitlement: a tick carries the feed id of the engine session that received it. The hub maps
// feed ids to accounts (engine GetSessions) and gives a tick only to users mapped (can_view) to that
// account when it is data-entitled. When a user subscribes, the hub registers interest in the engine
// through one of that user's own entitled accounts, so the instrument streams on a feed they may see.
//
// Locking: Hub.mu before client.mu, never the other way round. Engine, NATS and database calls are
// made without holding Hub.mu.
package ws

import (
	"context"
	"log/slog"
	"sync"
	"sync/atomic"
	"time"

	"github.com/coder/websocket"

	enginev1 "github.com/jiteshraj/vyuha/gateway/gen/vyuha/engine/v1"
	marketdatav1 "github.com/jiteshraj/vyuha/gateway/gen/vyuha/marketdata/v1"
	wsv1 "github.com/jiteshraj/vyuha/gateway/gen/vyuha/ws/v1"
	"github.com/jiteshraj/vyuha/gateway/internal/auth"
)

// Config tunes the hub; DefaultConfig follows API.md §8.
type Config struct {
	HelloTimeout       time.Duration
	MaxQueueBytes      int           // outbound buffer per connection before snapshot-only mode
	SlowRecovery       time.Duration // snapshot-only mode lasts at least this long
	WriteTimeout       time.Duration // a frame that can't be written in this time closes the connection
	SessionPoll        time.Duration // engine GetSessions (feed ids, status bar)
	EntitlementRefresh time.Duration // re-read a connected user's account mappings
	InstrumentTTL      time.Duration
	MaxQuoteKeys       int
	MaxDepthKeys       int
	MaxBarKeys         int
	ReadLimit          int64
	OriginPatterns     []string // extra allowed browser origins (same-origin is always allowed)
}

// DefaultConfig returns the API.md §8 limits.
func DefaultConfig() Config {
	return Config{
		HelloTimeout:       5 * time.Second,
		MaxQueueBytes:      4 << 20,
		SlowRecovery:       5 * time.Second,
		WriteTimeout:       15 * time.Second,
		SessionPoll:        time.Second,
		EntitlementRefresh: 30 * time.Second,
		InstrumentTTL:      time.Hour,
		MaxQuoteKeys:       2000,
		MaxDepthKeys:       50,
		MaxBarKeys:         2000,
		ReadLimit:          64 << 10,
	}
}

// Account is one broker account a user may view.
type Account struct {
	ID           string
	Broker       string // zerodha | upstox | sim
	DataEntitled bool
	Label        string // client code, shown in the status bar
}

// InstrumentMap says which brokers can stream an instrument.
type InstrumentMap struct{ Kite, Upstox bool }

// Directory reads the facts entitlement depends on (Postgres in production).
type Directory interface {
	UserAccounts(ctx context.Context, userID string) ([]Account, error)
	Instruments(ctx context.Context, ids []uint64) (map[uint64]InstrumentMap, error)
	IsHoliday(ctx context.Context, day time.Time) (bool, error)
}

// Engine is the part of the engine client the hub uses.
type Engine interface {
	GetSessions(ctx context.Context) ([]*enginev1.SessionInfo, error)
	Subscribe(ctx context.Context, ids []uint64, mode marketdatav1.FeedMode, viaAccount string) error
	Unsubscribe(ctx context.Context, ids []uint64, viaAccount string) error
}

// Source delivers engine market data (NATS in production).
type Source interface {
	// Run calls onTick/onBar for every md.tick / md.bar.1m message until ctx ends. Callbacks run on
	// one goroutine per kind and must not block.
	Run(ctx context.Context, onTick func(*marketdatav1.Tick), onBar func(*marketdatav1.Bar)) error
	// Last returns the md_last snapshots that exist for ids.
	Last(ctx context.Context, ids []uint64) (map[uint64]*marketdatav1.Snapshot, error)
}

// Verifier checks an access token (auth.Service.VerifyAccess).
type Verifier func(token string) (*auth.Claims, error)

// Stats are counters for tests and logs.
type Stats struct {
	Clients        int64
	SlowConsumers  int64 // times a connection dropped to snapshot-only mode
	DroppedFrames  int64 // frames discarded when a connection overflowed
	UnknownFeed    int64 // ticks from a feed id the hub hasn't seen in GetSessions yet (not delivered)
	EngineFailures int64
}

// lastKey is the latest tick for an instrument from one account's feed.
type lastKey struct {
	id      uint64
	account string
}

// wantCount is how many client subscriptions need an instrument through an account, per channel.
type wantCount struct{ quote, depth, bar int }

func (w wantCount) zero() bool { return w.quote == 0 && w.depth == 0 && w.bar == 0 }

func (w wantCount) mode() marketdatav1.FeedMode {
	if w.depth > 0 {
		return marketdatav1.FeedMode_FEED_MODE_FULL
	}
	return marketdatav1.FeedMode_FEED_MODE_QUOTE
}

// applied is what the engine has been told for one account's current feed.
type applied struct {
	feedID uint32
	ids    map[uint64]marketdatav1.FeedMode
}

// maxCachedInstruments: above this, expired cache entries are swept on the next fill (the active
// master is ~66k instruments).
const maxCachedInstruments = 100_000

type cachedInstrument struct {
	m       InstrumentMap
	known   bool
	fetched time.Time
}

// Hub fans engine market data out to browser connections.
type Hub struct {
	cfg    Config
	verify Verifier
	dir    Directory
	engine Engine
	src    Source
	logger *slog.Logger
	now    func() time.Time

	mu           sync.Mutex
	clients      map[*client]struct{}
	byInstrument map[uint64]map[*client]int // clients with any market-data subscription to the id
	last         map[lastKey]*marketdatav1.Tick
	feeds        map[uint32]string // feed id → account
	sessions     map[string]*enginev1.SessionInfo
	engineOK     bool
	want         map[string]map[uint64]wantCount // account → id → counts
	applied      map[string]*applied
	latencyMS    float64 // EWMA of engine receive → gateway receive
	marketState  string

	instMu sync.Mutex
	inst   map[uint64]cachedInstrument

	holidayMu  sync.Mutex
	holidayDay string
	holiday    bool

	kick chan struct{}

	slowConsumers, droppedFrames, unknownFeed, engineFailures atomic.Int64
}

// New builds a hub. Call Run to start the engine and market-data loops.
func New(cfg Config, verify Verifier, dir Directory, engine Engine, src Source, logger *slog.Logger, now func() time.Time) *Hub {
	return &Hub{
		cfg: cfg, verify: verify, dir: dir, engine: engine, src: src, logger: logger, now: now,
		clients:      map[*client]struct{}{},
		byInstrument: map[uint64]map[*client]int{},
		last:         map[lastKey]*marketdatav1.Tick{},
		feeds:        map[uint32]string{},
		sessions:     map[string]*enginev1.SessionInfo{},
		want:         map[string]map[uint64]wantCount{},
		applied:      map[string]*applied{},
		inst:         map[uint64]cachedInstrument{},
		marketState:  "closed",
		kick:         make(chan struct{}, 1),
	}
}

// Stats returns a snapshot of the counters.
func (h *Hub) Stats() Stats {
	h.mu.Lock()
	n := int64(len(h.clients))
	h.mu.Unlock()
	return Stats{
		Clients: n, SlowConsumers: h.slowConsumers.Load(), DroppedFrames: h.droppedFrames.Load(),
		UnknownFeed: h.unknownFeed.Load(), EngineFailures: h.engineFailures.Load(),
	}
}

// Run polls the engine, keeps engine interest in sync and consumes market data until ctx ends.
func (h *Hub) Run(ctx context.Context) {
	var wg sync.WaitGroup
	wg.Add(1)
	go func() {
		defer wg.Done()
		for ctx.Err() == nil {
			if err := h.src.Run(ctx, h.onTick, h.onBar); err != nil && ctx.Err() == nil {
				h.logger.Error("ws: market data source stopped; retrying", "error", err)
				select {
				case <-ctx.Done():
				case <-time.After(time.Second):
				}
			}
		}
	}()
	poll := time.NewTicker(h.cfg.SessionPoll)
	defer poll.Stop()
	h.poll(ctx)
	for {
		select {
		case <-ctx.Done():
			wg.Wait()
			h.closeAll()
			return
		case <-poll.C:
			h.poll(ctx)
			h.reconcile(ctx)
			h.pushSystem()
		case <-h.kick:
			h.reconcile(ctx)
		}
	}
}

// ObserveSession records a session the gateway just started (broker login), so its feed id is
// known before the next poll.
func (h *Hub) ObserveSession(info *enginev1.SessionInfo) {
	if info == nil {
		return
	}
	h.mu.Lock()
	h.sessions[info.GetAccountId()] = info
	if info.GetFeedId() != 0 {
		h.feeds[info.GetFeedId()] = info.GetAccountId()
	}
	h.mu.Unlock()
	h.kickReconcile()
}

// closeAll ends every connection on shutdown (http.Server.Shutdown leaves hijacked connections open).
func (h *Hub) closeAll() {
	h.mu.Lock()
	clients := make([]*client, 0, len(h.clients))
	for c := range h.clients {
		clients = append(clients, c)
	}
	h.mu.Unlock()
	for _, c := range clients {
		_ = c.conn.Close(websocket.StatusGoingAway, "server shutting down")
		c.shutdown()
	}
}

func (h *Hub) kickReconcile() {
	select {
	case h.kick <- struct{}{}:
	default:
	}
}

func (h *Hub) poll(ctx context.Context) {
	list, err := h.engine.GetSessions(ctx)
	state := h.computeMarketState(ctx)
	h.mu.Lock()
	defer h.mu.Unlock()
	h.marketState = state
	if err != nil {
		if h.engineOK {
			h.logger.Warn("ws: engine unavailable", "error", err)
		}
		h.engineOK = false
		return
	}
	h.engineOK = true
	sessions := make(map[string]*enginev1.SessionInfo, len(list))
	feeds := make(map[uint32]string, len(list))
	for _, s := range list {
		sessions[s.GetAccountId()] = s
		if s.GetFeedId() != 0 {
			feeds[s.GetFeedId()] = s.GetAccountId()
		}
	}
	h.sessions, h.feeds = sessions, feeds
}

// connectedFeed returns the account's feed id when its data session is connected (caller holds mu).
func (h *Hub) connectedFeed(account string) (uint32, bool) {
	s, ok := h.sessions[account]
	if !ok || s.GetState() != "connected" || s.GetFeedId() == 0 || len(s.GetFeeds()) == 0 {
		return 0, false
	}
	return s.GetFeedId(), true
}

// ── market data in ───────────────────────────────────────────────────────────

func (h *Hub) onTick(t *marketdatav1.Tick) {
	h.mu.Lock()
	defer h.mu.Unlock()
	account, ok := h.feeds[t.GetFeedId()]
	if !ok {
		h.unknownFeed.Add(1)
		return
	}
	if recv := t.GetRecvTsNs(); recv > 0 {
		lat := float64(h.now().UnixNano()-recv) / 1e6
		h.latencyMS = 0.9*h.latencyMS + 0.1*max(lat, 0)
	}
	subs := h.byInstrument[t.GetInstrumentId()]
	if len(subs) == 0 {
		return
	}
	h.last[lastKey{t.GetInstrumentId(), account}] = t
	for c := range subs {
		c.offerTick(t, account)
	}
}

// onBar: bars carry no feed id (the engine builds them per instrument), so a bar goes only to users
// with an entitled account whose connected feed the engine has been told to carry the instrument on.
func (h *Hub) onBar(b *marketdatav1.Bar) {
	if b.GetInterval() != "1m" {
		return
	}
	h.mu.Lock()
	defer h.mu.Unlock()
	id := b.GetInstrumentId()
	subs := h.byInstrument[id]
	if len(subs) == 0 {
		return
	}
	carriers := map[string]bool{}
	for a, ap := range h.applied {
		if _, carried := ap.ids[id]; carried {
			if feed, ok := h.connectedFeed(a); ok && feed == ap.feedID {
				carriers[a] = true
			}
		}
	}
	if len(carriers) == 0 {
		return
	}
	for c := range subs {
		c.offerBar(b, carriers)
	}
}

// ── interest and engine reconciliation ───────────────────────────────────────

func countFor(ch wsv1.Channel, w *wantCount) *int {
	switch ch {
	case wsv1.Channel_CHANNEL_DEPTH:
		return &w.depth
	case wsv1.Channel_CHANNEL_BAR_1M:
		return &w.bar
	}
	return &w.quote
}

// chooseVia picks the account to carry id for a subscriber: one already carrying it, else one whose
// feed is connected, else the first candidate.
func (h *Hub) chooseVia(id uint64, candidates []string) string {
	h.mu.Lock()
	defer h.mu.Unlock()
	for _, a := range candidates {
		if _, ok := h.want[a][id]; ok {
			return a
		}
	}
	for _, a := range candidates {
		if _, ok := h.connectedFeed(a); ok {
			return a
		}
	}
	return candidates[0]
}

func (h *Hub) addInterest(c *client, ch wsv1.Channel, ids map[uint64]string) {
	h.mu.Lock()
	for id, via := range ids {
		if h.byInstrument[id] == nil {
			h.byInstrument[id] = map[*client]int{}
		}
		h.byInstrument[id][c]++
		if h.want[via] == nil {
			h.want[via] = map[uint64]wantCount{}
		}
		w := h.want[via][id]
		*countFor(ch, &w)++
		h.want[via][id] = w
	}
	h.mu.Unlock()
	h.kickReconcile()
}

func (h *Hub) removeInterest(c *client, ch wsv1.Channel, ids map[uint64]string) {
	h.mu.Lock()
	for id, via := range ids {
		if n := h.byInstrument[id][c] - 1; n > 0 {
			h.byInstrument[id][c] = n
		} else {
			delete(h.byInstrument[id], c)
			if len(h.byInstrument[id]) == 0 {
				delete(h.byInstrument, id)
				for k := range h.last {
					if k.id == id {
						delete(h.last, k)
					}
				}
			}
		}
		w := h.want[via][id]
		if p := countFor(ch, &w); *p > 0 {
			*p--
		}
		if w.zero() {
			delete(h.want[via], id)
			if len(h.want[via]) == 0 {
				delete(h.want, via)
			}
		} else {
			h.want[via][id] = w
		}
	}
	h.mu.Unlock()
	h.kickReconcile()
}

type change struct {
	account string
	feedID  uint32
	mode    marketdatav1.FeedMode
	ids     []uint64
	remove  bool
}

// reconcile brings the engine's per-account interest in line with what clients want. A new feed id
// means the engine session restarted (daily login) and lost its interest, so it is re-sent.
func (h *Hub) reconcile(ctx context.Context) {
	h.mu.Lock()
	var plan []change
	accounts := map[string]struct{}{}
	for a := range h.want {
		accounts[a] = struct{}{}
	}
	for a := range h.applied {
		accounts[a] = struct{}{}
	}
	for a := range accounts {
		feed, ok := h.connectedFeed(a)
		if !ok {
			delete(h.applied, a) // re-sent in full once the feed is back
			continue
		}
		ap := h.applied[a]
		if ap == nil || ap.feedID != feed {
			ap = &applied{feedID: feed, ids: map[uint64]marketdatav1.FeedMode{}}
			h.applied[a] = ap
		}
		byMode := map[marketdatav1.FeedMode][]uint64{}
		for id, w := range h.want[a] {
			if m := w.mode(); ap.ids[id] != m {
				byMode[m] = append(byMode[m], id)
			}
		}
		for m, ids := range byMode {
			plan = append(plan, change{account: a, feedID: feed, mode: m, ids: ids})
		}
		var gone []uint64
		for id := range ap.ids {
			if _, ok := h.want[a][id]; !ok {
				gone = append(gone, id)
			}
		}
		if len(gone) > 0 {
			plan = append(plan, change{account: a, feedID: feed, ids: gone, remove: true})
		}
	}
	h.mu.Unlock()

	for _, ch := range plan {
		var err error
		if ch.remove {
			err = h.engine.Unsubscribe(ctx, ch.ids, ch.account)
		} else {
			err = h.engine.Subscribe(ctx, ch.ids, ch.mode, ch.account)
		}
		if err != nil {
			h.engineFailures.Add(1)
			h.logger.Warn("ws: engine subscription change failed; will retry", "account", ch.account,
				"instruments", len(ch.ids), "remove", ch.remove, "error", err)
			continue
		}
		h.mu.Lock()
		if ap := h.applied[ch.account]; ap != nil && ap.feedID == ch.feedID {
			for _, id := range ch.ids {
				if ch.remove {
					delete(ap.ids, id)
				} else {
					ap.ids[id] = ch.mode
				}
			}
		}
		h.mu.Unlock()
	}
}

// ── instruments ──────────────────────────────────────────────────────────────

func (h *Hub) instruments(ctx context.Context, ids []uint64) (map[uint64]InstrumentMap, error) {
	now := h.now()
	out := make(map[uint64]InstrumentMap, len(ids))
	var missing []uint64
	h.instMu.Lock()
	for _, id := range ids {
		if c, ok := h.inst[id]; ok && now.Sub(c.fetched) < h.cfg.InstrumentTTL {
			if c.known {
				out[id] = c.m
			}
			continue
		}
		missing = append(missing, id)
	}
	h.instMu.Unlock()
	if len(missing) == 0 {
		return out, nil
	}
	got, err := h.dir.Instruments(ctx, missing)
	if err != nil {
		return nil, err
	}
	h.instMu.Lock()
	if len(h.inst) > maxCachedInstruments {
		for id, c := range h.inst {
			if now.Sub(c.fetched) >= h.cfg.InstrumentTTL {
				delete(h.inst, id)
			}
		}
	}
	for _, id := range missing {
		if m, ok := got[id]; ok { // unknown ids are not cached: they'd let a client grow the map at will
			h.inst[id] = cachedInstrument{m: m, known: true, fetched: now}
			out[id] = m
		}
	}
	h.instMu.Unlock()
	return out, nil
}

// ── system channel (status bar) ──────────────────────────────────────────────

func (h *Hub) pushSystem() {
	h.mu.Lock()
	clients := make([]*client, 0, len(h.clients))
	for c := range h.clients {
		clients = append(clients, c)
	}
	h.mu.Unlock()
	for _, c := range clients {
		c.pushSystem()
	}
}

// systemStatus builds the status bar view for a user: feeds of the accounts they can see.
func (h *Hub) systemStatus(seeAll bool, visible map[string]string) *wsv1.SystemStatus {
	h.mu.Lock()
	defer h.mu.Unlock()
	st := &wsv1.SystemStatus{MarketState: h.marketState, EngineOk: h.engineOK, DataLatencyMs: int64(h.latencyMS + 0.5)}
	for id, s := range h.sessions {
		label, mapped := visible[id]
		if !seeAll && !mapped {
			continue
		}
		for _, f := range s.GetFeeds() {
			fs := &marketdatav1.FeedStatus{
				AccountId: f.GetAccountId(), Broker: f.GetBroker(), State: f.GetState(),
				Subscribed: f.GetSubscribed(), LastTickNs: f.GetLastTickNs(), Detail: f.GetDetail(), Label: label,
			}
			if !h.engineOK {
				fs.State = marketdatav1.FeedStatus_STATE_DOWN
			}
			st.Feeds = append(st.Feeds, fs)
		}
	}
	return st
}

// computeMarketState: NSE session in IST — pre_open 09:00–09:15, open 09:15–15:30, else closed;
// weekends closed; exchange holidays (exchange_holiday) "holiday".
func (h *Hub) computeMarketState(ctx context.Context) string {
	t := h.now().In(ist)
	if wd := t.Weekday(); wd == time.Saturday || wd == time.Sunday {
		return "closed"
	}
	day := t.Format(time.DateOnly)
	h.holidayMu.Lock()
	cached := h.holidayDay == day
	holiday := h.holiday
	h.holidayMu.Unlock()
	if !cached {
		hctx, cancel := context.WithTimeout(ctx, time.Second)
		v, err := h.dir.IsHoliday(hctx, t)
		cancel()
		if err == nil {
			holiday = v
			h.holidayMu.Lock()
			h.holidayDay, h.holiday = day, v
			h.holidayMu.Unlock()
		}
	}
	if holiday {
		return "holiday"
	}
	mins := t.Hour()*60 + t.Minute()
	switch {
	case mins >= 9*60 && mins < 9*60+15:
		return "pre_open"
	case mins >= 9*60+15 && mins < 15*60+30:
		return "open"
	}
	return "closed"
}

var ist = time.FixedZone("IST", 5*3600+30*60)

// EntitledVia lists the accounts through which a user may receive an instrument's market data
// (ADR-009): mapped, data-entitled, non-sim accounts whose broker can stream it. REST endpoints
// (candles) use the same rule as the hub.
func EntitledVia(accounts []Account, m InstrumentMap) []string {
	var out []string
	for _, a := range accounts {
		if !a.DataEntitled || a.Broker == "sim" {
			continue
		}
		if (a.Broker == "zerodha" && m.Kite) || (a.Broker == "upstox" && m.Upstox) {
			out = append(out, a.ID)
		}
	}
	return out
}
