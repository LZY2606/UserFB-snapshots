package ferret_test

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"net/http"
	"net/http/httptest"
	"sort"
	"sync"
	"sync/atomic"
	"testing"

	ferret "github.com/MontFerret/ferret"
	httpdriver "github.com/MontFerret/ferret/pkg/drivers/http"
	"github.com/MontFerret/ferret/pkg/runtime"
	"github.com/MontFerret/ferret/pkg/runtime/core"
	"github.com/MontFerret/ferret/pkg/runtime/values"
)

// pipeListener feeds an httptest.Server through in-memory net.Pipe
// connections. The sandbox used for these tests forbids binding network
// ports, while the goal is still to run a real HTTP server and the real HTTP
// driver with DOCUMENT/INNER_TEXT requests.
type pipeListener struct {
	once sync.Once
	ch   chan net.Conn
}

func (l *pipeListener) Accept() (net.Conn, error) {
	conn, ok := <-l.ch
	if !ok {
		return nil, fmt.Errorf("listener closed")
	}

	return conn, nil
}

func (l *pipeListener) Close() error {
	l.once.Do(func() {
		close(l.ch)
	})

	return nil
}

func (l *pipeListener) Addr() net.Addr { return dummyPipeAddr{} }

type dummyPipeAddr struct{}

func (dummyPipeAddr) Network() string { return "pipe" }
func (dummyPipeAddr) String() string  { return "detail-site-pipe" }

// detailSite is a local detail page site backed by httptest over pipe
// connections, so the tests observe the real request count of the crawler.
type detailSite struct {
	listener *pipeListener
	server   *httptest.Server

	mu     sync.Mutex
	paths  []string
	failOn map[string]bool
	hits   atomic.Int32
}

func newDetailSite(t *testing.T, failPaths ...string) *detailSite {
	t.Helper()

	site := &detailSite{
		listener: &pipeListener{ch: make(chan net.Conn, 32)},
		failOn:   make(map[string]bool),
	}

	for _, p := range failPaths {
		site.failOn[p] = true
	}

	site.server = &httptest.Server{
		Listener: site.listener,
		Config:   &http.Server{Handler: http.HandlerFunc(site.handle)},
	}
	site.server.Start()

	t.Cleanup(site.server.Close)

	return site
}

func (s *detailSite) handle(w http.ResponseWriter, r *http.Request) {
	path := r.URL.Path

	s.hits.Add(1)

	s.mu.Lock()
	s.paths = append(s.paths, path)
	s.mu.Unlock()

	if s.failOn[path] {
		w.WriteHeader(http.StatusInternalServerError)
		_, _ = w.Write([]byte("boom"))

		return
	}

	// The path looks like "/<id>-<tag>", the page exposes it through h1.
	w.Header().Set("Content-Type", "text/html")
	_, _ = fmt.Fprintf(w, `<html><body><h1>detail %s</h1></body></html>`, path[1:])
}

func (s *detailSite) transport() *http.Transport {
	return &http.Transport{
		DialContext: func(_ context.Context, _, _ string) (net.Conn, error) {
			client, server := net.Pipe()
			s.listener.ch <- server

			return client, nil
		},
	}
}

func (s *detailSite) hitCount() int {
	return int(s.hits.Load())
}

func (s *detailSite) saw(path string) bool {
	s.mu.Lock()
	defer s.mu.Unlock()

	for _, p := range s.paths {
		if p == path {
			return true
		}
	}

	return false
}

func (s *detailSite) requestOrder() []string {
	s.mu.Lock()
	defer s.mu.Unlock()

	out := make([]string, len(s.paths))
	copy(out, s.paths)

	return out
}

// newFerretWithSite builds an instance whose HTTP driver talks to the site,
// with retries disabled so failed pages cost exactly one request.
func newFerretWithSite(site *detailSite) *ferret.Instance {
	f := ferret.New()

	drv := httpdriver.NewDriver(
		httpdriver.WithMaxRetries(0),
		httpdriver.WithCustomTransport(site.transport()),
	)

	if err := f.Drivers().Register(drv); err != nil {
		panic(err)
	}

	return f
}

