// Copyright 2013 Federico Sogaro. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package webdriver

import (
	"context"
	"encoding/json"
	"errors"
	"io/ioutil"
	"net/http"
	"net/http/httptest"
	"reflect"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/fedesog/webdriver/task_support/taskcheck"
)

// recordedRequest is one captured server hit. Bodies are kept verbatim so the
// protocol test can assert the raw W3C payload is not rewritten.
type recordedRequest struct {
	method string
	path   string
	body   string
}

// leaseTestServer is a controllable JSON Wire Protocol endpoint. Channel
// barriers decide when an actions or a reset request is accepted and when it
// is allowed to answer; failure counters force deterministic remote errors.
type leaseTestServer struct {
	srv *httptest.Server

	mu   sync.Mutex
	reqs []recordedRequest
	// failActions/failReset count down the next failing responses.
	failActions int
	failReset   int

	// Barrier channels are installed before the phase that needs them and
	// are swapped only through barrierMu so handler goroutines never race a
	// reassignment. Entered channels are buffered so handlers never block
	// reporting in.
	barrierMu     sync.Mutex
	actionEntered chan struct{}
	actionProceed chan struct{}
	resetEntered  chan struct{}
	resetProceed  chan struct{}
}

func newLeaseTestServer() *leaseTestServer {
	ts := &leaseTestServer{}
	ts.srv = httptest.NewServer(http.HandlerFunc(ts.handler))
	return ts
}

func (ts *leaseTestServer) URL() string { return ts.srv.URL }

func (ts *leaseTestServer) Close() { ts.srv.Close() }

func (ts *leaseTestServer) setActionBarriers(entered, proceed chan struct{}) {
	ts.barrierMu.Lock()
	ts.actionEntered, ts.actionProceed = entered, proceed
	ts.barrierMu.Unlock()
}

func (ts *leaseTestServer) actionBarriers() (chan struct{}, chan struct{}) {
	ts.barrierMu.Lock()
	defer ts.barrierMu.Unlock()
	return ts.actionEntered, ts.actionProceed
}

func (ts *leaseTestServer) setResetBarriers(entered, proceed chan struct{}) {
	ts.barrierMu.Lock()
	ts.resetEntered, ts.resetProceed = entered, proceed
	ts.barrierMu.Unlock()
}

func (ts *leaseTestServer) resetBarriers() (chan struct{}, chan struct{}) {
	ts.barrierMu.Lock()
	defer ts.barrierMu.Unlock()
	return ts.resetEntered, ts.resetProceed
}

func (ts *leaseTestServer) record(method, path, body string) {
	ts.mu.Lock()
	ts.reqs = append(ts.reqs, recordedRequest{method, path, body})
	ts.mu.Unlock()
}

func (ts *leaseTestServer) count(method, suffix string) int {
	ts.mu.Lock()
	defer ts.mu.Unlock()
	n := 0
	for _, q := range ts.reqs {
		if q.method == method && strings.HasSuffix(q.path, suffix) {
			n++
		}
	}
	return n
}

func (ts *leaseTestServer) total() int {
	ts.mu.Lock()
	defer ts.mu.Unlock()
	return len(ts.reqs)
}

func (ts *leaseTestServer) bodies(method, suffix string) []string {
	ts.mu.Lock()
	defer ts.mu.Unlock()
	var out []string
	for _, q := range ts.reqs {
		if q.method == method && strings.HasSuffix(q.path, suffix) {
			out = append(out, q.body)
		}
	}
	return out
}

func (ts *leaseTestServer) writeOK(w http.ResponseWriter, value interface{}) {
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]interface{}{
		"status":    0,
		"sessionId": "s",
		"value":     value,
	})
}

func (ts *leaseTestServer) writeCommandError(w http.ResponseWriter) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusInternalServerError)
	json.NewEncoder(w).Encode(map[string]interface{}{
		"status":    UnknownError,
		"sessionId": "s",
		"value":     map[string]string{"message": "synthetic remote failure"},
	})
}

