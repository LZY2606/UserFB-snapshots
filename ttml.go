package astisub

import (
	"encoding/xml"
	"fmt"
	"io"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"
	"unicode"

	"github.com/asticode/go-astikit"
)

// https://www.w3.org/TR/ttaf1-dfxp/
// http://www.skynav.com:8080/ttv/check
// https://www.speechpad.com/captions/ttml

// TTML languages
const (
	ttmlLanguageChinese   = "zh"
	ttmlLanguageEnglish   = "en"
	ttmlLanguageJapanese  = "ja"
	ttmlLanguageFrench    = "fr"
	ttmlLanguageNorwegian = "no"
)

// TTML language mapping
var ttmlLanguageMapping = astikit.NewBiMap().
	Set(ttmlLanguageChinese, LanguageChinese).
	Set(ttmlLanguageEnglish, LanguageEnglish).
	Set(ttmlLanguageFrench, LanguageFrench).
	Set(ttmlLanguageJapanese, LanguageJapanese).
	Set(ttmlLanguageNorwegian, LanguageNorwegian)

// TTML Clock Time Frames and Offset Time
var (
	ttmlRegexpClockTimeFrames = regexp.MustCompile(`\:[\d]+$`)
	ttmlRegexpOffsetTime      = regexp.MustCompile(`^(\d+(\.\d+)?)(h|m|s|ms|f|t)$`)
)

// TTML ruby property values
const (
	ttmlRubyBase          = "base"
	ttmlRubyText          = "text"
	ttmlRubyContainer     = "container"
	ttmlRubyBaseContainer = "baseContainer"
	ttmlRubyTextContainer = "textContainer"
)

type TTMLInBodyDiv struct {
	XMLName   xml.Name         `xml:"div"`
	Subtitles []TTMLInSubtitle `xml:"p"`

	Region string `xml:"region,attr,omitempty"`
	Style  string `xml:"style,attr,omitempty"`
	TTMLInStyleAttributes
}
type TTMLInBody struct {
	XMLName xml.Name        `xml:"body"`
	Divs    []TTMLInBodyDiv `xml:"div"`

	Region string `xml:"region,attr,omitempty"`
	Style  string `xml:"style,attr,omitempty"`
	TTMLInStyleAttributes
}

// TTMLIn represents an input TTML that must be unmarshaled
// We split it from the output TTML as we can't add strict namespace without breaking retrocompatibility
type TTMLIn struct {
	Framerate int            `xml:"frameRate,attr"`
	Lang      string         `xml:"lang,attr"`
	Metadata  TTMLInMetadata `xml:"head>metadata"`
	Regions   []TTMLInRegion `xml:"head>layout>region"`
	Styles    []TTMLInStyle  `xml:"head>styling>style"`
	Body      TTMLInBody     `xml:"body"`
	Tickrate  int            `xml:"tickRate,attr"`
	XMLName   xml.Name       `xml:"tt"`
}

// metadata returns the Metadata of the TTML
func (t TTMLIn) metadata() (m *Metadata) {
	m = &Metadata{
		Framerate:     t.Framerate,
		Title:         t.Metadata.Title,
		TTMLCopyright: t.Metadata.Copyright,
	}
	if v, ok := ttmlLanguageMapping.Get(astikit.StrPad(t.Lang, ' ', 2, astikit.PadCut)); ok {
		m.Language = v.(string)
	}
	return
}

// TTMLInMetadata represents an input TTML Metadata
type TTMLInMetadata struct {
	Copyright string `xml:"copyright"`
	Title     string `xml:"title"`
}

// TTMLInStyleAttributes represents input TTML style attributes
type TTMLInStyleAttributes struct {
	BackgroundColor *string `xml:"backgroundColor,attr,omitempty"`
	Color           *string `xml:"color,attr,omitempty"`
	Direction       *string `xml:"direction,attr,omitempty"`
	Display         *string `xml:"display,attr,omitempty"`
	DisplayAlign    *string `xml:"displayAlign,attr,omitempty"`
	Extent          *string `xml:"extent,attr,omitempty"`
	FontFamily      *string `xml:"fontFamily,attr,omitempty"`
	FontSize        *string `xml:"fontSize,attr,omitempty"`
	FontStyle       *string `xml:"fontStyle,attr,omitempty"`
	FontWeight      *string `xml:"fontWeight,attr,omitempty"`
	LineHeight      *string `xml:"lineHeight,attr,omitempty"`
	Opacity         *string `xml:"opacity,attr,omitempty"`
	Origin          *string `xml:"origin,attr,omitempty"`
	Overflow        *string `xml:"overflow,attr,omitempty"`
	Padding         *string `xml:"padding,attr,omitempty"`
	ShowBackground  *string `xml:"showBackground,attr,omitempty"`
	TextAlign       *string `xml:"textAlign,attr,omitempty"`
	TextDecoration  *string `xml:"textDecoration,attr,omitempty"`
	TextOutline     *string `xml:"textOutline,attr,omitempty"`
	UnicodeBidi     *string `xml:"unicodeBidi,attr,omitempty"`
	Visibility      *string `xml:"visibility,attr,omitempty"`
	WrapOption      *string `xml:"wrapOption,attr,omitempty"`
	WritingMode     *string `xml:"writingMode,attr,omitempty"`
	ZIndex          *int    `xml:"zIndex,attr,omitempty"`
}

