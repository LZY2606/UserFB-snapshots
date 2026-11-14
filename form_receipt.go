package godet

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"sync"
)

// FormRequestMatch identifies the form submission request to observe.
// All three fields are required and are matched exactly (the HTTP method
// is compared with the exact case provided by the caller) against the
// first Network.requestWillBeSent event seen after the observer is
// installed.
type FormRequestMatch struct {
	FrameID string // frame that issues the form request
	URL     string // exact request URL
	Method  string // exact HTTP method (case sensitive)
}

// FormHop describes a single HTTP hop of a form submission: the original
// POST, every redirect, and the final response.
type FormHop struct {
	URL          string            // URL of this hop's own request
	Method       string            // method of this hop's own request (may change across redirects)
	Status       int               // status reported by the response that closed this hop
	Headers      map[string]string // headers reported by the response that closed this hop
	HasExtraInfo bool              // whether wire-level extra info was declared for this hop
	WireStatus   int               // status from Network.responseReceivedExtraInfo (0 if none)
	ExtraHeaders map[string]string // headers from Network.responseReceivedExtraInfo (nil if none)
}

// FormReceipt is the saved submission receipt for a form request: the
// request id that was selected and every hop in the order it happened.
type FormReceipt struct {
	RequestID string
	Hops      []FormHop
}

// FormRequestError is returned by WaitFormReceipt when the observed
// request fails to load (Network.loadingFailed).
type FormRequestError struct {
	RequestID string
	ErrorText string
}

// Error implements the error interface for FormRequestError.
func (err *FormRequestError) Error() string {
	return fmt.Sprintf("form request %s failed: %s", err.RequestID, err.ErrorText)
}

var (
	// ErrorFormMatchInvalid is returned when a FormRequestMatch field is empty.
	ErrorFormMatchInvalid = errors.New("godet: form match requires non-empty FrameID, URL and Method")
	// ErrorFormActionNil is returned when WaitFormReceipt is called with a nil action.
	ErrorFormActionNil = errors.New("godet: form action must not be nil")
	// ErrorFormDisconnected is returned when the connection is lost while waiting for a form receipt.
	ErrorFormDisconnected = errors.New("godet: disconnected while waiting for form receipt")
)

// eventObserver is an internal event sink that receives a copy of every
// dispatched event, independently of user-registered callbacks.
type eventObserver interface {
	handleEvent(method string, params Params)
}

// addObserver registers an internal observer for the event stream.
func (remote *RemoteDebugger) addObserver(o eventObserver) {
	remote.Lock()
	remote.observers[o] = struct{}{}
	remote.Unlock()
}

// removeObserver unregisters an internal observer.
func (remote *RemoteDebugger) removeObserver(o eventObserver) {
	remote.Lock()
	delete(remote.observers, o)
	remote.Unlock()
}

// formExtraInfo holds the wire-level response info carried by
// Network.responseReceivedExtraInfo.
type formExtraInfo struct {
	status  int
	headers map[string]string
}

// formObserver tracks the network events of a single form submission.
type formObserver struct {
	match FormRequestMatch

	mu        sync.Mutex
	matched   bool
	requestID string

	hops        []FormHop                  // completed hops plus the currently open one
	open        bool                       // the last hop is still waiting for its response
	extraFilled []bool                     // parallel to hops: extra info already assigned
	extras      []formExtraInfo            // extras for the matched request, in arrival order
	assigned    int                        // number of extras consumed by hops
	early       map[string][]formExtraInfo // extras seen before the request id was known

	finished bool          // loadingFinished received for the matched request
	settled  bool          // observation concluded (complete, failed or aborted)
	err      error         // failure outcome, if any
	done     chan struct{} // closed when the observation concludes
}

// handleEvent processes one event from the debugger event stream.
func (o *formObserver) handleEvent(method string, p Params) {
	o.mu.Lock()
	defer o.mu.Unlock()

	if o.settled {
		return
	}

	switch method {
	case "Network.requestWillBeSent":
		o.onRequestWillBeSent(p)
	case "Network.responseReceived":
		o.onResponseReceived(p)
	case "Network.responseReceivedExtraInfo":
		o.onExtraInfo(p)
	case "Network.loadingFinished":
		o.onLoadingFinished(p)
	case "Network.loadingFailed":
		o.onLoadingFailed(p)
	case EventClosed:
		o.fail(ErrorClose)
	case EventDisconnect:
		o.fail(ErrorFormDisconnected)
	}
}

