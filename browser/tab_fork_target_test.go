package browser

import (
	"context"
	"fmt"
	"net"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/headzoo/surf/jar"
	"github.com/headzoo/surf/task_support/taskcheck"
)

// pipeListener is an in-memory net.Listener joined with a dial function, so
// the fork tests can run a real local HTTP site without depending on
// loopback TCP ports.
type pipeListener struct {
	conns  chan net.Conn
	closed chan struct{}
	once   sync.Once
}

func newPipeListener() *pipeListener {
	return &pipeListener{
		conns:  make(chan net.Conn),
		closed: make(chan struct{}),
	}
}

func (l *pipeListener) Accept() (net.Conn, error) {
	select {
	case c := <-l.conns:
		return c, nil
	case <-l.closed:
		return nil, net.ErrClosed
	}
}

func (l *pipeListener) Close() error {
	l.once.Do(func() { close(l.closed) })
	return nil
}

func (l *pipeListener) Addr() net.Addr {
	return &net.TCPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 80}
}

// dial returns the client end of an in-memory pipe whose other end is
// handed to the HTTP server through Accept.
func (l *pipeListener) dial(ctx context.Context, _, _ string) (net.Conn, error) {
	client, server := net.Pipe()
	select {
	case l.conns <- server:
		return client, nil
	case <-l.closed:
		client.Close()
		server.Close()
		return nil, net.ErrClosed
	case <-ctx.Done():
		client.Close()
		server.Close()
		return nil, ctx.Err()
	}
}

// newPipeServer starts a local HTTP site serving the given handler, and
// returns the server along with the transport a browser must use to reach
// it. The site is served over in-memory pipes instead of a loopback TCP
// port, so the tests do not depend on being allowed to bind ports.
func newPipeServer(handler http.HandlerFunc) (*httptest.Server, http.RoundTripper) {
	listener := newPipeListener()
	ts := &httptest.Server{
		Listener: listener,
		Config:   &http.Server{Handler: handler},
	}
	ts.Start()
	return ts, &http.Transport{DialContext: listener.dial}
}

// unforkableHistory is a custom history that does not implement
// jar.ForkableHistory, so tabs using it cannot be forked.
type unforkableHistory struct {
	jar.History
}

// recordedRequest captures the interesting parts of a request received by
// the test site.
type recordedRequest struct {
	method string
	path   string
	name   string
	ua     string
	branch string
	cookie string
}

// requestRecorder is an http.HandlerFunc that serves the fork test site and
// records every request it receives.
type requestRecorder struct {
	mu       sync.Mutex
	requests []recordedRequest
}

func (rr *requestRecorder) handler(w http.ResponseWriter, r *http.Request) {
	rec := recordedRequest{method: r.Method, path: r.URL.Path, ua: r.UserAgent()}
	rec.branch = r.Header.Get("X-Branch")
	if cookie, err := r.Cookie("sid"); err == nil {
		rec.cookie = cookie.String()
	}
	switch r.URL.Path {
	case "/form":
		http.SetCookie(w, &http.Cookie{Name: "sid", Value: "abc", Path: "/"})
		fmt.Fprint(w, `<html><head><title>apply</title></head><body>
<form method="post" action="/submit"><input name="name" value="seed"></form>
</body></html>`)
	case "/submit":
		if err := r.ParseForm(); err == nil {
			rec.name = r.Form.Get("name")
		}
		w.Header().Set("Location", "/receipt?name="+url.QueryEscape(rec.name))
		w.WriteHeader(http.StatusFound)
	case "/receipt":
		rec.name = r.URL.Query().Get("name")
		fmt.Fprintf(w, `<html><head><title>receipt</title></head><body>name=%s</body></html>`, rec.name)
	case "/echo":
		fmt.Fprint(w, `<html><head><title>echo</title></head><body>echo</body></html>`)
	default:
		http.NotFound(w, r)
	}
	rr.mu.Lock()
	rr.requests = append(rr.requests, rec)
	rr.mu.Unlock()
}

