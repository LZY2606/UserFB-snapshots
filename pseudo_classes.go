package cascadia

import (
	"bytes"
	"fmt"
	"regexp"
	"strings"

	"golang.org/x/net/html"
	"golang.org/x/net/html/atom"
)

// This file implements the pseudo classes selectors,
// which share the implementation of PseudoElement() and Specificity()

type abstractPseudoClass struct{}

func (s abstractPseudoClass) Specificity() Specificity {
	return Specificity{0, 1, 0}
}

func (c abstractPseudoClass) PseudoElement() string {
	return ""
}

// relativeSelector is a selector that may start with a combinator, as used
// inside :has() / :haschild(). combinator 0 means the implicit combinator
// for that pseudo-class (descendant for :has, child for :haschild).
type relativeSelector struct {
	combinator byte
	sel        Sel
}

type relativePseudoClassSelector struct {
	name string // one of "has", "haschild"
	rels []relativeSelector
}

func (s relativePseudoClassSelector) Match(n *html.Node) bool {
	if n.Type != html.ElementNode {
		return false
	}
	switch s.name {
	case "has", "haschild":
		for _, r := range s.rels {
			comb := r.combinator
			if comb == 0 {
				if s.name == "haschild" {
					comb = '>'
				} else {
					comb = ' '
				}
			}
			if relativeMatch(n, comb, r.sel) {
				return true
			}
		}
		return false
	default:
		panic(fmt.Sprintf("unsupported relative pseudo class selector : %s", s.name))
	}
}

// relativeStep is one compound of a flattened relative selector chain.
// combinator is the combinator preceding compound, or 0 for the first
// compound, whose combinator is supplied by the :has()/:haschild() anchor.
type relativeStep struct {
	combinator byte
	compound   Sel
}

// flattenRelativeSelector turns the right-leaning combinedSelector tree
// produced by the parser into a left-to-right list of compounds and the
// combinators that join them.
func flattenRelativeSelector(sel Sel) []relativeStep {
	var steps []relativeStep
	var walk func(Sel)
	walk = func(s Sel) {
		if c, ok := s.(combinedSelector); ok {
			walk(c.first)
			steps = append(steps, relativeStep{combinator: c.combinator, compound: c.second})
			return
		}
		steps = append(steps, relativeStep{compound: s})
	}
	walk(sel)
	return steps
}

// relativeMatch reports whether the relative selector sel matches when its
// first step leaves the anchor n through combinator. Only that first step is
// anchored to n: once it has been taken, the rest of the chain is evaluated
// normally and may reach the anchor's siblings (or further) through '+' and
// '~'. The implicit combinator (0) has already been resolved by the caller:
// descendant for :has(), child for :haschild().
func relativeMatch(n *html.Node, combinator byte, sel Sel) bool {
	steps := flattenRelativeSelector(sel)
	e := &relativeEval{steps: steps, memo: map[relativeChainState]bool{}}
	first := steps[0].compound
	switch combinator {
	case ' ':
		// Descendant first step: search n's subtree. The anchor itself and
		// its ancestors are never candidates.
		return e.descendantFirst(n, first)
	case '>':
		// Child first step: only n's own element children are candidates.
		for c := n.FirstChild; c != nil; c = c.NextSibling {
			if c.Type == html.ElementNode && first.Match(c) && e.chain(c, 1) {
				return true
			}
		}
		return false
	case '+':
		// Adjacent sibling first step: the first element sibling following n.
		// Text nodes and comments do not occupy an element-sibling position.
		c := nextElementSibling(n)
		return c != nil && first.Match(c) && e.chain(c, 1)
	case '~':
		// Subsequent-sibling first step.
		for c := n.NextSibling; c != nil; c = c.NextSibling {
			if c.Type == html.ElementNode && first.Match(c) && e.chain(c, 1) {
				return true
			}
		}
		return false
	default:
		return false
	}
}

// relativeChainState identifies one point of a forward chain evaluation:
// node reached with steps[idx:] still to walk.
type relativeChainState struct {
	node *html.Node
	idx  int
}

