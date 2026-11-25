package astisub

import (
	"sort"
	"strconv"
	"strings"
)

// This file implements the small, explicit WebVTT cue-style subset that can be
// resolved deterministically and exported to TTML. It intentionally does not
// mutate the Subtitles value: WriteToTTML asks for an effective copy of each
// run's style attributes.

type cueProperty int

const (
	cueColor cueProperty = iota
	cueBackgroundColor
	cueFontWeight
	cueFontStyle
	cueTextDecoration
)

type cueValueKind int

const (
	cueValueAbsolute cueValueKind = iota
	cueValueInherit
	cueValueInitial
)

type cueValue struct {
	kind    cueValueKind
	color   *Color
	keyword string
}

type cueDeclaration struct {
	prop      cueProperty
	value     cueValue
	important bool
	order     int
}

type cueSelector struct {
	root        bool
	typ         string
	classes     []string
	specificity int
}

type cueRule struct {
	selectors    []cueSelector
	declarations []cueDeclaration
}

type cueStylesheet struct {
	rules  []cueRule
	active bool
}

type cueComputedStyle struct {
	color           *Color
	backgroundColor *Color
	fontWeight      string
	fontStyle       string
	textDecoration  string
}

type cueCandidate struct {
	decl        cueDeclaration
	specificity int
	isDefault   bool
}

func initialCueComputedStyle() cueComputedStyle {
	return cueComputedStyle{
		color:           ColorWhite,
		backgroundColor: ColorBlack,
		fontWeight:      "normal",
		fontStyle:       "normal",
		textDecoration:  "none",
	}
}

// webVTTCueStylesheet reads the currently stored STYLE lines. The WebVTT
// reader stores all STYLE blocks, in read order, in the default style. Other
// StyleAttributes carrying WebVTTStyles are supported for manually assembled
// values, but are visited in sorted ID order so map iteration cannot affect
// the cascade.
func (s Subtitles) webVTTStyleLines() []string {
	var lines []string
	if st, ok := s.Styles[webvttDefaultStyleID]; ok && st != nil && st.InlineStyle != nil {
		lines = append(lines, st.InlineStyle.WebVTTStyles...)
	}

	ids := make([]string, 0, len(s.Styles))
	for id, st := range s.Styles {
		if id == webvttDefaultStyleID || st == nil || st.InlineStyle == nil {
			continue
		}
		if len(st.InlineStyle.WebVTTStyles) > 0 {
			ids = append(ids, id)
		}
	}
	sort.Strings(ids)
	for _, id := range ids {
		lines = append(lines, s.Styles[id].InlineStyle.WebVTTStyles...)
	}
	return lines
}

func (s Subtitles) webVTTCueStylesheet() *cueStylesheet {
	return parseCueStylesheet(strings.Join(s.webVTTStyleLines(), "\n"))
}

func parseCueStylesheet(input string) *cueStylesheet {
	css := stripCueComments(input)
	ss := &cueStylesheet{}
	order := 0
	i := 0

	for i < len(css) {
		for i < len(css) && isCueSpace(css[i]) {
			i++
		}
		// A stray semicolon is not part of a selector.
		for i < len(css) && css[i] == ';' {
			i++
			for i < len(css) && isCueSpace(css[i]) {
				i++
			}
		}
		if i >= len(css) {
			break
		}

		preludeStart := i
		parenDepth := 0
		for i < len(css) {
			switch css[i] {
			case '(':
				parenDepth++
			case ')':
				if parenDepth > 0 {
					parenDepth--
				}
			case '{', ';':
				if parenDepth == 0 {
					goto preludeDone
				}
			}
			i++
		}
	preludeDone:
		if i >= len(css) {
			break
		}

		// An at-rule statement such as @import...; is ignored.
		if css[i] == ';' {
			i++
			continue
		}

		prelude := strings.TrimSpace(css[preludeStart:i])
		open := i
		close := -1
		depth := 0
		for j := open; j < len(css); j++ {
			switch css[j] {
			case '{':
				depth++
			case '}':
				depth--
				if depth == 0 {
					close = j
				}
			}
			if close != -1 {
				break
			}
		}
		if close == -1 {
			// An unterminated block has no later rule to recover.
			break
		}

		// Unsupported at-rules, including keyframes/animations, are skipped as
		// complete balanced blocks.
		if !strings.HasPrefix(prelude, "@") {
			if selectors, ok := parseCueSelectorList(prelude); ok {
				declarations := parseCueDeclarations(css[open+1:close], &order)
				if len(declarations) > 0 {
					ss.active = true
					ss.rules = append(ss.rules, cueRule{
						selectors:    selectors,
						declarations: declarations,
					})
				}
			}
		}

		i = close + 1
	}

	return ss
}

