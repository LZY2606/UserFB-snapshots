package crawler

import (
	"context"
	"errors"
	"fmt"
	"io"
	"log"
	"net/http"
	"net/url"
	"slices"
	"strings"
	"sync"
	"time"

	"github.com/s0rg/set"
	"golang.org/x/net/html/atom"

	"github.com/s0rg/crawley/internal/client"
	"github.com/s0rg/crawley/internal/links"
	"github.com/s0rg/crawley/internal/robots"
)

type crawlClient interface {
	Get(context.Context, string) (io.ReadCloser, http.Header, error)
	Head(context.Context, string) (http.Header, error)
}

const (
	chMult     = 256
	chTimeout  = 100 * time.Millisecond
	doubleDash = "//"
)

type taskFlag byte

const (
	// TaskDefault marks result for printing only.
	TaskDefault taskFlag = iota
	// TaskCrawl marks result as to-be-crawled.
	TaskCrawl
	// TaskDone marks result as final - crawling ends here.
	TaskDone
)

type crawlResult struct {
	URI   string
	Hash  uint64
	Flag  taskFlag
	Style bool
}

// newCrawlClient is a hook for tests, it builds the http client used by Run.
var newCrawlClient = func(cfg *client.Config) crawlClient {
	return client.New(cfg)
}

// Crawler holds crawling process config and state.
type Crawler struct {
	cfg      *config
	handleCh chan string
	crawlCh  chan *url.URL
	styleCh  chan *url.URL
	resultCh chan crawlResult
	robots   *robots.TXT
	filter   links.TokenFilter
	styleEnq map[uint64]struct{} // stylesheets queued, in flight or done
	web      crawlClient         // overridable http client (tests)
	wg       sync.WaitGroup
}

// New creates Crawler instance.
func New(opts ...Option) (c *Crawler) {
	cfg := &config{}

	for _, o := range opts {
		o(cfg)
	}

	cfg.validate()

	return &Crawler{
		cfg:    cfg,
		robots: robots.AllowALL(),
		filter: prepareFilter(cfg.AlowedTags),
	}
}

// Run starts crawling process for given base uri.
func (c *Crawler) Run(uri string, urlcb func(string)) (err error) {
	var base *url.URL

	if base, err = url.Parse(uri); err != nil {
		return fmt.Errorf("parse url: %w", err)
	}

	workers := c.cfg.Client.Workers
	cssImports := c.cfg.CSSImports && c.cfg.ScanCSS

	n := (workers + 1)
	c.handleCh = make(chan string, n*chMult)
	c.crawlCh = make(chan *url.URL, n*chMult)
	c.styleCh = make(chan *url.URL, n*chMult)
	c.resultCh = make(chan crawlResult, n*chMult)

	if cssImports {
		c.styleEnq = make(map[uint64]struct{})
		c.styleEnq[urlhash(uri)] = struct{}{}
	}

	defer c.close()

	// crawlSeen dedups crawl candidates, outSeen dedups output. They are kept
	// apart so an address that was only reported as a resource can still be
	// crawled later, e.g. when it turns into a stylesheet entry.
	crawlSeen := make(set.Unordered[uint64])
	crawlSeen.Add(urlhash(uri))

	// the starting document is never printed
	outSeen := make(set.Unordered[uint64])
	outSeen.Add(urlhash(uri))

	web := c.web
	if web == nil {
		web = newCrawlClient(&c.cfg.Client)
	}

	c.initRobots(base, web)

	for i := 0; i < workers; i++ {
		go c.worker(web)
	}

	c.wg.Add(workers)

	go func() {
		for s := range c.handleCh {
			urlcb(s)
		}

		c.wg.Done()
	}()

	c.crawlCh <- base

	var t crawlResult

	for w := 1; w > 0; {
		t = <-c.resultCh

		switch {
		case t.Flag == TaskDone:
			w--
		case t.Style:
			// Style entries share output deduplication with ordinary results,
			// but they are scheduled separately so a type change is not lost
			// when the url has already been printed.
			if addOnce(outSeen, t.Hash) {
				c.tryHandle(t.URI)
			}

			if c.enqueueStyle(base, &t) {
				w++
			}
		default:
			// plain results are only produced once per url, so this branch is
			// reachable a single time for any address; the guards below keep
			// the original behaviour and are defensive.
			emitted := addOnce(outSeen, t.Hash)

			if t.Flag == TaskCrawl && !crawlSeen.Has(t.Hash) {
				crawlSeen.Add(t.Hash)

				if c.tryEnqueue(base, &t) {
					w++
				}
			}

			if emitted {
				c.tryHandle(t.URI)
			}
		}
	}

	return nil
}

