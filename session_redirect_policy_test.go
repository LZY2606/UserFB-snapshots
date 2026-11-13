package grequests

import (
	"context"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"net/url"
	"reflect"
	"sync"
	"testing"
	"time"
)

// newRedirectPolicyServer returns a server where /start redirects once to
// /data, plus a hit counter keyed by path.
func newRedirectPolicyServer(t *testing.T) (*httptest.Server, func() map[string]int) {
	t.Helper()

	var mu sync.Mutex
	hits := map[string]int{}

	mux := http.NewServeMux()
	mux.HandleFunc("/start", func(w http.ResponseWriter, r *http.Request) {
		mu.Lock()
		hits["/start"]++
		mu.Unlock()
		http.Redirect(w, r, "/data", http.StatusFound)
	})
	mux.HandleFunc("/data", func(w http.ResponseWriter, r *http.Request) {
		mu.Lock()
		hits["/data"]++
		mu.Unlock()
		fmt.Fprint(w, "records=2")
	})

	srv := httptest.NewServer(mux)
	t.Cleanup(srv.Close)

	return srv, func() map[string]int {
		mu.Lock()
		defer mu.Unlock()
		out := make(map[string]int, len(hits))
		for k, v := range hits {
			out[k] = v
		}
		return out
	}
}

// TestRedirectPolicyIsolation verifies that the redirect policy of one
// request never leaks into another request sharing the same session, while
// the connection pool and the cookie jar are still shared.
func TestRedirectPolicyIsolation(t *testing.T) {
	srv, hits := newRedirectPolicyServer(t)
	session := NewSession(nil)

	// A negative limit returns the first redirect response...
	first, err := session.Get(context.Background(), srv.URL+"/start", &RequestOptions{RedirectLimit: -1})
	if err != nil {
		t.Fatalf("first Get: %v", err)
	}
	first.Close()
	if first.StatusCode != http.StatusFound {
		t.Fatalf("first Get: want 302, got %d", first.StatusCode)
	}
	if hits()["/data"] != 0 {
		t.Fatal("negative RedirectLimit must not follow the redirect")
	}

	// ...and the very next request on the same session follows redirects
	// with its own positive limit.
	second, err := session.Get(context.Background(), srv.URL+"/start", &RequestOptions{RedirectLimit: 4})
	if err != nil {
		t.Fatalf("second Get: %v", err)
	}
	defer second.Close()
	if second.StatusCode != http.StatusOK || second.String() != "records=2" {
		t.Fatalf("second Get: want 200 records=2, got %d %q", second.StatusCode, second.String())
	}
	if hits()["/data"] != 1 {
		t.Fatalf("positive limit should have followed the redirect, hits=%v", hits())
	}

	// Reuse the session in the opposite order: follow first, then stop.
	third, err := session.Get(context.Background(), srv.URL+"/start", &RequestOptions{RedirectLimit: 4})
	if err != nil {
		t.Fatalf("third Get: %v", err)
	}
	third.Close()
	if third.StatusCode != http.StatusOK {
		t.Fatalf("third Get: want 200, got %d", third.StatusCode)
	}

	fourth, err := session.Get(context.Background(), srv.URL+"/start", &RequestOptions{RedirectLimit: -1})
	if err != nil {
		t.Fatalf("fourth Get: %v", err)
	}
	fourth.Close()
	if fourth.StatusCode != http.StatusFound {
		t.Fatalf("fourth Get: want 302, got %d", fourth.StatusCode)
	}

	// Control: a plain request without any redirect options still works.
	direct, err := session.Get(context.Background(), srv.URL+"/data", nil)
	if err != nil {
		t.Fatalf("direct Get: %v", err)
	}
	defer direct.Close()
	if direct.String() != "records=2" {
		t.Fatalf("direct Get: want records=2, got %q", direct.String())
	}

	runConcurrentRedirectChains(t)
}

