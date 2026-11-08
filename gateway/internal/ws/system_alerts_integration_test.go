package ws

import (
	"context"
	"errors"
	"io"
	"log/slog"
	"net/http/httptest"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/golang-jwt/jwt/v5"
	"github.com/nats-io/nats.go"
	"google.golang.org/protobuf/proto"

	commonv1 "github.com/Jitesh25X/vyuha-terminal/gateway/gen/vyuha/common/v1"
	marketdatav1 "github.com/Jitesh25X/vyuha-terminal/gateway/gen/vyuha/marketdata/v1"
	wsv1 "github.com/Jitesh25X/vyuha-terminal/gateway/gen/vyuha/ws/v1"
	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/auth"
	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/testenv"
)

// noDatabase is the Directory with Postgres down: every call fails.
type noDatabase struct{}

var errNoDatabase = errors.New("postgres is down")

func (noDatabase) UserAccounts(context.Context, string) ([]Account, error) { return nil, errNoDatabase }
func (noDatabase) Instruments(context.Context, []uint64) (map[uint64]InstrumentMap, error) {
	return nil, errNoDatabase
}
func (noDatabase) IsHoliday(context.Context, time.Time) (bool, error) { return false, errNoDatabase }

// natsAlertsOnly is a market-data source with no data and real sys.alert delivery.
type natsAlertsOnly struct{ *NATSSource }

func (natsAlertsOnly) Run(ctx context.Context, _ func(*marketdatav1.Tick), _ func(*marketdatav1.Bar)) error {
	<-ctx.Done()
	return nil
}

const outageBanner = "ORDER LOG DOWN: new orders blocked"

func outage(active bool, sinceMS int64) []byte {
	b, _ := proto.Marshal(&commonv1.SystemAlert{Code: "ORDER_LOG_OUTAGE", Severity: "critical", Active: active, Banner: outageBanner, SinceMs: sinceMS})
	return b
}

func hasBanner(st *wsv1.SystemStatus) bool {
	return len(st.GetBanners()) == 1 && st.GetBanners()[0] == outageBanner
}

func statusOf(m *wsv1.ServerMsg) *wsv1.SystemStatus {
	if u := m.GetUpdates(); u != nil && u.GetChannel() == wsv1.Channel_CHANNEL_SYSTEM && len(u.GetItems()) == 1 {
		return u.GetItems()[0].GetSystem()
	}
	return nil
}

// D-19: with Postgres down and only NATS up, an admin gets a toast and everyone gets the banner, including
// a browser that connects during the outage. The banner clears on recovery.
func TestOutageReachesTheBrowserWithNoDatabase(t *testing.T) {
	natsURL := os.Getenv("VYUHA_TEST_NATS_URL")
	if natsURL == "" {
		testenv.SkipWithoutService(t, "VYUHA_TEST_NATS_URL not set")
	}
	nc, err := nats.Connect(natsURL)
	if err != nil {
		t.Fatal(err)
	}
	defer nc.Close()

	h := &harness{t: t}
	verify := func(token string) (*auth.Claims, error) {
		user, role, ok := strings.Cut(token, ":")
		if !ok {
			return nil, errors.New("bad token")
		}
		return &auth.Claims{RegisteredClaims: jwt.RegisteredClaims{Subject: user, ExpiresAt: jwt.NewNumericDate(time.Now().Add(time.Hour))}, Role: role}, nil
	}
	h.hub = New(fastConfig(), verify, noDatabase{}, &fakeEngine{}, natsAlertsOnly{NewNATSSource(nc, "unused")}, slog.New(slog.NewTextHandler(io.Discard, nil)), time.Now)
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() { h.hub.Run(ctx); close(done) }()
	srv := httptest.NewServer(h.hub)
	t.Cleanup(func() { srv.Close(); cancel(); <-done })
	h.url = "ws" + strings.TrimPrefix(srv.URL, "http")

	admin := h.dial("root:admin")
	trader := h.dial("alice:trader")
	for _, c := range []*wsClient{admin, trader} {
		c.subscribe(wsv1.Channel_CHANNEL_SYSTEM, 0)
		c.subscribe(wsv1.Channel_CHANNEL_ALERTS, 0)
		c.next(func(m *wsv1.ServerMsg) bool { return m.GetSnapshot().GetChannel() == wsv1.Channel_CHANNEL_ALERTS })
	}

	// Core NATS: repeat until the hub's subscription is live, as the engine does.
	const since = int64(1_793_190_900_000)
	raised := false
	for deadline := time.Now().Add(5 * time.Second); !raised && time.Now().Before(deadline); {
		if err := nc.Publish(SystemAlertSubject, outage(true, since)); err != nil {
			t.Fatal(err)
		}
		if a := admin.read(20 * time.Millisecond).GetAlert(); a != nil {
			if a.GetTitle() != "ORDER_LOG_OUTAGE" || a.GetSeverity() != "critical" || !strings.Contains(a.GetBodyJson(), outageBanner) {
				t.Fatalf("alert = %v", a)
			}
			raised = true
		}
	}
	if !raised {
		t.Fatal("no toast alert reached the admin")
	}
	for name, c := range map[string]*wsClient{"admin": admin, "trader": trader} {
		if st := statusOf(c.next(func(m *wsv1.ServerMsg) bool { return hasBanner(statusOf(m)) })); !hasBanner(st) {
			t.Fatalf("%s: no banner: %v", name, st)
		}
	}

	// Connecting during the outage (accounts can't be loaded) still shows the banner.
	late := h.dial("bob:trader")
	late.subscribe(wsv1.Channel_CHANNEL_SYSTEM, 0)
	st := late.next(func(m *wsv1.ServerMsg) bool { return m.GetSnapshot().GetChannel() == wsv1.Channel_CHANNEL_SYSTEM }).GetSnapshot().GetItems()[0].GetSystem()
	if !hasBanner(st) {
		t.Fatalf("late client: no banner: %v", st)
	}

	if err := nc.Publish(SystemAlertSubject, outage(false, since)); err != nil {
		t.Fatal(err)
	}
	trader.next(func(m *wsv1.ServerMsg) bool { s := statusOf(m); return s != nil && len(s.GetBanners()) == 0 })
}
