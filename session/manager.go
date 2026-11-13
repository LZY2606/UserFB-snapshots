package session

import (
	"context"
	"sync"
	"sync/atomic"
	"time"

	"github.com/mafredri/cdp"
	"github.com/mafredri/cdp/internal/errors"
	"github.com/mafredri/cdp/protocol/target"
	"github.com/mafredri/cdp/rpcc"
)

// Manager establishes session connections to targets.
type Manager struct {
	ctx    context.Context
	cancel context.CancelFunc

	c    *cdp.Client
	sC   chan *session
	done chan error
	errC chan error
	// errSink funnels auto-attach errors from takeover goroutines
	// into errC. It serializes shutdown so a publisher never writes to
	// errC after the watch loop has closed it.
	errSink *errSink

	// autoBeginMu guards the at-most-once lifecycle of AutoAttach. The
	// attempt counts as started while the setAutoAttach command is in
	// flight, so concurrent and repeated calls fail with
	// ErrAutoAttachActive.
	autoBeginMu sync.Mutex
	autoBegun   bool
	// auto holds the auto-attach coordinator once AutoAttach has been
	// called. It is installed before the command is sent so that
	// attachedToTarget events arriving ahead of the reply are captured.
	auto atomic.Pointer[autoAttach]

	// dialedMu guards dialed, the sessions established through
	// Manager.Dial. Their attachedToTarget events must be ignored by
	// auto-attach so that a manually dialed target is not taken over
	// twice.
	dialedMu sync.Mutex
	dialed   map[target.SessionID]struct{}
}

// markDialed records a session created by Manager.Dial.
func (m *Manager) markDialed(id target.SessionID) {
	m.dialedMu.Lock()
	if m.dialed == nil {
		m.dialed = make(map[target.SessionID]struct{})
	}
	m.dialed[id] = struct{}{}
	m.dialedMu.Unlock()
}

// isDialed reports whether id belongs to a Manager.Dial session.
func (m *Manager) isDialed(id target.SessionID) bool {
	m.dialedMu.Lock()
	defer m.dialedMu.Unlock()
	_, ok := m.dialed[id]
	return ok
}

// unmarkDialed removes a Manager.Dial session record.
func (m *Manager) unmarkDialed(id target.SessionID) {
	m.dialedMu.Lock()
	delete(m.dialed, id)
	m.dialedMu.Unlock()
}

const (
	// This timeout is used for invoking DetachFromTarget when a
	// session connection is closed. Meaning we will wait until
	// timeout for a confirmation response. If a response is not
	// received in this time, it's still possible that the session
	// will eventually close.
	//
	// The default timeout is a compromise between not blocking for
	// extended periods of time and allowing slow connections to
	// deliver the message.
	//
	// TODO(mafredri): Should we allow configuring the timeout?
	defaultDetachTimeout = 5 * time.Second
)

// Dial establishes a target session and creates a lightweight rpcc.Conn
// that uses SendMessageToTarget and ReceivedMessageFromTarget from the
// Target domain instead of a new websocket connection.
//
// Dial will invoke AttachToTarget. Close (rpcc.Conn) will invoke
// DetachFromTarget.
func (m *Manager) Dial(ctx context.Context, id target.ID) (*rpcc.Conn, error) {
	s, err := dial(ctx, id, m.c, defaultDetachTimeout)
	if err != nil {
		return nil, err
	}
	// The browser emits attachedToTarget for manual attaches as well;
	// record the session so auto-attach does not take it over again.
	m.markDialed(s.ID)
	select {
	case m.sC <- s:
	case <-m.ctx.Done():
		s.Close()
		return nil, errors.New("session.Manager: Dial failed: Manager is closed")
	}
	return s.Conn(), nil
}

// Close closes the Manager and all active sessions. All rpcc.Conn
// created by Dial will be closed.
func (m *Manager) Close() error {
	m.cancel()
	if m.done != nil {
		return errors.Wrapf(<-m.done, "session.Manager: close failed")
	}
	return nil
}

// errSink delivers background errors (from auto-attach takeover
// goroutines) onto the Manager error channel without ever blocking and
// without racing the channel close performed by the watch loop.
type errSink struct {
	mu   sync.Mutex
	done bool
	ch   chan<- error
}

func newErrSink(ch chan<- error) *errSink { return &errSink{ch: ch} }

// send delivers an error unless the sink has been shut down.
func (s *errSink) send(err error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.done {
		return
	}
	select {
	case s.ch <- err:
	default:
	}
}

// shutdown stops accepting errors. It must be called (from the watch
// loop) before errC is closed.
func (s *errSink) shutdown() {
	s.mu.Lock()
	s.done = true
	s.mu.Unlock()
}

