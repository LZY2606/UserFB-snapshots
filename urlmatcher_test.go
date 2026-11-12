package robotstxt

import (
	"fmt"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
)

func compileGroup(t *testing.T, robots, agent string) *URLMatcher {
	t.Helper()
	r, err := FromString(robots)
	if err != nil {
		t.Fatalf("parse robots: %v", err)
	}
	m, err := r.FindGroup(agent).CompileURLMatcher()
	if err != nil {
		t.Fatalf("CompileURLMatcher: %v", err)
	}
	return m
}

func mustTestURL(t *testing.T, m *URLMatcher, rawURL string) bool {
	t.Helper()
	allow, err := m.Test(rawURL)
	if err != nil {
		t.Fatalf("Test(%q): %v", rawURL, err)
	}
	return allow
}

func TestCompiledURLMatcherContract(t *testing.T) {
	t.Run("ByteEquivalence", func(t *testing.T) {
		m := compileGroup(t, "User-agent: bot\n"+
			"Disallow: /café\n"+
			"Disallow: /tilde~x\n"+
			"Disallow: /a%2Fb\n"+
			"Disallow: /file%2A\n", "bot")

		cases := []struct {
			url   string
			allow bool
		}{
			// Raw UTF-8 and per-byte escapes are the same octets.
			{"http://h/café", false},
			{"http://h/caf%C3%A9", false},
			{"http://h/caf%c3%a9", false}, // hex case normalized
			// Unreserved ASCII escapes equal the raw byte.
			{"http://h/tilde%7Ex", false},
			{"http://h/tilde%7ex", false},
			// Encoded slash is not a path separator.
			{"http://h/a/b", true},
			{"http://h/a%2Fb", false},
			{"http://h/a%2fb", false},
			// %2A in a rule is a literal '*', not a wildcard.
			{"http://h/file*", false},
			{"http://h/file%2A", false},
			{"http://h/fileABC", true},
		}
		for _, c := range cases {
			if got := mustTestURL(t, m, c.url); got != c.allow {
				t.Errorf("Test(%q) = %v, want %v", c.url, got, c.allow)
			}
		}
	})

	t.Run("WildcardAndLiteralCollision", func(t *testing.T) {
		m := compileGroup(t, "User-agent: bot\n"+
			"Disallow: /a*/b\n"+
			"Disallow: /mid$path\n"+
			"Disallow: /end$\n"+
			"Disallow: /dol%24\n"+
			"Disallow: /*F\n", "bot")

		cases := []struct {
			url   string
			allow bool
		}{
			{"/a/x/b", false},
			{"/a/b", false}, // '*' matches zero units
			{"/a/x/b/c", false},
			{"/a/x", true},
			// '$' in the middle of a rule is a literal byte.
			{"/mid$path", false},
			{"/mid$path/x", false},
			{"/midXpath", true},
			// Trailing '$' is an end anchor.
			{"/end", false},
			{"/end/more", true},
			// %24 is a literal '$', never an anchor.
			{"/dol$", false},
			{"/dol%24", false},
			{"/dol$/more", false}, // prefix match: %24 did not anchor
			{"/dolX", true},
			// '*' consumes whole octets: it cannot eat the "%2" of %2F
			// and leave "F" to match the literal suffix.
			{"/%2F", true},
			{"/xF", false},
			{"/F", false},
		}
		for _, c := range cases {
			if got := mustTestURL(t, m, "http://h"+c.url); got != c.allow {
				t.Errorf("Test(%q) = %v, want %v", c.url, got, c.allow)
			}
		}
	})

	t.Run("Priority", func(t *testing.T) {
		// Longest literal part wins; '*' and '$' do not count.
		m := compileGroup(t, "User-agent: bot\n"+
			"Disallow: /a\n"+
			"Allow: /a/b\n"+
			"Disallow: /x*/c\n"+
			"Allow: /x/y/z\n", "bot")
		cases := []struct {
			url   string
			allow bool
		}{
			{"/a/c", false},    // only /a matches
			{"/a/b/c", true},   // /a/b (4) beats /a (2)
			{"/x/y/z/c", true}, // /x/y/z (6) beats /x*/c (literal 4)
			{"/x/q/c", false},  // only /x*/c matches
		}
		for _, c := range cases {
			if got := mustTestURL(t, m, "http://h"+c.url); got != c.allow {
				t.Errorf("Test(%q) = %v, want %v", c.url, got, c.allow)
			}
		}

		// Equal literal length: Allow wins, and rule order must not matter.
		const rules1 = "User-agent: bot\nDisallow: /same\nAllow: /same\n"
		const rules2 = "User-agent: bot\nAllow: /same\nDisallow: /same\n"
		for _, doc := range []string{rules1, rules2} {
			m := compileGroup(t, doc, "bot")
			if got := mustTestURL(t, m, "http://h/same"); !got {
				t.Errorf("tie Allow-first failed for %q", doc)
			}
			if got := mustTestURL(t, m, "http://h/same/more"); !got {
				t.Errorf("tie Allow-first (prefix) failed for %q", doc)
			}
		}
		// Anchored and non-anchored rules of equal literal length tie too.
		mTie := compileGroup(t, "User-agent: bot\nDisallow: /eq$\nAllow: /eq\n", "bot")
		if got := mustTestURL(t, mTie, "http://h/eq"); !got {
			t.Errorf("anchored tie should prefer Allow")
		}
	})

	t.Run("URLForms", func(t *testing.T) {
		m := compileGroup(t, "User-agent: bot\n"+
			"Disallow: /\n", "bot")
		// Empty path becomes "/".
		if got := mustTestURL(t, m, "http://h"); got {
			t.Errorf("empty path should be matched as /")
		}
		if got := mustTestURL(t, m, "https://h:8443"); got {
			t.Errorf("empty path (https, port) should be matched as /")
		}

		mq := compileGroup(t, "User-agent: bot\n"+
			"Disallow: /p?a=1&b=2\n"+
			"Disallow: /q?\n", "bot")
		cases := []struct {
			url   string
			allow bool
		}{
			{"http://h/p?a=1&b=2", false},
			{"http://h/p?a=1&b=2&c=3", false}, // prefix on the matched string
			{"http://h/p?b=2&a=1", true},      // no parameter reordering
			{"http://h/p", true},
			{"http://h/q?", false}, // present-but-empty query keeps '?'
			{"http://h/q", true},
			{"http://h/other#/p?a=1&b=2", true}, // fragment never participates
		}
		for _, c := range cases {
			if got := mustTestURL(t, mq, c.url); got != c.allow {
				t.Errorf("Test(%q) = %v, want %v", c.url, got, c.allow)
			}
		}

		// Only absolute http/https URLs with a host are accepted.
		bad := []string{
			"",
			"/relative/path",
			"//h/path",
			"ftp://h/path",
			"http://",
			"http://h/%zz",
			"http://h/p?x=%zz",
			"http://h/p?x=%2",
		}
		for _, u := range bad {
			if _, err := m.Test(u); err == nil {
				t.Errorf("Test(%q) expected error, got nil", u)
			}
		}
	})

	t.Run("CompileErrorsAndLegacyStillWorks", func(t *testing.T) {
		r, err := FromString("User-agent: bot\nDisallow: /bad%2\nDisallow: /ok\n")
		if err != nil {
			t.Fatalf("parse: %v", err)
		}
		g := r.FindGroup("bot")
		if _, err := g.CompileURLMatcher(); err == nil {
			t.Fatalf("expected compile error for truncated percent-encoding")
		}
		// The group's legacy queries are unaffected.
		if g.Test("/ok/x") {
			t.Errorf("legacy Test(/ok/x) = true, want false")
		}
		if !g.Test("/other") {
			t.Errorf("legacy Test(/other) = false, want true")
		}

		r2, err := FromString("User-agent: bot\nDisallow: /bad%zz\n")
		if err != nil {
			t.Fatalf("parse: %v", err)
		}
		if _, err := r2.FindGroup("bot").CompileURLMatcher(); err == nil {
			t.Fatalf("expected compile error for invalid hex digits")
		}
	})

	t.Run("SpecialGroups", func(t *testing.T) {
		// 5xx -> disallow all.
		r, err := FromStatusAndString(503, "User-agent: *\nDisallow:\n")
		if err != nil {
			t.Fatalf("parse: %v", err)
		}
		m, err := r.FindGroup("bot").CompileURLMatcher()
		if err != nil {
			t.Fatalf("compile: %v", err)
		}
		if got := mustTestURL(t, m, "http://h/anything"); got {
			t.Errorf("disallowAll group should disallow everything")
		}

		// No matching group -> empty group -> allow.
		r2, err := FromString("User-agent: other\nDisallow: /\n")
		if err != nil {
			t.Fatalf("parse: %v", err)
		}
		m2, err := r2.FindGroup("bot").CompileURLMatcher()
		if err != nil {
			t.Fatalf("compile: %v", err)
		}
		if got := mustTestURL(t, m2, "http://h/anything"); !got {
			t.Errorf("empty group should allow everything")
		}
	})

	t.Run("UnrelatedBranchesWorkload", func(t *testing.T) {
		const base = "User-agent: bot\n" +
			"Disallow: /admin/\n" +
			"Allow: /admin/public/\n" +
			"Disallow: /target/deep/path\n"
		const query = "http://h/target/deep/path/and/more"

		var sb strings.Builder
		sb.WriteString(base)
		// 4096 long literal rules, each with a distinct first path
		// segment, none related to the query path.
		for i := 0; i < 4096; i++ {
			fmt.Fprintf(&sb, "Disallow: /z%04d/%s\n", i, strings.Repeat("q", 120))
		}

		mBase := compileGroup(t, base, "bot")
		mBase.steps = &atomic.Int64{}
		if got := mustTestURL(t, mBase, query); got {
			t.Fatalf("query should be disallowed")
		}
		stepsBase := mBase.steps.Load()

		mBig := compileGroup(t, sb.String(), "bot")
		mBig.steps = &atomic.Int64{}
		if got := mustTestURL(t, mBig, query); got {
			t.Fatalf("query should be disallowed")
		}
		stepsBig := mBig.steps.Load()

		units, err := urlToUnits(query)
		if err != nil {
			t.Fatalf("urlToUnits: %v", err)
		}
		t.Logf("steps base=%d big=%d pathUnits=%d", stepsBase, stepsBig, len(units))
		// Literal rules share one trie: 4096 unrelated rules must not add
		// transitions, and the total is proportional to the request length.
		if stepsBig != stepsBase {
			t.Errorf("steps grew with unrelated rules: base=%d big=%d", stepsBase, stepsBig)
		}
		if stepsBig > int64(len(units)) {
			t.Errorf("steps %d exceed request length %d", stepsBig, len(units))
		}
	})

	t.Run("ConcurrentQueries", func(t *testing.T) {
		m := compileGroup(t, "User-agent: bot\n"+
			"Disallow: /private/\n"+
			"Allow: /private/public/\n"+
			"Disallow: /café\n"+
			"Disallow: /a*/b$\n"+
			"Disallow: /file%2A\n", "bot")
		cases := []struct {
			url   string
			allow bool
		}{
			{"http://h/private/x", false},
			{"http://h/private/public/x", true},
			{"http://h/caf%C3%A9", false},
			{"http://h/a/any/thing/b", false},
			{"http://h/a/any/thing/b/c", true},
			{"http://h/file*", false},
			{"http://h/free", true},
		}
		var wg sync.WaitGroup
		for w := 0; w < 8; w++ {
			wg.Add(1)
			go func(w int) {
				defer wg.Done()
				for i := 0; i < 200; i++ {
					c := cases[(w+i)%len(cases)]
					got, err := m.Test(c.url)
					if err != nil {
						t.Errorf("Test(%q): %v", c.url, err)
						return
					}
					if got != c.allow {
						t.Errorf("Test(%q) = %v, want %v", c.url, got, c.allow)
						return
					}
				}
			}(w)
		}
		wg.Wait()
	})
}

