package gocrawl

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"io/ioutil"
	"net/http"
	"net/http/httptest"
	"net/url"
	"reflect"
	"sort"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/PuerkitoBio/gocrawl/task_support/taskcheck"
	"github.com/PuerkitoBio/goquery"
	"github.com/PuerkitoBio/purell"
	"github.com/temoto/robotstxt"
)

// This file is a concurrency-protocol regression suite. It drives the real
// Crawler.Run with real URLContext values and a real Extender, and checks the
// crawl against an event model derived from declaratively specified site
// rules, page contents and redirect relations.
//
// Expected events are computed from the declared plan (including an
// independent evaluation of the robots.txt rules), never reverse-engineered
// from an actual run log and never copied from the worker's branches.
//
// Synchronization uses a buffered event recorder and explicit channel
// barriers. No sleep is used for ordering: the only timer is a deadlock
// watchdog that fails the test and dumps the recorded events.

// protoWatchdog only breaks a deadlock; it never schedules ordering.
const protoWatchdog = 15 * time.Second

// Event kinds recorded through the crawl.
const (
	evStart         = "Start"
	evEnd           = "End"
	evFilter        = "Filter"
	evEnqueued      = "Enqueued"
	evDisallowed    = "Disallowed"
	evFetch         = "Fetch"
	evFetchRedirect = "FetchRedirect"
	evRequestGet    = "RequestGet"
	evRobotsFetched = "FetchedRobots"
	evVisit         = "Visit"
	evVisited       = "Visited"
)

// ---------------------------------------------------------------------------
// Declarative site plan: the single source of truth from which both the
// serving side and the expected event model are derived.
// ---------------------------------------------------------------------------

// protoPage declares one page. Every normal page body carries a relative
// anchor plus non-text binary bytes so body identity is checkable end to end.
type protoPage struct {
	path      string
	status    int
	headDeny  bool // RequestGet vetoes the GET after the HEAD
	body      []byte
	links     []string // declared links, relative or absolute
	htmlLinks bool     // when true, production code harvests the links itself
	visitHold string   // non-empty: the Visit hook blocks on this named gate
	fetchHold string   // non-empty: the Fetch hook blocks on this named gate
}

// protoSite declares one host: its robots text, pages and redirect relations.
type protoSite struct {
	host      string
	robotsTxt string
	pages     map[string]*protoPage
	redirects map[string]string // path -> absolute destination URL
}

// protoSeed is a declared seed URL with its State token.
type protoSeed struct {
	rawURL string
	state  string
}

// protoPlan is everything declared before a run starts. When gateVisits is
// set, every Visit hook enters that gate, used by the MaxVisits scenario.
type protoPlan struct {
	name          string
	headBeforeGet bool
	sameHostOnly  bool
	gateVisits    string
	sites         []*protoSite
	seeds         []protoSeed
}

func (p *protoPlan) site(host string) *protoSite {
	for _, s := range p.sites {
		if s.host == host {
			return s
		}
	}
	return nil
}

func (p *protoPlan) page(host, path string) *protoPage {
	if s := p.site(host); s != nil {
		return s.pages[path]
	}
	return nil
}

// pageBody builds a body that always contains a relative anchor and a binary
// byte tail, so that reading/parsing must preserve the exact bytes.
func pageBody(title, relLink string, extraLinks ...string) []byte {
	buf := []byte("<html><head><title>" + title + "</title></head><body><h1>" + title + "</h1>")
	buf = append(buf, []byte(`<a href="`+relLink+`">relative</a>`)...)
	for _, l := range extraLinks {
		buf = append(buf, []byte(`<a href="`+l+`">link</a>`)...)
	}
	buf = append(buf, 0x00, 0x01, 0xFE, 0xFF, 0x7F, 0x0A)
	buf = append(buf, []byte("</body></html>")...)
	return buf
}

// ---------------------------------------------------------------------------
// Tracked response bodies: real io.ReadCloser values handed to production
// code, which stays fully responsible for reading and closing them.
// ---------------------------------------------------------------------------

type trackedBody struct {
	id     string
	reader io.Reader
	closed int32
}

func newTrackedBody(id string, content []byte) *trackedBody {
	return &trackedBody{id: id, reader: strings.NewReader(string(content))}
}

func (b *trackedBody) Read(p []byte) (int, error) { return b.reader.Read(p) }

func (b *trackedBody) Close() error {
	atomic.AddInt32(&b.closed, 1)
	return nil
}

func (b *trackedBody) closeCount() int { return int(atomic.LoadInt32(&b.closed)) }

// ---------------------------------------------------------------------------
// Explicit channel gates, used instead of any timing-based synchronization.
// ---------------------------------------------------------------------------

type protoGate struct {
	entered chan struct{}
	release chan struct{}
	once    sync.Once
}

func newProtoGate() *protoGate {
	return &protoGate{entered: make(chan struct{}), release: make(chan struct{})}
}

func (g *protoGate) markEntered() { g.once.Do(func() { close(g.entered) }) }

// ---------------------------------------------------------------------------
// Buffered event recorder with subscription barriers.
// ---------------------------------------------------------------------------

type protoEvent struct {
	seq    int64
	kind   string
	host   string
	path   string
	method string // GET, HEAD or ""
	source string // normalized source URL, "" for seeds/robots
	state  string // State token rendered as string, "" when State is nil
	note   string // verdict, redirect destination, body hash, ...
}

// key renders the identity part of an event for multiset comparison.
func (e protoEvent) key() string {
	return fmt.Sprintf("%s|%s|%s|%s|src=%s|state=%s|note=%s",
		e.kind, e.host, e.path, e.method, e.source, e.state, e.note)
}

type protoWatcher struct {
	match func(protoEvent) bool
	ch    chan struct{}
}

type protoRecorder struct {
	mu       sync.Mutex
	seq      int64
	events   []protoEvent
	watchers []protoWatcher
	bodies   map[string]*trackedBody
	inFlight int64
}

func newProtoRecorder() *protoRecorder {
	return &protoRecorder{bodies: make(map[string]*trackedBody)}
}

func (r *protoRecorder) enter() int64 { return atomic.AddInt64(&r.inFlight, 1) }
func (r *protoRecorder) leave()       { atomic.AddInt64(&r.inFlight, -1) }

func (r *protoRecorder) add(kind, host, path, method, source string, state interface{}, note string) protoEvent {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.seq++
	ev := protoEvent{
		seq:    r.seq,
		kind:   kind,
		host:   host,
		path:   path,
		method: method,
		source: source,
		state:  stateLabel(state),
		note:   note,
	}
	r.events = append(r.events, ev)
	kept := r.watchers[:0]
	for _, w := range r.watchers {
		if w.match(ev) {
			close(w.ch)
		} else {
			kept = append(kept, w)
		}
	}
	r.watchers = kept
	return ev
}

// expect returns a barrier that fires as soon as a matching event exists,
// including events recorded before the call.
func (r *protoRecorder) expect(match func(protoEvent) bool) <-chan struct{} {
	ch := make(chan struct{})
	r.mu.Lock()
	defer r.mu.Unlock()
	for _, ev := range r.events {
		if match(ev) {
			close(ch)
			return ch
		}
	}
	r.watchers = append(r.watchers, protoWatcher{match: match, ch: ch})
	return ch
}