// collectStream runs the program and returns every emitted payload plus the
// order in which the receiver saw them.
func collectStream(t *testing.T, f *ferret.Instance, program *runtime.Program, opts ...runtime.Option) ([]json.RawMessage, error) {
	t.Helper()

	var out []json.RawMessage

	err := f.Stream(context.Background(), program, func(raw json.RawMessage) error {
		// Keep our own copy: the receiver owns the bytes.
		chunk := make(json.RawMessage, len(raw))
		copy(chunk, raw)
		out = append(out, chunk)

		return nil
	}, opts...)

	return out, err
}

// runAsArray returns Run output decoded into logical items for FOR programs.
func runAsArray(t *testing.T, f *ferret.Instance, program *runtime.Program, opts ...runtime.Option) []json.RawMessage {
	t.Helper()

	raw, err := f.Run(context.Background(), program, opts...)
	if err != nil {
		t.Fatalf("Run failed: %v", err)
	}

	var items []json.RawMessage
	if err := json.Unmarshal(raw, &items); err != nil {
		t.Fatalf("decoding Run output %s: %v", raw, err)
	}

	return items
}

func assertSameJSONSequence(t *testing.T, name string, got []json.RawMessage, want []json.RawMessage) {
	t.Helper()

	if len(got) != len(want) {
		t.Fatalf("%s: emitted %d items, Run produced %d (got=%v want=%v)", name, len(got), len(want), got, want)
	}

	for i := range got {
		if string(got[i]) != string(want[i]) {
			t.Fatalf("%s: item %d differs: stream=%s run=%s", name, i, got[i], want[i])
		}
	}
}

const (
	flatDetailQuery = `
		FOR id IN 1..5
			LET page = DOCUMENT(CONCAT(@base, "/", TO_STRING(id), "-x"))
			RETURN INNER_TEXT(page, "h1")
	`

	chainDetailQuery = `
		FOR id IN 1..4
			FOR sub IN [1, 2]
				LET page = DOCUMENT(CONCAT(@base, "/", TO_STRING(id), "-", TO_STRING(sub)))
				RETURN INNER_TEXT(page, "h1")
	`
)

