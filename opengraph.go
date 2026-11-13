// Package opengraph implements and parses "The Open Graph Protocol" of web pages.
// See http://ogp.me/ for more information.
package opengraph

import (
	"context"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"

	"golang.org/x/net/html"
)

const (
	// HTMLMetaTag is a tag name of <meta>
	HTMLMetaTag string = "meta"
	// HTMLLinkTag is a tag name of <link>
	HTMLLinkTag string = "link"
	// HTMLTitleTag is a tag name of <title>
	HTMLTitleTag string = "title"
	// HTMLBaseTag is a tag name of <base>
	HTMLBaseTag string = "base"
)

// OpenGraph represents web page information according to OGP <ogp.me>,
// and some more additional informations like URL.Host and so.
type OpenGraph struct {

	// Basic Metadata
	// https://ogp.me/#metadata
	Title string  `json:"title"`
	Type  string  `json:"type"`
	Image []Image `json:"image"` // could be multiple
	URL   string  `json:"url"`

	// Optional Metadata
	// https://ogp.me/#optional
	Audio       []Audio  `json:"audio"` // could be multiple
	Description string   `json:"description"`
	Determiner  string   `json:"determiner"` // TODO: enum of (a, an, the, "", auto)
	Locale      string   `json:"locale"`
	LocaleAlt   []string `json:"locale_alternate"`
	SiteName    string   `json:"site_name"`
	Video       []Video  `json:"video"`

	// Additional (unofficial)
	Favicon Favicon `json:"favicon"`

	// Intent represents how to fetch, parse, and complete properties
	// of this OpenGraph object.
	// This SHOULD NOT have any meaning for "OpenGraph Protocol".
	Intent Intent `json:"-"`

	// imageCursor, audioCursor and videoCursor remember the current record of
	// each media kind so structured properties bind to the nearest root.
	// They are reset at the beginning of every Parse or Walk, while the
	// collected slices above are kept and appended to.
	imageCursor mediaCursor
	audioCursor mediaCursor
	videoCursor mediaCursor

	// baseHref is the first valid HTTP(S) <base href> seen during the latest
	// Parse or Walk, already resolved against Intent.URL when it was relative.
	baseHref string

	// imagePage, audioPage and videoPage record which page (see pages below)
	// each media record was collected from, so ToAbs can expand every record
	// against its own page base even when several pages share one OpenGraph.
	imagePage []int
	audioPage []int
	videoPage []int
	// pages holds the base information of every Parse or Walk invocation,
	// in order. pageIndex identifies the page currently being walked; it is
	// -1 when meta tags are fed through direct Contribute calls.
	pages     []pageBaseInfo
	pageIndex int
}

// pageBaseInfo keeps everything needed to resolve relative URLs of the media
// collected from one parsed page.
type pageBaseInfo struct {
	// baseHref is the first valid HTTP(S) <base href> of the page, already
	// resolved against Intent.URL when it was relative. Empty when absent.
	baseHref string
	// intentURL is Intent.URL at the time the page was walked.
	intentURL string
	// ogURL is the page's og:url, snapshotted when the walk finished. It is
	// only used as a last-resort fallback.
	ogURL string
}

// New constructs new OpenGraph struct and fill nullable fields.
func New(rawurl string) *OpenGraph {
	return &OpenGraph{
		Image:       []Image{},
		Audio:       []Audio{},
		Video:       []Video{},
		LocaleAlt:   []string{},
		imageCursor: newMediaCursor(mediaKindImage),
		audioCursor: newMediaCursor(mediaKindAudio),
		videoCursor: newMediaCursor(mediaKindVideo),
		pageIndex:   -1,
		Intent: Intent{
			URL: rawurl,
		},
	}
}

// Fetch creates and parses OpenGraph with specified URL.
func Fetch(url string, intent ...Intent) (*OpenGraph, error) {
	ogp := New(url)
	if len(intent) > 0 {
		ogp.Intent = intent[0]
	}
	ogp.Intent.URL = url
	err := ogp.Fetch()
	return ogp, err
}