func (r *protoRecorder) registerBody(b *trackedBody) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.bodies[b.id] = b
}

func (r *protoRecorder) snapshot() []protoEvent {
	r.mu.Lock()
	defer r.mu.Unlock()
	out := make([]protoEvent, len(r.events))
	copy(out, r.events)
	return out
}

func (r *protoRecorder) count(match func(protoEvent) bool) int {
	n := 0
	for _, ev := range r.snapshot() {
		if match(ev) {
			n++
		}
	}
	return n
}

func (r *protoRecorder) firstIndex(match func(protoEvent) bool) int {
	for i, ev := range r.snapshot() {
		if match(ev) {
			return i
		}
	}
	return -1
}

func (r *protoRecorder) dump() string {
	evs := r.snapshot()
	out := fmt.Sprintf("inFlight=%d bodies=%d events=%d\n", atomic.LoadInt64(&r.inFlight), len(r.bodies), len(evs))
	for _, ev := range evs {
		out += fmt.Sprintf("  #%d %s host=%s path=%s method=%s src=%s state=%q note=%s\n",
			ev.seq, ev.kind, ev.host, ev.path, ev.method, ev.source, ev.state, ev.note)
	}
	return out
}

func stateLabel(v interface{}) string {
	if v == nil {
		return ""
	}
	return fmt.Sprint(v)
}

// ---------------------------------------------------------------------------
// Expected model, derived from the declared plan.
// ---------------------------------------------------------------------------

type protoModel struct {
	plan        *protoPlan
	allowed     map[string]map[string]bool // host -> path -> robots admission
	eventCounts map[string]int             // expected event multiset
	bodySlots   map[string]int             // host|path|method -> response count
	visited     map[string]map[string]bool
}

// robotsAdmission independently evaluates the declared robots.txt text. It is
// a specification-level evaluation, not the worker's policy code.
func robotsAdmission(robotUA, robotsText string, paths []string) map[string]bool {
	out := make(map[string]bool, len(paths))
	data, err := robotstxt.FromBytes([]byte(robotsText))
	if err != nil {
		panic(fmt.Sprintf("declared robots.txt is invalid: %v", err))
	}
	group := data.FindGroup(robotUA)
	for _, p := range paths {
		out[p] = group.Test(p)
	}
	return out
}

type modelURL struct {
	raw    string
	host   string
	path   string
	source string
	state  string
}

func normalizeProtoURL(u *url.URL) *url.URL {
	purell.NormalizeURL(u, purell.FlagsAllGreedy)
	return u
}

func parseMust(raw string) *url.URL {
	u, err := url.Parse(raw)
	if err != nil {
		panic(fmt.Sprintf("invalid URL %q: %v", raw, err))
	}
	return normalizeProtoURL(u)
}

func resolveURL(raw, source string) string {
	u, err := url.Parse(raw)
	if err != nil {
		panic(fmt.Sprintf("invalid declared URL %q: %v", raw, err))
	}
	if u.IsAbs() {
		return parseMust(raw).String()
	}
	src, err := url.Parse(source)
	if err != nil {
		panic(fmt.Sprintf("invalid declared source %q: %v", source, err))
	}
	return normalizeProtoURL(src.ResolveReference(u)).String()
}

// deriveProtoModel walks the declared graph the way the specification says the
// crawl must unfold, producing the exact multiset of allowed events and the
// set of response bodies that must be closed exactly once.
func deriveProtoModel(plan *protoPlan, robotUA string) *protoModel {
	m := &protoModel{
		plan:        plan,
		allowed:     make(map[string]map[string]bool),
		eventCounts: make(map[string]int),
		bodySlots:   make(map[string]int),
		visited:     make(map[string]map[string]bool),
	}

	for _, s := range plan.sites {
		paths := make([]string, 0, len(s.pages)+2*len(s.redirects))
		for p := range s.pages {
			paths = append(paths, p)
		}
		// Redirect source paths go through robots admission exactly like pages,
		// and so do their destinations once they are re-enqueued.
		for p, dst := range s.redirects {
			paths = append(paths, p)
			if u, err := url.Parse(dst); err == nil && u.Host == s.host {
				paths = append(paths, u.Path)
			}
		}
		sort.Strings(paths)
		m.allowed[s.host] = robotsAdmission(robotUA, s.robotsTxt, paths)
		m.visited[s.host] = map[string]bool{}
	}

	add := func(ev protoEvent) { m.eventCounts[ev.key()]++ }
	addBody := func(host, path, method string) {
		m.bodySlots[host+"|"+path+"|"+method]++
	}

	hostsStarted := map[string]bool{}
	queue := make([]modelURL, 0, len(plan.seeds))
	for _, sd := range plan.seeds {
		u := parseMust(sd.rawURL)
		queue = append(queue, modelURL{
			raw:    u.String(),
			host:   u.Host,
			path:   u.Path,
			source: "",
			state:  sd.state,
		})
	}

	for len(queue) > 0 {
		item := queue[0]
		queue = queue[1:]
		site := plan.site(item.host)
		if site == nil {
			panic("declared URL points at undeclared host: " + item.raw)
		}

		// Filter/dedupe, mirroring the declared normalization policy.
		seen := m.visited[item.host][item.path]
		add(protoEvent{kind: evFilter, host: item.host, path: item.path, source: item.source, state: item.state, note: fmt.Sprintf("isVisited=%v", seen)})
		if seen {
			continue
		}

		// First URL of a host launches the worker and enqueues robots.txt.
		if !hostsStarted[item.host] {
			hostsStarted[item.host] = true
			add(protoEvent{kind: evEnqueued, host: item.host, path: "/robots.txt", note: "robots=true"})
			add(protoEvent{kind: evFetch, host: item.host, path: "/robots.txt", method: "GET", note: ""})
			addBody(item.host, "/robots.txt", "GET")
			add(protoEvent{kind: evRobotsFetched, host: item.host, path: "/robots.txt"})
		}

		m.visited[item.host][item.path] = true
		add(protoEvent{kind: evEnqueued, host: item.host, path: item.path, source: item.source, state: item.state})

		// Robots admission, derived independently from the declared rules.
		if !m.allowed[item.host][item.path] {
			add(protoEvent{kind: evDisallowed, host: item.host, path: item.path, source: item.source, state: item.state})
			continue
		}

		// Declared redirect: a fetch without body or visit. With HEAD-before-GET
		// the redirecting request is itself a HEAD.
		if dst, ok := site.redirects[item.path]; ok {
			redirMethod := "GET"
			if plan.headBeforeGet {
				redirMethod = "HEAD"
			}
			add(protoEvent{kind: evFetch, host: item.host, path: item.path, method: redirMethod, source: item.source, state: item.state})
			add(protoEvent{kind: evFetchRedirect, host: item.host, path: item.path, method: redirMethod, source: item.source, state: item.state, note: dst})
			ru := parseMust(dst)
			queue = append(queue, modelURL{
				raw:    ru.String(),
				host:   ru.Host,
				path:   ru.Path,
				source: item.raw,
				state:  item.state, // State propagates across the redirect
			})
			continue
		}

		page := site.pages[item.path]
		if page == nil {
			panic(fmt.Sprintf("declared URL has no page/redirect: %s%s", item.host, item.path))
		}

		if plan.headBeforeGet {
			add(protoEvent{kind: evFetch, host: item.host, path: item.path, method: "HEAD", source: item.source, state: item.state})
			addBody(item.host, item.path, "HEAD")
			add(protoEvent{kind: evRequestGet, host: item.host, path: item.path, source: item.source, state: item.state, note: fmt.Sprintf("allow=%v", !page.headDeny)})
			if page.headDeny {
				continue
			}
		}

		add(protoEvent{kind: evFetch, host: item.host, path: item.path, method: "GET", source: item.source, state: item.state})
		addBody(item.host, item.path, "GET")
		add(protoEvent{kind: evVisit, host: item.host, path: item.path, method: "GET", source: item.source, state: item.state, note: hashBytes(page.body)})
		add(protoEvent{kind: evVisited, host: item.host, path: item.path, source: item.source, state: item.state})

		for _, l := range page.links {
			resolved := resolveURL(l, item.raw)
			ru := parseMust(resolved)
			queue = append(queue, modelURL{
				raw:    ru.String(),
				host:   ru.Host,
				path:   ru.Path,
				source: item.raw,
				state:  "", // harvested URLs start without State
			})
		}
	}
	return m
}

