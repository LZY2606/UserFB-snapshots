// Copyright 2014 Martin Angers and Contributors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package fetchbot

import (
	"io"
	"io/ioutil"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

// fakeClock is a controlled replacement for the package's time-wait entry
// point. Timers registered via after fire when advance moves the clock past
// their deadline; a timer registered once the clock is already past its
// deadline fires immediately, so tests never depend on the relative
// scheduling of the worker goroutines and the test goroutine.
type fakeClock struct {
	mu         sync.Mutex
	now        time.Duration
	pending    []*fakeTimer
	registered chan time.Duration
}

type fakeTimer struct {
	at time.Duration
	ch chan time.Time
}

func newFakeClock() *fakeClock {
	return &fakeClock{registered: make(chan time.Duration, 100)}
}

func (c *fakeClock) after(d time.Duration) <-chan time.Time {
	c.mu.Lock()
	defer c.mu.Unlock()
	t := &fakeTimer{at: c.now + d, ch: make(chan time.Time, 1)}
	if t.at <= c.now {
		t.ch <- time.Now()
	} else {
		c.pending = append(c.pending, t)
	}
	c.registered <- d
	return t.ch
}

func (c *fakeClock) advance(d time.Duration) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.now += d
	var keep []*fakeTimer
	for _, t := range c.pending {
		if t.at <= c.now {
			t.ch <- time.Now()
		} else {
			keep = append(keep, t)
		}
	}
	c.pending = keep
}

// agentCmd is a Command with a HeaderProvider that counts how many times it
// is evaluated.
type agentCmd struct {
	*Cmd
	header http.Header
	mu     sync.Mutex
	calls  int
}

func (c *agentCmd) Header() http.Header {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.calls++
	return c.header
}

func (c *agentCmd) headerCalls() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.calls
}

func mustParseURL(t *testing.T, raw string) *url.URL {
	t.Helper()
	u, err := url.Parse(raw)
	if err != nil {
		t.Fatal(err)
	}
	return u
}

// The same site admits one identity and rejects the other: the robots.txt
// group is selected by each command's effective User-Agent, and that same
// identity is the one sent on the wire.
func TestRobotsEffectiveAgent(t *testing.T) {
	const robots = "User-agent: BlockedBot\nDisallow: /\n\nUser-agent: AllowedBot\nDisallow:\n"

	type pageHit struct {
		path string
		ua   string
	}
	hits := make(chan pageHit, 10)
	var robotsHits int32
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/robots.txt" {
			atomic.AddInt32(&robotsHits, 1)
			io.WriteString(w, robots)
			return
		}
		hits <- pageHit{path: r.URL.Path, ua: r.Header.Get("User-Agent")}
		io.WriteString(w, "ok")
	}))
	defer srv.Close()

	type outcome struct {
		cmd Command
		err error
	}
	outcomes := make(chan outcome, 10)
	f := New(HandlerFunc(func(ctx *Context, res *http.Response, err error) {
		if res != nil && res.Body != nil {
			io.Copy(ioutil.Discard, res.Body)
		}
		outcomes <- outcome{cmd: ctx.Cmd, err: err}
	}))
	f.CrawlDelay = 0
	f.UserAgent = "AllowedBot"

	// Two commands on the same site with opposite identities.
	allowed := &agentCmd{
		Cmd:    &Cmd{U: mustParseURL(t, srv.URL+"/allowed"), M: "GET"},
		header: http.Header{"User-Agent": []string{"AllowedBot"}},
	}
	blocked := &agentCmd{
		Cmd:    &Cmd{U: mustParseURL(t, srv.URL+"/blocked"), M: "GET"},
		header: http.Header{"User-Agent": []string{"BlockedBot"}},
	}
	// A command without HeaderProvider uses the Fetcher's UserAgent.
	plain := &Cmd{U: mustParseURL(t, srv.URL+"/plain"), M: "GET"}

	q := f.Start()
	for _, c := range []Command{allowed, blocked, plain} {
		if err := q.Send(c); err != nil {
			t.Fatal(err)
		}
	}
	q.Close()
	close(outcomes)

	// Each command produced exactly one callback, on the original Command
	// object.
	seen := make(map[Command]error)
	for o := range outcomes {
		if _, dup := seen[o.cmd]; dup {
			t.Fatalf("duplicate handler callback for %s", o.cmd.URL())
		}
		seen[o.cmd] = o.err
	}
	if len(seen) != 3 {
		t.Fatalf("expected 3 handler callbacks, got %d", len(seen))
	}
	if err, ok := seen[allowed]; !ok || err != nil {
		t.Errorf("allowed identity: expected callback with no error, got err=%v present=%v", err, ok)
	}
	if err, ok := seen[blocked]; !ok || err != ErrDisallowed {
		t.Errorf("blocked identity: expected ErrDisallowed, got err=%v present=%v", err, ok)
	}
	if err, ok := seen[plain]; !ok || err != nil {
		t.Errorf("fetcher identity: expected callback with no error, got err=%v present=%v", err, ok)
	}

	// The HeaderProvider was evaluated exactly once per command: a single
	// preparation served both the admission check and the request.
	if n := allowed.headerCalls(); n != 1 {
		t.Errorf("expected 1 HeaderProvider evaluation for the allowed command, got %d", n)
	}
	if n := blocked.headerCalls(); n != 1 {
		t.Errorf("expected 1 HeaderProvider evaluation for the blocked command, got %d", n)
	}

	// Only the admitted pages were requested, in order, and the User-Agent
	// on the wire is the effective identity used for admission.
	var pages []pageHit
	for {
		select {
		case h := <-hits:
			pages = append(pages, h)
		default:
			goto drained
		}
	}
