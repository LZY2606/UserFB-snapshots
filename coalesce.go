// Copyright 2014 Martin Angers and Contributors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package fetchbot

import (
	"errors"
	"io"
	"io/ioutil"
	"net/http"
	"net/url"
	"os"
)

var (
	// ErrCoalesceUnsupported is returned by Queue.Send when a Command provides a
	// non-empty CoalesceKey but is not eligible for coalescing. Only GET and HEAD
	// commands without a ReaderProvider or ValuesProvider body can be coalesced.
	// The command is not enqueued and its Handler is not called.
	ErrCoalesceUnsupported = errors.New("fetchbot: command cannot be coalesced")

	// ErrCoalesceConflict is returned by Queue.Send when a Command provides a
	// CoalesceKey that is already bound to an in-flight fetch with a different
	// HTTP method or URL. The command is not enqueued and its Handler is not called.
	ErrCoalesceConflict = errors.New("fetchbot: coalesce key bound to a different method or URL")
)

// CoalescibleCommand is an optional interface a Command can implement to
// declare that its response may be shared with other commands. A non-empty
// key opts in to in-flight fetch coalescing: commands sent with the same key
// while the fetch is still in progress (queued or downloading) join the same
// group, a single HTTP request is made, and each command's Handler receives
// its own independent copy of the response. An empty key (or not implementing
// the interface) keeps the default one-request-per-command behavior.
//
// Keys are only meaningful within a single Start of the Fetcher, no result is
// cached across batches. Once the shared body has been fully downloaded, the
// group is sealed and further Send calls with the same key start a new fetch.
type CoalescibleCommand interface {
	CoalesceKey() string
}

// coalesceGroup tracks a set of commands admitted under the same coalesce
// key. The first admitted command is the leader: it goes through robots.txt
// validation and the host's crawl delay, and it alone determines the HTTP
// request that is sent (headers, authentication, cookies). Followers only
// declare, via the key, that the leader's request is shareable.
type coalesceGroup struct {
	key    string
	method string
	url    string
	leader Command

	// subs holds the admitted commands in admission order, leader first. It
	// is appended to under the Queue's coalesce mutex while the group is
	// joinable, and read after the group is sealed.
	subs   []Command
	sealed bool

	// Staged response metadata, captured once the network body has been
	// fully consumed into the temporary file.
	status           string
	statusCode       int
	proto            string
	protoMajor       int
	protoMinor       int
	header           http.Header
	trailer          http.Header
	request          *http.Request
	contentLength    int64
	transferEncoding []string
	close            bool
	uncompressed     bool
	tmpPath          string
}

// URL implements Command so the group can flow through the host queues; it
// returns the leader's URL.
func (g *coalesceGroup) URL() *url.URL {
	return g.leader.URL()
}

// Method implements Command so the group can flow through the host queues; it
// returns the leader's method.
func (g *coalesceGroup) Method() string {
	return g.leader.Method()
}

// cleanup removes the staged temporary file, if any.
func (g *coalesceGroup) cleanup() {
	if g.tmpPath != "" {
		os.Remove(g.tmpPath)
		g.tmpPath = ""
	}
}

// response builds an independent http.Response for a single subscriber: its
// own Header, Trailer and Request metadata, and a Body that reads the staged
// temporary file from the start. Memory usage does not depend on the body
// size, each subscriber reads from its own file handle.
func (g *coalesceGroup) response() (*http.Response, error) {
	body, err := os.Open(g.tmpPath)
	if err != nil {
		return nil, err
	}
	res := &http.Response{
		Status:           g.status,
		StatusCode:       g.statusCode,
		Proto:            g.proto,
		ProtoMajor:       g.protoMajor,
		ProtoMinor:       g.protoMinor,
		Header:           g.header.Clone(),
		Body:             body,
		ContentLength:    g.contentLength,
		TransferEncoding: append([]string(nil), g.transferEncoding...),
		Close:            g.close,
		Uncompressed:     g.uncompressed,
		Trailer:          g.trailer.Clone(),
	}
	if g.request != nil {
		res.Request = g.request.Clone(g.request.Context())
	}
	return res, nil
}

