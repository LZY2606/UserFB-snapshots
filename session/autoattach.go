package session

import (
	"context"
	"sync"

	"github.com/mafredri/cdp"
	cerrors "github.com/mafredri/cdp/internal/errors"
	"github.com/mafredri/cdp/protocol/target"
	"github.com/mafredri/cdp/rpcc"
)

// Auto attach phases, used in AutoAttachError.
const (
	// autoAttachPhaseInit is reported when the Init callback fails.
	autoAttachPhaseInit = "init"
	// autoAttachPhaseResume is reported when
	// Runtime.runIfWaitingForDebugger fails.
	autoAttachPhaseResume = "resume"
)

// Auto attach lifecycle states.
const (
	autoEnabling = iota // setAutoAttach has not been confirmed yet
	autoActive          // confirmed, targets are taken over
	autoAborted         // enabling failed, targets are detached
)

// ErrAutoAttachActive is returned by Manager.AutoAttach when auto-attach
// was already enabled (or enabling) for the Manager. It can be detected
// with errors.Is.
var ErrAutoAttachActive = cerrors.New("session.Manager: auto-attach is already active")

// AutoAttachOptions configures Manager.AutoAttach.
//
// Init is invoked for every auto-attached target, after its session
// connection has been registered with the Manager. The provided
// *cdp.Client is connected to the target described by target.Info. If
// Init returns an error, the target is detached without resuming its
// scripts and an *AutoAttachError with Phase "init" is published on
// Manager.Err.
//
// Ready is invoked after a target has been fully taken over: Init
// succeeded and, when the target was waiting for the debugger, the
// Runtime.runIfWaitingForDebugger resume command succeeded. The
// *rpcc.Conn is the session connection of the target.
//
// Both callbacks are optional. They run on their own goroutine and do
// not block the Manager event loop. The context passed to Init is
// canceled when the target is detached or the Manager is closed.
type AutoAttachOptions struct {
	Init  func(context.Context, target.Info, *cdp.Client) error
	Ready func(target.Info, *rpcc.Conn)
}

// AutoAttachError is published on Manager.Err when taking over an
// auto-attached target fails.
type AutoAttachError struct {
	// SessionID identifies the failed target session.
	SessionID target.SessionID
	// TargetID identifies the failed target.
	TargetID target.ID
	// Phase is either "init" (the Init callback failed) or "resume"
	// (Runtime.runIfWaitingForDebugger failed).
	Phase string
	// Err is the underlying error, e.g. the error returned by Init or
	// an *rpcc.ResponseError from the browser.
	Err error
}

// Error implements error.
func (e *AutoAttachError) Error() string {
	return "session.Manager: auto-attach target " + string(e.TargetID) +
		" (session " + string(e.SessionID) + ") failed during " + e.Phase +
		": " + e.Err.Error()
}

// Unwrap returns the underlying error so that it can be recovered with
// errors.Is and errors.As.
func (e *AutoAttachError) Unwrap() error { return e.Err }

// autoAttach coordinates the takeover of auto-attached targets. It is
// installed on the Manager before Target.setAutoAttach is sent and the
// Manager watch loop feeds it the attachedToTarget /
// detachedFromTarget events.
type autoAttach struct {
	m    *Manager
	opts AutoAttachOptions

	mu               sync.Mutex
	state            int
	pending          []*target.AttachedToTargetReply // Attached before confirmation.
	seen             map[target.SessionID]bool       // Deduplicates repeated attach events.
	detachedSessions map[target.SessionID]bool       // Sessions detached by the browser.
}

// AutoAttach enables Target.setAutoAttach with
// waitForDebuggerOnStart on the parent connection and takes over the
// auto-attached targets.
//
// Attached targets are exposed through the same session machinery as
// Manager.Dial: a lightweight *rpcc.Conn tunneled over
// Target.sendMessageToTarget / Target.receivedMessageFromTarget. Init
// runs against the target before its scripts are resumed (when the
// target reported waitingForDebugger), then Ready hands over the
// session connection.
//
// AutoAttach only waits for the Target.setAutoAttach command result;
// target initialization happens in the background. AutoAttach can only
// be attempted once per Manager; repeated calls return
// ErrAutoAttachActive.
//
// Auto-attach is enabled on the parent connection only, it is not
// re-enabled recursively on auto-attached targets. Manager.Dial keeps
// working independently.
func (m *Manager) AutoAttach(ctx context.Context, opts AutoAttachOptions) error {
	m.autoBeginMu.Lock()
	if m.autoBegun {
		m.autoBeginMu.Unlock()
		return ErrAutoAttachActive
	}
	m.autoBegun = true
	m.autoBeginMu.Unlock()

	h := &autoAttach{
		m:                m,
		opts:             opts,
		state:            autoEnabling,
		seen:             make(map[target.SessionID]bool),
		detachedSessions: make(map[target.SessionID]bool),
	}
	// Publish before sending the command so that attachedToTarget
	// events arriving ahead of the reply are captured.
	m.auto.Store(h)

	err := m.c.Target.SetAutoAttach(ctx,
		target.NewSetAutoAttachArgs(true, true).SetFlatten(false))
	if err != nil {
		h.abort()
		return err
	}

	h.activate()
	return nil
}

// attached is called by the Manager watch loop for every
// attachedToTarget event. It never blocks event dispatch.
func (h *autoAttach) attached(ev *target.AttachedToTargetReply) {
	h.mu.Lock()
	if h.seen[ev.SessionID] {
		// The same session is initialized and made Ready at most once.
		h.mu.Unlock()
		return
	}
	h.seen[ev.SessionID] = true

	switch h.state {
	case autoEnabling:
		// Wait for the setAutoAttach confirmation before Init.
		h.pending = append(h.pending, ev)
		h.mu.Unlock()
	case autoActive:
		h.mu.Unlock()
		go h.init(ev)
	case autoAborted:
		h.mu.Unlock()
		go h.detach(ev.SessionID)
	}
}