func hashBytes(b []byte) string {
	sum := sha256.Sum256(b)
	return hex.EncodeToString(sum[:])
}

// observedCounts turns the recorder into the same multiset shape as the model.
func observedCounts(rec *protoRecorder) map[string]int {
	out := map[string]int{}
	for _, ev := range rec.snapshot() {
		if ev.kind == evStart || ev.kind == evEnd {
			continue
		}
		out[ev.key()]++
	}
	return out
}

// checkModelEquality fails on every mismatched identity, naming the resource.
func checkModelEquality(c *taskcheck.C, rec *protoRecorder, m *protoModel) {
	obs := observedCounts(rec)
	for key, want := range m.eventCounts {
		got := obs[key]
		c.Check(got == want, "event mismatch for %s: model allows %d occurrence(s), recorded %d", key, want, got)
	}
	for key, got := range obs {
		_, want := m.eventCounts[key]
		c.Check(want, "unexpected event outside the declared model: %s (recorded %d)", key, got)
	}

	// Every response body handed to production code is closed exactly once.
	gotSlots := map[string]int{}
	rec.mu.Lock()
	for id, b := range rec.bodies {
		slot := id
		if i := strings.LastIndex(id, "#"); i >= 0 {
			slot = id[:i]
		}
		gotSlots[slot]++
		c.Check(b.closeCount() == 1, "body %s closed %d time(s), want exactly 1", id, b.closeCount())
	}
	rec.mu.Unlock()
	for slot, want := range m.bodySlots {
		c.Check(gotSlots[slot] == want, "body slot %s: model expects %d response(s), recorder tracks %d", slot, want, gotSlots[slot])
	}
}

// checkHappensBefore asserts a causal ordering. It is only used inside one
// host; cross-host order is never asserted.
func checkHappensBefore(c *taskcheck.C, rec *protoRecorder, desc string, first, second func(protoEvent) bool) {
	i := rec.firstIndex(first)
	j := rec.firstIndex(second)
	c.Check(i >= 0 && j >= 0 && i < j, "causal relation %q not observed: first index=%d second index=%d\n%s", desc, i, j, rec.dump())
}

func evIs(kind, host, path, method string) func(protoEvent) bool {
	return func(e protoEvent) bool {
		return e.kind == kind && e.host == host && e.path == path && (method == "" || e.method == method)
	}
}

// waitBarrier blocks until the barrier fires; the watchdog exists only to
// surface a deadlock with a full event dump.
func waitBarrier(t *testing.T, rec *protoRecorder, ch <-chan struct{}, what string) {
	t.Helper()
	select {
	case <-ch:
	case <-time.After(protoWatchdog):
		t.Fatalf("deadlock waiting for %s\n%s", what, rec.dump())
	}
}

// ---------------------------------------------------------------------------
// Extender: in-memory Fetch builds real responses, but production code reads,
// closes and parses them. The HTTP variant wraps DefaultExtender.Fetch so real
// redirects go through the package-level HttpClient path.
// ---------------------------------------------------------------------------

type protoExtender struct {
	DefaultExtender
	plan      *protoPlan
	rec       *protoRecorder
	bodySeq   int64
	gates     map[string]*protoGate
	gatesMu   sync.Mutex
	harvest   map[string]interface{}
	harvestMu sync.Mutex
}

func newProtoExtender(plan *protoPlan, rec *protoRecorder) *protoExtender {
	return &protoExtender{
		plan:    plan,
		rec:     rec,
		gates:   map[string]*protoGate{},
		harvest: map[string]interface{}{},
	}
}

func (e *protoExtender) gate(name string) *protoGate {
	e.gatesMu.Lock()
	defer e.gatesMu.Unlock()
	g, ok := e.gates[name]
	if !ok {
		g = newProtoGate()
		e.gates[name] = g
	}
	return g
}

func (e *protoExtender) hostOf(ctx *URLContext) string {
	return ctx.NormalizedURL().Host
}

func (e *protoExtender) sourceOf(ctx *URLContext) string {
	if s := ctx.NormalizedSourceURL(); s != nil {
		return s.String()
	}
	return ""
}

func (e *protoExtender) Start(seeds interface{}) interface{} {
	e.rec.enter()
	defer e.rec.leave()
	e.rec.add(evStart, "", "", "", "", nil, "")
	return seeds
}

func (e *protoExtender) End(err error) {
	e.rec.enter()
	defer e.rec.leave()
	note := ""
	if err != nil {
		note = err.Error()
	}
	e.rec.add(evEnd, "", "", "", "", nil, note)
}

func (e *protoExtender) Error(*CrawlError) {
	// Expected protocol outcomes (non-2xx, ...) are covered by the event model.
}

func (e *protoExtender) ComputeDelay(string, *DelayInfo, *FetchInfo) time.Duration {
	return 0
}

func (e *protoExtender) RequestRobots(*URLContext, string) ([]byte, bool) {
	return nil, true
}

func (e *protoExtender) FetchedRobots(ctx *URLContext, _ *http.Response) {
	e.rec.enter()
	defer e.rec.leave()
	e.rec.add(evRobotsFetched, e.hostOf(ctx), ctx.NormalizedURL().Path, "", e.sourceOf(ctx), ctx.State, "")
}

func (e *protoExtender) Filter(ctx *URLContext, isVisited bool) bool {
	e.rec.enter()
	defer e.rec.leave()
	e.rec.add(evFilter, e.hostOf(ctx), ctx.NormalizedURL().Path, "", e.sourceOf(ctx), ctx.State, fmt.Sprintf("isVisited=%v", isVisited))
	return !isVisited
}

func (e *protoExtender) Enqueued(ctx *URLContext) {
	e.rec.enter()
	defer e.rec.leave()
	note := ""
	if ctx.IsRobotsURL() {
		note = "robots=true"
	}
	e.rec.add(evEnqueued, e.hostOf(ctx), ctx.NormalizedURL().Path, "", e.sourceOf(ctx), ctx.State, note)
}

func (e *protoExtender) Disallowed(ctx *URLContext) {
	e.rec.enter()
	defer e.rec.leave()
	e.rec.add(evDisallowed, e.hostOf(ctx), ctx.NormalizedURL().Path, "", e.sourceOf(ctx), ctx.State, "")
}

