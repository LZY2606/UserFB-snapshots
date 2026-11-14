package geziyor_test

import (
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/geziyor/geziyor"
	"github.com/geziyor/geziyor/client"
	"github.com/geziyor/geziyor/export"
	"github.com/geziyor/geziyor/task_support/taskcheck"
)

const pipelineTimeout = 10 * time.Second

// recordingSink records every value it receives from the export dispatcher.
type recordingSink struct {
	mu  sync.Mutex
	got []interface{}
}

func (s *recordingSink) Export(ch chan interface{}) error {
	for v := range ch {
		s.mu.Lock()
		s.got = append(s.got, v)
		s.mu.Unlock()
	}
	return nil
}

func (s *recordingSink) values() []interface{} {
	s.mu.Lock()
	defer s.mu.Unlock()
	return append([]interface{}(nil), s.got...)
}

// immediateErrSink returns an error without consuming anything.
type immediateErrSink struct {
	err      error
	returned chan struct{}
}

func (s *immediateErrSink) Export(chan interface{}) error {
	close(s.returned)
	return s.err
}

// immediateNilSink returns nil before its input is closed.
type immediateNilSink struct {
	returned chan struct{}
}

func (s *immediateNilSink) Export(chan interface{}) error {
	close(s.returned)
	return nil
}

// slowSink records its first value, then blocks until the test releases it.
type slowSink struct {
	mu      sync.Mutex
	got     []interface{}
	first   chan struct{}
	release chan struct{}
}

func (s *slowSink) Export(ch chan interface{}) error {
	first := true
	for v := range ch {
		s.mu.Lock()
		s.got = append(s.got, v)
		s.mu.Unlock()
		if first {
			first = false
			close(s.first)
			<-s.release
		}
	}
	return nil
}

func (s *slowSink) values() []interface{} {
	s.mu.Lock()
	defer s.mu.Unlock()
	return append([]interface{}(nil), s.got...)
}

// signalSink records values and publishes each one for ordering assertions.
type signalSink struct {
	mu       sync.Mutex
	got      []interface{}
	received chan interface{}
}

func (s *signalSink) Export(ch chan interface{}) error {
	for v := range ch {
		s.mu.Lock()
		s.got = append(s.got, v)
		s.mu.Unlock()
		s.received <- v
	}
	return nil
}

func (s *signalSink) values() []interface{} {
	s.mu.Lock()
	defer s.mu.Unlock()
	return append([]interface{}(nil), s.got...)
}

// gatedSink records values, announces that its input was closed, and then
// stays inside Export until the test allows it to return.
type gatedSink struct {
	mu          sync.Mutex
	got         []interface{}
	closed      chan struct{}
	allowReturn chan struct{}
	returned    chan struct{}
}

func (s *gatedSink) Export(ch chan interface{}) error {
	for v := range ch {
		s.mu.Lock()
		s.got = append(s.got, v)
		s.mu.Unlock()
	}
	close(s.closed)
	<-s.allowReturn
	close(s.returned)
	return nil
}

func (s *gatedSink) values() []interface{} {
	s.mu.Lock()
	defer s.mu.Unlock()
	return append([]interface{}(nil), s.got...)
}

func runStartE(g *geziyor.Geziyor) <-chan error {
	ch := make(chan error, 1)
	go func() { ch <- g.StartE() }()
	return ch
}

func waitClosed(t *testing.T, ch <-chan struct{}, message string) {
	t.Helper()
	select {
	case <-ch:
	case <-time.After(pipelineTimeout):
		t.Fatal(message)
	}
}

func waitValue(t *testing.T, ch <-chan interface{}, message string) interface{} {
	t.Helper()
	select {
	case v := <-ch:
		return v
	case <-time.After(pipelineTimeout):
		t.Fatal(message)
	}
	return nil
}

