package api

import (
	"context"
	"log/slog"
	"net/http"
	"time"

	"github.com/jackc/pgx/v5/pgtype"
	"go.opentelemetry.io/contrib/instrumentation/net/http/otelhttp"

	"github.com/jiteshraj/vyuha/gateway/internal/auth"
	"github.com/jiteshraj/vyuha/gateway/internal/config"
)

// BaseURL is the prefix for every REST route.
const BaseURL = "/api/v1"

// dbTimeout is the default deadline for database calls made by handlers.
const dbTimeout = time.Second

// DB is the subset of store.Queries the handlers use (faked in tests).
type DB interface {
	DatabaseNow(ctx context.Context) (pgtype.Timestamptz, error)
}

// Server implements StrictServerInterface.
type Server struct {
	version string
	env     config.Env
	db      DB
	auth    *auth.Service
	logger  *slog.Logger
}

var _ StrictServerInterface = (*Server)(nil)

// NewServer builds the REST handler implementation. authSvc may be nil in tests that only hit public routes.
func NewServer(version string, env config.Env, db DB, authSvc *auth.Service, logger *slog.Logger) *Server {
	return &Server{version: version, env: env, db: db, auth: authSvc, logger: logger}
}

// NewHandler wires the generated routes, problem+json error handling and middleware.
func NewHandler(s *Server, logger *slog.Logger) http.Handler {
	badRequest := func(w http.ResponseWriter, _ *http.Request, err error) {
		writeProblem(w, http.StatusBadRequest, "BAD_REQUEST", "Invalid request", err.Error())
	}
	strict := NewStrictHandlerWithOptions(s, nil, StrictHTTPServerOptions{
		RequestErrorHandlerFunc: badRequest,
		ResponseErrorHandlerFunc: func(w http.ResponseWriter, r *http.Request, err error) {
			logger.ErrorContext(r.Context(), "handler error", "error", err, "path", r.URL.Path, "request_id", RequestID(r.Context()))
			writeProblem(w, http.StatusInternalServerError, "INTERNAL", "Internal server error", "")
		},
	})

	mux := http.NewServeMux()
	HandlerWithOptions(strict, StdHTTPServerOptions{BaseURL: BaseURL, BaseRouter: mux, ErrorHandlerFunc: badRequest})
	mux.HandleFunc(BaseURL+"/", func(w http.ResponseWriter, _ *http.Request) {
		writeProblem(w, http.StatusNotFound, "NOT_FOUND", "Not found", "")
	})

	var verify func(string) (*auth.Claims, error)
	if s.auth != nil {
		verify = s.auth.VerifyAccess
	}

	var h http.Handler = mux
	h = recoverPanics(logger, h)
	h = authenticate(verify, h)
	h = captureClient(h)
	h = logRequests(logger, h)
	h = requestID(h)
	return otelhttp.NewHandler(h, "gateway")
}
