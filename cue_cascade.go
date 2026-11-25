package astisub

import (
	"sort"
	"strconv"
	"strings"
)

// This file implements the limited WebVTT STYLE CSS subset used to compute cue
// styles at TTML export time. See the task specification for the supported
// selectors, declarations and cascade rules. The computation is intentionally
// stateless and read-only: WriteToTTML calls it on every export using the
// current StyleAttributes.WebVTTStyles and each run's current WebVTTTags, so
// edits made after ReadFromWebVTT are reflected and the source Subtitles are
// never mutated.

// cueProp identifies one of the five cascaded properties.
type cueProp uint8

const (
	cuePropColor cueProp = iota
	cuePropBackgroundColor
	cuePropFontWeight
	cuePropFontStyle
	cuePropTextDecoration
)

// cueValueKind distinguishes an absolute value from the inherit/initial
// keywords.
type cueValueKind uint8

const (
	cueValueAbsolute cueValueKind = iota
	cueValueInherit
	cueValueInitial
)

// cueValue is a parsed declaration value. Color is used for color and
// background-color, text for the three keyword properties.
type cueValue struct {
	kind  cueValueKind
	color *Color
	text  string
}

func cueColorValue(c *Color) cueValue { return cueValue{kind: cueValueAbsolute, color: c} }
func cueTextValue(s string) cueValue  { return cueValue{kind: cueValueAbsolute, text: s} }
func cueInheritValue() cueValue       { return cueValue{kind: cueValueInherit} }
func cueInitialValue() cueValue       { return cueValue{kind: cueValueInitial} }

// cueSelector is a parsed, supported ::cue selector.
type cueSelector struct {
	root    bool     // bare ::cue
	typ     string   // lower-cased c/b/i/u when a type was specified
	classes []string // required class names, case sensitive
	spec    int      // specificity: root 0, type 1, each class 10
}

// cueDeclaration is one valid, supported declaration. order is its global
// occurrence index across the concatenated STYLE lines.
type cueDeclaration struct {
	prop      cueProp
	value     cueValue
	important bool
	order     int
}

type cueRule struct {
	selectors    []cueSelector
	declarations []cueDeclaration
}

type cueStylesheet struct {
	rules []cueRule
	// enabled is true when at least one supported, valid declaration survived
	// parsing. Only then does the five-property cascade replace the legacy
	// export values.
	enabled bool
}

// cueNode is one level of a run's tag stack, or the cue root.
type cueNode struct {
	root    bool
	name    string
	classes []string
}

// cueComputed holds the fully resolved values of the five cascaded properties.
type cueComputed struct {
	color           *Color
	backgroundColor *Color
	fontWeight      string
	fontStyle       string
	textDecoration  string
}

// cuePropNames maps supported property names (ASCII case insensitive).
var cuePropNames = map[string]cueProp{
	"color":            cuePropColor,
	"background-color": cuePropBackgroundColor,
	"font-weight":      cuePropFontWeight,
	"font-style":       cuePropFontStyle,
	"text-decoration":  cuePropTextDecoration,
}

// cueNamedColors are the supported WebVTT named colors for declaration values.
// The canonical eight are black/red/green/yellow/blue/magenta/cyan/white; lime
// and fuchsia are accepted as the WebVTT/CSS aliases of pure green and
// magenta.
var cueNamedColors = map[string]*Color{
	"black":   ColorBlack,
	"red":     ColorRed,
	"green":   ColorGreen,
	"yellow":  ColorYellow,
	"blue":    ColorBlue,
	"magenta": ColorMagenta,
	"cyan":    ColorCyan,
	"white":   ColorWhite,
	"lime":    ColorLime,
	"fuchsia": ColorMagenta,
}

func cueIsSpace(b byte) bool {
	return b == ' ' || b == '\t' || b == '\n' || b == '\r' || b == '\f'
}

func cueContainsSpace(s string) bool {
	for i := 0; i < len(s); i++ {
		if cueIsSpace(s[i]) {
			return true
		}
	}
	return false
}

func cueIsLetter(b byte) bool {
	return b >= 'a' && b <= 'z' || b >= 'A' && b <= 'Z'
}

func cueIsIdentStart(b byte) bool {
	return cueIsLetter(b) || b == '_'
}

func cueIsIdentPart(b byte) bool {
	return cueIsIdentStart(b) || b >= '0' && b <= '9' || b == '-'
}

func cueIsHex(b byte) bool {
	return b >= '0' && b <= '9' || b >= 'a' && b <= 'f' || b >= 'A' && b <= 'F'
}