// StyleAttributes converts TTMLInStyleAttributes into a StyleAttributes
func (s TTMLInStyleAttributes) styleAttributes() (o *StyleAttributes) {
	o = &StyleAttributes{
		TTMLDirection:      s.Direction,
		TTMLDisplay:        s.Display,
		TTMLDisplayAlign:   s.DisplayAlign,
		TTMLExtent:         s.Extent,
		TTMLFontFamily:     s.FontFamily,
		TTMLFontSize:       s.FontSize,
		TTMLFontStyle:      s.FontStyle,
		TTMLFontWeight:     s.FontWeight,
		TTMLLineHeight:     s.LineHeight,
		TTMLOpacity:        s.Opacity,
		TTMLOrigin:         s.Origin,
		TTMLOverflow:       s.Overflow,
		TTMLPadding:        s.Padding,
		TTMLShowBackground: s.ShowBackground,
		TTMLTextAlign:      s.TextAlign,
		TTMLTextDecoration: s.TextDecoration,
		TTMLTextOutline:    s.TextOutline,
		TTMLUnicodeBidi:    s.UnicodeBidi,
		TTMLVisibility:     s.Visibility,
		TTMLWrapOption:     s.WrapOption,
		TTMLWritingMode:    s.WritingMode,
		TTMLZIndex:         s.ZIndex,
	}
	// Parse colors if present
	if s.Color != nil {
		o.TTMLColor = newColorFromHTMLString(*s.Color)
	}
	if s.BackgroundColor != nil {
		o.TTMLBackgroundColor = newColorFromHTMLString(*s.BackgroundColor)
	}
	o.propagateTTMLAttributes()
	return
}

// TTMLInHeader represents an input TTML header
type TTMLInHeader struct {
	ID    string `xml:"id,attr,omitempty"`
	Style string `xml:"style,attr,omitempty"`
	TTMLInStyleAttributes
}

// TTMLInRegion represents an input TTML region
type TTMLInRegion struct {
	TTMLInHeader
	XMLName xml.Name `xml:"region"`
}

// TTMLInStyle represents an input TTML style
type TTMLInStyle struct {
	TTMLInHeader
	XMLName xml.Name `xml:"style"`
}

// TTMLInSubtitle represents an input TTML subtitle
type TTMLInSubtitle struct {
	Begin *TTMLInDuration `xml:"begin,attr,omitempty"`
	End   *TTMLInDuration `xml:"end,attr,omitempty"`
	ID    string          `xml:"id,attr,omitempty"`
	// We must store inner XML temporarily here since there's no tag to describe both any tag and chardata
	// Real unmarshal will be done manually afterwards
	Items  string  `xml:",innerxml"`
	Region string  `xml:"region,attr,omitempty"`
	Space  *string `xml:"space,attr,omitempty"`
	Style  string  `xml:"style,attr,omitempty"`
	TTMLInStyleAttributes
}

// TTMLInItem represents an input TTML item. It is kept for API compatibility;
// inline content is now parsed as an ordered ttmlNode tree.
type TTMLInItem struct {
	Style string `xml:"style,attr,omitempty"`
	// Ruby holds the value of the tts:ruby property ("base", "text",
	// "container", "baseContainer" or "textContainer"). Matching is by local
	// name, so any prefix bound to the styling namespace works.
	Ruby  *string `xml:"ruby,attr,omitempty"`
	Space *string `xml:"space,attr,omitempty"`
	Text  string  `xml:",chardata"`
	TTMLInStyleAttributes
	XMLName xml.Name
	// Children keeps nested spans so ruby containers and other inline markup
	// stay structured instead of being flattened away.
	Children []TTMLInItem `xml:",any"`
}

// ttmlNode is a minimal ordered XML tree used for inline TTML content. It
// distinguishes element children from text so markup order (ruby containers,
// <br>) and inline styling can be reconstructed.
type ttmlNode struct {
	attrs    []xml.Attr
	children []*ttmlNode
	name     xml.Name
	text     string
}

// ttmlNodeTreeDecoder decodes inline TTML content into a ttmlNode tree.
// Unlike encoding/xml's struct tags it needs no namespace prefix to be
// declared on each span, and it keeps text and elements in document order.
type ttmlNodeTreeDecoder struct {
	decoder *xml.Decoder
}

func newTTMLNodeTreeDecoder(s string) *ttmlNodeTreeDecoder {
	return &ttmlNodeTreeDecoder{
		decoder: xml.NewDecoder(strings.NewReader("<p>" + s + "</p>")),
	}
}

func (d *ttmlNodeTreeDecoder) decode() (root *ttmlNode, err error) {
	var stack []*ttmlNode
	for {
		var t xml.Token
		if t, err = d.decoder.Token(); err != nil {
			if err == io.EOF {
				err = nil
				break
			}
			err = fmt.Errorf("astisub: getting next token failed: %w", err)
			return
		}

		switch v := t.(type) {
		case xml.StartElement:
			n := &ttmlNode{attrs: v.Attr, name: v.Name}
			if len(stack) == 0 {
				root = n
			} else {
				stack[len(stack)-1].children = append(stack[len(stack)-1].children, n)
			}
			stack = append(stack, n)
		case xml.EndElement:
			if len(stack) > 0 {
				stack = stack[:len(stack)-1]
			}
		case xml.CharData:
			if len(stack) == 0 {
				continue
			}
			parent := stack[len(stack)-1]
			// Whitespace-only nodes are layout indentation unless the parent is
			// inline content that explicitly preserves whitespace. Containers
			// (p, ruby containers) lay their children out with indentation even
			// when xml:space="preserve" is inherited.
			if len(strings.TrimSpace(string(v))) == 0 &&
				(!parent.ttmlSpacePreserved() || ttmlNodeIsStructural(parent)) {
				continue
			}
			parent.children = append(parent.children, &ttmlNode{text: string(v)})
		}
	}

	if root == nil {
		err = fmt.Errorf("astisub: no inline TTML content found")
		return
	}
	return
}

