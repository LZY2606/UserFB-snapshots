package regress_test

import (
	"context"
	"errors"
	"net/http"
	"net/http/httptest"
	"os"
	"testing"
	"time"

	c "github.com/chromedp/chromedp"
)

// sourceTestContext starts a headless browser for one test and returns a
// context with a deadline. The runner image sets CHROMEDP_TEST_RUNNER.
func sourceTestContext(t *testing.T) (context.Context, context.CancelFunc) {
	t.Helper()
	a, ac := c.NewExecAllocator(context.Background(), append(c.DefaultExecAllocatorOptions[:],
		c.ExecPath(os.Getenv("CHROMEDP_TEST_RUNNER")), c.NoSandbox)...)
	ctx, cancel := c.NewContext(a)
	ctx, deadline := context.WithTimeout(ctx, 20*time.Second)
	cleanup := func() {
		deadline()
		cancel()
		ac()
	}
	return ctx, cleanup
}

// waitFor polls fn until it reports true or the context ends. The browser
// owns this state, so no Go event can wake the test.
func waitFor(ctx context.Context, t *testing.T, what string, fn func() (bool, error)) {
	t.Helper()
	tick := time.NewTicker(25 * time.Millisecond)
	defer tick.Stop()
	for {
		ok, err := fn()
		if err != nil {
			t.Fatalf("%s: %v", what, err)
		}
		if ok {
			return
		}
		select {
		case <-ctx.Done():
			t.Fatalf("%s: %v", what, ctx.Err())
		case <-tick.C:
		}
	}
}

// rec is the record that the pages of the origin test send.
type rec struct {
	ID int `json:"id"`
}

// TestSourceBindingOrigin makes the main frame and two same-site iframes
// send the same record. One iframe reaches its document through a redirect.
// The test asserts the source that the target built from its own records.
func TestSourceBindingOrigin(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		switch r.URL.Path {
		case "/redir":
			// The source must name the final address of the document.
			http.Redirect(w, r, "/a", http.StatusFound)
		case "/a", "/b":
			w.Write([]byte(`<script>collect({id: 7}).then(function (r) { window.out = r; })</script>`))
		default:
			w.Write([]byte(`<iframe id="f1" src="/redir"></iframe><iframe id="f2" src="/b"></iframe>` +
				`<script>collect({id: 7}).then(function (r) { window.out = r; })</script>`))
		}
	}))
	defer srv.Close()

	ctx, cleanup := sourceTestContext(t)
	defer cleanup()

	type got struct {
		source c.CallSource
		arg    rec
	}
	calls := make(chan got, 8)
	if err := c.Do(ctx,
		c.ExposeSourceFunc("collect", func(ctx context.Context, source c.CallSource, v rec) (string, error) {
			calls <- got{source, v}
			// The document gets its own document address back.
			return source.URL, nil
		}),
		c.Navigate(srv.URL+"/"),
	); err != nil {
		t.Fatal(err)
	}

	wantURLs := map[string]bool{
		srv.URL + "/":  false,
		srv.URL + "/a": false,
		srv.URL + "/b": false,
	}
	seenFrame := map[c.CallSource]bool{}
	for len(wantURLs) > 0 {
		select {
		case call := <-calls:
			if call.arg.ID != 7 {
				t.Fatalf("got record id %d, want 7", call.arg.ID)
			}
			if call.source.ExecutionContextID == 0 {
				t.Fatalf("call from %q has no execution context id", call.source.URL)
			}
			if _, ok := wantURLs[call.source.URL]; !ok {
				t.Fatalf("unexpected source url %q", call.source.URL)
			}
			if wantURLs[call.source.URL] {
				t.Fatalf("the url %q arrived twice", call.source.URL)
			}
			if seenFrame[call.source] {
				t.Fatalf("the source %+v arrived twice", call.source)
			}
			seenFrame[call.source] = true
			delete(wantURLs, call.source.URL)
		case <-ctx.Done():
			t.Fatalf("waiting for the three sources: %v", ctx.Err())
		}
	}

	// The frame ids and the execution context ids of the three documents
	// must differ.
	byFrame := map[string]c.CallSource{}
	for source := range seenFrame {
		byFrame[string(source.FrameID)] = source
	}
	if len(byFrame) != 3 {
		t.Fatalf("got %d frame ids, want 3", len(byFrame))
	}
	ctxIDs := map[int64]bool{}
	for _, source := range byFrame {
		if ctxIDs[int64(source.ExecutionContextID)] {
			t.Fatalf("the execution context id %d is used twice", source.ExecutionContextID)
		}
		ctxIDs[int64(source.ExecutionContextID)] = true
	}

	// Each document must receive its own result, the address of its own
	// document. The redirect gives the final address.
	waitFor(ctx, t, "the main frame result", func() (bool, error) {
		return c.Run(ctx, c.Evaluate[bool](`window.out === location.href`))
	})
	waitFor(ctx, t, "the iframe results", func() (bool, error) {
		got, err := c.Run(ctx, c.Evaluate[bool](`
			var f1 = document.getElementById("f1").contentWindow;
			var f2 = document.getElementById("f2").contentWindow;
			f1.out === f1.location.href && f2.out === f2.location.href
		`))
		return got, err
	})
}