// barrier waits until the phase releases the handler, but also unwinds if the
// client gave up so the server goroutine cannot leak.
func barrier(proceed <-chan struct{}, r *http.Request) {
	select {
	case <-proceed:
	case <-r.Context().Done():
	}
}

func (ts *leaseTestServer) handler(w http.ResponseWriter, r *http.Request) {
	raw, _ := ioutil.ReadAll(r.Body)
	body := string(raw)
	path := r.URL.Path

	switch {
	case r.Method == "POST" && path == "/session":
		ts.record(r.Method, path, body)
		ts.writeOK(w, Capabilities{})
	case r.Method == "GET" && path == "/sessions":
		ts.record(r.Method, path, body)
		ts.writeOK(w, []map[string]interface{}{
			{"id": "s", "capabilities": Capabilities{}},
		})
	case r.Method == "POST" && strings.HasSuffix(path, "/actions"):
		ts.record(r.Method, path, body)
		if entered, proceed := ts.actionBarriers(); entered != nil {
			select {
			case entered <- struct{}{}:
			default:
			}
			barrier(proceed, r)
		}
		ts.mu.Lock()
		fail := ts.failActions > 0
		if fail {
			ts.failActions--
		}
		ts.mu.Unlock()
		if fail {
			ts.writeCommandError(w)
			return
		}
		ts.writeOK(w, nil)
	case r.Method == "DELETE" && strings.HasSuffix(path, "/actions"):
		ts.record(r.Method, path, body)
		if entered, proceed := ts.resetBarriers(); entered != nil {
			select {
			case entered <- struct{}{}:
			default:
			}
			barrier(proceed, r)
		}
		ts.mu.Lock()
		fail := ts.failReset > 0
		if fail {
			ts.failReset--
		}
		ts.mu.Unlock()
		if fail {
			ts.writeCommandError(w)
			return
		}
		ts.writeOK(w, nil)
	case r.Method == "POST" && path == "/session/s/keys":
		ts.record(r.Method, path, body)
		ts.writeOK(w, nil)
	case r.Method == "DELETE" && path == "/session/s":
		ts.record(r.Method, path, body)
		ts.writeOK(w, nil)
	default:
		w.WriteHeader(http.StatusNotFound)
	}
}

func newChromeTestDriver(ts *leaseTestServer) *ChromeDriver {
	return &ChromeDriver{WebDriverCore: WebDriverCore{url: ts.URL()}}
}

func newFirefoxTestDriver(ts *leaseTestServer) *FirefoxDriver {
	return &FirefoxDriver{WebDriverCore: WebDriverCore{url: ts.URL()}}
}