func waitError(t *testing.T, ch <-chan error) error {
	t.Helper()
	select {
	case err := <-ch:
		return err
	case <-time.After(pipelineTimeout):
		t.Fatal("StartE did not return")
		return nil
	}
}

func startDone(ch <-chan error) bool {
	select {
	case <-ch:
		return true
	default:
		return false
	}
}

// joinedErrors flattens an errors.Join result without depending on any
// geziyor-specific error type.
func joinedErrors(err error) []error {
	if err == nil {
		return nil
	}
	if joined, ok := err.(interface{ Unwrap() []error }); ok {
		return joined.Unwrap()
	}
	return []error{err}
}

// mentionsIndex reports whether an export error carries the zero-based
// exporter index as a standalone token (for example "exporter 2").
func mentionsIndex(err error, index int) bool {
	if err == nil {
		return false
	}
	want := fmt.Sprintf("%d", index)
	for _, field := range strings.FieldsFunc(err.Error(), func(r rune) bool {
		return !(r >= '0' && r <= '9')
	}) {
		if field == want {
			return true
		}
	}
	return false
}

// errorWithIndex returns the joined error that mentions index, if any.
func errorWithIndex(err error, index int) (error, bool) {
	for _, e := range joinedErrors(err) {
		if mentionsIndex(e, index) {
			return e, true
		}
	}
	return nil, false
}

func TestCleanBatch(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			fmt.Fprint(w, "catalog")
		}))
		defer server.Close()

		sinkA, sinkB := &recordingSink{}, &recordingSink{}
		cleanErr := errors.New("clean rejected batch")
		cleanCalls := 0
		var body string

		g := geziyor.NewGeziyor(&geziyor.Options{
			StartURLs:         []string{server.URL},
			RobotsTxtDisabled: true,
			LogDisabled:       true,
			Exporters:         []export.Exporter{sinkA, sinkB},
			CleanFunc: func(in interface{}) ([]interface{}, error) {
				cleanCalls++
				switch n := in.(int); n {
				case 1:
					// One input producing multiple results: they must stay
					// together and in order for every exporter.
					return []interface{}{"one-a", "one-b"}, nil
				case 2:
					// Zero results filters the input out.
					return nil, nil
				case 3:
					// Results returned alongside an error are discarded too.
					return []interface{}{"dropped-a", "dropped-b"}, cleanErr
				default:
					return []interface{}{fmt.Sprintf("v%d", n)}, nil
				}
			},
			ParseFunc: func(g *geziyor.Geziyor, r *client.Response) {
				body = string(r.Body)
				for _, n := range []int{1, 2, 3, 4} {
					g.Exports <- n
				}
			},
		})

		err := g.StartE()
		c.Check(body == "catalog", "callback saw body %q, want catalog", body)

		want := []interface{}{"one-a", "one-b", "v4"}
		c.Check(reflect.DeepEqual(sinkA.values(), want), "sink 0 got %v, want %v", sinkA.values(), want)
		c.Check(reflect.DeepEqual(sinkB.values(), want), "sink 1 got %v, want %v", sinkB.values(), want)
		c.Check(cleanCalls == 4, "CleanFunc called %d times, want once per input (4)", cleanCalls)
		c.Check(err != nil, "StartE returned nil, want the clean error")
		c.Check(errors.Is(err, cleanErr), "errors.Is could not find the clean error in %v", err)

		// Without CleanFunc records must pass through untouched.
		plain := &recordingSink{}
		g2 := geziyor.NewGeziyor(&geziyor.Options{
			StartURLs:         []string{server.URL},
			RobotsTxtDisabled: true,
			LogDisabled:       true,
			Exporters:         []export.Exporter{plain},
			ParseFunc: func(g *geziyor.Geziyor, r *client.Response) {
				g.Exports <- 5
				g.Exports <- 6
			},
		})
		err2 := g2.StartE()
		c.Check(err2 == nil, "StartE without CleanFunc returned %v", err2)
		c.Check(reflect.DeepEqual(plain.values(), []interface{}{5, 6}), "pass-through sink got %v, want [5 6]", plain.values())
	})
}