// runConcurrentRedirectChains runs two redirect chains with different
// limits through one session at the same time.
func runConcurrentRedirectChains(t *testing.T) {
	var mu sync.Mutex
	cookiesAtMid := []string{}
	tailHits := 0

	entered := make(chan struct{}, 2)
	release := make(chan struct{})

	mux := http.NewServeMux()
	mux.HandleFunc("/chain", func(w http.ResponseWriter, r *http.Request) {
		http.SetCookie(w, &http.Cookie{Name: "chain", Value: "1"})
		http.Redirect(w, r, "/chain-mid", http.StatusFound)
	})
	mux.HandleFunc("/chain-mid", func(w http.ResponseWriter, r *http.Request) {
		cookieValue := ""
		if c, err := r.Cookie("chain"); err == nil {
			cookieValue = c.Value
		}
		mu.Lock()
		cookiesAtMid = append(cookiesAtMid, cookieValue)
		mu.Unlock()

		entered <- struct{}{}
		<-release

		http.Redirect(w, r, "/chain-tail", http.StatusFound)
	})
	mux.HandleFunc("/chain-tail", func(w http.ResponseWriter, r *http.Request) {
		mu.Lock()
		tailHits++
		mu.Unlock()
		fmt.Fprint(w, "tail")
	})
	mux.HandleFunc("/echo-cookie", func(w http.ResponseWriter, r *http.Request) {
		if c, err := r.Cookie("chain"); err == nil {
			fmt.Fprint(w, c.Value)
		}
	})

	srv := httptest.NewServer(mux)
	defer srv.Close()

	session := NewSession(nil)

	type chainResult struct {
		limit  int
		status int
		body   string
		err    error
	}
	results := make(chan chainResult, 2)

	for _, limit := range []int{2, 5} {
		go func(limit int) {
			resp, err := session.Get(context.Background(), srv.URL+"/chain", &RequestOptions{RedirectLimit: limit})
			res := chainResult{limit: limit, err: err}
			if err == nil {
				res.status = resp.StatusCode
				res.body = resp.String()
				resp.Close()
			}
			results <- res
		}(limit)
	}

	// Both chains must be in flight at the same time before either is
	// allowed to continue - their policies are resolved independently.
	for i := 0; i < 2; i++ {
		select {
		case <-entered:
		case <-time.After(10 * time.Second):
			t.Fatal("timed out waiting for both redirect chains to be in flight")
		}
	}
	close(release)

	byLimit := map[int]chainResult{}
	for i := 0; i < 2; i++ {
		select {
		case res := <-results:
			byLimit[res.limit] = res
		case <-time.After(10 * time.Second):
			t.Fatal("timed out waiting for redirect chains to finish")
		}
	}

	// The limit=2 chain stops at the second redirect...
	if !errors.Is(byLimit[2].err, ErrRedirectLimitExceeded) {
		t.Fatalf("limit=2 chain: want ErrRedirectLimitExceeded, got %v", byLimit[2].err)
	}

	// ...while the limit=5 chain reaches the tail on the same session.
	if byLimit[5].err != nil || byLimit[5].status != http.StatusOK || byLimit[5].body != "tail" {
		t.Fatalf("limit=5 chain: want 200 tail, got %d %q err=%v", byLimit[5].status, byLimit[5].body, byLimit[5].err)
	}

	mu.Lock()
	if tailHits != 1 {
		t.Errorf("tail should be hit exactly once, got %d", tailHits)
	}
	if len(cookiesAtMid) != 2 || cookiesAtMid[0] != "1" || cookiesAtMid[1] != "1" {
		t.Errorf("both chains should carry the shared cookie, got %v", cookiesAtMid)
	}
	mu.Unlock()

	// The cookie jar is still shared after the concurrent chains.
	echo, err := session.Get(context.Background(), srv.URL+"/echo-cookie", nil)
	if err != nil {
		t.Fatalf("echo-cookie Get: %v", err)
	}
	defer echo.Close()
	if echo.String() != "1" {
		t.Fatalf("cookie jar not shared: want 1, got %q", echo.String())
	}
}