// waitUntil polls fn until it holds, recording a taskcheck failure otherwise.
func waitUntil(c *taskcheck.C, fn func() bool, msg string, args ...interface{}) {
	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		if fn() {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	c.Check(false, msg, args...)
}

var validActions = json.RawMessage(`[{"type":"key","id":"keyboard","actions":[{"type":"keyDown","key":"a"}]}]`)

// TestInputLeaseOwnership covers the latch itself: value-copy sharing, the
// single live lease per real session, driver independence, cancellation while
// queued, the production NewSession/Sessions entries of both drivers and the
// session generation switch after Delete.
func TestInputLeaseOwnership(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		ts := newLeaseTestServer()
		defer ts.Close()
		ctx := context.Background()

		d := newChromeTestDriver(ts)
		s, err := d.NewSession(nil, nil)
		c.Check(err == nil, "NewSession failed: %v", err)

		// An already canceled context cannot take a free latch.
		canceledCtx, cancel := context.WithCancel(ctx)
		cancel()
		_, err = s.AcquireInputs(canceledCtx)
		c.Check(errors.Is(err, context.Canceled), "canceled context took free latch: %v", err)

		requestsBeforeAcquire := ts.total()
		lease, err := s.AcquireInputs(ctx)
		c.Check(err == nil, "AcquireInputs failed: %v", err)
		c.Check(lease != nil, "AcquireInputs returned nil lease")
		// Acquisition itself is local state only.
		c.Check(ts.total() == requestsBeforeAcquire, "AcquireInputs sent %d remote requests", ts.total()-requestsBeforeAcquire)

		// A second live lease on the same session is impossible.
		_, err = s.AcquireInputs(canceledCtx)
		c.Check(errors.Is(err, context.Canceled), "queued acquire did not honor canceled context: %v", err)
		waitCtx, waitCancel := context.WithTimeout(ctx, 100*time.Millisecond)
		_, err = s.AcquireInputs(waitCtx)
		waitCancel()
		c.Check(errors.Is(err, context.DeadlineExceeded), "busy latch was granted: %v", err)

		// Value copies of the Session share the very same latch.
		sessionCopy := *s
		c.Check(sessionCopy.input == s.input, "Session value copy got a different latch")

		// Views produced by Sessions() resolve to the same generation.
		views, err := d.Sessions()
		c.Check(err == nil, "Sessions failed: %v", err)
		c.Check(len(views) == 1 && views[0].input == s.input, "enumerated session does not share latch")
		_, err = views[0].AcquireInputs(canceledCtx)
		c.Check(errors.Is(err, context.Canceled), "enumerated view bypassed the live lease: %v", err)

		// A different driver is an independent latch even with the same id.
		d2 := newChromeTestDriver(ts)
		s2, err := d2.NewSession(nil, nil)
		c.Check(err == nil, "second driver NewSession failed: %v", err)
		c.Check(s2.input != s.input, "different drivers share input state")
		otherLease, err := s2.AcquireInputs(ctx)
		c.Check(err == nil, "independent driver was blocked by foreign lease: %v", err)
		c.Check(otherLease.Release(ctx) == nil, "independent lease release failed")

		// Cancellation while queued leaves the lease and the latch intact.
		queueCtx, queueCancel := context.WithCancel(ctx)
		queuedErr := make(chan error, 1)
		go func() {
			_, qerr := sessionCopy.AcquireInputs(queueCtx)
			queuedErr <- qerr
		}()
		queueCancel()
		select {
		case qerr := <-queuedErr:
			c.Check(errors.Is(qerr, context.Canceled), "queued acquire returned %v", qerr)
		case <-time.After(3 * time.Second):
			c.Check(false, "queued acquire ignored cancellation")
		}

		// Legacy entry points are not gated by the lease and still go out.
		c.Check(s.SendKeysOnActiveElement("ab") == nil, "legacy SendKeys failed")
		c.Check(ts.count("POST", "/keys") == 1, "legacy keys endpoint not hit")

		// Releasing opens the latch and a real new lease can be acquired.
		deleteBefore := ts.count("DELETE", "/actions")
		c.Check(lease.Release(ctx) == nil, "Release failed")
		c.Check(ts.count("DELETE", "/actions") == deleteBefore+1, "release did not DELETE actions")
		nextLease, err := sessionCopy.AcquireInputs(ctx)
		c.Check(err == nil, "acquire after release failed: %v", err)
		c.Check(nextLease != lease, "released handle was handed out again")

		// Firefox production entries share state the same way.
		fd := newFirefoxTestDriver(ts)
		fs, err := fd.NewSession(nil, nil)
		c.Check(err == nil, "firefox NewSession failed: %v", err)
		fViews, err := fd.Sessions()
		c.Check(err == nil, "firefox Sessions failed: %v", err)
		c.Check(len(fViews) == 1 && fViews[0].input == fs.input, "firefox enumerated view lost shared state")
		fLease, err := fs.AcquireInputs(ctx)
		c.Check(err == nil, "firefox acquire failed: %v", err)
		c.Check(fLease.Release(ctx) == nil, "firefox release failed")

		// Successful session Delete retires the whole generation.
		c.Check(s.Delete() == nil, "session Delete failed")
		c.Check(ts.count("DELETE", "/session/s") == 1, "session delete not sent")
		_, err = s.AcquireInputs(ctx)
		c.Check(errors.Is(err, ErrInputLeaseClosed), "deleted generation still acquirable: %v", err)
		c.Check(errors.Is(nextLease.Perform(ctx, validActions), ErrInputLeaseClosed), "retired lease still performs")
		c.Check(errors.Is(nextLease.Release(ctx), ErrInputLeaseClosed), "retired lease still releases")

		// Recreating the same id starts a fresh generation.
		reborn, err := d.NewSession(nil, nil)
		c.Check(err == nil, "recreate session failed: %v", err)
		c.Check(reborn.input != s.input, "recreated session reused dead generation")
		rebLease, err := reborn.AcquireInputs(ctx)
		c.Check(err == nil, "fresh generation could not acquire: %v", err)
		c.Check(rebLease.Release(ctx) == nil, "fresh generation release failed")
	})
}

