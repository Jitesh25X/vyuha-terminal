package ws

import (
	"context"
	"net/http"
	"slices"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/coder/websocket"
	"google.golang.org/protobuf/proto"

	marketdatav1 "github.com/jiteshraj/vyuha/gateway/gen/vyuha/marketdata/v1"
	wsv1 "github.com/jiteshraj/vyuha/gateway/gen/vyuha/ws/v1"
	"github.com/jiteshraj/vyuha/gateway/internal/rbac"
)

// Close codes and error codes (API.md §8, ws.proto Error.code).
const (
	closeUnauthenticated websocket.StatusCode = 4401

	codeNotEntitled  = "MD_NOT_ENTITLED"
	codeSlowConsumer = "SLOW_CONSUMER"
	codeAuthExpired  = "AUTH_EXPIRED"
	codeBadRequest   = "BAD_REQUEST"
	codeUnavailable  = "UNAVAILABLE"

	// NotEntitledMessage is the PRD F-MD-2 text the panel shows.
	NotEntitledMessage = "No market-data entitlement — ask admin to map a data-enabled account"

	defaultConflate = 250 * time.Millisecond
	minConflate     = 50
	maxConflate     = 1000
)

// ServeHTTP upgrades /ws and runs the connection until it closes.
func (h *Hub) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	conn, err := websocket.Accept(w, r, &websocket.AcceptOptions{OriginPatterns: h.cfg.OriginPatterns})
	if err != nil {
		return // Accept already wrote the HTTP error
	}
	conn.SetReadLimit(h.cfg.ReadLimit)
	ctx, cancel := context.WithCancel(context.Background()) // outlives the HTTP handler's request context
	defer cancel()

	c, ok := h.hello(ctx, conn)
	if !ok {
		return
	}
	h.mu.Lock()
	h.clients[c] = struct{}{}
	h.mu.Unlock()
	defer h.drop(c)

	go c.writeLoop()
	go c.housekeeping()
	c.send(&wsv1.ServerMsg{Body: &wsv1.ServerMsg_Welcome{Welcome: &wsv1.Welcome{
		UserId: c.userID, Role: c.role, ServerTsMs: h.now().UnixMilli(),
	}}})
	c.readLoop()
}

// hello waits for ClientHello with a valid access token, or closes with 4401.
func (h *Hub) hello(ctx context.Context, conn *websocket.Conn) (*client, bool) {
	timer := time.AfterFunc(h.cfg.HelloTimeout, func() {
		_ = conn.Close(closeUnauthenticated, "hello timeout")
	})
	typ, data, err := conn.Read(ctx)
	if !timer.Stop() {
		return nil, false
	}
	reject := func() (*client, bool) {
		_ = conn.Close(closeUnauthenticated, "unauthenticated")
		return nil, false
	}
	if err != nil {
		return nil, false
	}
	var msg wsv1.ClientMsg
	if typ != websocket.MessageBinary || proto.Unmarshal(data, &msg) != nil || msg.GetHello() == nil {
		return reject()
	}
	claims, err := h.verify(msg.GetHello().GetAccessToken())
	if err != nil || claims.Subject == "" || claims.ExpiresAt == nil {
		return reject()
	}
	actx, cancel := context.WithTimeout(ctx, time.Second)
	accounts, err := h.dir.UserAccounts(actx, claims.Subject)
	cancel()
	if err != nil {
		h.logger.Error("ws: load user accounts", "user", claims.Subject, "error", err)
		_ = conn.Close(websocket.StatusInternalError, "try again")
		return nil, false
	}
	c := &client{
		hub: h, conn: conn, ctx: ctx, userID: claims.Subject, role: claims.Role, expires: claims.ExpiresAt.Time,
		wake: make(chan struct{}, 1), done: make(chan struct{}),
		groups: map[groupKey]*group{}, member: map[wsv1.Channel]map[uint64]*group{}, via: map[subKey]string{},
	}
	c.setAccounts(accounts)
	return c, true
}

