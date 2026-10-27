// Command gateway serves the Vyuha REST API (and, from M1.8, the WebSocket hub).
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

	"github.com/jiteshraj/vyuha/gateway/internal/api"
	"github.com/jiteshraj/vyuha/gateway/internal/auth"
	"github.com/jiteshraj/vyuha/gateway/internal/config"
	vcrypto "github.com/jiteshraj/vyuha/gateway/internal/crypto"
	"github.com/jiteshraj/vyuha/gateway/internal/store"
	"github.com/jiteshraj/vyuha/gateway/internal/telemetry"
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
	authSvc, err := auth.NewService(pool, envelope, auth.NewSigner(signingKey), authCfg, time.Now)
	if err != nil {
		return err
	}

	srv := &http.Server{
		Addr:              cfg.HTTPAddr,
		Handler:           api.NewHandler(api.NewServer(version, cfg.Env, store.New(pool), authSvc, logger), logger),
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
