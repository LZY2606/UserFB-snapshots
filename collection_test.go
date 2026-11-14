package gofeed

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"

	"github.com/mmcdole/gofeed/task_support/taskcheck"
)

// fakeTransport answers requests from in-memory documents, keyed by request
// path. It records every request and any Authorization header so tests can
// prove the Parser's Client and AuthConfig remain in effect.
type fakeTransport struct {
	mu     sync.Mutex
	docs   map[string]string
	status map[string]int
	// final rewrites resp.Request.URL to simulate a redirect landing page.
	final map[string]string
	seen  []string
	auth  []string
}

func (t *fakeTransport) RoundTrip(r *http.Request) (*http.Response, error) {
	if err := r.Context().Err(); err != nil {
		return nil, err
	}
	t.mu.Lock()
	t.seen = append(t.seen, r.URL.String())
	t.auth = append(t.auth, r.Header.Get("Authorization"))
	body, ok := t.docs[r.URL.Path]
	code := t.status[r.URL.Path]
	final := t.final[r.URL.Path]
	t.mu.Unlock()

	if code == 0 {
		code = http.StatusOK
	}
	if !ok && code == http.StatusOK {
		code = http.StatusNotFound
	}
	respReq := r
	if final != "" {
		u, err := url.Parse(final)
		if err != nil {
			return nil, err
		}
		respReq = r.Clone(r.Context())
		respReq.URL = u
	}
	return &http.Response{
		StatusCode: code,
		Status:     fmt.Sprintf("%d status", code),
		Header:     make(http.Header),
		Request:    respReq,
		Body:       io.NopCloser(strings.NewReader(body)),
	}, nil
}

func (t *fakeTransport) requestCount() int {
	t.mu.Lock()
	defer t.mu.Unlock()
	return len(t.seen)
}

func (t *fakeTransport) lastAuth() string {
	t.mu.Lock()
	defer t.mu.Unlock()
	if len(t.auth) == 0 {
		return ""
	}
	return t.auth[len(t.auth)-1]
}

func newCollectionParser(transport http.RoundTripper) *Parser {
	p := NewParser()
	p.Client = &http.Client{Transport: transport}
	return p
}

const rssShell = `<rss version="2.0"><channel><title>%s</title>%s</channel></rss>`
const atomShell = `<feed xmlns="http://www.w3.org/2005/Atom"><id>urn:%s</id><title>%s</title>%s</feed>`

func rssItem(guid, title, link string) string {
	var b strings.Builder
	b.WriteString("<item>")
	if guid != "" {
		fmt.Fprintf(&b, `<guid isPermaLink="false">%s</guid>`, guid)
	}
	if title != "" {
		fmt.Fprintf(&b, "<title>%s</title>", title)
	}
	if link != "" {
		fmt.Fprintf(&b, "<link>%s</link>", link)
	}
	b.WriteString("</item>")
	return b.String()
}

func atomEntry(id, title string, hrefs ...string) string {
	var b strings.Builder
	b.WriteString("<entry>")
	if id != "" {
		fmt.Fprintf(&b, "<id>%s</id>", id)
	}
	if title != "" {
		fmt.Fprintf(&b, "<title>%s</title>", title)
	}
	for _, href := range hrefs {
		fmt.Fprintf(&b, `<link href="%s"/>`, href)
	}
	b.WriteString("</entry>")
	return b.String()
}

func readCollectionFile(t *testing.T, dir string) []byte {
	t.Helper()
	data, err := os.ReadFile(filepath.Join(dir, "collection.json"))
	if err != nil {
		t.Fatalf("collection.json missing: %v", err)
	}
	return data
}

