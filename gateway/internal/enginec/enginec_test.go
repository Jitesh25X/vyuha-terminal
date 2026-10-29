package enginec

import (
	"context"
	"errors"
	"net"
	"testing"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	enginev1 "github.com/jiteshraj/vyuha/gateway/gen/vyuha/engine/v1"
	marketdatav1 "github.com/jiteshraj/vyuha/gateway/gen/vyuha/marketdata/v1"
)

type fakeEngine struct {
	enginev1.UnimplementedEngineServiceServer
	lastSub *enginev1.SubscribeRequest
}

func (f *fakeEngine) StartSession(_ context.Context, r *enginev1.StartSessionRequest) (*enginev1.SessionInfo, error) {
	switch r.GetAccountId() {
	case "bad-ip":
		return nil, status.Error(codes.FailedPrecondition, "STATIC_IP_NOT_ON_HOST")
	case "bad-cap":
		return nil, status.Error(codes.InvalidArgument, "ops_cap must be 1..10")
	case "slow":
		time.Sleep(500 * time.Millisecond)
	}
	return &enginev1.SessionInfo{AccountId: r.GetAccountId(), State: "connected", FeedId: 4}, nil
}

func (f *fakeEngine) StopSession(context.Context, *enginev1.StopSessionRequest) (*enginev1.Ack, error) {
	return nil, status.Error(codes.InvalidArgument, "session: unknown account")
}

func (f *fakeEngine) Subscribe(_ context.Context, r *enginev1.SubscribeRequest) (*enginev1.Ack, error) {
	f.lastSub = r
	return &enginev1.Ack{Ok: true}, nil
}

func TestClient(t *testing.T) {
	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	srv := grpc.NewServer()
	fake := &fakeEngine{}
	enginev1.RegisterEngineServiceServer(srv, fake)
	go func() { _ = srv.Serve(lis) }()
	defer srv.Stop()

	c, err := Dial(lis.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer func() { _ = c.Close() }()
	ctx := context.Background()

	info, err := c.StartSession(ctx, &enginev1.StartSessionRequest{AccountId: "a1"})
	if err != nil || info.GetFeedId() != 4 {
		t.Fatalf("start = %v, %v", info, err)
	}
	var refused *RefusedError
	if _, err := c.StartSession(ctx, &enginev1.StartSessionRequest{AccountId: "bad-ip"}); !errors.As(err, &refused) || refused.Code != "STATIC_IP_NOT_ON_HOST" {
		t.Fatalf("bad ip: %v", err)
	}
	if _, err := c.StartSession(ctx, &enginev1.StartSessionRequest{AccountId: "bad-cap"}); !errors.As(err, &refused) || refused.Code != "InvalidArgument" {
		t.Fatalf("bad cap: %v", err)
	}
	c.sessionTimeout = 100 * time.Millisecond
	if _, err := c.StartSession(ctx, &enginev1.StartSessionRequest{AccountId: "slow"}); !errors.Is(err, ErrUnavailable) {
		t.Fatalf("deadline: %v", err)
	}
	c.sessionTimeout = SessionTimeout
	if err := c.StopSession(ctx, "never-started", "test"); err != nil {
		t.Fatalf("stopping an unknown session is not an error: %v", err)
	}
	if err := c.Subscribe(ctx, []uint64{1, 2}, marketdatav1.FeedMode_FEED_MODE_FULL, "a1"); err != nil {
		t.Fatal(err)
	}
	if r := fake.lastSub; r.GetRequester() != Requester || r.GetViaAccountId() != "a1" || len(r.GetInstrumentIds()) != 2 {
		t.Fatalf("subscribe request %v", r)
	}

	srv.Stop()
	if _, err := c.GetSessions(ctx); !errors.Is(err, ErrUnavailable) {
		t.Fatalf("engine down: %v", err)
	}
}
