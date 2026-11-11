// Copyright 2026 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package demangle

import (
	"bufio"
	"os"
	"strconv"
	"strings"
	"testing"
)

// This file is the contract test for the Rust v0 syntax graph exposed
// by ToRustAST/RustASTToString. Every span, reference target and
// lifetime below is written out as a literal computed by hand against
// the short symbols; expected values are never produced by a second
// call to ToRustAST. The existing Rust corpora are re-run at the end of
// TestRustASTContract_Printer to prove output compatibility.

// findNode walks the child edges of root and returns the first node
// with the given span, or nil. Back-reference edges are deliberately
// not followed here.
func findNode(root *RustNode, span [2]int) *RustNode {
	var found *RustNode
	var walk func(n *RustNode)
	walk = func(n *RustNode) {
		if found != nil || n == nil {
			return
		}
		if n.Span == span {
			found = n
			return
		}
		for _, c := range n.Children {
			walk(c)
		}
	}
	walk(root)
	return found
}

// findIdentifier returns the identifier node with Value value under
// root, or nil.
func findIdentifier(root *RustNode, value string) *RustNode {
	var found *RustNode
	var walk func(n *RustNode)
	walk = func(n *RustNode) {
		if found != nil || n == nil {
			return
		}
		if n.Kind == RustNodeIdentifier && n.Value == value {
			found = n
			return
		}
		for _, c := range n.Children {
			walk(c)
		}
	}
	walk(root)
	return found
}

// collectBackrefs returns every back-reference node reachable through
// child edges.
func collectBackrefs(root *RustNode) []*RustNode {
	var out []*RustNode
	var walk func(n *RustNode)
	walk = func(n *RustNode) {
		if n == nil {
			return
		}
		if n.Kind == RustNodeBackref {
			out = append(out, n)
		}
		for _, c := range n.Children {
			walk(c)
		}
	}
	walk(root)
	return out
}

// findBackref returns the back-reference node with the given span, or
// nil. It is distinct from findNode because a back-reference's span can
// equal its enclosing generic argument's span.
func findBackref(root *RustNode, span [2]int) *RustNode {
	for _, r := range collectBackrefs(root) {
		if r.Span == span {
			return r
		}
	}
	return nil
}

// checkSpans asserts that every span is inside input, that every span
// is non-empty and half-open, and that every child span is contained in
// its parent's.
func checkSpans(t *testing.T, input string, n *RustNode) {
	t.Helper()
	if n == nil {
		t.Fatal("nil node")
	}
	if n.Span[0] < 0 || n.Span[1] <= n.Span[0] || n.Span[1] > len(input) {
		t.Fatalf("node %q has invalid span %v for input of length %d",
			n.Kind, n.Span, len(input))
	}
	for _, c := range n.Children {
		checkSpans(t, input, c)
		if c.Span[0] < n.Span[0] || c.Span[1] > n.Span[1] {
			t.Fatalf("child %q span %v is not contained in parent %q span %v",
				c.Kind, c.Span, n.Kind, n.Span)
		}
	}
}

// requireParseError asserts that ToRustAST fails and that the error
// message names the given input offset.
func requireParseError(t *testing.T, input string, wantOffset int) {
	t.Helper()
	ast, err := ToRustAST(input)
	if err == nil {
		t.Fatalf("ToRustAST(%q) succeeded, want error", input)
	}
	if ast != nil {
		t.Fatalf("ToRustAST(%q) returned a non-nil AST with an error", input)
	}
	if !strings.Contains(err.Error(), "at ") {
		t.Fatalf("error for %q does not locate an offset: %v", input, err)
	}
	if !strings.Contains(err.Error(), "at "+strconv.Itoa(wantOffset)) {
		t.Fatalf("error for %q = %v, want offset %d", input, err, wantOffset)
	}
}

