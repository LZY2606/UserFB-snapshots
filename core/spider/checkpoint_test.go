package spider

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/hu17889/go_spider/core/common/page"
	"github.com/hu17889/go_spider/core/common/request"
	"github.com/hu17889/go_spider/core/scheduler"
)

// ---------------------------------------------------------------------------
// Deterministic test doubles
// ---------------------------------------------------------------------------

type fakeDownloader struct {
	mu sync.Mutex

	counts map[string]int
	// failAlways marks urls that never download successfully.
	failAlways map[string]bool
	// failFirst fails the first n attempts of a url, then succeeds.
	failFirst map[string]int

	// hook is invoked inside Download, before returning. It may block.
	hook func(url string)
}

func newFakeDownloader() *fakeDownloader {
	return &fakeDownloader{
		counts:     make(map[string]int),
		failAlways: make(map[string]bool),
		failFirst:  make(map[string]int),
	}
}

func (d *fakeDownloader) Download(req *request.Request) *page.Page {
	d.mu.Lock()
	d.counts[req.GetUrl()]++
	attempts := d.counts[req.GetUrl()]
	alwaysFail := d.failAlways[req.GetUrl()]
	failBudget := d.failFirst[req.GetUrl()]
	hook := d.hook
	d.mu.Unlock()

	if hook != nil {
		hook(req.GetUrl())
	}

	p := page.NewPage(req)
	if alwaysFail || attempts <= failBudget {
		p.SetStatus(true, "simulated download failure")
		return p
	}
	p.SetBodyStr("body:"+req.GetUrl()).SetStatus(false, "")
	return p
}

func (d *fakeDownloader) callCount(url string) int {
	d.mu.Lock()
	defer d.mu.Unlock()
	return d.counts[url]
}

// linkProcessor is a PageProcesser over a static url -> child links graph.
type linkProcessor struct {
	links map[string][]string

	mu        sync.Mutex
	processed []string
	finishes  int

	// onPage runs while processing the page, before its commit.
	onPage func(url string)
	// finishHook runs inside Finish.
	finishHook func()
}

func (p *linkProcessor) Process(pg *page.Page) {
	url := pg.GetRequest().GetUrl()
	p.mu.Lock()
	p.processed = append(p.processed, url)
	onPage := p.onPage
	p.mu.Unlock()

	if onPage != nil {
		onPage(url)
	}
	for _, child := range p.links[url] {
		pg.AddTargetRequest(child, "text")
	}
}

func (p *linkProcessor) Finish() {
	p.mu.Lock()
	p.finishes++
	hook := p.finishHook
	p.mu.Unlock()
	if hook != nil {
		hook()
	}
}

func (p *linkProcessor) processedSnapshot() []string {
	p.mu.Lock()
	defer p.mu.Unlock()
	return append([]string{}, p.processed...)
}

func newCheckpointSpider(proc *linkProcessor, dl *fakeDownloader) *Spider {
	return NewSpider(proc, "checkpoint-test").CloseStrace().
		SetDownloader(dl).
		SetThreadnum(1)
}

func readSnapshotFile(t *testing.T, dir string) snapshot {
	t.Helper()
	data, err := os.ReadFile(filepath.Join(dir, checkpointFileName))
	if err != nil {
		t.Fatalf("read snapshot: %v", err)
	}
	var snap snapshot
	if err := json.Unmarshal(data, &snap); err != nil {
		t.Fatalf("decode snapshot: %v", err)
	}
	return snap
}

func snapshotURLs(reqs []snapshotRequest) []string {
	out := make([]string, 0, len(reqs))
	for _, r := range reqs {
		out = append(out, r.Url)
	}
	return out
}

func (s snapshot) doneURLs() []string {
	return append([]string{}, s.Done...)
}

func containsURL(urls []string, want string) bool {
	for _, u := range urls {
		if u == want {
			return true
		}
	}
	return false
}

// ---------------------------------------------------------------------------
// In-process tests
// ---------------------------------------------------------------------------

