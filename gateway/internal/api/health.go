package api

import (
	"context"
	"time"
)

// GetHealth reports gateway health. Dependency errors are logged, never returned to the caller.
func (s *Server) GetHealth(ctx context.Context, _ GetHealthRequestObject) (GetHealthResponseObject, error) {
	h := Health{
		Status:  HealthStatusOk,
		Version: s.version,
		Env:     HealthEnv(s.env),
		Checks:  map[string]Check{"postgres": s.checkPostgres(ctx)},
	}
	for _, c := range h.Checks {
		if c.Status != CheckStatusOk {
			h.Status = HealthStatusDegraded
		}
	}
	if h.Status != HealthStatusOk {
		return GetHealth503JSONResponse(h), nil
	}
	return GetHealth200JSONResponse(h), nil
}

func (s *Server) checkPostgres(ctx context.Context) Check {
	ctx, cancel := context.WithTimeout(ctx, dbTimeout)
	defer cancel()
	start := time.Now()
	_, err := s.db.DatabaseNow(ctx)
	latency := time.Since(start).Milliseconds()
	if err != nil {
		s.logger.WarnContext(ctx, "health: postgres check failed", "error", err, "request_id", RequestID(ctx))
		reason := "unavailable"
		return Check{Status: CheckStatusFail, LatencyMs: &latency, Error: &reason}
	}
	return Check{Status: CheckStatusOk, LatencyMs: &latency}
}