// find returns the recorded requests matching the given method and path.
func (rr *requestRecorder) find(method, path string) []recordedRequest {
	rr.mu.Lock()
	defer rr.mu.Unlock()
	var found []recordedRequest
	for _, rec := range rr.requests {
		if rec.method == method && rec.path == path {
			found = append(found, rec)
		}
	}
	return found
}

// TestForkTabSnapshots verifies that a forked tab gets independent copies of
// the current state, the history and the mutable configuration, while the
// fork continues from the DOM as modified on the original tab.
func TestForkTabSnapshots(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		ts, transport := newPipeServer(func(w http.ResponseWriter, r *http.Request) {
			switch r.URL.Path {
			case "/page1":
				fmt.Fprint(w, `<html><head><title>page1</title></head><body><h1>page-one</h1></body></html>`)
			case "/page2":
				fmt.Fprint(w, `<html><head><title>page2</title></head><body><h1>confirm</h1><p id="msg">original</p></body></html>`)
			case "/page3":
				fmt.Fprint(w, `<html><head><title>page3</title></head><body><h1>page-three</h1></body></html>`)
			case "/based":
				fmt.Fprint(w, `<html><head><title>based</title><base href="/sub/"></head><body><a href="x.html">x</a></body></html>`)
			default:
				http.NotFound(w, r)
			}
		})
		defer ts.Close()

		parent := newDefaultTestBrowser()
		parent.SetTransport(transport)
		parent.headers.Add("X-Multi", "a")
		parent.headers.Add("X-Multi", "b")
		if err := parent.Open(ts.URL + "/page1"); err != nil {
			c.Check(false, "open page1: %v", err)
			return
		}
		if err := parent.Open(ts.URL + "/page2"); err != nil {
			c.Check(false, "open page2: %v", err)
			return
		}
		c.Check(parent.HistoryJar().Len() == 2, "expected history length 2, got %d", parent.HistoryJar().Len())

		// Modify the live DOM before forking; the fork must continue from
		// this modified document instead of re-parsing the page.
		parent.Find("#msg").SetText("edited-by-parent")

		fork, err := parent.ForkTab()
		c.Check(err == nil, "fork tab: %v", err)
		if err != nil {
			return
		}
		c.Check(fork != parent, "fork returned the same browser")

		// The current state is copied, not shared.
		c.Check(fork.State() != parent.State(), "fork shares the current state with the parent")
		c.Check(fork.Url().String() == parent.Url().String(), "fork url %q, want %q", fork.Url(), parent.Url())
		c.Check(fork.Title() == "page2", "fork title %q, want page2", fork.Title())
		c.Check(fork.Find("#msg").Text() == "edited-by-parent",
			"fork did not continue from the modified DOM, got %q", fork.Find("#msg").Text())
		c.Check(fork.Find("body > p#msg").Length() == 1, "fork lost the parent-child node relationships")

		// DOM modifications stay on the tab that made them.
		fork.Find("#msg").SetText("edited-by-fork")
		c.Check(parent.Find("#msg").Text() == "edited-by-parent",
			"parent DOM changed by fork, got %q", parent.Find("#msg").Text())
		parent.Find("h1").SetText("parent-h1")
		c.Check(fork.Find("h1").Text() == "confirm", "fork DOM changed by parent, got %q", fork.Find("h1").Text())

		// Request, response and URL metadata are independent.
		fork.State().Request.Header.Set("X-Branch", "fork")
		c.Check(parent.State().Request.Header.Get("X-Branch") == "", "parent request headers changed by fork")
		fork.State().Response.Header.Set("X-Branch", "fork")
		c.Check(parent.State().Response.Header.Get("X-Branch") == "", "parent response headers changed by fork")
		c.Check(fork.State().Request.URL != parent.State().Request.URL, "fork shares the request URL with the parent")
		c.Check(fork.State().Response.Request.URL != parent.State().Response.Request.URL,
			"fork shares the response URL with the parent")
		fork.State().Request.URL.Path = "/changed"
		c.Check(parent.State().Request.URL.Path == "/page2",
			"parent request URL changed by fork, got %q", parent.State().Request.URL.Path)

		// The request headers jar, including multi-value slices, is copied.
		c.Check(len(parent.headers.Values("X-Multi")) == 2, "parent multi-value header lost values")
		c.Check(len(fork.headers.Values("X-Multi")) == 2, "fork did not inherit the multi-value header")
		fork.headers.Add("X-Multi", "c")
		c.Check(len(fork.headers.Values("X-Multi")) == 3, "fork header values not updated")
		c.Check(len(parent.headers.Values("X-Multi")) == 2, "parent header values changed by fork")

		// Attributes and the current body are copied.
		fork.SetAttribute(FollowRedirects, false)
		c.Check(parent.attributes[FollowRedirects], "parent FollowRedirects changed by fork")
		c.Check(!fork.attributes[FollowRedirects], "fork attribute not updated")
		c.Check(string(fork.body) == string(parent.body), "fork body differs from parent body")
		if len(fork.body) > 0 && len(parent.body) > 0 {
			fork.body[0] = 'X'
			c.Check(parent.body[0] == '<', "parent body bytes changed by fork")
		}

		// The history is copied with its order preserved.
		c.Check(fork.HistoryJar() != parent.HistoryJar(), "fork shares the history jar with the parent")
		c.Check(fork.HistoryJar().Len() == 2, "fork history length %d, want 2", fork.HistoryJar().Len())
		c.Check(fork.HistoryJar().Top() != parent.HistoryJar().Top(), "fork shares history states with the parent")
		c.Check(fork.HistoryJar().Top().Response.Request.URL.String() == ts.URL+"/page1",
			"fork history order not preserved, top is %q", fork.HistoryJar().Top().Response.Request.URL)

		// States recorded in the history are independent as well.
		fork.HistoryJar().Top().Dom.Find("h1").SetText("fork-hist")
		c.Check(parent.HistoryJar().Top().Dom.Find("h1").Text() == "page-one",
			"parent history DOM changed by fork, got %q", parent.HistoryJar().Top().Dom.Find("h1").Text())
		fork.HistoryJar().Top().Response.Header.Set("X-Hist", "fork")
		c.Check(parent.HistoryJar().Top().Response.Header.Get("X-Hist") == "",
			"parent history response headers changed by fork")

		// Back and Pop only affect the tab they are called on.
		c.Check(fork.Back(), "fork could not go back")
		c.Check(fork.Url().String() == ts.URL+"/page1", "fork url after Back %q, want page1", fork.Url())
		c.Check(parent.Url().String() == ts.URL+"/page2", "parent url changed by fork Back, got %q", parent.Url())
		c.Check(parent.HistoryJar().Len() == 2, "parent history length changed by fork Back")
		c.Check(fork.HistoryJar().Len() == 1, "fork history length %d after Back, want 1", fork.HistoryJar().Len())

		// Push only affects the tab that navigates.
		if err := parent.Open(ts.URL + "/page3"); err != nil {
			c.Check(false, "open page3: %v", err)
			return
		}
		c.Check(parent.HistoryJar().Len() == 3, "parent history length %d, want 3", parent.HistoryJar().Len())
		c.Check(fork.HistoryJar().Len() == 1, "fork history length changed by parent navigation")
		c.Check(fork.Url().String() == ts.URL+"/page1", "fork url changed by parent navigation, got %q", fork.Url())

		// SetMax only affects the tab it is called on.
		parent.HistoryJar().SetMax(1)
		if err := parent.Open(ts.URL + "/page1"); err != nil {
			c.Check(false, "re-open page1: %v", err)
			return
		}
		c.Check(parent.HistoryJar().Len() == 1, "parent history not trimmed, length %d", parent.HistoryJar().Len())
		fork.HistoryJar().Push(&jar.State{})
		fork.HistoryJar().Push(&jar.State{})
		c.Check(fork.HistoryJar().Len() == 3, "fork history trimmed by parent SetMax, length %d", fork.HistoryJar().Len())
		fork.HistoryJar().SetMax(1)
		fork.HistoryJar().Push(&jar.State{})
		c.Check(fork.HistoryJar().Len() == 1, "fork SetMax did not trim its own history, length %d", fork.HistoryJar().Len())
		c.Check(parent.HistoryJar().Len() == 1 && parent.HistoryJar().Top().Response.Request.URL.Path == "/page3",
			"parent history changed by fork SetMax")

		// Clear only affects the tab it is called on.
		fork.HistoryJar().Clear()
		c.Check(fork.HistoryJar().Len() == 0, "fork history not cleared")
		c.Check(parent.HistoryJar().Len() == 1, "parent history cleared by fork")

		// The relative URL configuration is inherited and then independent.
		parent2 := newDefaultTestBrowser()
		parent2.SetTransport(transport)
		if err := parent2.Open(ts.URL + "/based"); err != nil {
			c.Check(false, "open based: %v", err)
			return
		}
		fork2, err := parent2.ForkTab()
		c.Check(err == nil, "fork tab with base tag: %v", err)
		if err != nil {
			return
		}
		resolved, err := fork2.ResolveStringUrl("y.html")
		c.Check(err == nil && resolved == ts.URL+"/sub/y.html",
			"fork resolved %q, want %q", resolved, ts.URL+"/sub/y.html")
		c.Check(fork2.relativeUrl != parent2.relativeUrl, "fork shares the relative URL with the parent")
		fork2.relativeUrl.Path = "/other/"
		resolved, _ = parent2.ResolveStringUrl("y.html")
		c.Check(resolved == ts.URL+"/sub/y.html", "parent relative URL changed by fork, resolved %q", resolved)
		resolved, _ = fork2.ResolveStringUrl("y.html")
		c.Check(resolved == ts.URL+"/other/y.html", "fork relative URL not updated, resolved %q", resolved)
	})
}

