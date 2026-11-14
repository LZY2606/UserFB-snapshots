package geziyor_test

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

	"github.com/geziyor/geziyor"
	"github.com/geziyor/geziyor/client"
	"github.com/geziyor/geziyor/export"
	"github.com/geziyor/geziyor/task_support/taskcheck"
)

func newPlanTestGeziyor(extra func(*geziyor.Options)) *geziyor.Geziyor {
	opt := &geziyor.Options{
		LogDisabled:       true,
		RobotsTxtDisabled: true,
		ParseHTMLDisabled: true,
		RetryTimes:        -1,
	}
	if extra != nil {
		extra(opt)
	}
	return geziyor.NewGeziyor(opt)
}

func containsString(list []string, s string) bool {
	for _, item := range list {
		if item == s {
			return true
		}
	}
	return false
}

type probeExporter struct {
	started chan struct{}
}

func (p *probeExporter) Export(ch chan interface{}) error {
	close(p.started)
	for range ch {
	}
	return nil
}

// TestPlanGraph covers plan validation, dependency ordering, empty stages
// and concurrent progress of independent branches.
func TestPlanGraph(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		var hits int32
		srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			atomic.AddInt32(&hits, 1)
			fmt.Fprint(w, "ok")
		}))
		defer srv.Close()

		requestingSeed := func(s *geziyor.Stage) error {
			return s.Get(srv.URL+"/never", nil)
		}

		// Invalid plans must be rejected with the offending names, before
		// any request is made and before any exporter is started.
		probe := &probeExporter{started: make(chan struct{})}
		badPlans := []struct {
			caseName string
			stages   []geziyor.CrawlStage
			wantName string
		}{
			{"duplicate name", []geziyor.CrawlStage{
				{Name: "a", Seed: requestingSeed}, {Name: "a", Seed: requestingSeed}}, "a"},
			{"missing dependency", []geziyor.CrawlStage{
				{Name: "a", After: []string{"ghost"}, Seed: requestingSeed}}, "ghost"},
			{"duplicate dependency", []geziyor.CrawlStage{
				{Name: "a", Seed: requestingSeed},
				{Name: "b", After: []string{"a", "a"}, Seed: requestingSeed}}, "a"},
			{"cycle", []geziyor.CrawlStage{
				{Name: "a", After: []string{"b"}, Seed: requestingSeed},
				{Name: "b", After: []string{"a"}, Seed: requestingSeed}}, "a"},
			{"nil seed", []geziyor.CrawlStage{
				{Name: "a", Seed: requestingSeed}, {Name: "b"}}, "b"},
		}
		for _, bp := range badPlans {
			g := newPlanTestGeziyor(func(o *geziyor.Options) {
				o.Exporters = []export.Exporter{probe}
			})
			err := g.StartPlan(bp.stages)
			c.Check(err != nil, "case %q: expected configuration error", bp.caseName)
			if err != nil {
				c.Check(strings.Contains(err.Error(), bp.wantName),
					"case %q: error %q should mention %q", bp.caseName, err, bp.wantName)
			}
		}
		c.Check(atomic.LoadInt32(&hits) == 0,
			"invalid plans must not issue requests, got %d", hits)
		select {
		case <-probe.started:
			c.Check(false, "exporter must not start on invalid plan")
		default:
		}

		// Dependency ordering, empty stages and interleaving branches.
		var mu sync.Mutex
		var order []string
		record := func(name string) {
			mu.Lock()
			order = append(order, name)
			mu.Unlock()
		}
		seedGet := func(name, path string) func(*geziyor.Stage) error {
			return func(s *geziyor.Stage) error {
				record(name)
				return s.Get(srv.URL+path, nil)
			}
		}

		xStarted := make(chan struct{})
		yStarted := make(chan struct{})
		var interleaved int32 = 1
		waitPeer := func(ch chan struct{}) {
			select {
			case <-ch:
			case <-time.After(10 * time.Second):
				atomic.StoreInt32(&interleaved, 0)
			}
		}

		g := newPlanTestGeziyor(nil)
		err := g.StartPlan([]geziyor.CrawlStage{
			{Name: "catalog", Seed: seedGet("catalog", "/index")},
			{Name: "details", After: []string{"catalog"}, Seed: seedGet("details", "/detail")},
			{Name: "related", After: []string{"catalog"}, Seed: seedGet("related", "/related")},
			{Name: "summary", After: []string{"details", "related"}, Seed: func(s *geziyor.Stage) error {
				record("summary")
				return nil
			}},
			{Name: "empty", Seed: func(s *geziyor.Stage) error {
				record("empty")
				return nil
			}},
			{Name: "afterEmpty", After: []string{"empty"}, Seed: func(s *geziyor.Stage) error {
				record("afterEmpty")
				return nil
			}},
			{Name: "x", Seed: func(s *geziyor.Stage) error {
				record("x")
				close(xStarted)
				waitPeer(yStarted)
				return nil
			}},
			{Name: "y", Seed: func(s *geziyor.Stage) error {
				record("y")
				close(yStarted)
				waitPeer(xStarted)
				return nil
			}},
		})
		c.Check(err == nil, "valid plan must succeed: %v", err)
		c.Check(atomic.LoadInt32(&hits) == 3, "graph hits = %d, want 3", hits)
		c.Check(atomic.LoadInt32(&interleaved) == 1,
			"independent stages x and y must make progress concurrently")

		index := make(map[string]int)
		counts := make(map[string]int)
		for i, name := range order {
			if _, seen := index[name]; !seen {
				index[name] = i
			}
			counts[name]++
		}
		for _, name := range []string{"catalog", "details", "related", "summary", "empty", "afterEmpty", "x", "y"} {
			c.Check(counts[name] == 1, "stage %q ran %d times, want exactly 1", name, counts[name])
		}
		before := func(a, b string) bool {
			ca, oka := index[a]
			cb, okb := index[b]
			return oka && okb && ca < cb
		}
		c.Check(before("catalog", "details"), "details must start after catalog: %v", order)
		c.Check(before("catalog", "related"), "related must start after catalog: %v", order)
		c.Check(before("details", "summary"), "summary must start after details: %v", order)
		c.Check(before("related", "summary"), "summary must start after related: %v", order)
		c.Check(before("empty", "afterEmpty"), "empty stage must unlock its successor: %v", order)
	})
}