func stripCueComments(input string) string {
	var b strings.Builder
	for i := 0; i < len(input); {
		if i+1 < len(input) && input[i] == '/' && input[i+1] == '*' {
			end := strings.Index(input[i+2:], "*/")
			if end < 0 {
				// Unterminated comment extends to EOF.
				break
			}
			i = i + 2 + end + 2
			b.WriteByte(' ')
			continue
		}
		b.WriteByte(input[i])
		i++
	}
	return b.String()
}

func webVTTStyleBlockComplete(lines []string) bool {
	css := stripCueComments(strings.Join(lines, "\n"))
	depth := 0
	for i := 0; i < len(css); i++ {
		switch css[i] {
		case '{':
			depth++
		case '}':
			if depth > 0 {
				depth--
			}
		}
	}
	css = strings.TrimRight(css, " \t\r\n\f;")
	return depth == 0 && strings.HasSuffix(css, "}")
}

func parseCueSelectorList(input string) ([]cueSelector, bool) {
	var selectors []cueSelector
	for _, part := range splitCueTopLevelCommas(input) {
		selector, ok := parseCueSelector(part)
		if !ok {
			return nil, false
		}
		selectors = append(selectors, selector)
	}
	if len(selectors) == 0 {
		return nil, false
	}
	return selectors, true
}

func splitCueTopLevelCommas(input string) []string {
	var parts []string
	depth := 0
	start := 0
	for i := 0; i < len(input); i++ {
		switch input[i] {
		case '(':
			depth++
		case ')':
			if depth > 0 {
				depth--
			}
		case ',':
			if depth == 0 {
				parts = append(parts, strings.TrimSpace(input[start:i]))
				start = i + 1
			}
		}
	}
	parts = append(parts, strings.TrimSpace(input[start:]))
	return parts
}

func parseCueSelector(input string) (cueSelector, bool) {
	input = strings.TrimSpace(input)
	if input == "" {
		return cueSelector{}, false
	}
	if len(input) < len("::cue") || !strings.EqualFold(input[:len("::cue")], "::cue") {
		return cueSelector{}, false
	}
	rest := strings.TrimSpace(input[len("::cue"):])
	if rest == "" {
		return cueSelector{root: true}, true
	}
	if len(rest) < 2 || rest[0] != '(' || rest[len(rest)-1] != ')' {
		return cueSelector{}, false
	}
	inside := strings.TrimSpace(rest[1 : len(rest)-1])
	if inside == "" || strings.Contains(inside, ",") {
		// Comma grouping is handled at stylesheet selector-list level. A
		// functional selector with an inner list is not part of the subset.
		return cueSelector{}, false
	}

	sel := cueSelector{}
	i := 0
	if i < len(inside) && isCueIdentStart(inside[i]) {
		start := i
		for i < len(inside) && isCueIdentPart(inside[i]) {
			i++
		}
		sel.typ = strings.ToLower(inside[start:i])
		switch sel.typ {
		case "c", "b", "i", "u":
		default:
			return cueSelector{}, false
		}
		sel.specificity++
	}

	for i < len(inside) {
		if inside[i] != '.' {
			return cueSelector{}, false
		}
		i++
		start := i
		if i >= len(inside) || !isCueClassNameStart(inside[i]) {
			return cueSelector{}, false
		}
		for i < len(inside) && isCueClassNamePart(inside[i]) {
			i++
		}
		name := inside[start:i]
		sel.classes = append(sel.classes, name)
		sel.specificity += 10
	}

	if sel.typ == "" && len(sel.classes) == 0 {
		return cueSelector{}, false
	}
	return sel, true
}