func (m *Manager) watch(ev *sessionEvents, created <-chan *session, done, errC chan<- error) {
	defer ev.Close()

	isClosing := func(err error) bool {
		// Test if this is an rpcc.closeError.
		var e interface{ Closed() bool }
		if ok := errors.As(err, &e); ok && e.Closed() {
			// Cleanup, the underlying connection was closed
			// before the Manager and its context does not
			// inherit from rpcc.Conn.
			m.cancel()
			return true
		}

		if errors.Is(err, context.Canceled) {
			// Manager was closed.
			return true
		}
		return false
	}

	sessions := make(map[target.SessionID]*session)
	defer func() {
		var err []error
		for _, ss := range sessions {
			// TODO(mafredri): Speed up by closing sessions concurrently.
			err = append(err, ss.Close())
		}
		if m.errSink != nil {
			// Stop background error publishers before closing errC.
			m.errSink.shutdown()
		}
		done <- errors.Merge(err...)
		close(done)
		close(errC)
	}()

	for {
		select {
		case s := <-created:
			sessions[s.ID] = s

		// Checking detached should be sufficient for monitoring the
		// session. A DetachedFromTarget event is always sent before
		// TargetDestroyed.
		case <-ev.detached.Ready():
			ev, err := ev.detached.Recv()
			if err != nil {
				if isClosing(err) {
					return
				}
				err = errors.Wrapf(err, "Manager.watch: error receiving detached event")
				sendOrDiscardErr(errC, err)
				continue
			}

			if h := m.auto.Load(); h != nil {
				h.detached(ev.SessionID)
			}
			m.unmarkDialed(ev.SessionID)

			if s, ok := sessions[ev.SessionID]; ok {
				delete(sessions, s.ID)
				s.Close()
			}

		case <-ev.attached.Ready():
			attachedEvent, err := ev.attached.Recv()
			if err != nil {
				if isClosing(err) {
					return
				}
				err = errors.Wrapf(err, "Manager.watch: error receiving attached event")
				sendOrDiscardErr(errC, err)
				continue
			}

			if m.isDialed(attachedEvent.SessionID) {
				// This session was established through Manager.Dial,
				// which registered it directly. Auto-attach must not
				// take it over a second time.
				continue
			}

			// The attached stream is synchronized with the other
			// session event streams, so events keep their arrival
			// order relative to messages and detaches.
			if h := m.auto.Load(); h != nil {
				h.attached(attachedEvent)
			}

		case <-ev.message.Ready():
			ev, err := ev.message.Recv()
			if err != nil {
				if isClosing(err) {
					return
				}
				err = errors.Wrapf(err, "Manager.watch: error receiving message event")
				sendOrDiscardErr(errC, err)
				continue
			}

			if s, ok := sessions[ev.SessionID]; ok {
				// We rely on the implementation of *rpcc.Conn
				// to read this message in a reasonably short
				// amount of time. Blocking here can potentially
				// delay other session messages but that should
				// not happen.
				err = s.Write([]byte(ev.Message))
				if err != nil {
					delete(sessions, s.ID)
				}
			}
		}
	}
}

func sendOrDiscardErr(errC chan<- error, err error) {
	select {
	case errC <- err:
	default:
	}
}

// Err is a channel that blocks until the Manager encounters an error.
// The channel is closed if Manager is closed.
//
// Errors could happen if the debug target sends events that cannot be
// decoded from JSON.
func (m *Manager) Err() <-chan error {
	return m.errC
}

// NewManager creates a new session Manager.
//
// The cdp.Client will be used to listen to events and invoke commands
// on the Target domain. It will also be used by all rpcc.Conn created
// by Dial.
func NewManager(c *cdp.Client) (*Manager, error) {
	m := &Manager{
		c:    c,
		sC:   make(chan *session),
		errC: make(chan error, 1),
	}
	m.errSink = newErrSink(m.errC)

	// TODO(mafredri): Inherit the context from rpcc.Conn in cdp.Client.
	// cdp.Client does not yet expose the context, nor rpcc.Conn.
	m.ctx, m.cancel = context.WithCancel(context.TODO())

	ev, err := newSessionEvents(m.ctx, c)
	if err != nil {
		close(m.errC)
		m.Close()
		return nil, err
	}

	m.done = make(chan error, 1)
	go m.watch(ev, m.sC, m.done, m.errC)
	return m, nil
}

type sessionEvents struct {
	attached target.AttachedToTargetClient
	detached target.DetachedFromTargetClient
	message  target.ReceivedMessageFromTargetClient
}

func newSessionEvents(ctx context.Context, c *cdp.Client) (events *sessionEvents, err error) {
	ev := new(sessionEvents)
	defer func() {
		if err != nil {
			ev.Close()
		}
	}()

	// The attachedToTarget stream is created up-front (and synchronized
	// with the other streams) even though it only carries events once
	// AutoAttach has been enabled. Creating it on demand would force a
	// second Sync group, breaking event ordering across the streams.
	ev.attached, err = c.Target.AttachedToTarget(ctx)
	if err != nil {
		return nil, err
	}
	ev.detached, err = c.Target.DetachedFromTarget(ctx)
	if err != nil {
		return nil, err
	}
	ev.message, err = c.Target.ReceivedMessageFromTarget(ctx)
	if err != nil {
		return nil, err
	}

	err = cdp.Sync(ev.attached, ev.detached, ev.message)
	if err != nil {
		return nil, err
	}

	return ev, nil
}

func (ev *sessionEvents) Close() (err error) {
	for _, c := range []interface {
		Close() error
	}{
		ev.attached,
		ev.detached,
		ev.message,
	} {
		if c != nil {
			e := c.Close()
			if err == nil {
				err = e
			}
		}
	}
	return err
}