func (e *protoExtender) RequestGet(ctx *URLContext, _ *http.Response) bool {
	e.rec.enter()
	defer e.rec.leave()
	allow := true
	if page := e.plan.page(e.hostOf(ctx), ctx.NormalizedURL().Path); page != nil {
		allow = !page.headDeny
	}
	e.rec.add(evRequestGet, e.hostOf(ctx), ctx.NormalizedURL().Path, "", e.sourceOf(ctx), ctx.State, fmt.Sprintf("allow=%v", allow))
	return allow
}

func (e *protoExtender) nextBodyID(ctx *URLContext, method string) string {
	n := atomic.AddInt64(&e.bodySeq, 1)
	return fmt.Sprintf("%s|%s|%s#%d", e.hostOf(ctx), ctx.NormalizedURL().Path, method, n)
}

func (e *protoExtender) wrapBody(ctx *URLContext, method string, content []byte) *trackedBody {
	body := newTrackedBody(e.nextBodyID(ctx, method), content)
	e.rec.registerBody(body)
	return body
}

// maybeFetchHold blocks a content-page Fetch on its declared gate. Robots.txt
// fetches are never held, so a worker can always finish its robots handshake.
func (e *protoExtender) maybeFetchHold(ctx *URLContext) {
	if ctx.IsRobotsURL() {
		return
	}
	if page := e.plan.page(e.hostOf(ctx), ctx.NormalizedURL().Path); page != nil && page.fetchHold != "" {
		g := e.gate(page.fetchHold)
		g.markEntered()
		<-g.release
	}
}

// Fetch is the in-memory implementation. It constructs real http.Response
// values with real Readers and lets the worker read, parse and close them.
// Redirects are reported exactly like the real client, so cloneForRedirect
// stays inside production code.
func (e *protoExtender) Fetch(ctx *URLContext, agent string, headRequest bool) (*http.Response, error) {
	e.rec.enter()
	defer e.rec.leave()
	method := "GET"
	if headRequest {
		method = "HEAD"
	}
	host := e.hostOf(ctx)
	path := ctx.NormalizedURL().Path
	site := e.plan.site(host)
	e.maybeFetchHold(ctx)

	if site != nil {
		if dst, ok := site.redirects[path]; ok {
			e.rec.add(evFetch, host, path, method, e.sourceOf(ctx), ctx.State, "")
			e.rec.add(evFetchRedirect, host, path, method, e.sourceOf(ctx), ctx.State, dst)
			return nil, &url.Error{Op: method, URL: dst, Err: ErrEnqueueRedirect}
		}
	}

	req, _ := http.NewRequest(method, ctx.url.String(), nil)
	res := &http.Response{
		Status:     "200 OK",
		StatusCode: 200,
		Header:     make(http.Header),
		Request:    req,
	}
	content := []byte{}
	if site != nil && ctx.IsRobotsURL() {
		content = []byte(site.robotsTxt)
	} else if page := e.plan.page(host, path); page != nil {
		res.StatusCode = page.status
		res.Status = fmt.Sprintf("%d status", page.status)
		content = page.body
	}
	if headRequest {
		content = []byte{}
	}
	res.Body = e.wrapBody(ctx, method, content)
	e.rec.add(evFetch, host, path, method, e.sourceOf(ctx), ctx.State, "")
	return res, nil
}

func (e *protoExtender) Visit(ctx *URLContext, res *http.Response, _ *goquery.Document) (interface{}, bool) {
	e.rec.enter()
	defer e.rec.leave()
	host := e.hostOf(ctx)
	path := ctx.NormalizedURL().Path

	body, err := ioutil.ReadAll(res.Body)
	if err != nil {
		return nil, false
	}
	e.rec.add(evVisit, host, path, "GET", e.sourceOf(ctx), ctx.State, hashBytes(body))

	if e.plan.gateVisits != "" {
		g := e.gate(e.plan.gateVisits)
		g.markEntered()
		<-g.release
	}
	page := e.plan.page(host, path)
	if page != nil && page.visitHold != "" {
		g := e.gate(page.visitHold)
		g.markEntered()
		<-g.release
	}

	if page != nil && page.htmlLinks {
		// Production code parses and harvests the relative links itself.
		return nil, true
	}
	if page == nil || len(page.links) == 0 {
		return nil, false
	}
	harvested := make([]*url.URL, 0, len(page.links))
	for _, l := range page.links {
		harvested = append(harvested, parseMust(resolveURL(l, ctx.URL().String())))
	}
	e.harvestMu.Lock()
	e.harvest[host+"|"+path] = harvested
	e.harvestMu.Unlock()
	return harvested, false
}

func (e *protoExtender) Visited(ctx *URLContext, harvested interface{}) {
	e.rec.enter()
	defer e.rec.leave()
	host := e.hostOf(ctx)
	path := ctx.NormalizedURL().Path
	key := host + "|" + path
	e.harvestMu.Lock()
	want, ok := e.harvest[key]
	e.harvestMu.Unlock()
	note := ""
	if ok && !sameSliceBacking(want, harvested) {
		// Visited must receive the exact value Visit returned.
		note = "HARVEST-MISMATCH"
	}
	e.rec.add(evVisited, host, path, "", e.sourceOf(ctx), ctx.State, note)
}

func sameSliceBacking(a, b interface{}) bool {
	va, oka := a.([]*url.URL)
	vb, okb := b.([]*url.URL)
	if !oka || !okb || len(va) != len(vb) {
		return false
	}
	if len(va) == 0 {
		return true
	}
	return reflect.ValueOf(va).Pointer() == reflect.ValueOf(vb).Pointer()
}

// ---------------------------------------------------------------------------
// HTTP variant: real DefaultExtender.Fetch against local httptest servers;
// only the returned bodies are wrapped so closes stay observable.
// ---------------------------------------------------------------------------

type httpProtoExtender struct {
	*protoExtender
}

func (e *httpProtoExtender) Fetch(ctx *URLContext, agent string, headRequest bool) (*http.Response, error) {
	e.rec.enter()
	defer e.rec.leave()
	method := "GET"
	if headRequest {
		method = "HEAD"
	}
	host := e.hostOf(ctx)
	path := ctx.NormalizedURL().Path
	e.maybeFetchHold(ctx)

	res, err := e.DefaultExtender.Fetch(ctx, agent, headRequest)
	if err != nil {
		if ue, ok := err.(*url.Error); ok && ue.Err == ErrEnqueueRedirect {
			e.rec.add(evFetch, host, path, method, e.sourceOf(ctx), ctx.State, "")
			e.rec.add(evFetchRedirect, host, path, method, e.sourceOf(ctx), ctx.State, ue.URL)
		}
		return res, err
	}
	// Buffer the streamed bytes for identity tracking, then hand production
	// code a fresh Reader that it reads and closes itself. The original
	// server body is fully consumed and closed here.
	b, rerr := ioutil.ReadAll(res.Body)
	if cerr := res.Body.Close(); cerr != nil && rerr == nil {
		rerr = cerr
	}
	if rerr != nil {
		return nil, rerr
	}
	res.Body = e.wrapBody(ctx, method, b)
	e.rec.add(evFetch, host, path, method, e.sourceOf(ctx), ctx.State, "")
	return res, nil
}

