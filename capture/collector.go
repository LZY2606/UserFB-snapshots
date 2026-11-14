package capture

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"github.com/mafredri/cdp"
	"github.com/mafredri/cdp/protocol/fetch"
	cdpio "github.com/mafredri/cdp/protocol/io"
	"github.com/mafredri/cdp/protocol/network"
)

// Lifecycle phases of a paused request known to the collector.
const (
	// The requestPaused event has been received but no protocol
	// command affecting the request has been acknowledged.
	phaseReceived = iota
	// takeResponseBodyAsStream was sent; the handle is not known yet.
	phaseTakeSent
	// A stream handle was acquired.
	phaseHandle
	// continueRequest succeeded. The request is resumed and must
	// not be failed during shutdown.
	phaseContinued
	// fulfillRequest succeeded. The request must never be failed,
	// even if publishing the sample fails afterwards.
	phaseFulfilled
	// failRequest was already attempted.
	phaseFailed
)

type requestState struct {
	phase int
}

// registry tracks every requestPaused event received during a Run so
// that shutdown can fail any request left in a paused state.
type registry struct {
	mu sync.Mutex
	m  map[fetch.RequestID]*requestState
}

func newRegistry() *registry {
	return &registry{m: make(map[fetch.RequestID]*requestState)}
}

func (r *registry) add(id fetch.RequestID) {
	r.mu.Lock()
	if _, ok := r.m[id]; !ok {
		r.m[id] = &requestState{phase: phaseReceived}
	}
	r.mu.Unlock()
}

func (r *registry) setPhase(id fetch.RequestID, phase int) {
	r.mu.Lock()
	if st, ok := r.m[id]; ok {
		st.phase = phase
	}
	r.mu.Unlock()
}

// failRequest fails the request with Aborted unless it already reached
// a terminal phase. The state transition happens before the network
// call, so the request is failed at most once.
func (r *registry) failRequest(ctx context.Context, client *cdp.Client, id fetch.RequestID) error {
	r.mu.Lock()
	st, ok := r.m[id]
	if !ok {
		st = &requestState{}
		r.m[id] = st
	}
	if st.phase == phaseContinued || st.phase == phaseFulfilled || st.phase == phaseFailed {
		r.mu.Unlock()
		return nil
	}
	st.phase = phaseFailed
	r.mu.Unlock()

	return client.Fetch.FailRequest(ctx, fetch.NewFailRequestArgs(id, network.ErrorReasonAborted))
}

// sweep fails every request that is still paused after the workers
// stopped. Requests already continued, fulfilled or failed are left
// alone.
func (r *registry) sweep(ctx context.Context, client *cdp.Client) []error {
	r.mu.Lock()
	ids := make([]fetch.RequestID, 0, len(r.m))
	for id := range r.m {
		ids = append(ids, id)
	}
	r.mu.Unlock()

	var errs []error
	for _, id := range ids {
		if err := r.failRequest(ctx, client, id); err != nil {
			errs = append(errs, fmt.Errorf("capture: failRequest %s: %w", id, err))
		}
	}
	return errs
}

type collector struct {
	client *cdp.Client
	opts   Options

	runCtx      context.Context
	runCancel   context.CancelFunc
	reg         *registry
	sem         chan struct{}
	wg          sync.WaitGroup
	publishedMu sync.Mutex
	published   int
	errMu       sync.Mutex
	firstErr    error
	enableTried bool
}

// Run enables Fetch interception, collects qualifying response bodies
// into sample directories under opts.Dir and lets the page continue.
//
// Run owns the Fetch domain for its duration. It blocks until ctx is
// canceled, the connection closes or an error occurs. The shared
// connection is not closed. The returned count only includes samples
// that were fully published.
func Run(ctx context.Context, client *cdp.Client, opts Options) (int, error) {
	if err := validate(client, opts); err != nil {
		return 0, err
	}

	col := &collector{
		client: client,
		opts:   opts,
		reg:    newRegistry(),
		sem:    make(chan struct{}, opts.Workers),
	}
	col.runCtx, col.runCancel = context.WithCancel(ctx)
	defer col.runCancel()

	var errs []error

	// Subscribe before enabling: events emitted immediately by
	// Fetch.enable must not be missed. The stream is bound to the
	// run context so that canceling the run (and not only the
	// caller's context) unblocks Recv during shutdown.
	events, err := client.Fetch.RequestPaused(col.runCtx)
	if err != nil {
		return 0, fmt.Errorf("capture: subscribe Fetch.requestPaused: %w", err)
	}
	defer events.Close()

	col.enableTried = true
	pattern := "*"
	enableArgs := fetch.NewEnableArgs().SetPatterns([]fetch.RequestPattern{{
		URLPattern:   &pattern,
		RequestStage: fetch.RequestStageResponse,
	}})
	if err := client.Fetch.Enable(ctx, enableArgs); err != nil {
		errs = append(errs, fmt.Errorf("capture: Fetch.enable: %w", err))
		// Enable was attempted and failed; disable still gets a try.
		errs = append(errs, col.shutdown()...)
		return col.publishedCount(), errors.Join(errs...)
	}

	col.receive(events)

	errs = append(errs, col.shutdown()...)
	return col.publishedCount(), errors.Join(errs...)
}

