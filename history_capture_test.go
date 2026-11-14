package rss

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"slices"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/SlyMarbo/rss/v2/atom1"
	"github.com/SlyMarbo/rss/v2/task_support/taskcheck"
)

// The tests in this file use an in-memory HTTP server instead of
// httptest, so that they do not depend on the ability to bind a
// local socket.

type historyTestAddr string

func (a historyTestAddr) Network() string { return "history-test" }
func (a historyTestAddr) String() string  { return string(a) }

type historyTestListener struct {
	accepted chan net.Conn
	closed   chan struct{}
	once     sync.Once
}

func newHistoryTestListener() *historyTestListener {
	return &historyTestListener{
		accepted: make(chan net.Conn),
		closed:   make(chan struct{}),
	}
}

func (l *historyTestListener) Accept() (net.Conn, error) {
	select {
	case conn := <-l.accepted:
		return conn, nil
	case <-l.closed:
		return nil, net.ErrClosed
	}
}

func (l *historyTestListener) Close() error {
	l.once.Do(func() { close(l.closed) })
	return nil
}

func (l *historyTestListener) Addr() net.Addr { return historyTestAddr("history.test") }

func (l *historyTestListener) dial(ctx context.Context) (net.Conn, error) {
	client, server := net.Pipe()
	select {
	case l.accepted <- server:
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

// historyTestServer records every request and serves the configured
// routes. Routes are keyed by URL path.
type historyTestServer struct {
	t *testing.T

	mu          sync.Mutex
	routes      map[string]http.HandlerFunc
	requests    []string
	conditional []string
	closeErrors map[string]bool

	listener *historyTestListener
	server   *http.Server
	client   *http.Client
	base     string
}

func newHistoryTestServer(t *testing.T) *historyTestServer {
	t.Helper()

	s := &historyTestServer{
		t:           t,
		routes:      make(map[string]http.HandlerFunc),
		closeErrors: make(map[string]bool),
		listener:    newHistoryTestListener(),
		base:        "http://history.test",
	}
	s.server = &http.Server{Handler: s}
	transport := &http.Transport{
		DisableKeepAlives: true,
		DialContext: func(ctx context.Context, _, _ string) (net.Conn, error) {
			return s.listener.dial(ctx)
		},
	}
	s.client = &http.Client{
		Transport: &historyTestTransport{base: transport, server: s},
		Timeout:   10 * time.Second,
	}

	go func() { _ = s.server.Serve(s.listener) }()
	t.Cleanup(s.close)

	return s
}

func (s *historyTestServer) close() {
	s.client.CloseIdleConnections()
	_ = s.server.Close()
	_ = s.listener.Close()
}

func (s *historyTestServer) URL(path string) string { return s.base + path }

func (s *historyTestServer) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	s.mu.Lock()
	s.requests = append(s.requests, r.Method+" "+r.URL.RequestURI())
	if r.Header.Get("If-None-Match") != "" || r.Header.Get("If-Modified-Since") != "" || r.Header.Get("If-Range") != "" {
		s.conditional = append(s.conditional, r.Method+" "+r.URL.RequestURI())
	}
	handler := s.routes[r.URL.Path]
	s.mu.Unlock()

	if handler == nil {
		http.NotFound(w, r)
		return
	}
	handler(w, r)
}

func (s *historyTestServer) handle(path string, handler http.HandlerFunc) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.routes[path] = handler
}

func (s *historyTestServer) set(path, body string) {
	s.handle(path, func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/atom+xml")
		_, _ = io.WriteString(w, body)
	})
}

func (s *historyTestServer) status(path string, code int) {
	s.handle(path, func(w http.ResponseWriter, _ *http.Request) {
		http.Error(w, http.StatusText(code), code)
	})
}

func (s *historyTestServer) redirect(path, location string, code int) {
	s.handle(path, func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, location, code)
	})
}

func (s *historyTestServer) truncated(path, body string) {
	s.handle(path, func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/atom+xml")
		w.Header().Set("Content-Length", strconv.Itoa(len(body)+64))
		_, _ = io.WriteString(w, body)
	})
}

func (s *historyTestServer) failClose(path string, fail bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if fail {
		s.closeErrors[path] = true
		return
	}
	delete(s.closeErrors, path)
}

func (s *historyTestServer) reset() {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.requests = nil
	s.conditional = nil
}

func (s *historyTestServer) requestLog() []string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return slices.Clone(s.requests)
}

func (s *historyTestServer) conditionalLog() []string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return slices.Clone(s.conditional)
}

func (s *historyTestServer) reader() *Reader {
	s.t.Helper()

	reader, err := NewReader(
		WithHTTPClient(s.client),
		WithNow(func() time.Time {
			return time.Date(2026, time.November, 14, 12, 0, 0, 0, time.UTC)
		}),
	)
	if err != nil {
		s.t.Fatalf("failed to construct reader: %v", err)
	}
	return reader
}

type historyTestTransport struct {
	base   http.RoundTripper
	server *historyTestServer
}

func (t *historyTestTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	res, err := t.base.RoundTrip(req)
	if err != nil {
		return nil, err
	}

	t.server.mu.Lock()
	fail := t.server.closeErrors[req.URL.Path]
	t.server.mu.Unlock()

	if fail {
		res.Body = historyTestCloseError{ReadCloser: res.Body}
	}
	return res, nil
}

type historyTestCloseError struct {
	io.ReadCloser
}

