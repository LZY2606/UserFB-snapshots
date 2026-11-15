package ant

import (
	"context"
	"errors"
	"fmt"
	"net/http"
	"net/url"
	"sync"

	"github.com/yields/ant/internal/normalize"
	"github.com/yields/ant/internal/robots"
)

// ErrRefreshInactive is returned by (*Engine).Refresh when there is no
// active refresh round that can accept the notification.
var ErrRefreshInactive = errors.New("ant: refresh round is not active")

// Refresh modes accepted by (*Engine).RunRefresh and (*Engine).Refresh.
const (
	// RefreshFull re-fetches the page unconditionally, no
	// conditional request headers are sent.
	RefreshFull = "full"

	// RefreshIncremental re-fetches the page conditionally when
	// a representation was committed by a previous round, it
	// sends If-None-Match when an ETag is known, otherwise
	// If-Modified-Since when a Last-Modified date is known.
	//
	// A URL without a committed representation is fetched
	// unconditionally.
	RefreshIncremental = "incremental"
)

// RunRefresh runs a single refresh round with the given start URLs.
//
// A refresh round uses its own in-memory frontier and exact
// de-duplication, it drains once there are no more URLs to fetch and
// no more accepted refresh notifications to process.
//
// The mode must be either "full" or "incremental", it applies to the
// start URLs and to any URL discovered during the round, refresh
// notifications carry their own mode.
//
// When the round completes successfully the representations of all
// successfully processed pages are saved on the engine and re-used by
// later RunRefresh calls, when the round fails or is canceled the
// previously saved representations are kept and accepted but
// unexecuted notifications are discarded.
//
// The engine must use the default in-memory queue and de-duplicator,
// an engine configured with a custom Queue or Deduper returns an
// error. Two rounds cannot run concurrently on the same engine.
func (eng *Engine) RunRefresh(ctx context.Context, mode string, urls ...string) error {
	base, err := parseRefreshMode(mode)
	if err != nil {
		return err
	}

	if eng.customQueue || eng.customDeduper {
		return errors.New("ant: RunRefresh requires the default in-memory queue and deduper")
	}

	var batch = make(URLs, 0, len(urls))
	for _, rawurl := range urls {
		u, err := url.Parse(rawurl)
		if err != nil {
			return fmt.Errorf("ant: parse url %q - %w", rawurl, err)
		}
		switch u.Scheme {
		case "https", "http":
		default:
			return fmt.Errorf("ant: cannot enqueue invalid URL %q", u)
		}
		batch = append(batch, u)
	}

	eng.refreshMu.Lock()
	if eng.round != nil {
		eng.refreshMu.Unlock()
		return errors.New("ant: a refresh round is already running")
	}
	r := newRefreshRound(eng, base)
	eng.round = r
	eng.refreshMu.Unlock()

	defer func() {
		eng.refreshMu.Lock()
		eng.round = nil
		eng.refreshMu.Unlock()
	}()

	return r.run(ctx, batch)
}

// Refresh notifies the active refresh round that the given URL should
// be refreshed with the given mode ("full" or "incremental").
//
// Notifications are never de-duplicated by the visited set, repeated
// notifications for the same URL are coalesced and a full refresh
// always takes priority over an incremental one.
//
// A nil return only means the notification was accepted by the
// current round, when no round is active the method returns an error
// that matches errors.Is(err, ErrRefreshInactive).
func (eng *Engine) Refresh(ctx context.Context, rawurl, mode string) error {
	m, err := parseRefreshMode(mode)
	if err != nil {
		return err
	}

	u, err := url.Parse(rawurl)
	if err != nil {
		return fmt.Errorf("ant: parse url %q - %w", rawurl, err)
	}
	switch u.Scheme {
	case "https", "http":
	default:
		return fmt.Errorf("ant: cannot refresh invalid URL %q", u)
	}
	u = normalize.URL(u)

	eng.refreshMu.Lock()
	r := eng.round
	eng.refreshMu.Unlock()

	if r == nil {
		return ErrRefreshInactive
	}

	if err := ctx.Err(); err != nil {
		return err
	}

	return r.notify(u, m)
}

