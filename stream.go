package gocrawl

import (
	"bytes"
	"io"
	"io/ioutil"
	"net/http"
	"net/url"
	"strings"

	"github.com/PuerkitoBio/goquery"
	"golang.org/x/net/html"
	"golang.org/x/net/html/atom"
)

// streamedLinks carries the links discovered while a response body is still
// being read in StreamLinks mode. It travels through the crawler's enqueue
// channel so that the exact same Filter, normalization, de-duplication,
// same-host, robots and politeness policies apply as for harvested links.
// The source is the page being read, never the (possibly different) base URL.
type streamedLinks struct {
	source *URLContext
	urls   []*url.URL
}

// headLevelTags lists the elements allowed in a document's head section by
// the HTML spec. When no explicit head tag is present, the base URL is
// finalized at the first token that is not one of these (i.e. the first body
// token).
var headLevelTags = map[atom.Atom]bool{
	atom.Base:     true,
	atom.Link:     true,
	atom.Meta:     true,
	atom.Noscript: true,
	atom.Script:   true,
	atom.Style:    true,
	atom.Template: true,
	atom.Title:    true,
}

// linkStreamer is a token-driven state machine that extracts a[href] links
// from an HTML token stream. It only sees parser tokens, so links hidden in
// comments, script/style raw text or half-written attributes are never
// extracted.
type linkStreamer struct {
	// emit is called for each link resolved against the finalized base.
	emit func(*url.URL)

	// fallback is the page URL used when no valid base tag is present.
	fallback *url.URL

	// base is the effective base URL. It is only safe to use once
	// baseFinalized is true.
	base *url.URL

	// baseAttempted reports whether the first base[href] tag has already
	// been seen. A first base that is empty, invalid or not http(s) locks
	// the fallback in, and later base tags are ignored.
	baseAttempted bool

	// baseFinalized reports whether the head section is over and the
	// effective base URL has been determined for good.
	baseFinalized bool

	// explicitHead reports whether an explicit <head> start tag was seen.
	explicitHead bool

	// pending holds the href values found in an explicit head section
	// before the base is finalized; they are flushed at </head> or <body>.
	pending []string
}

func newLinkStreamer(ctx *URLContext, res *http.Response, emit func(*url.URL)) *linkStreamer {
	fallback := ctx.url
	if res.Request != nil && res.Request.URL != nil {
		fallback = res.Request.URL
	}
	return &linkStreamer{
		emit:     emit,
		fallback: fallback,
		base:     fallback,
	}
}

// resolveStreamBase resolves a base[href] value against the fallback page
// URL. Relative base hrefs are supported. The second return value is false
// when the href is empty, cannot be parsed or is not an http(s) URL.
func resolveStreamBase(fallback *url.URL, href string) (*url.URL, bool) {
	if href == "" {
		return nil, false
	}
	ref, err := url.Parse(href)
	if err != nil {
		return nil, false
	}
	abs := fallback.ResolveReference(ref)
	if abs.Scheme != "http" && abs.Scheme != "https" {
		return nil, false
	}
	return abs, true
}

func attrVal(t html.Token, key string) (string, bool) {
	for i := range t.Attr {
		if t.Attr[i].Key == key {
			return t.Attr[i].Val, true
		}
	}
	return "", false
}

// handle processes a single HTML token.
func (s *linkStreamer) handle(t html.Token) {
	switch t.Type {
	case html.CommentToken, html.DoctypeToken:
		// Comments and doctypes never contain extractable links.
		return

	case html.TextToken:
		if !s.explicitHead && !s.baseFinalized && strings.TrimSpace(t.Data) != "" {
			// No explicit head: the first non-whitespace text is body content.
			s.finalizeBase()
		}

	case html.StartTagToken, html.SelfClosingTagToken:
		s.handleStart(t)

	case html.EndTagToken:
		switch t.DataAtom {
		case atom.Head, atom.Body, atom.Html:
			s.finalizeBase()
		}
	}
}

