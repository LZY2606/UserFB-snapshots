package spider_test

import (
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/hu17889/go_spider/core/common/com_interfaces"
	"github.com/hu17889/go_spider/core/common/page"
	"github.com/hu17889/go_spider/core/common/page_items"
	"github.com/hu17889/go_spider/core/common/request"
	"github.com/hu17889/go_spider/core/downloader"
	"github.com/hu17889/go_spider/core/spider"
	taskcheck "github.com/hu17889/go_spider/task_support/taskcheck"
)

// ---- shared helpers --------------------------------------------------

func jsonBody(w http.ResponseWriter, body string) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	io.WriteString(w, body)
}

// stalledBody writes the given complete JSON document, flushes it, then holds
// the connection open so the client has the whole body prefix but no EOF. A
// racing candidate must not treat that as finished. Handlers watch the request
// context so a canceled candidate also unblocks them.
func stalledBody(w http.ResponseWriter, r *http.Request, document string, release <-chan struct{}) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(http.StatusOK)
	io.WriteString(w, document)
	if fl, ok := w.(http.Flusher); ok {
		fl.Flush()
	}
	select {
	case <-release:
	case <-r.Context().Done():
		return
	}
	// Trailing whitespace keeps the document valid once the stall ends.
	io.WriteString(w, "\n")
	if fl, ok := w.(http.Flusher); ok {
		fl.Flush()
	}
}

func mirrorRequest(u string, mirrors []string, delay time.Duration) *request.Request {
	req := request.NewRequest(u, "json", "task", "GET", "", nil, nil, nil, "meta-marker")
	req.MirrorURLs = mirrors
	req.MirrorDelay = delay
	return req
}

func waitFor(t *testing.T, what string, cond func() bool) bool {
	t.Helper()
	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		if cond() {
			return true
		}
		time.Sleep(5 * time.Millisecond)
	}
	return false
}

// ---- TestMirrorWinner ------------------------------------------------

