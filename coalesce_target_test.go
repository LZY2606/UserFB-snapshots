// Copyright 2014 Martin Angers and Contributors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package fetchbot

import (
	"errors"
	"io"
	"io/ioutil"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/PuerkitoBio/fetchbot/task_support/taskcheck"
)

// coalesceTestCmd is a Command with a coalesce key and its own Handler.
type coalesceTestCmd struct {
	*Cmd
	key string
	fn  HandlerFunc
}

func (c *coalesceTestCmd) CoalesceKey() string { return c.key }

func (c *coalesceTestCmd) Handle(ctx *Context, res *http.Response, err error) {
	c.fn(ctx, res, err)
}

func newCoalesceTestCmd(rawurl, method, key string, fn HandlerFunc) *coalesceTestCmd {
	u, err := url.Parse(rawurl)
	if err != nil {
		panic(err)
	}
	return &coalesceTestCmd{Cmd: &Cmd{U: u, M: method}, key: key, fn: fn}
}

// coalesceReaderCmd adds a request body via ReaderProvider.
type coalesceReaderCmd struct{ *coalesceTestCmd }

func (coalesceReaderCmd) Reader() io.Reader { return strings.NewReader("x") }

// coalesceValuesCmd adds a request body via ValuesProvider.
type coalesceValuesCmd struct{ *coalesceTestCmd }

func (coalesceValuesCmd) Values() url.Values { return url.Values{"k": []string{"v"}} }

// coalesceGate controls a blocking test server: the handler writes a first
// chunk of the body, flushes, and then blocks until release is closed.
type coalesceGate struct {
	requested chan struct{}
	release   chan struct{}
	count     int32
}

func newCoalesceBlockingServer(part1, part2 string) (*httptest.Server, *coalesceGate) {
	g := &coalesceGate{requested: make(chan struct{}, 32), release: make(chan struct{})}
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		atomic.AddInt32(&g.count, 1)
		g.requested <- struct{}{}
		w.Header().Set("X-Source", "coalesced")
		w.Header().Set("Trailer", "X-Trailer")
		w.WriteHeader(http.StatusOK)
		io.WriteString(w, part1)
		if fl, ok := w.(http.Flusher); ok {
			fl.Flush()
		}
		<-g.release
		io.WriteString(w, part2)
		w.Header().Set("X-Trailer", "tail-end")
	}))
	return srv, g
}

// coalesceOutcome records what a Handler observed, for assertions in the
// main test goroutine.
type coalesceOutcome struct {
	cmd       Command
	err       error
	status    int
	body      string
	source    string
	mutated   string
	trailer   string
	reqMethod string
	reqURL    string
	reqMut    string
}

// recordOutcome returns a HandlerFunc that reads the whole body and records
// the response metadata into out, closing done when it returns.
func recordOutcome(out *coalesceOutcome, done chan<- struct{}) HandlerFunc {
	return HandlerFunc(func(ctx *Context, res *http.Response, err error) {
		defer close(done)
		out.cmd = ctx.Cmd
		out.err = err
		if res == nil {
			return
		}
		out.status = res.StatusCode
		out.source = res.Header.Get("X-Source")
		out.mutated = res.Header.Get("X-Mutated")
		if res.Request != nil {
			out.reqMethod = res.Request.Method
			out.reqURL = res.Request.URL.String()
			out.reqMut = res.Request.Header.Get("X-Req-Mut")
		}
		if res.Body != nil {
			b, err := ioutil.ReadAll(res.Body)
			if err != nil {
				out.err = err
				return
			}
			out.body = string(b)
			// The trailer is only complete once the body has been read.
			out.trailer = res.Trailer.Get("X-Trailer")
		}
	})
}

func mustParseURL(raw string) *url.URL {
	u, err := url.Parse(raw)
	if err != nil {
		panic(err)
	}
	return u
}