// detached records a browser-side detach. The watch loop closes the
// session connection of registered targets itself; this only allows
// pre-confirmation targets to be skipped during activation.
func (h *autoAttach) detached(sessionID target.SessionID) {
	h.mu.Lock()
	h.detachedSessions[sessionID] = true
	h.mu.Unlock()
}

// activate marks enabling as confirmed and starts initialization of the
// targets that attached before the command reply arrived.
func (h *autoAttach) activate() {
	h.mu.Lock()
	pending := h.pending
	h.pending = nil
	h.state = autoActive
	h.mu.Unlock()

	for _, ev := range pending {
		h.mu.Lock()
		gone := h.detachedSessions[ev.SessionID]
		h.mu.Unlock()
		if gone {
			// Attached and detached again before confirmation:
			// never initialize this target.
			continue
		}
		go h.init(ev)
	}
}

// abort marks enabling as failed and detaches every target that
// attached before (or arrives after) the failure. Init and Ready are
// never invoked, no resume command is sent and enabling is not retried.
func (h *autoAttach) abort() {
	h.mu.Lock()
	h.state = autoAborted
	pending := h.pending
	h.pending = nil
	h.mu.Unlock()

	for _, ev := range pending {
		h.mu.Lock()
		gone := h.detachedSessions[ev.SessionID]
		h.mu.Unlock()
		if !gone {
			go h.detach(ev.SessionID)
		}
	}
}

// init takes over a single auto-attached target. The session
// connection is registered with the Manager message dispatcher before
// Init runs, so replies from the target keep flowing while Init is in
// progress.
func (h *autoAttach) init(ev *target.AttachedToTargetReply) {
	m := h.m

	s, err := newSession(m.ctx, ev.SessionID, ev.TargetInfo.TargetID, m.c, defaultDetachTimeout)
	if err != nil {
		// The Manager is shutting down; nothing was handed out.
		return
	}

	// Register the child connection before Init so the target client
	// used by Init is fully wired to the target.
	select {
	case m.sC <- s:
	case <-m.ctx.Done():
		_ = s.Close()
		return
	}

	// A detach event that arrived before registration only updated the
	// detached set, the watch loop could not close the session itself.
	// Catch it here; any later detach closes the registered session.
	h.mu.Lock()
	gone := h.detachedSessions[ev.SessionID]
	h.mu.Unlock()
	if gone {
		_ = s.Close()
		return
	}

	ctx, cancel := context.WithCancel(m.ctx)
	defer cancel()
	go func() {
		select {
		case <-s.Conn().Context().Done():
			// A detachedFromTarget event closes the session; the
			// initialization context must follow.
			cancel()
		case <-ctx.Done():
		}
	}()

	client := cdp.NewClient(s.Conn())

	if h.opts.Init != nil {
		err = h.opts.Init(ctx, ev.TargetInfo, client)
		if err != nil {
			if h.takeoverEnded(ev) {
				// Detached (or Manager closed) during initialization:
				// no error, no resume, no Ready. The detached flag is
				// set before the session closes, so this check does
				// not depend on goroutine scheduling.
				return
			}
			h.fail(ev, s, autoAttachPhaseInit, err)
			return
		}
		if h.takeoverEnded(ev) {
			// Detached while Init was running.
			return
		}
	}

	if ev.WaitingForDebugger {
		err = client.Runtime.RunIfWaitingForDebugger(ctx)
		if err != nil {
			if h.takeoverEnded(ev) {
				// The detach arrived while the resume command was in
				// flight; a late reply must not trigger Ready.
				return
			}
			h.fail(ev, s, autoAttachPhaseResume, err)
			return
		}
	}

	if h.opts.Ready != nil {
		h.opts.Ready(ev.TargetInfo, s.Conn())
	}
}

// takeoverEnded reports whether the target was detached (or the
// Manager is closing) before the takeover completed. The detached flag
// is recorded by the watch loop before the session connection is
// closed, so the result does not depend on when the Init context gets
// canceled.
func (h *autoAttach) takeoverEnded(ev *target.AttachedToTargetReply) bool {
	h.mu.Lock()
	defer h.mu.Unlock()
	return h.detachedSessions[ev.SessionID] || h.m.ctx.Err() != nil
}

// fail publishes the takeover error on Manager.Err and detaches the
// target. A cleanup failure is preserved but does not affect any other
// target.
func (h *autoAttach) fail(ev *target.AttachedToTargetReply, s *session, phase string, err error) {
	h.m.publishAutoErr(&AutoAttachError{
		SessionID: ev.SessionID,
		TargetID:  ev.TargetInfo.TargetID,
		Phase:     phase,
		Err:       err,
	})

	if detachErr := s.Close(); detachErr != nil {
		h.m.publishAutoErr(cerrors.Wrapf(detachErr,
			"session.Manager: auto-attach cleanup failed for session %s", ev.SessionID))
	}
}

// detach asks the browser to detach an auto-attached target for which
// no session connection was created (enabling was not confirmed).
func (h *autoAttach) detach(sessionID target.SessionID) {
	ctx, cancel := context.WithTimeout(context.Background(), defaultDetachTimeout)
	defer cancel()
	_ = h.m.c.Target.DetachFromTarget(ctx,
		target.NewDetachFromTargetArgs().SetSessionID(sessionID))
}

// publishAutoErr delivers an error on Manager.Err without blocking the
// takeover of other targets. The sink drops the error if the Manager is
// shutting down, so this never races the close of the error channel.
func (m *Manager) publishAutoErr(err error) {
	m.errSink.send(err)
}
