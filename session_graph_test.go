package sitemap

import (
	"fmt"
	"sort"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/yterajima/go-sitemap/task_support/taskcheck"
)

// fakeServer is an in-memory sitemap server driven through SetFetch. It records
// every request so tests can prove which documents were downloaded.
type fakeServer struct {
	mu       sync.Mutex
	docs     map[string]string
	fail     map[string]bool
	requests map[string]int
	options  []interface{}
}

func newFakeServer(docs map[string]string) *fakeServer {
	copied := map[string]string{}
	for k, v := range docs {
		copied[k] = v
	}
	return &fakeServer{docs: copied, fail: map[string]bool{}, requests: map[string]int{}}
}

func (f *fakeServer) fetch(docURL string, options interface{}) ([]byte, error) {
	f.mu.Lock()
	defer f.mu.Unlock()

	f.requests[docURL]++
	f.options = append(f.options, options)
	if f.fail[docURL] {
		return nil, fmt.Errorf("simulated fetch failure for %s", docURL)
	}
	body, ok := f.docs[docURL]
	if !ok {
		return nil, fmt.Errorf("document not found: %s", docURL)
	}
	return []byte(body), nil
}

func (f *fakeServer) count(docURL string) int {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.requests[docURL]
}

func (f *fakeServer) totalRequests() int {
	f.mu.Lock()
	defer f.mu.Unlock()
	total := 0
	for _, n := range f.requests {
		total += n
	}
	return total
}

func (f *fakeServer) resetRequests() {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.requests = map[string]int{}
}

func (f *fakeServer) set(docURL, body string) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.docs[docURL] = body
}

func indexXML(entries ...string) string {
	var b strings.Builder
	b.WriteString(`<sitemapindex xmlns="http://www.sitemaps.org/schemas/sample/0.9">`)
	for _, e := range entries {
		b.WriteString(e)
	}
	b.WriteString(`</sitemapindex>`)
	return b.String()
}

func indexEntry(loc, lastmod string) string {
	if lastmod == "" {
		return fmt.Sprintf(`<sitemap><loc>%s</loc></sitemap>`, loc)
	}
	return fmt.Sprintf(`<sitemap><loc>%s</loc><lastmod>%s</lastmod></sitemap>`, loc, lastmod)
}

func urlsetXML(locs ...string) string {
	var b strings.Builder
	b.WriteString(`<urlset xmlns="http://www.sitemaps.org/schemas/sample/0.9">`)
	for _, loc := range locs {
		b.WriteString(fmt.Sprintf(`<url><loc>%s</loc></url>`, loc))
	}
	b.WriteString(`</urlset>`)
	return b.String()
}

func sortedEqual(a, b []string) bool {
	if len(a) != len(b) {
		return false
	}
	x := append([]string(nil), a...)
	y := append([]string(nil), b...)
	sort.Strings(x)
	sort.Strings(y)
	for i := range x {
		if x[i] != y[i] {
			return false
		}
	}
	return true
}

// withFakeServer installs srv through SetFetch for the duration of the test.
func withFakeServer(t *testing.T, srv *fakeServer) {
	t.Helper()
	previous := fetch
	SetFetch(srv.fetch)
	SetInterval(time.Nanosecond)
	t.Cleanup(func() {
		fetch = previous
	})
}

const (
	rootURL   = "https://example.test/root.xml"
	midURL    = "https://example.test/mid.xml"
	sharedURL = "https://example.test/shared.xml"
	leafAURL  = "https://example.test/leaf-a.xml"
	leafBURL  = "https://example.test/leaf-b.xml"
)