func TestMirrorWinner(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// A primary that sends a complete, valid JSON prefix and then stalls
		// must not win; the mirror that delivers its whole document first does.
		release := make(chan struct{})
		var primaryHits, mirrorHits int32
		srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			switch r.URL.Path {
			case "/primary":
				atomic.AddInt32(&primaryHits, 1)
				stalledBody(w, r, `{"winner":"primary","next":"/rel/detail"}`, release)
			case "/mirror":
				atomic.AddInt32(&mirrorHits, 1)
				jsonBody(w, `{"winner":"mirror","next":"/rel/detail"}`)
			default:
				http.NotFound(w, r)
			}
		}))
		defer srv.Close()

		req := mirrorRequest(srv.URL+"/primary", []string{srv.URL + "/mirror"}, 80*time.Millisecond)
		dl := downloader.NewHttpDownloader()

		done := make(chan *page.Page, 1)
		go func() { done <- dl.Download(req) }()

		var got *page.Page
		select {
		case got = <-done:
		case <-time.After(3 * time.Second):
			c.Check(false, "Download blocked on the stalled primary instead of racing the mirror")
			close(release)
			<-done
			return
		}
		close(release)

		c.Check(got.IsSucc(), "mirror winner reports success, err=%q", got.Errormsg())
		if got.IsSucc() {
			c.Check(got.GetJson().Get("winner").MustString() == "mirror",
				"the fully-read mirror wins over a stalled primary prefix")
			c.Check(got.GetFetchedURL() == srv.URL+"/mirror",
				"fetched URL is the winning address, got %q", got.GetFetchedURL())
			c.Check(got.GetRequest() == req, "GetRequest keeps the original logical request pointer")
			c.Check(got.GetUrlTag() == "task", "UrlTag is unchanged")
			c.Check(got.GetRequest().GetMeta() == "meta-marker", "Meta is unchanged")
		}
		c.Check(atomic.LoadInt32(&primaryHits) == 1, "primary requested exactly once")
		c.Check(atomic.LoadInt32(&mirrorHits) == 1, "mirror requested exactly once")

		// Malformed JSON, a 503 carrying a valid JSON body, and a body that is
		// truncated after a parseable prefix must all lose to a healthy mirror.
		var badHits, goodHits, truncatedHits int32
		srv2 := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			switch r.URL.Path {
			case "/badjson":
				atomic.AddInt32(&badHits, 1)
				w.Header().Set("Content-Type", "application/json; charset=utf-8")
				io.WriteString(w, `{"winner":`)
			case "/503":
				w.Header().Set("Content-Type", "application/json; charset=utf-8")
				w.WriteHeader(http.StatusServiceUnavailable)
				io.WriteString(w, `{"winner":"503","next":"/x"}`)
			case "/truncated":
				// Claim 40 bytes but send a shorter, fully parseable prefix and
				// return: the client sees an unexpected EOF, not a valid body.
				atomic.AddInt32(&truncatedHits, 1)
				w.Header().Set("Content-Type", "application/json; charset=utf-8")
				w.Header().Set("Content-Length", "40")
				w.WriteHeader(http.StatusOK)
				io.WriteString(w, `{"winner":"truncated"}`)
			case "/good":
				atomic.AddInt32(&goodHits, 1)
				jsonBody(w, `{"winner":"good","next":"/rel/next"}`)
			default:
				http.NotFound(w, r)
			}
		}))
		defer srv2.Close()

		bad := downloader.NewHttpDownloader().Download(mirrorRequest(srv2.URL+"/badjson", []string{srv2.URL + "/good"}, 0))
		c.Check(bad.IsSucc() && bad.GetJson().Get("winner").MustString() == "good",
			"bad JSON primary must not win, err=%q", bad.Errormsg())
		c.Check(bad.GetFetchedURL() == srv2.URL+"/good",
			"bad primary cannot supply the fetched URL, got %q", bad.GetFetchedURL())
		c.Check(atomic.LoadInt32(&badHits) == 1, "bad-JSON primary requested once")

		unavailable := downloader.NewHttpDownloader().Download(mirrorRequest(srv2.URL+"/503", []string{srv2.URL + "/good"}, 0))
		c.Check(unavailable.IsSucc() && unavailable.GetJson().Get("winner").MustString() == "good",
			"a 503 with a normal JSON body must not win, err=%q", unavailable.Errormsg())
		c.Check(unavailable.GetFetchedURL() == srv2.URL+"/good",
			"non-2xx primary cannot supply the fetched URL, got %q", unavailable.GetFetchedURL())

		truncated := make(chan *page.Page, 1)
		go func() {
			truncated <- downloader.NewHttpDownloader().Download(mirrorRequest(srv2.URL+"/truncated", []string{srv2.URL + "/good"}, 100*time.Millisecond))
		}()
		var truncPage *page.Page
		select {
		case truncPage = <-truncated:
		case <-time.After(3 * time.Second):
			c.Check(false, "a truncated candidate stalled the race instead of losing")
			truncPage = nil
		}
		if truncPage != nil {
			c.Check(truncPage.IsSucc() && truncPage.GetJson().Get("winner").MustString() == "good",
				"a read failure must lose even when the received prefix is valid JSON, err=%q", truncPage.Errormsg())
		}
		c.Check(atomic.LoadInt32(&truncatedHits) == 1, "truncated candidate requested once")
		c.Check(atomic.LoadInt32(&goodHits) >= 1, "the healthy mirror served a winning body")

		// Invalid racing configurations fail before any address is contacted.
		var configHits int32
		srv3 := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			atomic.AddInt32(&configHits, 1)
			jsonBody(w, `{"ok":true}`)
		}))
		defer srv3.Close()

		post := request.NewRequest(srv3.URL+"/a", "json", "task", "POST", "x=1", nil, nil, nil, nil)
		post.MirrorURLs = []string{srv3.URL + "/b"}
		postPage := downloader.NewHttpDownloader().Download(post)
		c.Check(!postPage.IsSucc() && postPage.Errormsg() != "", "POST with mirrors fails before sending")

		postdata := request.NewRequest(srv3.URL+"/a", "json", "task", "GET", "x=1", nil, nil, nil, nil)
		postdata.MirrorURLs = []string{srv3.URL + "/b"}
		postdataPage := downloader.NewHttpDownloader().Download(postdata)
		c.Check(!postdataPage.IsSucc() && postdataPage.Errormsg() != "", "non-empty Postdata fails before sending")

		relative := mirrorRequest(srv3.URL+"/a", []string{"/not-absolute"}, 0)
		relPage := downloader.NewHttpDownloader().Download(relative)
		c.Check(!relPage.IsSucc() && relPage.Errormsg() != "", "non-absolute mirror URL fails before sending")

		negative := mirrorRequest(srv3.URL+"/a", []string{srv3.URL + "/b"}, -time.Second)
		negPage := downloader.NewHttpDownloader().Download(negative)
		c.Check(!negPage.IsSucc() && negPage.Errormsg() != "", "negative MirrorDelay fails before sending")

		otherType := mirrorRequest(srv3.URL+"/a", []string{srv3.URL + "/b"}, 0)
		otherType.RespType = "xml"
		typePage := downloader.NewHttpDownloader().Download(otherType)
		c.Check(!typePage.IsSucc() && typePage.Errormsg() != "", "unsupported RespType fails before sending")

		c.Check(atomic.LoadInt32(&configHits) == 0,
			"invalid mirror configs send no requests, got %d", atomic.LoadInt32(&configHits))
	})
}

