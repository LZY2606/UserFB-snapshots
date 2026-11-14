// Copyright 2013 Federico Sogaro. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package webdriver

import (
	"context"
	"encoding/json"
	"errors"
	"sync"
)

// Sentinel errors for the W3C input lease API.
var (
	// ErrInputBusy is returned by ResetInputs when a live lease owns the
	// session. ResetInputs never preempts a lease.
	ErrInputBusy = errors.New("webdriver: input lease is active")

	// ErrInputUncertain is returned to callers waiting for a lease when the
	// session has been quarantined: an input request was sent but its result
	// is unknown, so no new stateful input is admitted until ResetInputs
	// completes a remote release successfully.
	ErrInputUncertain = errors.New("webdriver: remote input state is uncertain, reset required")

	// ErrInputLeaseClosed is returned by Perform or Release on a lease that
	// has already been retired, or by AcquireInputs on a session generation
	// that has been deleted.
	ErrInputLeaseClosed = errors.New("webdriver: input lease is closed")
)

// errEmptyActions covers null and the empty array, both rejected locally.
var errEmptyActions = errors.New("webdriver: actions must be a non-empty JSON array")

// inputStateKey identifies one real browser session: the driver connection
// plus the server assigned session id. Different driver instances never share
// state even when session ids collide.
type inputStateKey struct {
	driver WebDriver
	id     string
}

// inputStates holds one *inputState per live session generation. It is a
// package level registry because Session is a value type: every view produced
// by NewSession or Sessions must resolve to the same latch without relying on
// pointer identity of the Session copy.
var inputStates sync.Map // inputStateKey -> *inputState

// inputState is the per-session generation latch. All fields are guarded by
// mu; cond broadcasts every state transition so queued callers can re-evaluate.
type inputState struct {
	mu sync.Mutex
	// cond broadcasts lease retirement, quarantine changes, reset completion
	// and session deletion.
	cond *sync.Cond

	// lease is the single live lease, or nil when the latch is open.
	lease *InputLease
	// quarantined is set when a request is sent without a known outcome and
	// cleared only by a successful ResetInputs remote release.
	quarantined bool
	// resetInFlight serializes ResetInputs calls and blocks new acquisitions
	// until the remote DELETE has fully completed.
	resetInFlight bool
	// dead marks a session generation whose DELETE /session/:id succeeded.
	dead bool
}

func newInputState() *inputState {
	st := &inputState{}
	st.cond = sync.NewCond(&st.mu)
	return st
}

// sharedInputState resolves the latch for a driver/session pair, creating the
// current generation on first use.
func sharedInputState(wd WebDriver, id string) *inputState {
	key := inputStateKey{driver: wd, id: id}
	actual, _ := inputStates.LoadOrStore(key, newInputState())
	return actual.(*inputState)
}

// bindInputState attaches the shared generation latch to a Session view.
// Session copies made afterwards keep the same pointer and therefore the same
// latch; a recreated session id resolves to a fresh generation.
func (s Session) bindInputState() Session {
	if s.input == nil {
		s.input = sharedInputState(s.wd, s.Id)
	}
	return s
}

// broadcastOnCancel wakes cond waiters when ctx is done. The returned stop
// function releases the watcher goroutine and must be called when the wait is
// over. A context that is already done simply triggers one broadcast.
func broadcastOnCancel(ctx context.Context, cond *sync.Cond) (stop func()) {
	done := make(chan struct{})
	go func() {
		select {
		case <-ctx.Done():
			cond.L.Lock()
			cond.Broadcast()
			cond.L.Unlock()
		case <-done:
		}
	}()
	return func() { close(done) }
}