// attr returns the value of the first attribute with the given local name.
func (n *ttmlNode) attr(local string) (string, bool) {
	for _, a := range n.attrs {
		if strings.EqualFold(a.Name.Local, local) {
			return a.Value, true
		}
	}
	return "", false
}

// isElement reports whether n is an element with the given local name.
func (n *ttmlNode) isElement(local string) bool {
	return n.name.Local != "" && strings.EqualFold(n.name.Local, local)
}

// ttmlRubyValue is the tts:ruby property value of the node.
func (n *ttmlNode) ttmlRubyValue() string {
	if v, ok := n.attr("ruby"); ok {
		return strings.TrimSpace(v)
	}
	return ""
}

// ttmlSpacePreserved reports whether xml:space="preserve" is set on the node.
func (n *ttmlNode) ttmlSpacePreserved() bool {
	if v, ok := n.attr("space"); ok {
		return v == "preserve"
	}
	return false
}

// ttmlNodeIsStructural reports whether the node lays out element children on
// separate lines. Whitespace between such children is always indentation, even
// under xml:space="preserve".
func ttmlNodeIsStructural(n *ttmlNode) bool {
	switch strings.ToLower(n.name.Local) {
	case "p", "div", "body", "tt":
		return true
	case "span":
		switch strings.ToLower(n.ttmlRubyValue()) {
		case ttmlRubyContainer, ttmlRubyBaseContainer, ttmlRubyTextContainer:
			return true
		}
	}
	return false
}

// ttmlCleanText applies the whitespace model of one text node. Whitespace at
// the beginning of a source line is layout indentation and is dropped. Other
// whitespace is kept verbatim with xml:space="preserve" and collapsed to a
// single space otherwise. Source newlines themselves are layout.
func ttmlCleanText(s string, preserve bool) string {
	var b strings.Builder
	lineStart := false
	lastSpace := false
	for _, r := range s {
		if r == '\n' {
			lineStart = true
			lastSpace = false
			continue
		}
		if r == '\r' {
			continue
		}
		if lineStart {
			if unicode.IsSpace(r) {
				continue
			}
			lineStart = false
		}
		if !preserve && unicode.IsSpace(r) {
			if !lastSpace {
				b.WriteByte(' ')
				lastSpace = true
			}
			continue
		}
		lastSpace = false
		b.WriteRune(r)
	}
	return b.String()
}

// ttmlStyleAttributes builds the style attributes carried by the node itself.
func (n *ttmlNode) ttmlStyleAttributes() *StyleAttributes {
	var sa TTMLInStyleAttributes
	for _, a := range n.attrs {
		switch strings.ToLower(a.Name.Local) {
		case "color":
			v := a.Value
			sa.Color = &v
		case "backgroundcolor":
			v := a.Value
			sa.BackgroundColor = &v
		}
	}
	return sa.styleAttributes()
}

// TTMLInDuration represents an input TTML duration
type TTMLInDuration struct {
	d                 time.Duration
	frames, framerate int // Framerate is in frame/s
	ticks, tickrate   int // Tickrate is in ticks/s
}

// UnmarshalText implements the TextUnmarshaler interface
// Possible formats are:
// - hh:mm:ss.mmm
// - hh:mm:ss:fff (fff being frames)
// - [ticks]t ([ticks] being the tick amount)
func (d *TTMLInDuration) UnmarshalText(i []byte) (err error) {
	// Reset duration
	d.d = time.Duration(0)
	d.frames = 0
	d.ticks = 0

	// Check offset time
	text := string(i)
	if matches := ttmlRegexpOffsetTime.FindStringSubmatch(text); matches != nil {
		// Parse value
		var value float64
		if value, err = strconv.ParseFloat(matches[1], 64); err != nil {
			err = fmt.Errorf("astisub: failed to parse value %s", matches[1])
			return
		}

		// Parse metric
		metric := matches[3]

		// Update duration
		if metric == "t" {
			d.ticks = int(value)
		} else if metric == "f" {
			d.frames = int(value)
		} else {
			// Get timebase
			var timebase time.Duration
			switch metric {
			case "h":
				timebase = time.Hour
			case "m":
				timebase = time.Minute
			case "s":
				timebase = time.Second
			case "ms":
				timebase = time.Millisecond
			default:
				err = fmt.Errorf("astisub: invalid metric %s", metric)
				return
			}

			// Update duration
			d.d = time.Duration(value * float64(timebase.Nanoseconds()))
		}
		return
	}

	// Extract clock time frames
	if indexes := ttmlRegexpClockTimeFrames.FindStringIndex(text); indexes != nil {
		// Parse frames
		var s = text[indexes[0]+1 : indexes[1]]
		if d.frames, err = strconv.Atoi(s); err != nil {
			err = fmt.Errorf("astisub: atoi %s failed: %w", s, err)
			return
		}

		// Update text
		text = text[:indexes[0]] + ".000"
	}

	d.d, err = parseDuration(text, ".", 3)
	return
}