drained:
	want := []pageHit{{"/allowed", "AllowedBot"}, {"/plain", "AllowedBot"}}
	if len(pages) != len(want) {
		t.Fatalf("expected %d page requests, got %d: %v", len(want), len(pages), pages)
	}
	for i, w := range want {
		if pages[i] != w {
			t.Errorf("page request %d: expected %+v, got %+v", i, w, pages[i])
		}
	}

	// The robots.txt of the site was fetched exactly once.
	if n := atomic.LoadInt32(&robotsHits); n != 1 {
		t.Errorf("expected 1 robots.txt request, got %d", n)
	}

	// With DisablePoliteness, no robots.txt is fetched and no command is
	// rejected, whatever its identity.
	hits2 := make(chan pageHit, 10)
	var robotsHits2 int32
	srv2 := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/robots.txt" {
			atomic.AddInt32(&robotsHits2, 1)
			io.WriteString(w, robots)
			return
		}
		hits2 <- pageHit{path: r.URL.Path, ua: r.Header.Get("User-Agent")}
		io.WriteString(w, "ok")
	}))
	defer srv2.Close()

	var err2 error
	f2 := New(HandlerFunc(func(ctx *Context, res *http.Response, err error) {
		if res != nil && res.Body != nil {
			io.Copy(ioutil.Discard, res.Body)
		}
		err2 = err
	}))
	f2.CrawlDelay = 0
	f2.UserAgent = "AllowedBot"
	f2.DisablePoliteness = true
	denied := &agentCmd{
		Cmd:    &Cmd{U: mustParseURL(t, srv2.URL+"/page"), M: "GET"},
		header: http.Header{"User-Agent": []string{"BlockedBot"}},
	}
	q2 := f2.Start()
	if err := q2.Send(denied); err != nil {
		t.Fatal(err)
	}
	q2.Close()
	if err2 != nil {
		t.Errorf("DisablePoliteness: expected no error, got %v", err2)
	}
	select {
	case h := <-hits2:
		if h.path != "/page" || h.ua != "BlockedBot" {
			t.Errorf("DisablePoliteness: unexpected page request %+v", h)
		}
	default:
		t.Error("DisablePoliteness: expected the page request to be sent")
	}
	if n := atomic.LoadInt32(&robotsHits2); n != 0 {
		t.Errorf("DisablePoliteness: expected no robots.txt request, got %d", n)
	}
}

// prepCmd implements all the provider interfaces recognized by the Fetcher,
// counting the evaluations of each.
type prepCmd struct {
	*Cmd
	header       http.Header
	headerCalls  int
	readerCalls  int
	valuesCalls  int
	cookiesCalls int
	authCalls    int
}

func (c *prepCmd) Header() http.Header {
	c.headerCalls++
	return c.header
}

