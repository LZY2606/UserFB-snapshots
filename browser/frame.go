package browser

import (
	"bytes"
	"io"
	"mime/multipart"
	"net/http"
	"net/url"
	"strings"

	"github.com/PuerkitoBio/goquery"
	"github.com/andybalholm/cascadia"
	"github.com/headzoo/surf/errors"
	"github.com/headzoo/surf/jar"
	"golang.org/x/net/html"
)

// ErrFrameTarget is returned when a form target cannot be resolved to exactly
// one open browsing context in the same frame tree. It is also returned for
// the "_blank" target, which does not open a new window.
var ErrFrameTarget = errors.New(
	"Form target does not resolve to a single open frame in the browsing context tree.")

// ErrFrameDetached is returned by a browsing context whose document, or an
// ancestor document, has been replaced. The handle itself stays valid for
// reading the old DOM snapshot, but it can no longer issue requests.
var ErrFrameDetached = errors.New(
	"The browsing context has been detached because its document was replaced.")

// frameDocument identifies a loaded document. Every successful navigation
// mints a new identity, which is used to bind forms and detach descendants.
type frameDocument struct {
	id uint64
}

var frameDocumentSeq uint64

func newFrameDocument() *frameDocument {
	frameDocumentSeq++
	return &frameDocument{id: frameDocumentSeq}
}

// frameTree is the registry shared by every browsing context of one frame
// tree. The root owns it; NewTab copies never get a reference.
type frameTree struct {
	root *Browser
}

// Frame opens the frame (iframe or frame element) matched by expr in the
// current document and returns its browsing context.
//
// The expression must match exactly one direct iframe/frame element whose src
// is a non-empty, parseable HTTP(S) URL, resolved against the current
// document's effective base when relative. Selecting the same element again
// returns the same context without another request; a failed open is not
// registered and may be retried. Frames are loaded lazily: elements that are
// never passed to Frame do not issue requests.
func (bow *Browser) Frame(expr string) (*Browser, error) {
	if err := bow.checkAlive(); err != nil {
		return nil, err
	}
	if _, err := cascadia.Compile(expr); err != nil {
		return nil, err
	}

	sel := bow.Find(expr)
	if sel.Length() == 0 {
		return nil, errors.NewElementNotFound(
			"Frame not found matching expr '%s'.", expr)
	}
	if sel.Length() > 1 {
		return nil, errors.NewElementNotFound(
			"Expr '%s' must match exactly one frame element, matched %d.", expr, sel.Length())
	}
	if !sel.Is("iframe, frame") {
		return nil, errors.NewElementNotFound(
			"Expr '%s' does not match an iframe or frame tag.", expr)
	}
	if _, ok := sel.Attr("srcdoc"); ok {
		return nil, errors.New(
			"Frame matched by '%s' uses srcdoc, which is not supported.", expr)
	}
	src, ok := sel.Attr("src")
	if !ok || strings.TrimSpace(src) == "" {
		return nil, errors.New(
			"Frame matched by '%s' has no usable src attribute.", expr)
	}
	srcURL, err := url.Parse(src)
	if err != nil {
		return nil, err
	}
	srcURL = bow.ResolveUrl(srcURL)
	if srcURL.Scheme != "http" && srcURL.Scheme != "https" {
		return nil, errors.New(
			"Frame src '%s' is not an HTTP(S) URL.", srcURL.String())
	}

	node := sel.Nodes[0]
	for _, child := range bow.frameChildren {
		if child.frameElement == node {
			return child, nil
		}
	}

	// Prepare the shared registry, but only commit the parent to the tree
	// once the child document opened successfully. A failed attempt leaves
	// the parent a plain standalone browser, so the frame rules never apply
	// to a tree that has no successfully opened frame.
	tree := bow.frameTree
	if tree == nil {
		tree = &frameTree{root: bow}
	}

	name, _ := sel.Attr("name")
	child := bow.newFrameContext(tree, name, node)
	// The child only joins the tree after the document has been opened
	// successfully, so a failed attempt leaves no trace and can be retried.
	if err := child.Open(srcURL.String()); err != nil {
		return nil, err
	}
	bow.frameTree = tree
	bow.frameChildren = append(bow.frameChildren, child)
	return child, nil
}

// newFrameContext creates a child browsing context. Request configuration is
// copied so child navigation cannot mutate the parent or a sibling, while the
// cookie jar and the bookmarks jar stay shared with the rest of the tree.
func (bow *Browser) newFrameContext(tree *frameTree, name string, node *html.Node) *Browser {
	child := &Browser{
		userAgent:     bow.userAgent,
		bookmarks:     bow.bookmarks,
		history:       jar.NewMemoryHistory(),
		headers:       copyHeaders(bow.headers),
		attributes:    make(AttributeMap, len(bow.attributes)),
		frameTree:     tree,
		frameParent:   bow,
		frameElement:  node,
		frameName:     name,
		frameDetached: false,
		frameChildren: nil,
	}
	for attr, v := range bow.attributes {
		child.attributes[attr] = v
	}

	client := child.buildClient()
	if bow.client != nil {
		client.Jar = bow.client.Jar
		client.Transport = bow.client.Transport
		client.Timeout = bow.client.Timeout
	}
	child.client = client
	child.state = &jar.State{}
	return child
}

// rootContext returns the root of the frame tree, or the browser itself when
// it is a standalone top-level context.
func (bow *Browser) rootContext() *Browser {
	if bow.frameTree != nil {
		return bow.frameTree.root
	}
	return bow
}

// walkTree visits the context and every attached descendant in document order.
func (bow *Browser) walkTree(fn func(*Browser)) {
	fn(bow)
	for _, child := range bow.frameChildren {
		child.walkTree(fn)
	}
}