// TestCheckpointHappyPathAndDedup runs a small graph with a cycle
// (root -> a -> root, plus duplicate links) straight through and checks that
// every url is handled once and Finish runs exactly once.
func TestCheckpointHappyPathAndDedup(t *testing.T) {
	dir := t.TempDir()
	proc := &linkProcessor{links: map[string][]string{
		"http://site/root": {"http://site/a", "http://site/a"},
		"http://site/a":    {"http://site/root", "http://site/b"},
		"http://site/b":    {"http://site/root"},
	}}
	dl := newFakeDownloader()
	sp := newCheckpointSpider(proc, dl)
	sp.AddUrl("http://site/root", "text")

	if err := sp.RunCheckpoint(context.Background(), dir); err != nil {
		t.Fatalf("RunCheckpoint: %v", err)
	}

	want := map[string]bool{"http://site/root": true, "http://site/a": true, "http://site/b": true}
	got := map[string]bool{}
	for _, u := range proc.processedSnapshot() {
		if got[u] {
			t.Errorf("url %q processed more than once", u)
		}
		got[u] = true
	}
	if len(got) != len(want) {
		t.Fatalf("processed %v, want %v", got, want)
	}
	for u := range want {
		if !got[u] {
			t.Errorf("missing processed url %q", u)
		}
		if dl.callCount(u) != 1 {
			t.Errorf("url %q downloaded %d times, want 1", u, dl.callCount(u))
		}
	}
	if proc.finishes != 1 {
		t.Errorf("Finish called %d times, want 1", proc.finishes)
	}

	snap := readSnapshotFile(t, dir)
	if !snap.Finished || len(snap.Queued) != 0 || len(snap.Inflight) != 0 {
		t.Fatalf("snapshot not in finished state: %+v", snap)
	}

	// Reopening a finished crawl is a no-op and must not Finish again.
	again := newCheckpointSpider(&linkProcessor{links: proc.links}, newFakeDownloader())
	again.AddUrl("http://site/root", "text")
	if err := again.RunCheckpoint(context.Background(), dir); err != nil {
		t.Fatalf("reopen finished checkpoint: %v", err)
	}
	if again.pPageProcesser.(*linkProcessor).finishes != 0 {
		t.Error("Finish called again on an already finished checkpoint")
	}
}

// TestCheckpointPersistsRequestFields makes sure a round trip through the
// snapshot keeps method, postdata, urltag, header, cookies and proxyHost,
// instead of resurrecting everything as a GET request.
func TestCheckpointPersistsRequestFields(t *testing.T) {
	dir := t.TempDir()

	header := http.Header{}
	header.Set("X-Test", "kept")
	cookies := []*http.Cookie{{Name: "sid", Value: "abc"}}
	original := request.NewRequestWithProxy(
		"http://site/post", "text", "my-tag", "POST", "a=1&b=2",
		header, cookies, "http://proxy.example:8080", nil, nil,
	)

	store, _, err := loadCheckpoint(dir)
	if err != nil {
		t.Fatalf("loadCheckpoint: %v", err)
	}
	if err := store.bootstrap([]*request.Request{original}); err != nil {
		t.Fatalf("bootstrap: %v", err)
	}
	claimed, err := store.claim()
	if err != nil || claimed == nil {
		t.Fatalf("claim: req=%v err=%v", claimed, err)
	}

	// Reload straight from disk: the in-flight entry must hold every field.
	reopened, _, err := loadCheckpoint(dir)
	if err != nil {
		t.Fatalf("reload: %v", err)
	}
	if len(reopened.inflight) != 1 {
		t.Fatalf("inflight = %d, want 1", len(reopened.inflight))
	}
	got := reopened.inflight[0]
	if got.Method != "POST" || got.Postdata != "a=1&b=2" || got.Urltag != "my-tag" ||
		got.RespType != "text" || got.ProxyHost != "http://proxy.example:8080" {
		t.Errorf("scalar fields lost: %+v", got)
	}
	if got.Header.Get("X-Test") != "kept" {
		t.Errorf("header lost: %+v", got.Header)
	}
	if len(got.Cookies) != 1 || got.Cookies[0].Name != "sid" || got.Cookies[0].Value != "abc" {
		t.Errorf("cookies lost: %+v", got.Cookies)
	}
	if got.GetRedirectFunc() != nil || got.GetMeta() != nil {
		t.Error("restored request should carry no redirect func or meta")
	}

	// First-admission parameters win for duplicate urls.
	dir2 := t.TempDir()
	store2, _, _ := loadCheckpoint(dir2)
	second := request.NewRequest("http://site/post", "text", "other-tag", "GET", "x=9", nil, nil, nil, nil)
	if err := store2.bootstrap([]*request.Request{original, second}); err != nil {
		t.Fatalf("bootstrap dup: %v", err)
	}
	if len(store2.queued) != 1 {
		t.Fatalf("duplicate url admitted twice: %+v", store2.queued)
	}
	if store2.queued[0].Method != "POST" || store2.queued[0].Urltag != "my-tag" {
		t.Errorf("first-admission parameters were overwritten: %+v", store2.queued[0])
	}

	// End to end: the downloader must see a POST with all parameters after
	// the request has been persisted and claimed.
	dl := newFakeDownloader()
	dl.hook = func(url string) {}
	proc := &linkProcessor{links: map[string][]string{}}
	sp := newCheckpointSpider(proc, dl)
	sp.AddRequest(request.NewRequestWithProxy(
		"http://site/post2", "text", "tag2", "POST", "k=v",
		header, cookies, "http://proxy.example:8080", nil, nil,
	))
	dir3 := t.TempDir()
	if err := sp.RunCheckpoint(context.Background(), dir3); err != nil {
		t.Fatalf("RunCheckpoint post: %v", err)
	}
	if dl.callCount("http://site/post2") != 1 {
		t.Errorf("post page downloads = %d, want 1", dl.callCount("http://site/post2"))
	}
}

