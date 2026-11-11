// Package cdptest provides a deterministic, in-memory rod.CDPClient for
// driving the production Browser/lane code without launching Chromium. It only
// answers the Chrome DevTools Protocol and scripts timing; all state merging
// stays in the code under test.
package cdptest

import (
	"context"
	"encoding/json"
	"fmt"
	"sync"
	"sync/atomic"

	"github.com/go-rod/rod/lib/cdp"
	"github.com/go-rod/rod/lib/proto"
)

// globalSeq emulates Chrome's process-wide unique ids so targets created by
// separate Browser instances never collide in the lane page registry.
var globalSeq atomic.Uint64

// CallRecord is one CDP command observed by the fake transport.
type CallRecord struct {
	SessionID string
	Method    string
	Params    json.RawMessage
}

// Target is one scripted browser target (an isolated page).
type Target struct {
	ID        proto.TargetTargetID
	SessionID proto.TargetSessionID
	ContextID proto.BrowserBrowserContextID
}

// Transport is a scripted rod.CDPClient. It is safe for concurrent use.
type Transport struct {
	mu sync.Mutex

	eventCh chan *cdp.Event

	targets  []Target
	byTarget map[proto.TargetTargetID]*Target
	failures map[string]error

	calls      []CallRecord
	setCookies map[proto.TargetSessionID][]*proto.NetworkCookieParam
	// finalCookies is what Network.getCookies (full context read) returns per
	// page session.
	finalCookies map[proto.TargetSessionID][]*proto.NetworkCookie

	// getCookiesEntered is closed once a getCookies call starts; the call then
	// blocks on getCookiesRelease (or the call's context) when release is set.
	getCookiesEntered chan struct{}
	getCookiesRelease chan struct{}

	getCookiesCalls  int
	closedTargets    map[proto.TargetTargetID]bool
	disposedContexts map[proto.BrowserBrowserContextID]bool
}

// NewTransport builds an empty fake transport.
func NewTransport() *Transport {
	return &Transport{
		eventCh:          make(chan *cdp.Event, 128),
		byTarget:         map[proto.TargetTargetID]*Target{},
		failures:         map[string]error{},
		setCookies:       map[proto.TargetSessionID][]*proto.NetworkCookieParam{},
		finalCookies:     map[proto.TargetSessionID][]*proto.NetworkCookie{},
		closedTargets:    map[proto.TargetTargetID]bool{},
		disposedContexts: map[proto.BrowserBrowserContextID]bool{},
	}
}

// Event implements rod.CDPClient.
func (t *Transport) Event() <-chan *cdp.Event { return t.eventCh }

// FailMethod makes every call of method return err until cleared.
func (t *Transport) FailMethod(method string, err error) {
	t.mu.Lock()
	defer t.mu.Unlock()
	if err == nil {
		delete(t.failures, method)
		return
	}
	t.failures[method] = err
}

// BlockGetCookies installs a barrier: the next Network.getCookies call signals
// entered and waits until ReleaseGetCookies (or context cancellation).
func (t *Transport) BlockGetCookies() (entered, release chan struct{}) {
	t.mu.Lock()
	defer t.mu.Unlock()
	entered = make(chan struct{})
	release = make(chan struct{})
	t.getCookiesEntered = entered
	t.getCookiesRelease = release
	return entered, release
}

// ReleaseGetCookies removes any installed getCookies barrier.
func (t *Transport) ReleaseGetCookies() {
	t.mu.Lock()
	release := t.getCookiesRelease
	t.getCookiesEntered = nil
	t.getCookiesRelease = nil
	t.mu.Unlock()
	if release != nil {
		close(release)
	}
}

// SetFinalCookies scripts the complete cookie set a page's context reports at
// settlement.
func (t *Transport) SetFinalCookies(sessionID proto.TargetSessionID, cookies []*proto.NetworkCookie) {
	t.mu.Lock()
	defer t.mu.Unlock()
	cloned := make([]*proto.NetworkCookie, 0, len(cookies))
	for _, cookie := range cookies {
		if cookie == nil {
			continue
		}
		c := *cookie
		cloned = append(cloned, &c)
	}
	t.finalCookies[sessionID] = cloned
}

