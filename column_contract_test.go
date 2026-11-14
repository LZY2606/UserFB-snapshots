package cascadia

import (
	"fmt"
	"strings"
	"sync"
	"testing"

	"github.com/andybalholm/cascadia/task_support/taskcheck"
	"golang.org/x/net/html"
)

// The tests in this file pin down the DOM-only column model described in
// task_support/columns.md. Expected logical column ranges and expected node
// IDs are written out independently of the implementation so that a change in
// the grid rules shows up as a test failure instead of silently passing.

func columnTestDoc(t *testing.T, source string) *html.Node {
	t.Helper()
	doc, err := html.Parse(strings.NewReader(source))
	if err != nil {
		t.Fatalf("parsing fixture: %v", err)
	}
	return doc
}

func nodeByID(root *html.Node, id string) *html.Node {
	var found *html.Node
	var walk func(*html.Node)
	walk = func(n *html.Node) {
		if found != nil {
			return
		}
		if getId(n) == id {
			found = n
			return
		}
		for c := n.FirstChild; c != nil; c = c.NextSibling {
			walk(c)
		}
	}
	walk(root)
	return found
}

func matchedIDs(nodes []*html.Node) string {
	ids := make([]string, 0, len(nodes))
	for _, n := range nodes {
		if id := getId(n); id != "" {
			ids = append(ids, id)
		}
	}
	return strings.Join(ids, " ")
}

// columnQueryIDs parses selector with ParseGroup and returns the IDs of the
// nodes QueryAll finds under root, in document order.
func columnQueryIDs(t *testing.T, root *html.Node, selector string) string {
	t.Helper()
	s, err := ParseGroup(selector)
	if err != nil {
		t.Fatalf("parsing %q: %v", selector, err)
	}
	return matchedIDs(QueryAll(root, s))
}

// columnPlainQueryIDs uses the compatibility Selector entry point (Compile
// plus MatchAll) so every query API is exercised with the same meaning.
func columnPlainQueryIDs(t *testing.T, root *html.Node, selector string) string {
	t.Helper()
	s, err := Compile(selector)
	if err != nil {
		t.Fatalf("compiling %q: %v", selector, err)
	}
	return matchedIDs(s.MatchAll(root))
}

// columnFixture is the main geometry table. Declared columns:
//
//	c1      -> 1
//	c2      -> 2-3   (span=2, money)
//	cg      -> 1-3   (union of its child cols, its own span is ignored)
//	cgEmpty -> 4-5   (no child col, owns its own span=2)
//	c6      -> 6
//
// So the table is six logical columns wide, including the empty tail.
// Cells (rowspan=2 on "a" pushes the second row's cursor):
//
//	h1 -> 1,  h2 -> 2-3
//	a  -> 1,  b -> 2-3,  c -> 2,  d -> 3-4
const columnFixture = `<html><body>
<table id="t" class="report">
 <colgroup id="cg"><col id="c1"><col id="c2" class="money" span="2"></colgroup>
 <colgroup id="cgEmpty" span="2"></colgroup>
 <col id="c6" class="tail">
 <thead><tr id="r0"><th id="h1">H1</th><th id="h2" colspan="2">H2</th></tr></thead>
 <tbody>
  <tr id="r1"><td id="a" rowspan="2">A</td><td id="b" colspan="2"><a id="la" href="/a">B</a></td></tr>
  <tr id="r2"><td id="c"><a id="lc" href="/b">C</a></td><td id="d" colspan="2">D</td></tr>
 </tbody>
</table>
<table id="plain">
 <tr id="pr0"><td id="p1" rowspan="2">P</td><td id="p2" colspan="2">Q</td></tr>
 <tr id="pr1"><td id="p3">R</td></tr>
</table>
<table id="bad"><colgroup><col id="badcol" class="any"></colgroup>
 <tbody><tr><td id="bad1">1</td><td id="bad2" rowspan="2">2</td></tr>
 <tr><td id="bad3" colspan="2">3</td></tr></tbody>
</table>
<table id="clamp">
 <tbody><tr><td id="k1" rowspan="9">K</td></tr><tr><td id="k2">L</td></tr></tbody>
</table>
<table id="zero">
 <tr id="zr1"><td id="z1" rowspan="0">Z</td><td id="z2">1</td></tr>
 <tr id="zr2"><td id="z3">2</td></tr>
</table>
<table id="implicit">
 <tr id="ir1"><td id="i1">a</td></tr>
 <tr id="ir2"><td id="i2">b</td></tr>
 <tr id="ir3"><td id="i3">c</td></tr>
</table>
<table id="capped">
 <tr><td id="cap1" colspan="1001">x</td><td id="cap2" class="last">y</td></tr>
</table>
</body></html>`