func TestCoalesceAdmission(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		srv, gate := newCoalesceBlockingServer("hello-", "world")
		defer srv.Close()

		var defaultCalls, rejectedCalls int32
		f := New(HandlerFunc(func(ctx *Context, res *http.Response, err error) {
			atomic.AddInt32(&defaultCalls, 1)
			if res != nil && res.Body != nil {
				io.Copy(ioutil.Discard, res.Body)
			}
		}))
		f.DisablePoliteness = true
		f.CrawlDelay = 0
		q := f.Start()

		rejectedFn := HandlerFunc(func(ctx *Context, res *http.Response, err error) {
			atomic.AddInt32(&rejectedCalls, 1)
		})

		// Commands that may not be coalesced are rejected by Send: they are
		// neither enqueued nor called back.
		post := newCoalesceTestCmd(srv.URL+"/x", "POST", "K-post", rejectedFn)
		c.Check(errors.Is(q.Send(post), ErrCoalesceUnsupported), "POST with key must return ErrCoalesceUnsupported")
		rd := &coalesceReaderCmd{newCoalesceTestCmd(srv.URL+"/x", "GET", "K-reader", rejectedFn)}
		c.Check(errors.Is(q.Send(rd), ErrCoalesceUnsupported), "GET with ReaderProvider must return ErrCoalesceUnsupported")
		vals := &coalesceValuesCmd{newCoalesceTestCmd(srv.URL+"/x", "GET", "K-values", rejectedFn)}
		c.Check(errors.Is(q.Send(vals), ErrCoalesceUnsupported), "GET with ValuesProvider must return ErrCoalesceUnsupported")

		// The leader starts a group and blocks while downloading the body.
		leaderOut, leaderDone := &coalesceOutcome{}, make(chan struct{})
		leader := newCoalesceTestCmd(srv.URL+"/page", "GET", "K1", recordOutcome(leaderOut, leaderDone))
		c.Check(q.Send(leader) == nil, "leader Send failed")
		<-gate.requested // the leader's download is in flight

		// The key is bound to the leader's method and URL.
		otherURL := newCoalesceTestCmd(srv.URL+"/other", "GET", "K1", rejectedFn)
		c.Check(errors.Is(q.Send(otherURL), ErrCoalesceConflict), "same key, different URL must return ErrCoalesceConflict")
		otherMeth := newCoalesceTestCmd(srv.URL+"/page", "HEAD", "K1", rejectedFn)
		c.Check(errors.Is(q.Send(otherMeth), ErrCoalesceConflict), "same key, different method must return ErrCoalesceConflict")

		// Same key, method and URL joins the in-flight fetch.
		followerOut, followerDone := &coalesceOutcome{}, make(chan struct{})
		follower := newCoalesceTestCmd(srv.URL+"/page", "GET", "K1", recordOutcome(followerOut, followerDone))
		c.Check(q.Send(follower) == nil, "follower Send failed")

		// A leader still queued behind the in-flight GET accepts followers too.
		head1Out, head1Done := &coalesceOutcome{}, make(chan struct{})
		head1 := newCoalesceTestCmd(srv.URL+"/head", "HEAD", "K-head", recordOutcome(head1Out, head1Done))
		c.Check(q.Send(head1) == nil, "queued HEAD leader Send failed")
		head2Out, head2Done := &coalesceOutcome{}, make(chan struct{})
		head2 := newCoalesceTestCmd(srv.URL+"/head", "HEAD", "K-head", recordOutcome(head2Out, head2Done))
		c.Check(q.Send(head2) == nil, "queued HEAD follower Send failed")

		close(gate.release)
		<-leaderDone
		<-followerDone
		<-head1Done
		<-head2Done

		// An empty key takes the normal path, one request per command.
		empty1Out, empty1Done := &coalesceOutcome{}, make(chan struct{})
		empty2Out, empty2Done := &coalesceOutcome{}, make(chan struct{})
		c.Check(q.Send(newCoalesceTestCmd(srv.URL+"/e1", "GET", "", recordOutcome(empty1Out, empty1Done))) == nil, "empty key Send failed")
		c.Check(q.Send(newCoalesceTestCmd(srv.URL+"/e2", "GET", "", recordOutcome(empty2Out, empty2Done))) == nil, "empty key Send failed")
		// A command without the interface takes the normal path too.
		c.Check(q.Send(&Cmd{U: mustParseURL(srv.URL + "/plain"), M: "GET"}) == nil, "plain Send failed")
		<-empty1Done
		<-empty2Done

		c.Check(q.Close() == nil, "Close failed")

		// 1 coalesced GET + 1 coalesced HEAD + 2 empty-key + 1 plain.
		c.Check(atomic.LoadInt32(&gate.count) == 5, "expected 5 network requests, got %d", gate.count)
		c.Check(atomic.LoadInt32(&rejectedCalls) == 0, "rejected commands must not be called back, got %d", rejectedCalls)
		c.Check(atomic.LoadInt32(&defaultCalls) == 1, "only the plain command must use the fetcher handler, got %d", defaultCalls)

		c.Check(leaderOut.err == nil && leaderOut.body == "hello-world", "leader body: %q err: %v", leaderOut.body, leaderOut.err)
		c.Check(followerOut.err == nil && followerOut.body == "hello-world", "follower body: %q err: %v", followerOut.body, followerOut.err)
		c.Check(leaderOut.cmd == leader && followerOut.cmd == follower, "subscribers must keep their own command identity")
		c.Check(leaderOut.trailer == "tail-end" && followerOut.trailer == "tail-end", "trailers: %q / %q", leaderOut.trailer, followerOut.trailer)
		c.Check(head1Out.err == nil && head1Out.status == http.StatusOK && head1Out.body == "", "head1: status %d body %q err %v", head1Out.status, head1Out.body, head1Out.err)
		c.Check(head2Out.err == nil && head2Out.status == http.StatusOK && head2Out.body == "", "head2: status %d body %q err %v", head2Out.status, head2Out.body, head2Out.err)
		c.Check(head1Out.cmd == head1 && head2Out.cmd == head2, "HEAD subscribers must keep their own command identity")
		c.Check(empty1Out.err == nil && empty1Out.body == "hello-world", "empty key 1 body: %q err: %v", empty1Out.body, empty1Out.err)
		c.Check(empty2Out.err == nil && empty2Out.body == "hello-world", "empty key 2 body: %q err: %v", empty2Out.body, empty2Out.err)
	})
}

