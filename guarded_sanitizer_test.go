package taint

import (
	"fmt"
	"strings"
	"testing"

	"golang.org/x/tools/go/callgraph"
)

// guardedModelYAML renders the guarded-sanitizer model used by the branch,
// pair, and join tests. clean takes a source-derived string and returns
// (cleaned string, error), so result 0 is trustworthy only after the same
// call's error (result 1) is proven nil.
func guardedModelYAML(pkg string) string {
	return fmt.Sprintf(`package: %[1]s
sanitizers:
  - func: %[1]s.clean
    result: 0
    error_result: 1
`, pkg)
}

// guardedFixture loads a benign program through the existing SSA graph
// helpers, applies the guarded sanitizer model, and runs CheckDetailed with
// the package-local source and sink.
func guardedFixture(t *testing.T, src string) (*callgraph.Graph, string, []Model) {
	t.Helper()
	cg, pkg := detailedGraphForSource(t, src)
	models := mustParseModels(t, guardedModelYAML(pkg))
	return cg, pkg, models
}

func runGuarded(t *testing.T, cg *callgraph.Graph, pkg string, models []Model) Diagnostics {
	t.Helper()
	return CheckDetailed(
		cg,
		NewSources(pkg+".source"),
		NewSinks(pkg+".sink"),
		WithModels(models...),
	)
}

// assertGuardedDiagnostics checks the count and, when findings are expected,
// that each keeps the ordinary source/sink localization and evidence rather
// than being dropped from the call chain.
func assertGuardedDiagnostics(t *testing.T, diags Diagnostics, pkg string, want int) {
	t.Helper()
	if len(diags) != want {
		t.Fatalf("diagnostics = %d, want %d (%v)", len(diags), want, diags)
	}
	for i, diag := range diags {
		if got, wantType := diag.Result.SourceType, pkg+".source"; got != wantType {
			t.Fatalf("diagnostic %d source = %q, want %q", i, got, wantType)
		}
		if got, wantType := diag.Result.SinkType, pkg+".sink"; got != wantType {
			t.Fatalf("diagnostic %d sink = %q, want %q", i, got, wantType)
		}
		if !diag.Result.ReportPos().IsValid() {
			t.Fatalf("diagnostic %d has no report position", i)
		}
		if diag.Result.SourceValue == nil {
			t.Fatalf("diagnostic %d lost its source value", i)
		}
		kinds := evidenceKinds(diag.Evidence)
		assertEvidenceContains(t, kinds, EvidenceSourceMatch)
		assertEvidenceContains(t, kinds, EvidenceSinkMatch)
	}
}

// TestGuardedSanitizerBranches covers the branch shapes that decide whether a
// guarded result is trusted: early exit, `err == nil` positive branch, missing
// checks, uses inside the error branch, and uses before the check.
func TestGuardedSanitizerBranches(t *testing.T) {
	for _, tc := range []struct {
		name string
		body string
		want int
	}{
		{
			name: "early exit on error",
			body: `v, err := clean(source())
	if err != nil {
		return
	}
	sink(v)`,
			want: 0,
		},
		{
			name: "positive nil branch",
			body: `v, err := clean(source())
	if err == nil {
		sink(v)
	}`,
			want: 0,
		},
		{
			name: "else return then sink",
			body: `v, err := clean(source())
	if err == nil {
	} else {
		return
	}
	sink(v)`,
			want: 0,
		},
		{
			name: "no error check",
			body: `v, _ := clean(source())
	sink(v)`,
			want: 1,
		},
		{
			name: "use inside error branch",
			body: `v, err := clean(source())
	if err != nil {
		sink(v)
	}`,
			want: 1,
		},
		{
			name: "use before check",
			body: `v, err := clean(source())
	sink(v)
	if err != nil {
		return
	}`,
			want: 1,
		},
		{
			name: "reassigned to raw source after check",
			body: `v, err := clean(source())
	if err != nil {
		return
	}
	v = source()
	sink(v)`,
			want: 1,
		},
		{
			name: "conversion alias of proven result",
			body: `v, err := clean(source())
	if err != nil {
		return
	}
	sink(string(v))`,
			want: 0,
		},
		{
			name: "local load alias of proven result",
			body: `v, err := clean(source())
	if err != nil {
		return
	}
	p := &v
	sink(*p)`,
			want: 0,
		},
		{
			name: "one nested path bypasses the proof",
			body: `v, err := clean(source())
	if cond() {
		if err != nil {
			return
		}
		sink(v)
	} else {
		sink(v)
	}`,
			want: 1,
		},
		{
			name: "nested condition without proof",
			body: `v, err := clean(source())
	if err != nil {
		return
	}
	if cond() {
		sink(v)
	}`,
			want: 0,
		},
		{
			name: "nested only other branch",
			body: `v, err := clean(source())
	if err != nil {
		if cond() {
		}
		return
	}
	sink(v)`,
			want: 0,
		},
		{
			name: "same value used on error path and after proof",
			body: `v, err := clean(source())
	if err != nil {
		sink(v)
		return
	}
	sink(v)`,
			want: 1,
		},
		{
			name: "or condition can reach sink without proof",
			body: `v, err := clean(source())
	if err == nil || cond() {
		sink(v)
	}`,
			want: 1,
		},
		{
			name: "and condition proves before sink",
			body: `v, err := clean(source())
	if err == nil && cond() {
		sink(v)
	}`,
			want: 0,
		},
	} {
		t.Run(tc.name, func(t *testing.T) {
			src := "package main\n\nfunc source() string { return \"user\" }\n\n" +
				"func clean(s string) (string, error) { return s, nil }\n\n" +
				"func cond() bool { return true }\n\n" +
				"func sink(s string) {}\n\n" +
				"func main() {\n" + tc.body + "\n}\n"
			cg, pkg, models := guardedFixture(t, src)
			diags := runGuarded(t, cg, pkg, models)
			assertGuardedDiagnostics(t, diags, pkg, tc.want)
		})
	}
}