// refreshMode is a parsed refresh mode.
type refreshMode int

const (
	refreshFull refreshMode = iota
	refreshIncremental
)

// parseRefreshMode parses the given refresh mode.
func parseRefreshMode(mode string) (refreshMode, error) {
	switch mode {
	case RefreshFull:
		return refreshFull, nil
	case RefreshIncremental:
		return refreshIncremental, nil
	default:
		return 0, fmt.Errorf("ant: invalid refresh mode %q", mode)
	}
}

// representation is the last successfully processed state of a URL.
type representation struct {
	etag    string
	lastmod string
	urls    URLs
}

// refreshEntry tracks the refresh state of a single URL within a round.
type refreshEntry struct {
	key  string
	url  *URL
	mode refreshMode

	scheduled bool // waiting for dispatch with e.mode.
	running   bool // being fetched or scraped.

	pending bool        // a successor fetch was requested.
	pmode   refreshMode // coalesced successor mode.
}

// refreshAttempt carries per-request refresh state through the
// request context to the round's client wrapper.
type refreshAttempt struct {
	mode   refreshMode
	rep    *representation
	status int
}

// refreshAttemptKey is the context key of a refresh attempt.
type refreshAttemptKey struct{}

// refreshClient wraps the configured client of an engine and injects
// the conditional request headers of the current attempt, it also
// records the response status back into the attempt.
//
// The wrapper never mutates the shared client or fetcher.
type refreshClient struct {
	next Client
}

// Do implementation.
func (c refreshClient) Do(req *http.Request) (*http.Response, error) {
	a, _ := req.Context().Value(refreshAttemptKey{}).(*refreshAttempt)
	if a == nil {
		return c.next.Do(req)
	}

	if a.mode == refreshIncremental && a.rep != nil {
		switch {
		case a.rep.etag != "":
			req.Header.Set("If-None-Match", a.rep.etag)
		case a.rep.lastmod != "":
			req.Header.Set("If-Modified-Since", a.rep.lastmod)
		}
	}

	resp, err := c.next.Do(req)
	if resp != nil {
		a.status = resp.StatusCode
	}
	return resp, err
}

// refreshRound is a single refresh crawl started by RunRefresh.
//
// The round keeps its own in-memory frontier and exact de-duplication
// set, it never touches the engine's queue or deduper.
type refreshRound struct {
	eng     *Engine
	base    refreshMode
	fetcher *Fetcher

	mu        sync.Mutex
	cond      sync.Cond
	accepting bool
	closing   bool
	ready     []*refreshEntry
	entries   map[string]*refreshEntry
	seen      map[string]struct{}
	reps      map[string]*representation
	active    int
	err       error

	cancel context.CancelFunc
	wg     sync.WaitGroup
}

// newRefreshRound returns a new round for the engine.
//
// The round works on a deep copy of the engine's representations, the
// copy is committed back only when the round completes successfully.
func newRefreshRound(eng *Engine, base refreshMode) *refreshRound {
	r := &refreshRound{
		eng:     eng,
		base:    base,
		entries: make(map[string]*refreshEntry),
		seen:    make(map[string]struct{}),
		reps:    make(map[string]*representation, len(eng.reps)),
	}
	r.cond.L = &r.mu

	for k, v := range eng.reps {
		urls := make(URLs, len(v.urls))
		copy(urls, v.urls)
		r.reps[k] = &representation{
			etag:    v.etag,
			lastmod: v.lastmod,
			urls:    urls,
		}
	}

	// Shallow-copy the fetcher and wrap the configured client, the
	// shared fetcher and client configurations are never modified.
	f := *eng.fetcher
	f.Client = refreshClient{next: f.client()}
	r.fetcher = &f

	return r
}

