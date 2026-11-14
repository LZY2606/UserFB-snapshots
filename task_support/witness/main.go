package main

import (
	"fmt"
	"io"
	"net/http"
	"os"
	"strings"
	"sync"
	"time"

	g "github.com/PuerkitoBio/gocrawl"
)

// This observes existing Stop/Run behavior; it supplies no pause implementation.
type witness struct {
	g.DefaultExtender
	mu                   sync.Mutex
	started              chan struct{}
	release              chan struct{}
	first                bool
	calls                map[string]int
	visits               map[string]int
	starts, ends, robots int
}

func (w *witness) Start(s interface{}) interface{} { w.mu.Lock(); w.starts++; w.mu.Unlock(); return s }
func (w *witness) End(error)                       { w.mu.Lock(); w.ends++; w.mu.Unlock() }
func (w *witness) RequestRobots(*g.URLContext, string) ([]byte, bool) {
	w.mu.Lock()
	w.robots++
	w.mu.Unlock()
	return []byte("User-agent: *\nAllow: /\n"), false
}
func (w *witness) Fetch(c *g.URLContext, _ string, head bool) (*http.Response, error) {
	method := "GET"
	if head {
		method = "HEAD"
	}
	key := method + " " + c.URL().Path
	w.mu.Lock()
	w.calls[key]++
	block := w.first && c.URL().Path == "/one" && head
	if block {
		w.first = false
	}
	w.mu.Unlock()
	if block {
		close(w.started)
		<-w.release
	}
	body := "<p>ok</p>"
	if c.URL().Path == "/one" {
		body = `<a href="http://a.invalid/child">child</a>`
	}
	if head {
		body = ""
	}
	req, _ := http.NewRequest(method, c.URL().String(), nil)
	return &http.Response{StatusCode: 200, Status: "200 OK", Header: make(http.Header), Body: io.NopCloser(strings.NewReader(body)), Request: req}, nil
}
func (w *witness) Visited(c *g.URLContext, _ interface{}) {
	w.mu.Lock()
	w.visits[c.URL().Path]++
	w.mu.Unlock()
}
func require(ok bool, msg string) {
	if !ok {
		fmt.Fprintln(os.Stderr, msg)
		os.Exit(1)
	}
}
func main() {
	w := &witness{started: make(chan struct{}), release: make(chan struct{}), first: true, calls: map[string]int{}, visits: map[string]int{}}
	o := g.NewOptions(w)
	o.LogFlags = 0
	o.CrawlDelay = 0
	o.HeadBeforeGet = true
	o.WorkerIdleTTL = time.Second
	c := g.NewCrawlerWithOptions(o)
	seeds := []string{"http://a.invalid/one", "http://a.invalid/two", "http://a.invalid/three"}
	done := make(chan error, 1)
	go func() { done <- c.Run(seeds) }()
	select {
	case <-w.started:
	case <-time.After(3 * time.Second):
		require(false, "HEAD did not start")
	}
	c.Stop()
	close(w.release)
	select {
	case err := <-done:
		require(err == g.ErrInterrupted, "Stop did not interrupt Run")
	case <-time.After(3 * time.Second):
		require(false, "Stop did not finish")
	}
	w.mu.Lock()
	require(w.calls["HEAD /one"] == 1 && w.calls["GET /one"] == 1 && w.calls["GET /two"] == 0 && w.calls["GET /three"] == 0, "unexpected stopped request sequence")
	require(w.starts == 1 && w.ends == 1, "stopped lifecycle did not finish")
	w.mu.Unlock()
	fmt.Println("BASELINE Stop finishes Run after HEAD and GET; queued /two,/three are not fetched")
	require(c.Run(seeds) == nil, "second Run failed")
	w.mu.Lock()
	defer w.mu.Unlock()
	require(w.calls["HEAD /one"] == 2 && w.calls["GET /one"] == 2 && w.visits["/one"] == 2, "second Run did not repeat /one")
	for _, u := range []string{"/two", "/three", "/child"} {
		require(w.calls["HEAD "+u] == 1 && w.calls["GET "+u] == 1 && w.visits[u] == 1, "normal discovery or HEAD/GET failed: "+u)
	}
	require(w.starts == 2 && w.ends == 2 && w.robots == 2, "second Run did not reset worker lifecycle")
	fmt.Println("CONTROL second Run repeats /one and robots; HEAD/GET plus HTML discovery /child work")
}
