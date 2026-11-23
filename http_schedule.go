package grawler

import (
	"errors"
	"fmt"
	"net"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"time"
)

// HTTPOptions configures the per-origin-aware HTTP scheduler used by
// CrawlHTTP.
type HTTPOptions struct {
	// MaxWorkers bounds the number of physical GETs in flight at
	// any moment, across every origin.
	MaxWorkers int

	// MaxPerOrigin bounds concurrent physical GETs against a single
	// origin (scheme/host/effective port).
	MaxPerOrigin int

	// MinOriginInterval is the minimum delay between the end of one
	// response at an origin and the start of the next request to
	// that origin. Zero disables the gap.
	MinOriginInterval time.Duration

	// Client optionally supplies an HTTP client. Its Transport and
	// Timeout are retained without mutating the caller's client. A
	// nil client causes CrawlHTTP to use a private client.
	Client *http.Client
}

// Physical target lifecycle states.
const (
	stateQueued = iota
	stateRunning
	stateRedirect
	stateTerminal
)

var (
	errRedirectCycle = errors.New("redirect cycle detected")
	errNoLocation    = errors.New("redirect response is missing a Location header")
)

// physicalOutcome is the terminal result of one normalized physical
// URL: either a successfully parsed HTML document or an error shared
// by every logical entry that converged on the URL.
type physicalOutcome struct {
	err    error
	links  []string
	assets []string
}

// logicalEntry is one accepted entry (a seed, or a link that passed
// follow) whose Page must keep its own logical URL even when its
// physical fetch converges with other entries.
type logicalEntry struct {
	logicalURL string
}

// physicalTarget tracks one normalized physical URL through the
// scheduler. A redirect target does not settle itself; it forwards its
// waiters to the destination target.
type physicalTarget struct {
	url      string
	state    int
	waiters  []*logicalEntry
	outcome  *physicalOutcome
	forward  *physicalTarget
	expanded bool
}

// head follows redirect forwards to the target that currently owns
// the chain's waiters. Cycles are broken when detected, so this walk
// terminates; the visited set is defensive only.
func (t *physicalTarget) head() *physicalTarget {
	seen := map[*physicalTarget]bool{}
	for t.forward != nil && !seen[t] {
		seen[t] = true
		t = t.forward
	}
	return t
}

// originGate holds the concurrency and interval bookkeeping for one
// origin (lowercase scheme/host/effective port).
type originGate struct {
	active      int
	nextReadyAt time.Time
}

// httpScheduler owns all mutable state of a single CrawlHTTP call.
// State is touched only by the scheduler goroutine; workers perform
// blocking I/O and report results over a channel, so an origin waiting
// out its interval never occupies a global worker slot.
type httpScheduler struct {
	options HTTPOptions
	client  *http.Client
	follow  FollowLink

	// accepted dedupes logical URLs: every distinct accepted seed or
	// discovered link yields exactly one Page.
	accepted map[string]bool
	pages    []*Page
	targets  map[string]*physicalTarget
	gates    map[string]*originGate
	queue    []*physicalTarget
	active   int

	work     chan string
	finished chan physicalResult
}

// physicalResult is reported by a worker after one scheduled GET has
// settled. Every response body is already closed by the time this is
// sent.
type physicalResult struct {
	url        string
	origin     string
	err        error
	nextHop    string
	redirect   bool
	statusCode int
	links      []string
	assets     []string
}

// normalizeURL validates an absolute http/https URL and returns its
// canonical form: lowercase host (with effective port), fragment
// stripped, path and query preserved.
func normalizeURL(raw string) (string, error) {
	u, err := url.Parse(strings.TrimSpace(raw))
	if err != nil {
		return "", fmt.Errorf("invalid URL %q: %w", raw, err)
	}
	if u.Scheme != "http" && u.Scheme != "https" {
		return "", fmt.Errorf("unsupported URL scheme %q", raw)
	}
	if u.Host == "" {
		return "", fmt.Errorf("URL %q has no host", raw)
	}
	// Lowercase the hostname for identity comparisons; keep any
	// explicit port and the path/query untouched.
	hostname := strings.ToLower(u.Hostname())
	if port := u.Port(); port != "" {
		u.Host = net.JoinHostPort(hostname, port)
	} else {
		u.Host = hostname
	}
	u.Fragment = ""
	return u.String(), nil
}

// originKey compares origins by lowercase scheme/hostname and the
// effective port for the scheme.
func originKey(u *url.URL) string {
	port := u.Port()
	if port == "" {
		if u.Scheme == "https" {
			port = "443"
		} else {
			port = "80"
		}
	}
	return strings.ToLower(u.Scheme) + "://" + strings.ToLower(u.Hostname()) + ":" + port
}