// TestRustASTContract_Structure covers the shape of the graph: node
// kinds, decoded identifier Values, byte spans (including identifier
// length prefixes), the separate instantiating crate and suffix, and
// the fact that printing options never change the parsed structure.
func TestRustASTContract_Structure(t *testing.T) {
	// A simple nested path, with byte intervals marked by hand.
	//
	//	_R N v C 1 a 4 main
	//	0  1 2 3 4 5 6 7...11
	const simple = "_RNvC1a4main"
	ast, err := ToRustAST(simple)
	if err != nil {
		t.Fatalf("ToRustAST(%q): %v", simple, err)
	}
	if ast.InstantiatingCrate != nil || ast.Suffix != "" {
		t.Fatalf("unexpected crate/suffix: %+v %q", ast.InstantiatingCrate, ast.Suffix)
	}
	checkSpans(t, simple, ast.Root)

	if ast.Root.Kind != RustNodePath || ast.Root.Span != [2]int{2, 12} {
		t.Fatalf("root = %q %v, want path [2 12]", ast.Root.Kind, ast.Root.Span)
	}
	ns := findNode(ast.Root, [2]int{3, 4})
	if ns == nil || ns.Kind != RustNodeNamespace || ns.Value != "v" {
		t.Fatalf("namespace node missing or wrong: %+v", ns)
	}
	cratePath := findNode(ast.Root, [2]int{4, 7})
	if cratePath == nil || cratePath.Kind != RustNodePath || cratePath.Value != "crate" {
		t.Fatalf("crate path missing or wrong: %+v", cratePath)
	}
	// Identifier spans include the length encoding: byte 5 is the
	// length digit "1", byte 6 is the identifier "a".
	idA := findNode(ast.Root, [2]int{5, 7})
	if idA == nil || idA.Kind != RustNodeIdentifier || idA.Value != "a" ||
		simple[idA.Span[0]] != '1' {
		t.Fatalf("identifier a missing or span omits length prefix: %+v", idA)
	}
	idMain := findNode(ast.Root, [2]int{7, 12})
	if idMain == nil || idMain.Kind != RustNodeIdentifier || idMain.Value != "main" ||
		simple[idMain.Span[0]] != '4' {
		t.Fatalf("identifier main missing or span omits length prefix: %+v", idMain)
	}

	// The instantiating crate is a separate node and never enters the
	// main path's printed name.
	const withCrate = "_RC5crateC3foo"
	cast, err := ToRustAST(withCrate)
	if err != nil {
		t.Fatalf("ToRustAST(%q): %v", withCrate, err)
	}
	checkSpans(t, withCrate, cast.Root)
	if cast.InstantiatingCrate == nil {
		t.Fatal("missing instantiating crate")
	}
	if cast.InstantiatingCrate.Span != [2]int{9, 14} {
		t.Fatalf("instantiating crate span = %v, want [9 14]", cast.InstantiatingCrate.Span)
	}
	fooID := findNode(cast.InstantiatingCrate, [2]int{10, 14})
	if fooID == nil || fooID.Value != "foo" {
		t.Fatalf("instantiating crate identifier wrong: %+v", fooID)
	}
	if s, err := RustASTToString(cast); err != nil || s != "crate" {
		t.Fatalf("instantiating crate leaked into name: %q %v", s, err)
	}

	// The dot suffix is recorded verbatim and is only rendered in LLVM
	// style.
	const withSuffix = "_RC3dot.llvm.1234"
	sast, err := ToRustAST(withSuffix)
	if err != nil {
		t.Fatalf("ToRustAST(%q): %v", withSuffix, err)
	}
	if sast.Suffix != ".llvm.1234" {
		t.Fatalf("suffix = %q", sast.Suffix)
	}
	if s, _ := RustASTToString(sast); s != "dot" {
		t.Fatalf("default suffix rendering = %q, want dot", s)
	}
	if s, _ := RustASTToString(sast, LLVMStyle); s != "dot (.llvm.1234)" {
		t.Fatalf("LLVM suffix rendering = %q", s)
	}

	// Punycode is decoded into Value; the span covers the "u" marker,
	// the length, the underscore and the raw bytes.
	const puny = "_RNvC8punycodeu7_1lqs71d"
	past, err := ToRustAST(puny)
	if err != nil {
		t.Fatalf("ToRustAST(%q): %v", puny, err)
	}
	checkSpans(t, puny, past.Root)
	tokyo := findNode(past.Root, [2]int{14, 24})
	if tokyo == nil || tokyo.Kind != RustNodeIdentifier || tokyo.Value != "東京" ||
		puny[tokyo.Span[0]] != 'u' {
		t.Fatalf("punycode identifier wrong: %+v", tokyo)
	}

	// NoTemplateParams is purely a printing option: the generic
	// arguments are still present in the graph.
	const generic = "_RINvC3foo3barlE"
	gast, err := ToRustAST(generic)
	if err != nil {
		t.Fatalf("ToRustAST(%q): %v", generic, err)
	}
	args := findNode(gast.Root, [2]int{14, 15})
	if args == nil || args.Kind != RustNodeGenericArgs || len(args.Children) != 1 {
		t.Fatalf("generic arguments missing despite parse-time check: %+v", args)
	}
	if s, _ := RustASTToString(gast); s != "foo::bar::<i32>" {
		t.Fatalf("default generic print = %q", s)
	}
	if s, _ := RustASTToString(gast, NoTemplateParams); s != "foo::bar::<>" {
		t.Fatalf("NoTemplateParams print = %q", s)
	}

	// Non-Rust-v0 inputs are reported as not a mangled Rust name.
	if _, err := ToRustAST("not_a_symbol"); err != ErrNotMangledName {
		t.Fatalf("want ErrNotMangledName, got %v", err)
	}

	// The input string is never modified.
	original := simple
	mutable := simple
	if _, err := ToRustAST(mutable); err != nil {
		t.Fatal(err)
	}
	if mutable != original {
		t.Fatalf("input mutated: %q != %q", mutable, original)
	}
}

