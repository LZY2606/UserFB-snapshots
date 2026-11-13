package browser

import (
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"sync"
	"testing"

	taskassert "github.com/headzoo/surf/task_support/assert"
)

// frameCounters records what the test portal observed about incoming requests.
type frameCounters struct {
	mu       sync.Mutex
	hits     map[string]int
	referers map[string][]string
	methods  []string
	forms    url.Values
	files    map[string]string
}

func newFrameCounters() *frameCounters {
	return &frameCounters{hits: map[string]int{}, referers: map[string][]string{}, files: map[string]string{}}
}

func (c *frameCounters) hit(r *http.Request) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.hits[r.URL.Path]++
	c.referers[r.URL.Path] = append(c.referers[r.URL.Path], r.Header.Get("Referer"))
	c.methods = append(c.methods, r.Method)
}

func (c *frameCounters) count(path string) int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.hits[path]
}

func (c *frameCounters) referer(path string) string {
	c.mu.Lock()
	defer c.mu.Unlock()
	refs := c.referers[path]
	if len(refs) == 0 {
		return ""
	}
	return refs[len(refs)-1]
}

// page renders a minimal HTML document with its own title.
func page(title, body string) string {
	return "<html><head><title>" + title + "</title></head><body>" + body + "</body></html>"
}

const queryForms = `<form method="POST" action="save" enctype="multipart/form-data" target="result">` +
	`<input name="station" value="7"><input type="file" name="upload">` +
	`<input type="submit" name="go" value="1"></form>` +
	`<form method="POST" action="/encoded" target="result"><input name="station" value="9"></form>` +
	`<form method="GET" action="/search" target="result"><input name="station" value="3"></form>` +
	`<form method="GET" action="/self" target="_self"><input name="k" value="s"></form>` +
	`<form method="GET" action="/parent" target="_parent"><input name="k" value="p"></form>` +
	`<form method="GET" action="/top" target="_TOP"><input name="k" value="tt"></form>` +
	`<form method="GET" action="/blank" target="_blank"><input name="k" value="b"></form>` +
	`<form method="GET" action="/ghost" target="ghost"><input name="k" value="g"></form>` +
	`<form method="GET" action="/dupname" target="dup"><input name="k" value="d"></form>`