func (b historyTestCloseError) Close() error {
	_ = b.ReadCloser.Close()
	return errors.New("simulated close failure")
}

func atomDocument(attrs, content string) string {
	return `<?xml version="1.0" encoding="utf-8"?>` +
		`<feed xmlns="http://www.w3.org/2005/Atom" xmlns:fh="http://purl.org/syndication/history/1.0"` + attrs + `>` +
		content +
		`</feed>`
}

func atomEntry(id, title, updated string) string {
	return fmt.Sprintf(`<entry><id>%s</id><title>%s</title><updated>%s</updated></entry>`, id, title, updated)
}

func itemIDs(feed *Feed) []string {
	if feed == nil {
		return nil
	}
	ids := make([]string, len(feed.Items))
	for i, item := range feed.Items {
		ids[i] = item.ID
	}
	return ids
}

func itemTitles(feed *Feed) []string {
	if feed == nil {
		return nil
	}
	titles := make([]string, len(feed.Items))
	for i, item := range feed.Items {
		titles[i] = item.Title
	}
	return titles
}

func itemReads(feed *Feed) []bool {
	if feed == nil {
		return nil
	}
	reads := make([]bool, len(feed.Items))
	for i, item := range feed.Items {
		reads[i] = item.Read
	}
	return reads
}

func findItem(feed *Feed, id string) *Item {
	if feed == nil {
		return nil
	}
	for _, item := range feed.Items {
		if item.ID == id {
			return item
		}
	}
	return nil
}

type historySnapshot struct {
	documents []string
	ids       []string
	titles    []string
	reads     []bool
	unread    uint32
}

func snapshotHistory(history *History) historySnapshot {
	if history == nil || history.Feed == nil {
		return historySnapshot{}
	}
	return historySnapshot{
		documents: slices.Clone(history.Documents),
		ids:       itemIDs(history.Feed),
		titles:    itemTitles(history.Feed),
		reads:     itemReads(history.Feed),
		unread:    history.Feed.Unread,
	}
}

func (s historySnapshot) equal(other historySnapshot) bool {
	return slices.Equal(s.documents, other.documents) &&
		slices.Equal(s.ids, other.ids) &&
		slices.Equal(s.titles, other.titles) &&
		slices.Equal(s.reads, other.reads) &&
		s.unread == other.unread
}

func checkHistoryItems(c *taskcheck.C, history *History, wantIDs, wantTitles []string) {
	if history == nil || history.Feed == nil {
		c.Check(false, "history or feed is nil")
		return
	}
	c.Check(slices.Equal(itemIDs(history.Feed), wantIDs), "unexpected item IDs: got %v, want %v", itemIDs(history.Feed), wantIDs)
	c.Check(slices.Equal(itemTitles(history.Feed), wantTitles), "unexpected item titles: got %v, want %v", itemTitles(history.Feed), wantTitles)
}

