package colly

import (
	"errors"
	"io"
	"net/http"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

func robotsDiscoveryResponse(r *http.Request, body string) *http.Response {
	return &http.Response{
		StatusCode: 200,
		Header:     make(http.Header),
		Body:       io.NopCloser(strings.NewReader(body)),
		Request:    r,
	}
}

// newRobotsDiscoveryCollector returns a collector with robots.txt
// enforcement enabled and a controlled in-process transport. The
// returned counter counts robots.txt fetches; onRobots, if non-nil,
// is invoked inside every robots.txt request.
func newRobotsDiscoveryCollector(robotsBody string, onRobots func()) (*Collector, *atomic.Int32) {
	var robotsCalls atomic.Int32
	c := NewCollector()
	c.IgnoreRobotsTxt = false
	c.WithTransport(stubTransport(func(r *http.Request) (*http.Response, error) {
		if r.URL.Path == "/robots.txt" {
			robotsCalls.Add(1)
			if onRobots != nil {
				onRobots()
			}
			return robotsDiscoveryResponse(r, robotsBody), nil
		}
		return robotsDiscoveryResponse(r, "ok"), nil
	}))
	return c, &robotsCalls
}

// A Collector and its Clone join one in-flight discovery for the same
// origin and User-Agent, then reuse the parsed rules from the cache.
func TestRobotsDiscoverySharedWithClone(t *testing.T) {
	var robotsCalls, pageCalls atomic.Int32
	entered := make(chan struct{}, 1)
	release := make(chan struct{})
	c := NewCollector()
	c.IgnoreRobotsTxt = false
	c.WithTransport(stubTransport(func(r *http.Request) (*http.Response, error) {
		if r.URL.Path == "/robots.txt" {
			robotsCalls.Add(1)
			entered <- struct{}{}
			<-release
			return robotsDiscoveryResponse(r, "User-agent: *\nDisallow: /blocked\n"), nil
		}
		pageCalls.Add(1)
		return robotsDiscoveryResponse(r, "ok"), nil
	}))
	clone := c.Clone()

	results := make(chan error, 2)
	go func() { results <- c.Visit("http://sample.invalid/a") }()
	go func() { results <- clone.Visit("http://sample.invalid/b") }()

	select {
	case <-entered:
	case <-time.After(2 * time.Second):
		t.Fatal("robots fetch did not start")
	}
	// Give the second caller time to join the in-flight discovery
	// instead of starting its own fetch.
	time.Sleep(100 * time.Millisecond)
	close(release)

	for i := 0; i < 2; i++ {
		if err := <-results; err != nil {
			t.Fatal(err)
		}
	}
	if got := pageCalls.Load(); got != 2 {
		t.Fatalf("expected both page requests, got %d", got)
	}
	if got := robotsCalls.Load(); got != 1 {
		t.Fatalf("expected one shared robots discovery, got %d fetches", got)
	}
	// The parsed rules are cached and enforced by both collectors
	// without any further fetch.
	if err := clone.Visit("http://sample.invalid/blocked"); err != ErrRobotsTxtBlocked {
		t.Fatalf("expected ErrRobotsTxtBlocked, got %v", err)
	}
	if err := c.Visit("http://sample.invalid/blocked"); err != ErrRobotsTxtBlocked {
		t.Fatalf("expected ErrRobotsTxtBlocked, got %v", err)
	}
	if got := robotsCalls.Load(); got != 1 {
		t.Fatalf("cache miss after discovery: %d robots fetches", got)
	}
}

// Discoveries of different origins run in parallel: a slow site must
// not block the first visit of another origin.
func TestRobotsDiscoveryOriginsParallel(t *testing.T) {
	entered := make(chan string, 2)
	release := make(chan struct{})
	c := NewCollector()
	c.IgnoreRobotsTxt = false
	c.WithTransport(stubTransport(func(r *http.Request) (*http.Response, error) {
		if r.URL.Path == "/robots.txt" {
			entered <- r.URL.Host
			<-release
			return robotsDiscoveryResponse(r, "User-agent: *\nAllow: /\n"), nil
		}
		return robotsDiscoveryResponse(r, "ok"), nil
	}))

	results := make(chan error, 2)
	go func() { results <- c.Visit("http://slow-a.invalid/") }()
	go func() { results <- c.Visit("http://slow-b.invalid/") }()

	seen := map[string]bool{}
	for i := 0; i < 2; i++ {
		select {
		case host := <-entered:
			seen[host] = true
		case <-time.After(2 * time.Second):
			t.Fatal("discoveries for distinct origins did not run in parallel")
		}
	}
	close(release)
	for i := 0; i < 2; i++ {
		if err := <-results; err != nil {
			t.Fatal(err)
		}
	}
	if !seen["slow-a.invalid"] || !seen["slow-b.invalid"] {
		t.Fatalf("expected both origins discovered, got %v", seen)
	}
}

// A failed discovery reports its error to every caller that joined the
// round, lets no page request bypass robots, leaves no waiter behind,
// and is not cached so a later visit retries.
func TestRobotsDiscoveryFailureRetries(t *testing.T) {
	var robotsCalls, pageCalls atomic.Int32
	var fail atomic.Bool
	fail.Store(true)
	entered := make(chan struct{}, 1)
	release := make(chan struct{})
	errDiscovery := errors.New("robots fetch failed")
	c := NewCollector()
	c.IgnoreRobotsTxt = false
	c.WithTransport(stubTransport(func(r *http.Request) (*http.Response, error) {
		if r.URL.Path == "/robots.txt" {
			robotsCalls.Add(1)
			if fail.Load() {
				entered <- struct{}{}
				<-release
				return nil, errDiscovery
			}
			return robotsDiscoveryResponse(r, "User-agent: *\nAllow: /\n"), nil
		}
		pageCalls.Add(1)
		return robotsDiscoveryResponse(r, "ok"), nil
	}))

	results := make(chan error, 2)
	go func() { results <- c.Visit("http://flaky.invalid/a") }()
	go func() { results <- c.Visit("http://flaky.invalid/b") }()

	select {
	case <-entered:
	case <-time.After(2 * time.Second):
		t.Fatal("robots fetch did not start")
	}
	// Let the second caller join this discovery round before it fails.
	time.Sleep(100 * time.Millisecond)
	close(release)

	for i := 0; i < 2; i++ {
		select {
		case err := <-results:
			if !errors.Is(err, errDiscovery) {
				t.Fatalf("expected discovery error, got %v", err)
			}
		case <-time.After(2 * time.Second):
			t.Fatal("a caller was left waiting after a failed discovery")
		}
	}
	if got := pageCalls.Load(); got != 0 {
		t.Fatalf("page requests bypassed robots after failed discovery: %d", got)
	}

	// The failure was not cached: the next visit starts a new discovery.
	fail.Store(false)
	if err := c.Visit("http://flaky.invalid/a"); err != nil {
		t.Fatal(err)
	}
	if got := robotsCalls.Load(); got != 2 {
		t.Fatalf("expected a retry after the failed discovery, got %d robots fetches", got)
	}
	if got := pageCalls.Load(); got != 1 {
		t.Fatalf("expected the retried page request, got %d", got)
	}
}

// Discovery identity is the origin (lowercase scheme, lowercase
// hostname, effective port) plus the User-Agent sent on the robots
// request.
func TestRobotsDiscoveryKeySeparation(t *testing.T) {
	allowAll := "User-agent: *\nAllow: /\n"

	t.Run("DefaultPortEquivalence", func(t *testing.T) {
		c, calls := newRobotsDiscoveryCollector(allowAll, nil)
		for _, u := range []string{
			"http://port.invalid/a",
			"http://port.invalid:80/b",
			"http://PORT.invalid:80/c",
		} {
			if err := c.Visit(u); err != nil {
				t.Fatal(err)
			}
		}
		if got := calls.Load(); got != 1 {
			t.Fatalf("omitted and explicit default port must share one discovery, got %d fetches", got)
		}
	})

	t.Run("SchemeSeparation", func(t *testing.T) {
		c, calls := newRobotsDiscoveryCollector(allowAll, nil)
		if err := c.Visit("http://scheme.invalid/a"); err != nil {
			t.Fatal(err)
		}
		if err := c.Visit("https://scheme.invalid/a"); err != nil {
			t.Fatal(err)
		}
		if got := calls.Load(); got != 2 {
			t.Fatalf("http and https must never share a discovery, got %d fetches", got)
		}
	})

	t.Run("PortSeparation", func(t *testing.T) {
		c, calls := newRobotsDiscoveryCollector(allowAll, nil)
		if err := c.Visit("http://port.invalid/a"); err != nil {
			t.Fatal(err)
		}
		if err := c.Visit("http://port.invalid:8080/a"); err != nil {
			t.Fatal(err)
		}
		if got := calls.Load(); got != 2 {
			t.Fatalf("different ports must not share a discovery, got %d fetches", got)
		}
	})

	t.Run("IPv6", func(t *testing.T) {
		c, calls := newRobotsDiscoveryCollector(allowAll, nil)
		if err := c.Visit("http://[::1]/a"); err != nil {
			t.Fatal(err)
		}
		if err := c.Visit("http://[::1]:80/b"); err != nil {
			t.Fatal(err)
		}
		if got := calls.Load(); got != 1 {
			t.Fatalf("IPv6 host with default port must share one discovery, got %d fetches", got)
		}
		if err := c.Visit("http://[::1]:8080/c"); err != nil {
			t.Fatal(err)
		}
		if got := calls.Load(); got != 2 {
			t.Fatalf("IPv6 host with another port needs its own discovery, got %d fetches", got)
		}
	})

	t.Run("UserAgentSeparation", func(t *testing.T) {
		c, calls := newRobotsDiscoveryCollector(allowAll, nil)
		c.UserAgent = "agent-one"
		clone := c.Clone()
		clone.UserAgent = "agent-two"
		if err := c.Visit("http://ua.invalid/a"); err != nil {
			t.Fatal(err)
		}
		if err := clone.Visit("http://ua.invalid/b"); err != nil {
			t.Fatal(err)
		}
		if got := calls.Load(); got != 2 {
			t.Fatalf("different User-Agent identities must not share a discovery, got %d fetches", got)
		}
	})

	t.Run("IdentityChange", func(t *testing.T) {
		c, calls := newRobotsDiscoveryCollector(allowAll, nil)
		c.UserAgent = "agent-one"
		if err := c.Visit("http://id.invalid/a"); err != nil {
			t.Fatal(err)
		}
		// Changing the identity afterwards is looked up under the new
		// identity without rewriting the old identity's entry.
		c.UserAgent = "agent-two"
		if err := c.Visit("http://id.invalid/b"); err != nil {
			t.Fatal(err)
		}
		if got := calls.Load(); got != 2 {
			t.Fatalf("new identity must start a new discovery, got %d fetches", got)
		}
		c.UserAgent = "agent-one"
		if err := c.Visit("http://id.invalid/c"); err != nil {
			t.Fatal(err)
		}
		if got := calls.Load(); got != 2 {
			t.Fatalf("old identity entry must be preserved, got %d fetches", got)
		}
	})
}

// Collectors sharing one discovered RobotsData still select their rule
// group with their own UserAgent and may get different outcomes.
func TestRobotsDiscoveryGroupSelectionPerCollector(t *testing.T) {
	var robotsCalls atomic.Int32
	c := NewCollector()
	c.IgnoreRobotsTxt = false
	c.UserAgent = "agent-a"
	// Both collectors send the same User-Agent on the robots.txt
	// request, so they share a single discovery.
	c.Headers = &http.Header{"User-Agent": {"shared-discovery-agent"}}
	c.WithTransport(stubTransport(func(r *http.Request) (*http.Response, error) {
		if r.URL.Path == "/robots.txt" {
			robotsCalls.Add(1)
			if got := r.Header.Get("User-Agent"); got != "shared-discovery-agent" {
				t.Errorf("robots request User-Agent = %q, want shared-discovery-agent", got)
			}
			return robotsDiscoveryResponse(r, "User-agent: agent-a\nDisallow: /blocked\n\nUser-agent: agent-b\nAllow: /\n"), nil
		}
		return robotsDiscoveryResponse(r, "ok"), nil
	}))
	clone := c.Clone()
	clone.UserAgent = "agent-b"

	if err := c.Visit("http://groups.invalid/blocked"); err != ErrRobotsTxtBlocked {
		t.Fatalf("agent-a should be blocked, got %v", err)
	}
	if err := clone.Visit("http://groups.invalid/blocked"); err != nil {
		t.Fatalf("agent-b should be allowed, got %v", err)
	}
	if got := robotsCalls.Load(); got != 1 {
		t.Fatalf("expected one shared discovery, got %d fetches", got)
	}
}

// Request.Visit from page callbacks and direct Collector.Request calls
// go through the same robots rules as Visit.
func TestRobotsDiscoveryCallbackVisit(t *testing.T) {
	var robotsCalls atomic.Int32
	c := NewCollector()
	c.IgnoreRobotsTxt = false
	c.WithTransport(stubTransport(func(r *http.Request) (*http.Response, error) {
		if r.URL.Path == "/robots.txt" {
			robotsCalls.Add(1)
			return robotsDiscoveryResponse(r, "User-agent: *\nDisallow: /blocked\n"), nil
		}
		return robotsDiscoveryResponse(r, "ok"), nil
	}))

	callbackErr := make(chan error, 1)
	c.OnResponse(func(r *Response) {
		callbackErr <- r.Request.Visit("http://paths.invalid/blocked")
	})
	if err := c.Visit("http://paths.invalid/start"); err != nil {
		t.Fatal(err)
	}
	if err := <-callbackErr; err != ErrRobotsTxtBlocked {
		t.Fatalf("callback Request.Visit should be blocked, got %v", err)
	}
	if err := c.Request("POST", "http://paths.invalid/blocked", nil, nil, nil); err != ErrRobotsTxtBlocked {
		t.Fatalf("POST request should be blocked, got %v", err)
	}
	if got := robotsCalls.Load(); got != 1 {
		t.Fatalf("expected one robots fetch, got %d", got)
	}
}

// HEAD requests keep skipping robots.txt, and IgnoreRobotsTxt keeps
// disabling discovery entirely.
func TestRobotsDiscoverySkipped(t *testing.T) {
	t.Run("Head", func(t *testing.T) {
		var robotsCalls, headCalls atomic.Int32
		c := NewCollector()
		c.IgnoreRobotsTxt = false
		c.WithTransport(stubTransport(func(r *http.Request) (*http.Response, error) {
			if r.URL.Path == "/robots.txt" {
				robotsCalls.Add(1)
			} else {
				headCalls.Add(1)
			}
			return robotsDiscoveryResponse(r, "ok"), nil
		}))
		if err := c.Head("http://head.invalid/"); err != nil {
			t.Fatal(err)
		}
		if got := robotsCalls.Load(); got != 0 {
			t.Fatalf("HEAD requests must skip robots discovery, got %d fetches", got)
		}
		if got := headCalls.Load(); got != 1 {
			t.Fatalf("expected the HEAD request to be sent, got %d", got)
		}
	})

	t.Run("IgnoreRobotsTxt", func(t *testing.T) {
		var robotsCalls, pageCalls atomic.Int32
		c := NewCollector()
		c.IgnoreRobotsTxt = true
		c.WithTransport(stubTransport(func(r *http.Request) (*http.Response, error) {
			if r.URL.Path == "/robots.txt" {
				robotsCalls.Add(1)
			} else {
				pageCalls.Add(1)
			}
			return robotsDiscoveryResponse(r, "ok"), nil
		}))
		if err := c.Visit("http://ignore.invalid/"); err != nil {
			t.Fatal(err)
		}
		if got := robotsCalls.Load(); got != 0 {
			t.Fatalf("IgnoreRobotsTxt must skip robots discovery, got %d fetches", got)
		}
		if got := pageCalls.Load(); got != 1 {
			t.Fatalf("expected the page request to be sent, got %d", got)
		}
	})
}