// TestRustASTContract_Backrefs covers shared (not copied) reference
// targets, role-specific path/type/const references, rejection of
// forward/self/mid-identifier references, and the rule that equal
// printed text never causes node merging.
func TestRustASTContract_Backrefs(t *testing.T) {
	// Two textually identical "foo" identifiers must remain distinct
	// nodes; printing equality must not merge them.
	const twoFoo = "_RIC3fooC3fooE"
	ast, err := ToRustAST(twoFoo)
	if err != nil {
		t.Fatalf("ToRustAST(%q): %v", twoFoo, err)
	}
	first := findNode(ast.Root, [2]int{4, 8})
	second := findNode(ast.Root, [2]int{9, 13})
	if first == nil || second == nil || first == second {
		t.Fatalf("identical-text identifiers were merged or lost: %p %p", first, second)
	}
	if first.Value != "foo" || second.Value != "foo" {
		t.Fatalf("identifier values = %q %q", first.Value, second.Value)
	}

	// Renaming one leaves the other untouched, proving they are
	// separate nodes even though their text matches.
	second.Value = "bar"
	if s, _ := RustASTToString(ast); s != "foo::<bar>" {
		t.Fatalf("rename of one identical-text node changed the other: %q", s)
	}
	second.Value = "foo"

	// Two identical const payloads are also distinct nodes.
	const twoConst = "_RIC3fooKi7_Ki7_E"
	cast, err := ToRustAST(twoConst)
	if err != nil {
		t.Fatalf("ToRustAST(%q): %v", twoConst, err)
	}
	c1 := findNode(cast.Root, [2]int{9, 12})
	c2 := findNode(cast.Root, [2]int{13, 16})
	if c1 == nil || c2 == nil || c1 == c2 || c1.Kind != RustNodeConst {
		t.Fatalf("identical consts merged or lost: %p %p", c1, c2)
	}

	// Two back-references to one target share the exact same pointer,
	// and back-reference nodes carry no children of their own.
	const shared = "_RIC3fooB0_B0_E"
	shast, err := ToRustAST(shared)
	if err != nil {
		t.Fatalf("ToRustAST(%q): %v", shared, err)
	}
	refs := collectBackrefs(shast.Root)
	if len(refs) != 2 {
		t.Fatalf("want 2 backrefs, got %d", len(refs))
	}
	if refs[0].Span != [2]int{8, 11} || refs[1].Span != [2]int{11, 14} {
		t.Fatalf("backref spans = %v %v", refs[0].Span, refs[1].Span)
	}
	if len(refs[0].Children) != 0 || len(refs[1].Children) != 0 {
		t.Fatal("back-reference nodes must not carry children")
	}
	if refs[0].Ref == nil || refs[0].Ref != refs[1].Ref {
		t.Fatal("backrefs to one target do not share the same node")
	}
	if refs[0].Ref.Span != [2]int{3, 8} || refs[0].Ref.Kind != RustNodePath {
		t.Fatalf("shared target = %+v", refs[0].Ref)
	}
	if s, _ := RustASTToString(shast); s != "foo::<foo, foo>" {
		t.Fatalf("shared target print = %q", s)
	}

	// Role-specific targets: path, basic type and constant.
	pathAst, err := ToRustAST("_RINvC3foo3barB2_E")
	if err != nil {
		t.Fatal(err)
	}
	pref := findBackref(pathAst.Root, [2]int{14, 17})
	if pref == nil || pref.Kind != RustNodeBackref ||
		pref.Ref == nil || pref.Ref.Kind != RustNodePath ||
		pref.Ref.Span != [2]int{5, 10} {
		t.Fatalf("path backref wrong: %+v", pref)
	}

	typeAst, err := ToRustAST("_RIC7backrefuB9_E")
	if err != nil {
		t.Fatal(err)
	}
	tref := findBackref(typeAst.Root, [2]int{13, 16})
	if tref == nil || tref.Kind != RustNodeBackref ||
		tref.Ref == nil || tref.Ref.Kind != RustNodeType ||
		tref.Ref.Span != [2]int{12, 13} {
		t.Fatalf("type backref wrong: %+v", tref)
	}
	if s, _ := RustASTToString(typeAst); s != "backref::<(), ()>" {
		t.Fatalf("type backref print = %q", s)
	}

	constAst, err := ToRustAST("_RIC7backrefKi7_KBa_E")
	if err != nil {
		t.Fatal(err)
	}
	kref := findBackref(constAst.Root, [2]int{17, 20})
	if kref == nil || kref.Kind != RustNodeBackref ||
		kref.Ref == nil || kref.Ref.Kind != RustNodeConst ||
		kref.Ref.Span != [2]int{13, 16} {
		t.Fatalf("const backref wrong: %+v", kref)
	}
	if s, _ := RustASTToString(constAst); s != "backref::<7, 7>" {
		t.Fatalf("const backref print = %q", s)
	}

	// A back-reference nested in an N path resolves to the enclosing
	// crate path rather than copying it.
	nestAst, err := ToRustAST("_RIC7backrefNvB0_5identE")
	if err != nil {
		t.Fatal(err)
	}
	nref := findBackref(nestAst.Root, [2]int{14, 17})
	if nref == nil || nref.Ref == nil || nref.Ref.Span != [2]int{3, 12} {
		t.Fatalf("nested path backref wrong: %+v", nref)
	}

	// Forward, self, role-mismatched and mid-identifier references are
	// all rejected with a located offset.
	requireParseError(t, "_RB_", 2)                 // index 0 at position 0: self/forward
	requireParseError(t, "_RB5_", 2)                // forward reference
	requireParseError(t, "_RNvB_1a", 4)             // self reference in a path
	requireParseError(t, "_RIC7backrefSB9_E", 13)   // type role, bad target
	requireParseError(t, "_RIC7backrefKBa_E", 13)   // const role pointing at a type
	requireParseError(t, "_RIC3fooB4_E", 8)         // offset lands inside an identifier
	requireParseError(t, "_RINvC3foo3barB99_E", 14) // bad ref hidden inside generics

	// NoTemplateParams cannot let a broken hidden reference bypass the
	// structural build.
	if _, err := ToString("_RINvC3foo3barB99_E", NoTemplateParams); err == nil {
		t.Fatal("NoTemplateParams masked a broken back-reference")
	}
}

