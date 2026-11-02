package main

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestPipeTokenUsesStdinOnly(t *testing.T) {
	t.Setenv("VYUHA_MASTER_KEY", "master-key-value")
	t.Setenv("ANTHROPIC_API_KEY", "other-secret-value")
	t.Setenv("POSTGRES_URL", "postgres://kept")
	dir := t.TempDir()
	script := `cat > "$1/stdin"; printf '%s' "$*" > "$1/argv"; env > "$1/env"`
	const token = "token-value"
	if err := pipeToken(context.Background(), []string{"sh", "-c", script, "job", dir}, token); err != nil {
		t.Fatal(err)
	}
	read := func(name string) string {
		b, err := os.ReadFile(filepath.Join(dir, name))
		if err != nil {
			t.Fatal(err)
		}
		return string(b)
	}
	if got := read("stdin"); got != token+"\n" {
		t.Errorf("stdin = %q", got)
	}
	if strings.Contains(read("argv"), token) || strings.Contains(read("env"), token) {
		t.Error("token visible in argv or the environment")
	}
	if env := read("env"); strings.Contains(env, "master-key-value") || strings.Contains(env, "other-secret-value") {
		t.Error("the job received a secret from the environment")
	}
	if !strings.Contains(read("env"), "POSTGRES_URL=postgres://kept") {
		t.Error("the job lost a setting it needs")
	}
}
