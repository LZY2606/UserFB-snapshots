package astisub

import (
	"encoding/xml"
	"fmt"
	"io"
	"strconv"
	"strings"
	"unicode"
)

// ttmlNamespaceTTS is the TTML styling namespace that carries the tts:ruby,
// tts:color and related attributes.
const ttmlNamespaceTTS = "http://www.w3.org/ns/ttml#styling"

// TTML ruby roles
const (
	ttmlRubyRoleContainer     = "container"
	ttmlRubyRoleBase          = "base"
	ttmlRubyRoleText          = "text"
	ttmlRubyRoleBaseContainer = "basecontainer"
	ttmlRubyRoleTextContainer = "textcontainer"
)

// Canonical spelling used when writing grouped ruby containers. Role matching
// is case-insensitive, so both spellings are accepted on input.
const (
	ttmlOutRubyBaseContainer = "baseContainer"
	ttmlOutRubyTextContainer = "textContainer"
)

// ttmlNode is a minimal ordered XML tree. Text and child nodes are kept in a
// single ordered list so mixed content (text interleaved with spans) can be
// processed in document order.
type ttmlNode struct {
	Name    xml.Name
	Attr    []xml.Attr
	Content []ttmlNodeContent
}

type ttmlNodeContent struct {
	Text string
	Node *ttmlNode
}

// parseTTMLNodes parses an XML fragment into an ordered list of content.
// Namespace prefixes that are not declared inside the fragment are reported by
// encoding/xml as the prefix itself (e.g. "tts"), which is why attribute
// lookups below match on the local name.
func parseTTMLNodes(i string) (o []ttmlNodeContent, err error) {
	var root = &ttmlNode{Name: xml.Name{Local: "root"}}
	d := xml.NewDecoder(strings.NewReader("<root>" + i + "</root>"))
	stack := []*ttmlNode{root}
	var wrapperClosed bool
	for {
		var t xml.Token
		if t, err = d.Token(); err != nil {
			if err == io.EOF {
				err = nil
				break
			}
			return
		}
		switch v := t.(type) {
		case xml.StartElement:
			// The first start element is the synthetic wrapper itself.
			if len(stack) == 1 && !wrapperClosed {
				wrapperClosed = true
				continue
			}
			var n = &ttmlNode{Name: v.Name, Attr: v.Attr}
			var parent = stack[len(stack)-1]
			parent.Content = append(parent.Content, ttmlNodeContent{Node: n})
			stack = append(stack, n)
		case xml.EndElement:
			if len(stack) > 1 {
				stack = stack[:len(stack)-1]
			}
		case xml.CharData:
			var parent = stack[len(stack)-1]
			parent.Content = append(parent.Content, ttmlNodeContent{Text: string(v)})
		}
	}
	return root.Content, nil
}

// ttmlRubyRole returns the normalized ruby role carried by a node, or an empty
// string when the node has no tts:ruby attribute.
func ttmlRubyRole(n *ttmlNode) string {
	if n == nil {
		return ""
	}
	for _, a := range n.Attr {
		if !strings.EqualFold(a.Name.Local, "ruby") {
			continue
		}
		switch a.Name.Space {
		case "", "tts", "tt", ttmlNamespaceTTS, "http://www.w3.org/2006/10/ttaf1#styling":
		default:
			continue
		}
		return strings.ToLower(strings.TrimSpace(a.Value))
	}
	return ""
}

// ttmlSpace returns the xml:space value carried by a node, if any.
func ttmlSpace(n *ttmlNode) (string, bool) {
	if n == nil {
		return "", false
	}
	for _, a := range n.Attr {
		if strings.EqualFold(a.Name.Local, "space") {
			return strings.ToLower(strings.TrimSpace(a.Value)), true
		}
	}
	return "", false
}