func validate(client *cdp.Client, opts Options) error {
	if client == nil {
		return errors.New("capture: nil client")
	}
	if opts.Workers <= 0 {
		return fmt.Errorf("capture: Workers must be positive, got %d", opts.Workers)
	}
	if opts.MaxBodyBytes <= 0 {
		return fmt.Errorf("capture: MaxBodyBytes must be positive, got %d", opts.MaxBodyBytes)
	}
	entries, err := os.ReadDir(opts.Dir)
	if err != nil {
		return fmt.Errorf("capture: output directory: %w", err)
	}
	if len(entries) != 0 {
		return fmt.Errorf("capture: output directory %q must be empty", opts.Dir)
	}
	return nil
}

// receive dispatches requestPaused events to workers until the run
// context ends or an event can no longer be received.
func (col *collector) receive(events fetch.RequestPausedClient) {
	for {
		event, err := events.Recv()
		if err != nil {
			// Context cancellation or connection closure ends the run.
			col.report(err)
			return
		}
		col.reg.add(event.RequestID)

		select {
		case col.sem <- struct{}{}:
		case <-col.runCtx.Done():
			// Never dispatched: the sweep during shutdown fails it.
			col.report(col.runCtx.Err())
			return
		}

		col.wg.Add(1)
		go col.handle(event)
	}
}

// shutdown cancels and joins all workers, fails any request still
// paused, then disables the Fetch domain using an independent
// context. Remote cleanup never uses the canceled run context.
func (col *collector) shutdown() []error {
	col.runCancel()
	col.wg.Wait()

	var errs []error

	// Independent one-second context for requests the workers could
	// not settle themselves.
	sweepCtx, sweepCancel := context.WithTimeout(context.Background(), time.Second)
	errs = append(errs, col.reg.sweep(sweepCtx, col.client)...)
	sweepCancel()

	if col.enableTried {
		// Another independent one-second context for Fetch.disable,
		// issued only after every worker has stopped reading.
		disableCtx, disableCancel := context.WithTimeout(context.Background(), time.Second)
		if err := col.client.Fetch.Disable(disableCtx); err != nil {
			errs = append(errs, fmt.Errorf("capture: Fetch.disable: %w", err))
		}
		disableCancel()
	}

	col.errMu.Lock()
	if col.firstErr != nil {
		errs = append([]error{col.firstErr}, errs...)
	}
	col.errMu.Unlock()
	return errs
}

// report records the first fatal error and cancels the run so that no
// new work is accepted and running workers stop.
func (col *collector) report(err error) {
	if err == nil {
		return
	}
	col.errMu.Lock()
	if col.firstErr == nil {
		col.firstErr = err
	}
	col.errMu.Unlock()
	col.runCancel()
}

func (col *collector) publishedCount() int {
	col.publishedMu.Lock()
	defer col.publishedMu.Unlock()
	return col.published
}