func originOf(raw string) string {
	u, err := url.Parse(raw)
	if err != nil {
		return raw
	}
	return originKey(u)
}

func validateOptions(options HTTPOptions) error {
	if options.MaxWorkers < 1 {
		return errors.New("CrawlHTTP: MaxWorkers must be positive")
	}
	if options.MaxPerOrigin < 1 {
		return errors.New("CrawlHTTP: MaxPerOrigin must be positive")
	}
	if options.MinOriginInterval < 0 {
		return errors.New("CrawlHTTP: MinOriginInterval must be non-negative")
	}
	return nil
}

// buildClient creates the client for one call. A nil options client
// yields a private client; otherwise only the caller's Transport and
// Timeout are retained, without mutating the caller's client.
// Redirects are never followed automatically: every hop must pass
// through the scheduler and the destination origin's gates.
func buildClient(options HTTPOptions) *http.Client {
	noRedirect := func(req *http.Request, via []*http.Request) error {
		return http.ErrUseLastResponse
	}
	if options.Client == nil {
		return &http.Client{CheckRedirect: noRedirect}
	}
	return &http.Client{
		Transport:     options.Client.Transport,
		Timeout:       options.Client.Timeout,
		CheckRedirect: noRedirect,
	}
}

// CrawlHTTP concurrently crawls the supplied seeds under global and
// per-origin concurrency limits plus a per-origin access interval.
// Cross-origin redirects are scheduled like any other request: each
// physical GET, redirect hops included, occupies a global slot and a
// destination-origin slot until its response body has been consumed
// or closed, and respects the origin's interval. One Page is returned
// for every distinct accepted seed/discovered URL; redirect-only
// intermediates produce no Page.
func CrawlHTTP(seeds []string, options HTTPOptions, follow FollowLink) ([]*Page, error) {
	if err := validateOptions(options); err != nil {
		return nil, err
	}

	normalizedSeeds := make([]string, 0, len(seeds))
	for _, seed := range seeds {
		normalized, err := normalizeURL(seed)
		if err != nil {
			return nil, err
		}
		normalizedSeeds = append(normalizedSeeds, normalized)
	}
	if len(normalizedSeeds) == 0 {
		return []*Page{}, nil
	}

	s := &httpScheduler{
		options:  options,
		client:   buildClient(options),
		follow:   follow,
		accepted: map[string]bool{},
		pages:    []*Page{},
		targets:  map[string]*physicalTarget{},
		gates:    map[string]*originGate{},
		work:     make(chan string),
		finished: make(chan physicalResult),
	}

	// Seeds bypass follow; configuration and seeds have already been
	// validated before any request is made.
	for _, seed := range normalizedSeeds {
		s.admit(seed, seed)
	}

	s.run()
	return s.pages, nil
}

// gateOf returns the scheduling gate for a physical URL's origin.
func (s *httpScheduler) gateOf(physicalURL string) *originGate {
	key := originOf(physicalURL)
	gate, ok := s.gates[key]
	if !ok {
		gate = &originGate{}
		s.gates[key] = gate
	}
	return gate
}

// getOrCreateTarget returns the target for physicalURL, queueing it
// the first time the URL is seen. Each normalized URL is therefore
// fetched at most once per call.
func (s *httpScheduler) getOrCreateTarget(physicalURL string) *physicalTarget {
	target, ok := s.targets[physicalURL]
	if !ok {
		target = &physicalTarget{url: physicalURL, state: stateQueued}
		s.targets[physicalURL] = target
		s.queue = append(s.queue, target)
	}
	return target
}

// admit registers a logical entry against physicalURL's work. The
// entry's own URL is preserved on its eventual Page. Entries
// converging on a queued, running, redirecting or completed target
// attach to the chain head instead of triggering another GET. A
// logical URL already accepted is ignored so each distinct entry
// produces exactly one Page.
func (s *httpScheduler) admit(physicalURL, logicalURL string) {
	if s.accepted[logicalURL] {
		return
	}
	s.accepted[logicalURL] = true

	target := s.getOrCreateTarget(physicalURL)
	head := target.head()
	entry := &logicalEntry{logicalURL: logicalURL}
	if head.state == stateTerminal {
		s.settleEntry(entry, head.outcome)
	} else {
		head.waiters = append(head.waiters, entry)
	}
}

