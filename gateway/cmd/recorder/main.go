// Command recorder subscribes to market data on NATS and writes it to ClickHouse (F-MD-4).
//
// Env: NATS_URL, CLICKHOUSE_URL (clickhouse://user:pass@host:9000/vyuha),
// RECORDER_METRICS_ADDR (default 127.0.0.1:9466: /metrics, /healthz), LOG_LEVEL.
package main

import (
	"context"
	"errors"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"sync"
	"syscall"
	"time"

	"github.com/nats-io/nats.go"

	"github.com/jiteshraj/vyuha/gateway/internal/recorder"
)

func env(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}

func main() {
	var level slog.Level
	_ = level.UnmarshalText([]byte(env("LOG_LEVEL", "info")))
	log := slog.New(slog.NewJSONHandler(os.Stderr, &slog.HandlerOptions{Level: level}))
	if err := run(log); err != nil {
		log.Error("recorder stopped", "err", err)
		os.Exit(1)
	}
}

func run(log *slog.Logger) error {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	ch, err := recorder.OpenClickHouse(ctx, env("CLICKHOUSE_URL", "clickhouse://vyuha:vyuha@127.0.0.1:9000/vyuha"))
	if err != nil {
		return err
	}
	defer func() { _ = ch.Close() }()

	rec := recorder.New(recorder.DefaultConfig(), ch, log)
	nc, err := nats.Connect(env("NATS_URL", nats.DefaultURL), nats.Name("vyuha-recorder"), nats.MaxReconnects(-1),
		nats.ErrorHandler(func(_ *nats.Conn, sub *nats.Subscription, err error) {
			if errors.Is(err, nats.ErrSlowConsumer) && sub != nil {
				if n, derr := sub.Dropped(); derr == nil && n > 0 {
					rec.AddDropped(uint64(n)) //nolint:gosec // G115: n > 0
				}
			}
		}))
	if err != nil {
		return err
	}
	defer nc.Close()
	handler := func(m *nats.Msg) { rec.Handle(m.Subject, m.Data) }
	for _, subj := range []string{"md.tick.>", "md.bar.>"} {
		sub, err := nc.QueueSubscribe(subj, "recorder", handler)
		if err != nil {
			return err
		}
		if err := sub.SetPendingLimits(1_000_000, 1<<30); err != nil {
			return err
		}
	}

	var wg sync.WaitGroup
	wg.Add(1)
	go func() { defer wg.Done(); rec.Run(ctx) }()

	mux := http.NewServeMux()
	mux.HandleFunc("GET /metrics", func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "text/plain; version=0.0.4")
		_ = recorder.WriteMetrics(w, rec.Stats())
	})
	mux.HandleFunc("GET /healthz", func(w http.ResponseWriter, _ *http.Request) {
		if !nc.IsConnected() {
			http.Error(w, "nats disconnected", http.StatusServiceUnavailable)
			return
		}
		_, _ = w.Write([]byte("ok\n"))
	})
	srv := &http.Server{Addr: env("RECORDER_METRICS_ADDR", "127.0.0.1:9466"), Handler: mux, ReadHeaderTimeout: 5 * time.Second}
	go func() {
		if err := srv.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
			log.Error("metrics server", "err", err)
		}
	}()

	// F-MD-4: drops must be visible; until the obs alert rule exists (M3.8), say so loudly.
	go func() {
		var last uint64
		t := time.NewTicker(10 * time.Second)
		defer t.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-t.C:
				if d := rec.Stats().Dropped; d > last {
					log.Error("recorder dropping market data", "recorder_dropped_total", d, "new", d-last)
					last = d
				}
			}
		}
	}()

	log.Info("recorder started")
	<-ctx.Done()
	_ = nc.Drain()
	wg.Wait()
	shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	_ = srv.Shutdown(shutdownCtx)
	s := rec.Stats()
	log.Info("recorder stopped", "received", s.Received, "inserted_ticks", s.InsertedTicks, "dropped", s.Dropped, "lost", s.LostRows)
	return nil
}