// ---- TestMirrorPermits ---------------------------------------------

// gateServer parks every handler until the test opens its gate, so the test can
// observe exactly how many physical requests are allowed in flight.
type gateServer struct {
	srv    *httptest.Server
	gates  chan chan struct{}
	inside int32
	peak   int32
	hits   int32
}

func newGateServer() *gateServer {
	g := &gateServer{gates: make(chan chan struct{}, 128)}
	g.srv = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		atomic.AddInt32(&g.hits, 1)
		now := atomic.AddInt32(&g.inside, 1)
		for {
			old := atomic.LoadInt32(&g.peak)
			if now <= old || atomic.CompareAndSwapInt32(&g.peak, old, now) {
				break
			}
		}
		gate := make(chan struct{})
		g.gates <- gate
		select {
		case <-gate:
		case <-r.Context().Done():
		}
		atomic.AddInt32(&g.inside, -1)
		jsonBody(w, `{"ok":true}`)
	}))
	return g
}

func (g *gateServer) nextGate(timeout time.Duration) (chan struct{}, bool) {
	select {
	case gate := <-g.gates:
		return gate, true
	case <-time.After(timeout):
		return nil, false
	}
}

func TestMirrorPermits(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		g := newGateServer()
		defer g.srv.Close()

		dl := downloader.NewHttpDownloader().SetMaxConcurrentRequests(1)

		// An ordinary download holds the only physical slot.
		ordinaryDone := make(chan *page.Page, 1)
		go func() { ordinaryDone <- dl.Download(mirrorRequest(g.srv.URL+"/ordinary", nil, 0)) }()
		ordinaryGate, ok := g.nextGate(3 * time.Second)
		c.Check(ok, "ordinary request reached the server")
		c.Check(atomic.LoadInt32(&g.hits) == 1, "ordinary request occupies the only slot, hits=%d", atomic.LoadInt32(&g.hits))

		// A race started now must not send anything: its primary waits for a
		// slot, and the mirror delay has not even started.
		raceDone := make(chan *page.Page, 1)
		req := mirrorRequest(g.srv.URL+"/race-primary", []string{g.srv.URL + "/race-mirror"}, 0)
		go func() { raceDone <- dl.Download(req) }()

		time.Sleep(120 * time.Millisecond)
		c.Check(atomic.LoadInt32(&g.hits) == 1, "racing candidates wait for quota, hits=%d", atomic.LoadInt32(&g.hits))

		// Free the slot: exactly one physical request may enter.
		close(ordinaryGate)
		raceGate, ok := g.nextGate(3 * time.Second)
		c.Check(ok, "a racing candidate entered once the slot was free")
		c.Check(waitFor(t, "two hits", func() bool { return atomic.LoadInt32(&g.hits) == 2 }),
			"the primary was sent after the slot freed, hits=%d", atomic.LoadInt32(&g.hits))
		time.Sleep(120 * time.Millisecond)
		c.Check(atomic.LoadInt32(&g.hits) == 2,
			"the mirror could not start while the primary held the only slot, hits=%d", atomic.LoadInt32(&g.hits))
		c.Check(atomic.LoadInt32(&g.peak) == 1, "physical concurrency never exceeded the cap, peak=%d", atomic.LoadInt32(&g.peak))

		served := <-ordinaryDone
		c.Check(served.IsSucc(), "ordinary download succeeded, err=%q", served.Errormsg())

		// Let the primary win. Any candidate still queued for a slot must never
		// be sent afterwards.
		close(raceGate)
		var winner *page.Page
		select {
		case winner = <-raceDone:
		case <-time.After(3 * time.Second):
			c.Check(false, "race did not return after the winner was released")
			return
		}
		c.Check(winner.IsSucc(), "race produced a winner, err=%q", winner.Errormsg())
		c.Check(winner.GetFetchedURL() == g.srv.URL+"/race-primary",
			"the first racing candidate won, got %q", winner.GetFetchedURL())

		hitsAtWin := atomic.LoadInt32(&g.hits)
		c.Check(hitsAtWin == 2, "the mirror waiting on quota was never sent, hits=%d", hitsAtWin)
		c.Check(atomic.LoadInt32(&g.peak) == 1, "peak physical concurrency stayed at the cap, peak=%d", atomic.LoadInt32(&g.peak))

		// The slot was returned: a later ordinary request is served at once.
		afterDone := make(chan *page.Page, 1)
		go func() { afterDone <- dl.Download(mirrorRequest(g.srv.URL+"/after", nil, 0)) }()
		afterGate, ok := g.nextGate(3 * time.Second)
		c.Check(ok, "quota was returned; a later ordinary download acquires a slot")
		if afterGate != nil {
			close(afterGate)
		}
		select {
		case p := <-afterDone:
			c.Check(p.IsSucc(), "later ordinary download succeeded, err=%q", p.Errormsg())
		case <-time.After(3 * time.Second):
			c.Check(false, "later ordinary download did not finish")
		}
	})
}