// settleEntry produces the Page for one logical entry, sharing the
// terminal content or error of the physical work it converged on.
func (s *httpScheduler) settleEntry(entry *logicalEntry, outcome *physicalOutcome) {
	page := &Page{URL: entry.logicalURL, Links: []string{}, Assets: []string{}}
	if outcome.err != nil {
		page.FetcherError = outcome.err
	} else {
		page.Links = outcome.links
		page.Assets = outcome.assets
	}
	s.pages = append(s.pages, page)
}

func (s *httpScheduler) run() {
	var workerWg sync.WaitGroup
	for i := 0; i < s.options.MaxWorkers; i++ {
		workerWg.Add(1)
		go s.worker(&workerWg)
	}

	for !(s.active == 0 && len(s.queue) == 0) {
		nextTarget, timer := s.plan()
		var timerCh <-chan time.Time
		if timer != nil {
			timerCh = timer.C
		}
		if nextTarget != nil {
			select {
			case s.work <- nextTarget.url:
				s.markRunning(nextTarget)
			case res := <-s.finished:
				s.complete(res)
			case <-timerCh:
				// A gated origin reached its readiness time; re-plan.
			}
		} else {
			select {
			case res := <-s.finished:
				s.complete(res)
			case <-timerCh:
				// The only queued origins are waiting out their
				// interval; no worker slot is held while doing so.
			}
		}
		if timer != nil {
			timer.Stop()
		}
	}

	close(s.work)
	workerWg.Wait()
}

// plan inspects the queue and returns a target eligible to start now,
// or nil when every queued target is gated. The returned timer fires
// at the earliest origin readiness so the loop wakes to re-plan.
func (s *httpScheduler) plan() (*physicalTarget, *time.Timer) {
	var earliest time.Time
	now := time.Now()

	for _, target := range s.queue {
		gate := s.gateOf(target.url)
		if gate.active >= s.options.MaxPerOrigin {
			continue
		}
		if s.active >= s.options.MaxWorkers {
			continue
		}
		if !gate.nextReadyAt.IsZero() && now.Before(gate.nextReadyAt) {
			if earliest.IsZero() || gate.nextReadyAt.Before(earliest) {
				earliest = gate.nextReadyAt
			}
			continue
		}
		return target, nil
	}

	if earliest.IsZero() {
		return nil, nil
	}
	delay := time.Until(earliest)
	if delay < 0 {
		delay = 0
	}
	return nil, time.NewTimer(delay)
}

func (s *httpScheduler) markRunning(target *physicalTarget) {
	for i, queued := range s.queue {
		if queued == target {
			s.queue = append(s.queue[:i], s.queue[i+1:]...)
			break
		}
	}
	target.state = stateRunning
	s.gateOf(target.url).active++
	s.active++
}

// complete settles one physical GET. The response body has already
// been closed by the worker, so the origin and global slots are
// released before a redirect destination is arranged.
func (s *httpScheduler) complete(res physicalResult) {
	target := s.targets[res.url]
	gate := s.gates[res.origin]
	gate.active--
	s.active--
	// A response actually finished: the origin's next start must wait
	// out the interval from this instant. Transport failures never
	// received a response and leave the previous timing untouched.
	if res.statusCode != 0 {
		gate.nextReadyAt = time.Now().Add(s.options.MinOriginInterval)
	}

	switch {
	case res.err != nil:
		s.terminate(target, &physicalOutcome{err: res.err})
	case res.redirect:
		s.followRedirect(target, res.nextHop)
	default:
		outcome := &physicalOutcome{links: res.links, assets: res.assets}
		s.terminate(target, outcome)
		s.discover(target, outcome.links)
	}
}

// terminate publishes a terminal outcome to every logical entry
// attached to the target.
func (s *httpScheduler) terminate(target *physicalTarget, outcome *physicalOutcome) {
	target.state = stateTerminal
	target.forward = nil
	target.outcome = outcome
	for _, entry := range target.waiters {
		s.settleEntry(entry, outcome)
	}
	target.waiters = nil
}

// followRedirect attaches the redirecting target's chain to its
// destination. Redirect destinations bypass follow because the
// request is completing an already accepted page. A dependency cycle
// fails every target in the cycle together with all predecessors that
// forwarded into it.
func (s *httpScheduler) followRedirect(target *physicalTarget, nextHop string) {
	destination := s.getOrCreateTarget(nextHop)

	// Walk the destination's forward chain before linking target in.
	// Reaching target itself closes a cycle through target; repeating
	// another node means that node already sits in a loop.
	path := []*physicalTarget{}
	seen := map[*physicalTarget]bool{}
	current := destination
	for current != nil && !seen[current] {
		if current == target {
			s.failCycle(append(path, target))
			return
		}
		seen[current] = true
		path = append(path, current)
		current = current.forward
	}
	if current != nil {
		// Defensive: an already-open loop was reached.
		s.failCycle(append(path, current))
		return
	}

	target.state = stateRedirect
	target.forward = destination
	head := destination.head()
	for _, entry := range target.waiters {
		if head.state == stateTerminal {
			s.settleEntry(entry, head.outcome)
		} else {
			head.waiters = append(head.waiters, entry)
		}
	}
	target.waiters = nil
}

