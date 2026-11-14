package gocrawl_test

import (
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"sync"
	"testing"
	"time"

	crawl "github.com/PuerkitoBio/gocrawl"
	"github.com/PuerkitoBio/gocrawl/task_support/taskcheck"
	"github.com/PuerkitoBio/goquery"
)

// oneShot is a channel closed at most once.
type oneShot struct {
	once sync.Once
	ch   chan struct{}
}

func newOneShot() *oneShot { return &oneShot{ch: make(chan struct{})} }

func (o *oneShot) fire() { o.once.Do(func() { close(o.ch) }) }

func (o *oneShot) done() <-chan struct{} { return o.ch }

// layerRecorder records the requests received by the test HTTP servers.
type layerRecorder struct {
	mu     sync.Mutex
	counts map[string]int
	events []string
}

func newLayerRecorder() *layerRecorder {
	return &layerRecorder{counts: make(map[string]int)}
}

func (r *layerRecorder) record(method, path string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.counts[method+" "+path]++
	r.events = append(r.events, method+" "+path)
}

func (r *layerRecorder) count(method, path string) int {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.counts[method+" "+path]
}

func (r *layerRecorder) snapshot() map[string]int {
	r.mu.Lock()
	defer r.mu.Unlock()
	c := make(map[string]int, len(r.counts))
	for k, v := range r.counts {
		c[k] = v
	}
	return c
}

func (r *layerRecorder) since(before map[string]int, method, path string) int {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.counts[method+" "+path] - before[method+" "+path]
}

func (r *layerRecorder) eventsCopy() []string {
	r.mu.Lock()
	defer r.mu.Unlock()
	return append([]string(nil), r.events...)
}

// readFailBody turns a successful response body into a read error.
type readFailBody struct{ io.ReadCloser }

func (b *readFailBody) Read([]byte) (int, error) {
	return 0, errors.New("simulated body read error")
}

// layerProbe is the extender used by the layered tests. It records what the
// crawler sees and delegates the actual HTTP work to DefaultExtender.
type layerProbe struct {
	crawl.DefaultExtender

	mu         sync.Mutex
	run        int
	starts     int
	ends       int
	lastEnd    error
	depths     map[string]int // "<run>|<host><path>" -> depth seen at fetch/visit
	docOK      map[string]bool
	visitState map[string]interface{}
	visitSrc   map[string]string
	visitHead  map[string]bool
	filterCnt  map[string]int
	reqGetCnt  map[string]int
	disallowed map[string]int
	errKinds   map[crawl.CrawlErrorKind]int

	headPaths  map[string]bool
	denyGet    map[string]bool
	brokenPath string

	onVisit   func(*crawl.URLContext, *http.Response, *goquery.Document) (interface{}, bool)
	onVisited func(*crawl.URLContext)
	onFilter  func(*crawl.URLContext, bool) bool
}

func newLayerProbe() *layerProbe {
	return &layerProbe{
		depths:     make(map[string]int),
		docOK:      make(map[string]bool),
		visitState: make(map[string]interface{}),
		visitSrc:   make(map[string]string),
		visitHead:  make(map[string]bool),
		filterCnt:  make(map[string]int),
		reqGetCnt:  make(map[string]int),
		disallowed: make(map[string]int),
		errKinds:   make(map[crawl.CrawlErrorKind]int),
		headPaths:  make(map[string]bool),
		denyGet:    make(map[string]bool),
	}
}

func keyOf(ctx *crawl.URLContext) string {
	return ctx.URL().Host + ctx.URL().Path
}

func (p *layerProbe) depthOf(run int, hostPath string) int {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.depths[fmt.Sprintf("%d|%s", run, hostPath)]
}

func (p *layerProbe) recordDepth(ctx *crawl.URLContext) {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.depths[fmt.Sprintf("%d|%s", p.run, keyOf(ctx))] = ctx.Depth()
}

func (p *layerProbe) Start(seeds interface{}) interface{} {
	p.mu.Lock()
	p.run++
	p.starts++
	p.mu.Unlock()
	return seeds
}

func (p *layerProbe) End(err error) {
	p.mu.Lock()
	p.ends++
	p.lastEnd = err
	p.mu.Unlock()
}

