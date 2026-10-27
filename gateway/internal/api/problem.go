package api

import (
	"encoding/json"
	"net/http"
)

const problemTypeBase = "https://vyuha.dev/errors/"

// writeProblem writes an RFC 9457 application/problem+json response.
// slug becomes both the type URI suffix and the stable machine code.
func writeProblem(w http.ResponseWriter, status int, slug, title, detail string) {
	p := Problem{Type: problemTypeBase + slug, Title: title, Status: status, Code: &slug}
	if detail != "" {
		p.Detail = &detail
	}
	w.Header().Set("Content-Type", "application/problem+json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(p) // the status line is already sent; nothing useful to do on error
}