func BenchmarkCompileURLMatcher(b *testing.B) {
	var sb strings.Builder
	sb.WriteString("User-agent: bot\n")
	for i := 0; i < 4096; i++ {
		fmt.Fprintf(&sb, "Disallow: /z%04d/%s\n", i, strings.Repeat("q", 120))
	}
	sb.WriteString("Disallow: /a*/b$\nDisallow: /café\n")
	doc := sb.String()
	r, err := FromString(doc)
	if err != nil {
		b.Fatalf("parse: %v", err)
	}
	g := r.FindGroup("bot")
	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		if _, err := g.CompileURLMatcher(); err != nil {
			b.Fatalf("compile: %v", err)
		}
	}
}

func BenchmarkURLMatcherTest(b *testing.B) {
	var sb strings.Builder
	sb.WriteString("User-agent: bot\n")
	for i := 0; i < 4096; i++ {
		fmt.Fprintf(&sb, "Disallow: /z%04d/%s\n", i, strings.Repeat("q", 120))
	}
	sb.WriteString("Disallow: /target/deep/path\n")
	r, err := FromString(sb.String())
	if err != nil {
		b.Fatalf("parse: %v", err)
	}
	m, err := r.FindGroup("bot").CompileURLMatcher()
	if err != nil {
		b.Fatalf("compile: %v", err)
	}
	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		if _, err := m.Test("http://h/target/deep/path/and/more?x=1"); err != nil {
			b.Fatalf("test: %v", err)
		}
	}
}
