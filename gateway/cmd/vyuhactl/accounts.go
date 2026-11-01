package main

import (
	"bufio"
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"strings"
	"time"

	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/brokers"
	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/config"
	vcrypto "github.com/Jitesh25X/vyuha-terminal/gateway/internal/crypto"
	"github.com/Jitesh25X/vyuha-terminal/gateway/internal/store"
)

func runAccountCreate(args []string) error {
	fs := flag.NewFlagSet("account create", flag.ContinueOnError)
	var a brokers.NewAccount
	fs.StringVar(&a.Broker, "broker", "", "zerodha | upstox | sim")
	fs.StringVar(&a.ClientCode, "client-code", "", "broker client id (UCC), e.g. AB1234")
	fs.StringVar(&a.HolderName, "holder", "", "account holder's name")
	fs.StringVar(&a.StaticIP, "static-ip", "", "host-local address this account's traffic binds to (required unless sim)")
	fs.StringVar(&a.PublicIP, "public-ip", "", "public static IP registered with the broker")
	fs.StringVar(&a.BackupIP, "backup-ip", "", "backup host-local address")
	fs.BoolVar(&a.DataEntitled, "data-entitled", false, "stream market data through this account (ADR-009)")
	fs.IntVar(&a.OpsCap, "ops-cap", 8, fmt.Sprintf("orders per second cap, 1..%d", brokers.MaxOpsCap))
	if err := fs.Parse(args); err != nil {
		return err
	}
	cfg, err := config.Load(os.Getenv)
	if err != nil {
		return err
	}
	env, err := vcrypto.NewEnvelope(cfg.MasterKey)
	if err != nil {
		return err
	}
	if a.Broker != "sim" {
		if a.APIKey, a.APISecret, err = readAPICredentials(); err != nil {
			return err
		}
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	pool, err := store.NewPool(ctx, cfg.PostgresURL)
	if err != nil {
		return err
	}
	defer pool.Close()
	id, err := brokers.CreateAccount(ctx, pool, env, a)
	if err != nil {
		return err
	}
	fmt.Printf("✔ created %s account %s id=%s (API key and secret encrypted)\n", a.Broker, a.ClientCode, id)
	return nil
}

func runAccountMap(args []string) error {
	fs := flag.NewFlagSet("account map", flag.ContinueOnError)
	account := fs.String("account", "", "account id, or broker:client_code")
	username := fs.String("user", "", "username")
	trade := fs.Bool("trade", false, "allow trading (and the daily broker login)")
	def := fs.Bool("default", false, "make this the user's default account")
	if err := fs.Parse(args); err != nil {
		return err
	}
	if *account == "" || *username == "" {
		return errors.New("-account and -user are required")
	}
	cfg, err := config.Load(os.Getenv)
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
	if err := brokers.MapUser(ctx, pool, *account, *username, *trade, *def); err != nil {
		return err
	}
	fmt.Printf("✔ %s can view%s %s\n", *username, map[bool]string{true: " and trade", false: ""}[*trade], *account)
	return nil
}

// readAPICredentials reads the API key and secret: prompted with echo off on a terminal, or two
// lines from piped stdin. Never from flags (argv is visible to other users via ps).
func readAPICredentials() (string, string, error) {
	fi, err := os.Stdin.Stat()
	if err != nil {
		return "", "", err
	}
	if fi.Mode()&os.ModeCharDevice == 0 {
		r := bufio.NewReader(os.Stdin)
		var lines [2]string
		for i := range lines {
			line, err := r.ReadString('\n')
			if err != nil && !errors.Is(err, io.EOF) {
				return "", "", err
			}
			lines[i] = strings.TrimSpace(line)
		}
		return lines[0], lines[1], nil
	}
	key, err := promptNoEcho("API key: ")
	if err != nil {
		return "", "", err
	}
	secret, err := promptNoEcho("API secret: ")
	if err != nil {
		return "", "", err
	}
	return strings.TrimSpace(key), strings.TrimSpace(secret), nil
}