func (c *prepCmd) Reader() io.Reader {
	c.readerCalls++
	return strings.NewReader("reader-body")
}

func (c *prepCmd) Values() url.Values {
	c.valuesCalls++
	return url.Values{"form": []string{"values"}}
}

func (c *prepCmd) Cookies() []*http.Cookie {
	c.cookiesCalls++
	return []*http.Cookie{{Name: "session", Value: "cmd"}}
}

func (c *prepCmd) BasicAuth() (string, string) {
	c.authCalls++
	return "user", "pass"
}

// formCmd only provides form values.
type formCmd struct {
	*Cmd
	valuesCalls int
}

func (c *formCmd) Values() url.Values {
	c.valuesCalls++
	return url.Values{"k": []string{"v"}}
}

// Request preparation: the HeaderProvider is evaluated once per command, the
// command's own header map is never written back to, a rejected command
// never reaches the other providers, and the admitted providers keep their
// documented priorities.
func TestRobotsPreparation(t *testing.T) {
	const robots = "User-agent: PrepBot\nDisallow:\n\nUser-agent: DeniedBot\nDisallow: /\n"

	type pageRecord struct {
		path        string
		ua          string
		authUser    string
		authPwd     string
		cookie      string
		xCustom     []string
		contentType string
		body        string
	}
	records := make(chan pageRecord, 10)
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/robots.txt" {
			io.WriteString(w, robots)
			return
		}
		body, _ := ioutil.ReadAll(r.Body)
		user, pwd, _ := r.BasicAuth()
		rec := pageRecord{
			path:        r.URL.Path,
			ua:          r.Header.Get("User-Agent"),
			authUser:    user,
			authPwd:     pwd,
			cookie:      r.Header.Get("Cookie"),
			contentType: r.Header.Get("Content-Type"),
			body:        string(body),
		}
		if v := r.Header["X-Custom"]; v != nil {
			rec.xCustom = append([]string(nil), v...)
		}
		records <- rec
		io.WriteString(w, "ok")
	}))
	defer srv.Close()

	type outcome struct {
		path string
		cmd  Command
		err  error
	}
	outcomes := make(chan outcome, 10)
	f := New(HandlerFunc(func(ctx *Context, res *http.Response, err error) {
		if res != nil && res.Body != nil {
			io.Copy(ioutil.Discard, res.Body)
		}
		outcomes <- outcome{path: ctx.Cmd.URL().Path, cmd: ctx.Cmd, err: err}
	}))
	f.CrawlDelay = 0
	f.UserAgent = "PrepBot"

	// A command admitted under the default identity, exercising every
	// provider at once.
	full := &prepCmd{
		Cmd: &Cmd{U: mustParseURL(t, srv.URL+"/full"), M: "POST"},
		header: http.Header{
			"X-Custom":      []string{"a", "b"},
			"Cookie":        []string{"theme=dark"},
			"Authorization": []string{"Bearer stale"},
		},
	}
	// A command rejected by its own identity: none of the body, cookie or
	// auth providers may be evaluated.
	denied := &prepCmd{
		Cmd:    &Cmd{U: mustParseURL(t, srv.URL+"/denied"), M: "POST"},
		header: http.Header{"User-Agent": []string{"DeniedBot"}},
	}
	// A form-only command.
	form := &formCmd{Cmd: &Cmd{U: mustParseURL(t, srv.URL+"/form"), M: "POST"}}

	// A command with its own handler still receives the original Command
	// object.
	handlerSeen := make(chan Command, 1)
	hc, err := NewHandlerCmd("GET", srv.URL+"/handler", func(ctx *Context, res *http.Response, err error) {
		if res != nil && res.Body != nil {
			io.Copy(ioutil.Discard, res.Body)
		}
		handlerSeen <- ctx.Cmd
	})
	if err != nil {
		t.Fatal(err)
	}

	q := f.Start()
	for _, c := range []Command{full, denied, form, hc} {
		if err := q.Send(c); err != nil {
			t.Fatal(err)
		}
	}
	q.Close()
	close(outcomes)

	// full, denied and form went to the Fetcher's handler, hc to its own.
	seen := make(map[string]outcome)
	for o := range outcomes {
		seen[o.path] = o
	}
	if len(seen) != 3 {
		t.Errorf("expected 3 fetcher-handler callbacks, got %d", len(seen))
	}
	if o, ok := seen["/full"]; !ok || o.err != nil || o.cmd != full {
		t.Errorf("full command: unexpected outcome %+v (present=%v)", o, ok)
	}
	if o, ok := seen["/denied"]; !ok || o.err != ErrDisallowed || o.cmd != denied {
		t.Errorf("denied command: expected exactly one ErrDisallowed callback, got %+v (present=%v)", o, ok)
	}
	if o, ok := seen["/form"]; !ok || o.err != nil || o.cmd != form {
		t.Errorf("form command: unexpected outcome %+v (present=%v)", o, ok)
	}
	select {
	case c := <-handlerSeen:
		if c != hc {
			t.Errorf("handler command: expected the original HandlerCmd, got %T", c)
		}
	default:
		t.Error("handler command: own handler was not called")
	}

	// The requests that actually went out.
	recs := make(map[string]pageRecord)
	for {
		select {
		case r := <-records:
			recs[r.path] = r
		default:
			goto drained
		}
	}
