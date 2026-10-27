package api

import (
	"context"
	"encoding/base64"
	"errors"
	"net/http"
	"time"

	"github.com/jiteshraj/vyuha/gateway/internal/auth"
)

const (
	refreshCookieName = "vyuha_rt"
	refreshCookiePath = BaseURL + "/auth"
)

// authProblem classifies an auth error. ok=false means it is unexpected and becomes a 500.
type authProblemKind int

const (
	kindUnauthorized authProblemKind = iota
	kindLocked
	kindConflict
)

func classify(err error) (authProblemKind, Problem, bool) {
	var le *auth.LockedError
	switch {
	case errors.As(err, &le):
		p := problem(http.StatusLocked, "AUTH_LOCKED", "Account locked", "Too many failed attempts; try again later.")
		until := le.Until.UTC()
		p.LockedUntil = &until
		return kindLocked, p, true
	case errors.Is(err, auth.ErrInvalidCredentials):
		return kindUnauthorized, problem(http.StatusUnauthorized, "AUTH_INVALID", "Invalid credentials", ""), true
	case errors.Is(err, auth.ErrUnauthenticated):
		return kindUnauthorized, problem(http.StatusUnauthorized, "AUTH_UNAUTHENTICATED", "Login expired or invalid; start again", ""), true
	case errors.Is(err, auth.ErrMFANotEnrolled):
		return kindConflict, problem(http.StatusConflict, "AUTH_MFA_NOT_ENROLLED", "TOTP enrollment required", ""), true
	case errors.Is(err, auth.ErrMFAAlreadyEnrolled):
		return kindConflict, problem(http.StatusConflict, "AUTH_MFA_ALREADY_ENROLLED", "TOTP already enrolled", ""), true
	case errors.Is(err, auth.ErrEnrollNotStarted):
		return kindConflict, problem(http.StatusConflict, "AUTH_MFA_ENROLL_NOT_STARTED", "Start TOTP enrollment first", ""), true
	}
	return 0, Problem{}, false
}

// Login: password step.
func (s *Server) Login(ctx context.Context, req LoginRequestObject) (LoginResponseObject, error) {
	res, err := s.auth.Login(ctx, req.Body.Username, req.Body.Password, clientMeta(ctx))
	if err == nil {
		return Login200JSONResponse{MfaToken: res.MFAToken, MfaEnrolled: res.MFAEnrolled}, nil
	}
	kind, p, ok := classify(err)
	switch {
	case ok && kind == kindLocked:
		return Login423ApplicationProblemPlusJSONResponse{LockedApplicationProblemPlusJSONResponse(p)}, nil
	case ok && kind == kindUnauthorized:
		return Login401ApplicationProblemPlusJSONResponse{UnauthorizedApplicationProblemPlusJSONResponse(p)}, nil
	}
	return nil, err
}

// VerifyMfa: TOTP step for enrolled users.
func (s *Server) VerifyMfa(ctx context.Context, req VerifyMfaRequestObject) (VerifyMfaResponseObject, error) {
	sess, err := s.auth.VerifyMFA(ctx, req.Body.MfaToken, req.Body.Totp, clientMeta(ctx))
	if err == nil {
		return VerifyMfa200JSONResponse{sessionStarted(sess)}, nil
	}
	kind, p, ok := classify(err)
	switch {
	case ok && kind == kindLocked:
		return VerifyMfa423ApplicationProblemPlusJSONResponse{LockedApplicationProblemPlusJSONResponse(p)}, nil
	case ok && kind == kindConflict:
		return VerifyMfa409ApplicationProblemPlusJSONResponse{ConflictApplicationProblemPlusJSONResponse(p)}, nil
	case ok:
		return VerifyMfa401ApplicationProblemPlusJSONResponse{UnauthorizedApplicationProblemPlusJSONResponse(p)}, nil
	}
	return nil, err
}

// BeginMfaEnroll: first-login TOTP secret.
func (s *Server) BeginMfaEnroll(ctx context.Context, req BeginMfaEnrollRequestObject) (BeginMfaEnrollResponseObject, error) {
	e, err := s.auth.BeginEnroll(ctx, req.Body.MfaToken, clientMeta(ctx))
	if err == nil {
		return BeginMfaEnroll200JSONResponse{OtpauthUrl: e.OtpauthURL, QrPngBase64: base64.StdEncoding.EncodeToString(e.QRPNG)}, nil
	}
	kind, p, ok := classify(err)
	switch {
	case ok && kind == kindLocked:
		return BeginMfaEnroll423ApplicationProblemPlusJSONResponse{LockedApplicationProblemPlusJSONResponse(p)}, nil
	case ok && kind == kindConflict:
		return BeginMfaEnroll409ApplicationProblemPlusJSONResponse{ConflictApplicationProblemPlusJSONResponse(p)}, nil
	case ok:
		return BeginMfaEnroll401ApplicationProblemPlusJSONResponse{UnauthorizedApplicationProblemPlusJSONResponse(p)}, nil
	}
	return nil, err
}