// duration returns the input TTML Duration's time.Duration
func (d TTMLInDuration) duration() (o time.Duration) {
	if d.ticks > 0 && d.tickrate > 0 {
		return time.Duration(float64(d.ticks) * 1e9 / float64(d.tickrate))
	}
	o = d.d
	if d.frames > 0 && d.framerate > 0 {
		o += time.Duration(float64(d.frames) / float64(d.framerate) * float64(time.Second.Nanoseconds()))
	}
	return
}

// ReadFromTTML parses a .ttml content
func ReadFromTTML(i io.Reader) (o *Subtitles, err error) {
	// Init
	o = NewSubtitles()

	// Unmarshal XML
	var ttml TTMLIn
	if err = xml.NewDecoder(i).Decode(&ttml); err != nil {
		err = fmt.Errorf("astisub: xml decoding failed: %w", err)
		return
	}

	// Add metadata
	o.Metadata = ttml.metadata()

	// Loop through styles
	var parentStyles = make(map[string]*Style)
	for _, ts := range ttml.Styles {
		var s = &Style{
			ID:          ts.ID,
			InlineStyle: ts.TTMLInStyleAttributes.styleAttributes(),
		}
		o.Styles[s.ID] = s
		if len(ts.Style) > 0 {
			parentStyles[ts.Style] = s
		}
	}

	// Take care of parent styles
	for id, s := range parentStyles {
		if _, ok := o.Styles[id]; !ok {
			err = fmt.Errorf("astisub: Style %s requested by style %s doesn't exist", id, s.ID)
			return
		}
		s.Style = o.Styles[id]
	}

	// Loop through regions
	for _, tr := range ttml.Regions {
		var r = &Region{
			ID:          tr.ID,
			InlineStyle: tr.TTMLInStyleAttributes.styleAttributes(),
		}
		if len(tr.Style) > 0 {
			if _, ok := o.Styles[tr.Style]; !ok {
				err = fmt.Errorf("astisub: Style %s requested by region %s doesn't exist", tr.Style, r.ID)
				return
			}
			r.Style = o.Styles[tr.Style]
		}
		o.Regions[r.ID] = r
	}

	// Loop through subtitles
	bodyInlineStyle := ttml.Body.TTMLInStyleAttributes.styleAttributes()
	for _, div := range ttml.Body.Divs {
		divInlineStyle := div.TTMLInStyleAttributes.styleAttributes()

		// Propagate styles from Body -> Div
		divInlineStyle.merge(bodyInlineStyle)
		if div.Region == "" {
			div.Region = ttml.Body.Region
		}
		if div.Style == "" {
			div.Style = ttml.Body.Style
		}
		for _, ts := range div.Subtitles {
			// Init item
			ts.Begin.framerate = ttml.Framerate
			ts.Begin.tickrate = ttml.Tickrate
			ts.End.framerate = ttml.Framerate
			ts.End.tickrate = ttml.Tickrate

			itemInlineStyle := ts.TTMLInStyleAttributes.styleAttributes()

			// Propagate styles from Body -> Div -> Item.
			// If the item has its own Region, Style, or InlineStyle, it overrides the Div's.
			// This ensures all relevant styles are preserved at the item level,
			// maintaining compatibility with existing logic that relies on the Subtitles structure.
			itemInlineStyle.merge(divInlineStyle)
			if ts.Region == "" {
				ts.Region = div.Region
			}
			if ts.Style == "" {
				ts.Style = div.Style
			}

			var s = &Item{
				EndAt:       ts.End.duration(),
				InlineStyle: itemInlineStyle,
				StartAt:     ts.Begin.duration(),
			}

			// Add region
			if len(ts.Region) > 0 {
				if _, ok := o.Regions[ts.Region]; !ok {
					err = fmt.Errorf("astisub: Region %s requested by subtitle between %s and %s doesn't exist", ts.Region, s.StartAt, s.EndAt)
					return
				}
				s.Region = o.Regions[ts.Region]
			}

			// Add style
			if len(ts.Style) > 0 {
				if _, ok := o.Styles[ts.Style]; !ok {
					err = fmt.Errorf("astisub: Style %s requested by subtitle between %s and %s doesn't exist", ts.Style, s.StartAt, s.EndAt)
					return
				}
				s.Style = o.Styles[ts.Style]
			}

			root, errTree := newTTMLNodeTreeDecoder(ts.Items).decode()
			if errTree != nil {
				err = fmt.Errorf("astisub: unmarshaling items failed: %w", errTree)
				return
			}

			var c = &ttmlLineBuilder{
				item:      s,
				styles:    o.Styles,
				lineStart: true,
			}
			// Pass the children rather than the synthetic wrapper so the
			// surrounding indentation is not interpreted as content.
			preserve := root.ttmlSpacePreserved()
			if ts.Space != nil {
				preserve = *ts.Space == "preserve"
			}
			c.appendNodes(root.children, itemInlineStyle, nil, preserve)
			c.endLine()

			// Append subtitle
			o.Items = append(o.Items, s)
		}
	}
	return
}