// addOnce adds v to the set and reports whether it was not there before.
func addOnce(s set.Set[uint64], v uint64) (added bool) {
	if s.Has(v) {
		return false
	}

	s.Add(v)

	return true
}

// DumpConfig returns internal config representation.
func (c *Crawler) DumpConfig() string {
	return c.cfg.String()
}

func (c *Crawler) tryHandle(u string) {
	show := true

	idx := strings.LastIndexByte(u, '/')
	if idx == -1 {
		return
	}

	switch c.cfg.Dirs {
	case DirsHide:
		show = isResorce(u[idx:])
	case DirsOnly:
		show = !isResorce(u[idx:])
	}

	if !show {
		return
	}

	t := time.NewTimer(chTimeout)
	defer t.Stop()

	select {
	case c.handleCh <- u:
	case <-t.C:
	}
}

func (c *Crawler) tryEnqueue(base *url.URL, r *crawlResult) (yes bool) {
	u, err := url.Parse(r.URI)
	if err != nil {
		return
	}

	if !canCrawl(base, u, c.cfg.Depth, c.cfg.Subdomains) ||
		c.robots.Forbidden(u.Path) ||
		(c.cfg.Dirs == DirsOnly && isResorce(u.Path)) {
		return
	}

	t := time.NewTimer(chTimeout)
	defer t.Stop()

	select {
	case c.crawlCh <- u:
		return true
	case <-t.C:
	}

	return false
}

// enqueueStyle schedules a stylesheet entry for direct GET scanning. Unlike
// tryEnqueue it does not apply the resource-oriented directory policy, it
// still honours crawl scope and robots rules. A stylesheet is only enqueued
// once per canonical url even if it merges from several places or forms a
// cycle, but its discovery is always reported upstream.
func (c *Crawler) enqueueStyle(base *url.URL, r *crawlResult) (yes bool) {
	if c.styleEnq == nil {
		return false
	}

	u, err := url.Parse(r.URI)
	if err != nil {
		return false
	}

	if !canCrawl(base, u, c.cfg.Depth, c.cfg.Subdomains) ||
		c.robots.Forbidden(u.Path) ||
		c.isIgnored(r.URI) ||
		(c.cfg.Dirs == DirsOnly && isResorce(u.Path)) {
		return false
	}

	if _, ok := c.styleEnq[r.Hash]; ok {
		return false
	}

	t := time.NewTimer(chTimeout)
	defer t.Stop()

	select {
	case c.styleCh <- u:
		c.styleEnq[r.Hash] = struct{}{}

		return true
	case <-t.C:
	}

	return false
}

func (c *Crawler) close() {
	if c.crawlCh != nil {
		close(c.crawlCh)
	}

	if c.styleCh != nil {
		close(c.styleCh)
	}

	c.wg.Wait() // wait for crawlers

	c.wg.Add(1) // for handler`s Done()
	close(c.handleCh)
	c.wg.Wait() // wait for handler

	close(c.resultCh)
}

func (c *Crawler) initRobots(host *url.URL, web crawlClient) {
	if c.cfg.Robots == RobotsIgnore {
		return
	}

	ctx, cancel := context.WithTimeout(context.Background(), c.cfg.Client.Timeout)
	defer cancel()

	body, _, err := web.Get(ctx, robots.URL(host))
	if err != nil {
		var herr client.HTTPError

		if !errors.As(err, &herr) {
			log.Println("[-] GET /robots.txt:", err)

			return
		}

		if herr.Code() == http.StatusInternalServerError {
			c.robots = robots.DenyALL()
		}

		return
	}

	defer body.Close()

	rbt, err := robots.FromReader(c.cfg.Client.UserAgent, body)
	if err != nil {
		log.Println("[-] parse robots.txt:", err)

		return
	}

	c.robots = rbt

	c.crawlRobots(host)
}

