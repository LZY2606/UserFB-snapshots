package httpu

import (
	"fmt"
	"net/http"
	"strings"
	"sync"
	"time"
)

// DelegateError is the error returned by a single delegate of a MultiClient
// or MultiClientCtx, annotated with the index of the delegate in the order
// the delegates were passed to the constructor.
type DelegateError struct {
	// Index of the delegate that produced the error.
	Index int

	// The error returned by the delegate.
	Err error
}

func (e DelegateError) Error() string {
	return fmt.Sprintf("httpu: delegate %d: %v", e.Index, e.Err)
}

func (e DelegateError) Unwrap() error {
	return e.Err
}

// multiError aggregates the errors of several delegates, in delegate order.
// Each element can be recovered with errors.Is/errors.As.
type multiError []error

func (m multiError) Error() string {
	msgs := make([]string, len(m))
	for i, err := range m {
		msgs[i] = err.Error()
	}
	return strings.Join(msgs, "; ")
}

func (m multiError) Unwrap() []error {
	return []error(m)
}

// orNil returns nil for an empty multiError, so that a fully successful fan
// out reports no error at all.
func (m multiError) orNil() error {
	if len(m) == 0 {
		return nil
	}
	return m
}

// runDelegates invokes fn once per delegate index, waits for every invocation
// to finish (a failing delegate does not cancel its siblings), and
// concatenates the responses in delegate order. Errors are aggregated in
// delegate order, each annotated with its delegate index. Responses returned
// by a delegate alongside an error are kept.
func runDelegates(n int, fn func(i int) ([]*http.Response, error)) ([]*http.Response, error) {
	if n == 0 {
		return nil, nil
	}

	results := make([][]*http.Response, n)
	errs := make([]error, n)
	var wg sync.WaitGroup
	for i := 0; i < n; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			results[i], errs[i] = fn(i)
		}(i)
	}
	wg.Wait()

	var responses []*http.Response
	var merr multiError
	for i := range results {
		responses = append(responses, results[i]...)
		if errs[i] != nil {
			merr = append(merr, DelegateError{Index: i, Err: errs[i]})
		}
	}
	return responses, merr.orNil()
}

// cloneRequest makes an independent copy of a (bodyless) discovery request
// and its Header, so that a delegate may annotate it - e.g. with local source
// information - without affecting the other delegates or the caller.
func cloneRequest(req *http.Request) *http.Request {
	return req.Clone(req.Context())
}

// MultiClient dispatches requests out to all the delegated clients.
type MultiClient struct {
	// The HTTPU clients to delegate to.
	delegates []ClientInterface
}

var _ ClientInterface = &MultiClient{}

// NewMultiClient creates a new MultiClient that delegates to all the given
// clients.
func NewMultiClient(delegates []ClientInterface) *MultiClient {
	return &MultiClient{
		delegates: delegates,
	}
}

// Do implements ClientInterface.Do.
//
// Every delegate is awaited: a failing delegate does not cancel its siblings,
// and responses returned by a delegate alongside an error are kept. Responses
// are concatenated in delegate order, and delegate errors are aggregated in
// delegate order as DelegateError values. Each delegate receives its own copy
// of the request and its Header. Only bodyless discovery requests are
// supported.
func (mc *MultiClient) Do(
	req *http.Request,
	timeout time.Duration,
	numSends int,
) ([]*http.Response, error) {
	return runDelegates(len(mc.delegates), func(i int) ([]*http.Response, error) {
		return mc.delegates[i].Do(cloneRequest(req), timeout, numSends)
	})
}

// MultiClientCtx dispatches requests out to all the delegated clients.
type MultiClientCtx struct {
	// The HTTPU clients to delegate to.
	delegates []ClientInterfaceCtx
}

var _ ClientInterfaceCtx = &MultiClientCtx{}

// NewMultiClient creates a new MultiClient that delegates to all the given
// clients.
func NewMultiClientCtx(delegates []ClientInterfaceCtx) *MultiClientCtx {
	return &MultiClientCtx{
		delegates: delegates,
	}
}

// DoWithContext implements ClientInterfaceCtx.DoWithContext.
//
// Every delegate is awaited: a failing delegate does not cancel its siblings,
// only cancellation of the request's context ends the whole collaboration.
// Responses returned by a delegate alongside an error are kept. Responses are
// concatenated in delegate order, and delegate errors are aggregated in
// delegate order as DelegateError values. Each delegate receives its own copy
// of the request and its Header. Only bodyless discovery requests are
// supported.
func (mc *MultiClientCtx) DoWithContext(
	req *http.Request,
	numSends int,
) ([]*http.Response, error) {
	return runDelegates(len(mc.delegates), func(i int) ([]*http.Response, error) {
		return mc.delegates[i].DoWithContext(cloneRequest(req), numSends)
	})
}