// TestHistoryArchive covers full captures: redirects, relative
// bases, archive metadata, and the failure modes of a chain.
func TestHistoryArchive(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		t.Run("full chain", func(t *testing.T) {
			srv := newHistoryTestServer(t)
			reader := srv.reader()

			current := atomDocument(` xml:base="archives/"`, `
 <title>Entry bulletin</title>
 <id>urn:entry</id>
 <updated>2026-11-14T00:00:00Z</updated>
 <link rel="prev-archive" href="2026-11.atom" xml:base="older/"/>
 <link rel="next" href="/next"/>
 <link rel="next-archive" href="/next-archive"/>
 <link rel="current" href="/current"/>
 <x:link xmlns:x="urn:example:other" rel="prev-archive" href="/other"/>
 <entry>
  <id>urn:recent</id><title>Recent record</title><updated>2026-11-14T00:00:00Z</updated>
  <link rel="prev-archive" href="/evil"/>
 </entry>
 `+atomEntry("urn:shared", "Shared current", "2026-11-13T00:00:00Z"))

			november := atomDocument("", `
 <title>November archive</title>
 <id>urn:november</id>
 <updated>2026-11-01T00:00:00Z</updated>
 <fh:archive/>
 <link rel="prev-archive" href="/feeds/archives/2026-10.atom"/>
 <link rel="next-archive" href="/feeds/archives/2026-12.atom"/>
 `+atomEntry("urn:shared", "Shared November", "2030-01-01T00:00:00Z")+
				atomEntry("urn:mid", "Mid record", "2026-10-20T00:00:00Z"))

			october := atomDocument("", `
 <title>October archive</title>
 <id>urn:october</id>
 <updated>2026-10-01T00:00:00Z</updated>
 <fh:archive/>
 `+atomEntry("urn:old", "Old record", "2026-09-20T00:00:00Z")+
				atomEntry("urn:shared", "Shared October", "2000-01-01T00:00:00Z"))

			srv.redirect("/current", "/feeds/current.atom#latest", http.StatusFound)
			srv.set("/feeds/current.atom", current)
			srv.set("/feeds/archives/older/2026-11.atom", november)
			srv.set("/feeds/archives/2026-10.atom", october)

			history, err := reader.CaptureHistory(context.Background(), srv.URL("/current"), nil, "full")
			if err != nil {
				t.Fatalf("CaptureHistory failed: %v", err)
			}

			checkHistoryItems(c, history,
				[]string{"urn:recent", "urn:shared", "urn:mid", "urn:old"},
				[]string{"Recent record", "Shared current", "Mid record", "Old record"})
			c.Check(history.Feed.Title == "Entry bulletin", "unexpected feed title %q", history.Feed.Title)
			c.Check(history.Feed.UpdateURL == srv.URL("/feeds/current.atom"), "unexpected update URL %q", history.Feed.UpdateURL)
			c.Check(history.Feed.Unread == 4, "unexpected unread count %d", history.Feed.Unread)

			wantDocuments := []string{
				srv.URL("/feeds/current.atom"),
				srv.URL("/feeds/archives/older/2026-11.atom"),
				srv.URL("/feeds/archives/2026-10.atom"),
			}
			c.Check(slices.Equal(history.Documents, wantDocuments), "unexpected documents: got %v, want %v", history.Documents, wantDocuments)

			wantRequests := []string{
				"GET /current",
				"GET /feeds/current.atom",
				"GET /feeds/archives/older/2026-11.atom",
				"GET /feeds/archives/2026-10.atom",
			}
			c.Check(slices.Equal(srv.requestLog(), wantRequests), "unexpected requests: got %v, want %v", srv.requestLog(), wantRequests)
			c.Check(len(srv.conditionalLog()) == 0, "unexpected conditional requests: %v", srv.conditionalLog())

			// The parser extensions are visible on their own.
			raw, err := atom1.Parse(strings.NewReader(current))
			c.Check(err == nil, "failed to parse current document: %v", err)
			if err == nil {
				c.Check(raw.Updated == "2026-11-14T00:00:00Z", "raw feed updated = %q", raw.Updated)
				c.Check(raw.XMLBase == "archives/", "raw feed xml:base = %q", raw.XMLBase)
				c.Check(raw.Archive == nil, "entry document must not be marked as an archive")
				c.Check(len(raw.Link) > 0 && raw.Link[0].XMLBase == "older/", "raw link xml:base missing")
			}
			rawArchive, err := atom1.Parse(strings.NewReader(november))
			c.Check(err == nil, "failed to parse archive document: %v", err)
			if err == nil {
				c.Check(rawArchive.Archive != nil, "archive marker missing from raw archive document")
			}
		})

		t.Run("default fetch unchanged", func(t *testing.T) {
			srv := newHistoryTestServer(t)
			reader := srv.reader()
			srv.set("/current", atomDocument("", `
 <title>Entry bulletin</title>
 <id>urn:entry</id>
 <updated>2026-11-14T00:00:00Z</updated>
 <link rel="prev-archive" href="/archive"/>
 `+atomEntry("urn:recent", "Recent record", "2026-11-14T00:00:00Z")))

			feed, err := reader.Fetch(context.Background(), srv.URL("/current"))
			c.Check(err == nil, "Fetch failed: %v", err)
			c.Check(len(feed.Items) == 1 && feed.Items[0].ID == "urn:recent", "ordinary Fetch changed: %v", itemIDs(feed))
			c.Check(slices.Equal(srv.requestLog(), []string{"GET /current"}), "Fetch followed links: %v", srv.requestLog())
		})

		t.Run("only atom feed links are followed", func(t *testing.T) {
			srv := newHistoryTestServer(t)
			reader := srv.reader()
			srv.set("/entry", atomDocument("", `
 <title>Entry</title>
 <id>urn:entry</id>
 <updated>2026-11-14T00:00:00Z</updated>
 <x:link xmlns:x="urn:example:other" rel="prev-archive" href="/other"/>
 <entry><id>urn:one</id><title>One</title><updated>2026-11-14T00:00:00Z</updated>
  <link rel="prev-archive" href="/evil"/>
 </entry>`))
			srv.set("/other", atomDocument("", "<title>Other</title><id>urn:other</id><updated>2026-11-01T00:00:00Z</updated>"))

			history, err := reader.CaptureHistory(context.Background(), srv.URL("/entry"), nil, "full")
			c.Check(err == nil, "CaptureHistory failed: %v", err)
			if err == nil {
				checkHistoryItems(c, history, []string{"urn:one"}, []string{"One"})
				c.Check(slices.Equal(history.Documents, []string{srv.URL("/entry")}), "unexpected documents: %v", history.Documents)
			}
			c.Check(slices.Equal(srv.requestLog(), []string{"GET /entry"}), "unexpected requests: %v", srv.requestLog())
		})

		t.Run("validation before requests", func(t *testing.T) {
			srv := newHistoryTestServer(t)
			reader := srv.reader()
			srv.set("/entry", atomDocument("", "<title>Entry</title><id>urn:entry</id><updated>2026-11-14T00:00:00Z</updated>"))
			srv.set("/other", atomDocument("", "<title>Other</title><id>urn:other</id><updated>2026-11-14T00:00:00Z</updated>"))

			var nilContext context.Context
			_, err := reader.CaptureHistory(nilContext, srv.URL("/entry"), nil, "full")
			c.Check(err != nil, "nil context was accepted")
			c.Check(len(srv.requestLog()) == 0, "nil context made requests: %v", srv.requestLog())

			for _, mode := range []string{"", "FULL", "lazy", "full "} {
				_, err = reader.CaptureHistory(context.Background(), srv.URL("/entry"), nil, mode)
				c.Check(err != nil, "invalid mode %q was accepted", mode)
			}
			for _, start := range []string{"", "/relative", "ftp://history.test/entry", "http://", "://bad"} {
				_, err = reader.CaptureHistory(context.Background(), start, nil, "full")
				c.Check(err != nil, "invalid entry %q was accepted", start)
			}
			c.Check(len(srv.requestLog()) == 0, "validation made requests: %v", srv.requestLog())

			history, err := reader.CaptureHistory(context.Background(), srv.URL("/entry"), nil, "full")
			c.Check(err == nil, "CaptureHistory failed: %v", err)
			if err != nil {
				return
			}

			srv.reset()
			_, err = reader.CaptureHistory(context.Background(), srv.URL("/other"), history, "incremental")
			c.Check(err != nil, "previous history from another entry was accepted")
			c.Check(len(srv.requestLog()) == 0, "mismatched previous made requests: %v", srv.requestLog())

			// Fragments are not part of the entry identity.
			srv.reset()
			again, err := reader.CaptureHistory(context.Background(), srv.URL("/entry")+"#section", history, "incremental")
			c.Check(err == nil, "entry fragment rejected: %v", err)
			if err == nil {
				c.Check(slices.Equal(again.Documents, []string{srv.URL("/entry")}), "unexpected documents: %v", again.Documents)
			}
		})

		t.Run("chain errors", func(t *testing.T) {
			srv := newHistoryTestServer(t)
			reader := srv.reader()
			ctx := context.Background()

			archive := func(id, updated, prev string) string {
				link := ""
				if prev != "" {
					link = fmt.Sprintf(`<link rel="prev-archive" href="%s"/>`, prev)
				}
				return atomDocument("", fmt.Sprintf(`<title>%s</title><id>%s</id><updated>%s</updated><fh:archive/>%s`, id, id, updated, link))
			}

			// Unsupported scheme in a chain address.
			srv.set("/unsupported", atomDocument("", `<title>Entry</title><id>urn:e</id><updated>2026-11-14T00:00:00Z</updated><link rel="prev-archive" href="ftp://history.test/archive"/>`))
			srv.reset()
			_, err := reader.CaptureHistory(ctx, srv.URL("/unsupported"), nil, "full")
			c.Check(err != nil, "unsupported archive scheme was accepted")

			// A chain document without the archive marker.
			srv.set("/no-marker", atomDocument("", `<title>Entry</title><id>urn:e</id><updated>2026-11-14T00:00:00Z</updated><link rel="prev-archive" href="/plain"/>`))
			srv.set("/plain", atomDocument("", `<title>Plain</title><id>urn:p</id><updated>2026-11-01T00:00:00Z</updated>`))
			_, err = reader.CaptureHistory(ctx, srv.URL("/no-marker"), nil, "full")
			c.Check(err != nil, "archive without fh:archive was accepted")

			// An fh:archive declaration inside an entry does not mark
			// the document as an archive.
			srv.set("/entry-marker", atomDocument("", `<title>Entry</title><id>urn:e</id><updated>2026-11-14T00:00:00Z</updated><link rel="prev-archive" href="/entry-marker/a"/>`))
			srv.set("/entry-marker/a", atomDocument("", `<title>A</title><id>urn:a</id><updated>2026-11-01T00:00:00Z</updated>
 <entry><id>urn:x</id><title>X</title><updated>2026-10-01T00:00:00Z</updated><fh:archive/></entry>`))
			_, err = reader.CaptureHistory(ctx, srv.URL("/entry-marker"), nil, "full")
			c.Check(err != nil, "entry-level fh:archive was treated as a feed archive")

			// A document has at most one prev-archive link; the first
			// one in document order is followed.
			srv.set("/two-links", atomDocument("", `<title>Entry</title><id>urn:e</id><updated>2026-11-14T00:00:00Z</updated>
 <link rel="prev-archive" href="/two-links/a"/><link rel="prev-archive" href="/two-links/b"/>`))
			srv.set("/two-links/a", archive("twoa", "2026-11-01T00:00:00Z", ""))
			srv.set("/two-links/b", archive("twob", "2026-10-01T00:00:00Z", ""))
			srv.reset()
			twoLinks, err := reader.CaptureHistory(ctx, srv.URL("/two-links"), nil, "full")
			c.Check(err == nil, "two prev-archive links rejected: %v", err)
			if err == nil {
				c.Check(slices.Equal(twoLinks.Documents, []string{srv.URL("/two-links"), srv.URL("/two-links/a")}), "unexpected documents with two links: %v", twoLinks.Documents)
			}
			c.Check(slices.Equal(srv.requestLog(), []string{"GET /two-links", "GET /two-links/a"}), "unexpected requests for two links: %v", srv.requestLog())

			// A duplicate request address.
			srv.set("/dup", archive("dup", "2026-11-01T00:00:00Z", "/dup/x"))
			srv.set("/dup/x", archive("dupx", "2026-10-01T00:00:00Z", "/dup/x"))
			_, err = reader.CaptureHistory(ctx, srv.URL("/dup"), nil, "full")
			c.Check(err != nil, "duplicate request address was accepted")

			// A cycle in the final response addresses.
			srv.set("/cycle/a", archive("cyclea", "2026-11-01T00:00:00Z", "/cycle/b"))
			srv.redirect("/cycle/b", "/cycle/a", http.StatusFound)
			_, err = reader.CaptureHistory(ctx, srv.URL("/cycle/a"), nil, "full")
			c.Check(err != nil, "final address cycle was accepted")

			// Invalid feed-level updated times.
			for name, updated := range map[string]string{
				"missing":   "",
				"no-zone":   "2026-11-14T00:00:00",
				"date-only": "2026-11-14",
				"nonsense":  "yesterday",
			} {
				srv.set("/bad-time/"+name, atomDocument("", fmt.Sprintf(`<title>Entry</title><id>urn:e</id><updated>%s</updated>`, updated)))
				_, err = reader.CaptureHistory(ctx, srv.URL("/bad-time/"+name), nil, "full")
				c.Check(err != nil, "invalid updated time %q was accepted", updated)
			}

			// An invalid updated time deep in the chain.
			srv.set("/bad-chain", atomDocument("", `<title>Entry</title><id>urn:e</id><updated>2026-11-14T00:00:00Z</updated><link rel="prev-archive" href="/bad-chain/archive"/>`))
			srv.set("/bad-chain/archive", atomDocument("", `<title>A</title><id>urn:a</id><updated>2026-11-01T00:00:00</updated><fh:archive/>`))
			_, err = reader.CaptureHistory(ctx, srv.URL("/bad-chain"), nil, "full")
			c.Check(err != nil, "invalid archive updated time was accepted")

			// HTTP failures.
			srv.set("/status", atomDocument("", `<title>Entry</title><id>urn:e</id><updated>2026-11-14T00:00:00Z</updated><link rel="prev-archive" href="/status/archive"/>`))
			srv.status("/status/archive", http.StatusServiceUnavailable)
			_, err = reader.CaptureHistory(ctx, srv.URL("/status"), nil, "full")
			c.Check(err != nil, "non-200 archive was accepted")

			// Read and close failures.
			body := archive("truncated", "2026-11-01T00:00:00Z", "")
			srv.set("/read-error/entry", atomDocument("", `<title>Entry</title><id>urn:e</id><updated>2026-11-14T00:00:00Z</updated><link rel="prev-archive" href="/read-error/archive"/>`))
			srv.truncated("/read-error/archive", body)
			_, err = reader.CaptureHistory(ctx, srv.URL("/read-error/entry"), nil, "full")
			c.Check(err != nil, "truncated archive was accepted")

			srv.set("/close-error/entry", atomDocument("", `<title>Entry</title><id>urn:e</id><updated>2026-11-14T00:00:00Z</updated><link rel="prev-archive" href="/close-error/archive"/>`))
			srv.set("/close-error/archive", archive("close", "2026-11-01T00:00:00Z", ""))
			srv.failClose("/close-error/archive", true)
			_, err = reader.CaptureHistory(ctx, srv.URL("/close-error/entry"), nil, "full")
			c.Check(err != nil, "archive with a failing Close was accepted")
			srv.failClose("/close-error/archive", false)
		})

		t.Run("failed capture leaves no partial result", func(t *testing.T) {
			srv := newHistoryTestServer(t)
			reader := srv.reader()
			ctx := context.Background()

			srv.set("/entry", atomDocument("", `<title>Entry</title><id>urn:e</id><updated>2026-11-14T00:00:00Z</updated><link rel="prev-archive" href="/entry/1"/>`))
			srv.set("/entry/1", atomDocument("", `<title>One</title><id>urn:one</id><updated>2026-11-01T00:00:00Z</updated><fh:archive/><link rel="prev-archive" href="/entry/2"/>`+atomEntry("urn:a", "A", "2026-10-20T00:00:00Z")))
			srv.status("/entry/2", http.StatusInternalServerError)

			history, err := reader.CaptureHistory(ctx, srv.URL("/entry"), nil, "full")
			c.Check(err != nil, "capture with a failing older archive succeeded")
			c.Check(history == nil, "failed capture returned a history")

			reader.historyMu.Lock()
			_, cached := reader.history[srv.URL("/entry")]
			reader.historyMu.Unlock()
			c.Check(!cached, "failed capture populated the archive cache")

			// A later capture starts from scratch and succeeds.
			srv.set("/entry/2", atomDocument("", `<title>Two</title><id>urn:two</id><updated>2026-09-01T00:00:00Z</updated><fh:archive/>`+atomEntry("urn:b", "B", "2026-08-20T00:00:00Z")))
			srv.reset()

			history, err = reader.CaptureHistory(ctx, srv.URL("/entry"), nil, "incremental")
			c.Check(err == nil, "retry failed: %v", err)
			if err == nil {
				checkHistoryItems(c, history,
					[]string{"urn:a", "urn:b"},
					[]string{"A", "B"})
				c.Check(slices.Equal(history.Documents, []string{srv.URL("/entry"), srv.URL("/entry/1"), srv.URL("/entry/2")}), "unexpected documents: %v", history.Documents)
			}
			c.Check(slices.Equal(srv.requestLog(), []string{"GET /entry", "GET /entry/1", "GET /entry/2"}), "unexpected retry requests: %v", srv.requestLog())
		})
	})
}