func (s *linkStreamer) handleStart(t html.Token) {
	switch t.DataAtom {
	case atom.Html:
		// The root element is neither head nor body content.
		return

	case atom.Head:
		s.explicitHead = true
		if t.Type == html.SelfClosingTagToken {
			s.finalizeBase()
		}
		return

	case atom.Body:
		s.finalizeBase()
		return

	case atom.Base:
		if s.baseFinalized || s.baseAttempted {
			// Only the first base tag, seen before the body starts, may
			// take over. It never changes once finalized, and a first
			// invalid base locks the fallback in for later base tags.
			return
		}
		s.baseAttempted = true
		if href, ok := attrVal(t, "href"); ok {
			if base, ok := resolveStreamBase(s.fallback, href); ok {
				s.base = base
			}
		}
		// An empty, invalid or non-http(s) base keeps the fallback URL.
		return

	case atom.A:
		href, ok := attrVal(t, "href")
		if !ok {
			return
		}
		if s.explicitHead && !s.baseFinalized {
			// Links declared in an explicit head are held until the base
			// URL is known for sure.
			s.pending = append(s.pending, href)
			return
		}
		if !s.baseFinalized {
			// An <a> outside of an explicit head is itself a body token.
			s.finalizeBase()
		}
		s.resolveAndEmit(href)
		return
	}

	if !s.explicitHead && !s.baseFinalized && !headLevelTags[t.DataAtom] {
		// No explicit head: any non-head-level start tag is body content.
		s.finalizeBase()
	}
}

// finalizeBase locks the effective base URL in and releases the links held
// back from an explicit head section.
func (s *linkStreamer) finalizeBase() {
	if s.baseFinalized {
		return
	}
	s.baseFinalized = true
	pending := s.pending
	s.pending = nil
	for _, href := range pending {
		s.resolveAndEmit(href)
	}
}

// flushEOF releases any link still held back because the document ended
// without closing the head section or starting a body.
func (s *linkStreamer) flushEOF() {
	s.finalizeBase()
}

// resolveAndEmit expands an href against the effective base and hands the
// absolute URL to the emitter. Empty hrefs and fragment-only links are
// skipped; unparsable hrefs are ignored like in the legacy link processing.
func (s *linkStreamer) resolveAndEmit(href string) {
	if href == "" || strings.HasPrefix(href, "#") {
		return
	}
	ref, err := url.Parse(href)
	if err != nil {
		return
	}
	s.emit(s.base.ResolveReference(ref))
}

// streamVisitURL reads and tokenizes the response body incrementally,
// enqueuing each complete a[href] link as soon as it is read. On a clean EOF
// it builds the DOM from the full body and runs the Visit/Visited hooks once,
// with a re-readable body. A read failure is reported once as CekReadBody,
// does not trigger Visit, and settles the parent request as not visited.
func (w *worker) streamVisitURL(ctx *URLContext, res *http.Response) (interface{}, bool) {
	var buf bytes.Buffer
	streamer := newLinkStreamer(ctx, res, func(u *url.URL) {
		// Drop the link if the crawler is shutting down, otherwise the
		// buffered enqueue channel could block indefinitely.
		select {
		case <-w.stop:
			w.logFunc(LogInfo, "ignoring streamed link, will stop.")
			return
		case w.enqueue <- &streamedLinks{source: ctx, urls: []*url.URL{u}}:
		}
	})

	z := html.NewTokenizer(io.TeeReader(res.Body, &buf))
	for {
		tt := z.Next()
		if tt == html.ErrorToken {
			break
		}
		streamer.handle(z.Token())
	}
	if err := z.Err(); err != io.EOF {
		w.opts.Extender.Error(newCrawlError(ctx, err, CekReadBody))
		w.logFunc(LogError, "ERROR reading body %s: %s", ctx.url, err)
		return nil, false
	}

	// Clean EOF: release links held back by a head section left open.
	streamer.flushEOF()

	// Clean EOF: build the DOM from the complete original bytes, exactly
	// like the non-streaming visit path.
	bd := buf.Bytes()
	var doc *goquery.Document
	if node, e := html.Parse(bytes.NewBuffer(bd)); e != nil {
		w.opts.Extender.Error(newCrawlError(ctx, e, CekParseBody))
		w.logFunc(LogError, "ERROR parsing %s: %s", ctx.url, e)
	} else {
		doc = goquery.NewDocumentFromNode(node)
		doc.Url = res.Request.URL
	}
	// Re-assign the body so the visitor can re-read the full content.
	res.Body = ioutil.NopCloser(bytes.NewBuffer(bd))

	// Automatic out-links were already streamed, so the doLinks flag is
	// intentionally ignored: no whole-document re-scan happens here. A
	// harvested value explicitly returned by Visit still follows the
	// regular source and scheduling rules via the worker response.
	harvested, _ := w.opts.Extender.Visit(ctx, res, doc)
	w.opts.Extender.Visited(ctx, harvested)
	return harvested, true
}