// abort marks the observer as concluded without an outcome; used when the
// waiter goes away (context cancelled, connection closed) so late events
// cannot touch the observation state anymore.
func (o *formObserver) abort() {
	o.mu.Lock()
	o.settled = true
	o.mu.Unlock()
}

func (o *formObserver) onRequestWillBeSent(p Params) {
	reqID := p.String("requestId")
	req := p.Map("request")

	if !o.matched {
		if req == nil ||
			p.String("frameId") != o.match.FrameID ||
			paramsString(req, "url") != o.match.URL ||
			paramsString(req, "method") != o.match.Method {
			return
		}

		// the first exact match fixes the request id for this observation
		o.matched = true
		o.requestID = reqID
		o.hops = append(o.hops, FormHop{URL: o.match.URL, Method: o.match.Method})
		o.extraFilled = append(o.extraFilled, false)
		o.open = true

		// extra info that arrived before the request id was known
		if saved, ok := o.early[reqID]; ok {
			o.extras = append(o.extras, saved...)
			delete(o.early, reqID)
			o.assignExtras()
		}
		return
	}

	if reqID != o.requestID || !o.open {
		return
	}

	redirect := p.Map("redirectResponse")
	if redirect == nil {
		return
	}

	// the redirect response closes the current hop; whether it carries
	// extra info is declared by this same event
	o.closeHop(redirect, p.Bool("redirectHasExtraInfo"))

	// the next hop starts from this event's own request
	o.hops = append(o.hops, FormHop{
		URL:    paramsString(req, "url"),
		Method: paramsString(req, "method"),
	})
	o.extraFilled = append(o.extraFilled, false)
	o.open = true
}

func (o *formObserver) onResponseReceived(p Params) {
	if !o.matched || !o.open || p.String("requestId") != o.requestID {
		return
	}

	resp := p.Map("response")
	if resp == nil {
		return
	}

	// the final hop is closed by its response
	o.closeHop(resp, p.Bool("hasExtraInfo"))
	o.checkComplete()
}

func (o *formObserver) onExtraInfo(p Params) {
	ex := formExtraInfo{
		status:  p.Int("statusCode"),
		headers: paramsStringMap(p.Map("headers")),
	}
	reqID := p.String("requestId")

	if !o.matched {
		// early extra info for a request id we don't know yet: stash it
		// for this observation only
		o.early[reqID] = append(o.early[reqID], ex)
		return
	}

	if reqID != o.requestID {
		return
	}

	o.extras = append(o.extras, ex)
	o.assignExtras()
	o.checkComplete()
}

func (o *formObserver) onLoadingFinished(p Params) {
	if !o.matched || p.String("requestId") != o.requestID {
		return
	}

	o.finished = true
	o.checkComplete()
}

func (o *formObserver) onLoadingFailed(p Params) {
	if !o.matched || p.String("requestId") != o.requestID {
		return
	}

	o.fail(&FormRequestError{RequestID: o.requestID, ErrorText: p.String("errorText")})
}

// closeHop completes the currently open hop with the given response.
func (o *formObserver) closeHop(response map[string]interface{}, hasExtraInfo bool) {
	h := &o.hops[len(o.hops)-1]
	h.Status = paramsInt(response, "status")
	h.Headers = paramsStringMap(paramsMap(response, "headers"))
	h.HasExtraInfo = hasExtraInfo
	o.open = false
	o.assignExtras()
}

// assignExtras consumes received extras in arrival order, assigning each
// to the next hop (in hop order) that declared extra info. Hops declared
// without extra info never consume an entry.
func (o *formObserver) assignExtras() {
	for o.assigned < len(o.extras) {
		target := -1

		for i := range o.hops {
			closed := i < len(o.hops)-1 || !o.open
			if closed && o.hops[i].HasExtraInfo && !o.extraFilled[i] {
				target = i
				break
			}
		}

		if target < 0 {
			return
		}

		ex := o.extras[o.assigned]
		o.assigned++
		o.hops[target].WireStatus = ex.status
		o.hops[target].ExtraHeaders = ex.headers
		o.extraFilled[target] = true
	}
}

// checkComplete settles the observation once the final loadingFinished
// arrived, the last hop is closed and every declared extra info has been
// consumed.
func (o *formObserver) checkComplete() {
	if !o.finished || o.open {
		return
	}

	for i := range o.hops {
		if o.hops[i].HasExtraInfo && !o.extraFilled[i] {
			return
		}
	}

	o.settled = true
	close(o.done)
}