// servePlan turns a declared site into a real local HTTP handler.
func servePlan(site *protoSite) http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/robots.txt", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/plain")
		_, _ = w.Write([]byte(site.robotsTxt))
	})
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		if dst, ok := site.redirects[r.URL.Path]; ok {
			http.Redirect(w, r, dst, http.StatusTemporaryRedirect)
			return
		}
		page := site.pages[r.URL.Path]
		if page == nil {
			http.NotFound(w, r)
			return
		}
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.WriteHeader(page.status)
		if r.Method != http.MethodHead {
			_, _ = w.Write(page.body)
		}
	})
	return mux
}

// protoHandlerHolder defers handler wiring until the servers' own URLs are
// known and the plan has been rebased onto them.
type protoHandlerHolder struct {
	mu sync.RWMutex
	h  http.Handler
}

func (x *protoHandlerHolder) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	x.mu.RLock()
	h := x.h
	x.mu.RUnlock()
	h.ServeHTTP(w, r)
}

func (x *protoHandlerHolder) set(h http.Handler) {
	x.mu.Lock()
	x.h = h
	x.mu.Unlock()
}

// startPlanServers boots one httptest server per declared host with an empty
// holder, returning the host -> base URL mapping and the holders to wire once
// the plan has been rebased onto the real local hosts.
func startPlanServers(t *testing.T, plan *protoPlan) (map[string]string, map[string]*protoHandlerHolder, func()) {
	t.Helper()
	bases := map[string]string{}
	holders := map[string]*protoHandlerHolder{}
	servers := []*httptest.Server{}
	for _, site := range plan.sites {
		holder := &protoHandlerHolder{}
		srv := httptest.NewServer(holder)
		servers = append(servers, srv)
		bases[site.host] = srv.URL
		// Keyed by the real local host so it matches the rebased plan's sites.
		holders[parseMust(srv.URL).Host] = holder
	}
	return bases, holders, func() {
		for _, srv := range servers {
			srv.Close()
		}
	}
}

// wirePlanServers points each holder at the handler of its rebased site.
func wirePlanServers(holders map[string]*protoHandlerHolder, plan *protoPlan) {
	for _, site := range plan.sites {
		holders[site.host].set(servePlan(site))
	}
}

// rebasePlan rewrites declared hosts onto real local server URLs, keeping the
// same paths, relations and bodies.
func rebasePlan(plan *protoPlan, bases map[string]string) *protoPlan {
	hostMap := map[string]string{}
	for declared, base := range bases {
		hostMap[declared] = parseMust(base).Host
	}
	out := &protoPlan{
		name:          plan.name,
		headBeforeGet: plan.headBeforeGet,
		sameHostOnly:  plan.sameHostOnly,
		gateVisits:    plan.gateVisits,
	}
	for _, s := range plan.sites {
		realHost := hostMap[s.host]
		ns := &protoSite{
			host:      realHost,
			robotsTxt: s.robotsTxt,
			pages:     s.pages,
			redirects: map[string]string{},
		}
		for path, dst := range s.redirects {
			dstU := parseMust(dst)
			ns.redirects[path] = "http://" + hostMap[dstU.Host] + dstU.Path
		}
		out.sites = append(out.sites, ns)
		for _, page := range s.pages {
			for i, l := range page.links {
				if u, err := url.Parse(l); err == nil && u.IsAbs() && u.Host != "" {
					page.links[i] = "http://" + hostMap[u.Host] + u.Path
				}
			}
		}
	}
	for _, sd := range plan.seeds {
		u := parseMust(sd.rawURL)
		out.seeds = append(out.seeds, protoSeed{
			rawURL: "http://" + hostMap[u.Host] + u.Path,
			state:  sd.state,
		})
	}
	return out
}

func newCrawlerForPlan(plan *protoPlan, ext Extender) *Crawler {
	opts := NewOptions(ext)
	opts.CrawlDelay = 0
	opts.WorkerIdleTTL = 0
	opts.LogFlags = LogNone
	opts.HeadBeforeGet = plan.headBeforeGet
	opts.SameHostOnly = plan.sameHostOnly
	opts.RobotUserAgent = DefaultRobotUserAgent
	return NewCrawlerWithOptions(opts)
}

func seedValues(plan *protoPlan) interface{} {
	out := S{}
	for _, sd := range plan.seeds {
		out[sd.rawURL] = sd.state
	}
	return out
}

func realHost(bases map[string]string, declared string) string {
	return parseMust(bases[declared]).Host
}

// ===========================================================================
// Test 1: admission. Two sites with different robots rules, canonical
// duplicates, a HEAD veto and real GET visits, each kept distinct.
// ===========================================================================

