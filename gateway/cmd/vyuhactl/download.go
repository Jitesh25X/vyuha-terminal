package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"os"
	"os/exec"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/brokers"
	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/config"
	vcrypto "github.com/Jitesh25X/vyuha-terminal/gateway/internal/crypto"
	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/store"
)

func runDownload(args []string) error {
	fs := flag.NewFlagSet("download run", flag.ContinueOnError)
	account := fs.String("account", "", "account id, or broker:client_code")
	username := fs.String("user", "", "username requesting the job")
	spec := fs.String("spec", "", "job file, e.g. quant/jobs/first_1minute.json")
	quant := fs.String("quant", "uv run --project quant vyuha-quant", "command that runs the quant CLI")
	if err := fs.Parse(args); err != nil {
		return err
	}
	if *account == "" || *username == "" || *spec == "" {
		return errors.New("-account, -user and -spec are required")
	}
	cfg, err := config.Load(os.Getenv)
	if err != nil {
		return err
	}
	env, err := vcrypto.NewEnvelope(cfg.MasterKey)
	if err != nil {
		return err
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	pool, err := store.NewPool(ctx, cfg.PostgresURL)
	if err != nil {
		return err
	}
	id, token, err := brokers.OpenAccessToken(ctx, pool, env, *account, *username, "historical_download", time.Now())
	pool.Close()
	if err != nil {
		return err
	}
	run, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	argv := append(strings.Fields(*quant), "download", "run", "--spec", *spec, "--account", id, "--user", *username)
	return pipeToken(run, argv, token)
}

// pipeToken runs a job with the token on its stdin: argv and the environment are visible to other
// processes. The job gets only the settings it needs, never VYUHA_MASTER_KEY or other .env secrets.
func pipeToken(ctx context.Context, argv []string, token string) error {
	cmd := exec.CommandContext(ctx, argv[0], argv[1:]...) //nolint:gosec // the operator's own command line
	cmd.Stdin = strings.NewReader(token + "\n")
	cmd.Stdout, cmd.Stderr = os.Stdout, os.Stderr
	cmd.Env = jobEnv(os.Environ())
	cmd.Cancel = func() error { return cmd.Process.Signal(syscall.SIGTERM) }
	cmd.WaitDelay = 10 * time.Second
	if err := cmd.Run(); err != nil {
		return fmt.Errorf("download job: %w", err)
	}
	return nil
}

// jobEnvPrefixes is everything a download job may see of this process's environment.
var jobEnvPrefixes = []string{
	"PATH=", "HOME=", "USER=", "TMPDIR=", "LANG=", "LC_", "TZ=", "UV_", "VIRTUAL_ENV=",
	"VYUHA_ENV=", "POSTGRES_URL=", "DATA_LAKE_DIR=", "RISK_FREE_", "IV_", "UPSTOX_API_BASE=", "DOWNLOAD_PACE_S=",
}

func jobEnv(environ []string) []string {
	out := make([]string, 0, len(environ))
	for _, e := range environ {
		for _, p := range jobEnvPrefixes {
			if strings.HasPrefix(e, p) {
				out = append(out, e)
				break
			}
		}
	}
	return out
}
