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
	Items  string `xml:",innerxml"`
	Region string `xml:"region,attr,omitempty"`
	Style  string `xml:"style,attr,omitempty"`
	TTMLInStyleAttributes
}

// TTMLInItems represents input TTML items
type TTMLInItems []TTMLInItem

// UnmarshalXML implements the XML unmarshaler interface
func (i *TTMLInItems) UnmarshalXML(d *xml.Decoder, start xml.StartElement) (err error) {
	// Get next tokens
	var t xml.Token
	for {
		// Get next token
		if t, err = d.Token(); err != nil {
			if err == io.EOF {
				break
			}
			err = fmt.Errorf("astisub: getting next token failed: %w", err)
			return
		}

		// Start element
		if se, ok := t.(xml.StartElement); ok {
			var e = TTMLInItem{}
			if err = d.DecodeElement(&e, &se); err != nil {
				err = fmt.Errorf("astisub: decoding xml.StartElement failed: %w", err)
				return
			}
			*i = append(*i, e)
		} else if b, ok := t.(xml.CharData); ok {
			if str := string(b); len(strings.TrimSpace(str)) > 0 {
				*i = append(*i, TTMLInItem{Text: str})
			}
		}
	}
	return nil
}

type ttmlXmlTokenReader struct {
	xmlTokenReader xml.TokenReader
	holdingToken   xml.Token
}

// Token implements the TokenReader interface, when it meets the "br" tag, it will hold the token and return a newline
// instead. This is to work around the fact that the go xml unmarshaler will ignore the "br" tag if it's within a
// character data field.
func (r *ttmlXmlTokenReader) Token() (xml.Token, error) {
	if r.holdingToken != nil {
		returnToken := r.holdingToken
		r.holdingToken = nil
		return returnToken, nil
	}

	t, err := r.xmlTokenReader.Token()
	if err != nil {
		return nil, err
	}

	if se, ok := t.(xml.StartElement); ok && strings.ToLower(se.Name.Local) == "br" {
		r.holdingToken = t
		return xml.CharData("\n"), nil
	}

	return t, nil
}

func newTTMLXmlDecoder(s string) *xml.Decoder {
	return xml.NewTokenDecoder(
		&ttmlXmlTokenReader{
			xmlTokenReader: xml.NewDecoder(strings.NewReader("<p>" + s + "</p>")),
			holdingToken:   nil,
		},
	)
}