func TestSinkIsolation(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			fmt.Fprint(w, "rows")
		}))
		defer server.Close()

		failErr := errors.New("sink zero failed")
		failed := &immediateErrSink{err: failErr, returned: make(chan struct{})}
		slow := &slowSink{first: make(chan struct{}), release: make(chan struct{})}
		fast := &signalSink{received: make(chan interface{}, 4)}
		early := &immediateNilSink{returned: make(chan struct{})}

		g := geziyor.NewGeziyor(&geziyor.Options{
			StartURLs:         []string{server.URL},
			RobotsTxtDisabled: true,
			LogDisabled:       true,
			Exporters:         []export.Exporter{failed, slow, fast, early},
			ParseFunc: func(g *geziyor.Geziyor, r *client.Response) {
				for _, n := range []int{10, 20, 30, 40} {
					g.Exports <- n
				}
			},
		})

		errCh := runStartE(g)
		waitClosed(t, slow.first, "slow sink never received its first record")

		// The dispatcher delivers to sinks in order and waits on the slow sink,
		// so the fast sink must have received only the first record so far. No
		// record may be buffered, dropped or replayed to get past this point.
		first := waitValue(t, fast.received, "fast sink never received the first record")
		c.Check(first == 10, "fast sink first record = %v, want 10", first)
		select {
		case v := <-fast.received:
			c.Check(false, "fast sink received %v while the slow sink was still blocking", v)
		default:
		}
		c.Check(reflect.DeepEqual(fast.values(), []interface{}{10}), "fast sink got %v before release, want [10]", fast.values())

		close(slow.release)
		err := waitError(t, errCh)

		wantAll := []interface{}{10, 20, 30, 40}
		c.Check(reflect.DeepEqual(slow.values(), wantAll), "slow sink got %v, want %v", slow.values(), wantAll)
		c.Check(reflect.DeepEqual(fast.values(), wantAll), "fast sink got %v, want %v", fast.values(), wantAll)

		c.Check(err != nil, "StartE returned nil, want exporter failures")
		c.Check(errors.Is(err, failErr), "errors.Is could not find the failing exporter error in %v", err)
		_, failIndexed := errorWithIndex(err, 0)
		c.Check(failIndexed, "no joined error attributes failure to exporter 0 in %v", err)
		_, earlyIndexed := errorWithIndex(err, 3)
		c.Check(earlyIndexed, "nil-returning early exporter at index 3 was not reported in %v", err)
		waitClosed(t, failed.returned, "failing sink did not return")
		waitClosed(t, early.returned, "early sink did not return")

		// When every exporter exits, Exports must still be drained so the
		// callback's direct sends terminate, and both failures must be kept.
		errA, errB := errors.New("sink A failed"), errors.New("sink B failed")
		allExit := geziyor.NewGeziyor(&geziyor.Options{
			StartURLs:         []string{server.URL},
			RobotsTxtDisabled: true,
			LogDisabled:       true,
			Exporters: []export.Exporter{
				&immediateErrSink{err: errA, returned: make(chan struct{})},
				&immediateErrSink{err: errB, returned: make(chan struct{})},
			},
			ParseFunc: func(g *geziyor.Geziyor, r *client.Response) {
				for i := 0; i < 5; i++ {
					g.Exports <- i
				}
			},
		})
		errAll := allExit.StartE()
		c.Check(errors.Is(errAll, errA), "errors.Is could not find sink A error in %v", errAll)
		c.Check(errors.Is(errAll, errB), "errors.Is could not find sink B error in %v", errAll)
		_, indexedA := errorWithIndex(errAll, 0)
		_, indexedB := errorWithIndex(errAll, 1)
		c.Check(indexedA, "no joined error attributes sink A failure to index 0 in %v", errAll)
		c.Check(indexedB, "no joined error attributes sink B failure to index 1 in %v", errAll)
	})
}