// ttmlNodeContainsRuby reports whether n or one of its descendants carries a
// ruby role.
func ttmlNodeContainsRuby(n *ttmlNode) bool {
	if n == nil {
		return false
	}
	if ttmlRubyRole(n) != "" {
		return true
	}
	for _, c := range n.Content {
		if c.Node != nil && ttmlNodeContainsRuby(c.Node) {
			return true
		}
	}
	return false
}

// ttmlStyleAttributesFromNode converts a node's styling attributes into a
// TTMLInStyleAttributes, matching on local names so that both bound and
// unbound namespace prefixes are supported.
func ttmlStyleAttributesFromNode(n *ttmlNode) TTMLInStyleAttributes {
	var o TTMLInStyleAttributes
	if n == nil {
		return o
	}
	for _, a := range n.Attr {
		var v = a.Value
		switch a.Name.Local {
		case "backgroundColor":
			o.BackgroundColor = &v
		case "color":
			o.Color = &v
		case "direction":
			o.Direction = &v
		case "display":
			o.Display = &v
		case "displayAlign":
			o.DisplayAlign = &v
		case "extent":
			o.Extent = &v
		case "fontFamily":
			o.FontFamily = &v
		case "fontSize":
			o.FontSize = &v
		case "fontStyle":
			o.FontStyle = &v
		case "fontWeight":
			o.FontWeight = &v
		case "lineHeight":
			o.LineHeight = &v
		case "opacity":
			o.Opacity = &v
		case "origin":
			o.Origin = &v
		case "overflow":
			o.Overflow = &v
		case "padding":
			o.Padding = &v
		case "showBackground":
			o.ShowBackground = &v
		case "textAlign":
			o.TextAlign = &v
		case "textDecoration":
			o.TextDecoration = &v
		case "textOutline":
			o.TextOutline = &v
		case "unicodeBidi":
			o.UnicodeBidi = &v
		case "visibility":
			o.Visibility = &v
		case "wrapOption":
			o.WrapOption = &v
		case "writingMode":
			o.WritingMode = &v
		case "zIndex":
			if i, err := strconv.Atoi(strings.TrimSpace(v)); err == nil {
				o.ZIndex = &i
			}
		}
	}
	return o
}

// ttmlNodeStyle converts a node's styling attributes into a StyleAttributes.
func ttmlNodeStyle(n *ttmlNode) *StyleAttributes {
	return ttmlStyleAttributesFromNode(n).styleAttributes()
}

// cloneStyleAttributes copies a StyleAttributes value so that editing one ruby
// run's color does not mutate its siblings. The struct holds pointers for a
// number of scalar fields; assigning a new color replaces the pointer, so a
// shallow copy is enough.
func cloneStyleAttributes(s *StyleAttributes) *StyleAttributes {
	if s == nil {
		return nil
	}
	var c = *s
	return &c
}

// mergeStyleAttributes returns own merged with parent (own wins) without
// mutating either argument. It returns a non-nil value whenever either input
// is non-nil so ruby runs are always editable through InlineStyle.
func mergeStyleAttributes(own, parent *StyleAttributes) *StyleAttributes {
	if own == nil {
		return cloneStyleAttributes(parent)
	}
	var c = cloneStyleAttributes(own)
	c.merge(parent)
	return c
}

// ttmlNodeStyleID returns the style reference carried by a node, if any.
func ttmlNodeStyleID(n *ttmlNode) string {
	for _, a := range n.Attr {
		if a.Name.Local == "style" && a.Name.Space == "" {
			return a.Value
		}
	}
	return ""
}

