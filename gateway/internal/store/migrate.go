package store

import (
	"errors"
	"fmt"
	"strings"

	"github.com/golang-migrate/migrate/v4"
	_ "github.com/golang-migrate/migrate/v4/database/pgx/v5" // pgx5:// driver, schema_migrations(version, dirty)
	_ "github.com/golang-migrate/migrate/v4/source/file"
)

// MigrateUp applies all pending migrations in dir (db/migrations) and returns the resulting version.
func MigrateUp(dir, postgresURL string) (uint, error) {
	m, err := newMigrate(dir, postgresURL)
	if err != nil {
		return 0, err
	}
	defer closeMigrate(m)
	if err := m.Up(); err != nil && !errors.Is(err, migrate.ErrNoChange) {
		return 0, fmt.Errorf("migrate up: %w", err)
	}
	v, _, err := m.Version()
	if err != nil {
		return 0, fmt.Errorf("migrate version: %w", err)
	}
	return v, nil
}

// MigrationVersion returns the applied version and whether the last migration left the schema dirty.
func MigrationVersion(dir, postgresURL string) (uint, bool, error) {
	m, err := newMigrate(dir, postgresURL)
	if err != nil {
		return 0, false, err
	}
	defer closeMigrate(m)
	v, dirty, err := m.Version()
	if errors.Is(err, migrate.ErrNilVersion) {
		return 0, false, nil
	}
	if err != nil {
		return 0, false, fmt.Errorf("migrate version: %w", err)
	}
	return v, dirty, nil
}

func newMigrate(dir, postgresURL string) (*migrate.Migrate, error) {
	m, err := migrate.New("file://"+dir, pgx5URL(postgresURL))
	if err != nil {
		return nil, fmt.Errorf("open migrations: %w", err)
	}
	return m, nil
}

func closeMigrate(m *migrate.Migrate) { _, _ = m.Close() }

// pgx5URL switches a postgres:// URL to the scheme registered by golang-migrate's pgx/v5 driver.
func pgx5URL(u string) string {
	for _, scheme := range []string{"postgresql://", "postgres://"} {
		if rest, ok := strings.CutPrefix(u, scheme); ok {
			return "pgx5://" + rest
		}
	}
	return u
}