func TestExportCompletion(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			fmt.Fprint(w, "complete")
		}))
		defer server.Close()

		callbackEntered := make(chan struct{})
		callbackRelease := make(chan struct{})
		sink := &gatedSink{
			closed:      make(chan struct{}),
			allowReturn: make(chan struct{}),
			returned:    make(chan struct{}),
		}

		g := geziyor.NewGeziyor(&geziyor.Options{
			StartURLs:         []string{server.URL},
			RobotsTxtDisabled: true,
			LogDisabled:       true,
			Exporters:         []export.Exporter{sink},
			ParseFunc: func(g *geziyor.Geziyor, r *client.Response) {
				close(callbackEntered)
				<-callbackRelease
				g.Exports <- "row"
			},
		})

		errCh := runStartE(g)
		waitClosed(t, callbackEntered, "callback never started")
		c.Check(!startDone(errCh), "StartE returned before the request callback finished")

		close(callbackRelease)
		waitClosed(t, sink.closed, "dispatcher never closed the exporter input")

		// The callback is done and the input is closed, but Export has not
		// returned, so StartE must still be waiting.
		c.Check(!startDone(errCh), "StartE returned before Export exited")

		close(sink.allowReturn)
		err := waitError(t, errCh)
		waitClosed(t, sink.returned, "Export goroutine did not exit before StartE returned")
		c.Check(err == nil, "StartE returned %v after a normal shutdown", err)
		c.Check(reflect.DeepEqual(sink.values(), []interface{}{"row"}), "gated sink got %v, want [row]", sink.values())

		// With no exporters, configured cleaning still runs and Exports is
		// drained instead of blocking on an absent receiver.
		cleanCalls := 0
		noSink := geziyor.NewGeziyor(&geziyor.Options{
			StartURLs:         []string{server.URL},
			RobotsTxtDisabled: true,
			LogDisabled:       true,
			CleanFunc: func(in interface{}) ([]interface{}, error) {
				cleanCalls++
				return []interface{}{in}, nil
			},
			ParseFunc: func(g *geziyor.Geziyor, r *client.Response) {
				for i := 0; i < 3; i++ {
					g.Exports <- i
				}
			},
		})
		errNoSink := noSink.StartE()
		c.Check(errNoSink == nil, "StartE without exporters returned %v", errNoSink)
		c.Check(cleanCalls == 3, "CleanFunc ran %d times without exporters, want 3", cleanCalls)

		// One instance may only start once.
		errAgain := noSink.StartE()
		c.Check(errAgain != nil, "second StartE returned nil, want a non-nil error")

		// Start keeps working and shares the same implementation.
		started := make(chan struct{})
		var startCalls int
		oldStart := geziyor.NewGeziyor(&geziyor.Options{
			StartURLs:         []string{server.URL},
			RobotsTxtDisabled: true,
			LogDisabled:       true,
			ParseFunc: func(g *geziyor.Geziyor, r *client.Response) {
				startCalls++
				close(started)
			},
		})
		oldStart.Start()
		waitClosed(t, started, "Start never ran the request callback")
		c.Check(oldStart.StartE() != nil, "StartE after Start returned nil, want a non-nil error")
		c.Check(startCalls == 1, "callback ran %d times across two starts, want 1", startCalls)
	})
}

