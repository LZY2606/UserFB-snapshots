package gowebdav

import (
	"context"
	"errors"
	"io"
	"net/http"
	"strings"
	"sync"
	"testing"

	"github.com/studio-b12/gowebdav/task_support/check"
)

// --- scripted test infrastructure -------------------------------------------

type trackedBody interface {
	io.ReadCloser
	isClosed() bool
}

type syncBody struct {
	reader io.Reader
	mu     sync.Mutex
	closed bool
}

func (b *syncBody) Read(p []byte) (int, error) { return b.reader.Read(p) }
func (b *syncBody) Close() error {
	b.mu.Lock()
	b.closed = true
	b.mu.Unlock()
	return nil
}
func (b *syncBody) isClosed() bool {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.closed
}

type errBody struct{ err error }

func (b *errBody) Read([]byte) (int, error) { return 0, b.err }
func (b *errBody) Close() error             { return nil }
func (b *errBody) isClosed() bool           { return true }

// blockingBody simulates an in-flight response read that only unblocks when
// the response is closed, as cancelling an http.Request does on real bodies.
type blockingBody struct {
	once    sync.Once
	started chan struct{}
	gate    chan struct{}
	mu      sync.Mutex
	closed  bool
}

func newBlockingBody() *blockingBody {
	return &blockingBody{started: make(chan struct{}), gate: make(chan struct{})}
}

func (b *blockingBody) Read([]byte) (int, error) {
	b.once.Do(func() { close(b.started) })
	<-b.gate
	return 0, context.Canceled
}

func (b *blockingBody) Close() error {
	b.mu.Lock()
	defer b.mu.Unlock()
	if !b.closed {
		b.closed = true
		close(b.gate)
	}
	return nil
}

func (b *blockingBody) isClosed() bool {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.closed
}

type syncCall struct {
	method  string
	uri     string
	headers http.Header
	body    string
}

type syncFakeTransport struct {
	mu     sync.Mutex
	calls  []syncCall
	bodies []trackedBody
	handle func(step int, call syncCall) *http.Response
}

func (t *syncFakeTransport) RoundTrip(r *http.Request) (*http.Response, error) {
	bodyText := ""
	if r.Body != nil {
		data, err := io.ReadAll(r.Body)
		if err != nil {
			return nil, err
		}
		bodyText = string(data)
	}
	call := syncCall{method: r.Method, uri: r.URL.String(), headers: r.Header.Clone(), body: bodyText}

	t.mu.Lock()
	t.calls = append(t.calls, call)
	step := len(t.calls)
	handle := t.handle
	t.mu.Unlock()

	rs := handle(step, call)
	if tb, ok := rs.Body.(trackedBody); ok {
		t.mu.Lock()
		t.bodies = append(t.bodies, tb)
		t.mu.Unlock()
	}
	rs.Request = r
	return rs, nil
}

func (t *syncFakeTransport) snapshot() []syncCall {
	t.mu.Lock()
	defer t.mu.Unlock()
	out := make([]syncCall, len(t.calls))
	copy(out, t.calls)
	return out
}

func (t *syncFakeTransport) allClosed() bool {
	t.mu.Lock()
	defer t.mu.Unlock()
	for _, b := range t.bodies {
		if !b.isClosed() {
			return false
		}
	}
	return true
}

func (t *syncFakeTransport) respond(status int, payload string) *http.Response {
	return &http.Response{
		StatusCode: status,
		Header:     make(http.Header),
		Body:       &syncBody{reader: strings.NewReader(payload)},
	}
}

func newSyncTestClient() (*Client, *syncFakeTransport) {
	tr := &syncFakeTransport{}
	cli := NewAuthClient("http://dav.invalid/base/", NewPreemptiveAuth(&noAuth{}))
	cli.SetTransport(tr)
	return cli, tr
}

const syncCollection = "http://dav.invalid/base/docs/"

// --- XML builders -----------------------------------------------------------

func syncMulti(token, self string, members ...string) string {
	var b strings.Builder
	b.WriteString(`<?xml version="1.0" encoding="utf-8"?>`)
	b.WriteString(`<D:multistatus xmlns:D="DAV:">`)
	switch self {
	case "trunc":
		b.WriteString(`<D:response><D:href>/base/docs/</D:href>` +
			`<D:status>HTTP/1.1 507 Insufficient Storage</D:status>` +
			`<D:error><D:number-of-matches-within-limits/></D:error></D:response>`)
	case "ok":
		b.WriteString(`<D:response><D:href>/base/docs/</D:href>` +
			`<D:propstat><D:prop><D:resourcetype><D:collection/></D:resourcetype></D:prop>` +
			`<D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>`)
	}
	for _, m := range members {
		b.WriteString(m)
	}
	b.WriteString(`<D:sync-token>` + token + `</D:sync-token></D:multistatus>`)
	return b.String()
}

