package readability

import (
	nurl "net/url"
	"strings"

	"github.com/go-shiori/dom"
	"golang.org/x/net/html"
)

// indexFootnoteTargets builds an index of every element that carries
// an id in the original (cloned) document, so footnote references can
// be resolved without walking the whole tree for each link.
func (ps *Parser) indexFootnoteTargets() {
	ps.footnoteTargets = make(map[string][]*html.Node)

	var walk func(*html.Node)
	walk = func(node *html.Node) {
		if node.Type == html.ElementNode {
			if id := dom.ID(node); id != "" {
				ps.footnoteTargets[id] = append(ps.footnoteTargets[id], node)
			}
		}

		for child := node.FirstChild; child != nil; child = child.NextSibling {
			walk(child)
		}
	}

	walk(ps.doc)
}

// isFootnoteReference reports whether the link is a footnote reference
// according to the rules: an <a> with role="doc-noteref" compared as a
// full value, or a rel token list (split on ASCII whitespace, matched
// case-insensitively) that contains "footnote". Any other in-page
// navigation link is not a reference.
func isFootnoteReference(link *html.Node) bool {
	if dom.TagName(link) != "a" {
		return false
	}

	if dom.GetAttribute(link, "role") == "doc-noteref" {
		return true
	}

	for _, token := range splitASCIISpaces(dom.GetAttribute(link, "rel")) {
		if strings.EqualFold(token, "footnote") {
			return true
		}
	}

	return false
}

// splitASCIISpaces splits s on ASCII whitespace only, so for example a
// non-breaking space is treated as a regular character.
func splitASCIISpaces(s string) []string {
	return strings.FieldsFunc(s, func(r rune) bool {
		switch r {
		case ' ', '\t', '\n', '\r', '\v', '\f':
			return true
		}
		return false
	})
}

// resolveFootnoteFragment extracts the target id from a footnote
// reference href. The href may be a bare "#fragment", a relative URL,
// or an absolute http(s) URL whose part before the fragment is
// identical to the page URL. Relative URLs are resolved against
// pageURL, never against an HTML <base>. It returns the fragment after
// exactly one round of percent-unescaping ("+" is kept as-is) and
// whether the reference points at the current page.
func (ps *Parser) resolveFootnoteFragment(href string) (string, bool) {
	ref, err := nurl.Parse(href)
	if err != nil {
		// Invalid percent-escaping or any other parse error means the
		// reference cannot trigger a restore.
		return "", false
	}

	pageURL := ps.documentURI
	if ref.IsAbs() && (ref.Scheme != "http" && ref.Scheme != "https") {
		return "", false
	}

	if pageURL != nil {
		ref = pageURL.ResolveReference(ref)
	}

	// Compare the part before the fragment with the page URL, so
	// cross-article addresses never trigger a restore.
	withoutFragment := *ref
	withoutFragment.Fragment = ""
	withoutFragment.RawFragment = ""
	if pageURL == nil || withoutFragment.String() != pageURL.String() {
		return "", false
	}

	rawFragment := ref.RawFragment
	if rawFragment == "" {
		rawFragment = ref.EscapedFragment()
	}
	if rawFragment == "" {
		return "", false
	}

	// Decode percent-escaping exactly once. PathUnescape does not turn
	// "+" into a space.
	fragment, err := nurl.PathUnescape(rawFragment)
	if err != nil || fragment == "" {
		return "", false
	}

	return fragment, true
}

// collectFootnoteRefs walks the final extracted article and records the
// ids referenced by footnote links, in order of first appearance.
func (ps *Parser) collectFootnoteRefs(articleContent *html.Node) {
	ps.footnoteOrder = make([]string, 0)
	seen := make(map[string]struct{})

	for _, link := range dom.GetElementsByTagName(articleContent, "a") {
		if !isFootnoteReference(link) {
			continue
		}

		fragment, ok := ps.resolveFootnoteFragment(dom.GetAttribute(link, "href"))
		if !ok {
			continue
		}

		if _, exist := seen[fragment]; !exist {
			seen[fragment] = struct{}{}
			ps.footnoteOrder = append(ps.footnoteOrder, fragment)
		}
	}
}

// resolveFootnoteTarget returns the unique footnote element for id.
// Only an <li role="doc-footnote"> qualifies, and an id shared by two
// elements makes the target ambiguous, so nothing is returned.
func (ps *Parser) resolveFootnoteTarget(id string) *html.Node {
	nodes := ps.footnoteTargets[id]
	if len(nodes) != 1 {
		return nil
	}

	node := nodes[0]
	if dom.TagName(node) != "li" || dom.GetAttribute(node, "role") != "doc-footnote" {
		return nil
	}

	return node
}

// treeContainsID reports whether an element with the given id is part
// of the given tree. The extracted article is a clone of the original
// document, so nodes can never be compared by pointer.
func treeContainsID(root *html.Node, id string) bool {
	var found bool
	var walk func(*html.Node)
	walk = func(node *html.Node) {
		if found {
			return
		}

		if node.Type == html.ElementNode && dom.ID(node) == id {
			found = true
			return
		}

		for child := node.FirstChild; child != nil; child = child.NextSibling {
			walk(child)
		}
	}

	walk(root)
	return found
}

// cleanRestoredFootnote runs the same article cleaning pipeline on the
// cloned footnote as the main content went through (junk tags, styles,
// forms, embeds, conditional cleanup, etc.). Relative link conversion
// and class handling are done afterwards together with the whole
// article. Comments, scripts and styles are already gone from the
// indexed source document, but removing them again is harmless.
func (ps *Parser) cleanRestoredFootnote(note *html.Node) {
	ps.removeComments(note)
	ps.removeNodes(ps.getAllNodesWithTag(note, "script", "noscript", "style"), nil)
	ps.prepArticle(note)
}

// restoreFootnotes finds the footnote references inside the extracted
// article and appends the uniquely referenced standalone footnotes in
// a single <ol> at the end of the article. It is a no-op unless
// KeepFootnotes is enabled. Footnote content is never scanned for more
// references, so mutual footnote references cannot expand the result.
func (ps *Parser) restoreFootnotes(articleContent *html.Node) {
	ps.indexFootnoteTargets()
	ps.collectFootnoteRefs(articleContent)
	if len(ps.footnoteOrder) == 0 {
		return
	}

	list := dom.CreateElement("ol")
	for _, id := range ps.footnoteOrder {
		target := ps.resolveFootnoteTarget(id)
		if target == nil {
			continue
		}

		// A footnote that is already part of the article needs no copy.
		if treeContainsID(articleContent, id) {
			continue
		}

		note := dom.Clone(target, true)
		ps.cleanRestoredFootnote(note)
		dom.AppendChild(list, note)
	}

	if dom.FirstElementChild(list) == nil {
		return
	}

	// Article.Node is the article's first element child (the
	// readability-page-1 wrapper), so the list goes inside it to stay
	// visible through Node, Content and TextContent alike.
	if parent := dom.FirstElementChild(articleContent); parent != nil {
		dom.AppendChild(parent, list)
	} else {
		dom.AppendChild(articleContent, list)
	}
}