// relativeEval runs the forward walk of a flattened relative chain, memoized
// per (node, chain position) so that long descendant chains stay linear in
// the tree size instead of re-exploring the same subtree for every branch.
type relativeEval struct {
	steps []relativeStep
	memo  map[relativeChainState]bool
}

// descendantFirst implements the anchored descendant (or child) first step:
// it searches n's element subtree for a node matching first, from which the
// rest of the chain matches.
func (e *relativeEval) descendantFirst(n *html.Node, first Matcher) bool {
	for c := n.FirstChild; c != nil; c = c.NextSibling {
		if c.Type != html.ElementNode {
			continue
		}
		if first.Match(c) && e.chain(c, 1) {
			return true
		}
		if e.descendantFirst(c, first) {
			return true
		}
	}
	return false
}

// chain reports whether steps[idx:] can be walked from n. Unlike the anchored
// first step, these combinators are unrestricted: '+' and '~' may leave the
// anchor's subtree.
func (e *relativeEval) chain(n *html.Node, idx int) bool {
	if idx == len(e.steps) {
		return true
	}
	key := relativeChainState{n, idx}
	if result, seen := e.memo[key]; seen {
		return result
	}
	step := e.steps[idx]
	result := false
	switch step.combinator {
	case ' ':
		for c := n.FirstChild; c != nil; c = c.NextSibling {
			if c.Type != html.ElementNode {
				continue
			}
			if step.compound.Match(c) && e.chain(c, idx+1) {
				result = true
				break
			}
			// The descendant combinator also allows skipping this level.
			if e.chain(c, idx) {
				result = true
				break
			}
		}
	case '>':
		for c := n.FirstChild; c != nil; c = c.NextSibling {
			if c.Type == html.ElementNode && step.compound.Match(c) && e.chain(c, idx+1) {
				result = true
				break
			}
		}
	case '+':
		c := nextElementSibling(n)
		result = c != nil && step.compound.Match(c) && e.chain(c, idx+1)
	case '~':
		for c := n.NextSibling; c != nil; c = c.NextSibling {
			if c.Type == html.ElementNode && step.compound.Match(c) && e.chain(c, idx+1) {
				result = true
				break
			}
		}
	}
	e.memo[key] = result
	return result
}

// nextElementSibling returns the first element node following n, skipping
// text and comment nodes, or nil if there is none.
func nextElementSibling(n *html.Node) *html.Node {
	for c := n.NextSibling; c != nil; c = c.NextSibling {
		if c.Type == html.ElementNode {
			return c
		}
	}
	return nil
}

// Specificity returns the specificity of the most specific selectors
// in the pseudo-class arguments.
// See https://www.w3.org/TR/selectors/#specificity-rules
func (s relativePseudoClassSelector) Specificity() Specificity {
	var max Specificity
	for _, r := range s.rels {
		newSpe := r.sel.Specificity()
		if max.Less(newSpe) {
			max = newSpe
		}
	}

	return max
}

func (c relativePseudoClassSelector) PseudoElement() string {
	return ""
}

// notPseudoClassSelector implements the :not pseudoclass.
type notPseudoClassSelector struct {
	match SelectorGroup
}

func (s notPseudoClassSelector) Match(n *html.Node) bool {
	if n.Type != html.ElementNode {
		return false
	}
	// matches elements that do not match a.
	return !s.match.Match(n)
}

func (s notPseudoClassSelector) Specificity() Specificity {
	var max Specificity

	for _, sel := range s.match {
		newSpe := sel.Specificity()
		if max.Less(newSpe) {
			max = newSpe
		}
	}
	return max
}

func (c notPseudoClassSelector) PseudoElement() string {
	return ""
}

// isPseudoClassSelector implements the :is() and :where() pseudo-classes.
type isPseudoClassSelector struct {
	where bool // :where() instead of :is()
	match SelectorGroup
}

func (s isPseudoClassSelector) Match(n *html.Node) bool {
	if n.Type != html.ElementNode {
		return false
	}
	return s.match.Match(n)
}