// Fetch ...
func (og *OpenGraph) Fetch() error {
	if og.Intent.URL == "" {
		return fmt.Errorf("no URL given yet")
	}

	if og.Intent.HTTPClient == nil {
		og.Intent.HTTPClient = http.DefaultClient
	}

	req, err := http.NewRequest("GET", og.Intent.URL, nil)
	if err != nil {
		return err
	}

	if og.Intent.Context == nil {
		og.Intent.Context = context.Background()
	}

	req = req.WithContext(og.Intent.Context)

	// Apply headers
	headers := og.Intent.Headers
	if headers == nil {
		// Use default browser-like headers to avoid bot detection
		headers = map[string]string{
			"User-Agent":      "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/91.0.4472.114 Safari/537.36",
			"Accept":          "text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,image/apng,*/*;q=0.8",
			"Accept-Language": "en-US,en;q=0.9",
		}
	}
	for key, value := range headers {
		req.Header.Set(key, value)
	}

	res, err := og.Intent.HTTPClient.Do(req)
	if err != nil {
		return err
	}
	defer res.Body.Close()

	if !strings.HasPrefix(res.Header.Get("Content-Type"), "text/html") {
		return fmt.Errorf("Content type must be text/html")
	}

	if err = og.Parse(res.Body); err != nil {
		return err
	}

	if !og.Intent.Strict && og.Favicon.URL == "" {
		og.Favicon.URL = "/favicon.ico"
	}

	return nil
}

// Parse parses http.Response.Body and construct OpenGraph informations.
// Caller should close body after it gets parsed.
func (og *OpenGraph) Parse(body io.Reader) error {
	node, err := html.Parse(body)
	if err != nil {
		return err
	}
	return og.Walk(node)
}

// Walk scans HTML nodes to pick up meaningful OGP data. Media root bindings are
// reset first so a new page never attaches orphan properties to records left
// over from a previous page; already collected media slices are preserved and
// keep growing. Every Walk opens a new page session whose base is remembered
// per media record, so a later ToAbs resolves each record with its own page
// base. The walk stops at the first error returned by a tag.
func (og *OpenGraph) Walk(node *html.Node) error {
	og.resetMediaCursors()
	og.baseHref = ""
	if len(og.Intent.TrustedTags) == 0 {
		if og.Intent.Strict {
			og.Intent.TrustedTags = []string{HTMLMetaTag}
		} else {
			og.Intent.TrustedTags = []string{HTMLMetaTag, HTMLTitleTag, HTMLLinkTag}
		}
	}

	og.pageIndex = len(og.pages)
	og.pages = append(og.pages, pageBaseInfo{intentURL: og.Intent.URL})
	err := og.walk(node)
	og.pages[og.pageIndex].baseHref = og.baseHref
	og.pages[og.pageIndex].ogURL = og.URL
	og.pageIndex = -1
	return err
}

func (og *OpenGraph) walk(node *html.Node) error {
	if node.Type == html.ElementNode {
		switch {
		case node.Data == HTMLMetaTag && og.trust(node.Data):
			return MetaTag(node).Contribute(og)
		case node.Data == HTMLTitleTag && og.trust(node.Data):
			return TitleTag(node).Contribute(og)
		case node.Data == HTMLLinkTag && og.trust(node.Data):
			return LinkTag(node).Contribute(og)
		case node.Data == HTMLBaseTag:
			// <base> defines document-level URL resolution, so it is honored
			// regardless of Strict / TrustedTags, which only scope content tags.
			return BaseTag(node).Contribute(og)
		}
	}

	for child := node.FirstChild; child != nil; child = child.NextSibling {
		if err := og.walk(child); err != nil {
			return err
		}
	}

	return nil
}

func (og *OpenGraph) trust(tagName string) bool {
	for _, name := range og.Intent.TrustedTags {
		if name == tagName {
			return true
		}
	}
	return false
}

// resetMediaCursors clears the current root binding of every media kind.
func (og *OpenGraph) resetMediaCursors() {
	og.imageCursor.reset()
	og.audioCursor.reset()
	og.videoCursor.reset()
}

// appendMedia opens a new empty media record of the given kind in page order.
// Even records sharing the same URL stay separate records with their own
// structured properties. The record is tagged with the page currently being
// walked so ToAbs knows which base applies to it.
func (og *OpenGraph) appendMedia(kind mediaKind, rawurl string) {
	switch kind {
	case mediaKindImage:
		og.Image = append(og.Image, Image{URL: rawurl})
		og.imagePage = append(og.imagePage, og.pageIndex)
	case mediaKindAudio:
		og.Audio = append(og.Audio, Audio{URL: rawurl})
		og.audioPage = append(og.audioPage, og.pageIndex)
	case mediaKindVideo:
		og.Video = append(og.Video, Video{URL: rawurl})
		og.videoPage = append(og.videoPage, og.pageIndex)
	}
}

