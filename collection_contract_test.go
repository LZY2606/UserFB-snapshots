package antch

import (
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/antchfx/antch/task_support/taskcheck"
)

func collectionTestCrawler(t *testing.T) *Crawler {
	t.Helper()
	c := NewCrawler()
	exit := make(chan struct{})
	c.Exit = exit
	c.DownloadDelay = time.Millisecond
	c.MaxConcurrentRequests = 16
	c.MaxConcurrentRequestsPerSite = 8
	c.MaxConcurrentItems = 16
	c.ErrorLog = NilLogger
	t.Cleanup(func() { close(exit) })
	return c
}

type collectionOutcome struct {
	state CollectionState
	err   error
}

func collectionGoCollect(c *Crawler, options CollectionOptions) <-chan collectionOutcome {
	done := make(chan collectionOutcome, 1)
	go func() {
		state, err := c.Collect(options)
		done <- collectionOutcome{state: state, err: err}
	}()
	return done
}

func collectionAwait(t *testing.T, done <-chan collectionOutcome, what string) collectionOutcome {
	t.Helper()
	select {
	case out := <-done:
		return out
	case <-time.After(5 * time.Second):
		t.Fatalf("timed out waiting for %s", what)
		return collectionOutcome{}
	}
}

func collectionWaitFor(t *testing.T, what string, timeout time.Duration, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		if cond() {
			return
		}
		time.Sleep(time.Millisecond)
	}
	t.Fatalf("timed out waiting for %s", what)
}

func collectionReceiveState(t *testing.T, ch <-chan CollectionState, what string) CollectionState {
	t.Helper()
	select {
	case s := <-ch:
		return s
	case <-time.After(5 * time.Second):
		t.Fatalf("timed out waiting for %s", what)
		return CollectionState{}
	}
}

type collectionStateRecorder struct {
	mu     sync.Mutex
	states []CollectionState
}

func (r *collectionStateRecorder) save(s CollectionState) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.states = append(r.states, s)
	return nil
}

func (r *collectionStateRecorder) snapshot() []CollectionState {
	r.mu.Lock()
	defer r.mu.Unlock()
	return append([]CollectionState(nil), r.states...)
}