func fileResp(href, etag string) string {
	return `<D:response><D:href>` + href + `</D:href><D:propstat><D:prop>` +
		`<D:getetag>` + etag + `</D:getetag><D:resourcetype/></D:prop>` +
		`<D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>`
}

func dirResp(href string) string {
	return `<D:response><D:href>` + href + `</D:href><D:propstat><D:prop>` +
		`<D:resourcetype><D:collection/></D:resourcetype></D:prop>` +
		`<D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>`
}

func delResp(href string) string {
	return `<D:response><D:href>` + href + `</D:href>` +
		`<D:status>HTTP/1.1 404 Not Found</D:status></D:response>`
}

func etagHeader(etag string) http.Header {
	header := make(http.Header)
	header.Set("ETag", etag)
	return header
}

func getResponse(etag, payload string) *http.Response {
	header := make(http.Header)
	header.Set("ETag", etag)
	return &http.Response{
		StatusCode: http.StatusOK,
		Header:     header,
		Body:       &syncBody{reader: strings.NewReader(payload)},
	}
}

func reportCalls(calls []syncCall) []syncCall {
	out := make([]syncCall, 0, len(calls))
	for _, call := range calls {
		if call.method == "REPORT" {
			out = append(out, call)
		}
	}
	return out
}

func getCalls(calls []syncCall) []syncCall {
	out := make([]syncCall, 0, len(calls))
	for _, call := range calls {
		if call.method == http.MethodGet {
			out = append(out, call)
		}
	}
	return out
}

func mustSnapshot(t *testing.T, snap *SyncSnapshot, err error) *SyncSnapshot {
	t.Helper()
	if err != nil {
		t.Fatalf("CollectSync failed: %v", err)
	}
	if snap == nil {
		t.Fatal("CollectSync returned nil snapshot without error")
	}
	return snap
}

// --- TestSyncPages ----------------------------------------------------------