// TestInputLeaseRecovery covers uncertain outcomes: remote Perform failures,
// client cancellation after the server accepted the request, reset failures
// followed by success, reset serialization, and retired handles.
func TestInputLeaseRecovery(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		ts := newLeaseTestServer()
		defer ts.Close()
		ctx := context.Background()

		d := newChromeTestDriver(ts)
		s, err := d.NewSession(nil, nil)
		c.Check(err == nil, "NewSession failed: %v", err)
		lease, err := s.AcquireInputs(ctx)
		c.Check(err == nil, "AcquireInputs failed: %v", err)

		// A remote Perform error quarantines the session and retires the lease.
		ts.failActions = 1
		err = lease.Perform(ctx, validActions)
		var commandErr *CommandError
		c.Check(errors.As(err, &commandErr), "Perform error not traceable to CommandError: %v", err)
		c.Check(commandErr != nil && commandErr.StatusCode == UnknownError, "unexpected command error: %v", err)
		c.Check(ts.count("POST", "/actions") == 1, "failed Perform sent more than one request")
		c.Check(errors.Is(lease.Perform(ctx, validActions), ErrInputLeaseClosed), "failed lease was not retired")
		c.Check(errors.Is(lease.Release(ctx), ErrInputLeaseClosed), "retired lease emitted another release")
		c.Check(ts.count("DELETE", "/actions") == 0, "retired handle sent a DELETE")

		// Recovery must go through a real new acquire, not an internal flag.
		_, err = s.AcquireInputs(ctx)
		c.Check(errors.Is(err, ErrInputUncertain), "quarantined acquire returned %v", err)

		// ResetInputs refuses to preempt a live lease and sends nothing.
		healthy, err := newChromeTestDriver(ts).NewSession(nil, nil)
		c.Check(err == nil, "second session failed: %v", err)
		healthyLease, err := healthy.AcquireInputs(ctx)
		c.Check(err == nil, "healthy acquire failed: %v", err)
		deleteBefore := ts.count("DELETE", "/actions")
		err = healthy.ResetInputs(ctx)
		c.Check(errors.Is(err, ErrInputBusy), "ResetInputs did not report busy: %v", err)
		c.Check(ts.count("DELETE", "/actions") == deleteBefore, "busy reset wrote the remote")
		c.Check(healthyLease.Release(ctx) == nil, "healthy release failed")

		// A failed reset preserves quarantine.
		ts.failReset = 1
		err = s.ResetInputs(ctx)
		c.Check(errors.As(err, &commandErr), "failed reset lost its cause: %v", err)
		_, err = s.AcquireInputs(ctx)
		c.Check(errors.Is(err, ErrInputUncertain), "failed reset cleared quarantine")

		// The next, successful reset clears quarantine and admits a new lease.
		c.Check(s.ResetInputs(ctx) == nil, "recovery reset failed")
		c.Check(ts.count("DELETE", "/actions") >= 2, "reset DELETEs not observed")
		recovered, err := s.AcquireInputs(ctx)
		c.Check(err == nil, "acquire after recovery failed: %v", err)
		c.Check(recovered.Perform(ctx, validActions) == nil, "perform on recovered lease failed")
		c.Check(recovered.Release(ctx) == nil, "recovered lease release failed")
		c.Check(errors.Is(recovered.Perform(ctx, validActions), ErrInputLeaseClosed), "released lease still performs")

		// Cancellation after remote acceptance: one packet, uncertain result.
		uncertainLease, err := s.AcquireInputs(ctx)
		c.Check(err == nil, "second acquire failed: %v", err)
		actionEntered := make(chan struct{}, 1)
		actionProceed := make(chan struct{})
		ts.setActionBarriers(actionEntered, actionProceed)
		performCtx, performCancel := context.WithCancel(ctx)
		performErr := make(chan error, 1)
		go func() { performErr <- uncertainLease.Perform(performCtx, validActions) }()
		<-actionEntered
		performCancel()
		select {
		case perr := <-performErr:
			c.Check(errors.Is(perr, context.Canceled), "canceled Perform returned %v", perr)
		case <-time.After(3 * time.Second):
			c.Check(false, "canceled Perform never returned")
		}
		close(actionProceed)
		c.Check(ts.count("POST", "/actions") >= 2, "canceled Perform did not reach the server")
		c.Check(errors.Is(uncertainLease.Release(ctx), ErrInputLeaseClosed), "uncertain lease was not retired")
		_, err = s.AcquireInputs(ctx)
		c.Check(errors.Is(err, ErrInputUncertain), "canceled Perform did not quarantine: %v", err)
		c.Check(s.ResetInputs(ctx) == nil, "post-cancel reset failed")

		// A failed Release retires the lease and keeps quarantine.
		releaseLease, err := s.AcquireInputs(ctx)
		c.Check(err == nil, "post-reset acquire failed: %v", err)
		ts.failReset = 1
		err = releaseLease.Release(ctx)
		c.Check(errors.As(err, &commandErr), "release error lost its cause: %v", err)
		deleteBefore = ts.count("DELETE", "/actions")
		c.Check(errors.Is(releaseLease.Release(ctx), ErrInputLeaseClosed), "failed release stayed alive")
		c.Check(ts.count("DELETE", "/actions") == deleteBefore, "retired release sent another DELETE")
		c.Check(s.ResetInputs(ctx) == nil, "final recovery reset failed")

		// Resets are serialized: while one DELETE is on the wire no second
		// reset can write, and acquisitions wait until every reset finishes.
		resetEntered := make(chan struct{}, 1)
		resetProceed := make(chan struct{})
		ts.setResetBarriers(resetEntered, resetProceed)
		deletesAtStart := ts.count("DELETE", "/actions")
		resetErrA := make(chan error, 1)
		go func() { resetErrA <- s.ResetInputs(ctx) }()
		<-resetEntered
		resetErrB := make(chan error, 1)
		go func() { resetErrB <- s.ResetInputs(ctx) }()
		// Only the first reset may be writing while its barrier is closed.
		waitUntil(c, func() bool { return ts.count("DELETE", "/actions") == deletesAtStart+1 }, "first reset never arrived")
		time.Sleep(50 * time.Millisecond)
		c.Check(ts.count("DELETE", "/actions") == deletesAtStart+1, "second reset wrote concurrently")
		close(resetProceed)
		select {
		case rerr := <-resetErrA:
			c.Check(rerr == nil, "reset A failed: %v", rerr)
		case <-time.After(3 * time.Second):
			c.Check(false, "reset A never returned")
		}
		select {
		case rerr := <-resetErrB:
			c.Check(rerr == nil, "reset B failed: %v", rerr)
		case <-time.After(3 * time.Second):
			c.Check(false, "reset B never returned")
		}
		c.Check(ts.count("DELETE", "/actions") == deletesAtStart+2, "expected exactly two serialized resets")

		// A fresh reset blocks AcquireInputs until its remote call completes.
		resetEntered2 := make(chan struct{}, 1)
		resetProceed2 := make(chan struct{})
		ts.setResetBarriers(resetEntered2, resetProceed2)
		resetErrC := make(chan error, 1)
		go func() { resetErrC <- s.ResetInputs(ctx) }()
		<-resetEntered2
		acquireErr := make(chan error, 1)
		go func() {
			l, aerr := s.AcquireInputs(ctx)
			if aerr == nil {
				c.Check(l.Release(ctx) == nil, "queued lease release failed")
			}
			acquireErr <- aerr
		}()
		select {
		case aerr := <-acquireErr:
			c.Check(false, "acquire ran before reset completed: %v", aerr)
		case <-time.After(100 * time.Millisecond):
		}
		close(resetProceed2)
		select {
		case rerr := <-resetErrC:
			c.Check(rerr == nil, "reset C failed: %v", rerr)
		case <-time.After(3 * time.Second):
			c.Check(false, "reset C never returned")
		}
		select {
		case aerr := <-acquireErr:
			c.Check(aerr == nil, "acquire after resets failed: %v", aerr)
		case <-time.After(3 * time.Second):
			c.Check(false, "acquire stayed blocked after resets")
		}
	})
}