// TestPlanDrain covers recursive descendants, per-stage dedup, cross-stage
// refetching and settlement of requests dropped by filters.
func TestPlanDrain(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		var mu sync.Mutex
		counts := make(map[string]int)
		mux := http.NewServeMux()
		for _, path := range []string{"/index", "/page1", "/page2", "/detail"} {
			p := path
			mux.HandleFunc(p, func(w http.ResponseWriter, r *http.Request) {
				mu.Lock()
				counts[p]++
				mu.Unlock()
				fmt.Fprint(w, "page:"+p)
			})
		}
		srv := httptest.NewServer(mux)
		defer srv.Close()

		var otherHits int32
		other := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			atomic.AddInt32(&otherHits, 1)
		}))
		defer other.Close()

		var parsed int32
		parse := func(s *geziyor.Stage, r *client.Response) error {
			atomic.AddInt32(&parsed, 1)
			return nil
		}

		g := newPlanTestGeziyor(func(o *geziyor.Options) {
			o.AllowedDomains = []string{strings.TrimPrefix(srv.URL, "http://")}
		})
		err := g.StartPlan([]geziyor.CrawlStage{
			{Name: "catalog", Seed: func(s *geziyor.Stage) error {
				return s.Get(srv.URL+"/index", func(s *geziyor.Stage, r *client.Response) error {
					atomic.AddInt32(&parsed, 1)
					// Recursive descendants, submitted while the callback runs.
					if err := s.Get(srv.URL+"/page1", parse); err != nil {
						return err
					}
					// Same-stage duplicate, dropped by the dedup filter.
					if err := s.Get(srv.URL+"/page1", parse); err != nil {
						return err
					}
					if err := s.Get(srv.URL+"/page2", func(s *geziyor.Stage, r *client.Response) error {
						atomic.AddInt32(&parsed, 1)
						// Second level descendant.
						return s.Get(srv.URL+"/detail", parse)
					}); err != nil {
						return err
					}
					// Dropped by the allowed domains filter, must still settle.
					return s.Get(other.URL+"/elsewhere", parse)
				})
			}},
			// A different stage may fetch the same URL again.
			{Name: "followup", After: []string{"catalog"}, Seed: func(s *geziyor.Stage) error {
				return s.Get(srv.URL+"/index", parse)
			}},
		})
		c.Check(err == nil, "plan with filtered drops must succeed: %v", err)

		mu.Lock()
		gotIndex, gotPage1, gotPage2, gotDetail := counts["/index"], counts["/page1"], counts["/page2"], counts["/detail"]
		mu.Unlock()
		c.Check(gotIndex == 2, "/index fetched %d times, want 2 (once per stage)", gotIndex)
		c.Check(gotPage1 == 1, "/page1 fetched %d times, want 1 (same-stage dedup)", gotPage1)
		c.Check(gotPage2 == 1, "/page2 fetched %d times, want 1", gotPage2)
		c.Check(gotDetail == 1, "/detail fetched %d times, want 1 (recursive descendant drained)", gotDetail)
		c.Check(atomic.LoadInt32(&otherHits) == 0,
			"disallowed domain must not be fetched, got %d hits", otherHits)
		c.Check(atomic.LoadInt32(&parsed) == 5,
			"parsed callbacks = %d, want 5 (dropped requests never reach callbacks)", parsed)

		// Robots.txt drops must also settle without failing the stage.
		var privateHits, publicHits, afterRobotsRan int32
		robotsMux := http.NewServeMux()
		robotsMux.HandleFunc("/robots.txt", func(w http.ResponseWriter, r *http.Request) {
			fmt.Fprint(w, "User-agent: *\nDisallow: /private\n")
		})
		robotsMux.HandleFunc("/private", func(w http.ResponseWriter, r *http.Request) {
			atomic.AddInt32(&privateHits, 1)
		})
		robotsMux.HandleFunc("/public", func(w http.ResponseWriter, r *http.Request) {
			atomic.AddInt32(&publicHits, 1)
		})
		robotsSrv := httptest.NewServer(robotsMux)
		defer robotsSrv.Close()

		g2 := newPlanTestGeziyor(func(o *geziyor.Options) {
			o.RobotsTxtDisabled = false
		})
		err = g2.StartPlan([]geziyor.CrawlStage{
			{Name: "robots", Seed: func(s *geziyor.Stage) error {
				if err := s.Get(robotsSrv.URL+"/private", nil); err != nil {
					return err
				}
				return s.Get(robotsSrv.URL+"/public", nil)
			}},
			{Name: "afterRobots", After: []string{"robots"}, Seed: func(s *geziyor.Stage) error {
				atomic.AddInt32(&afterRobotsRan, 1)
				return nil
			}},
		})
		c.Check(err == nil, "robots drops are not failures: %v", err)
		c.Check(atomic.LoadInt32(&privateHits) == 0,
			"robots.txt must forbid /private, got %d hits", privateHits)
		c.Check(atomic.LoadInt32(&publicHits) == 1,
			"/public fetched %d times, want 1", publicHits)
		c.Check(atomic.LoadInt32(&afterRobotsRan) == 1,
			"robots-filtered stage must settle and unlock successors, ran %d", afterRobotsRan)
	})
}

