package opengraph

import (
	"strconv"
	"strings"

	"golang.org/x/net/html"
)

// Meta represents any "<meta ...>" HTML tag.
type Meta struct {
	Name     string
	Property string
	Content  string
}

// MetaTag constructs MetaTag.
func MetaTag(node *html.Node) *Meta {
	meta := new(Meta)
	for _, attr := range node.Attr {
		switch attr.Key {
		case "property":
			meta.Property = attr.Val
		case "content":
			meta.Content = attr.Val
		case "name":
			meta.Name = attr.Val
		}
	}
	return meta
}

// mediaCursor tracks where structured properties of one media kind are bound.
// Each kind keeps its own cursor, so meta tags of another kind interleaved in
// between never change the binding of this kind.
type mediaCursor struct {
	// kind is the root name without the "og:" prefix, e.g. "image".
	kind mediaKind
	// rootPending reports whether the immediately preceding meta tag (no other
	// meta in between) was the bare root property of this kind.
	rootPending bool
	// rootValue holds the content of that pending bare root property.
	rootValue string
	// index points at the current record inside the OpenGraph slice, or -1.
	index int
}

func newMediaCursor(kind mediaKind) mediaCursor {
	return mediaCursor{kind: kind, index: -1}
}

// reset forgets the current record of this kind. It is called at the beginning
// of every Parse or Walk so orphan properties of a new page are not attached
// to the last record collected from a previous page.
func (c *mediaCursor) reset() {
	c.rootPending = false
	c.rootValue = ""
	c.index = -1
}

// beginRoot opens a new record for a bare root property ("og:image", ...).
func (c *mediaCursor) beginRoot(og *OpenGraph, content string) {
	og.appendMedia(c.kind, content)
	c.index = og.mediaLen(c.kind) - 1
	c.rootPending = true
	c.rootValue = content
}

// beginURL handles an ":url" property. It only counts as an alias of the
// preceding bare root when it follows it immediately, with no other meta tag
// in between, and carries exactly the same content. Otherwise it opens a
// record of its own.
func (c *mediaCursor) beginURL(og *OpenGraph, content string) {
	if c.rootPending && c.index >= 0 && c.rootValue == content {
		return
	}
	og.appendMedia(c.kind, content)
	c.index = og.mediaLen(c.kind) - 1
}

// noteOther records that a meta tag which cannot be the ":url" alias was seen,
// breaking any pending bare-root/alias adjacency.
func (c *mediaCursor) noteOther() {
	c.rootPending = false
	c.rootValue = ""
}

// current returns the index of the current record, or -1 when no root has
// been seen yet. Structured properties appearing before their root are
// orphaned and ignored.
func (c *mediaCursor) current() int { return c.index }

// Contribute feeds a single meta tag into the OpenGraph object. Calling it
// repeatedly outside Parse or Walk still behaves as one continuous stream of
// properties, so a fresh OpenGraph object can be built tag by tag.
func (meta *Meta) Contribute(og *OpenGraph) error {
	prop := meta.Property

	switch {
	case meta.IsTitle():
		og.Title = meta.Content
	case meta.IsOGDescription():
		og.Description = meta.Content
	case meta.IsDescription() && !og.Intent.Strict && og.Description == "":
		og.Description = meta.Content
	case meta.IsSiteName():
		og.SiteName = meta.Content
	case prop == "og:image":
		og.imageCursor.beginRoot(og, meta.Content)
	case prop == "og:image:url":
		og.imageCursor.beginURL(og, meta.Content)
	case meta.IsPropertyOf("og:image"):
		if i := og.imageCursor.current(); i >= 0 {
			if err := og.contributeImage(i, prop, meta.Content); err != nil {
				og.breakMediaAdjacency(prop)
				return err
			}
		}
	case prop == "og:audio":
		og.audioCursor.beginRoot(og, meta.Content)
	case prop == "og:audio:url":
		og.audioCursor.beginURL(og, meta.Content)
	case meta.IsPropertyOf("og:audio"):
		if i := og.audioCursor.current(); i >= 0 {
			og.contributeAudio(i, prop, meta.Content)
		}
	case prop == "og:video":
		og.videoCursor.beginRoot(og, meta.Content)
	case prop == "og:video:url":
		og.videoCursor.beginURL(og, meta.Content)
	case meta.IsPropertyOf("og:video"):
		if i := og.videoCursor.current(); i >= 0 {
			if err := og.contributeVideo(i, prop, meta.Content); err != nil {
				og.breakMediaAdjacency(prop)
				return err
			}
		}
	case meta.IsType():
		og.Type = meta.Content
	case meta.IsURL():
		og.URL = meta.Content
	}

	og.breakMediaAdjacency(prop)
	return nil
}

