// Package quantc is the gateway's client for the quant service's internal HTTP API
// (quant/src/vyuha_quant/api). The gateway sends only prices the requesting user may see (ADR-020);
// quant computes analytics on them and holds no market data of its own.
package quantc

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"time"
)

// Timeout bounds one call (ARCHITECTURE §8: when quant is slow or down, analytics panels go stale and
// trading is unaffected).
const Timeout = 2 * time.Second

// maxResponse bounds a response body (a 1,000-strike chain is ~300 KB).
const maxResponse = 4 << 20

// SideQuote is one contract's latest market fields. Prices are decimal strings in rupees.
type SideQuote struct {
	LTP *string `json:"ltp,omitempty"`
	Bid *string `json:"bid,omitempty"`
	Ask *string `json:"ask,omitempty"`
	OI  uint64  `json:"oi"`
	// TradedAt is when the gateway last saw the contract trade (nil: not seen); quant uses the LTP for
	// IV only if this is recent.
	TradedAt *time.Time `json:"traded_at,omitempty"`
}

// OIStrike is one strike's OI across the whole expiry (PCR, max pain).
type OIStrike struct {
	Strike string `json:"strike"`
	CEOI   uint64 `json:"ce_oi"`
	PEOI   uint64 `json:"pe_oi"`
}

// Strike is one strike of the request.
type Strike struct {
	Strike string     `json:"strike"`
	CE     *SideQuote `json:"ce,omitempty"`
	PE     *SideQuote `json:"pe,omitempty"`
}

// Dividend is a cash dividend going ex before expiry (stock options).
type Dividend struct {
	ExDate string `json:"ex_date"` // YYYY-MM-DD
	Amount string `json:"amount"`
}

// ChainRequest mirrors vyuha_quant.optionchain.ChainRequest.
type ChainRequest struct {
	Kind      string     `json:"kind"` // index | stock
	Now       time.Time  `json:"now"`
	Expiry    string     `json:"expiry"` // YYYY-MM-DD
	TickSize  string     `json:"tick_size"`
	Spot      *string    `json:"spot,omitempty"`
	Future    *string    `json:"future,omitempty"`
	Dividends []Dividend `json:"dividends,omitempty"`
	Strikes   []Strike   `json:"strikes"` // display window: IV/Greeks
	OI        []OIStrike `json:"oi"`      // full expiry: PCR, max pain
}

// Side is per-contract analytics; nil where undefined.
type Side struct {
	IV       *float64 `json:"iv"`
	IVSource *string  `json:"iv_source"` // mid | ltp
	Delta    *float64 `json:"delta"`
	Gamma    *float64 `json:"gamma"`
	Theta    *float64 `json:"theta"`
	Vega     *float64 `json:"vega"`
}

// Row is one strike of the result, in request order.
type Row struct {
	Strike string `json:"strike"`
	CE     *Side  `json:"ce"`
	PE     *Side  `json:"pe"`
}

// ChainResult mirrors vyuha_quant.optionchain.ChainResult.
type ChainResult struct {
	Model           string   `json:"model"`
	UnderlyingPrice *float64 `json:"underlying_price"`
	ForwardSource   *string  `json:"forward_source"`
	TYears          float64  `json:"t_years"`
	Rate            float64  `json:"rate"`
	RateAsOf        *string  `json:"rate_as_of"` // YYYY-MM-DD
	ATMStrike       *string  `json:"atm_strike"`
	PCR             *float64 `json:"pcr"`
	MaxPain         *string  `json:"max_pain"`
	OIStrikes       int      `json:"oi_strikes"`
	Rows            []Row    `json:"rows"`
}

// Client calls quant over loopback HTTP.
type Client struct {
	base string
	hc   *http.Client
}

// New returns a client for baseURL (config QUANT_URL).
func New(baseURL string) *Client {
	return &Client{base: baseURL, hc: &http.Client{Timeout: Timeout}}
}

// OptionChain computes IV, Greeks, PCR and max pain for one expiry.
func (c *Client) OptionChain(ctx context.Context, req *ChainRequest) (*ChainResult, error) {
	body, err := json.Marshal(req)
	if err != nil {
		return nil, fmt.Errorf("quantc: encode: %w", err)
	}
	ctx, cancel := context.WithTimeout(ctx, Timeout)
	defer cancel()
	hr, err := http.NewRequestWithContext(ctx, http.MethodPost, c.base+"/internal/optionchain", bytes.NewReader(body))
	if err != nil {
		return nil, fmt.Errorf("quantc: request: %w", err)
	}
	hr.Header.Set("Content-Type", "application/json")
	resp, err := c.hc.Do(hr)
	if err != nil {
		return nil, fmt.Errorf("quantc: optionchain: %w", err)
	}
	defer func() { _ = resp.Body.Close() }()
	if resp.StatusCode != http.StatusOK {
		_, _ = io.Copy(io.Discard, io.LimitReader(resp.Body, maxResponse))
		return nil, fmt.Errorf("quantc: optionchain: status %d", resp.StatusCode)
	}
	var out ChainResult
	if err := json.NewDecoder(io.LimitReader(resp.Body, maxResponse)).Decode(&out); err != nil {
		return nil, fmt.Errorf("quantc: decode: %w", err)
	}
	if len(out.Rows) != len(req.Strikes) {
		return nil, errors.New("quantc: optionchain returned a different number of rows")
	}
	return &out, nil
}