func TestSyncPages(t *testing.T) {
	check.Run(t, func(c *check.C) {
		cli, tr := newSyncTestClient()
		tr.handle = func(step int, call syncCall) *http.Response {
			switch call.method {
			case "REPORT":
				switch step {
				case 1:
					return tr.respond(207, syncMulti("token-1", "trunc",
						fileResp("/base/docs/a.txt", `"v1"`),
						delResp("/base/docs/b.txt"),
						fileResp("/base/docs/c.txt", `"v9"`),
						fileResp("/base/docs/c.txt", `"v9"`),
					))
				case 2:
					return tr.respond(207, syncMulti("token-2", "trunc",
						delResp("/base/docs/a.txt"),
						fileResp("/base/docs/b.txt", `"v2"`),
						dirResp("/base/docs/sub/"),
					))
				case 3:
					return tr.respond(207, syncMulti("token-3", "ok"))
				}
			case http.MethodGet:
				switch call.uri {
				case syncCollection + "b.txt":
					if call.headers.Get("If-Match") != `"v2"` {
						t.Fatalf("b.txt If-Match = %q", call.headers.Get("If-Match"))
					}
					return getResponse(`"v2"`, "b-data")
				case syncCollection + "c.txt":
					if call.headers.Get("If-Match") != `"v9"` {
						t.Fatalf("c.txt If-Match = %q", call.headers.Get("If-Match"))
					}
					return getResponse(`"v9"`, "c-data")
				}
			}
			t.Fatalf("unexpected request %d: %s %s", step, call.method, call.uri)
			return nil
		}

		snap, err := cli.CollectSync(context.Background(), "docs", nil, 2)
		mustSnapshot(t, snap, err)

		c.Check(t, snap.Collection == syncCollection, "collection URL should be the trailing-slash request URL")
		c.Check(t, snap.Token == "token-3", "final token should be token-3")
		c.Check(t, len(snap.Resources) == 3, "expected b.txt, c.txt and sub/ only")
		c.Check(t, snap.Resources[syncCollection+"a.txt"].ETag == "" && !snap.Resources[syncCollection+"a.txt"].IsDir,
			"a.txt modified on page 1 then deleted on page 2 must be absent")
		_, aPresent := snap.Resources[syncCollection+"a.txt"]
		c.Check(t, !aPresent, "a.txt key must not exist")
		b := snap.Resources[syncCollection+"b.txt"]
		c.Check(t, !b.IsDir && b.ETag == `"v2"` && string(b.Data) == "b-data",
			"b.txt deleted then recreated must end at v2 content")
		cc := snap.Resources[syncCollection+"c.txt"]
		c.Check(t, cc.ETag == `"v9"` && string(cc.Data) == "c-data",
			"identical duplicate member within one page merges")
		sub := snap.Resources[syncCollection+"sub/"]
		c.Check(t, sub.IsDir && sub.ETag == "" && sub.Data == nil, "directory only records IsDir, no body")

		calls := tr.snapshot()
		reports := reportCalls(calls)
		c.Check(t, len(reports) == 3, "three paged REPORT requests expected")
		c.Check(t, len(getCalls(calls)) == 2, "only final surviving files get GETs; intermediate versions never do")
		c.Check(t, reports[0].body == string(buildSyncReport("", 2)) || strings.Contains(reports[0].body, "<D:sync-token/>"),
			"first REPORT carries an empty sync-token")
		c.Check(t, strings.Contains(reports[0].body, "<D:sync-level>1</D:sync-level>"), "sync-level is 1")
		c.Check(t, strings.Contains(reports[0].body, "<D:getetag/>"), "REPORT requests getetag")
		c.Check(t, strings.Contains(reports[0].body, "<D:resourcetype/>"), "REPORT requests resourcetype")
		c.Check(t, strings.Contains(reports[0].body, "<D:nresults>2</D:nresults>"), "REPORT carries the page limit")
		c.Check(t, strings.Contains(reports[1].body, "token-1") && !strings.Contains(reports[1].body, "<D:sync-token/>"),
			"second REPORT continues with the page-1 token")
		c.Check(t, strings.Contains(reports[2].body, "token-2"), "third REPORT continues with the page-2 token")
		for _, call := range reports {
			c.Check(t, call.headers.Get("Depth") == "0", "REPORT uses Depth:0")
			c.Check(t, strings.Contains(call.headers.Get("Content-Type"), "application/xml"), "REPORT is XML")
			c.Check(t, call.headers.Get(XInhibitRedirect) == "1", "redirects must be inhibited")
			c.Check(t, call.uri == syncCollection, "REPORT always targets the collection URL")
		}
		c.Check(t, tr.allClosed(), "every response body is closed")

		// incremental sync with no changes: token repeats, no GETs.
		prevNoChange := &SyncSnapshot{
			Collection: syncCollection,
			Token:      "token-3",
			Resources: map[string]SyncResource{
				syncCollection + "b.txt": {ETag: `"v2"`, Data: []byte("b-data")},
			},
		}
		cli2, tr2 := newSyncTestClient()
		tr2.handle = func(step int, call syncCall) *http.Response {
			if call.method != "REPORT" || !strings.Contains(call.body, "token-3") {
				t.Fatalf("unexpected request: %s %s", call.method, call.uri)
			}
			return tr2.respond(207, syncMulti("token-3", "ok"))
		}
		snap2, err := cli2.CollectSync(context.Background(), "docs", prevNoChange, 5)
		mustSnapshot(t, snap2, err)
		c.Check(t, snap2.Token == "token-3", "unchanged token on an empty final page is normal")
		c.Check(t, string(snap2.Resources[syncCollection+"b.txt"].Data) == "b-data", "old body retained")
		c.Check(t, len(getCalls(tr2.snapshot())) == 0, "no GETs when nothing changed")
		c.Check(t, tr2.allClosed(), "no-change bodies closed")

		// paging token loop is fatal.
		cli3, tr3 := newSyncTestClient()
		tr3.handle = func(step int, call syncCall) *http.Response {
			return tr3.respond(207, syncMulti("loop-token", "trunc", fileResp("/base/docs/x.txt", `"v1"`)))
		}
		snap3, err := cli3.CollectSync(context.Background(), "docs", nil, 1)
		c.Check(t, err != nil && snap3 == nil, "repeated continuation token must fail")
		c.Check(t, len(reportCalls(tr3.snapshot())) == 2, "loop is detected after the repeated page")
		c.Check(t, tr3.allClosed(), "loop bodies closed")

		// HTTP-level 507 is an error, not a truncation marker.
		cli4, tr4 := newSyncTestClient()
		tr4.handle = func(step int, call syncCall) *http.Response {
			return tr4.respond(http.StatusInsufficientStorage, "storage exhausted")
		}
		snap4, err := cli4.CollectSync(context.Background(), "docs", nil, 1)
		c.Check(t, err != nil && snap4 == nil, "bare HTTP 507 must not start another page")
		c.Check(t, len(tr4.snapshot()) == 1, "HTTP 507 stops immediately")
		c.Check(t, tr4.allClosed(), "507 body closed")

		// redirects are returned, never followed.
		cli5, tr5 := newSyncTestClient()
		tr5.handle = func(step int, call syncCall) *http.Response {
			body := &syncBody{reader: strings.NewReader("")}
			return &http.Response{
				StatusCode: http.StatusFound,
				Header:     http.Header{"Location": []string{"http://elsewhere.invalid/collection/"}},
				Body:       body,
			}
		}
		snap5, err := cli5.CollectSync(context.Background(), "docs", nil, 1)
		c.Check(t, err != nil && snap5 == nil, "redirect response is an error")
		c.Check(t, len(tr5.snapshot()) == 1, "redirect must not be followed")
		c.Check(t, tr5.allClosed(), "redirect body closed")

		// an authenticator asking to redo a redirect must be refused as well.
		cliR2, trR2 := newSyncTestClient()
		cliR2.auth = NewPreemptiveAuth(&redirectAuth{})
		trR2.handle = func(step int, call syncCall) *http.Response {
			return trR2.respond(http.StatusFound, "moved")
		}
		snapR2, err := cliR2.CollectSync(context.Background(), "docs", nil, 1)
		c.Check(t, err != nil && snapR2 == nil, "auth-driven redirect retries must be refused")
		c.Check(t, len(trR2.snapshot()) == 1, "no authentication redirect loop")
		c.Check(t, trR2.allClosed(), "auth redirect body closed")

		// malformed multistatus documents.
		badDocuments := map[string]string{
			"missing token": `<D:multistatus xmlns:D="DAV:"><D:response><D:href>/base/docs/x.txt</D:href></D:response></D:multistatus>`,
			"two tokens":    `<D:multistatus xmlns:D="DAV:"><D:sync-token>t1</D:sync-token><D:sync-token>t2</D:sync-token></D:multistatus>`,
			"truncated xml": `<D:multistatus xmlns:D="DAV:"><D:response><D:href>/base/docs/x.txt</D:href>`,
			"wrong root":    `<D:something xmlns:D="DAV:"></D:something>`,
			"empty token":   `<D:multistatus xmlns:D="DAV:"><D:sync-token>   </D:sync-token></D:multistatus>`,
		}
		for label, doc := range badDocuments {
			cliB, trB := newSyncTestClient()
			trB.handle = func(step int, call syncCall) *http.Response {
				return trB.respond(207, doc)
			}
			snapB, err := cliB.CollectSync(context.Background(), "docs", nil, 1)
			c.Check(t, err != nil && snapB == nil, "malformed document must fail: "+label)
			c.Check(t, trB.allClosed(), "malformed document body closed: "+label)
		}

		// contradictory or incomplete member states.
		badMembers := map[string]string{
			"weak etag":            syncMulti("t", "ok", `<D:response><D:href>/base/docs/w.txt</D:href><D:propstat><D:prop><D:getetag>W/"v1"</D:getetag><D:resourcetype/></D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>`),
			"missing etag":         syncMulti("t", "ok", `<D:response><D:href>/base/docs/n.txt</D:href><D:propstat><D:prop><D:resourcetype/></D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>`),
			"member error status":  syncMulti("t", "ok", `<D:response><D:href>/base/docs/e.txt</D:href><D:status>HTTP/1.1 403 Forbidden</D:status></D:response>`),
			"conflicting dup":      syncMulti("t", "ok", fileResp("/base/docs/d.txt", `"v1"`), fileResp("/base/docs/d.txt", `"v2"`)),
			"conflicting propstat": syncMulti("t", "ok", `<D:response><D:href>/base/docs/p.txt</D:href><D:propstat><D:prop><D:getetag>"v1"</D:getetag><D:resourcetype/></D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat><D:propstat><D:prop><D:getetag>"v2"</D:getetag><D:resourcetype/></D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>`),
		}
		for label, doc := range badMembers {
			cliB, trB := newSyncTestClient()
			trB.handle = func(step int, call syncCall) *http.Response {
				return trB.respond(207, doc)
			}
			snapB, err := cliB.CollectSync(context.Background(), "docs", nil, 1)
			c.Check(t, err != nil && snapB == nil, "bad member state must fail: "+label)
		}

		// href resolution: relative, absolute, escaped segment and %2F.
		cliH, trH := newSyncTestClient()
		trH.handle = func(step int, call syncCall) *http.Response {
			if call.method == "REPORT" {
				return trH.respond(207, syncMulti("th", "ok",
					fileResp("rel.txt", `"1"`),
					fileResp("http://dav.invalid/base/docs/abs.txt", `"1"`),
					fileResp("/base/docs/a%20b.txt", `"1"`),
					fileResp("/base/docs/s%2Fg.txt", `"1"`),
				))
			}
			return getResponse(`"1"`, "z")
		}
		snapH, err := cliH.CollectSync(context.Background(), "docs", nil, 10)
		mustSnapshot(t, snapH, err)
		expectedKeys := []string{
			syncCollection + "rel.txt",
			syncCollection + "abs.txt",
			syncCollection + "a%20b.txt",
			syncCollection + "s%2Fg.txt",
		}
		for _, key := range expectedKeys {
			_, ok := snapH.Resources[key]
			c.Check(t, ok, "href must resolve to key "+key)
		}
		_, decodedSlash := snapH.Resources[syncCollection+"g.txt"]
		c.Check(t, !decodedSlash, "%2F stays an escaped segment and must not become a nested /g.txt")

		// rejected hrefs.
		invalidHrefs := []string{
			"/base/docs/nested/x.txt",
			"/base/other/x.txt",
			"http://other.invalid/base/docs/x.txt",
			"/base/docs/x.txt?query=1",
			"/base/docs/x.txt#fragment",
			"http://user:pass@dav.invalid/base/docs/x.txt",
			"../root.txt",
		}
		for _, href := range invalidHrefs {
			cliX, trX := newSyncTestClient()
			trX.handle = func(step int, call syncCall) *http.Response {
				return trX.respond(207, syncMulti("tx", "ok", fileResp(href, `"1"`)))
			}
			snapX, err := cliX.CollectSync(context.Background(), "docs", nil, 1)
			c.Check(t, err != nil && snapX == nil, "href must be rejected: "+href)
		}
	})
}