func TestCollectionStreamShape(t *testing.T) {
	t.Run("top level FOR streams item by item without prefetching", func(t *testing.T) {
		site := newDetailSite(t)
		f := newFerretWithSite(site)
		program := f.MustCompile(flatDetailQuery)

		var emitted []string

		err := f.Stream(context.Background(), program, func(raw json.RawMessage) error {
			var text string
			if err := json.Unmarshal(raw, &text); err != nil {
				return err
			}

			emitted = append(emitted, text)

			// No prefetch: while the receiver holds the callback, only the
			// pages already delivered have been requested.
			wantHits := len(emitted)
			if site.hitCount() != wantHits {
				t.Fatalf("after %d emissions the site received %d requests", len(emitted), site.hitCount())
			}

			return nil
		}, runtime.WithParam("base", "http://detail.test"))
		if err != nil {
			t.Fatalf("Stream: %v", err)
		}

		if len(emitted) != 5 || site.hitCount() != 5 {
			t.Fatalf("emitted=%v hits=%d", emitted, site.hitCount())
		}

		assertSameJSONSequence(t, "flat FOR", mustRawMessages(t, emitted), runAsArray(t, f, program, runtime.WithParam("base", "http://detail.test")))
	})

	t.Run("loop chain expands lazily and streams inner results one by one", func(t *testing.T) {
		site := newDetailSite(t)
		f := newFerretWithSite(site)
		program := f.MustCompile(chainDetailQuery)

		var emitted []string
		var emitOrder []string

		err := f.Stream(context.Background(), program, func(raw json.RawMessage) error {
			var text string
			if err := json.Unmarshal(raw, &text); err != nil {
				return err
			}

			emitted = append(emitted, text)
			emitOrder = append(emitOrder, text)

			switch len(emitted) {
			case 1:
				// First inner iteration of the first outer item.
				if site.hitCount() != 1 || site.saw("/1-2") {
					t.Fatalf("inner loop ran ahead: hits=%d paths=%v", site.hitCount(), site.requestOrder())
				}
			case 2:
				if site.saw("/2-1") {
					t.Fatalf("outer loop ran ahead: %v", site.requestOrder())
				}
			case 4:
				if site.saw("/3-1") {
					t.Fatalf("outer loop ran ahead after two outer items: %v", site.requestOrder())
				}
			}

			return nil
		}, runtime.WithParam("base", "http://detail.test"))
		if err != nil {
			t.Fatalf("Stream: %v", err)
		}

		want := []string{"detail 1-1", "detail 1-2", "detail 2-1", "detail 2-2", "detail 3-1", "detail 3-2", "detail 4-1", "detail 4-2"}
		if fmt.Sprint(emitted) != fmt.Sprint(want) {
			t.Fatalf("loop chain order: got=%v want=%v", emitted, want)
		}

		// Requests and emissions happen in the same order.
		reqOrder := site.requestOrder()
		if len(reqOrder) != len(emitOrder) {
			t.Fatalf("requests=%d emissions=%d", len(reqOrder), len(emitOrder))
		}

		for i, path := range reqOrder {
			if "detail "+path[1:] != emitOrder[i] {
				t.Fatalf("request/emission order diverged at %d: %s vs %s", i, path, emitOrder[i])
			}
		}

		assertSameJSONSequence(t, "loop chain", mustRawMessages(t, emitted), runAsArray(t, f, program, runtime.WithParam("base", "http://detail.test")))
	})

	t.Run("value position subqueries stay whole values", func(t *testing.T) {
		f := ferret.New()

		program := f.MustCompile(`FOR a IN [1, 2] RETURN (FOR b IN [3, 4] RETURN b)`)

		got, err := collectStream(t, f, program)
		if err != nil {
			t.Fatalf("Stream: %v", err)
		}

		if len(got) != 2 || string(got[0]) != "[3,4]" || string(got[1]) != "[3,4]" {
			t.Fatalf("value subquery was split: %v", got)
		}

		assertSameJSONSequence(t, "value subquery", got, runAsArray(t, f, program))

		// LET-bound subqueries and subqueries inside objects/arrays are plain
		// value computations too.
		letProgram := f.MustCompile(`
			FOR a IN [1, 2]
				LET sub = (FOR b IN [3, 4] RETURN b)
				RETURN { a: a, sub: sub, nested: [(FOR x IN [5] RETURN x)] }
		`)

		letGot, err := collectStream(t, f, letProgram)
		if err != nil {
			t.Fatalf("Stream: %v", err)
		}

		if len(letGot) != 2 {
			t.Fatalf("LET subquery leaked intermediate values: %v", letGot)
		}

		assertSameJSONSequence(t, "LET subquery", letGot, runAsArray(t, f, letProgram))
	})

	t.Run("direct RETURN delivers the whole value once", func(t *testing.T) {
		f := ferret.New()

		program := f.MustCompile(`RETURN [1, 2]`)

		var calls int
		err := program.Stream(context.Background(), func(raw json.RawMessage) error {
			calls++

			if string(raw) != "[1,2]" {
				t.Fatalf("RETURN [1,2] was split: %s", raw)
			}

			return nil
		})
		if err != nil {
			t.Fatalf("Stream: %v", err)
		}

		if calls != 1 {
			t.Fatalf("RETURN [1,2] emitted %d times", calls)
		}

		noneProgram := f.MustCompile(`RETURN NONE`)

		var got []json.RawMessage
		err = noneProgram.Stream(context.Background(), func(raw json.RawMessage) error {
			got = append(got, append(json.RawMessage(nil), raw...))
			return nil
		})
		if err != nil {
			t.Fatalf("Stream RETURN NONE: %v", err)
		}

		if len(got) != 1 || string(got[0]) != "null" {
			t.Fatalf("RETURN NONE should deliver null once, got %v", got)
		}

		runOut, err := noneProgram.Run(context.Background())
		if err != nil || string(runOut) != "null" {
			t.Fatalf("Run RETURN NONE changed: %s %v", runOut, err)
		}
	})

	t.Run("empty FOR never calls emit", func(t *testing.T) {
		f := ferret.New()
		program := f.MustCompile(`FOR a IN [] RETURN a`)

		calls := 0
		err := program.Stream(context.Background(), func(json.RawMessage) error {
			calls++
			return nil
		})
		if err != nil {
			t.Fatalf("Stream: %v", err)
		}

		if calls != 0 {
			t.Fatalf("empty FOR emitted %d values", calls)
		}
	})

	t.Run("RETURN NONE keeps running the loop actions", func(t *testing.T) {
		f := ferret.New()

		var marks atomic.Int32
		if err := f.Functions().RegisterFunction("mark", func(_ context.Context, args ...core.Value) (core.Value, error) {
			marks.Add(1)
			return values.True, nil
		}); err != nil {
			t.Fatal(err)
		}

		program := f.MustCompile(`FOR i IN 1..3 MARK(i) RETURN NONE`)

		calls := 0
		err := program.Stream(context.Background(), func(json.RawMessage) error {
			calls++
			return nil
		})
		if err != nil {
			t.Fatalf("Stream: %v", err)
		}

		if calls != 0 {
			t.Fatalf("RETURN NONE delivered %d values", calls)
		}

		if marks.Load() != 3 {
			t.Fatalf("loop actions ran %d times, want 3", marks.Load())
		}

		runOut, err := program.Run(context.Background())
		if err != nil || string(runOut) != "[]" {
			t.Fatalf("Run RETURN NONE semantics changed: %s %v", runOut, err)
		}
	})

	t.Run("FILTER LIMIT DISTINCT match Run", func(t *testing.T) {
		f := ferret.New()

		cases := map[string]string{
			"filter":   `FOR a IN 1..6 FILTER a % 2 == 0 RETURN a`,
			"limit":    `FOR a IN 1..6 LIMIT 2, 3 RETURN a`,
			"distinct": `FOR a IN [1, 1, 2, 2, 3, 1] RETURN DISTINCT a`,
			"chain distinct": `
				FOR a IN [1, 1, 2]
					FOR b IN [1, 2]
						RETURN DISTINCT [a, b]
			`,
		}

		names := make([]string, 0, len(cases))
		for name := range cases {
			names = append(names, name)
		}

		sort.Strings(names)

		for _, name := range names {
			t.Run(name, func(t *testing.T) {
				program := f.MustCompile(cases[name])

				got, err := collectStream(t, f, program)
				if err != nil {
					t.Fatalf("Stream: %v", err)
				}

				assertSameJSONSequence(t, name, got, runAsArray(t, f, program))
			})
		}
	})

	t.Run("SORT and COLLECT materialize input but evaluate RETURN per delivery", func(t *testing.T) {
		site := newDetailSite(t)
		f := newFerretWithSite(site)

		// SORT must read every input page before the first delivery, but the
		// RETURN expression (INNER_TEXT) is still evaluated per item.
		sortProgram := f.MustCompile(`
			FOR id IN [3, 1, 2]
				LET page = DOCUMENT(CONCAT(@base, "/sort-", TO_STRING(id)))
				SORT id
				RETURN { id: id, text: INNER_TEXT(page, "h1") }
		`)

		var emissions int
		err := f.Stream(context.Background(), sortProgram, func(raw json.RawMessage) error {
			emissions++

			// All three pages were fetched while materializing the sort input.
			if site.hitCount() != 3 {
				t.Fatalf("SORT emitted before input was fully read: hits=%d", site.hitCount())
			}

			return nil
		}, runtime.WithParam("base", "http://detail.test"))
		if err != nil {
			t.Fatalf("Stream SORT: %v", err)
		}

		if emissions != 3 {
			t.Fatalf("SORT emissions=%d", emissions)
		}

		assertSameJSONSequence(t, "SORT", collectStreamMust(t, f, sortProgram, runtime.WithParam("base", "http://detail.test")),
			runAsArray(t, f, sortProgram, runtime.WithParam("base", "http://detail.test")))

		// COLLECT groups the full input, then streams the groups.
		collectProgram := f.MustCompile(`
			FOR u IN [{g: "b", v: 1}, {g: "a", v: 2}, {g: "b", v: 3}]
				COLLECT group = u.g
				RETURN { group: group }
		`)

		got, err := collectStream(t, f, collectProgram)
		if err != nil {
			t.Fatalf("Stream COLLECT: %v", err)
		}

		assertSameJSONSequence(t, "COLLECT", got, runAsArray(t, f, collectProgram))

		countProgram := f.MustCompile(`FOR i IN 1..4 COLLECT WITH COUNT INTO c RETURN c`)

		countGot, err := collectStream(t, f, countProgram)
		if err != nil {
			t.Fatalf("Stream COLLECT COUNT: %v", err)
		}

		assertSameJSONSequence(t, "COLLECT COUNT", countGot, runAsArray(t, f, countProgram))

		// SORT reads all input first, but the RETURN action still happens per
		// delivery: MARK() inside RETURN must not run ahead of the receiver.
		var returnMarks atomic.Int32
		if err := f.Functions().RegisterFunction("mark", func(_ context.Context, args ...core.Value) (core.Value, error) {
			returnMarks.Add(1)
			return args[0], nil
		}); err != nil {
			t.Fatal(err)
		}

		perItemProgram := f.MustCompile(`FOR a IN [3, 1, 2] SORT a RETURN MARK(a)`)

		var order []int
		err = f.Stream(context.Background(), perItemProgram, func(raw json.RawMessage) error {
			var v int
			if err := json.Unmarshal(raw, &v); err != nil {
				return err
			}

			order = append(order, v)

			if returnMarks.Load() != int32(len(order)) {
				t.Fatalf("RETURN evaluated ahead of delivery: marks=%d emissions=%d", returnMarks.Load(), len(order))
			}

			return nil
		})
		if err != nil {
			t.Fatalf("Stream per-item RETURN: %v", err)
		}

		if fmt.Sprint(order) != "[1 2 3]" || returnMarks.Load() != 3 {
			t.Fatalf("per-item RETURN order=%v marks=%d", order, returnMarks.Load())
		}
	})
}