// drop unregisters a closed connection and releases its engine interest.
func (h *Hub) drop(c *client) {
	c.shutdown()
	h.mu.Lock()
	delete(h.clients, c)
	h.mu.Unlock()
	c.mu.Lock()
	byChannel := map[wsv1.Channel]map[uint64]string{}
	for k, via := range c.via {
		if byChannel[k.ch] == nil {
			byChannel[k.ch] = map[uint64]string{}
		}
		byChannel[k.ch][k.id] = via
	}
	c.via = map[subKey]string{}
	c.mu.Unlock()
	for ch, ids := range byChannel {
		h.removeInterest(c, ch, ids)
	}
}

type groupKey struct {
	ch       wsv1.Channel
	interval time.Duration
}

type subKey struct {
	ch wsv1.Channel
	id uint64
}

// group is one conflation group: keys of one channel flushed on one ticker.
type group struct {
	key      groupKey
	keys     map[uint64]struct{}
	latest   map[uint64]*marketdatav1.Tick
	dirty    map[uint64]struct{}
	bars     map[uint64][]*marketdatav1.Bar // pending bars (forming bar replaced, finals kept)
	lastBar  map[uint64]*marketdatav1.Bar
	stopOnce sync.Once
	stop     chan struct{}
}

type client struct {
	hub    *Hub
	conn   *websocket.Conn
	ctx    context.Context
	userID string

	wake chan struct{}
	done chan struct{}
	once sync.Once

	mu         sync.Mutex
	role       string
	expires    time.Time
	data       map[string]bool   // data-entitled accounts (ADR-009)
	visible    map[string]string // accounts whose status the user may see → label
	candidates map[string][]string
	groups     map[groupKey]*group
	member     map[wsv1.Channel]map[uint64]*group
	via        map[subKey]string // the account each subscription was registered through
	system     bool
	queue      [][]byte
	queued     int
	slow       bool
	slowSince  time.Time
}

func itemID(it *wsv1.Item) uint64 {
	if q := it.GetQuote(); q != nil {
		return q.GetTick().GetInstrumentId()
	}
	return it.GetDepth().GetInstrumentId()
}

func (c *client) setAccounts(accounts []Account) {
	data, visible := map[string]bool{}, map[string]string{}
	byBroker := map[string][]string{}
	for _, a := range accounts {
		visible[a.ID] = a.Label
		if rbac.CanReceiveMarketData(&rbac.Mapping{CanView: true}, a.DataEntitled) && a.Broker != "sim" {
			data[a.ID] = true
			byBroker[a.Broker] = append(byBroker[a.Broker], a.ID)
		}
	}
	c.mu.Lock()
	c.data, c.visible, c.candidates = data, visible, byBroker
	c.mu.Unlock()
}

// candidatesFor lists the user's entitled accounts whose broker can stream the instrument.
func (c *client) candidatesFor(m InstrumentMap) []string {
	var out []string
	if m.Kite {
		out = append(out, c.candidates["zerodha"]...)
	}
	if m.Upstox {
		out = append(out, c.candidates["upstox"]...)
	}
	return out
}

// ── reading ──────────────────────────────────────────────────────────────────

func (c *client) readLoop() {
	for {
		typ, data, err := c.conn.Read(c.ctx)
		if err != nil {
			return
		}
		var msg wsv1.ClientMsg
		if typ != websocket.MessageBinary || proto.Unmarshal(data, &msg) != nil {
			c.sendError(0, codeBadRequest, "binary vyuha.ws.v1.ClientMsg frames only")
			continue
		}
		switch b := msg.GetBody().(type) {
		case *wsv1.ClientMsg_Subscribe:
			c.subscribe(msg.GetReqId(), b.Subscribe)
		case *wsv1.ClientMsg_Unsubscribe:
			c.unsubscribe(b.Unsubscribe)
		case *wsv1.ClientMsg_Ping:
			c.send(&wsv1.ServerMsg{ReqId: msg.GetReqId(), Body: &wsv1.ServerMsg_Pong{Pong: &wsv1.Pong{
				ClientTsMs: b.Ping.GetClientTsMs(), ServerTsMs: c.hub.now().UnixMilli(),
			}}})
		case *wsv1.ClientMsg_TokenRefresh:
			c.tokenRefresh(msg.GetReqId(), b.TokenRefresh.GetAccessToken())
		default:
			c.sendError(msg.GetReqId(), codeBadRequest, "unexpected message")
		}
	}
}