// checkAlive returns ErrFrameDetached before any request, body read or
// attachment read when the context has been detached. Standalone browsers
// that never took part in a frame tree keep their old behavior.
func (bow *Browser) checkAlive() error {
	if bow.frameTree != nil && bow.frameDetached {
		return ErrFrameDetached
	}
	return nil
}

// checkFormBinding binds a form to the document that created it. A frame-tree
// form may only be submitted while its owner context is attached and still
// displays the same document.
func (bow *Browser) checkFormBinding(doc *frameDocument) error {
	if err := bow.checkAlive(); err != nil {
		return err
	}
	if bow.frameDoc != doc {
		return ErrFrameDetached
	}
	return nil
}

// detachChildren permanently removes every descendant context after this
// context's document is replaced. The child handles stay readable, but they
// can no longer navigate.
func (bow *Browser) detachChildren() {
	for _, child := range bow.frameChildren {
		child.markDetached()
	}
	bow.frameChildren = nil
}

func (bow *Browser) markDetached() {
	bow.frameDetached = true
	for _, child := range bow.frameChildren {
		child.markDetached()
	}
}

// resolveFrameTarget maps a form target attribute to the context that must
// receive the response. Reserved keywords are case-insensitive; ordinary
// names are case-sensitive and must match exactly one open context of the
// same frame tree.
func (bow *Browser) resolveFrameTarget(target string) (*Browser, error) {
	switch strings.ToLower(target) {
	case "", "_self":
		return bow, nil
	case "_parent":
		if bow.frameParent != nil {
			return bow.frameParent, nil
		}
		return bow, nil
	case "_top":
		return bow.rootContext(), nil
	case "_blank":
		return nil, ErrFrameTarget
	}

	if bow.frameTree == nil {
		return nil, ErrFrameTarget
	}
	var matches []*Browser
	bow.rootContext().walkTree(func(ctx *Browser) {
		if ctx.frameName != "" && ctx.frameName == target {
			matches = append(matches, ctx)
		}
	})
	if len(matches) != 1 {
		return nil, ErrFrameTarget
	}
	return matches[0], nil
}

// submitForm serializes the form with the existing encoding paths, resolves
// action and Referer against the source document, and delivers the response
// to the target context without touching any other context.
func (source *Browser) submitForm(f *Form, buttonName, buttonValue string) error {
	targetAttr, _ := f.selection.Attr("target")
	target, err := source.resolveFrameTarget(targetAttr)
	if err != nil {
		return err
	}
	if err := target.checkAlive(); err != nil {
		return err
	}

	method, ok := f.selection.Attr("method")
	if !ok {
		method = "GET"
	}
	action, ok := f.selection.Attr("action")
	if !ok {
		action = source.Url().String()
	}
	actionURL, err := url.Parse(action)
	if err != nil {
		return err
	}
	actionURL = source.ResolveUrl(actionURL)

	values := make(url.Values, len(f.fields)+1)
	for name, vals := range f.fields {
		values[name] = vals
	}
	if buttonName != "" {
		values.Set(buttonName, buttonValue)
	}

	var req *http.Request
	if strings.ToUpper(method) == "GET" {
		actionURL.RawQuery = values.Encode()
		req, err = source.buildRequest("GET", actionURL.String(), source.Url(), nil)
		if err != nil {
			return err
		}
	} else {
		enctype, _ := f.selection.Attr("enctype")
		if enctype == "multipart/form-data" {
			body, contentType, err := buildMultipartBody(values, f.files)
			if err != nil {
				return err
			}
			req, err = source.buildRequest("POST", actionURL.String(), source.Url(), body)
			if err != nil {
				return err
			}
			req.Header.Set("Content-Type", contentType)
		} else {
			req, err = source.buildRequest(
				"POST", actionURL.String(), source.Url(),
				strings.NewReader(values.Encode()))
			if err != nil {
				return err
			}
			req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
		}
	}

	// The request runs with the source context's client and configuration
	// (redirect policy, headers, shared cookie jar). Only the resulting
	// document is installed on the target context, so the source page and
	// every other context stay untouched.
	resp, body, err := source.fetch(req)
	if err != nil {
		return err
	}
	return target.adoptResponse(req, resp, body)
}

// buildMultipartBody encodes fields and files exactly like the historical
// PostMultipart path. Each file reader is consumed at most once.
func buildMultipartBody(fields url.Values, files FileSet) (*bytes.Buffer, string, error) {
	body := &bytes.Buffer{}
	writer := multipart.NewWriter(body)

	for k, vs := range fields {
		for _, v := range vs {
			if err := writer.WriteField(k, v); err != nil {
				return nil, "", err
			}
		}
	}
	for k, file := range files {
		fw, err := writer.CreateFormFile(k, file.fileName)
		if err != nil {
			return nil, "", err
		}
		if file.data != nil {
			if _, err := io.Copy(fw, file.data); err != nil {
				return nil, "", err
			}
		}
	}
	if err := writer.Close(); err != nil {
		return nil, "", err
	}
	return body, writer.FormDataContentType(), nil
}

// applyBaseTag sets the document's relative URL base from its first valid
// <base href>. Relative hrefs resolve against the final document URL, and the
// base applies regardless of the meta refresh setting.
func (bow *Browser) applyBaseTag() {
	var base *url.URL
	bow.Find("base[href]").EachWithBreak(func(_ int, s *goquery.Selection) bool {
		href, exists := s.Attr("href")
		if !exists || strings.TrimSpace(href) == "" {
			return true
		}
		parsed, err := url.Parse(href)
		if err != nil {
			return true
		}
		base = parsed
		return false
	})
	if base == nil {
		return
	}
	bow.relativeUrl = bow.RelativeUrl().ResolveReference(base)
}