func TestBuiltinErrors(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		dir := t.TempDir()

		// Successful output formats are preserved.
		jsonPath := filepath.Join(dir, "rows.jsonl")
		jsonCh := make(chan interface{}, 1)
		jsonCh <- []string{"A", "B"}
		close(jsonCh)
		jsonErr := (&export.JSONLine{FileName: jsonPath}).Export(jsonCh)
		c.Check(jsonErr == nil, "JSONLine Export returned %v for good data", jsonErr)
		jsonBytes, readErr := os.ReadFile(jsonPath)
		c.Check(readErr == nil, "reading JSONLine output: %v", readErr)
		c.Check(string(jsonBytes) == "[\"A\",\"B\"]\n", "JSONLine output = %q, want [\"A\",\"B\"]\\n", jsonBytes)

		csvPath := filepath.Join(dir, "rows.csv")
		csvCh := make(chan interface{}, 1)
		csvCh <- []string{"A", "B"}
		close(csvCh)
		csvErr := (&export.CSV{FileName: csvPath}).Export(csvCh)
		c.Check(csvErr == nil, "CSV Export returned %v for good data", csvErr)
		csvBytes, readErr := os.ReadFile(csvPath)
		c.Check(readErr == nil, "reading CSV output: %v", readErr)
		c.Check(string(csvBytes) == "A,B\n", "CSV output = %q, want A,B\\n", csvBytes)

		// Encoding failures must come back from Export instead of only being
		// logged as a success.
		badJSONCh := make(chan interface{}, 2)
		badJSONCh <- func() {}
		badJSONCh <- []string{"ok"}
		close(badJSONCh)
		badJSON := (&export.JSONLine{FileName: filepath.Join(dir, "bad.jsonl")}).Export(badJSONCh)
		c.Check(badJSON != nil, "JSONLine reported success for unencodable data")
		c.Check(badJSON != nil && strings.Contains(badJSON.Error(), "json encoding"), "JSONLine error = %v, want a json encoding error", badJSON)

		// CSV write failures (invalid delimiter) must come back from Export.
		badCSVCh := make(chan interface{}, 1)
		badCSVCh <- []string{"value"}
		close(badCSVCh)
		badCSV := (&export.CSV{FileName: filepath.Join(dir, "bad.csv"), Comma: '"'}).Export(badCSVCh)
		c.Check(badCSV != nil, "CSV reported success for an unwritable row")
		c.Check(badCSV != nil && strings.Contains(badCSV.Error(), "csv write"), "CSV error = %v, want a csv write error", badCSV)

		// Where the platform offers a write-only-failing device, assert that a
		// write error is also surfaced rather than logged and ignored.
		if _, statErr := os.Stat("/dev/full"); statErr == nil {
			ch := make(chan interface{}, 1)
			ch <- map[string]string{"key": "value"}
			close(ch)
			err := (&export.JSONLine{FileName: "/dev/full"}).Export(ch)
			c.Check(err != nil, "JSONLine reported success when writes fail")
		}

		// Built-in exporter failures reach StartE with their exporter index.
		server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			fmt.Fprint(w, "builtin")
		}))
		defer server.Close()

		g := geziyor.NewGeziyor(&geziyor.Options{
			StartURLs:         []string{server.URL},
			RobotsTxtDisabled: true,
			LogDisabled:       true,
			Exporters: []export.Exporter{
				&export.JSONLine{FileName: dir}, // opening a directory fails
				&export.CSV{FileName: filepath.Join(dir, "bad2.csv"), Comma: '"'},
			},
			ParseFunc: func(g *geziyor.Geziyor, r *client.Response) {
				g.Exports <- []string{"A", "B"}
			},
		})
		err := g.StartE()
		c.Check(err != nil, "StartE returned nil, want built-in exporter errors")
		if err != nil {
			for _, index := range []int{0, 1} {
				joined, ok := errorWithIndex(err, index)
				c.Check(ok, "no joined error attributes a built-in failure to exporter %d in %v", index, err)
				if !ok {
					continue
				}
				switch index {
				case 0:
					c.Check(strings.Contains(joined.Error(), "output file creation"), "JSONLine error = %v, want a creation error", joined)
				case 1:
					c.Check(strings.Contains(joined.Error(), "csv write"), "CSV error = %v, want a write error", joined)
				}
			}
		}
	})
}
