// Command gateway serves the Vyuha REST API and the WebSocket hub (/ws).
package main

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/nats-io/nats.go"

	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/api"
	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/auth"
	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/brokers"
	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/candles"
	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/config"
	vcrypto "github.com/Jitesh25X/vyuha-terminal/gateway/internal/crypto"
	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/enginec"
	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/quantc"
	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/store"
	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/telemetry"
	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/ws"
)

// version is overridden at build time with -ldflags "-X main.version=...".
var version = "0.0.1"

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "gateway:", err)
		os.Exit(1)
	}
}

func run() error {
	cfg, err := config.Load(os.Getenv)
	if err != nil {
		return err
	}
	logger := telemetry.NewLogger(os.Stdout, cfg.LogLevel)
	slog.SetDefault(logger)

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	shutdownTracing, err := telemetry.SetupTracing(ctx, cfg.OTLPEndpoint, "vyuha-gateway", version)
	if err != nil {
		return err
	}
	pool, err := store.NewPool(ctx, cfg.PostgresURL)
	if err != nil {
		return err
	}
	defer pool.Close()

	if cfg.MasterKey == "" {
		return errors.New("VYUHA_MASTER_KEY is not set (dev: make dev-secrets)")
	}
	envelope, err := vcrypto.NewEnvelope(cfg.MasterKey)
	if err != nil {
		return err
	}
	signingKey, err := auth.LoadSigningKey(cfg.JWTKeyPath)
	if err != nil {
		return err
	}
	authCfg := auth.DefaultConfig()
	authCfg.AccessTTL, authCfg.RefreshTTL = cfg.AccessTokenTTL, cfg.RefreshTTL
	// A quarter of the pool: a TOTP check briefly holds two connections, so at most half goes to auth bursts.
	authCfg.MaxConcurrentChecks = max(1, int(pool.Config().MaxConns)/4)
	authSvc, err := auth.NewService(pool, envelope, auth.NewSigner(signingKey), authCfg, time.Now)
	if err != nil {
		return err
	}

	engine, err := enginec.Dial(cfg.EngineGRPCAddr) // lazy: the gateway starts without the engine
	if err != nil {
		return err
	}
	defer func() { _ = engine.Close() }()
	// Market data is optional at startup: connect in the background and keep retrying.
	nc, err := nats.Connect(cfg.NATSURL, nats.Name("vyuha-gateway"), nats.RetryOnFailedConnect(true), nats.MaxReconnects(-1))
	if err != nil {
		return fmt.Errorf("nats: %w", err)
	}
	defer nc.Close()

	queries := store.New(pool)
	bars, err := candles.Open(cfg.ClickHouseURL) // lazy: connects on the first candles request
	if err != nil {
		return err
	}
	defer func() { _ = bars.Close() }()
	directory := ws.NewPGDirectory(queries)
	hubCfg := ws.DefaultConfig()
	hubCfg.OriginPatterns = cfg.WSAllowedOrigins
	hub := ws.New(hubCfg, authSvc.VerifyAccess, directory, engine, ws.NewNATSSource(nc, "md_last"), logger, time.Now)
	brokerSvc, err := brokers.NewService(queries, envelope, engine, brokers.DefaultConfig(cfg.PublicBaseURL), logger, time.Now, hub.ObserveSession)
	if err != nil {
		return err
	}
	hubCtx, stopHub := context.WithCancel(ctx)
	hubDone := make(chan struct{})
	go func() { hub.Run(hubCtx); close(hubDone) }()
	defer func() { stopHub(); <-hubDone }()

	mux := http.NewServeMux()
	mux.Handle("/ws", hub)
	mux.Handle("/", api.NewHandler(api.NewServer(version, cfg.Env, queries, authSvc, logger).
		WithTrustedProxies(cfg.TrustedProxies).WithBrokers(brokerSvc).
		WithMarketData(&api.MarketData{Instruments: queries, Candles: bars, Entitlement: directory}).
		WithOptionChains(&api.OptionChains{
			Store: queries, Entitlement: directory, Latest: hub, OI: bars, Quant: quantc.New(cfg.QuantURL),
		}), logger))
	srv := &http.Server{
		Addr:              cfg.HTTPAddr,
		Handler:           mux,
		ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout:       30 * time.Second,
		IdleTimeout:       120 * time.Second,
		ErrorLog:          slog.NewLogLogger(logger.Handler(), slog.LevelWarn),
	}

	logger.Info("gateway starting", "version", version, "config", cfg)
	errCh := make(chan error, 1)
	go func() { errCh <- srv.ListenAndServe() }()

	select {
	case err := <-errCh:
		if !errors.Is(err, http.ErrServerClosed) {
			return fmt.Errorf("listen: %w", err)
		}
	case <-ctx.Done():
		logger.Info("gateway shutting down")
	}

	shutdownCtx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if err := srv.Shutdown(shutdownCtx); err != nil {
		return fmt.Errorf("shutdown: %w", err)
	}
	if err := shutdownTracing(shutdownCtx); err != nil {
		return fmt.Errorf("tracing shutdown: %w", err)
	}
	return nil
}