// TestGuardedSanitizerPairs covers call identity: results and errors from
// different clean calls must never be cross-paired, even with identical
// arguments, and a reassigned error must not vouch for an older result.
func TestGuardedSanitizerPairs(t *testing.T) {
	for _, tc := range []struct {
		name string
		body string
		want int
	}{
		{
			name: "two calls each checked",
			body: `v1, err1 := clean(source())
	v2, err2 := clean(source())
	if err1 != nil {
		return
	}
	if err2 != nil {
		return
	}
	sink(v1)
	sink(v2)`,
			want: 0,
		},
		{
			name: "cross paired errors",
			body: `v1, err1 := clean(source())
	_, err2 := clean(source())
	_ = v1
	if err2 != nil {
		return
	}
	sink(v1)
	_ = err1`,
			want: 1,
		},
		{
			name: "cross paired results",
			body: `_, err1 := clean(source())
	v2, _ := clean(source())
	if err1 != nil {
		return
	}
	sink(v2)`,
			want: 1,
		},
		{
			name: "same argument still distinct calls",
			body: `src := source()
	v1, _ := clean(src)
	_, err2 := clean(src)
	_ = v1
	if err2 != nil {
		return
	}
	sink(v1)`,
			want: 1,
		},
		{
			name: "reassigned error does not prove old result",
			body: `v, err := clean(source())
	err = nil
	if err == nil {
		sink(v)
	}`,
			want: 1,
		},
		{
			name: "reassigned after check is still proven",
			body: `v, err := clean(source())
	if err != nil {
		return
	}
	err = nil
	sink(v)`,
			want: 0,
		},
		{
			name: "inner call error does not prove outer result",
			body: `v, err := clean(source())
	if wrapped() != nil {
		return
	}
	_ = err
	sink(v)`,
			want: 1,
		},
	} {
		t.Run(tc.name, func(t *testing.T) {
			src := "package main\n\nfunc source() string { return \"user\" }\n\n" +
				"func clean(s string) (string, error) { return s, nil }\n\n" +
				"func wrapped() error { return nil }\n\n" +
				"func sink(s string) {}\n\n" +
				"func main() {\n" + tc.body + "\n}\n"
			cg, pkg, models := guardedFixture(t, src)
			diags := runGuarded(t, cg, pkg, models)
			assertGuardedDiagnostics(t, diags, pkg, tc.want)
		})
	}
}