// RestoredCookies returns the cookies written via Network.setCookies for a
// page session.
func (t *Transport) RestoredCookies(sessionID proto.TargetSessionID) []*proto.NetworkCookieParam {
	t.mu.Lock()
	defer t.mu.Unlock()
	src := t.setCookies[sessionID]
	out := make([]*proto.NetworkCookieParam, 0, len(src))
	for _, cookie := range src {
		c := *cookie
		out = append(out, &c)
	}
	return out
}

// Calls returns a copy of every CDP call in arrival order, optionally filtered
// by method.
func (t *Transport) Calls(methods ...string) []CallRecord {
	t.mu.Lock()
	defer t.mu.Unlock()
	want := map[string]bool{}
	for _, method := range methods {
		want[method] = true
	}
	out := make([]CallRecord, 0, len(t.calls))
	for _, record := range t.calls {
		if len(want) > 0 && !want[record.Method] {
			continue
		}
		out = append(out, record)
	}
	return out
}

// GetCookiesCalls reports how many full-context getCookies calls occurred.
func (t *Transport) GetCookiesCalls() int {
	t.mu.Lock()
	defer t.mu.Unlock()
	return t.getCookiesCalls
}

// Targets returns every created target in creation order.
func (t *Transport) Targets() []Target {
	t.mu.Lock()
	defer t.mu.Unlock()
	out := make([]Target, len(t.targets))
	copy(out, t.targets)
	return out
}

// LastTarget returns the most recently created target.
func (t *Transport) LastTarget() Target {
	t.mu.Lock()
	defer t.mu.Unlock()
	return t.targets[len(t.targets)-1]
}

// ClosedTargets reports the targets closed via Page.close.
func (t *Transport) ClosedTargets() []proto.TargetTargetID {
	t.mu.Lock()
	defer t.mu.Unlock()
	out := make([]proto.TargetTargetID, 0, len(t.closedTargets))
	for id := range t.closedTargets {
		out = append(out, id)
	}
	return out
}

// DisposedContexts reports the disposed browser context ids.
func (t *Transport) DisposedContexts() []proto.BrowserBrowserContextID {
	t.mu.Lock()
	defer t.mu.Unlock()
	out := make([]proto.BrowserBrowserContextID, 0, len(t.disposedContexts))
	for id := range t.disposedContexts {
		out = append(out, id)
	}
	return out
}

// Call implements rod.CDPClient.
func (t *Transport) Call(ctx context.Context, sessionID, method string, params interface{}) ([]byte, error) {
	rawParams, _ := json.Marshal(params)

	t.mu.Lock()
	if err, ok := t.failures[method]; ok {
		t.mu.Unlock()
		return nil, err
	}
	t.calls = append(t.calls, CallRecord{SessionID: sessionID, Method: method, Params: rawParams})
	t.mu.Unlock()

	switch method {
	case "Target.createBrowserContext":
		return t.createBrowserContext()
	case "Target.createTarget":
		return t.createTarget()
	case "Target.attachToTarget":
		return t.attachToTarget(sessionID, rawParams)
	case "Target.getTargetInfo":
		return t.getTargetInfo(rawParams)
	case "Target.disposeBrowserContext":
		return t.disposeBrowserContext(rawParams)
	case "Network.setCookies":
		return t.setCookiesCall(proto.TargetSessionID(sessionID), rawParams)
	case "Network.getCookies":
		return t.getCookies(ctx, proto.TargetSessionID(sessionID))
	case "Runtime.evaluate":
		return []byte(`{"result":{"objectId":"scripted-js-context","type":"object"}}`), nil
	case "Runtime.callFunctionOn":
		return []byte(`{"result":{"value":null}}`), nil
	case "Page.close":
		return t.pageClose(sessionID)
	default:
		// Enables, emulation, navigation, and all other commands succeed.
		return []byte(`{}`), nil
	}
}

func (t *Transport) createBrowserContext() ([]byte, error) {
	id := proto.BrowserBrowserContextID(fmt.Sprintf("context-%d", globalSeq.Add(1)))
	return json.Marshal(struct {
		BrowserContextID proto.BrowserBrowserContextID `json:"browserContextId"`
	}{id})
}

