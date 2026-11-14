package fetchbot

import (
	"bytes"
	"encoding/base64"
	"errors"
	"fmt"
	"io"
	"io/ioutil"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/PuerkitoBio/fetchbot/task_support/taskcheck"
)

// --- shared scaffolding for the retry transaction tests ---------------------

// rtTrackingReader is a Reader/Closer that counts reads and closes and can
// fail after its data is exhausted.
type rtTrackingReader struct {
	data   []byte
	off    int
	err    error
	reads  *int32
	closes *int32
}

func (r *rtTrackingReader) Read(p []byte) (int, error) {
	if r.reads != nil {
		atomic.AddInt32(r.reads, 1)
	}
	if r.off < len(r.data) {
		n := copy(p, r.data[r.off:])
		r.off += n
		return n, nil
	}
	if r.err != nil {
		return 0, r.err
	}
	return 0, io.EOF
}

func (r *rtTrackingReader) Close() error {
	if r.closes != nil {
		atomic.AddInt32(r.closes, 1)
	}
	return nil
}

// rtCounts records how many times each provider of rtFullCmd was called.
type rtCounts struct {
	retryable, header, auth, cookies, reader, values int32
}

// rtFullCmd implements every recognized provider interface plus
// RetryableCommand, counting every call.
type rtFullCmd struct {
	*Cmd
	cnt   *rtCounts
	allow bool
	hdr   http.Header
	cks   []*http.Cookie
	rdr   *rtTrackingReader
	vals  url.Values
}

func (c *rtFullCmd) Retryable() bool { atomic.AddInt32(&c.cnt.retryable, 1); return c.allow }
func (c *rtFullCmd) Header() http.Header {
	atomic.AddInt32(&c.cnt.header, 1)
	return c.hdr
}
func (c *rtFullCmd) BasicAuth() (string, string) {
	atomic.AddInt32(&c.cnt.auth, 1)
	return "rt-user", "rt-pass"
}
func (c *rtFullCmd) Cookies() []*http.Cookie {
	atomic.AddInt32(&c.cnt.cookies, 1)
	return c.cks
}
func (c *rtFullCmd) Reader() io.Reader {
	atomic.AddInt32(&c.cnt.reader, 1)
	return c.rdr
}
func (c *rtFullCmd) Values() url.Values {
	atomic.AddInt32(&c.cnt.values, 1)
	return c.vals
}

// rtValuesCmd provides only form values and opts in to retries.
type rtValuesCmd struct {
	*Cmd
	calls *int32
	vals  url.Values
}

func (c *rtValuesCmd) Retryable() bool    { return true }
func (c *rtValuesCmd) Values() url.Values { atomic.AddInt32(c.calls, 1); return c.vals }

// rtHandlerCmd carries its own Handler and an opt-in flag for retries.
type rtHandlerCmd struct {
	*Cmd
	allow  bool
	handle func(*Context, *http.Response, error)
}

func (c *rtHandlerCmd) Retryable() bool { return c.allow }
func (c *rtHandlerCmd) Handle(ctx *Context, res *http.Response, err error) {
	if c.handle != nil {
		c.handle(ctx, res, err)
	}
}

// rtPlainRetryCmd only opts in to retries.
type rtPlainRetryCmd struct {
	*Cmd
	allow bool
}

func (c *rtPlainRetryCmd) Retryable() bool { return c.allow }

// rtResponse builds an in-memory response for the controlled Doer, with a
// body that counts closes.
func rtResponse(req *http.Request, status int, retryAfter, body string, closed *int32) *http.Response {
	h := http.Header{}
	if retryAfter != "" {
		h.Set("Retry-After", retryAfter)
	}
	return &http.Response{
		StatusCode: status,
		Status:     fmt.Sprintf("%d status", status),
		Proto:      "HTTP/1.1", ProtoMajor: 1, ProtoMinor: 1,
		Header:  h,
		Body:    &testBody{strings.NewReader(body), closed},
		Request: req,
	}
}

func rtURL(raw string) *url.URL {
	u, err := url.Parse(raw)
	if err != nil {
		panic(err)
	}
	return u
}

// --- TestRetrySnapshot ------------------------------------------------------

