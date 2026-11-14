package opengraph

import (
	"bytes"
	"fmt"
	"net/url"
	"sort"
	"strings"

	"golang.org/x/net/html"
)

// Microformats2 support.
//
// This implements the subset of the Microformats2 parsing specification
// described in the task: modern h-* roots and p-*/u-*/e-* properties.
// dt-*, legacy microformats, include/itemref/headers and rel collections are
// intentionally out of scope.

// mfPropertyPrefixes lists the property prefixes handled by this package.
const mfPropertyPrefixes = "pue"

// mfOpaque reports whether n is an element whose subtree must never be
// searched for roots or properties.
func mfOpaque(n *html.Node) bool {
	if n == nil || n.Type != html.ElementNode {
		return false
	}
	switch n.Data {
	case "script", "style", "template":
		return true
	}
	return false
}

// mfClassWords splits a class attribute the way HTML splits ASCII whitespace.
func mfClassWords(s string) []string {
	var words []string
	start := -1
	for i := 0; i < len(s); i++ {
		switch s[i] {
		case ' ', '\t', '\n', '\f', '\r':
			if start >= 0 {
				words = append(words, s[start:i])
				start = -1
			}
		default:
			if start < 0 {
				start = i
			}
		}
	}
	if start >= 0 {
		words = append(words, s[start:])
	}
	return words
}

// mfTrim trims HTML ASCII whitespace from both ends.
func mfTrim(s string) string {
	return strings.TrimFunc(s, func(r rune) bool {
		switch r {
		case ' ', '\t', '\n', '\f', '\r':
			return true
		}
		return false
	})
}

// mfValidSuffix reports whether s matches [a-z]+(?:-[a-z]+)*.
func mfValidSuffix(s string) bool {
	if s == "" {
		return false
	}
	for _, part := range strings.Split(s, "-") {
		if part == "" {
			return false
		}
		for _, r := range part {
			if r < 'a' || r > 'z' {
				return false
			}
		}
	}
	return true
}

// mfAttr returns the value of the named attribute and whether it is present.
func mfAttr(n *html.Node, key string) (string, bool) {
	for _, attr := range n.Attr {
		if attr.Key == key {
			return attr.Val, true
		}
	}
	return "", false
}

// mfRootTypes returns the sorted, deduplicated h-* root class names of n.
func mfRootTypes(n *html.Node) ([]string, bool) {
	if n == nil || n.Type != html.ElementNode {
		return nil, false
	}
	class, ok := mfAttr(n, "class")
	if !ok {
		return nil, false
	}
	seen := map[string]bool{}
	var types []string
	for _, word := range mfClassWords(class) {
		if !strings.HasPrefix(word, "h-") || !mfValidSuffix(word[2:]) || seen[word] {
			continue
		}
		seen[word] = true
		types = append(types, word)
	}
	if len(types) == 0 {
		return nil, false
	}
	sort.Strings(types)
	return types, true
}

func mfIsRoot(n *html.Node) bool {
	_, ok := mfRootTypes(n)
	return ok
}

// mfPropertyToken describes one valid p-*/u-*/e-* class token.
type mfPropertyToken struct {
	prefix byte
	name   string
}

// mfPropertyTokens returns valid property tokens of n in class order,
// with duplicate tokens removed.
func mfPropertyTokens(n *html.Node) []mfPropertyToken {
	if n == nil || n.Type != html.ElementNode {
		return nil
	}
	class, ok := mfAttr(n, "class")
	if !ok {
		return nil
	}
	seen := map[string]bool{}
	var tokens []mfPropertyToken
	for _, word := range mfClassWords(class) {
		if len(word) < 3 || word[1] != '-' {
			continue
		}
		prefix := word[0]
		if !strings.ContainsRune(mfPropertyPrefixes, rune(prefix)) {
			continue
		}
		if !mfValidSuffix(word[2:]) || seen[word] {
			continue
		}
		seen[word] = true
		tokens = append(tokens, mfPropertyToken{prefix: prefix, name: word[2:]})
	}
	return tokens
}

// mfElementChildren returns the element children of n, in document order.
func mfElementChildren(n *html.Node) []*html.Node {
	var children []*html.Node
	for c := n.FirstChild; c != nil; c = c.NextSibling {
		if c.Type == html.ElementNode {
			children = append(children, c)
		}
	}
	return children
}

// mfOnlyOfType returns the single element child with the given tag name.
func mfOnlyOfType(children []*html.Node, tag string) (*html.Node, bool) {
	var found *html.Node
	for _, c := range children {
		if c.Data != tag {
			continue
		}
		if found != nil {
			return nil, false
		}
		found = c
	}
	return found, found != nil
}