func parseCueDeclarations(body string, order *int) []cueDeclaration {
	var declarations []cueDeclaration
	for _, chunk := range strings.Split(body, ";") {
		decl, ok := parseCueDeclaration(chunk, *order)
		if !ok {
			continue
		}
		*order++
		declarations = append(declarations, decl)
	}
	return declarations
}

func parseCueDeclaration(chunk string, order int) (cueDeclaration, bool) {
	chunk = strings.TrimSpace(chunk)
	if chunk == "" {
		return cueDeclaration{}, false
	}
	colon := strings.IndexByte(chunk, ':')
	if colon <= 0 {
		return cueDeclaration{}, false
	}

	prop := strings.ToLower(strings.TrimSpace(chunk[:colon]))
	rawValue := strings.TrimSpace(chunk[colon+1:])
	important := false
	if bang := strings.LastIndexByte(rawValue, '!'); bang >= 0 {
		suffix := strings.TrimSpace(rawValue[bang+1:])
		if strings.EqualFold(suffix, "important") {
			important = true
			rawValue = strings.TrimSpace(rawValue[:bang])
		} else {
			return cueDeclaration{}, false
		}
	}
	if rawValue == "" {
		return cueDeclaration{}, false
	}

	decl := cueDeclaration{important: important, order: order}
	if strings.EqualFold(rawValue, "inherit") {
		decl.value = cueValue{kind: cueValueInherit}
	} else if strings.EqualFold(rawValue, "initial") {
		decl.value = cueValue{kind: cueValueInitial}
	} else {
		switch prop {
		case "color":
			color, ok := parseCueColorValue(rawValue)
			if !ok {
				return cueDeclaration{}, false
			}
			decl.prop = cueColor
			decl.value = cueValue{kind: cueValueAbsolute, color: color}
		case "background-color":
			color, ok := parseCueColorValue(rawValue)
			if !ok {
				return cueDeclaration{}, false
			}
			decl.prop = cueBackgroundColor
			decl.value = cueValue{kind: cueValueAbsolute, color: color}
		case "font-weight":
			keyword := strings.ToLower(rawValue)
			if keyword != "normal" && keyword != "bold" {
				return cueDeclaration{}, false
			}
			decl.prop = cueFontWeight
			decl.value = cueValue{kind: cueValueAbsolute, keyword: keyword}
		case "font-style":
			keyword := strings.ToLower(rawValue)
			if keyword != "normal" && keyword != "italic" {
				return cueDeclaration{}, false
			}
			decl.prop = cueFontStyle
			decl.value = cueValue{kind: cueValueAbsolute, keyword: keyword}
		case "text-decoration":
			keyword := strings.ToLower(rawValue)
			if keyword != "none" && keyword != "underline" {
				return cueDeclaration{}, false
			}
			decl.prop = cueTextDecoration
			decl.value = cueValue{kind: cueValueAbsolute, keyword: keyword}
		default:
			return cueDeclaration{}, false
		}
	}

	// inherit/initial apply to all five supported properties.
	if decl.value.kind != cueValueAbsolute {
		switch prop {
		case "color":
			decl.prop = cueColor
		case "background-color":
			decl.prop = cueBackgroundColor
		case "font-weight":
			decl.prop = cueFontWeight
		case "font-style":
			decl.prop = cueFontStyle
		case "text-decoration":
			decl.prop = cueTextDecoration
		default:
			return cueDeclaration{}, false
		}
	}
	return decl, true
}