// TestHistoryIncremental covers reuse of cached archives, read state,
// mutation isolation, detached cached pages, and failures.
func TestHistoryIncremental(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		srv := newHistoryTestServer(t)
		reader := srv.reader()
		ctx := context.Background()
		entryURL := srv.URL("/incremental/entry")

		entry := func(updated, prev string, entries ...string) string {
			return atomDocument("", fmt.Sprintf(`<title>Incremental bulletin</title><id>urn:entry</id><updated>%s</updated><link rel="prev-archive" href="%s"/>`, updated, prev)+strings.Join(entries, ""))
		}
		archive := func(id, updated, prev string, entries ...string) string {
			link := ""
			if prev != "" {
				link = fmt.Sprintf(`<link rel="prev-archive" href="%s"/>`, prev)
			}
			return atomDocument("", fmt.Sprintf(`<title>%s</title><id>%s</id><updated>%s</updated><fh:archive/>%s`, id, id, updated, link)+strings.Join(entries, ""))
		}

		srv.set("/incremental/entry", entry("2026-11-14T00:00:00Z", "/incremental/2",
			atomEntry("urn:recent", "Recent record", "2026-11-14T00:00:00Z")))
		srv.set("/incremental/2", archive("urn:archive2", "2026-11-01T00:00:00Z", "/incremental/1",
			atomEntry("urn:shared", "Shared archive", "2026-10-20T00:00:00Z")))
		srv.set("/incremental/1", archive("urn:archive1", "2026-10-01T00:00:00Z", "",
			atomEntry("urn:old", "Old record", "2026-09-20T00:00:00Z")))

		first, err := reader.CaptureHistory(ctx, entryURL, nil, "full")
		if err != nil {
			t.Fatalf("initial capture failed: %v", err)
		}
		checkHistoryItems(c, first,
			[]string{"urn:recent", "urn:shared", "urn:old"},
			[]string{"Recent record", "Shared archive", "Old record"})
		c.Check(slices.Equal(first.Documents, []string{entryURL, srv.URL("/incremental/2"), srv.URL("/incremental/1")}), "unexpected documents: %v", first.Documents)
		c.Check(first.Feed.Unread == 3, "unexpected unread count %d", first.Feed.Unread)

		// A full capture ignores the cache and crawls the whole chain.
		srv.reset()
		fullAgain, err := reader.CaptureHistory(ctx, entryURL, first, "full")
		c.Check(err == nil, "full re-crawl failed: %v", err)
		if err == nil {
			checkHistoryItems(c, fullAgain,
				[]string{"urn:recent", "urn:shared", "urn:old"},
				[]string{"Recent record", "Shared archive", "Old record"})
			c.Check(slices.Equal(fullAgain.Documents, first.Documents), "full re-crawl changed documents: %v", fullAgain.Documents)
		}
		c.Check(slices.Equal(srv.requestLog(), []string{"GET /incremental/entry", "GET /incremental/2", "GET /incremental/1"}), "full re-crawl did not refetch the chain: %v", srv.requestLog())

		oldItem := findItem(fullAgain.Feed, "urn:old")
		c.Check(oldItem != nil, "old item missing from full re-crawl")
		if oldItem != nil {
			oldItem.Read = true
		}

		// Add a new archive at the head of the chain.
		srv.set("/incremental/entry", entry("2026-11-20T00:00:00Z", "/incremental/3",
			atomEntry("urn:recent2", "Newer record", "2026-11-20T00:00:00Z"),
			atomEntry("urn:recent", "Recent record", "2026-11-14T00:00:00Z")))
		srv.set("/incremental/3", archive("urn:archive3", "2026-11-18T00:00:00Z", "/incremental/2",
			atomEntry("urn:new3", "New 3", "2026-11-18T00:00:00Z")))

		srv.reset()
		second, err := reader.CaptureHistory(ctx, entryURL, fullAgain, "incremental")
		if err != nil {
			t.Fatalf("incremental capture failed: %v", err)
		}
		checkHistoryItems(c, second,
			[]string{"urn:recent2", "urn:recent", "urn:new3", "urn:shared", "urn:old"},
			[]string{"Newer record", "Recent record", "New 3", "Shared archive", "Old record"})
		c.Check(slices.Equal(second.Documents, []string{
			entryURL,
			srv.URL("/incremental/3"),
			srv.URL("/incremental/2"),
			srv.URL("/incremental/1"),
		}), "unexpected documents: %v", second.Documents)
		c.Check(slices.Equal(srv.requestLog(), []string{"GET /incremental/entry", "GET /incremental/3"}), "incremental capture did not reuse cached archives: %v", srv.requestLog())
		c.Check(len(srv.conditionalLog()) == 0, "unexpected conditional requests: %v", srv.conditionalLog())

		oldItem = findItem(second.Feed, "urn:old")
		c.Check(oldItem != nil && oldItem.Read, "read state was not preserved")
		c.Check(second.Feed.Unread == 4, "unexpected unread count %d", second.Feed.Unread)
		for _, id := range []string{"urn:recent2", "urn:recent", "urn:new3", "urn:shared"} {
			item := findItem(second.Feed, id)
			c.Check(item != nil && !item.Read, "item %q should be unread", id)
		}

		// Mutating the returned result must not affect cached content.
		if item := findItem(second.Feed, "urn:new3"); item != nil {
			item.Title = "Mutated title"
			item.Read = true
		}

		// The chain moves: cached archives 2 and 1 are now detached and
		// must not contribute to the result.
		srv.set("/incremental/entry", entry("2026-12-01T00:00:00Z", "/incremental/5",
			atomEntry("urn:recent4", "December record", "2026-12-01T00:00:00Z")))
		srv.set("/incremental/5", archive("urn:archive5", "2026-11-30T00:00:00Z", "/incremental/6",
			atomEntry("urn:new5", "New 5", "2026-11-30T00:00:00Z")))
		srv.set("/incremental/6", archive("urn:archive6", "2026-11-25T00:00:00Z", "",
			atomEntry("urn:far", "Far record", "2026-11-25T00:00:00Z")))

		srv.reset()
		third, err := reader.CaptureHistory(ctx, entryURL, second, "incremental")
		if err != nil {
			t.Fatalf("third capture failed: %v", err)
		}
		checkHistoryItems(c, third,
			[]string{"urn:recent4", "urn:new5", "urn:far"},
			[]string{"December record", "New 5", "Far record"})
		c.Check(slices.Equal(third.Documents, []string{
			entryURL,
			srv.URL("/incremental/5"),
			srv.URL("/incremental/6"),
		}), "detached cached archives leaked into documents: %v", third.Documents)
		c.Check(slices.Equal(srv.requestLog(), []string{"GET /incremental/entry", "GET /incremental/5", "GET /incremental/6"}), "unexpected requests: %v", srv.requestLog())

		// Mark read and mutate an item that came from a document that
		// will be reused from the cache in the next capture.
		if item := findItem(third.Feed, "urn:new5"); item != nil {
			item.Title = "Mutated five"
			item.Read = true
		}
		before := snapshotHistory(third)

		// A failing new archive leaves the previous result untouched.
		srv.set("/incremental/entry", entry("2026-12-05T00:00:00Z", "/incremental/7",
			atomEntry("urn:recent5", "Fifth record", "2026-12-05T00:00:00Z")))
		srv.status("/incremental/7", http.StatusBadGateway)

		srv.reset()
		failed, err := reader.CaptureHistory(ctx, entryURL, third, "incremental")
		c.Check(err != nil, "capture with a failing new archive succeeded")
		c.Check(failed == nil, "failed capture returned a history")
		c.Check(snapshotHistory(third).equal(before), "failed capture modified the previous history")
		c.Check(slices.Equal(srv.requestLog(), []string{"GET /incremental/entry", "GET /incremental/7"}), "unexpected failed requests: %v", srv.requestLog())

		// Retrying with the same previous history reuses the older
		// cached archives and yields pristine content.
		srv.set("/incremental/7", archive("urn:archive7", "2026-12-04T00:00:00Z", "/incremental/5",
			atomEntry("urn:new7", "New 7", "2026-12-04T00:00:00Z")))

		srv.reset()
		fourth, err := reader.CaptureHistory(ctx, entryURL, third, "incremental")
		if err != nil {
			t.Fatalf("retry after failure failed: %v", err)
		}
		checkHistoryItems(c, fourth,
			[]string{"urn:recent5", "urn:new7", "urn:new5", "urn:far"},
			[]string{"Fifth record", "New 7", "New 5", "Far record"})
		c.Check(slices.Equal(fourth.Documents, []string{
			entryURL,
			srv.URL("/incremental/7"),
			srv.URL("/incremental/5"),
			srv.URL("/incremental/6"),
		}), "unexpected documents: %v", fourth.Documents)
		c.Check(slices.Equal(srv.requestLog(), []string{"GET /incremental/entry", "GET /incremental/7"}), "unexpected retry requests: %v", srv.requestLog())

		item := findItem(fourth.Feed, "urn:new5")
		c.Check(item != nil && item.Title == "New 5", "cached content was mutated by the caller: %v", item)
		c.Check(item != nil && item.Read, "read state was not preserved from previous")
		// previous history must not have been reused or modified.
		c.Check(snapshotHistory(third).equal(before), "successful capture modified the previous history")

		// With no previous history, incremental starts from scratch.
		srv.reset()
		fromScratch, err := reader.CaptureHistory(ctx, entryURL, nil, "incremental")
		c.Check(err == nil, "incremental without previous failed: %v", err)
		if err == nil {
			checkHistoryItems(c, fromScratch,
				[]string{"urn:recent5", "urn:new7", "urn:new5", "urn:far"},
				[]string{"Fifth record", "New 7", "New 5", "Far record"})
			c.Check(fromScratch.Feed.Unread == 4, "unexpected unread count %d", fromScratch.Feed.Unread)
			c.Check(slices.Equal(itemReads(fromScratch.Feed), []bool{false, false, false, false}), "incremental without previous kept read state: %v", itemReads(fromScratch.Feed))
		}
		c.Check(slices.Equal(srv.requestLog(), []string{
			"GET /incremental/entry",
			"GET /incremental/7",
			"GET /incremental/5",
			"GET /incremental/6",
		}), "incremental without previous reused the cache: %v", srv.requestLog())

		// A previously captured archive that reappears later in the
		// chain is a duplicate request address, even though no new
		// request would be made for it.
		srv.set("/loop/entry", entry("2026-12-10T00:00:00Z", "/loop/1",
			atomEntry("urn:loop-recent", "Loop recent", "2026-12-10T00:00:00Z")))
		srv.set("/loop/1", archive("urn:loop1", "2026-12-09T00:00:00Z", "",
			atomEntry("urn:loop-old", "Loop old", "2026-12-08T00:00:00Z")))
		loopEntry := srv.URL("/loop/entry")

		srv.reset()
		loopFirst, err := reader.CaptureHistory(ctx, loopEntry, nil, "full")
		if err != nil {
			t.Fatalf("loop capture failed: %v", err)
		}
		c.Check(slices.Equal(loopFirst.Documents, []string{loopEntry, srv.URL("/loop/1")}), "unexpected loop documents: %v", loopFirst.Documents)

		srv.set("/loop/entry", entry("2026-12-11T00:00:00Z", "/loop/2",
			atomEntry("urn:loop-new", "Loop new", "2026-12-11T00:00:00Z")))
		srv.set("/loop/2", archive("urn:loop2", "2026-12-10T00:00:00Z", "/loop/1",
			atomEntry("urn:loop-mid", "Loop mid", "2026-12-10T00:00:00Z")))

		srv.reset()
		loopSecond, err := reader.CaptureHistory(ctx, loopEntry, loopFirst, "incremental")
		if err != nil {
			t.Fatalf("loop extension failed: %v", err)
		}
		c.Check(slices.Equal(loopSecond.Documents, []string{loopEntry, srv.URL("/loop/2"), srv.URL("/loop/1")}), "unexpected extended loop documents: %v", loopSecond.Documents)

		// Reused archives are immutable: a server-side change to an
		// archive that is still reachable is not observed.
		srv.set("/loop/2", archive("urn:loop2", "2026-12-10T00:00:00Z", "/loop/2",
			atomEntry("urn:loop-mid", "Changed mid", "2026-12-10T00:00:00Z")))
		srv.reset()
		reused, err := reader.CaptureHistory(ctx, loopEntry, loopSecond, "incremental")
		c.Check(err == nil, "reused capture failed: %v", err)
		if err == nil {
			checkHistoryItems(c, reused,
				[]string{"urn:loop-new", "urn:loop-mid", "urn:loop-old"},
				[]string{"Loop new", "Loop mid", "Loop old"})
			c.Check(slices.Equal(reused.Documents, loopSecond.Documents), "reused capture changed documents: %v", reused.Documents)
		}
		c.Check(slices.Equal(srv.requestLog(), []string{"GET /loop/entry"}), "reused archive was refetched: %v", srv.requestLog())
	})
}