// --- TestSyncBodies ---------------------------------------------------------

func previousBodySnapshot() *SyncSnapshot {
	return &SyncSnapshot{
		Collection: syncCollection,
		Token:      "token-9",
		Resources: map[string]SyncResource{
			syncCollection + "keep.txt":   {ETag: `"v1"`, Data: []byte("keep-body")},
			syncCollection + "change.txt": {ETag: `"v1"`, Data: []byte("old-body")},
			syncCollection + "gone.txt":   {ETag: `"v1"`, Data: []byte("gone-body")},
			syncCollection + "keepdir/":   {IsDir: true},
		},
	}
}

func TestSyncBodies(t *testing.T) {
	check.Run(t, func(c *check.C) {
		cli, tr := newSyncTestClient()
		cli.SetHeader("X-Task-Header", "present")
		intercepted := make([]string, 0)
		cli.SetInterceptor(func(method string, rq *http.Request) {
			intercepted = append(intercepted, method)
		})

		tr.handle = func(step int, call syncCall) *http.Response {
			if call.method == "REPORT" {
				return tr.respond(207, syncMulti("token-10", "ok",
					fileResp("/base/docs/keep.txt", `"v1"`),
					fileResp("/base/docs/change.txt", `"v2"`),
					delResp("/base/docs/gone.txt"),
					fileResp("/base/docs/new.txt", `"v3"`),
				))
			}
			switch call.uri {
			case syncCollection + "change.txt":
				return getResponse(`"v2"`, "new-body")
			case syncCollection + "new.txt":
				return getResponse(`"v3"`, "brand-body")
			}
			t.Fatalf("unexpected GET %s", call.uri)
			return nil
		}

		previous := previousBodySnapshot()
		snap, err := cli.CollectSync(context.Background(), "docs", previous, 4)
		mustSnapshot(t, snap, err)

		gets := getCalls(tr.snapshot())
		c.Check(t, len(gets) == 2, "only changed and new files are downloaded")
		c.Check(t, gets[0].uri == syncCollection+"change.txt" && gets[0].headers.Get("If-Match") == `"v2"`,
			"changed file GET carries If-Match of the new ETag")
		c.Check(t, gets[1].uri == syncCollection+"new.txt" && gets[1].headers.Get("If-Match") == `"v3"`,
			"new file GET carries If-Match of its ETag")
		for _, call := range tr.snapshot() {
			c.Check(t, call.headers.Get("X-Task-Header") == "present", "custom headers are reused")
		}
		c.Check(t, containsString(intercepted, "REPORT") && containsString(intercepted, "GET"),
			"interceptor sees REPORT and GET requests")

		keep := snap.Resources[syncCollection+"keep.txt"]
		c.Check(t, string(keep.Data) == "keep-body" && keep.ETag == `"v1"`, "unchanged ETag reuses the old body")
		change := snap.Resources[syncCollection+"change.txt"]
		c.Check(t, string(change.Data) == "new-body" && change.ETag == `"v2"`, "changed file gets the new body")
		newFile := snap.Resources[syncCollection+"new.txt"]
		c.Check(t, string(newFile.Data) == "brand-body" && newFile.ETag == `"v3"`, "new file is downloaded")
		_, gonePresent := snap.Resources[syncCollection+"gone.txt"]
		c.Check(t, !gonePresent, "deleted member is removed")
		c.Check(t, snap.Resources[syncCollection+"keepdir/"].IsDir, "directory survives without a GET")
		c.Check(t, snap.Token == "token-10", "new token returned")

		// the input snapshot is untouched and shares no mutable storage.
		c.Check(t, len(previous.Resources) == 4, "previous map is not modified")
		c.Check(t, previous.Resources[syncCollection+"change.txt"].ETag == `"v1"`, "previous ETag untouched")
		c.Check(t, string(previous.Resources[syncCollection+"gone.txt"].Data) == "gone-body", "previous deleted member untouched")
		keep.Data[0] = 'X'
		c.Check(t, string(previous.Resources[syncCollection+"keep.txt"].Data) == "keep-body",
			"reused body data is a copy, not shared backing storage")

		// input validation happens before any request.
		cliV, trV := newSyncTestClient()
		_, err = cliV.CollectSync(context.Background(), "docs", nil, 0)
		c.Check(t, err != nil, "pageSize must be positive")
		badCollection := previousBodySnapshot()
		badCollection.Collection = "http://dav.invalid/base/other/"
		_, err = cliV.CollectSync(context.Background(), "docs", badCollection, 4)
		c.Check(t, err != nil, "snapshot from another collection is rejected")
		badToken := previousBodySnapshot()
		badToken.Token = ""
		_, err = cliV.CollectSync(context.Background(), "docs", badToken, 4)
		c.Check(t, err != nil, "empty snapshot token is rejected")
		c.Check(t, len(trV.snapshot()) == 0, "validation failures send no request")

		// GET conflicts: no partial snapshot, no retries.
		conflicts := map[string]func(*syncFakeTransport, int, syncCall) *http.Response{
			"etag mismatch": func(_ *syncFakeTransport, _ int, _ syncCall) *http.Response {
				return getResponse(`"v9"`, "sneaky")
			},
			"412 precondition": func(tr *syncFakeTransport, _ int, _ syncCall) *http.Response {
				return tr.respond(http.StatusPreconditionFailed, "precondition failed")
			},
			"404 not found": func(tr *syncFakeTransport, _ int, _ syncCall) *http.Response {
				return tr.respond(http.StatusNotFound, "missing")
			},
			"broken body": func(_ *syncFakeTransport, _ int, _ syncCall) *http.Response {
				header := make(http.Header)
				header.Set("ETag", `"v2"`)
				return &http.Response{
					StatusCode: http.StatusOK,
					Header:     header,
					Body:       &errBody{err: errors.New("connection reset")},
				}
			},
		}
		for label, responder := range conflicts {
			cliC, trC := newSyncTestClient()
			trC.handle = func(step int, call syncCall) *http.Response {
				if call.method == "REPORT" {
					return trC.respond(207, syncMulti("tc", "ok", fileResp("/base/docs/change.txt", `"v2"`)))
				}
				return responder(trC, step, call)
			}
			snapC, err := cliC.CollectSync(context.Background(), "docs", previousBodySnapshot(), 4)
			c.Check(t, err != nil && snapC == nil, "GET failure must abort: "+label)
			c.Check(t, len(getCalls(trC.snapshot())) == 1, "no automatic retry after: "+label)
			c.Check(t, trC.allClosed(), "bodies closed after: "+label)
		}

		// ReadDir/Read control: the legacy GET is not constrained by If-Match.
		cliR, trR := newSyncTestClient()
		trR.handle = func(step int, call syncCall) *http.Response {
			if call.method == "PROPFIND" {
				return trR.respond(207, `<D:multistatus xmlns:D="DAV:">`+
					`<D:response><D:href>/base/docs/</D:href><D:propstat><D:prop><D:resourcetype><D:collection/></D:resourcetype></D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>`+
					`<D:response><D:href>/base/docs/report.txt</D:href><D:propstat><D:prop><D:resourcetype/><D:getetag>"v1"</D:getetag></D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>`+
					`</D:multistatus>`)
			}
			c.Check(t, call.headers.Get("If-Match") == "", "legacy Read must not send If-Match")
			return getResponse(`"v1"`, "old")
		}
		files, err := cliR.ReadDir("docs")
		c.Check(t, err == nil && len(files) == 1, "ReadDir control still works")
		data, err := cliR.Read("docs/report.txt")
		c.Check(t, err == nil && string(data) == "old", "Read control still works")
		c.Check(t, trR.allClosed(), "control bodies closed")
	})
}

