package cmd

import (
	"context"
	"errors"
	"sync"
	"testing"
	"time"

	"github.com/go-rod/rod/lib/proto"
	"github.com/karust/openserp/core"
	"github.com/karust/openserp/testutil/cdptest"
	"golang.org/x/time/rate"
)

// challengeEngine is a SearchEngine that runs the real Browser.Navigate and
// DeferClosePage production paths over a scripted CDP transport, then returns
// a scripted result. It lets the test hold one page's settlement open while a
// concurrent challenge clears the lane.
type challengeEngine struct {
	name      string
	transport *cdptest.Transport
	browser   *core.Browser
	opts      core.SearchEngineOptions

	hold    chan struct{}
	entered chan struct{}

	mu        sync.Mutex
	scripts   map[string]challengeScript
	enterOnce sync.Once
}

type challengeScript struct {
	err     error
	cookies []*proto.NetworkCookie
}

func challengeCookie(name, value, domain string) *proto.NetworkCookie {
	return &proto.NetworkCookie{
		Name:    name,
		Value:   value,
		Domain:  domain,
		Path:    "/",
		Session: true,
	}
}

func (e *challengeEngine) scriptFor(q core.Query) challengeScript {
	e.mu.Lock()
	defer e.mu.Unlock()
	return e.scripts[q.Text]
}

func (e *challengeEngine) Search(ctx context.Context, q core.Query) ([]core.SearchResult, error) {
	page, err := e.browser.Navigate(ctx, "https://engine.example.invalid/search")
	if err != nil {
		return nil, err
	}
	defer core.DeferClosePage(ctx, page, e.browser)()

	target := e.transport.LastTarget()
	script := e.scriptFor(q)
	e.transport.SetFinalCookies(target.SessionID, script.cookies)

	if q.Text == "hold" {
		e.enterOnce.Do(func() { close(e.entered) })
		select {
		case <-e.hold:
		case <-ctx.Done():
			return nil, ctx.Err()
		}
	}

	if script.err != nil {
		return nil, script.err
	}
	return []core.SearchResult{{Rank: 1, Title: "result"}}, nil
}

func (e *challengeEngine) SearchImage(context.Context, core.Query) ([]core.SearchResult, error) {
	return nil, errors.New("image search not supported in test")
}

func (e *challengeEngine) Name() string        { return e.name }
func (e *challengeEngine) IsInitialized() bool { return true }
func (e *challengeEngine) GetRateLimiter() *rate.Limiter {
	e.opts.Init()
	return e.opts.GetRateLimiter()
}

// newChallengeStack wires the real browserPool, pooledBrowserEngine and
// ResilientSearcher around a transport-backed browser, so the full captcha
// cookie-drop chain runs without Chrome.
func newChallengeStack(t *testing.T, dropOnChallenge bool, engine *challengeEngine) (*core.ResilientSearcher, core.SearchEngine, *core.LaneStore, *browserPool) {
	t.Helper()

	store := core.NewLaneStore(10)
	transport := engine.transport
	pool := &browserPool{
		base: core.BrowserOpts{
			IsHeadless:     true,
			Timeout:        5 * time.Second,
			WaitLoadTime:   time.Second,
			ProxyLaneStore: store,
		},
		laneStore: store,
		browserFactory: func(opts core.BrowserOpts) (*core.Browser, error) {
			return core.NewBrowserWithCDPClient(opts, transport), nil
		},
		maxProcesses: 4,
		browsers:     map[string]*pooledBrowser{},
		stopSweeper:  make(chan struct{}),
		sweeperDone:  make(chan struct{}),
	}
	close(pool.sweeperDone)
	t.Cleanup(func() { _ = pool.close() })

	opts := core.SearchEngineOptions{RateRequests: 1000, RateTime: 1, RateBurst: 1000, SelectorTimeout: 1}
	opts.Init()
	engine.opts = opts
	pooled := &pooledBrowserEngine{
		name: "bing",
		opts: opts,
		factory: func(b core.Browser, _ core.SearchEngineOptions) core.SearchEngine {
			engine.browser = &b
			return engine
		},
		pool:            pool,
		reportLaneStats: true,
	}

	cfg := core.ResilientConfig{
		Retry:          core.RetryConfig{MaxRetries: 0},
		CircuitBreaker: core.DefaultCircuitBreakerConfig(),
		Proxy: core.ProxyConfig{
			Runtime: core.ProxyRuntimeBrowser,
			Proxies: core.ProxiesConfig{
				AllowRequestProxyURL: true,
				Lanes: core.ProxyLanesConfig{
					Enabled:                true,
					MaxLanes:               10,
					DropCookiesOnChallenge: dropOnChallenge,
				},
			},
			EnginePolicies: map[string]string{},
		},
	}
	searcher := core.NewResilientSearcher([]core.SearchEngine{pooled}, cfg)
	return searcher, pooled, store, pool
}