// acquire waits for the open latch while honoring ctx. It sends no remote
// request. A canceled context, including one canceled before the call, can
// never take the latch. Waiters present when quarantine starts are woken with
// ErrInputUncertain once; retrying then blocks until a reset clears it.
func (st *inputState) acquire(ctx context.Context) (*InputLease, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	stop := broadcastOnCancel(ctx, st.cond)
	defer stop()

	st.mu.Lock()
	defer st.mu.Unlock()

	for {
		if err := ctx.Err(); err != nil {
			return nil, err
		}
		if st.dead {
			return nil, ErrInputLeaseClosed
		}
		// While quarantined every attempt is rejected with the sentinel so
		// callers stop input and drive recovery through ResetInputs.
		if st.quarantined {
			return nil, ErrInputUncertain
		}
		if st.lease == nil && !st.resetInFlight {
			lease := &InputLease{
				state: st,
				exec:  make(chan struct{}, 1),
			}
			// The single token serializes Perform and Release on this lease.
			lease.exec <- struct{}{}
			st.lease = lease
			return lease, nil
		}
		st.cond.Wait()
	}
}

// beginReset reserves the reset slot. Active leases are never preempted: the
// caller gets ErrInputBusy instead of waiting. Concurrent resets queue behind
// the running one without ever holding mu across the HTTP call.
func (st *inputState) beginReset(ctx context.Context) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	stop := broadcastOnCancel(ctx, st.cond)
	defer stop()

	st.mu.Lock()
	defer st.mu.Unlock()

	for {
		if err := ctx.Err(); err != nil {
			return err
		}
		if st.dead {
			return ErrInputLeaseClosed
		}
		if st.lease != nil {
			return ErrInputBusy
		}
		if !st.resetInFlight {
			st.resetInFlight = true
			return nil
		}
		st.cond.Wait()
	}
}

// endReset releases the reset slot, clearing quarantine only when the remote
// release succeeded.
func (st *inputState) endReset(ok bool) {
	st.mu.Lock()
	st.resetInFlight = false
	if ok {
		st.quarantined = false
	}
	st.cond.Broadcast()
	st.mu.Unlock()
}

// markDead retires the generation after a successful session delete. The old
// state pointer stays bound to old Session views, so they can never reach the
// fresh generation created for a recreated session id.
func (st *inputState) markDead() {
	st.mu.Lock()
	st.dead = true
	if st.lease != nil {
		st.lease.closed = true
		st.lease = nil
	}
	st.cond.Broadcast()
	st.mu.Unlock()
}

// retire closes the lease and opens the latch. On uncertain outcomes
// quarantine stays set and queued acquirers are woken with ErrInputUncertain.
func (st *inputState) retire(lease *InputLease, quarantine bool) {
	st.mu.Lock()
	lease.closed = true
	if st.lease == lease {
		st.lease = nil
	}
	if quarantine {
		st.quarantined = true
	}
	st.cond.Broadcast()
	st.mu.Unlock()
}

// InputLease is exclusive ownership of the W3C input state of one session.
// A lease is safe for concurrent use: Perform and Release run serially in the
// order they win the local execution token. The lease does not gate the legacy
// Click, SendKeys and similar entry points.
type InputLease struct {
	state *inputState
	sess  Session

	// exec is a one-token semaphore. Taking the token means the request has
	// been admitted: cancellation after that point cannot prevent the packet
	// from being sent.
	exec chan struct{}

	// closed is guarded by state.mu. A closed lease rejects every call without
	// sending another request.
	closed bool
}

// validateActions enforces the local contract only. Source internals are left
// to the remote endpoint. The lease remains usable when validation fails.
func validateActions(actions json.RawMessage) error {
	if len(actions) == 0 {
		return errEmptyActions
	}
	var sources []json.RawMessage
	if err := json.Unmarshal(actions, &sources); err != nil {
		return err
	}
	// Unmarshaling "null" leaves sources nil without an error.
	if len(sources) == 0 {
		return errEmptyActions
	}
	return nil
}

// acquireExec waits for the lease execution token, honoring ctx. Cancellation
// before the token is taken sends nothing and leaves the lease untouched.
func (l *InputLease) acquireExec(ctx context.Context) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	select {
	case <-l.exec:
		return nil
	case <-ctx.Done():
		return ctx.Err()
	}
}

func (l *InputLease) releaseExec() {
	l.exec <- struct{}{}
}