// TestCheckpointResumeAfterContextCancel cancels mid-crawl and then resumes
// from the same directory. Already-started pages finish and save; nothing new
// is claimed after cancellation.
func TestCheckpointResumeAfterContextCancel(t *testing.T) {
	dir := t.TempDir()
	links := map[string][]string{}
	pages := []string{"p0", "p1", "p2", "p3", "p4"}
	for i := range pages {
		if i+1 < len(pages) {
			links["http://chain/"+pages[i]] = []string{"http://chain/" + pages[i+1]}
		}
	}

	started := make(chan string, 4)
	release := make(chan struct{})
	dl := newFakeDownloader()
	dl.hook = func(url string) {
		started <- url
		if url == "http://chain/p1" {
			<-release // block the second page until the test cancels
		}
	}

	proc := &linkProcessor{links: links}
	sp := newCheckpointSpider(proc, dl)
	sp.AddUrl("http://chain/p0", "text")

	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- sp.RunCheckpoint(ctx, dir) }()

	select {
	case u := <-started:
		if u != "http://chain/p0" {
			t.Fatalf("first download was %q", u)
		}
	case <-time.After(10 * time.Second):
		t.Fatal("first page never started")
	}
	select {
	case u := <-started:
		if u != "http://chain/p1" {
			t.Fatalf("second download was %q", u)
		}
	case <-time.After(10 * time.Second):
		t.Fatal("second page never started")
	}

	cancel()
	close(release)

	select {
	case err := <-done:
		if !errors.Is(err, context.Canceled) {
			t.Fatalf("RunCheckpoint err = %v, want context.Canceled", err)
		}
	case <-time.After(15 * time.Second):
		t.Fatal("RunCheckpoint did not return after cancellation")
	}

	// No page beyond the two already started may have been fetched.
	for _, p := range []string{"p2", "p3", "p4"} {
		if dl.callCount("http://chain/"+p) != 0 {
			t.Errorf("page %s downloaded after cancellation", p)
		}
	}

	snap := readSnapshotFile(t, dir)
	if snap.Finished {
		t.Fatal("cancelled crawl must not be marked finished")
	}

	// Resume with a plain, non-blocking downloader and crawl the rest.
	dl2 := newFakeDownloader()
	proc2 := &linkProcessor{links: links}
	sp2 := newCheckpointSpider(proc2, dl2)
	if err := sp2.RunCheckpoint(context.Background(), dir); err != nil {
		t.Fatalf("resume RunCheckpoint: %v", err)
	}

	all := map[string]bool{}
	for _, u := range proc.processedSnapshot() {
		all[u] = true
	}
	for _, u := range proc2.processedSnapshot() {
		all[u] = true
	}
	if len(all) != len(pages) {
		t.Fatalf("after resume processed %v, want all %d pages", all, len(pages))
	}
	if proc2.finishes != 1 {
		t.Errorf("Finish after resume = %d, want 1", proc2.finishes)
	}
}