// TestGuardedSanitizerJoins covers Phi merges and loop back edges. A join is
// trusted only when every incoming edge proves its own sanitizer call; an edge
// that bypasses the check, substitutes the raw source, or carries a value
// from a previous loop iteration must keep reporting.
func TestGuardedSanitizerJoins(t *testing.T) {
	for _, tc := range []struct {
		name string
		body string
		want int
	}{
		{
			name: "both branches prove before join",
			body: `var v string
	if cond() {
		x, err := clean(source())
		if err != nil {
			return
		}
		v = x
	} else {
		x, err := clean(source())
		if err != nil {
			return
		}
		v = x
	}
	sink(v)`,
			want: 0,
		},
		{
			name: "one branch bypasses the check",
			body: `var v string
	if cond() {
		x, err := clean(source())
		if err != nil {
			return
		}
		v = x
	} else {
		x, _ := clean(source())
		v = x
	}
	sink(v)`,
			want: 1,
		},
		{
			name: "one branch substitutes the raw source",
			body: `var v string
	if cond() {
		x, err := clean(source())
		if err != nil {
			return
		}
		v = x
	} else {
		v = source()
	}
	sink(v)`,
			want: 1,
		},
		{
			name: "phi of positively proven results",
			body: `var v string
	if cond() {
		x, err := clean(source())
		if err == nil {
			v = x
		}
	} else {
		x, err := clean(source())
		if err == nil {
			v = x
		}
	}
	sink(v)`,
			want: 1,
		},
		{
			name: "loop back edge does not lend last iteration's proof",
			body: `_, err := clean(source())
	for i := 0; i < 2; i++ {
		prevErr := err
		v, e := clean(source())
		err = e
		if prevErr == nil {
			sink(v)
		}
	}`,
			want: 1,
		},
		{
			name: "loop use guarded in the same iteration",
			body: `for i := 0; i < 2; i++ {
		v, err := clean(source())
		if err != nil {
			continue
		}
		sink(v)
	}`,
			want: 0,
		},
		{
			name: "loop carried result after loop",
			body: `var v string
	var err error
	for i := 0; i < 2; i++ {
		v, err = clean(source())
		if err != nil {
			return
		}
	}
	sink(v)`,
			want: 1,
		},
		{
			name: "loop head use before current check",
			body: `var v string
	var err error
	for i := 0; i < 2; i++ {
		if err == nil {
			sink(v)
		}
		v, err = clean(source())
	}`,
			want: 1,
		},
	} {
		t.Run(tc.name, func(t *testing.T) {
			src := "package main\n\nfunc source() string { return \"user\" }\n\n" +
				"func clean(s string) (string, error) { return s, nil }\n\n" +
				"func cond() bool { return true }\n\n" +
				"func sink(s string) {}\n\n" +
				"func main() {\n" + tc.body + "\n}\n"
			cg, pkg, models := guardedFixture(t, src)
			diags := runGuarded(t, cg, pkg, models)
			assertGuardedDiagnostics(t, diags, pkg, tc.want)
		})
	}
}