var cueNamedColors = map[string]*Color{
	"black":   ColorBlack,
	"red":     ColorRed,
	"green":   ColorGreen,
	"yellow":  ColorYellow,
	"blue":    ColorBlue,
	"magenta": ColorMagenta,
	"cyan":    ColorCyan,
	"white":   ColorWhite,
}

func parseCueColorValue(value string) (*Color, bool) {
	if len(value) == 7 && value[0] == '#' {
		hex := value[1:]
		n, err := strconv.ParseUint(hex, 16, 32)
		if err != nil {
			return nil, false
		}
		return &Color{Red: uint8(n >> 16), Green: uint8(n >> 8), Blue: uint8(n)}, true
	}
	color, ok := cueNamedColors[strings.ToLower(value)]
	return color, ok
}

func (ss *cueStylesheet) effectiveStyleAttributes(base *StyleAttributes) *StyleAttributes {
	effective := &StyleAttributes{}
	if base != nil {
		*effective = *base
	}
	computed := ss.compute(base.webVTTTags())
	effective.TTMLColor = computed.color
	effective.TTMLBackgroundColor = computed.backgroundColor
	effective.TTMLFontWeight = strPtrCue(computed.fontWeight)
	effective.TTMLFontStyle = strPtrCue(computed.fontStyle)
	effective.TTMLTextDecoration = strPtrCue(computed.textDecoration)
	return effective
}

func (ss *cueStylesheet) compute(tags []WebVTTTag) cueComputedStyle {
	parent := initialCueComputedStyle()
	root := ss.computeNode(nil, parent)
	parent = root
	for i := range tags {
		parent = ss.computeNode(&tags[i], parent)
	}
	return parent
}

func (ss *cueStylesheet) computeNode(tag *WebVTTTag, parent cueComputedStyle) cueComputedStyle {
	result := parent
	initial := initialCueComputedStyle()
	for _, prop := range []cueProperty{cueColor, cueBackgroundColor, cueFontWeight, cueFontStyle, cueTextDecoration} {
		candidate, ok := ss.winningCandidate(tag, prop)
		if !ok {
			continue
		}
		switch candidate.decl.value.kind {
		case cueValueInherit:
			result.setCueProperty(prop, parent.getCueProperty(prop))
		case cueValueInitial:
			result.setCueProperty(prop, initial.getCueProperty(prop))
		default:
			result.setCueProperty(prop, candidate.decl.value.cuePropertyValue(prop))
		}
	}
	return result
}

func (v cueValue) cuePropertyValue(prop cueProperty) interface{} {
	switch prop {
	case cueColor, cueBackgroundColor:
		return v.color
	default:
		return v.keyword
	}
}

func (c *cueComputedStyle) getCueProperty(prop cueProperty) interface{} {
	switch prop {
	case cueColor:
		return c.color
	case cueBackgroundColor:
		return c.backgroundColor
	case cueFontWeight:
		return c.fontWeight
	case cueFontStyle:
		return c.fontStyle
	default:
		return c.textDecoration
	}
}

func (c *cueComputedStyle) setCueProperty(prop cueProperty, value interface{}) {
	switch prop {
	case cueColor:
		c.color = value.(*Color)
	case cueBackgroundColor:
		c.backgroundColor = value.(*Color)
	case cueFontWeight:
		c.fontWeight = value.(string)
	case cueFontStyle:
		c.fontStyle = value.(string)
	case cueTextDecoration:
		c.textDecoration = value.(string)
	}
}

func (ss *cueStylesheet) winningCandidate(tag *WebVTTTag, prop cueProperty) (cueCandidate, bool) {
	var best cueCandidate
	found := false
	consider := func(candidate cueCandidate) {
		if !found || cueCandidateBetter(candidate, best) {
			best = candidate
			found = true
		}
	}

	for _, rule := range ss.rules {
		for _, selector := range rule.selectors {
			if !cueSelectorMatches(selector, tag) {
				continue
			}
			for _, decl := range rule.declarations {
				if decl.prop != prop {
					continue
				}
				consider(cueCandidate{decl: decl, specificity: selector.specificity})
			}
		}
	}

	for i, decl := range cueDefaultDeclarations(tag) {
		if decl.prop != prop {
			continue
		}
		decl.order = i
		consider(cueCandidate{decl: decl, isDefault: true})
	}
	return best, found
}