// mfClone deep-copies the map/slice shapes produced by this package so that
// separately projected objects never share backing arrays.
func mfClone(v any) any {
	switch t := v.(type) {
	case map[string]any:
		m := make(map[string]any, len(t))
		for k, value := range t {
			m[k] = mfClone(value)
		}
		return m
	case []any:
		s := make([]any, len(t))
		for i, value := range t {
			s[i] = mfClone(value)
		}
		return s
	case []string:
		s := make([]string, len(t))
		copy(s, t)
		return s
	default:
		return v
	}
}

// mfParser carries per-collection state. A fresh parser is created for every
// Parse/Walk/Fetch invocation so no state leaks between collections.
type mfParser struct {
	base *url.URL
}

// resolve turns raw into an absolute URL against the effective document base.
func (p *mfParser) resolve(raw string) string {
	ref, err := url.Parse(raw)
	if err != nil {
		return raw
	}
	return p.base.ResolveReference(ref).String()
}

// innerHTML serializes the children of n.
func mfInnerHTML(n *html.Node) string {
	var buf bytes.Buffer
	for c := n.FirstChild; c != nil; c = c.NextSibling {
		_ = html.Render(&buf, c)
	}
	return mfTrim(buf.String())
}

// textP implements the text rule shared by p-* and e-* values: opaque
// subtrees are skipped and embedded images contribute their alt (falling
// back to the resolved src) surrounded by single spaces.
func (p *mfParser) textP(n *html.Node) string {
	var b strings.Builder
	var walk func(*html.Node)
	walk = func(x *html.Node) {
		for c := x.FirstChild; c != nil; c = c.NextSibling {
			switch c.Type {
			case html.TextNode:
				b.WriteString(c.Data)
			case html.ElementNode:
				if mfOpaque(c) {
					continue
				}
				if c.Data == "img" {
					alt, ok := mfAttr(c, "alt")
					if !ok {
						if src, ok := mfAttr(c, "src"); ok {
							alt = p.resolve(src)
						}
					}
					b.WriteString(" ")
					b.WriteString(alt)
					b.WriteString(" ")
					continue
				}
				walk(c)
			}
		}
	}
	walk(n)
	return mfTrim(b.String())
}

// textPlain implements the plain text fallback used for u-* values.
func (p *mfParser) textPlain(n *html.Node) string {
	var b strings.Builder
	var walk func(*html.Node)
	walk = func(x *html.Node) {
		for c := x.FirstChild; c != nil; c = c.NextSibling {
			switch c.Type {
			case html.TextNode:
				b.WriteString(c.Data)
			case html.ElementNode:
				if mfOpaque(c) {
					continue
				}
				walk(c)
			}
		}
	}
	walk(n)
	return mfTrim(b.String())
}

// textImplied is the text used by implied name: images contribute their alt
// without surrounding spaces and without falling back to src.
func (p *mfParser) textImplied(n *html.Node) string {
	var b strings.Builder
	var walk func(*html.Node)
	walk = func(x *html.Node) {
		for c := x.FirstChild; c != nil; c = c.NextSibling {
			switch c.Type {
			case html.TextNode:
				b.WriteString(c.Data)
			case html.ElementNode:
				if mfOpaque(c) {
					continue
				}
				if c.Data == "img" {
					alt, _ := mfAttr(c, "alt")
					b.WriteString(alt)
					continue
				}
				walk(c)
			}
		}
	}
	walk(n)
	return mfTrim(b.String())
}

// mfURLAttr extracts a URL-bearing attribute according to element kind.
func mfURLAttr(n *html.Node) (string, bool) {
	switch n.Data {
	case "a", "area", "link":
		return mfAttr(n, "href")
	case "img":
		return mfAttr(n, "src")
	case "audio", "video", "source", "iframe":
		if v, ok := mfAttr(n, "src"); ok {
			return v, true
		}
		if n.Data == "video" {
			return mfAttr(n, "poster")
		}
		return "", false
	case "object":
		return mfAttr(n, "data")
	}
	return "", false
}