// TestFrameWorkflowRouting drives a portal with a top navigation page, two
// iframe levels holding the query form, and a named sibling result frame.
// Real form submissions (GET, urlencoded POST and multipart POST) are routed
// to the declared form target, including a POST/redirect/final-page chain.
func TestFrameWorkflowRouting(t *testing.T) {
	ut := taskassert.New(t)
	cnt := newFrameCounters()

	mux := http.NewServeMux()
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		cnt.hit(r)
		switch r.URL.Path {
		case "/":
			io.WriteString(w, page("portal-top",
				`<iframe name="nav" src="/nav"></iframe>`+
					`<iframe name="result" src="/result/old"></iframe>`+
					`<iframe name="dup" src="/dup1"></iframe>`+
					`<iframe name="dup" src="/dup2"></iframe>`))
		case "/nav":
			io.WriteString(w, page("nav-frame",
				`<iframe name="query" src="/app/inner"></iframe>`))
		case "/app/inner":
			// A relative base that differs from the result frame's address;
			// the relative action "save" resolves against this base.
			io.WriteString(w, page("query-frame", `<base href="/forms/q/">`+queryForms))
		case "/result/old":
			io.WriteString(w, page("old-result", "nothing yet"))
		case "/dup1", "/dup2":
			io.WriteString(w, page("dup", ""))
		case "/forms/q/save":
			if r.Method != "POST" {
				t.Errorf("save: want POST, got %s", r.Method)
			}
			if err := r.ParseMultipartForm(1 << 20); err != nil {
				t.Fatal(err)
			}
			cnt.mu.Lock()
			cnt.forms = r.MultipartForm.Value
			for name, fhs := range r.MultipartForm.File {
				for _, fh := range fhs {
					f, _ := fh.Open()
					data, _ := io.ReadAll(f)
					cnt.files[name] = fh.Filename + ":" + string(data)
				}
			}
			cnt.mu.Unlock()
			http.Redirect(w, r, "/result/receipt", http.StatusSeeOther)
		case "/encoded":
			r.ParseForm()
			cnt.mu.Lock()
			cnt.forms = r.Form
			cnt.mu.Unlock()
			http.Redirect(w, r, "/result/receipt", http.StatusSeeOther)
		case "/search", "/self", "/parent", "/top":
			io.WriteString(w, page("hit-"+r.URL.Path, r.URL.RawQuery))
		case "/result/receipt":
			io.WriteString(w, page("receipt", "record=7"))
		default:
			http.NotFound(w, r)
		}
	})
	ts := httptest.NewServer(mux)
	defer ts.Close()

	// openStack builds a fresh root/nav/query/result tree.
	openStack := func() (*Browser, *Browser, *Browser, *Browser) {
		b := newDefaultTestBrowser()
		ut.AssertNil(b.Open(ts.URL + "/"))
		nav, err := b.Frame(`iframe[name="nav"]`)
		ut.AssertNil(err)
		query, err := nav.Frame(`iframe[name="query"]`)
		ut.AssertNil(err)
		result, err := b.Frame(`iframe[name="result"]`)
		ut.AssertNil(err)
		return b, nav, query, result
	}

	bow, nav, query, result := openStack()
	ut.AssertEquals("portal-top", bow.Title())

	// Lazy loading: nothing fetched until Frame is called.
	ut.AssertEquals(1, cnt.count("/nav"))
	// Selecting the same element again returns the same context, no request.
	navAgain, err := bow.Frame(`iframe[name="nav"]`)
	ut.AssertNil(err)
	ut.AssertTrue(nav == navAgain)
	ut.AssertEquals(1, cnt.count("/nav"))
	ut.AssertEquals("query-frame", query.Title())
	ut.AssertEquals("old-result", result.Title())

	// Multipart POST from the second-level frame into the sibling result.
	form, err := query.Form(`form[action="save"]`)
	ut.AssertNil(err)
	ut.AssertNil(form.Input("station", "7"))
	form.(*Form).SetFile("upload", "note.txt", strings.NewReader("FILEBYTES"))
	ut.AssertNil(form.Submit())

	ut.AssertEquals(1, cnt.count("/forms/q/save"))
	ut.AssertEquals(1, cnt.count("/result/receipt"))
	ut.AssertEquals("7", cnt.forms.Get("station"))
	ut.AssertEquals("note.txt:FILEBYTES", cnt.files["upload"])
	// Referer is the source document's final URL, not the old result page.
	ut.AssertEquals(ts.URL+"/app/inner", cnt.referer("/forms/q/save"))

	// The final document belongs to the result context only.
	ut.AssertEquals("receipt", result.Title())
	ut.AssertContains("record=7", result.Body())
	ut.AssertEquals(ts.URL+"/result/receipt", result.Url().String())
	ut.AssertContains("record=7", result.Find("body").Text())
	ut.AssertEquals("query-frame", query.Title())
	ut.AssertEquals("portal-top", bow.Title())
	ut.AssertEquals("nav-frame", nav.Title())

	// urlencoded POST with redirect into the same target.
	encForm, err := query.Form(`form[action="/encoded"]`)
	ut.AssertNil(err)
	ut.AssertNil(encForm.Submit())
	ut.AssertEquals(1, cnt.count("/encoded"))
	ut.AssertEquals("9", cnt.forms.Get("station"))
	ut.AssertEquals(ts.URL+"/app/inner", cnt.referer("/encoded"))
	ut.AssertEquals("receipt", result.Title())

	// GET submission with query string, still routed to the result frame.
	getForm, err := query.Form(`form[action="/search"]`)
	ut.AssertNil(err)
	ut.AssertNil(getForm.Submit())
	ut.AssertEquals(1, cnt.count("/search"))
	ut.AssertContains("station=3", result.Body())

	// _self stays in the source frame.
	b, n, q, _ := openStack()
	selfForm, _ := q.Form(`form[target="_self"]`)
	ut.AssertNil(selfForm.Submit())
	ut.AssertEquals("hit-/self", q.Title())
	ut.AssertEquals("portal-top", b.Title())
	ut.AssertEquals("nav-frame", n.Title())

	// _parent lands on the direct parent context.
	_, _, q, _ = openStack()
	parentForm, _ := q.Form(`form[target="_parent"]`)
	ut.AssertNil(parentForm.Submit())
	ut.AssertEquals("hit-/parent", q.frameParent.Title())

	// Reserved keywords are case-insensitive; _top lands on the tree root.
	bow2, _, q, _ := openStack()
	topForm, _ := q.Form(`form[target="_TOP"]`)
	ut.AssertNil(topForm.Submit())
	ut.AssertEquals("hit-/top", bow2.Title())

	// Unresolvable names, duplicate names and _blank produce ErrFrameTarget
	// and never issue a request.
	_, _, q, res := openStack()
	before := cnt.count("/blank")
	blankForm, _ := q.Form(`form[target="_blank"]`)
	ut.AssertTrue(errors.Is(blankForm.Submit(), ErrFrameTarget))
	ut.AssertEquals(before, cnt.count("/blank"))
	ut.AssertEquals("old-result", res.Title())

	before = cnt.count("/ghost")
	ghostForm, _ := q.Form(`form[target="ghost"]`)
	ut.AssertTrue(errors.Is(ghostForm.Submit(), ErrFrameTarget))
	ut.AssertEquals(before, cnt.count("/ghost"))

	before = cnt.count("/dupname")
	dupForm, _ := q.Form(`form[target="dup"]`)
	ut.AssertTrue(errors.Is(dupForm.Submit(), ErrFrameTarget))
	ut.AssertEquals(before, cnt.count("/dupname"))

	// A name that only exists in another NewTab is invisible to this tree.
	tab := bow2.NewTab()
	ghostServer := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		io.WriteString(w, page("tab", `<iframe name="ghost" src="`+ts.URL+`/dup1"></iframe>`))
	}))
	defer ghostServer.Close()
	ut.AssertNil(tab.Open(ghostServer.URL))
	ghostTab, err := tab.Frame(`iframe[name="ghost"]`)
	ut.AssertNil(err)
	ut.AssertTrue(ghostTab != nil)
	before = cnt.count("/ghost")
	ut.AssertTrue(errors.Is(ghostForm.Submit(), ErrFrameTarget))
	ut.AssertEquals(before, cnt.count("/ghost"))
}