// nestedFixture holds an inner table inside an outer cell. The two tables
// declare different money columns, so a column of one must never satisfy a
// column condition evaluated for a cell of the other.
const nestedFixture = `<html><body>
<table id="no">
 <colgroup><col id="nc1"><col id="nc2" class="money"></colgroup>
 <tbody>
  <tr id="nr1"><td id="nA">A</td><td id="nB"><table id="ni">
   <colgroup><col id="nic" class="inner"></colgroup>
   <tbody><tr id="nri"><td id="ni1">1</td><td id="ni2">2</td></tr></tbody>
  </table></td></tr>
  <tr id="nr2"><td id="nC">C</td><td id="nD">D</td></tr>
 </tbody>
</table>
</body></html>`

func TestColumnSyntax(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		valid := []string{
			"col.money || td",
			"table.report > colgroup > col.money || td > a",
			"[data-kind|=money] || td",
			"col:first-child || td",
			"td:nth-col(2)",
			"td:nth-col(odd)",
			"td:nth-col(2n+1)",
			"td:nth-last-col(-n+2)",
			"td:nth-last-col(2n)",
			":is(col.money || td)",
			":not(col.money || td)",
			":has(col.money || td)",
			"div:is(col.money || td):not(:has(> col.money || td))",
		}
		for _, selector := range valid {
			_, err := Parse(selector)
			c.Check(err == nil, "Parse(%q) returned error: %v", selector, err)
			_, err = ParseGroup(selector)
			c.Check(err == nil, "ParseGroup(%q) returned error: %v", selector, err)
			_, err = Compile(selector)
			c.Check(err == nil, "Compile(%q) returned error: %v", selector, err)
		}

		invalid := []string{
			"div | p",             // a single vertical bar is not a combinator
			"div ||",              // missing right operand
			"|| td",               // no leading relative column branch
			"div ||| p",           // consecutive combinators
			"div || > p",          // missing right operand
			"div || , p",          // missing right operand
			"div|||p",             // consecutive combinators without spaces
			"div || td | p",       // trailing single pipe
			"td:nth-col()",        // missing An+B
			"td:nth-col(2 of .x)", // no "of" clause
			"td:nth-col(2 of)",
			"td:nth-last-col(2 of td)",
			":has(|| td)", // no relative branch starting with ||
			"td:nth-col",
			"td:nth-last-col",
		}
		for _, selector := range invalid {
			_, err := Parse(selector)
			c.Check(err != nil, "Parse(%q) should have failed", selector)
			_, err = ParseGroup(selector)
			c.Check(err != nil, "ParseGroup(%q) should have failed", selector)
		}

		// The attribute dash-match operator and escaped pipes keep their
		// original meaning; they are not the column combinator.
		attr, err := Parse(`[lang|="en"]`)
		c.Check(err == nil, "attribute |= selector should parse: %v", err)
		en := &html.Node{Type: html.ElementNode, Data: "p", Attr: []html.Attribute{{Key: "lang", Val: "en-GB"}}}
		c.Check(attr.Match(en), `[lang|="en"] should still match lang="en-GB"`)
		other := &html.Node{Type: html.ElementNode, Data: "p", Attr: []html.Attribute{{Key: "lang", Val: "enough"}}}
		c.Check(!attr.Match(other), `[lang|="en"] should not match lang="enough"`)
		_, err = Parse(`col\|money || td`)
		c.Check(err == nil, `escaped pipe in an identifier should parse: %v`, err)

		// || adds no weight; the position pseudo-classes add one class each.
		specs := map[string]Specificity{
			"col || td":       {0, 0, 2},
			"col.money || td": {0, 1, 2},
			"table.report > colgroup > col.money || td > a": {0, 2, 5},
			"td:nth-col(2)":                 {0, 1, 1},
			"td:nth-last-col(2)":            {0, 1, 1},
			"td:nth-col(2):nth-last-col(3)": {0, 2, 1},
		}
		for selector, want := range specs {
			s, err := Parse(selector)
			if err != nil {
				c.Check(false, "Parse(%q): %v", selector, err)
				continue
			}
			c.Check(s.Specificity() == want, "%s specificity = %v, want %v", selector, s.Specificity(), want)
		}
		plain, err := Parse("col td")
		c.Check(err == nil, "plain descendant selector should parse: %v", err)
		column, err := Parse("col || td")
		if err == nil {
			c.Check(column.Specificity() == plain.Specificity(),
				"|| changed specificity: %v vs %v", column.Specificity(), plain.Specificity())
		}

		// String output has to re-parse to the same compiled selector,
		// including its specificity.
		for _, selector := range valid {
			s, err := ParseGroup(selector)
			if err != nil {
				c.Check(false, "ParseGroup(%q): %v", selector, err)
				continue
			}
			serialized := s.String()
			again, err := ParseGroup(serialized)
			if err != nil {
				c.Check(false, "serialized %q of %q does not parse: %v", serialized, selector, err)
				continue
			}
			c.Check(again.String() == serialized, "%q serializes back to %q, not %q", selector, again.String(), serialized)
			c.Check(groupSpecificity(again) == groupSpecificity(s),
				"%q serialized as %q changed specificity from %v to %v", selector, serialized, groupSpecificity(s), groupSpecificity(again))
		}
	})
}