drained:
	if _, ok := recs["/denied"]; ok {
		t.Error("denied command: no request should have been sent")
	}

	fullRec, ok := recs["/full"]
	if !ok {
		t.Fatal("full command: no request received")
	}
	// The User-Agent on the wire is the one used for admission.
	if fullRec.ua != "PrepBot" {
		t.Errorf("expected User-Agent %q, got %q", "PrepBot", fullRec.ua)
	}
	// BasicAuth overrides the Authorization header of the HeaderProvider.
	if fullRec.authUser != "user" || fullRec.authPwd != "pass" {
		t.Errorf("expected basic auth user/pass, got %q/%q", fullRec.authUser, fullRec.authPwd)
	}
	// Cookies from the HeaderProvider and the CookiesProvider are merged.
	if !strings.Contains(fullRec.cookie, "theme=dark") || !strings.Contains(fullRec.cookie, "session=cmd") {
		t.Errorf("expected merged cookies, got %q", fullRec.cookie)
	}
	// Multi-value headers survive.
	if len(fullRec.xCustom) != 2 || fullRec.xCustom[0] != "a" || fullRec.xCustom[1] != "b" {
		t.Errorf("expected X-Custom [a b], got %v", fullRec.xCustom)
	}
	// ReaderProvider wins over ValuesProvider, so no form Content-Type and
	// no ValuesProvider evaluation.
	if fullRec.body != "reader-body" {
		t.Errorf("expected reader body, got %q", fullRec.body)
	}
	if fullRec.contentType != "" {
		t.Errorf("expected no Content-Type with a Reader body, got %q", fullRec.contentType)
	}
	if full.headerCalls != 1 || full.readerCalls != 1 || full.valuesCalls != 0 ||
		full.cookiesCalls != 1 || full.authCalls != 1 {
		t.Errorf("provider evaluations: header=%d reader=%d values=%d cookies=%d auth=%d, want 1/1/0/1/1",
			full.headerCalls, full.readerCalls, full.valuesCalls, full.cookiesCalls, full.authCalls)
	}

	// The command's own header map and slices were never written back to.
	if ua := full.header.Get("User-Agent"); ua != "" {
		t.Errorf("command header map gained a User-Agent: %q", ua)
	}
	if ct := full.header.Get("Content-Type"); ct != "" {
		t.Errorf("command header map gained a Content-Type: %q", ct)
	}
	if got := full.header["Cookie"]; len(got) != 1 || got[0] != "theme=dark" {
		t.Errorf("command Cookie slice was modified: %v", got)
	}
	if got := full.header["X-Custom"]; len(got) != 2 || got[0] != "a" || got[1] != "b" {
		t.Errorf("command X-Custom slice was modified: %v", got)
	}
	if got := full.header.Get("Authorization"); got != "Bearer stale" {
		t.Errorf("command Authorization header was modified: %q", got)
	}

	// The rejected command was evaluated for its identity only.
	if denied.headerCalls != 1 {
		t.Errorf("denied command: expected 1 HeaderProvider evaluation, got %d", denied.headerCalls)
	}
	if denied.readerCalls != 0 || denied.valuesCalls != 0 || denied.cookiesCalls != 0 || denied.authCalls != 0 {
		t.Errorf("denied command: unexpected provider evaluations: reader=%d values=%d cookies=%d auth=%d",
			denied.readerCalls, denied.valuesCalls, denied.cookiesCalls, denied.authCalls)
	}

	// The form-only command gets the form body and the default Content-Type.
	formRec, ok := recs["/form"]
	if !ok {
		t.Fatal("form command: no request received")
	}
	if formRec.body != "k=v" {
		t.Errorf("expected form body %q, got %q", "k=v", formRec.body)
	}
	if formRec.contentType != "application/x-www-form-urlencoded" {
		t.Errorf("expected form Content-Type, got %q", formRec.contentType)
	}
	if form.valuesCalls != 1 {
		t.Errorf("expected 1 ValuesProvider evaluation, got %d", form.valuesCalls)
	}
}

