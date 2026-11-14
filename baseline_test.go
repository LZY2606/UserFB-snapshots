package fetchbot

import (
	"io"
	"net/http"
 "net/url"
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

func TestRetryAbsenceBaseline(t *testing.T) {
 calls, delivered, closed := 0, 0, int32(0)
 original := &Cmd{M:"POST"}
 u, err := url.Parse("https://retry.invalid/query")
 if err != nil { t.Fatal(err) }; original.U = u
 f := New(HandlerFunc(func(c *Context, r *http.Response, e error) {
  delivered++
  if e != nil || c.Cmd != original || r == nil || r.StatusCode != 503 { t.Errorf("terminal identity/status differs: %v",e) }
 }))
 f.DisablePoliteness, f.CrawlDelay = true, 0
 f.HttpClient = testDoer(func(r *http.Request) (*http.Response,error) {
  calls++
  return &http.Response{StatusCode:503,Header:http.Header{"Retry-After":[]string{"0"}},Body:&testBody{strings.NewReader("busy"),&closed},Request:r},nil
 })
 q:=f.Start();if e:=q.Send(original);e!=nil{t.Fatal(e)};if e:=q.Close();e!=nil{t.Fatal(e)}
 if calls!=1 || delivered!=1 || closed!=1 { t.Fatalf("calls=%d delivered=%d closed=%d",calls,delivered,closed) }
 t.Log("FETCHBOT_SINGLE_ATTEMPT_503_BASELINE_OK")
}