func TestColumnGeometry(t *testing.T) {
	doc := columnTestDoc(t, columnFixture)
	outer := nodeByID(doc, "t")
	plain := nodeByID(doc, "plain")
	bad := nodeByID(doc, "bad")
	clamp := nodeByID(doc, "clamp")
	nested := columnTestDoc(t, nestedFixture)
	nOuter := nodeByID(nested, "no")
	nInner := nodeByID(nested, "ni")

	taskcheck.Run(t, func(c *taskcheck.C) {
		// Logical ranges are written out by hand: (start, end), one-based.
		wantRanges := map[string][2]int{
			"h1":  {1, 1},
			"h2":  {2, 3},
			"a":   {1, 1},
			"b":   {2, 3},
			"c":   {2, 2},
			"d":   {3, 4},
			"p1":  {1, 1},
			"p2":  {2, 3},
			"p3":  {2, 2},
			"k1":  {1, 1},
			"k2":  {2, 2},
			"nA":  {1, 1},
			"nB":  {2, 2},
			"nC":  {1, 1},
			"nD":  {2, 2},
			"ni1": {1, 1},
			"ni2": {2, 2},
		}
		layouts := map[*html.Node]*columnLayout{}
		for _, table := range []*html.Node{outer, plain, clamp, nOuter, nInner} {
			if table == nil {
				c.Check(false, "fixture table is missing")
				continue
			}
			layout := buildColumnLayout(table)
			layouts[table] = &layout
			c.Check(layout.valid, "table with id %q should have a valid grid", getId(table))
		}
		if l := layouts[outer]; l != nil {
			c.Check(l.width == 6, "outer table width = %d, want 6 (empty declared tail counts)", l.width)
		}
		if l := layouts[plain]; l != nil {
			c.Check(l.width == 3, "plain table width = %d, want 3", l.width)
		}
		if l := layouts[clamp]; l != nil {
			c.Check(l.width == 2, "clamp table width = %d, want 2", l.width)
		}
		if l := layouts[nOuter]; l != nil {
			c.Check(l.width == 2, "nested outer table width = %d, want 2", l.width)
		}
		if l := layouts[nInner]; l != nil {
			c.Check(l.width == 2, "inner table width = %d, want 2 (second cell is wider than the declaration)", l.width)
		}

		// Declared ranges: the colgroup "cg" covers the union of its child
		// cols (1-3); child-less "cgEmpty" owns 4-5; tail col owns 6.
		declRanges := map[string][2]int{
			"c1":      {1, 1},
			"c2":      {2, 3},
			"cg":      {1, 3},
			"cgEmpty": {4, 5},
			"c6":      {6, 6},
			"badcol":  {1, 1},
			"nc1":     {1, 1},
			"nc2":     {2, 2},
			"nic":     {1, 1},
		}
		for id, want := range declRanges {
			var table *html.Node
			switch id {
			case "badcol":
				table = bad
			case "nc1", "nc2":
				table = nOuter
			case "nic":
				table = nInner
			default:
				table = outer
			}
			layout := layouts[table]
			n := nodeByID(doc, id)
			if table == nOuter || table == nInner {
				n = nodeByID(nested, id)
			}
			if layout == nil {
				continue
			}
			decl := findDeclaration(layout, n)
			if decl == nil {
				c.Check(false, "column declaration %s not found", id)
				continue
			}
			c.Check(decl.start == want[0] && decl.end == want[1],
				"column declaration %s covers %d-%d, want %d-%d", id, decl.start, decl.end, want[0], want[1])
		}

		for id, want := range wantRanges {
			var table *html.Node
			var root *html.Node
			switch id {
			case "p1", "p2", "p3":
				table, root = plain, doc
			case "k1", "k2":
				table, root = clamp, doc
			case "nA", "nB", "nC", "nD":
				table, root = nOuter, nested
			case "ni1", "ni2":
				table, root = nInner, nested
			default:
				table, root = outer, doc
			}
			layout := layouts[table]
			n := nodeByID(root, id)
			if layout == nil || n == nil {
				c.Check(false, "missing layout or node for %s", id)
				continue
			}
			p, ok := layout.placement(n)
			if !ok {
				c.Check(false, "cell %s has no placement", id)
				continue
			}
			c.Check(p.colStart == want[0] && p.colEnd == want[1],
				"cell %s covers columns %d-%d, want %d-%d", id, p.colStart, p.colEnd, want[0], want[1])
		}

		// Column combinator: the cell rectangle has to intersect the declared
		// range. A multi-column cell is reported once even when several of
		// its columns satisfy the condition.
		combinatorCases := map[string]string{
			"#c1 || td":             "a",
			"#c2 || td":             "b c d",
			"#cg || td":             "a b c d",
			"#cgEmpty || td":        "d",
			"#c6 || td":             "",
			"#c6 || th":             "",
			"col.money || td":       "b c d",
			"col:first-child || td": "a",
			"[id=c2] || td":         "b c d",
			"th.colnone || td":      "",
		}
		for selector, want := range combinatorCases {
			got := columnQueryIDs(t, outer, selector)
			c.Check(got == want, "%s = %q, want %q", selector, got, want)
		}

		// The right side still supports the ordinary relations, and the
		// left side still supports attributes and pseudo-classes.
		relationCases := map[string]string{
			"table.report > colgroup > col.money || td > a": "la lc",
			"col.money || td > a":                           "la lc",
			"table.report > colgroup > col.money || td":     "b c d",
			"col.money || td + td":                          "d",
			"col.money || td ~ td":                          "d",
		}
		for selector, want := range relationCases {
			got := columnQueryIDs(t, outer, selector)
			c.Check(got == want, "%s = %q, want %q", selector, got, want)
		}

		// :nth-col / :nth-last-col use the full width (6), including the
		// empty declared tail, and match when any covered column fits.
		nthCases := map[string]string{
			"td:nth-col(1)":       "a",
			"td:nth-col(2)":       "b c",
			"td:nth-col(3)":       "b d",
			"td:nth-col(4)":       "d",
			"td:nth-col(5)":       "",
			"td:nth-col(6)":       "",
			"td:nth-last-col(1)":  "",
			"td:nth-last-col(2)":  "",
			"td:nth-last-col(3)":  "d",
			"td:nth-last-col(4)":  "b d",
			"td:nth-last-col(5)":  "b c",
			"td:nth-last-col(6)":  "a",
			"td:nth-col(2n)":      "b c d",
			"td:nth-col(2n+1)":    "a b d",
			"td:nth-last-col(2n)": "a b d",
			"td:nth-col(-n+2)":    "a b c",
			"td:nth-col(odd)":     "a b d",
			"th:nth-col(2)":       "h2",
			"div:nth-col(1)":      "",
			":nth-col(1):not(td)": "h1",
		}
		for selector, want := range nthCases {
			got := columnQueryIDs(t, outer, selector)
			c.Check(got == want, "%s = %q, want %q", selector, got, want)
		}

		// Plain tables without col declarations still support positions.
		plainCases := map[string]string{
			"td:nth-col(1)":      "p1",
			"td:nth-col(2)":      "p2 p3",
			"td:nth-col(3)":      "p2",
			"td:nth-last-col(1)": "p2",
			"td:nth-last-col(2)": "p2 p3",
			"td:nth-last-col(3)": "p1",
		}
		for selector, want := range plainCases {
			got := columnQueryIDs(t, plain, selector)
			c.Check(got == want, "%s under plain = %q, want %q", selector, got, want)
		}

		// A positive rowspan is clamped to the current row group.
		c.Check(columnQueryIDs(t, clamp, "td:nth-col(1)") == "k1", "clamped rowspan should leave k1 in column 1")
		c.Check(columnQueryIDs(t, clamp, "td:nth-col(2)") == "k2", "clamped rowspan should leave k2 in column 2")

		// rowspan=0 covers the remaining rows of its own row group.
		zeroTable := nodeByID(doc, "zero")
		c.Check(columnQueryIDs(t, zeroTable, "td:nth-col(1)") == "z1", "rowspan=0 keeps column 1 occupied only in its row group")
		c.Check(columnQueryIDs(t, zeroTable, "td:nth-col(2)") == "z2 z3", "rowspan=0 should not block the second row")

		// Consecutive direct tr children form one implicit row group, so
		// rows are ordered in DOM order and spans stay inside it.
		implicitTable := nodeByID(doc, "implicit")
		c.Check(buildColumnLayout(implicitTable).width == 1, "implicit row group table width = %d, want 1", buildColumnLayout(implicitTable).width)
		c.Check(columnQueryIDs(t, implicitTable, "td:nth-col(1)") == "i1 i2 i3", "implicit row group should expose each row's first cell")

		// colspan is capped at 1000; the next cell still follows it.
		cappedTable := nodeByID(doc, "capped")
		cappedLayout := buildColumnLayout(cappedTable)
		c.Check(cappedLayout.valid, "capped colspan grid should be valid")
		if p, ok := cappedLayout.cells[nodeByID(doc, "cap1")]; ok {
			c.Check(p.colStart == 1 && p.colEnd == 1000, "colspan cap covers %d-%d, want 1-1000", p.colStart, p.colEnd)
		} else {
			c.Check(false, "capped colspan cell has no placement")
		}
		if p, ok := cappedLayout.cells[nodeByID(doc, "cap2")]; ok {
			c.Check(p.colStart == 1001 && p.colEnd == 1001, "cell after capped colspan covers %d-%d, want 1001-1001", p.colStart, p.colEnd)
		} else {
			c.Check(false, "cell after capped colspan has no placement")
		}
		c.Check(columnQueryIDs(t, cappedTable, "td:nth-last-col(1)") == "cap2", "reverse position 1 should be the cell after the capped span")

		// A conflicting grid invalidates all three column conditions for
		// every cell of that table.
		c.Check(!buildColumnLayout(bad).valid, "overlapping cell rectangle should invalidate the grid")
		for _, selector := range []string{"td:nth-col(1)", "td:nth-col(2)", "td:nth-last-col(2)", "col.any || td"} {
			c.Check(columnQueryIDs(t, bad, selector) == "",
				"%s should match nothing in a conflicting table", selector)
		}

		// Nested tables keep their own declarations and cells.
		c.Check(columnQueryIDs(t, nOuter, "col.money || td") == "nB nD", "nested tree column query = %q", columnQueryIDs(t, nOuter, "col.money || td"))
		c.Check(columnQueryIDs(t, nInner, "#nc2 || td") == "", "an outer col must not reach the inner table")
		c.Check(columnQueryIDs(t, nOuter, "#nic || td") == "ni1", "an inner col must only reach inner cells")
		c.Check(columnQueryIDs(t, nOuter, "#nic || td > x") == "", "inner column must not match outer cells")
		c.Check(columnQueryIDs(t, nOuter, "td:nth-col(2)") == "nB ni2 nD", "nested position query = %q", columnQueryIDs(t, nOuter, "td:nth-col(2)"))
		c.Check(columnQueryIDs(t, nInner, "td:nth-last-col(2)") == "ni1", "inner width is two, so reverse position 2 is its first column")

		// SVG nodes with the same local name are not HTML table columns.
		svgTable, _ := buildSVGSiblingFixture()
		c.Check(columnQueryIDs(t, svgTable, "col || td") == "", "an SVG col must not act as a table column declaration")
		c.Check(columnQueryIDs(t, svgTable, "#svgcol || td") == "", "an SVG col must not match the column combinator")

		// Regular relations on :is / :not / :has arguments use the same
		// column meaning.
		filterCases := map[string]string{
			":is(col.money || td)":    "b c d",
			"td:not(col.money || td)": "a",
			"tr:has(col.money || td)": "r1 r2",
		}
		for selector, want := range filterCases {
			got := columnQueryIDs(t, outer, selector)
			c.Check(got == want, "%s = %q, want %q", selector, got, want)
		}

		// All query entry points agree, and Filter keeps input order and
		// duplicates.
		selector := "col.money || td"
		group := columnQueryIDs(t, outer, selector)
		c.Check(columnPlainQueryIDs(t, outer, selector) == group, "Compile/MatchAll disagree with ParseGroup/QueryAll")
		parsed, err := Parse(selector)
		if err != nil {
			c.Check(false, "Parse(%q): %v", selector, err)
		} else {
			c.Check(matchedIDs(QueryAll(outer, parsed)) == group, "Parse/QueryAll disagree with ParseGroup/QueryAll")
			c.Check(matchedIDs(Selector(MustCompile(selector)).MatchAll(outer)) == group, "Selector.MatchAll disagrees with QueryAll")
			all := QueryAll(outer, MustCompile("*"))
			filtered := Filter(all, parsed)
			c.Check(matchedIDs(filtered) == group, "Filter over document order disagrees with QueryAll")
			b, a := nodeByID(doc, "b"), nodeByID(doc, "a")
			dup := Filter([]*html.Node{b, a, b, b}, parsed)
			c.Check(matchedIDs(dup) == "b b b", "Filter should preserve input order and duplicates, got %q", matchedIDs(dup))
			c.Check(matchedIDs(Selector(MustCompile("#c2 || td")).Filter([]*html.Node{nodeByID(doc, "d"), nodeByID(doc, "c"), nodeByID(doc, "a")})) == "d c",
				"Selector.Filter should keep only matching nodes in input order")
		}
	})
}