// TestCollectionBarrier verifies that a page is only saved after the Handler
// returned and every Item it sent finished in the original Pipeline, that Save
// blocks the next page request, and that multi-item pages and empty pages both
// settle correctly.
func TestCollectionBarrier(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		var page2Requests, deltaRequests atomic.Int32
		page2Hit := make(chan struct{}, 1)
		deltaHit := make(chan struct{}, 1)

		srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			switch r.URL.Path {
			case "/start":
				w.Header().Set(collectionNextHeader, "page2")
				fmt.Fprint(w, "start")
			case "/page2":
				page2Requests.Add(1)
				select {
				case page2Hit <- struct{}{}:
				default:
				}
				// No Items from this page: the empty page must still settle.
				w.Header().Set(collectionDeltaHeader, "/delta")
				fmt.Fprint(w, "page2")
			case "/delta":
				deltaRequests.Add(1)
				select {
				case deltaHit <- struct{}{}:
				default:
				}
				fmt.Fprint(w, "delta")
			default:
				http.NotFound(w, r)
			}
		}))
		defer srv.Close()

		crawler := collectionTestCrawler(t)
		pipelineStarted := make(chan string, 4)
		pipelineFinished := make(chan string, 4)
		gate1 := make(chan struct{})
		gate2 := make(chan struct{})
		var (
			processedMu sync.Mutex
			processed   []string
		)
		crawler.UsePipeline(func(next PipelineHandler) PipelineHandler {
			return PipelineHandlerFunc(func(v Item) {
				value := v.(string)
				pipelineStarted <- value
				switch value {
				case "a1":
					<-gate1
				case "a2":
					<-gate2
				}
				next.ServePipeline(v)
				processedMu.Lock()
				processed = append(processed, value)
				processedMu.Unlock()
				pipelineFinished <- value
			})
		})
		crawler.Handle("*", HandlerFunc(func(out chan<- Item, res *http.Response) {
			if res.Request.URL.Path == "/start" {
				out <- "a1"
				out <- "a2"
			}
		}))

		saveEntered := make(chan CollectionState, 2)
		saveRelease := make(chan struct{}, 2)
		done := collectionGoCollect(crawler, CollectionOptions{
			URL:  srv.URL + "/start",
			Full: true,
			Save: func(state CollectionState) error {
				saveEntered <- state
				<-saveRelease
				return nil
			},
		})

		started := map[string]bool{}
		collectionWaitFor(t, "both Items to enter the Pipeline", 5*time.Second, func() bool {
			for {
				select {
				case value := <-pipelineStarted:
					started[value] = true
				default:
					return started["a1"] && started["a2"]
				}
			}
		})
		time.Sleep(30 * time.Millisecond)
		c.Check(len(saveEntered) == 0, "Save ran while Items were still in the Pipeline")

		// Release one Item only: the page must stay unsettled.
		close(gate1)
		finished := map[string]bool{}
		collectionWaitFor(t, "the first Item to finish", 5*time.Second, func() bool {
			for {
				select {
				case value := <-pipelineFinished:
					finished[value] = true
				default:
					return finished["a1"]
				}
			}
		})
		time.Sleep(30 * time.Millisecond)
		c.Check(len(saveEntered) == 0, "Save ran before every Item of the page completed")

		close(gate2)
		collectionWaitFor(t, "the second Item to finish", 5*time.Second, func() bool {
			for {
				select {
				case value := <-pipelineFinished:
					finished[value] = true
				default:
					return finished["a1"] && finished["a2"]
				}
			}
		})
		first := collectionReceiveState(t, saveEntered, "first Save")

		processedMu.Lock()
		c.Check(len(processed) == 2, "first page processed %d Items, want 2", len(processed))
		processedMu.Unlock()
		c.Check(first.SourceURL == srv.URL+"/start", "first SourceURL = %q", first.SourceURL)
		c.Check(first.NextURL == srv.URL+"/page2", "first NextURL = %q, want %q", first.NextURL, srv.URL+"/page2")
		c.Check(first.DeltaURL == "", "first DeltaURL = %q, want empty", first.DeltaURL)

		// Save is still blocked: no continuation may be requested yet.
		time.Sleep(80 * time.Millisecond)
		c.Check(page2Requests.Load() == 0, "continuation was requested before Save returned")

		saveRelease <- struct{}{}
		select {
		case <-page2Hit:
		case <-time.After(5 * time.Second):
			t.Fatal("timed out waiting for the continuation page request")
		}

		second := collectionReceiveState(t, saveEntered, "final Save")
		c.Check(second.SourceURL == srv.URL+"/start", "final SourceURL = %q", second.SourceURL)
		c.Check(second.NextURL == "", "final NextURL = %q, want empty", second.NextURL)
		c.Check(second.DeltaURL == srv.URL+"/delta", "final DeltaURL = %q, want %q", second.DeltaURL, srv.URL+"/delta")

		saveRelease <- struct{}{}
		out := collectionAwait(t, done, "Collect")
		c.Check(out.err == nil, "Collect error = %v, want nil", out.err)
		c.Check(out.state == second, "Collect state = %+v, want %+v", out.state, second)
		c.Check(deltaRequests.Load() == 0, "the delta entry was fetched during this collection run")
	})
}

