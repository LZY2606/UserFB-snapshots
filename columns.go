package cascadia

import (
	"strconv"
	"strings"

	"golang.org/x/net/html"
)

// This file implements the column combinator "||" and the :nth-col() /
// :nth-last-col() pseudo-classes for offline HTML documents.
//
// The logical grid of a table is derived from the DOM alone, following the
// rules in task_support/columns.md. The layout is computed on every match from
// the current tree; no state is cached in the compiled selector, so reuse on a
// different table, later DOM mutation and concurrent read-only queries all
// observe the tree as it is at the time of the match.

const (
	maxColumnSpan = 1000
	maxRowSpan    = 65534
)

// cellPlacement records the logical rectangle occupied by one td/th element.
// Columns are one-based; row indices are zero-based and local to the table.
type cellPlacement struct {
	rowStart, rowEnd int
	colStart, colEnd int
}

// columnDeclaration records the logical columns declared by a real col or
// colgroup element of a table.
type columnDeclaration struct {
	node       *html.Node
	start, end int
}

// columnLayout is an immutable snapshot of one table's logical grid.
type columnLayout struct {
	valid bool // false if two cells claim the same grid slot
	width int  // total number of logical columns in the table
	cells map[*html.Node]cellPlacement
	decls []columnDeclaration
}

func isHTMLElement(n *html.Node, tag string) bool {
	return n != nil && n.Type == html.ElementNode && n.Namespace == "" && n.Data == tag
}

func isHTMLCell(n *html.Node) bool {
	return isHTMLElement(n, "td") || isHTMLElement(n, "th")
}

func attributeValue(n *html.Node, key string) (string, bool) {
	for _, a := range n.Attr {
		if a.Key == key {
			return a.Val, true
		}
	}
	return "", false
}

// parseSpan reads span/colspan/rowspan. Missing, non-numeric and overflowing
// values count as one; span/colspan zero also counts as one; positive values
// are capped. rowspan zero is preserved because it has a meaning of its own.
func parseSpan(n *html.Node, key string, limit int) int {
	raw, ok := attributeValue(n, key)
	if !ok {
		return 1
	}
	s := strings.Trim(raw, " \t\r\n\f")
	if s == "" {
		return 1
	}
	for i := 0; i < len(s); i++ {
		if s[i] < '0' || s[i] > '9' {
			return 1
		}
	}
	v, err := strconv.ParseUint(s, 10, 64)
	if err != nil {
		// Overflow is not a usable non-negative integer.
		return 1
	}
	if v > uint64(limit) {
		v = uint64(limit)
	}
	if v == 0 && key != "rowspan" {
		return 1
	}
	return int(v)
}

func parseColumnSpan(n *html.Node) int { return parseSpan(n, "span", maxColumnSpan) }
func parseColSpan(n *html.Node) int    { return parseSpan(n, "colspan", maxColumnSpan) }
func parseRowSpan(n *html.Node) int    { return parseSpan(n, "rowspan", maxRowSpan) }

// enclosingTable returns the nearest HTML table ancestor of n, or nil.
func enclosingTable(n *html.Node) *html.Node {
	for p := n.Parent; p != nil; p = p.Parent {
		if isHTMLElement(p, "table") {
			return p
		}
	}
	return nil
}

// rowGroups returns the rows of table in DOM order. A run of direct tr
// children forms one implicit row group; thead, tbody and tfoot contribute
// their direct tr children (empty groups are skipped). Only the table's own
// direct children are considered, so rows of nested tables never leak in.
func rowGroups(table *html.Node) [][]*html.Node {
	var groups [][]*html.Node
	var implicit []*html.Node
	flush := func() {
		if len(implicit) > 0 {
			groups = append(groups, implicit)
			implicit = nil
		}
	}
	for c := table.FirstChild; c != nil; c = c.NextSibling {
		switch {
		case isHTMLElement(c, "tr"):
			implicit = append(implicit, c)
		case isHTMLElement(c, "thead"), isHTMLElement(c, "tbody"), isHTMLElement(c, "tfoot"):
			flush()
			var rows []*html.Node
			for r := c.FirstChild; r != nil; r = r.NextSibling {
				if isHTMLElement(r, "tr") {
					rows = append(rows, r)
				}
			}
			if len(rows) > 0 {
				groups = append(groups, rows)
			}
		case c.Type == html.ElementNode:
			// Any other element (caption, colgroup, col, ...) breaks a run
			// of direct tr children.
			flush()
		}
	}
	flush()
	return groups
}

