package ws

import (
	"context"
	"encoding/json"
	"fmt"
	"slices"
	"strconv"
	"time"

	"github.com/nats-io/nats.go"
	"google.golang.org/protobuf/proto"

	commonv1 "github.com/Jitesh25X/vyuha-terminal/gateway/gen/vyuha/common/v1"
	wsv1 "github.com/Jitesh25X/vyuha-terminal/gateway/gen/vyuha/ws/v1"
)

// SystemAlertSubject carries conditions that must reach the desk with Postgres down (ADR-027).
const SystemAlertSubject = "sys.alert"

// Field limits for a sys.alert: what's relayed to browsers stays small.
const (
	maxAlertCode   = 64
	maxAlertBanner = 200
)

// SystemAlertSource delivers sys.alert messages. A Source that also implements it feeds the alerts channel
// and the status bar banners.
type SystemAlertSource interface {
	RunSystemAlerts(ctx context.Context, onAlert func(*commonv1.SystemAlert)) error
}

// RunSystemAlerts implements SystemAlertSource.
func (s *NATSSource) RunSystemAlerts(ctx context.Context, onAlert func(*commonv1.SystemAlert)) error {
	sub, err := s.nc.Subscribe(SystemAlertSubject, func(m *nats.Msg) {
		a := &commonv1.SystemAlert{}
		if proto.Unmarshal(m.Data, a) == nil {
			onAlert(a)
		}
	})
	if err != nil {
		return fmt.Errorf("ws: subscribe %s: %w", SystemAlertSubject, err)
	}
	defer func() { _ = sub.Unsubscribe() }()
	<-ctx.Done()
	return nil
}

// banner is one condition holding now.
type banner struct {
	text    string
	sinceMS int64
	seen    time.Time
}

func (h *Hub) runSystemAlerts(ctx context.Context, src SystemAlertSource) {
	for ctx.Err() == nil {
		if err := src.RunSystemAlerts(ctx, h.onSystemAlert); err != nil && ctx.Err() == nil {
			h.logger.Error("ws: system alert source stopped; retrying", "error", err)
			select {
			case <-ctx.Done():
			case <-time.After(time.Second):
			}
		}
	}
}

// onSystemAlert keeps the banner for a condition while it holds, and raises one alert per occurrence.
// Neither touches Postgres.
func (h *Hub) onSystemAlert(a *commonv1.SystemAlert) {
	code := a.GetCode()
	if code == "" || len(code) > maxAlertCode || len(a.GetBanner()) > maxAlertBanner {
		h.logger.Warn("ws: dropped a malformed system alert", "code_len", len(code), "banner_len", len(a.GetBanner()))
		return
	}
	h.mu.Lock()
	cur, had := h.banners[code]
	occurrence := false
	changed := false
	if a.GetActive() {
		last, alerted := h.toasted[code]
		occurrence = !alerted || last != a.GetSinceMs() // a banner that lapsed and came back is the same outage
		changed = !had || cur.sinceMS != a.GetSinceMs()
		h.toasted[code] = a.GetSinceMs()
		h.banners[code] = &banner{text: a.GetBanner(), sinceMS: a.GetSinceMs(), seen: h.now()}
	} else if had && cur.sinceMS == a.GetSinceMs() { // an older occurrence's end doesn't clear a newer one
		delete(h.banners, code)
		changed = true
	}
	clients := h.clientList()
	h.mu.Unlock()

	if occurrence {
		alert := wsAlert(a)
		for _, c := range clients {
			c.pushAlert(alert)
		}
	}
	if changed { // now, not on the next one-second status push
		for _, c := range clients {
			c.pushSystem()
		}
	}
}

func wsAlert(a *commonv1.SystemAlert) *wsv1.Alert {
	body, _ := json.Marshal(map[string]string{"message": a.GetBanner()}) // a map of strings always marshals
	return &wsv1.Alert{
		Id:       a.GetCode() + ":" + strconv.FormatInt(a.GetSinceMs(), 10),
		Severity: a.GetSeverity(),
		Title:    a.GetCode(),
		BodyJson: string(body),
		TsMs:     a.GetSinceMs(),
	}
}

// expireBanners drops conditions nobody has repeated lately, such as an engine that died mid-outage. The
// engine-health indicator covers that case.
func (h *Hub) expireBanners() {
	h.mu.Lock()
	defer h.mu.Unlock()
	now := h.now()
	for code, b := range h.banners {
		if now.Sub(b.seen) >= h.cfg.SystemAlertTTL {
			delete(h.banners, code)
		}
	}
}

// bannerTexts lists the banners in a stable order (caller holds mu).
func (h *Hub) bannerTexts() []string {
	if len(h.banners) == 0 {
		return nil
	}
	out := make([]string, 0, len(h.banners))
	for _, b := range h.banners {
		out = append(out, b.text)
	}
	slices.Sort(out)
	return out
}

// clientList copies the connected clients (caller holds mu).
func (h *Hub) clientList() []*client {
	clients := make([]*client, 0, len(h.clients))
	for c := range h.clients {
		clients = append(clients, c)
	}
	return clients
}