// ---- TestMirrorLifecycle -------------------------------------------

// forwardProxy is a minimal HTTP forward proxy that records the absolute URLs
// it served and forwards cancellations from the client.
type forwardProxy struct {
	srv  *httptest.Server
	mu   sync.Mutex
	seen []string
}

func newForwardProxy() *forwardProxy {
	// A dedicated transport with no environment proxy: this proxy must reach
	// the local test servers directly.
	transport := &http.Transport{Proxy: nil}
	p := &forwardProxy{}
	p.srv = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		p.mu.Lock()
		p.seen = append(p.seen, r.URL.String())
		p.mu.Unlock()

		outReq, err := http.NewRequestWithContext(r.Context(), r.Method, r.URL.String(), r.Body)
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadGateway)
			return
		}
		outReq.Header = r.Header.Clone()
		out, err := transport.RoundTrip(outReq)
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadGateway)
			return
		}
		defer out.Body.Close()
		for k, vs := range out.Header {
			for _, v := range vs {
				w.Header().Add(k, v)
			}
		}
		w.WriteHeader(out.StatusCode)
		io.Copy(w, out.Body)
	}))
	return p
}

func (p *forwardProxy) count() int {
	p.mu.Lock()
	defer p.mu.Unlock()
	return len(p.seen)
}

// lifecycleProcessor records which final URLs were processed exactly once and
// follows a relative "next" hint using GetFetchedURL.
type lifecycleProcessor struct {
	mu        sync.Mutex
	processed map[string]int
	finished  int
}

func (p *lifecycleProcessor) Process(result *page.Page) {
	p.mu.Lock()
	p.processed[result.GetFetchedURL()]++
	p.mu.Unlock()

	next := ""
	if js := result.GetJson(); js != nil {
		next = js.Get("next").MustString()
	}
	if next == "" {
		return
	}
	base, err := url.Parse(result.GetFetchedURL())
	if err != nil || base.Host == "" {
		return
	}
	rel, err := url.Parse(next)
	if err != nil {
		return
	}
	result.AddTargetRequest(base.ResolveReference(rel).String(), "json")
}

func (p *lifecycleProcessor) Finish() {
	p.mu.Lock()
	p.finished++
	p.mu.Unlock()
}