// TestFrameWorkflowLifetime covers document ownership: failed opens are not
// registered, navigation detaches descendants and invalidates old forms and
// handles without issuing requests or reading attachments, and a failed
// response leaves the old tree intact.
func TestFrameWorkflowLifetime(t *testing.T) {
	ut := taskassert.New(t)
	cnt := newFrameCounters()
	var failChildOpen bool
	var failResultRead bool

	mux := http.NewServeMux()
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		cnt.hit(r)
		switch r.URL.Path {
		case "/":
			io.WriteString(w, page("top-v1",
				`<iframe name="f" src="/child"></iframe>`+
					`<iframe name="result" src="/result/old"></iframe>`))
		case "/v2":
			io.WriteString(w, page("top-v2",
				`<iframe name="f" src="/child"></iframe>`+
					`<iframe name="result" src="/result/old"></iframe>`))
		case "/child":
			io.WriteString(w, page("child",
				`<iframe name="g" src="/grand"></iframe>`+
					`<form method="POST" action="/go" target="result"><input name="x" value="1">`+
					`<input type="file" name="upload"><input type="submit" name="send" value="go"></form>`))
		case "/grand":
			io.WriteString(w, page("grand", ""))
		case "/result/old":
			io.WriteString(w, page("old-result", ""))
		case "/go":
			http.Redirect(w, r, "/result/receipt", http.StatusSeeOther)
		case "/result/receipt":
			io.WriteString(w, page("receipt", "done"))
		default:
			http.NotFound(w, r)
		}
	})
	ts := httptest.NewServer(mux)
	defer ts.Close()

	bow := newDefaultTestBrowser()
	bow.SetTransport(&controlTransport{
		RoundTripper: http.DefaultTransport.(*http.Transport).Clone(),
		openError:    func(path string) bool { return failChildOpen && path == "/child" },
		bodyError:    func(path string) bool { return failResultRead && path == "/result/receipt" },
	})

	ut.AssertNil(bow.Open(ts.URL + "/"))

	// First frame open fails at the transport and must not register anything.
	failChildOpen = true
	_, err := bow.Frame(`iframe[name="f"]`)
	ut.AssertNotNil(err)
	ut.AssertEquals(0, len(bow.frameChildren))
	failChildOpen = false
	f, err := bow.Frame(`iframe[name="f"]`)
	ut.AssertNil(err)
	ut.AssertEquals(1, len(bow.frameChildren))

	g, err := f.Frame(`iframe[name="g"]`)
	ut.AssertNil(err)
	oldForm, err := f.Form("form")
	ut.AssertNil(err)

	// A counting attachment proves stale forms never even read files.
	reads := 0
	oldForm.(*Form).SetFile("upload", "a.txt", &countingReader{
		r: strings.NewReader("DATA"), onRead: func() { reads++ },
	})

	// Navigate the parent document: all descendants detach permanently.
	ut.AssertNil(bow.Open(ts.URL + "/v2"))
	ut.AssertEquals("top-v2", bow.Title())

	staleChecks := []struct {
		name string
		fn   func() error
	}{
		{"Open", func() error { return f.Open(ts.URL + "/child") }},
		{"OpenForm", func() error { return f.OpenForm(ts.URL+"/child", nil) }},
		{"Post", func() error {
			return f.Post(ts.URL+"/go", "text/plain", strings.NewReader("x"))
		}},
		{"PostForm", func() error { return f.PostForm(ts.URL+"/go", url.Values{}) }},
		{"PostMultipart", func() error {
			return f.PostMultipart(ts.URL+"/go", nil, nil)
		}},
		{"Reload", f.Reload},
		{"Frame", func() error { _, e := f.Frame("iframe"); return e }},
		{"form submit", oldForm.Submit},
	}
	goBefore := cnt.count("/go")
	childBefore := cnt.count("/child")
	for _, sc := range staleChecks {
		if err := sc.fn(); !errors.Is(err, ErrFrameDetached) {
			t.Fatalf("%s: want ErrFrameDetached, got %v", sc.name, err)
		}
	}
	ut.AssertFalse(f.Back())
	ut.AssertEquals(goBefore, cnt.count("/go"))
	ut.AssertEquals(childBefore, cnt.count("/child"))
	ut.AssertEquals(0, reads)

	// The grandchild detached as well.
	ut.AssertTrue(errors.Is(g.Open(ts.URL+"/grand"), ErrFrameDetached))
	// But the old DOM snapshot stays readable.
	ut.AssertEquals("child", f.Title())
	ut.AssertContains("<form", f.Body())

	// A same-name frame in the new document creates a fresh handle; the old
	// one is never revived.
	f2, err := bow.Frame(`iframe[name="f"]`)
	ut.AssertNil(err)
	ut.AssertTrue(f2 != f)
	ut.AssertTrue(errors.Is(f.Reload(), ErrFrameDetached))

	// Failed target response read: old target page and descendants survive.
	result, err := bow.Frame(`iframe[name="result"]`)
	ut.AssertNil(err)
	g2, err := f2.Frame(`iframe[name="g"]`)
	ut.AssertNil(err)
	liveForm, err := f2.Form("form")
	ut.AssertNil(err)

	failResultRead = true
	ut.AssertNotNil(liveForm.Submit())
	failResultRead = false
	ut.AssertEquals("old-result", result.Title())
	ut.AssertNil(g2.Open(ts.URL + "/grand"))
	ut.AssertEquals("grand", g2.Title())

	// A later, healthy submission still works against the preserved tree.
	ut.AssertNil(liveForm.Submit())
	ut.AssertEquals("receipt", result.Title())
}