// valueFragments implements the value-class-pattern. It returns the
// concatenated fragments (in document order) and whether at least one
// fragment (possibly empty) was found. A selected "value" fragment is
// evaluated with the p-* rules and is not searched any deeper.
func (p *mfParser) valueFragments(n *html.Node) (string, bool) {
	var parts []string
	hit := false
	var walk func(*html.Node)
	walk = func(x *html.Node) {
		for c := x.FirstChild; c != nil; c = c.NextSibling {
			if c.Type != html.ElementNode || mfOpaque(c) {
				continue
			}
			// Never step into another property element or a nested root.
			if mfIsRoot(c) || len(mfPropertyTokens(c)) > 0 {
				continue
			}
			isTitle, isValue := false, false
			class, _ := mfAttr(c, "class")
			for _, word := range mfClassWords(class) {
				switch word {
				case "value-title":
					isTitle = true
				case "value":
					isValue = true
				}
			}
			if isTitle {
				if title, ok := mfAttr(c, "title"); ok {
					parts = append(parts, title)
				}
				hit = true
				continue // A selected fragment's descendants are not searched.
			}
			if isValue {
				parts = append(parts, p.pValueShallow(c))
				hit = true
				continue
			}
			walk(c)
		}
	}
	walk(n)
	if !hit {
		return "", false
	}
	return mfTrim(strings.Join(parts, "")), true
}

// pValueShallow applies the p-* element-kind rules without value-class-pattern.
func (p *mfParser) pValueShallow(n *html.Node) string {
	switch n.Data {
	case "abbr", "link":
		if v, ok := mfAttr(n, "title"); ok {
			return v
		}
	case "data", "input":
		if v, ok := mfAttr(n, "value"); ok {
			return v
		}
	case "img", "area":
		if v, ok := mfAttr(n, "alt"); ok {
			return v
		}
	}
	return p.textP(n)
}

// pValue extracts a p-* value.
func (p *mfParser) pValue(n *html.Node) string {
	if v, ok := p.valueFragments(n); ok {
		return v
	}
	return p.pValueShallow(n)
}

// uValue extracts a u-* value and resolves it against the document base.
func (p *mfParser) uValue(n *html.Node) any {
	var raw string
	if v, ok := mfURLAttr(n); ok {
		raw = v
	} else if v, ok := p.valueFragments(n); ok {
		raw = v
	} else if n.Data == "abbr" {
		if v, ok := mfAttr(n, "title"); ok {
			raw = v
		} else {
			raw = p.textPlain(n)
		}
	} else if n.Data == "data" || n.Data == "input" {
		if v, ok := mfAttr(n, "value"); ok {
			raw = v
		} else {
			raw = p.textPlain(n)
		}
	} else {
		raw = p.textPlain(n)
	}
	abs := p.resolve(raw)
	if n.Data == "img" {
		if alt, ok := mfAttr(n, "alt"); ok {
			return map[string]any{"value": abs, "alt": alt}
		}
	}
	return abs
}

// eValue extracts an e-* value.
func (p *mfParser) eValue(n *html.Node) map[string]any {
	return map[string]any{
		"html":  mfInnerHTML(n),
		"value": p.textP(n),
	}
}

// mfFirstString returns the first name value when it is a plain string.
func mfFirstString(values any) (string, bool) {
	list, ok := values.([]any)
	if !ok || len(list) == 0 {
		return "", false
	}
	s, ok := list[0].(string)
	return s, ok
}

// mfFirstURLString returns the first u-* value as a string, falling back to
// its "value" member when the entry is an object.
func mfFirstURLString(values any) (string, bool) {
	list, ok := values.([]any)
	if !ok || len(list) == 0 {
		return "", false
	}
	if s, ok := list[0].(string); ok {
		return s, true
	}
	if m, ok := list[0].(map[string]any); ok {
		if s, ok := m["value"].(string); ok {
			return s, true
		}
	}
	return "", false
}

// mfObjectProperty returns the properties member of a parsed root object.
func mfObjectProperty(obj map[string]any, name string) any {
	properties, ok := obj["properties"].(map[string]any)
	if !ok {
		return nil
	}
	return properties[name]
}

// projectP builds the object shape used when a nested root is the value of a
// p-* property.
func (p *mfParser) projectP(obj map[string]any, node *html.Node) map[string]any {
	m := mfClone(obj).(map[string]any)
	if v, ok := mfFirstString(mfObjectProperty(obj, "name")); ok {
		m["value"] = v
	} else {
		m["value"] = p.pValue(node)
	}
	return m
}

// projectU builds the object shape used when a nested root is the value of a
// u-* property.
func (p *mfParser) projectU(obj map[string]any, node *html.Node) map[string]any {
	m := mfClone(obj).(map[string]any)
	if v, ok := mfFirstURLString(mfObjectProperty(obj, "url")); ok {
		m["value"] = v
	} else {
		switch v := p.uValue(node).(type) {
		case string:
			m["value"] = v
		case map[string]any:
			m["value"] = v["value"]
		default:
			m["value"] = ""
		}
	}
	return m
}