type recordingPipeline struct {
	mu    sync.Mutex
	byURL map[string]int
}

func (p *recordingPipeline) Process(items *page_items.PageItems, _ com_interfaces.Task) {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.byURL[items.GetRequest().GetUrl()]++
}

func (p *recordingPipeline) countFor(u string) int {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.byURL[u]
}

type noopProcessor struct{}

func (noopProcessor) Process(*page.Page) {}
func (noopProcessor) Finish()            {}

func TestMirrorLifecycle(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// (a) CheckRedirect still runs. A primary redirect vetoed by the
		//     callback ends as a non-2xx, so the mirror wins; a mirror redirect
		//     that is allowed is followed and its final URL is reported.
		var vetoSeen, followSeen int32
		srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			switch r.URL.Path {
			case "/redir":
				http.Redirect(w, r, "/final", http.StatusFound)
			case "/final":
				jsonBody(w, `{"winner":"redirected"}`)
			case "/mirror":
				jsonBody(w, `{"winner":"mirror","next":"/relative/detail"}`)
			}
		}))
		defer srv.Close()

		veto := func(req *http.Request, via []*http.Request) error {
			atomic.AddInt32(&vetoSeen, 1)
			return http.ErrUseLastResponse
		}
		req := request.NewRequest(srv.URL+"/redir", "json", "task", "GET", "", nil, nil, veto, "meta-marker")
		req.MirrorURLs = []string{srv.URL + "/mirror"}
		req.MirrorDelay = 0

		got := downloader.NewHttpDownloader().Download(req)
		c.Check(atomic.LoadInt32(&vetoSeen) >= 1, "CheckRedirect callback was invoked for the primary")
		c.Check(got.IsSucc() && got.GetJson().Get("winner").MustString() == "mirror",
			"a vetoed redirect leaves a non-2xx primary, so the mirror wins; err=%q", got.Errormsg())
		c.Check(got.GetFetchedURL() == srv.URL+"/mirror",
			"fetched URL is the winning mirror, got %q", got.GetFetchedURL())

		srvFollow := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			switch r.URL.Path {
			case "/mirror2":
				http.Redirect(w, r, "/final2", http.StatusFound)
			case "/final2":
				jsonBody(w, `{"winner":"followed"}`)
			default:
				http.NotFound(w, r)
			}
		}))
		defer srvFollow.Close()

		follow := func(r *http.Request, via []*http.Request) error {
			atomic.AddInt32(&followSeen, 1)
			return nil
		}
		reqFollow := request.NewRequest(srvFollow.URL+"/primary2", "json", "task", "GET", "", nil, nil, follow, nil)
		reqFollow.MirrorURLs = []string{srvFollow.URL + "/mirror2"}
		reqFollow.MirrorDelay = 0
		gotFollow := downloader.NewHttpDownloader().Download(reqFollow)
		c.Check(gotFollow.IsSucc() && gotFollow.GetJson().Get("winner").MustString() == "followed",
			"the mirror's redirect is followed; err=%q", gotFollow.Errormsg())
		c.Check(gotFollow.GetFetchedURL() == srvFollow.URL+"/final2",
			"fetched URL reflects the redirected address, got %q", gotFollow.GetFetchedURL())
		c.Check(atomic.LoadInt32(&followSeen) >= 1, "redirect callback ran for the mirror's redirect")

		// (b) The caller's Header and Cookie values are never rewritten, and
		//     every concurrent candidate sees the original values.
		header := http.Header{}
		header.Set("X-Client", "original")
		cookies := []*http.Cookie{{Name: "session", Value: "original"}}
		var badHeader, badCookie int32
		srv2 := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			if r.Header.Get("X-Client") != "original" {
				atomic.AddInt32(&badHeader, 1)
			}
			if ck, err := r.Cookie("session"); err != nil || ck.Value != "original" {
				atomic.AddInt32(&badCookie, 1)
			}
			jsonBody(w, `{"winner":"ok"}`)
		}))
		defer srv2.Close()

		req2 := request.NewRequest(srv2.URL+"/a", "json", "task", "GET", "", header, cookies, nil, nil)
		req2.MirrorURLs = []string{srv2.URL + "/b", srv2.URL + "/c"}
		req2.MirrorDelay = 0
		_ = downloader.NewHttpDownloader().Download(req2)
		c.Check(header.Get("X-Client") == "original", "caller Header was not rewritten")
		c.Check(cookies[0].Value == "original", "caller Cookie was not rewritten")
		c.Check(atomic.LoadInt32(&badHeader) == 0, "no request carried a rewritten header value")
		c.Check(atomic.LoadInt32(&badCookie) == 0, "no request carried a rewritten cookie value")

		// (c) A local forward proxy serves both the stalled primary and the
		//     mirror, and the mirror's redirect goes through it as well.
		prox := newForwardProxy()
		defer prox.srv.Close()
		var proxPrimary, proxMirror int32
		proxyRedirectSeen := int32(0)
		srv3 := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			switch r.URL.Path {
			case "/prox-primary":
				atomic.AddInt32(&proxPrimary, 1)
				stalledBody(w, r, `{"winner":"prox-primary"}`+strings.Repeat(" ", 64), make(chan struct{}))
			case "/prox-mirror":
				atomic.AddInt32(&proxMirror, 1)
				http.Redirect(w, r, "/prox-final", http.StatusFound)
			case "/prox-final":
				jsonBody(w, `{"winner":"prox-mirror"}`)
			default:
				http.NotFound(w, r)
			}
		}))
		defer srv3.Close()

		proxyRedirect := func(r *http.Request, via []*http.Request) error {
			atomic.AddInt32(&proxyRedirectSeen, 1)
			return nil
		}
		req3 := request.NewRequest(srv3.URL+"/prox-primary", "json", "task", "GET", "", nil, nil, proxyRedirect, nil)
		req3.AddProxyHost(prox.srv.URL)
		req3.MirrorURLs = []string{srv3.URL + "/prox-mirror"}
		req3.MirrorDelay = 50 * time.Millisecond
		gotProxy := downloader.NewHttpDownloader().Download(req3)
		c.Check(gotProxy.IsSucc(), "proxied raced download succeeded, err=%q", gotProxy.Errormsg())
		c.Check(gotProxy.GetFetchedURL() == srv3.URL+"/prox-final",
			"fetched URL is the proxied, redirected winner, got %q", gotProxy.GetFetchedURL())
		c.Check(prox.count() >= 2, "the forward proxy served the primary and the mirror, got %d", prox.count())
		c.Check(atomic.LoadInt32(&proxPrimary) == 1 && atomic.LoadInt32(&proxMirror) == 1,
			"each candidate went through the proxy exactly once (primary=%d mirror=%d)",
			atomic.LoadInt32(&proxPrimary), atomic.LoadInt32(&proxMirror))
		c.Check(atomic.LoadInt32(&proxyRedirectSeen) >= 1, "redirect callback ran for the proxied mirror")

		// (d) All-failure: failed Page, named candidates, and Spider's original
		//     three download attempts still apply.
		var failPrimary, failMirror int32
		srv4 := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			switch r.URL.Path {
			case "/fail-primary":
				atomic.AddInt32(&failPrimary, 1)
				w.WriteHeader(http.StatusInternalServerError)
			case "/fail-mirror":
				atomic.AddInt32(&failMirror, 1)
				w.WriteHeader(http.StatusBadGateway)
			case "/ok":
				jsonBody(w, `{"winner":"recovered"}`)
			}
		}))
		defer srv4.Close()

		failed := downloader.NewHttpDownloader().Download(mirrorRequest(srv4.URL+"/fail-primary", []string{srv4.URL + "/fail-mirror"}, 0))
		c.Check(!failed.IsSucc(), "all candidates failing yields a failed page")
		c.Check(strings.Contains(failed.Errormsg(), "/fail-primary") &&
			strings.Contains(failed.Errormsg(), "/fail-mirror"),
			"failure report names every candidate, got %q", failed.Errormsg())
		c.Check(failed.GetFetchedURL() == "", "a failed page has no fetched URL")

		// Reset the counters: the direct Download above already made one
		// attempt at each address, and Spider.Run is expected to make three.
		atomic.StoreInt32(&failPrimary, 0)
		atomic.StoreInt32(&failMirror, 0)
		failSpider := spider.NewSpider(noopProcessor{}, "retry").SetThreadnum(1).CloseStrace()
		failSpider.SetDownloader(downloader.NewHttpDownloader())
		failSpider.AddRequest(mirrorRequest(srv4.URL+"/fail-primary", []string{srv4.URL + "/fail-mirror"}, 0))
		failSpider.Run()
		c.Check(atomic.LoadInt32(&failPrimary) == 3,
			"primary was attempted three times across Spider retries, got %d", atomic.LoadInt32(&failPrimary))
		c.Check(atomic.LoadInt32(&failMirror) == 3,
			"mirror was attempted three times across Spider retries, got %d", atomic.LoadInt32(&failMirror))

		// (e) Real Spider: the winner is processed once, its single discovered
		//     child is enqueued and processed once, and Finish runs once.
		var spiderPrimary, spiderMirror int32
		srv5 := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			switch r.URL.Path {
			case "/spider-primary":
				atomic.AddInt32(&spiderPrimary, 1)
				stalledBody(w, r, `{"winner":"stalled","next":"/relative/detail"}`, make(chan struct{}))
			case "/spider-mirror":
				atomic.AddInt32(&spiderMirror, 1)
				jsonBody(w, `{"winner":"spider-mirror","next":"/relative/detail"}`)
			case "/relative/detail":
				jsonBody(w, `{"winner":"detail"}`)
			default:
				http.NotFound(w, r)
			}
		}))
		defer srv5.Close()

		req5 := mirrorRequest(srv5.URL+"/spider-primary", []string{srv5.URL + "/spider-mirror"}, 100*time.Millisecond)
		proc := &lifecycleProcessor{processed: make(map[string]int)}
		pip := &recordingPipeline{byURL: make(map[string]int)}
		sp := spider.NewSpider(proc, "mirror-lifecycle").SetThreadnum(2).CloseStrace()
		sp.AddPipeline(pip)
		items := sp.GetByRequest(req5)
		c.Check(items != nil, "spider returned items for the winning page")
		c.Check(atomic.LoadInt32(&spiderPrimary) <= 1, "primary fetched at most once in the spider, got %d", atomic.LoadInt32(&spiderPrimary))
		c.Check(atomic.LoadInt32(&spiderMirror) == 1, "winning mirror fetched exactly once in the spider, got %d", atomic.LoadInt32(&spiderMirror))

		proc.mu.Lock()
		winnerProcessed := proc.processed[srv5.URL+"/spider-mirror"]
		totalProcessed := len(proc.processed)
		finished := proc.finished
		proc.mu.Unlock()
		c.Check(winnerProcessed == 1, "the winning mirror entered Process exactly once, got %d", winnerProcessed)
		c.Check(totalProcessed == 2, "root plus one discovered child were processed, got %d", totalProcessed)
		c.Check(pip.countFor(srv5.URL+"/relative/detail") == 1,
			"the discovered child was enqueued once, got %d", pip.countFor(srv5.URL+"/relative/detail"))
		c.Check(finished == 1, "Finish ran once, got %d", finished)

		// (f) After a race, the downloader's quota is reusable by an ordinary
		//     request on the same instance.
		recoverDl := downloader.NewHttpDownloader().SetMaxConcurrentRequests(1)
		gotRecovery := recoverDl.Download(mirrorRequest(srv4.URL+"/ok", nil, 0))
		c.Check(gotRecovery.IsSucc() && gotRecovery.GetJson().Get("winner").MustString() == "recovered",
			"ordinary request after a race reuses the released quota, err=%q", gotRecovery.Errormsg())

		// (g) Spider.Run binds its threadnum to the downloader's physical cap.
		capDl := downloader.NewHttpDownloader()
		capSpider := spider.NewSpider(noopProcessor{}, "cap").SetThreadnum(3).CloseStrace()
		capSpider.SetDownloader(capDl)
		capSpider.AddRequest(request.NewRequest(srv4.URL+"/ok", "json", "cap", "GET", "", nil, nil, nil, nil))
		capSpider.Run()
		c.Check(capDl.GetMaxConcurrentRequests() == 3,
			"Run wired threadnum to the physical cap, got %d", capDl.GetMaxConcurrentRequests())
	})
}