// Specificity returns zero for :where() and the specificity of the most
// specific argument for :is().
// See https://www.w3.org/TR/selectors-4/#specificity-rules
func (s isPseudoClassSelector) Specificity() Specificity {
	var max Specificity
	if s.where {
		return max
	}
	for _, sel := range s.match {
		newSpe := sel.Specificity()
		if max.Less(newSpe) {
			max = newSpe
		}
	}
	return max
}

func (c isPseudoClassSelector) PseudoElement() string {
	return ""
}

type containsPseudoClassSelector struct {
	abstractPseudoClass
	value string
	own   bool
}

func (s containsPseudoClassSelector) Match(n *html.Node) bool {
	var text string
	if s.own {
		// matches nodes that directly contain the given text
		text = strings.ToLower(nodeOwnText(n))
	} else {
		// matches nodes that contain the given text.
		text = strings.ToLower(nodeText(n))
	}
	return strings.Contains(text, s.value)
}

type regexpPseudoClassSelector struct {
	abstractPseudoClass
	regexp *regexp.Regexp
	own    bool
}

func (s regexpPseudoClassSelector) Match(n *html.Node) bool {
	var text string
	if s.own {
		// matches nodes whose text directly matches the specified regular expression
		text = nodeOwnText(n)
	} else {
		// matches nodes whose text matches the specified regular expression
		text = nodeText(n)
	}
	return s.regexp.MatchString(text)
}

// writeNodeText writes the text contained in n and its descendants to b.
func writeNodeText(n *html.Node, b *bytes.Buffer) {
	switch n.Type {
	case html.TextNode:
		b.WriteString(n.Data)
	case html.ElementNode:
		for c := n.FirstChild; c != nil; c = c.NextSibling {
			writeNodeText(c, b)
		}
	}
}

// nodeText returns the text contained in n and its descendants.
func nodeText(n *html.Node) string {
	var b bytes.Buffer
	writeNodeText(n, &b)
	return b.String()
}

// nodeOwnText returns the contents of the text nodes that are direct
// children of n.
func nodeOwnText(n *html.Node) string {
	var b bytes.Buffer
	for c := n.FirstChild; c != nil; c = c.NextSibling {
		if c.Type == html.TextNode {
			b.WriteString(c.Data)
		}
	}
	return b.String()
}

type nthPseudoClassSelector struct {
	abstractPseudoClass
	a, b         int
	last, ofType bool
	of           SelectorGroup // the S in :nth-child(An+B of S); nil when absent
}

func (s nthPseudoClassSelector) Match(n *html.Node) bool {
	if s.of != nil {
		return nthOfMatch(s.a, s.b, s.last, s.of, n)
	}
	if s.a == 0 {
		if s.last {
			return simpleNthLastChildMatch(s.b, s.ofType, n)
		} else {
			return simpleNthChildMatch(s.b, s.ofType, n)
		}
	}
	return nthChildMatch(s.a, s.b, s.last, s.ofType, n)
}

// Specificity returns the specificity of a pseudo-class plus that of the
// most specific selector in the 'of' clause, if there is one.
// See https://www.w3.org/TR/selectors-4/#specificity-rules
func (s nthPseudoClassSelector) Specificity() Specificity {
	max := Specificity{0, 1, 0}
	if s.of == nil {
		return max
	}
	for _, sel := range s.of {
		newSpe := sel.Specificity().Add(Specificity{0, 1, 0})
		if max.Less(newSpe) {
			max = newSpe
		}
	}
	return max
}

// nthOfMatch implements :nth-child(an+b of S) and, if last is true,
// :nth-last-child(an+b of S): n must match S and be the an+b-th sibling
// among those that match S.
func nthOfMatch(a, b int, last bool, of SelectorGroup, n *html.Node) bool {
	if n.Type != html.ElementNode || !of.Match(n) {
		return false
	}

	parent := n.Parent
	if parent == nil {
		return false
	}

	i := 0
	if last {
		for c := n; c != nil; c = c.NextSibling {
			if c.Type == html.ElementNode && of.Match(c) {
				i++
			}
		}
	} else {
		for c := n; c != nil; c = c.PrevSibling {
			if c.Type == html.ElementNode && of.Match(c) {
				i++
			}
		}
	}

	i -= b
	if a == 0 {
		return i == 0
	}
	return i%a == 0 && i/a >= 0
}