func TestCoalesceBodies(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const part1, part2 = "first-part-", "second-part"
		srv, gate := newCoalesceBlockingServer(part1, part2)
		defer srv.Close()

		dir, err := ioutil.TempDir("", "fetchbot-coalesce-bodies")
		c.Check(err == nil, "temp dir: %v", err)
		defer os.RemoveAll(dir)

		f := New(HandlerFunc(func(ctx *Context, res *http.Response, err error) {}))
		f.DisablePoliteness = true
		f.CrawlDelay = 0
		f.CoalesceTempDir = dir
		q := f.Start()

		// Deliveries happen sequentially on the host worker goroutine, in
		// admission order.
		var order []string

		// The leader reads only a prefix, mutates its header and closes its
		// body early: none of this may leak into the other subscribers.
		leaderOut, leaderDone := &coalesceOutcome{}, make(chan struct{})
		leader := newCoalesceTestCmd(srv.URL+"/big", "GET", "KB", HandlerFunc(func(ctx *Context, res *http.Response, err error) {
			defer close(leaderDone)
			order = append(order, "leader")
			leaderOut.cmd = ctx.Cmd
			leaderOut.err = err
			if res == nil {
				return
			}
			leaderOut.status = res.StatusCode
			p := make([]byte, len(part1)-1)
			if _, err := io.ReadFull(res.Body, p); err != nil {
				leaderOut.err = err
				return
			}
			leaderOut.body = string(p)
			res.Header.Set("X-Mutated", "leader")
			res.Body.Close()
		}))
		c.Check(q.Send(leader) == nil, "leader Send failed")
		<-gate.requested // the leader is downloading, followers join in flight

		// Follower 1 reads everything and mutates its own copies of the
		// trailer and request metadata.
		f1Out, f1Done := &coalesceOutcome{}, make(chan struct{})
		f1 := newCoalesceTestCmd(srv.URL+"/big", "GET", "KB", HandlerFunc(func(ctx *Context, res *http.Response, err error) {
			defer close(f1Done)
			order = append(order, "f1")
			f1Out.cmd = ctx.Cmd
			f1Out.err = err
			if res == nil {
				return
			}
			f1Out.status = res.StatusCode
			f1Out.source = res.Header.Get("X-Source")
			f1Out.mutated = res.Header.Get("X-Mutated")
			b, err := ioutil.ReadAll(res.Body)
			if err != nil {
				f1Out.err = err
				return
			}
			f1Out.body = string(b)
			f1Out.trailer = res.Trailer.Get("X-Trailer")
			if res.Request != nil {
				f1Out.reqMethod = res.Request.Method
				f1Out.reqURL = res.Request.URL.String()
				res.Request.Header.Set("X-Req-Mut", "f1")
			}
			res.Trailer.Set("X-Trailer", "f1-was-here")
		}))
		c.Check(q.Send(f1) == nil, "follower 1 Send failed")

		// Follower 2 re-enters Send with the same key from inside its
		// Handler: the group is sealed by then, so this starts a new fetch.
		f2Out, f2Done := &coalesceOutcome{}, make(chan struct{})
		reOut, reDone := &coalesceOutcome{}, make(chan struct{})
		var reSendErr error
		reCmd := newCoalesceTestCmd(srv.URL+"/big", "GET", "KB", recordOutcome(reOut, reDone))
		f2 := newCoalesceTestCmd(srv.URL+"/big", "GET", "KB", HandlerFunc(func(ctx *Context, res *http.Response, err error) {
			defer close(f2Done)
			order = append(order, "f2")
			f2Out.cmd = ctx.Cmd
			f2Out.err = err
			if res == nil {
				return
			}
			f2Out.source = res.Header.Get("X-Source")
			f2Out.mutated = res.Header.Get("X-Mutated")
			b, err := ioutil.ReadAll(res.Body)
			if err != nil {
				f2Out.err = err
				return
			}
			f2Out.body = string(b)
			f2Out.trailer = res.Trailer.Get("X-Trailer")
			if res.Request != nil {
				f2Out.reqMut = res.Request.Header.Get("X-Req-Mut")
			}
			reSendErr = ctx.Q.Send(reCmd)
		}))
		c.Check(q.Send(f2) == nil, "follower 2 Send failed")

		close(gate.release)
		<-leaderDone
		<-f1Done
		<-f2Done
		<-reDone
		c.Check(q.Close() == nil, "Close failed")

		// One network request for the initial group, one for the re-entry.
		c.Check(atomic.LoadInt32(&gate.count) == 2, "expected 2 network requests, got %d", gate.count)
		c.Check(len(order) == 3 && order[0] == "leader" && order[1] == "f1" && order[2] == "f2", "delivery order: %v", order)

		c.Check(leaderOut.err == nil && leaderOut.body == part1[:len(part1)-1], "leader prefix: %q err: %v", leaderOut.body, leaderOut.err)
		c.Check(leaderOut.cmd == leader, "leader must get its own command")
		c.Check(f1Out.err == nil && f1Out.body == part1+part2, "f1 body: %q err: %v", f1Out.body, f1Out.err)
		c.Check(f1Out.cmd == f1, "f1 must get its own command")
		c.Check(f1Out.source == "coalesced" && f1Out.mutated == "", "f1 header must be independent: source=%q mutated=%q", f1Out.source, f1Out.mutated)
		c.Check(f1Out.trailer == "tail-end", "f1 trailer: %q", f1Out.trailer)
		c.Check(f1Out.reqMethod == "GET" && f1Out.reqURL == srv.URL+"/big", "f1 request metadata: %s %s", f1Out.reqMethod, f1Out.reqURL)
		c.Check(f2Out.err == nil && f2Out.body == part1+part2, "f2 body: %q err: %v", f2Out.body, f2Out.err)
		c.Check(f2Out.cmd == f2, "f2 must get its own command")
		c.Check(f2Out.mutated == "", "f2 header must not see earlier mutations, got %q", f2Out.mutated)
		c.Check(f2Out.trailer == "tail-end", "f2 trailer must not see earlier mutations, got %q", f2Out.trailer)
		c.Check(f2Out.reqMut == "", "f2 request must not see earlier mutations, got %q", f2Out.reqMut)

		c.Check(reSendErr == nil, "re-entrant Send after seal must succeed, got %v", reSendErr)
		c.Check(reOut.err == nil && reOut.body == part1+part2, "re-entry body: %q err: %v", reOut.body, reOut.err)
		c.Check(reOut.cmd == reCmd, "re-entry must get its own command")

		// The temporary directory is cleaned up after each group.
		entries, err := ioutil.ReadDir(dir)
		c.Check(err == nil && len(entries) == 0, "coalesce temp dir must be empty, got %d entries (err %v)", len(entries), err)
	})
}

