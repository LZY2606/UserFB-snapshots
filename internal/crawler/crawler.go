package crawler

import (
	"bytes"
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

// crawlClientEx is an optional client extension able to report
// the final (after redirects) URL of a GET request.
type crawlClientEx interface {
	crawlClient
	GetURL(context.Context, string) (io.ReadCloser, http.Header, string, error)
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
	// TaskEdge marks result as an identity edge between two URLs.
	TaskEdge
)

type crawlResult struct {
	URI  string
	Hash uint64
	Flag taskFlag
	Edge string // edge target, for TaskEdge only
}

// Crawler holds crawling process config and state.
type Crawler struct {
	cfg      *config
	handleCh chan string
	crawlCh  chan *url.URL
	resultCh chan crawlResult
	robots   *robots.TXT
	filter   links.TokenFilter
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

	n := (workers + 1)
	c.handleCh = make(chan string, n*chMult)
	c.crawlCh = make(chan *url.URL, n*chMult)
	c.resultCh = make(chan crawlResult, n*chMult)

	defer c.close()

	seen := make(set.Unordered[uint64])
	seen.Add(urlhash(uri))

	web := client.New(&c.cfg.Client)
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

	if c.cfg.Canonical {
		c.runCanonical(base, urlcb)

		return nil
	}

	var t crawlResult

	for w := 1; w > 0; {
		t = <-c.resultCh

		switch {
		case t.Flag == TaskDone:
			w--
		case seen.Add(t.Hash):
			if t.Flag == TaskCrawl && c.tryEnqueue(base, &t) {
				w++
			}

			c.tryHandle(t.URI)
		}
	}

	return nil
}

// runCanonical is the scheduler loop for canonical-links mode: identity edges
// are merged as they arrive, dispatch dedup happens per equivalence group and
// all output is deferred until every dispatched task and its discovery events
// are fully processed.
func (c *Crawler) runCanonical(base *url.URL, urlcb func(string)) {
	cs := newCanonicalState()
	cs.dispatch(urlIdentity(base.String()))

	for w := 1; w > 0; {
		t := <-c.resultCh

		switch t.Flag {
		case TaskDone:
			w--

		case TaskEdge:
			cs.addEdge(t.URI, t.Edge)

		default:
			if c.showable(t.URI) {
				cs.addCandidate(t.URI)
			}

			if t.Flag == TaskCrawl &&
				!cs.dispatchedTo(urlIdentity(t.URI)) &&
				c.tryEnqueue(base, &t) {
				w++
				cs.dispatch(urlIdentity(t.URI))
			}
		}
	}

	if urlcb == nil {
		return
	}

	for _, u := range cs.results() {
		urlcb(u)
	}
}

// DumpConfig returns internal config representation.
func (c *Crawler) DumpConfig() string {
	return c.cfg.String()
}

func (c *Crawler) showable(u string) (show bool) {
	idx := strings.LastIndexByte(u, '/')
	if idx == -1 {
		return
	}

	show = true

	switch c.cfg.Dirs {
	case DirsHide:
		show = isResorce(u[idx:])
	case DirsOnly:
		show = !isResorce(u[idx:])
	}

	return
}

func (c *Crawler) tryHandle(u string) {
	if !c.showable(u) {
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

func (c *Crawler) close() {
	close(c.crawlCh)
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
	r := crawlResult{
		URI:  s,
		Hash: urlhash(s),
	}

	fetch := (a == atom.A || a == atom.Iframe) ||
		(c.cfg.ScanJS && a == atom.Script) ||
		(c.cfg.ScanCSS && a == atom.Link)

	if fetch && !c.isIgnored(s) {
		r.Flag = TaskCrawl
	}

	if c.cfg.Canonical {
		// no timed sends in canonical mode - discoveries must not be lost
		c.resultCh <- r

		return
	}

	t := time.NewTimer(chTimeout)
	defer t.Stop()

	select {
	case c.resultCh <- r:
	case <-t.C:
	}
}

func (c *Crawler) staticHandler(s string) {
	c.linkHandler(atom.Link, s)
}

func (c *Crawler) crawlHandler(s string) {
	c.linkHandler(atom.A, s)
}

func (c *Crawler) fetch(
	ctx context.Context,
	web crawlClient,
	uri string,
) (body io.ReadCloser, hdrs http.Header, finalURL string, err error) {
	if c.cfg.Canonical {
		if ex, ok := web.(crawlClientEx); ok {
			return ex.GetURL(ctx, uri)
		}
	}

	body, hdrs, err = web.Get(ctx, uri)
	finalURL = uri

	return
}

func (c *Crawler) staticResolver(scheme, docURL string) links.URLHandler {
	return func(s string) {
		var ok bool

		switch {
		case strings.HasPrefix(s, doubleDash):
			s, ok = scheme+s, true
		case strings.Contains(s, doubleDash):
			ok = true
		default:
			s, ok = resolveRef(docURL, s)
		}

		if ok {
			c.staticHandler(s)
		}
	}
}

func (c *Crawler) emitEdge(from, to string) {
	c.resultCh <- crawlResult{
		Flag: TaskEdge,
		URI:  from,
		Edge: to,
	}
}

// recordRedirect stores request-start -> final-document identity edge,
// both endpoints must share the same origin.
func (c *Crawler) recordRedirect(req string, doc *url.URL) {
	from, ok := normalizeURL(req)
	if !ok {
		return
	}

	to := urlIdentity(doc.String())
	if from == to {
		return
	}

	src, err := url.Parse(req)
	if err != nil || !sameOrigin(src, doc) {
		return
	}

	c.emitEdge(from, to)
}

// processHTMLCanonical handles HTML body in canonical mode: head control
// declarations (only from successful responses) become identity edges,
// ordinary links are resolved against the effective head base.
func (c *Crawler) processHTMLCanonical(body io.Reader, doc *url.URL, declare bool) {
	data, err := io.ReadAll(body)
	if err != nil {
		return
	}

	base := doc

	if declare {
		head := links.ExtractHead(bytes.NewReader(data))
		base = resolveHeadBase(doc, head)

		if target, ok := pickCanonical(doc, base, head.Canonicals); ok {
			c.emitEdge(urlIdentity(doc.String()), urlIdentity(target))
		}
	}

	links.ExtractHTML(bytes.NewReader(data), base, links.HTMLParams{
		Brute:        c.cfg.Brute,
		ScanJS:       c.cfg.ScanJS,
		ScanCSS:      c.cfg.ScanCSS,
		Filter:       withoutCanonicalLinks(c.filter),
		HandleHTML:   c.linkHandler,
		HandleStatic: c.staticResolver(doc.Scheme, doc.String()),
	})
}

func (c *Crawler) processCanonical(
	body io.ReadCloser,
	content, uri string,
	doc *url.URL,
	success bool,
) {
	switch {
	case isHTML(content):
		c.processHTMLCanonical(body, doc, success)
	case isSitemap(uri):
		links.ExtractSitemap(body, doc, c.crawlHandler)
	case c.cfg.ScanJS && isJS(content, uri):
		links.ExtractJS(body, c.staticResolver(doc.Scheme, doc.String()))
	case c.cfg.ScanCSS && isCSS(content, uri):
		links.ExtractCSS(body, c.staticResolver(doc.Scheme, doc.String()))
	}
}

func (c *Crawler) process(
	ctx context.Context,
	web crawlClient,
	base *url.URL,
	uri string,
) {
	body, hdrs, finalURL, err := c.fetch(ctx, web, uri)
	if err != nil {
		var herr client.HTTPError

		// ignore any http errors, just parse body (if any)
		if !errors.As(err, &herr) {
			log.Printf("[-] GET %s: %v", uri, err)

			return
		}
	}

	content := hdrs.Get(contentType)

	if c.cfg.Canonical {
		doc := base

		if u, perr := url.Parse(finalURL); perr == nil {
			doc = u
		}

		if err == nil {
			c.recordRedirect(uri, doc)
		}

		c.processCanonical(body, content, uri, doc, err == nil)
		client.Discard(body)

		return
	}

	handleStatic := c.staticResolver(base.Scheme, uri)

	switch {
	case isHTML(content):
		links.ExtractHTML(body, base, links.HTMLParams{
			Brute:        c.cfg.Brute,
			ScanJS:       c.cfg.ScanJS,
			ScanCSS:      c.cfg.ScanCSS,
			Filter:       c.filter,
			HandleHTML:   c.linkHandler,
			HandleStatic: handleStatic,
		})
	case isSitemap(uri):
		links.ExtractSitemap(body, base, c.crawlHandler)
	case c.cfg.ScanJS && isJS(content, uri):
		links.ExtractJS(body, handleStatic)
	case c.cfg.ScanCSS && isCSS(content, uri):
		links.ExtractCSS(body, handleStatic)
	}

	client.Discard(body)
}

func (c *Crawler) worker(web crawlClient) {
	defer c.wg.Done()

	for uri := range c.crawlCh {
		if c.cfg.Delay > 0 {
			time.Sleep(c.cfg.Delay)
		}

		ctx, cancel := context.WithTimeout(context.Background(), c.cfg.Client.Timeout)
		us := uri.String()

		var canProcess bool

		if c.cfg.NoHEAD {
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
			c.process(ctx, web, uri, us)
		}

		cancel()

		c.resultCh <- crawlResult{Flag: TaskDone}
	}
}
