package httpu

import (
	"context"
	"errors"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"
)

// gatedDelegate blocks until its gate is closed (or the request context is
// canceled), then returns its canned responses and error. It records the
// request it was called with.
type gatedDelegate struct {
	gate      chan struct{}
	responses []*http.Response
	err       error

	mu  sync.Mutex
	req *http.Request
}

func (d *gatedDelegate) Do(req *http.Request, _ time.Duration, _ int) ([]*http.Response, error) {
	<-d.gate
	d.mu.Lock()
	d.req = req
	d.mu.Unlock()
	return d.responses, d.err
}

func (d *gatedDelegate) DoWithContext(req *http.Request, _ int) ([]*http.Response, error) {
	select {
	case <-d.gate:
	case <-req.Context().Done():
		d.mu.Lock()
		d.req = req
		d.mu.Unlock()
		return d.responses, req.Context().Err()
	}
	d.mu.Lock()
	d.req = req
	d.mu.Unlock()
	return d.responses, d.err
}

func (d *gatedDelegate) request() *http.Request {
	d.mu.Lock()
	defer d.mu.Unlock()
	return d.req
}

// delegateFunc adapts a function to both client interfaces.
type delegateFunc func(req *http.Request) ([]*http.Response, error)

func (f delegateFunc) Do(req *http.Request, _ time.Duration, _ int) ([]*http.Response, error) {
	return f(req)
}

func (f delegateFunc) DoWithContext(req *http.Request, _ int) ([]*http.Response, error) {
	return f(req)
}

func closedGate() chan struct{} {
	c := make(chan struct{})
	close(c)
	return c
}

func partialTestResponse(id string) *http.Response {
	return &http.Response{
		Status:     "200 OK",
		StatusCode: 200,
		Header:     http.Header{"X-Id": []string{id}},
	}
}

func responseIDs(responses []*http.Response) []string {
	ids := make([]string, len(responses))
	for i, r := range responses {
		ids[i] = r.Header.Get("X-Id")
	}
	return ids
}

// TestCollectorPartialDiscoveryMultiClientOrdering checks that a delegate
// failing first does not cancel its siblings, and that responses and errors
// are assembled in delegate order regardless of completion order. Both
// directions ("later NIC fails first" and "earlier NIC fails first") are
// exercised for MultiClient and MultiClientCtx.
func TestCollectorPartialDiscoveryMultiClientOrdering(t *testing.T) {
	errNIC := errors.New("nic down")

	run := func(t *testing.T, failIdx int, useCtx bool) {
		gates := []chan struct{}{make(chan struct{}), make(chan struct{})}
		responses := [][]*http.Response{
			{partialTestResponse("a1"), partialTestResponse("a2")},
			{partialTestResponse("b1"), partialTestResponse("b2")},
		}
		delegates := []*gatedDelegate{
			{gate: gates[0], responses: responses[0]},
			{gate: gates[1], responses: responses[1]},
		}
		delegates[failIdx].responses = nil
		delegates[failIdx].err = errNIC

		req := &http.Request{Method: "M-SEARCH", Header: http.Header{}}
		type outcome struct {
			responses []*http.Response
			err       error
		}
		done := make(chan outcome, 1)
		if useCtx {
			mc := NewMultiClientCtx([]ClientInterfaceCtx{delegates[0], delegates[1]})
			go func() {
				rs, err := mc.DoWithContext(req, 1)
				done <- outcome{rs, err}
			}()
		} else {
			mc := NewMultiClient([]ClientInterface{delegates[0], delegates[1]})
			go func() {
				rs, err := mc.Do(req, time.Second, 1)
				done <- outcome{rs, err}
			}()
		}

		// The failing delegate finishes first; the call must still wait for
		// its sibling.
		close(gates[failIdx])
		select {
		case <-done:
			t.Fatal("Do returned before all delegates finished")
		case <-time.After(50 * time.Millisecond):
		}
		close(gates[1-failIdx])
		var out outcome
		select {
		case out = <-done:
		case <-time.After(time.Second):
			t.Fatal("Do did not return after all delegates finished")
		}

		// Responses stay in delegate order, keeping each delegate's internal
		// order, regardless of which delegate completed first.
		want := responseIDs(responses[1-failIdx])
		got := responseIDs(out.responses)
		if len(got) != len(want) || got[0] != want[0] || got[1] != want[1] {
			t.Fatalf("responses = %v, want %v", got, want)
		}
		if !errors.Is(out.err, errNIC) {
			t.Fatalf("error %v does not unwrap to %v", out.err, errNIC)
		}
		var derr DelegateError
		if !errors.As(out.err, &derr) || derr.Index != failIdx {
			t.Fatalf("error %v does not identify delegate %d", out.err, failIdx)
		}
		if !strings.Contains(out.err.Error(), strconv.Itoa(failIdx)) {
			t.Fatalf("error %q does not mention delegate index %d", out.err, failIdx)
		}
	}

	for _, failIdx := range []int{0, 1} {
		for _, useCtx := range []bool{false, true} {
			name := "MultiClient"
			if useCtx {
				name = "MultiClientCtx"
			}
			if failIdx == 0 {
				name += "/EarlierNICFailsFirst"
			} else {
				name += "/LaterNICFailsFirst"
			}
			failIdx, useCtx := failIdx, useCtx
			t.Run(name, func(t *testing.T) { run(t, failIdx, useCtx) })
		}
	}
}

