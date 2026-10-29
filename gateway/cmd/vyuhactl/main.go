// Command vyuhactl is the admin CLI: migrations, users, broker accounts, dev secrets and audit
// verification (kill / sessions arrive with their tasks).
package main

import (
	"bufio"
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/x509"
	"encoding/base64"
	"encoding/hex"
	"encoding/pem"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"

	"github.com/jiteshraj/vyuha/gateway/internal/audit"
	"github.com/jiteshraj/vyuha/gateway/internal/auth"
	"github.com/jiteshraj/vyuha/gateway/internal/config"
	"github.com/jiteshraj/vyuha/gateway/internal/store"
)

const usage = `usage:
  vyuhactl migrate up|version [-dir ../db/migrations]
  vyuhactl user create -username NAME -display-name "Full Name" -role admin|risk_manager|trader|analyst
      (password is read from the terminal with echo off, or from stdin when piped; never from a flag)
  vyuhactl secrets init [-env-file ../.env] [-env-example ../.env.example] [-jwt-key ../deploy/secrets/jwt_ed25519.pem]
      (creates missing dev secrets; never prints them; never overwrites an existing key)
  vyuhactl audit verify [-anchor ID:HEXHASH ...]
      (recomputes the audit hash chain; exit 1 on tampering. Anchors from 'audit head' catch deleted newest rows)
  vyuhactl audit head
      (prints the newest ID:HEXHASH; store it outside the database, e.g. in the daily compliance report)
  vyuhactl account create -broker zerodha|upstox|sim -client-code ID -holder NAME -static-ip IP
      [-public-ip IP] [-backup-ip IP] [-data-entitled] [-ops-cap 8]
      (API key and secret are read from the terminal with echo off, or two lines of piped stdin; stored encrypted)
  vyuhactl account map -account ID|broker:client_code -user USERNAME [-trade] [-default]
Reads POSTGRES_URL etc. from the environment (VYUHA_ENV=dev defaults apply).`

// errUsage makes main print the usage text.
var errUsage = errors.New("invalid arguments")

func main() {
	if err := run(os.Args[1:]); err != nil {
		if errors.Is(err, errUsage) {
			fmt.Fprintln(os.Stderr, usage)
		}
		fmt.Fprintln(os.Stderr, "vyuhactl:", err)
		os.Exit(1)
	}
}

func run(args []string) error {
	if len(args) < 2 {
		return errUsage
	}
	switch args[0] + " " + args[1] {
	case "migrate up", "migrate version":
		return runMigrate(args[1], args[2:])
	case "user create":
		return runUserCreate(args[2:])
	case "secrets init":
		return runSecretsInit(args[2:])
	case "audit verify", "audit head":
		return runAudit(args[1], args[2:])
	case "account create":
		return runAccountCreate(args[2:])
	case "account map":
		return runAccountMap(args[2:])
	}
	return errUsage
}

func runMigrate(cmd string, args []string) error {
	fs := flag.NewFlagSet("migrate", flag.ContinueOnError)
	dir := fs.String("dir", "../db/migrations", "directory holding NNNNNN_name.{up,down}.sql files")
	if err := fs.Parse(args); err != nil {
		return err
	}
	cfg, err := config.Load(os.Getenv)
	if err != nil {
		return err
	}
	if cmd == "up" {
		v, err := store.MigrateUp(*dir, cfg.PostgresURL)
		if err != nil {
			return err
		}
		fmt.Printf("✔ schema version %d\n", v)
		return nil
	}
	v, dirty, err := store.MigrationVersion(*dir, cfg.PostgresURL)
	if err != nil {
		return err
	}
	fmt.Printf("schema version %d (dirty=%t)\n", v, dirty)
	return nil
}