// run runs the round until the frontier drains, an error occurs or
// the context is canceled.
func (r *refreshRound) run(ctx context.Context, seeds URLs) error {
	ctx, cancel := context.WithCancel(ctx)
	r.cancel = cancel
	defer cancel()

	r.mu.Lock()
	r.accepting = true
	r.mu.Unlock()

	stop := context.AfterFunc(ctx, func() {
		r.fail(ctx.Err())
	})
	defer stop()

	r.discover(seeds)

	var workers sync.WaitGroup
	workers.Add(r.eng.workers)
	for i := 0; i < r.eng.workers; i++ {
		go func() {
			defer workers.Done()
			r.worker(ctx)
		}()
	}
	workers.Wait()

	// Wait for in-flight fetches and callbacks to wind down.
	r.wg.Wait()

	r.mu.Lock()
	err := r.err
	r.mu.Unlock()
	if err != nil {
		return err
	}

	// The round completed, commit the representations.
	r.eng.refreshMu.Lock()
	r.eng.reps = r.reps
	r.eng.refreshMu.Unlock()

	return nil
}

// worker dequeues ready entries and dispatches them.
func (r *refreshRound) worker(ctx context.Context) {
	for {
		e, mode := r.take()
		if e == nil {
			return
		}

		if sema := r.eng.sema; sema != nil {
			if err := sema.Acquire(ctx, 1); err != nil {
				r.complete(e)
				r.fail(err)
				continue
			}
		}

		r.wg.Add(1)
		go func() {
			defer r.wg.Done()
			if sema := r.eng.sema; sema != nil {
				defer sema.Release(1)
			}
			if err := r.process(ctx, e, mode); err != nil {
				r.fail(err)
			}
			r.complete(e)
		}()
	}
}

// take takes the next ready entry, blocking until one is available,
// the frontier drains or the round is closed.
func (r *refreshRound) take() (*refreshEntry, refreshMode) {
	r.mu.Lock()
	defer r.mu.Unlock()

	for {
		if r.closing {
			return nil, 0
		}
		if len(r.ready) > 0 {
			e := r.ready[0]
			r.ready = r.ready[1:]
			e.scheduled = false
			e.running = true
			r.active++
			return e, e.mode
		}
		if r.active == 0 {
			// The frontier drained, close the acceptance gate
			// and shut all workers down.
			r.accepting = false
			r.closing = true
			r.cond.Broadcast()
			return nil, 0
		}
		r.cond.Wait()
	}
}

// complete completes the entry, scheduling a coalesced successor
// notification if one exists.
func (r *refreshRound) complete(e *refreshEntry) {
	r.mu.Lock()
	defer r.mu.Unlock()

	e.running = false
	r.active--

	if e.pending {
		e.pending = false
		if !r.closing {
			e.mode = e.pmode
			e.scheduled = true
			r.ready = append(r.ready, e)
		}
	}

	r.cond.Broadcast()
}

// fail terminates the round with the given error, keeping the first
// error that triggered the termination.
func (r *refreshRound) fail(err error) {
	r.mu.Lock()
	if r.err == nil {
		r.err = err
	}
	r.accepting = false
	r.closing = true
	r.cond.Broadcast()
	r.mu.Unlock()

	if r.cancel != nil {
		r.cancel()
	}
}

// notify notifies the round that the URL should be refreshed.
func (r *refreshRound) notify(u *URL, mode refreshMode) error {
	var key = u.String()

	r.mu.Lock()
	defer r.mu.Unlock()

	if !r.accepting || r.closing {
		return ErrRefreshInactive
	}

	e, ok := r.entries[key]
	if !ok {
		e = &refreshEntry{key: key, url: u, mode: mode, scheduled: true}
		r.entries[key] = e
		r.ready = append(r.ready, e)
		r.cond.Broadcast()
		return nil
	}

	switch {
	case e.running:
		// The URL is being fetched or scraped, coalesce all
		// notifications into a single successor fetch that
		// starts once the current one winds down, a full
		// refresh takes priority.
		if !e.pending {
			e.pending = true
			e.pmode = mode
		} else if mode == refreshFull {
			e.pmode = refreshFull
		}
	case e.scheduled:
		// The URL is waiting for dispatch, merge into the
		// pending fetch, a full refresh takes priority.
		if mode == refreshFull {
			e.mode = refreshFull
		}
	default:
		// The URL is idle, schedule a new fetch.
		e.mode = mode
		e.scheduled = true
		r.ready = append(r.ready, e)
		r.cond.Broadcast()
	}

	return nil
}