func (col *collector) handle(event *fetch.RequestPausedReply) {
	defer col.wg.Done()
	defer func() { <-col.sem }()

	id := event.RequestID

	if !col.capturable(event) {
		if err := col.client.Fetch.ContinueRequest(col.runCtx, fetch.NewContinueRequestArgs(id)); err != nil {
			// The request is still paused; fail it during cleanup.
			if failErr := col.failRequest(id); failErr != nil {
				col.report(fmt.Errorf("capture: failRequest %s: %w", id, failErr))
			}
			col.report(fmt.Errorf("capture: continueRequest %s: %w", id, err))
			return
		}
		col.reg.setPhase(id, phaseContinued)
		return
	}

	col.reg.setPhase(id, phaseTakeSent)
	stream, err := col.client.Fetch.TakeResponseBodyAsStream(
		col.runCtx, fetch.NewTakeResponseBodyAsStreamArgs(id))
	if err != nil {
		// The handle was never acquired: only the known request id
		// may be used, no stream handle may be invented.
		if failErr := col.failRequest(id); failErr != nil {
			col.report(fmt.Errorf("capture: failRequest %s: %w", id, failErr))
		}
		col.report(fmt.Errorf("capture: takeResponseBodyAsStream %s: %w", id, err))
		return
	}
	handle := stream.Stream
	col.reg.setPhase(id, phaseHandle)

	// Single, idempotent closer for the remote handle. Every error
	// path goes through it, so IO.close is attempted at most once.
	var closed bool
	closeStream := func() error {
		if closed {
			return nil
		}
		closed = true
		ctx, cancel := context.WithTimeout(context.Background(), time.Second)
		defer cancel()
		return col.client.IO.Close(ctx, cdpio.NewCloseArgs(handle))
	}

	tmp, err := os.MkdirTemp(col.opts.Dir, ".tmp-*")
	if err != nil {
		col.cleanupFailed(id, closeStream, "", err)
		return
	}

	body, err := col.readBody(event, tmp, handle, closeStream)
	if err != nil {
		col.cleanupFailed(id, closeStream, tmp, err)
		return
	}

	// EOF reached: the remote handle and the local file are already
	// closed by readBody. Hand the same body back to the page.
	fulfillErr := col.client.Fetch.FulfillRequest(col.runCtx, col.fulfillArgs(event, body))
	if fulfillErr != nil {
		if failErr := col.failRequest(id); failErr != nil {
			col.report(fmt.Errorf("capture: failRequest %s: %w", id, failErr))
		}
		os.RemoveAll(tmp)
		col.report(fmt.Errorf("capture: fulfillRequest %s: %w", id, fulfillErr))
		return
	}
	col.reg.setPhase(id, phaseFulfilled)

	final := filepath.Join(col.opts.Dir, sampleDirName(id))
	if err := os.Rename(tmp, final); err != nil {
		// Fulfilled requests must not be failed; the sample simply
		// stays unpublished.
		os.RemoveAll(tmp)
		col.report(fmt.Errorf("capture: publish %s: %w", id, err))
		return
	}

	col.publishedMu.Lock()
	col.published++
	col.publishedMu.Unlock()
}

// cleanupFailed closes the stream handle at most once, fails the not
// yet fulfilled request and removes its unpublished temp directory.
func (col *collector) cleanupFailed(id fetch.RequestID, closeStream func() error, tmp string, cause error) {
	if closeErr := closeStream(); closeErr != nil {
		col.report(fmt.Errorf("capture: IO.close %s: %w", id, closeErr))
	}
	if failErr := col.failRequest(id); failErr != nil {
		col.report(fmt.Errorf("capture: failRequest %s: %w", id, failErr))
	}
	if tmp != "" {
		if rmErr := os.RemoveAll(tmp); rmErr != nil {
			col.report(fmt.Errorf("capture: remove temp dir: %w", rmErr))
		}
	}
	col.report(cause)
}

// failRequest fails a paused request with Aborted using an independent
// one-second cleanup context.
func (col *collector) failRequest(id fetch.RequestID) error {
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	return col.reg.failRequest(ctx, col.client, id)
}

// capturable reports whether the paused response qualifies for body
// capture: GET requests with status 200 and no Content-Encoding other
// than identity.
func (col *collector) capturable(event *fetch.RequestPausedReply) bool {
	if event.Request.Method != "GET" {
		return false
	}
	if event.ResponseStatusCode == nil || *event.ResponseStatusCode != 200 {
		return false
	}
	for _, h := range event.ResponseHeaders {
		if strings.EqualFold(h.Name, "Content-Encoding") {
			if !strings.EqualFold(h.Value, "identity") {
				return false
			}
		}
	}
	return true
}