// stripCueComments replaces every /* ... */ comment with a space so that
// parentheses, braces and semicolons inside comments cannot affect parsing.
// An unterminated comment runs to the end of the input.
func stripCueComments(s string) string {
	var b strings.Builder
	i := 0
	for i < len(s) {
		if i+1 < len(s) && s[i] == '/' && s[i+1] == '*' {
			j := i + 2
			closed := false
			for j+1 < len(s) {
				if s[j] == '*' && s[j+1] == '/' {
					closed = true
					break
				}
				j++
			}
			if !closed {
				break
			}
			b.WriteByte(' ')
			i = j + 2
			continue
		}
		b.WriteByte(s[i])
		i++
	}
	return b.String()
}

// subtitleCueCSSText concatenates the WebVTTStyles currently carried by the
// subtitle styles. Style IDs are sorted so map iteration order never influences
// the cascade; within one Style the slice already preserves read order across
// all STYLE blocks.
func (s Subtitles) subtitleCueCSSText() string {
	ids := make([]string, 0, len(s.Styles))
	for id, st := range s.Styles {
		if st != nil && st.InlineStyle != nil && len(st.InlineStyle.WebVTTStyles) > 0 {
			ids = append(ids, id)
		}
	}
	sort.Strings(ids)
	var lines []string
	for _, id := range ids {
		lines = append(lines, s.Styles[id].InlineStyle.WebVTTStyles...)
	}
	return strings.Join(lines, "\n")
}

// parseCueStylesheet parses the supported CSS subset. It never returns an
// error: unsupported selectors/properties/values are skipped until the end of
// the current rule or declaration.
func parseCueStylesheet(css string) (sheet cueStylesheet) {
	css = stripCueComments(css)
	n := len(css)
	i := 0
	order := 0
	for i < n {
		// Skip whitespace.
		for i < n && cueIsSpace(css[i]) {
			i++
		}
		if i >= n {
			break
		}

		// Read the prelude until the opening brace of a block, a semicolon
		// (statement at-rule or malformed chunk) or a stray closing brace.
		preludeStart := i
		for i < n && css[i] != '{' && css[i] != ';' && css[i] != '}' {
			i++
		}
		if i >= n {
			// Trailing prelude without a block: nothing to emit.
			break
		}
		if css[i] == ';' || css[i] == '}' {
			// Statement at-rule or stray brace: consume and resynchronize.
			i++
			continue
		}

		prelude := css[preludeStart:i]

		// Consume the block with balanced brace matching so nested at-rule
		// blocks (e.g. @keyframes) do not swallow later rules.
		blockStart := i
		depth := 0
		j := i
		close := -1
		for j < n {
			switch css[j] {
			case '{':
				depth++
			case '}':
				depth--
				if depth == 0 {
					close = j
				}
			}
			j++
			if close >= 0 {
				break
			}
		}
		if close < 0 {
			// Unclosed block: ignore the remainder.
			break
		}
		body := css[blockStart+1 : close]
		i = close + 1

		// Parse the comma grouped selector list. An unsupported selector
		// invalidates the whole rule ("ignore to the end of the rule").
		selectors, ok := parseCueSelectorList(prelude)
		if !ok {
			continue
		}

		rule := cueRule{selectors: selectors}
		for _, segment := range strings.Split(body, ";") {
			if decl, valid := parseCueDeclaration(segment, order); valid {
				rule.declarations = append(rule.declarations, decl)
				order++
				sheet.enabled = true
			}
		}
		sheet.rules = append(sheet.rules, rule)
	}
	return sheet
}

// parseCueSelectorList parses a comma separated list of ::cue selectors.
func parseCueSelectorList(prelude string) ([]cueSelector, bool) {
	parts := strings.Split(prelude, ",")
	var selectors []cueSelector
	for _, part := range parts {
		sel, ok := parseCueSelector(part)
		if !ok {
			return nil, false
		}
		selectors = append(selectors, sel)
	}
	if len(selectors) == 0 {
		return nil, false
	}
	return selectors, true
}

// parseCueSelector parses one selector: bare ::cue or
// ::cue(c|b|i|u|.cls|type.cls...).
func parseCueSelector(raw string) (cueSelector, bool) {
	s := strings.TrimSpace(raw)
	if len(s) < 5 || !strings.EqualFold(s[:5], "::cue") {
		return cueSelector{}, false
	}
	rest := strings.TrimSpace(s[5:])
	if rest == "" {
		return cueSelector{root: true, spec: 0}, true
	}
	if rest[0] != '(' || rest[len(rest)-1] != ')' {
		return cueSelector{}, false
	}
	inner := strings.TrimSpace(rest[1 : len(rest)-1])
	return parseCueCompoundSelector(inner)
}