func (c *client) tokenRefresh(reqID uint32, token string) {
	claims, err := c.hub.verify(token)
	if err != nil || claims.Subject != c.userID || claims.ExpiresAt == nil {
		c.sendError(reqID, codeBadRequest, "token refresh rejected")
		return
	}
	c.mu.Lock()
	c.expires, c.role = claims.ExpiresAt.Time, claims.Role
	c.mu.Unlock()
	c.refreshEntitlements()
}

func (c *client) subscribe(reqID uint32, s *wsv1.Subscribe) {
	switch s.GetChannel() {
	case wsv1.Channel_CHANNEL_QUOTE, wsv1.Channel_CHANNEL_DEPTH, wsv1.Channel_CHANNEL_BAR_1M:
		c.subscribeMD(reqID, s)
	case wsv1.Channel_CHANNEL_SYSTEM:
		c.mu.Lock()
		c.system = true
		seeAll, visible := rbac.SeesAllAccounts(c.role), c.visible
		c.mu.Unlock()
		c.send(&wsv1.ServerMsg{ReqId: reqID, Body: &wsv1.ServerMsg_Snapshot{Snapshot: &wsv1.Snapshot{
			Channel: wsv1.Channel_CHANNEL_SYSTEM,
			Items:   []*wsv1.Item{{Value: &wsv1.Item_System{System: c.hub.systemStatus(seeAll, visible)}}},
		}}})
	default:
		c.sendError(reqID, codeBadRequest, "channel not available yet")
	}
}

func (c *client) subscribeMD(reqID uint32, s *wsv1.Subscribe) {
	ch := s.GetChannel()
	interval := defaultConflate
	if ms := s.GetConflateMs(); ms != 0 {
		if ms < minConflate || ms > maxConflate {
			c.sendError(reqID, codeBadRequest, "conflate_ms must be 50..1000")
			return
		}
		interval = time.Duration(ms) * time.Millisecond
	}
	ids, ok := parseKeys(s.GetKeys())
	if !ok || len(ids) == 0 {
		c.sendError(reqID, codeBadRequest, "keys must be instrument ids")
		return
	}

	c.mu.Lock()
	var fresh []uint64
	for _, id := range ids {
		if _, have := c.via[subKey{ch, id}]; !have {
			fresh = append(fresh, id)
		}
	}
	if limit := c.hub.limit(ch); len(c.member[ch])+len(fresh) > limit {
		c.mu.Unlock()
		c.sendError(reqID, codeBadRequest, "too many keys on this channel (limit "+strconv.Itoa(limit)+")")
		return
	}
	c.mu.Unlock()

	ictx, cancel := context.WithTimeout(c.ctx, time.Second)
	maps, err := c.hub.instruments(ictx, fresh)
	cancel()
	if err != nil {
		c.sendError(reqID, codeUnavailable, "instrument lookup failed; retry")
		return
	}
	candidates := map[uint64][]string{}
	var denied []uint64
	c.mu.Lock()
	for _, id := range fresh {
		if cands := c.candidatesFor(maps[id]); len(cands) > 0 {
			candidates[id] = cands
		} else {
			denied = append(denied, id)
		}
	}
	c.mu.Unlock()
	granted := make(map[uint64]string, len(candidates))
	for id, cands := range candidates { // chooseVia takes Hub.mu, so not under c.mu
		granted[id] = c.hub.chooseVia(id, cands)
	}
	c.hub.addInterest(c, ch, granted)

	snap := c.snapshotItems(ch, ids, denied)
	c.mu.Lock()
	lost := map[uint64]string{}
	for id, via := range granted {
		if !c.data[via] { // unmapped by a refresh that ran while this subscribe was in flight
			lost[id] = via
			denied = append(denied, id)
			delete(snap.ticks, id)
			continue
		}
		c.via[subKey{ch, id}] = via
	}
	var place []uint64
	for _, id := range ids {
		if _, ok := c.via[subKey{ch, id}]; ok {
			place = append(place, id)
		}
	}
	if len(place) > 0 {
		g := c.groupLocked(groupKey{ch, interval})
		for _, id := range place {
			if old := c.member[ch][id]; old != nil && old != g {
				c.leaveGroupLocked(old, id)
			}
			g.keys[id] = struct{}{}
			c.member[ch][id] = g
			if t := snap.ticks[id]; t != nil && g.latest[id] == nil {
				g.latest[id] = t
			}
		}
	}
	c.mu.Unlock()
	if len(lost) > 0 {
		c.hub.removeInterest(c, ch, lost)
	}
	items := snap.items[:0:0]
	for _, it := range snap.items {
		if _, gone := lost[itemID(it)]; !gone {
			items = append(items, it)
		}
	}
	snap.items = items

	c.send(&wsv1.ServerMsg{ReqId: reqID, Body: &wsv1.ServerMsg_Snapshot{Snapshot: &wsv1.Snapshot{Channel: ch, Items: snap.items}}})
	if len(denied) > 0 {
		c.sendError(reqID, codeNotEntitled, NotEntitledMessage+" ["+joinIDs(denied)+"]")
	}
}