// TestSessionEvidence covers download trajectories: per-round reuse, stable
// shards not being re-fetched, uneven shared references, evidence formats and
// the Get/ForceGet recursion controls.
func TestSessionEvidence(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		docs := map[string]string{
			rootURL: indexXML(
				indexEntry(sharedURL, "2024-01-01"),
				indexEntry(midURL, "2024-01-01"),
			),
			midURL: indexXML(indexEntry(sharedURL, "2024-01-01")),
			sharedURL: urlsetXML(
				"https://pages.test/shared-1",
				"https://pages.test/shared-2",
			),
		}
		srv := newFakeServer(docs)
		withFakeServer(t, srv)

		session, err := NewSession(rootURL)
		c.Check(err == nil, "NewSession(valid root) error = %v", err)

		// First round: every reachable document is downloaded, shared only once.
		snap, err := session.Sync("incremental", "opt-1")
		c.Check(err == nil, "first Sync error = %v", err)
		c.Check(srv.count(rootURL) == 1, "root downloaded once, got %d", srv.count(rootURL))
		c.Check(srv.count(midURL) == 1, "mid downloaded once, got %d", srv.count(midURL))
		c.Check(srv.count(sharedURL) == 1, "shared downloaded once despite two parents, got %d", srv.count(sharedURL))
		c.Check(sortedEqual(snap.URLs, []string{"https://pages.test/shared-1", "https://pages.test/shared-2"}),
			"first URLs = %v", snap.URLs)
		c.Check(len(srv.options) == 3 && srv.options[0] == "opt-1",
			"options passed through to fetch, got %v", srv.options)

		// Unchanged round: only the root is read; stable shards are reused.
		srv.resetRequests()
		snap, err = session.Sync("incremental", nil)
		c.Check(err == nil, "second Sync error = %v", err)
		c.Check(srv.count(rootURL) == 1, "root re-read every round, got %d", srv.count(rootURL))
		c.Check(srv.count(midURL) == 0, "unchanged mid must not be re-downloaded, got %d", srv.count(midURL))
		c.Check(srv.count(sharedURL) == 0, "unchanged shared must not be re-downloaded, got %d", srv.count(sharedURL))
		c.Check(srv.totalRequests() == 1, "only root requested, got %d entries %v", srv.totalRequests(), srv.requests)
		c.Check(len(snap.Added) == 0 && len(snap.Removed) == 0, "unchanged round has no diff, got +%v -%v", snap.Added, snap.Removed)

		// The deeper branch changes its mind later: mid is re-declared with a
		// new date (so it must be downloaded) and its new content vouches a
		// different instant for shared. The earlier "unchanged" declaration
		// must not pin shared forever.
		srv.set(rootURL, indexXML(
			indexEntry(sharedURL, "2024-01-01"),
			indexEntry(midURL, "2024-06-06"),
		))
		srv.set(midURL, indexXML(indexEntry(sharedURL, "2024-02-02")))
		srv.resetRequests()
		snap, err = session.Sync("incremental", nil)
		c.Check(err == nil, "third Sync error = %v", err)
		c.Check(srv.count(sharedURL) == 1, "conflicting evidence re-downloads shared once, got %d", srv.count(sharedURL))
		c.Check(srv.count(midURL) == 1, "re-declared mid is re-downloaded, got %d", srv.count(midURL))
		c.Check(srv.totalRequests() == 3, "root, mid and shared requested, got %v", srv.requests)

		// Full mode re-reads every reachable document.
		srv.resetRequests()
		_, err = session.Sync("full", nil)
		c.Check(err == nil, "full Sync error = %v", err)
		c.Check(srv.count(rootURL) == 1 && srv.count(midURL) == 1 && srv.count(sharedURL) == 1,
			"full mode downloads every document once, got %v", srv.requests)

		// Evidence formats on a clean graph: a date, UTC-midnight RFC3339 and
		// an offset timestamp all describe the same instant.
		fmtDocs := map[string]string{
			rootURL: indexXML(
				indexEntry(sharedURL, "2024-02-02"),
				indexEntry(midURL, "2024-02-02"),
			),
			midURL:    indexXML(indexEntry(sharedURL, "2024-02-02")),
			sharedURL: urlsetXML("https://pages.test/fmt"),
		}
		fmtSrv := newFakeServer(fmtDocs)
		withFakeServer(t, fmtSrv)
		fmtSession, err := NewSession(rootURL)
		c.Check(err == nil, "format session error = %v", err)
		_, err = fmtSession.Sync("full", nil)
		c.Check(err == nil, "format baseline Sync error = %v", err)

		// mid changes content, so root must re-declare it with a new date;
		// its new edge uses RFC3339 for the same UTC midnight.
		fmtSrv.set(rootURL, indexXML(
			indexEntry(sharedURL, "2024-02-02"),
			indexEntry(midURL, "2024-03-03"),
		))
		fmtSrv.set(midURL, indexXML(indexEntry(sharedURL, "2024-02-02T00:00:00Z")))
		fmtSrv.resetRequests()
		_, err = fmtSession.Sync("incremental", nil)
		c.Check(err == nil, "RFC3339 Sync error = %v", err)
		c.Check(fmtSrv.count(sharedURL) == 0, "date and UTC-midnight RFC3339 are the same instant, got %d", fmtSrv.count(sharedURL))

		// The same instant expressed with a +09:00 offset still matches. mid's
		// content changes, so root re-declares it with a fresh date first.
		fmtSrv.set(rootURL, indexXML(
			indexEntry(sharedURL, "2024-02-02"),
			indexEntry(midURL, "2024-03-04"),
		))
		fmtSrv.set(midURL, indexXML(indexEntry(sharedURL, "2024-02-02T09:00:00+09:00")))
		fmtSrv.resetRequests()
		_, err = fmtSession.Sync("incremental", nil)
		c.Check(err == nil, "offset RFC3339 Sync error = %v", err)
		c.Check(fmtSrv.count(sharedURL) == 0, "offset timestamp normalizes to the same instant, got %d", fmtSrv.count(sharedURL))

		// Missing lastmod forces a download.
		fmtSrv.set(rootURL, indexXML(
			indexEntry(sharedURL, "2024-02-02"),
			indexEntry(midURL, "2024-03-05"),
		))
		fmtSrv.set(midURL, indexXML(indexEntry(sharedURL, "")))
		fmtSrv.resetRequests()
		_, err = fmtSession.Sync("incremental", nil)
		c.Check(err == nil, "missing-lastmod Sync error = %v", err)
		c.Check(fmtSrv.count(sharedURL) == 1, "missing lastmod forces download, got %d", fmtSrv.count(sharedURL))

		// Invalid lastmod forces a download.
		fmtSrv.set(rootURL, indexXML(
			indexEntry(sharedURL, "2024-02-02"),
			indexEntry(midURL, "2024-03-06"),
		))
		fmtSrv.set(midURL, indexXML(indexEntry(sharedURL, "not-a-date")))
		fmtSrv.resetRequests()
		_, err = fmtSession.Sync("incremental", nil)
		c.Check(err == nil, "invalid-lastmod Sync error = %v", err)
		c.Check(fmtSrv.count(sharedURL) == 1, "invalid lastmod forces download, got %d", fmtSrv.count(sharedURL))

		// Fragment-only differences are the same document identity.
		fragDocs := map[string]string{
			rootURL: indexXML(
				indexEntry(sharedURL+"#part-1", "2024-03-03"),
				indexEntry(sharedURL+"#part-2", "2024-03-03"),
			),
			sharedURL: urlsetXML("https://pages.test/frag"),
		}
		fragSrv := newFakeServer(fragDocs)
		withFakeServer(t, fragSrv)
		fragSession, err := NewSession(rootURL)
		c.Check(err == nil, "fragment session error = %v", err)
		_, err = fragSession.Sync("full", nil)
		c.Check(err == nil, "fragment Sync error = %v", err)
		c.Check(fragSrv.count(sharedURL) == 1, "fragment-stripped identity fetched once, got %d", fragSrv.count(sharedURL))

		// Relative locs resolve against the declaring document.
		relRoot := "https://example.test/dir/root.xml"
		relLeaf := "https://example.test/dir/nested/leaf.xml"
		relDocs := map[string]string{
			relRoot: indexXML(indexEntry("nested/leaf.xml", "2024-04-04")),
			relLeaf: urlsetXML("https://pages.test/relative"),
		}
		relSrv := newFakeServer(relDocs)
		withFakeServer(t, relSrv)
		relSession, err := NewSession(relRoot)
		c.Check(err == nil, "relative session error = %v", err)
		relSnap, err := relSession.Sync("full", nil)
		c.Check(err == nil, "relative Sync error = %v", err)
		c.Check(relSrv.count(relLeaf) == 1, "relative loc resolved against parent document, requests=%v", relSrv.requests)
		c.Check(sortedEqual(relSnap.URLs, []string{"https://pages.test/relative"}), "relative URLs = %v", relSnap.URLs)

		// Query strings are not merged: a loc without a query does not inherit
		// the parent document's query; fragments never become part of identity.
		qRoot := "https://example.test/dir/root.xml?parent=1"
		qLeaf := "https://example.test/dir/leaf.xml"
		qDocs := map[string]string{
			qRoot: indexXML(indexEntry("leaf.xml#section", "2024-04-05")),
			qLeaf: urlsetXML("https://pages.test/query"),
		}
		qSrv := newFakeServer(qDocs)
		withFakeServer(t, qSrv)
		qSession, err := NewSession(qRoot)
		c.Check(err == nil, "query session error = %v", err)
		qSnap, err := qSession.Sync("full", nil)
		c.Check(err == nil, "query Sync error = %v", err)
		c.Check(qSrv.count(qLeaf) == 1, "child loc keeps no parent query and drops fragment, requests=%v", qSrv.requests)
		c.Check(sortedEqual(qSnap.URLs, []string{"https://pages.test/query"}), "query URLs = %v", qSnap.URLs)

		// Constructor and mode validation.
		_, err = NewSession("/not-absolute.xml")
		c.Check(err != nil, "relative root rejected")
		_, err = NewSession("ftp://example.test/root.xml")
		c.Check(err != nil, "non-http(s) root rejected")
		badMode, err := NewSession(rootURL)
		c.Check(err == nil, "bad-mode session error = %v", err)
		_, err = badMode.Sync("weekly", nil)
		c.Check(err != nil, "unknown mode rejected")

		// Get/ForceGet recursion control versus the flat single-level case.
		getDocs := map[string]string{
			rootURL: indexXML(indexEntry(midURL, "")),
			midURL:  indexXML(indexEntry(leafAURL, ""), indexEntry(leafBURL, "")),
			leafAURL: urlsetXML(
				"https://pages.test/a-1",
				"https://pages.test/dup",
			),
			leafBURL: urlsetXML(
				"https://pages.test/b-1",
				"https://pages.test/dup",
			),
		}
		getSrv := newFakeServer(getDocs)
		withFakeServer(t, getSrv)
		got, err := Get(rootURL, "get-opt")
		c.Check(err == nil, "nested Get error = %v", err)
		c.Check(len(got.URL) == 4, "nested Get collects every leaf page, got %d", len(got.URL))
		c.Check(got.URL[1].Loc == "https://pages.test/dup" && got.URL[3].Loc == "https://pages.test/dup",
			"duplicate page behavior and DFS order preserved, got %v", got.URL)
		c.Check(len(getSrv.options) == 4 && getSrv.options[0] == "get-opt",
			"Get passes options through every fetch, got %v", getSrv.options)

		// Flat index keeps the historical ordering and duplication semantics.
		flatDocs := map[string]string{
			rootURL:  indexXML(indexEntry(leafAURL, ""), indexEntry(leafBURL, "")),
			leafAURL: urlsetXML("https://pages.test/a-1", "https://pages.test/a-2"),
			leafBURL: urlsetXML("https://pages.test/a-1", "https://pages.test/b-2"),
		}
		flatSrv := newFakeServer(flatDocs)
		withFakeServer(t, flatSrv)
		flat, err := Get(rootURL, nil)
		c.Check(err == nil, "flat Get error = %v", err)
		flatLocs := []string{}
		for _, u := range flat.URL {
			flatLocs = append(flatLocs, u.Loc)
		}
		c.Check(fmt.Sprint(flatLocs) == fmt.Sprint([]string{
			"https://pages.test/a-1", "https://pages.test/a-2",
			"https://pages.test/a-1", "https://pages.test/b-2",
		}), "flat index order/duplication unchanged, got %v", flatLocs)

		// ForceGet ignores a broken child but still returns the healthy branch,
		// including through a nested index.
		forceDocs := map[string]string{
			rootURL:  indexXML(indexEntry(midURL, ""), indexEntry(leafBURL, "")),
			midURL:   indexXML(indexEntry(leafAURL, ""), indexEntry("https://example.test/missing.xml", "")),
			leafAURL: urlsetXML("https://pages.test/a-1"),
			leafBURL: urlsetXML("https://pages.test/b-1"),
		}
		forceSrv := newFakeServer(forceDocs)
		withFakeServer(t, forceSrv)
		force, err := ForceGet(rootURL, nil)
		c.Check(err == nil, "ForceGet error = %v", err)
		c.Check(len(force.URL) == 2, "ForceGet keeps reachable healthy pages, got %d", len(force.URL))
		_, err = Get(rootURL, nil)
		c.Check(err != nil, "strict Get still fails on the broken child")
	})
}