// TestCollectionResume verifies full and incremental entry selection, relative
// continuation after redirects, replay of failed pages, Save errors, and
// protocol errors.
func TestCollectionResume(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		t.Run("redirect relative continuation", func(t *testing.T) {
			var entryHits, firstHits, page2Hits, page3Hits, changesHits atomic.Int32
			srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				switch r.URL.Path {
				case "/entry":
					entryHits.Add(1)
					http.Redirect(w, r, "/dir/first", http.StatusFound)
				case "/dir/first":
					firstHits.Add(1)
					w.Header().Set(collectionNextHeader, "page2")
					fmt.Fprint(w, "first")
				case "/dir/page2":
					page2Hits.Add(1)
					w.Header().Set(collectionNextHeader, "page3")
					fmt.Fprint(w, "second")
				case "/dir/page3":
					page3Hits.Add(1)
					w.Header().Set(collectionDeltaHeader, "changes")
					fmt.Fprint(w, "third")
				case "/dir/changes":
					changesHits.Add(1)
					fmt.Fprint(w, "changes")
				default:
					http.NotFound(w, r)
				}
			}))
			defer srv.Close()

			crawler := collectionTestCrawler(t)
			var (
				itemsMu sync.Mutex
				items   []string
			)
			crawler.UsePipeline(func(next PipelineHandler) PipelineHandler {
				return PipelineHandlerFunc(func(v Item) {
					itemsMu.Lock()
					items = append(items, v.(string))
					itemsMu.Unlock()
					next.ServePipeline(v)
				})
			})
			crawler.Handle("*", HandlerFunc(func(out chan<- Item, res *http.Response) {
				switch res.Request.URL.Path {
				case "/dir/first":
					out <- "first"
				case "/dir/page2":
					out <- "second"
				case "/dir/page3":
					out <- "third"
				}
			}))

			var recorder collectionStateRecorder
			out := collectionAwait(t, collectionGoCollect(crawler, CollectionOptions{
				URL:   srv.URL + "/entry",
				Full:  true,
				State: CollectionState{SourceURL: srv.URL + "/entry"},
				Save:  recorder.save,
			}), "full collection through redirect")

			c.Check(out.err == nil, "Collect error = %v, want nil", out.err)
			c.Check(entryHits.Load() == 1, "entry requests = %d, want 1", entryHits.Load())
			c.Check(firstHits.Load() == 1, "redirected page requests = %d, want 1", firstHits.Load())
			c.Check(page2Hits.Load() == 1, "second page requests = %d, want 1", page2Hits.Load())
			c.Check(page3Hits.Load() == 1, "third page requests = %d, want 1", page3Hits.Load())
			c.Check(changesHits.Load() == 0, "delta entry was fetched in the same run")

			states := recorder.snapshot()
			c.Check(len(states) == 3, "Save calls = %d, want 3", len(states))
			if len(states) == 3 {
				c.Check(states[0].SourceURL == srv.URL+"/entry", "state 0 SourceURL = %q", states[0].SourceURL)
				c.Check(states[0].NextURL == srv.URL+"/dir/page2",
					"relative Next after redirect = %q, want %q", states[0].NextURL, srv.URL+"/dir/page2")
				c.Check(states[0].DeltaURL == "", "state 0 DeltaURL = %q, want empty", states[0].DeltaURL)
				c.Check(states[1].NextURL == srv.URL+"/dir/page3",
					"state 1 NextURL = %q, want %q", states[1].NextURL, srv.URL+"/dir/page3")
				c.Check(states[1].DeltaURL == "", "state 1 DeltaURL = %q, want empty", states[1].DeltaURL)
				c.Check(states[2].NextURL == "", "state 2 NextURL = %q, want empty", states[2].NextURL)
				c.Check(states[2].DeltaURL == srv.URL+"/dir/changes",
					"relative Delta = %q, want %q", states[2].DeltaURL, srv.URL+"/dir/changes")
				c.Check(out.state == states[2], "returned state = %+v, final Save = %+v",
					out.state, states[2])
			}

			itemsMu.Lock()
			c.Check(strings.Join(items, ",") == "first,second,third",
				"Pipeline Items = %v, want [first second third]", items)
			itemsMu.Unlock()
		})

		t.Run("next before delta and delta fallback", func(t *testing.T) {
			var nextHits, deltaHits, zeroHits atomic.Int32
			srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				switch r.URL.Path {
				case "/next":
					nextHits.Add(1)
					w.Header().Set(collectionDeltaHeader, "/nextdelta")
				case "/delta":
					deltaHits.Add(1)
					w.Header().Set(collectionDeltaHeader, "/delta2")
				case "/zero":
					zeroHits.Add(1)
					w.Header().Set(collectionDeltaHeader, "/zerodelta")
				default:
					http.NotFound(w, r)
				}
			}))
			defer srv.Close()

			crawler := collectionTestCrawler(t)
			crawler.Handle("*", VoidHandler())

			var nextRecorder collectionStateRecorder
			nextOut := collectionAwait(t, collectionGoCollect(crawler, CollectionOptions{
				URL: srv.URL,
				State: CollectionState{
					SourceURL: srv.URL,
					NextURL:   srv.URL + "/next",
					DeltaURL:  srv.URL + "/delta",
				},
				Save: nextRecorder.save,
			}), "next-page resume")
			c.Check(nextOut.err == nil, "next resume error = %v", nextOut.err)
			c.Check(nextHits.Load() == 1, "NextURL requests = %d, want 1", nextHits.Load())
			c.Check(deltaHits.Load() == 0, "DeltaURL was requested while NextURL was unfinished")
			nextStates := nextRecorder.snapshot()
			c.Check(len(nextStates) == 1, "next resume Save calls = %d, want 1", len(nextStates))
			if len(nextStates) == 1 {
				c.Check(nextStates[0].SourceURL == srv.URL, "next resume SourceURL = %q", nextStates[0].SourceURL)
				c.Check(nextStates[0].NextURL == "", "next resume NextURL = %q, want empty", nextStates[0].NextURL)
				c.Check(nextStates[0].DeltaURL == srv.URL+"/nextdelta",
					"next resume DeltaURL = %q, want %q", nextStates[0].DeltaURL, srv.URL+"/nextdelta")
			}

			var deltaRecorder collectionStateRecorder
			deltaOut := collectionAwait(t, collectionGoCollect(crawler, CollectionOptions{
				URL:   srv.URL,
				State: CollectionState{SourceURL: srv.URL, DeltaURL: srv.URL + "/delta"},
				Save:  deltaRecorder.save,
			}), "delta resume")
			c.Check(deltaOut.err == nil, "delta resume error = %v", deltaOut.err)
			c.Check(deltaHits.Load() == 1, "DeltaURL requests = %d, want 1", deltaHits.Load())
			deltaStates := deltaRecorder.snapshot()
			c.Check(len(deltaStates) == 1, "delta resume Save calls = %d, want 1", len(deltaStates))
			if len(deltaStates) == 1 {
				c.Check(deltaStates[0].DeltaURL == srv.URL+"/delta2",
					"delta resume DeltaURL = %q, want %q", deltaStates[0].DeltaURL, srv.URL+"/delta2")
			}

			var zeroRecorder collectionStateRecorder
			zeroOut := collectionAwait(t, collectionGoCollect(crawler, CollectionOptions{
				URL:  srv.URL + "/zero",
				Save: zeroRecorder.save,
			}), "zero-state collection")
			c.Check(zeroOut.err == nil, "zero-state error = %v", zeroOut.err)
			c.Check(zeroHits.Load() == 1, "entry URL requests = %d, want 1", zeroHits.Load())
			c.Check(zeroOut.state.SourceURL == srv.URL+"/zero",
				"zero-state SourceURL = %q", zeroOut.state.SourceURL)
			c.Check(zeroOut.state.DeltaURL == srv.URL+"/zerodelta",
				"zero-state DeltaURL = %q", zeroOut.state.DeltaURL)
		})

		t.Run("save failure replay", func(t *testing.T) {
			sentinel := errors.New("business save failed")
			var page2Hits atomic.Int32
			var saveAttempts atomic.Int32
			srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				switch r.URL.Path {
				case "/start":
					w.Header().Set(collectionNextHeader, "/page2")
				case "/page2":
					page2Hits.Add(1)
					w.Header().Set(collectionDeltaHeader, "/done")
				default:
					http.NotFound(w, r)
				}
			}))
			defer srv.Close()

			crawler := collectionTestCrawler(t)
			var (
				itemsMu sync.Mutex
				items   []string
			)
			crawler.UsePipeline(func(next PipelineHandler) PipelineHandler {
				return PipelineHandlerFunc(func(v Item) {
					itemsMu.Lock()
					items = append(items, v.(string))
					itemsMu.Unlock()
					next.ServePipeline(v)
				})
			})
			crawler.Handle("*", HandlerFunc(func(out chan<- Item, res *http.Response) {
				switch res.Request.URL.Path {
				case "/start":
					out <- "start-item"
				case "/page2":
					out <- "page2-item"
				}
			}))

			var recorder collectionStateRecorder
			first := collectionAwait(t, collectionGoCollect(crawler, CollectionOptions{
				URL:  srv.URL + "/start",
				Full: true,
				Save: func(state CollectionState) error {
					if saveAttempts.Add(1) == 2 {
						return sentinel
					}
					return recorder.save(state)
				},
			}), "collection with failing Save")

			c.Check(first.err != nil, "first collection error = nil, want Save error")
			c.Check(errors.Is(first.err, sentinel), "first collection error = %v, want errors.Is(..., Save error)", first.err)
			firstStates := recorder.snapshot()
			c.Check(len(firstStates) == 1, "successful Saves after failed Save = %d, want 1", len(firstStates))
			c.Check(first.state.SourceURL == srv.URL+"/start", "returned SourceURL = %q", first.state.SourceURL)
			c.Check(first.state.NextURL == srv.URL+"/page2",
				"returned NextURL = %q, want %q", first.state.NextURL, srv.URL+"/page2")
			c.Check(first.state.DeltaURL == "", "returned DeltaURL = %q, want empty", first.state.DeltaURL)

			second := collectionAwait(t, collectionGoCollect(crawler, CollectionOptions{
				URL:   srv.URL + "/start",
				State: first.state,
				Save:  recorder.save,
			}), "recollection after Save failure")
			c.Check(second.err == nil, "recollection error = %v, want nil", second.err)
			c.Check(second.state.NextURL == "", "recollection NextURL = %q, want empty", second.state.NextURL)
			c.Check(second.state.DeltaURL == srv.URL+"/done",
				"recollection DeltaURL = %q, want %q", second.state.DeltaURL, srv.URL+"/done")
			c.Check(page2Hits.Load() == 2, "page2 requests = %d, want 2 (failed page replayed)", page2Hits.Load())

			itemsMu.Lock()
			replays := 0
			for _, item := range items {
				if item == "page2-item" {
					replays++
				}
			}
			c.Check(replays == 2, "page2 items processed = %d, want 2 (unsaved output replayed)", replays)
			itemsMu.Unlock()
		})

		t.Run("protocol failure replay and cycle", func(t *testing.T) {
			var page2Hits atomic.Int32
			var c1Hits, c2Hits atomic.Int32
			srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				switch r.URL.Path {
				case "/f1":
					w.Header().Set(collectionNextHeader, "/f2")
				case "/f2":
					if page2Hits.Add(1) == 1 {
						http.Error(w, "temporary", http.StatusInternalServerError)
						return
					}
					w.Header().Set(collectionDeltaHeader, "/fdone")
				case "/c1":
					c1Hits.Add(1)
					w.Header().Set(collectionNextHeader, "/c2")
				case "/c2":
					c2Hits.Add(1)
					w.Header().Set(collectionNextHeader, "/c1")
				default:
					http.NotFound(w, r)
				}
			}))
			defer srv.Close()

			crawler := collectionTestCrawler(t)
			crawler.Handle("*", VoidHandler())

			var recorder collectionStateRecorder
			first := collectionAwait(t, collectionGoCollect(crawler, CollectionOptions{
				URL:  srv.URL + "/f1",
				Full: true,
				Save: recorder.save,
			}), "collection with a non-200 page")
			c.Check(first.err != nil, "non-200 page error = nil, want error")
			c.Check(first.state.NextURL == srv.URL+"/f2",
				"state after non-200 page = %q, want %q", first.state.NextURL, srv.URL+"/f2")
			c.Check(len(recorder.snapshot()) == 1, "Saves after non-200 page = %d, want 1", len(recorder.snapshot()))

			second := collectionAwait(t, collectionGoCollect(crawler, CollectionOptions{
				URL:   srv.URL + "/f1",
				State: first.state,
				Save:  recorder.save,
			}), "recollection after a non-200 page")
			c.Check(second.err == nil, "recollection error = %v, want nil", second.err)
			c.Check(page2Hits.Load() == 2, "failed page requests = %d, want 2", page2Hits.Load())
			c.Check(second.state.DeltaURL == srv.URL+"/fdone",
				"recollection DeltaURL = %q, want %q", second.state.DeltaURL, srv.URL+"/fdone")

			var cycleRecorder collectionStateRecorder
			cycle := collectionAwait(t, collectionGoCollect(crawler, CollectionOptions{
				URL:  srv.URL + "/c1",
				Full: true,
				Save: cycleRecorder.save,
			}), "cyclic collection")
			c.Check(cycle.err != nil, "cycle error = nil, want error")
			c.Check(c1Hits.Load() == 1, "cycle c1 requests = %d, want 1", c1Hits.Load())
			c.Check(c2Hits.Load() == 1, "cycle c2 requests = %d, want 1", c2Hits.Load())
			cycleStates := cycleRecorder.snapshot()
			c.Check(len(cycleStates) == 1, "cycle Saves = %d, want 1", len(cycleStates))
			if len(cycleStates) == 1 {
				c.Check(cycle.state == cycleStates[0], "cycle returned state = %+v, want last Save %+v", cycle.state, cycleStates[0])
			}
		})

		t.Run("invalid configuration and headers", func(t *testing.T) {
			var requests atomic.Int32
			srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				requests.Add(1)
				switch r.URL.Path {
				case "/both":
					w.Header().Set(collectionNextHeader, "/x")
					w.Header().Set(collectionDeltaHeader, "/y")
				case "/none":
				case "/bad":
					w.Header().Set(collectionNextHeader, "ftp://example.com/page")
				}
			}))
			defer srv.Close()

			crawler := collectionTestCrawler(t)
			crawler.Handle("*", VoidHandler())

			saveCalls := 0
			save := func(CollectionState) error {
				saveCalls++
				return nil
			}
			out, err := crawler.Collect(CollectionOptions{URL: srv.URL + "/none"})
			c.Check(err != nil, "missing Save error = nil, want error")
			c.Check(out == (CollectionState{}), "missing Save returned state = %+v, want zero", out)

			out, err = crawler.Collect(CollectionOptions{
				URL:   srv.URL + "/none",
				State: CollectionState{SourceURL: srv.URL + "/other", NextURL: srv.URL + "/next"},
				Save:  save,
			})
			c.Check(err != nil, "mismatched state error = nil, want error")
			c.Check(out.SourceURL == srv.URL+"/other", "mismatched state SourceURL = %q", out.SourceURL)
			c.Check(requests.Load() == 0, "requests made before state validation: %d", requests.Load())

			out, err = crawler.Collect(CollectionOptions{URL: "relative/page", Save: save})
			c.Check(err != nil, "relative entry URL error = nil, want error")
			c.Check(out == (CollectionState{}), "relative entry returned state = %+v, want zero", out)

			out, err = crawler.Collect(CollectionOptions{URL: "ftp://example.com/", Save: save})
			c.Check(err != nil, "non-HTTP entry URL error = nil, want error")
			c.Check(out == (CollectionState{}), "non-HTTP entry returned state = %+v, want zero", out)

			for _, path := range []string{"/both", "/none", "/bad"} {
				out, err = crawler.Collect(CollectionOptions{URL: srv.URL + path, Full: true, Save: save})
				c.Check(err != nil, "%s header error = nil, want error", path)
				c.Check(out == (CollectionState{}), "%s returned state = %+v, want zero", path, out)
			}
			c.Check(saveCalls == 0, "Save called %d times for invalid pages, want 0", saveCalls)
		})
	})
}