// ttmlLineBuilder turns the ordered inline node tree of one <p> into Lines.
// It keeps the physical line-start state across nodes so layout indentation
// can be dropped without trimming meaningful spaces inside a line.
type ttmlLineBuilder struct {
	item      *Item
	styles    map[string]*Style
	line      *Line
	lineStart bool
}

// appendNodes walks inline nodes in document order.
func (b *ttmlLineBuilder) appendNodes(nodes []*ttmlNode, inherited *StyleAttributes, style *Style, preserve bool) {
	for _, n := range nodes {
		if n.name.Local == "" {
			b.appendText(n.text, inherited, style, preserve)
			continue
		}

		if n.isElement("br") {
			b.endLine()
			continue
		}

		nodePreserve := preserve || n.ttmlSpacePreserved()
		nodeStyle := b.styleForNode(n, style)
		nodeAttrs := n.ttmlStyleAttributes()
		if inherited != nil && inherited.TTMLColor != nil && nodeAttrs.TTMLColor == nil {
			// Inline colors inherit from outer to inner.
			merged := *nodeAttrs
			merged.TTMLColor = inherited.TTMLColor
			merged.propagateTTMLAttributes()
			nodeAttrs = &merged
		}

		if strings.EqualFold(n.ttmlRubyValue(), ttmlRubyContainer) {
			b.appendRubyContainer(n, nodeAttrs, nodeStyle, nodePreserve)
			continue
		}
		b.appendNodes(n.children, nodeAttrs, nodeStyle, nodePreserve)
	}
}

// appendText emits one text node.
func (b *ttmlLineBuilder) appendText(s string, inherited *StyleAttributes, style *Style, preserve bool) {
	s = ttmlCleanText(s, preserve)
	if s == "" {
		return
	}
	b.lineStart = false
	b.appendItem(LineItem{
		InlineStyle: inherited,
		Style:       style,
		Text:        s,
	})
}

func (b *ttmlLineBuilder) styleForNode(n *ttmlNode, parent *Style) *Style {
	v, ok := n.attr("style")
	if !ok || strings.TrimSpace(v) == "" {
		return parent
	}
	return b.styles[strings.TrimSpace(v)]
}

func (b *ttmlLineBuilder) appendItem(li LineItem) {
	if b.line == nil {
		b.line = &Line{}
	}
	b.line.Items = append(b.line.Items, li)
}

func (b *ttmlLineBuilder) endLine() {
	if b.line == nil {
		b.line = &Line{}
	}
	b.item.Lines = append(b.item.Lines, *b.line)
	b.line = nil
	b.lineStart = true
}

// appendRubyContainer converts a ruby container into pairs in the public model.
// A simple container holds one base and one text in order; a grouped container
// holds a baseContainer followed by a textContainer whose children pair by
// position. Only complete, equally sized groupings are expanded.
func (b *ttmlLineBuilder) appendRubyContainer(container *ttmlNode, inherited *StyleAttributes, style *Style, preserve bool) {
	var bases, readings [][]LineItem

	// pairByPosition emits the bases collected so far, each with the reading at
	// the same position. Called at group boundaries so a br does not make pairs
	// from different groups.
	pairByPosition := func() {
		n := len(bases)
		if len(readings) > n {
			n = len(readings)
		}
		for i := 0; i < n; i++ {
			if i >= len(bases) || len(bases[i]) == 0 {
				continue
			}
			var r *Ruby
			if i < len(readings) && len(readings[i]) > 0 {
				r = &Ruby{Text: readings[i]}
			}
			for _, li := range bases[i] {
				li.Ruby = r
				b.appendItem(li)
			}
		}
		bases = nil
		readings = nil
	}

	for _, c := range container.children {
		if c.name.Local == "" {
			continue
		}
		switch {
		case c.isElement("br"):
			// A break between groups stays a line break.
			pairByPosition()
			b.endLine()
		case strings.EqualFold(c.ttmlRubyValue(), ttmlRubyBaseContainer):
			for _, u := range collectRubyUnits(c, ttmlRubyBase) {
				bases = append(bases, flattenRubyNode(u, inherited, b.styles, preserve))
			}
		case strings.EqualFold(c.ttmlRubyValue(), ttmlRubyTextContainer):
			for _, u := range collectRubyUnits(c, ttmlRubyText) {
				readings = append(readings, flattenRubyNode(u, inherited, b.styles, preserve))
			}
		case strings.EqualFold(c.ttmlRubyValue(), ttmlRubyBase):
			bases = append(bases, flattenRubyNode(c, inherited, b.styles, preserve))
		case strings.EqualFold(c.ttmlRubyValue(), ttmlRubyText):
			readings = append(readings, flattenRubyNode(c, inherited, b.styles, preserve))
		}
	}
	pairByPosition()
	b.lineStart = false
}

// collectRubyUnits returns, in order, the child elements carrying the given
// ruby role. Content without a role belongs to the preceding unit.
func collectRubyUnits(container *ttmlNode, role string) []*ttmlNode {
	var units []*ttmlNode
	for _, c := range container.children {
		if c.name.Local == "" {
			continue
		}
		if strings.EqualFold(c.ttmlRubyValue(), role) {
			units = append(units, c)
		} else if len(units) > 0 {
			units[len(units)-1].children = append(units[len(units)-1].children, c)
		}
	}
	return units
}