// TestForkTabRequests verifies that both branches of a fork submit real
// forms over HTTP with their own fields, request headers and redirect
// behavior, while the cookie jar, transport and timeout start out shared.
func TestForkTabRequests(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		recorder := &requestRecorder{}
		ts, transport := newPipeServer(recorder.handler)
		defer ts.Close()

		parent := newDefaultTestBrowser()
		parent.SetTransport(transport)
		parent.SetUserAgent("parent-ua")
		parent.SetTimeout(7 * time.Second)
		if err := parent.Open(ts.URL + "/form"); err != nil {
			c.Check(false, "open form: %v", err)
			return
		}

		// Obtain and fill the form on the parent before forking.
		parentForm, err := parent.Form("form")
		c.Check(err == nil, "parent form: %v", err)
		if err != nil {
			return
		}
		if err := parentForm.Input("name", "parent-name"); err != nil {
			c.Check(false, "parent input: %v", err)
			return
		}

		fork, err := parent.ForkTab()
		c.Check(err == nil, "fork tab: %v", err)
		if err != nil {
			return
		}

		// Cookie jar, transport and bookmarks jar are shared objects, and
		// the forked client inherits the timeout.
		c.Check(fork.CookieJar() == parent.CookieJar(), "fork did not share the cookie jar")
		c.Check(fork.client.Transport == parent.client.Transport, "fork did not share the transport")
		c.Check(fork.BookmarksJar() == parent.BookmarksJar(), "fork did not share the bookmarks jar")
		c.Check(fork.client.Timeout == 7*time.Second, "fork timeout %v, want 7s", fork.client.Timeout)

		fork.SetUserAgent("fork-ua")
		fork.AddRequestHeader("X-Branch", "fork")

		// The fork fills its own copy of the form and submits it; the
		// submit response redirects to the receipt page.
		forkForm, err := fork.Form("form")
		c.Check(err == nil, "fork form: %v", err)
		if err != nil {
			return
		}
		if err := forkForm.Input("name", "fork-name"); err != nil {
			c.Check(false, "fork input: %v", err)
			return
		}
		if err := forkForm.Submit(); err != nil {
			c.Check(false, "fork submit: %v", err)
			return
		}
		c.Check(fork.Url().Path == "/receipt", "fork url after submit %q, want /receipt", fork.Url())
		c.Check(strings.Contains(fork.Find("body").Text(), "name=fork-name"),
			"fork receipt body %q, want name=fork-name", fork.Find("body").Text())
		c.Check(parent.Url().Path == "/form", "parent url changed by fork navigation, got %q", parent.Url())

		// The form obtained from the parent before forking still belongs to
		// the parent, and the parent's redirect setting is its own.
		parent.SetAttribute(FollowRedirects, false)
		err = parentForm.Submit()
		c.Check(err != nil, "parent submit should fail: redirects are disabled on the parent")
		c.Check(parent.Url().Path == "/form", "parent url changed by refused redirect, got %q", parent.Url())

		// The site saw both submissions with the fields, headers and
		// cookies of the tab that issued them.
		posts := recorder.find("POST", "/submit")
		c.Check(len(posts) == 2, "recorded %d posts to /submit, want 2", len(posts))
		if len(posts) == 2 {
			c.Check(posts[0].name == "fork-name", "fork post name %q, want fork-name", posts[0].name)
			c.Check(posts[0].ua == "fork-ua", "fork post user agent %q, want fork-ua", posts[0].ua)
			c.Check(posts[0].branch == "fork", "fork post X-Branch %q, want fork", posts[0].branch)
			c.Check(posts[0].cookie == "sid=abc", "fork post cookie %q, want sid=abc", posts[0].cookie)
			c.Check(posts[1].name == "parent-name", "parent post name %q, want parent-name", posts[1].name)
			c.Check(posts[1].ua == "parent-ua", "parent post user agent %q, want parent-ua", posts[1].ua)
			c.Check(posts[1].branch == "", "parent post sent fork header X-Branch=%q", posts[1].branch)
			c.Check(posts[1].cookie == "sid=abc", "parent post cookie %q, want sid=abc", posts[1].cookie)
		}

		// The redirect was followed using the fork's own user agent.
		receipts := recorder.find("GET", "/receipt")
		c.Check(len(receipts) == 1, "recorded %d gets to /receipt, want 1", len(receipts))
		if len(receipts) == 1 {
			c.Check(receipts[0].ua == "fork-ua", "redirect user agent %q, want fork-ua", receipts[0].ua)
			c.Check(receipts[0].name == "fork-name", "receipt name %q, want fork-name", receipts[0].name)
		}

		// Replacing the cookie jar on the fork leaves the parent's jar alone.
		fork.SetCookieJar(jar.NewMemoryCookies())
		if err := fork.Open(ts.URL + "/echo"); err != nil {
			c.Check(false, "fork open echo: %v", err)
			return
		}
		if err := parent.Open(ts.URL + "/echo"); err != nil {
			c.Check(false, "parent open echo: %v", err)
			return
		}
		echos := recorder.find("GET", "/echo")
		c.Check(len(echos) == 2, "recorded %d gets to /echo, want 2", len(echos))
		if len(echos) == 2 {
			c.Check(echos[0].cookie == "", "fork sent cookie %q after replacing its jar", echos[0].cookie)
			c.Check(echos[1].cookie == "sid=abc", "parent cookie %q, want sid=abc", echos[1].cookie)
		}

		// Timeout and transport changes stay on the tab that made them.
		fork.SetTimeout(3 * time.Second)
		c.Check(parent.client.Timeout == 7*time.Second, "parent timeout changed by fork, got %v", parent.client.Timeout)
		c.Check(fork.client.Timeout == 3*time.Second, "fork timeout %v, want 3s", fork.client.Timeout)
		fork.SetTransport(&http.Transport{})
		c.Check(parent.client.Transport == transport, "parent transport changed by fork")
		c.Check(fork.client.Transport != parent.client.Transport, "fork transport not replaced")
	})
}