type snapshot struct {
	items []*wsv1.Item
	ticks map[uint64]*marketdatav1.Tick
}

// snapshotItems gathers the latest value of each id from a feed this user may see: the hub's cache
// first, then md_last (only when its tick came from one of the user's feeds).
func (c *client) snapshotItems(ch wsv1.Channel, ids, denied []uint64) snapshot {
	out := snapshot{ticks: map[uint64]*marketdatav1.Tick{}}
	if ch == wsv1.Channel_CHANNEL_BAR_1M {
		return out // bars arrive with the next update
	}
	c.mu.Lock()
	data := c.data
	c.mu.Unlock()
	h := c.hub
	var missing []uint64
	h.mu.Lock()
	for _, id := range ids {
		if slices.Contains(denied, id) {
			continue
		}
		var best *marketdatav1.Tick
		for a := range data {
			if t := h.last[lastKey{id, a}]; t != nil && (best == nil || t.GetRecvTsNs() > best.GetRecvTsNs()) {
				best = t
			}
		}
		if best != nil {
			out.ticks[id] = best
		} else {
			missing = append(missing, id)
		}
	}
	h.mu.Unlock()
	if len(missing) > 0 {
		lctx, cancel := context.WithTimeout(c.ctx, time.Second)
		last, err := h.src.Last(lctx, missing)
		cancel()
		if err == nil {
			h.mu.Lock()
			for id, s := range last {
				if t := s.GetTick(); t != nil && data[h.feeds[t.GetFeedId()]] {
					out.ticks[id] = t
				}
			}
			h.mu.Unlock()
		}
	}
	for _, id := range ids {
		if t := out.ticks[id]; t != nil {
			out.items = append(out.items, tickItem(ch, t))
		}
	}
	return out
}

func (c *client) unsubscribe(u *wsv1.Unsubscribe) {
	ch := u.GetChannel()
	if ch == wsv1.Channel_CHANNEL_SYSTEM {
		c.mu.Lock()
		c.system = false
		c.mu.Unlock()
		return
	}
	ids, _ := parseKeys(u.GetKeys())
	c.hub.removeInterest(c, ch, c.forget(ch, ids))
}

// forget removes subscriptions and returns them (id → via account) for removeInterest.
func (c *client) forget(ch wsv1.Channel, ids []uint64) map[uint64]string {
	gone := map[uint64]string{}
	c.mu.Lock()
	defer c.mu.Unlock()
	for _, id := range ids {
		via, ok := c.via[subKey{ch, id}]
		if !ok {
			continue
		}
		gone[id] = via
		delete(c.via, subKey{ch, id})
		if g := c.member[ch][id]; g != nil {
			c.leaveGroupLocked(g, id)
		}
		delete(c.member[ch], id)
	}
	return gone
}