func (og *OpenGraph) mediaLen(kind mediaKind) int {
	switch kind {
	case mediaKindImage:
		return len(og.Image)
	case mediaKindAudio:
		return len(og.Audio)
	case mediaKindVideo:
		return len(og.Video)
	}
	return 0
}

// noteBaseHref records the first valid HTTP(S) base href of the current tree.
// A relative base href is resolved against Intent.URL. Invalid or non-HTTP(S)
// bases are ignored so a later valid base can still be picked up.
func (og *OpenGraph) noteBaseHref(href string) {
	if href == "" || og.baseHref != "" {
		return
	}
	ref, err := url.Parse(href)
	if err != nil {
		return
	}
	if !ref.IsAbs() {
		intent, err := url.Parse(og.Intent.URL)
		if err != nil || !intent.IsAbs() {
			return
		}
		ref = intent.ResolveReference(ref)
	}
	if ref.Scheme != "http" && ref.Scheme != "https" {
		return
	}
	og.baseHref = ref.String()
}

// baseForPage resolves the base URL of one collected page. The first valid
// HTTP(S) <base href> of that page wins; otherwise the Intent.URL in effect
// when the page was walked is used; only when both are missing does that
// page's og:url serve as a fallback. It returns nil when no absolute base is
// available at all.
func (og *OpenGraph) baseForPage(page int) (*url.URL, error) {
	var info pageBaseInfo
	if page >= 0 && page < len(og.pages) {
		info = og.pages[page]
	} else {
		// Media added via direct Contribute calls: use the live state.
		info = pageBaseInfo{
			baseHref:  og.baseHref,
			intentURL: og.Intent.URL,
			ogURL:     og.URL,
		}
	}
	if info.baseHref != "" {
		return url.Parse(info.baseHref)
	}
	for _, raw := range []string{info.intentURL, info.ogURL} {
		if raw == "" {
			continue
		}
		u, err := url.Parse(raw)
		if err != nil {
			return nil, err
		}
		if u.IsAbs() {
			return u, nil
		}
	}
	return nil, nil
}

// ToAbs makes all relative URLs absolute, following the relative-reference
// rules of net/url (relative paths, queries, fragments and scheme-relative
// URLs are all covered). Both ordinary and secure URLs of every media record
// are resolved against the base of the page they were collected from, so
// media of one page is never moved onto the host of another page's og:url.
// Empty fields stay empty, already absolute URLs keep their meaning, and
// repeated calls do not change the result.
func (og *OpenGraph) ToAbs() error {
	for i := range og.Image {
		base, err := og.baseForPage(og.imagePage[i])
		if err != nil {
			return err
		}
		if err := og.resolveMediaURL(base, &og.Image[i].URL); err != nil {
			return err
		}
		if err := og.resolveMediaURL(base, &og.Image[i].SecureURL); err != nil {
			return err
		}
	}
	for i := range og.Audio {
		base, err := og.baseForPage(og.audioPage[i])
		if err != nil {
			return err
		}
		if err := og.resolveMediaURL(base, &og.Audio[i].URL); err != nil {
			return err
		}
		if err := og.resolveMediaURL(base, &og.Audio[i].SecureURL); err != nil {
			return err
		}
	}
	for i := range og.Video {
		base, err := og.baseForPage(og.videoPage[i])
		if err != nil {
			return err
		}
		if err := og.resolveMediaURL(base, &og.Video[i].URL); err != nil {
			return err
		}
		if err := og.resolveMediaURL(base, &og.Video[i].SecureURL); err != nil {
			return err
		}
	}
	// Favicon is not part of the three media groups and uses the live state.
	base, err := og.baseForPage(-1)
	if err != nil {
		return err
	}
	if err := og.resolveMediaURL(base, &og.Favicon.URL); err != nil {
		return err
	}
	return nil
}

// resolveMediaURL resolves one field in place against base. Absolute values
// are returned unchanged, which also makes repeated ToAbs calls idempotent.
func (og *OpenGraph) resolveMediaURL(base *url.URL, field *string) error {
	if *field == "" {
		return nil
	}
	ref, err := url.Parse(*field)
	if err != nil {
		return err
	}
	if ref.IsAbs() {
		return nil
	}
	if base == nil {
		return fmt.Errorf("opengraph: no base URL available to resolve %q", *field)
	}
	*field = base.ResolveReference(ref).String()
	return nil
}