func findDeclaration(layout *columnLayout, n *html.Node) *columnDeclaration {
	for i := range layout.decls {
		if layout.decls[i].node == n {
			return &layout.decls[i]
		}
	}
	return nil
}

func groupSpecificity(g SelectorGroup) Specificity {
	var spec Specificity
	for _, s := range g {
		spec = spec.Add(s.Specificity())
	}
	return spec
}

// buildSVGSiblingFixture builds a table that has an SVG-namespaced col as a
// direct child, plus one ordinary row. The SVG node must not contribute a
// logical column.
func buildSVGSiblingFixture() (*html.Node, *html.Node) {
	table := &html.Node{Type: html.ElementNode, Data: "table"}
	svgCol := &html.Node{Type: html.ElementNode, Data: "col", Namespace: "svg",
		Attr: []html.Attribute{{Key: "id", Val: "svgcol"}, {Key: "class", Val: "money"}}}
	table.AppendChild(svgCol)
	row := &html.Node{Type: html.ElementNode, Data: "tr"}
	cell := &html.Node{Type: html.ElementNode, Data: "td", Attr: []html.Attribute{{Key: "id", Val: "svgcell"}}}
	row.AppendChild(cell)
	table.AppendChild(row)
	return table, svgCol
}

func TestColumnReuse(t *testing.T) {
	docA := columnTestDoc(t, columnFixture)
	outer := nodeByID(docA, "t")
	shared, err := ParseGroup("col.money || td")
	if err != nil {
		t.Fatalf("compiling shared selector: %v", err)
	}
	sharedLast, err := ParseGroup("td:nth-last-col(3)")
	if err != nil {
		t.Fatalf("compiling shared selector: %v", err)
	}

	second := columnTestDoc(t, `<html><body><table id="second">
 <colgroup><col id="s1"><col id="s2" class="money"></colgroup>
 <tbody><tr><td id="q1">1</td><td id="q2">2</td></tr></tbody></table></body></html>`)
	secondTable := nodeByID(second, "second")
	secondCopy := nodeByID(second, "s2")
	setAttr(secondCopy, "class", "money")

	nested := columnTestDoc(t, nestedFixture)
	nOuter := nodeByID(nested, "no")

	taskcheck.Run(t, func(c *taskcheck.C) {
		// One compiled matcher, two tables with different declarations.
		c.Check(matchedIDs(QueryAll(outer, shared)) == "b c d", "shared selector on first table = %q", matchedIDs(QueryAll(outer, shared)))
		c.Check(matchedIDs(QueryAll(secondTable, shared)) == "q2", "shared selector on second table = %q", matchedIDs(QueryAll(secondTable, shared)))
		c.Check(matchedIDs(QueryAll(outer, sharedLast)) == "d", "shared position selector on first table = %q", matchedIDs(QueryAll(outer, sharedLast)))
		// Second table width 2: reverse position 3 has no column at all,
		// while the first table's width six does.
		c.Check(matchedIDs(QueryAll(secondTable, sharedLast)) == "", "shared position selector on second table = %q, want empty", matchedIDs(QueryAll(secondTable, sharedLast)))

		// Mutating span attributes must be visible on the next query.
		fresh := columnTestDoc(t, columnFixture)
		mutOuter := nodeByID(fresh, "t")
		c2 := nodeByID(fresh, "c2")
		cgEmpty := nodeByID(fresh, "cgEmpty")
		setAttr(c2, "span", "1")
		c.Check(columnQueryIDs(t, mutOuter, "#c2 || td") == "b c", "after col span change #c2 || td = %q, want \"b c\"", columnQueryIDs(t, mutOuter, "#c2 || td"))
		// With c2 at column 2 only, the width is 1+1+2+1 = 5, so reverse
		// position 5 is column 1.
		c.Check(columnQueryIDs(t, mutOuter, "td:nth-last-col(5)") == "a", "after col span change nth-last-col(5) = %q, want \"a\"", columnQueryIDs(t, mutOuter, "td:nth-last-col(5)"))
		setAttr(cgEmpty, "span", "3")
		c.Check(columnQueryIDs(t, mutOuter, "td:nth-last-col(6)") == "a", "after declaration growth nth-last-col(6) = %q, want \"a\"", columnQueryIDs(t, mutOuter, "td:nth-last-col(6)"))

		// Mutating a cell span must be visible on the next query.
		freshSpan := columnTestDoc(t, columnFixture)
		mutSpanOuter := nodeByID(freshSpan, "t")
		d := nodeByID(freshSpan, "d")
		setAttr(d, "colspan", "1")
		c.Check(columnQueryIDs(t, mutSpanOuter, "td:nth-col(4)") == "", "after colspan change nth-col(4) = %q, want empty", columnQueryIDs(t, mutSpanOuter, "td:nth-col(4)"))
		c.Check(columnQueryIDs(t, mutSpanOuter, "td:nth-col(3)") == "b d", "after colspan change nth-col(3) = %q, want \"b d\"", columnQueryIDs(t, mutSpanOuter, "td:nth-col(3)"))
		c.Check(columnQueryIDs(t, mutSpanOuter, "td:nth-last-col(4)") == "b d", "after colspan change nth-last-col(4) = %q, want \"b d\"", columnQueryIDs(t, mutSpanOuter, "td:nth-last-col(4)"))

		// Moving a row into a new row group clears the rowspan occupancy.
		freshGroup := columnTestDoc(t, columnFixture)
		groupOuter := nodeByID(freshGroup, "t")
		tbody := nodeByID(freshGroup, "r1").Parent
		row2 := nodeByID(freshGroup, "r2")
		newGroup := &html.Node{Type: html.ElementNode, Data: "tbody"}
		tbody.RemoveChild(row2)
		newGroup.AppendChild(row2)
		groupOuter.AppendChild(newGroup)
		c.Check(columnQueryIDs(t, groupOuter, "td:nth-col(1)") == "a c", "after splitting the row group nth-col(1) = %q, want \"a c\"", columnQueryIDs(t, groupOuter, "td:nth-col(1)"))
		c.Check(columnQueryIDs(t, groupOuter, "td:nth-col(2)") == "b d", "after splitting the row group nth-col(2) = %q, want \"b d\"", columnQueryIDs(t, groupOuter, "td:nth-col(2)"))
		c.Check(columnQueryIDs(t, groupOuter, "td:nth-col(4)") == "", "after splitting the row group nth-col(4) = %q, want empty", columnQueryIDs(t, groupOuter, "td:nth-col(4)"))

		// Read-only concurrent queries may share one compiled matcher
		// across several trees; no mutable layout state may escape.
		docs := []*html.Node{outer, secondTable, nOuter}
		want := []string{"b c d", "q2", "nB nD"}
		var wg sync.WaitGroup
		problems := make(chan string, 64)
		for i := 0; i < 8; i++ {
			wg.Add(1)
			go func(i int) {
				defer wg.Done()
				for round := 0; round < 50; round++ {
					for j, root := range docs {
						got := matchedIDs(QueryAll(root, shared))
						if got != want[j] {
							select {
							case problems <- fmt.Sprintf("goroutine %d round %d tree %d: %q, want %q", i, round, j, got, want[j]):
							default:
							}
							return
						}
					}
				}
			}(i)
		}
		wg.Wait()
		close(problems)
		ok := true
		for problem := range problems {
			ok = false
			c.Check(false, "concurrent query: %s", problem)
		}
		c.Check(ok, "concurrent read-only queries share no layout state")
	})
}

func setAttr(n *html.Node, key, value string) {
	if n == nil {
		return
	}
	for i := range n.Attr {
		if n.Attr[i].Key == key {
			n.Attr[i].Val = value
			return
		}
	}
	n.Attr = append(n.Attr, html.Attribute{Key: key, Val: value})
}

// ExampleQueryAll_columnMoneyLinks shows how to collect the links of a money column
// without exporting a table or filtering rows by their td position.
func ExampleQueryAll_columnMoneyLinks() {
	const page = `<html><body>
<table class="report">
 <colgroup><col><col class="money"></colgroup>
 <tbody>
  <tr><td>Item</td><td><a href="/item">10</a></td></tr>
  <tr><td>Total</td><td><a href="/total">20</a></td></tr>
 </tbody>
</table>
</body></html>`

	doc, err := html.Parse(strings.NewReader(page))
	if err != nil {
		panic(err)
	}

	// The column combinator keeps the relation inside the compiled selector,
	// so the result is still the original html.Node.
	links := QueryAll(doc, MustCompile("table.report > colgroup > col.money || td > a"))
	for _, link := range links {
		href, _ := attributeValue(link, "href")
		fmt.Println(href)
	}

	// Output:
	// /item
	// /total
}
