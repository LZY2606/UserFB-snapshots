package fetchbot

import (
	"io"
	"net/http"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
)

// The controlled Doer scaffold is adapted from an earlier public scaffold at the same upstream commit.
type testDoer func(*http.Request) (*http.Response, error)

func (d testDoer) Do(r *http.Request) (*http.Response, error) { return d(r) }

type testBody struct {
	io.Reader
	closed *int32
}

func (b *testBody) Close() error { atomic.AddInt32(b.closed, 1); return nil }
func TestBaseline(t *testing.T) {
	var lock sync.Mutex
	var closed int32
	got := map[string]string{}
	f := New(HandlerFunc(func(c *Context, r *http.Response, e error) {
		if e != nil {
			t.Error(e)
			return
		}
		b, e := io.ReadAll(r.Body)
		if e != nil {
			t.Error(e)
			return
		}
		lock.Lock()
		defer lock.Unlock()
		got[c.Cmd.URL().String()] = string(b)
	}))
	f.DisablePoliteness, f.CrawlDelay = true, 0
	f.HttpClient = testDoer(func(r *http.Request) (*http.Response, error) {
		return &http.Response{
			StatusCode: 200, Status: "200 OK", Proto: "HTTP/1.1", ProtoMajor: 1, ProtoMinor: 1,
			Header: http.Header{"Content-Type": []string{"application/octet-stream"}},
			Body:   &testBody{strings.NewReader("a\x00b"), &closed}, Request: r}, nil
	})
	q := f.Start()
	n, e := q.SendStringGet("https://one.invalid/a", "https://two.invalid/b")
	if e != nil || n != 2 {
		t.Fatalf("queue admit: %d %v", n, e)
	}
	if e = q.Close(); e != nil {
		t.Fatal(e)
	}
	lock.Lock()
	defer lock.Unlock()
	if len(got) != 2 || got["https://one.invalid/a"] != "a\x00b" || got["https://two.invalid/b"] != "a\x00b" || atomic.LoadInt32(&closed) != 2 {
		t.Fatalf("response delivery/ownership: %#v closed=%d", got, closed)
	}
	t.Log("FETCHBOT_CAPTURE_CHAIN_BASELINE_OK")
	t.Log("TWO_HOST_BINARY_DELIVERY_AND_SINGLE_BODY_CLOSE_OK")
}