// TestCollectionIdentity covers in-source identity rules: equal GUIDs across
// sources never merge, GUID and link identities are separate kinds, folded
// items keep their raw spelling, and snapshots are independent copies.
func TestCollectionIdentity(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		dir := t.TempDir()
		transport := &fakeTransport{docs: map[string]string{
			"/a": fmt.Sprintf(rssShell, "A", rssItem("shared-id", "A1", "https://a.example/post/1#frag")),
			"/b": fmt.Sprintf(atomShell, "b", "B", atomEntry("shared-id", "B1", "https://b.example/post/2")),
			// First item is GUID-identified; the second has an empty GUID and
			// falls back to a link whose canonical spelling equals that GUID
			// string. The two kinds must not fold together.
			"/c": `{"version":"https://jsonfeed.org/version/1.1","title":"C","items":[` +
				`{"id":"https://c.example/g","title":"G"},` +
				`{"id":"","title":"L","url":"https://c.example/g"}]}`,
		}}
		collection, err := OpenCollection(dir, newCollectionParser(transport))
		c.Check(err == nil, "open collection: %v", err)

		snapA, err := collection.Collect(context.Background(), "https://feeds.invalid/a")
		c.Check(err == nil, "collect a: %v", err)
		c.Check(len(snapA.Sources) == 1 && len(snapA.Records) == 1, "first collect produced %d sources %d records", len(snapA.Sources), len(snapA.Records))

		_, err = collection.Collect(context.Background(), "https://feeds.invalid/b")
		c.Check(err == nil, "collect b: %v", err)
		snap, err := collection.Collect(context.Background(), "https://feeds.invalid/c")
		c.Check(err == nil, "collect c: %v", err)

		c.Check(len(snap.Sources) == 3, "want 3 sources, got %d", len(snap.Sources))
		c.Check(snap.Sources[0].URL == "https://feeds.invalid/a" && snap.Sources[1].URL == "https://feeds.invalid/b" && snap.Sources[2].URL == "https://feeds.invalid/c",
			"sources not sorted: %+v", snap.Sources)
		c.Check(len(snap.Records) == 4, "same GUID across sources must not merge and identity kinds must stay separate, got %d records: %+v", len(snap.Records), snap.Records)
		for _, record := range snap.Records {
			c.Check(len(record.Origins) == 1, "unexpected merged record: %+v", record.Origins)
		}

		// FinalURL is the address the body came from; the source key keeps the
		// caller's spelling even though a fragment was supplied.
		keyed, err := collection.Collect(context.Background(), "https://feeds.invalid/a#section")
		c.Check(err == nil, "re-collect with fragment: %v", err)
		c.Check(keyed.Sources[0].URL == "https://feeds.invalid/a", "source key must drop the fragment: %q", keyed.Sources[0].URL)
		c.Check(keyed.Sources[0].FinalURL == "https://feeds.invalid/a", "final URL: %q", keyed.Sources[0].FinalURL)

		// The folded Item keeps the raw link spelling: relative resolution and
		// canonicalization only drive grouping, they are never written back.
		c.Check(keyed.Sources[0].Items[0].Link == "https://a.example/post/1#frag",
			"item link was rewritten: %q", keyed.Sources[0].Items[0].Link)

		// Returned snapshots are independent of collection state and of each
		// other: mutating one must not change the next Snapshot().
		snap.Sources[0].Items[0].Title = "mutated"
		snap.Records[0].Item.Title = "mutated-record"
		snap.Records[0].Origins[0].ItemIndex = 99
		fresh := collection.Snapshot()
		c.Check(fresh.Sources[0].Items[0].Title != "mutated", "source item mutation leaked into collection")
		c.Check(fresh.Records[0].Item.Title != "mutated-record", "record item mutation leaked into collection")
		c.Check(fresh.Records[0].Origins[0].ItemIndex != 99, "origin mutation leaked into collection")

		// Empty collections and empty lists encode as [], never null.
		empty, err := OpenCollection(t.TempDir(), newCollectionParser(transport))
		c.Check(err == nil, "open empty collection: %v", err)
		encoded, err := json.Marshal(empty.Snapshot())
		c.Check(err == nil, "marshal empty snapshot: %v", err)
		c.Check(bytes.Contains(encoded, []byte(`"sources":[]`)) && bytes.Contains(encoded, []byte(`"records":[]`)),
			"empty lists must encode as []: %s", encoded)

		// Classification failures are reported rather than partially stored.
		_, err = classifyEntry(nil, "https://feeds.invalid/")
		c.Check(err != nil, "nil item must fail classification")
		_, err = classifyEntry(&Item{Title: "no identity"}, "https://feeds.invalid/")
		c.Check(err != nil, "guidless/linkless item must fail classification")

		// Only absolute http(s) URLs are valid source keys.
		_, err = collection.Collect(context.Background(), "ftp://feeds.invalid/a")
		c.Check(errors.Is(err, ErrInvalidCollectionSource), "ftp source should be rejected, got %v", err)
		_, err = collection.Remove("/relative/path")
		c.Check(errors.Is(err, ErrInvalidCollectionSource), "relative remove should be rejected, got %v", err)
	})
}