func (p *layerProbe) Fetch(ctx *crawl.URLContext, ua string, head bool) (*http.Response, error) {
	p.recordDepth(ctx)
	res, err := p.DefaultExtender.Fetch(ctx, ua, head)
	p.mu.Lock()
	broken := p.brokenPath != "" && ctx.URL().Path == p.brokenPath && !head
	p.mu.Unlock()
	if err == nil && broken && res != nil {
		res.Body = &readFailBody{res.Body}
	}
	return res, err
}

func (p *layerProbe) Filter(ctx *crawl.URLContext, isVisited bool) bool {
	k := keyOf(ctx)
	p.mu.Lock()
	p.filterCnt[k]++
	if p.headPaths[k] {
		ctx.HeadBeforeGet = true
	}
	hook := p.onFilter
	p.mu.Unlock()
	if hook != nil {
		return hook(ctx, isVisited)
	}
	return !isVisited
}

func (p *layerProbe) RequestGet(ctx *crawl.URLContext, res *http.Response) bool {
	k := keyOf(ctx)
	p.mu.Lock()
	p.reqGetCnt[k]++
	deny := p.denyGet[k]
	p.mu.Unlock()
	if deny {
		return false
	}
	return res.StatusCode >= 200 && res.StatusCode < 300
}

func (p *layerProbe) Visit(ctx *crawl.URLContext, res *http.Response, doc *goquery.Document) (interface{}, bool) {
	k := keyOf(ctx)
	p.recordDepth(ctx)
	p.mu.Lock()
	p.docOK[k] = doc != nil
	p.visitState[k] = ctx.State
	p.visitHead[k] = ctx.HeadBeforeGet
	if src := ctx.SourceURL(); src != nil {
		p.visitSrc[k] = src.String()
	}
	hook := p.onVisit
	p.mu.Unlock()
	if hook != nil {
		return hook(ctx, res, doc)
	}
	return nil, true
}

func (p *layerProbe) Visited(ctx *crawl.URLContext, harvested interface{}) {
	p.mu.Lock()
	hook := p.onVisited
	p.mu.Unlock()
	if hook != nil {
		hook(ctx)
	}
}

func (p *layerProbe) Disallowed(ctx *crawl.URLContext) {
	p.mu.Lock()
	p.disallowed[keyOf(ctx)]++
	p.mu.Unlock()
}

func (p *layerProbe) Error(err *crawl.CrawlError) {
	p.mu.Lock()
	p.errKinds[err.Kind]++
	p.mu.Unlock()
}

func (p *layerProbe) errCount(kind crawl.CrawlErrorKind) int {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.errKinds[kind]
}

func (p *layerProbe) filterCount(hostPath string) int {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.filterCnt[hostPath]
}

func (p *layerProbe) disallowedCount(hostPath string) int {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.disallowed[hostPath]
}

func (p *layerProbe) visitDocOK(hostPath string) bool {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.docOK[hostPath]
}

func (p *layerProbe) stateOf(hostPath string) interface{} {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.visitState[hostPath]
}

func (p *layerProbe) sourceOf(hostPath string) string {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.visitSrc[hostPath]
}

func (p *layerProbe) headOf(hostPath string) bool {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.visitHead[hostPath]
}

func (p *layerProbe) startCount() int {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.starts
}

func (p *layerProbe) endCount() (int, error) {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.ends, p.lastEnd
}

func newLayeredCrawler(p *layerProbe) *crawl.Crawler {
	opts := crawl.NewOptions(p)
	opts.Layered = true
	opts.SameHostOnly = false
	opts.CrawlDelay = 0
	opts.WorkerIdleTTL = 0
	opts.LogFlags = crawl.LogNone
	return crawl.NewCrawlerWithOptions(opts)
}

func mustHost(raw string) string {
	u, err := url.Parse(raw)
	if err != nil {
		panic(err)
	}
	return u.Host
}

func allowRobots(w http.ResponseWriter, r *http.Request) {
	fmt.Fprint(w, "User-agent: *\nAllow: /\n")
}

func waitClosed(ch <-chan struct{}, d time.Duration) bool {
	select {
	case <-ch:
		return true
	case <-time.After(d):
		return false
	}
}