// parseCueCompoundSelector parses the content inside ::cue(...).
func parseCueCompoundSelector(s string) (sel cueSelector, ok bool) {
	if s == "" || cueContainsSpace(s) {
		// Empty selector or a descendant combinator: unsupported.
		return cueSelector{}, false
	}
	i := 0
	if cueIsLetter(s[0]) {
		start := 0
		for i < len(s) && cueIsIdentPart(s[i]) {
			i++
		}
		typ := strings.ToLower(s[start:i])
		if typ != "c" && typ != "b" && typ != "i" && typ != "u" {
			return cueSelector{}, false
		}
		sel.typ = typ
	}
	for i < len(s) {
		if s[i] != '.' {
			return cueSelector{}, false
		}
		i++
		if i >= len(s) || !cueIsIdentStart(s[i]) {
			return cueSelector{}, false
		}
		nameStart := i
		i++
		for i < len(s) && cueIsIdentPart(s[i]) {
			i++
		}
		// Class names are kept verbatim: they are case sensitive.
		sel.classes = append(sel.classes, s[nameStart:i])
	}
	if sel.typ == "" && len(sel.classes) == 0 {
		return cueSelector{}, false
	}
	sel.spec = len(sel.classes) * 10
	if sel.typ != "" {
		sel.spec++
	}
	return sel, true
}

// parseCueDeclaration parses one declaration segment.
func parseCueDeclaration(segment string, order int) (cueDeclaration, bool) {
	colon := strings.IndexByte(segment, ':')
	if colon < 0 {
		return cueDeclaration{}, false
	}
	propName := strings.ToLower(strings.TrimSpace(segment[:colon]))
	prop, supported := cuePropNames[propName]
	if !supported {
		return cueDeclaration{}, false
	}
	rawValue := strings.TrimSpace(segment[colon+1:])

	// Parse an optional !important suffix.
	important := false
	if bang := strings.IndexByte(rawValue, '!'); bang >= 0 {
		tail := strings.TrimSpace(rawValue[bang+1:])
		if !strings.EqualFold(tail, "important") {
			return cueDeclaration{}, false
		}
		important = true
		rawValue = strings.TrimSpace(rawValue[:bang])
	}
	if rawValue == "" {
		return cueDeclaration{}, false
	}

	lower := strings.ToLower(rawValue)
	var value cueValue
	switch lower {
	case "inherit":
		value = cueInheritValue()
	case "initial":
		value = cueInitialValue()
	default:
		switch prop {
		case cuePropColor, cuePropBackgroundColor:
			c, valid := parseCueColor(rawValue)
			if !valid {
				return cueDeclaration{}, false
			}
			value = cueColorValue(c)
		case cuePropFontWeight:
			if lower != "normal" && lower != "bold" {
				return cueDeclaration{}, false
			}
			value = cueTextValue(lower)
		case cuePropFontStyle:
			if lower != "normal" && lower != "italic" {
				return cueDeclaration{}, false
			}
			value = cueTextValue(lower)
		case cuePropTextDecoration:
			if lower != "none" && lower != "underline" {
				return cueDeclaration{}, false
			}
			value = cueTextValue(lower)
		}
	}
	return cueDeclaration{prop: prop, value: value, important: important, order: order}, true
}

// parseCueColor parses #RRGGBB or one of the supported named colors.
func parseCueColor(v string) (*Color, bool) {
	v = strings.TrimSpace(v)
	if strings.HasPrefix(v, "#") {
		hex := v[1:]
		if len(hex) != 6 {
			return nil, false
		}
		for i := 0; i < len(hex); i++ {
			if !cueIsHex(hex[i]) {
				return nil, false
			}
		}
		u, err := strconv.ParseUint(hex, 16, 32)
		if err != nil {
			return nil, false
		}
		return &Color{Red: uint8(u >> 16), Green: uint8(u >> 8), Blue: uint8(u)}, true
	}
	if c, ok := cueNamedColors[strings.ToLower(v)]; ok {
		return c, true
	}
	return nil, false
}