// discover feeds one-time discoveries into the round's frontier.
//
// Discoveries are de-duplicated exactly once per round and they never
// downgrade a pending full refresh.
func (r *refreshRound) discover(urls URLs) {
	if len(urls) == 0 {
		return
	}

	for j := range urls {
		urls[j] = normalize.URL(urls[j])
	}
	urls = r.eng.matches(urls)

	r.mu.Lock()
	defer r.mu.Unlock()

	if !r.accepting || r.closing {
		return
	}

	for _, u := range urls {
		key := u.String()
		if _, ok := r.seen[key]; ok {
			continue
		}
		r.seen[key] = struct{}{}
		if _, ok := r.entries[key]; ok {
			// The URL is already scheduled or was already
			// fetched this round, the discovery must not
			// downgrade a pending full refresh.
			continue
		}
		e := &refreshEntry{
			key:       key,
			url:       u,
			mode:      r.base,
			scheduled: true,
		}
		r.entries[key] = e
		r.ready = append(r.ready, e)
	}
	r.cond.Broadcast()
}

// process processes a single entry.
func (r *refreshRound) process(ctx context.Context, e *refreshEntry, mode refreshMode) error {
	var eng = r.eng
	var url = e.url

	if !eng.impolite {
		allowed, err := eng.robots.Allowed(ctx, robots.Request{
			URL:       url,
			UserAgent: UserAgent.String(),
		})
		if err != nil {
			return err
		}
		if !allowed {
			return nil
		}
	}

	if err := eng.limit(ctx, url); err != nil {
		return err
	}

	r.mu.Lock()
	var rep *representation
	if mode == refreshIncremental {
		rep = r.reps[e.key]
	}
	r.mu.Unlock()

	attempt := &refreshAttempt{mode: mode, rep: rep}
	ctx = context.WithValue(ctx, refreshAttemptKey{}, attempt)

	page, err := r.fetcher.Fetch(ctx, url)
	if err != nil {
		return fmt.Errorf("ant: fetch %q - %w", url, err)
	}
	if page == nil {
		return nil
	}
	defer page.close()

	if attempt.status == http.StatusNotModified {
		if rep == nil {
			return &FetchError{
				URL:    url,
				Status: http.StatusNotModified,
			}
		}
		return r.notModified(rep, page)
	}

	urls, err := eng.scraper.Scrape(ctx, page)
	if err != nil {
		return fmt.Errorf("ant: scrape %q - %w", url, err)
	}

	// The body was fully read and the scraper succeeded, replace
	// the validators and the outlinks together, clearing any
	// validator the response did not include.
	r.mu.Lock()
	r.reps[e.key] = &representation{
		etag:    page.Header.Get("ETag"),
		lastmod: page.Header.Get("Last-Modified"),
		urls:    urls,
	}
	r.mu.Unlock()

	r.discover(urls)
	return nil
}

// notModified handles a 304 response, the scraper is not called, the
// old outlinks are kept and re-discovered and only the validators
// present in the response are updated.
func (r *refreshRound) notModified(rep *representation, page *Page) error {
	r.mu.Lock()
	if v := page.Header.Get("ETag"); v != "" {
		rep.etag = v
	}
	if v := page.Header.Get("Last-Modified"); v != "" {
		rep.lastmod = v
	}
	urls := rep.urls
	r.mu.Unlock()

	r.discover(urls)
	return nil
}
