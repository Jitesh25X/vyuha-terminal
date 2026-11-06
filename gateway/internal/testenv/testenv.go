// Package testenv decides what a test does when a service it needs isn't there.
package testenv

import (
	"os"
	"testing"
)

// SkipWithoutService skips the test, for quick local runs. With VYUHA_REQUIRE_DB=1 (CI and `make verify`) it fails
// instead, so a service being down can never look like a pass.
func SkipWithoutService(t testing.TB, message string) {
	t.Helper()
	if os.Getenv("VYUHA_REQUIRE_DB") == "1" {
		t.Fatal(message + " and VYUHA_REQUIRE_DB=1: it must not skip")
	}
	t.Skip(message)
}
