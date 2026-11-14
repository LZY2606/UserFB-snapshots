package grab

import (
	"bytes"
	"context"
	"errors"
	"io"
	"os"
	"sync"
)

// streamBuffer is the NoStore destination. Unlike bytes.Buffer it is safe for
// the transfer goroutine to append while OpenStream views snapshot its
// contents. It remains the single in-memory copy of the body: views copy out
// of it during Read rather than retaining a private snapshot.
type streamBuffer struct {
	mu  sync.Mutex
	buf bytes.Buffer
}

func (b *streamBuffer) Write(p []byte) (int, error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.buf.Write(p)
}

// Bytes returns a snapshot of the stored prefix. It is only used by the
// completion-only accessors after the transfer has finished; live views use
// ReadAt so the copy happens under the buffer lock.
func (b *streamBuffer) Bytes() []byte {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.buf.Bytes()
}

// ReadAt copies bytes out of the stored prefix under the buffer lock, so an
// in-progress append can never race a view. It never retains the underlying
// slice: the body stays the single in-memory copy.
func (b *streamBuffer) ReadAt(p []byte, off int64) (int, error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	if off >= int64(b.buf.Len()) {
		return 0, io.EOF
	}
	return copy(p, b.buf.Bytes()[off:]), nil
}

// streamWriter sits between the transfer loop and the real destination. It
// publishes only the bytes a Write actually delivered, which keeps visibility
// honest for short writes and for reads that surface data and an error at the
// same time. Truncate and Close are delegated so the existing file lifecycle
// and overwrite handling are unchanged.
type streamWriter struct {
	dst    io.Writer
	stream *streamState
}

func (w *streamWriter) Write(p []byte) (int, error) {
	n, err := w.dst.Write(p)
	// Publish after the write returned: the visible prefix never runs ahead of
	// the bytes storage actually accepted.
	w.stream.publish(int64(n))
	return n, err
}

// Truncate preserves the truncater behavior Client.copyFile relies on when an
// existing destination is overwritten rather than resumed.
func (w *streamWriter) Truncate(size int64) error {
	t, ok := w.dst.(interface{ Truncate(int64) error })
	if !ok {
		return os.ErrInvalid
	}
	return t.Truncate(size)
}

// Close delegates to the destination file handle. Views opened through
// OpenStream hold their own read handles, so closing the writer does not
// invalidate them.
func (w *streamWriter) Close() error {
	if closer, ok := w.dst.(io.Closer); ok {
		return closer.Close()
	}
	return nil
}

// streamState is the growth/terminal bookkeeping shared by a Response and all
// of the sequential views opened against it. A single state backs every view,
// so the transfer only has to notify one condition variable when new bytes
// become available rather than waking readers individually.
type streamState struct {
	mu sync.Mutex
	// wake is broadcast whenever the visible prefix grows, the transfer reaches
	// a terminal state, or a view needs to be unblocked after its context was
	// canceled. Readers wait on this instead of polling storage for growth.
	wake     sync.Cond
	visible  int64 // bytes that have been durably written to storage
	terminal bool  // the transfer has run to completion or failed
	err      error // terminal error, nil on success
	// ready is closed once the destination (file or memory buffer) exists and
	// views can open it. Client.Do returns before openWriter has run, so a view
	// opened immediately waits here rather than racing file creation.
	ready chan struct{}
}

func newStreamState() *streamState {
	s := &streamState{ready: make(chan struct{})}
	s.wake.L = &s.mu
	return s
}

// markReady closes ready exactly once. It is called once storage is open.
func (s *streamState) markReady() {
	select {
	case <-s.ready:
	default:
		close(s.ready)
	}
}

// waitReady blocks until storage can be opened or ctx is canceled.
func (s *streamState) waitReady(ctx context.Context) error {
	select {
	case <-s.ready:
		return nil
	case <-ctx.Done():
		return ctx.Err()
	}
}

// publish records that n additional bytes, starting at offset visible, have
// been written to storage. It must be called only after the corresponding
// write has succeeded: bytes are never advertised before they exist.
func (s *streamState) publish(n int64) {
	if n <= 0 {
		return
	}
	s.mu.Lock()
	s.visible += n
	s.wake.Broadcast()
	s.mu.Unlock()
}

// finish marks the transfer terminal. On success err must be nil; otherwise it
// is the original download error, which is handed to readers verbatim so that
// errors.Is keeps working. Any view blocked waiting for growth is unblocked.
func (s *streamState) finish(err error) {
	if s == nil {
		return
	}
	// A failure before openWriter never reaches markReady; release any view
	// waiting for storage so it can observe the terminal error.
	s.markReady()
	s.mu.Lock()
	if !s.terminal {
		s.terminal = true
		s.err = err
		s.wake.Broadcast()
	}
	s.mu.Unlock()
}