// nthChildMatch implements :nth-child(an+b).
// If last is true, implements :nth-last-child instead.
// If ofType is true, implements :nth-of-type instead.
func nthChildMatch(a, b int, last, ofType bool, n *html.Node) bool {
	if n.Type != html.ElementNode {
		return false
	}

	parent := n.Parent
	if parent == nil {
		return false
	}

	i := -1
	count := 0
	for c := parent.FirstChild; c != nil; c = c.NextSibling {
		if (c.Type != html.ElementNode) || (ofType && (c.Data != n.Data || c.Namespace != n.Namespace)) {
			continue
		}
		count++
		if c == n {
			i = count
			if !last {
				break
			}
		}
	}

	if i == -1 {
		// This shouldn't happen, since n should always be one of its parent's children.
		return false
	}

	if last {
		i = count - i + 1
	}

	i -= b
	if a == 0 {
		return i == 0
	}

	return i%a == 0 && i/a >= 0
}

// simpleNthChildMatch implements :nth-child(b).
// If ofType is true, implements :nth-of-type instead.
func simpleNthChildMatch(b int, ofType bool, n *html.Node) bool {
	if n.Type != html.ElementNode {
		return false
	}

	parent := n.Parent
	if parent == nil {
		return false
	}

	count := 0
	for c := parent.FirstChild; c != nil; c = c.NextSibling {
		if c.Type != html.ElementNode || (ofType && (c.Data != n.Data || c.Namespace != n.Namespace)) {
			continue
		}
		count++
		if c == n {
			return count == b
		}
		if count >= b {
			return false
		}
	}
	return false
}

// simpleNthLastChildMatch implements :nth-last-child(b).
// If ofType is true, implements :nth-last-of-type instead.
func simpleNthLastChildMatch(b int, ofType bool, n *html.Node) bool {
	if n.Type != html.ElementNode {
		return false
	}

	parent := n.Parent
	if parent == nil {
		return false
	}

	count := 0
	for c := parent.LastChild; c != nil; c = c.PrevSibling {
		if c.Type != html.ElementNode || (ofType && (c.Data != n.Data || c.Namespace != n.Namespace)) {
			continue
		}
		count++
		if c == n {
			return count == b
		}
		if count >= b {
			return false
		}
	}
	return false
}

type onlyChildPseudoClassSelector struct {
	abstractPseudoClass
	ofType bool
}

// Match implements :only-child.
// If `ofType` is true, it implements :only-of-type instead.
func (s onlyChildPseudoClassSelector) Match(n *html.Node) bool {
	if n.Type != html.ElementNode {
		return false
	}

	parent := n.Parent
	if parent == nil {
		return false
	}

	count := 0
	for c := parent.FirstChild; c != nil; c = c.NextSibling {
		if (c.Type != html.ElementNode) || (s.ofType && (c.Data != n.Data || c.Namespace != n.Namespace)) {
			continue
		}
		count++
		if count > 1 {
			return false
		}
	}

	return count == 1
}

type inputPseudoClassSelector struct {
	abstractPseudoClass
}

// Matches input, select, textarea and button elements.
func (s inputPseudoClassSelector) Match(n *html.Node) bool {
	return n.Type == html.ElementNode && (n.Data == "input" || n.Data == "select" || n.Data == "textarea" || n.Data == "button")
}

type emptyElementPseudoClassSelector struct {
	abstractPseudoClass
}

// Matches empty elements.
func (s emptyElementPseudoClassSelector) Match(n *html.Node) bool {
	if n.Type != html.ElementNode {
		return false
	}

	for c := n.FirstChild; c != nil; c = c.NextSibling {
		switch c.Type {
		case html.ElementNode:
			return false
		case html.TextNode:
			if strings.Trim(c.Data, " \t\r\n\f") == "" {
				continue
			} else {
				return false
			}
		}
	}

	return true
}

