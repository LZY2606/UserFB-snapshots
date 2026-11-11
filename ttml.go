package astisub

import (
	"encoding/xml"
	"fmt"
	"io"
	"reflect"
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
	ttmlRegexpOffsetTime      = regexp.MustCompile(`^([+-]?\d+(\.\d+)?)(h|m|s|ms|f|t)$`)
)

// ttmlInElement is the generic, order-preserving representation of a TTML
// element. Its inner XML is kept raw and parsed recursively so that nested
// divs and spans, mixed character data and "br" elements keep their source
// order.
type ttmlInElement struct {
	XMLName xml.Name

	Begin *TTMLInDuration `xml:"begin,attr,omitempty"`
	Dur   *TTMLInDuration `xml:"dur,attr,omitempty"`
	End   *TTMLInDuration `xml:"end,attr,omitempty"`
	ID    string          `xml:"id,attr,omitempty"`
	// Region and Style are inherited down the tree.
	Region string `xml:"region,attr,omitempty"`
	Style  string `xml:"style,attr,omitempty"`
	// XMLSpace is xml:space.
	XMLSpace string `xml:"space,attr,omitempty"`

	// Timing features that are deliberately not supported.
	Repeat        string `xml:"repeat,attr,omitempty"`
	RepeatCount   string `xml:"repeatCount,attr,omitempty"`
	RepeatDur     string `xml:"repeatDur,attr,omitempty"`
	TimeContainer string `xml:"timeContainer,attr,omitempty"`

	TTMLInStyleAttributes

	InnerXML string `xml:",innerxml"`

	// Set manually for character data nodes.
	IsText bool   `xml:"-"`
	Text   string `xml:"-"`
}