// TestRustASTContract_Binders covers binder scope structure, raw
// lifetime indices kept on the nodes, nested binders, dyn-bound
// scopes, and the no-cached-name rule when a shared subtree is reached
// under different binder environments.
func TestRustASTContract_Binders(t *testing.T) {
	// Nested function-signature binders.
	const nested = "_RIC7bindersFG0_EFG0_RL3_pRL0_pEuE"
	ast, err := ToRustAST(nested)
	if err != nil {
		t.Fatalf("ToRustAST(%q): %v", nested, err)
	}
	checkSpans(t, nested, ast.Root)

	binder1 := findNode(ast.Root, [2]int{13, 16})
	binder2 := findNode(ast.Root, [2]int{18, 21})
	if binder1 == nil || binder1.Kind != RustNodeBinder || binder1.Value != "2" {
		t.Fatalf("outer binder wrong: %+v", binder1)
	}
	if binder2 == nil || binder2.Kind != RustNodeBinder || binder2.Value != "2" {
		t.Fatalf("inner binder wrong: %+v", binder2)
	}

	// Lifetime nodes keep the raw encoded index; the printable name is
	// the printer's job.
	life4 := findNode(ast.Root, [2]int{22, 25})
	life1 := findNode(ast.Root, [2]int{27, 30})
	if life4 == nil || life4.Kind != RustNodeLifetime || life4.Value != "4" {
		t.Fatalf("raw lifetime index 4 wrong: %+v", life4)
	}
	if life1 == nil || life1.Kind != RustNodeLifetime || life1.Value != "1" {
		t.Fatalf("raw lifetime index 1 wrong: %+v", life1)
	}

	want := "binders::<for<'a, 'b> fn() -> for<'c, 'd> fn(&'a _, &'d _)>"
	if s, _ := RustASTToString(ast); s != want {
		t.Fatalf("nested binder print = %q, want %q", s, want)
	}

	// A dyn-bound binder is scoped to the bound list; the trailing D
	// lifetime is resolved against the outer fn binder.
	const dyn = "_RIC5traitFG_DG_EL0_EuE"
	dast, err := ToRustAST(dyn)
	if err != nil {
		t.Fatalf("ToRustAST(%q): %v", dyn, err)
	}
	dynBinder := findNode(dast.Root, [2]int{14, 16})
	if dynBinder == nil || dynBinder.Kind != RustNodeBinder || dynBinder.Value != "1" {
		t.Fatalf("dyn binder wrong: %+v", dynBinder)
	}
	trailing := findNode(dast.Root, [2]int{17, 20})
	if trailing == nil || trailing.Kind != RustNodeLifetime || trailing.Value != "1" {
		t.Fatalf("dyn trailing lifetime wrong: %+v", trailing)
	}
	if s, _ := RustASTToString(dast); s != "trait::<for<'a> fn(dyn for<'b> + 'a)>" {
		t.Fatalf("dyn binder print = %q", s)
	}

	// One shared reference type reached under two different binders.
	// The raw lifetime index (1) is identical on the single shared
	// node, yet it must print as 'a in the one-lifetime scope and 'c in
	// the two-lifetime scope: names are recomputed, never cached.
	const cross = "_RIC4dutyFG_RL0_pEFG0_B9_EuE"
	xast, err := ToRustAST(cross)
	if err != nil {
		t.Fatalf("ToRustAST(%q): %v", cross, err)
	}
	sharedRef := findNode(xast.Root, [2]int{12, 17})
	if sharedRef == nil || sharedRef.Kind != RustNodeType || sharedRef.Value != "ref" {
		t.Fatalf("shared reference type missing: %+v", sharedRef)
	}
	xref := findBackref(xast.Root, [2]int{22, 25})
	if xref == nil || xref.Kind != RustNodeBackref || xref.Ref != sharedRef {
		t.Fatalf("cross-binder backref does not point at the shared ref node: %+v", xref)
	}
	out, err := RustASTToString(xast)
	if err != nil {
		t.Fatal(err)
	}
	wantCross := "duty::<for<'a> fn(&'a _) -> for<'b, 'c> fn(&'c _)>"
	if out != wantCross {
		t.Fatalf("cross-binder print = %q, want %q", out, wantCross)
	}
	// Re-printing must be deterministic and must not leak scope state.
	if again, _ := RustASTToString(xast); again != out {
		t.Fatalf("second print differed: %q vs %q", again, out)
	}
	// A subsequent unrelated parse/print must not inherit any state.
	if s, err := RustASTToString(ast); err != nil || s != want {
		t.Fatalf("state leaked across calls: %q %v", s, err)
	}

	// A binder introducing too many lifetimes is rejected at parse.
	requireParseError(t, "_RIC7bindersFGFF_EuE", 17)

	// A lifetime index with no introducing binder is rejected. The
	// index 1 (L0_) has no binder in scope, matching the corpus symbol
	// that must demangle to an error.
	requireParseError(t, "_RIC16generic_lifetimeL0_E", 22)
}