// projectE builds the object shape used when a nested root is the value of an
// e-* property: the parsed child object plus its html and value.
func (p *mfParser) projectE(obj map[string]any, node *html.Node) map[string]any {
	m := mfClone(obj).(map[string]any)
	m["html"] = mfInnerHTML(node)
	m["value"] = p.textP(node)
	return m
}

// parseRoot parses one h-* root into a Microformats2 object.
func (p *mfParser) parseRoot(n *html.Node, types []string) map[string]any {
	typeList := make([]any, len(types))
	for i, t := range types {
		typeList[i] = t
	}
	obj := map[string]any{"type": typeList}
	if id, ok := mfAttr(n, "id"); ok && id != "" {
		obj["id"] = id
	}
	properties := map[string]any{}
	appendProp := func(name string, value any) {
		list, _ := properties[name].([]any)
		properties[name] = append(list, value)
	}
	var children []any
	var seenP, seenU, seenE, seenRoot bool

	var scan func(*html.Node)
	scan = func(x *html.Node) {
		for c := x.FirstChild; c != nil; c = c.NextSibling {
			if c.Type != html.ElementNode || mfOpaque(c) {
				continue
			}
			if childTypes, ok := mfRootTypes(c); ok {
				seenRoot = true
				child := p.parseRoot(c, childTypes)
				tokens := mfPropertyTokens(c)
				projected := false
				for _, token := range tokens {
					projected = true
					switch token.prefix {
					case 'p':
						seenP = true
						appendProp(token.name, p.projectP(child, c))
					case 'u':
						seenU = true
						appendProp(token.name, p.projectU(child, c))
					case 'e':
						seenE = true
						appendProp(token.name, p.projectE(child, c))
					}
				}
				if !projected {
					children = append(children, mfClone(child))
				}
				continue // The child root's internals belong to the child only.
			}
			for _, token := range mfPropertyTokens(c) {
				switch token.prefix {
				case 'p':
					seenP = true
					appendProp(token.name, p.pValue(c))
				case 'u':
					seenU = true
					appendProp(token.name, p.uValue(c))
				case 'e':
					seenE = true
					appendProp(token.name, p.eValue(c))
				}
			}
			scan(c)
		}
	}
	scan(n)

	if !seenP && !seenE && !seenRoot {
		appendProp("name", p.impliedName(n))
	}
	if !seenU && !seenRoot {
		if photo, ok := p.impliedPhoto(n); ok {
			appendProp("photo", photo)
		}
		if u, ok := p.impliedURL(n); ok {
			appendProp("url", u)
		}
	}

	obj["properties"] = properties
	if len(children) > 0 {
		obj["children"] = children
	}
	return obj
}

// impliedName implements "parsing for implied name".
func (p *mfParser) impliedName(root *html.Node) string {
	switch root.Data {
	case "img", "area":
		if alt, ok := mfAttr(root, "alt"); ok {
			return alt
		}
	case "abbr":
		if title, ok := mfAttr(root, "title"); ok {
			return title
		}
	}
	children := mfElementChildren(root)
	if len(children) == 1 && !mfIsRoot(children[0]) && !mfOpaque(children[0]) {
		wrapper := children[0]
		if v, ok := mfNameCandidate(wrapper); ok {
			return v
		}
		grandchildren := mfElementChildren(wrapper)
		if len(grandchildren) == 1 && !mfIsRoot(grandchildren[0]) && !mfOpaque(grandchildren[0]) {
			if v, ok := mfNameCandidate(grandchildren[0]); ok {
				return v
			}
		}
	}
	return p.textImplied(root)
}

// mfNameCandidate returns a non-empty implied-name candidate value.
func mfNameCandidate(n *html.Node) (string, bool) {
	switch n.Data {
	case "img", "area":
		if alt, ok := mfAttr(n, "alt"); ok && alt != "" {
			return alt, true
		}
	case "abbr":
		if title, ok := mfAttr(n, "title"); ok && title != "" {
			return title, true
		}
	}
	return "", false
}

// mfPhotoFrom extracts an implied photo from a single candidate element.
func (p *mfParser) mfPhotoFrom(n *html.Node) (any, bool) {
	switch n.Data {
	case "img":
		src, ok := mfAttr(n, "src")
		if !ok {
			return nil, false
		}
		abs := p.resolve(src)
		if alt, ok := mfAttr(n, "alt"); ok {
			return map[string]any{"value": abs, "alt": alt}, true
		}
		return abs, true
	case "object":
		data, ok := mfAttr(n, "data")
		if !ok {
			return nil, false
		}
		return p.resolve(data), true
	}
	return nil, false
}

