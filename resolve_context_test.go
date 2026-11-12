package ssh_config

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func mustResolveDecode(t *testing.T, config string) *Config {
	t.Helper()
	cfg, err := DecodeExtended(strings.NewReader(config))
	if err != nil {
		t.Fatalf("unexpected extended parse error: %v", err)
	}
	return cfg
}

func wantGet(t *testing.T, res *ResolveResult, key, want string) {
	t.Helper()
	if got := res.Get(key); got != want {
		t.Errorf("Get(%q) = %q, want %q", key, got, want)
	}
}

func wantGetAll(t *testing.T, res *ResolveResult, key string, want ...string) {
	t.Helper()
	got := res.GetAll(key)
	if len(got) != len(want) {
		t.Fatalf("GetAll(%q) = %v, want %v", key, got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Errorf("GetAll(%q)[%d] = %q, want %q", key, i, got[i], want[i])
		}
	}
}

func TestResolvePhases(t *testing.T) {
	config := `
Host prod-*
    User deploy
    Port 2222
    IdentityFile ~/.ssh/prod_key

Host *.corp
    Port 2200
    IdentityFile ~/.ssh/corp_key

Match host = *.corp,!*.staging user = alice
    User alice
    IdentityFile ~/.ssh/corp_match_key
`

	baseCtx := ResolveContext{
		OriginalHost:  "prod-web",
		Host:          "prod-web",
		CanonicalHost: "web.corp",
		RemoteUser:    "alice",
		LocalUser:     "kevin",
	}

	t.Run("alias and canonical differ", func(t *testing.T) {
		cfg := mustResolveDecode(t, config)
		res := cfg.Resolve(baseCtx)
		// Host prod-* matched the original alias in the normal round; the
		// canonical-only Host *.corp must not overwrite the first value.
		wantGet(t, res, "User", "deploy")
		wantGet(t, res, "Port", "2222")
		wantGetAll(t, res, "IdentityFile",
			"~/.ssh/prod_key", "~/.ssh/corp_key", "~/.ssh/corp_match_key")

		// Same config tree, different connection context.
		ctx2 := baseCtx
		ctx2.RemoteUser = "guest"
		res2 := cfg.Resolve(ctx2)
		wantGet(t, res2, "User", "deploy")
		wantGetAll(t, res2, "IdentityFile", "~/.ssh/prod_key", "~/.ssh/corp_key")
	})

	t.Run("negated pattern list", func(t *testing.T) {
		cfg := mustResolveDecode(t, `
Match host = *.corp,!*.staging.corp
    User denied
`)
		// The negated entry matches, so the whole list is rejected.
		res := cfg.Resolve(ResolveContext{
			OriginalHost:  "x",
			Host:          "x",
			CanonicalHost: "web.staging.corp",
		})
		wantGet(t, res, "User", "")
	})

	t.Run("only negated patterns never match", func(t *testing.T) {
		cfg := mustResolveDecode(t, `
Match host = !*.internal
    User allowed
`)
		res := cfg.Resolve(ResolveContext{
			OriginalHost:  "x",
			Host:          "x",
			CanonicalHost: "web.corp",
		})
		wantGet(t, res, "User", "")
	})

	t.Run("originalhost uses the original alias", func(t *testing.T) {
		cfg := mustResolveDecode(t, `
Match originalhost = prod-* user = deploy
    User matched
    Port 2000
`)
		ctx := baseCtx
		ctx.RemoteUser = "deploy"
		res := cfg.Resolve(ctx)
		wantGet(t, res, "User", "matched")
		wantGet(t, res, "Port", "2000")
	})

	t.Run("user conditions only consult the context", func(t *testing.T) {
		// No User directive exists in the configuration, yet the user
		// condition must not be inferred from anything but the context.
		cfg := mustResolveDecode(t, `
Match user = nobody
    Compression yes
`)
		res := cfg.Resolve(ResolveContext{OriginalHost: "x", Host: "x", RemoteUser: ""})
		wantGet(t, res, "Compression", "")
		res = cfg.Resolve(ResolveContext{OriginalHost: "x", Host: "x", RemoteUser: "nobody"})
		wantGet(t, res, "Compression", "yes")
	})

	t.Run("localuser condition", func(t *testing.T) {
		cfg := mustResolveDecode(t, `
Match localuser = root,!guest
    Port 22
`)
		res := cfg.Resolve(ResolveContext{OriginalHost: "x", Host: "x", LocalUser: "root"})
		wantGet(t, res, "Port", "22")
		res = cfg.Resolve(ResolveContext{OriginalHost: "x", Host: "x", LocalUser: "guest"})
		wantGet(t, res, "Port", "")
	})

	t.Run("final condition requests a second round", func(t *testing.T) {
		cfg := mustResolveDecode(t, `
Host *
    User first

Match final
    Port 4242
`)
		// CanonicalHost is empty, but final in the tree still requests the
		// final round, which uses ctx.Host.
		res := cfg.Resolve(ResolveContext{OriginalHost: "alias", Host: "target"})
		wantGet(t, res, "User", "first")
		wantGet(t, res, "Port", "4242")
	})

	t.Run("canonical condition requires a canonical result", func(t *testing.T) {
		cfg := mustResolveDecode(t, `
Match canonical
    Port 1234
`)
		res := cfg.Resolve(ResolveContext{OriginalHost: "a", Host: "b"})
		wantGet(t, res, "Port", "")
		res = cfg.Resolve(ResolveContext{OriginalHost: "a", Host: "b", CanonicalHost: "c"})
		wantGet(t, res, "Port", "1234")
	})

	t.Run("match host during normal round uses ctx.Host", func(t *testing.T) {
		cfg := mustResolveDecode(t, `
Match host = target.example
    Port 5555
`)
		// OriginalHost differs from Host: the normal round's host condition
		// must use ctx.Host, not the alias.
		res := cfg.Resolve(ResolveContext{OriginalHost: "alias", Host: "target.example"})
		wantGet(t, res, "Port", "5555")
		res = cfg.Resolve(ResolveContext{OriginalHost: "target.example", Host: "alias"})
		wantGet(t, res, "Port", "")
	})

	t.Run("identity file dedup across rounds", func(t *testing.T) {
		cfg := mustResolveDecode(t, `
Host *
    IdentityFile ~/.ssh/everywhere

Host *.corp
    IdentityFile ~/.ssh/everywhere
`)
		// The Host * line matches both rounds: only one copy. The Host *.corp
		// line is a different directive that happens to hold the same value:
		// keep it.
		res := cfg.Resolve(ResolveContext{
			OriginalHost:  "prod",
			Host:          "prod",
			CanonicalHost: "web.corp",
		})
		wantGetAll(t, res, "IdentityFile", "~/.ssh/everywhere", "~/.ssh/everywhere")
	})

	t.Run("results are independent and slices are copies", func(t *testing.T) {
		cfg := mustResolveDecode(t, config)
		res := cfg.Resolve(baseCtx)
		vals := res.GetAll("IdentityFile")
		vals[0] = "mutated"

		// Subsequent queries on the same result are unaffected.
		wantGetAll(t, res, "IdentityFile",
			"~/.ssh/prod_key", "~/.ssh/corp_key", "~/.ssh/corp_match_key")
		// A new resolution is unaffected too.
		res2 := cfg.Resolve(baseCtx)
		wantGetAll(t, res2, "IdentityFile",
			"~/.ssh/prod_key", "~/.ssh/corp_key", "~/.ssh/corp_match_key")
		// The configuration tree string is unchanged.
		cfg2 := mustResolveDecode(t, config)
		if cfg.String() != cfg2.String() {
			t.Errorf("Resolve mutated the configuration tree:\n%s\nwant:\n%s", cfg.String(), cfg2.String())
		}
	})

	t.Run("missing values are empty, defaults are not filled", func(t *testing.T) {
		cfg := mustResolveDecode(t, "Host *\n    User deploy\n")
		res := cfg.Resolve(baseCtx)
		wantGet(t, res, "Port", "") // no default "22" injected
		wantGetAll(t, res, "Nonexistent")
		if res.GetAll("Nonexistent") != nil {
			t.Errorf("expected nil for unset keyword")
		}
		if res.Get("nonexistent") != "" {
			t.Errorf("expected empty for unset keyword")
		}
	})
}

