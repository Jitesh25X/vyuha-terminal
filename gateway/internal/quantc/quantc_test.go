package quantc

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

func TestOptionChainRoundTrip(t *testing.T) {
	var got map[string]any
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/internal/optionchain" || r.Method != http.MethodPost || r.Header.Get("Content-Type") != "application/json" {
			http.Error(w, "bad request", http.StatusBadRequest)
			return
		}
		_ = json.NewDecoder(r.Body).Decode(&got)
		_, _ = w.Write([]byte(`{"model":"black76","underlying_price":24310.5,"forward_source":"future","t_years":0.02,
			"rate":0.055,"atm_strike":"24300","pcr":0.8,"max_pain":null,
			"rows":[{"strike":"24300","ce":{"iv":0.12,"delta":0.5,"gamma":null,"theta":-4.2,"vega":12.1},"pe":null}]}`))
	}))
	defer srv.Close()

	ltp := "180.5000"
	req := &ChainRequest{Kind: "index", Now: time.Date(2026, 10, 29, 5, 30, 0, 0, time.UTC), Expiry: "2026-11-03", TickSize: "0.05",
		Strikes: []Strike{{Strike: "24300", CE: &SideQuote{LTP: &ltp, OI: 10}}}}
	res, err := New(srv.URL).OptionChain(context.Background(), req)
	if err != nil {
		t.Fatal(err)
	}
	if res.Model != "black76" || *res.UnderlyingPrice != 24310.5 || res.MaxPain != nil || *res.Rows[0].CE.IV != 0.12 || res.Rows[0].CE.Gamma != nil || res.Rows[0].PE != nil {
		t.Fatalf("result %+v", res)
	}
	// Absent quotes are omitted, not sent as null; time is RFC 3339 UTC.
	if got["now"] != "2026-10-29T05:30:00Z" || got["spot"] != nil || got["strikes"].([]any)[0].(map[string]any)["pe"] != nil {
		t.Fatalf("request body %v", got)
	}
}

func TestOptionChainErrors(t *testing.T) {
	for name, h := range map[string]http.HandlerFunc{
		"status":    func(w http.ResponseWriter, _ *http.Request) { http.Error(w, "boom", http.StatusUnprocessableEntity) },
		"row count": func(w http.ResponseWriter, _ *http.Request) { _, _ = w.Write([]byte(`{"model":"bsm","rows":[]}`)) },
		"not json":  func(w http.ResponseWriter, _ *http.Request) { _, _ = w.Write([]byte(`<html>`)) },
	} {
		t.Run(name, func(t *testing.T) {
			srv := httptest.NewServer(h)
			defer srv.Close()
			_, err := New(srv.URL).OptionChain(context.Background(), &ChainRequest{Strikes: []Strike{{Strike: "1"}}})
			if err == nil || !strings.HasPrefix(err.Error(), "quantc:") {
				t.Fatalf("err = %v", err)
			}
		})
	}
	// Unreachable quant fails fast instead of hanging the request.
	_, err := New("http://127.0.0.1:1").OptionChain(context.Background(), &ChainRequest{})
	if err == nil {
		t.Fatal("expected a connection error")
	}
}
