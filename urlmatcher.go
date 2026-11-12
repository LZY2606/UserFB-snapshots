package robotstxt

// Compiled URL matcher.
//
// This file implements an alternative, compile-once query entry point:
//
//	matcher, err := group.CompileURLMatcher()
//	allow, err := matcher.Test("https://example.com/some/path?query")
//
// The semantics here intentionally differ from the legacy Group.Test in a
// few documented ways (see README.rst):
//
//   - Rules always match from the first byte of the path.
//   - A bare '*' matches zero or more octets; a bare '$' is an end anchor
//     only when it is the last byte of the rule, elsewhere it is literal.
//   - Percent-encoded octets are single matching units; '%2A' and '%24'
//     are always literal '*' and '$', never operators.
//   - Encoded and raw forms of the same octet are equivalent, except that
//     encoded '/' (%2F) and '?' (%3F) stay distinct from the real path and
//     query separators.
//   - Among matching rules the one whose literal part covers the most
//     octets wins ('*' and the end anchor do not count); ties are resolved
//     in favour of Allow, regardless of rule order.

import (
	"fmt"
	"net/url"
	"strings"
	"sync/atomic"
)

// URLMatcher is a compiled, immutable form of a Group's rules. It is safe
// for concurrent use by multiple goroutines.
type URLMatcher struct {
	disallowAll bool
	root        *matchNode
	globs       []globRule

	// steps, when non-nil, counts path/state transitions performed during
	// matching. It is a test aid and stays nil in normal use.
	steps *atomic.Int64
}

// matchNode is a node of the trie shared by all literal (wildcard-free)
// rules. Rules with equal normalized prefixes share the same nodes, so a
// query walks each path octet at most once no matter how many rules exist.
type matchNode struct {
	children map[uint16]*matchNode
	prefix   *decision // best non-anchored literal rule ending here
	exact    *decision // best anchored (trailing '$') literal rule ending here
}

// decision is the outcome attached to a single rule.
type decision struct {
	length int // number of literal octets in the rule
	allow  bool
}

// globRule is a compiled rule containing at least one bare '*'.
type globRule struct {
	tokens   []patternToken
	anchored bool
	decision
}

type patternToken struct {
	wildcard bool
	key      uint16 // valid when wildcard == false
}

// CompileURLMatcher compiles the group's rules into a reusable URLMatcher.
// Rules with malformed percent-encoding are reported as an error; the
// group's legacy query methods (Group.Test, RobotsData.TestAgent) are not
// affected by such rules.
func (g *Group) CompileURLMatcher() (*URLMatcher, error) {
	m := &URLMatcher{
		disallowAll: g.disallowAll,
		root:        &matchNode{},
	}
	for _, r := range g.rules {
		tokens, anchored, litLen, err := compilePattern(r.raw)
		if err != nil {
			return nil, fmt.Errorf("robotstxt: cannot compile rule %q: %w", r.raw, err)
		}
		d := decision{length: litLen, allow: r.allow}
		if hasWildcard(tokens) {
			m.globs = append(m.globs, globRule{tokens: tokens, anchored: anchored, decision: d})
		} else {
			m.root.insert(tokens, anchored, d)
		}
	}
	return m, nil
}

// Test reports whether the group's rules allow fetching rawURL, which must
// be an absolute http or https URL with a host. The matched string is the
// escaped path (empty path becomes "/") plus the query when present; the
// fragment never participates.
func (m *URLMatcher) Test(rawURL string) (bool, error) {
	if m.disallowAll {
		return false, nil
	}
	units, err := urlToUnits(rawURL)
	if err != nil {
		return false, err
	}
	if d, ok := m.match(units); ok {
		return d.allow, nil
	}
	// From Google's spec:
	// By default, there are no restrictions for crawling for the designated crawlers.
	return true, nil
}

func (m *URLMatcher) match(units []uint16) (decision, bool) {
	var best decision
	found := false
	consider := func(d decision) {
		if !found || d.length > best.length || (d.length == best.length && d.allow) {
			best, found = d, true
		}
	}

	// Walk the literal trie, one transition per path octet.
	node := m.root
	if node.prefix != nil {
		consider(*node.prefix)
	}
	depth := 0
	for _, k := range units {
		next := node.children[k]
		if next == nil {
			break
		}
		node = next
		depth++
		m.countStep()
		if node.prefix != nil {
			consider(*node.prefix)
		}
	}
	if depth == len(units) && node.exact != nil {
		consider(*node.exact)
	}

	// Wildcard rules may keep several branches alive; evaluate each.
	for i := range m.globs {
		if m.globs[i].match(units, m.countStep) {
			consider(m.globs[i].decision)
		}
	}
	return best, found
}

func (m *URLMatcher) countStep() {
	if m.steps != nil {
		m.steps.Add(1)
	}
}

func (n *matchNode) insert(tokens []patternToken, anchored bool, d decision) {
	node := n
	for _, t := range tokens {
		if node.children == nil {
			node.children = make(map[uint16]*matchNode)
		}
		child := node.children[t.key]
		if child == nil {
			child = &matchNode{}
			node.children[t.key] = child
		}
		node = child
	}
	slot := &node.prefix
	if anchored {
		slot = &node.exact
	}
	// Same normalized literal: keep Allow on ties, order must not matter.
	if *slot == nil || (d.allow && !(*slot).allow) {
		dd := d
		*slot = &dd
	}
}