// TestPlanFailure covers seed, callback, transport and HTTP status
// failures, successor skipping and error unwrapping.
func TestPlanFailure(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		seedErr := errors.New("seed boom")
		callbackErr := errors.New("callback boom")

		mux := http.NewServeMux()
		mux.HandleFunc("/ok", func(w http.ResponseWriter, r *http.Request) {
			fmt.Fprint(w, "ok")
		})
		mux.HandleFunc("/fail", func(w http.ResponseWriter, r *http.Request) {
			w.WriteHeader(http.StatusInternalServerError)
		})
		mux.HandleFunc("/slow", func(w http.ResponseWriter, r *http.Request) {
			time.Sleep(100 * time.Millisecond)
			fmt.Fprint(w, "slow")
		})
		srv := httptest.NewServer(mux)
		defer srv.Close()

		dead := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {}))
		deadURL := dead.URL
		dead.Close()

		var errMu sync.Mutex
		var transportErr error
		var ranSkipped, ranIndependent, slowFinished int32

		g := newPlanTestGeziyor(func(o *geziyor.Options) {
			o.ErrorFunc = func(g *geziyor.Geziyor, r *client.Request, err error) {
				errMu.Lock()
				transportErr = err
				errMu.Unlock()
			}
		})
		skipSeed := func(s *geziyor.Stage) error {
			atomic.AddInt32(&ranSkipped, 1)
			return nil
		}
		err := g.StartPlan([]geziyor.CrawlStage{
			{Name: "seedFail", Seed: func(s *geziyor.Stage) error {
				return seedErr
			}},
			{Name: "cbFail", Seed: func(s *geziyor.Stage) error {
				if err := s.Get(srv.URL+"/ok", func(s *geziyor.Stage, r *client.Response) error {
					return callbackErr
				}); err != nil {
					return err
				}
				// Already accepted requests keep running even though the
				// sibling callback fails the stage.
				return s.Get(srv.URL+"/slow", func(s *geziyor.Stage, r *client.Response) error {
					atomic.AddInt32(&slowFinished, 1)
					return nil
				})
			}},
			{Name: "httpFail", Seed: func(s *geziyor.Stage) error {
				return s.Get(srv.URL+"/fail", nil)
			}},
			{Name: "transportFail", Seed: func(s *geziyor.Stage) error {
				return s.Get(deadURL+"/x", nil)
			}},
			{Name: "afterSeedFail", After: []string{"seedFail"}, Seed: skipSeed},
			{Name: "afterCbFail", After: []string{"cbFail"}, Seed: skipSeed},
			{Name: "afterHttpFail", After: []string{"httpFail"}, Seed: skipSeed},
			{Name: "afterTransportFail", After: []string{"transportFail"}, Seed: skipSeed},
			{Name: "transitive", After: []string{"afterSeedFail"}, Seed: skipSeed},
			{Name: "independent", Seed: func(s *geziyor.Stage) error {
				atomic.AddInt32(&ranIndependent, 1)
				return s.Get(srv.URL+"/ok", nil)
			}},
		})

		c.Check(err != nil, "plan with failing stages must return an error")
		var planErr *geziyor.PlanError
		c.Check(errors.As(err, &planErr), "error must be a *geziyor.PlanError, got %T", err)
		if planErr != nil {
			for _, name := range []string{"seedFail", "cbFail", "httpFail", "transportFail"} {
				c.Check(containsString(planErr.Failed, name),
					"failed stages %v must contain %q", planErr.Failed, name)
			}
			for _, name := range []string{"afterSeedFail", "afterCbFail", "afterHttpFail", "afterTransportFail", "transitive"} {
				c.Check(containsString(planErr.Skipped, name),
					"skipped stages %v must contain %q", planErr.Skipped, name)
			}
			c.Check(!containsString(planErr.Failed, "independent") && !containsString(planErr.Skipped, "independent"),
				"independent branch must neither fail nor be skipped: %v %v", planErr.Failed, planErr.Skipped)
		}
		if err != nil {
			c.Check(strings.Contains(err.Error(), "seedFail") && strings.Contains(err.Error(), "afterSeedFail"),
				"error message must name failed and skipped stages: %q", err)
		}
		c.Check(errors.Is(err, seedErr), "errors.Is must find the seed error")
		c.Check(errors.Is(err, callbackErr), "errors.Is must find the callback error")
		errMu.Lock()
		captured := transportErr
		errMu.Unlock()
		c.Check(captured != nil, "ErrorFunc must observe the transport error")
		c.Check(captured != nil && errors.Is(err, captured),
			"errors.Is must find the transport error")

		c.Check(atomic.LoadInt32(&ranSkipped) == 0,
			"skipped stages must not run, ran %d", ranSkipped)
		c.Check(atomic.LoadInt32(&ranIndependent) == 1,
			"independent branch must complete, ran %d", ranIndependent)
		c.Check(atomic.LoadInt32(&slowFinished) == 1,
			"accepted requests of a failing stage must finish, finished %d", slowFinished)
	})
}

