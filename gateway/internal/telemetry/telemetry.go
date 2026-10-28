// Package telemetry sets up structured logging (log/slog, JSON, UTC) and OpenTelemetry tracing.
package telemetry

import (
	"context"
	"fmt"
	"io"
	"log/slog"

	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/attribute"
	"go.opentelemetry.io/otel/exporters/otlp/otlptrace/otlptracegrpc"
	"go.opentelemetry.io/otel/propagation"
	"go.opentelemetry.io/otel/sdk/resource"
	sdktrace "go.opentelemetry.io/otel/sdk/trace"
)

// NewLogger returns a JSON logger that writes timestamps in UTC.
func NewLogger(w io.Writer, level slog.Level) *slog.Logger {
	return slog.New(slog.NewJSONHandler(w, &slog.HandlerOptions{
		Level: level,
		ReplaceAttr: func(groups []string, a slog.Attr) slog.Attr {
			if len(groups) == 0 && a.Key == slog.TimeKey && a.Value.Kind() == slog.KindTime {
				a.Value = slog.TimeValue(a.Value.Time().UTC())
			}
			return a
		},
	}))
}

// SetupTracing installs a global tracer provider exporting OTLP/gRPC to endpoint (e.g. http://localhost:4317).
// With an empty endpoint it installs nothing (the OTel default no-op provider stays) and returns a no-op shutdown.
func SetupTracing(ctx context.Context, endpoint, service, version string) (func(context.Context) error, error) {
	otel.SetTextMapPropagator(propagation.TraceContext{})
	if endpoint == "" {
		return func(context.Context) error { return nil }, nil
	}
	exp, err := otlptracegrpc.New(ctx, otlptracegrpc.WithEndpointURL(endpoint))
	if err != nil {
		return nil, fmt.Errorf("otlp exporter: %w", err)
	}
	tp := sdktrace.NewTracerProvider(
		sdktrace.WithBatcher(exp),
		sdktrace.WithResource(resource.NewSchemaless(
			attribute.String("service.name", service),
			attribute.String("service.version", version),
		)),
	)
	otel.SetTracerProvider(tp)
	return tp.Shutdown, nil
}