// flattenRubyNode collects the runs of one base or text unit, resolving colors
// from outer to inner. Nested spans without a ruby role are transparent.
func flattenRubyNode(n *ttmlNode, inherited *StyleAttributes, styles map[string]*Style, preserve bool) (o []LineItem) {
	attrs := n.ttmlStyleAttributes()
	if inherited != nil && inherited.TTMLColor != nil && attrs.TTMLColor == nil {
		merged := *attrs
		merged.TTMLColor = inherited.TTMLColor
		merged.propagateTTMLAttributes()
		attrs = &merged
	}
	var style *Style
	if v, ok := n.attr("style"); ok {
		style = styles[strings.TrimSpace(v)]
	}
	unitStart := true
	usedStart := false
	flattenRubyChildren(n.children, attrs, style, preserve || n.ttmlSpacePreserved(), &unitStart, &usedStart, &o)
	return
}

func flattenRubyChildren(nodes []*ttmlNode, inherited *StyleAttributes, style *Style, preserve bool, unitStart, usedStart *bool, o *[]LineItem) {
	for _, n := range nodes {
		if n.name.Local == "" {
			s := ttmlCleanText(n.text, preserve)
			// Whitespace-only content before the first run of a unit is layout
			// indentation.
			if !*usedStart && !preserve && strings.TrimSpace(s) == "" {
				continue
			}
			if s == "" {
				continue
			}
			*usedStart = true
			*unitStart = false
			*o = append(*o, LineItem{InlineStyle: inherited, Style: style, Text: s})
			continue
		}
		if n.isElement("br") {
			// Line breaks inside a pair are not expanded.
			continue
		}
		attrs := n.ttmlStyleAttributes()
		if inherited != nil && inherited.TTMLColor != nil && attrs.TTMLColor == nil {
			merged := *attrs
			merged.TTMLColor = inherited.TTMLColor
			merged.propagateTTMLAttributes()
			attrs = &merged
		}
		nodeStyle := style
		if v, ok := n.attr("style"); ok {
			nodeStyle = nil
			_ = v
		}
		flattenRubyChildren(n.children, attrs, nodeStyle, preserve || n.ttmlSpacePreserved(), unitStart, usedStart, o)
	}
}

// TTMLOut represents an output TTML that must be marshaled
// We split it from the input TTML as this time we'll add strict namespaces
type TTMLOut struct {
	Lang            string            `xml:"xml:lang,attr,omitempty"`
	Metadata        *TTMLOutMetadata  `xml:"head>metadata,omitempty"`
	Styles          []TTMLOutStyle    `xml:"head>styling>style,omitempty"` //!\\ Order is important! Keep Styling above Layout
	Regions         []TTMLOutRegion   `xml:"head>layout>region,omitempty"`
	Subtitles       []TTMLOutSubtitle `xml:"body>div>p,omitempty"`
	XMLName         xml.Name          `xml:"http://www.w3.org/ns/ttml tt"`
	XMLNamespaceTTM string            `xml:"xmlns:ttm,attr"`
	XMLNamespaceTTS string            `xml:"xmlns:tts,attr"`
}

// TTMLOutMetadata represents an output TTML Metadata
type TTMLOutMetadata struct {
	Copyright string `xml:"ttm:copyright,omitempty"`
	Title     string `xml:"ttm:title,omitempty"`
}

// TTMLOutStyleAttributes represents output TTML style attributes
type TTMLOutStyleAttributes struct {
	BackgroundColor *string `xml:"tts:backgroundColor,attr,omitempty"`
	Color           *string `xml:"tts:color,attr,omitempty"`
	Direction       *string `xml:"tts:direction,attr,omitempty"`
	Display         *string `xml:"tts:display,attr,omitempty"`
	DisplayAlign    *string `xml:"tts:displayAlign,attr,omitempty"`
	Extent          *string `xml:"tts:extent,attr,omitempty"`
	FontFamily      *string `xml:"tts:fontFamily,attr,omitempty"`
	FontSize        *string `xml:"tts:fontSize,attr,omitempty"`
	FontStyle       *string `xml:"tts:fontStyle,attr,omitempty"`
	FontWeight      *string `xml:"tts:fontWeight,attr,omitempty"`
	LineHeight      *string `xml:"tts:lineHeight,attr,omitempty"`
	Opacity         *string `xml:"tts:opacity,attr,omitempty"`
	Origin          *string `xml:"tts:origin,attr,omitempty"`
	Overflow        *string `xml:"tts:overflow,attr,omitempty"`
	Padding         *string `xml:"tts:padding,attr,omitempty"`
	ShowBackground  *string `xml:"tts:showBackground,attr,omitempty"`
	TextAlign       *string `xml:"tts:textAlign,attr,omitempty"`
	TextDecoration  *string `xml:"tts:textDecoration,attr,omitempty"`
	TextOutline     *string `xml:"tts:textOutline,attr,omitempty"`
	UnicodeBidi     *string `xml:"tts:unicodeBidi,attr,omitempty"`
	Visibility      *string `xml:"tts:visibility,attr,omitempty"`
	WrapOption      *string `xml:"tts:wrapOption,attr,omitempty"`
	WritingMode     *string `xml:"tts:writingMode,attr,omitempty"`
	ZIndex          *int    `xml:"tts:zIndex,attr,omitempty"`
}

