// Package config loads gateway settings from the environment (see .env.example).
package config

import (
	"errors"
	"fmt"
	"log/slog"
	"net/url"
	"strconv"
	"strings"
	"time"
)

// Env is the deployment environment.
type Env string

const (
	EnvDev     Env = "dev"
	EnvStaging Env = "staging"
	EnvProd    Env = "prod"
)

// devPostgresURL matches deploy/docker-compose.dev.yml; it is only ever used when VYUHA_ENV=dev.
const devPostgresURL = "postgres://vyuha:vyuha@localhost:5432/vyuha?sslmode=disable" //nolint:gosec // G101: well-known dev-only credentials from docker-compose.dev.yml; never used outside VYUHA_ENV=dev

// Config holds gateway settings. It implements slog.LogValuer so it can be logged without secrets.
type Config struct {
	Env                Env
	HTTPAddr           string
	PostgresURL        string
	LogLevel           slog.Level
	OTLPEndpoint       string // empty disables trace export
	LiveTradingEnabled bool

	// Auth. MasterKey is a secret: never log it (LogValue omits it).
	MasterKey      string // VYUHA_MASTER_KEY, base64 32 bytes; required by the gateway, not by every vyuhactl command
	JWTKeyPath     string
	AccessTokenTTL time.Duration
	RefreshTTL     time.Duration
}

// Load reads the configuration through getenv (os.Getenv in production, a map in tests).
func Load(getenv func(string) string) (Config, error) {
	var errs []error
	get := func(key, def string) string {
		if v := strings.TrimSpace(getenv(key)); v != "" {
			return v
		}
		return def
	}

	cfg := Config{
		Env:          Env(get("VYUHA_ENV", string(EnvDev))),
		HTTPAddr:     get("GATEWAY_HTTP_ADDR", "127.0.0.1:8080"),
		OTLPEndpoint: get("OTEL_EXPORTER_OTLP_ENDPOINT", ""),
	}

	switch cfg.Env {
	case EnvDev, EnvStaging, EnvProd:
	default:
		errs = append(errs, fmt.Errorf("VYUHA_ENV must be dev, staging or prod, got %q", cfg.Env))
	}

	cfg.PostgresURL = get("POSTGRES_URL", "")
	if cfg.PostgresURL == "" {
		if cfg.Env == EnvDev {
			cfg.PostgresURL = devPostgresURL
		} else {
			errs = append(errs, errors.New("POSTGRES_URL is required outside dev"))
		}
	}

	cfg.MasterKey = getenv("VYUHA_MASTER_KEY")
	cfg.JWTKeyPath = get("JWT_SIGNING_KEY_PATH", "")
	if cfg.JWTKeyPath == "" {
		if cfg.Env == EnvDev {
			cfg.JWTKeyPath = "./deploy/secrets/jwt_ed25519.pem"
		} else {
			errs = append(errs, errors.New("JWT_SIGNING_KEY_PATH is required outside dev"))
		}
	}
	for _, d := range []struct {
		key string
		def string
		dst *time.Duration
		max time.Duration
	}{
		{"ACCESS_TOKEN_TTL", "15m", &cfg.AccessTokenTTL, time.Hour},
		{"REFRESH_TOKEN_TTL", "168h", &cfg.RefreshTTL, 30 * 24 * time.Hour},
	} {
		v, err := time.ParseDuration(get(d.key, d.def))
		if err != nil || v <= 0 || v > d.max {
			errs = append(errs, fmt.Errorf("%s must be a duration in (0, %s]", d.key, d.max))
			continue
		}
		*d.dst = v
	}

	if err := cfg.LogLevel.UnmarshalText([]byte(get("LOG_LEVEL", "info"))); err != nil {
		errs = append(errs, fmt.Errorf("LOG_LEVEL: %w", err))
	}

	live, err := strconv.ParseBool(get("LIVE_TRADING_ENABLED", "false"))
	if err != nil {
		errs = append(errs, fmt.Errorf("LIVE_TRADING_ENABLED: %w", err))
	}
	cfg.LiveTradingEnabled = live
	// Invariant #3: live trading only when LIVE_TRADING_ENABLED is true AND VYUHA_ENV is staging or prod.
	if live && cfg.Env == EnvDev {
		errs = append(errs, errors.New("LIVE_TRADING_ENABLED must be false when VYUHA_ENV is dev"))
	}

	if err := errors.Join(errs...); err != nil {
		return Config{}, fmt.Errorf("config: %w", err)
	}
	return cfg, nil
}

// LogValue reports the config without credentials: only the Postgres host and database are shown.
func (c Config) LogValue() slog.Value {
	return slog.GroupValue(
		slog.String("env", string(c.Env)),
		slog.String("http_addr", c.HTTPAddr),
		slog.String("postgres", redactURL(c.PostgresURL)),
		slog.String("log_level", c.LogLevel.String()),
		slog.Bool("otlp_export", c.OTLPEndpoint != ""),
		slog.Bool("live_trading_enabled", c.LiveTradingEnabled),
		slog.String("jwt_key_path", c.JWTKeyPath),
		slog.Bool("master_key_set", c.MasterKey != ""),
		slog.Duration("access_token_ttl", c.AccessTokenTTL),
		slog.Duration("refresh_token_ttl", c.RefreshTTL),
	)
}

func redactURL(raw string) string {
	u, err := url.Parse(raw)
	if err != nil {
		return "<unparseable>"
	}
	return u.Host + u.Path
}