// fail settles the observation with an error.
func (o *formObserver) fail(err error) {
	o.settled = true
	o.err = err
	close(o.done)
}

// outcome returns the observation result as an independent snapshot.
func (o *formObserver) outcome() (*FormReceipt, error) {
	o.mu.Lock()
	defer o.mu.Unlock()

	if o.err != nil {
		return nil, o.err
	}

	hops := make([]FormHop, len(o.hops))
	for i, h := range o.hops {
		hops[i] = h
		hops[i].Headers = copyStringMap(h.Headers)
		hops[i].ExtraHeaders = copyStringMap(h.ExtraHeaders)
	}

	return &FormReceipt{RequestID: o.requestID, Hops: hops}, nil
}

// paramsString returns the string value of a key in a raw event map.
func paramsString(m map[string]interface{}, key string) string {
	val, _ := m[key].(string)
	return val
}

// paramsInt returns the int value of a key in a raw event map.
func paramsInt(m map[string]interface{}, key string) int {
	val, _ := m[key].(float64)
	return int(val)
}

// paramsMap returns the map value of a key in a raw event map.
func paramsMap(m map[string]interface{}, key string) map[string]interface{} {
	val, _ := m[key].(map[string]interface{})
	return val
}

// paramsStringMap copies a raw headers object into a string map.
func paramsStringMap(m map[string]interface{}) map[string]string {
	if m == nil {
		return nil
	}

	out := make(map[string]string, len(m))
	for k, v := range m {
		if s, ok := v.(string); ok {
			out[k] = s
		} else {
			out[k] = fmt.Sprint(v)
		}
	}
	return out
}

// copyStringMap returns an independent copy of a string map.
func copyStringMap(m map[string]string) map[string]string {
	if m == nil {
		return nil
	}

	out := make(map[string]string, len(m))
	for k, v := range m {
		out[k] = v
	}
	return out
}

// WaitFormReceipt observes the network events of a form submission and
// returns a receipt with every hop of the request: the original POST,
// each redirect and the final response, including the wire-level status
// reported via Network.responseReceivedExtraInfo.
//
// The observer is installed first, then action is invoked exactly once to
// trigger the submission (for example via Evaluate calling
// form.requestSubmit(), or via the native input methods). The observation
// starts at the first Network.requestWillBeSent that exactly matches
// match; from then on the request id is fixed and later submissions of
// the same URL, sibling frames or unrelated requests cannot take it over.
//
// The receipt is delivered only when the final Network.loadingFinished
// for the request arrived, all declared extra info has been received and
// action returned successfully. If action fails, its error is returned
// as-is. If the request fails to load, a *FormRequestError is returned.
// If ctx is cancelled the wait ends immediately; an action already in
// flight is neither terminated nor waited for.
//
// NetworkEvents(true) must be enabled by the caller beforehand; this
// method does not switch domains or tabs.
func (remote *RemoteDebugger) WaitFormReceipt(ctx context.Context, match FormRequestMatch, action func() error) (*FormReceipt, error) {
	if match.FrameID == "" || match.URL == "" || match.Method == "" {
		return nil, ErrorFormMatchInvalid
	}

	if action == nil {
		return nil, ErrorFormActionNil
	}

	if err := ctx.Err(); err != nil {
		return nil, err
	}

	obs := &formObserver{
		match: match,
		early: map[string][]formExtraInfo{},
		done:  make(chan struct{}),
	}

	remote.addObserver(obs)
	defer func() {
		remote.removeObserver(obs)
		obs.abort()
	}()

	// the action runs in its own goroutine so that a context cancellation
	// can end the wait without waiting for (or killing) the action
	actionCh := make(chan error, 1)
	go func() {
		actionCh <- action()
	}()

	doneCh := obs.done

	for actionCh != nil || doneCh != nil {
		select {
		case err := <-actionCh:
			actionCh = nil
			if err != nil {
				return nil, err
			}

		case <-doneCh:
			doneCh = nil
			if _, err := obs.outcome(); err != nil {
				return nil, err
			}

		case <-ctx.Done():
			return nil, ctx.Err()

		case <-remote.closed:
			return nil, ErrorClose
		}
	}

	return obs.outcome()
}

// unmarshalEventParams decodes event parameters for observer dispatch.
func unmarshalEventParams(raw json.RawMessage) (Params, error) {
	var params Params
	err := json.Unmarshal(raw, &params)
	return params, err
}