// TTMLIn represents an input TTML that must be unmarshaled
// We split it from the output TTML as we can't add strict namespace without breaking retrocompatibility
type TTMLIn struct {
	Framerate     int            `xml:"frameRate,attr"`
	Lang          string         `xml:"lang,attr"`
	Metadata      TTMLInMetadata `xml:"head>metadata"`
	Regions       []TTMLInRegion `xml:"head>layout>region"`
	Styles        []TTMLInStyle  `xml:"head>styling>style"`
	Body          ttmlInElement  `xml:"body"`
	Tickrate      int            `xml:"tickRate,attr"`
	TimeBase      string         `xml:"timeBase,attr,omitempty"`
	TimeContainer string         `xml:"timeContainer,attr,omitempty"`
	Repeat        string         `xml:"repeat,attr,omitempty"`
	RepeatCount   string         `xml:"repeatCount,attr,omitempty"`
	RepeatDur     string         `xml:"repeatDur,attr,omitempty"`
	XMLSpace      string         `xml:"space,attr,omitempty"`
	XMLName       xml.Name       `xml:"tt"`
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

// TTMLInDuration represents an input TTML duration
type TTMLInDuration struct {
	d                 time.Duration
	frames, framerate int // Framerate is in frame/s
	ticks, tickrate   int // Tickrate is in ticks/s
	raw               string
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
	d.raw = strings.TrimSpace(string(i))

	// Check offset time
	text := d.raw
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

	// Handle a negative clock time. The sign is remembered so that negative
	// time values can be rejected with a node-aware error later on.
	negative := strings.HasPrefix(text, "-")
	if negative {
		text = strings.TrimPrefix(text, "-")
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
	if err != nil {
		return
	}
	if negative {
		d.d = -d.d
		d.frames = -d.frames
	}
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

// ttmlTimingContext carries the document-wide timing configuration.
type ttmlTimingContext struct {
	framerate    int
	tickrate     int
	timeBase     string
	nestedTiming bool // timing exists below p (body/div/span)
}

// ttmlRuntimeNode is a parsed TTML node with its resolved timing and styles.
type ttmlRuntimeNode struct {
	Name string
	ID   string
	Path string

	IsText bool
	Text   string

	Elem     *ttmlInElement
	Parent   *ttmlRuntimeNode
	Children []*ttmlRuntimeNode

	// RawStart/RawEnd are unclipped times relative to the document. They are
	// the origin used to resolve children, even when this node is clipped by an
	// ancestor.
	RawStart  time.Duration
	RawEnd    time.Duration
	HasRawEnd bool

	// Effective times are the intersection of the raw times with every ancestor.
	EffStart  time.Duration
	EffEnd    time.Duration
	HasEffEnd bool

	InlineStyle   *StyleAttributes
	NamedStyle    *Style // effective named style (inherited through containers)
	OwnNamedStyle *Style // named style declared by this element itself
	SpanStyle     *Style // nearest named style declared on a span chain
	Region        *Region

	SpacePreserve bool
}

// ttmlLeaf is a piece of text or a line break active over a time interval.
type ttmlLeaf struct {
	IsBreak bool
	Text    string

	Start  time.Duration
	End    time.Duration
	HasEnd bool

	InlineStyle *StyleAttributes
	Style       *Style
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

	// Validate the document-wide timing features we support.
	timeBase := strings.ToLower(strings.TrimSpace(ttml.TimeBase))
	if timeBase == "" {
		timeBase = "media"
	}
	if timeBase != "media" && timeBase != "smpte" {
		err = fmt.Errorf("astisub: ttml: tt: unsupported timeBase %q", ttml.TimeBase)
		return
	}
	if err = validateTTMLUnsupportedTiming("tt", ttml.TimeContainer, ttml.Repeat, ttml.RepeatCount, ttml.RepeatDur); err != nil {
		return
	}

	var cfg = &ttmlTimingContext{
		framerate: ttml.Framerate,
		tickrate:  ttml.Tickrate,
		timeBase:  timeBase,
	}

	// Build a virtual root. The body is a parallel time container relative to it.
	var root = &ttmlRuntimeNode{
		InlineStyle:   &StyleAttributes{},
		Name:          "tt",
		Path:          "tt",
		SpacePreserve: strings.EqualFold(strings.TrimSpace(ttml.XMLSpace), "preserve"),
	}
	if ttml.Body.XMLName.Local == "" && len(ttml.Body.InnerXML) == 0 {
		return
	}

	var body *ttmlRuntimeNode
	if body, err = buildTTMLRuntimeNode(&ttml.Body, "tt>body", root, cfg, o, "tt>body"); err != nil {
		return
	}

	// Emit one or more static items for every p in document order.
	if body != nil {
		if err = processTTMLContainer(body, o, cfg); err != nil {
			return
		}
	}
	return
}

// parseTTMLFragment parses one level of mixed TTML content. Start elements keep
// their inner XML, so callers can recurse at their own pace; character data is
// returned as text nodes in source order.
func parseTTMLFragment(raw string) (out []*ttmlInElement, err error) {
	d := xml.NewDecoder(strings.NewReader("<astisub-ttml-fragment>" + raw + "</astisub-ttml-fragment>"))
	var first = true
	for {
		var tok xml.Token
		if tok, err = d.Token(); err != nil {
			if err == io.EOF {
				err = nil
				return
			}
			err = fmt.Errorf("astisub: ttml: parsing content failed: %w", err)
			return
		}

		switch t := tok.(type) {
		case xml.StartElement:
			// The first start element is our synthetic wrapper.
			if first {
				first = false
				continue
			}
			var e ttmlInElement
			if err = d.DecodeElement(&e, &t); err != nil {
				err = fmt.Errorf("astisub: ttml: decoding <%s> failed: %w", t.Name.Local, err)
				return
			}
			out = append(out, &e)
		case xml.CharData:
			out = append(out, &ttmlInElement{IsText: true, Text: string(t)})
		}
	}
}

// normalizeTTMLInnerXML reproduces the legacy indentation stripping. It is only
// used when xml:space="preserve" is not in effect.
func normalizeTTMLInnerXML(s string) string {
	lines := strings.Split(s, "\n")
	for i := 0; i < len(lines); i++ {
		lines[i] = strings.TrimLeftFunc(lines[i], unicode.IsSpace)
	}
	return strings.Join(lines, "")
}

// validateTTMLUnsupportedTiming rejects timing features outside the supported
// media time base / parallel containers subset.
func validateTTMLUnsupportedTiming(path, timeContainer, repeat, repeatCount, repeatDur string) error {
	if v := strings.TrimSpace(timeContainer); v != "" && !strings.EqualFold(v, "par") {
		return fmt.Errorf("astisub: ttml: %s: unsupported timeContainer %q", path, v)
	}
	if strings.TrimSpace(repeat) != "" || strings.TrimSpace(repeatCount) != "" || strings.TrimSpace(repeatDur) != "" {
		return fmt.Errorf("astisub: ttml: %s: repeat timing is not supported", path)
	}
	return nil
}

// resolveTTMLDuration resolves a raw duration using the document frame/tick
// rates, rejecting negative values and missing required rates.
func resolveTTMLDuration(d *TTMLInDuration, path string, cfg *ttmlTimingContext) (time.Duration, error) {
	if d == nil {
		return 0, nil
	}
	if d.d < 0 || d.frames < 0 || d.ticks < 0 {
		return 0, fmt.Errorf("astisub: ttml: %s: negative time value %q", path, d.raw)
	}
	if d.frames != 0 && cfg.framerate <= 0 {
		return 0, fmt.Errorf("astisub: ttml: %s: frame-based time %q requires frameRate", path, d.raw)
	}
	if d.ticks != 0 && cfg.tickrate <= 0 {
		return 0, fmt.Errorf("astisub: ttml: %s: tick-based time %q requires tickRate", path, d.raw)
	}
	var c = *d
	c.framerate = cfg.framerate
	c.tickrate = cfg.tickrate
	return c.duration(), nil
}

func ttmlHasTimingAttributes(e *ttmlInElement) bool {
	return e.Begin != nil || e.End != nil || e.Dur != nil
}

// buildTTMLRuntimeNode resolves timing and styles for an element and, for p
// elements, recursively parses the mixed content below it.
func buildTTMLRuntimeNode(e *ttmlInElement, path string, parent *ttmlRuntimeNode, cfg *ttmlTimingContext, o *Subtitles, pathForChildren string) (n *ttmlRuntimeNode, err error) {
	// Character data.
	if e.IsText {
		if e.Text == "" {
			return nil, nil
		}
		n = &ttmlRuntimeNode{
			IsText:        true,
			Text:          e.Text,
			Parent:        parent,
			Path:          path,
			RawStart:      parent.RawStart,
			RawEnd:        parent.RawEnd,
			HasRawEnd:     parent.HasRawEnd,
			EffStart:      parent.EffStart,
			EffEnd:        parent.EffEnd,
			HasEffEnd:     parent.HasEffEnd,
			InlineStyle:   parent.InlineStyle,
			Region:        parent.Region,
			SpacePreserve: parent.SpacePreserve,
		}
		// Direct text of a p belongs to the item's named style. Text inside a
		// span keeps the nearest named style declared on the span chain, which
		// preserves the existing single-reference behaviour.
		if parent.Name != "" && parent.Name != "p" && parent.Name != "body" && parent.Name != "div" {
			n.OwnNamedStyle = parent.SpanStyle
			if n.OwnNamedStyle == nil {
				n.OwnNamedStyle = parent.OwnNamedStyle
			}
		}
		return
	}

	n = &ttmlRuntimeNode{
		Name:   strings.ToLower(e.XMLName.Local),
		ID:     e.ID,
		Elem:   e,
		Parent: parent,
		Path:   path,
	}

	// Reject unsupported timing methods with a node-aware error.
	if err = validateTTMLUnsupportedTiming(path, e.TimeContainer, e.Repeat, e.RepeatCount, e.RepeatDur); err != nil {
		return nil, err
	}

	// This implementation is scoped to media time base relative timing. The
	// legacy p-only absolute timing (used by smpte documents) keeps working.
	if cfg.timeBase != "media" && n.Name != "p" && ttmlHasTimingAttributes(e) {
		return nil, fmt.Errorf("astisub: ttml: %s: timing on <%s> requires timeBase \"media\"", path, n.Name)
	}
	// Mixed text in documents using the nested timing feature is treated as
	// xml:space="preserve" so significant spaces between elements survive.
	if n.Name != "p" && ttmlHasTimingAttributes(e) {
		cfg.nestedTiming = true
	}

	// xml:space is inherited and controls whether indentation is stripped.
	n.SpacePreserve = parent.SpacePreserve
	if v := strings.TrimSpace(e.XMLSpace); v != "" {
		n.SpacePreserve = strings.EqualFold(v, "preserve")
	}

	// Inline style inheritance. The child owns a fresh StyleAttributes so
	// siblings can never mutate a shared parent style.
	var own = e.TTMLInStyleAttributes.styleAttributes()
	own.merge(parent.InlineStyle)
	n.InlineStyle = own

	// Named styles keep a single reference. Containers inherit a style name;
	// spans keep their own declared reference for line items.
	if len(e.Style) > 0 {
		if n.OwnNamedStyle, err = ttmlLookupStyle(o, e.Style, path); err != nil {
			return nil, err
		}
	}
	n.NamedStyle = n.OwnNamedStyle
	if n.NamedStyle == nil {
		n.NamedStyle = parent.NamedStyle
	}

	// Track named styles declared on spans separately so that a p's style is
	// never copied onto line items (which would change legacy output).
	n.SpanStyle = parent.SpanStyle
	if n.Name == "span" && n.OwnNamedStyle != nil {
		n.SpanStyle = n.OwnNamedStyle
	}

	// Regions are inherited from body/div down to p.
	n.Region = parent.Region
	if len(e.Region) > 0 {
		if n.Region, err = ttmlLookupRegion(o, e.Region, path); err != nil {
			return nil, err
		}
	}

	// Resolve timing. begin is relative to the parent's unclipped start;
	// end too. dur starts at this node's own start.
	var begin time.Duration
	if begin, err = resolveTTMLDuration(e.Begin, path, cfg); err != nil {
		return nil, err
	}
	n.RawStart = parent.RawStart + begin

	if e.End != nil {
		var end time.Duration
		if end, err = resolveTTMLDuration(e.End, path, cfg); err != nil {
			return nil, err
		}
		n.RawEnd = parent.RawStart + end
		n.HasRawEnd = true
	}
	if e.Dur != nil {
		var dur time.Duration
		if dur, err = resolveTTMLDuration(e.Dur, path, cfg); err != nil {
			return nil, err
		}
		var durEnd = n.RawStart + dur
		if !n.HasRawEnd || durEnd < n.RawEnd {
			n.RawEnd = durEnd
			n.HasRawEnd = true
		}
	}
	if !n.HasRawEnd {
		n.RawEnd = parent.RawEnd
		n.HasRawEnd = parent.HasRawEnd
	}
	if n.HasRawEnd && n.RawEnd < n.RawStart {
		return nil, fmt.Errorf("astisub: ttml: %s: inverted interval [%s, %s]", path, n.RawStart, n.RawEnd)
	}

	// Intersect with the ancestor effective interval. Clipping never changes
	// the unclipped origin above, which remains the base for children.
	n.EffStart = n.RawStart
	if parent.EffStart > n.EffStart {
		n.EffStart = parent.EffStart
	}
	if n.HasRawEnd {
		n.EffEnd = n.RawEnd
		n.HasEffEnd = true
	}
	if parent.HasEffEnd && (!n.HasEffEnd || parent.EffEnd < n.EffEnd) {
		n.EffEnd = parent.EffEnd
		n.HasEffEnd = true
	}

	// "br" is a leaf. Recurse into everything else.
	if n.Name == "br" {
		return n, nil
	}

	var raw = e.InnerXML
	if n.Name == "p" && !n.SpacePreserve {
		raw = normalizeTTMLInnerXML(raw)
	}

	var fragments []*ttmlInElement
	if fragments, err = parseTTMLFragment(raw); err != nil {
		return nil, err
	}
	var childIndex = 0
	for _, fragment := range fragments {
		if fragment.IsText {
			var child *ttmlRuntimeNode
			if child, err = buildTTMLRuntimeNode(fragment, pathForChildren+">text", n, cfg, o, pathForChildren); err != nil {
				return nil, err
			}
			if child != nil {
				n.Children = append(n.Children, child)
			}
			continue
		}
		childIndex++
		var name = strings.ToLower(fragment.XMLName.Local)
		var childPath = fmt.Sprintf("%s>%s[%d]", pathForChildren, name, childIndex)
		if fragment.ID != "" {
			childPath += fmt.Sprintf("[id=%s]", fragment.ID)
		}
		var child *ttmlRuntimeNode
		if child, err = buildTTMLRuntimeNode(fragment, childPath, n, cfg, o, childPath); err != nil {
			return nil, err
		}
		if child != nil {
			n.Children = append(n.Children, child)
		}
	}
	return n, nil
}

func ttmlLookupStyle(o *Subtitles, id, path string) (*Style, error) {
	var s, ok = o.Styles[id]
	if !ok {
		return nil, fmt.Errorf("astisub: ttml: %s: style %q doesn't exist", path, id)
	}
	return s, nil
}

func ttmlLookupRegion(o *Subtitles, id, path string) (*Region, error) {
	var r, ok = o.Regions[id]
	if !ok {
		return nil, fmt.Errorf("astisub: ttml: %s: region %q doesn't exist", path, id)
	}
	return r, nil
}

// processTTMLContainer walks body/div containers in document order.
func processTTMLContainer(n *ttmlRuntimeNode, o *Subtitles, cfg *ttmlTimingContext) error {
	for _, child := range n.Children {
		if child.IsText {
			continue
		}
		if child.Name == "p" {
			if err := processTTMLParagraph(child, o, cfg); err != nil {
				return err
			}
			continue
		}
		if err := processTTMLContainer(child, o, cfg); err != nil {
			return err
		}
	}
	return nil
}

func collectTTMLLeaves(n *ttmlRuntimeNode, out *[]ttmlLeaf, keepWhitespace bool) {
	if n.IsText {
		var parts = strings.Split(n.Text, "\n")
		for idx, part := range parts {
			if idx > 0 {
				*out = append(*out, ttmlLeaf{
					IsBreak:     true,
					Start:       n.EffStart,
					End:         n.EffEnd,
					HasEnd:      n.HasEffEnd,
					InlineStyle: n.InlineStyle,
				})
			}
			if part == "" {
				continue
			}
			if !keepWhitespace && strings.TrimSpace(part) == "" {
				continue
			}
			*out = append(*out, ttmlLeaf{
				Text:        part,
				Start:       n.EffStart,
				End:         n.EffEnd,
				HasEnd:      n.HasEffEnd,
				InlineStyle: n.InlineStyle,
				Style:       n.OwnNamedStyle,
			})
		}
		return
	}
	if n.Name == "br" {
		*out = append(*out, ttmlLeaf{
			IsBreak:     true,
			Start:       n.EffStart,
			End:         n.EffEnd,
			HasEnd:      n.HasEffEnd,
			InlineStyle: n.InlineStyle,
		})
		return
	}
	for _, child := range n.Children {
		collectTTMLLeaves(child, out, keepWhitespace)
	}
}

func cloneTTMLStyleAttributes(s *StyleAttributes) *StyleAttributes {
	if s == nil {
		return nil
	}
	var c = *s
	return &c
}

func ttmlStyleID(s *Style) string {
	if s == nil {
		return ""
	}
	return s.ID
}

func ttmlRegionID(r *Region) string {
	if r == nil {
		return ""
	}
	return r.ID
}

func ttmlLineItemEqual(a, b LineItem) bool {
	if a.Text != b.Text || a.StartAt != b.StartAt {
		return false
	}
	if ttmlStyleID(a.Style) != ttmlStyleID(b.Style) {
		return false
	}
	return reflect.DeepEqual(a.InlineStyle, b.InlineStyle)
}

func ttmlItemContentEqual(a, b *Item) bool {
	if ttmlRegionID(a.Region) != ttmlRegionID(b.Region) || ttmlStyleID(a.Style) != ttmlStyleID(b.Style) {
		return false
	}
	if !reflect.DeepEqual(a.InlineStyle, b.InlineStyle) {
		return false
	}
	if len(a.Lines) != len(b.Lines) {
		return false
	}
	for idx := range a.Lines {
		if a.Lines[idx].VoiceName != b.Lines[idx].VoiceName || len(a.Lines[idx].Items) != len(b.Lines[idx].Items) {
			return false
		}
		for j := range a.Lines[idx].Items {
			if !ttmlLineItemEqual(a.Lines[idx].Items[j], b.Lines[idx].Items[j]) {
				return false
			}
		}
	}
	return true
}

// processTTMLParagraph splits one p into half-open intervals at every effective
// text change point and emits an item per interval.
func processTTMLParagraph(p *ttmlRuntimeNode, o *Subtitles, cfg *ttmlTimingContext) error {
	// Existing non-timed nested documents keep the legacy behaviour of dropping
	// whitespace-only text nodes. Documents that use nested timing (the newly
	// supported inputs) treat mixed text as xml:space="preserve".
	var keepWhitespace = p.SpacePreserve || cfg.nestedTiming

	var leaves []ttmlLeaf
	collectTTMLLeaves(p, &leaves, keepWhitespace)

	// Determine whether the paragraph actually carries text.
	var hasText bool
	for _, leaf := range leaves {
		if leaf.IsBreak || leaf.Text == "" {
			continue
		}
		if !keepWhitespace && strings.TrimSpace(leaf.Text) == "" {
			continue
		}
		hasText = true
		break
	}
	if !hasText {
		return nil
	}

	// Every text-bearing p must end at a finite time after intersection.
	if !p.HasEffEnd {
		return fmt.Errorf("astisub: ttml: %s: paragraph containing text has no finite end", p.Path)
	}
	if p.EffEnd <= p.EffStart {
		return nil
	}

	// Collect every change point, including breaks because a line break
	// appearing or disappearing changes the rendered lines.
	var points = []time.Duration{p.EffStart, p.EffEnd}
	for _, leaf := range leaves {
		if !leaf.HasEnd || leaf.End <= leaf.Start {
			continue
		}
		var start, end = leaf.Start, leaf.End
		if start < p.EffStart {
			start = p.EffStart
		}
		if end > p.EffEnd {
			end = p.EffEnd
		}
		if end <= start {
			continue
		}
		points = append(points, start, end)
	}
	sort.Slice(points, func(i, j int) bool { return points[i] < points[j] })

	var items []*Item
	for idx := 0; idx+1 < len(points); idx++ {
		var start, end = points[idx], points[idx+1]
		if end <= start {
			continue
		}

		var lines []Line
		var line = Line{}
		var intervalHasText bool
		for _, leaf := range leaves {
			if !leaf.HasEnd {
				continue
			}
			var leafStart, leafEnd = leaf.Start, leaf.End
			if leafStart < p.EffStart {
				leafStart = p.EffStart
			}
			if leafEnd > p.EffEnd {
				leafEnd = p.EffEnd
			}
			if leafStart > start || leafEnd < end {
				continue
			}
			if leaf.IsBreak {
				lines = append(lines, line)
				line = Line{}
				continue
			}
			if leaf.Text == "" {
				continue
			}
			line.Items = append(line.Items, LineItem{
				InlineStyle: cloneTTMLStyleAttributes(leaf.InlineStyle),
				Style:       leaf.Style,
				Text:        leaf.Text,
			})
			intervalHasText = true
		}
		lines = append(lines, line)

		// Content without any text (for instance a lone br) is not emitted.
		if !intervalHasText {
			continue
		}

		var item = &Item{
			StartAt:     start,
			EndAt:       end,
			InlineStyle: cloneTTMLStyleAttributes(p.InlineStyle),
			Lines:       lines,
			Region:      p.Region,
			Style:       p.NamedStyle,
		}

		// Merge fragments whose text, line breaks, effective styles and
		// region are identical.
		if len(items) > 0 && items[len(items)-1].EndAt == item.StartAt && ttmlItemContentEqual(items[len(items)-1], item) {
			items[len(items)-1].EndAt = item.EndAt
			continue
		}
		items = append(items, item)
	}

	o.Items = append(o.Items, items...)
	return nil
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
	Text  string `xml:",chardata"`
	TTMLOutStyleAttributes
	XMLName xml.Name
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
			// Loop through line items
			for _, lineItem := range line.Items {
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