// TestCollectionIsolation verifies that concurrent collections keep their own
// page ownership: a slow Pipeline in one source does not block another source's
// processing or Save, and ordinary Crawl Items do not settle collection pages.
func TestCollectionIsolation(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		var ordinaryHits atomic.Int32
		srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			switch r.URL.Path {
			case "/a/start":
				w.Header().Set(collectionNextHeader, "a2")
			case "/a/a2":
				w.Header().Set(collectionDeltaHeader, "adelta")
			case "/b/start":
				w.Header().Set(collectionNextHeader, "b2")
			case "/b/b2":
				w.Header().Set(collectionDeltaHeader, "bdelta")
			case "/ordinary":
				ordinaryHits.Add(1)
				fmt.Fprint(w, "ordinary")
			default:
				http.NotFound(w, r)
			}
		}))
		defer srv.Close()

		crawler := collectionTestCrawler(t)
		aGate := make(chan struct{})
		ordinaryGate := make(chan struct{})
		started := make(chan string, 16)
		finished := make(chan string, 16)

		crawler.UsePipeline(func(next PipelineHandler) PipelineHandler {
			return PipelineHandlerFunc(func(v Item) {
				value := v.(string)
				started <- value
				switch value {
				case "A1", "A2":
					<-aGate
				case "N1":
					<-ordinaryGate
				}
				next.ServePipeline(v)
				finished <- value
			})
		})
		crawler.Handle("*", HandlerFunc(func(out chan<- Item, res *http.Response) {
			switch res.Request.URL.Path {
			case "/a/start":
				out <- "A1"
				out <- "A2"
			case "/b/start":
				out <- "B1"
			case "/ordinary":
				out <- "N1"
			}
		}))

		aSave := make(chan CollectionState, 4)
		bSave := make(chan CollectionState, 4)
		aDone := collectionGoCollect(crawler, CollectionOptions{
			URL:  srv.URL + "/a/start",
			Full: true,
			Save: func(state CollectionState) error {
				aSave <- state
				return nil
			},
		})
		bDone := collectionGoCollect(crawler, CollectionOptions{
			URL:  srv.URL + "/b/start",
			Full: true,
			Save: func(state CollectionState) error {
				bSave <- state
				return nil
			},
		})
		if err := crawler.EnqueueURL(srv.URL + "/ordinary"); err != nil {
			t.Fatalf("EnqueueURL: %v", err)
		}

		wantStarted := map[string]bool{"A1": false, "B1": false, "N1": false}
		collectionWaitFor(t, "A1, B1 and N1 to enter the Pipeline", 5*time.Second, func() bool {
			for {
				select {
				case value := <-started:
					if _, ok := wantStarted[value]; ok {
						wantStarted[value] = true
					}
				default:
					return wantStarted["A1"] && wantStarted["B1"] && wantStarted["N1"]
				}
			}
		})

		// Source B must finish both pages and Save while source A is blocked.
		bFirst := collectionReceiveState(t, bSave, "source B first Save")
		bSecond := collectionReceiveState(t, bSave, "source B final Save")
		bOut := collectionAwait(t, bDone, "source B Collect")
		c.Check(bOut.err == nil, "source B error = %v, want nil", bOut.err)
		c.Check(bFirst.SourceURL == srv.URL+"/b/start", "source B first SourceURL = %q", bFirst.SourceURL)
		c.Check(bFirst.NextURL == srv.URL+"/b/b2", "source B first NextURL = %q", bFirst.NextURL)
		c.Check(bSecond.DeltaURL == srv.URL+"/b/bdelta",
			"source B final DeltaURL = %q, want %q", bSecond.DeltaURL, srv.URL+"/b/bdelta")

		// A is still blocked in its Pipeline, so it must not have saved.
		time.Sleep(30 * time.Millisecond)
		c.Check(len(aSave) == 0, "source A saved while its Pipeline was blocked")
		select {
		case out := <-aDone:
			c.Check(false, "source A completed while its Pipeline was blocked: %+v", out)
		default:
			c.Check(true, "source A remained pending while its Pipeline was blocked")
		}

		// Completing the ordinary item must not settle any collection page.
		close(ordinaryGate)
		ordinaryFinished := false
		collectionWaitFor(t, "the ordinary Item to finish", 5*time.Second, func() bool {
			for {
				select {
				case value := <-finished:
					if value == "N1" {
						ordinaryFinished = true
					}
				default:
					return ordinaryFinished
				}
			}
		})
		c.Check(ordinaryFinished, "ordinary Item finished = %v, want true", ordinaryFinished)
		time.Sleep(30 * time.Millisecond)
		c.Check(len(aSave) == 0, "ordinary Item settled source A's collection page")
		c.Check(ordinaryHits.Load() == 1, "ordinary page requests = %d, want 1", ordinaryHits.Load())

		// Releasing A lets its two Items complete and its pages settle.
		close(aGate)
		aFirst := collectionReceiveState(t, aSave, "source A first Save")
		aSecond := collectionReceiveState(t, aSave, "source A final Save")
		aOut := collectionAwait(t, aDone, "source A Collect")
		c.Check(aOut.err == nil, "source A error = %v, want nil", aOut.err)
		c.Check(aFirst.SourceURL == srv.URL+"/a/start", "source A first SourceURL = %q", aFirst.SourceURL)
		c.Check(aFirst.NextURL == srv.URL+"/a/a2", "source A first NextURL = %q", aFirst.NextURL)
		c.Check(aFirst.DeltaURL == "", "source A first DeltaURL = %q, want empty", aFirst.DeltaURL)
		c.Check(aSecond.NextURL == "", "source A final NextURL = %q, want empty", aSecond.NextURL)
		c.Check(aSecond.DeltaURL == srv.URL+"/a/adelta",
			"source A final DeltaURL = %q, want %q", aSecond.DeltaURL, srv.URL+"/a/adelta")
		c.Check(aSecond.DeltaURL != bSecond.DeltaURL, "source A received source B's delta state")

		// Both sources own exactly their own state, regardless of arrival order.
		c.Check(aOut.state == aSecond, "source A returned state = %+v, want %+v", aOut.state, aSecond)
		c.Check(bOut.state == bSecond, "source B returned state = %+v, want %+v", bOut.state, bSecond)
		c.Check(aOut.state.SourceURL != bOut.state.SourceURL,
			"concurrent collections returned the same source: %q", aOut.state.SourceURL)
	})
}
