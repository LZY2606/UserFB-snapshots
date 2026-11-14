package godet

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log"
)

// Lifecycle event names accepted by NavigateAndWait.
const (
	LifecycleDOMContentLoaded = "DOMContentLoaded"
	LifecycleLoad             = "load"
)

// maxBufferedLifecycleEvents bounds the number of lifecycle events recorded
// before their Page.navigate reply arrives. Overflow handling is out of
// scope, so the oldest entries are dropped once the buffer is full.
const maxBufferedLifecycleEvents = 1024

// sentinel errors so callers can use errors.Is.
var (
	// ErrNavigationCanceled is returned when the context passed to
	// NavigateAndWait is canceled (or expires) before the navigation completes.
	ErrNavigationCanceled = errors.New("navigation canceled")
	// ErrNavigationClosed is returned when RemoteDebugger.Close or a
	// websocket disconnect terminates a navigation in progress.
	ErrNavigationClosed = errors.New("debugger connection closed")
	// ErrInvalidLifecycleEvent is returned when the event argument of
	// NavigateAndWait is neither "DOMContentLoaded" nor "load". The check
	// happens before any command is sent.
	ErrInvalidLifecycleEvent = errors.New("invalid lifecycle event")
)

// NavigationResult identifies the document produced by a navigation.
// The values come from the Page.navigate reply and are confirmed by a
// Page.lifecycleEvent carrying the same frameId, loaderId and name.
type NavigationResult struct {
	FrameID      string // frameId reported by Page.navigate (and the lifecycle event)
	LoaderID     string // loaderId reported by Page.navigate (and the lifecycle event)
	SameDocument bool   // true for same-document navigations, which create no new loader
}

// navOutcome is the single result delivered to a waiting NavigateAndWait call.
type navOutcome struct {
	result NavigationResult
	err    error
}

// navCall tracks a single NavigateAndWait invocation. All fields are accessed
// with RemoteDebugger.Lock held, except ctx which is safe to read concurrently
// and the done channel which is only sent to non-blocking.
type navCall struct {
	id       int
	ctx      context.Context
	event    string
	done     chan navOutcome // buffered(1): the single terminal outcome
	removed  bool            // unregistered: the read loop/sender must discard anything for it
	replied  bool            // the Page.navigate reply has been processed
	frameID  string
	loaderID string
}

// navigationObserver records which lifecycle events have been observed for a
// specific (frameId, loaderId) pair. A lifecycle event only counts when both
// ids match the ones in the Page.navigate reply.
type navigationObserver struct {
	callID   int
	frameID  string
	loaderID string
	events   map[string]struct{}
}

// navigationWait is handed from NavigateAndWait to the sender goroutine, which
// owns writing the command exactly once.
type navigationWait struct {
	call    *navCall
	command Params
}

// NavigateAndWait sends Page.navigate and waits until the new document,
// identified by the frameId and loaderId in the Page.navigate reply, reaches
// the requested lifecycle stage (a Page.lifecycleEvent with the same frameId,
// loaderId and name).
//
// The caller is expected to have enabled Page events (PageEvents(true)); this
// method does not change domain configuration and does not switch tabs.
//
// event must be LifecycleDOMContentLoaded ("DOMContentLoaded") or
// LifecycleLoad ("load"); any other value is rejected before anything is sent
// over the websocket.
//
// A Page.navigate reply containing frameId but no loaderId denotes a
// same-document navigation: NavigationResult{SameDocument: true} is returned
// immediately, without waiting for new-document events. A reply containing
// errorText is returned as a NavigationError.
//
// The context covers the whole operation, including the wait for the
// Page.navigate command reply. An already-canceled context fails without
// sending the command. On cancellation the call's registrations are removed;
// the navigation itself is not undone, but its late reply and duplicate
// events are discarded by the read loop and can neither block it nor complete
// another call. Each call is associated with its own reply and loader, so
// canceling one never tears down another.
func (remote *RemoteDebugger) NavigateAndWait(ctx context.Context, url string, trans TransitionType, event string) (NavigationResult, error) {
	if event != LifecycleDOMContentLoaded && event != LifecycleLoad {
		return NavigationResult{}, fmt.Errorf("%w: %q (want %q or %q)",
			ErrInvalidLifecycleEvent, event, LifecycleDOMContentLoaded, LifecycleLoad)
	}

	// An already-canceled call never sends a command.
	if err := ctx.Err(); err != nil {
		return NavigationResult{}, navigationContextError(err)
	}

	remote.Lock()
	if remote.ws == nil {
		remote.Unlock()
		return NavigationResult{}, ErrorClose
	}

	call := &navCall{
		ctx:   ctx,
		event: event,
		done:  make(chan navOutcome, 1),
	}
	call.id = remote.reqID
	remote.reqID++
	remote.navCalls[call.id] = call
	remote.Unlock()

	params := Params{"url": url}
	if trans != NoTransition {
		params["transitionType"] = string(trans)
	}
	command := Params{
		"id":     call.id,
		"method": "Page.navigate",
		"params": params,
	}

	select {
	case remote.navRequests <- &navigationWait{call: call, command: command}:
	case <-ctx.Done():
		// Still queued, never written: make sure it cannot be sent later.
		remote.removeNavCall(call.id)
		return NavigationResult{}, navigationContextError(ctx.Err())
	case <-remote.closed:
		remote.removeNavCall(call.id)
		return NavigationResult{}, ErrNavigationClosed
	}

	select {
	case out := <-call.done:
		return out.result, out.err
	case <-ctx.Done():
		remote.removeNavCall(call.id)
		return NavigationResult{}, navigationContextError(ctx.Err())
	case <-remote.closed:
		remote.removeNavCall(call.id)
		return NavigationResult{}, ErrNavigationClosed
	}
}

