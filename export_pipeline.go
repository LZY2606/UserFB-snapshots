package geziyor

import (
	"errors"
	"fmt"
	"sync"

	"github.com/geziyor/geziyor/export"
)

var (
	// ErrAlreadyStarted is returned when Start or StartE is called more than
	// once on the same Geziyor instance.
	ErrAlreadyStarted = errors.New("geziyor: instance already started")

	// ErrExporterExitedEarly is returned (wrapped with the exporter index) when
	// an Exporter.Export call returns nil before its input channel is closed.
	ErrExporterExitedEarly = errors.New("exporter returned before its input was closed")
)

// ExporterError attributes an export failure to the exporter at Index in
// Options.Exporters (zero-based).
type ExporterError struct {
	Index int
	Err   error
}

func (e *ExporterError) Error() string {
	return fmt.Sprintf("exporter %d: %v", e.Index, e.Err)
}

func (e *ExporterError) Unwrap() error {
	return e.Err
}

// exporterSink is one exporter's input channel plus the state needed to tell a
// normal shutdown (input closed, Export returned nil) apart from an early
// return.
type exporterSink struct {
	index int
	ch    chan interface{}
	done  chan struct{} // closed as soon as Export returns

	mu          sync.Mutex
	inputClosed bool
	returned    bool

	// err is written before wgExporters.Done and read after wgExporters.Wait.
	err error
}

// send hands one value to the exporter. It never blocks on an exporter that
// has already returned, and it applies ordinary backpressure to live ones.
func (s *exporterSink) send(value interface{}) {
	select {
	case <-s.done:
		return
	default:
	}

	select {
	case s.ch <- value:
	case <-s.done:
	}
}

// closeInput closes the exporter's input, unless the exporter already returned
// on its own. The mutex orders this against the classification in the exporter
// goroutine so only one side can make that decision.
func (s *exporterSink) closeInput() {
	s.mu.Lock()
	if s.returned {
		s.mu.Unlock()
		return
	}
	s.inputClosed = true
	s.mu.Unlock()
	close(s.ch)
}

// exportDispatcher is the single goroutine that receives raw records from
// Exports, cleans them once, and fans the cleaned sequence out to all live
// exporters.
type exportDispatcher struct {
	sinks     []*exporterSink
	cleanErrs []error // written only by the dispatcher goroutine
	done      chan struct{}
}

// startExporters launches the exporter goroutines and the single dispatcher.
// The caller must wait on the returned dispatcher's done channel and on
// g.wgExporters before collecting errors.
func (g *Geziyor) startExporters() *exportDispatcher {
	d := &exportDispatcher{done: make(chan struct{})}

	for i, exporter := range g.Opt.Exporters {
		sink := &exporterSink{
			index: i,
			ch:    make(chan interface{}),
			done:  make(chan struct{}),
		}
		d.sinks = append(d.sinks, sink)

		g.wgExporters.Add(1)
		go func(s *exporterSink, e export.Exporter) {
			defer g.wgExporters.Done()

			err := e.Export(s.ch)

			s.mu.Lock()
			s.returned = true
			normalShutdown := s.inputClosed
			s.mu.Unlock()
			close(s.done)

			switch {
			case err != nil:
				s.err = &ExporterError{Index: s.index, Err: err}
			case !normalShutdown:
				s.err = &ExporterError{Index: s.index, Err: ErrExporterExitedEarly}
			}
		}(sink, exporter)
	}

	go func() {
		defer close(d.done)

		for data := range g.Exports {
			var cleaned []interface{}
			if g.Opt.CleanFunc != nil {
				var err error
				cleaned, err = g.Opt.CleanFunc(data)
				if err != nil {
					// The whole batch is dropped, including any results
					// returned alongside the error. Keep going.
					d.cleanErrs = append(d.cleanErrs, fmt.Errorf("clean: %w", err))
					continue
				}
			} else {
				cleaned = []interface{}{data}
			}

			// Results from one input are sent consecutively to every live
			// exporter, so each exporter sees the same cleaned sequence.
			for _, value := range cleaned {
				for _, sink := range d.sinks {
					sink.send(value)
				}
			}
		}

		// Inputs are all closed by the dispatcher, once no more records can
		// arrive. This is also what distinguishes normal shutdown from an
		// exporter that returned early.
		for _, sink := range d.sinks {
			sink.closeInput()
		}
	}()

	return d
}

// errors returns clean errors (in dispatch order) followed by exporter errors
// (in exporter index order). Must be called after the dispatcher has finished
// and wgExporters has been waited on.
func (d *exportDispatcher) errors() []error {
	var errs []error
	errs = append(errs, d.cleanErrs...)
	for _, sink := range d.sinks {
		if sink.err != nil {
			errs = append(errs, sink.err)
		}
	}
	return errs
}
