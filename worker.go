package gocrawl

import (
	"bytes"
	"io"
	"io/ioutil"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"time"

	"path"

	"github.com/PuerkitoBio/goquery"
	"github.com/andybalholm/cascadia"
	"github.com/temoto/robotstxt"
	"golang.org/x/net/html"
)

// The worker is dedicated to fetching and visiting a given host, respecting
// this host's robots.txt crawling policies.
type worker struct {
	// Worker identification
	host  string
	index int

	// Communication channels and sync
	push    chan<- *workerResponse
	pop     popChannel
	stop    chan struct{}
	enqueue chan<- interface{}
	wg      *sync.WaitGroup

	// Pause/Resume barrier shared with the master crawler. The timing fields
	// below are protected by gate.mu when a pause freezes or restarts them.
	gate *crawlGate

	// Robots validation
	robotsGroup *robotstxt.Group

	// Logging
	logFunc func(LogFlags, string, ...interface{})

	// Implementation fields
	waitEnd        time.Time     // deadline of the pending crawl delay
	idleDeadline   time.Time     // deadline of the current idle TTL wait
	frozenWait     time.Duration // crawl delay left when the worker parked
	frozenWaitOK   bool
	frozenIdle     time.Duration // idle TTL left when the worker parked
	frozenIdleOK   bool
	idleWaiting    bool // the worker is resuming an idle wait interrupted by a pause
	parked         bool // the worker is waiting while admission is closed
	lastFetch      *FetchInfo
	lastCrawlDelay time.Duration
	headPending    *URLContext // a HEAD phase completed, its GET is still due
	opts           *Options
}

// Start crawling the host.
func (w *worker) run() {
	defer func() {
		w.logFunc(LogInfo, "worker done.")
		// Unregister before releasing the WaitGroup, so that a Pause observing
		// the unit as quiescent also observes that the worker can no longer
		// invoke any hook.
		w.gate.removeWorker(w)
		w.wg.Done()
	}()

	// Enter loop to process URLs until stop signal is received
	for {
		// Park before accepting a new phase. This is the only place where a
		// worker that is between phases can be seen as quiescent by Pause.
		if !w.awaitAdmission() {
			w.logFunc(LogInfo, "stop signal received.")
			return
		}

		var idleChan <-chan time.Time

		w.logFunc(LogInfo, "waiting for pop...")

		// Initialize the idle timeout channel, if required. A worker that is
		// resuming an idle wait interrupted by a pause re-arms from its frozen
		// remainder, so the countdown continues instead of restarting.
		if w.opts.WorkerIdleTTL > 0 {
			w.gate.armIdle(w, w.idleWaiting)
			idleChan = time.After(w.gate.remainingIdleW(w))
		}
		pauseChan, _ := w.gate.pauseSignal()

		select {
		case <-w.stop:
			w.logFunc(LogInfo, "stop signal received.")
			return

		case <-pauseChan:
			// A pause began while the worker was idle: park so that the
			// quiescence barrier can complete, and preserve the remaining
			// idle TTL for after the resume.
			if !w.parkUntilResume() {
				w.logFunc(LogInfo, "stop signal received.")
				return
			}
			w.idleWaiting = true
			continue

		case <-idleChan:
			// Do not let an idle TTL that expires at the pause boundary tear
			// down a paused worker.
			if !w.gate.isOpen() {
				if !w.parkUntilResume() {
					w.logFunc(LogInfo, "stop signal received.")
					return
				}
				w.idleWaiting = true
				continue
			}
			w.logFunc(LogInfo, "idle timeout received.")
			w.sendResponse(nil, false, nil, true)
			return

		case batch := <-w.pop:
			// Activity resets the idle countdown.
			w.gate.resetIdleDeadline(w)
			w.idleWaiting = false

			// Got a batch of urls to crawl, loop and check at each iteration if a
			// stop is received or a pause must be honored.
			for _, ctx := range batch {
				if !w.awaitAdmission() {
					// The batch is left in the worker's hands; it is
					// preserved only in memory after a stop, which is fine.
					w.logFunc(LogInfo, "stop signal received.")
					return
				}

				w.logFunc(LogInfo, "popped: %s", ctx.url)
				w.processURL(ctx)

				// A HEAD phase completes on its own (including the RequestGet
				// decision). The GET it decided on is a new phase, and must
				// observe the pause barrier before starting.
				if w.headPending == ctx {
					if !w.awaitAdmission() {
						w.logFunc(LogInfo, "stop signal received.")
						return
					}
					w.continueHeadGet(ctx)
				}
			}

			// No need to check for idle timeout here, no idling while looping
			// through a batch of URLs.
		}
	}
}

