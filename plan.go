package geziyor

import (
	"errors"
	"fmt"
	"strings"
	"sync"
	"sync/atomic"

	"github.com/geziyor/geziyor/client"
	"github.com/geziyor/geziyor/internal"
	"github.com/geziyor/geziyor/metrics"
)

// CrawlStage describes one stage of a planned crawl.
// Stages without After start immediately. A stage starts only after all
// stages listed in After completed successfully. Independent stages run
// concurrently.
type CrawlStage struct {
	// Name uniquely identifies the stage.
	Name string

	// After lists names of stages that must complete successfully
	// before this stage starts.
	After []string

	// Seed issues the initial requests of the stage. It may submit more
	// requests with Stage.Get and Stage.Do while it runs.
	Seed func(*Stage) error
}

// Stage is the handle of a running crawl stage. Requests submitted through
// a Stage are deduplicated per stage, tracked until their callbacks finish
// and reported to the stage's outcome.
type Stage struct {
	geziyor *Geziyor
	name    string

	mu      sync.Mutex
	pending int
	closed  bool
	doneCh  chan struct{}
	errs    []error
}

// Name returns the stage name given in CrawlStage.
func (s *Stage) Name() string {
	return s.name
}

// Geziyor returns the Geziyor instance this stage belongs to.
// Use it to send extracted data to g.Exports.
func (s *Stage) Geziyor() *Geziyor {
	return s.geziyor
}

// ErrStageClosed is returned when a request is submitted to a stage that
// has already finished.
var ErrStageClosed = errors.New("geziyor: stage already finished")

// Get issues a GET request tracked by the stage. The returned error only
// reports submission problems; network results are aggregated when the
// stage finishes.
func (s *Stage) Get(url string, callback func(*Stage, *client.Response) error) error {
	req, err := client.NewRequest("GET", url, nil)
	if err != nil {
		return fmt.Errorf("stage %q: %w", s.name, err)
	}
	return s.Do(req, callback)
}

// Do sends a request tracked by the stage. Rendered and Synchronized
// requests are not supported in crawl plans and rejected immediately.
// The returned error only reports submission problems; network results are
// aggregated when the stage finishes.
func (s *Stage) Do(req *client.Request, callback func(*Stage, *client.Response) error) error {
	if req.Rendered {
		return fmt.Errorf("stage %q: rendered requests are not supported in crawl plans", s.name)
	}
	if req.Synchronized {
		return fmt.Errorf("stage %q: synchronized requests are not supported in crawl plans", s.name)
	}
	// Scope duplicate filtering to this stage. Caller Meta is left untouched.
	req.DedupScope = s.name

	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		return fmt.Errorf("stage %q: %w", s.name, ErrStageClosed)
	}
	s.pending++
	s.mu.Unlock()

	go s.execute(req, callback)
	return nil
}

// execute runs a single stage request through the regular middleware,
// client and response processing chain.
func (s *Stage) execute(req *client.Request, callback func(*Stage, *client.Response) error) {
	g := s.geziyor
	defer s.finish()
	defer g.recoverMe()
	g.acquireSem(req)
	defer g.releaseSem(req)

	for _, middlewareFunc := range g.reqMiddlewares {
		middlewareFunc.ProcessRequest(req)
		if req.Cancelled {
			// Regular drops (domain, robots, duplicate filters) are not
			// failures, but the request must be settled.
			return
		}
	}

	res, err := g.Client.DoRequest(req)
	if err != nil {
		s.recordErr(fmt.Errorf("stage %q: request %s: %w", s.name, req.URL, err))
		if g.Opt.ErrorFunc != nil {
			g.Opt.ErrorFunc(g, req, err)
		} else {
			internal.Logger.Println(err)
		}
		return
	}

	for _, middlewareFunc := range g.resMiddlewares {
		middlewareFunc.ProcessResponse(res)
	}

	if res.StatusCode >= 400 {
		s.recordErr(fmt.Errorf("stage %q: request %s: HTTP status %d", s.name, req.URL, res.StatusCode))
	}

	if callback != nil {
		if err := callback(s, res); err != nil {
			s.recordErr(fmt.Errorf("stage %q: callback %s: %w", s.name, req.URL, err))
		}
	}
}

// run executes the stage seed and waits until the seed and every submitted
// request, including recursively submitted descendants, are finished.
func (s *Stage) run(seed func(*Stage) error) {
	s.pending = 1
	if err := seed(s); err != nil {
		s.recordErr(fmt.Errorf("stage %q: seed: %w", s.name, err))
	}
	s.finish()
	<-s.doneCh
}

func (s *Stage) recordErr(err error) {
	s.mu.Lock()
	s.errs = append(s.errs, err)
	s.mu.Unlock()
}

func (s *Stage) finish() {
	s.mu.Lock()
	s.pending--
	if s.pending == 0 {
		s.closed = true
		close(s.doneCh)
	}
	s.mu.Unlock()
}

// PlanError is returned by StartPlan when one or more stages failed.
// Failed lists stages that ran and failed, Skipped lists stages that never
// ran because a predecessor failed or was skipped. Unwrap returns the
// underlying seed, callback and transport errors, so errors.Is and
// errors.As find them.
type PlanError struct {
	Failed  []string
	Skipped []string
	Errs    []error
}

func (e *PlanError) Error() string {
	return fmt.Sprintf("geziyor: crawl plan failed, failed stages: [%s], skipped stages: [%s]",
		strings.Join(e.Failed, ", "), strings.Join(e.Skipped, ", "))
}

// Unwrap returns the underlying stage errors.
func (e *PlanError) Unwrap() []error {
	return e.Errs
}

