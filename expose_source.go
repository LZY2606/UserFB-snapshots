package chromedp

import (
	"context"
	"errors"
	"sync"

	"github.com/chromedp/cdproto/cdp"
	"github.com/chromedp/cdproto/runtime"
)

// CallSource names the document that made a call to an
// [ExposeSourceFunc]. The target builds it from the events of the browser,
// so a page script cannot forge it.
type CallSource struct {
	// FrameID is the frame of the document that made the call.
	FrameID cdp.FrameID

	// ExecutionContextID is the default JavaScript execution context of
	// that document.
	ExecutionContextID runtime.ExecutionContextID

	// URL is the address of the document when the browser created it. A
	// redirect gives the final address. A later change of the hash part
	// does not change a source that the target already delivered.
	URL string
}

// The causes that end the context of a call of an ExposeSourceFunc.
var (
	errSourceContextDestroyed = errors.New("the execution context of the call is destroyed")
	errSourceFrameDetached    = errors.New("the frame of the call is removed from the target")
	errSourceTargetEnded      = errors.New("the target of the call ended")
)

// sourceTracker keeps the contexts of the calls of every ExposeSourceFunc on
// one target. A browser event can end a context when the execution context or
// the frame of its call goes.
type sourceTracker struct {
	// root is the parent of each context of a call. It keeps the values
	// of the registration context, and it has no deadline.
	root context.Context

	mu      sync.Mutex
	byFrame map[cdp.FrameID]map[*sourceCall]struct{}
}

// sourceCall is one in-flight call.
type sourceCall struct {
	frameID cdp.FrameID
	cancel  context.CancelCauseFunc
}

// newSourceTracker makes a tracker. parent only gives values. The tracker
// does not inherit the deadline or the cancellation of parent.
func newSourceTracker(parent context.Context) *sourceTracker {
	return &sourceTracker{
		root:    context.WithoutCancel(parent),
		byFrame: make(map[cdp.FrameID]map[*sourceCall]struct{}),
	}
}

// start makes the context of one call and records the call by its frame.
func (s *sourceTracker) start(frameID cdp.FrameID) (context.Context, *sourceCall) {
	callCtx, cancel := context.WithCancelCause(s.root)
	call := &sourceCall{frameID: frameID, cancel: cancel}
	s.mu.Lock()
	if s.byFrame[frameID] == nil {
		s.byFrame[frameID] = make(map[*sourceCall]struct{})
	}
	s.byFrame[frameID][call] = struct{}{}
	s.mu.Unlock()
	return callCtx, call
}

// end removes a call and ends its context.
func (s *sourceTracker) end(call *sourceCall) {
	call.cancel(context.Canceled)
	s.mu.Lock()
	calls := s.byFrame[call.frameID]
	delete(calls, call)
	if len(calls) == 0 {
		delete(s.byFrame, call.frameID)
	}
	s.mu.Unlock()
}

// endFrame ends every call of a frame.
func (s *sourceTracker) endFrame(frameID cdp.FrameID, cause error) {
	s.mu.Lock()
	calls := s.byFrame[frameID]
	delete(s.byFrame, frameID)
	s.mu.Unlock()
	for call := range calls {
		call.cancel(cause)
	}
}

// endAll ends every call on the target.
func (s *sourceTracker) endAll(cause error) {
	s.mu.Lock()
	byFrame := s.byFrame
	s.byFrame = make(map[cdp.FrameID]map[*sourceCall]struct{})
	s.mu.Unlock()
	for _, calls := range byFrame {
		for call := range calls {
			call.cancel(cause)
		}
	}
}

// sourceTrackerFor returns the tracker of the target, and makes it on the
// first ExposeSourceFunc.
func (t *Target) sourceTrackerFor(ctx context.Context) *sourceTracker {
	t.sourceMu.Lock()
	defer t.sourceMu.Unlock()
	if t.sourceCalls == nil {
		t.sourceCalls = newSourceTracker(ctx)
	}
	return t.sourceCalls
}

// addSourceBinding registers dispatch for the BindingCalled events of one
// ExposeSourceFunc. The loop that reads the messages calls dispatch, after it
// handled every event that arrived earlier.
func (t *Target) addSourceBinding(binding string, dispatch func(*runtime.EventBindingCalled)) {
	t.sourceMu.Lock()
	if t.sourceBindings == nil {
		t.sourceBindings = make(map[string]func(*runtime.EventBindingCalled))
	}
	t.sourceBindings[binding] = dispatch
	t.sourceMu.Unlock()
}

// endSourceContextsLocked ends every call of a frame. The caller holds
// frameMu.
func (t *Target) endSourceContextsLocked(frameID cdp.FrameID, cause error) {
	t.sourceMu.Lock()
	tracker := t.sourceCalls
	t.sourceMu.Unlock()
	if tracker != nil {
		tracker.endFrame(frameID, cause)
	}
}

// endAllSourceContexts ends every call when the target ends.
func (t *Target) endAllSourceContexts() {
	t.sourceMu.Lock()
	tracker := t.sourceCalls
	t.sourceMu.Unlock()
	if tracker != nil {
		tracker.endAll(errSourceTargetEnded)
	}
}

// callSource builds the source of a call from the frame records and the
// default execution contexts of the target. It returns false for a context
// that the records do not know, so the caller never treats it as a source of
// the top frame.
func (t *Target) callSource(ev *runtime.EventBindingCalled) (CallSource, string, bool) {
	t.frameMu.RLock()
	frameID, ok := t.frameContexts[ev.ExecutionContextID]
	if !ok {
		t.frameMu.RUnlock()
		return CallSource{}, "", false
	}
	frame := t.frames[frameID]
	uniqueID := t.execUniqueIDs[frameID]
	t.frameMu.RUnlock()
	if frame == nil {
		return CallSource{}, "", false
	}
	frame.RLock()
	url := frame.URL
	frame.RUnlock()
	return CallSource{
		FrameID:            frameID,
		ExecutionContextID: ev.ExecutionContextID,
		URL:                url,
	}, uniqueID, true
}

// contextLives reports whether the document of a call still owns the same
// default execution context as when the call started. A reused context id
// fails the check, so a late result cannot reach a new document.
func (t *Target) contextLives(frameID cdp.FrameID, uniqueID string) bool {
	t.frameMu.RLock()
	defer t.frameMu.RUnlock()
	return t.execUniqueIDs[frameID] == uniqueID
}