// TestInputLeaseProtocol verifies real HTTP paths, methods, the verbatim
// actions envelope, local validation, request counts and request ordering.
func TestInputLeaseProtocol(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		ts := newLeaseTestServer()
		defer ts.Close()
		ctx := context.Background()

		d := newChromeTestDriver(ts)
		s, err := d.NewSession(nil, nil)
		c.Check(err == nil, "NewSession failed: %v", err)
		lease, err := s.AcquireInputs(ctx)
		c.Check(err == nil, "AcquireInputs failed: %v", err)

		// Perform posts the wrapped sources verbatim.
		c.Check(lease.Perform(ctx, validActions) == nil, "Perform failed")
		actionBodies := ts.bodies("POST", "/actions")
		c.Check(len(actionBodies) == 1, "expected one actions request, got %d", len(actionBodies))
		c.Check(actionBodies[0] == `{"actions":`+string(validActions)+`}`, "actions body rewritten: %s", actionBodies[0])
		var envelope map[string]json.RawMessage
		c.Check(json.Unmarshal([]byte(actionBodies[0]), &envelope) == nil, "envelope is not a JSON object")
		var gotActions, wantActions interface{}
		c.Check(json.Unmarshal(envelope["actions"], &gotActions) == nil, "envelope lacks actions")
		c.Check(json.Unmarshal(validActions, &wantActions) == nil, "fixture invalid")
		c.Check(reflect.DeepEqual(gotActions, wantActions), "actions payload altered")

		// Local validation rejects null, empty, non-array and malformed input
		// without sending anything; the lease stays usable afterwards.
		badPayloads := []json.RawMessage{
			nil,
			json.RawMessage(`null`),
			json.RawMessage(`[]`),
			json.RawMessage(`{}`),
			json.RawMessage(`[`),
			json.RawMessage(`"not-array"`),
		}
		for _, bad := range badPayloads {
			before := ts.count("POST", "/actions")
			perr := lease.Perform(ctx, bad)
			c.Check(perr != nil, "invalid actions accepted: %s", string(bad))
			c.Check(ts.count("POST", "/actions") == before, "invalid actions sent a request: %s", string(bad))
		}
		c.Check(lease.Perform(ctx, validActions) == nil, "lease unusable after validation failures")

		// Perform and Release on one lease are strictly serialized.
		actionEntered := make(chan struct{}, 1)
		actionProceed := make(chan struct{})
		ts.setActionBarriers(actionEntered, actionProceed)
		first := json.RawMessage(`[{"type":"key","id":"first","actions":[{"type":"keyDown","key":"a"}]}]`)
		second := json.RawMessage(`[{"type":"key","id":"second","actions":[{"type":"keyDown","key":"b"}]}]`)
		errFirst := make(chan error, 1)
		errSecond := make(chan error, 1)
		go func() { errFirst <- lease.Perform(ctx, first) }()
		<-actionEntered
		go func() { errSecond <- lease.Perform(ctx, second) }()
		time.Sleep(50 * time.Millisecond)
		c.Check(ts.count("POST", "/actions") == 3, "second Perform ran before the first finished")
		close(actionProceed)
		select {
		case perr := <-errFirst:
			c.Check(perr == nil, "serialized first Perform failed: %v", perr)
		case <-time.After(3 * time.Second):
			c.Check(false, "first Perform hung")
		}
		select {
		case perr := <-errSecond:
			c.Check(perr == nil, "serialized second Perform failed: %v", perr)
		case <-time.After(3 * time.Second):
			c.Check(false, "second Perform hung")
		}
		ordered := ts.bodies("POST", "/actions")
		c.Check(len(ordered) == 4, "expected four actions requests, got %d", len(ordered))
		c.Check(strings.Contains(ordered[2], `"id":"first"`) && strings.Contains(ordered[3], `"id":"second"`),
			"serialization order wrong: %s %s", ordered[2], ordered[3])

		// Cancellation while queued for the token sends no packet.
		tokenCtx, tokenCancel := context.WithCancel(ctx)
		tokenErr := make(chan error, 1)
		tokenEntered := make(chan struct{}, 1)
		tokenProceed := make(chan struct{})
		ts.setActionBarriers(tokenEntered, tokenProceed)
		go func() { tokenErr <- lease.Perform(ctx, first) }()
		<-tokenEntered
		go func() {
			lerr := lease.acquireExec(tokenCtx)
			if lerr == nil {
				lease.releaseExec()
			}
			tokenErr <- lerr
		}()
		tokenCancel()
		select {
		case lerr := <-tokenErr:
			c.Check(errors.Is(lerr, context.Canceled), "token wait returned %v", lerr)
		case <-time.After(3 * time.Second):
			c.Check(false, "token wait ignored cancellation")
		}
		close(tokenProceed)
		select {
		case perr := <-tokenErr:
			c.Check(perr == nil, "in-flight Perform failed: %v", perr)
		case <-time.After(3 * time.Second):
			c.Check(false, "in-flight Perform hung")
		}

		// Release and ResetInputs both hit DELETE /session/:id/actions.
		deleteBefore := ts.count("DELETE", "/actions")
		c.Check(lease.Release(ctx) == nil, "Release failed")
		c.Check(ts.count("DELETE", "/actions") == deleteBefore+1, "Release used wrong request count")
		deleteBefore = ts.count("DELETE", "/actions")
		c.Check(s.ResetInputs(ctx) == nil, "ResetInputs failed")
		c.Check(ts.count("DELETE", "/actions") == deleteBefore+1, "ResetInputs used wrong request count")
		for _, q := range ts.bodies("DELETE", "/actions") {
			c.Check(q == "", "DELETE actions carried a body: %q", q)
		}

		// Legacy input commands remain on their original endpoint.
		c.Check(s.SendKeysOnActiveElement("xy") == nil, "legacy SendKeys failed")
		legacyBodies := ts.bodies("POST", "/keys")
		c.Check(len(legacyBodies) == 1 && strings.Contains(legacyBodies[0], `"x"`), "legacy payload wrong: %v", legacyBodies)
	})
}