// TestCheckpointDownloadFailureKeepsRecoverableRequest keeps the three-attempt
// rule, then retains the request and returns an error instead of looping
// forever or marking the page done. A later resume can finish it.
func TestCheckpointDownloadFailureKeepsRecoverableRequest(t *testing.T) {
	dir := t.TempDir()
	dl := newFakeDownloader()
	dl.failAlways["http://flaky/page"] = true
	proc := &linkProcessor{links: map[string][]string{}}
	sp := newCheckpointSpider(proc, dl)
	sp.AddUrl("http://flaky/page", "text")

	err := sp.RunCheckpoint(context.Background(), dir)
	if err == nil {
		t.Fatal("expected error after exhausted retries")
	}
	if dl.callCount("http://flaky/page") != 3 {
		t.Errorf("attempts = %d, want 3", dl.callCount("http://flaky/page"))
	}

	snap := readSnapshotFile(t, dir)
	if containsURL(snap.doneURLs(), "http://flaky/page") {
		t.Fatal("failed request must not be recorded as done")
	}
	retained := append(snapshotURLs(snap.Queued), snapshotURLs(snap.Inflight)...)
	if !containsURL(retained, "http://flaky/page") {
		t.Fatalf("failed request was dropped: %+v", snap)
	}

	// Resume: the same url now succeeds on the first attempt.
	dl2 := newFakeDownloader()
	proc2 := &linkProcessor{links: map[string][]string{}}
	sp2 := newCheckpointSpider(proc2, dl2)
	if err := sp2.RunCheckpoint(context.Background(), dir); err != nil {
		t.Fatalf("resume after failure: %v", err)
	}
	if dl2.callCount("http://flaky/page") != 1 {
		t.Errorf("resume downloads = %d, want 1", dl2.callCount("http://flaky/page"))
	}
	if !containsURL(proc2.processedSnapshot(), "http://flaky/page") {
		t.Error("flaky page never processed after resume")
	}
}

// TestCheckpointPanicRequeuesAndReplays turns processor and pipeline panics
// into errors, keeps the page recoverable and completes it on resume.
func TestCheckpointPanicRequeuesAndReplays(t *testing.T) {
	dir := t.TempDir()
	proc := &linkProcessor{
		links: map[string][]string{},
		onPage: func(url string) {
			if url == "http://panic/page" {
				panic("boom from processor")
			}
		},
	}
	sp := newCheckpointSpider(proc, newFakeDownloader())
	sp.AddUrl("http://panic/page", "text")

	err := sp.RunCheckpoint(context.Background(), dir)
	if err == nil || !strings.Contains(err.Error(), "panicked") {
		t.Fatalf("want panic error, got %v", err)
	}
	snap := readSnapshotFile(t, dir)
	retained := append(snapshotURLs(snap.Queued), snapshotURLs(snap.Inflight)...)
	if !containsURL(retained, "http://panic/page") {
		t.Fatalf("panicking page lost: %+v", snap)
	}

	proc2 := &linkProcessor{links: map[string][]string{}}
	sp2 := newCheckpointSpider(proc2, newFakeDownloader())
	if err := sp2.RunCheckpoint(context.Background(), dir); err != nil {
		t.Fatalf("resume after panic: %v", err)
	}
	if !containsURL(proc2.processedSnapshot(), "http://panic/page") {
		t.Error("panicking page not replayed after resume")
	}
}

// TestCheckpointAtomicParentChildCommit verifies there is no persisted state
// with the parent done but its children missing: when the commit write fails,
// the old snapshot stays readable and the page replays after recovery.
func TestCheckpointAtomicParentChildCommit(t *testing.T) {
	dir := t.TempDir()
	proc := &linkProcessor{
		links: map[string][]string{
			"http://tree/parent": {"http://tree/c1", "http://tree/c2"},
		},
		onPage: func(url string) {
			if url == "http://tree/parent" {
				// Make the snapshot directory unwritable so the atomic
				// commit cannot land its temp file.
				if err := os.Chmod(dir, 0o555); err != nil {
					t.Errorf("chmod: %v", err)
				}
			}
		},
	}
	t.Cleanup(func() { _ = os.Chmod(dir, 0o755) })

	sp := newCheckpointSpider(proc, newFakeDownloader())
	sp.AddUrl("http://tree/parent", "text")
	err := sp.RunCheckpoint(context.Background(), dir)
	if err == nil {
		t.Fatal("expected commit error when snapshot dir is read-only")
	}

	if err := os.Chmod(dir, 0o755); err != nil {
		t.Fatalf("restore chmod: %v", err)
	}

	// Old snapshot is still readable: parent is still in flight, children
	// were never persisted.
	snap := readSnapshotFile(t, dir)
	doneURLs := snap.doneURLs()
	if containsURL(doneURLs, "http://tree/parent") {
		t.Fatal("parent marked done despite failed commit")
	}
	if containsURL(snapshotURLs(snap.Queued), "http://tree/c1") ||
		containsURL(snapshotURLs(snap.Queued), "http://tree/c2") {
		t.Fatal("children persisted without the parent commit")
	}

	// Resume: parent replays and the whole family commits together.
	proc2 := &linkProcessor{links: proc.links}
	sp2 := newCheckpointSpider(proc2, newFakeDownloader())
	if err := sp2.RunCheckpoint(context.Background(), dir); err != nil {
		t.Fatalf("resume after failed commit: %v", err)
	}
	snap2 := readSnapshotFile(t, dir)
	finalDone := snap2.doneURLs()
	for _, u := range []string{"http://tree/parent", "http://tree/c1", "http://tree/c2"} {
		if !containsURL(finalDone, u) {
			t.Errorf("after resume %q not done: %+v", u, snap2)
		}
	}
	if len(snap2.Queued) != 0 || len(snap2.Inflight) != 0 || !snap2.Finished {
		t.Fatalf("crawl not finished cleanly: %+v", snap2)
	}
}

