// Command consume demonstrates concurrent event processing with explicit
// acknowledgement using Connection.Consume.
//
// It runs a tiny SSE server that publishes numbered events, consumes them with
// a bounded window and hands each event to a worker goroutine. An event is
// acknowledged only after the worker finished its work. The example also shows
// what the acknowledgement does NOT guarantee: it says the consumer is done with
// the event, not that some external storage committed it exactly once. A
// reconnection can replay unacknowledged events, so the business operation must
// be idempotent (or keyed by the event ID) if it writes to an external system.
package main

import (
	"context"
	"errors"
	"fmt"
	"log"
	"net/http"
	"os"
	"os/signal"
	"sync"
	"syscall"
	"time"

	"github.com/tmaxmax/go-sse"
)

func main() {
	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer cancel()

	srv := &http.Server{
		Addr:              "127.0.0.1:8000",
		Handler:           newServer(),
		ReadHeaderTimeout: 10 * time.Second,
	}

	go func() {
		if err := srv.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
			log.Printf("server error: %v", err)
			cancel()
		}
	}()

	if err := consume(ctx); err != nil && !errors.Is(err, context.Canceled) {
		fmt.Fprintln(os.Stderr, err)
	}

	shutdownCtx, shutdownCancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer shutdownCancel()
	_ = srv.Shutdown(shutdownCtx)
}

// newServer publishes one event per 100ms until the request is cancelled.
func newServer() http.Handler {
	s := &sse.Server{}
	mux := http.NewServeMux()
	mux.Handle("/events", s)

	go func() {
		for n := 1; ; n++ {
			msg := &sse.Message{ID: sse.ID(fmt.Sprintf("evt-%d", n))}
			msg.AppendData(fmt.Sprintf("job %d", n))
			if err := s.Publish(msg); err != nil {
				return
			}
			time.Sleep(100 * time.Millisecond)
		}
	}()

	return mux
}

// consume processes events concurrently. It is the working example of the
// acknowledgement contract:
//
//   - the handler only enqueues work and returns quickly, so parsing is never
//     blocked by slow business logic;
//   - the acknowledgement function is stored and called by the worker when it is
//     done with the event;
//   - the window bounds how many unacknowledged events may be in flight: when
//     it is full, Consume stops parsing instead of buffering the rest;
//   - acknowledgements for events delivered by a previous connection attempt
//     become invalid when the connection is reestablished.
func consume(ctx context.Context) error {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, "http://127.0.0.1:8000/events", http.NoBody)
	if err != nil {
		return err
	}

	conn := sse.NewConnection(req)

	// Work is bounded both by the Consume window and by the worker pool. The
	// window is what bounds unacknowledged deliveries and memory in the SSE
	// client itself; the pool bounds concurrency of the business operation.
	const (
		window  = 8
		workers = 4
	)

	type job struct {
		event sse.Event
		ack   func() bool
	}

	jobs := make(chan job, window)
	var workersDone sync.WaitGroup
	workersDone.Add(workers)

	for range workers {
		go func() {
			defer workersDone.Done()
			for j := range jobs {
				// The actual work: pretend to persist by keying on the event ID.
				// Doing this idempotently is what makes replays harmless.
				err := processEvent(ctx, j.event)

				// Acknowledge only after the work completed. A false result
				// means this delivery is stale: either it was acknowledged
				// before, or the connection was reestablished and the event
				// will be replayed. Either way, do not treat the event as new
				// work on the next attempt if processEvent is idempotent.
				ok := j.ack()
				if !ok {
					fmt.Printf("stale acknowledgement for %s (replayed after reconnect)\n", j.event.LastEventID)
				}
				if err != nil {
					fmt.Printf("failed to process %s: %v\n", j.event.LastEventID, err)
				}
			}
		}()
	}

	err = conn.Consume(window, func(event sse.Event, ack func() bool) {
		// Return immediately: Consume keeps parsing as long as the window has
		// room. The worker calls ack when it is done.
		select {
		case jobs <- job{event: event, ack: ack}:
		case <-ctx.Done():
			// The connection is shutting down; ack is about to be invalidated.
		}
	})

	close(jobs)
	workersDone.Wait()

	return err
}

// processEvent simulates a piece of business work. It is deliberately
// idempotent: the same event ID may be processed again after a reconnection,
// because the SSE client does not deduplicate replayed events. Acknowledgement
// means "the business logic is done with this event"; it does not make the
// external side effect exactly-once.
func processEvent(ctx context.Context, event sse.Event) error {
	select {
	case <-time.After(50 * time.Millisecond):
		fmt.Printf("processed %s: %s\n", event.LastEventID, event.Data)
		return nil
	case <-ctx.Done():
		return ctx.Err()
	}
}