// TestSessionMembership covers snapshot contents, diffs, shared ownership,
// index-to-urlset type changes, empty documents and slice isolation.
func TestSessionMembership(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		docs := map[string]string{
			rootURL: indexXML(
				indexEntry(leafAURL, "2024-01-01"),
				indexEntry(leafBURL, "2024-01-01"),
			),
			leafAURL: urlsetXML(
				"  https://pages.test/shared  ",
				"https://pages.test/only-a",
				"https://pages.test/dup",
				"https://pages.test/dup",
			),
			leafBURL: urlsetXML(
				"https://pages.test/shared",
				"https://pages.test/only-b",
			),
		}
		srv := newFakeServer(docs)
		withFakeServer(t, srv)

		session, err := NewSession(rootURL)
		c.Check(err == nil, "NewSession error = %v", err)

		first, err := session.Sync("full", nil)
		c.Check(err == nil, "first Sync error = %v", err)
		wantFirst := []string{
			"https://pages.test/dup",
			"https://pages.test/only-a",
			"https://pages.test/only-b",
			"https://pages.test/shared",
		}
		c.Check(fmt.Sprint(first.URLs) == fmt.Sprint(wantFirst),
			"trimmed, deduped, sorted URLs, got %v", first.URLs)
		c.Check(fmt.Sprint(first.Added) == fmt.Sprint(wantFirst), "first round reports everything as Added, got %v", first.Added)
		c.Check(len(first.Removed) == 0, "first round has no Removed, got %v", first.Removed)

		// Drop leaf A, but leaf B still claims "shared": that page survives.
		srv.set(rootURL, indexXML(indexEntry(leafBURL, "2024-01-01")))
		second, err := session.Sync("incremental", nil)
		c.Check(err == nil, "second Sync error = %v", err)
		c.Check(sortedEqual(second.URLs, []string{"https://pages.test/only-b", "https://pages.test/shared"}),
			"shared page kept while only-a/dup leave, got %v", second.URLs)
		c.Check(sortedEqual(second.Removed, []string{"https://pages.test/dup", "https://pages.test/only-a"}),
			"Removed = %v", second.Removed)
		c.Check(len(second.Added) == 0, "no additions, got %v", second.Added)

		// Type change: an index child becomes a urlset. Its old out-edge and
		// descendants must leave the result even though they were reachable
		// in every earlier round.
		leafCURL := "https://example.test/leaf-c.xml"
		srv.set(rootURL, indexXML(indexEntry(sharedURL, "2024-05-05")))
		srv.set(sharedURL, indexXML(indexEntry(leafCURL, "2024-05-05")))
		srv.set(leafCURL, urlsetXML("https://pages.test/old-child"))
		third, err := session.Sync("incremental", nil)
		c.Check(err == nil, "index-shape Sync error = %v", err)
		c.Check(sortedEqual(third.URLs, []string{"https://pages.test/old-child"}),
			"index child reachable, got %v", third.URLs)

		// shared changes type; root must re-declare it with a new date so the
		// change is observable under the incremental evidence rules.
		srv.set(rootURL, indexXML(indexEntry(sharedURL, "2024-06-06")))
		srv.set(sharedURL, urlsetXML("https://pages.test/new-shape"))
		fourth, err := session.Sync("incremental", nil)
		c.Check(err == nil, "type-change Sync error = %v", err)
		c.Check(sortedEqual(fourth.URLs, []string{"https://pages.test/new-shape"}),
			"urlset replacement pages only, got %v", fourth.URLs)
		c.Check(sortedEqual(fourth.Added, []string{"https://pages.test/new-shape"}), "Added after type change = %v", fourth.Added)
		c.Check(sortedEqual(fourth.Removed, []string{"https://pages.test/old-child"}), "old descendant removed = %v", fourth.Removed)

		// Empty index and empty urlset are legal.
		emptyRoot := "https://example.test/empty-root.xml"
		emptyLeaf := "https://example.test/empty-leaf.xml"
		srv.set(emptyRoot, `<sitemapindex></sitemapindex>`)
		srv.set(emptyLeaf, `<urlset></urlset>`)
		emptySession, err := NewSession(emptyRoot)
		c.Check(err == nil, "empty-index session error = %v", err)
		emptySnap, err := emptySession.Sync("full", nil)
		c.Check(err == nil, "empty index Sync error = %v", err)
		c.Check(len(emptySnap.URLs) == 0, "empty index yields no pages, got %v", emptySnap.URLs)

		leafSession, err := NewSession(emptyLeaf)
		c.Check(err == nil, "empty-urlset session error = %v", err)
		leafSnap, err := leafSession.Sync("full", nil)
		c.Check(err == nil, "empty urlset Sync error = %v", err)
		c.Check(len(leafSnap.URLs) == 0 && len(leafSnap.Added) == 0, "empty urlset yields empty snapshot, got %v", leafSnap)

		// Last ownership removal: a page declared by two leaves survives the
		// loss of one, then enters Removed when its last declarer drops it.
		ownRoot := "https://example.test/own-root.xml"
		ownA := "https://example.test/own-a.xml"
		ownB := "https://example.test/own-b.xml"
		ownDocs := map[string]string{
			ownRoot: indexXML(
				indexEntry(ownA, "2024-01-01"),
				indexEntry(ownB, "2024-01-01"),
			),
			ownA: urlsetXML("https://pages.test/owned"),
			ownB: urlsetXML("https://pages.test/owned", "https://pages.test/b-only"),
		}
		ownSrv := newFakeServer(ownDocs)
		withFakeServer(t, ownSrv)
		ownSession, err := NewSession(ownRoot)
		c.Check(err == nil, "ownership session error = %v", err)
		ownFirst, err := ownSession.Sync("full", nil)
		c.Check(err == nil, "ownership first Sync error = %v", err)
		c.Check(sortedEqual(ownFirst.URLs, []string{"https://pages.test/b-only", "https://pages.test/owned"}),
			"ownership baseline = %v", ownFirst.URLs)

		ownSrv.set(ownRoot, indexXML(indexEntry(ownB, "2024-01-01")))
		ownSecond, err := ownSession.Sync("incremental", nil)
		c.Check(err == nil, "ownership second Sync error = %v", err)
		c.Check(sortedEqual(ownSecond.URLs, []string{"https://pages.test/b-only", "https://pages.test/owned"}),
			"owned page survives the loss of one declarer, got %v", ownSecond.URLs)

		ownSrv.set(ownRoot, indexXML(indexEntry(ownB, "2024-07-07")))
		ownSrv.set(ownB, urlsetXML("https://pages.test/b-only"))
		ownThird, err := ownSession.Sync("incremental", nil)
		c.Check(err == nil, "ownership third Sync error = %v", err)
		c.Check(sortedEqual(ownThird.URLs, []string{"https://pages.test/b-only"}),
			"last declarer dropped the page, got %v", ownThird.URLs)
		c.Check(sortedEqual(ownThird.Removed, []string{"https://pages.test/owned"}),
			"last-ownership removal reported, got %v", ownThird.Removed)

		// Returned slices are isolated from session state.
		withFakeServer(t, srv)
		isolationSession, err := NewSession(rootURL)
		c.Check(err == nil, "isolation session error = %v", err)
		srv.set(rootURL, indexXML(indexEntry(leafBURL, "2024-01-01")))
		isoSnap, err := isolationSession.Sync("full", nil)
		c.Check(err == nil, "isolation Sync error = %v", err)
		isoSnap.URLs = append(isoSnap.URLs, "https://pages.test/tampered")
		isoSnap.Added = append(isoSnap.Added, "https://pages.test/tampered")
		again, err := isolationSession.Sync("incremental", nil)
		c.Check(err == nil, "isolation second Sync error = %v", err)
		c.Check(sortedEqual(again.URLs, []string{"https://pages.test/only-b", "https://pages.test/shared"}),
			"mutating returned slices does not affect session state, got %v", again.URLs)
		c.Check(len(again.Added) == 0, "no diff after unchanged round, got %v", again.Added)
	})
}

