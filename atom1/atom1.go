// Package atom1 provides parsing functionality for [Atom] feeds.
//
// [Atom]: https://datatracker.ietf.org/doc/html/rfc4287
package atom1

import (
	"encoding/xml"
	"io"

	"golang.org/x/net/html/charset"
)

// Atom namespaces used by feed and archive metadata.
const (
	// AtomNamespace is the namespace of Atom 1.0 elements.
	AtomNamespace = "http://www.w3.org/2005/Atom"

	// HistoryNamespace is the namespace used by RFC 5005
	// archive markers and archive links.
	HistoryNamespace = "http://purl.org/syndication/history/1.0"

	// XMLNamespace is the namespace of the xml:base attribute.
	XMLNamespace = "http://www.w3.org/XML/1998/namespace"
)

// Parse decodes data from r as an Atom 1.0 feed.
func Parse(r io.Reader) (*Feed, error) {
	var feed Feed
	p := xml.NewDecoder(r)
	p.CharsetReader = charset.NewReaderLabel
	err := p.Decode(&feed)
	if err != nil {
		return nil, err
	}

	return &feed, nil
}

type (
	Feed struct {
		XMLName     xml.Name `xml:"feed"`
		Title       string   `xml:"title"`
		Description string   `xml:"subtitle"`
		Author      *Author  `xml:"author"`
		Link        []Link   `xml:"link"`
		Image       *Image   `xml:"image"`
		Items       []Item   `xml:"entry"`
		Updated     string   `xml:"updated"`

		// XMLBase is the xml:base attribute on the feed
		// element. It is not part of the Atom data model
		// and is only used when resolving relative links.
		XMLBase string `xml:"http://www.w3.org/XML/1998/namespace base,attr" json:"-"`

		// Archive reports whether the feed declares the
		// RFC 5005 fh:archive marker as a direct child.
		Archive *Archive `xml:"http://purl.org/syndication/history/1.0 archive" json:"-"`
	}

	// Archive is the RFC 5005 archive marker element.
	Archive struct {
		XMLName xml.Name `xml:"archive"`
	}

	Author struct {
		Name  string `xml:"name"`
		URI   string `xml:"uri"`
		Email string `xml:"email"`
		Links []Link `xml:"link"`
	}

	Link struct {
		XMLName xml.Name `xml:"link" json:"-"`
		Href    string   `xml:"href,attr"`
		Rel     string   `xml:"rel,attr"`
		Type    string   `xml:"type,attr"`
		Length  uint     `xml:"length,attr"`

		// XMLBase is the xml:base attribute on the link
		// element. It is not part of the Atom data model
		// and is only used when resolving relative links.
		XMLBase string `xml:"http://www.w3.org/XML/1998/namespace base,attr" json:"-"`
	}

	Image struct {
		XMLName xml.Name `xml:"image"`
		Title   string   `xml:"title"`
		URL     string   `xml:"url"`
		Height  int      `xml:"height"`
		Width   int      `xml:"width"`
	}

	Item struct {
		XMLName xml.Name `xml:"entry"`
		Title   string   `xml:"title"`
		Summary string   `xml:"summary"`
		Content *Content `xml:"content"`
		Links   []Link   `xml:"link"`
		Date    string   `xml:"updated"`
		ID      string   `xml:"id"`
	}

	Content struct {
		RAWContent string `xml:",innerxml"`
	}
)