// navigationContextError wraps the context error together with
// ErrNavigationCanceled so errors.Is recognizes both.
func navigationContextError(err error) error {
	return fmt.Errorf("%w: %w", ErrNavigationCanceled, err)
}

// removeNavCall deletes the pending-call registration (and its observer) so a
// late command reply or duplicate lifecycle events are discarded by the read
// loop. Safe to call after the call has already been removed.
func (remote *RemoteDebugger) removeNavCall(id int) {
	remote.Lock()
	defer remote.Unlock()

	call := remote.navCalls[id]
	if call == nil || call.removed {
		return
	}
	call.removed = true
	delete(remote.navCalls, id)
	if ob := remote.navObservers[call.loaderID]; ob != nil && ob.callID == id {
		delete(remote.navObservers, call.loaderID)
	}
}

// sendNavigation runs on the single sender goroutine. The command is written
// at most once; a call canceled while still queued is skipped, and a write
// failure terminates just that call.
func (remote *RemoteDebugger) sendNavigation(nav *navigationWait) {
	call := nav.call

	remote.Lock()
	skip := call.removed || call.ctx.Err() != nil
	ws := remote.ws
	remote.Unlock()

	if skip {
		remote.removeNavCall(call.id) // ensure the canceled registration is gone
		return
	}

	if ws == nil {
		remote.finishNavCall(call.id, navOutcome{err: ErrorClose})
		return
	}

	if err := ws.WriteJSON(nav.command); err != nil {
		log.Println("write navigation:", err)
		remote.finishNavCall(call.id, navOutcome{err: err})
	}
}

// navigationLifecycleEvent is the payload of Page.lifecycleEvent.
type navigationLifecycleEvent struct {
	Name     string `json:"name"`
	FrameID  string `json:"frameId"`
	LoaderID string `json:"loaderId"`
}

// dispatchLifecycleEvent feeds a Page.lifecycleEvent into the observer
// machinery. It is invoked synchronously from the read loop, independently of
// the user callback machinery, so a temporarily blocked user callback cannot
// stall navigation confirmation. Events for unknown loaders (they may arrive
// before the Page.navigate reply) are buffered until the reply identifies the
// loader.
func (remote *RemoteDebugger) dispatchLifecycleEvent(raw json.RawMessage) {
	var ev navigationLifecycleEvent
	if err := json.Unmarshal(raw, &ev); err != nil {
		return
	}
	if ev.Name != LifecycleDOMContentLoaded && ev.Name != LifecycleLoad {
		return
	}

	remote.Lock()
	defer remote.Unlock()

	ob := remote.navObservers[ev.LoaderID]
	if ob == nil {
		// The reply for this loader is not known yet. Only keep the event if
		// some navigation is genuinely waiting for its Page.navigate reply;
		// events with no pending navigation can never become relevant.
		waiting := false
		for _, c := range remote.navCalls {
			if !c.replied {
				waiting = true
				break
			}
		}
		if !waiting {
			return
		}
		if len(remote.navEvents) >= maxBufferedLifecycleEvents {
			remote.navEvents = remote.navEvents[1:]
		}
		remote.navEvents = append(remote.navEvents, ev)
		return
	}

	// Both ids must match: a sibling frame or an old loader cannot substitute.
	if ob.frameID != ev.FrameID {
		return
	}

	if _, ok := ob.events[ev.Name]; ok {
		return // duplicate event, keep the first observation
	}
	ob.events[ev.Name] = struct{}{}

	remote.tryCompleteObserverLocked(ob)
}