func (l *InputLease) isClosed() bool {
	l.state.mu.Lock()
	closed := l.closed
	l.state.mu.Unlock()
	return closed
}

// AcquireInputs waits for exclusive ownership of the session W3C input state.
// The call blocks while another lease is live or a reset is running and sends
// no remote command itself. A canceled context exits without taking the latch;
// an already canceled context cannot take a free latch. When the session has
// been quarantined while this call is queued it returns ErrInputUncertain.
func (s Session) AcquireInputs(ctx context.Context) (*InputLease, error) {
	if s.input == nil {
		s = s.bindInputState()
	}
	lease, err := s.input.acquire(ctx)
	if err != nil {
		return nil, err
	}
	lease.sess = s
	return lease, nil
}

// Perform posts one W3C actions request to POST /session/:id/actions. actions
// is the raw W3C sources array; it is wrapped as {"actions": ...} and sent
// verbatim, without compiling device timing or falling back to legacy mouse
// and keyboard commands.
//
// Validation errors are returned before any packet is sent and the lease stays
// usable. Once the HTTP request is actually in flight any error or context
// cancellation means the remote outcome is unknown: the request is sent
// exactly once with no retry or compensation, the lease is retired and the
// session is quarantined. The original HTTP, context or CommandError cause is
// returned unchanged. On success the lease remains held with its browser input
// state intact.
func (l *InputLease) Perform(ctx context.Context, actions json.RawMessage) error {
	// Retirement dominates: every call on a closed lease is rejected without
	// touching the payload or the network.
	if l.isClosed() {
		return ErrInputLeaseClosed
	}
	// Validation happens before any packet is sent. On a live lease a rejected
	// payload leaves the lease fully usable.
	if err := validateActions(actions); err != nil {
		return err
	}
	if err := l.acquireExec(ctx); err != nil {
		return err
	}
	defer l.releaseExec()
	if l.isClosed() {
		return ErrInputLeaseClosed
	}

	// A map of RawMessage marshals back to {"actions":<actions>} byte for
	// byte, preserving the caller's payload.
	p := map[string]json.RawMessage{"actions": actions}
	_, _, err := l.sess.wd.doContext(ctx, p, "POST", "/session/%s/actions", l.sess.Id)
	if err != nil {
		// The packet may have reached the browser: retire into quarantine and
		// surface the original cause without wrapping or retrying it.
		l.state.retire(l, true)
		return err
	}
	return nil
}

// Release releases the remote input state via DELETE /session/:id/actions.
// Success retires the lease and opens the latch. A failure after the request
// was sent retires the lease and keeps the session quarantined. Calls on a
// retired lease return ErrInputLeaseClosed without sending another packet.
// Cancellation while queued for the execution token leaves the lease alone.
func (l *InputLease) Release(ctx context.Context) error {
	if l.isClosed() {
		return ErrInputLeaseClosed
	}
	if err := l.acquireExec(ctx); err != nil {
		return err
	}
	defer l.releaseExec()
	if l.isClosed() {
		return ErrInputLeaseClosed
	}

	_, _, err := l.sess.wd.doContext(ctx, nil, "DELETE", "/session/%s/actions", l.sess.Id)
	if err != nil {
		l.state.retire(l, true)
		return err
	}
	l.state.retire(l, false)
	return nil
}

// ResetInputs performs an explicit remote input release via
// DELETE /session/:id/actions without holding a lease. It returns
// ErrInputBusy if a lease is live instead of preempting it. Resets run
// exclusively: concurrent resets queue and AcquireInputs waits until the
// reset fully completes. A successful reset clears quarantine; a failed one
// preserves it so the session stays stopped until recovery is confirmed.
func (s Session) ResetInputs(ctx context.Context) error {
	if s.input == nil {
		s = s.bindInputState()
	}
	st := s.input
	if err := st.beginReset(ctx); err != nil {
		return err
	}
	// The state lock is not held across the HTTP round trip.
	_, _, err := s.wd.doContext(ctx, nil, "DELETE", "/session/%s/actions", s.Id)
	st.endReset(err == nil)
	return err
}
