package links

import (
	"io"
	"strings"

	"golang.org/x/net/html"
	"golang.org/x/net/html/atom"
)

// HeadInfo holds control data declared inside the HTML head element.
type HeadInfo struct {
	// BaseHref is a raw href value of the first <base> tag with href attribute.
	BaseHref string
	// HasBase marks presence of <base> tag with href attribute (even an empty one).
	HasBase bool
	// Canonicals holds raw href values of <link rel~=canonical> tags, in document order.
	Canonicals []string
}

// HasRelToken reports if rel attribute value contains given token.
// Tokens are separated by ASCII whitespace and compared case-insensitively.
func HasRelToken(rel, token string) (yes bool) {
	for _, t := range strings.FieldsFunc(rel, isASCIIspace) {
		if strings.EqualFold(t, token) {
			return true
		}
	}

	return false
}

// ExtractHead parses HTML document and extracts control declarations (base href
// and canonical links) from its head element. Contents of template elements,
// comments, script/style literals and anything inside body are not declarations.
func ExtractHead(r io.Reader) (info HeadInfo) {
	doc, err := html.Parse(r)
	if err != nil {
		return
	}

	head := findElement(doc, atom.Head)
	if head == nil {
		return
	}

	var walk func(n *html.Node)

	walk = func(n *html.Node) {
		for c := n.FirstChild; c != nil; c = c.NextSibling {
			if c.Type != html.ElementNode {
				continue
			}

			switch c.DataAtom {
			case atom.Template:
				continue // template contents are not a part of the document

			case atom.Base:
				if !info.HasBase {
					if v, ok := attrValue(c, keyHREF); ok {
						info.BaseHref = v
						info.HasBase = true
					}
				}

			case atom.Link:
				if rel, ok := attrValue(c, "rel"); ok && HasRelToken(rel, "canonical") {
					if v, ok := attrValue(c, keyHREF); ok {
						info.Canonicals = append(info.Canonicals, v)
					}
				}
			}

			walk(c)
		}
	}

	walk(head)

	return info
}

func isASCIIspace(r rune) (yes bool) {
	switch r {
	case ' ', '\t', '\n', '\f', '\r':
		return true
	}

	return false
}

func findElement(n *html.Node, a atom.Atom) *html.Node {
	if n.Type == html.ElementNode && n.DataAtom == a {
		return n
	}

	for c := n.FirstChild; c != nil; c = c.NextSibling {
		if rv := findElement(c, a); rv != nil {
			return rv
		}
	}

	return nil
}

func attrValue(n *html.Node, key string) (rv string, ok bool) {
	for i := 0; i < len(n.Attr); i++ {
		if a := &n.Attr[i]; a.Key == key {
			return a.Val, true
		}
	}

	return "", false
}