func waitCond(d time.Duration, cond func() bool) bool {
	deadline := time.Now().Add(d)
	for time.Now().Before(deadline) {
		if cond() {
			return true
		}
		time.Sleep(time.Millisecond)
	}
	return cond()
}

func closed(ch <-chan struct{}) bool {
	select {
	case <-ch:
		return true
	default:
		return false
	}
}

func TestLayerBarrier(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		rec := newLayerRecorder()

		rootAStarted := newOneShot()
		rootBStarted := newOneShot()
		aRootVisited := newOneShot()
		childAStarted := newOneShot()
		childBStarted := newOneShot()
		cRobotsStarted := newOneShot()
		cPageStarted := newOneShot()

		releaseRootB := make(chan struct{})
		var releaseRootBOnce sync.Once
		unblockRootB := func() { releaseRootBOnce.Do(func() { close(releaseRootB) }) }
		releaseChildB := make(chan struct{})
		var releaseChildBOnce sync.Once
		unblockChildB := func() { releaseChildBOnce.Do(func() { close(releaseChildB) }) }

		// Source A: one of the two seeds, fast.
		aSrv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			switch r.URL.Path {
			case "/robots.txt":
				rec.record(r.Method, "/a/robots.txt")
				allowRobots(w, r)
			case "/root":
				rec.record(r.Method, "/a/root")
				rootAStarted.fire()
				fmt.Fprint(w, `<a href="/child">child</a>`)
			case "/child":
				rec.record(r.Method, "/a/child")
				childAStarted.fire()
				fmt.Fprint(w, `<a href="/leaf">leaf</a>`)
			case "/leaf":
				rec.record(r.Method, "/a/leaf")
				fmt.Fprint(w, "leaf")
			default:
				http.NotFound(w, r)
			}
		}))
		defer aSrv.Close()

		// Source B: the other seed, deliberately slow.
		bSrv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			switch r.URL.Path {
			case "/robots.txt":
				rec.record(r.Method, "/b/robots.txt")
				allowRobots(w, r)
			case "/root":
				rec.record(r.Method, "/b/root")
				rootBStarted.fire()
				<-releaseRootB
				fmt.Fprint(w, `<a href="/child">child</a>`)
			case "/child":
				rec.record(r.Method, "/b/child")
				childBStarted.fire()
				<-releaseChildB
				fmt.Fprint(w, "child")
			default:
				http.NotFound(w, r)
			}
		}))
		defer bSrv.Close()

		// Source C: discovered during layer 0, must not jump the barrier.
		cSrv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			switch r.URL.Path {
			case "/robots.txt":
				rec.record(r.Method, "/c/robots.txt")
				cRobotsStarted.fire()
				allowRobots(w, r)
			case "/page":
				rec.record(r.Method, "/c/page")
				cPageStarted.fire()
				fmt.Fprint(w, "page")
			case "/extra":
				rec.record(r.Method, "/c/extra")
				fmt.Fprint(w, "extra")
			default:
				http.NotFound(w, r)
			}
		}))
		defer cSrv.Close()
		defer unblockRootB()
		defer unblockChildB()

		hostA, hostB, hostC := mustHost(aSrv.URL), mustHost(bSrv.URL), mustHost(cSrv.URL)

		p := newLayerProbe()
		// HEAD-then-GET for the enqueued URL must remain inside its layer.
		p.headPaths[hostC+"/extra"] = true
		var missedConcurrency bool
		p.onVisit = func(ctx *crawl.URLContext, res *http.Response, doc *goquery.Document) (interface{}, bool) {
			if ctx.URL().Host == hostA && ctx.URL().Path == "/root" {
				// The two seeds belong to the same layer and must be
				// requested concurrently: wait for B's root to be in flight
				// while A's Visit is still running.
				if !waitClosed(rootBStarted.done(), 3*time.Second) {
					missedConcurrency = true
				}
				// Buffered EnqueueChan message: belongs to layer 1.
				p.EnqueueChan <- cSrv.URL + "/extra"
				// Harvested addresses: belong to layer 1.
				child, _ := url.Parse(aSrv.URL + "/child")
				page, _ := url.Parse(cSrv.URL + "/page")
				return []*url.URL{child, page}, false
			}
			return nil, true
		}
		p.onVisited = func(ctx *crawl.URLContext) {
			if ctx.URL().Host == hostA && ctx.URL().Path == "/root" {
				aRootVisited.fire()
			}
		}

		cr := newLayeredCrawler(p)
		finished := make(chan struct{})
		var runErr error
		go func() {
			runErr = cr.Run([]string{aSrv.URL + "/root", bSrv.URL + "/root"})
			close(finished)
		}()

		c.Check(waitClosed(rootAStarted.done(), 5*time.Second), "source A layer-0 request should start")
		c.Check(waitClosed(rootBStarted.done(), 5*time.Second), "source B layer-0 request should start")
		c.Check(!missedConcurrency, "both sources must be fetched concurrently within layer 0")
		c.Check(waitClosed(aRootVisited.done(), 5*time.Second), "A root should finish its Visit and Visited hooks")

		// While B's root is still pending, no layer-1 work may start, and the
		// robots.txt of the host discovered in layer 0 must not jump ahead.
		c.Check(rec.count("GET", "/a/child") == 0, "A child must not be fetched before layer 0 completes")
		c.Check(rec.count("GET", "/c/extra") == 0, "buffered enqueue must not be fetched before layer 0 completes")
		c.Check(!closed(cRobotsStarted.done()), "new host robots.txt must not jump ahead of the layer barrier")
		c.Check(rec.count("GET", "/c/page") == 0, "harvested layer-1 URL must not be fetched before layer 0 completes")
		c.Check(!closed(finished), "a pending response in the current layer must not end the run")

		unblockRootB()
		c.Check(waitClosed(childAStarted.done(), 5*time.Second), "layer 1 should start fetching A child")
		c.Check(waitClosed(childBStarted.done(), 5*time.Second), "layer 1 should fetch B child on the other source")
		c.Check(waitCond(5*time.Second, func() bool { return rec.count("GET", "/c/extra") == 1 }), "buffered enqueue must be drained in layer 1")
		c.Check(waitClosed(cRobotsStarted.done(), 5*time.Second), "layer 1 should fetch robots.txt for the new host")
		c.Check(waitClosed(cPageStarted.done(), 5*time.Second), "layer 1 should fetch the harvested URL")
		c.Check(rec.count("HEAD", "/c/extra") == 1 && rec.count("GET", "/c/extra") == 1,
			"HEAD then GET of the same page must stay in one layer (HEAD=1 GET=1)")

		// Layer 2 must wait for all of layer 1, including B's blocked child.
		c.Check(rec.count("GET", "/a/leaf") == 0, "layer 2 must not start while layer 1 has a pending response")
		c.Check(!closed(finished), "a pending response in layer 1 must not end the run")
		unblockChildB()

		c.Check(waitClosed(finished, 10*time.Second), "run should finish once every layer completes")
		c.Check(runErr == nil, "layered run should return nil, got %v", runErr)

		// Depths.
		c.Check(p.depthOf(1, hostA+"/root") == 0, "seed A root should be depth 0")
		c.Check(p.depthOf(1, hostB+"/root") == 0, "seed B root should be depth 0")
		c.Check(p.depthOf(1, hostA+"/child") == 1, "harvested A child should be depth 1")
		c.Check(p.depthOf(1, hostB+"/child") == 1, "harvested B child should be depth 1")
		c.Check(p.depthOf(1, hostC+"/extra") == 1, "EnqueueChan URL should join the next layer (depth 1)")
		c.Check(p.depthOf(1, hostC+"/page") == 1, "harvested cross-host URL should be depth 1")
		c.Check(p.depthOf(1, hostA+"/leaf") == 2, "grand-child should be depth 2")

		// Single physical scheduling.
		c.Check(rec.count("GET", "/a/root") == 1, "A root fetched once")
		c.Check(rec.count("GET", "/b/root") == 1, "B root fetched once")
		c.Check(rec.count("GET", "/a/child") == 1, "A child fetched once")
		c.Check(rec.count("GET", "/b/child") == 1, "B child fetched once")
		c.Check(rec.count("GET", "/a/leaf") == 1, "A leaf fetched once")
		c.Check(rec.count("GET", "/c/page") == 1, "C page fetched once")
		c.Check(rec.count("GET", "/c/extra") == 1, "enqueued URL fetched once")
	})
}

