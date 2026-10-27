package config

import (
	"bytes"
	"log/slog"
	"strings"
	"testing"
	"time"
)

func TestLoad(t *testing.T) {
	tests := []struct {
		name    string
		env     map[string]string
		wantErr string
		check   func(t *testing.T, c Config)
	}{
		{
			name: "dev defaults",
			env:  map[string]string{},
			check: func(t *testing.T, c Config) {
				if c.Env != EnvDev || c.HTTPAddr != "127.0.0.1:8080" || c.PostgresURL != devPostgresURL {
					t.Fatalf("unexpected defaults: %+v", c)
				}
				if c.LiveTradingEnabled || c.OTLPEndpoint != "" || c.LogLevel != slog.LevelInfo {
					t.Fatalf("unexpected defaults: %+v", c)
				}
				if c.JWTKeyPath != "./deploy/secrets/jwt_ed25519.pem" || c.AccessTokenTTL != 15*time.Minute || c.RefreshTTL != 168*time.Hour {
					t.Fatalf("unexpected auth defaults: %+v", c)
				}
			},
		},
		{
			name: "explicit values",
			env: map[string]string{
				"VYUHA_ENV": "staging", "POSTGRES_URL": "postgres://u:p@db:5432/v", "GATEWAY_HTTP_ADDR": ":8080",
				"LOG_LEVEL": "debug", "OTEL_EXPORTER_OTLP_ENDPOINT": "http://otel:4317", "LIVE_TRADING_ENABLED": "true",
				"JWT_SIGNING_KEY_PATH": "/run/secrets/jwt.pem", "ACCESS_TOKEN_TTL": "10m",
			},
			check: func(t *testing.T, c Config) {
				if c.Env != EnvStaging || c.HTTPAddr != ":8080" || c.LogLevel != slog.LevelDebug || !c.LiveTradingEnabled {
					t.Fatalf("unexpected config: %+v", c)
				}
			},
		},
		{name: "unknown env", env: map[string]string{"VYUHA_ENV": "production"}, wantErr: "VYUHA_ENV"},
		{name: "prod needs postgres url", env: map[string]string{"VYUHA_ENV": "prod"}, wantErr: "POSTGRES_URL is required"},
		{name: "live trading refused in dev", env: map[string]string{"LIVE_TRADING_ENABLED": "true"}, wantErr: "must be false when VYUHA_ENV is dev"},
		{name: "live trading flag must parse", env: map[string]string{"LIVE_TRADING_ENABLED": "maybe"}, wantErr: "LIVE_TRADING_ENABLED"},
		{name: "bad log level", env: map[string]string{"LOG_LEVEL": "loud"}, wantErr: "LOG_LEVEL"},
		{name: "staging needs jwt key path", env: map[string]string{"VYUHA_ENV": "staging", "POSTGRES_URL": "postgres://x"}, wantErr: "JWT_SIGNING_KEY_PATH"},
		{name: "access ttl capped", env: map[string]string{"ACCESS_TOKEN_TTL": "24h"}, wantErr: "ACCESS_TOKEN_TTL"},
		{name: "refresh ttl must parse", env: map[string]string{"REFRESH_TOKEN_TTL": "a week"}, wantErr: "REFRESH_TOKEN_TTL"},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			c, err := Load(func(k string) string { return tt.env[k] })
			if tt.wantErr != "" {
				if err == nil || !strings.Contains(err.Error(), tt.wantErr) {
					t.Fatalf("want error containing %q, got %v", tt.wantErr, err)
				}
				return
			}
			if err != nil {
				t.Fatalf("unexpected error: %v", err)
			}
			tt.check(t, c)
		})
	}
}

func TestLogValueHidesCredentials(t *testing.T) {
	c, err := Load(func(k string) string {
		return map[string]string{"POSTGRES_URL": "postgres://vyuha:s3cret-pw@db.internal:5432/vyuha", "VYUHA_MASTER_KEY": "TUFTVEVSLUtFWS1TRUNSRVQ="}[k]
	})
	if err != nil {
		t.Fatal(err)
	}
	var buf bytes.Buffer
	slog.New(slog.NewJSONHandler(&buf, nil)).Info("cfg", "config", c)
	out := buf.String()
	if strings.Contains(out, "s3cret-pw") || strings.Contains(out, "vyuha:") || strings.Contains(out, "TUFTVEVSLUtFWS1TRUNSRVQ") {
		t.Fatalf("credentials leaked into log: %s", out)
	}
	if !strings.Contains(out, "db.internal:5432/vyuha") {
		t.Fatalf("expected redacted host/db in log: %s", out)
	}
}