// normalizeTTMLText turns the character data of one XML text node into the
// text that must be kept. Pretty-printing newlines and the indentation that
// follows them are layout, never text, so each line after the first loses its
// leading indentation. Spaces inside a line are always kept: xml:space
// governs them. A node made only of spaces is indentation and is dropped.
func normalizeTTMLText(text string, preserve bool) (out string, ok bool) {
	if text == "" {
		return "", false
	}
	var sb strings.Builder
	for idx, line := range strings.Split(strings.ReplaceAll(text, "\r\n", "\n"), "\n") {
		if idx > 0 {
			line = strings.TrimLeftFunc(line, unicode.IsSpace)
		}
		sb.WriteString(line)
	}
	out = sb.String()
	if strings.TrimSpace(out) == "" {
		// Indentation. A run of spaces without a newline is real text only
		// when xml:space="preserve" asks for it.
		if preserve && !strings.ContainsAny(text, "\r\n") {
			return text, true
		}
		return "", false
	}
	return out, true
}

// ttmlItemParser turns a TTML <p> inner XML fragment into Lines. It is also
// used for the individual base/text sides of ruby annotations.
type ttmlItemParser struct {
	styles map[string]*Style
	err    error
}

// ttmlSpaceValue interprets an xml:space attribute value.
func ttmlSpaceValue(v *string) (string, bool) {
	if v == nil {
		return "", false
	}
	return strings.ToLower(strings.TrimSpace(*v)), true
}

// lookupStyle resolves a style reference, recording a parser error when it
// does not exist (the same validation the historical item decoder performed).
func (p *ttmlItemParser) lookupStyle(id, context string) *Style {
	if id == "" {
		return nil
	}
	s, ok := p.styles[id]
	if !ok {
		if p.err == nil {
			p.err = fmt.Errorf("astisub: Style %s requested by item %s doesn't exist", id, context)
		}
		return nil
	}
	return s
}

// parseInnerXML parses a <p> inner XML fragment into lines, preserving ruby
// annotations and line breaks.
func (p *ttmlItemParser) parseInnerXML(inner string, preserve bool) (lines []Line, err error) {
	content, err := parseTTMLNodes(inner)
	if err != nil {
		return
	}
	var b = &ttmlLineBuilder{}
	p.walkContent(content, preserve, &StyleAttributes{}, b)
	b.lines = append(b.lines, b.cur)
	return b.lines, nil
}

type ttmlLineBuilder struct {
	cur   Line
	lines []Line
}

func (b *ttmlLineBuilder) addLine() {
	b.lines = append(b.lines, b.cur)
	b.cur = Line{}
}

func (b *ttmlLineBuilder) addItem(i LineItem) {
	b.cur.Items = append(b.cur.Items, i)
}

// walkContent walks an ordered content list and appends lines and items to b.
func (p *ttmlItemParser) walkContent(content []ttmlNodeContent, preserve bool, inherited *StyleAttributes, b *ttmlLineBuilder) {
	for _, c := range content {
		if c.Node == nil {
			p.walkText(c.Text, preserve, inherited, b)
			continue
		}

		var n = c.Node
		if strings.EqualFold(n.Name.Local, "br") {
			b.addLine()
			continue
		}

		var nodePreserve = preserve
		if v, ok := ttmlSpace(n); ok {
			nodePreserve = v == "preserve"
		}

		switch ttmlRubyRole(n) {
		case ttmlRubyRoleContainer:
			if r := p.parseRubyContainer(n, inherited, nodePreserve); r != nil && len(r.Pairs) > 0 {
				b.addItem(LineItem{Ruby: r})
			} else {
				// A container without any pair: keep its content as normal text
				// so nothing is silently dropped.
				p.walkContent(n.Content, nodePreserve, mergeStyleAttributes(ttmlNodeStyle(n), inherited), b)
			}
		case ttmlRubyRoleBase, ttmlRubyRoleText,
			ttmlRubyRoleBaseContainer, ttmlRubyRoleTextContainer:
			// A stray ruby role outside a container (e.g. a style-only
			// reference): render its content as ordinary text.
			p.walkContent(n.Content, nodePreserve, mergeStyleAttributes(ttmlNodeStyle(n), inherited), b)
		default:
			if ttmlNodeContainsRuby(n) {
				// A wrapper span (typically carrying styling) around ruby
				// containers: descend so the ruby survives, inheriting the
				// wrapper's inline color.
				p.walkContent(n.Content, nodePreserve, mergeStyleAttributes(ttmlNodeStyle(n), inherited), b)
				continue
			}
			p.walkLeaf(n, nodePreserve, inherited, b)
		}
	}
}

