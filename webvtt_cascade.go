package astisub

import (
	"sort"
	"strings"
)

// https://www.w3.org/TR/webvtt1/#styling
//
// This file implements the cascade of the WebVTT STYLE blocks stored on
// StyleAttributes.WebVTTStyles. Only a fixed, explicit subset of CSS is
// understood; when none of the STYLE blocks contain a supported declaration
// the legacy parsing path is kept untouched.

// Supported cascade properties, in their canonical (lowercase) spelling.
const (
	cascadePropColor           = "color"
	cascadePropBackgroundColor = "background-color"
	cascadePropFontWeight      = "font-weight"
	cascadePropFontStyle       = "font-style"
	cascadePropTextDecoration  = "text-decoration"
)

// cascadeInitials holds the initial computed value of every supported
// property. The WebVTT initial values are white text on a transparent black
// background, normal weight, normal style and no text decoration.
var cascadeInitials = cueComputedStyle{
	color:           "#ffffff",
	backgroundColor: "#000000",
	fontWeight:      "normal",
	fontStyle:       "normal",
	textDecoration:  "none",
}

// cueComputedStyle is the fully resolved value of the five supported
// properties for one node of a cue's tag stack. Color values are stored in
// the same spelling accepted by the TTML writer (hex or a named color).
type cueComputedStyle struct {
	color           string
	backgroundColor string
	fontWeight      string
	fontStyle       string
	textDecoration  string
}

// cueSelector is a parsed ::cue selector. A zero tagName matches the cue root
// (the bare ::cue form); otherwise tagName is one of c/b/i/u and classes is
// the (possibly empty) class list.
type cueSelector struct {
	tagName     string
	classes     []string
	specificity int
}

// cueDeclaration is one supported, syntactically valid declaration.
type cueDeclaration struct {
	property  string
	value     string
	important bool
	order     int
	selector  cueSelector
}

// cueRule is a selector group plus the declarations shared by every selector
// in the group. An unsupported selector only drops that selector: the other
// selectors of the same group still match.
type cueRule struct {
	selectors    []cueSelector
	declarations []cueDeclaration
}

// cueStylesheet is the parsed, order-preserving result of every STYLE block.
type cueStylesheet struct {
	rules []cueRule
	// active is false when no supported, valid declaration was found, in
	// which case callers must stay on the legacy path.
	active bool
}

// cueNode describes one node of a run's tag stack when matching selectors.
type cueNode struct {
	tagName string
	classes []string
}

// webVTTNamedColors are the eight extended CSS color keywords defined by the
// WebVTT styling subset, mapped to their canonical hex spelling.
var webVTTNamedColors = map[string]string{
	"black":   "#000000",
	"blue":    "#0000ff",
	"cyan":    "#00ffff",
	"green":   "#008000",
	"magenta": "#ff00ff",
	"red":     "#ff0000",
	"white":   "#ffffff",
	"yellow":  "#ffff00",
}

// webVTTClassColors mirrors newColorFromWebVTTString: the color names the
// built-in <c.name>/<c.bg_name> classes understood by the legacy parser. They
// only seed the lowest-priority defaults and are deliberately broader than
// the eight keywords accepted in authored STYLE declarations.
var webVTTClassColors = map[string]string{
	"black":   "#000000",
	"red":     "#ff0000",
	"green":   "#008000",
	"yellow":  "#ffff00",
	"blue":    "#0000ff",
	"magenta": "#ff00ff",
	"cyan":    "#00ffff",
	"white":   "#ffffff",
	"silver":  "#c0c0c0",
	"gray":    "#808080",
	"maroon":  "#800000",
	"olive":   "#808000",
	"lime":    "#00ff00",
	"teal":    "#008080",
	"navy":    "#000080",
	"purple":  "#800080",
}