// liveCall is one call that the lifetime test keeps waiting.
type liveCall struct {
	ctx context.Context
	src c.CallSource
}

// TestSourceBindingLifetime ends the context of a call when its execution
// context is destroyed or its frame is removed, and keeps it for a hash
// change and for a call in another document. Channels order every step.
func TestSourceBindingLifetime(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		switch r.URL.Path {
		case "/child", "/child2":
			w.Write([]byte(`<p>child</p>`))
		default:
			w.Write([]byte(`<iframe id="c" src="/child"></iframe><p>first document</p>`))
		}
	}))
	defer srv.Close()

	ctx, cleanup := sourceTestContext(t)
	defer cleanup()

	started := make(chan liveCall, 8)
	ended := make(chan string, 8)
	if err := c.Do(ctx, c.ExposeSourceFunc("hold", func(ctx context.Context, src c.CallSource, _ c.Void) (c.Void, error) {
		started <- liveCall{ctx, src}
		<-ctx.Done()
		ended <- string(src.FrameID)
		return c.Void{}, ctx.Err()
	})); err != nil {
		t.Fatal(err)
	}
	if err := c.Do(ctx, c.Navigate(srv.URL+"/life")); err != nil {
		t.Fatal(err)
	}

	// The wrapper script runs in the open document and in the iframe.
	waitFor(ctx, t, "the binding in the iframe", func() (bool, error) {
		return c.Run(ctx, c.Evaluate[bool](`typeof document.getElementById("c").contentWindow.hold === "function"`))
	})

	// Start one call in the main frame and one in the iframe.
	if _, err := c.Run(ctx, c.Evaluate[any](`hold()`)); err != nil {
		t.Fatal(err)
	}
	if _, err := c.Run(ctx, c.Evaluate[any](`document.getElementById("c").contentWindow.hold()`)); err != nil {
		t.Fatal(err)
	}

	var top, child liveCall
	for i := 0; i < 2; i++ {
		select {
		case call := <-started:
			switch call.src.URL {
			case srv.URL + "/life":
				top = call
			case srv.URL + "/child":
				child = call
			default:
				t.Fatalf("unexpected source %q", call.src.URL)
			}
		case <-ctx.Done():
			t.Fatalf("waiting for the two calls to start: %v", ctx.Err())
		}
	}
	if top.ctx == nil || child.ctx == nil {
		t.Fatal("the main frame or the iframe never called hold")
	}

	// Navigate the iframe to another document. Its call must end when the
	// browser destroys its execution context. The call in the main frame
	// is in another document and must keep running.
	if err := c.Do(ctx, c.Evaluate[c.Void](`document.getElementById("c").src = "/child2"`)); err != nil {
		t.Fatal(err)
	}
	select {
	case id := <-ended:
		if id != string(child.src.FrameID) {
			t.Fatalf("the frame %s ended, want the iframe %s", id, child.src.FrameID)
		}
	case <-ctx.Done():
		t.Fatalf("waiting for the iframe call to end: %v", ctx.Err())
	}
	if !errors.Is(child.ctx.Err(), context.Canceled) {
		t.Fatalf("the iframe call ended with %v, want context.Canceled", child.ctx.Err())
	}
	if top.ctx.Err() != nil {
		t.Fatalf("the main frame call ended with %v while the iframe navigated", top.ctx.Err())
	}

	// A hash change is no navigation to another document. The call in the
	// main frame must stay alive.
	if err := c.Do(ctx, c.Evaluate[c.Void](`location.hash = "x"`)); err != nil {
		t.Fatal(err)
	}
	waitFor(ctx, t, "the hash change", func() (bool, error) {
		return c.Run(ctx, c.Evaluate[bool](`location.hash === "#x"`))
	})
	if top.ctx.Err() != nil {
		t.Fatalf("the main frame call ended with %v after a hash change", top.ctx.Err())
	}

	// Start a call in the new iframe document, then remove the frame. Its
	// call must end when the browser removes the frame.
	waitFor(ctx, t, "the binding in the new iframe document", func() (bool, error) {
		return c.Run(ctx, c.Evaluate[bool](`
			var f = document.getElementById("c").contentWindow;
			f.location.pathname === "/child2" && typeof f.hold === "function"
		`))
	})
	if _, err := c.Run(ctx, c.Evaluate[any](`document.getElementById("c").contentWindow.hold()`)); err != nil {
		t.Fatal(err)
	}
	var second liveCall
	select {
	case second = <-started:
	case <-ctx.Done():
		t.Fatalf("waiting for the second iframe call to start: %v", ctx.Err())
	}
	if err := c.Do(ctx, c.Evaluate[c.Void](`document.getElementById("c").remove()`)); err != nil {
		t.Fatal(err)
	}
	select {
	case id := <-ended:
		if id != string(second.src.FrameID) {
			t.Fatalf("the frame %s ended, want the iframe %s", id, second.src.FrameID)
		}
	case <-ctx.Done():
		t.Fatalf("waiting for the second iframe call to end: %v", ctx.Err())
	}

	// The call in the main frame ends when the target closes.
	if top.ctx.Err() != nil {
		t.Fatalf("the main frame call ended early with %v", top.ctx.Err())
	}
	cleanup()
	select {
	case id := <-ended:
		if id != string(top.src.FrameID) {
			t.Fatalf("the frame %s ended, want the main frame %s", id, top.src.FrameID)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("the main frame call did not end when the target closed")
	}
	if !errors.Is(top.ctx.Err(), context.Canceled) {
		t.Fatalf("the main frame call ended with %v, want context.Canceled", top.ctx.Err())
	}
}