// readBody streams the decoded response body into tmp/body.bin,
// hashing it and enforcing MaxBodyBytes on decoded length. Once EOF is
// reached the remote handle and the local file are closed and the
// (bounded) body is read back for fulfillment.
func (col *collector) readBody(event *fetch.RequestPausedReply, tmp string, handle cdpio.StreamHandle, closeStream func() error) (body []byte, err error) {
	bodyPath := filepath.Join(tmp, "body.bin")
	f, err := os.Create(bodyPath)
	if err != nil {
		return nil, fmt.Errorf("capture: create body file: %w", err)
	}

	reader := col.client.NewSequentialIOStreamReader(col.runCtx, handle)
	hash := sha256.New()
	buf := make([]byte, 32*1024)
	var written int64

	readErr := func() error {
		for {
			n, rerr := reader.Read(buf)
			if n > 0 {
				if written+int64(n) > col.opts.MaxBodyBytes {
					return ErrBodyTooLarge
				}
				if _, werr := f.Write(buf[:n]); werr != nil {
					return fmt.Errorf("capture: write body file: %w", werr)
				}
				hash.Write(buf[:n])
				written += int64(n)
			}
			if rerr == io.EOF {
				return nil
			}
			if rerr != nil {
				return fmt.Errorf("capture: read stream %s: %w", event.RequestID, rerr)
			}
		}
	}()

	// EOF or not, the remote handle and local file must be closed.
	// The shared closer makes sure IO.close runs at most once.
	closeErr := closeStream()
	fileErr := f.Close()

	var errs []error
	if readErr != nil {
		errs = append(errs, readErr)
	}
	if closeErr != nil {
		errs = append(errs, fmt.Errorf("capture: IO.close %s: %w", event.RequestID, closeErr))
	}
	if fileErr != nil {
		errs = append(errs, fmt.Errorf("capture: close body file: %w", fileErr))
	}
	if len(errs) > 0 {
		return nil, errors.Join(errs...)
	}

	body, err = os.ReadFile(bodyPath)
	if err != nil {
		return nil, fmt.Errorf("capture: read back body: %w", err)
	}
	// A non-nil slice makes the custom FulfillRequestArgs marshaler
	// emit "body": "", even for an empty response.
	if body == nil {
		body = []byte{}
	}

	sum := hex.EncodeToString(hash.Sum(nil))
	if err := col.writeMeta(event, tmp, written, sum); err != nil {
		return nil, err
	}
	return body, nil
}

// writeMeta writes and closes tmp/meta.json describing the sample.
func (col *collector) writeMeta(event *fetch.RequestPausedReply, tmp string, size int64, sum string) error {
	m := meta{
		RequestID: string(event.RequestID),
		URL:       event.Request.URL,
		Status:    *event.ResponseStatusCode,
		Headers:   event.ResponseHeaders,
		Size:      size,
		SHA256:    sum,
	}
	data, err := json.MarshalIndent(m, "", "  ")
	if err != nil {
		return fmt.Errorf("capture: encode meta: %w", err)
	}
	if err := os.WriteFile(filepath.Join(tmp, "meta.json"), data, 0o600); err != nil {
		return fmt.Errorf("capture: write meta: %w", err)
	}
	return nil
}

// fulfillArgs builds the fulfillment request. The received event is
// not modified: the headers are copied, Content-Length and
// Transfer-Encoding are dropped (case-insensitively) and a single
// Content-Length matching the actual byte count is appended.
func (col *collector) fulfillArgs(event *fetch.RequestPausedReply, body []byte) *fetch.FulfillRequestArgs {
	headers := make([]fetch.HeaderEntry, 0, len(event.ResponseHeaders)+1)
	for _, h := range event.ResponseHeaders {
		if strings.EqualFold(h.Name, "Content-Length") ||
			strings.EqualFold(h.Name, "Transfer-Encoding") {
			continue
		}
		headers = append(headers, h)
	}
	headers = append(headers, fetch.HeaderEntry{
		Name:  "Content-Length",
		Value: fmt.Sprintf("%d", len(body)),
	})

	args := fetch.NewFulfillRequestArgs(event.RequestID, *event.ResponseStatusCode)
	args.ResponseHeaders = headers
	// A non-nil slice, including an empty one, is encoded by the
	// generated client's []byte field as base64.
	args.Body = body
	return args
}

// sampleDirName returns the sample directory name: the lowercase
// hexadecimal SHA-256 of the request id.
func sampleDirName(id fetch.RequestID) string {
	sum := sha256.Sum256([]byte(id))
	return hex.EncodeToString(sum[:])
}