// walkText appends a text chunk. A text node's leading spaces belong to the
// text (it may follow a sibling element), but a node that is only indentation
// must not become an empty line item.
func (p *ttmlItemParser) walkText(text string, preserve bool, inherited *StyleAttributes, b *ttmlLineBuilder) {
	if t, ok := normalizeTTMLText(text, preserve); ok {
		b.addItem(LineItem{
			InlineStyle: cloneStyleAttributes(inherited),
			Text:        t,
		})
	}
}

// walkLeaf handles an element without any ruby descendant. Nested non-ruby
// elements are flattened in place (a <br> becomes a newline), which is what
// the historical decoder did, and every element contributes at least one item
// even when it holds no text.
func (p *ttmlItemParser) walkLeaf(n *ttmlNode, preserve bool, inherited *StyleAttributes, b *ttmlLineBuilder) {
	var sa = mergeStyleAttributes(ttmlNodeStyle(n), inherited)
	var style = p.lookupStyle(ttmlNodeStyleID(n), fmt.Sprintf("with text %s", ttmlLeafText(n)))

	var text = ttmlLeafText(n)
	for idx, part := range strings.Split(strings.ReplaceAll(text, "\r\n", "\n"), "\n") {
		if idx > 0 {
			b.addLine()
			// Indentation after a line break is layout, never text.
			part = strings.TrimLeftFunc(part, unicode.IsSpace)
		}
		b.addItem(LineItem{
			InlineStyle: cloneStyleAttributes(sa),
			Style:       style,
			Text:        part,
		})
	}
}

// ttmlLeafText flattens the content of a non-ruby element into a single
// string: direct and nested text are concatenated and <br> becomes a newline.
func ttmlLeafText(n *ttmlNode) string {
	var sb strings.Builder
	for _, c := range n.Content {
		if c.Node == nil {
			sb.WriteString(c.Text)
			continue
		}
		if strings.EqualFold(c.Node.Name.Local, "br") {
			sb.WriteString("\n")
			continue
		}
		sb.WriteString(ttmlLeafText(c.Node))
	}
	return sb.String()
}

// parseRubyContainer turns a tts:ruby="container" node into a Ruby. Both the
// simple structure (one base, one text) and the grouped structure
// (baseContainer followed by textContainer) are supported. Pairs are matched
// by position and splitting a side into several styled runs never changes the
// number of pairs.
func (p *ttmlItemParser) parseRubyContainer(n *ttmlNode, inherited *StyleAttributes, preserve bool) *Ruby {
	var containerStyle = mergeStyleAttributes(ttmlNodeStyle(n), inherited)
	var bases, texts []*ttmlNode
	var baseAncestor, textAncestor *StyleAttributes

	for _, c := range n.Content {
		if c.Node == nil {
			continue
		}
		switch ttmlRubyRole(c.Node) {
		case ttmlRubyRoleBaseContainer:
			for _, cc := range c.Node.Content {
				if cc.Node != nil && ttmlRubyRole(cc.Node) == ttmlRubyRoleBase {
					bases = append(bases, cc.Node)
				}
			}
			baseAncestor = mergeStyleAttributes(ttmlNodeStyle(c.Node), containerStyle)
		case ttmlRubyRoleTextContainer:
			for _, cc := range c.Node.Content {
				if cc.Node != nil && ttmlRubyRole(cc.Node) == ttmlRubyRoleText {
					texts = append(texts, cc.Node)
				}
			}
			textAncestor = mergeStyleAttributes(ttmlNodeStyle(c.Node), containerStyle)
		case ttmlRubyRoleBase:
			bases = append(bases, c.Node)
		case ttmlRubyRoleText:
			texts = append(texts, c.Node)
		}
	}

	var count = len(bases)
	if len(texts) < count {
		count = len(texts)
	}
	if count == 0 {
		return nil
	}

	var r = &Ruby{}
	for i := 0; i < count; i++ {
		var baseInherited = containerStyle
		if baseAncestor != nil {
			baseInherited = baseAncestor
		}
		var textInherited = containerStyle
		if textAncestor != nil {
			textInherited = textAncestor
		}
		r.Pairs = append(r.Pairs, RubyPair{
			Base: p.parseRubySide(bases[i], baseInherited, preserve),
			Text: p.parseRubySide(texts[i], textInherited, preserve),
		})
	}
	return r
}

