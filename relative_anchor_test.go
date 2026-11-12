package cascadia

import (
	"reflect"
	"strings"
	"testing"

	"golang.org/x/net/html"
)

// idOf returns the element's id attribute, or the empty string.
func idOf(n *html.Node) string {
	for _, a := range n.Attr {
		if a.Key == "id" {
			return a.Val
		}
	}
	return ""
}

// nodeByID finds the first element with the given id in n's subtree.
func nodeByID(n *html.Node, id string) *html.Node {
	if n.Type == html.ElementNode && idOf(n) == id {
		return n
	}
	for c := n.FirstChild; c != nil; c = c.NextSibling {
		if found := nodeByID(c, id); found != nil {
			return found
		}
	}
	return nil
}

// queryIDs parses query and returns the ids of the elements it selects,
// in document order.
func queryIDs(t *testing.T, doc *html.Node, query string) []string {
	t.Helper()
	s, err := Parse(query)
	if err != nil {
		t.Fatalf("Parse(%q): %v", query, err)
	}
	return matchedIDs(doc, s)
}

// matchedIDs runs a compiled selector through QueryAll and collects ids.
func matchedIDs(doc *html.Node, m Matcher) []string {
	var ids []string
	for _, n := range QueryAll(doc, m) {
		ids = append(ids, idOf(n))
	}
	return ids
}

func assertIDs(t *testing.T, doc *html.Node, query string, want []string) {
	t.Helper()
	got := queryIDs(t, doc, query)
	if !reflect.DeepEqual(got, want) {
		t.Errorf("%s: got %v, want %v", query, got, want)
	}
}