func runUserCreate(args []string) error {
	fs := flag.NewFlagSet("user create", flag.ContinueOnError)
	username := fs.String("username", "", "lowercase login name, 3–32 of [a-z0-9_.]")
	display := fs.String("display-name", "", "display name")
	role := fs.String("role", "", "admin | risk_manager | trader | analyst")
	if err := fs.Parse(args); err != nil {
		return err
	}
	if *username == "" || *display == "" || *role == "" {
		return errors.New("-username, -display-name and -role are required")
	}
	cfg, err := config.Load(os.Getenv)
	if err != nil {
		return err
	}
	password, err := readPassword()
	if err != nil {
		return err
	}

	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	pool, err := store.NewPool(ctx, cfg.PostgresURL)
	if err != nil {
		return err
	}
	defer pool.Close()
	// CreateUser only hashes and inserts: no envelope or signer is needed here.
	svc, err := auth.NewService(pool, nil, nil, auth.DefaultConfig(), time.Now)
	if err != nil {
		return err
	}
	id, err := svc.CreateUser(ctx, auth.SystemActor, *username, *display, store.UserRole(*role), password)
	if err != nil {
		return err
	}
	fmt.Printf("✔ created %s (%s) id=%s; TOTP is enrolled at first login\n", *username, *role, id)
	return nil
}

// readPassword prompts twice with echo off on a terminal, or reads one line from piped stdin.
func readPassword() (string, error) {
	fi, err := os.Stdin.Stat()
	if err != nil {
		return "", err
	}
	if fi.Mode()&os.ModeCharDevice == 0 {
		line, err := bufio.NewReader(os.Stdin).ReadString('\n')
		if err != nil && !errors.Is(err, io.EOF) {
			return "", err
		}
		return strings.TrimRight(line, "\r\n"), nil
	}
	first, err := promptNoEcho(fmt.Sprintf("Password (%d–%d chars): ", auth.MinPasswordLen, auth.MaxPasswordLen))
	if err != nil {
		return "", err
	}
	second, err := promptNoEcho("Repeat password: ")
	if err != nil {
		return "", err
	}
	if first != second {
		return "", errors.New("passwords do not match")
	}
	return first, nil
}

func promptNoEcho(prompt string) (string, error) {
	fmt.Fprint(os.Stderr, prompt)
	if err := stty(false); err != nil {
		return "", fmt.Errorf("disable echo: %w", err)
	}
	defer func() { _ = stty(true); fmt.Fprintln(os.Stderr) }()
	line, err := bufio.NewReader(os.Stdin).ReadString('\n')
	if err != nil && !errors.Is(err, io.EOF) {
		return "", err
	}
	return strings.TrimRight(line, "\r\n"), nil
}

func stty(echo bool) error {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, "stty", "-echo")
	if echo {
		cmd = exec.CommandContext(ctx, "stty", "echo")
	}
	cmd.Stdin = os.Stdin
	return cmd.Run()
}

func runSecretsInit(args []string) error {
	fs := flag.NewFlagSet("secrets init", flag.ContinueOnError)
	envFile := fs.String("env-file", "../.env", "dotenv file to create or complete")
	envExample := fs.String("env-example", "../.env.example", "template used when -env-file does not exist")
	jwtKey := fs.String("jwt-key", "../deploy/secrets/jwt_ed25519.pem", "Ed25519 PKCS#8 PEM to create if missing")
	if err := fs.Parse(args); err != nil {
		return err
	}
	if err := ensureJWTKey(*jwtKey); err != nil {
		return err
	}
	return ensureMasterKey(*envFile, *envExample)
}

func ensureJWTKey(path string) error {
	if _, err := os.Stat(path); err == nil {
		fmt.Printf("· %s exists; left unchanged\n", path)
		return nil
	}
	_, priv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		return err
	}
	der, err := x509.MarshalPKCS8PrivateKey(priv)
	if err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		return err
	}
	// Paths below come from the operator's own -jwt-key / -env-file flags on an admin CLI.
	f, err := os.OpenFile(filepath.Clean(path), os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o600) //nolint:gosec // G304: operator-supplied path
	if err != nil {
		return err
	}
	if err := pem.Encode(f, &pem.Block{Type: "PRIVATE KEY", Bytes: der}); err != nil {
		_ = f.Close()
		return err
	}
	if err := f.Close(); err != nil {
		return fmt.Errorf("write %s: %w", path, err)
	}
	fmt.Printf("✔ wrote JWT signing key %s (0600)\n", path)
	return nil
}

