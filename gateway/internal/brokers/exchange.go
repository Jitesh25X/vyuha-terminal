package brokers

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/netip"
	"net/url"
	"strings"
	"syscall"
	"time"
)

// exchangeTimeout bounds one token exchange (the request token / code is short-lived anyway).
const exchangeTimeout = 10 * time.Second

// maxResponse caps how much of a broker response is read.
const maxResponse = 1 << 20

var (
	// ErrSourceIP: the account's static IP is not on this host (invariant 4); nothing was sent.
	ErrSourceIP = errors.New("brokers: static IP not on host")
	// ErrExchange: the broker refused the token exchange or answered unexpectedly. Details are not
	// kept: broker error bodies can echo request parameters.
	ErrExchange = errors.New("brokers: token exchange failed")
)

// grant is what a token exchange yields. AccessToken is a secret.
type grant struct {
	UserID      string // broker client code the token belongs to
	AccessToken string
}

// boundClient returns an HTTP client whose connections leave from sourceIP (invariant 4: an
// account's traffic uses its own static IP). Proxies from the environment are ignored, since a
// proxy would change the egress address.
func boundClient(sourceIP netip.Addr) *http.Client {
	d := &net.Dialer{Timeout: 5 * time.Second, LocalAddr: &net.TCPAddr{IP: sourceIP.AsSlice()}}
	return &http.Client{
		Timeout: exchangeTimeout,
		Transport: &http.Transport{
			Proxy:               nil,
			DialContext:         d.DialContext,
			ForceAttemptHTTP2:   true,
			TLSHandshakeTimeout: 5 * time.Second,
			MaxIdleConns:        1,
			DisableKeepAlives:   true,
		},
		CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse },
	}
}

// kiteChecksum is SHA-256(api_key + request_token + api_secret), hex (Kite Connect v3 /session/token).
func kiteChecksum(apiKey, requestToken, apiSecret string) string {
	sum := sha256.Sum256([]byte(apiKey + requestToken + apiSecret))
	return hex.EncodeToString(sum[:])
}

// exchangeKite: POST <base>/session/token (api_key, request_token, checksum), X-Kite-Version: 3.
func exchangeKite(ctx context.Context, c *http.Client, base, apiKey, apiSecret, requestToken string) (grant, error) {
	form := url.Values{
		"api_key":       {apiKey},
		"request_token": {requestToken},
		"checksum":      {kiteChecksum(apiKey, requestToken, apiSecret)},
	}
	var out struct {
		Status string `json:"status"`
		Data   struct {
			UserID      string `json:"user_id"`
			AccessToken string `json:"access_token"`
		} `json:"data"`
	}
	if err := postForm(ctx, c, base+"/session/token", form, map[string]string{"X-Kite-Version": "3"}, &out); err != nil {
		return grant{}, err
	}
	if out.Status != "success" || out.Data.AccessToken == "" || out.Data.UserID == "" {
		return grant{}, ErrExchange
	}
	return grant{UserID: out.Data.UserID, AccessToken: out.Data.AccessToken}, nil
}

// exchangeUpstox: POST <base>/v2/login/authorization/token (code, client_id, client_secret,
// redirect_uri, grant_type=authorization_code). The code is single-use whether or not this succeeds.
func exchangeUpstox(ctx context.Context, c *http.Client, base, apiKey, apiSecret, code, redirectURI string) (grant, error) {
	form := url.Values{
		"code":          {code},
		"client_id":     {apiKey},
		"client_secret": {apiSecret},
		"redirect_uri":  {redirectURI},
		"grant_type":    {"authorization_code"},
	}
	var out struct {
		UserID      string `json:"user_id"`
		AccessToken string `json:"access_token"`
	}
	if err := postForm(ctx, c, base+"/v2/login/authorization/token", form, map[string]string{"Accept": "application/json"}, &out); err != nil {
		return grant{}, err
	}
	if out.AccessToken == "" || out.UserID == "" {
		return grant{}, ErrExchange
	}
	return grant{UserID: out.UserID, AccessToken: out.AccessToken}, nil
}

func postForm(ctx context.Context, c *http.Client, endpoint string, form url.Values, headers map[string]string, out any) error {
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, endpoint, strings.NewReader(form.Encode()))
	if err != nil {
		return fmt.Errorf("%w: build request", ErrExchange)
	}
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	for k, v := range headers {
		req.Header.Set(k, v)
	}
	resp, err := c.Do(req)
	if err != nil {
		if errors.Is(err, syscall.EADDRNOTAVAIL) {
			return ErrSourceIP
		}
		// Never wrap err: *url.Error carries the URL, and the form travels in the body only, but
		// keep the message generic so nothing request-specific reaches logs.
		return fmt.Errorf("%w: transport", ErrExchange)
	}
	defer func() { _ = resp.Body.Close() }()
	body, err := io.ReadAll(io.LimitReader(resp.Body, maxResponse))
	if err != nil {
		return fmt.Errorf("%w: read", ErrExchange)
	}
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("%w: http %d", ErrExchange, resp.StatusCode)
	}
	if err := json.Unmarshal(body, out); err != nil {
		return fmt.Errorf("%w: decode", ErrExchange)
	}
	return nil
}
