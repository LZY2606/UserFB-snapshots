package opengraph

import "golang.org/x/net/html"

// Link represents any "<link ...>" HTML tag.
// <link> will NOT be used when Intent.String == true.
type Link struct {
	Rel  string
	Href string
}

// LinkTag constructs Link
func LinkTag(n *html.Node) *Link {
	link := new(Link)
	for _, attr := range n.Attr {
		switch attr.Key {
		case "rel":
			link.Rel = attr.Val
		case "href":
			link.Href = attr.Val
		}
	}
	return link
}

// Contribute contributes OpenGraph
func (link *Link) Contribute(og *OpenGraph) error {
	switch {
	case link.IsFavicon():
		og.Favicon = Favicon{URL: link.Href}
	}
	return nil
}

// IsFavicon returns if it can be "favicon" of *opengraph.OpenGraph
func (link *Link) IsFavicon() bool {
	return link.Rel == "shortcut icon" || link.Rel == "icon"
}

// Title represents any "<title ...>" HTML tag.
// <title> will NOT be used when Intent.Strict == true.
type Title struct {
	Text string
}

// TitleTag constructs Title.
func TitleTag(n *html.Node) *Title {
	t := new(Title)
	if n.FirstChild != nil {
		t.Text = n.FirstChild.Data
	}
	return t
}

// Contribute contributes to OpenGraph
func (t *Title) Contribute(og *OpenGraph) error {
	if og.Title == "" && t.Text != "" {
		og.Title = t.Text
	}
	return nil
}

// Base represents any "<base ...>" HTML tag. Its href establishes the
// document base used to resolve relative media URLs.
type Base struct {
	Href string
}

// BaseTag constructs Base.
func BaseTag(n *html.Node) *Base {
	b := new(Base)
	for _, attr := range n.Attr {
		if attr.Key == "href" {
			b.Href = attr.Val
		}
	}
	return b
}

// Contribute contributes to OpenGraph. Only the first valid HTTP(S) base of
// the current page is kept.
func (b *Base) Contribute(og *OpenGraph) error {
	og.noteBaseHref(b.Href)
	return nil
}
