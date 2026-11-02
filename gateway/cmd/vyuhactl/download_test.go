package main

import (
	"bytes"
	"context"
	"errors"
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
	script := `cat > "$1/stdin"; printf '%s' "$*" > "$1/argv"; env > "$1/env"; echo job log; echo job error >&2`
	const secret = "token-value"
	token := []byte(secret)
	logs := captureOutput(t, func() {
		if err := pipeToken(context.Background(), []string{"sh", "-c", script, "job", dir}, token); err != nil {
			t.Error(err)
		}
	})
	read := func(name string) string {
		b, err := os.ReadFile(filepath.Join(dir, name))
		if err != nil {
			t.Fatal(err)
		}
		return string(b)
	}
	if got := read("stdin"); got != secret+"\n" {
		t.Errorf("stdin = %q", got)
	}
	if strings.Contains(read("argv"), secret) {
		t.Error("token visible in argv")
	}
	if strings.Contains(read("env"), secret) {
		t.Error("token visible in the environment")
	}
	if !strings.Contains(logs, "job log") || !strings.Contains(logs, "job error") {
		t.Errorf("job output not captured: %q", logs)
	}
	if strings.Contains(logs, secret) {
		t.Error("token visible in the logs")
	}
	if env := read("env"); strings.Contains(env, "master-key-value") || strings.Contains(env, "other-secret-value") {
		t.Error("the job received a secret from the environment")
	}
	if !strings.Contains(read("env"), "POSTGRES_URL=postgres://kept") {
		t.Error("the job lost a setting it needs")
	}
	if !bytes.Equal(token, make([]byte, len(secret))) {
		t.Errorf("token not zeroed: %q", token)
	}
}

func TestTokenIsZeroedWhileTheJobStillRuns(t *testing.T) {
	dir := t.TempDir()
	token := []byte("token-value")
	// The job holds until the test has looked; a real job runs for hours.
	script := `cat > /dev/null; while [ ! -e "$1/seen" ]; do sleep 0.01; done`
	cmd, err := startWithToken(context.Background(), []string{"sh", "-c", script, "job", dir}, token)
	if err != nil {
		t.Fatal(err)
	}
	zeroed := bytes.Equal(token, make([]byte, len(token)))
	if err := os.WriteFile(filepath.Join(dir, "seen"), nil, 0o600); err != nil {
		t.Fatal(err)
	}
	if err := cmd.Wait(); err != nil {
		t.Fatal(err)
	}
	if !zeroed {
		t.Errorf("token still in memory while the job runs: %q", token)
	}
}

func TestJobFailureKeepsTheExitCodeAndHidesTheToken(t *testing.T) {
	for _, tc := range []struct {
		name, script string
		want         int
	}{
		{"incomplete", "cat > /dev/null; exit 3", 3},
		{"failed", "cat > /dev/null; exit 1", 1},
		{"died without reading stdin", "exit 3", 3},
		{"killed", "kill -9 $$", 1},
	} {
		t.Run(tc.name, func(t *testing.T) {
			const secret = "token-value"
			var err error
			logs := captureOutput(t, func() {
				err = pipeToken(context.Background(), []string{"sh", "-c", tc.script}, []byte(secret))
			})
			if err == nil {
				t.Fatal("a failed job returned no error")
			}
			if got := exitCode(err); got != tc.want {
				t.Errorf("exit code = %d, want %d (%v)", got, tc.want, err)
			}
			if strings.Contains(err.Error(), secret) || strings.Contains(logs, secret) {
				t.Error("token visible in the error or the logs")
			}
		})
	}
	if got := exitCode(errors.New("no job ran")); got != 1 {
		t.Errorf("exit code = %d, want 1", got)
	}
}

// captureOutput returns what fn and any job it starts write to stdout and stderr.
func captureOutput(t *testing.T, fn func()) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "output")
	f, err := os.Create(path) //nolint:gosec // a test temp file
	if err != nil {
		t.Fatal(err)
	}
	defer func() { _ = f.Close() }()
	stdout, stderr := os.Stdout, os.Stderr
	os.Stdout, os.Stderr = f, f
	defer func() { os.Stdout, os.Stderr = stdout, stderr }()
	fn()
	b, err := os.ReadFile(path) //nolint:gosec // a test temp file
	if err != nil {
		t.Fatal(err)
	}
	return string(b)
}