// failCycle terminates every member of a redirect cycle with a shared
// FetcherError; predecessors that forwarded into a member keep their
// own forward link and inherit the error when they settle.
func (s *httpScheduler) failCycle(members []*physicalTarget) {
	outcome := &physicalOutcome{err: errRedirectCycle}
	for _, member := range members {
		s.removeFromQueue(member)
		s.terminate(member, outcome)
	}
}

func (s *httpScheduler) removeFromQueue(target *physicalTarget) {
	for i, queued := range s.queue {
		if queued == target {
			s.queue = append(s.queue[:i], s.queue[i+1:]...)
			return
		}
	}
}

// discover enqueues the outgoing links of a successful terminal
// document exactly once. Converging shares never multiply discovery,
// unsupported schemes are not schedulable, and ordinary links must
// pass follow. A link that points at an existing redirect-only target
// is still accepted on its own merits and converges onto that chain.
func (s *httpScheduler) discover(target *physicalTarget, links []string) {
	if target.expanded {
		return
	}
	target.expanded = true

	for _, link := range links {
		normalized, err := normalizeURL(link)
		if err != nil {
			continue
		}
		if s.follow != nil && !s.follow(normalized) {
			continue
		}
		s.admit(normalized, normalized)
	}
}

func (s *httpScheduler) worker(wg *sync.WaitGroup) {
	defer wg.Done()
	for physicalURL := range s.work {
		s.finished <- s.fetchPhysical(physicalURL)
	}
}

// fetchPhysical performs one scheduled GET. It returns only after the
// response body has been consumed or closed, so the slot release in
// complete() always happens after the body is shut. Redirects are
// returned to the scheduler rather than followed.
func (s *httpScheduler) fetchPhysical(physicalURL string) physicalResult {
	res := physicalResult{url: physicalURL, origin: originOf(physicalURL)}

	req, err := http.NewRequest(http.MethodGet, physicalURL, nil)
	if err != nil {
		res.err = err
		return res
	}

	resp, err := s.client.Do(req)
	if err != nil {
		res.err = err
		return res
	}
	// The crawler owns this body for exactly one Close.
	defer resp.Body.Close()
	res.statusCode = resp.StatusCode

	if redirectStatuses[resp.StatusCode] {
		next, err := resolveRedirect(physicalURL, resp.Header.Get("Location"))
		if err != nil {
			res.err = err
			return res
		}
		res.redirect = true
		res.nextHop = next
		return res
	}

	if resp.StatusCode != http.StatusOK {
		res.err = fmt.Errorf("fetch failed: HTTP status %d", resp.StatusCode)
		return res
	}

	// Relative links resolve against the final response URL. With
	// redirects disabled this is the hop that answered successfully.
	finalURL, err := url.Parse(resp.Request.URL.String())
	if err != nil {
		res.err = err
		return res
	}

	// A body that fails halfway through reading yields an error and
	// no links from the partial prefix are exposed or expanded.
	links, assets, err := parseDocument(resp.Body, finalURL)
	if err != nil {
		res.err = err
		return res
	}
	res.links = links
	res.assets = assets
	return res
}

var redirectStatuses = map[int]bool{
	http.StatusMovedPermanently:  true, // 301
	http.StatusFound:             true, // 302
	http.StatusSeeOther:          true, // 303
	http.StatusTemporaryRedirect: true, // 307
	http.StatusPermanentRedirect: true, // 308
}

// resolveRedirect validates a Location header and resolves it against
// the hop's own URL, the same base a browser would use.
func resolveRedirect(hopURL, location string) (string, error) {
	if strings.TrimSpace(location) == "" {
		return "", errNoLocation
	}
	base, err := url.Parse(hopURL)
	if err != nil {
		return "", err
	}
	ref, err := url.Parse(location)
	if err != nil {
		return "", fmt.Errorf("invalid redirect Location %q: %w", location, err)
	}
	resolved := base.ResolveReference(ref)
	if resolved.Scheme != "http" && resolved.Scheme != "https" {
		return "", fmt.Errorf("unsupported redirect scheme %q", resolved.Scheme)
	}
	return normalizeURL(resolved.String())
}