// processURL runs one crawling phase for the given URL. For a HEAD decision,
// only the HEAD fragment runs here; continueHeadGet runs the subsequent GET.
func (w *worker) processURL(ctx *URLContext) {
	if ctx.IsRobotsURL() {
		w.requestRobotsTxt(ctx)
		return
	}
	if !w.isAllowedPerRobotsPolicies(ctx.url) {
		// Must still notify Crawler that this URL was processed, although not visited
		w.opts.Extender.Disallowed(ctx)
		w.sendResponse(ctx, false, nil, false)
		return
	}
	w.requestURL(ctx, ctx.HeadBeforeGet)
}

// continueHeadGet runs the GET phase that follows a completed HEAD phase,
// reusing the decision already made by RequestGet.
func (w *worker) continueHeadGet(ctx *URLContext) {
	if w.headPending == ctx {
		w.headPending = nil
	}
	if res, ok := w.fetchURL(ctx, w.opts.UserAgent, false); ok {
		w.consumeResponse(ctx, res)
	}
}

// consumeResponse closes the response body after visiting or reporting it.
func (w *worker) consumeResponse(ctx *URLContext, res *http.Response) {
	var harvested interface{}
	var visited bool

	// Close the body on function end
	defer res.Body.Close()

	// Any 2xx status code is good to go
	if res.StatusCode >= 200 && res.StatusCode < 300 {
		// Success, visit the URL
		harvested = w.visitURL(ctx, res)
		visited = true
	} else {
		// Error based on status code received
		w.opts.Extender.Error(newCrawlErrorMessage(ctx, res.Status, CekHttpStatusCode))
		w.logFunc(LogError, "ERROR status code for %s: %s", ctx.url, res.Status)
	}
	w.sendResponse(ctx, visited, harvested, false)
}

// awaitAdmission blocks until the crawler is running again or reports that it
// is stopping. While admission is closed, the worker is registered as parked.
// A closed stop signal always wins: no new phase may start after a Stop.
func (w *worker) awaitAdmission() bool {
	for {
		g := w.gate
		g.mu.Lock()
		if w.isStopped() {
			// Leave w.parked set; removeWorker accounts for it.
			g.mu.Unlock()
			return false
		}
		if g.open {
			g.mu.Unlock()
			return true
		}
		g.park(w, time.Now())
		resumeCh, stop := g.resumeCh, w.stop
		g.mu.Unlock()

		select {
		case <-resumeCh:
		case <-stop:
			// Leave w.parked set; removeWorker accounts for it.
			return false
		}

		g.mu.Lock()
		g.unpark(w, time.Now())
		open := g.open
		g.mu.Unlock()
		if open {
			return true
		}
	}
}

// isStopped reports whether the stop signal has been closed.
func (w *worker) isStopped() bool {
	select {
	case <-w.stop:
		return true
	default:
		return false
	}
}

// parkUntilResume parks the worker (freezing its crawl delay and idle TTL) and
// waits for the resume. It returns false if the crawler is stopping instead.
func (w *worker) parkUntilResume() bool {
	g := w.gate
	g.mu.Lock()
	if g.open {
		// The pause was already resolved; nothing to wait for.
		g.mu.Unlock()
		return true
	}
	g.park(w, time.Now())
	resumeCh, stop := g.resumeCh, w.stop
	g.mu.Unlock()

	select {
	case <-resumeCh:
	case <-stop:
		return false
	}

	g.mu.Lock()
	g.unpark(w, time.Now())
	g.mu.Unlock()
	return true
}