// ttmlOutStyleAttributesFromStyleAttributes converts StyleAttributes into a TTMLOutStyleAttributes
func ttmlOutStyleAttributesFromStyleAttributes(s *StyleAttributes) TTMLOutStyleAttributes {
	if s == nil {
		return TTMLOutStyleAttributes{}
	}
	var color *string
	if s.TTMLColor != nil {
		// Try to convert to named color first, fall back to hex
		color = astikit.StrPtr(s.TTMLColor.HTMLString())
	}
	var backgroundColor *string
	if s.TTMLBackgroundColor != nil {
		backgroundColor = astikit.StrPtr(s.TTMLBackgroundColor.HTMLString())
	}
	return TTMLOutStyleAttributes{
		BackgroundColor: backgroundColor,
		Color:           color,
		Direction:       s.TTMLDirection,
		Display:         s.TTMLDisplay,
		DisplayAlign:    s.TTMLDisplayAlign,
		Extent:          s.TTMLExtent,
		FontFamily:      s.TTMLFontFamily,
		FontSize:        s.TTMLFontSize,
		FontStyle:       s.TTMLFontStyle,
		FontWeight:      s.TTMLFontWeight,
		LineHeight:      s.TTMLLineHeight,
		Opacity:         s.TTMLOpacity,
		Origin:          s.TTMLOrigin,
		Overflow:        s.TTMLOverflow,
		Padding:         s.TTMLPadding,
		ShowBackground:  s.TTMLShowBackground,
		TextAlign:       s.TTMLTextAlign,
		TextDecoration:  s.TTMLTextDecoration,
		TextOutline:     s.TTMLTextOutline,
		UnicodeBidi:     s.TTMLUnicodeBidi,
		Visibility:      s.TTMLVisibility,
		WrapOption:      s.TTMLWrapOption,
		WritingMode:     s.TTMLWritingMode,
		ZIndex:          s.TTMLZIndex,
	}
}

// TTMLOutHeader represents an output TTML header
type TTMLOutHeader struct {
	ID    string `xml:"xml:id,attr,omitempty"`
	Style string `xml:"style,attr,omitempty"`
	TTMLOutStyleAttributes
}

// TTMLOutRegion represents an output TTML region
type TTMLOutRegion struct {
	TTMLOutHeader
	XMLName xml.Name `xml:"region"`
}

// TTMLOutStyle represents an output TTML style
type TTMLOutStyle struct {
	TTMLOutHeader
	XMLName xml.Name `xml:"style"`
}

// TTMLOutSubtitle represents an output TTML subtitle
type TTMLOutSubtitle struct {
	Begin  TTMLOutDuration `xml:"begin,attr"`
	End    TTMLOutDuration `xml:"end,attr"`
	ID     string          `xml:"id,attr,omitempty"`
	Items  []TTMLOutItem
	Region string `xml:"region,attr,omitempty"`
	Style  string `xml:"style,attr,omitempty"`
	TTMLOutStyleAttributes
}

// TTMLOutItem represents an output TTML Item
type TTMLOutItem struct {
	Style string `xml:"style,attr,omitempty"`
	// Ruby is the tts:ruby property for base, text and container spans.
	Ruby     *string `xml:"tts:ruby,attr,omitempty"`
	Text     string  `xml:",chardata"`
	Children []TTMLOutItem
	TTMLOutStyleAttributes
	XMLName xml.Name
}

// ttmlOutRubyItem writes one pair as a simple container holding a single base
// span and a single text span. Each side keeps one nested span per run so the
// exact runs, and therefore colors, survive the round trip.
func ttmlOutRubyItem(base []LineItem, r *Ruby) TTMLOutItem {
	var container = TTMLOutItem{
		XMLName: xml.Name{Local: "span"},
		Ruby:    astikit.StrPtr(ttmlRubyContainer),
	}
	var baseSpan = TTMLOutItem{
		XMLName: xml.Name{Local: "span"},
		Ruby:    astikit.StrPtr(ttmlRubyBase),
	}
	for _, li := range base {
		baseSpan.Children = append(baseSpan.Children, ttmlOutRun(li))
	}
	container.Children = append(container.Children, baseSpan)

	var textSpan = TTMLOutItem{
		XMLName: xml.Name{Local: "span"},
		Ruby:    astikit.StrPtr(ttmlRubyText),
	}
	if r != nil {
		for _, li := range r.Text {
			textSpan.Children = append(textSpan.Children, ttmlOutRun(li))
		}
	}
	container.Children = append(container.Children, textSpan)
	return container
}

// ttmlOutRun converts one LineItem run into a styled span.
func ttmlOutRun(li LineItem) TTMLOutItem {
	item := TTMLOutItem{
		Text:                   li.Text,
		TTMLOutStyleAttributes: ttmlOutStyleAttributesFromStyleAttributes(li.InlineStyle),
		XMLName:                xml.Name{Local: "span"},
	}
	if li.Style != nil {
		item.Style = li.Style.ID
	}
	return item
}

// TTMLOutDuration represents an output TTML duration
type TTMLOutDuration time.Duration

// MarshalText implements the TextMarshaler interface
func (t TTMLOutDuration) MarshalText() ([]byte, error) {
	return []byte(formatDuration(time.Duration(t), ".", 3)), nil
}

// WriteToTTMLOptions represents TTML write options.
type WriteToTTMLOptions struct {
	Indent string // Default is 4 spaces.
}