func TestRetrySnapshot(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Part 1: providers with side effects are called exactly once, the
		// binary body is replayed byte-for-byte on every attempt.
		var closed, handlerCalls, handlerStatus int32
		var handlerCmd Command
		cnt := &rtCounts{}
		var reads, closes int32
		payload := []byte{'a', 0x00, 'b', 0xff, 'c', 0x01}
		cmd := &rtFullCmd{
			Cmd:   &Cmd{U: rtURL("https://snap.invalid/post"), M: "POST"},
			cnt:   cnt,
			allow: true,
			hdr: http.Header{
				"X-Custom":   []string{"one", "two"},
				"User-Agent": []string{"snap-agent"},
			},
			cks:  []*http.Cookie{{Name: "sid", Value: "abc"}},
			rdr:  &rtTrackingReader{data: payload, reads: &reads, closes: &closes},
			vals: url.Values{"ignored": []string{"yes"}},
		}
		var mu sync.Mutex
		var bodies [][]byte
		var headers []http.Header
		f := New(HandlerFunc(func(ctx *Context, res *http.Response, err error) {
			atomic.AddInt32(&handlerCalls, 1)
			if err == nil {
				atomic.StoreInt32(&handlerStatus, int32(res.StatusCode))
			}
			handlerCmd = ctx.Cmd
		}))
		f.DisablePoliteness, f.CrawlDelay = true, 0
		f.RetryMax, f.RetryDelay, f.RetryMaxDelay = 2, 5*time.Millisecond, 50*time.Millisecond
		f.HttpClient = testDoer(func(r *http.Request) (*http.Response, error) {
			b, _ := ioutil.ReadAll(r.Body)
			mu.Lock()
			bodies = append(bodies, b)
			headers = append(headers, r.Header.Clone())
			n := len(bodies)
			mu.Unlock()
			if n == 1 {
				// Tamper with the command's header map: the snapshot taken
				// before the first attempt must be isolated from it.
				cmd.hdr.Set("X-Custom", "tampered")
				return rtResponse(r, 503, "", "busy", &closed), nil
			}
			return rtResponse(r, 200, "", "ok", &closed), nil
		})
		q := f.Start()
		if err := q.Send(cmd); err != nil {
			t.Fatal(err)
		}
		if err := q.Close(); err != nil {
			t.Fatal(err)
		}
		mu.Lock()
		defer mu.Unlock()
		wantAuth := "Basic " + base64.StdEncoding.EncodeToString([]byte("rt-user:rt-pass"))
		c.Check(len(bodies) == 2, "snapshot: expected 2 attempts, got %d", len(bodies))
		for i := range bodies {
			c.Check(bytes.Equal(bodies[i], payload), "snapshot: attempt %d body %v != %v", i, bodies[i], payload)
			c.Check(strings.Join(headers[i]["X-Custom"], ",") == "one,two",
				"snapshot: attempt %d X-Custom %v", i, headers[i]["X-Custom"])
			c.Check(headers[i].Get("Authorization") == wantAuth,
				"snapshot: attempt %d Authorization %q", i, headers[i].Get("Authorization"))
			c.Check(headers[i].Get("Cookie") == "sid=abc",
				"snapshot: attempt %d Cookie %q", i, headers[i].Get("Cookie"))
			c.Check(headers[i].Get("User-Agent") == "snap-agent",
				"snapshot: attempt %d User-Agent %q", i, headers[i].Get("User-Agent"))
		}
		c.Check(atomic.LoadInt32(&cnt.retryable) == 1, "snapshot: Retryable called %d times", cnt.retryable)
		c.Check(atomic.LoadInt32(&cnt.header) == 1, "snapshot: Header called %d times", cnt.header)
		c.Check(atomic.LoadInt32(&cnt.auth) == 1, "snapshot: BasicAuth called %d times", cnt.auth)
		c.Check(atomic.LoadInt32(&cnt.cookies) == 1, "snapshot: Cookies called %d times", cnt.cookies)
		c.Check(atomic.LoadInt32(&cnt.reader) == 1, "snapshot: Reader called %d times", cnt.reader)
		c.Check(atomic.LoadInt32(&cnt.values) == 0, "snapshot: Values called %d times (Reader has priority)", cnt.values)
		c.Check(atomic.LoadInt32(&closes) == 1, "snapshot: snapshot reader closed %d times", closes)
		c.Check(atomic.LoadInt32(&handlerCalls) == 1, "snapshot: handler called %d times", handlerCalls)
		c.Check(atomic.LoadInt32(&handlerStatus) == 200, "snapshot: final status %d", handlerStatus)
		c.Check(handlerCmd == cmd, "snapshot: Context.Cmd identity lost: %T %p", handlerCmd, handlerCmd)
		c.Check(atomic.LoadInt32(&closed) == 2, "snapshot: response bodies closed %d times", closed)

		// Part 2: a ValuesProvider body is encoded exactly once and replayed.
		var closed2, delivered2, valuesCalls int32
		vals := url.Values{"q": []string{"a b"}, "n": []string{"1"}}
		vcmd := &rtValuesCmd{Cmd: &Cmd{U: rtURL("https://snap2.invalid/post"), M: "POST"}, calls: &valuesCalls, vals: vals}
		var mu2 sync.Mutex
		var bodies2 []string
		var ctypes []string
		f2 := New(HandlerFunc(func(ctx *Context, res *http.Response, err error) {
			atomic.AddInt32(&delivered2, 1)
		}))
		f2.DisablePoliteness, f2.CrawlDelay = true, 0
		f2.RetryMax, f2.RetryDelay, f2.RetryMaxDelay = 1, time.Millisecond, 20*time.Millisecond
		f2.HttpClient = testDoer(func(r *http.Request) (*http.Response, error) {
			b, _ := ioutil.ReadAll(r.Body)
			mu2.Lock()
			bodies2 = append(bodies2, string(b))
			ctypes = append(ctypes, r.Header.Get("Content-Type"))
			n := len(bodies2)
			mu2.Unlock()
			if n == 1 {
				return rtResponse(r, 429, "", "slow", &closed2), nil
			}
			return rtResponse(r, 200, "", "ok", &closed2), nil
		})
		q2 := f2.Start()
		if err := q2.Send(vcmd); err != nil {
			t.Fatal(err)
		}
		if err := q2.Close(); err != nil {
			t.Fatal(err)
		}
		mu2.Lock()
		defer mu2.Unlock()
		encoded := vals.Encode()
		c.Check(len(bodies2) == 2, "snapshot: values command attempts %d", len(bodies2))
		for i, b := range bodies2 {
			c.Check(b == encoded, "snapshot: values attempt %d body %q != %q", i, b, encoded)
			c.Check(ctypes[i] == "application/x-www-form-urlencoded", "snapshot: values attempt %d Content-Type %q", i, ctypes[i])
		}
		c.Check(atomic.LoadInt32(&valuesCalls) == 1, "snapshot: Values called %d times", valuesCalls)
		c.Check(atomic.LoadInt32(&delivered2) == 1, "snapshot: values handler called %d times", delivered2)

		// Part 3: a body over RetryBodyLimit fails with ErrRetryBodyTooLarge
		// and not even a prefix is sent.
		var doCalls3, delivered3, closes3, dummy3 int32
		var err3 error
		cnt3 := &rtCounts{}
		cmd3 := &rtFullCmd{
			Cmd:   &Cmd{U: rtURL("https://snap3.invalid/post"), M: "POST"},
			cnt:   cnt3,
			allow: true,
			rdr:   &rtTrackingReader{data: []byte("0123456789abcdef"), closes: &closes3},
		}
		f3 := New(HandlerFunc(func(ctx *Context, res *http.Response, err error) {
			atomic.AddInt32(&delivered3, 1)
			err3 = err
		}))
		f3.DisablePoliteness, f3.CrawlDelay = true, 0
		f3.RetryMax, f3.RetryDelay, f3.RetryMaxDelay = 2, time.Millisecond, 20*time.Millisecond
		f3.RetryBodyLimit = 8
		f3.HttpClient = testDoer(func(r *http.Request) (*http.Response, error) {
			atomic.AddInt32(&doCalls3, 1)
			return rtResponse(r, 200, "", "ok", &dummy3), nil
		})
		q3 := f3.Start()
		if err := q3.Send(cmd3); err != nil {
			t.Fatal(err)
		}
		if err := q3.Close(); err != nil {
			t.Fatal(err)
		}
		c.Check(atomic.LoadInt32(&doCalls3) == 0, "snapshot: oversized body sent %d requests", doCalls3)
		c.Check(errors.Is(err3, ErrRetryBodyTooLarge), "snapshot: oversized body error %v", err3)
		c.Check(atomic.LoadInt32(&delivered3) == 1, "snapshot: oversized handler called %d times", delivered3)
		c.Check(atomic.LoadInt32(&closes3) == 1, "snapshot: oversized reader closed %d times", closes3)

		// Part 4: a body read error is delivered as-is, nothing is sent.
		errRead := errors.New("rt: read boom")
		var doCalls4, delivered4, closes4, dummy4 int32
		var err4 error
		cnt4 := &rtCounts{}
		cmd4 := &rtFullCmd{
			Cmd:   &Cmd{U: rtURL("https://snap4.invalid/post"), M: "POST"},
			cnt:   cnt4,
			allow: true,
			rdr:   &rtTrackingReader{data: []byte("partial"), err: errRead, closes: &closes4},
		}
		f4 := New(HandlerFunc(func(ctx *Context, res *http.Response, err error) {
			atomic.AddInt32(&delivered4, 1)
			err4 = err
		}))
		f4.DisablePoliteness, f4.CrawlDelay = true, 0
		f4.RetryMax, f4.RetryDelay, f4.RetryMaxDelay = 2, time.Millisecond, 20*time.Millisecond
		f4.HttpClient = testDoer(func(r *http.Request) (*http.Response, error) {
			atomic.AddInt32(&doCalls4, 1)
			return rtResponse(r, 200, "", "ok", &dummy4), nil
		})
		q4 := f4.Start()
		if err := q4.Send(cmd4); err != nil {
			t.Fatal(err)
		}
		if err := q4.Close(); err != nil {
			t.Fatal(err)
		}
		c.Check(atomic.LoadInt32(&doCalls4) == 0, "snapshot: read-error body sent %d requests", doCalls4)
		c.Check(err4 == errRead, "snapshot: read error delivered as %v", err4)
		c.Check(atomic.LoadInt32(&delivered4) == 1, "snapshot: read-error handler called %d times", delivered4)
		c.Check(atomic.LoadInt32(&closes4) == 1, "snapshot: read-error reader closed %d times", closes4)

		// Part 5: provider side effects never happen before the robots.txt
		// admission.
		var closed5, doCalls5, delivered5, closes5, reads5 int32
		var err5 error
		cnt5 := &rtCounts{}
		cmd5 := &rtFullCmd{
			Cmd:   &Cmd{U: rtURL("https://snap5.invalid/private/x"), M: "GET"},
			cnt:   cnt5,
			allow: true,
			rdr:   &rtTrackingReader{data: []byte("x"), reads: &reads5, closes: &closes5},
		}
		f5 := New(HandlerFunc(func(ctx *Context, res *http.Response, err error) {
			atomic.AddInt32(&delivered5, 1)
			err5 = err
		}))
		f5.CrawlDelay = 0
		f5.RetryMax, f5.RetryDelay, f5.RetryMaxDelay = 2, time.Millisecond, 20*time.Millisecond
		f5.HttpClient = testDoer(func(r *http.Request) (*http.Response, error) {
			atomic.AddInt32(&doCalls5, 1)
			if r.URL.Path == "/robots.txt" {
				return rtResponse(r, 200, "", "User-agent: *\nDisallow: /private\n", &closed5), nil
			}
			return rtResponse(r, 200, "", "ok", &closed5), nil
		})
		q5 := f5.Start()
		if err := q5.Send(cmd5); err != nil {
			t.Fatal(err)
		}
		if err := q5.Close(); err != nil {
			t.Fatal(err)
		}
		c.Check(errors.Is(err5, ErrDisallowed), "snapshot: disallowed error %v", err5)
		c.Check(atomic.LoadInt32(&doCalls5) == 1, "snapshot: disallowed command sent %d requests", doCalls5)
		c.Check(atomic.LoadInt32(&delivered5) == 1, "snapshot: disallowed handler called %d times", delivered5)
		c.Check(atomic.LoadInt32(&cnt5.retryable) == 0, "snapshot: Retryable called before admission %d times", cnt5.retryable)
		c.Check(atomic.LoadInt32(&cnt5.header) == 0, "snapshot: Header called before admission %d times", cnt5.header)
		c.Check(atomic.LoadInt32(&cnt5.auth) == 0, "snapshot: BasicAuth called before admission %d times", cnt5.auth)
		c.Check(atomic.LoadInt32(&cnt5.cookies) == 0, "snapshot: Cookies called before admission %d times", cnt5.cookies)
		c.Check(atomic.LoadInt32(&cnt5.reader) == 0, "snapshot: Reader called before admission %d times", cnt5.reader)
		c.Check(atomic.LoadInt32(&cnt5.values) == 0, "snapshot: Values called before admission %d times", cnt5.values)
	})
}