// TestCollectorPartialDiscoverySingleDelegatePartial checks that responses
// returned by a delegate together with an error are kept, and that a client
// with no delegates returns an empty result and no error.
func TestCollectorPartialDiscoverySingleDelegatePartial(t *testing.T) {
	errPartial := errors.New("send failed after replies")
	resp := partialTestResponse("only")

	t.Run("MultiClient", func(t *testing.T) {
		d := &gatedDelegate{gate: closedGate(), responses: []*http.Response{resp}, err: errPartial}
		mc := NewMultiClient([]ClientInterface{d})
		responses, err := mc.Do(&http.Request{Header: http.Header{}}, time.Second, 1)
		if len(responses) != 1 || responses[0] != resp {
			t.Fatalf("responses = %v, want the partial response kept", responseIDs(responses))
		}
		if !errors.Is(err, errPartial) {
			t.Fatalf("error %v does not unwrap to %v", err, errPartial)
		}
	})

	t.Run("MultiClientCtx", func(t *testing.T) {
		d := &gatedDelegate{gate: closedGate(), responses: []*http.Response{resp}, err: errPartial}
		mc := NewMultiClientCtx([]ClientInterfaceCtx{d})
		responses, err := mc.DoWithContext(&http.Request{Header: http.Header{}}, 1)
		if len(responses) != 1 || responses[0] != resp {
			t.Fatalf("responses = %v, want the partial response kept", responseIDs(responses))
		}
		if !errors.Is(err, errPartial) {
			t.Fatalf("error %v does not unwrap to %v", err, errPartial)
		}
	})

	t.Run("NoDelegates", func(t *testing.T) {
		responses, err := NewMultiClient(nil).Do(&http.Request{Header: http.Header{}}, time.Second, 1)
		if len(responses) != 0 || err != nil {
			t.Fatalf("MultiClient with no delegates = %v, %v; want empty, nil", responses, err)
		}
		responses, err = NewMultiClientCtx(nil).DoWithContext(&http.Request{Header: http.Header{}}, 1)
		if len(responses) != 0 || err != nil {
			t.Fatalf("MultiClientCtx with no delegates = %v, %v; want empty, nil", responses, err)
		}
	})
}

// TestCollectorPartialDiscoveryRequestIsolation checks that each delegate
// receives an independent copy of the request and its Header: annotations
// made by one delegate are invisible to the others and to the caller.
func TestCollectorPartialDiscoveryRequestIsolation(t *testing.T) {
	orig := &http.Request{
		Method: "M-SEARCH",
		Header: http.Header{"ST": []string{"ssdp:all"}},
	}
	firstDone := make(chan struct{})
	var firstReq, secondReq *http.Request
	var secondSawLocal string
	first := delegateFunc(func(req *http.Request) ([]*http.Response, error) {
		firstReq = req
		req.Header.Set(LocalAddressHeader, "192.0.2.10")
		close(firstDone)
		return nil, nil
	})
	second := delegateFunc(func(req *http.Request) ([]*http.Response, error) {
		<-firstDone
		secondReq = req
		secondSawLocal = req.Header.Get(LocalAddressHeader)
		req.Header.Set(LocalAddressHeader, "192.0.2.11")
		return nil, nil
	})

	mc := NewMultiClientCtx([]ClientInterfaceCtx{first, second})
	if _, err := mc.DoWithContext(orig, 1); err != nil {
		t.Fatal(err)
	}

	if firstReq == orig || secondReq == orig || firstReq == secondReq {
		t.Fatal("delegates did not receive independent request copies")
	}
	if secondSawLocal != "" {
		t.Fatalf("second delegate saw first delegate's header annotation %q", secondSawLocal)
	}
	if got := orig.Header.Get(LocalAddressHeader); got != "" {
		t.Fatalf("caller's request was annotated with %q", got)
	}
	if got := orig.Header["ST"]; len(got) != 1 || got[0] != "ssdp:all" {
		t.Fatalf("caller's request header changed: ST = %v", got)
	}
}

// TestCollectorPartialDiscoveryParentCancel checks that cancellation of the
// parent context ends the collaboration for all delegates.
func TestCollectorPartialDiscoveryParentCancel(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	started := make(chan struct{}, 2)
	d := delegateFunc(func(req *http.Request) ([]*http.Response, error) {
		started <- struct{}{}
		<-req.Context().Done()
		return nil, req.Context().Err()
	})

	mc := NewMultiClientCtx([]ClientInterfaceCtx{d, d})
	req := (&http.Request{Method: "M-SEARCH", Header: http.Header{}}).WithContext(ctx)
	done := make(chan error, 1)
	go func() {
		_, err := mc.DoWithContext(req, 1)
		done <- err
	}()

	<-started
	<-started
	cancel()
	select {
	case err := <-done:
		if !errors.Is(err, context.Canceled) {
			t.Fatalf("error %v does not unwrap to context.Canceled", err)
		}
	case <-time.After(time.Second):
		t.Fatal("DoWithContext did not return after parent cancellation")
	}
}