// --- TestSyncRecovery -------------------------------------------------------

func TestSyncRecovery(t *testing.T) {
	check.Run(t, func(c *check.C) {
		validSyncTokenError := `<?xml version="1.0" encoding="utf-8"?>` +
			`<D:error xmlns:D="DAV:"><D:valid-sync-token/></D:error>`

		// expired token triggers one full re-collection from the empty token.
		cli, tr := newSyncTestClient()
		tr.handle = func(step int, call syncCall) *http.Response {
			switch {
			case step == 1:
				if !strings.Contains(call.body, "token-9") {
					t.Fatalf("first REPORT must use the old token")
				}
				return tr.respond(http.StatusForbidden, validSyncTokenError)
			case step == 2:
				if !strings.Contains(call.body, "<D:sync-token/>") {
					t.Fatalf("recovery REPORT must restart with an empty token")
				}
				return tr.respond(207, syncMulti("r1", "trunc",
					fileResp("/base/docs/keep.txt", `"v1"`),
					fileResp("/base/docs/change.txt", `"v2"`),
				))
			case step == 3:
				if !strings.Contains(call.body, "r1") {
					t.Fatalf("recovery paging must continue with r1")
				}
				return tr.respond(207, syncMulti("r2", "ok",
					fileResp("/base/docs/new.txt", `"v3"`),
				))
			case call.method == http.MethodGet:
				if call.headers.Get("If-Match") == "" {
					t.Fatal("recovery GET must keep If-Match")
				}
				if call.uri == syncCollection+"change.txt" {
					return getResponse(`"v2"`, "new-body")
				}
				return getResponse(`"v3"`, "brand-body")
			}
			t.Fatalf("unexpected request %d: %s %s", step, call.method, call.uri)
			return nil
		}

		snap, err := cli.CollectSync(context.Background(), "docs", previousBodySnapshot(), 2)
		mustSnapshot(t, snap, err)
		c.Check(t, snap.Token == "r2", "recovery ends on the re-collection token")
		c.Check(t, string(snap.Resources[syncCollection+"keep.txt"].Data) == "keep-body",
			"same URL and ETag reuses the old body without a GET")
		c.Check(t, string(snap.Resources[syncCollection+"change.txt"].Data) == "new-body", "changed body is fetched")
		c.Check(t, string(snap.Resources[syncCollection+"new.txt"].Data) == "brand-body", "new body is fetched")
		_, gone := snap.Resources[syncCollection+"gone.txt"]
		c.Check(t, !gone, "member missing from the full re-collection is removed")
		gets := getCalls(tr.snapshot())
		c.Check(t, len(gets) == 2, "the unchanged kept file needs no GET")
		c.Check(t, len(reportCalls(tr.snapshot())) == 3, "one 403 plus two recovery REPORT pages")
		c.Check(t, tr.allClosed(), "recovery bodies closed")

		// a second invalid-token failure during re-collection is fatal.
		cli2, tr2 := newSyncTestClient()
		tr2.handle = func(step int, call syncCall) *http.Response {
			return tr2.respond(http.StatusForbidden, validSyncTokenError)
		}
		snap2, err := cli2.CollectSync(context.Background(), "docs", previousBodySnapshot(), 2)
		c.Check(t, err != nil && snap2 == nil, "repeated valid-sync-token failure must error")
		c.Check(t, len(reportCalls(tr2.snapshot())) == 2, "only one automatic re-collection attempt")
		c.Check(t, tr2.allClosed(), "double-failure bodies closed")

		// plain 403 responses do not trigger a re-collection.
		plainErrors := map[string]string{
			"other dav error": `<D:error xmlns:D="DAV:"><D:something-else/></D:error>`,
			"non-xml body":    `forbidden`,
			"empty body":      ``,
		}
		for label, payload := range plainErrors {
			cli3, tr3 := newSyncTestClient()
			tr3.handle = func(step int, call syncCall) *http.Response {
				return tr3.respond(http.StatusForbidden, payload)
			}
			snap3, err := cli3.CollectSync(context.Background(), "docs", previousBodySnapshot(), 2)
			c.Check(t, err != nil && snap3 == nil, "ordinary 403 must fail: "+label)
			c.Check(t, len(tr3.snapshot()) == 1, "no re-collection after: "+label)
			c.Check(t, tr3.allClosed(), "403 body closed: "+label)
		}

		// cancellation while reading the second REPORT page.
		cli4, tr4 := newSyncTestClient()
		blockedReport := newBlockingBody()
		tr4.handle = func(step int, call syncCall) *http.Response {
			if step == 1 {
				return tr4.respond(207, syncMulti("c1", "trunc", fileResp("/base/docs/a.txt", `"v1"`)))
			}
			return &http.Response{StatusCode: http.StatusMultiStatus, Header: make(http.Header), Body: blockedReport}
		}
		ctx4, cancel4 := context.WithCancel(context.Background())
		go func() {
			<-blockedReport.started
			cancel4()
			_ = blockedReport.Close()
		}()
		snap4, err := cli4.CollectSync(ctx4, "docs", nil, 1)
		c.Check(t, err != nil && snap4 == nil, "cancelled REPORT read must fail")
		c.Check(t, len(tr4.snapshot()) == 2, "cancellation stops before the next page or any GET")
		c.Check(t, tr4.allClosed(), "cancelled REPORT bodies closed")

		// cancellation while reading a GET body.
		cli5, tr5 := newSyncTestClient()
		blockedGet := newBlockingBody()
		tr5.handle = func(step int, call syncCall) *http.Response {
			if call.method == "REPORT" {
				return tr5.respond(207, syncMulti("g1", "ok", fileResp("/base/docs/a.txt", `"v1"`)))
			}
			return &http.Response{
				StatusCode: http.StatusOK,
				Header:     etagHeader(`"v1"`),
				Body:       blockedGet,
			}
		}
		ctx5, cancel5 := context.WithCancel(context.Background())
		go func() {
			<-blockedGet.started
			cancel5()
			_ = blockedGet.Close()
		}()
		snap5, err := cli5.CollectSync(ctx5, "docs", nil, 1)
		c.Check(t, err != nil && snap5 == nil, "cancelled GET read must fail")
		c.Check(t, len(getCalls(tr5.snapshot())) == 1, "cancellation stops before the next file")
		c.Check(t, tr5.allClosed(), "cancelled GET body closed")

		// pre-cancelled context fails before the first request.
		cli6, tr6 := newSyncTestClient()
		ctx6, cancel6 := context.WithCancel(context.Background())
		cancel6()
		snap6, err := cli6.CollectSync(ctx6, "docs", nil, 1)
		c.Check(t, err != nil && snap6 == nil, "pre-cancelled context is rejected")
		c.Check(t, len(tr6.snapshot()) == 0, "no request with an already cancelled context")
	})
}

func containsString(items []string, want string) bool {
	for _, item := range items {
		if item == want {
			return true
		}
	}
	return false
}

// redirectAuth mimics an authenticator that negotiates on redirects.
type redirectAuth struct{}

func (*redirectAuth) Authorize(*http.Client, *http.Request, string) error { return nil }
func (*redirectAuth) Verify(_ *http.Client, rs *http.Response, _ string) (bool, error) {
	return rs.StatusCode == http.StatusFound, nil
}
func (*redirectAuth) Close() error         { return nil }
func (*redirectAuth) Clone() Authenticator { return &redirectAuth{} }