// coalesceFailDoer returns an error for the first failures requests, gating
// the first one so followers can join while it is in flight, and delegates
// to the inner Doer afterwards.
type coalesceFailDoer struct {
	failures int32
	started  chan struct{}
	release  chan struct{}
	err      error
	inner    Doer
}

func (d *coalesceFailDoer) Do(req *http.Request) (*http.Response, error) {
	if atomic.AddInt32(&d.failures, -1) >= 0 {
		d.started <- struct{}{}
		<-d.release
		return nil, d.err
	}
	return d.inner.Do(req)
}

// coalesceGatedFailBody fails every read with err; the first read blocks
// until release is closed, so followers can join while the body is staged.
type coalesceGatedFailBody struct {
	started chan struct{}
	release chan struct{}
	once    sync.Once
	err     error
}

func (b *coalesceGatedFailBody) Read(p []byte) (int, error) {
	b.once.Do(func() {
		close(b.started)
		<-b.release
	})
	return 0, b.err
}

func (b *coalesceGatedFailBody) Close() error { return nil }

// coalesceStageFailDoer returns a response with a failing body for the first
// failures requests, and delegates to the inner Doer afterwards.
type coalesceStageFailDoer struct {
	failures int32
	body     *coalesceGatedFailBody
	inner    Doer
}