// TestFrameWorkflowIsolation checks per-context state, shared cookies and
// copied request configuration, and NewTab independence.
func TestFrameWorkflowIsolation(t *testing.T) {
	ut := taskassert.New(t)
	cnt := newFrameCounters()

	mux := http.NewServeMux()
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		cnt.hit(r)
		switch r.URL.Path {
		case "/":
			http.SetCookie(w, &http.Cookie{Name: "session", Value: "abc", Path: "/"})
			io.WriteString(w, page("top", `<iframe name="f" src="/child"></iframe>`))
		case "/child":
			cookie := "none"
			if c, err := r.Cookie("session"); err == nil {
				cookie = c.Value
			}
			io.WriteString(w, page("child", fmt.Sprintf(
				`<form method="POST" action="/go" target="_top"><input name="x" value="1"></form>`+
					`<p id="cookie">%s</p><p id="hdr">%s</p>`, cookie, r.Header.Get("X-Child"))))
		case "/go":
			io.WriteString(w, page("went", ""))
		}
	})
	ts := httptest.NewServer(mux)
	defer ts.Close()

	bow := newDefaultTestBrowser()
	ut.AssertNil(bow.Open(ts.URL + "/"))
	f, err := bow.Frame(`iframe[name="f"]`)
	ut.AssertNil(err)

	// Cookie jar is shared with the tree root, so the cookie set on the root
	// document is presented by the child.
	ut.AssertTrue(bow.CookieJar() == f.CookieJar())
	ut.AssertContains("abc", f.Find("#cookie").Text())

	// Each context keeps its own history and current address.
	ut.AssertTrue(bow.HistoryJar() != f.HistoryJar())
	ut.AssertEquals(ts.URL+"/", bow.Url().String())
	ut.AssertEquals(ts.URL+"/child", f.Url().String())

	// Request configuration is a copy: child header changes do not leak.
	f.AddRequestHeader("X-Child", "frame-only")
	_, ok := bow.headers["X-Child"]
	ut.AssertFalse(ok)
	// A fresh request from the child carries the copied configuration.
	ut.AssertNil(f.Open(ts.URL + "/child"))
	ut.AssertContains("frame-only", f.Find("#hdr").Text())

	// A submit to _top changes the root only; the child keeps its document.
	form, err := f.Form("form")
	ut.AssertNil(err)
	ut.AssertNil(form.Submit())
	ut.AssertEquals("went", bow.Title())
	ut.AssertEquals("child", f.Title())
	ut.AssertEquals(ts.URL+"/child", f.Url().String())

	// NewTab is a standalone top-level context without the frame registry,
	// but the shared jar conventions are preserved.
	tab := bow.NewTab()
	ut.AssertNil(tab.frameTree)
	ut.AssertNil(tab.frameParent)
	ut.AssertTrue(tab.CookieJar() == bow.CookieJar())
	ut.AssertTrue(tab.HistoryJar() == bow.HistoryJar())
	ut.AssertNil(tab.Open(ts.URL + "/child"))
	// Navigating the tab never detaches the original tree.
	ut.AssertEquals("child", f.Title())
}