// TestRustASTContract_Printer covers rendering from decoded node
// contents (including rename propagation), the supported options,
// graceful errors on malformed hand-built graphs, and a round trip over
// the whole existing Rust corpus.
func TestRustASTContract_Printer(t *testing.T) {
	// Hand-written symbols with literal expected output, covering the
	// productions the printer must preserve.
	cases := []struct {
		input string
		want  string
	}{
		{"_RNCC5crate0", "crate::{closure#0}"},
		{"_RNCC5crates_0", "crate::{closure#1}"},
		{"_RNCC5crate3foo", "crate::{closure:foo#0}"},
		{"_RNSC5crate5reify", "crate::{shim:reify#0}"},
		{"_RNZC5crates8_5ident", "crate::{Z:ident#10}"},
		{"_RMC5cratep", "<_>"},
		{"_RXC5cratepC5Clone", "<_ as Clone>"},
		{"_RYpC3Ord", "<_ as Ord>"},
		{"_RIC5typesAhj0_E", "types::<[u8; 0]>"},
		{"_RIC5typesSpE", "types::<[_]>"},
		{"_RIC5typesTEE", "types::<()>"},
		{"_RIC5typesTpEE", "types::<(_,)>"},
		{"_RIC5typesTppEE", "types::<(_, _)>"},
		{"_RIC5typesRpE", "types::<&_>"},
		{"_RIC5typesQpE", "types::<&mut _>"},
		{"_RIC5typesPpE", "types::<*const _>"},
		{"_RIC5typesOpE", "types::<*mut _>"},
		{"_RIC8functionFEuE", "function::<fn()>"},
		{"_RIC8functionFUEuE", "function::<unsafe fn()>"},
		{"_RIC8functionFKCEuE", `function::<extern "C" fn()>`},
		{"_RIC8functionFK5cdeclEuE", `function::<extern "cdecl" fn()>`},
		{"_RIC5traitDEL_E", "trait::<dyn >"},
		{"_RIC5traitDC7DisplayEL_E", "trait::<dyn Display>"},
		{"_RIC5traitDC12IntoIteratorp4ItempEL_E",
			"trait::<dyn IntoIterator<Item = _>>"},
		{"_RIC5traitDIC12IntoIteratorpEp4ItempEL_E",
			"trait::<dyn IntoIterator<_, Item = _>>"},
		{"_RIC7integerKi0_E", "integer::<0>"},
		{"_RIC7integerKin1_E", "integer::<-1>"},
		{"_RIC4boolKb1_E", "bool::<true>"},
		{"_RIC4charKc61_E", "char::<'a'>"},
		{"_RIC16generic_lifetimeL_E", "generic_lifetime::<'_>"},
		{"_RNvC8punycodeu7_1lqs71d", "punycode::東京"},
	}
	for _, tc := range cases {
		ast, err := ToRustAST(tc.input)
		if err != nil {
			t.Errorf("ToRustAST(%q): %v", tc.input, err)
			continue
		}
		got, err := RustASTToString(ast)
		if err != nil {
			t.Errorf("RustASTToString(%q): %v", tc.input, err)
			continue
		}
		if got != tc.want {
			t.Errorf("input %q: got %q, want %q", tc.input, got, tc.want)
		}
	}

	// NoTemplateParams on the large template-heavy symbol.
	if got, err := ToString(rustMangledTemplates, NoTemplateParams); err != nil ||
		got != "<timely::dataflow::operators::generic::builder_rc::OperatorBuilder<>>::build_reschedule::<>::{closure#0}" {
		t.Fatalf("large NoTemplateParams output = %q, err %v", got, err)
	}

	// MaxLength truncates the rendered text but never the structural
	// validation, and a malformed input still errors under MaxLength.
	const trunc = "_RNvC1a4main"
	tast, err := ToRustAST(trunc)
	if err != nil {
		t.Fatal(err)
	}
	if s, _ := RustASTToString(tast, MaxLength(1)); s != "a:" {
		t.Fatalf("MaxLength output = %q, want first 2 bytes", s)
	}
	// Truncation never short-circuits validation: a malformed symbol
	// still returns an error even with MaxLength set.
	if _, err := ToString("_RNvC1a5main", MaxLength(1)); err == nil {
		t.Fatal("MaxLength truncated a malformed symbol into a success")
	}

	// Renaming an identifier changes both the direct occurrence and the
	// back-reference that points at its containing node, while the rest
	// of the structure (spans, kinds) is preserved.
	rast, err := ToRustAST("_RINvC3foo3barB2_E")
	if err != nil {
		t.Fatal(err)
	}
	foo := findIdentifier(rast.Root, "foo")
	if foo == nil || foo.Span != [2]int{6, 10} {
		t.Fatalf("foo identifier not found: %+v", foo)
	}
	foo.Value = "qux"
	if s, _ := RustASTToString(rast); s != "qux::bar::<qux>" {
		t.Fatalf("rename did not propagate through the back-reference: %q", s)
	}
	if foo.Span != [2]int{6, 10} || foo.Kind != RustNodeIdentifier {
		t.Fatal("renaming Value altered structural metadata")
	}

	// Malformed hand-built graphs produce errors instead of crashing.
	mustPrintError(t, nil)
	mustPrintError(t, &RustAST{})
	cyc := &RustNode{Kind: RustNodeType, Value: "tuple"}
	cyc.Children = []*RustNode{cyc}
	mustPrintError(t, &RustAST{Root: cyc})
	mustPrintError(t, &RustAST{Root: &RustNode{Kind: RustNodePath, Value: "crate"}})
	mustPrintError(t, &RustAST{Root: &RustNode{Kind: RustNodeBackref}})
	mustPrintError(t, &RustAST{Root: &RustNode{Kind: "bogus"}})

	// A back-reference cycle is an error, even when it sits inside a
	// subtree that an option would skip, and must never overflow.
	b1 := &RustNode{Kind: RustNodeBackref}
	b2 := &RustNode{Kind: RustNodeBackref}
	b1.Ref, b2.Ref = b2, b1
	mustPrintError(t, &RustAST{Root: b1})
	cratePath := &RustNode{Kind: RustNodePath, Value: "crate",
		Children: []*RustNode{{Kind: RustNodeIdentifier, Value: "foo"}}}
	args := &RustNode{Kind: RustNodeGenericArgs,
		Children: []*RustNode{{Kind: RustNodeGenericArg, Children: []*RustNode{b1}}}}
	genericRoot := &RustNode{Kind: RustNodePath,
		Children: []*RustNode{cratePath, args}}
	if _, err := RustASTToString(&RustAST{Root: genericRoot}, NoTemplateParams); err == nil {
		t.Fatal("back-ref cycle hidden by NoTemplateParams was not rejected")
	}

	// Filter keeps returning the original input when the structural
	// parse rejects a symbol.
	if got := Filter("_RNvC1a5main"); got != "_RNvC1a5main" {
		t.Fatalf("Filter on invalid symbol = %q", got)
	}

	runRustCorpusRoundTrip(t)
}