// sendCoalescible admits a command with a non-empty coalesce key. Only GET and
// HEAD commands without a request body provider are eligible. A command that
// joins an existing group does not occupy a slot in the host queue and its
// request providers are never consulted.
func (q *Queue) sendCoalescible(c Command, key string) error {
	switch c.Method() {
	case "GET", "HEAD":
	default:
		return ErrCoalesceUnsupported
	}
	if _, ok := c.(ReaderProvider); ok {
		return ErrCoalesceUnsupported
	}
	if _, ok := c.(ValuesProvider); ok {
		return ErrCoalesceUnsupported
	}
	select {
	case <-q.closed:
		return ErrQueueClosed
	default:
	}

	q.cmu.Lock()
	if g, ok := q.coalesceGroups[key]; ok {
		// A group is present in the table if and only if it is still
		// joinable: sealing removes it under the same mutex.
		if g.method != c.Method() || g.url != c.URL().String() {
			q.cmu.Unlock()
			return ErrCoalesceConflict
		}
		g.subs = append(g.subs, c)
		q.cmu.Unlock()
		return nil
	}
	g := &coalesceGroup{
		key:    key,
		method: c.Method(),
		url:    c.URL().String(),
		leader: c,
		subs:   []Command{c},
	}
	q.coalesceGroups[key] = g
	q.cmu.Unlock()

	select {
	case <-q.closed:
		q.sealGroup(g)
		return ErrQueueClosed
	default:
		q.ch <- g
	}
	return nil
}

// sealGroup closes the group to new subscribers and removes it from the
// joinable table, so that a later Send with the same key starts a new fetch.
// It only deletes the table entry if it still references this group, so the
// cleanup of an old group never evicts a newer one. It returns the frozen
// list of subscribers in admission order.
func (q *Queue) sealGroup(g *coalesceGroup) []Command {
	q.cmu.Lock()
	defer q.cmu.Unlock()
	g.sealed = true
	if cur, ok := q.coalesceGroups[g.key]; ok && cur == g {
		delete(q.coalesceGroups, g.key)
	}
	return append([]Command(nil), g.subs...)
}

// fetchGroup executes the single HTTP request for a coalesced group, stages
// the body, seals the group, and delivers an independent response to each
// subscriber in admission order.
func (f *Fetcher) fetchGroup(g *coalesceGroup) error {
	res, err := f.doRequest(g.leader)
	if err != nil {
		f.failGroup(g, err)
		return err
	}
	if err := f.stageGroup(g, res); err != nil {
		f.failGroup(g, err)
		return err
	}
	// The body is fully staged: seal the group and remove it from the
	// joinable table before the first Handler delivery. A Send with the same
	// key from now on - including one re-entered from a Handler of this
	// group - starts a new fetch.
	subs := f.q.sealGroup(g)
	defer g.cleanup()
	for _, sub := range subs {
		res, err := g.response()
		if err != nil {
			f.visit(sub, nil, err)
			continue
		}
		f.visit(sub, res, nil)
	}
	return nil
}

// stageGroup consumes the network body exactly once into a temporary file and
// captures the response metadata shared by all subscribers.
func (f *Fetcher) stageGroup(g *coalesceGroup, res *http.Response) error {
	tf, err := ioutil.TempFile(f.CoalesceTempDir, "fetchbot-coalesce-")
	if err != nil {
		if res.Body != nil {
			res.Body.Close()
		}
		return err
	}
	g.tmpPath = tf.Name()
	if res.Body != nil {
		if _, err = io.Copy(tf, res.Body); err != nil {
			res.Body.Close()
			tf.Close()
			return err
		}
		if err = res.Body.Close(); err != nil {
			tf.Close()
			return err
		}
	}
	// The trailer is only complete once the network body has been fully
	// consumed, copy it after the read.
	g.trailer = res.Trailer.Clone()
	if err = tf.Close(); err != nil {
		return err
	}
	g.status = res.Status
	g.statusCode = res.StatusCode
	g.proto = res.Proto
	g.protoMajor = res.ProtoMajor
	g.protoMinor = res.ProtoMinor
	g.header = res.Header.Clone()
	g.contentLength = res.ContentLength
	g.transferEncoding = append([]string(nil), res.TransferEncoding...)
	g.close = res.Close
	g.uncompressed = res.Uncompressed
	g.request = res.Request
	return nil
}

// failGroup delivers the original error to every subscriber of the group,
// exactly once each, and releases the key so the same key can be fetched
// again. No subscriber receives a partial success.
func (f *Fetcher) failGroup(g *coalesceGroup, err error) {
	subs := f.q.sealGroup(g)
	defer g.cleanup()
	for _, sub := range subs {
		f.visit(sub, nil, err)
	}
}