// buildColumnLayout computes the logical grid of table.
func buildColumnLayout(table *html.Node) columnLayout {
	layout := columnLayout{valid: true, cells: map[*html.Node]cellPlacement{}}

	baseRow := 0
	for _, rows := range rowGroups(table) {
		// spanEnds holds, per logical column, the last local row still
		// occupied by a rowspan from an earlier row of this row group.
		// A new row group starts with an empty slice.
		spanEnds := []int{}

		for r, row := range rows {
			cursor := 1
			for cell := row.FirstChild; cell != nil; cell = cell.NextSibling {
				if !isHTMLCell(cell) {
					continue
				}

				// Move the cursor to the first logical column not covered
				// by a rowspan started in a previous row.
				for {
					for len(spanEnds) < cursor {
						spanEnds = append(spanEnds, -1)
					}
					if spanEnds[cursor-1] < r {
						break
					}
					cursor++
				}

				colSpan := parseColSpan(cell)
				rowSpan := parseRowSpan(cell)
				rowEnd := r + rowSpan - 1
				if rowSpan == 0 {
					rowEnd = len(rows) - 1
				} else if rowEnd > len(rows)-1 {
					// A positive rowspan cannot extend past the row group.
					rowEnd = len(rows) - 1
				}

				for col := cursor; col < cursor+colSpan; col++ {
					for len(spanEnds) < col {
						spanEnds = append(spanEnds, -1)
					}
					if spanEnds[col-1] >= r {
						// Another cell already occupies this slot.
						layout.valid = false
						return layout
					}
					spanEnds[col-1] = rowEnd
				}

				layout.cells[cell] = cellPlacement{
					rowStart: baseRow + r,
					rowEnd:   baseRow + rowEnd,
					colStart: cursor,
					colEnd:   cursor + colSpan - 1,
				}
				cursor += colSpan
			}
		}
		baseRow += len(rows)
	}

	// Column declarations from direct col / colgroup children of the table,
	// in DOM order. Columns are allocated independently of the cells.
	next := 1
	for c := table.FirstChild; c != nil; c = c.NextSibling {
		switch {
		case isHTMLElement(c, "col"):
			span := parseColumnSpan(c)
			layout.decls = append(layout.decls, columnDeclaration{node: c, start: next, end: next + span - 1})
			next += span
		case isHTMLElement(c, "colgroup"):
			var children []*html.Node
			for g := c.FirstChild; g != nil; g = g.NextSibling {
				if isHTMLElement(g, "col") {
					children = append(children, g)
				}
			}
			if len(children) == 0 {
				// Without child cols the group is sized by its own span.
				span := parseColumnSpan(c)
				layout.decls = append(layout.decls, columnDeclaration{node: c, start: next, end: next + span - 1})
				next += span
				break
			}
			// With child cols the group's span attribute is ignored and
			// the children get their own ranges; the group covers their union.
			start := next
			for _, col := range children {
				span := parseColumnSpan(col)
				layout.decls = append(layout.decls, columnDeclaration{node: col, start: next, end: next + span - 1})
				next += span
			}
			layout.decls = append(layout.decls, columnDeclaration{node: c, start: start, end: next - 1})
		}
	}

	// The table width is the largest right edge among cells and declarations;
	// declared empty columns count too.
	layout.width = next - 1
	for _, p := range layout.cells {
		if p.colEnd > layout.width {
			layout.width = p.colEnd
		}
	}
	return layout
}

// layoutForCell returns the layout of the table that owns the td/th n, or nil
// when n is not a cell inside a table.
func layoutForCell(n *html.Node) *columnLayout {
	if !isHTMLCell(n) {
		return nil
	}
	table := enclosingTable(n)
	if table == nil {
		return nil
	}
	layout := buildColumnLayout(table)
	return &layout
}

// placement returns the grid rectangle of n in the layout.
func (l *columnLayout) placement(n *html.Node) (cellPlacement, bool) {
	if l == nil || !l.valid {
		return cellPlacement{}, false
	}
	p, ok := l.cells[n]
	return p, ok
}

// nthColumnMatches applies the existing An+B non-negative-n rule to a
// one-based position.
func nthColumnMatches(a, b, pos int) bool {
	i := pos - b
	if a == 0 {
		return i == 0
	}
	return i%a == 0 && i/a >= 0
}

// columnPositionMatches implements :nth-col(An+B) and :nth-last-col(An+B).
// A cell whose rectangle covers several logical columns matches when any one
// of them satisfies the position expression. The reverse position count
// starts from the full table width, not from the number of cells in the row.
func columnPositionMatches(a, b int, last bool, n *html.Node) bool {
	layout := layoutForCell(n)
	p, ok := layout.placement(n)
	if !ok {
		return false
	}
	for col := p.colStart; col <= p.colEnd; col++ {
		pos := col
		if last {
			pos = layout.width + 1 - col
		}
		if nthColumnMatches(a, b, pos) {
			return true
		}
	}
	return false
}

// columnMatch implements "left || right" for the node n that matched the right
// side. The right subject has to be a td/th of a table; the left side has to
// match a real col or colgroup of that same table whose declared range
// intersects any logical column covered by the cell. No implicit col nodes are
// synthesized.
func columnMatch(left, right Matcher, n *html.Node) bool {
	if !isHTMLCell(n) || !right.Match(n) {
		return false
	}
	layout := layoutForCell(n)
	p, ok := layout.placement(n)
	if !ok {
		return false
	}
	for _, decl := range layout.decls {
		if decl.end < p.colStart || decl.start > p.colEnd {
			continue
		}
		if left.Match(decl.node) {
			return true
		}
	}
	return false
}