// parseRubySide turns one base or text element into its styled runs, merging
// inline colors from the outside in (child overrides parent).
func (p *ttmlItemParser) parseRubySide(n *ttmlNode, inherited *StyleAttributes, preserve bool) []LineItem {
	var style = mergeStyleAttributes(ttmlNodeStyle(n), inherited)
	var runs []LineItem
	p.collectRubyRuns(n.Content, style, preserve, &runs)
	return runs
}

func (p *ttmlItemParser) collectRubyRuns(content []ttmlNodeContent, style *StyleAttributes, preserve bool, runs *[]LineItem) {
	for _, c := range content {
		if c.Node == nil {
			if c.Text == "" {
				continue
			}
			var t string
			var ok bool
			if t, ok = normalizeTTMLText(c.Text, preserve); !ok {
				continue
			}
			*runs = append(*runs, LineItem{
				InlineStyle: cloneStyleAttributes(style),
				Text:        t,
			})
			continue
		}

		var n = c.Node
		// Line breaks inside a ruby group are out of scope; drop them rather
		// than polluting the base/annotation text.
		if strings.EqualFold(n.Name.Local, "br") {
			continue
		}
		// A nested span without a ruby role: recurse with the child's inline
		// style overriding the parent's.
		var childPreserve = preserve
		if v, ok := ttmlSpace(n); ok {
			childPreserve = v == "preserve"
		}
		var childStyle = mergeStyleAttributes(ttmlNodeStyle(n), style)
		p.collectRubyRuns(n.Content, childStyle, childPreserve, runs)
	}
}

// ttmlOutRubyItem builds the output TTML item for a ruby LineItem.
//
// One pair is normalized to a simple container (container > base + text).
// Several pairs use a grouped container (container > baseContainer +
// textContainer) so that the base/text grouping is explicit and pairs are
// never interleaved under a simple container.
func ttmlOutRubyItem(li LineItem) TTMLOutItem {
	var container = TTMLOutItem{XMLName: xml.Name{Local: "span"}, Ruby: ttmlRubyRoleContainer, Space: "preserve"}
	if len(li.Ruby.Pairs) == 1 {
		var pair = li.Ruby.Pairs[0]
		container.Children = append(container.Children,
			TTMLOutItem{
				XMLName:  xml.Name{Local: "span"},
				Ruby:     ttmlRubyRoleBase,
				Children: ttmlOutRubyRuns(pair.Base),
			},
			TTMLOutItem{
				XMLName:  xml.Name{Local: "span"},
				Ruby:     ttmlRubyRoleText,
				Children: ttmlOutRubyRuns(pair.Text),
			},
		)
		return container
	}

	var baseContainer = TTMLOutItem{XMLName: xml.Name{Local: "span"}, Ruby: ttmlOutRubyBaseContainer}
	var textContainer = TTMLOutItem{XMLName: xml.Name{Local: "span"}, Ruby: ttmlOutRubyTextContainer}
	for _, pair := range li.Ruby.Pairs {
		baseContainer.Children = append(baseContainer.Children, TTMLOutItem{
			XMLName:  xml.Name{Local: "span"},
			Ruby:     ttmlRubyRoleBase,
			Children: ttmlOutRubyRuns(pair.Base),
		})
		textContainer.Children = append(textContainer.Children, TTMLOutItem{
			XMLName:  xml.Name{Local: "span"},
			Ruby:     ttmlRubyRoleText,
			Children: ttmlOutRubyRuns(pair.Text),
		})
	}
	container.Children = append(container.Children, baseContainer, textContainer)
	return container
}