func (c *Crawler) crawlRobots(host *url.URL) {
	base := *host
	base.Fragment = ""
	base.RawQuery = ""

	for _, u := range c.robots.Links() {
		t := base
		t.Path = u

		c.linkHandler(atom.A, t.String())
	}

	for _, u := range c.robots.Sitemaps() {
		if _, e := url.Parse(u); e == nil {
			c.crawlHandler(u)
		}
	}
}

func (c *Crawler) isIgnored(v string) (yes bool) {
	if len(c.cfg.Ignored) == 0 {
		return
	}

	return slices.ContainsFunc(c.cfg.Ignored, func(s string) bool {
		return strings.Contains(v, s)
	})
}

func (c *Crawler) linkHandler(a atom.Atom, s string) {
	c.emitLink(a, s, false)
}

func (c *Crawler) emitLink(a atom.Atom, s string, style bool) {
	r := crawlResult{
		URI:   s,
		Hash:  urlhash(s),
		Style: style,
	}

	fetch := (a == atom.A || a == atom.Iframe) ||
		(c.cfg.ScanJS && a == atom.Script) ||
		(c.cfg.ScanCSS && a == atom.Link)

	if fetch && !c.isIgnored(s) {
		r.Flag = TaskCrawl
	}

	t := time.NewTimer(chTimeout)
	defer t.Stop()

	select {
	case c.resultCh <- r:
	case <-t.C:
	}
}

// emitOnly reports an address without making it a download candidate.
func (c *Crawler) emitOnly(s string) {
	t := time.NewTimer(chTimeout)
	defer t.Stop()

	select {
	case c.resultCh <- crawlResult{URI: s, Hash: urlhash(s)}:
	case <-t.C:
	}
}

// styleHandler reports a stylesheet entry (an external stylesheet link or an
// @import target). Style entries always carry their type into the scheduler
// and worker, regardless of the file extension.
func (c *Crawler) styleHandler(s string) {
	c.emitLink(atom.Link, s, true)
}

func (c *Crawler) staticHandler(s string) {
	c.emitLink(atom.Link, s, false)
}

func (c *Crawler) crawlHandler(s string) {
	c.emitLink(atom.A, s, false)
}