type rootPseudoClassSelector struct {
	abstractPseudoClass
}

// Match implements :root
func (s rootPseudoClassSelector) Match(n *html.Node) bool {
	if n.Type != html.ElementNode {
		return false
	}
	if n.Parent == nil {
		return false
	}
	return n.Parent.Type == html.DocumentNode
}

func hasAttr(n *html.Node, attr string) bool {
	return matchAttribute(n, attr, func(string) bool { return true })
}

type linkPseudoClassSelector struct {
	abstractPseudoClass
	anyLink bool // written as :any-link
}

// Match implements :link and :any-link. Without visited-link state the
// two pseudo-classes select the same elements.
func (s linkPseudoClassSelector) Match(n *html.Node) bool {
	return (n.DataAtom == atom.A || n.DataAtom == atom.Area || n.DataAtom == atom.Link) && hasAttr(n, "href")
}

type langPseudoClassSelector struct {
	abstractPseudoClass
	lang string
}

func (s langPseudoClassSelector) Match(n *html.Node) bool {
	if n.Type != html.ElementNode {
		return false
	}
	own := matchAttribute(n, "lang", func(val string) bool {
		val = toLowerASCII(val)
		return val == s.lang || strings.HasPrefix(val, s.lang+"-")
	})
	if hasAttr(n, "lang") || n.Parent == nil {
		return own
	}
	return s.Match(n.Parent)
}

type enabledPseudoClassSelector struct {
	abstractPseudoClass
}

func (s enabledPseudoClassSelector) Match(n *html.Node) bool {
	if n.Type != html.ElementNode {
		return false
	}
	switch n.DataAtom {
	case atom.Optgroup, atom.Menuitem, atom.Fieldset:
		return !hasAttr(n, "disabled")
	case atom.Option:
		return !hasAttr(n, "disabled") && !inDisabledFieldset(n) &&
			!(n.Parent != nil && n.Parent.DataAtom == atom.Optgroup && hasAttr(n.Parent, "disabled"))
	case atom.Button, atom.Input, atom.Select, atom.Textarea:
		return !hasAttr(n, "disabled") && !inDisabledFieldset(n)
	}
	return false
}

type disabledPseudoClassSelector struct {
	abstractPseudoClass
}

func (s disabledPseudoClassSelector) Match(n *html.Node) bool {
	if n.Type != html.ElementNode {
		return false
	}
	switch n.DataAtom {
	case atom.Optgroup, atom.Menuitem, atom.Fieldset:
		return hasAttr(n, "disabled")
	case atom.Option:
		return hasAttr(n, "disabled") || inDisabledFieldset(n) ||
			(n.Parent != nil && n.Parent.DataAtom == atom.Optgroup && hasAttr(n.Parent, "disabled"))
	case atom.Button, atom.Input, atom.Select, atom.Textarea:
		return hasAttr(n, "disabled") || inDisabledFieldset(n)
	}
	return false
}

func hasLegendInPreviousSiblings(n *html.Node) bool {
	for s := n.PrevSibling; s != nil; s = s.PrevSibling {
		if s.DataAtom == atom.Legend {
			return true
		}
	}
	return false
}

func inDisabledFieldset(n *html.Node) bool {
	if n.Parent == nil {
		return false
	}
	if n.Parent.DataAtom == atom.Fieldset && hasAttr(n.Parent, "disabled") &&
		(n.DataAtom != atom.Legend || hasLegendInPreviousSiblings(n)) {
		return true
	}
	return inDisabledFieldset(n.Parent)
}

type checkedPseudoClassSelector struct {
	abstractPseudoClass
}

func (s checkedPseudoClassSelector) Match(n *html.Node) bool {
	if n.Type != html.ElementNode {
		return false
	}
	switch n.DataAtom {
	case atom.Input, atom.Menuitem:
		return hasAttr(n, "checked") && matchAttribute(n, "type", func(val string) bool {
			t := toLowerASCII(val)
			return t == "checkbox" || t == "radio"
		})
	case atom.Option:
		return hasAttr(n, "selected")
	}
	return false
}