// TestForkTabLifetime verifies that forking does not inherit or disturb
// meta refresh timers, that a browser without a page can be forked, and
// that forking fails cleanly with a history jar that is not forkable.
func TestForkTabLifetime(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		var refreshHits int64
		ts, transport := newPipeServer(func(w http.ResponseWriter, r *http.Request) {
			switch r.URL.Path {
			case "/refresh":
				atomic.AddInt64(&refreshHits, 1)
				fmt.Fprint(w, `<html><head><meta http-equiv="refresh" content="1"></head><body>tick</body></html>`)
			case "/refresh10":
				fmt.Fprint(w, `<html><head><meta http-equiv="refresh" content="10"></head><body>tock</body></html>`)
			case "/other":
				fmt.Fprint(w, `<html><head><title>other</title></head><body>other</body></html>`)
			default:
				http.NotFound(w, r)
			}
		})
		defer ts.Close()

		// A pending meta refresh timer is not inherited by the fork, and
		// requests made by the fork do not stop the parent's timer.
		parent := newDefaultTestBrowser()
		parent.SetTransport(transport)
		if err := parent.Open(ts.URL + "/refresh"); err != nil {
			c.Check(false, "open refresh: %v", err)
			return
		}
		c.Check(parent.refresh != nil, "parent has no meta refresh timer")
		fork, err := parent.ForkTab()
		c.Check(err == nil, "fork tab: %v", err)
		if err != nil {
			return
		}
		c.Check(fork.refresh == nil, "fork inherited the pending meta refresh timer")
		if err := fork.Open(ts.URL + "/other"); err != nil {
			c.Check(false, "fork open other: %v", err)
			return
		}
		c.Check(parent.refresh != nil, "fork request stopped the parent's meta refresh timer")
		c.Check(fork.Url().Path == "/other", "fork url %q, want /other", fork.Url())
		reloaded := false
		for deadline := time.Now().Add(5 * time.Second); time.Now().Before(deadline); {
			if atomic.LoadInt64(&refreshHits) >= 2 {
				reloaded = true
				break
			}
			time.Sleep(20 * time.Millisecond)
		}
		c.Check(reloaded, "parent meta refresh timer did not fire after forking")

		// A custom history that is not a jar.ForkableHistory cannot be
		// forked, and the failed fork leaves page, history and timers alone.
		parent2 := newDefaultTestBrowser()
		parent2.SetTransport(transport)
		parent2.SetHistoryJar(&unforkableHistory{jar.NewMemoryHistory()})
		if err := parent2.Open(ts.URL + "/refresh10"); err != nil {
			c.Check(false, "open refresh10: %v", err)
			return
		}
		state := parent2.State()
		history := parent2.HistoryJar()
		refresh := parent2.refresh
		pageURL := parent2.Url().String()
		historyLen := history.Len()
		c.Check(refresh != nil, "parent2 has no meta refresh timer")
		fork2, err := parent2.ForkTab()
		c.Check(err != nil, "forking an unforkable history should fail")
		c.Check(fork2 == nil, "failed fork returned a browser")
		c.Check(parent2.State() == state, "failed fork changed the current state")
		c.Check(parent2.HistoryJar() == history, "failed fork changed the history jar")
		c.Check(parent2.HistoryJar().Len() == historyLen, "failed fork changed the history length")
		c.Check(parent2.refresh == refresh, "failed fork changed the meta refresh timer")
		c.Check(parent2.Url().String() == pageURL, "failed fork changed the page")
		parent2.refresh.Stop()

		// A browser that has not opened a page yet can be forked.
		fresh := newDefaultTestBrowser()
		fresh.SetTransport(transport)
		fork3, err := fresh.ForkTab()
		c.Check(err == nil, "fork fresh browser: %v", err)
		if err == nil {
			c.Check(fork3 != nil && fork3 != fresh, "fork of fresh browser is not a new browser")
			if err := fork3.Open(ts.URL + "/other"); err != nil {
				c.Check(false, "fork of fresh browser open: %v", err)
			} else {
				c.Check(fork3.Url().Path == "/other", "fork3 url %q, want /other", fork3.Url())
			}
			c.Check(fresh.Url() == nil, "fresh browser got a page from its fork")
		}
	})
}
