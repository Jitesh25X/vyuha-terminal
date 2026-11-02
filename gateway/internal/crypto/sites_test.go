package crypto

import (
	"go/ast"
	"go/parser"
	"go/token"
	"io/fs"
	"os"
	"path/filepath"
	"regexp"
	"slices"
	"strings"
	"testing"
)

// permittedDecryptCalls are the only places a secret is opened (gateway/CLAUDE.md), as file:function.
var permittedDecryptCalls = []string{
	"internal/auth/service.go:totpOK",              // login TOTP verification
	"internal/brokers/accounts.go:OpenAccessToken", // vyuhactl download run
	"internal/brokers/service.go:LoginURL",         // broker login URL
	"internal/brokers/service.go:complete",         // token exchange and StartSession
	"internal/brokers/service.go:openAudited",      // the helper those two go through
	"internal/crypto/envelope.go:Open",             // the implementation itself
}

// Packages whose Open has nothing to do with secrets. Any other .Open counts as a decrypt, so a new
// variable name can't hide one.
var notDecrypts = map[string]bool{"os": true, "clickhouse": true, "candles": true, "sql": true}

// decryptCalls returns file:function for each call that could open a secret.
func decryptCalls(t *testing.T, name, src string) []string {
	t.Helper()
	fset := token.NewFileSet()
	f, err := parser.ParseFile(fset, name, src, parser.SkipObjectResolution)
	if err != nil {
		t.Fatalf("%s: %v", name, err)
	}
	var out []string
	visit := func(fn string, body ast.Node) {
		ast.Inspect(body, func(n ast.Node) bool {
			call, ok := n.(*ast.CallExpr)
			if !ok {
				return true
			}
			sel, ok := call.Fun.(*ast.SelectorExpr)
			if !ok || (sel.Sel.Name != "Open" && sel.Sel.Name != "openAudited") {
				return true
			}
			if id, ok := sel.X.(*ast.Ident); ok && notDecrypts[id.Name] {
				return true
			}
			out = append(out, name+":"+fn)
			return true
		})
	}
	for _, d := range f.Decls {
		switch decl := d.(type) {
		case *ast.FuncDecl:
			if decl.Body != nil {
				visit(decl.Name.Name, decl.Body)
			}
		case *ast.GenDecl:
			visit("package level", decl)
		}
	}
	return out
}

func TestDecryptSitesAreOnlyThePermittedOnes(t *testing.T) {
	root := filepath.Join("..", "..")
	seen := map[string]bool{}
	err := filepath.WalkDir(root, func(path string, d fs.DirEntry, err error) error {
		if err != nil || d.IsDir() || !strings.HasSuffix(path, ".go") || strings.HasSuffix(path, "_test.go") {
			return err
		}
		src, err := os.ReadFile(path) //nolint:gosec // this repository's own files
		if err != nil {
			return err
		}
		rel, err := filepath.Rel(root, path)
		if err != nil {
			return err
		}
		for _, site := range decryptCalls(t, filepath.ToSlash(rel), string(src)) {
			seen[site] = true
			if !slices.Contains(permittedDecryptCalls, site) {
				t.Errorf("%s may decrypt a secret, and it isn't a permitted site (gateway/CLAUDE.md). The owner decides new ones", site)
			}
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	for _, site := range permittedDecryptCalls {
		if !seen[site] {
			t.Errorf("%s no longer decrypts anything: update this list and gateway/CLAUDE.md", site)
		}
	}
}

func TestDecryptSiteCheckCatchesNewCalls(t *testing.T) {
	for name, src := range map[string]string{
		"a new variable name": `package x; func f() { vault.Open(sealed, ad) }`,
		"the broker helper":   `package x; func f() { s.openAudited(ctx, sealed, "c", id, "u", "p") }`,
		"at package level":    `package x; var v, _ = env.Open(sealed, ad)`,
	} {
		if len(decryptCalls(t, "x.go", src)) == 0 {
			t.Errorf("%s: not caught", name)
		}
	}
	if got := decryptCalls(t, "x.go", `package x; func f() { os.Open("p"); clickhouse.Open(o) }`); len(got) != 0 {
		t.Errorf("flagged unrelated calls: %v", got)
	}
}

// Python and the engine never hold the master key: they get what they need from the gateway.
func TestOtherServicesDontDecrypt(t *testing.T) {
	aes := regexp.MustCompile(`AESGCM|from cryptography|import cryptography`)
	for _, dir := range []string{"quant/src", "intel/src"} {
		root := filepath.Join("..", "..", "..", dir)
		if _, err := os.Stat(root); err != nil {
			continue // not scaffolded yet
		}
		err := filepath.WalkDir(root, func(path string, d fs.DirEntry, err error) error {
			if err != nil || d.IsDir() || !strings.HasSuffix(path, ".py") {
				return err
			}
			src, err := os.ReadFile(path) //nolint:gosec // this repository's own files
			if err != nil {
				return err
			}
			if aes.Match(src) {
				t.Errorf("%s decrypts secrets: only the sites in gateway/CLAUDE.md may", path)
			}
			return nil
		})
		if err != nil {
			t.Fatal(err)
		}
	}
}