func TestCrawlProtocolAdmission(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const (
			alpha = "alpha.protocol.test"
			beta  = "beta.protocol.test"
		)
		plan := &protoPlan{
			name:          "admission",
			headBeforeGet: true,
			sameHostOnly:  true,
			sites: []*protoSite{{
				host:      alpha,
				robotsTxt: "User-agent: *\nDisallow: /private\n",
				pages: map[string]*protoPage{
					"/home":        {path: "/home", status: 200, body: pageBody("home", "child/1"), links: []string{"child/1"}},
					"/child/1":     {path: "/child/1", status: 200, body: pageBody("child", "other")},
					"/head-no":     {path: "/head-no", status: 200, headDeny: true, body: pageBody("headno", "x")},
					"/private/box": {path: "/private/box", status: 200, body: pageBody("secret", "x")},
				},
			}, {
				host:      beta,
				robotsTxt: "User-agent: *\nDisallow: /\nAllow: /open\n",
				pages: map[string]*protoPage{
					"/open":   {path: "/open", status: 200, body: pageBody("open", "rel/")},
					"/locked": {path: "/locked", status: 200, body: pageBody("locked", "x")},
				},
			}},
			seeds: []protoSeed{
				// Two spellings of the same canonical URL.
				{rawURL: "HTTP://ALPHA.PROTOCOL.TEST:80/home", state: "admission-seed"},
				{rawURL: "http://alpha.protocol.test/home", state: "admission-seed"},
				{rawURL: "http://alpha.protocol.test/private/box", state: "admission-seed"},
				{rawURL: "http://alpha.protocol.test/head-no", state: "admission-seed"},
				{rawURL: "http://beta.protocol.test/open", state: "admission-seed"},
				{rawURL: "http://beta.protocol.test/locked", state: "admission-seed"},
			},
		}

		rec := newProtoRecorder()
		ext := newProtoExtender(plan, rec)
		cr := newCrawlerForPlan(plan, ext)
		c.Check(cr.Run(seedValues(plan)) == nil, "admission crawl returned an error")

		model := deriveProtoModel(plan, cr.Options.RobotUserAgent)
		checkModelEquality(c, rec, model)

		// Explicit category distinctions, not just one total count.
		c.Check(rec.count(evIs(evDisallowed, alpha, "/private/box", "")) == 1, "alpha /private/box must be robots-disallowed")
		c.Check(rec.count(evIs(evDisallowed, beta, "/locked", "")) == 1, "beta /locked must be robots-disallowed")
		c.Check(rec.count(evIs(evFetch, beta, "/locked", "GET")) == 0, "robots-denied /locked must never be fetched")
		c.Check(rec.count(evIs(evFetch, alpha, "/private/box", "GET")) == 0, "robots-denied /private/box must never be fetched")
		c.Check(rec.count(evIs(evRequestGet, alpha, "/head-no", "")) == 1, "HEAD veto page must reach RequestGet once")
		c.Check(rec.count(evIs(evFetch, alpha, "/head-no", "HEAD")) == 1, "HEAD veto page must have exactly one HEAD")
		c.Check(rec.count(evIs(evFetch, alpha, "/head-no", "GET")) == 0, "HEAD veto must suppress the GET")
		c.Check(rec.count(evIs(evVisit, alpha, "/head-no", "")) == 0, "HEAD-vetoed page must not count as a visited page")
		c.Check(rec.count(evIs(evVisit, alpha, "/home", "")) == 1, "/home must be visited exactly once despite the canonical duplicate")
		c.Check(rec.count(evIs(evEnqueued, alpha, "/home", "")) == 1, "canonical duplicate must enqueue /home exactly once")
		c.Check(rec.count(func(e protoEvent) bool {
			return e.kind == evFilter && e.host == alpha && e.path == "/home" && e.note == "isVisited=true"
		}) == 1, "the duplicate canonical seed must be filtered as already visited")
		c.Check(rec.count(evIs(evVisit, beta, "/open", "")) == 1, "beta /open must be visited under the Allow rule")
		c.Check(rec.count(evIs(evVisit, alpha, "/child/1", "")) == 1, "relative-link discovery /child/1 must be visited")
		c.Check(rec.count(func(e protoEvent) bool { return e.kind == evVisited && e.note == "HARVEST-MISMATCH" }) == 0,
			"Visited must receive the same harvested value Visit returned")

		// Per-host causal chains; nothing is assumed across hosts.
		checkHappensBefore(c, rec, "alpha robots before first page",
			evIs(evFetch, alpha, "/robots.txt", "GET"), evIs(evFetch, alpha, "/home", "HEAD"))
		checkHappensBefore(c, rec, "alpha HEAD before GET on /home",
			evIs(evFetch, alpha, "/home", "HEAD"), evIs(evFetch, alpha, "/home", "GET"))
		checkHappensBefore(c, rec, "alpha GET before Visit",
			evIs(evFetch, alpha, "/home", "GET"), evIs(evVisit, alpha, "/home", ""))
		checkHappensBefore(c, rec, "Visit before discovery enqueue",
			evIs(evVisited, alpha, "/home", ""), evIs(evEnqueued, alpha, "/child/1", ""))
		checkHappensBefore(c, rec, "beta robots before /open",
			evIs(evFetch, beta, "/robots.txt", "GET"), evIs(evFetch, beta, "/open", "HEAD"))

		c.Check(rec.count(evIs(evStart, "", "", "")) == 1, "exactly one Start callback")
		c.Check(rec.count(evIs(evEnd, "", "", "")) == 1, "exactly one End callback")
		c.Check(atomic.LoadInt64(&rec.inFlight) == 0, "all hooks must have returned after Run")
	})
}

// ===========================================================================
// Test 2: handoff. Real cross-site HTTP redirect through
// DefaultExtender.Fetch + httptest, State/source propagation, Visit findings
// handed identically to Visited, next-round enqueue, and channel barriers
// proving one host keeps crawling while the other is stuck in Visit.
// ===========================================================================

func TestCrawlProtocolHandoff(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const (
			hostA = "a.handoff.protocol.test"
			hostB = "b.handoff.protocol.test"
		)
		plan := &protoPlan{
			name:          "handoff",
			headBeforeGet: false,
			sameHostOnly:  false,
			sites: []*protoSite{{
				host:      hostA,
				robotsTxt: "User-agent: *\nAllow: /\n",
				pages: map[string]*protoPage{
					"/a1": {path: "/a1", status: 200, body: pageBody("a1", "a-rel/1"), visitHold: "hold-a1"},
				},
				redirects: map[string]string{
					"/jump": "http://" + hostB + "/land",
				},
			}, {
				host:      hostB,
				robotsTxt: "User-agent: *\nAllow: /\n",
				pages: map[string]*protoPage{
					"/land": {path: "/land", status: 200, body: pageBody("land", "brel"), links: []string{"brel"}, htmlLinks: true},
					"/brel": {path: "/brel", status: 200, body: pageBody("brel", "x")},
					// Relative discovery: no absolute host survives the rebase.
					// B's first content fetch is gated until A is confirmed stuck,
					// so the whole B chain demonstrably runs during A's block.
					"/b1": {path: "/b1", status: 200, body: pageBody("b1", "b2"), links: []string{"b2"}, fetchHold: "hold-b1"},
					"/b2": {path: "/b2", status: 200, body: pageBody("b2", "x")},
				},
			}},
			seeds: []protoSeed{
				{rawURL: "http://" + hostA + "/jump", state: "jump-state"},
				{rawURL: "http://" + hostA + "/a1", state: "a1-state"},
				{rawURL: "http://" + hostB + "/b1", state: "b1-state"},
			},
		}

		bases, holders, cleanup := startPlanServers(t, plan)
		defer cleanup()
		realPlan := rebasePlan(plan, bases)
		// Serve the rebased graph so the real 307 Location and links point at
		// the local servers rather than the reserved declared host names.
		wirePlanServers(holders, realPlan)

		rec := newProtoRecorder()
		inner := newProtoExtender(realPlan, rec)
		ext := &httpProtoExtender{protoExtender: inner}
		cr := newCrawlerForPlan(realPlan, ext)
		holdA := inner.gate("hold-a1")
		holdB := inner.gate("hold-b1")
		bHost := realHost(bases, hostB)

		// Barrier for B's independent discovery, registered before Run.
		b2Done := rec.expect(evIs(evVisited, bHost, "/b2", ""))

		done := make(chan error, 1)
		go func() { done <- cr.Run(seedValues(realPlan)) }()

		// A is stuck inside Visit(/a1), and B has parked at its first content
		// fetch. Neither host has completed its gated step yet.
		waitBarrier(t, rec, holdA.entered, "host A Visit(/a1) entry")
		waitBarrier(t, rec, holdB.entered, "host B Fetch(/b1) parked while A is blocked")

		c.Check(rec.firstIndex(evIs(evVisited, realHost(bases, hostA), "/a1", "")) < 0,
			"host A Visited(/a1) must not fire while the Visit hook is blocked")
		c.Check(rec.firstIndex(evIs(evFetch, bHost, "/b1", "GET")) < 0,
			"host B's gated content fetch must not have returned yet")

		// Release only B. Its entire /b1 -> /b2 discovery chain now runs while
		// A remains blocked in Visit, which proves real cross-host progress.
		close(holdB.release)
		waitBarrier(t, rec, b2Done, "host B Visited(/b2) while host A is blocked")

		aHost := realHost(bases, hostA)
		stuckVisit := rec.firstIndex(evIs(evVisit, aHost, "/a1", ""))
		b2Visited := rec.firstIndex(evIs(evVisited, bHost, "/b2", ""))
		c.Check(stuckVisit >= 0 && b2Visited >= 0 && stuckVisit < b2Visited,
			"host A Visit(/a1) must be in progress while host B reaches Visited(/b2)")
		c.Check(rec.firstIndex(evIs(evVisited, aHost, "/a1", "")) < 0,
			"host A Visited(/a1) must not fire while the Visit hook is blocked")

		// Release A; the crawl drains to completion.
		close(holdA.release)
		select {
		case err := <-done:
			c.Check(err == nil, "handoff crawl returned error %v", err)
		case <-time.After(protoWatchdog):
			t.Fatalf("Run did not return after releasing the barrier\n%s", rec.dump())
		}

		model := deriveProtoModel(realPlan, cr.Options.RobotUserAgent)
		checkModelEquality(c, rec, model)
		c.Check(rec.count(evIs(evFetchRedirect, aHost, "/jump", "GET")) == 1, "the cross-site redirect must be observed once on host A")
		c.Check(rec.count(func(e protoEvent) bool {
			return e.kind == evFetchRedirect && e.host == aHost && e.path == "/jump" && e.note == "http://"+bHost+"/land"
		}) == 1, "redirect destination identity mismatch on /jump")
		c.Check(rec.count(func(e protoEvent) bool {
			return e.kind == evEnqueued && e.host == bHost && e.path == "/land" &&
				e.source == "http://"+aHost+"/jump" && e.state == "jump-state"
		}) == 1, "redirect must carry source /jump and State jump-state to /land")
		c.Check(rec.count(evIs(evVisit, bHost, "/land", "")) == 1, "redirect landing page must be visited on host B")
		c.Check(rec.count(func(e protoEvent) bool {
			return e.kind == evEnqueued && e.host == bHost && e.path == "/brel" && e.source == "http://"+bHost+"/land"
		}) == 1, "relative discovery from the landing page must be enqueued with the landing page as source")
		c.Check(rec.count(evIs(evVisit, bHost, "/brel", "")) == 1, "the landing page's relative discovery must be visited")
		c.Check(rec.count(evIs(evEnqueued, bHost, "/b2", "")) == 1, "Visit's returned discovery must be enqueued for the next round")
		c.Check(rec.count(func(e protoEvent) bool { return e.kind == evVisited && e.note == "HARVEST-MISMATCH" }) == 0,
			"Visited received a different harvested value than Visit returned")

		land := plan.site(hostB).pages["/land"]
		c.Check(rec.count(func(e protoEvent) bool {
			return e.kind == evVisit && e.host == bHost && e.path == "/land" && e.note == hashBytes(land.body)
		}) == 1, "landing page body identity mismatch (relative link and binary bytes)")

		rec.mu.Lock()
		for id, body := range rec.bodies {
			c.Check(body.closeCount() == 1, "body %s closed %d time(s), want exactly 1", id, body.closeCount())
		}
		rec.mu.Unlock()
		c.Check(atomic.LoadInt64(&rec.inFlight) == 0, "all callbacks must have exited before Run returned")
		c.Check(rec.count(evIs(evEnd, "", "", "")) == 1, "End must fire exactly once")
	})
}