// TestRedirectPolicyComposition verifies that a user supplied CheckRedirect
// is composed with (not replaced by) the library policy, and that neither
// the user's client nor http.DefaultClient is permanently modified.
func TestRedirectPolicyComposition(t *testing.T) {
	srv, _ := newRedirectPolicyServer(t)

	// The library policy runs first: when it rejects, the user callback
	// is never invoked and the user's client keeps its own CheckRedirect.
	userCalls := 0
	userFn := func(req *http.Request, via []*http.Request) error {
		userCalls++
		return nil
	}
	client := &http.Client{CheckRedirect: userFn}

	_, err := Get(context.Background(), srv.URL+"/start", HTTPClient(client), RedirectLimit(1))
	if !errors.Is(err, ErrRedirectLimitExceeded) {
		t.Fatalf("want ErrRedirectLimitExceeded, got %v", err)
	}
	if userCalls != 0 {
		t.Fatal("user callback must not run when the library policy rejects the redirect")
	}
	if reflect.ValueOf(client.CheckRedirect).Pointer() != reflect.ValueOf(userFn).Pointer() {
		t.Fatal("the composed closure must not be installed on the caller's client")
	}
	if client.Transport != nil || client.Jar != nil || client.Timeout != 0 {
		t.Fatal("the caller's client must keep its Transport, Jar and Timeout")
	}

	// A plain error from the user callback keeps its net/http meaning.
	boom := errors.New("user boom")
	boomClient := &http.Client{CheckRedirect: func(req *http.Request, via []*http.Request) error {
		return boom
	}}
	if _, err = Get(context.Background(), srv.URL+"/start", HTTPClient(boomClient), RedirectLimit(10)); !errors.Is(err, boom) {
		t.Fatalf("want the user's error, got %v", err)
	}

	// http.ErrUseLastResponse from the user callback returns the redirect
	// response itself.
	lastClient := &http.Client{CheckRedirect: func(req *http.Request, via []*http.Request) error {
		return http.ErrUseLastResponse
	}}
	last, err := Get(context.Background(), srv.URL+"/start", HTTPClient(lastClient), RedirectLimit(10))
	if err != nil {
		t.Fatalf("ErrUseLastResponse Get: %v", err)
	}
	last.Close()
	if last.StatusCode != http.StatusFound {
		t.Fatalf("ErrUseLastResponse: want 302, got %d", last.StatusCode)
	}

	// When both policies allow, the redirect is followed and the user
	// callback observes it exactly once.
	allowedCalls := 0
	allowClient := &http.Client{CheckRedirect: func(req *http.Request, via []*http.Request) error {
		allowedCalls++
		return nil
	}}
	followed, err := Get(context.Background(), srv.URL+"/start", HTTPClient(allowClient), RedirectLimit(10))
	if err != nil {
		t.Fatalf("composed Get: %v", err)
	}
	followed.Close()
	if followed.StatusCode != http.StatusOK || allowedCalls != 1 {
		t.Fatalf("composed Get: want 200 with 1 user call, got %d with %d", followed.StatusCode, allowedCalls)
	}

	// Package level requests must not mutate http.DefaultClient.
	neg, err := Get(context.Background(), srv.URL+"/start", RedirectLimit(-1))
	if err != nil {
		t.Fatalf("default client Get: %v", err)
	}
	neg.Close()
	if neg.StatusCode != http.StatusFound {
		t.Fatalf("default client Get: want 302, got %d", neg.StatusCode)
	}
	if http.DefaultClient.CheckRedirect != nil {
		t.Fatal("http.DefaultClient must not be modified by a request")
	}

	runHeaderCopyCheck(t)
}

// runHeaderCopyCheck verifies that headers are not duplicated on every
// redirect hop.
func runHeaderCopyCheck(t *testing.T) {
	var mu sync.Mutex
	seen := []string{}

	mux := http.NewServeMux()
	mux.HandleFunc("/hop1", func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, "/hop2", http.StatusFound)
	})
	mux.HandleFunc("/hop2", func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, "/hop3", http.StatusFound)
	})
	mux.HandleFunc("/hop3", func(w http.ResponseWriter, r *http.Request) {
		mu.Lock()
		seen = r.Header.Values("X-Custom")
		mu.Unlock()
		fmt.Fprint(w, "end")
	})
	srv := httptest.NewServer(mux)
	defer srv.Close()

	session := NewSession(nil)
	resp, err := session.Get(context.Background(), srv.URL+"/hop1", &RequestOptions{
		RedirectLimit: 10,
		Headers:       map[string]string{"X-Custom": "v"},
	})
	if err != nil {
		t.Fatalf("hop Get: %v", err)
	}
	defer resp.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("hop Get: want 200, got %d", resp.StatusCode)
	}

	mu.Lock()
	defer mu.Unlock()
	if len(seen) != 1 || seen[0] != "v" {
		t.Fatalf("header values must not grow per hop, got %v", seen)
	}
}