func (t *Transport) createTarget() ([]byte, error) {
	id := proto.TargetTargetID(fmt.Sprintf("target-%d", globalSeq.Add(1)))
	return json.Marshal(struct {
		TargetID proto.TargetTargetID `json:"targetId"`
	}{id})
}

func (t *Transport) attachToTarget(_ string, rawParams []byte) ([]byte, error) {
	var req proto.TargetAttachToTarget
	if err := json.Unmarshal(rawParams, &req); err != nil {
		return nil, err
	}
	seq := globalSeq.Add(1)
	sessionID := proto.TargetSessionID(fmt.Sprintf("session-%d", seq))
	contextID := proto.BrowserBrowserContextID(fmt.Sprintf("context-for-%s", req.TargetID))
	target := &Target{ID: req.TargetID, SessionID: sessionID, ContextID: contextID}
	t.mu.Lock()
	t.targets = append(t.targets, *target)
	t.byTarget[req.TargetID] = target
	t.mu.Unlock()
	return json.Marshal(struct {
		SessionID proto.TargetSessionID `json:"sessionId"`
	}{sessionID})
}

func (t *Transport) getTargetInfo(rawParams []byte) ([]byte, error) {
	var req proto.TargetGetTargetInfo
	if err := json.Unmarshal(rawParams, &req); err != nil {
		return nil, err
	}
	t.mu.Lock()
	target := t.byTarget[req.TargetID]
	t.mu.Unlock()
	if target == nil {
		return nil, fmt.Errorf("unknown target %q", req.TargetID)
	}
	return json.Marshal(struct {
		TargetInfo *proto.TargetTargetInfo `json:"targetInfo"`
	}{
		TargetInfo: &proto.TargetTargetInfo{
			TargetID:         target.ID,
			BrowserContextID: target.ContextID,
			Type:             "page",
			URL:              "about:blank",
		},
	})
}

func (t *Transport) disposeBrowserContext(rawParams []byte) ([]byte, error) {
	var req proto.TargetDisposeBrowserContext
	if err := json.Unmarshal(rawParams, &req); err != nil {
		return nil, err
	}
	t.mu.Lock()
	t.disposedContexts[req.BrowserContextID] = true
	t.mu.Unlock()
	return []byte(`{}`), nil
}

func (t *Transport) setCookiesCall(sessionID proto.TargetSessionID, rawParams []byte) ([]byte, error) {
	var req proto.NetworkSetCookies
	if err := json.Unmarshal(rawParams, &req); err != nil {
		return nil, err
	}
	t.mu.Lock()
	t.setCookies[sessionID] = req.Cookies
	t.mu.Unlock()
	return []byte(`{}`), nil
}

func (t *Transport) getCookies(ctx context.Context, sessionID proto.TargetSessionID) ([]byte, error) {
	t.mu.Lock()
	t.getCookiesCalls++
	cookies := t.finalCookies[sessionID]
	entered := t.getCookiesEntered
	release := t.getCookiesRelease
	t.mu.Unlock()

	if entered != nil {
		close(entered)
		select {
		case <-release:
		case <-ctx.Done():
			return nil, ctx.Err()
		}
	}

	return json.Marshal(struct {
		Cookies []*proto.NetworkCookie `json:"cookies"`
	}{cookies})
}

func (t *Transport) pageClose(sessionID string) ([]byte, error) {
	t.mu.Lock()
	var targetID proto.TargetTargetID
	for _, target := range t.targets {
		if string(target.SessionID) == sessionID {
			targetID = target.ID
			break
		}
	}
	t.closedTargets[targetID] = true
	t.mu.Unlock()

	// rod's Page.Close waits for Target.targetDestroyed; publish it.
	if targetID != "" {
		params, _ := json.Marshal(proto.TargetTargetDestroyed{TargetID: targetID})
		t.eventCh <- &cdp.Event{Method: "Target.targetDestroyed", Params: params}
	}
	return []byte(`{}`), nil
}