// processAs fetches and parses a document. knownStyle tells that the url is a
// stylesheet entry discovered via html or @import, so it is parsed as css no
// matter what the server declares.
func (c *Crawler) processAs(
	ctx context.Context,
	web crawlClient,
	base *url.URL,
	uri string,
	knownStyle bool,
) {
	body, hdrs, err := web.Get(ctx, uri)
	if err != nil {
		var herr client.HTTPError

		// ignore any http errors, just parse body (if any)
		if !errors.As(err, &herr) {
			log.Printf("[-] GET %s: %v", uri, err)

			return
		}
	}

	if body == nil {
		return
	}

	cssImports := c.cfg.CSSImports && c.cfg.ScanCSS

	// resolve turns a raw reference found in the document at uri into an
	// absolute address using that document as the base.
	resolve := func(s string) (rv string, ok bool) {
		switch {
		case strings.HasPrefix(s, doubleDash):
			return base.Scheme + s, true
		case strings.Contains(s, doubleDash):
			return s, true
		default:
			return resolveRef(uri, s)
		}
	}

	// handleStatic keeps the legacy behaviour: resources are reported and may
	// become crawl candidates depending on the enabled scanners.
	handleStatic := func(s string) {
		if s, ok := resolve(s); ok {
			c.staticHandler(s)
		}
	}

	// handleStyleRef resolves a css reference against the document it was
	// found in, using the same url cleaning rules as html extraction. Both
	// imports and resources are reported; the scheduler applies the ignore
	// policy to fetching, matching plain html links.
	handleStyleRef := func(ref links.CSSReference) {
		b, err := url.Parse(uri)
		if err != nil {
			return
		}

		s, ok := links.CleanURL(b, ref.URL)
		if !ok {
			return
		}

		if ref.Import {
			c.styleHandler(s)

			return
		}

		c.emitOnly(s)
	}

	content := hdrs.Get(contentType)

	switch {
	case knownStyle:
		// A known stylesheet entry is always parsed as css, except when the
		// server explicitly declares a non-css type.
		if cssImports && isCSSContent(content) {
			links.ExtractCSSReferences(body, handleStyleRef)
		}
	case isHTML(content):
		params := links.HTMLParams{
			Brute:        c.cfg.Brute,
			ScanJS:       c.cfg.ScanJS,
			ScanCSS:      c.cfg.ScanCSS,
			Filter:       c.filter,
			HandleHTML:   c.linkHandler,
			HandleStatic: handleStatic,
		}

		if cssImports {
			// In dependency collection mode every inline style block and every
			// downloaded stylesheet goes through the reference extractor.
			// Resources are report-only, imports are scheduled as stylesheets.
			params.HandleCSS = handleStyleRef

			// Only links whose rel contains "stylesheet" are style entries;
			// other links fall back to the regular handler and are still
			// reported (and crawled as before), and pre-existing tag filtering
			// still applies.
			params.HandleStyleSheet = func(_ atom.Atom, ref string) {
				b, err := url.Parse(uri)
				if err != nil {
					return
				}

				s, ok := links.CleanURL(b, ref)
				if !ok {
					return
				}

				c.styleHandler(s)
			}

			// Non-stylesheet links are still reported, but css scanning must
			// not turn them into download candidates.
			params.HandleHTML = func(a atom.Atom, s string) {
				if a == atom.Link {
					c.emitOnly(s)

					return
				}

				c.linkHandler(a, s)
			}
		}

		links.ExtractHTML(body, base, params)
	case isSitemap(uri):
		links.ExtractSitemap(body, base, c.crawlHandler)
	case c.cfg.ScanJS && isJS(content, uri):
		links.ExtractJS(body, handleStatic)
	case c.cfg.ScanCSS && isCSS(content, uri):
		if cssImports {
			links.ExtractCSSReferences(body, handleStyleRef)
		} else {
			links.ExtractCSS(body, handleStatic)
		}
	}

	client.Discard(body)
}

func (c *Crawler) worker(web crawlClient) {
	defer c.wg.Done()

	crawl, style := c.crawlCh, c.styleCh

	for crawl != nil || style != nil {
		select {
		case uri, ok := <-crawl:
			if !ok {
				crawl = nil

				continue
			}

			c.crawlOne(web, uri, false)
		case uri, ok := <-style:
			if !ok {
				style = nil

				continue
			}

			c.crawlOne(web, uri, true)
		}
	}
}

func (c *Crawler) crawlOne(web crawlClient, uri *url.URL, style bool) {
	if c.cfg.Delay > 0 {
		time.Sleep(c.cfg.Delay)
	}

	ctx, cancel := context.WithTimeout(context.Background(), c.cfg.Client.Timeout)
	us := uri.String()

	var canProcess bool

	if style {
		// Known stylesheet entries are fetched directly, no pre-flight HEAD.
		canProcess = true
	} else if c.cfg.NoHEAD {
		canProcess = canParse(uri.Path)
	} else {
		if hdrs, err := web.Head(ctx, us); err != nil {
			log.Printf("[-] HEAD %s: %v", us, err)
		} else {
			ct := hdrs.Get(contentType)

			canProcess = isHTML(ct) ||
				isSitemap(us) ||
				(c.cfg.ScanJS && isJS(ct, us)) ||
				(c.cfg.ScanCSS && isCSS(ct, us))
		}
	}

	if canProcess {
		c.processAs(ctx, web, uri, us, style)
	}

	cancel()

	c.resultCh <- crawlResult{Flag: TaskDone, Style: style}
}