func ttmlOutRubyRuns(runs []LineItem) []TTMLOutItem {
	var out []TTMLOutItem
	for _, run := range runs {
		var item = TTMLOutItem{
			XMLName:                xml.Name{Local: "span"},
			Text:                   run.Text,
			TTMLOutStyleAttributes: ttmlOutStyleAttributesFromStyleAttributes(run.InlineStyle),
		}
		if run.Style != nil {
			item.Style = run.Style.ID
		}
		out = append(out, item)
	}
	return out
}

// webVTTRunStyle builds the style of one base or annotation run from the
// currently open WebVTT tags. ruby/rt are structural and are not styling tags,
// while <c> classes contribute colors.
func webVTTRunStyle(tags []WebVTTTag) *StyleAttributes {
	var styling []WebVTTTag
	for _, t := range tags {
		if t.Name == "ruby" || t.Name == "rt" {
			continue
		}
		styling = append(styling, t)
	}
	var sa = &StyleAttributes{WebVTTTags: styling}
	sa.propagateWebVTTAttributes()
	return sa
}

// rubyRunWebVTTBytes writes one base or annotation run of a ruby annotation,
// using TTMLColor as the source of truth so that color edits are exported.
// When the color cannot be expressed as a WebVTT class, the original class
// recorded while parsing is reused.
func rubyRunWebVTTBytes(li LineItem) (c []byte) {
	var color string
	var tags []WebVTTTag
	var originalColor string
	if li.InlineStyle != nil {
		if li.InlineStyle.TTMLColor != nil {
			color = li.InlineStyle.TTMLColor.webVTTColorClass()
		}
		for _, t := range li.InlineStyle.WebVTTTags {
			switch t.Name {
			case "c":
				if originalColor == "" && len(t.Classes) > 0 {
					for _, class := range t.Classes {
						if !strings.HasPrefix(class, "bg_") {
							originalColor = class
							break
						}
					}
				}
				continue
			case "ruby", "rt":
				continue
			}
			tags = append(tags, t)
		}
	}
	if li.InlineStyle == nil || li.InlineStyle.TTMLColor == nil {
		// No modeled color: reuse the class the input used, if any, so an
		// unrepresentable value is not silently dropped.
		color = originalColor
	}

	if color != "" {
		c = append(c, []byte("<c."+color+">")...)
	}
	for _, t := range tags {
		c = append(c, []byte(t.startTag())...)
	}
	c = append(c, []byte(escapeHTML(li.Text))...)
	for i := len(tags) - 1; i >= 0; i-- {
		c = append(c, []byte(tags[i].endTag())...)
	}
	if color != "" {
		c = append(c, []byte("</c>")...)
	}
	return
}

// webVTTRubyBytes writes a whole ruby annotation. Each pair contributes its
// base runs followed by an <rt> holding its annotation runs, which is exactly
// the form the WebVTT parser understands.
func webVTTRubyBytes(r *Ruby) (c []byte) {
	if r == nil {
		return
	}
	c = append(c, []byte("<ruby>")...)
	for _, pair := range r.Pairs {
		for _, run := range pair.Base {
			c = append(c, rubyRunWebVTTBytes(run)...)
		}
		c = append(c, []byte("<rt>")...)
		for _, run := range pair.Text {
			c = append(c, rubyRunWebVTTBytes(run)...)
		}
		c = append(c, []byte("</rt>")...)
	}
	c = append(c, []byte("</ruby>")...)
	return
}