// TestGuardedSanitizerModels covers model loading and graceful degradation:
// validation rejects incomplete or contradictory index pairs, unconditional
// and WithSanitizers behavior is unchanged, and out-of-bounds, non-error,
// unresolvable, and dynamically dispatched calls stay conservative without
// crashing or globally suppressing the function's diagnostics.
func TestGuardedSanitizerModels(t *testing.T) {
	t.Run("validation", func(t *testing.T) {
		pkg := "example.com/guarded"
		valid := `
package: example.com/guarded
sanitizers:
  - func: example.com/guarded.clean
    result: 0
    error_result: 1
`
		if _, err := ParseModels(strings.NewReader(valid)); err != nil {
			t.Fatalf("valid guarded sanitizer model rejected: %v", err)
		}
		for _, tc := range []struct {
			name string
			yaml string
		}{
			{"missing error_result", "sanitizers:\n  - func: example.com/guarded.clean\n    result: 0\n"},
			{"missing result", "sanitizers:\n  - func: example.com/guarded.clean\n    error_result: 1\n"},
			{"negative result", "sanitizers:\n  - func: example.com/guarded.clean\n    result: -1\n    error_result: 0\n"},
			{"negative error_result", "sanitizers:\n  - func: example.com/guarded.clean\n    result: 0\n    error_result: -1\n"},
			{"equal indices", "sanitizers:\n  - func: example.com/guarded.clean\n    result: 1\n    error_result: 1\n"},
		} {
			t.Run(tc.name, func(t *testing.T) {
				doc := "package: " + pkg + "\n" + tc.yaml
				if _, err := ParseModels(strings.NewReader(doc)); err == nil {
					t.Fatalf("expected %s model to be rejected", tc.name)
				}
			})
		}
		// A func-only model stays unconditional and validates.
		if _, err := ParseModels(strings.NewReader("package: example.com/guarded\nsanitizers:\n  - func: example.com/guarded.clean\n")); err != nil {
			t.Fatalf("unconditional sanitizer model rejected: %v", err)
		}
	})

	t.Run("programmatic malformed pair fails closed", func(t *testing.T) {
		cg, pkg := detailedGraphForSource(t, `package main

func source() string { return "user" }

func clean(s string) (string, error) { return s, nil }

func sink(s string) {}

func main() {
	v, err := clean(source())
	if err != nil {
		return
	}
	sink(v)
}
`)
		// A Model built in Go can bypass YAML validation. A lone index must
		// not panic and must not be treated as unconditional; it stays
		// unproven so the finding survives.
		one := 0
		models := []Model{{
			Package: pkg,
			Sanitizers: []SanitizerModel{{
				Func:   pkg + ".clean",
				Result: &one,
			}},
		}}
		diags := CheckDetailed(cg, NewSources(pkg+".source"), NewSinks(pkg+".sink"), WithModels(models...))
		assertGuardedDiagnostics(t, diags, pkg, 1)
	})

	t.Run("unconditional behavior unchanged", func(t *testing.T) {
		cg, pkg := detailedGraphForSource(t, `package main

func source() string { return "user" }

func clean(s string) string { return s }

func sink(s string) {}

func main() {
	sink(clean(source()))
}
`)
		unconditional := mustParseModels(t, fmt.Sprintf(`package: %[1]s
sanitizers:
  - func: %[1]s.clean
`, pkg))
		if diags := CheckDetailed(cg, NewSources(pkg+".source"), NewSinks(pkg+".sink"), WithModels(unconditional...)); len(diags) != 0 {
			t.Fatalf("func-only model should stay unconditional, got %d diagnostics", len(diags))
		}
		if diags := CheckDetailed(cg, NewSources(pkg+".source"), NewSinks(pkg+".sink"), WithSanitizers(pkg+".clean")); len(diags) != 0 {
			t.Fatalf("WithSanitizers should stay unconditional, got %d diagnostics", len(diags))
		}
		if diags := CheckDetailed(cg, NewSources(pkg+".source"), NewSinks(pkg+".sink")); len(diags) != 1 {
			t.Fatalf("unconfigured sanitizer should report one diagnostic, got %d", len(diags))
		}
	})

	t.Run("out of bounds indices stay conservative", func(t *testing.T) {
		cg, pkg, _ := guardedFixture(t, `package main

func source() string { return "user" }

func clean(s string) (string, error) { return s, nil }

func sink(s string) {}

func main() {
	v, err := clean(source())
	if err != nil {
		return
	}
	sink(v)
}
`)
		models := mustParseModels(t, fmt.Sprintf(`package: %[1]s
sanitizers:
  - func: %[1]s.clean
    result: 2
    error_result: 3
`, pkg))
		diags := runGuarded(t, cg, pkg, models)
		// Out-of-bounds indices are unproven, so the ordinary source/sink
		// finding survives instead of crashing. The configured index 2 does
		// not name this tuple element, so no rule is attributed to it.
		assertGuardedDiagnostics(t, diags, pkg, 1)
		assertEvidenceContains(t, evidenceKinds(diags[0].Evidence), EvidenceSanitizerRejected)
	})

	t.Run("out of bounds does not suppress an unrelated sink", func(t *testing.T) {
		cg, pkg := detailedGraphForSource(t, `package main

func source() string { return "user" }

func clean(s string) (string, error) { return s, nil }

func sink(s string) {}

func main() {
	v, err := clean(source())
	if err != nil {
		return
	}
	sink(v)
	sink(source())
}
`)
		models := mustParseModels(t, fmt.Sprintf(`package: %[1]s
sanitizers:
  - func: %[1]s.clean
    result: 5
    error_result: 6
`, pkg))
		diags := CheckDetailed(cg, NewSources(pkg+".source"), NewSinks(pkg+".sink"), WithModels(models...))
		// The guarded result's sink stays reported and the independent raw
		// source flow is untouched: no global suppression of the function.
		if len(diags) != 2 {
			t.Fatalf("expected both the guarded-result flow and the raw flow, got %d diagnostics", len(diags))
		}
	})

	t.Run("non-error slot stays conservative", func(t *testing.T) {
		cg, pkg, _ := guardedFixture(t, `package main

func source() string { return "user" }

func clean(s string) (string, string) { return s, "" }

func sink(s string) {}

func main() {
	v, status := clean(source())
	if status == "" {
		sink(v)
	}
}
`)
		models := mustParseModels(t, fmt.Sprintf(`package: %[1]s
sanitizers:
  - func: %[1]s.clean
    result: 0
    error_result: 1
`, pkg))
		diags := runGuarded(t, cg, pkg, models)
		if len(diags) != 1 {
			t.Fatalf("non-error slot should leave one diagnostic, got %d", len(diags))
		}
	})

	t.Run("unresolvable call stays conservative", func(t *testing.T) {
		cg, pkg, _ := guardedFixture(t, `package main

type cleaner interface {
	Clean(string) (string, error)
}

func source() cleaner { return nil }

func sink(s string) {}

func main() {
	c := source()
	v, err := c.Clean("payload")
	if err != nil {
		return
	}
	sink(v)
}
`)
		models := mustParseModels(t, fmt.Sprintf(`package: %[1]s
sanitizers:
  - func: "(%[1]s.cleaner).Clean"
    result: 0
    error_result: 1
`, pkg))
		diags := runGuarded(t, cg, pkg, models)
		// The interface method has no statically resolvable callee, so the
		// guard cannot be proven and the conservative finding remains.
		assertGuardedDiagnostics(t, diags, pkg, 1)
	})

	t.Run("different configured result index", func(t *testing.T) {
		cg, pkg, _ := guardedFixture(t, `package main

func source() string { return "user" }

func clean(s string) (string, string, error) { return "safe", s, nil }

func sink(s string) {}

func main() {
	safe, v, err := clean(source())
	_, _ = safe, err
	if err != nil {
		return
	}
	sink(v)
}
`)
		models := mustParseModels(t, fmt.Sprintf(`package: %[1]s
sanitizers:
  - func: %[1]s.clean
    result: 1
    error_result: 2
`, pkg))
		if diags := runGuarded(t, cg, pkg, models); len(diags) != 0 {
			t.Fatalf("guarded result at configured index should clear the finding, got %d", len(diags))
		}
		// Result index 0 names the constant "safe" return, so the tainted
		// element 1 must not be treated as sanitized.
		models = mustParseModels(t, fmt.Sprintf(`package: %[1]s
sanitizers:
  - func: %[1]s.clean
    result: 0
    error_result: 2
`, pkg))
		if diags := runGuarded(t, cg, pkg, models); len(diags) != 1 {
			t.Fatalf("a different configured result index should not clean the tainted element, got %d", len(diags))
		}
	})

	t.Run("diagnostics stay stable across repeated analyses", func(t *testing.T) {
		src := `package main

func source() string { return "user" }

func clean(s string) (string, error) { return s, nil }

func sink(s string) {}

func main() {
	v, err := clean(source())
	if err != nil {
		return
	}
	sink(v)
}
`
		cg, pkg, models := guardedFixture(t, src)
		first := runGuarded(t, cg, pkg, models)
		if len(first) != 0 {
			t.Fatalf("guarded path should be clean, got %d", len(first))
		}
		// Interleave a configured run that finds a finding (drop the
		// sanitizer model) and then re-check with the guarded model.
		unconfigured := CheckDetailed(cg, NewSources(pkg+".source"), NewSinks(pkg+".sink"))
		if len(unconfigured) != 1 {
			t.Fatalf("unconfigured rerun should find one diagnostic, got %d", len(unconfigured))
		}
		second := runGuarded(t, cg, pkg, models)
		if len(second) != len(first) {
			t.Fatalf("guarded diagnostics changed across runs: %d then %d", len(first), len(second))
		}
		// A reported case must keep the same sink position, source type, and
		// evidence rule when analyzed again with a different configuration in
		// between.
		cg2, pkg2, models2 := guardedFixture(t, `package main

func source() string { return "user" }

func clean(s string) (string, error) { return s, nil }

func sink(s string) {}

func main() {
	v, _ := clean(source())
	sink(v)
}
`)
		reportFirst := runGuarded(t, cg2, pkg2, models2)
		if len(reportFirst) != 1 {
			t.Fatalf("unchecked case should report once, got %d", len(reportFirst))
		}
		if len(reportFirst[0].Result.Path) == 0 {
			t.Fatal("reported case lost its call-graph path")
		}
		_ = CheckDetailed(cg2, NewSources(pkg2+".source"), NewSinks(pkg2+".sink"))
		reportSecond := runGuarded(t, cg2, pkg2, models2)
		if len(reportSecond) != 1 {
			t.Fatalf("unchecked rerun should report once, got %d", len(reportSecond))
		}
		if reportFirst[0].Result.SinkType != reportSecond[0].Result.SinkType ||
			reportFirst[0].Result.SourceType != reportSecond[0].Result.SourceType ||
			reportFirst[0].Result.ReportPos() != reportSecond[0].Result.ReportPos() {
			t.Fatalf("a reported case changed across runs: %#v vs %#v", reportFirst[0].Result, reportSecond[0].Result)
		}
		assertEvidenceRule(t, reportSecond[0].Evidence, EvidenceSanitizerRejected, pkg2+".clean")
	})
}
