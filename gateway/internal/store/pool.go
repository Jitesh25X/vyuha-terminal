package store

import (
	"context"
	"fmt"

	"github.com/jackc/pgx/v5/pgxpool"
)

// NewPool creates a pgx pool. Connections are opened lazily, so the gateway starts (and reports
// a degraded health check) even when Postgres is down.
func NewPool(ctx context.Context, postgresURL string) (*pgxpool.Pool, error) {
	cfg, err := pgxpool.ParseConfig(postgresURL)
	if err != nil {
		return nil, fmt.Errorf("parse POSTGRES_URL: %w", err) // pgx parse errors do not echo the password
	}
	cfg.ConnConfig.RuntimeParams["timezone"] = "UTC"
	pool, err := pgxpool.NewWithConfig(ctx, cfg)
	if err != nil {
		return nil, fmt.Errorf("create pool: %w", err)
	}
	return pool, nil
}