// WriteToTTMLOption represents a WriteToTTML option.
type WriteToTTMLOption func(o *WriteToTTMLOptions)

// WriteToTTMLWithIndentOption sets the indent option.
func WriteToTTMLWithIndentOption(indent string) WriteToTTMLOption {
	return func(o *WriteToTTMLOptions) {
		o.Indent = indent
	}
}

// WriteToTTML writes subtitles in .ttml format
func (s Subtitles) WriteToTTML(o io.Writer, opts ...WriteToTTMLOption) (err error) {
	// Create write options
	wo := &WriteToTTMLOptions{Indent: "    "}
	for _, opt := range opts {
		opt(wo)
	}

	// Do not write anything if no subtitles
	if len(s.Items) == 0 {
		return ErrNoSubtitlesToWrite
	}

	// Init TTML
	var ttml = TTMLOut{
		XMLNamespaceTTM: "http://www.w3.org/ns/ttml#metadata",
		XMLNamespaceTTS: "http://www.w3.org/ns/ttml#styling",
	}

	// Add metadata
	if s.Metadata != nil {
		if v, ok := ttmlLanguageMapping.GetInverse(s.Metadata.Language); ok {
			ttml.Lang = v.(string)
		}
		if len(s.Metadata.TTMLCopyright) > 0 || len(s.Metadata.Title) > 0 {
			ttml.Metadata = &TTMLOutMetadata{
				Copyright: s.Metadata.TTMLCopyright,
				Title:     s.Metadata.Title,
			}
		}
	}

	// Add regions
	var k []string
	for _, region := range s.Regions {
		k = append(k, region.ID)
	}
	sort.Strings(k)
	for _, id := range k {
		var ttmlRegion = TTMLOutRegion{TTMLOutHeader: TTMLOutHeader{
			ID:                     s.Regions[id].ID,
			TTMLOutStyleAttributes: ttmlOutStyleAttributesFromStyleAttributes(s.Regions[id].InlineStyle),
		}}
		if s.Regions[id].Style != nil {
			ttmlRegion.Style = s.Regions[id].Style.ID
		}
		ttml.Regions = append(ttml.Regions, ttmlRegion)
	}

	// Add styles
	k = []string{}
	for _, style := range s.Styles {
		k = append(k, style.ID)
	}
	sort.Strings(k)
	for _, id := range k {
		var ttmlStyle = TTMLOutStyle{TTMLOutHeader: TTMLOutHeader{
			ID:                     s.Styles[id].ID,
			TTMLOutStyleAttributes: ttmlOutStyleAttributesFromStyleAttributes(s.Styles[id].InlineStyle),
		}}
		if s.Styles[id].Style != nil {
			ttmlStyle.Style = s.Styles[id].Style.ID
		}
		ttml.Styles = append(ttml.Styles, ttmlStyle)
	}

	// Add items
	for _, item := range s.Items {
		// Init subtitle
		var ttmlSubtitle = TTMLOutSubtitle{
			Begin:                  TTMLOutDuration(item.StartAt),
			End:                    TTMLOutDuration(item.EndAt),
			TTMLOutStyleAttributes: ttmlOutStyleAttributesFromStyleAttributes(item.InlineStyle),
		}

		// Add region
		if item.Region != nil {
			ttmlSubtitle.Region = item.Region.ID
		}

		// Add style
		if item.Style != nil {
			ttmlSubtitle.Style = item.Style.ID
		}

		// Add lines
		for _, line := range item.Lines {
			// Each ruby pair is written as one container holding a base
			// followed by a text. Runs are grouped by their shared *Ruby value,
			// so editing or constructing pairs directly in the model works.
			for idx := 0; idx < len(line.Items); idx++ {
				lineItem := line.Items[idx]
				if r := lineItem.Ruby; r != nil {
					start := idx
					for idx+1 < len(line.Items) && line.Items[idx+1].Ruby == r {
						idx++
					}
					ttmlSubtitle.Items = append(ttmlSubtitle.Items, ttmlOutRubyItem(line.Items[start:idx+1], r))
					continue
				}

				// Init ttml item
				var ttmlItem = TTMLOutItem{
					Text:                   lineItem.Text,
					TTMLOutStyleAttributes: ttmlOutStyleAttributesFromStyleAttributes(lineItem.InlineStyle),
					XMLName:                xml.Name{Local: "span"},
				}

				// Add style
				if lineItem.Style != nil {
					ttmlItem.Style = lineItem.Style.ID
				}

				// Add ttml item
				ttmlSubtitle.Items = append(ttmlSubtitle.Items, ttmlItem)
			}

			// Add line break
			ttmlSubtitle.Items = append(ttmlSubtitle.Items, TTMLOutItem{XMLName: xml.Name{Local: "br"}})
		}

		// Remove last line break
		if len(ttmlSubtitle.Items) > 0 {
			ttmlSubtitle.Items = ttmlSubtitle.Items[:len(ttmlSubtitle.Items)-1]
		}

		// Append subtitle
		ttml.Subtitles = append(ttml.Subtitles, ttmlSubtitle)
	}

	// Marshal XML
	var e = xml.NewEncoder(o)

	// Set indent
	e.Indent("", wo.Indent)

	if err = e.Encode(ttml); err != nil {
		err = fmt.Errorf("astisub: xml encoding failed: %w", err)
		return
	}
	return
}
