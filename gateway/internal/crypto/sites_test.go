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
	"cmd/vyuhactl/download.go:runDownload",         // the only caller of OpenAccessToken
}

// Names that reach a decrypt, however they're used.
var decryptNames = map[string]bool{"Open": true, "openAudited": true, "OpenAccessToken": true}

// Packages whose Open has nothing to do with secrets, by name and exact import path. Any other .Open counts as a
// decrypt, so a new variable name can't hide one, nor can a local named after one of these.
var notDecrypts = map[string]string{
	"os":         "os",
	"sql":        "database/sql",
	"clickhouse": "github.com/ClickHouse/clickhouse-go/v2",
	"candles":    "github.com/Jitesh25X/vyuha-terminal/gateway/internal/candles",
}

// A candle's Open price: `b.Open`, a local b, read and never called, in a file that can't reach a secret. Anything
// else in these functions still fails, including keeping a method value to call later.
var openPriceReads = []string{
	"internal/api/marketdata.go:GetCandles",
	"internal/candles/candles.go:Bars",
}

// importsPackage reports whether this file imports the harmless package under that very name: `_ "os"` beside
// an alias called os doesn't count.
func importsPackage(f *ast.File, name string) bool {
	want, ok := notDecrypts[name]
	if !ok {
		return false
	}
	return slices.ContainsFunc(f.Imports, func(imp *ast.ImportSpec) bool {
		return strings.Trim(imp.Path.Value, `"`) == want && (imp.Name == nil || imp.Name.Name == name)
	})
}

// reachesSecrets reports whether this file imports what decrypts, so a read of .Open there can't be trusted.
func reachesSecrets(f *ast.File) bool {
	return slices.ContainsFunc(f.Imports, func(imp *ast.ImportSpec) bool {
		p := strings.Trim(imp.Path.Value, `"`)
		return strings.HasSuffix(p, "/internal/crypto") || strings.HasSuffix(p, "/internal/brokers")
	})
}

// isOpenPriceRead reports a read of b.Open with b a local variable, in a function known to handle candles.
func isOpenPriceRead(f *ast.File, site string, sel *ast.SelectorExpr, called bool) bool {
	id, ok := sel.X.(*ast.Ident)
	return ok && !called && id.Name == "b" && id.Obj != nil && sel.Sel.Name == "Open" &&
		slices.Contains(openPriceReads, site) && !reachesSecrets(f)
}

// decryptCalls returns file:function for each use that could open a secret: a call, or a method value kept to
// call later (open := s.env.Open).
func decryptCalls(t *testing.T, name, src string) []string {
	t.Helper()
	fset := token.NewFileSet()
	f, err := parser.ParseFile(fset, name, src, 0) // with object resolution: it tells a local from a package
	if err != nil {
		t.Fatalf("%s: %v", name, err)
	}
	var out []string
	visit := func(fn string, body ast.Node) {
		called := map[*ast.SelectorExpr]bool{}
		ast.Inspect(body, func(n ast.Node) bool {
			if call, ok := n.(*ast.CallExpr); ok {
				if sel, ok := call.Fun.(*ast.SelectorExpr); ok {
					called[sel] = true
				}
			}
			site := name + ":" + fn
			// A bare use of a helper: inside its own package, or through a dot import. Open itself is only ever
			// a method or a package's function, so a bare Open isn't one.
			if id, ok := n.(*ast.Ident); ok && id.Name != "Open" && decryptNames[id.Name] {
				out = append(out, site)
				return true
			}
			sel, ok := n.(*ast.SelectorExpr)
			if !ok || !decryptNames[sel.Sel.Name] {
				return true
			}
			// A package's Open, only if nothing in this file declares that name and the file imports it.
			if id, ok := sel.X.(*ast.Ident); ok && sel.Sel.Name == "Open" && id.Obj == nil && importsPackage(f, id.Name) {
				return true
			}
			if isOpenPriceRead(f, site, sel, called[sel]) {
				return true
			}
			out = append(out, site)
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
		if strings.HasPrefix(filepath.ToSlash(rel), "gen/") { // generated protobuf: holds no secrets or keys
			return nil
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
		"a new variable name":            `package x; func f() { vault.Open(sealed, ad) }`,
		"the broker helper":              `package x; func f() { s.openAudited(ctx, sealed, "c", id, "u", "p") }`,
		"at package level":               `package x; var v, _ = env.Open(sealed, ad)`,
		"a method value":                 `package x; func f() { open := s.env.Open; open(sealed, ad) }`,
		"the token helper":               `package x; import "b/brokers"; func f() { brokers.OpenAccessToken(ctx) }`,
		"a local named after a package":  `package x; import "os"; func f() { candles := vault; candles.Open(sealed, ad); _ = os.Args }`,
		"a shadowed package":             `package x; import "os"; func f() { os := vault; os.Open(sealed, ad) }`,
		"a bare helper call":             `package x; func f() { OpenAccessToken(ctx, db, env) }`,
		"a bare helper value":            `package x; var v = OpenAccessToken`,
		"a dot-imported helper":          `package x; import . "b/brokers"; func f() { OpenAccessToken(ctx) }`,
		"an alias named after a package": `package x; import (_ "os"; os "b/brokers"); func f() { os.Open(a); os.OpenAccessToken(a) }`,
		"a helper off a harmless name":   `package x; import "os"; func f() { os.OpenAccessToken(ctx) }`,
		"a package not imported":         `package x; func f() { sql.Open(sealed, ad) }`,
	} {
		if len(decryptCalls(t, "x.go", src)) == 0 {
			t.Errorf("%s: not caught", name)
		}
	}
	if got := decryptCalls(t, "internal/candles/candles.go", `package x; func Bars() { var b Bar; read(b.Open); scan(&b.Open) }`); len(got) != 0 {
		t.Errorf("flagged a candle's open price: %v", got)
	}
	for what, src := range map[string]string{
		"a method value kept to call": `package x; func Bars() { zopen := zenv.Open; zopen(nil, nil) }`,
		"another local's Open":        `package x; func Bars() { e := vault; v := e.Open; v(nil, nil) }`,
		"a b that isn't local":        `package x; func Bars() { read(b.Open) }`,
		"a b in a file with secrets":  `package x; import "g/internal/crypto"; func Bars(b *crypto.Envelope) { v := b.Open; v(nil, nil) }`,
	} {
		if got := decryptCalls(t, "internal/candles/candles.go", src); len(got) == 0 {
			t.Errorf("%s: slipped through the open price exemption", what)
		}
	}
	if got := decryptCalls(t, "internal/candles/candles.go", `package x; func Bars() { vault.Open(sealed, ad) }`); len(got) != 1 {
		t.Errorf("a real call next to the open price wasn't flagged: %v", got)
	}
	if got := decryptCalls(t, "x.go", `package x; import ("os"; "github.com/ClickHouse/clickhouse-go/v2"); func f() { os.Open("p"); clickhouse.Open(o) }`); len(got) != 0 {
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
