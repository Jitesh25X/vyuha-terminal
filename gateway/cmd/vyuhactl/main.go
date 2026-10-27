// Command vyuhactl is the admin CLI (migrations now; user create / audit verify / kill from later tasks).
package main

import (
	"flag"
	"fmt"
	"os"

	"github.com/jiteshraj/vyuha/gateway/internal/config"
	"github.com/jiteshraj/vyuha/gateway/internal/store"
)

const usage = `usage: vyuhactl migrate up|version [-dir path]   (reads POSTGRES_URL; dev default from VYUHA_ENV=dev)`

func main() {
	if err := run(os.Args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, "vyuhactl:", err)
		os.Exit(1)
	}
}

func run(args []string) error {
	if len(args) < 2 || args[0] != "migrate" {
		return fmt.Errorf("%s", usage)
	}
	fs := flag.NewFlagSet("migrate", flag.ContinueOnError)
	dir := fs.String("dir", "../db/migrations", "directory holding NNNNNN_name.{up,down}.sql files")
	if err := fs.Parse(args[2:]); err != nil {
		return err
	}
	cfg, err := config.Load(os.Getenv)
	if err != nil {
		return err
	}

	switch args[1] {
	case "up":
		v, err := store.MigrateUp(*dir, cfg.PostgresURL)
		if err != nil {
			return err
		}
		fmt.Printf("✔ schema version %d\n", v)
	case "version":
		v, dirty, err := store.MigrationVersion(*dir, cfg.PostgresURL)
		if err != nil {
			return err
		}
		fmt.Printf("schema version %d (dirty=%t)\n", v, dirty)
	default:
		return fmt.Errorf("%s", usage)
	}
	return nil
}