type recorderExporter struct {
	mu    sync.Mutex
	items []interface{}
	done  chan struct{}
}

func (r *recorderExporter) Export(ch chan interface{}) error {
	for item := range ch {
		r.mu.Lock()
		r.items = append(r.items, item)
		r.mu.Unlock()
	}
	close(r.done)
	return nil
}

// TestPlanCompatibility covers the preserved old API, exports draining,
// Meta preservation, request flag rejection and single-start semantics.
func TestPlanCompatibility(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		var hits int32
		srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			atomic.AddInt32(&hits, 1)
			fmt.Fprint(w, "page:"+r.URL.Path)
		}))
		defer srv.Close()

		// Old Get/Do/Start behavior with global dedup is preserved.
		var parsed int32
		g1 := newPlanTestGeziyor(nil)
		g1.Opt.StartRequestsFunc = func(g *geziyor.Geziyor) {
			g.Get(srv.URL+"/index", func(g *geziyor.Geziyor, r *client.Response) {
				atomic.AddInt32(&parsed, 1)
				g.Get(srv.URL+"/detail", func(g *geziyor.Geziyor, r *client.Response) {
					atomic.AddInt32(&parsed, 1)
				})
				g.Get(srv.URL+"/index", nil) // dropped by global dedup
			})
		}
		g1.Start()
		c.Check(atomic.LoadInt32(&hits) == 2, "old path hits = %d, want 2 (global dedup)", hits)
		c.Check(atomic.LoadInt32(&parsed) == 2, "old path parsed = %d, want 2", parsed)

		// Exports stay open across stages and exporters finish before
		// StartPlan returns. Caller Meta is left untouched.
		rec := &recorderExporter{done: make(chan struct{})}
		g2 := newPlanTestGeziyor(func(o *geziyor.Options) {
			o.Exporters = []export.Exporter{rec}
		})
		var stageGeziyor *geziyor.Geziyor
		err := g2.StartPlan([]geziyor.CrawlStage{
			{Name: "produce", Seed: func(s *geziyor.Stage) error {
				stageGeziyor = s.Geziyor()
				req, err := client.NewRequest("GET", srv.URL+"/item", nil)
				if err != nil {
					return err
				}
				req.Meta["keep"] = "original"
				return s.Do(req, func(s *geziyor.Stage, r *client.Response) error {
					c.Check(len(r.Request.Meta) == 1 && r.Request.Meta["keep"] == "original",
						"caller Meta must not be rewritten, got %v", r.Request.Meta)
					s.Geziyor().Exports <- "item-1"
					return nil
				})
			}},
			{Name: "late", After: []string{"produce"}, Seed: func(s *geziyor.Stage) error {
				// Exports must still be open after the first stage finished.
				s.Geziyor().Exports <- "item-2"
				return nil
			}},
		})
		c.Check(err == nil, "export plan must succeed: %v", err)
		c.Check(stageGeziyor == g2, "stage must expose its Geziyor instance")
		rec.mu.Lock()
		items := append([]interface{}{}, rec.items...)
		rec.mu.Unlock()
		c.Check(len(items) == 2 && items[0] == "item-1" && items[1] == "item-2",
			"exporter must receive [item-1 item-2] in order before StartPlan returns, got %v", items)
		select {
		case <-rec.done:
		default:
			c.Check(false, "StartPlan must wait for exporters to finish")
		}

		// Rendered and Synchronized requests are rejected at submission.
		g3 := newPlanTestGeziyor(nil)
		err = g3.StartPlan([]geziyor.CrawlStage{
			{Name: "flags", Seed: func(s *geziyor.Stage) error {
				rendered, _ := client.NewRequest("GET", srv.URL+"/rendered", nil)
				rendered.Rendered = true
				c.Check(s.Do(rendered, nil) != nil, "rendered request must be rejected")
				synchronized, _ := client.NewRequest("GET", srv.URL+"/sync", nil)
				synchronized.Synchronized = true
				c.Check(s.Do(synchronized, nil) != nil, "synchronized request must be rejected")
				c.Check(s.Get("://invalid-url", nil) != nil, "invalid URL must be a submission error")
				return nil
			}},
		})
		c.Check(err == nil, "submission rejections must not fail the stage: %v", err)
		c.Check(atomic.LoadInt32(&hits) == 3,
			"rejected requests must not be sent, total hits = %d, want 3", hits)

		// One instance starts only once, Start and StartPlan exclude each other.
		err = g3.StartPlan([]geziyor.CrawlStage{
			{Name: "again", Seed: func(s *geziyor.Stage) error { return nil }},
		})
		c.Check(err != nil, "second StartPlan on the same instance must fail")
		g3.Start() // must return immediately without running again
		c.Check(atomic.LoadInt32(&hits) == 3,
			"Start after StartPlan must not run, total hits = %d, want 3", hits)

		// URLRevisitEnabled still applies inside stages.
		g4 := newPlanTestGeziyor(func(o *geziyor.Options) {
			o.URLRevisitEnabled = true
		})
		err = g4.StartPlan([]geziyor.CrawlStage{
			{Name: "revisit", Seed: func(s *geziyor.Stage) error {
				if err := s.Get(srv.URL+"/dup", nil); err != nil {
					return err
				}
				return s.Get(srv.URL+"/dup", nil)
			}},
		})
		c.Check(err == nil, "revisit plan must succeed: %v", err)
		c.Check(atomic.LoadInt32(&hits) == 5,
			"URLRevisitEnabled must fetch duplicates, total hits = %d, want 5", hits)
	})
}