func cueCandidateBetter(candidate, best cueCandidate) bool {
	// Built-in tag/color-class defaults form a lower origin than every
	// supported author declaration.
	if candidate.isDefault != best.isDefault {
		return !candidate.isDefault
	}
	if candidate.decl.important != best.decl.important {
		return candidate.decl.important
	}
	if candidate.specificity != best.specificity {
		return candidate.specificity > best.specificity
	}
	return candidate.decl.order > best.decl.order
}

func cueSelectorMatches(selector cueSelector, tag *WebVTTTag) bool {
	if tag == nil {
		return selector.root
	}
	if selector.root {
		return false
	}
	if selector.typ != "" && !strings.EqualFold(selector.typ, tag.Name) {
		return false
	}
	for _, class := range selector.classes {
		hasClass := false
		for _, tagClass := range tag.Classes {
			if class == tagClass {
				hasClass = true
				break
			}
		}
		if !hasClass {
			return false
		}
	}
	return true
}

// cueDefaultDeclarations returns the low-priority formatting implied by the
// WebVTT tags themselves and the built-in color classes.
func cueDefaultDeclarations(tag *WebVTTTag) []cueDeclaration {
	if tag == nil {
		return nil
	}
	var declarations []cueDeclaration
	switch strings.ToLower(tag.Name) {
	case "b":
		declarations = append(declarations, cueDeclaration{
			prop:  cueFontWeight,
			value: cueValue{kind: cueValueAbsolute, keyword: "bold"},
		})
	case "i":
		declarations = append(declarations, cueDeclaration{
			prop:  cueFontStyle,
			value: cueValue{kind: cueValueAbsolute, keyword: "italic"},
		})
	case "u":
		declarations = append(declarations, cueDeclaration{
			prop:  cueTextDecoration,
			value: cueValue{kind: cueValueAbsolute, keyword: "underline"},
		})
	case "c":
		for _, class := range tag.Classes {
			if strings.HasPrefix(class, "bg_") && len(class) > 3 {
				if color, err := newColorFromWebVTTString(class[3:]); err == nil {
					declarations = append(declarations, cueDeclaration{
						prop:  cueBackgroundColor,
						value: cueValue{kind: cueValueAbsolute, color: color},
					})
				}
				continue
			}
			if color, err := newColorFromWebVTTString(class); err == nil {
				declarations = append(declarations, cueDeclaration{
					prop:  cueColor,
					value: cueValue{kind: cueValueAbsolute, color: color},
				})
			}
		}
	}
	return declarations
}

func (sa *StyleAttributes) webVTTTags() []WebVTTTag {
	if sa == nil {
		return nil
	}
	return sa.WebVTTTags
}

func isCueSpace(ch byte) bool {
	switch ch {
	case ' ', '\t', '\n', '\r', '\f':
		return true
	default:
		return false
	}
}

func isCueASCIILetter(ch byte) bool {
	return ch >= 'a' && ch <= 'z' || ch >= 'A' && ch <= 'Z'
}

func isCueIdentStart(ch byte) bool {
	return isCueASCIILetter(ch) || ch == '_'
}

func isCueIdentPart(ch byte) bool {
	return isCueIdentStart(ch) || ch >= '0' && ch <= '9' || ch == '-'
}

// Class names use the same allowed grammar as idents in this subset.
func isCueClassNameStart(ch byte) bool { return isCueIdentStart(ch) }
func isCueClassNamePart(ch byte) bool  { return isCueIdentPart(ch) }

func strPtrCue(value string) *string {
	return &value
}