// awaitCrawlDelay waits for the pending crawl delay, if any. The wait is
// interruptible by a pause: the remaining time is frozen while parked and
// resumes afterwards, so a long pause does not turn into an immediate
// back-to-back request, and a pause does not have to wait out the full delay.
func (w *worker) awaitCrawlDelay() bool {
	for {
		waitEnd, hasWait := w.gate.waitDeadline(w)
		if !hasWait {
			return true
		}
		d := time.Until(waitEnd)
		if d <= 0 {
			w.gate.clearWaitDeadline(w)
			return true
		}

		timer := time.NewTimer(d)
		pauseChan, _ := w.gate.pauseSignal()
		select {
		case <-timer.C:
			w.gate.clearWaitDeadline(w)
			return true
		case <-w.stop:
			timer.Stop()
			return false
		case <-pauseChan:
			timer.Stop()
			if !w.parkUntilResume() {
				return false
			}
			// w.waitEnd was frozen while parked, then restarted from its
			// remainder by unpark; loop and wait that remainder.
		}
	}
}

// Checks if the given URL can be fetched based on robots.txt policies.
func (w *worker) isAllowedPerRobotsPolicies(u *url.URL) bool {
	if w.robotsGroup != nil {
		// Is this URL allowed per robots.txt policy?
		ok := w.robotsGroup.Test(u.Path)
		if !ok {
			w.logFunc(LogIgnored, "ignored on robots.txt policy: %s", u.String())
		}
		return ok
	}

	// No robots.txt = everything is allowed
	return true
}

// Process the specified URL.
func (w *worker) requestURL(ctx *URLContext, headRequest bool) {
	res, ok := w.fetchURL(ctx, w.opts.UserAgent, headRequest)
	if !ok || res == nil {
		// Fetch error, or a HEAD whose GET is now pending.
		return
	}
	w.consumeResponse(ctx, res)
}

// Process the robots.txt URL.
func (w *worker) requestRobotsTxt(ctx *URLContext) {
	// Ask if it should be fetched
	if robData, reqRob := w.opts.Extender.RequestRobots(ctx, w.opts.RobotUserAgent); !reqRob {
		w.logFunc(LogInfo, "using robots.txt from cache")
		w.robotsGroup = w.getRobotsTxtGroup(ctx, robData, nil)

	} else if res, ok := w.fetchURL(ctx, w.opts.UserAgent, false); ok {
		// Close the body on function end
		defer res.Body.Close()
		w.robotsGroup = w.getRobotsTxtGroup(ctx, nil, res)
	}
}

// Get the robots.txt group for this crawler.
func (w *worker) getRobotsTxtGroup(ctx *URLContext, b []byte, res *http.Response) (g *robotstxt.Group) {
	var data *robotstxt.RobotsData
	var e error

	if res != nil {
		var buf bytes.Buffer
		io.Copy(&buf, res.Body)
		res.Body = ioutil.NopCloser(bytes.NewReader(buf.Bytes()))
		data, e = robotstxt.FromResponse(res)
		// Rewind the res.Body (by re-creating it from the bytes)
		res.Body = ioutil.NopCloser(bytes.NewReader(buf.Bytes()))
		// Error or not, the robots.txt has been fetched, so notify
		w.opts.Extender.FetchedRobots(ctx, res)
	} else {
		data, e = robotstxt.FromBytes(b)
	}

	// If robots data cannot be parsed, will return nil, which will allow access by default.
	// Reasonable, since by default no robots.txt means full access, so invalid
	// robots.txt is similar behavior.
	if e != nil {
		w.opts.Extender.Error(newCrawlError(nil, e, CekParseRobots))
		w.logFunc(LogError, "ERROR parsing robots.txt for host %s: %s", w.host, e)
	} else {
		g = data.FindGroup(w.opts.RobotUserAgent)
	}
	return g
}