func (d *coalesceStageFailDoer) Do(req *http.Request) (*http.Response, error) {
	if atomic.AddInt32(&d.failures, -1) >= 0 {
		return &http.Response{
			Status:     "200 OK",
			StatusCode: http.StatusOK,
			Proto:      "HTTP/1.1",
			ProtoMajor: 1,
			ProtoMinor: 1,
			Header:     make(http.Header),
			Body:       d.body,
			Request:    req,
		}, nil
	}
	return d.inner.Do(req)
}

func TestCoalesceLifecycle(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		dir, err := ioutil.TempDir("", "fetchbot-coalesce-lifecycle")
		c.Check(err == nil, "temp dir: %v", err)
		defer os.RemoveAll(dir)
		noopHandler := HandlerFunc(func(ctx *Context, res *http.Response, err error) {})

		// --- robots.txt refusal: every subscriber gets ErrDisallowed once.
		var robotsHits, blockedHits int32
		robotsRequested := make(chan struct{})
		robotsRelease := make(chan struct{})
		mux := http.NewServeMux()
		mux.HandleFunc("/robots.txt", func(w http.ResponseWriter, r *http.Request) {
			atomic.AddInt32(&robotsHits, 1)
			close(robotsRequested)
			<-robotsRelease
			io.WriteString(w, "User-agent: *\nDisallow: /blocked\n")
		})
		mux.HandleFunc("/blocked/", func(w http.ResponseWriter, r *http.Request) {
			atomic.AddInt32(&blockedHits, 1)
			io.WriteString(w, "nope")
		})
		rsrv := httptest.NewServer(mux)
		defer rsrv.Close()

		rf := New(noopHandler)
		rf.CrawlDelay = 0
		rq := rf.Start()

		r1Out, r1Done := &coalesceOutcome{}, make(chan struct{})
		r2Out, r2Done := &coalesceOutcome{}, make(chan struct{})
		c.Check(rq.Send(newCoalesceTestCmd(rsrv.URL+"/blocked/a", "GET", "KR", recordOutcome(r1Out, r1Done))) == nil, "robots leader Send failed")
		<-robotsRequested // the group is queued behind the robots.txt fetch
		c.Check(rq.Send(newCoalesceTestCmd(rsrv.URL+"/blocked/a", "GET", "KR", recordOutcome(r2Out, r2Done))) == nil, "robots follower Send failed")
		close(robotsRelease)
		<-r1Done
		<-r2Done
		c.Check(rq.Close() == nil, "robots Close failed")

		c.Check(errors.Is(r1Out.err, ErrDisallowed), "leader must get ErrDisallowed, got %v", r1Out.err)
		c.Check(errors.Is(r2Out.err, ErrDisallowed), "follower must get ErrDisallowed, got %v", r2Out.err)
		c.Check(atomic.LoadInt32(&robotsHits) == 1, "robots.txt fetched once, got %d", robotsHits)
		c.Check(atomic.LoadInt32(&blockedHits) == 0, "disallowed path must not be fetched, got %d", blockedHits)

		// --- Transport failure: the original error reaches every subscriber
		// and the key can be fetched again afterwards.
		okSrv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			io.WriteString(w, "recovered-body")
		}))
		defer okSrv.Close()

		sentinel := errors.New("coalesce transport failure")
		tf := New(noopHandler)
		tf.DisablePoliteness = true
		tf.CrawlDelay = 0
		tf.CoalesceTempDir = dir
		doer := &coalesceFailDoer{failures: 1, started: make(chan struct{}, 1), release: make(chan struct{}), err: sentinel, inner: http.DefaultClient}
		tf.HttpClient = doer
		tq := tf.Start()

		t1Out, t1Done := &coalesceOutcome{}, make(chan struct{})
		t2Out, t2Done := &coalesceOutcome{}, make(chan struct{})
		c.Check(tq.Send(newCoalesceTestCmd(okSrv.URL+"/r", "GET", "KT", recordOutcome(t1Out, t1Done))) == nil, "transport leader Send failed")
		<-doer.started // the failing request is in flight
		c.Check(tq.Send(newCoalesceTestCmd(okSrv.URL+"/r", "GET", "KT", recordOutcome(t2Out, t2Done))) == nil, "transport follower Send failed")
		close(doer.release)
		<-t1Done
		<-t2Done
		c.Check(errors.Is(t1Out.err, sentinel), "leader must get the original transport error, got %v", t1Out.err)
		c.Check(errors.Is(t2Out.err, sentinel), "follower must get the original transport error, got %v", t2Out.err)

		// The same key can be fetched again once the failed group is cleaned up.
		t3Out, t3Done := &coalesceOutcome{}, make(chan struct{})
		c.Check(tq.Send(newCoalesceTestCmd(okSrv.URL+"/r", "GET", "KT", recordOutcome(t3Out, t3Done))) == nil, "refetch Send failed")
		<-t3Done
		c.Check(t3Out.err == nil && t3Out.body == "recovered-body", "refetch body: %q err: %v", t3Out.body, t3Out.err)
		c.Check(tq.Close() == nil, "transport Close failed")
		entries, err := ioutil.ReadDir(dir)
		c.Check(err == nil && len(entries) == 0, "temp dir must be clean after transport failure, got %d entries", len(entries))

		// --- Staging failure: no subscriber sees a partial success.
		stageErr := errors.New("coalesce staging failure")
		sbody := &coalesceGatedFailBody{started: make(chan struct{}), release: make(chan struct{}), err: stageErr}
		sf := New(noopHandler)
		sf.DisablePoliteness = true
		sf.CrawlDelay = 0
		sf.CoalesceTempDir = dir
		sf.HttpClient = &coalesceStageFailDoer{failures: 1, body: sbody, inner: http.DefaultClient}
		sq := sf.Start()

		s1Out, s1Done := &coalesceOutcome{}, make(chan struct{})
		s2Out, s2Done := &coalesceOutcome{}, make(chan struct{})
		c.Check(sq.Send(newCoalesceTestCmd(okSrv.URL+"/s", "GET", "KS", recordOutcome(s1Out, s1Done))) == nil, "staging leader Send failed")
		<-sbody.started // the body is being staged
		c.Check(sq.Send(newCoalesceTestCmd(okSrv.URL+"/s", "GET", "KS", recordOutcome(s2Out, s2Done))) == nil, "staging follower Send failed")
		close(sbody.release)
		<-s1Done
		<-s2Done
		c.Check(errors.Is(s1Out.err, stageErr), "leader must get the original staging error, got %v", s1Out.err)
		c.Check(errors.Is(s2Out.err, stageErr), "follower must get the original staging error, got %v", s2Out.err)

		s3Out, s3Done := &coalesceOutcome{}, make(chan struct{})
		c.Check(sq.Send(newCoalesceTestCmd(okSrv.URL+"/s", "GET", "KS", recordOutcome(s3Out, s3Done))) == nil, "staging refetch Send failed")
		<-s3Done
		c.Check(s3Out.err == nil && s3Out.body == "recovered-body", "staging refetch body: %q err: %v", s3Out.body, s3Out.err)
		c.Check(sq.Close() == nil, "staging Close failed")
		entries, err = ioutil.ReadDir(dir)
		c.Check(err == nil && len(entries) == 0, "temp dir must be clean after staging failure, got %d entries", len(entries))

		// --- Close waits for the handlers of an accepted group and for the
		// temporary file cleanup.
		wsrv, wgate := newCoalesceBlockingServer("close-", "body")
		defer wsrv.Close()
		wf := New(noopHandler)
		wf.DisablePoliteness = true
		wf.CrawlDelay = 0
		wf.CoalesceTempDir = dir
		wq := wf.Start()

		handlerStarted := make(chan struct{})
		releaseHandler := make(chan struct{})
		w1Out, w1Done := &coalesceOutcome{}, make(chan struct{})
		w1 := newCoalesceTestCmd(wsrv.URL+"/w", "GET", "KW", HandlerFunc(func(ctx *Context, res *http.Response, err error) {
			defer close(w1Done)
			w1Out.cmd = ctx.Cmd
			w1Out.err = err
			close(handlerStarted)
			<-releaseHandler
			if res != nil && res.Body != nil {
				b, _ := ioutil.ReadAll(res.Body)
				w1Out.body = string(b)
			}
		}))
		c.Check(wq.Send(w1) == nil, "close-wait leader Send failed")
		<-wgate.requested
		w2Out, w2Done := &coalesceOutcome{}, make(chan struct{})
		c.Check(wq.Send(newCoalesceTestCmd(wsrv.URL+"/w", "GET", "KW", recordOutcome(w2Out, w2Done))) == nil, "close-wait follower Send failed")
		close(wgate.release)
		<-handlerStarted

		closed := make(chan struct{})
		go func() {
			wq.Close()
			close(closed)
		}()
		select {
		case <-closed:
			c.Check(false, "Close returned while a coalesced Handler was still running")
		case <-time.After(200 * time.Millisecond):
			c.Check(true, "Close waits for a running coalesced Handler")
		}
		close(releaseHandler)
		<-closed
		<-w1Done
		<-w2Done
		c.Check(w1Out.err == nil && w1Out.body == "close-body", "blocked handler body: %q err: %v", w1Out.body, w1Out.err)
		c.Check(w2Out.err == nil && w2Out.body == "close-body", "follower body: %q err: %v", w2Out.body, w2Out.err)
		entries, err = ioutil.ReadDir(dir)
		c.Check(err == nil && len(entries) == 0, "temp dir must be clean after Close, got %d entries", len(entries))

		// --- Cancel drops pending work, an already running group finishes.
		csrv, cgate := newCoalesceBlockingServer("cancel-", "body")
		defer csrv.Close()
		cf := New(noopHandler)
		cf.DisablePoliteness = true
		cf.CrawlDelay = 0
		cf.CoalesceTempDir = dir
		cq := cf.Start()

		c1Out, c1Done := &coalesceOutcome{}, make(chan struct{})
		c2Out, c2Done := &coalesceOutcome{}, make(chan struct{})
		c.Check(cq.Send(newCoalesceTestCmd(csrv.URL+"/c", "GET", "KC1", recordOutcome(c1Out, c1Done))) == nil, "cancel leader Send failed")
		<-cgate.requested
		c.Check(cq.Send(newCoalesceTestCmd(csrv.URL+"/c", "GET", "KC1", recordOutcome(c2Out, c2Done))) == nil, "cancel follower Send failed")
		var pendCalls int32
		pendFn := HandlerFunc(func(ctx *Context, res *http.Response, err error) {
			atomic.AddInt32(&pendCalls, 1)
		})
		c.Check(cq.Send(newCoalesceTestCmd(csrv.URL+"/pending", "GET", "KC2", pendFn)) == nil, "pending Send failed")

		cancelled := make(chan struct{})
		go func() {
			cq.Cancel()
			close(cancelled)
		}()
		<-cq.cancelled // the cancel signal is in effect before the download ends
		close(cgate.release)
		<-cancelled
		<-c1Done
		<-c2Done
		c.Check(c1Out.err == nil && c1Out.body == "cancel-body", "running group leader body: %q err: %v", c1Out.body, c1Out.err)
		c.Check(c2Out.err == nil && c2Out.body == "cancel-body", "running group follower body: %q err: %v", c2Out.body, c2Out.err)
		c.Check(atomic.LoadInt32(&pendCalls) == 0, "pending group must be dropped on Cancel, got %d calls", pendCalls)
		c.Check(atomic.LoadInt32(&cgate.count) == 1, "only the running group must hit the network, got %d", cgate.count)
	})
}