// --- TestRetryDelivery ------------------------------------------------------

func TestRetryDelivery(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Part 1: the command's own Handler takes precedence, intermediate
		// bodies are closed once without reaching any Handler, and the final
		// body is owned by the terminal callback path.
		var c1, c2, c3 int32
		var doCalls, cmdHandlerCalls, fetcherHandlerCalls, gotStatus int32
		var gotBody string
		var gotCmd Command
		hcmd := &rtHandlerCmd{
			Cmd:   &Cmd{U: rtURL("https://dlv.invalid/x"), M: "GET"},
			allow: true,
			handle: func(ctx *Context, res *http.Response, err error) {
				atomic.AddInt32(&cmdHandlerCalls, 1)
				if err == nil {
					b, _ := ioutil.ReadAll(res.Body)
					gotBody = string(b)
					atomic.StoreInt32(&gotStatus, int32(res.StatusCode))
				}
				gotCmd = ctx.Cmd
			},
		}
		f := New(HandlerFunc(func(ctx *Context, res *http.Response, err error) {
			atomic.AddInt32(&fetcherHandlerCalls, 1)
		}))
		f.DisablePoliteness, f.CrawlDelay = true, 0
		f.RetryMax, f.RetryDelay, f.RetryMaxDelay = 3, time.Millisecond, 20*time.Millisecond
		f.HttpClient = testDoer(func(r *http.Request) (*http.Response, error) {
			switch atomic.AddInt32(&doCalls, 1) {
			case 1:
				return rtResponse(r, 503, "", "intermediate-1", &c1), nil
			case 2:
				return rtResponse(r, 429, "", "intermediate-2", &c2), nil
			default:
				return rtResponse(r, 200, "", "final", &c3), nil
			}
		})
		q := f.Start()
		if err := q.Send(hcmd); err != nil {
			t.Fatal(err)
		}
		if err := q.Close(); err != nil {
			t.Fatal(err)
		}
		c.Check(atomic.LoadInt32(&doCalls) == 3, "delivery: attempts %d", doCalls)
		c.Check(atomic.LoadInt32(&cmdHandlerCalls) == 1, "delivery: command handler called %d times", cmdHandlerCalls)
		c.Check(atomic.LoadInt32(&fetcherHandlerCalls) == 0, "delivery: fetcher handler called %d times", fetcherHandlerCalls)
		c.Check(gotBody == "final", "delivery: handler saw body %q", gotBody)
		c.Check(atomic.LoadInt32(&gotStatus) == 200, "delivery: handler saw status %d", gotStatus)
		c.Check(gotCmd == hcmd, "delivery: Context.Cmd identity lost: %T", gotCmd)
		c.Check(atomic.LoadInt32(&c1) == 1, "delivery: intermediate body 1 closed %d times", c1)
		c.Check(atomic.LoadInt32(&c2) == 1, "delivery: intermediate body 2 closed %d times", c2)
		c.Check(atomic.LoadInt32(&c3) == 1, "delivery: final body closed %d times", c3)

		// Part 2: attempts exhausted, the last real response is delivered.
		var doCalls2, delivered2, closed2, gotStatus2 int32
		var gotBody2 string
		f2 := New(HandlerFunc(func(ctx *Context, res *http.Response, err error) {
			atomic.AddInt32(&delivered2, 1)
			if err == nil {
				b, _ := ioutil.ReadAll(res.Body)
				gotBody2 = string(b)
				atomic.StoreInt32(&gotStatus2, int32(res.StatusCode))
			}
		}))
		f2.DisablePoliteness, f2.CrawlDelay = true, 0
		f2.RetryMax, f2.RetryDelay, f2.RetryMaxDelay = 1, time.Millisecond, 20*time.Millisecond
		f2.HttpClient = testDoer(func(r *http.Request) (*http.Response, error) {
			n := atomic.AddInt32(&doCalls2, 1)
			return rtResponse(r, 503, "", fmt.Sprintf("busy-%d", n), &closed2), nil
		})
		q2 := f2.Start()
		if err := q2.Send(&rtPlainRetryCmd{Cmd: &Cmd{U: rtURL("https://dlv2.invalid/y"), M: "GET"}, allow: true}); err != nil {
			t.Fatal(err)
		}
		if err := q2.Close(); err != nil {
			t.Fatal(err)
		}
		c.Check(atomic.LoadInt32(&doCalls2) == 2, "delivery: exhausted attempts %d", doCalls2)
		c.Check(atomic.LoadInt32(&delivered2) == 1, "delivery: exhausted handler calls %d", delivered2)
		c.Check(atomic.LoadInt32(&gotStatus2) == 503, "delivery: exhausted final status %d", gotStatus2)
		c.Check(gotBody2 == "busy-2", "delivery: exhausted final body %q", gotBody2)
		c.Check(atomic.LoadInt32(&closed2) == 2, "delivery: exhausted bodies closed %d times", closed2)

		// Part 3: transport errors are delivered directly, never retried.
		errTransport := errors.New("rt: connection reset")
		var doCalls3, delivered3 int32
		var err3 error
		var res3 *http.Response
		f3 := New(HandlerFunc(func(ctx *Context, res *http.Response, err error) {
			atomic.AddInt32(&delivered3, 1)
			err3, res3 = err, res
		}))
		f3.DisablePoliteness, f3.CrawlDelay = true, 0
		f3.RetryMax, f3.RetryDelay, f3.RetryMaxDelay = 3, time.Millisecond, 20*time.Millisecond
		f3.HttpClient = testDoer(func(r *http.Request) (*http.Response, error) {
			atomic.AddInt32(&doCalls3, 1)
			return nil, errTransport
		})
		q3 := f3.Start()
		if err := q3.Send(&rtPlainRetryCmd{Cmd: &Cmd{U: rtURL("https://dlv3.invalid/z"), M: "GET"}, allow: true}); err != nil {
			t.Fatal(err)
		}
		if err := q3.Close(); err != nil {
			t.Fatal(err)
		}
		c.Check(atomic.LoadInt32(&doCalls3) == 1, "delivery: transport error attempts %d", doCalls3)
		c.Check(atomic.LoadInt32(&delivered3) == 1, "delivery: transport handler calls %d", delivered3)
		c.Check(err3 == errTransport && res3 == nil, "delivery: transport delivered %v / %v", err3, res3)

		// Part 4: commands that do not opt in are sent exactly once, even
		// with retries configured.
		var delivered4, closed4 int32
		var mu4 sync.Mutex
		perURL := map[string]int{}
		f4 := New(HandlerFunc(func(ctx *Context, res *http.Response, err error) {
			atomic.AddInt32(&delivered4, 1)
		}))
		f4.DisablePoliteness, f4.CrawlDelay = true, 0
		f4.RetryMax, f4.RetryDelay, f4.RetryMaxDelay = 3, time.Millisecond, 20*time.Millisecond
		f4.HttpClient = testDoer(func(r *http.Request) (*http.Response, error) {
			mu4.Lock()
			perURL[r.URL.String()]++
			mu4.Unlock()
			return rtResponse(r, 503, "", "busy", &closed4), nil
		})
		q4 := f4.Start()
		if err := q4.Send(&rtPlainRetryCmd{Cmd: &Cmd{U: rtURL("https://dlv4.invalid/no"), M: "GET"}, allow: false}); err != nil {
			t.Fatal(err)
		}
		if err := q4.Send(&Cmd{U: rtURL("https://dlv5.invalid/plain"), M: "GET"}); err != nil {
			t.Fatal(err)
		}
		if err := q4.Close(); err != nil {
			t.Fatal(err)
		}
		mu4.Lock()
		c.Check(perURL["https://dlv4.invalid/no"] == 1, "delivery: opt-out command attempts %d", perURL["https://dlv4.invalid/no"])
		c.Check(perURL["https://dlv5.invalid/plain"] == 1, "delivery: plain command attempts %d", perURL["https://dlv5.invalid/plain"])
		mu4.Unlock()
		c.Check(atomic.LoadInt32(&delivered4) == 2, "delivery: opt-out deliveries %d", delivered4)
		c.Check(atomic.LoadInt32(&closed4) == 2, "delivery: opt-out bodies closed %d", closed4)

		// Part 5: after a successful retry, the following commands on the
		// same host are not lost.
		var doCalls5, delivered5, closed5 int32
		var mu5 sync.Mutex
		var got5 []string
		f5 := New(HandlerFunc(func(ctx *Context, res *http.Response, err error) {
			atomic.AddInt32(&delivered5, 1)
			mu5.Lock()
			got5 = append(got5, ctx.Cmd.URL().String())
			mu5.Unlock()
		}))
		f5.DisablePoliteness, f5.CrawlDelay = true, 0
		f5.RetryMax, f5.RetryDelay, f5.RetryMaxDelay = 2, time.Millisecond, 20*time.Millisecond
		f5.HttpClient = testDoer(func(r *http.Request) (*http.Response, error) {
			n := atomic.AddInt32(&doCalls5, 1)
			if n == 1 {
				return rtResponse(r, 503, "", "busy", &closed5), nil
			}
			return rtResponse(r, 200, "", "ok", &closed5), nil
		})
		q5 := f5.Start()
		if err := q5.Send(&rtPlainRetryCmd{Cmd: &Cmd{U: rtURL("https://dlv6.invalid/first"), M: "GET"}, allow: true}); err != nil {
			t.Fatal(err)
		}
		if err := q5.Send(&Cmd{U: rtURL("https://dlv6.invalid/second"), M: "GET"}); err != nil {
			t.Fatal(err)
		}
		if err := q5.Close(); err != nil {
			t.Fatal(err)
		}
		mu5.Lock()
		c.Check(len(got5) == 2, "delivery: deliveries after retry %d (%v)", len(got5), got5)
		c.Check(atomic.LoadInt32(&doCalls5) == 3, "delivery: attempts after retry %d", doCalls5)
		mu5.Unlock()
	})
}