// refreshEntitlements re-reads the user's accounts and drops subscriptions they may no longer see.
func (c *client) refreshEntitlements() {
	ctx, cancel := context.WithTimeout(c.ctx, time.Second)
	accounts, err := c.hub.dir.UserAccounts(ctx, c.userID)
	cancel()
	if err != nil {
		return // keep the current view; retried on the next refresh
	}
	c.mu.Lock()
	before := c.data
	c.mu.Unlock()
	c.setAccounts(accounts)
	c.mu.Lock()
	for a := range before {
		if !c.data[a] { // latest values may have come from that account's feed: forget them all
			for _, g := range c.groups {
				clear(g.latest)
				clear(g.lastBar)
			}
			break
		}
	}
	lost := map[wsv1.Channel][]uint64{}
	for k, via := range c.via {
		if !c.data[via] {
			lost[k.ch] = append(lost[k.ch], k.id)
		}
	}
	c.mu.Unlock()
	for ch, ids := range lost {
		c.hub.removeInterest(c, ch, c.forget(ch, ids))
		c.sendError(0, codeNotEntitled, NotEntitledMessage+" ["+joinIDs(ids)+"]")
	}
}

// ── market data in (called with Hub.mu held) ─────────────────────────────────

func (c *client) offerTick(t *marketdatav1.Tick, account string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if !c.data[account] {
		return // ADR-009: this feed belongs to an account the user isn't entitled through
	}
	id := t.GetInstrumentId()
	for _, ch := range [...]wsv1.Channel{wsv1.Channel_CHANNEL_QUOTE, wsv1.Channel_CHANNEL_DEPTH} {
		if g := c.member[ch][id]; g != nil {
			g.latest[id] = t
			g.dirty[id] = struct{}{}
		}
	}
}

func (c *client) offerBar(b *marketdatav1.Bar, carriers map[string]bool) {
	c.mu.Lock()
	defer c.mu.Unlock()
	entitled := false
	for a := range carriers {
		if c.data[a] {
			entitled = true
			break
		}
	}
	if !entitled {
		return // ADR-020: only bars of instruments carried through one of the user's own accounts
	}
	id := b.GetInstrumentId()
	g := c.member[wsv1.Channel_CHANNEL_BAR_1M][id]
	if g == nil {
		return
	}
	pending := g.bars[id]
	if n := len(pending); n > 0 && pending[n-1].GetTsNs() == b.GetTsNs() {
		pending[n-1] = b
	} else {
		pending = append(pending, b)
		if len(pending) > 4 {
			pending = pending[len(pending)-4:]
		}
	}
	g.bars[id] = pending
	g.lastBar[id] = b
}

// ── conflation ───────────────────────────────────────────────────────────────

func (c *client) groupLocked(k groupKey) *group {
	if c.member[k.ch] == nil {
		c.member[k.ch] = map[uint64]*group{}
	}
	if g := c.groups[k]; g != nil {
		return g
	}
	g := &group{
		key: k, keys: map[uint64]struct{}{}, latest: map[uint64]*marketdatav1.Tick{}, dirty: map[uint64]struct{}{},
		bars: map[uint64][]*marketdatav1.Bar{}, lastBar: map[uint64]*marketdatav1.Bar{}, stop: make(chan struct{}),
	}
	c.groups[k] = g
	go c.flushLoop(g)
	return g
}

func (c *client) leaveGroupLocked(g *group, id uint64) {
	delete(g.keys, id)
	delete(g.latest, id)
	delete(g.dirty, id)
	delete(g.bars, id)
	delete(g.lastBar, id)
	if len(g.keys) == 0 {
		delete(c.groups, g.key)
		g.stopOnce.Do(func() { close(g.stop) })
	}
}

func (c *client) flushLoop(g *group) {
	t := time.NewTicker(g.key.interval)
	defer t.Stop()
	for {
		select {
		case <-c.done:
			return
		case <-g.stop:
			return
		case <-t.C:
			c.flush(g)
		}
	}
}