// TestFrameFrameValidation verifies that every invalid frame selection is
// rejected before any request is sent.
func TestFrameFrameValidation(t *testing.T) {
	ut := taskassert.New(t)
	cnt := newFrameCounters()

	mux := http.NewServeMux()
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		cnt.hit(r)
		io.WriteString(w, page("top", `
			<div id="plain"></div>
			<iframe id="srcdoc" srcdoc="<p>x</p>"></iframe>
			<iframe id="nosrc"></iframe>
			<iframe id="rel" src="child"></iframe>
			<iframe id="ftp" src="ftp://example.com/x"></iframe>
			<iframe class="many" src="/one"></iframe>
			<iframe class="many" src="/two"></iframe>`))
	})
	mux.HandleFunc("/child", func(w http.ResponseWriter, r *http.Request) {
		cnt.hit(r)
		io.WriteString(w, page("child", ""))
	})
	ts := httptest.NewServer(mux)
	defer ts.Close()

	bow := newDefaultTestBrowser()
	ut.AssertNil(bow.Open(ts.URL + "/dir/page"))

	rejected := []string{
		`!!!not a selector`, // invalid selector
		`#missing`,          // no match
		`.many`,             // more than one match
		`#plain`,            // ordinary element
		`#srcdoc`,           // srcdoc frame
		`#nosrc`,            // missing src
		`#ftp`,              // non-HTTP(S) scheme
	}
	for _, expr := range rejected {
		if _, err := bow.Frame(expr); err == nil {
			t.Fatalf("Frame(%q) expected an error", expr)
		}
	}
	// Validation failures never reach the network.
	ut.AssertEquals(0, cnt.count("/child"))
	ut.AssertEquals(0, cnt.count("/one"))
	ut.AssertEquals(0, cnt.count("/two"))

	// A relative HTTP src resolves against the parent document's effective
	// base (here its final URL) and opens exactly one context.
	child, err := bow.Frame("#rel")
	ut.AssertNil(err)
	ut.AssertEquals(ts.URL+"/dir/child", child.Url().String())
	ut.AssertEquals(1, cnt.count("/child"))

	// Re-selecting the same element does not fetch again.
	again, err := bow.Frame("#rel")
	ut.AssertNil(err)
	ut.AssertTrue(again == child)
	ut.AssertEquals(1, cnt.count("/child"))
}

// controlTransport injects transport-level failures without extra sockets.
type controlTransport struct {
	http.RoundTripper
	openError func(path string) bool
	bodyError func(path string) bool
}

func (t *controlTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	if t.openError != nil && t.openError(req.URL.Path) {
		return nil, errors.New("simulated connection failure")
	}
	resp, err := t.RoundTripper.RoundTrip(req)
	if err != nil {
		return resp, err
	}
	if t.bodyError != nil && t.bodyError(req.URL.Path) {
		resp.Body = &failBody{ReadCloser: resp.Body}
	}
	return resp, nil
}

type failBody struct {
	io.ReadCloser
}

func (b *failBody) Read([]byte) (int, error) { return 0, errors.New("simulated broken response body") }
func (b *failBody) Close() error             { return b.ReadCloser.Close() }

type countingReader struct {
	r      io.Reader
	onRead func()
}

func (c *countingReader) Read(p []byte) (int, error) {
	c.onRead()
	return c.r.Read(p)
}