// TestSourceBindingIsolation interleaves a slow call of an old document with
// a call of the new document. Each document starts the wrapper numbering at
// 1, so the two calls use the same promise number. The new document must get
// its own result, and the late result of the old document must be dropped.
func TestSourceBindingIsolation(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		switch r.URL.Path {
		case "/iso2":
			w.Write([]byte(`<p>second</p>`))
		default:
			w.Write([]byte(`<p>first</p>`))
		}
	}))
	defer srv.Close()

	ctx, cleanup := sourceTestContext(t)
	defer cleanup()

	release := make(chan struct{})
	defer close(release)
	queryRan := make(chan string, 4)
	if err := c.Do(ctx, c.ExposeSourceFunc("slow", func(ctx context.Context, src c.CallSource, arg string) (string, error) {
		// A normal chromedp query inside the callback must not deadlock
		// the event loop of the target.
		loc, err := c.Run(ctx, c.Location())
		if err != nil {
			return "", err
		}
		// Every call from the old document waits. A call from the new
		// document returns at once, so its promise cannot take the old
		// result.
		if src.URL == srv.URL+"/iso1" {
			queryRan <- loc
			// Ignore the cancellation of the old document on purpose.
			<-release
			return "old-result", nil
		}
		return "new-result", nil
	})); err != nil {
		t.Fatal(err)
	}

	// The first document starts a slow call. Its promise is number 1.
	if err := c.Do(ctx, c.Navigate(srv.URL+"/iso1")); err != nil {
		t.Fatal(err)
	}
	if _, err := c.Run(ctx, c.Evaluate[any](`window.p1 = slow("first")`)); err != nil {
		t.Fatal(err)
	}
	var loc string
	select {
	case loc = <-queryRan:
	case <-ctx.Done():
		t.Fatalf("waiting for the query inside the callback: %v", ctx.Err())
	}
	if loc != srv.URL+"/iso1" {
		t.Fatalf("the callback query read %q, want %q", loc, srv.URL+"/iso1")
	}

	// The second document starts its own numbering, so its first call is
	// promise number 1 as well.
	if err := c.Do(ctx, c.Navigate(srv.URL+"/iso2"), c.WaitReady(c.CSS("p"))); err != nil {
		t.Fatal(err)
	}
	got, err := c.Run(ctx, c.Evaluate[string](`slow("second").then(function (r) { window.r2 = r; return r; })`,
		c.EvalAwaitPromise))
	if err != nil {
		t.Fatal(err)
	}
	if got != "new-result" {
		t.Fatalf("the new document got %q, want new-result", got)
	}

	// Now let the old call finish. Its result must not reach the new
	// document.
	release <- struct{}{}
	waitFor(ctx, t, "the settled result in the new document", func() (bool, error) {
		v, err := c.Run(ctx, c.Evaluate[string](`window.r2`))
		return v == "new-result", err
	})
	got, err = c.Run(ctx, c.Evaluate[string](`window.r2`))
	if err != nil || got != "new-result" {
		t.Fatalf("the new document holds %q (%v), want new-result", got, err)
	}
}