// Set the crawl delay between this request and the next.
func (w *worker) setCrawlDelay() {
	var robDelay time.Duration

	if w.robotsGroup != nil {
		robDelay = w.robotsGroup.CrawlDelay
	}
	w.lastCrawlDelay = w.opts.Extender.ComputeDelay(w.host,
		&DelayInfo{
			w.opts.CrawlDelay,
			robDelay,
			w.lastCrawlDelay,
		},
		w.lastFetch)
	w.logFunc(LogInfo, "using crawl-delay: %v", w.lastCrawlDelay)
}

// Request the specified URL and return the response.
func (w *worker) fetchURL(ctx *URLContext, agent string, headRequest bool) (res *http.Response, ok bool) {
	var e error
	var silent bool

	for {
		// Wait for crawl delay, if one is pending. This wait is pause-aware:
		// it parks without consuming the remaining delay.
		w.logFunc(LogTrace, "waiting for crawl delay")
		if !w.awaitCrawlDelay() {
			return nil, false
		}

		// Compute the next delay
		w.setCrawlDelay()

		// Compute the fetch duration
		now := time.Now()

		// Request the URL
		if res, e = w.opts.Extender.Fetch(ctx, agent, headRequest); e != nil {
			// Check if this is an ErrEnqueueRedirect, in which case we will enqueue
			// the redirect-to URL.
			if ue, ok := e.(*url.Error); ok {
				// We have a *url.Error, check if it was returned because of an ErrEnqueueRedirect
				if ue.Err == ErrEnqueueRedirect {
					// Do not notify this error outside of this if block, this is not a
					// "real" error. We either enqueue the new URL, or fail to parse it,
					// and then stop processing the current URL.
					silent = true
					// Parse the URL in the context of the original URL (so that relative URLs are ok).
					// Absolute URLs that point to another host are ok too.
					if ur, e := ctx.url.Parse(ue.URL); e != nil {
						// Notify error
						w.opts.Extender.Error(newCrawlError(nil, e, CekParseRedirectURL))
						w.logFunc(LogError, "ERROR parsing redirect URL %s: %s", ue.URL, e)
					} else {
						w.logFunc(LogTrace, "redirect to %s from %s, linked from %s", ur, ctx.URL(), ctx.SourceURL())
						// Enqueue the redirect-to URL with the original source
						rCtx := ctx.cloneForRedirect(ur, w.opts.URLNormalizationFlags)
						w.enqueue <- rCtx
					}
				}
			}

			// No fetch, so set to nil
			w.lastFetch = nil

			if !silent {
				// Notify error
				w.opts.Extender.Error(newCrawlError(ctx, e, CekFetch))
				w.logFunc(LogError, "ERROR fetching %s: %s", ctx.url, e)
			}

			// Return from this URL crawl
			w.sendResponse(ctx, false, nil, false)
			return nil, false

		}
		// Get the fetch duration
		fetchDuration := time.Now().Sub(now)
		// Crawl delay starts now. A pause freezes its remaining time; the
		// deadline is what the worker parks against.
		w.gate.setWaitEnd(w, time.Now().Add(w.lastCrawlDelay))

		// Keep trace of this last fetch info
		w.lastFetch = &FetchInfo{
			ctx,
			fetchDuration,
			res.StatusCode,
			headRequest,
		}

		if headRequest {
			// Close the HEAD request's body
			defer res.Body.Close()
			// Next up is GET request, maybe
			headRequest = false
			// Ask caller if we should proceed with a GET
			if !w.opts.Extender.RequestGet(ctx, res) {
				w.logFunc(LogIgnored, "ignored on HEAD filter policy: %s", ctx.url)
				w.sendResponse(ctx, false, nil, false)
				ok = false
				break
			}
			// The HEAD phase is complete, but the GET it decided on is a
			// separate phase and must observe the pause barrier. Signal the
			// caller that a GET is due for this context.
			w.headPending = ctx
			break
		}
		ok = true
		break
	}
	return
}