// deliverNavReply hands a Page.navigate reply to the owning call. Lifecycle
// events may have arrived first; they are matched here once the reply reveals
// frameId/loaderId. Success is never delivered before this point.
func (remote *RemoteDebugger) deliverNavReply(id int, raw json.RawMessage) {
	remote.Lock()
	defer remote.Unlock()

	call := remote.navCalls[id]
	if call == nil || call.removed {
		return // canceled call: its late reply is discarded
	}

	var reply struct {
		ErrorText string `json:"errorText"`
		FrameID   string `json:"frameId"`
		LoaderID  string `json:"loaderId"`
	}
	if err := json.Unmarshal(raw, &reply); err != nil {
		remote.finishNavCallLocked(call, navOutcome{err: err})
		return
	}

	call.replied = true

	if reply.ErrorText != "" {
		// A failed navigation never returns a success result.
		remote.finishNavCallLocked(call, navOutcome{err: NavigationError(reply.ErrorText)})
		return
	}

	result := NavigationResult{FrameID: reply.FrameID, LoaderID: reply.LoaderID}

	if reply.LoaderID == "" {
		// frameId without loaderId: same-document navigation, no new document
		// events to wait for.
		result.SameDocument = true
		remote.finishNavCallLocked(call, navOutcome{result: result})
		return
	}

	call.frameID = reply.FrameID
	call.loaderID = reply.LoaderID

	ob := remote.navObservers[reply.LoaderID]
	if ob == nil {
		ob = &navigationObserver{
			callID:   id,
			frameID:  reply.FrameID,
			loaderID: reply.LoaderID,
			events:   map[string]struct{}{},
		}
		remote.navObservers[reply.LoaderID] = ob
	}

	// Consume lifecycle events that arrived before the command reply.
	kept := remote.navEvents[:0]
	for _, ev := range remote.navEvents {
		if ev.LoaderID == reply.LoaderID && ev.FrameID == reply.FrameID {
			ob.events[ev.Name] = struct{}{}
		} else {
			kept = append(kept, ev)
		}
	}
	remote.navEvents = kept

	remote.tryCompleteObserverLocked(ob)
}

// tryCompleteObserverLocked completes the call associated with an observer once
// its requested lifecycle event has been observed. Only called with the lock
// held and only after the command reply was processed.
func (remote *RemoteDebugger) tryCompleteObserverLocked(ob *navigationObserver) {
	call := remote.navCalls[ob.callID]
	if call == nil || call.removed || !call.replied {
		return
	}
	if _, ok := ob.events[call.event]; !ok {
		return
	}

	remote.finishNavCallLocked(call, navOutcome{
		result: NavigationResult{FrameID: call.frameID, LoaderID: call.loaderID},
	})
}

// finishNavCallLocked delivers the terminal outcome and removes every
// registration of the call. The done channel is buffered, so a caller that
// already left on cancellation never blocks the read loop.
func (remote *RemoteDebugger) finishNavCallLocked(call *navCall, out navOutcome) {
	if call.removed {
		return
	}
	call.removed = true
	delete(remote.navCalls, call.id)
	if ob := remote.navObservers[call.loaderID]; ob != nil && ob.callID == call.id {
		delete(remote.navObservers, call.loaderID)
	}
	call.done <- out
}

// finishNavCall is the lock-taking wrapper used by the sender goroutine.
func (remote *RemoteDebugger) finishNavCall(id int, out navOutcome) {
	remote.Lock()
	defer remote.Unlock()

	if call := remote.navCalls[id]; call != nil && !call.removed {
		remote.finishNavCallLocked(call, out)
	}
}

// failAllNavCalls unblocks every pending navigation with the given error. It
// is invoked on Close and on websocket disconnect so no caller waits forever.
func (remote *RemoteDebugger) failAllNavCalls(err error) {
	remote.Lock()
	defer remote.Unlock()

	for _, call := range remote.navCalls {
		if call.removed {
			continue
		}
		remote.finishNavCallLocked(call, navOutcome{err: err})
	}
	remote.navObservers = map[string]*navigationObserver{}
	remote.navEvents = nil
}