// TestCollectionRebuild covers link bridging (including indirect chains),
// splitting once a bridge disappears, duplicate-observation overwrite,
// reopen verification, and preservation of the old file on failure.
func TestCollectionRebuild(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		dir := t.TempDir()
		transport := &fakeTransport{docs: map[string]string{
			// g1 uses an ugly spelling that must canonicalize to
			// https://www.example.com/a; g2 is an unrelated observation.
			"/s1": fmt.Sprintf(rssShell, "S1",
				rssItem("g1", "G1", "https://WWW.EXAMPLE.COM:443/a#frag")+
					rssItem("g2", "G2", "https://www.example.com/b")),
			// e1 bridges g1 through /a and e2 through /c; e2 only carries /c.
			"/s2": fmt.Sprintf(atomShell, "s2", "S2",
				atomEntry("e1", "E1", "https://www.example.com/a", "https://www.example.com/c")+
					atomEntry("e2", "E2", "https://www.example.com/c")),
		}}
		collection, err := OpenCollection(dir, newCollectionParser(transport))
		c.Check(err == nil, "open collection: %v", err)

		_, err = collection.Collect(context.Background(), "https://feeds.invalid/s1")
		c.Check(err == nil, "collect s1: %v", err)
		snap, err := collection.Collect(context.Background(), "https://feeds.invalid/s2")
		c.Check(err == nil, "collect s2: %v", err)

		c.Check(len(snap.Records) == 2, "want bridged group plus g2, got %d records", len(snap.Records))
		var bridged *CollectedRecord
		for i := range snap.Records {
			if len(snap.Records[i].Origins) == 3 {
				bridged = &snap.Records[i]
			}
		}
		c.Check(bridged != nil, "no 3-member bridged record found: %+v", snap.Records)
		if bridged != nil {
			c.Check(bridged.Item.Title == "G1", "representative must be the first ordered member, got %q", bridged.Item.Title)
			want := []ItemOrigin{
				{SourceURL: "https://feeds.invalid/s1", ItemIndex: 0},
				{SourceURL: "https://feeds.invalid/s2", ItemIndex: 0},
				{SourceURL: "https://feeds.invalid/s2", ItemIndex: 1},
			}
			c.Check(fmt.Sprint(bridged.Origins) == fmt.Sprint(want), "bridged origins order wrong: %+v", bridged.Origins)
		}
		// The raw, non-canonical spelling is still on the folded item.
		c.Check(snap.Sources[0].Items[0].Link == "https://WWW.EXAMPLE.COM:443/a#frag",
			"canonicalization must not rewrite the item: %q", snap.Sources[0].Items[0].Link)

		// Direct canonicalization contract.
		norm, err := normalizeHTTPURL("https://WWW.Example.COM:443/pa%20th?z=1&a=2#f")
		c.Check(err == nil, "normalize: %v", err)
		c.Check(norm == "https://www.example.com/pa%20th?z=1&a=2", "unexpected normalization %q", norm)
		norm, err = normalizeHTTPURL("http://example.com")
		c.Check(err == nil, "normalize empty path: %v", err)
		c.Check(norm == "http://example.com/", "empty path must become /, got %q", norm)
		norm, err = normalizeHTTPURL("http://example.com:80/x?a=1")
		c.Check(err == nil && norm == "http://example.com/x?a=1", "default port not stripped: %q %v", norm, err)
		_, err = normalizeHTTPURL("mailto:noreply@example.com")
		c.Check(err != nil, "non-http url must fail normalization")

		// Withdraw the bridge: e1 now points at an unrelated address, while e2
		// keeps /c. The historical g1<->e1 edge must disappear completely.
		transport.mu.Lock()
		transport.docs["/s2"] = fmt.Sprintf(atomShell, "s2", "S2",
			atomEntry("e1", "E1v2", "https://www.example.com/d")+
				atomEntry("e2", "E2", "https://www.example.com/c"))
		transport.mu.Unlock()
		snap, err = collection.Collect(context.Background(), "https://feeds.invalid/s2")
		c.Check(err == nil, "re-collect s2: %v", err)
		c.Check(len(snap.Records) == 4, "bridge removal must split the group into four singletons, got %d records", len(snap.Records))
		sizes := map[int]int{}
		for _, r := range snap.Records {
			sizes[len(r.Origins)]++
		}
		c.Check(sizes[1] == 4, "want four singleton records, got %+v", sizes)

		// Remove withdraws the whole source; removing an unknown key is a no-op.
		snap, err = collection.Remove("https://feeds.invalid/s2")
		c.Check(err == nil, "remove s2: %v", err)
		c.Check(len(snap.Sources) == 1 && len(snap.Records) == 2, "after remove want 1 source and 2 records, got %d/%d", len(snap.Sources), len(snap.Records))
		before := collection.Snapshot()
		snap, err = collection.Remove("https://feeds.invalid/never-collected")
		c.Check(err == nil, "unknown remove must succeed: %v", err)
		c.Check(len(snap.Sources) == len(before.Sources) && len(snap.Records) == len(before.Records), "unknown remove changed state")

		// Duplicate identities inside one source: first position wins for
		// ordering, last occurrence wins for payload and link set; GUID and
		// link identities sharing a string stay separate.
		transport.mu.Lock()
		transport.docs["/s3"] = fmt.Sprintf(rssShell, "S3",
			rssItem("dup", "First", "https://d.example/1")+
				rssItem("dup", "Last", "https://d.example/2")+
				rssItem("", "L1", "https://d.example/linkid")+
				rssItem("", "L2", "https://d.example/linkid")+
				rssItem("https://d.example/linkid", "G", ""))
		transport.mu.Unlock()
		snap, err = collection.Collect(context.Background(), "https://feeds.invalid/s3")
		c.Check(err == nil, "collect s3: %v", err)
		var s3 *SourceSnapshot
		for i := range snap.Sources {
			if snap.Sources[i].URL == "https://feeds.invalid/s3" {
				s3 = &snap.Sources[i]
			}
		}
		c.Check(s3 != nil && len(s3.Items) == 3, "five physical items fold to three identities, got %+v", s3)
		if s3 != nil && len(s3.Items) == 3 {
			c.Check(s3.Items[0].Title == "Last", "duplicate GUID must take the last payload, got %q", s3.Items[0].Title)
			c.Check(s3.Items[1].Title == "L2", "duplicate link identity must take the last payload, got %q", s3.Items[1].Title)
			c.Check(s3.Items[2].Title == "G" && s3.Items[2].GUID == "https://d.example/linkid", "GUID-kind item must survive alongside the link-kind one, got %+v", s3.Items[2])
		}
		// Origins keep pointing at folded positions.
		for _, record := range snap.Records {
			for _, origin := range record.Origins {
				c.Check(origin.ItemIndex >= 0, "negative item index: %+v", origin)
			}
		}

		// Reopen: rebuild from Sources without any requests and verify Records.
		reopened, err := OpenCollection(dir, newCollectionParser(noRequestsTransport{t: t}))
		c.Check(err == nil, "reopen collection: %v", err)
		c.Check(len(reopened.Snapshot().Records) == len(collection.Snapshot().Records), "reopened record count differs")
		c.Check(len(reopened.Snapshot().Sources) == len(collection.Snapshot().Sources), "reopened source count differs")

		// Corrupt files are rejected.
		badDir := t.TempDir()
		c.Check(os.WriteFile(filepath.Join(badDir, collectionFile), []byte("{not json"), 0o644) == nil, "write corrupt file")
		_, err = OpenCollection(badDir, newCollectionParser(noRequestsTransport{t: t}))
		c.Check(err != nil, "corrupt collection.json must be rejected")

		// Self-contradictory files are rejected too.
		tamperedDir := t.TempDir()
		goodBytes := readCollectionFile(t, dir)
		var payload map[string]any
		c.Check(json.Unmarshal(goodBytes, &payload) == nil, "decode good payload")
		records := payload["records"].([]any)
		firstRecord := records[0].(map[string]any)
		firstRecord["origins"].([]any)[0].(map[string]any)["itemIndex"] = 999
		tampered, err := json.Marshal(payload)
		c.Check(err == nil, "marshal tampered payload: %v", err)
		c.Check(os.WriteFile(filepath.Join(tamperedDir, collectionFile), tampered, 0o644) == nil, "write tampered file")
		_, err = OpenCollection(tamperedDir, newCollectionParser(noRequestsTransport{t: t}))
		c.Check(err != nil, "self-contradictory collection.json must be rejected")

		// A failed fetch leaves both the file bytes and the memory snapshot.
		goodFile := readCollectionFile(t, dir)
		goodSnap := collection.Snapshot()
		_, err = collection.Collect(context.Background(), "https://feeds.invalid/missing")
		c.Check(err != nil, "unknown feed path must fail, got nil error")
		c.Check(bytes.Equal(readCollectionFile(t, dir), goodFile), "failed collect changed collection.json bytes")
		c.Check(len(collection.Snapshot().Records) == len(goodSnap.Records), "failed collect changed in-memory snapshot")

		// A classification failure is likewise all-or-nothing.
		transport.mu.Lock()
		transport.docs["/broken"] = fmt.Sprintf(rssShell, "Broken", rssItem("", "NoIdentity", ""))
		transport.mu.Unlock()
		_, err = collection.Collect(context.Background(), "https://feeds.invalid/broken")
		c.Check(err != nil, "feed with an unidentifiable item must fail the whole collect, got nil")
		c.Check(bytes.Equal(readCollectionFile(t, dir), goodFile), "classification failure changed collection.json bytes")
		c.Check(len(collection.Snapshot().Records) == len(goodSnap.Records), "classification failure changed in-memory snapshot")
	})
}