func hasWildcard(tokens []patternToken) bool {
	for _, t := range tokens {
		if t.wildcard {
			return true
		}
	}
	return false
}

// compilePattern tokenizes rule text as received from the parser. A bare
// '*' becomes a wildcard token, a bare '$' in the last position becomes the
// end anchor (elsewhere it is a literal), and every percent-encoded octet
// becomes a single literal token. Malformed percent sequences are an error.
func compilePattern(raw string) (tokens []patternToken, anchored bool, litLen int, err error) {
	for i := 0; i < len(raw); {
		c := raw[i]
		switch {
		case c == '%':
			b, e := decodeOctet(raw, i)
			if e != nil {
				return nil, false, 0, e
			}
			tokens = append(tokens, patternToken{key: unitKey(b, true)})
			i += 3
		case c == '*':
			tokens = append(tokens, patternToken{wildcard: true})
			i++
		case c == '$' && i == len(raw)-1:
			anchored = true
			i++
		default:
			tokens = append(tokens, patternToken{key: unitKey(c, false)})
			i++
		}
	}
	for _, t := range tokens {
		if !t.wildcard {
			litLen++
		}
	}
	return tokens, anchored, litLen, nil
}

// match implements glob matching of the token pattern against the URL
// units. Without the end anchor the pattern is a prefix pattern: it matches
// as soon as it is exhausted. '*' consumes whole units only, so it can
// never swallow half of a percent-encoded octet.
func (g *globRule) match(units []uint16, countStep func()) bool {
	tokens := g.tokens
	ti, ui := 0, 0
	starT, starU := -1, 0
	for {
		if ti == len(tokens) {
			// Pattern exhausted: prefix match unless anchored.
			return !g.anchored || ui == len(units)
		}
		if tokens[ti].wildcard {
			starT, starU = ti, ui
			ti++
			continue
		}
		if ui < len(units) {
			if countStep != nil {
				countStep()
			}
			if tokens[ti].key == units[ui] {
				ti++
				ui++
				continue
			}
		}
		if starT >= 0 && starU < len(units) {
			// Let the last '*' eat one more unit and retry.
			ti = starT + 1
			starU++
			ui = starU
			continue
		}
		return false
	}
}

// unitKey maps an octet to its matching identity. Percent-encoded and raw
// forms are equivalent, except for the structural separators '/' and '?':
// an encoded slash is not a path separator and an encoded question mark
// does not start the query.
func unitKey(b byte, escaped bool) uint16 {
	if escaped && (b == '/' || b == '?') {
		return 0x100 | uint16(b)
	}
	return uint16(b)
}

// urlToUnits validates rawURL and converts the matched portion (escaped
// path plus query) into matching units.
func urlToUnits(rawURL string) ([]uint16, error) {
	// The fragment never participates in matching; drop it before parsing
	// so that even a malformed fragment cannot fail the query.
	if i := strings.IndexByte(rawURL, '#'); i >= 0 {
		rawURL = rawURL[:i]
	}
	u, err := url.Parse(rawURL)
	if err != nil {
		return nil, fmt.Errorf("robotstxt: invalid URL %q: %w", rawURL, err)
	}
	if scheme := strings.ToLower(u.Scheme); scheme != "http" && scheme != "https" {
		return nil, fmt.Errorf("robotstxt: URL %q: scheme must be http or https", rawURL)
	}
	if u.Host == "" {
		return nil, fmt.Errorf("robotstxt: URL %q: host is required", rawURL)
	}
	path := u.EscapedPath()
	if path == "" {
		path = "/"
	}
	s := path
	if u.ForceQuery || u.RawQuery != "" {
		// Keep the '?' even for a present-but-empty query. The query is
		// matched as-is: no parameter decoding or reordering.
		s += "?" + u.RawQuery
	}
	units, err := stringToUnits(s)
	if err != nil {
		return nil, fmt.Errorf("robotstxt: invalid URL %q: %w", rawURL, err)
	}
	return units, nil
}

// stringToUnits converts an escaped string into matching units, validating
// percent-encodings. Hex case is normalized by decoding.
func stringToUnits(s string) ([]uint16, error) {
	units := make([]uint16, 0, len(s))
	for i := 0; i < len(s); {
		if s[i] == '%' {
			b, err := decodeOctet(s, i)
			if err != nil {
				return nil, err
			}
			units = append(units, unitKey(b, true))
			i += 3
		} else {
			units = append(units, unitKey(s[i], false))
			i++
		}
	}
	return units, nil
}

func decodeOctet(s string, i int) (byte, error) {
	if i+2 >= len(s) || !isHexDigit(s[i+1]) || !isHexDigit(s[i+2]) {
		return 0, fmt.Errorf("invalid percent-encoding at offset %d", i)
	}
	return unhex(s[i+1])<<4 | unhex(s[i+2]), nil
}

func isHexDigit(c byte) bool {
	return '0' <= c && c <= '9' || 'a' <= c && c <= 'f' || 'A' <= c && c <= 'F'
}

func unhex(c byte) byte {
	switch {
	case '0' <= c && c <= '9':
		return c - '0'
	case 'a' <= c && c <= 'f':
		return c - 'a' + 10
	default: // 'A' <= c && c <= 'F'
		return c - 'A' + 10
	}
}