func TestLayerRedirectPromotion(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		rec := newLayerRecorder()
		sharedReserved := newOneShot()
		allowRedirect := make(chan struct{})
		var allowRedirectOnce sync.Once
		unblockRedirect := func() { allowRedirectOnce.Do(func() { close(allowRedirect) }) }

		// Source A discovers /shared and, separately, redirects and loops.
		aSrv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			switch r.URL.Path {
			case "/robots.txt":
				rec.record(r.Method, "/a/robots.txt")
				allowRobots(w, r)
			case "/root":
				rec.record(r.Method, "/a/root")
				fmt.Fprint(w, "root")
			case "/alt":
				rec.record(r.Method, "/a/alt")
				http.Redirect(w, r, "/other", http.StatusFound)
			case "/other":
				rec.record(r.Method, "/a/other")
				fmt.Fprint(w, "other")
			case "/loop1":
				rec.record(r.Method, "/a/loop1")
				http.Redirect(w, r, "/loop2", http.StatusFound)
			case "/loop2":
				rec.record(r.Method, "/a/loop2")
				http.Redirect(w, r, "/loop1", http.StatusFound)
			default:
				http.NotFound(w, r)
			}
		}))
		defer aSrv.Close()

		// Source B, in the same layer, redirects to A's /shared reservation.
		bSrv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			switch r.URL.Path {
			case "/robots.txt":
				rec.record(r.Method, "/b/robots.txt")
				allowRobots(w, r)
			case "/root":
				rec.record(r.Method, "/b/root")
				<-allowRedirect
				http.Redirect(w, r, "/shared", http.StatusFound)
			case "/shared":
				rec.record(r.Method, "/b/shared")
				fmt.Fprint(w, "shared")
			default:
				http.NotFound(w, r)
			}
		}))
		defer bSrv.Close()
		defer unblockRedirect()

		hostA, hostB := mustHost(aSrv.URL), mustHost(bSrv.URL)

		p := newLayerProbe()
		sharedKey := hostB + "/shared"
		p.headPaths[sharedKey] = true
		p.onFilter = func(ctx *crawl.URLContext, isVisited bool) bool {
			if keyOf(ctx) == sharedKey {
				sharedReserved.fire()
				unblockRedirect()
			}
			return !isVisited
		}
		sharedURL := bSrv.URL + "/shared"
		p.onVisit = func(ctx *crawl.URLContext, res *http.Response, doc *goquery.Document) (interface{}, bool) {
			if ctx.URL().Host == hostA && ctx.URL().Path == "/root" {
				return crawl.S{sharedURL: "shared-state"}, false
			}
			return nil, true
		}

		cr := newLayeredCrawler(p)
		finished := make(chan struct{})
		var runErr error
		go func() {
			runErr = cr.Run([]string{
				aSrv.URL + "/root",
				aSrv.URL + "/alt",
				aSrv.URL + "/loop1",
				bSrv.URL + "/root",
			})
			close(finished)
		}()

		// /shared is first discovered by A/root and reserved for layer 1.
		// Only then may the in-layer request redirect to it.
		c.Check(waitClosed(sharedReserved.done(), 5*time.Second), "shared URL should be reserved by the first discovery")
		unblockRedirect()
		c.Check(waitClosed(finished, 10*time.Second), "redirect promotion run should finish")
		c.Check(runErr == nil, "run should return nil, got %v", runErr)

		// Promotion to the current layer, no duplicate physical download.
		c.Check(p.depthOf(1, sharedKey) == 0, "promoted redirect target should be in the current layer (depth 0)")
		c.Check(rec.count("GET", "/b/shared") == 1, "promoted URL must be downloaded exactly once (GET=1)")
		c.Check(rec.count("HEAD", "/b/shared") == 1, "promoted URL must be scheduled once (HEAD=1)")
		c.Check(p.filterCount(sharedKey) == 1, "promotion must not re-run the Filter on an already admitted URL, got %d", p.filterCount(sharedKey))
		c.Check(p.stateOf(sharedKey) == "shared-state", "promotion must keep the first-admission State, got %v", p.stateOf(sharedKey))
		c.Check(p.sourceOf(sharedKey) == aSrv.URL+"/root", "promotion must keep the first-admission SourceURL, got %q", p.sourceOf(sharedKey))
		c.Check(p.headOf(sharedKey), "promotion must keep the first-admission HeadBeforeGet flag")

		// A never-admitted redirect target is still filtered normally.
		c.Check(rec.count("GET", "/a/other") == 1, "unreserved redirect target should be fetched once")
		c.Check(p.filterCount(hostA+"/other") == 1, "unreserved redirect target should go through Filter once")
		c.Check(p.depthOf(1, hostA+"/other") == 0, "redirect target stays in the current layer (depth 0)")

		// Redirect loops must terminate without refetching.
		c.Check(rec.count("GET", "/a/loop1") == 1, "loop1 fetched once")
		c.Check(rec.count("GET", "/a/loop2") == 1, "loop2 fetched once")
	})
}