// TestCheckpointRejectsUnsupportedRequests rejects custom redirect callbacks
// and non-nil Meta before persistence/download, and validates snapshot files.
func TestCheckpointRejectsUnsupportedRequests(t *testing.T) {
	t.Run("redirect callback", func(t *testing.T) {
		dir := t.TempDir()
		sp := newCheckpointSpider(&linkProcessor{}, newFakeDownloader())
		sp.AddRequest(request.NewRequest("http://site/r", "text", "", "GET", "", nil, nil,
			func(*http.Request, []*http.Request) error { return nil }, nil))
		err := sp.RunCheckpoint(context.Background(), dir)
		if err == nil || !strings.Contains(err.Error(), "redirect") {
			t.Fatalf("want redirect error, got %v", err)
		}
		if _, statErr := os.Stat(filepath.Join(dir, checkpointFileName)); !os.IsNotExist(statErr) {
			t.Fatal("unsupported seed must not create a snapshot")
		}
	})

	t.Run("non-nil meta", func(t *testing.T) {
		dir := t.TempDir()
		sp := newCheckpointSpider(&linkProcessor{}, newFakeDownloader())
		sp.AddRequest(request.NewRequest("http://site/m", "text", "", "GET", "", nil, nil, nil,
			map[string]string{"nope": "1"}))
		err := sp.RunCheckpoint(context.Background(), dir)
		if err == nil || !strings.Contains(err.Error(), "Meta") {
			t.Fatalf("want meta error, got %v", err)
		}
	})

	t.Run("meta child rejected at commit", func(t *testing.T) {
		dir := t.TempDir()
		store, _, err := loadCheckpoint(dir)
		if err != nil {
			t.Fatal(err)
		}
		root := request.NewRequest("http://site/root2", "text", "", "GET", "", nil, nil, nil, nil)
		if err := store.bootstrap([]*request.Request{root}); err != nil {
			t.Fatal(err)
		}
		claimed, err := store.claim()
		if err != nil || claimed == nil {
			t.Fatalf("claim: %v", err)
		}
		badChild := request.NewRequest("http://site/child", "text", "", "GET", "", nil, nil, nil, 42)
		if err := store.commit(root, []*request.Request{badChild}); err == nil {
			t.Fatal("commit with a Meta child must fail and leave the old state")
		}
		// The failed commit must not advance the in-memory frontier either.
		if len(store.inflight) != 1 || store.inflight[0].Url != root.Url {
			t.Fatalf("inflight changed after failed commit: %+v", store.inflight)
		}
	})

	t.Run("custom scheduler rejected", func(t *testing.T) {
		dir := t.TempDir()
		sp := NewSpider(&linkProcessor{}, "x").CloseStrace().
			SetScheduler(scheduler.NewSimpleScheduler()).
			SetDownloader(newFakeDownloader())
		sp.AddUrl("http://site/s", "text")
		if err := sp.RunCheckpoint(context.Background(), dir); err == nil {
			t.Fatal("custom scheduler must be rejected")
		}
	})
}

