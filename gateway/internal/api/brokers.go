package api

import (
	"context"
	"errors"
	"net/http"
	"net/url"

	"github.com/google/uuid"

	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/brokers"
)

// terminalPath is where broker callbacks send the browser back to.
const terminalPath = "/terminal"

// GetBrokerLoginUrl returns the daily broker login URL (admin, or a mapped trader with can_trade).
func (s *Server) GetBrokerLoginUrl(ctx context.Context, req GetBrokerLoginUrlRequestObject) (GetBrokerLoginUrlResponseObject, error) {
	if s.brokers == nil {
		return nil, errors.New("broker login not configured")
	}
	claims := ClaimsFrom(ctx)
	u, err := s.brokers.LoginURL(ctx, claims.Subject, req.AccountId.String())
	switch {
	case err == nil:
		return GetBrokerLoginUrl200JSONResponse{Url: u}, nil
	case errors.Is(err, brokers.ErrNotFound):
		return GetBrokerLoginUrl404ApplicationProblemPlusJSONResponse{NotFoundApplicationProblemPlusJSONResponse(
			problem(http.StatusNotFound, "NOT_FOUND", "Broker account not found", ""))}, nil
	case errors.Is(err, brokers.ErrForbidden):
		return GetBrokerLoginUrl403ApplicationProblemPlusJSONResponse{ForbiddenApplicationProblemPlusJSONResponse(
			problem(http.StatusForbidden, "FORBIDDEN", "Not allowed to log in this broker account", ""))}, nil
	case errors.Is(err, brokers.ErrNoLogin):
		return GetBrokerLoginUrl409ApplicationProblemPlusJSONResponse(
			problem(http.StatusConflict, "BROKER_NO_LOGIN", "This account has no broker login", "")), nil
	}
	return nil, err
}

// ListBrokerAccounts lists the broker accounts the user can see, with login status.
func (s *Server) ListBrokerAccounts(ctx context.Context, _ ListBrokerAccountsRequestObject) (ListBrokerAccountsResponseObject, error) {
	if s.brokers == nil {
		return nil, errors.New("broker login not configured")
	}
	accounts, err := s.brokers.Accounts(ctx, ClaimsFrom(ctx).Subject)
	if errors.Is(err, brokers.ErrForbidden) {
		return ListBrokerAccounts200JSONResponse{}, nil // inactive user: nothing to show
	} else if err != nil {
		return nil, err
	}
	out := make(ListBrokerAccounts200JSONResponse, len(accounts))
	for i, a := range accounts {
		id, err := uuid.Parse(a.ID)
		if err != nil {
			return nil, err
		}
		out[i] = BrokerAccount{
			Id: id, Broker: BrokerAccountBroker(a.Broker), ClientCode: a.ClientCode, HolderName: a.HolderName,
			Status: BrokerAccountStatus(a.Status), DataEntitled: a.DataEntitled, CanLogin: a.CanLogin, TokenExpiresAt: a.TokenExpiresAt,
		}
	}
	return out, nil
}

// BrokerCallbackZerodha completes a Kite Connect login.
func (s *Server) BrokerCallbackZerodha(ctx context.Context, req BrokerCallbackZerodhaRequestObject) (BrokerCallbackZerodhaResponseObject, error) {
	p := brokers.CallbackParams{State: deref(req.Params.State), Status: deref(req.Params.Status), RequestToken: deref(req.Params.RequestToken)}
	return s.brokerCallback(ctx, brokers.Zerodha, p), nil
}

// BrokerCallbackUpstox completes an Upstox OAuth login.
func (s *Server) BrokerCallbackUpstox(ctx context.Context, req BrokerCallbackUpstoxRequestObject) (BrokerCallbackUpstoxResponseObject, error) {
	p := brokers.CallbackParams{State: deref(req.Params.State), Code: deref(req.Params.Code)}
	return s.brokerCallback(ctx, brokers.Upstox, p), nil
}

func (s *Server) brokerCallback(ctx context.Context, broker string, p brokers.CallbackParams) BrokerLoginRedirectResponse {
	q := url.Values{}
	if s.brokers == nil {
		q.Set("broker_login", "error")
		q.Set("code", brokers.CodeInternal)
	} else {
		account, err := s.brokers.Callback(ctx, broker, p, clientMeta(ctx).IP)
		var le *brokers.LoginError
		switch {
		case err == nil:
			q.Set("broker_login", "ok")
			q.Set("account", account)
		case errors.As(err, &le):
			q.Set("broker_login", "error")
			q.Set("code", le.Code)
		default:
			q.Set("broker_login", "error")
			q.Set("code", brokers.CodeInternal)
		}
	}
	loc := terminalPath + "?" + q.Encode()
	return BrokerLoginRedirectResponse{Headers: BrokerLoginRedirectResponseHeaders{Location: &loc}}
}

func deref(p *string) string {
	if p == nil {
		return ""
	}
	return *p
}

// noStoreBrokerRoutes keeps broker URLs (which carry one-time tokens) out of caches and Referer headers.
func noStoreBrokerRoutes(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if len(r.URL.Path) >= len(BaseURL+"/brokers/") && r.URL.Path[:len(BaseURL+"/brokers/")] == BaseURL+"/brokers/" {
			w.Header().Set("Cache-Control", "no-store")
			w.Header().Set("Referrer-Policy", "no-referrer")
		}
		next.ServeHTTP(w, r)
	})
}