// mustPrintError asserts that rendering ast fails without panicking.
func mustPrintError(t *testing.T, ast *RustAST) {
	t.Helper()
	defer func() {
		if r := recover(); r != nil {
			t.Fatalf("RustASTToString panicked on malformed graph: %v", r)
		}
	}()
	if _, err := RustASTToString(ast); err == nil {
		t.Fatalf("RustASTToString(%+v) succeeded, want error", ast)
	}
}

// runRustCorpusRoundTrip re-runs every Rust v0 symbol shipped in the
// test data through the new entry points and checks equivalence with
// the established ToString output under the default, NoTemplateParams
// and MaxLength options. Symbols that ToString rejects must also be
// rejected by ToRustAST.
func runRustCorpusRoundTrip(t *testing.T) {
	t.Helper()

	check := func(input string) {
		ref, refErr := ToString(input)
		ast, astErr := ToRustAST(input)
		switch {
		case refErr != nil:
			if astErr == nil {
				t.Errorf("%q: ToString rejected but ToRustAST accepted", input)
			}
			return
		case astErr != nil:
			t.Errorf("%q: ToRustAST failed while ToString succeeded: %v", input, astErr)
			return
		}

		if got, err := RustASTToString(ast); err != nil || got != ref {
			t.Errorf("%q: default got %q (%v), want %q", input, got, err, ref)
		}
		if got, err := RustASTToString(ast, NoTemplateParams); err != nil {
			t.Errorf("%q: NoTemplateParams error: %v", input, err)
		} else if want, _ := ToString(input, NoTemplateParams); got != want {
			t.Errorf("%q: NoTemplateParams got %q, want %q", input, got, want)
		}
		if len(ref) > 64 {
			if got, err := RustASTToString(ast, MaxLength(6)); err != nil {
				t.Errorf("%q: MaxLength error: %v", input, err)
			} else if want, _ := ToString(input, MaxLength(6)); got != want {
				t.Errorf("%q: MaxLength got %q, want %q", input, got, want)
			}
		}
	}

	// rust-demangle-expected: option/input/expect triples.
	f, err := os.Open(rustFilename)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	scanner := bufio.NewScanner(f)
	lineno := 1
	for {
		format, ok := getOptLine(t, scanner, &lineno)
		if !ok {
			break
		}
		report := lineno
		input := getLine(t, scanner, &lineno)
		_ = getLine(t, scanner, &lineno)
		if skipExpectedRustTest(t, format, input, report) {
			continue
		}
		// ToRustAST covers only Rust v0; legacy _ZN symbols keep their
		// own demangler and are validated by TestRustExpected instead.
		if !strings.HasPrefix(input, "_R") {
			continue
		}
		check(input)
	}
	if err := scanner.Err(); err != nil {
		t.Fatal(err)
	}

	// rust.test: CHECK/input pairs.
	f2, err := os.Open(rustCheckFilename)
	if err != nil {
		t.Fatal(err)
	}
	defer f2.Close()
	s2 := bufio.NewScanner(f2)
	for s2.Scan() {
		line := strings.TrimSpace(s2.Text())
		if !strings.HasPrefix(line, "CHECK: ") {
			continue
		}
		if !s2.Scan() {
			t.Fatalf("%s: unexpected EOF", rustCheckFilename)
		}
		input := strings.TrimSpace(s2.Text())
		if strings.HasPrefix(input, "_R") {
			check(input)
		}
	}
	if err := s2.Err(); err != nil {
		t.Fatal(err)
	}
}