// flush sends the group's changes as one UpdateBatch. In snapshot-only mode it instead sends the
// latest value of every key, and only once the outbound queue has drained.
func (c *client) flush(g *group) {
	now := c.hub.now()
	c.mu.Lock()
	var (
		items    []*wsv1.Item
		snapshot = c.slow
	)
	if snapshot {
		if len(c.queue) > 0 || (len(g.dirty) == 0 && len(g.bars) == 0) {
			c.mu.Unlock()
			return
		}
		for id := range g.keys {
			if t := g.latest[id]; t != nil {
				items = append(items, tickItem(g.key.ch, t))
			} else if b := g.lastBar[id]; b != nil {
				items = append(items, &wsv1.Item{Value: &wsv1.Item_Bar{Bar: b}})
			}
		}
		if now.Sub(c.slowSince) >= c.hub.cfg.SlowRecovery {
			c.slow = false
		}
	} else {
		for id := range g.dirty {
			if t := g.latest[id]; t != nil {
				items = append(items, tickItem(g.key.ch, t))
			}
		}
		for _, bars := range g.bars {
			for _, b := range bars {
				items = append(items, &wsv1.Item{Value: &wsv1.Item_Bar{Bar: b}})
			}
		}
	}
	clear(g.dirty)
	clear(g.bars)
	c.mu.Unlock()
	if len(items) == 0 {
		return
	}
	if snapshot {
		c.send(&wsv1.ServerMsg{Body: &wsv1.ServerMsg_Snapshot{Snapshot: &wsv1.Snapshot{Channel: g.key.ch, Items: items}}})
		return
	}
	c.send(&wsv1.ServerMsg{Body: &wsv1.ServerMsg_Updates{Updates: &wsv1.UpdateBatch{
		Channel: g.key.ch, Items: items, ServerTsMs: now.UnixMilli(),
	}}})
}

func (c *client) pushSystem() {
	c.mu.Lock()
	on, seeAll, visible := c.system, rbac.SeesAllAccounts(c.role), c.visible
	c.mu.Unlock()
	if !on {
		return
	}
	c.send(&wsv1.ServerMsg{Body: &wsv1.ServerMsg_Updates{Updates: &wsv1.UpdateBatch{
		Channel:    wsv1.Channel_CHANNEL_SYSTEM,
		Items:      []*wsv1.Item{{Value: &wsv1.Item_System{System: c.hub.systemStatus(seeAll, visible)}}},
		ServerTsMs: c.hub.now().UnixMilli(),
	}}})
}

// tickItem: quote items carry the snapshot without depth; depth items carry the full tick.
func tickItem(ch wsv1.Channel, t *marketdatav1.Tick) *wsv1.Item {
	if ch == wsv1.Channel_CHANNEL_DEPTH {
		return &wsv1.Item{Value: &wsv1.Item_Depth{Depth: t}}
	}
	q := &marketdatav1.Tick{
		InstrumentId: t.GetInstrumentId(), ExchangeTsNs: t.GetExchangeTsNs(), RecvTsNs: t.GetRecvTsNs(),
		Source: t.GetSource(), Ltp: t.GetLtp(), Ltq: t.GetLtq(), Volume: t.GetVolume(), Oi: t.GetOi(),
		Open: t.GetOpen(), High: t.GetHigh(), Low: t.GetLow(), PrevClose: t.GetPrevClose(), AvgPrice: t.GetAvgPrice(),
		Bid: t.GetBid(), Ask: t.GetAsk(), BidQty: t.GetBidQty(), AskQty: t.GetAskQty(),
		Iv: t.Iv, Delta: t.Delta, Gamma: t.Gamma, Theta: t.Theta, Vega: t.Vega, Seq: t.GetSeq(), FeedId: t.GetFeedId(),
	}
	var change float64
	if pc := t.GetPrevClose(); pc > 0 && t.GetLtp() > 0 {
		change = float64(t.GetLtp()-pc) / float64(pc) * 100
	}
	return &wsv1.Item{Value: &wsv1.Item_Quote{Quote: &marketdatav1.Snapshot{Tick: q, ChangePct: change}}}
}

// ── writing ──────────────────────────────────────────────────────────────────

func (c *client) sendError(reqID uint32, code, message string) {
	c.send(&wsv1.ServerMsg{ReqId: reqID, Body: &wsv1.ServerMsg_Error{Error: &wsv1.Error{Code: code, Message: message}}})
}