// ConfirmMfaEnroll: first code from the new authenticator; starts a session.
func (s *Server) ConfirmMfaEnroll(ctx context.Context, req ConfirmMfaEnrollRequestObject) (ConfirmMfaEnrollResponseObject, error) {
	sess, err := s.auth.ConfirmEnroll(ctx, req.Body.MfaToken, req.Body.Totp, clientMeta(ctx))
	if err == nil {
		return ConfirmMfaEnroll200JSONResponse{sessionStarted(sess)}, nil
	}
	kind, p, ok := classify(err)
	switch {
	case ok && kind == kindLocked:
		return ConfirmMfaEnroll423ApplicationProblemPlusJSONResponse{LockedApplicationProblemPlusJSONResponse(p)}, nil
	case ok && kind == kindConflict:
		return ConfirmMfaEnroll409ApplicationProblemPlusJSONResponse{ConflictApplicationProblemPlusJSONResponse(p)}, nil
	case ok:
		return ConfirmMfaEnroll401ApplicationProblemPlusJSONResponse{UnauthorizedApplicationProblemPlusJSONResponse(p)}, nil
	}
	return nil, err
}

// RefreshSession rotates the refresh cookie. Every failure clears the cookie.
func (s *Server) RefreshSession(ctx context.Context, req RefreshSessionRequestObject) (RefreshSessionResponseObject, error) {
	raw := ""
	if req.Params.VyuhaRt != nil {
		raw = *req.Params.VyuhaRt
	}
	sess, err := s.auth.Refresh(ctx, raw, clientMeta(ctx))
	if err == nil {
		return RefreshSession200JSONResponse{sessionStarted(sess)}, nil
	}
	var p Problem
	switch {
	case errors.Is(err, auth.ErrRefreshReused):
		s.logger.WarnContext(ctx, "auth: refresh token reuse detected; session revoked", "request_id", RequestID(ctx))
		p = problem(http.StatusUnauthorized, "AUTH_REFRESH_REUSED", "Session revoked", "This session was ended for safety; log in again.")
	case errors.Is(err, auth.ErrRefreshInvalid):
		p = problem(http.StatusUnauthorized, "AUTH_REFRESH_INVALID", "Session expired", "Log in again.")
	default:
		return nil, err
	}
	clear := clearRefreshCookie()
	return RefreshSession401ApplicationProblemPlusJSONResponse{Body: p, Headers: RefreshSession401ResponseHeaders{SetCookie: clear}}, nil
}

// Logout revokes the session; always clears the cookie.
func (s *Server) Logout(ctx context.Context, req LogoutRequestObject) (LogoutResponseObject, error) {
	if req.Params.VyuhaRt != nil {
		if err := s.auth.Logout(ctx, *req.Params.VyuhaRt, clientMeta(ctx)); err != nil {
			return nil, err
		}
	}
	return Logout204Response{Headers: Logout204ResponseHeaders{SetCookie: clearRefreshCookie()}}, nil
}

// Reauth: password + TOTP again for sensitive actions (bearer required).
func (s *Server) Reauth(ctx context.Context, req ReauthRequestObject) (ReauthResponseObject, error) {
	c := ClaimsFrom(ctx)
	if c == nil {
		return Reauth401ApplicationProblemPlusJSONResponse{UnauthorizedApplicationProblemPlusJSONResponse(
			problem(http.StatusUnauthorized, "AUTH_UNAUTHENTICATED", "Authentication required", ""))}, nil
	}
	tok, ttl, err := s.auth.Reauth(ctx, c.Subject, req.Body.Password, req.Body.Totp, clientMeta(ctx))
	if err == nil {
		return Reauth200JSONResponse{ReauthToken: tok, ExpiresIn: int(ttl / time.Second)}, nil
	}
	kind, p, ok := classify(err)
	switch {
	case ok && kind == kindLocked:
		return Reauth423ApplicationProblemPlusJSONResponse{LockedApplicationProblemPlusJSONResponse(p)}, nil
	case ok:
		return Reauth401ApplicationProblemPlusJSONResponse{UnauthorizedApplicationProblemPlusJSONResponse(p)}, nil
	}
	return nil, err
}

func sessionStarted(sess auth.Session) SessionStartedJSONResponse {
	var id User
	_ = id.Id.UnmarshalText([]byte(sess.User.ID)) // IDs come from Postgres uuid columns
	cookie := (&http.Cookie{
		Name: refreshCookieName, Value: sess.RefreshToken, Path: refreshCookiePath,
		MaxAge:   int(sess.RefreshMaxAge / time.Second),
		HttpOnly: true, Secure: true, SameSite: http.SameSiteStrictMode,
	}).String()
	return SessionStartedJSONResponse{
		Body: SessionResponse{
			AccessToken: sess.AccessToken,
			ExpiresIn:   int(sess.AccessExpiresIn / time.Second),
			User: User{Id: id.Id, Username: sess.User.Username, DisplayName: sess.User.DisplayName,
				Role: UserRole(sess.User.Role)},
		},
		Headers: SessionStartedResponseHeaders{SetCookie: &cookie},
	}
}

func clearRefreshCookie() *string {
	c := (&http.Cookie{
		Name: refreshCookieName, Value: "", Path: refreshCookiePath, MaxAge: -1,
		HttpOnly: true, Secure: true, SameSite: http.SameSiteStrictMode,
	}).String()
	return &c
}