func TestResolveInclude(t *testing.T) {
	dir := t.TempDir()
	write := func(name, contents string) string {
		t.Helper()
		path := filepath.Join(dir, name)
		if err := os.WriteFile(path, []byte(contents), 0644); err != nil {
			t.Fatalf("write %s: %v", name, err)
		}
		return path
	}

	leaf2 := write("leaf2", `
Match final
    IdentityFile ~/.ssh/final_key
`)
	leaf := write("leaf", fmt.Sprintf(`
Include %s

Host *.corp
    IdentityFile ~/.ssh/leaf_key
`, leaf2))
	middle := write("middle", fmt.Sprintf(`
Host *.corp
    Port 2200

Include %s

Match user = guest
    Compression no
`, leaf))
	outer := write("outer", fmt.Sprintf(`
Host *
    User outer
    Include %s
    Compression yes

Host other-*
    Include %s
`, middle, middle))
	cfg, err := DecodeExtendedFile(outer)
	if err != nil {
		t.Fatalf("unexpected parse error: %v", err)
	}

	ctx := ResolveContext{
		OriginalHost:  "prod-web",
		Host:          "prod-web",
		CanonicalHost: "web.corp",
		RemoteUser:    "root", // not guest: the child Match does not apply
	}
	res := cfg.Resolve(ctx)

	wantGet(t, res, "User", "outer")
	// Nested Include contributed a canonical-only Host block.
	wantGet(t, res, "Port", "2200")
	// A Match at the end of a child file must not affect lines that follow the
	// Include in the parent.
	wantGet(t, res, "Compression", "yes")
	// Values from the nested files, in configuration order. The leaf's nested
	// Include (leading to the final-match file) precedes the leaf's own
	// Host *.corp block, and none of them matched the normal round.
	wantGetAll(t, res, "IdentityFile", "~/.ssh/final_key", "~/.ssh/leaf_key")

	// The Include inside a non-matching outer Host block contributes nothing.
	cfgNonMatch := mustResolveDecode(t, fmt.Sprintf(`
Host unrelated-*
    Include %s
`, middle))
	resNonMatch := cfgNonMatch.Resolve(ctx)
	wantGet(t, resNonMatch, "Port", "")
	wantGetAll(t, resNonMatch, "IdentityFile")

	// A child Match can change state inside the child. With RemoteUser guest
	// the child's Match applies inside the child only.
	ctxGuest := ctx
	ctxGuest.RemoteUser = "guest"
	resGuest := cfg.Resolve(ctxGuest)
	// Ordering: the parent Host block's Include runs before the parent's
	// Compression line, so the child's Match user = guest set Compression
	// first. First-one-wins keeps the child value, showing the child Match
	// only affected lines inside the include.
	if got := resGuest.Get("Compression"); got != "no" {
		t.Errorf("Compression = %q, want child Match value %q", got, "no")
	}

	// Round-trip of the parent file, including the Include comments.
	original, err := os.ReadFile(outer)
	if err != nil {
		t.Fatal(err)
	}
	if cfg.String() != string(original) {
		t.Errorf("outer round-trip mismatch:\ngot:\n%s\nwant:\n%s", cfg.String(), original)
	}
}