// TestRelativeAnchorContract pins down the anchoring rules for the relative
// selectors accepted by :has() and :haschild(): the first combinator of each
// comma branch is evaluated against the element being tested, and only the
// rest of the chain may walk away from it.
func TestRelativeAnchorContract(t *testing.T) {
	// The baseline document: two article cards followed by an aside, all
	// wrapped in a .outside element. The whitespace between elements produces
	// text nodes that must not count as element siblings.
	doc := mustParseRelativeDoc(t, `<main class="outside">
<article id="a"><h2><span class="flag">x</span></h2></article>
<article id="b"></article>
<aside><span class="flag">y</span></aside>
</main>`)

	// Four kinds of first step.
	assertIDs(t, doc, `article:has(.flag)`, []string{"a"})           // implicit descendant
	assertIDs(t, doc, `article:has(> h2 .flag)`, []string{"a"})      // explicit child, then descendant
	assertIDs(t, doc, `article:has(> h2 > .flag)`, []string{"a"})    // two child steps
	assertIDs(t, doc, `article:has(+ aside .flag)`, []string{"b"})   // adjacent sibling, then descendant
	assertIDs(t, doc, `article:has(+ aside > .flag)`, []string{"b"}) // adjacent sibling, then child
	assertIDs(t, doc, `article:has(~ aside)`, []string{"a", "b"})    // both cards precede the aside
	assertIDs(t, doc, `article:has(+ article)`, []string{"a"})
	assertIDs(t, doc, `article:has(~ article)`, []string{"a"})

	// The anchor itself and the anchor's ancestors are never candidates for
	// the descendant first step, even though .outside is an ancestor of the
	// cards and itself carries the class.
	assertIDs(t, doc, `article:has(.outside .flag)`, nil)
	assertIDs(t, doc, `article:has(> span.flag)`, nil) // the span is not a direct child
	assertIDs(t, doc, `article:has(+ h2)`, nil)        // h2 lives inside article a, not next to it

	// Several comma branches: one success is enough, and a candidate matched
	// by several branches is still returned once.
	assertIDs(t, doc, `article:has(.flag, + aside .flag)`, []string{"a", "b"})
	assertIDs(t, doc, `article:has(.flag, > h2 .flag)`, []string{"a"})

	// Mixed chains longer than two compounds.
	mixed := mustParseRelativeDoc(t, `<article id="c"><h2>Title</h2><p><a href="#">link</a></p></article>
<article id="d"><div><h2>Title</h2></div><p><a href="#">link</a></p></article>`)
	assertIDs(t, mixed, `article:has(> h2 + p a)`, []string{"c"})
	assertIDs(t, mixed, `article:has(> h2 + p > a)`, []string{"c"})
	assertIDs(t, mixed, `article:has(> h2)`, []string{"c"}) // d's h2 is a deeper descendant
	assertIDs(t, mixed, `article:has(> p a)`, []string{"c", "d"})
	assertIDs(t, mixed, `article:has(div h2)`, []string{"d"})
	assertIDs(t, mixed, `article:has(> div > h2)`, []string{"d"})

	// The anchor element is not its own descendant, and ordinary compound
	// conditions, :not(), :is() and positional pseudo-classes keep working
	// anywhere along the chain.
	cond := mustParseRelativeDoc(t, `<article id="e" class="flag"><h2 class="skip"></h2><p></p></article>
<article id="f"><h2></h2><p class="flag"></p></article>`)
	assertIDs(t, cond, `article:has(.flag)`, []string{"f"}) // e carries .flag itself, not in a descendant
	assertIDs(t, cond, `article:has(> h2:not(.skip) + p)`, []string{"f"})
	assertIDs(t, cond, `article:has(> :is(h2, h3) + p)`, []string{"e", "f"})
	assertIDs(t, cond, `article:has(> h2:first-child)`, []string{"e", "f"})
	assertIDs(t, cond, `article:has(> p:first-child)`, nil)

	// Comments and whitespace text nodes do not occupy an element-sibling
	// position; an actual element in between does break adjacency.
	sib := mustParseRelativeDoc(t, `<div id="box"><!-- lead --> <i id="i1"></i> <!-- mid --> text <b id="b1"></b></div>
<div id="box2"><i id="i2"></i><em id="e2"></em><b id="b2"></b></div>`)
	assertIDs(t, sib, `i:has(+ b)`, []string{"i1"})
	assertIDs(t, sib, `div:has(> i + b)`, []string{"box"})
	assertIDs(t, sib, `i:has(~ b)`, []string{"i1", "i2"})

	// :haschild() keeps the implicit child first step, but the rest of the
	// chain is an ordinary, possibly complex relative chain. An explicit
	// combinator still overrides the default.
	assertIDs(t, doc, `article:haschild(h2 .flag)`, []string{"a"})
	assertIDs(t, doc, `article:haschild(+ article)`, []string{"a"})
	assertIDs(t, mixed, `article:haschild(h2)`, []string{"c"}) // d's h2 is not a direct child
	assertIDs(t, mixed, `article:haschild(div > h2)`, []string{"d"})

	// A single compiled selector is reusable on different trees and always
	// reads the trees as they currently are: there is no cached result.
	sel, err := Compile(`article:has(+ aside .flag)`)
	if err != nil {
		t.Fatalf("Compile: %v", err)
	}
	tree1 := mustParseRelativeDoc(t, `<main><article id="x"></article><article id="y"></article><aside id="side"><span class="flag">z</span></aside></main>`)
	tree2 := mustParseRelativeDoc(t, `<main><article id="x"></article><article id="y"></article><aside id="side"><span class="flag">z</span></aside></main>`)
	if got := matchedIDs(tree1, sel); !reflect.DeepEqual(got, []string{"y"}) {
		t.Fatalf("tree1: got %v, want [y]", got)
	}
	if got := matchedIDs(tree2, sel); !reflect.DeepEqual(got, []string{"y"}) {
		t.Fatalf("tree2 with the same compiled selector: got %v, want [y]", got)
	}

	aside := nodeByID(tree1, "side")
	x := nodeByID(tree1, "x")
	main := x.Parent
	main.RemoveChild(aside)
	if got := matchedIDs(tree1, sel); len(got) != 0 {
		t.Fatalf("after removing the aside: got %v, want no match", got)
	}
	main.AppendChild(aside) // order is now x, y, aside again: y matches once more
	if got := matchedIDs(tree1, sel); !reflect.DeepEqual(got, []string{"y"}) {
		t.Fatalf("after re-appending the aside: got %v, want [y]", got)
	}
	main.RemoveChild(x)
	main.InsertBefore(x, aside) // order is now y, x, aside: the adjacent article is x
	if got := matchedIDs(tree1, sel); !reflect.DeepEqual(got, []string{"x"}) {
		t.Fatalf("after moving x between y and the aside: got %v, want [x]", got)
	}
	// Per-node Match on the same compiled object reflects the new order too.
	if !sel.Match(x) || sel.Match(nodeByID(tree1, "y")) {
		t.Fatal("per-node Match did not follow the mutated sibling order")
	}

	// Serialization round trip and specificity survive a re-parse.
	roundTrips := []string{
		`article:has(.flag)`,
		`article:has(> h2 .flag)`,
		`article:has(+ aside .flag)`,
		`article:has(~ aside)`,
		`article:has(> h2 + p a)`,
		`article:has(.flag, + aside > .flag)`,
		`article:haschild(h2 .flag)`,
		`article:has(> :is(h2, h3):not(.skip) + p:first-child)`,
	}
	for _, query := range roundTrips {
		first, err := ParseGroup(query)
		if err != nil {
			t.Fatalf("ParseGroup(%q): %v", query, err)
		}
		second, err := ParseGroup(first.String())
		if err != nil {
			t.Fatalf("re-parsing %q (serialized as %q): %v", query, first.String(), err)
		}
		if !reflect.DeepEqual(first, second) {
			t.Errorf("round trip changed %q: %#v vs %#v", query, first, second)
		}
		for i := range first {
			if first[i].Specificity() != second[i].Specificity() {
				t.Errorf("specificity changed for %q: %v vs %v", query,
					first[i].Specificity(), second[i].Specificity())
			}
		}
	}

	// :has() contributes the specificity of its most specific argument; the
	// three type selectors inside the argument add to the article type.
	s, err := Parse(`article:has(> h2 + p a)`)
	if err != nil {
		t.Fatal(err)
	}
	if got, want := s.Specificity(), (Specificity{0, 0, 4}); got != want {
		t.Errorf("specificity: got %v, want %v", got, want)
	}
}

func mustParseRelativeDoc(t *testing.T, markup string) *html.Node {
	t.Helper()
	doc, err := html.Parse(strings.NewReader(markup))
	if err != nil {
		t.Fatalf("html.Parse: %v", err)
	}
	return doc
}