// TTMLInItem represents an input TTML item
type TTMLInItem struct {
	Style string `xml:"style,attr,omitempty"`
	Text  string `xml:",chardata"`
	TTMLInStyleAttributes
	XMLName xml.Name
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

	// Loop through styles. The raw "style" attributes are kept verbatim: a
	// single reference is still represented through Style.Style, while several
	// space-separated references go through Style.StyleReferences.
	var rawStyleRefs = make(map[string][]string)
	for _, ts := range ttml.Styles {
		var s = &Style{
			ID:          ts.ID,
			InlineStyle: ts.TTMLInStyleAttributes.styleAttributes(),
		}
		o.Styles[s.ID] = s
		rawStyleRefs[s.ID] = ttmlStyleRefIDs(ts.Style)
	}

	// Resolve the whole style graph up front. Missing references and cycles
	// must be reported even when the faulty style is not referenced by any cue,
	// and diamond sharing resolves each style only once.
	var resolver = &ttmlStyleResolver{
		styles:    o.Styles,
		rawRefs:   rawStyleRefs,
		effective: make(map[string]*StyleAttributes),
	}
	for _, ts := range ttml.Styles {
		if _, err = resolver.resolve(ts.ID, nil); err != nil {
			return
		}
	}

	// Preserve the reference topology on the public Style objects.
	for _, ts := range ttml.Styles {
		ids := rawStyleRefs[ts.ID]
		s := o.Styles[ts.ID]
		if len(ids) == 1 {
			s.Style = o.Styles[ids[0]]
		} else if len(ids) > 1 {
			refs := newTTMLStyleReferences(ids)
			refs.link(o.Styles)
			s.StyleReferences = refs
		}
	}

	// Synthetic styles materializing multi-reference lists are shared between
	// nodes through this cache and never emitted back to TTML.
	var mergedStyleCache = make(map[string]*Style)

	// resolveNodeStyle maps a node's raw "style" attribute to the style it
	// should point at. A single reference resolves to the declared style as
	// before unless that style itself combines several references, in which
	// case the composed effective attributes are materialized into a shared
	// synthetic style; several references resolve the same way directly.
	resolveNodeStyle := func(raw, owner string) (*Style, error) {
		ids := ttmlStyleRefIDs(raw)
		if len(ids) == 0 {
			return nil, nil
		}
		for _, id := range ids {
			if _, ok := o.Styles[id]; !ok {
				return nil, fmt.Errorf("astisub: Style %s requested by %s doesn't exist", id, owner)
			}
		}
		if len(ids) == 1 {
			declared := o.Styles[ids[0]]
			if declared.StyleReferences == nil {
				return declared, nil
			}
			// The declared style itself combines references: reuse its already
			// resolved effective attributes, which include its own tts attrs.
			return ttmlMergedStyle(o.Styles, mergedStyleCache, raw, rawStyleRefs[ids[0]], ids[0], resolver.effective[ids[0]]), nil
		}
		effective := resolver.compose(ids, &StyleAttributes{})
		refAttribute := strings.Join(ids, " ")
		return ttmlMergedStyle(o.Styles, mergedStyleCache, refAttribute, ids, refAttribute, effective), nil
	}

	// Loop through regions
	for _, tr := range ttml.Regions {
		var r = &Region{
			ID:          tr.ID,
			InlineStyle: tr.TTMLInStyleAttributes.styleAttributes(),
		}
		if len(tr.Style) > 0 {
			style, e := resolveNodeStyle(tr.Style, "region "+r.ID)
			if e != nil {
				err = e
				return
			}
			// The synthetic style of a multi-reference region carries the
			// composed effective attributes; the region keeps only its own tts
			// attributes, exactly like the single-reference case.
			r.Style = style
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
				style, e := resolveNodeStyle(ts.Style, fmt.Sprintf("subtitle between %s and %s", s.StartAt, s.EndAt))
				if e != nil {
					err = e
					return
				}
				s.Style = style
			}

			// Remove items identation
			lines := strings.Split(ts.Items, "\n")
			for i := 0; i < len(lines); i++ {
				lines[i] = strings.TrimLeftFunc(lines[i], unicode.IsSpace)
			}

			// Unmarshal items
			var items = TTMLInItems{}
			if err = newTTMLXmlDecoder(strings.Join(lines, "")).Decode(&items); err != nil {
				err = fmt.Errorf("astisub: unmarshaling items failed: %w", err)
				return
			}

			// Loop through texts
			var l = &Line{}
			for _, tt := range items {
				// New line specified with the "br" tag
				if strings.ToLower(tt.XMLName.Local) == "br" {
					s.Lines = append(s.Lines, *l)
					l = &Line{}
					continue
				}

				// New line decoded as a line break. This can happen if there's a "br" tag within the text since
				// since the go xml unmarshaler will unmarshal a "br" tag as a line break if the field has the
				// chardata xml tag.
				for idx, li := range strings.Split(tt.Text, "\n") {
					// New line
					if idx > 0 {
						s.Lines = append(s.Lines, *l)
						l = &Line{}
					}

					// Init line item
					var t = LineItem{
						InlineStyle: tt.TTMLInStyleAttributes.styleAttributes(),
						Text:        li,
					}

					// Add style
					if len(tt.Style) > 0 {
						style, e := resolveNodeStyle(tt.Style, "item with text "+tt.Text)
						if e != nil {
							err = e
							return
						}
						t.Style = style
					}

					// Append items
					l.Items = append(l.Items, t)
				}

			}
			s.Lines = append(s.Lines, *l)

			// Append subtitle
			o.Items = append(o.Items, s)
		}
	}
	return
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
			ttmlRegion.Style = ttmlStyleRefAttribute(s.Regions[id].Style, s.Regions[id].Style.StyleReferences)
		}
		ttml.Regions = append(ttml.Regions, ttmlRegion)
	}

	// Add styles. Synthetic styles only exist to carry composed multi-reference
	// attributes on nodes; they are not part of the document and are skipped.
	k = []string{}
	for _, style := range s.Styles {
		if style.ttmlSynthetic {
			continue
		}
		k = append(k, style.ID)
	}
	sort.Strings(k)
	for _, id := range k {
		var ttmlStyle = TTMLOutStyle{TTMLOutHeader: TTMLOutHeader{
			ID:                     s.Styles[id].ID,
			TTMLOutStyleAttributes: ttmlOutStyleAttributesFromStyleAttributes(s.Styles[id].InlineStyle),
		}}
		if s.Styles[id].Style != nil || s.Styles[id].StyleReferences != nil {
			ttmlStyle.Style = ttmlStyleRefAttribute(s.Styles[id].Style, s.Styles[id].StyleReferences)
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
			ttmlSubtitle.Style = ttmlStyleRefAttribute(item.Style, item.Style.StyleReferences)
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
					ttmlItem.Style = ttmlStyleRefAttribute(lineItem.Style, lineItem.Style.StyleReferences)
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