func cueSelectorMatches(sel cueSelector, node cueNode) bool {
	if node.root {
		return sel.root
	}
	if sel.root {
		return false
	}
	if sel.typ != "" && !strings.EqualFold(sel.typ, node.name) {
		return false
	}
	for _, wanted := range sel.classes {
		found := false
		for _, have := range node.classes {
			// Exact comparison: class names are case sensitive.
			if wanted == have {
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

// cueDeclBeats reports whether candidate (decl, spec) beats the current best.
// Priority is important first, then specificity, then occurrence order.
func cueDeclBeats(decl cueDeclaration, spec int, best cueDeclaration, bestSpec int) bool {
	if decl.important != best.important {
		return decl.important
	}
	if spec != bestSpec {
		return spec > bestSpec
	}
	return decl.order > best.order
}

// winningValue returns the highest priority stylesheet declaration for prop on
// node. It does not apply inherit/initial or built-in defaults.
func (sh cueStylesheet) winningValue(prop cueProp, node cueNode) (cueDeclaration, int, bool) {
	var best cueDeclaration
	bestSpec := 0
	found := false
	for ri := range sh.rules {
		rule := &sh.rules[ri]
		for si := range rule.selectors {
			sel := &rule.selectors[si]
			if !cueSelectorMatches(*sel, node) {
				continue
			}
			for di := range rule.declarations {
				decl := rule.declarations[di]
				if decl.prop != prop {
					continue
				}
				if !found || cueDeclBeats(decl, sel.spec, best, bestSpec) {
					best = decl
					bestSpec = sel.spec
					found = true
				}
			}
		}
	}
	return best, bestSpec, found
}

func cueInitialComputed() cueComputed {
	return cueComputed{
		color:           ColorWhite,
		backgroundColor: ColorBlack,
		fontWeight:      "normal",
		fontStyle:       "normal",
		textDecoration:  "none",
	}
}

func (c cueComputed) propValue(prop cueProp) cueValue {
	switch prop {
	case cuePropColor:
		return cueColorValue(c.color)
	case cuePropBackgroundColor:
		return cueColorValue(c.backgroundColor)
	case cuePropFontWeight:
		return cueTextValue(c.fontWeight)
	case cuePropFontStyle:
		return cueTextValue(c.fontStyle)
	default:
		return cueTextValue(c.textDecoration)
	}
}

// cueBuiltinValue returns the lowest priority default implied by b/i/u tags
// and built-in WebVTT color classes.
func cueBuiltinValue(prop cueProp, node cueNode) (cueValue, bool) {
	if node.root {
		return cueValue{}, false
	}
	name := strings.ToLower(node.name)
	switch prop {
	case cuePropFontWeight:
		if name == "b" {
			return cueTextValue("bold"), true
		}
	case cuePropFontStyle:
		if name == "i" {
			return cueTextValue("italic"), true
		}
	case cuePropTextDecoration:
		if name == "u" {
			return cueTextValue("underline"), true
		}
	case cuePropColor, cuePropBackgroundColor:
		// Mirror propagateWebVTTAttributes: built-in color classes are only
		// interpreted on c nodes, with later classes winning.
		if name != "c" {
			return cueValue{}, false
		}
		var fg, bg *Color
		var haveFg, haveBg bool
		for _, class := range node.classes {
			if strings.HasPrefix(class, "bg_") && len(class) > 3 {
				if c, err := newColorFromWebVTTString(class[3:]); err == nil {
					bg = c
					haveBg = true
				}
			} else if c, err := newColorFromWebVTTString(class); err == nil {
				fg = c
				haveFg = true
			}
		}
		if prop == cuePropColor && haveFg {
			return cueColorValue(fg), true
		}
		if prop == cuePropBackgroundColor && haveBg {
			return cueColorValue(bg), true
		}
	}
	return cueValue{}, false
}

// evaluate resolves all five properties for one stack level. parent holds the
// already computed values of the parent level (the cue root uses the initial
// values), which also backs inherit.
func (sh cueStylesheet) evaluate(node cueNode, parent cueComputed) cueComputed {
	out := parent
	initial := cueInitialComputed()
	for _, prop := range []cueProp{cuePropColor, cuePropBackgroundColor, cuePropFontWeight, cuePropFontStyle, cuePropTextDecoration} {
		var resolved cueValue
		if decl, _, ok := sh.winningValue(prop, node); ok {
			switch decl.value.kind {
			case cueValueInherit:
				resolved = parent.propValue(prop)
			case cueValueInitial:
				resolved = initial.propValue(prop)
			default:
				resolved = decl.value
			}
		} else if builtin, ok := cueBuiltinValue(prop, node); ok {
			resolved = builtin
		} else {
			// Inherit the parent's computed value. At the root parent is the
			// initial style, so root inherit also yields the initial value.
			resolved = parent.propValue(prop)
		}
		switch prop {
		case cuePropColor:
			out.color = resolved.color
		case cuePropBackgroundColor:
			out.backgroundColor = resolved.color
		case cuePropFontWeight:
			out.fontWeight = resolved.text
		case cuePropFontStyle:
			out.fontStyle = resolved.text
		case cuePropTextDecoration:
			out.textDecoration = resolved.text
		}
	}
	return out
}

// computeLineItem runs the cascade across the run's current complete tag
// stack. It is a pure read of sa.WebVTTTags.
func (sh cueStylesheet) computeLineItem(sa *StyleAttributes) cueComputed {
	computed := sh.evaluate(cueNode{root: true}, cueInitialComputed())
	var tags []WebVTTTag
	if sa != nil {
		tags = sa.WebVTTTags
	}
	for _, tag := range tags {
		computed = sh.evaluate(cueNode{name: tag.Name, classes: tag.Classes}, computed)
	}
	return computed
}