func TestCollectionStreamDemand(t *testing.T) {
	t.Run("receiver error stops evaluation and preserves the prefix", func(t *testing.T) {
		site := newDetailSite(t)
		f := newFerretWithSite(site)
		program := f.MustCompile(flatDetailQuery)

		stopErr := errors.New("downstream is full")

		var got []string
		err := f.Stream(context.Background(), program, func(raw json.RawMessage) error {
			var text string
			if err := json.Unmarshal(raw, &text); err != nil {
				return err
			}

			got = append(got, text)

			if len(got) == 2 {
				return stopErr
			}

			return nil
		}, runtime.WithParam("base", "http://detail.test"))

		if !errors.Is(err, stopErr) {
			t.Fatalf("errors.Is lost the receiver error: %v", err)
		}

		if len(got) != 2 || got[0] != "detail 1-x" || got[1] != "detail 2-x" {
			t.Fatalf("delivered prefix: %v", got)
		}

		// No work happened after the receiver bailed out.
		if site.hitCount() != 2 || site.saw("/3-x") || site.saw("/5-x") {
			t.Fatalf("evaluation continued after stop: hits=%d paths=%v", site.hitCount(), site.requestOrder())
		}
	})

	t.Run("receiver error inside an inner loop stops the whole chain", func(t *testing.T) {
		site := newDetailSite(t)
		f := newFerretWithSite(site)
		program := f.MustCompile(chainDetailQuery)

		stopErr := errors.New("enough details")

		var got []string
		err := f.Stream(context.Background(), program, func(raw json.RawMessage) error {
			var text string
			if err := json.Unmarshal(raw, &text); err != nil {
				return err
			}

			got = append(got, text)

			if text == "detail 1-2" {
				return stopErr
			}

			return nil
		}, runtime.WithParam("base", "http://detail.test"))

		if !errors.Is(err, stopErr) {
			t.Fatalf("errors.Is lost the receiver error: %v", err)
		}

		if fmt.Sprint(got) != fmt.Sprint([]string{"detail 1-1", "detail 1-2"}) {
			t.Fatalf("chain prefix: %v", got)
		}

		if site.saw("/2-1") || site.saw("/1-1") == false {
			t.Fatalf("inner stop leaked into next outer iteration: %v", site.requestOrder())
		}
	})

	t.Run("later page failure keeps the delivered prefix and stops fetching", func(t *testing.T) {
		site := newDetailSite(t, "/3-x")
		f := newFerretWithSite(site)
		program := f.MustCompile(flatDetailQuery)

		var got []json.RawMessage
		err := f.Stream(context.Background(), program, func(raw json.RawMessage) error {
			got = append(got, append(json.RawMessage(nil), raw...))
			return nil
		}, runtime.WithParam("base", "http://detail.test"))

		if err == nil {
			t.Fatal("expected the failed page to surface an error")
		}

		if len(got) != 2 || string(got[0]) != `"detail 1-x"` || string(got[1]) != `"detail 2-x"` {
			t.Fatalf("prefix before failure: %v", got)
		}

		// /1 and /2 succeeded, /3 was attempted once (retries disabled),
		// /4 and /5 were never requested.
		if site.hitCount() != 3 || !site.saw("/3-x") || site.saw("/4-x") || site.saw("/5-x") {
			t.Fatalf("unexpected request pattern: hits=%d paths=%v", site.hitCount(), site.requestOrder())
		}
	})

	t.Run("context cancellation is reported like Run and keeps the prefix", func(t *testing.T) {
		site := newDetailSite(t)
		f := newFerretWithSite(site)
		program := f.MustCompile(flatDetailQuery)

		ctx, cancel := context.WithCancel(context.Background())

		var got []json.RawMessage
		err := f.Stream(ctx, program, func(raw json.RawMessage) error {
			got = append(got, append(json.RawMessage(nil), raw...))

			if len(got) == 2 {
				cancel()
			}

			return nil
		}, runtime.WithParam("base", "http://detail.test"))

		if !errors.Is(err, core.ErrTerminated) {
			t.Fatalf("cancellation must be core.ErrTerminated: %v", err)
		}

		if len(got) != 2 {
			t.Fatalf("cancelled prefix: %v", got)
		}

		if site.saw("/3-x") {
			t.Fatalf("requests continued after cancellation: %v", site.requestOrder())
		}

		// A pre-cancelled context delivers nothing and performs no work.
		preCancelled, preCancel := context.WithCancel(context.Background())
		preCancel()

		calls := 0
		before := site.hitCount()
		err = f.Stream(preCancelled, program, func(json.RawMessage) error {
			calls++
			return nil
		}, runtime.WithParam("base", "http://detail.test"))

		if !errors.Is(err, core.ErrTerminated) || calls != 0 || site.hitCount() != before {
			t.Fatalf("pre-cancelled run: err=%v calls=%d hits before=%d after=%d", err, calls, before, site.hitCount())
		}
	})

	t.Run("delivered bytes stay valid after later executions", func(t *testing.T) {
		f := ferret.New()
		program := f.MustCompile(`FOR a IN [1, 2] RETURN { a: a }`)

		var first json.RawMessage

		err := f.Stream(context.Background(), program, func(raw json.RawMessage) error {
			first = append(json.RawMessage(nil), raw...)
			return nil
		})
		if err != nil {
			t.Fatalf("Stream: %v", err)
		}

		snapshot := string(first)

		if _, err := collectStream(t, f, program); err != nil {
			t.Fatalf("second Stream: %v", err)
		}

		if _, err := f.Run(context.Background(), program); err != nil {
			t.Fatalf("Run after Stream: %v", err)
		}

		if string(first) != snapshot {
			t.Fatalf("previously delivered bytes were rewritten: %s", first)
		}
	})
}