func challengeQuery(text, sessionID string) core.Query {
	return core.Query{
		Text:           text,
		ProxyURL:       "http://user:pass@proxy.example:8080",
		ProxySessionID: sessionID,
	}
}

func challengeLaneKey(sessionID string) core.ProxyLaneKey {
	return core.ProxyLaneKeyForTenant("bing", "tenant-a", challengeQuery("", sessionID), "http://user:pass@proxy.example:8080")
}

// TestLaneCookieLifecycleChallenge verifies captcha cleanup through the real
// ResilientSearcher + pooledBrowserEngine chain: a challenged lane is fenced,
// a late successful page on the old session cannot repollute it, other lanes
// keep committing, and the drop switch / network-error / stats rules hold.
func TestLaneCookieLifecycleChallenge(t *testing.T) {
	transport := cdptest.NewTransport()
	engine := &challengeEngine{
		name:      "bing",
		transport: transport,
		hold:      make(chan struct{}),
		entered:   make(chan struct{}),
		scripts: map[string]challengeScript{
			"hold": {
				err:     nil,
				cookies: []*proto.NetworkCookie{challengeCookie("polluted", "late", "engine.example.invalid")},
			},
			"captcha": {
				err: core.ErrCaptcha,
				// The captcha response kept the inherited seed and set a new
				// challenge cookie; the resilient layer must clear both afterward.
				cookies: []*proto.NetworkCookie{
					challengeCookie("seed", "1", "engine.example.invalid"),
					challengeCookie("challenge", "1", "engine.example.invalid"),
				},
			},
			"captcha-clean": {
				// No cookies at all in this page: the drop that follows sees an
				// empty lane and must not raise cookies_dropped.
				err:     core.ErrCaptcha,
				cookies: nil,
			},
			"ok": {
				err:     nil,
				cookies: []*proto.NetworkCookie{challengeCookie("other", "1", "engine.example.invalid")},
			},
			"network": {
				err: core.ErrProxyConnect,
				// The inherited cookie is untouched by the failed request.
				cookies: []*proto.NetworkCookie{challengeCookie("kept", "1", "engine.example.invalid")},
			},
		},
	}

	searcher, pooledEngine, store, _ := newChallengeStack(t, true, engine)
	ctx := core.WithTenant(context.Background(), "tenant-a")
	laneA := challengeLaneKey("sid-a")
	laneB := challengeLaneKey("sid-b")

	// Seed the challenged lane with an existing session.
	store.SaveCookies(laneA, []*proto.NetworkCookie{challengeCookie("seed", "1", "engine.example.invalid")})

	// Request 1 opens a page on lane A and stays open past the challenge,
	// mimicking an in-flight page that only returns successfully later.
	holdDone := make(chan error, 1)
	go func() {
		_, _, _, err := searcher.SearchPrimary(ctx, pooledEngine, challengeQuery("hold", "sid-a"))
		holdDone <- err
	}()
	select {
	case <-engine.entered:
	case <-time.After(5 * time.Second):
		t.Fatal("held page never reached its barrier")
	}

	// Request 2 on the same lane hits a captcha: the resilient layer drops the
	// lane cookies through the pooled engine's real drop hook.
	if _, _, _, err := searcher.SearchPrimary(ctx, pooledEngine, challengeQuery("captcha", "sid-a")); !errors.Is(err, core.ErrCaptcha) {
		t.Fatalf("expected ErrCaptcha, got %v", err)
	}
	if got := store.Cookies(laneA); len(got) != 0 {
		t.Fatalf("captcha did not clear the lane cookies: %#v", got)
	}
	if stats := store.Stats(); stats.CookiesDropped != 1 {
		t.Fatalf("expected cookies_dropped=1 after a populated-lane drop, got %#v", stats)
	}

	// Now the old page returns successfully. Its snapshot predates the drop,
	// so its polluting cookies must be rejected.
	close(engine.hold)
	select {
	case err := <-holdDone:
		if err != nil {
			t.Fatalf("held search failed: %v", err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("held search never completed")
	}
	if got := store.Cookies(laneA); len(got) != 0 {
		t.Fatalf("late successful page repolluted the dropped lane: %#v", got)
	}

	// A different channel (other session/lane) still commits normally.
	if _, _, _, err := searcher.SearchPrimary(ctx, pooledEngine, challengeQuery("ok", "sid-b")); err != nil {
		t.Fatalf("other channel search failed: %v", err)
	}
	if got := store.Cookies(laneB); len(got) != 1 || got[0].Name != "other" {
		t.Fatalf("other lane could not commit after the challenge: %#v", got)
	}

	// A second captcha on a page that set no new cookies: the lane is already
	// empty, so this drop must not inflate the counter.
	if _, _, _, err := searcher.SearchPrimary(ctx, pooledEngine, challengeQuery("captcha-clean", "sid-a")); !errors.Is(err, core.ErrCaptcha) {
		t.Fatalf("expected second ErrCaptcha, got %v", err)
	}
	if stats := store.Stats(); stats.CookiesDropped != 1 {
		t.Fatalf("empty-lane drop changed the counter: %#v", stats)
	}
	if got := store.Cookies(laneA); len(got) != 0 {
		t.Fatalf("second challenge left cookies behind: %#v", got)
	}

	// A normal network error never clears cookies.
	laneC := challengeLaneKey("sid-c")
	store.SaveCookies(laneC, []*proto.NetworkCookie{challengeCookie("kept", "1", "engine.example.invalid")})
	if _, _, _, err := searcher.SearchPrimary(ctx, pooledEngine, challengeQuery("network", "sid-c")); !errors.Is(err, core.ErrProxyConnect) {
		t.Fatalf("expected ErrProxyConnect, got %v", err)
	}
	if got := store.Cookies(laneC); len(got) != 1 || got[0].Name != "kept" {
		t.Fatalf("network error dropped lane cookies: %#v", got)
	}
	if stats := store.Stats(); stats.CookiesDropped != 1 {
		t.Fatalf("network error changed cookies_dropped: %#v", stats)
	}
}

// TestLaneCookieLifecycleChallengeSwitchOff verifies the
// drop_cookies_on_challenge switch fully disables the cleanup path.
func TestLaneCookieLifecycleChallengeSwitchOff(t *testing.T) {
	transport := cdptest.NewTransport()
	engine := &challengeEngine{
		name:      "bing",
		transport: transport,
		hold:      make(chan struct{}),
		entered:   make(chan struct{}),
		scripts: map[string]challengeScript{
			"captcha": {
				err: core.ErrCaptcha,
				// The page kept the inherited seed and added a challenge cookie.
				cookies: []*proto.NetworkCookie{
					challengeCookie("seed", "1", "engine.example.invalid"),
					challengeCookie("challenge", "1", "engine.example.invalid"),
				},
			},
		},
	}

	searcher, pooledEngine, store, _ := newChallengeStack(t, false, engine)
	ctx := core.WithTenant(context.Background(), "tenant-a")
	laneA := challengeLaneKey("sid-off")
	store.SaveCookies(laneA, []*proto.NetworkCookie{challengeCookie("seed", "1", "engine.example.invalid")})

	if _, _, _, err := searcher.SearchPrimary(ctx, pooledEngine, challengeQuery("captcha", "sid-off")); !errors.Is(err, core.ErrCaptcha) {
		t.Fatalf("expected ErrCaptcha, got %v", err)
	}
	got := store.Cookies(laneA)
	if findChallengeCookie(got, "seed") == nil {
		t.Fatalf("switch-off path must keep the inherited lane cookie: %#v", got)
	}
	if stats := store.Stats(); stats.CookiesDropped != 0 {
		t.Fatalf("switch-off path incremented cookies_dropped: %#v", stats)
	}
}

func findChallengeCookie(cookies []*proto.NetworkCookie, name string) *proto.NetworkCookie {
	for _, cookie := range cookies {
		if cookie.Name == name {
			return cookie
		}
	}
	return nil
}