// TestHistoryMerge covers ID merging, winner selection, positions,
// and read-state recalculation.
func TestHistoryMerge(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		srv := newHistoryTestServer(t)
		reader := srv.reader()
		ctx := context.Background()
		entryURL := srv.URL("/merge/root")

		srv.set("/merge/root", atomDocument("", `
 <title>Merge bulletin</title>
 <id>urn:merge</id>
 <updated>2026-11-01T00:00:00Z</updated>
 <link rel="prev-archive" href="/merge/archive1"/>
 `+atomEntry("urn:a", "A root", "2031-01-01T00:00:00Z")+
			atomEntry("urn:b", "B root", "2026-01-01T00:00:00Z")+
			atomEntry("urn:d", "D root", "2001-01-01T00:00:00Z")))

		srv.set("/merge/archive1", atomDocument("", `
 <title>First archive</title>
 <id>urn:merge1</id>
 <updated>2026-11-05T00:00:00Z</updated>
 <fh:archive/>
 <link rel="prev-archive" href="/merge/archive2"/>
 `+atomEntry("urn:c", "C archive1", "2026-10-10T00:00:00Z")+
			atomEntry("urn:a", "A archive1", "1990-01-01T00:00:00Z")))

		srv.set("/merge/archive2", atomDocument("", `
 <title>Second archive</title>
 <id>urn:merge2</id>
 <updated>2026-11-01T00:00:00Z</updated>
 <fh:archive/>
 `+atomEntry("urn:d", "D archive2", "2031-01-01T00:00:00Z")+
			atomEntry("urn:f", "F archive2", "2026-09-01T00:00:00Z")))

		history, err := reader.CaptureHistory(ctx, entryURL, nil, "full")
		if err != nil {
			t.Fatalf("merge capture failed: %v", err)
		}

		// The newer feed-level updated time wins for urn:a even though
		// its entry updated is older. The tie for urn:d goes to the
		// first document encountered, not to entry updated.
		checkHistoryItems(c, history,
			[]string{"urn:a", "urn:b", "urn:d", "urn:c", "urn:f"},
			[]string{"A archive1", "B root", "D root", "C archive1", "F archive2"})
		c.Check(findItem(history.Feed, "urn:a") == history.Feed.Items[0], "replacement moved the item position")
		c.Check(history.Feed.Unread == 5, "unexpected unread count %d", history.Feed.Unread)
		c.Check(history.Feed.Title == "Merge bulletin", "channel metadata did not come from the root: %q", history.Feed.Title)

		// Read state is kept by ID, new members are unread, and Unread
		// is recalculated.
		if item := findItem(history.Feed, "urn:b"); item != nil {
			item.Read = true
		}
		srv.reset()
		merged, err := reader.CaptureHistory(ctx, entryURL, history, "full")
		if err != nil {
			t.Fatalf("second merge capture failed: %v", err)
		}
		checkHistoryItems(c, merged,
			[]string{"urn:a", "urn:b", "urn:d", "urn:c", "urn:f"},
			[]string{"A archive1", "B root", "D root", "C archive1", "F archive2"})
		c.Check(slices.Equal(itemReads(merged.Feed), []bool{false, true, false, false, false}), "unexpected read states: %v", itemReads(merged.Feed))
		c.Check(merged.Feed.Unread == 4, "unexpected unread count %d", merged.Feed.Unread)
		c.Check(slices.Equal(srv.requestLog(), []string{"GET /merge/root", "GET /merge/archive1", "GET /merge/archive2"}), "unexpected requests: %v", srv.requestLog())
	})
}