func TestCollectionStreamIsolation(t *testing.T) {
	t.Run("missing parameters deliver nothing and call no business functions", func(t *testing.T) {
		site := newDetailSite(t)
		f := newFerretWithSite(site)

		var marks atomic.Int32
		if err := f.Functions().RegisterFunction("mark", func(_ context.Context, _ ...core.Value) (core.Value, error) {
			marks.Add(1)
			return values.True, nil
		}); err != nil {
			t.Fatal(err)
		}

		program := f.MustCompile(`
			FOR id IN 1..3
				MARK(id)
				LET page = DOCUMENT(CONCAT(@base, "/", TO_STRING(id), "-x"))
				RETURN INNER_TEXT(page, "h1")
		`)

		calls := 0
		err := f.Stream(context.Background(), program, func(json.RawMessage) error {
			calls++
			return nil
		})

		if err == nil {
			t.Fatal("missing parameter was not reported")
		}

		if calls != 0 || marks.Load() != 0 || site.hitCount() != 0 {
			t.Fatalf("work happened before parameter validation: calls=%d marks=%d hits=%d", calls, marks.Load(), site.hitCount())
		}
	})

	t.Run("nil arguments are rejected without executing the query", func(t *testing.T) {
		site := newDetailSite(t)
		f := newFerretWithSite(site)
		program := f.MustCompile(flatDetailQuery)

		if err := f.Stream(context.Background(), nil, func(json.RawMessage) error { return nil }); err == nil {
			t.Fatal("nil program accepted")
		}

		if err := f.Stream(context.Background(), program, nil, runtime.WithParam("base", "http://detail.test")); err == nil {
			t.Fatal("nil emit accepted")
		}

		if err := program.Stream(context.Background(), nil); err == nil {
			t.Fatal("Program.Stream accepted nil emit")
		}

		if site.hitCount() != 0 {
			t.Fatalf("rejected arguments still issued requests: %d", site.hitCount())
		}
	})

	t.Run("distinct state is per execution", func(t *testing.T) {
		f := ferret.New()
		program := f.MustCompile(`FOR a IN [1, 1, 2, 2, 3] RETURN DISTINCT a`)

		first, err := collectStream(t, f, program)
		if err != nil {
			t.Fatalf("first Stream: %v", err)
		}

		second, err := collectStream(t, f, program)
		if err != nil {
			t.Fatalf("second Stream: %v", err)
		}

		want := []json.RawMessage{json.RawMessage(`1`), json.RawMessage(`2`), json.RawMessage(`3`)}
		assertSameJSONSequence(t, "first DISTINCT", first, want)
		assertSameJSONSequence(t, "second DISTINCT", second, want)
	})

	t.Run("interleaved executions keep their own parameters and state", func(t *testing.T) {
		site := newDetailSite(t)
		f := newFerretWithSite(site)

		program := f.MustCompile(`
			FOR id IN 1..3
				LET page = DOCUMENT(CONCAT(@base, "/", TO_STRING(id), "-", @tag))
				RETURN { tag: @tag, text: INNER_TEXT(page, "h1") }
		`)

		type turn struct {
			tag     string
			release chan struct{}
		}

		turnCh := make(chan *turn)
		results := make(map[string][]json.RawMessage)
		var resultsMu sync.Mutex

		run := func(tag string) error {
			return f.Stream(context.Background(), program, func(raw json.RawMessage) error {
				release := make(chan struct{})
				turnCh <- &turn{tag: tag, release: release}
				<-release

				resultsMu.Lock()
				results[tag] = append(results[tag], append(json.RawMessage(nil), raw...))
				resultsMu.Unlock()

				return nil
			},
				runtime.WithParam("base", "http://detail.test"),
				runtime.WithParam("tag", tag),
			)
		}

		var wg sync.WaitGroup
		errs := make(chan error, 2)

		wg.Add(2)

		go func() { defer wg.Done(); errs <- run("alpha") }()
		go func() { defer wg.Done(); errs <- run("beta") }()

		// Force strict alternation between the two executions.
		for i := 0; i < 6; i++ {
			current := <-turnCh
			close(current.release)
		}

		wg.Wait()
		close(errs)

		for err := range errs {
			if err != nil {
				t.Fatalf("interleaved Stream: %v", err)
			}
		}

		if len(results["alpha"]) != 3 || len(results["beta"]) != 3 {
			t.Fatalf("interleaved results: %v", results)
		}

		for _, raw := range results["alpha"] {
			var item struct {
				Tag string `json:"tag"`
			}
			if err := json.Unmarshal(raw, &item); err != nil {
				t.Fatal(err)
			}

			if item.Tag != "alpha" {
				t.Fatalf("parameter leaked between executions: %s", raw)
			}
		}

		for _, raw := range results["beta"] {
			var item struct {
				Tag string `json:"tag"`
			}
			if err := json.Unmarshal(raw, &item); err != nil {
				t.Fatal(err)
			}

			if item.Tag != "beta" {
				t.Fatalf("parameter leaked between executions: %s", raw)
			}
		}

		for id := 1; id <= 3; id++ {
			if !site.saw(fmt.Sprintf("/%d-alpha", id)) || !site.saw(fmt.Sprintf("/%d-beta", id)) {
				t.Fatalf("interleaved requests missing: %v", site.requestOrder())
			}
		}
	})

	t.Run("failed Stream does not poison a later Run or Stream", func(t *testing.T) {
		site := newDetailSite(t)
		f := newFerretWithSite(site)
		program := f.MustCompile(flatDetailQuery)
		opts := []runtime.Option{runtime.WithParam("base", "http://detail.test")}

		stopErr := errors.New("first attempt stops early")

		err := f.Stream(context.Background(), program, func(raw json.RawMessage) error {
			return stopErr
		}, opts...)
		if !errors.Is(err, stopErr) {
			t.Fatalf("expected receiver error, got %v", err)
		}

		// Re-streaming the same program starts from scratch.
		again, err := collectStream(t, f, program, opts...)
		if err != nil {
			t.Fatalf("Stream after failure: %v", err)
		}

		assertSameJSONSequence(t, "Stream after failure", again, runAsArray(t, f, program, opts...))

		// Run keeps working on the same program.
		runOut, err := f.Run(context.Background(), program, opts...)
		if err != nil {
			t.Fatalf("Run after failed Stream: %v", err)
		}

		var runItems []json.RawMessage
		if err := json.Unmarshal(runOut, &runItems); err != nil {
			t.Fatal(err)
		}

		assertSameJSONSequence(t, "Run after failure", again, runItems)
	})

	t.Run("panic in the receiver is contained and re-execution works", func(t *testing.T) {
		f := ferret.New()
		program := f.MustCompile(`FOR a IN [1, 2, 3] RETURN a`)

		var delivered []int

		err := f.Stream(context.Background(), program, func(raw json.RawMessage) error {
			var v int
			if err := json.Unmarshal(raw, &v); err != nil {
				return err
			}

			delivered = append(delivered, v)
			panic("receiver blew up")
		})

		if err == nil {
			t.Fatal("panic escaped Stream as nil")
		}

		if !errors.Is(err, runtime.ErrStreamCallback) || !containsText(err.Error(), "receiver blew up") {
			t.Fatalf("panic not wrapped with its reason: %v", err)
		}

		if fmt.Sprint(delivered) != "[1]" {
			t.Fatalf("prefix before panic: %v", delivered)
		}

		again, err := collectStream(t, f, program)
		if err != nil {
			t.Fatalf("re-execution after panic: %v", err)
		}

		assertSameJSONSequence(t, "re-execution after panic", again,
			[]json.RawMessage{json.RawMessage(`1`), json.RawMessage(`2`), json.RawMessage(`3`)})
	})

	t.Run("a panic inside the query itself is handled like Run", func(t *testing.T) {
		f := ferret.New()
		if err := f.Functions().RegisterFunction("boom", func(_ context.Context, _ ...core.Value) (core.Value, error) {
			panic("query blew up")
		}); err != nil {
			t.Fatal(err)
		}

		program := f.MustCompile(`FOR a IN [1, 2, 3] RETURN BOOM()`)

		var calls int
		err := program.Stream(context.Background(), func(json.RawMessage) error {
			calls++
			return nil
		})

		runErr := func() error {
			_, e := program.Run(context.Background())
			return e
		}()

		if err == nil || runErr == nil || err.Error() != runErr.Error() {
			t.Fatalf("query panic differs from Run: stream=%v run=%v", err, runErr)
		}

		if calls != 0 {
			t.Fatalf("a failing query emitted %d values", calls)
		}
	})
}

func mustRawMessages(t *testing.T, values []string) []json.RawMessage {
	t.Helper()

	out := make([]json.RawMessage, len(values))

	for i, v := range values {
		raw, err := json.Marshal(v)
		if err != nil {
			t.Fatal(err)
		}

		out[i] = raw
	}

	return out
}

func collectStreamMust(t *testing.T, f *ferret.Instance, program *runtime.Program, opts ...runtime.Option) []json.RawMessage {
	t.Helper()

	out, err := collectStream(t, f, program, opts...)
	if err != nil {
		t.Fatalf("Stream: %v", err)
	}

	return out
}

func containsText(haystack, needle string) bool {
	for i := 0; i+len(needle) <= len(haystack); i++ {
		if haystack[i:i+len(needle)] == needle {
			return true
		}
	}

	return false
}