// TestSessionFailure covers atomic failure handling, retry behavior and cycles.
func TestSessionFailure(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		docs := map[string]string{
			rootURL: indexXML(
				indexEntry(leafAURL, "2024-01-01"),
				indexEntry(midURL, "2024-01-01"),
			),
			midURL:   indexXML(indexEntry(leafBURL, "2024-01-01")),
			leafAURL: urlsetXML("https://pages.test/a"),
			leafBURL: urlsetXML("https://pages.test/b"),
		}
		srv := newFakeServer(docs)
		withFakeServer(t, srv)

		session, err := NewSession(rootURL)
		c.Check(err == nil, "NewSession error = %v", err)

		first, err := session.Sync("full", nil)
		c.Check(err == nil, "baseline Sync error = %v", err)
		c.Check(sortedEqual(first.URLs, []string{"https://pages.test/a", "https://pages.test/b"}),
			"baseline pages = %v", first.URLs)

		// New parent dates force every child to be re-downloaded; make one of
		// them fail mid-round.
		srv.set(rootURL, indexXML(
			indexEntry(leafAURL, "2024-09-09"),
			indexEntry(midURL, "2024-09-09"),
		))
		srv.set(midURL, indexXML(indexEntry(leafBURL, "2024-09-09")))
		srv.fail[leafBURL] = true
		srv.resetRequests()

		failed, err := session.Sync("incremental", nil)
		c.Check(err != nil, "fetch failure must surface an error")
		c.Check(len(failed.URLs) == 0 && len(failed.Added) == 0 && len(failed.Removed) == 0,
			"failed round returns empty Snapshot, got %v", failed)
		c.Check(srv.count(leafAURL) == 1 && srv.count(midURL) == 1 && srv.count(leafBURL) == 1,
			"all re-declared documents were attempted before the failure, got %v", srv.requests)
		srv.fail[leafBURL] = false

		// Half-round new dates must not become reuse evidence: the saved state
		// still carries the old dates, so the retry has to download again.
		srv.resetRequests()
		retry, err := session.Sync("incremental", nil)
		c.Check(err == nil, "retry Sync error = %v", err)
		c.Check(srv.count(leafAURL) == 1, "uncommitted new date cannot skip the retry download, got %d", srv.count(leafAURL))
		c.Check(srv.count(midURL) == 1 && srv.count(leafBURL) == 1,
			"retry re-downloads the whole changed branch, got %v", srv.requests)
		c.Check(sortedEqual(retry.URLs, []string{"https://pages.test/a", "https://pages.test/b"}),
			"retry rebuilds the same pages, got %v", retry.URLs)
		c.Check(len(retry.Added) == 0 && len(retry.Removed) == 0,
			"retry against unchanged committed state has no diff, got +%v -%v", retry.Added, retry.Removed)

		// Root fetch failure.
		srv.fail[rootURL] = true
		rootSnap, err := session.Sync("incremental", nil)
		c.Check(err != nil, "root failure errors")
		c.Check(len(rootSnap.URLs) == 0, "root failure gives empty Snapshot")
		srv.fail[rootURL] = false

		// Parse failure also aborts without committing. A fresh parent date
		// makes the malformed document download mandatory.
		srv.set(rootURL, indexXML(
			indexEntry(leafAURL, "2024-10-10"),
			indexEntry(midURL, "2024-09-09"),
		))
		srv.set(leafAURL, "<urlset><url><loc>broken")
		_, err = session.Sync("incremental", nil)
		c.Check(err != nil, "malformed document errors")
		srv.set(leafAURL, urlsetXML("https://pages.test/a"))
		parsed, err := session.Sync("incremental", nil)
		c.Check(err == nil, "retry after parse failure succeeds, got %v", err)
		c.Check(sortedEqual(parsed.URLs, []string{"https://pages.test/a", "https://pages.test/b"}),
			"state survived the parse failure, got %v", parsed.URLs)

		// Cycles error, while shared acyclic subgraphs keep working.
		cycleDocs := map[string]string{
			rootURL: indexXML(indexEntry(midURL, "")),
			midURL:  indexXML(indexEntry(rootURL, "")),
		}
		cycleSrv := newFakeServer(cycleDocs)
		withFakeServer(t, cycleSrv)
		cycleSession, err := NewSession(rootURL)
		c.Check(err == nil, "cycle session error = %v", err)
		_, err = cycleSession.Sync("full", nil)
		c.Check(err != nil, "session must reject a cycle")
		_, err = Get(rootURL, nil)
		c.Check(err != nil, "Get rejects cycles instead of looping")

		sharedDocs := map[string]string{
			rootURL: indexXML(indexEntry(midURL, ""), indexEntry(sharedURL, "")),
			midURL:  indexXML(indexEntry(sharedURL, "")),
			sharedURL: urlsetXML(
				"https://pages.test/shared-1",
				"https://pages.test/shared-2",
			),
		}
		sharedSrv := newFakeServer(sharedDocs)
		withFakeServer(t, sharedSrv)
		sharedSession, err := NewSession(rootURL)
		c.Check(err == nil, "shared session error = %v", err)
		sharedSnap, err := sharedSession.Sync("full", nil)
		c.Check(err == nil, "shared acyclic graph Sync error = %v", err)
		c.Check(sharedSrv.count(sharedURL) == 1, "shared subgraph downloaded once, got %d", sharedSrv.count(sharedURL))
		c.Check(sortedEqual(sharedSnap.URLs, []string{"https://pages.test/shared-1", "https://pages.test/shared-2"}),
			"shared subgraph pages = %v", sharedSnap.URLs)
	})
}