func TestLayerTermination(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		rec := newLayerRecorder()
		slowStarted := newOneShot()
		releaseSlow := make(chan struct{})
		var releaseSlowOnce sync.Once
		unblockSlow := func() { releaseSlowOnce.Do(func() { close(releaseSlow) }) }

		aSrv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			switch r.URL.Path {
			case "/robots.txt":
				rec.record(r.Method, "/a/robots.txt")
				fmt.Fprint(w, "User-agent: *\nDisallow: /denied\n")
			case "/root":
				rec.record(r.Method, "/a/root")
				fmt.Fprint(w, `<a href="/denied">d</a><a href="/headno">h</a><a href="/bad">b</a><a href="/broken">x</a><a href="/child">c</a>`)
			case "/denied":
				rec.record(r.Method, "/a/denied")
				fmt.Fprint(w, "should not be fetched")
			case "/headno":
				rec.record(r.Method, "/a/headno")
				fmt.Fprint(w, "head only")
			case "/bad":
				rec.record(r.Method, "/a/bad")
				w.WriteHeader(http.StatusInternalServerError)
			case "/broken":
				rec.record(r.Method, "/a/broken")
				fmt.Fprint(w, "body that fails to read")
			case "/child":
				rec.record(r.Method, "/a/child")
				fmt.Fprint(w, "child")
			case "/slow":
				rec.record(r.Method, "/a/slow")
				slowStarted.fire()
				<-releaseSlow
				fmt.Fprint(w, "slow")
			default:
				http.NotFound(w, r)
			}
		}))
		defer aSrv.Close()

		bSrv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			switch r.URL.Path {
			case "/robots.txt":
				rec.record(r.Method, "/b/robots.txt")
				allowRobots(w, r)
			case "/second":
				rec.record(r.Method, "/b/second")
				w.WriteHeader(http.StatusServiceUnavailable)
			default:
				http.NotFound(w, r)
			}
		}))
		defer bSrv.Close()
		defer unblockSlow()

		hostA := mustHost(aSrv.URL)
		c.Check(mustHost(bSrv.URL) != hostA, "the two local sources must be distinct hosts")

		// 1) Rejections, non-success responses and read errors must end the
		// current layer's work, without counting as successful visits, so
		// that the next layer still runs and the run terminates.
		p := newLayerProbe()
		p.headPaths[hostA+"/headno"] = true
		p.denyGet[hostA+"/headno"] = true
		p.brokenPath = "/broken"
		cr := newLayeredCrawler(p)
		finished := make(chan struct{})
		var runErr error
		go func() {
			runErr = cr.Run([]string{aSrv.URL + "/root", bSrv.URL + "/second"})
			close(finished)
		}()
		c.Check(waitClosed(finished, 10*time.Second), "run must terminate despite rejected/failed responses")
		c.Check(runErr == nil, "run should return nil, got %v", runErr)

		c.Check(rec.count("GET", "/a/denied") == 0, "robots-denied URL must not be fetched")
		c.Check(p.disallowedCount(hostA+"/denied") == 1, "robots-denied URL must call Disallowed once")
		c.Check(rec.count("HEAD", "/a/headno") == 1, "HEAD must be issued for the HEAD-denied URL")
		c.Check(rec.count("GET", "/a/headno") == 0, "HEAD-denied URL must not be fetched with GET")
		c.Check(p.errCount(crawl.CekHttpStatusCode) == 2, "both non-success responses must be reported, got %d", p.errCount(crawl.CekHttpStatusCode))
		c.Check(p.errCount(crawl.CekReadBody) == 1, "body read error must be reported, got %d", p.errCount(crawl.CekReadBody))
		c.Check(p.errCount(crawl.CekFetch) == 0, "no transport error expected, got %d", p.errCount(crawl.CekFetch))

		c.Check(p.visitDocOK(hostA+"/root"), "root is a successful visit")
		c.Check(p.visitDocOK(hostA+"/child"), "child must still be visited in the next layer")
		c.Check(!p.visitDocOK(hostA+"/broken"), "read-error page must not be a successful visit")
		c.Check(p.depthOf(1, hostA+"/child") == 1, "child should be depth 1")
		c.Check(p.depthOf(1, hostA+"/broken") == 1, "failed page still belongs to depth 1")
		c.Check(p.startCount() == 1, "Start called once")
		if n, err := p.endCount(); true {
			c.Check(n == 1, "End called once, got %d", n)
			c.Check(err == runErr, "End must receive the Run error, got %v", err)
		}

		// 2) MaxVisits contract and End error are unchanged.
		p2 := newLayerProbe()
		cr2 := newLayeredCrawler(p2)
		cr2.Options.MaxVisits = 1
		err := cr2.Run(aSrv.URL + "/root")
		c.Check(err == crawl.ErrMaxVisits, "MaxVisits should return ErrMaxVisits, got %v", err)
		if n, e := p2.endCount(); true {
			c.Check(n == 1, "End called once on MaxVisits, got %d", n)
			c.Check(e == crawl.ErrMaxVisits, "End must receive ErrMaxVisits, got %v", e)
		}
		c.Check(p2.depthOf(1, hostA+"/root") == 0, "MaxVisits run root is depth 0")

		// 3) Consecutive Run on the same Crawler restarts at layer 0.
		before := rec.snapshot()
		p3 := newLayerProbe()
		cr3 := newLayeredCrawler(p3)
		c.Check(cr3.Run(aSrv.URL+"/root") == nil, "first run should finish cleanly")
		c.Check(cr3.Run(aSrv.URL+"/root") == nil, "second run should finish cleanly")
		c.Check(p3.startCount() == 2, "Start called once per Run, got %d", p3.startCount())
		if n, _ := p3.endCount(); true {
			c.Check(n == 2, "End called once per Run, got %d", n)
		}
		c.Check(p3.depthOf(1, hostA+"/root") == 0, "first run root restarts at depth 0")
		c.Check(p3.depthOf(2, hostA+"/root") == 0, "second run root restarts at depth 0")
		c.Check(rec.since(before, "GET", "/a/root") == 2, "both runs must fetch the seed")
		c.Check(rec.since(before, "GET", "/a/child") == 2, "both runs must reach the next layer")

		// 4) Stop and Run return convention.
		p4 := newLayerProbe()
		cr4 := newLayeredCrawler(p4)
		stopDone := make(chan struct{})
		var stopErr error
		go func() {
			stopErr = cr4.Run(aSrv.URL + "/slow")
			close(stopDone)
		}()
		c.Check(waitClosed(slowStarted.done(), 5*time.Second), "slow seed request should start")
		cr4.Stop()
		unblockSlow()
		c.Check(waitClosed(stopDone, 10*time.Second), "Stop must terminate Run")
		c.Check(stopErr == crawl.ErrInterrupted, "Stop should make Run return ErrInterrupted, got %v", stopErr)
		if n, e := p4.endCount(); true {
			c.Check(n == 1, "End called once after Stop, got %d", n)
			c.Check(e == crawl.ErrInterrupted, "End must receive ErrInterrupted, got %v", e)
		}
	})
}