// noRequestsTransport fails the test if a reopened collection ever reaches the
// network: reload must be purely local.
type noRequestsTransport struct{ t *testing.T }

func (noRequestsTransport) RoundTrip(r *http.Request) (*http.Response, error) {
	return nil, fmt.Errorf("reload must not issue requests, got %s", r.URL.String())
}

// prefixTranslator wraps the default RSS translator to prove custom
// translators configured on the Parser stay in effect during Collect.
type prefixTranslator struct{ inner Translator }

func (t prefixTranslator) Translate(feed interface{}) (*Feed, error) {
	f, err := t.inner.Translate(feed)
	if err != nil {
		return nil, err
	}
	for _, item := range f.Items {
		item.Title = "X:" + item.Title
	}
	return f, nil
}

// TestCollectionPublication exercises real RSS, Atom and JSON Feed
// collections end to end, along with Parser features (auth, context, byte
// limit, custom translator, redirect FinalURL and relative-link rebuilding).
func TestCollectionPublication(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		dir := t.TempDir()
		transport := &fakeTransport{
			docs: map[string]string{
				"/rss":  fmt.Sprintf(rssShell, "RSS", rssItem("r1", "RSS Story", "https://p.example/ext")),
				"/atom": fmt.Sprintf(atomShell, "atom", "Atom", atomEntry("a1", "Atom Story", "post/42")),
				"/json": `{"version":"https://jsonfeed.org/version/1.1","title":"JSON","items":[` +
					`{"id":"j1","title":"JSON Story","url":"https://p.example/j","external_url":"https://p.example/ext"}]}`,
			},
			// The Atom document is served after a redirect: relative links in
			// its entries must resolve against this final address.
			final: map[string]string{"/atom": "https://feeds.invalid/final/atom"},
		}
		parser := newCollectionParser(transport)
		parser.AuthConfig = &Auth{Username: "user", Password: "pass"}
		collection, err := OpenCollection(dir, parser)
		c.Check(err == nil, "open collection: %v", err)

		snap, err := collection.Collect(context.Background(), "https://feeds.invalid/rss")
		c.Check(err == nil, "collect rss: %v", err)
		c.Check(snap.Sources[0].Items[0].GUID == "r1" && snap.Sources[0].Items[0].Title == "RSS Story", "rss item not translated: %+v", snap.Sources[0].Items[0])
		c.Check(transport.lastAuth() != "" && strings.HasPrefix(transport.lastAuth(), "Basic "), "basic auth was not applied: %q", transport.lastAuth())

		_, err = collection.Collect(context.Background(), "https://feeds.invalid/atom")
		c.Check(err == nil, "collect atom: %v", err)
		snap, err = collection.Collect(context.Background(), "https://feeds.invalid/json")
		c.Check(err == nil, "collect json: %v", err)
		c.Check(len(snap.Sources) == 3, "all three formats collected, got %d sources", len(snap.Sources))

		// JSON Feed contributes both url and external_url; the RSS story
		// shares external_url, so those two observations form one article.
		var shared *CollectedRecord
		for i := range snap.Records {
			if len(snap.Records[i].Origins) == 2 {
				shared = &snap.Records[i]
			}
		}
		c.Check(shared != nil, "rss/json external_url bridge missing: %+v", snap.Records)
		if shared != nil {
			c.Check(shared.Item.Title == "JSON Story", "representative should come from the earlier-sorted json source, got %q", shared.Item.Title)
		}

		// The Atom entry's relative link resolves against the redirect's final
		// URL: https://feeds.invalid/final/post/42. Prove it by collecting a
		// JSON source that links to the resolved address.
		transport.mu.Lock()
		transport.docs["/json2"] = `{"version":"https://jsonfeed.org/version/1.1","title":"JSON2","items":[` +
			`{"id":"j2","title":"Relative Bridge","url":"https://feeds.invalid/final/post/42"}]}`
		transport.mu.Unlock()
		snap, err = collection.Collect(context.Background(), "https://feeds.invalid/json2")
		c.Check(err == nil, "collect json2: %v", err)
		bridged := false
		for _, record := range snap.Records {
			for _, origin := range record.Origins {
				if origin.SourceURL == "https://feeds.invalid/atom" {
					bridged = bridged || len(record.Origins) == 2
				}
			}
		}
		c.Check(bridged, "relative atom link did not resolve against FinalURL: %+v", snap.Records)
		var atomSource *SourceSnapshot
		for i := range snap.Sources {
			if snap.Sources[i].URL == "https://feeds.invalid/atom" {
				atomSource = &snap.Sources[i]
			}
		}
		c.Check(atomSource != nil && atomSource.FinalURL == "https://feeds.invalid/final/atom", "FinalURL not persisted: %+v", atomSource)
		c.Check(atomSource != nil && atomSource.Items[0].Link == "post/42", "relative link must stay relative on the Item: %+v", atomSource)

		// Persisted file ends with a newline and survives a reopen with all
		// bridges intact, using FinalURL as the resolution base.
		raw := readCollectionFile(t, dir)
		c.Check(bytes.HasSuffix(raw, []byte("\n")), "collection.json must end with a newline")
		reopened, err := OpenCollection(dir, newCollectionParser(noRequestsTransport{t: t}))
		c.Check(err == nil, "reopen: %v", err)
		c.Check(len(reopened.Snapshot().Records) == len(collection.Snapshot().Records), "reopen changed records")

		// Context cancellation surfaces as a collect error and preserves state.
		ctx, cancel := context.WithCancel(context.Background())
		cancel()
		beforeCount := len(collection.Snapshot().Records)
		_, err = collection.Collect(ctx, "https://feeds.invalid/rss")
		c.Check(err != nil, "canceled context must fail Collect")
		c.Check(len(collection.Snapshot().Records) == beforeCount, "canceled collect changed state")

		// MaxByteSize is honored through the collection's Parser.
		limitedDir := t.TempDir()
		limitedParser := newCollectionParser(transport)
		limitedParser.MaxByteSize = 8
		limited, err := OpenCollection(limitedDir, limitedParser)
		c.Check(err == nil, "open limited collection: %v", err)
		_, err = limited.Collect(context.Background(), "https://feeds.invalid/rss")
		c.Check(errors.Is(err, ErrResponseTooLarge), "want ErrResponseTooLarge, got %v", err)
		c.Check(len(limited.Snapshot().Sources) == 0, "oversized response must not be stored")

		// A custom translator configured on the Parser runs during Collect.
		customDir := t.TempDir()
		customParser := newCollectionParser(transport)
		customParser.RSSTranslator = prefixTranslator{inner: &DefaultRSSTranslator{}}
		custom, err := OpenCollection(customDir, customParser)
		c.Check(err == nil, "open custom collection: %v", err)
		customSnap, err := custom.Collect(context.Background(), "https://feeds.invalid/rss")
		c.Check(err == nil, "custom collect: %v", err)
		c.Check(customSnap.Sources[0].Items[0].Title == "X:RSS Story", "custom translator not used, got %q", customSnap.Sources[0].Items[0].Title)

		// The Parser's Client really is the one making the requests.
		c.Check(transport.requestCount() >= 6, "expected many real fetches through the parser client, got %d", transport.requestCount())
	})
}