// breakMediaAdjacency maintains root/:url adjacency after a meta tag is
// processed. A bare root keeps its own pending state but breaks the other two
// kinds; every other meta tag breaks adjacency for all three kinds.
func (og *OpenGraph) breakMediaAdjacency(property string) {
	switch property {
	case "og:image":
		og.audioCursor.noteOther()
		og.videoCursor.noteOther()
	case "og:audio":
		og.imageCursor.noteOther()
		og.videoCursor.noteOther()
	case "og:video":
		og.imageCursor.noteOther()
		og.audioCursor.noteOther()
	default:
		og.imageCursor.noteOther()
		og.audioCursor.noteOther()
		og.videoCursor.noteOther()
	}
}

// contributeImage binds a structured "og:image:*" property to record i.
// A later valid value of the same property overwrites an earlier one.
func (og *OpenGraph) contributeImage(i int, property, content string) error {
	switch property {
	case "og:image:secure_url":
		og.Image[i].SecureURL = content
	case "og:image:type":
		og.Image[i].Type = content
	case "og:image:alt":
		og.Image[i].Alt = content
	case "og:image:width":
		v, err := parseNonNegativeInt(property, content)
		if err != nil {
			return err
		}
		og.Image[i].Width = v
	case "og:image:height":
		v, err := parseNonNegativeInt(property, content)
		if err != nil {
			return err
		}
		og.Image[i].Height = v
	}
	return nil
}

// contributeAudio binds a structured "og:audio:*" property to record i.
func (og *OpenGraph) contributeAudio(i int, property, content string) {
	switch property {
	case "og:audio:secure_url":
		og.Audio[i].SecureURL = content
	case "og:audio:type":
		og.Audio[i].Type = content
	}
}

// contributeVideo binds a structured "og:video:*" property to record i.
func (og *OpenGraph) contributeVideo(i int, property, content string) error {
	switch property {
	case "og:video:secure_url":
		og.Video[i].SecureURL = content
	case "og:video:type":
		og.Video[i].Type = content
	case "og:video:width":
		v, err := parseNonNegativeInt(property, content)
		if err != nil {
			return err
		}
		og.Video[i].Width = v
	case "og:video:height":
		v, err := parseNonNegativeInt(property, content)
		if err != nil {
			return err
		}
		og.Video[i].Height = v
	case "og:video:duration":
		v, err := parseNonNegativeInt(property, content)
		if err != nil {
			return err
		}
		og.Video[i].Duration = v
	}
	return nil
}

// parseNonNegativeInt accepts only decimal non-negative integers that fit in
// int. Any other value yields a NumericPropertyError carrying the property
// name and the original content, leaving the field's previous value intact.
func parseNonNegativeInt(property, content string) (int, error) {
	v, err := strconv.Atoi(content)
	if err != nil || v < 0 {
		return 0, &NumericPropertyError{Property: property, Content: content}
	}
	return v, nil
}

// IsTitle returns if it can be "title" of OGP
func (meta *Meta) IsTitle() bool {
	return meta.Property == "og:title" && meta.Content != ""
}

// IsOGDescription returns if it can be "description" of OGP
func (meta *Meta) IsOGDescription() bool {
	return meta.Property == "og:description" && meta.Content != ""
}

// IsDescription returns if it can be "description" of OGP.
// CAUTION: This property SHOULD NOT be used when Intent.Strict == true.
func (meta *Meta) IsDescription() bool {
	return meta.Name == "description" && meta.Content != ""
}

// IsPropertyOf returns if it can be a property of specified struct
func (meta *Meta) IsPropertyOf(name string) bool {
	return strings.HasPrefix(meta.Property, name+":")
}

// IsType returns if it can be "og:type"
func (meta *Meta) IsType() bool {
	return meta.Property == "og:type"
}

// IsSiteName returns if it can be "og:site_name"
func (meta *Meta) IsSiteName() bool {
	return meta.Property == "og:site_name"
}

// IsURL returns if it can be "og:url"
func (meta *Meta) IsURL() bool {
	return meta.Property == "og:url"
}