// ensureMasterKey creates envFile from the example if needed and fills an empty VYUHA_MASTER_KEY.
// An existing non-empty key is never replaced: TOTP secrets are encrypted under it.
func ensureMasterKey(envFile, example string) error {
	content, err := os.ReadFile(filepath.Clean(envFile)) //nolint:gosec // G304: operator-supplied path
	created := false
	if errors.Is(err, os.ErrNotExist) {
		if content, err = os.ReadFile(filepath.Clean(example)); err != nil { //nolint:gosec // G304: operator-supplied path
			return fmt.Errorf("read %s: %w", example, err)
		}
		created = true
	} else if err != nil {
		return err
	}

	lines := strings.Split(string(content), "\n")
	found, filled := false, false
	for i, l := range lines {
		rest, ok := strings.CutPrefix(l, "VYUHA_MASTER_KEY=")
		if !ok {
			continue
		}
		found = true
		if strings.TrimSpace(strings.SplitN(rest, "#", 2)[0]) != "" {
			break
		}
		key := make([]byte, 32)
		if _, err := rand.Read(key); err != nil {
			return err
		}
		lines[i] = "VYUHA_MASTER_KEY=" + base64.StdEncoding.EncodeToString(key)
		filled = true
		break
	}
	if !found {
		return fmt.Errorf("%s has no VYUHA_MASTER_KEY= line", envFile)
	}
	if !created && !filled {
		fmt.Printf("· %s already has VYUHA_MASTER_KEY; left unchanged\n", envFile)
		return nil
	}
	if err := os.WriteFile(filepath.Clean(envFile), []byte(strings.Join(lines, "\n")), 0o600); err != nil { //nolint:gosec // G304/G703: operator-supplied path
		return err
	}
	if err := os.Chmod(envFile, 0o600); err != nil {
		return err
	}
	fmt.Printf("✔ %s: VYUHA_MASTER_KEY generated (0600; back it up — TOTP secrets are encrypted with it)\n", envFile)
	return nil
}

type anchorFlags []audit.Anchor

func (a *anchorFlags) String() string { return fmt.Sprint(len(*a)) }

func (a *anchorFlags) Set(v string) error {
	an, err := audit.ParseAnchor(v)
	if err != nil {
		return err
	}
	*a = append(*a, an)
	return nil
}

func runAudit(cmd string, args []string) error {
	fs := flag.NewFlagSet("audit "+cmd, flag.ContinueOnError)
	var anchors anchorFlags
	if cmd == "verify" {
		fs.Var(&anchors, "anchor", "ID:HEXHASH from an earlier 'audit head' (repeatable)")
	}
	if err := fs.Parse(args); err != nil {
		return err
	}
	cfg, err := config.Load(os.Getenv)
	if err != nil {
		return err
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Minute)
	defer cancel()
	pool, err := store.NewPool(ctx, cfg.PostgresURL)
	if err != nil {
		return err
	}
	defer pool.Close()

	res, err := audit.Verify(ctx, pool, anchors...)
	if err != nil {
		return err
	}
	if cmd == "head" {
		if res.RowsChecked == 0 {
			fmt.Println("audit log is empty")
			return nil
		}
		fmt.Printf("%d:%s\n", res.HeadID, hex.EncodeToString(res.HeadHash))
		if !res.OK {
			return fmt.Errorf("chain is NOT intact (first bad id %d: %s); do not use this head as an anchor", res.FirstBadID, res.Reason)
		}
		return nil
	}
	if !res.OK {
		return fmt.Errorf("✖ audit chain FAILED at id %d after %d rows: %s", res.FirstBadID, res.RowsChecked, res.Reason)
	}
	fmt.Printf("✔ audit chain intact: %d rows, head %d:%s\n", res.RowsChecked, res.HeadID, hex.EncodeToString(res.HeadHash))
	return nil
}
