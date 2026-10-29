package api

import (
	"context"
	"net/http"
)

// ConstituentSource provides index membership with weights and sectors (reference data for MOV/HM).
// The graph seed (M6.1) implements it; it returns nil for an index it doesn't know.
type ConstituentSource interface {
	Constituents(ctx context.Context, indexID int64) (*IndexConstituents, error)
}

// WithConstituents enables GET /indices/{id}/constituents with a data source.
func (s *Server) WithConstituents(src ConstituentSource) *Server {
	s.constituents = src
	return s
}

// noConstituents is the answer while no source is configured (until M6.1) or the index is unknown.
func noConstituents(detail string) GetIndexConstituentsResponseObject {
	return GetIndexConstituents404ApplicationProblemPlusJSONResponse{NotFoundApplicationProblemPlusJSONResponse(
		problem(http.StatusNotFound, "NO_CONSTITUENTS", "No constituent data for this index", detail))}
}

// GetIndexConstituents returns an index's members. Reference data, so no market-data entitlement check:
// prices for them arrive over the WebSocket under the usual rules.
func (s *Server) GetIndexConstituents(ctx context.Context, req GetIndexConstituentsRequestObject) (GetIndexConstituentsResponseObject, error) {
	if s.constituents == nil {
		return noConstituents("index constituents arrive with the graph seed (M6.1)"), nil
	}
	ctx, cancel := context.WithTimeout(ctx, dbTimeout)
	defer cancel()
	ic, err := s.constituents.Constituents(ctx, req.Id)
	if err != nil {
		return nil, err
	}
	if ic == nil {
		return noConstituents(""), nil
	}
	return GetIndexConstituents200JSONResponse(*ic), nil
}