// TestCheckpointSnapshotValidation covers the on-disk format contract:
// version field, missing file creation, truncation, unknown version and
// incomplete requests.
func TestCheckpointSnapshotValidation(t *testing.T) {
	t.Run("missing file is created", func(t *testing.T) {
		dir := t.TempDir()
		store, existed, err := loadCheckpoint(dir)
		if err != nil || existed {
			t.Fatalf("load missing: existed=%v err=%v", existed, err)
		}
		if err := store.bootstrap(nil); err != nil {
			t.Fatalf("bootstrap empty: %v", err)
		}
		snap := readSnapshotFile(t, dir)
		if snap.Version != snapshotVersion {
			t.Fatalf("version = %d, want %d", snap.Version, snapshotVersion)
		}
	})

	cases := map[string]string{
		"truncated":          `{"version":1,"queued":[`,
		"unknown version":    `{"version":99,"queued":[],"inflight":[],"done":[]}`,
		"incomplete request": `{"version":1,"queued":[{"url":""}],"inflight":[],"done":[]}`,
		"not json":           `this is not json at all`,
	}
	for name, content := range cases {
		t.Run(name, func(t *testing.T) {
			dir := t.TempDir()
			if err := os.WriteFile(filepath.Join(dir, checkpointFileName), []byte(content), 0o644); err != nil {
				t.Fatal(err)
			}
			if _, _, err := loadCheckpoint(dir); err == nil {
				t.Fatal("expected snapshot error")
			}
			sp := newCheckpointSpider(&linkProcessor{}, newFakeDownloader())
			sp.AddUrl("http://site/x", "text")
			if err := sp.RunCheckpoint(context.Background(), dir); err == nil {
				t.Fatal("RunCheckpoint must reject a corrupt snapshot")
			}
		})
	}
}

// ---------------------------------------------------------------------------
// Subprocess tests: real process exits and resume
// ---------------------------------------------------------------------------

const (
	checkpointChildEnv  = "GO_SPIDER_CHECKPOINT_CHILD"
	checkpointDirEnv    = "GO_SPIDER_CHECKPOINT_DIR"
	checkpointExitPoint = 7
)

// TestMain routes re-executed test binaries into the child scenario driver.
func TestMain(m *testing.M) {
	if mode := os.Getenv(checkpointChildEnv); mode != "" {
		os.Exit(runCheckpointChild(mode, os.Getenv(checkpointDirEnv)))
	}
	os.Exit(m.Run())
}

func childLogPath(dir string) string  { return filepath.Join(dir, "child.log") }
func childSentPath(dir string) string { return filepath.Join(dir, "sentinel") }

func appendChildLog(dir, line string) {
	f, err := os.OpenFile(childLogPath(dir), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644)
	if err != nil {
		fmt.Fprintln(os.Stderr, "open log:", err)
		return
	}
	defer f.Close()
	fmt.Fprintln(f, line)
}

func readChildLog(t *testing.T, dir string) []string {
	t.Helper()
	data, err := os.ReadFile(childLogPath(dir))
	if err != nil {
		t.Fatalf("read child log: %v", err)
	}
	lines := strings.Split(strings.TrimSpace(string(data)), "\n")
	if len(lines) == 1 && lines[0] == "" {
		return nil
	}
	return lines
}

// runCheckpointChild drives one scenario inside a fresh process. It exits
// with checkpointExitPoint at deliberate crash points, 0 on clean completion
// and 1 on unexpected errors.
func runCheckpointChild(mode, dir string) int {
	links := map[string][]string{
		"http://site/root": {"http://site/a", "http://site/a"},
		"http://site/a":    {"http://site/root", "http://site/b"},
		"http://site/b":    {},
	}

	switch mode {
	case "restart1", "restart2", "restart3":
		proc := &linkProcessor{links: links}
		proc.onPage = func(url string) {
			appendChildLog(dir, "process:"+url)
			if mode == "restart1" && url == "http://site/b" {
				// Hard exit before this page's commit: root is already durably
				// done, b is in flight, a is queued.
				os.Exit(checkpointExitPoint)
			}
		}
		dl := newFakeDownloader()
		dl.hook = func(url string) { appendChildLog(dir, "download:"+url) }
		sp := newCheckpointSpider(proc, dl)
		sp.AddUrl("http://site/root", "text")
		sp.AddUrl("http://site/b", "text")
		proc.finishHook = func() { appendChildLog(dir, "finish") }
		if err := sp.RunCheckpoint(context.Background(), dir); err != nil {
			fmt.Fprintln(os.Stderr, "child RunCheckpoint:", err)
			return 1
		}
		return 0

	case "kill-inflight":
		dl := newFakeDownloader()
		dl.hook = func(url string) {
			if url == "http://q/a" {
				// Signal the parent that the claim is durable and the
				// download has started, then park until killed.
				if err := os.WriteFile(childSentPath(dir), []byte("a"), 0o644); err != nil {
					fmt.Fprintln(os.Stderr, "sentinel:", err)
				}
				select {}
			}
		}
		sp := newCheckpointSpider(&linkProcessor{links: map[string][]string{}}, dl)
		sp.AddUrl("http://q/a", "text")
		sp.AddUrl("http://q/b", "text")
		sp.AddUrl("http://q/c", "text")
		if err := sp.RunCheckpoint(context.Background(), dir); err != nil {
			fmt.Fprintln(os.Stderr, "child RunCheckpoint:", err)
			return 1
		}
		return 0

	case "kill-resume":
		dl := newFakeDownloader()
		dl.hook = func(url string) { appendChildLog(dir, "download:"+url) }
		sp := newCheckpointSpider(&linkProcessor{links: map[string][]string{}}, dl)
		if err := sp.RunCheckpoint(context.Background(), dir); err != nil {
			fmt.Fprintln(os.Stderr, "child resume:", err)
			return 1
		}
		appendChildLog(dir, "finish")
		return 0

	default:
		fmt.Fprintln(os.Stderr, "unknown child mode:", mode)
		return 1
	}
}