func TestResolveSyntax(t *testing.T) {
	t.Run("unknown condition", func(t *testing.T) {
		_, err := DecodeExtended(strings.NewReader("Match bogus = x\n    Port 22"))
		if err == nil {
			t.Fatal("expected error for unknown condition")
		}
		var wantPos = "(1, 7)"
		if !strings.Contains(err.Error(), wantPos) {
			t.Errorf("error %q does not carry position %s", err, wantPos)
		}
		if !strings.Contains(err.Error(), "unknown condition") {
			t.Errorf("error %q does not name the problem", err)
		}
	})

	t.Run("missing argument", func(t *testing.T) {
		_, err := DecodeExtended(strings.NewReader("Match host =\n    Port 22"))
		if err == nil || !strings.Contains(err.Error(), "requires at least one pattern") {
			t.Fatalf("expected missing-pattern error, got %v", err)
		}
	})

	t.Run("illegal combination: all mixed", func(t *testing.T) {
		_, err := DecodeExtended(strings.NewReader("Match user = root all\n    Port 22"))
		if err == nil || !strings.Contains(err.Error(), "Match all") {
			t.Fatalf("expected all-combination error, got %v", err)
		}
	})

	t.Run("illegal combination: condition after flag", func(t *testing.T) {
		_, err := DecodeExtended(strings.NewReader("Match final user = root\n    Port 22"))
		if err == nil || !strings.Contains(err.Error(), "no conditions may follow") {
			t.Fatalf("expected flag-ordering error, got %v", err)
		}
	})

	t.Run("canonical takes no argument", func(t *testing.T) {
		_, err := DecodeExtended(strings.NewReader("Match canonical = yes\n    Port 22"))
		if err == nil || !strings.Contains(err.Error(), "does not take an argument") {
			t.Fatalf("expected argument error, got %v", err)
		}
	})

	t.Run("exec rejected with position", func(t *testing.T) {
		_, err := DecodeExtended(strings.NewReader("Match exec = \"echo hi\"\n    Port 22"))
		if err == nil {
			t.Fatal("expected exec rejection")
		}
		if !strings.Contains(err.Error(), "Exec is not supported") || !strings.Contains(err.Error(), "(1, 7)") {
			t.Errorf("unexpected exec error: %v", err)
		}
	})

	t.Run("empty match", func(t *testing.T) {
		_, err := DecodeExtended(strings.NewReader("Match\n    Port 22"))
		if err == nil || !strings.Contains(err.Error(), "requires at least one criterion") {
			t.Fatalf("expected empty-Match error, got %v", err)
		}
	})

	t.Run("round trip keeps comments and syntax", func(t *testing.T) {
		configs := []string{
			"Match host = *.corp,!*.staging user = alice # corp only\n    Port 2222\n",
			"Match originalhost = prod-*,*.example Host = *.corp final # last\n    User x\n", "Match all # everything\n    Port 1\n",
			"Match canonical#attached comment\n    Port 2\n",
			"Match Final\n    Port 3\n",
		}
		for _, config := range configs {
			cfg, err := DecodeExtended(strings.NewReader(config))
			if err != nil {
				t.Errorf("parse %q: %v", config, err)
				continue
			}
			if got := cfg.String(); got != config {
				t.Errorf("round-trip mismatch for %q:\ngot %q", config, got)
			}
		}
	})

	t.Run("old entry still rejects extended-only criteria", func(t *testing.T) {
		for _, config := range []string{
			"Match user = root\n    Port 22",
			"Match originalhost = *.example\n    Port 22",
			"Match localuser = kevin\n    Port 22",
			"Match final\n    Port 22",
			"Match canonical\n    Port 22",
			"Match exec \"echo hi\"\n    Port 22",
		} {
			_, err := Decode(strings.NewReader(config))
			if err == nil {
				t.Errorf("Decode accepted extended-only syntax %q", config)
			}
		}
	})

	t.Run("DecodeExtended accepts legacy Match forms", func(t *testing.T) {
		cfg := mustResolveDecode(t, "Match Host *.corp\n    Port 22\n")
		res := cfg.Resolve(ResolveContext{OriginalHost: "web.corp", Host: "web.corp"})
		wantGet(t, res, "Port", "22")
	})
}