// --- TestRetryScheduling ----------------------------------------------------

func TestRetryScheduling(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Part 1: two hosts, FIFO per host. A retrying command keeps its
		// position in its host queue while the other host proceeds in
		// parallel.
		type callRec struct {
			host, path string
			at         time.Time
		}
		var mu sync.Mutex
		var calls []callRec
		perURL := map[string]int{}
		var delivered []string
		var closed int32
		f := New(HandlerFunc(func(ctx *Context, res *http.Response, err error) {
			mu.Lock()
			delivered = append(delivered, ctx.Cmd.URL().String())
			mu.Unlock()
		}))
		f.DisablePoliteness, f.CrawlDelay = true, 0
		f.RetryMax, f.RetryDelay, f.RetryMaxDelay = 2, 120*time.Millisecond, 400*time.Millisecond
		f.HttpClient = testDoer(func(r *http.Request) (*http.Response, error) {
			mu.Lock()
			calls = append(calls, callRec{r.URL.Host, r.URL.Path, time.Now()})
			perURL[r.URL.String()]++
			n := perURL[r.URL.String()]
			mu.Unlock()
			if r.URL.Host == "a.invalid" && r.URL.Path == "/a1" && n == 1 {
				return rtResponse(r, 503, "", "busy", &closed), nil
			}
			return rtResponse(r, 200, "", "ok", &closed), nil
		})
		q := f.Start()
		if err := q.Send(&rtPlainRetryCmd{Cmd: &Cmd{U: rtURL("https://a.invalid/a1"), M: "GET"}, allow: true}); err != nil {
			t.Fatal(err)
		}
		if err := q.Send(&Cmd{U: rtURL("https://a.invalid/a2"), M: "GET"}); err != nil {
			t.Fatal(err)
		}
		if err := q.Send(&Cmd{U: rtURL("https://b.invalid/b1"), M: "GET"}); err != nil {
			t.Fatal(err)
		}
		if err := q.Close(); err != nil {
			t.Fatal(err)
		}
		mu.Lock()
		var a1First, a1Second, a2, b1 = -1, -1, -1, -1
		seenA1 := 0
		for i, cl := range calls {
			switch {
			case cl.host == "a.invalid" && cl.path == "/a1":
				seenA1++
				if seenA1 == 1 {
					a1First = i
				} else {
					a1Second = i
				}
			case cl.host == "a.invalid" && cl.path == "/a2":
				a2 = i
			case cl.host == "b.invalid" && cl.path == "/b1":
				b1 = i
			}
		}
		c.Check(a1First >= 0 && a1Second > a1First, "scheduling: a1 attempts at %d,%d in %v", a1First, a1Second, calls)
		c.Check(a2 > a1Second, "scheduling: a2 overtook the retrying a1 (a2=%d a1#2=%d)", a2, a1Second)
		if a1First >= 0 && a1Second >= 0 {
			gap := calls[a1Second].at.Sub(calls[a1First].at)
			c.Check(gap >= 80*time.Millisecond, "scheduling: retry gap %s below RetryDelay", gap)
			c.Check(gap < 3*time.Second, "scheduling: retry gap %s unreasonably long", gap)
		}
		if b1 >= 0 && a1Second >= 0 {
			c.Check(calls[b1].at.Before(calls[a1Second].at),
				"scheduling: host b blocked behind host a retry (%s vs %s)", calls[b1].at, calls[a1Second].at)
		}
		idxA1, idxA2 := -1, -1
		for i, u := range delivered {
			switch u {
			case "https://a.invalid/a1":
				idxA1 = i
			case "https://a.invalid/a2":
				idxA2 = i
			}
		}
		c.Check(len(delivered) == 3, "scheduling: deliveries %v", delivered)
		c.Check(idxA1 >= 0 && idxA2 > idxA1, "scheduling: delivery order %v", delivered)
		mu.Unlock()

		// Part 2: Retry-After seconds, dates and invalid values, all capped
		// by RetryMaxDelay.
		var mu2 sync.Mutex
		gaps := map[string]time.Duration{}
		firstAt := map[string]time.Time{}
		perHost := map[string]int{}
		var closed2 int32
		f2 := New(HandlerFunc(func(ctx *Context, res *http.Response, err error) {}))
		f2.DisablePoliteness, f2.CrawlDelay = true, 0
		f2.RetryMax, f2.RetryDelay, f2.RetryMaxDelay = 1, 20*time.Millisecond, 150*time.Millisecond
		f2.HttpClient = testDoer(func(r *http.Request) (*http.Response, error) {
			now := time.Now()
			mu2.Lock()
			perHost[r.URL.Host]++
			n := perHost[r.URL.Host]
			if n == 1 {
				firstAt[r.URL.Host] = now
			} else {
				gaps[r.URL.Host] = now.Sub(firstAt[r.URL.Host])
			}
			mu2.Unlock()
			if n > 1 {
				return rtResponse(r, 200, "", "ok", &closed2), nil
			}
			switch r.URL.Host {
			case "c.invalid":
				return rtResponse(r, 503, "5", "busy", &closed2), nil
			case "d.invalid":
				return rtResponse(r, 503, "not-a-date", "busy", &closed2), nil
			default: // e.invalid
				return rtResponse(r, 503, time.Now().Add(2*time.Second).UTC().Format(http.TimeFormat), "busy", &closed2), nil
			}
		})
		q2 := f2.Start()
		for _, h := range []string{"c.invalid", "d.invalid", "e.invalid"} {
			if err := q2.Send(&rtPlainRetryCmd{Cmd: &Cmd{U: rtURL("https://" + h + "/x"), M: "GET"}, allow: true}); err != nil {
				t.Fatal(err)
			}
		}
		if err := q2.Close(); err != nil {
			t.Fatal(err)
		}
		mu2.Lock()
		gapC, gapD, gapE := gaps["c.invalid"], gaps["d.invalid"], gaps["e.invalid"]
		c.Check(gapC >= 100*time.Millisecond && gapC < 2*time.Second,
			"scheduling: Retry-After seconds capped, gap %s", gapC)
		c.Check(gapE >= 100*time.Millisecond && gapE < 2*time.Second,
			"scheduling: Retry-After date honored and capped, gap %s", gapE)
		c.Check(gapD < time.Second, "scheduling: invalid Retry-After added wait, gap %s", gapD)
		c.Check(gapD < gapC, "scheduling: invalid Retry-After gap %s not below valid gap %s", gapD, gapC)
		mu2.Unlock()

		// Part 3: the host crawl delay is a floor that RetryMaxDelay cannot
		// shorten.
		var mu3 sync.Mutex
		var at3 []time.Time
		var closed3 int32
		f3 := New(HandlerFunc(func(ctx *Context, res *http.Response, err error) {}))
		f3.DisablePoliteness = true
		f3.CrawlDelay = 200 * time.Millisecond
		f3.RetryMax, f3.RetryDelay, f3.RetryMaxDelay = 1, 10*time.Millisecond, 50*time.Millisecond
		f3.HttpClient = testDoer(func(r *http.Request) (*http.Response, error) {
			mu3.Lock()
			at3 = append(at3, time.Now())
			n := len(at3)
			mu3.Unlock()
			if n == 1 {
				return rtResponse(r, 503, "1", "busy", &closed3), nil
			}
			return rtResponse(r, 200, "", "ok", &closed3), nil
		})
		q3 := f3.Start()
		if err := q3.Send(&rtPlainRetryCmd{Cmd: &Cmd{U: rtURL("https://g.invalid/g1"), M: "GET"}, allow: true}); err != nil {
			t.Fatal(err)
		}
		if err := q3.Close(); err != nil {
			t.Fatal(err)
		}
		mu3.Lock()
		if len(at3) == 2 {
			gap := at3[1].Sub(at3[0])
			c.Check(gap >= 150*time.Millisecond, "scheduling: crawl delay floor shortened to %s", gap)
			c.Check(gap < 2*time.Second, "scheduling: crawl delay floor gap %s unreasonably long", gap)
		} else {
			c.Check(false, "scheduling: crawl delay floor attempts %d", len(at3))
		}
		mu3.Unlock()

		// Part 4: a plain Close drains every attempt of an admitted command.
		var doCalls4, delivered4, closed4, status4 int32
		f4 := New(HandlerFunc(func(ctx *Context, res *http.Response, err error) {
			atomic.AddInt32(&delivered4, 1)
			if err == nil {
				atomic.StoreInt32(&status4, int32(res.StatusCode))
			}
		}))
		f4.DisablePoliteness, f4.CrawlDelay = true, 0
		f4.RetryMax, f4.RetryDelay, f4.RetryMaxDelay = 1, 100*time.Millisecond, 200*time.Millisecond
		f4.HttpClient = testDoer(func(r *http.Request) (*http.Response, error) {
			if atomic.AddInt32(&doCalls4, 1) == 1 {
				return rtResponse(r, 503, "", "busy", &closed4), nil
			}
			return rtResponse(r, 200, "", "ok", &closed4), nil
		})
		q4 := f4.Start()
		if err := q4.Send(&rtPlainRetryCmd{Cmd: &Cmd{U: rtURL("https://h.invalid/h1"), M: "GET"}, allow: true}); err != nil {
			t.Fatal(err)
		}
		t0 := time.Now()
		if err := q4.Close(); err != nil {
			t.Fatal(err)
		}
		elapsed := time.Since(t0)
		c.Check(elapsed >= 50*time.Millisecond, "scheduling: Close returned after %s without draining retries", elapsed)
		c.Check(atomic.LoadInt32(&doCalls4) == 2, "scheduling: Close drained %d attempts", doCalls4)
		c.Check(atomic.LoadInt32(&delivered4) == 1, "scheduling: Close deliveries %d", delivered4)
		c.Check(atomic.LoadInt32(&status4) == 200, "scheduling: Close final status %d", status4)

		// Part 5: Cancel interrupts the wait between attempts promptly, and
		// the command that never reached its terminal delivery gets no
		// callback.
		firstAttempt := make(chan struct{}, 1)
		var doCalls5, delivered5, closed5 int32
		f5 := New(HandlerFunc(func(ctx *Context, res *http.Response, err error) {
			atomic.AddInt32(&delivered5, 1)
		}))
		f5.DisablePoliteness, f5.CrawlDelay = true, 0
		f5.RetryMax, f5.RetryDelay, f5.RetryMaxDelay = 3, 10*time.Second, 30*time.Second
		f5.HttpClient = testDoer(func(r *http.Request) (*http.Response, error) {
			atomic.AddInt32(&doCalls5, 1)
			select {
			case firstAttempt <- struct{}{}:
			default:
			}
			return rtResponse(r, 503, "", "busy", &closed5), nil
		})
		q5 := f5.Start()
		if err := q5.Send(&rtPlainRetryCmd{Cmd: &Cmd{U: rtURL("https://i.invalid/i1"), M: "GET"}, allow: true}); err != nil {
			t.Fatal(err)
		}
		<-firstAttempt
		t1 := time.Now()
		if err := q5.Cancel(); err != nil {
			t.Fatal(err)
		}
		cancelElapsed := time.Since(t1)
		c.Check(cancelElapsed < 2*time.Second, "scheduling: Cancel took %s during retry wait", cancelElapsed)
		c.Check(atomic.LoadInt32(&doCalls5) == 1, "scheduling: attempts after Cancel %d", doCalls5)
		c.Check(atomic.LoadInt32(&delivered5) == 0, "scheduling: terminal callback after Cancel %d", delivered5)
		c.Check(atomic.LoadInt32(&closed5) == 1, "scheduling: intermediate body closed %d times", closed5)
		select {
		case <-q5.Done():
			c.Check(true, "scheduling: queue done after Cancel")
		default:
			c.Check(false, "scheduling: queue not done after Cancel")
		}
	})
}