func runChildScenario(t *testing.T, mode, dir string) *exec.Cmd {
	t.Helper()
	cmd := exec.Command(os.Args[0], "-test.run=^TestMain$")
	cmd.Env = append(os.Environ(),
		checkpointChildEnv+"="+mode,
		checkpointDirEnv+"="+dir,
	)
	if err := cmd.Start(); err != nil {
		t.Fatalf("start child %s: %v", mode, err)
	}
	return cmd
}

func waitForFile(t *testing.T, path string) {
	t.Helper()
	deadline := time.Now().Add(15 * time.Second)
	for time.Now().Before(deadline) {
		if _, err := os.Stat(path); err == nil {
			return
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatalf("file %s never appeared", path)
}

// TestCheckpointConcurrent exercises the same guarantees with several
// workers: every url is processed exactly once and the crawl drains cleanly.
func TestCheckpointConcurrent(t *testing.T) {
	dir := t.TempDir()
	links := map[string][]string{}
	var seeds []string
	for i := 0; i < 40; i++ {
		u := fmt.Sprintf("http://grid/n%d", i)
		seeds = append(seeds, u)
		var children []string
		for j := 0; j < 3; j++ {
			children = append(children, fmt.Sprintf("http://grid/n%d", (i*7+j*13)%40))
		}
		// Throw in a self loop and a cross seed to force dedup under load.
		children = append(children, u, "http://grid/n0")
		links[u] = children
	}

	proc := &linkProcessor{links: links}
	sp := NewSpider(proc, "concurrent").CloseStrace().
		SetDownloader(newFakeDownloader()).
		SetThreadnum(4)
	for _, u := range seeds {
		sp.AddUrl(u, "text")
	}

	if err := sp.RunCheckpoint(context.Background(), dir); err != nil {
		t.Fatalf("RunCheckpoint: %v", err)
	}
	got := map[string]int{}
	for _, u := range proc.processedSnapshot() {
		got[u]++
	}
	if len(got) != len(seeds) {
		t.Fatalf("processed %d distinct urls, want %d", len(got), len(seeds))
	}
	for u, n := range got {
		if n != 1 {
			t.Errorf("url %q processed %d times, want 1", u, n)
		}
	}
	if proc.finishes != 1 {
		t.Errorf("Finish = %d, want 1", proc.finishes)
	}
	snap := readSnapshotFile(t, dir)
	if len(snap.Done) != len(seeds) || !snap.Finished {
		t.Fatalf("final snapshot wrong: done=%d finished=%v", len(snap.Done), snap.Finished)
	}
}

// TestCheckpointRestartAcrossProcess kills the crawler at a deliberate exit
// point and resumes it in brand-new processes, checking replay, cycle
// de-duplication, in-flight recovery and the single Finish call.
func TestCheckpointRestartAcrossProcess(t *testing.T) {
	dir := t.TempDir()

	cmd := runChildScenario(t, "restart1", dir)
	err := cmd.Wait()
	var exitErr *exec.ExitError
	if !errors.As(err, &exitErr) || exitErr.ExitCode() != checkpointExitPoint {
		t.Fatalf("phase1 exit = %v, want exit code %d", err, checkpointExitPoint)
	}

	snap := readSnapshotFile(t, dir)
	if snap.Finished {
		t.Fatal("phase1 must not be finished")
	}
	if got := snapshotURLs(snap.Inflight); len(got) != 1 || got[0] != "http://site/b" {
		t.Errorf("inflight = %v, want [b]", got)
	}
	if got := snapshotURLs(snap.Queued); len(got) != 1 || got[0] != "http://site/a" {
		t.Errorf("queued = %v, want [a]", got)
	}
	if !containsURL(snap.doneURLs(), "http://site/root") {
		t.Errorf("root should be done: %+v", snap)
	}

	cmd = runChildScenario(t, "restart2", dir)
	if err := cmd.Wait(); err != nil {
		t.Fatalf("phase2: %v", err)
	}

	snap = readSnapshotFile(t, dir)
	if !snap.Finished {
		t.Fatal("phase2 should finish the crawl")
	}
	done := snap.doneURLs()
	for _, u := range []string{"http://site/root", "http://site/a", "http://site/b"} {
		if !containsURL(done, u) {
			t.Errorf("missing done url %q: %v", u, done)
		}
	}

	lines := readChildLog(t, dir)
	var downloads, finishes []string
	for _, line := range lines {
		switch {
		case strings.HasPrefix(line, "download:"):
			downloads = append(downloads, strings.TrimPrefix(line, "download:"))
		case line == "finish":
			finishes = append(finishes, line)
		}
	}
	countDownloads := func(u string) int {
		n := 0
		for _, d := range downloads {
			if d == u {
				n++
			}
		}
		return n
	}
	if countDownloads("http://site/root") != 1 {
		t.Errorf("root downloads = %d, want 1", countDownloads("http://site/root"))
	}
	if countDownloads("http://site/b") != 2 {
		t.Errorf("in-flight b downloads = %d, want 2 (replayed once)", countDownloads("http://site/b"))
	}
	if countDownloads("http://site/a") != 1 {
		t.Errorf("a downloads = %d, want 1", countDownloads("http://site/a"))
	}
	// The first download logged by the second process is b (in-flight first).
	// Both processes share one log; phase1 logged root then b, so phase2's
	// first entry is the third download line.
	if len(downloads) < 3 || downloads[2] != "http://site/b" {
		t.Errorf("resume did not dispatch in-flight b first: %v", downloads)
	}
	if len(finishes) != 1 {
		t.Errorf("Finish lines = %d, want exactly 1 across all processes", len(finishes))
	}

	// Third process over the finished directory: no work, no Finish.
	cmd = runChildScenario(t, "restart3", dir)
	if err := cmd.Wait(); err != nil {
		t.Fatalf("phase3: %v", err)
	}
	lines = readChildLog(t, dir)
	finishes = nil
	for _, line := range lines {
		if line == "finish" {
			finishes = append(finishes, line)
		}
	}
	if len(finishes) != 1 {
		t.Errorf("finished directory triggered Finish again: %d lines", len(finishes))
	}
}

// TestCheckpointInFlightRestoredFirst SIGKILLs a process while a request is
// in flight and checks that recovery puts it ahead of the untouched queue,
// preserving claim order.
func TestCheckpointInFlightRestoredFirst(t *testing.T) {
	dir := t.TempDir()

	cmd := runChildScenario(t, "kill-inflight", dir)
	waitForFile(t, childSentPath(dir))
	if err := cmd.Process.Kill(); err != nil {
		t.Fatalf("kill child: %v", err)
	}
	_ = cmd.Wait()

	snap := readSnapshotFile(t, dir)
	if got := snapshotURLs(snap.Inflight); len(got) != 1 || got[0] != "http://q/a" {
		t.Fatalf("inflight after kill = %v, want [a]", got)
	}
	if got := snapshotURLs(snap.Queued); len(got) != 2 || got[0] != "http://q/b" || got[1] != "http://q/c" {
		t.Fatalf("queued after kill = %v, want [b c]", got)
	}

	cmd = runChildScenario(t, "kill-resume", dir)
	if err := cmd.Wait(); err != nil {
		t.Fatalf("resume child: %v", err)
	}

	var downloads []string
	for _, line := range readChildLog(t, dir) {
		if strings.HasPrefix(line, "download:") {
			downloads = append(downloads, strings.TrimPrefix(line, "download:"))
		}
	}
	want := []string{"http://q/a", "http://q/b", "http://q/c"}
	if len(downloads) != len(want) {
		t.Fatalf("resume downloads = %v, want %v", downloads, want)
	}
	for i, u := range want {
		if downloads[i] != u {
			t.Fatalf("resume order = %v, want %v", downloads, want)
		}
	}

	snap = readSnapshotFile(t, dir)
	if !snap.Finished || len(snap.Queued) != 0 || len(snap.Inflight) != 0 {
		t.Fatalf("resumed crawl not finished: %+v", snap)
	}
}