// waitFor blocks until at least offset bytes are visible, the transfer reaches
// a terminal state, or the given context is canceled. It returns the number of
// currently visible bytes and any context error.
func (s *streamState) waitFor(ctx context.Context, offset int64) (int64, error) {
	s.mu.Lock()
	defer s.mu.Unlock()

	if offset < s.visible || s.terminal {
		return s.visible, nil
	}

	// A context cancel must wake only this view's Read, so re-check the state
	// under the same lock as soon as the context fires.
	stop := context.AfterFunc(ctx, func() {
		s.mu.Lock()
		s.wake.Broadcast()
		s.mu.Unlock()
	})
	defer stop()

	for offset >= s.visible && !s.terminal {
		if err := ctx.Err(); err != nil {
			return s.visible, err
		}
		s.wake.Wait()
	}
	return s.visible, ctx.Err()
}

// stream is an independent sequential view over the bytes written for a
// Response. Each stream keeps its own read offset; multiple streams of the same
// Response never share one. Data is read straight from the underlying storage
// (the destination file or the in-memory buffer), so neither the whole body
// nor per-reader copies are retained by the stream itself.
type stream struct {
	state   *streamState
	ctx     context.Context
	cancel  context.CancelFunc
	read    func(p []byte, offset int64) (int, error)
	closeFn func() error

	mu     sync.Mutex
	offset int64
	closed bool
}

// Read returns bytes that have actually been written to storage, blocking when
// it reaches the current end of the visible prefix until more data arrives.
// On success it returns io.EOF only after the whole transfer - including the
// AfterCopy hook, checksum validation and response finalization - has
// completed; a failed transfer surfaces its original error instead of EOF once
// the visible prefix has been drained.
func (s *stream) Read(p []byte) (int, error) {
	if len(p) == 0 {
		return 0, nil
	}

	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		return 0, os.ErrClosed
	}
	offset := s.offset
	s.mu.Unlock()

	visible, waitErr := s.state.waitFor(s.ctx, offset)

	// Close and context cancelation both fire the view context; a closed view
	// reports os.ErrClosed even when the wake came from Close, while an open
	// view reports its context error.
	s.mu.Lock()
	closed := s.closed
	s.mu.Unlock()
	if closed {
		return 0, os.ErrClosed
	}

	var n int
	var readErr error
	if visible > offset {
		limit := visible - offset
		if int64(len(p)) > limit {
			p = p[:int(limit)]
		}
		n, readErr = s.read(p, offset)
		// Only bytes the storage read actually delivered become the new offset.
		// A read that returns data together with an error therefore never
		// advertises bytes it did not hand to the caller.
		if n > 0 {
			s.mu.Lock()
			s.offset += int64(n)
			offset = s.offset
			s.mu.Unlock()
		}
	}
	if waitErr != nil {
		// Context cancelation ends only this view; the download itself is
		// unaffected.
		return n, waitErr
	}
	if readErr != nil {
		return n, readErr
	}

	// Drain whatever is visible first; terminal status is only consulted once
	// the view has caught up, so a failure is never reported ahead of the bytes
	// that were already stored.
	s.state.mu.Lock()
	terminal, terminalErr := s.state.terminal, s.state.err
	caughtUp := offset >= s.state.visible
	s.state.mu.Unlock()

	if terminal && caughtUp {
		if terminalErr != nil {
			return n, terminalErr
		}
		if n == 0 {
			return 0, io.EOF
		}
	}
	return n, nil
}

// Close ends this view only. It never cancels the Response and never closes a
// sibling view. Repeating Close is safe.
func (s *stream) Close() error {
	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		return nil
	}
	s.closed = true
	s.mu.Unlock()

	s.cancel()
	var err error
	if s.closeFn != nil {
		err = s.closeFn()
	}
	return err
}

// openStreamView opens a new independent view over the Response storage. The
// resumed prefix is immediately visible: bytes present in a destination file
// before the transfer started count as stored data.
func (c *Response) openStreamView(ctx context.Context) (*stream, error) {
	if ctx == nil {
		return nil, errors.New("grab: cannot open stream with nil context")
	}

	// A transfer that has already failed reports the same terminal error. The
	// terminal flag is set before Done is closed, so consult it directly rather
	// than racing the channel close. A completed successful transfer still
	// allows fresh views over the full result.
	c.stream.mu.Lock()
	if c.stream.terminal && c.stream.err != nil {
		err := c.stream.err
		c.stream.mu.Unlock()
		return nil, err
	}
	c.stream.mu.Unlock()

	// Do returns before openWriter has created the destination, so wait for
	// storage without holding anything. A view's context cancelation aborts the
	// wait and leaves the download untouched.
	if err := c.stream.waitReady(ctx); err != nil {
		return nil, err
	}

	// The transfer may have failed while the view waited for storage; surface
	// that terminal error rather than attempting to open a missing destination.
	c.stream.mu.Lock()
	if c.stream.terminal && c.stream.err != nil {
		err := c.stream.err
		c.stream.mu.Unlock()
		return nil, err
	}
	c.stream.mu.Unlock()

	ctx, cancel := context.WithCancel(ctx)
	st := &stream{
		state:  c.stream,
		ctx:    ctx,
		cancel: cancel,
	}

	if c.Request.NoStore {
		st.read = c.storeBuffer.ReadAt
	} else {
		f, err := os.Open(c.Filename)
		if err != nil {
			cancel()
			return nil, err
		}
		st.read = f.ReadAt
		st.closeFn = f.Close
	}
	return st, nil
}