// TestSessionOptionsSnapshot verifies that merging session defaults never
// writes back into the caller's RequestOptions and that a Context passed to
// one call does not leak into later calls reusing the same options.
func TestSessionOptionsSnapshot(t *testing.T) {
	// The landing server is reached through a different hostname so that
	// net/http strips the Authorization header before the redirect policy
	// decides whether to re-attach it.
	var authMu sync.Mutex
	authSeen := []string{}
	landing := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		authMu.Lock()
		authSeen = append(authSeen, r.Header.Get("Authorization"))
		authMu.Unlock()
		fmt.Fprint(w, "landed")
	}))
	defer landing.Close()

	landingURL, err := url.Parse(landing.URL)
	if err != nil {
		t.Fatal(err)
	}
	landingHost := "localhost:" + landingURL.Port()

	jumper := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/plain" {
			fmt.Fprint(w, "plain")
			return
		}
		http.Redirect(w, r, "http://"+landingHost+"/landing", http.StatusFound)
	}))
	defer jumper.Close()

	lastAuth := func() string {
		authMu.Lock()
		defer authMu.Unlock()
		return authSeen[len(authSeen)-1]
	}

	session := NewSession(&RequestOptions{
		UserAgent:     "ua",
		Headers:       map[string]string{"A": "1"},
		RedirectLimit: -1,
	})

	ro := &RequestOptions{Headers: map[string]string{"B": "2"}}
	snapshot := ro.clone()

	// The session default (-1) applies when the request does not set a
	// limit...
	resp, err := session.Get(context.Background(), jumper.URL+"/jump", ro)
	if err != nil {
		t.Fatalf("snapshot Get: %v", err)
	}
	resp.Close()
	if resp.StatusCode != http.StatusFound {
		t.Fatalf("session default RedirectLimit should apply: want 302, got %d", resp.StatusCode)
	}

	// ...and a non-zero limit on the request overrides the session
	// default.
	override := &RequestOptions{RedirectLimit: 5}
	followed, err := session.Get(context.Background(), jumper.URL+"/jump", override)
	if err != nil {
		t.Fatalf("override Get: %v", err)
	}
	followed.Close()
	if followed.StatusCode != http.StatusOK || followed.String() != "landed" {
		t.Fatalf("request RedirectLimit should override the session: got %d %q", followed.StatusCode, followed.String())
	}

	// The caller's options are untouched by everything above.
	if !reflect.DeepEqual(snapshot, ro) {
		t.Fatalf("request options mutated: before %+v after %+v", snapshot, ro)
	}
	if override.Context != nil || len(override.Headers) != 0 {
		t.Fatalf("override options mutated: %+v", override)
	}
	if session.RequestOptions.RedirectLimit != -1 {
		t.Fatalf("session options mutated: %+v", session.RequestOptions)
	}

	// A Context passed to one call must not leak into the next call that
	// reuses the same options object.
	ctx, cancel := context.WithCancel(context.Background())
	withCtx, err := session.Get(ctx, jumper.URL+"/plain", ro)
	if err != nil {
		t.Fatalf("ctx Get: %v", err)
	}
	withCtx.Close()
	cancel()
	if ro.Context != nil {
		t.Fatal("context leaked into the caller's RequestOptions")
	}
	afterCancel, err := session.Get(context.Background(), jumper.URL+"/plain", ro)
	if err != nil {
		t.Fatalf("Get after cancel with reused options: %v", err)
	}
	afterCancel.Close()
	if afterCancel.String() != "plain" {
		t.Fatalf("reused options: want plain, got %q", afterCancel.String())
	}

	// Session level SensitiveHTTPHeaders participate in the merge: an
	// explicitly empty set means no header is stripped, so Authorization
	// is re-attached even across hosts.
	emptySetSession := NewSession(&RequestOptions{SensitiveHTTPHeaders: map[string]struct{}{}})
	forwarded, err := emptySetSession.Get(context.Background(), jumper.URL+"/jump", &RequestOptions{
		RedirectLimit: 5,
		Auth:          []string{"u", "p"},
	})
	if err != nil {
		t.Fatalf("empty set Get: %v", err)
	}
	forwarded.Close()
	if lastAuth() == "" {
		t.Fatal("empty sensitive header set should not strip Authorization")
	}

	// A non-nil set on the request overrides the session default.
	stripped, err := emptySetSession.Get(context.Background(), jumper.URL+"/jump", &RequestOptions{
		RedirectLimit:        5,
		Auth:                 []string{"u", "p"},
		SensitiveHTTPHeaders: map[string]struct{}{"Authorization": {}},
	})
	if err != nil {
		t.Fatalf("override sensitive Get: %v", err)
	}
	stripped.Close()
	if lastAuth() != "" {
		t.Fatalf("request sensitive header set should override the session, got %q", lastAuth())
	}

	// With no configuration at all the global default strips
	// Authorization.
	plainSession := NewSession(nil)
	strippedDefault, err := plainSession.Get(context.Background(), jumper.URL+"/jump", &RequestOptions{
		RedirectLimit: 5,
		Auth:          []string{"u", "p"},
	})
	if err != nil {
		t.Fatalf("default sensitive Get: %v", err)
	}
	strippedDefault.Close()
	if lastAuth() != "" {
		t.Fatalf("default sensitive headers should strip Authorization, got %q", lastAuth())
	}
}