// send queues a frame for the writer. If the outbound buffer would exceed MaxQueueBytes the queued
// frames are discarded, the client is told SLOW_CONSUMER and moves to snapshot-only mode.
func (c *client) send(m *wsv1.ServerMsg) {
	frame, err := proto.Marshal(m)
	if err != nil {
		c.hub.logger.Error("ws: marshal", "error", err)
		return
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	limit := c.hub.cfg.MaxQueueBytes
	if len(frame) > limit {
		c.hub.droppedFrames.Add(1)
		return
	}
	if c.queued+len(frame) > limit {
		c.hub.droppedFrames.Add(int64(len(c.queue)) + 1)
		c.queue, c.queued = c.queue[:0], 0
		if !c.slow {
			c.hub.slowConsumers.Add(1)
			c.hub.logger.Warn("ws: slow consumer; snapshot-only mode", "user", c.userID)
		}
		c.slow, c.slowSince = true, c.hub.now()
		frame, _ = proto.Marshal(&wsv1.ServerMsg{Body: &wsv1.ServerMsg_Error{Error: &wsv1.Error{
			Code: codeSlowConsumer, Message: "outbound buffer full; sending snapshots only",
		}}})
	}
	c.queue = append(c.queue, frame)
	c.queued += len(frame)
	select {
	case c.wake <- struct{}{}:
	default:
	}
}

func (c *client) writeLoop() {
	for {
		select {
		case <-c.done:
			return
		case <-c.wake:
		}
		for {
			c.mu.Lock()
			if len(c.queue) == 0 {
				c.mu.Unlock()
				break
			}
			f := c.queue[0]
			c.queue[0] = nil
			c.queue = c.queue[1:]
			c.queued -= len(f)
			c.mu.Unlock()
			wctx, cancel := context.WithTimeout(c.ctx, c.hub.cfg.WriteTimeout)
			err := c.conn.Write(wctx, websocket.MessageBinary, f)
			cancel()
			if err != nil {
				c.shutdown()
				return
			}
		}
	}
}

// housekeeping ends the connection when the access token expires and periodically re-reads the
// user's account mappings (an admin may unmap them mid-session).
func (c *client) housekeeping() {
	t := time.NewTicker(time.Second)
	defer t.Stop()
	refresh := c.hub.now().Add(c.hub.cfg.EntitlementRefresh)
	for {
		select {
		case <-c.done:
			return
		case <-t.C:
		}
		now := c.hub.now()
		c.mu.Lock()
		expired := !now.Before(c.expires)
		c.mu.Unlock()
		if expired {
			frame, _ := proto.Marshal(&wsv1.ServerMsg{Body: &wsv1.ServerMsg_Error{Error: &wsv1.Error{
				Code: codeAuthExpired, Message: "access token expired; reconnect",
			}}})
			wctx, cancel := context.WithTimeout(c.ctx, time.Second)
			_ = c.conn.Write(wctx, websocket.MessageBinary, frame)
			cancel()
			_ = c.conn.Close(closeUnauthenticated, "token expired")
			c.shutdown()
			return
		}
		if !now.Before(refresh) {
			refresh = now.Add(c.hub.cfg.EntitlementRefresh)
			c.refreshEntitlements()
		}
	}
}

func (c *client) shutdown() {
	c.once.Do(func() {
		close(c.done)
		_ = c.conn.CloseNow()
	})
}

func (h *Hub) limit(ch wsv1.Channel) int {
	switch ch {
	case wsv1.Channel_CHANNEL_DEPTH:
		return h.cfg.MaxDepthKeys
	case wsv1.Channel_CHANNEL_BAR_1M:
		return h.cfg.MaxBarKeys
	}
	return h.cfg.MaxQuoteKeys
}

func parseKeys(keys []string) ([]uint64, bool) {
	seen := map[uint64]bool{}
	out := make([]uint64, 0, len(keys))
	for _, k := range keys {
		id, err := strconv.ParseUint(k, 10, 64)
		if err != nil || id == 0 {
			return nil, false
		}
		if !seen[id] {
			seen[id] = true
			out = append(out, id)
		}
	}
	return out, true
}

func joinIDs(ids []uint64) string {
	parts := make([]string, len(ids))
	for i, id := range ids {
		parts[i] = strconv.FormatUint(id, 10)
	}
	return strings.Join(parts, ",")
}