// StartPlan starts scraping with a staged crawl plan. It is an alternative
// to Start: a Geziyor instance can be started only once, with either of
// them. The plan is fully validated before anything runs; on a
// configuration error no request is made and no exporter is started.
//
// Stages whose predecessors all succeeded run exactly once, independent
// branches run concurrently. If a stage fails, its descendants are skipped
// while unrelated branches run to completion. StartPlan blocks until the
// last branch and all exporters are done.
func (g *Geziyor) StartPlan(stages []CrawlStage) error {
	if err := validatePlan(stages); err != nil {
		return err
	}
	if !atomic.CompareAndSwapInt32(&g.started, 0, 1) {
		return errors.New("geziyor: scraper already started")
	}

	internal.Logger.Println("Scraping Started")

	// Metrics
	if g.Opt.MetricsType == metrics.Prometheus || g.Opt.MetricsType == metrics.ExpVar {
		metricsServer := metrics.StartMetricsServer(g.Opt.MetricsType)
		defer metricsServer.Close()
	}

	// Start Exporters
	g.startExporters()

	type node struct {
		name    string
		seed    func(*Stage) error
		stage   *Stage
		preds   []*node
		done    chan struct{}
		failed  bool
		skipped bool
	}

	nodes := make(map[string]*node, len(stages))
	for _, cfg := range stages {
		nodes[cfg.Name] = &node{
			name:  cfg.Name,
			seed:  cfg.Seed,
			stage: &Stage{geziyor: g, name: cfg.Name, doneCh: make(chan struct{})},
			done:  make(chan struct{}),
		}
	}
	for _, cfg := range stages {
		for _, dep := range cfg.After {
			nodes[cfg.Name].preds = append(nodes[cfg.Name].preds, nodes[dep])
		}
	}

	var wg sync.WaitGroup
	for _, n := range nodes {
		wg.Add(1)
		go func(n *node) {
			defer wg.Done()
			defer close(n.done)
			for _, pred := range n.preds {
				<-pred.done
				if pred.failed || pred.skipped {
					n.skipped = true
				}
			}
			if n.skipped {
				return
			}
			n.stage.run(n.seed)
			n.stage.mu.Lock()
			n.failed = len(n.stage.errs) > 0
			n.stage.mu.Unlock()
		}(n)
	}
	wg.Wait()

	// All branches are done, only now exports can be closed.
	close(g.Exports)
	g.wgExporters.Wait()
	internal.Logger.Println("Scraping Finished")

	var planErr *PlanError
	for _, cfg := range stages {
		n := nodes[cfg.Name]
		switch {
		case n.failed:
			if planErr == nil {
				planErr = &PlanError{}
			}
			planErr.Failed = append(planErr.Failed, n.name)
			planErr.Errs = append(planErr.Errs, n.stage.errs...)
		case n.skipped:
			if planErr == nil {
				planErr = &PlanError{}
			}
			planErr.Skipped = append(planErr.Skipped, n.name)
		}
	}
	if planErr != nil {
		return planErr
	}
	return nil
}

// validatePlan checks the whole plan configuration before anything runs:
// duplicate names, missing dependencies, duplicate dependencies, cycles
// and nil seeds.
func validatePlan(stages []CrawlStage) error {
	var problems []error
	names := make(map[string]bool, len(stages))
	for _, st := range stages {
		if st.Name == "" {
			problems = append(problems, errors.New("geziyor: plan: stage with empty name"))
		} else if names[st.Name] {
			problems = append(problems, fmt.Errorf("geziyor: plan: duplicate stage name %q", st.Name))
		} else {
			names[st.Name] = true
		}
		if st.Seed == nil {
			problems = append(problems, fmt.Errorf("geziyor: plan: stage %q has nil Seed", st.Name))
		}
		seen := make(map[string]bool, len(st.After))
		for _, dep := range st.After {
			if seen[dep] {
				problems = append(problems, fmt.Errorf("geziyor: plan: stage %q lists dependency %q more than once", st.Name, dep))
			}
			seen[dep] = true
		}
	}
	for _, st := range stages {
		for _, dep := range st.After {
			if !names[dep] {
				problems = append(problems, fmt.Errorf("geziyor: plan: stage %q depends on unknown stage %q", st.Name, dep))
			}
		}
	}
	if cycle := planCycle(stages, names); len(cycle) != 0 {
		problems = append(problems, fmt.Errorf("geziyor: plan: dependency cycle among stages %s", strings.Join(cycle, " -> ")))
	}
	return errors.Join(problems...)
}

// planCycle returns one dependency cycle as a path of stage names, or nil.
func planCycle(stages []CrawlStage, names map[string]bool) []string {
	adj := make(map[string][]string, len(stages))
	for _, st := range stages {
		for _, dep := range st.After {
			if names[dep] {
				adj[st.Name] = append(adj[st.Name], dep)
			}
		}
	}

	const (
		white = iota
		gray
		black
	)
	state := make(map[string]int, len(stages))
	var stack []string
	var cycle []string
	var visit func(name string) bool
	visit = func(name string) bool {
		state[name] = gray
		stack = append(stack, name)
		for _, dep := range adj[name] {
			switch state[dep] {
			case gray:
				i := 0
				for i < len(stack) && stack[i] != dep {
					i++
				}
				cycle = append(append([]string{}, stack[i:]...), dep)
				return true
			case white:
				if visit(dep) {
					return true
				}
			}
		}
		stack = stack[:len(stack)-1]
		state[name] = black
		return false
	}
	for _, st := range stages {
		if state[st.Name] == white && visit(st.Name) {
			return cycle
		}
	}
	return nil
}
