// Package enginec is the gateway's gRPC client to the C++ engine (proto/vyuha/engine/v1).
// Every call carries a deadline (2 s by default, gateway/CLAUDE.md).
package enginec

import (
	"context"
	"errors"
	"fmt"
	"regexp"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"

	enginev1 "github.com/Jitesh25X/vyuha-terminal/gateway/gen/vyuha/engine/v1"
	marketdatav1 "github.com/Jitesh25X/vyuha-terminal/gateway/gen/vyuha/marketdata/v1"
)

// DefaultTimeout bounds each engine call; SessionTimeout the session start/stop calls, which run
// the engine's preflight and stop feeds (a timeout there would mark a session failed that started).
const (
	DefaultTimeout = 2 * time.Second
	SessionTimeout = 10 * time.Second
)

// Requester is the gateway's identity in the engine's per-requester subscription interest.
const Requester = "gateway"

// stableCode matches the engine's stable error codes, sent as the whole status message.
var stableCode = regexp.MustCompile(`^[A-Z][A-Z0-9_]+$`)

// ErrUnavailable means the engine could not be reached or did not answer in time.
var ErrUnavailable = errors.New("enginec: engine unavailable")

// RefusedError is a request the engine rejected; Code is the engine's stable code when it sent one
// (e.g. STATIC_IP_NOT_ON_HOST), otherwise the gRPC code name.
type RefusedError struct {
	Code    string
	Message string
}

func (e *RefusedError) Error() string { return "enginec: refused: " + e.Code + ": " + e.Message }

// Client wraps the generated EngineService client.
type Client struct {
	conn           *grpc.ClientConn
	rpc            enginev1.EngineServiceClient
	timeout        time.Duration
	sessionTimeout time.Duration
}

// Dial connects to the engine. The engine listens on loopback (ENGINE_GRPC_ADDR); mTLS for a
// private network arrives with the production deploy (API.md §9).
func Dial(addr string) (*Client, error) {
	conn, err := grpc.NewClient(addr, grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		return nil, fmt.Errorf("enginec: dial %s: %w", addr, err)
	}
	return &Client{conn: conn, rpc: enginev1.NewEngineServiceClient(conn), timeout: DefaultTimeout, sessionTimeout: SessionTimeout}, nil
}

// Close releases the connection.
func (c *Client) Close() error { return c.conn.Close() }

func (c *Client) ctx(parent context.Context) (context.Context, context.CancelFunc) {
	return context.WithTimeout(parent, c.timeout)
}

// StartSession starts (or replaces) an account's broker session. The request carries secrets: it is
// never logged here, and callers drop it after the call.
func (c *Client) StartSession(ctx context.Context, req *enginev1.StartSessionRequest) (*enginev1.SessionInfo, error) {
	ctx, cancel := context.WithTimeout(ctx, c.sessionTimeout)
	defer cancel()
	info, err := c.rpc.StartSession(ctx, req)
	return info, wrap(err)
}

// StopSession stops an account's session; an unknown account is not an error.
func (c *Client) StopSession(ctx context.Context, accountID, reason string) error {
	ctx, cancel := context.WithTimeout(ctx, c.sessionTimeout)
	defer cancel()
	_, err := c.rpc.StopSession(ctx, &enginev1.StopSessionRequest{AccountId: accountID, Reason: reason})
	if c := status.Code(err); c == codes.NotFound || c == codes.InvalidArgument { // not running
		return nil
	}
	return wrap(err)
}

// GetSessions lists the engine's sessions with their feed ids.
func (c *Client) GetSessions(ctx context.Context) ([]*enginev1.SessionInfo, error) {
	ctx, cancel := c.ctx(ctx)
	defer cancel()
	list, err := c.rpc.GetSessions(ctx, &enginev1.GetSessionsRequest{})
	if err != nil {
		return nil, wrap(err)
	}
	return list.GetSessions(), nil
}

// Subscribe registers the gateway's interest in ids through one account's feed.
func (c *Client) Subscribe(ctx context.Context, ids []uint64, mode marketdatav1.FeedMode, viaAccount string) error {
	ctx, cancel := c.ctx(ctx)
	defer cancel()
	_, err := c.rpc.Subscribe(ctx, &enginev1.SubscribeRequest{
		Requester: Requester, InstrumentIds: ids, Mode: mode, ViaAccountId: viaAccount,
	})
	return wrap(err)
}

// Unsubscribe drops the gateway's interest in ids on one account's feed.
func (c *Client) Unsubscribe(ctx context.Context, ids []uint64, viaAccount string) error {
	ctx, cancel := c.ctx(ctx)
	defer cancel()
	_, err := c.rpc.Unsubscribe(ctx, &enginev1.SubscribeRequest{
		Requester: Requester, InstrumentIds: ids, ViaAccountId: viaAccount,
	})
	return wrap(err)
}

func wrap(err error) error {
	if err == nil {
		return nil
	}
	st, ok := status.FromError(err)
	if !ok {
		return fmt.Errorf("%w: %w", ErrUnavailable, err)
	}
	switch st.Code() {
	case codes.Unavailable, codes.DeadlineExceeded, codes.Canceled:
		return fmt.Errorf("%w: %s", ErrUnavailable, st.Code())
	}
	code := st.Code().String()
	if stableCode.MatchString(st.Message()) { // e.g. FAILED_PRECONDITION "STATIC_IP_NOT_ON_HOST"
		code = st.Message()
	}
	return &RefusedError{Code: code, Message: st.Message()}
}
