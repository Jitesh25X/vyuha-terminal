package crypto

import (
	"fmt"
	"go/ast"
	"go/parser"
	"go/token"
	"io/fs"
	"maps"
	"os"
	"path/filepath"
	"regexp"
	"slices"
	"strings"
	"testing"
)

// permittedDecrypts are the only places a secret is opened (gateway/CLAUDE.md): file:Type.function, then what it
// may use and exactly how many times. So a listed function can't quietly open one more thing, and `s.env.Open` in a
// function that should go through the audited helper fails.
var permittedDecrypts = map[string]map[string]int{
	"internal/auth/service.go:Service.totpOK":         {"Open": 1},            // login TOTP verification
	"internal/brokers/accounts.go:OpenAccessToken":    {"Open": 1},            // vyuhactl download run
	"internal/brokers/service.go:Service.LoginURL":    {"openAudited": 1},     // broker login URL
	"internal/brokers/service.go:Service.complete":    {"openAudited": 2},     // token exchange and StartSession
	"internal/brokers/service.go:Service.openAudited": {"Open": 1},            // the helper those two go through
	"internal/crypto/envelope.go:Envelope.Open":       {"Open": 1},            // the implementation itself
	"cmd/vyuhactl/download.go:runDownload":            {"OpenAccessToken": 1}, // the only caller of OpenAccessToken
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
	"internal/api/marketdata.go:Server.GetCandles",
	"internal/candles/candles.go:Reader.Bars",
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

// funcName is Type.function for a method, so a new `complete` on another type is its own site.
func funcName(decl *ast.FuncDecl) string {
	if decl.Recv == nil || len(decl.Recv.List) == 0 {
		return decl.Name.Name
	}
	typ := decl.Recv.List[0].Type
	if star, ok := typ.(*ast.StarExpr); ok {
		typ = star.X
	}
	if id, ok := typ.(*ast.Ident); ok {
		return id.Name + "." + decl.Name.Name
	}
	return decl.Name.Name
}

type decryptUse struct{ site, name string }

// decryptUses returns each use that could open a secret: a call, or a method value kept to call later
// (open := s.env.Open).
func decryptUses(t *testing.T, name, src string) []decryptUse {
	t.Helper()
	fset := token.NewFileSet()
	f, err := parser.ParseFile(fset, name, src, 0) // with object resolution: it tells a local from a package
	if err != nil {
		t.Fatalf("%s: %v", name, err)
	}
	var out []decryptUse
	visit := func(fn string, body ast.Node) {
		called := map[*ast.SelectorExpr]bool{}
		selected := map[*ast.Ident]bool{} // a selector's name, already counted with its selector
		site := name + ":" + fn
		ast.Inspect(body, func(n ast.Node) bool {
			if call, ok := n.(*ast.CallExpr); ok {
				if sel, ok := call.Fun.(*ast.SelectorExpr); ok {
					called[sel] = true
				}
			}
			// A bare use of a helper: inside its own package, or through a dot import. Open itself is only ever
			// a method or a package's function, so a bare Open isn't one.
			if id, ok := n.(*ast.Ident); ok && !selected[id] && id.Name != "Open" && decryptNames[id.Name] {
				out = append(out, decryptUse{site, id.Name})
				return true
			}
			sel, ok := n.(*ast.SelectorExpr)
			if !ok {
				return true
			}
			selected[sel.Sel] = true
			if !decryptNames[sel.Sel.Name] {
				return true
			}
			// A package's Open, only if nothing in this file declares that name and the file imports it.
			if id, ok := sel.X.(*ast.Ident); ok && sel.Sel.Name == "Open" && id.Obj == nil && importsPackage(f, id.Name) {
				return true
			}
			if isOpenPriceRead(f, site, sel, called[sel]) {
				return true
			}
			out = append(out, decryptUse{site, sel.Sel.Name})
			return true
		})
	}
	for _, d := range f.Decls {
		switch decl := d.(type) {
		case *ast.FuncDecl:
			if decl.Body != nil {
				visit(funcName(decl), decl.Body)
			}
		case *ast.GenDecl:
			visit("package level", decl)
		}
	}
	return out
}

func decryptCalls(t *testing.T, name, src string) []string {
	t.Helper()
	var out []string
	for _, u := range decryptUses(t, name, src) {
		out = append(out, u.site)
	}
	return out
}

// sitesProblems compares what the code uses with permittedDecrypts, both ways.
func sitesProblems(uses []decryptUse) []string {
	got := map[string]map[string]int{}
	for _, u := range uses {
		if got[u.site] == nil {
			got[u.site] = map[string]int{}
		}
		got[u.site][u.name]++
	}
	var out []string
	for site, names := range got {
		want, listed := permittedDecrypts[site]
		if !listed {
			out = append(out, fmt.Sprintf("%s may decrypt a secret (%v), and it isn't a permitted site (gateway/CLAUDE.md). The owner decides new ones", site, names))
		} else if !maps.Equal(names, want) {
			out = append(out, fmt.Sprintf("%s uses %v, and only %v is permitted: every decrypt must be audited (gateway/CLAUDE.md)", site, names, want))
		}
	}
	for site := range permittedDecrypts {
		if got[site] == nil {
			out = append(out, site+" no longer decrypts anything: update this list and gateway/CLAUDE.md")
		}
	}
	slices.Sort(out)
	return out
}

func TestDecryptSitesAreOnlyThePermittedOnes(t *testing.T) {
	root := filepath.Join("..", "..")
	var uses []decryptUse
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
		uses = append(uses, decryptUses(t, filepath.ToSlash(rel), string(src))...)
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	for _, problem := range sitesProblems(uses) {
		t.Error(problem)
	}
}

// A listed function can't quietly open one more thing, or skip the audited helper.
func TestDecryptSiteCountsAreExact(t *testing.T) {
	const service = "internal/brokers/service.go"
	listed := []decryptUse{}
	for site, names := range permittedDecrypts {
		for name, n := range names {
			for range n {
				listed = append(listed, decryptUse{site, name})
			}
		}
	}
	if got := sitesProblems(listed); len(got) != 0 {
		t.Fatalf("the list itself fails: %v", got)
	}
	for what, src := range map[string]string{
		"a direct Open in a listed function":  `package x; func (s *Service) LoginURL() { s.openAudited(); s.env.Open(a, b) }`,
		"a third secret in complete":          `package x; func (s *Service) complete() { s.openAudited(); s.openAudited(); s.openAudited() }`,
		"a method value in a listed function": `package x; func (s *Service) complete() { s.openAudited(); s.openAudited(); v := s.env.Open; v(a, b) }`,
		"a complete on another type":          `package x; func (o *Other) complete() { o.openAudited() }`,
	} {
		uses := append(slices.Clone(listed), decryptUses(t, service, src)...)
		if got := sitesProblems(uses); len(got) == 0 {
			t.Errorf("%s: not caught", what)
		}
	}
	if got := len(decryptUses(t, service, `package x; func (s *Service) LoginURL() { s.openAudited() }`)); got != 1 {
		t.Errorf("one helper call counted %d times", got)
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
	if got := decryptCalls(t, "internal/candles/candles.go", `package x; func (r *Reader) Bars() { var b Bar; read(b.Open); scan(&b.Open) }`); len(got) != 0 {
		t.Errorf("flagged a candle's open price: %v", got)
	}
	for what, src := range map[string]string{
		"a method value kept to call": `package x; func (r *Reader) Bars() { zopen := zenv.Open; zopen(nil, nil) }`,
		"another local's Open":        `package x; func (r *Reader) Bars() { e := vault; v := e.Open; v(nil, nil) }`,
		"a b that isn't local":        `package x; func (r *Reader) Bars() { read(b.Open) }`,
		"a b in a file with secrets":  `package x; import "g/internal/crypto"; func (r *Reader) Bars(b *crypto.Envelope) { v := b.Open; v(nil, nil) }`,
	} {
		if got := decryptCalls(t, "internal/candles/candles.go", src); len(got) == 0 {
			t.Errorf("%s: slipped through the open price exemption", what)
		}
	}
	if got := decryptCalls(t, "internal/candles/candles.go", `package x; func (r *Reader) Bars() { vault.Open(sealed, ad) }`); len(got) != 1 {
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