// isASCIILetter reports whether r is an ASCII letter.
func isASCIILetter(r byte) bool {
	return (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z')
}

// isASCIIDigit reports whether r is an ASCII digit.
func isASCIIDigit(r byte) bool {
	return r >= '0' && r <= '9'
}

// isNameContinue reports whether r may appear after the first byte of a class
// name: ASCII letters, digits, underscore and hyphen.
func isNameContinue(r byte) bool {
	return isASCIILetter(r) || isASCIIDigit(r) || r == '_' || r == '-'
}

// stripCSSComments removes every /* ... */ comment. Comments do not nest and
// bytes inside them (brackets, semicolons, colons) must never be interpreted
// as CSS syntax.
func stripCSSComments(s string) string {
	var b strings.Builder
	for i := 0; i < len(s); {
		if i+1 < len(s) && s[i] == '/' && s[i+1] == '*' {
			end := strings.Index(s[i+2:], "*/")
			if end < 0 {
				// Unterminated comment swallows the rest of the input.
				break
			}
			i += 2 + end + 2
			continue
		}
		b.WriteByte(s[i])
		i++
	}
	return b.String()
}

// parseCueClass parses a class token (without the leading dot) and reports
// whether it is a valid WebVTT class name: starting with an ASCII letter or
// underscore and followed only by letters, digits, underscores or hyphens.
// Class names are case-sensitive.
func parseCueClass(s string) (string, bool) {
	if s == "" {
		return "", false
	}
	r0 := s[0]
	if !isASCIILetter(r0) && r0 != '_' {
		return "", false
	}
	for i := 1; i < len(s); i++ {
		if !isNameContinue(s[i]) {
			return "", false
		}
	}
	return s, true
}

// parseCueSelector parses the content of a single ::cue(...) selector (or the
// empty content for the bare ::cue form). The ok result is false for any
// selector outside the supported subset (descendant combinators, pseudo
// classes, voices, escaped identifiers, unknown tags, ...).
func parseCueSelector(content string) (sel cueSelector, ok bool) {
	content = strings.TrimSpace(content)
	if content == "" {
		// ::cue matches the cue root; its specificity is 0.
		return cueSelector{}, true
	}

	// The supported forms are a type name optionally followed by classes:
	// c, b, i, u, .a or c.a.b. Whitespace around the tokens is allowed. Any
	// remaining byte makes the selector unsupported.
	var classes []string
	cursor := 0
	if content[0] == '.' {
		// Class-only selector: ::cue(.a) matches a node carrying the class,
		// whatever its type.
		sel.tagName = ""
	} else {
		// Read the type name up to the first dot (or end of input).
		end := strings.IndexByte(content, '.')
		name := content
		if end >= 0 {
			name = content[:end]
		}
		switch strings.ToLower(strings.TrimSpace(name)) {
		case "c", "b", "i", "u":
			sel.tagName = strings.ToLower(strings.TrimSpace(name))
		default:
			return cueSelector{}, false
		}
		cursor = len(name)
	}

	for cursor < len(content) {
		if content[cursor] != '.' {
			return cueSelector{}, false
		}
		next := strings.IndexByte(content[cursor+1:], '.')
		var raw string
		if next < 0 {
			raw = content[cursor+1:]
			cursor = len(content)
		} else {
			raw = content[cursor+1 : cursor+1+next]
			cursor += 1 + next
		}
		class, valid := parseCueClass(strings.TrimSpace(raw))
		if !valid {
			return cueSelector{}, false
		}
		classes = append(classes, class)
	}

	// A bare type selector has specificity 1, every class adds 10.
	sel.classes = classes
	sel.specificity = len(classes) * 10
	if sel.tagName != "" {
		sel.specificity++
	}
	return sel, true
}

// parseCueSelectors splits a selector group on commas and parses each member.
// Unsupported members are dropped; an entirely unsupported group simply
// yields no selectors (its declarations then match nothing).
func parseCueSelectors(segment string) []cueSelector {
	var selectors []cueSelector
	for _, part := range strings.Split(segment, ",") {
		part = strings.TrimSpace(part)
		if !strings.HasPrefix(part, "::cue") {
			continue
		}
		rest := strings.TrimSpace(part[len("::cue"):])
		var content string
		switch {
		case rest == "":
			content = ""
		case rest[0] == '(' && strings.HasSuffix(rest, ")"):
			content = rest[1 : len(rest)-1]
		default:
			continue
		}
		if sel, ok := parseCueSelector(content); ok {
			selectors = append(selectors, sel)
		}
	}
	return selectors
}

// parseCueColor parses a supported color value: the WebVTT named colors
// (case-insensitive) or #RRGGBB. The returned string keeps the canonical
// spelling used on output (hex for named colors, original hex otherwise).
func parseCueColor(value string) (string, bool) {
	if named, ok := webVTTNamedColors[strings.ToLower(value)]; ok {
		return named, true
	}
	if len(value) == 7 && value[0] == '#' {
		for i := 1; i < 7; i++ {
			r := value[i]
			if !isASCIIDigit(r) && !(r >= 'a' && r <= 'f') && !(r >= 'A' && r <= 'F') {
				return "", false
			}
		}
		return strings.ToLower(value), true
	}
	return "", false
}

// parseCueDeclaration parses one declaration body (the text between two
// semicolons, without the trailing semicolon). Illegal values or unknown
// properties are reported as invalid so the caller can ignore only that
// declaration.
func parseCueDeclaration(body string, order int, sel cueSelector) (d cueDeclaration, ok bool) {
	colon := strings.IndexByte(body, ':')
	if colon < 0 {
		return cueDeclaration{}, false
	}
	property := strings.ToLower(strings.TrimSpace(body[:colon]))
	value := strings.TrimSpace(body[colon+1:])

	important := false
	if i := strings.LastIndex(strings.ToLower(value), "!important"); i >= 0 {
		// !important must be the last non-space token of the value.
		tail := strings.TrimSpace(value[i+len("!important"):])
		if tail == "" {
			important = true
			value = strings.TrimSpace(value[:i])
		}
	}

	switch property {
	case cascadePropColor, cascadePropBackgroundColor:
		switch strings.ToLower(value) {
		case "inherit", "initial":
			return cueDeclaration{property: property, value: strings.ToLower(value), important: important, order: order, selector: sel}, true
		}
		parsed, valid := parseCueColor(value)
		if !valid {
			return cueDeclaration{}, false
		}
		return cueDeclaration{property: property, value: parsed, important: important, order: order, selector: sel}, true
	case cascadePropFontWeight:
		switch strings.ToLower(value) {
		case "normal", "bold", "inherit", "initial":
		default:
			return cueDeclaration{}, false
		}
		return cueDeclaration{property: property, value: strings.ToLower(value), important: important, order: order, selector: sel}, true
	case cascadePropFontStyle:
		switch strings.ToLower(value) {
		case "normal", "italic", "inherit", "initial":
		default:
			return cueDeclaration{}, false
		}
		return cueDeclaration{property: property, value: strings.ToLower(value), important: important, order: order, selector: sel}, true
	case cascadePropTextDecoration:
		switch strings.ToLower(value) {
		case "none", "underline", "inherit", "initial":
		default:
			return cueDeclaration{}, false
		}
		return cueDeclaration{property: property, value: strings.ToLower(value), important: important, order: order, selector: sel}, true
	}
	return cueDeclaration{}, false
}

// parseCueStylesheet parses the concatenation of every STYLE block (in read
// order). STYLE lines are stored one physical line per slice entry, so they
// are joined first: a rule may span several lines and a comment may span
// lines too. Rules and declarations keep their source order so cascade ties
// can be resolved deterministically without relying on any map iteration.
func parseCueStylesheet(styleLines []string) cueStylesheet {
	var sheet cueStylesheet
	order := 0
	clean := stripCSSComments(strings.Join(styleLines, "\n"))
	for len(clean) > 0 {
		open := strings.IndexByte(clean, '{')
		if open < 0 {
			break
		}
		close := strings.IndexByte(clean[open:], '}')
		if close < 0 {
			// A block without a closing brace swallows the remainder,
			// matching CSS error recovery.
			break
		}
		selectorText := clean[:open]
		bodyText := clean[open+1 : open+close]
		clean = clean[open+close+1:]

		selectors := parseCueSelectors(selectorText)
		if len(selectors) == 0 {
			continue
		}

		rule := cueRule{selectors: selectors}
		for _, chunk := range strings.Split(bodyText, ";") {
			if strings.TrimSpace(chunk) == "" {
				continue
			}
			declared := false
			for _, sel := range selectors {
				if d, ok := parseCueDeclaration(chunk, order, sel); ok {
					rule.declarations = append(rule.declarations, d)
					sheet.active = true
					declared = true
				}
			}
			// One source position per textual declaration, independent of
			// how many selectors share the group.
			if declared {
				order++
			}
		}
		if len(rule.declarations) > 0 {
			sheet.rules = append(sheet.rules, rule)
		}
	}
	return sheet
}

// matches reports whether the selector applies to node. The bare ::cue form
// (no type and no class) only matches the cue root; a class-only selector
// matches any node type carrying the class.
func (sel cueSelector) matches(node cueNode) bool {
	if sel.tagName == "" && len(sel.classes) == 0 {
		return node.tagName == "" && len(node.classes) == 0
	}
	if sel.tagName != "" && sel.tagName != node.tagName {
		return false
	}
	for _, wanted := range sel.classes {
		found := false
		for _, got := range node.classes {
			if got == wanted {
				found = true
				break
			}
		}
		if !found {
			return false
		}
	}
	return true
}

// declarationWins reports whether candidate replaces current under the
// per-node cascade: important beats normal, then specificity, then source
// order.
func declarationWins(candidate, current cueDeclaration) bool {
	if candidate.important != current.important {
		return candidate.important
	}
	if candidate.selector.specificity != current.selector.specificity {
		return candidate.selector.specificity > current.selector.specificity
	}
	return candidate.order > current.order
}

// nodeSpecifiedStyle resolves the declarations matching a single node. The
// root is represented by the zero cueNode. Property names are the keys, so
// callers never depend on map iteration order for the cascade itself.
func (sheet cueStylesheet) nodeSpecifiedStyle(node cueNode) map[string]cueDeclaration {
	winners := map[string]cueDeclaration{}
	for _, rule := range sheet.rules {
		for _, d := range rule.declarations {
			if !d.selector.matches(node) {
				continue
			}
			current, exists := winners[d.property]
			if !exists || declarationWins(d, current) {
				winners[d.property] = d
			}
		}
	}
	return winners
}

// resolveCueValue applies inherit/initial on top of a winning declaration,
// given the parent's computed value.
func resolveCueValue(d cueDeclaration, parent cueComputedStyle) string {
	switch strings.ToLower(d.value) {
	case "inherit":
		return parent.valueFor(d.property)
	case "initial":
		return cascadeInitials.valueFor(d.property)
	}
	return d.value
}

// valueFor returns the computed value stored for a property.
func (cs cueComputedStyle) valueFor(property string) string {
	switch property {
	case cascadePropColor:
		return cs.color
	case cascadePropBackgroundColor:
		return cs.backgroundColor
	case cascadePropFontWeight:
		return cs.fontWeight
	case cascadePropFontStyle:
		return cs.fontStyle
	case cascadePropTextDecoration:
		return cs.textDecoration
	}
	return ""
}

// setValueFor stores a computed value for a property.
func (cs *cueComputedStyle) setValueFor(property, value string) {
	switch property {
	case cascadePropColor:
		cs.color = value
	case cascadePropBackgroundColor:
		cs.backgroundColor = value
	case cascadePropFontWeight:
		cs.fontWeight = value
	case cascadePropFontStyle:
		cs.fontStyle = value
	case cascadePropTextDecoration:
		cs.textDecoration = value
	}
}

// computeCueStack walks a run's tag stack from the cue root down and returns
// the computed style of the deepest node. Each level inherits every property
// without a winning declaration from its parent; inherit on the root falls
// back to the initial values.
func (sheet cueStylesheet) computeCueStack(frames []map[string]cueDeclaration) cueComputedStyle {
	computed := cascadeInitials
	for _, winners := range frames {
		computed = applyNodeDeclarations(winners, computed)
	}
	return computed
}

// applyNodeDeclarations builds a node's computed style from its winning
// declarations and its parent's computed style.
func applyNodeDeclarations(decls map[string]cueDeclaration, parent cueComputedStyle) cueComputedStyle {
	out := parent // unspecified properties inherit the parent value
	for property, d := range decls {
		out.setValueFor(property, resolveCueValue(d, parent))
	}
	return out
}

// tagStackToCueNodes converts a run's parsed WebVTT tags into selector nodes.
func tagStackToCueNodes(tags []WebVTTTag) []cueNode {
	nodes := make([]cueNode, 0, len(tags))
	for _, tag := range tags {
		name := strings.ToLower(tag.Name)
		classes := make([]string, len(tag.Classes))
		copy(classes, tag.Classes)
		nodes = append(nodes, cueNode{tagName: name, classes: classes})
	}
	return nodes
}

// defaultNodeDeclarations returns the lowest-priority declarations implied by
// the built-in b/i/u tags and WebVTT color classes of one stack node. CSS
// rules always outrank them, so they are only consulted for properties the
// stylesheet leaves unspecified on that node; a CSS normal/none declaration
// therefore switches the built-in effect off.
func defaultNodeDeclarations(node cueNode) []cueDeclaration {
	var defaults []cueDeclaration
	add := func(property, value string, sel cueSelector) {
		defaults = append(defaults, cueDeclaration{property: property, value: value, selector: sel})
	}
	switch node.tagName {
	case "b":
		add(cascadePropFontWeight, "bold", cueSelector{tagName: "b", specificity: 1})
	case "i":
		add(cascadePropFontStyle, "italic", cueSelector{tagName: "i", specificity: 1})
	case "u":
		add(cascadePropTextDecoration, "underline", cueSelector{tagName: "u", specificity: 1})
	case "c":
		for _, class := range node.classes {
			sel := cueSelector{tagName: "c", classes: []string{class}, specificity: 11}
			if strings.HasPrefix(class, "bg_") && len(class) > 3 {
				if hex, ok := webVTTClassColors[strings.ToLower(class[3:])]; ok {
					add(cascadePropBackgroundColor, hex, sel)
				}
			} else if hex, ok := webVTTClassColors[strings.ToLower(class)]; ok {
				add(cascadePropColor, hex, sel)
			}
		}
	}
	return defaults
}

// buildRunFrames resolves, for every level of a run's tag stack (root first),
// the declarations that win on that node. Authored stylesheet declarations
// are cascaded first; the built-in tag/class defaults only fill properties
// left unspecified, so they can never override the stylesheet.
func (sheet cueStylesheet) buildRunFrames(stack []cueNode) []map[string]cueDeclaration {
	frames := make([]map[string]cueDeclaration, 0, len(stack)+1)
	frames = append(frames, sheet.nodeSpecifiedStyle(cueNode{}))
	for _, node := range stack {
		winners := sheet.nodeSpecifiedStyle(node)
		for _, d := range defaultNodeDeclarations(node) {
			if _, authored := winners[d.property]; !authored {
				winners[d.property] = d
			}
		}
		frames = append(frames, winners)
	}
	return frames
}

// computeRunStyle resolves the five cascade properties for one run given the
// run's current tag stack.
func (sheet cueStylesheet) computeRunStyle(tags []WebVTTTag) cueComputedStyle {
	if !sheet.active {
		// Defensive: callers only invoke the cascade for active sheets.
		return cascadeInitials
	}
	return sheet.computeCueStack(sheet.buildRunFrames(tagStackToCueNodes(tags)))
}

// collectWebVTTStyles gathers the STYLE lines carried by the subtitles'
// styles in a deterministic order. ReadFromWebVTT stores every STYLE block on
// the default style object in read order, so it is collected first; any other
// style id (native TTML objects or hand-built documents) is then collected in
// sorted id order.
func (s Subtitles) collectWebVTTStyles() []string {
	ids := make([]string, 0, len(s.Styles))
	for id := range s.Styles {
		if id != webvttDefaultStyleID {
			ids = append(ids, id)
		}
	}
	sort.Strings(ids)

	var lines []string
	if def, ok := s.Styles[webvttDefaultStyleID]; ok && def.InlineStyle != nil {
		lines = append(lines, def.InlineStyle.WebVTTStyles...)
	}
	for _, id := range ids {
		if st := s.Styles[id]; st.InlineStyle != nil {
			lines = append(lines, st.InlineStyle.WebVTTStyles...)
		}
	}
	return lines
}

// cueStylesheetForBuild parses the current STYLE lines for a TTML/WebVTT
// build. The second result reports whether the supported-cascade path should
// run at all.
func (s Subtitles) cueStylesheetForBuild() (cueStylesheet, bool) {
	lines := s.collectWebVTTStyles()
	if len(lines) == 0 {
		return cueStylesheet{}, false
	}
	sheet := parseCueStylesheet(lines)
	return sheet, sheet.active
}
