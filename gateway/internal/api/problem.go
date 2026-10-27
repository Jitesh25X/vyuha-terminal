package api

import (
	"encoding/json"
	"net/http"
	"strings"
)

const problemTypeBase = "https://vyuha.dev/errors/"

// problem builds an RFC 9457 body. code is the stable machine code (e.g. AUTH_INVALID); the type URI
// is derived from it (https://vyuha.dev/errors/auth-invalid).
func problem(status int, code, title, detail string) Problem {
	p := Problem{Type: problemTypeBase + strings.ReplaceAll(strings.ToLower(code), "_", "-"), Title: title, Status: status, Code: &code}
	if detail != "" {
		p.Detail = &detail
	}
	return p
}

// writeProblem writes an application/problem+json response outside the generated handlers.
func writeProblem(w http.ResponseWriter, status int, code, title, detail string) {
	w.Header().Set("Content-Type", "application/problem+json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(problem(status, code, title, detail)) // status already sent; nothing to do on error
}