// impliedPhoto implements "parsing for implied photo".
func (p *mfParser) impliedPhoto(root *html.Node) (any, bool) {
	if !mfOpaque(root) {
		if v, ok := p.mfPhotoFrom(root); ok {
			return v, true
		}
	}
	children := mfElementChildren(root)
	for _, tag := range []string{"img", "object"} {
		if n, ok := mfOnlyOfType(children, tag); ok && !mfIsRoot(n) {
			if v, ok := p.mfPhotoFrom(n); ok {
				return v, true
			}
		}
	}
	if len(children) == 1 && !mfIsRoot(children[0]) && !mfOpaque(children[0]) {
		grandchildren := mfElementChildren(children[0])
		for _, tag := range []string{"img", "object"} {
			if n, ok := mfOnlyOfType(grandchildren, tag); ok && !mfIsRoot(n) && !mfOpaque(n) {
				if v, ok := p.mfPhotoFrom(n); ok {
					return v, true
				}
			}
		}
	}
	return nil, false
}

// mfURLFrom extracts an implied url from a single candidate element.
func (p *mfParser) mfURLFrom(n *html.Node) (string, bool) {
	switch n.Data {
	case "a", "area":
		href, ok := mfAttr(n, "href")
		if !ok {
			return "", false
		}
		return p.resolve(href), true
	}
	return "", false
}

// impliedURL implements "parsing for implied url".
func (p *mfParser) impliedURL(root *html.Node) (string, bool) {
	if !mfOpaque(root) {
		if v, ok := p.mfURLFrom(root); ok {
			return v, true
		}
	}
	children := mfElementChildren(root)
	for _, tag := range []string{"a", "area"} {
		if n, ok := mfOnlyOfType(children, tag); ok && !mfIsRoot(n) {
			if v, ok := p.mfURLFrom(n); ok {
				return v, true
			}
		}
	}
	if len(children) == 1 && !mfIsRoot(children[0]) && !mfOpaque(children[0]) {
		grandchildren := mfElementChildren(children[0])
		for _, tag := range []string{"a", "area"} {
			if n, ok := mfOnlyOfType(grandchildren, tag); ok && !mfIsRoot(n) && !mfOpaque(n) {
				if v, ok := p.mfURLFrom(n); ok {
					return v, true
				}
			}
		}
	}
	return "", false
}

// mfFindBase returns the first base[href] inside the traversal scope,
// skipping template subtrees.
func mfFindBase(scope *html.Node) *html.Node {
	var found *html.Node
	var walk func(*html.Node)
	walk = func(n *html.Node) {
		if found != nil {
			return
		}
		for c := n.FirstChild; c != nil; c = c.NextSibling {
			if c.Type != html.ElementNode || mfOpaque(c) {
				continue
			}
			if c.Data == "base" {
				if _, ok := mfAttr(c, "href"); ok {
					found = c
					return
				}
			}
			walk(c)
		}
	}
	if scope.Type == html.ElementNode && !mfOpaque(scope) && scope.Data == "base" {
		if _, ok := mfAttr(scope, "href"); ok {
			return scope
		}
	}
	walk(scope)
	return found
}

// parseMicroformats collects the top-level Microformats2 roots in scope.
func (p *mfParser) parseMicroformats(scope *html.Node) []map[string]any {
	result := []map[string]any{}
	var walk func(*html.Node)
	walk = func(n *html.Node) {
		if n.Type == html.ElementNode {
			if mfOpaque(n) {
				return
			}
			if types, ok := mfRootTypes(n); ok {
				result = append(result, p.parseRoot(n, types))
				return
			}
		}
		for c := n.FirstChild; c != nil; c = c.NextSibling {
			walk(c)
		}
	}
	walk(scope)
	return result
}

// collectMicroformats extracts Microformats2 data into og.Microformats.
// The existing field is left untouched when an error is returned.
func (og *OpenGraph) collectMicroformats(scope *html.Node, initialBase string) error {
	base, err := url.Parse(initialBase)
	if err != nil {
		return err
	}
	if !base.IsAbs() || (base.Scheme != "http" && base.Scheme != "https") || base.Host == "" {
		return fmt.Errorf("microformats: base URL must be absolute HTTP(S): %q", initialBase)
	}
	if b := mfFindBase(scope); b != nil {
		href, _ := mfAttr(b, "href")
		ref, err := url.Parse(href)
		if err != nil {
			return err
		}
		base = base.ResolveReference(ref)
	}
	parser := &mfParser{base: base}
	og.Microformats = parser.parseMicroformats(scope)
	return nil
}