// ===========================================================================
// Test 3: two runs on one Crawler with replaced robots and page graphs, plus
// an independent MaxVisits scenario.
// ===========================================================================

func TestCrawlProtocolRuns(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const (
			g1 = "g1.runs.protocol.test"
			g2 = "g2.runs.protocol.test"
		)

		// Round 1: /root allowed, /deny1 disallowed, g2 redirects /go -> /land.
		round1 := &protoPlan{
			name:          "round-1",
			headBeforeGet: false,
			sameHostOnly:  true,
			sites: []*protoSite{{
				host:      g1,
				robotsTxt: "User-agent: *\nDisallow: /deny1\n",
				pages: map[string]*protoPage{
					"/root":   {path: "/root", status: 200, body: pageBody("r1-root", "down/1"), links: []string{"down/1"}},
					"/down/1": {path: "/down/1", status: 200, body: pageBody("r1-down", "x")},
					"/deny1":  {path: "/deny1", status: 200, body: pageBody("r1-deny", "x")},
				},
			}, {
				host:      g2,
				robotsTxt: "User-agent: *\nAllow: /\n",
				pages: map[string]*protoPage{
					"/land": {path: "/land", status: 200, body: pageBody("r1-land", "leaf"), links: []string{"leaf"}},
					"/leaf": {path: "/leaf", status: 200, body: pageBody("r1-leaf", "x")},
				},
				redirects: map[string]string{"/go": "http://" + g2 + "/land"},
			}},
			seeds: []protoSeed{
				{rawURL: "HTTP://G1.RUNS.PROTOCOL.TEST:80/root", state: "round-1-state"},
				{rawURL: "http://g1.runs.protocol.test/root", state: "round-1-state"},
				{rawURL: "http://g1.runs.protocol.test/deny1", state: "round-1-state"},
				{rawURL: "http://g2.runs.protocol.test/go", state: "round-1-state"},
			},
		}

		// Round 2 on the same Crawler: robots inverted on g1, g2 now denies
		// /land, a new redirect and new discoveries. Nothing is inherited.
		round2 := &protoPlan{
			name:          "round-2",
			headBeforeGet: false,
			sameHostOnly:  true,
			sites: []*protoSite{{
				host:      g1,
				robotsTxt: "User-agent: *\nDisallow: /root\nAllow: /deny1\n",
				pages: map[string]*protoPage{
					"/root":    {path: "/root", status: 200, body: pageBody("r2-root", "x")},
					"/deny1":   {path: "/deny1", status: 200, body: pageBody("r2-deny", "fresh/9"), links: []string{"fresh/9"}},
					"/fresh/9": {path: "/fresh/9", status: 200, body: pageBody("r2-fresh", "x")},
				},
			}, {
				host:      g2,
				robotsTxt: "User-agent: *\nDisallow: /land\nAllow: /jump2\nAllow: /dest2\n",
				pages: map[string]*protoPage{
					"/land":  {path: "/land", status: 200, body: pageBody("r2-land", "x")},
					"/dest2": {path: "/dest2", status: 200, body: pageBody("r2-dest", "x")},
				},
				redirects: map[string]string{"/jump2": "http://" + g2 + "/dest2"},
			}},
			seeds: []protoSeed{
				{rawURL: "http://g1.runs.protocol.test/root", state: "round-2-state"},
				{rawURL: "http://g1.runs.protocol.test/deny1", state: "round-2-state"},
				{rawURL: "http://g2.runs.protocol.test/land", state: "round-2-state"},
				{rawURL: "http://g2.runs.protocol.test/jump2", state: "round-2-state"},
			},
		}

		cr := newCrawlerForPlan(round1, nil)

		runRound := func(plan *protoPlan) *protoRecorder {
			rec := newProtoRecorder()
			ext := newProtoExtender(plan, rec)
			cr.Options.Extender = ext
			c.Check(cr.Run(seedValues(plan)) == nil, "%s crawl returned an error", plan.name)
			return rec
		}

		rec1 := runRound(round1)
		checkModelEquality(c, rec1, deriveProtoModel(round1, cr.Options.RobotUserAgent))
		c.Check(rec1.count(evIs(evDisallowed, g1, "/deny1", "")) == 1, "round 1 must disallow /deny1 per its robots text")
		c.Check(rec1.count(evIs(evVisit, g1, "/root", "")) == 1, "round 1 visits /root")
		c.Check(rec1.count(evIs(evVisit, g2, "/land", "")) == 1, "round 1 follows the redirect to /land")

		rec2 := runRound(round2)
		checkModelEquality(c, rec2, deriveProtoModel(round2, cr.Options.RobotUserAgent))

		// Independent recomputation: round 2 inverts round 1's decisions and
		// carries over neither its dedupe map nor its robots state.
		c.Check(rec2.count(evIs(evDisallowed, g1, "/root", "")) == 1, "round 2 must disallow /root under its new robots text")
		c.Check(rec2.count(evIs(evVisit, g1, "/root", "")) == 0, "round 2 /root is robots-denied, not visited")
		c.Check(rec2.count(evIs(evVisit, g1, "/deny1", "")) == 1, "round 2 admits /deny1 although round 1 denied it")
		c.Check(rec2.count(evIs(evDisallowed, g2, "/land", "")) == 1, "round 2 denies g2 /land although round 1 visited it")
		c.Check(rec2.count(evIs(evFetchRedirect, g2, "/jump2", "GET")) == 1, "round 2 redirect participates in the new graph")
		c.Check(rec2.count(evIs(evVisit, g2, "/dest2", "")) == 1, "round 2 redirect destination is visited")
		c.Check(rec2.count(evIs(evVisit, g1, "/fresh/9", "")) == 1, "round 2 discovery /fresh/9 is visited")
		c.Check(rec2.count(func(e protoEvent) bool { return e.state == "round-1-state" }) == 0,
			"no round 1 State token may survive into round 2 events")
		c.Check(rec2.count(func(e protoEvent) bool {
			return e.kind == evEnqueued && e.state != "round-2-state" && e.state != ""
		}) == 0, "every round 2 enqueue carries current-round State or none")

		// MaxVisits scenario. Each non-counting outcome gets its own host, so
		// there is no intra-worker ordering race: all three must occur while
		// every completed visit is held behind a gate.
		const (
			hDeny  = "deny.max.protocol.test"
			hHead  = "head.max.protocol.test"
			hRedir = "redir.max.protocol.test"
			hVisit = "visit.max.protocol.test"
		)
		maxPlan := &protoPlan{
			name:          "max-visits",
			headBeforeGet: true,
			sameHostOnly:  false,
			gateVisits:    "max-gate",
			sites: []*protoSite{{
				host:      hDeny,
				robotsTxt: "User-agent: *\nDisallow: /no\n",
				pages: map[string]*protoPage{
					"/no": {path: "/no", status: 200, body: pageBody("no", "x")},
				},
			}, {
				host:      hHead,
				robotsTxt: "User-agent: *\nAllow: /\n",
				pages: map[string]*protoPage{
					"/headx": {path: "/headx", status: 200, headDeny: true, body: pageBody("head", "x")},
				},
			}, {
				host:      hRedir,
				robotsTxt: "User-agent: *\nDisallow: /q1\n",
				pages: map[string]*protoPage{
					// /q1 is declared so the model's universe closure covers the
					// redirect destination, which robots then denies.
					"/q1": {path: "/q1", status: 200, body: pageBody("q1", "x")},
				},
				redirects: map[string]string{"/q0": "http://" + hRedir + "/q1"},
			}, {
				host:      hVisit,
				robotsTxt: "User-agent: *\nAllow: /\n",
				pages: map[string]*protoPage{
					"/v1": {path: "/v1", status: 200, body: pageBody("v1", "x")},
					"/v2": {path: "/v2", status: 200, body: pageBody("v2", "x")},
					"/v3": {path: "/v3", status: 200, body: pageBody("v3", "x")},
				},
			}},
			seeds: []protoSeed{
				{rawURL: "http://" + hDeny + "/no", state: "max"},
				{rawURL: "http://" + hHead + "/headx", state: "max"},
				{rawURL: "http://" + hRedir + "/q0", state: "max"},
				{rawURL: "http://" + hVisit + "/v1", state: "max"},
				{rawURL: "http://" + hVisit + "/v2", state: "max"},
				{rawURL: "http://" + hVisit + "/v3", state: "max"},
			},
		}

		maxRec := newProtoRecorder()
		maxExt := newProtoExtender(maxPlan, maxRec)
		maxCr := newCrawlerForPlan(maxPlan, maxExt)
		maxCr.Options.MaxVisits = 2
		maxGate := maxExt.gate("max-gate")

		runErr := make(chan error, 1)
		go func() { runErr <- maxCr.Run(seedValues(maxPlan)) }()

		// Every Visit funnels through the same gate, so once a visit has
		// entered it, none can have completed yet. Gate entry is independent
		// of the three protocol decisions exercised elsewhere, which keeps
		// this synchronization robust to any single degradation.
		waitBarrier(t, maxRec, maxGate.entered, "first gated Visit entry")
		c.Check(maxRec.count(func(e protoEvent) bool { return e.kind == evVisited }) == 0,
			"no page may complete a visit while visits are gated")

		// Let the gated visits proceed; the counter stops the crawl once
		// MaxVisits is reached. Already-queued same-worker pages may finish,
		// so the exact total is an implementation detail and is not asserted.
		close(maxGate.release)
		select {
		case err := <-runErr:
			c.Check(err == ErrMaxVisits, "MaxVisits run must return ErrMaxVisits, got %v", err)
		case <-time.After(protoWatchdog):
			t.Fatalf("MaxVisits run did not finish\n%s", maxRec.dump())
		}

		visits := maxRec.count(func(e protoEvent) bool { return e.kind == evVisit })
		visited := maxRec.count(func(e protoEvent) bool { return e.kind == evVisited })
		c.Check(visits >= 2, "at least MaxVisits(2) pages must be visited, got %d", visits)
		c.Check(visits <= 3, "the crawl must not escape the three queued visitable pages, got %d", visits)
		c.Check(visited == visits, "every Visit must be paired with one Visited callback")
		c.Check(maxRec.count(evIs(evEnd, "", "", "")) == 1, "the MaxVisits run still ends through End")

		// Closure invariant: nothing outside the declared universe is touched.
		declared := map[string]bool{}
		for _, s := range maxPlan.sites {
			declared[s.host+"|/robots.txt"] = true
			for p := range s.pages {
				declared[s.host+"|"+p] = true
			}
			for p := range s.redirects {
				declared[s.host+"|"+p] = true
			}
		}
		for _, ev := range maxRec.snapshot() {
			if ev.kind == evStart || ev.kind == evEnd || ev.host == "" {
				continue
			}
			c.Check(declared[ev.host+"|"+ev.path], "event %s on undeclared resource %s|%s escaped the model", ev.kind, ev.host, ev.path)
		}
		// The non-counting outcomes never produced a visit of their own.
		c.Check(maxRec.count(evIs(evVisit, hDeny, "/no", "")) == 0, "robots denial must not produce a visit")
		c.Check(maxRec.count(evIs(evVisit, hHead, "/headx", "")) == 0, "HEAD veto must not produce a visit")
		c.Check(maxRec.count(evIs(evVisit, hRedir, "/q0", "")) == 0, "redirect response must not produce a visit")
		maxRec.mu.Lock()
		for id, body := range maxRec.bodies {
			c.Check(body.closeCount() == 1, "body %s closed %d time(s), want exactly 1", id, body.closeCount())
		}
		maxRec.mu.Unlock()
		c.Check(atomic.LoadInt64(&maxRec.inFlight) == 0, "all MaxVisits callbacks must have exited")
	})
}