// Send a response to the crawler.
func (w *worker) sendResponse(ctx *URLContext, visited bool, harvested interface{}, idleDeath bool) {
	// Push harvested urls back to crawler, even if empty (uses the channel communication
	// to decrement reference count of pending URLs)
	if ctx == nil || !isRobotsURL(ctx.url) {
		// If a stop signal has been received, ignore the response, since the push
		// channel may be full and could block indefinitely.
		select {
		case <-w.stop:
			w.logFunc(LogInfo, "ignoring send response, will stop.")
			return
		default:
			// Nothing, just continue...
		}

		// No stop signal, send the response
		res := &workerResponse{
			ctx,
			visited,
			harvested,
			w.host,
			idleDeath,
		}
		w.push <- res
	}
}

// Process the response for a URL.
func (w *worker) visitURL(ctx *URLContext, res *http.Response) interface{} {
	var doc *goquery.Document
	var harvested interface{}
	var doLinks bool

	// Load a goquery document and call the visitor function
	if bd, e := ioutil.ReadAll(res.Body); e != nil {
		w.opts.Extender.Error(newCrawlError(ctx, e, CekReadBody))
		w.logFunc(LogError, "ERROR reading body %s: %s", ctx.url, e)
	} else {
		if node, e := html.Parse(bytes.NewBuffer(bd)); e != nil {
			w.opts.Extender.Error(newCrawlError(ctx, e, CekParseBody))
			w.logFunc(LogError, "ERROR parsing %s: %s", ctx.url, e)
		} else {
			doc = goquery.NewDocumentFromNode(node)
			doc.Url = res.Request.URL
		}
		// Re-assign the body so it can be consumed by the visitor function
		res.Body = ioutil.NopCloser(bytes.NewBuffer(bd))
	}

	// Visit the document (with nil goquery doc if failed to load)
	if harvested, doLinks = w.opts.Extender.Visit(ctx, res, doc); doLinks {
		// Links were not processed by the visitor, so process links
		if doc != nil {
			harvested = w.processLinks(doc)
		} else {
			w.opts.Extender.Error(newCrawlErrorMessage(ctx, "No goquery document to process links.", CekProcessLinks))
			w.logFunc(LogError, "ERROR processing links %s", ctx.url)
		}
	}
	// Notify that this URL has been visited
	w.opts.Extender.Visited(ctx, harvested)

	return harvested
}

func handleBaseTag(root *url.URL, baseHref string, aHref string) string {
	resolvedBase, err := root.Parse(baseHref)
	if err != nil {
		return ""
	}

	parsedURL, err := url.Parse(aHref)
	if err != nil {
		return ""
	}
	// If a[href] starts with a /, it overrides the base[href]
	if parsedURL.Host == "" && !strings.HasPrefix(aHref, "/") {
		aHref = path.Join(resolvedBase.Path, aHref)
	}

	resolvedURL, err := resolvedBase.Parse(aHref)
	if err != nil {
		return ""
	}
	return resolvedURL.String()
}

var (
	aHrefMatcher    = cascadia.MustCompile("a[href]")
	baseHrefMatcher = cascadia.MustCompile("base[href]")
)

// Scrape the document's content to gather all links
func (w *worker) processLinks(doc *goquery.Document) (result []*url.URL) {
	baseURL, _ := doc.FindMatcher(baseHrefMatcher).Attr("href")
	urls := doc.FindMatcher(aHrefMatcher).Map(func(_ int, s *goquery.Selection) string {
		val, _ := s.Attr("href")
		if baseURL != "" {
			val = handleBaseTag(doc.Url, baseURL, val)
		}
		return val
	})
	for _, s := range urls {
		// If href starts with "#", then it points to this same exact URL, ignore (will fail to parse anyway)
		if len(s) > 0 && !strings.HasPrefix(s, "#") {
			if parsed, e := url.Parse(s); e == nil {
				parsed = doc.Url.ResolveReference(parsed)
				result = append(result, parsed)
			} else {
				w.logFunc(LogIgnored, "ignore on unparsable policy %s: %s", s, e.Error())
			}
		}
	}
	return
}