// Politeness delays come from the group selected by each command's effective
// identity, tracked per host: a faster identity never skips the wait left by
// a slower one, a rejected command leaves a pending wait untouched, and
// other hosts proceed in parallel. Driven by a controlled clock, no real
// sleeping is involved.
func TestRobotsGroupDelay(t *testing.T) {
	clock := newFakeClock()
	origAfter := after
	after = clock.after
	defer func() { after = origAfter }()

	type pageHit struct {
		path string
		ua   string
	}
	hits := make(chan pageHit, 10)
	var robots1, robots2 int32

	// Site 1: a slow group with a crawl delay and a disallowed path, and a
	// fast group without a delay (falls back to the Fetcher's CrawlDelay).
	srv1 := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/robots.txt" {
			atomic.AddInt32(&robots1, 1)
			io.WriteString(w, "User-agent: SlowBot\nCrawl-delay: 10\nDisallow: /denied\n\nUser-agent: FastBot\nDisallow:\n")
			return
		}
		hits <- pageHit{path: "site1" + r.URL.Path, ua: r.Header.Get("User-Agent")}
		io.WriteString(w, "ok")
	}))
	defer srv1.Close()

	// Site 2: no robots.txt, everything allowed with the default delay.
	srv2 := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/robots.txt" {
			atomic.AddInt32(&robots2, 1)
			w.WriteHeader(http.StatusNotFound)
			return
		}
		hits <- pageHit{path: "site2" + r.URL.Path, ua: r.Header.Get("User-Agent")}
		io.WriteString(w, "ok")
	}))
	defer srv2.Close()

	type outcome struct {
		cmd Command
		err error
	}
	outcomes := make(chan outcome, 10)
	f := New(HandlerFunc(func(ctx *Context, res *http.Response, err error) {
		if res != nil && res.Body != nil {
			io.Copy(ioutil.Discard, res.Body)
		}
		outcomes <- outcome{cmd: ctx.Cmd, err: err}
	}))
	f.UserAgent = "FastBot"
	f.CrawlDelay = 3 * time.Second
	f.WorkerIdleTTL = time.Hour

	slow := &agentCmd{
		Cmd:    &Cmd{U: mustParseURL(t, srv1.URL+"/slow"), M: "GET"},
		header: http.Header{"User-Agent": []string{"SlowBot"}},
	}
	denied := &agentCmd{
		Cmd:    &Cmd{U: mustParseURL(t, srv1.URL+"/denied"), M: "GET"},
		header: http.Header{"User-Agent": []string{"SlowBot"}},
	}
	fast1 := &agentCmd{
		Cmd:    &Cmd{U: mustParseURL(t, srv1.URL+"/fast1"), M: "GET"},
		header: http.Header{"User-Agent": []string{"FastBot"}},
	}
	fast2 := &agentCmd{
		Cmd:    &Cmd{U: mustParseURL(t, srv1.URL+"/fast2"), M: "GET"},
		header: http.Header{"User-Agent": []string{"FastBot"}},
	}
	other := &Cmd{U: mustParseURL(t, srv2.URL+"/other"), M: "GET"}

	recvOutcome := func() outcome {
		t.Helper()
		select {
		case o := <-outcomes:
			return o
		case <-time.After(5 * time.Second):
			t.Fatal("timed out waiting for a handler callback")
			return outcome{}
		}
	}
	recvHit := func() pageHit {
		t.Helper()
		select {
		case h := <-hits:
			return h
		case <-time.After(5 * time.Second):
			t.Fatal("timed out waiting for a page request")
			return pageHit{}
		}
	}
	expectNoHit := func() {
		t.Helper()
		select {
		case h := <-hits:
			t.Fatalf("unexpected page request before the wait elapsed: %+v", h)
		default:
		}
	}
	// waitTimer blocks until a wait of the given duration has been scheduled,
	// ignoring the worker idle TTL timers.
	waitTimer := func(want time.Duration) {
		t.Helper()
		for {
			select {
			case d := <-clock.registered:
				if d == want {
					return
				}
			case <-time.After(5 * time.Second):
				t.Fatalf("timed out waiting for a %s timer", want)
			}
		}
	}

	q := f.Start()

	// The first page of a site waits the crawl delay of its own group after
	// the robots.txt response (SlowBot: 10s).
	if err := q.Send(slow); err != nil {
		t.Fatal(err)
	}
	waitTimer(10 * time.Second)
	expectNoHit()
	clock.advance(10 * time.Second) // t=10
	if h := recvHit(); h.path != "site1/slow" || h.ua != "SlowBot" {
		t.Fatalf("expected site1/slow as SlowBot, got %+v", h)
	}
	if o := recvOutcome(); o.cmd != slow || o.err != nil {
		t.Fatalf("slow command: unexpected outcome %+v", o)
	}
	// The successful slow command leaves a 10s wait behind.
	waitTimer(10 * time.Second)

	// A disallowed command is rejected without consuming the pending wait.
	if err := q.Send(denied); err != nil {
		t.Fatal(err)
	}
	if o := recvOutcome(); o.cmd != denied || o.err != ErrDisallowed {
		t.Fatalf("denied command: expected ErrDisallowed, got %+v", o)
	}

	// Another site proceeds in parallel, with the default crawl delay.
	if err := q.Send(other); err != nil {
		t.Fatal(err)
	}
	waitTimer(3 * time.Second)
	expectNoHit()
	clock.advance(3 * time.Second) // t=13
	if h := recvHit(); h.path != "site2/other" || h.ua != "FastBot" {
		t.Fatalf("expected site2/other as FastBot, got %+v", h)
	}
	if o := recvOutcome(); o.cmd != other || o.err != nil {
		t.Fatalf("other command: unexpected outcome %+v", o)
	}
	waitTimer(3 * time.Second)

	// The fast identity cannot skip the wait left by the slow group, which
	// still has 7s to go.
	if err := q.Send(fast1); err != nil {
		t.Fatal(err)
	}
	clock.advance(5 * time.Second) // t=18
	expectNoHit()
	clock.advance(2 * time.Second) // t=20, the slow wait elapses
	if h := recvHit(); h.path != "site1/fast1" || h.ua != "FastBot" {
		t.Fatalf("expected site1/fast1 as FastBot, got %+v", h)
	}
	if o := recvOutcome(); o.cmd != fast1 || o.err != nil {
		t.Fatalf("fast1 command: unexpected outcome %+v", o)
	}
	// The FastBot group has no crawl delay: the default 3s applies.
	waitTimer(3 * time.Second)

	if err := q.Send(fast2); err != nil {
		t.Fatal(err)
	}
	clock.advance(2 * time.Second) // t=22
	expectNoHit()
	clock.advance(1 * time.Second) // t=23
	if h := recvHit(); h.path != "site1/fast2" || h.ua != "FastBot" {
		t.Fatalf("expected site1/fast2 as FastBot, got %+v", h)
	}
	if o := recvOutcome(); o.cmd != fast2 || o.err != nil {
		t.Fatalf("fast2 command: unexpected outcome %+v", o)
	}

	q.Close()

	// Each site's robots.txt was fetched exactly once, and nothing else was
	// requested.
	if n := atomic.LoadInt32(&robots1); n != 1 {
		t.Errorf("expected 1 robots.txt request on site1, got %d", n)
	}
	if n := atomic.LoadInt32(&robots2); n != 1 {
		t.Errorf("expected 1 robots.txt request on site2, got %d", n)
	}
	expectNoHit()
}
