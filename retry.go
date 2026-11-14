// Copyright 2014 Martin Angers and Contributors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package fetchbot

import (
	"bytes"
	"errors"
	"io"
	"io/ioutil"
	"net/http"
	"regexp"
	"strconv"
	"strings"
	"time"
)

// ErrRetryBodyTooLarge is delivered to the terminal Handler of a retryable
// Command when its body exceeds the Fetcher's RetryBodyLimit and can thus not
// be captured for replay. No request is sent for such a Command, not even a
// prefix of the body. Use errors.Is to recognize it.
var ErrRetryBodyTooLarge = errors.New("fetchbot: retry body too large")

const (
	// DefaultRetryMaxDelay is the default cap on a single wait between two
	// attempts of a retryable Command.
	DefaultRetryMaxDelay = 30 * time.Second

	// DefaultRetryBodyLimit is the default maximum size, in bytes, of a
	// Command body that can be captured for replay (1 MiB).
	DefaultRetryBodyLimit = 1 << 20
)

// maxRetryWait is a saturating value used when a Retry-After header holds a
// number too large to represent as a time.Duration. It is always capped by
// RetryMaxDelay before being used, so it cannot overflow.
const maxRetryWait = time.Duration(1<<63 - 1)

// RetryableCommand is implemented by Commands that explicitly allow their
// request to be replayed. Retries are enabled for a Command only if the
// Fetcher's RetryMax is greater than zero and the Command implements this
// interface and returns true. The decision is taken exactly once per Command,
// after the robots.txt admission and before the first attempt.
type RetryableCommand interface {
	Retryable() bool
}

// retrySnapshot holds an immutable, replayable copy of everything needed to
// build the request of a retryable Command. Each provider is queried exactly
// once, when the snapshot is taken.
type retrySnapshot struct {
	header   http.Header
	user     string
	pwd      string
	hasAuth  bool
	cookies  []*http.Cookie
	body     []byte
	hasBody  bool
	formBody bool
}

// newRetrySnapshot captures the request data of cmd, following the same
// provider priorities as doRequest. Every provider implemented by cmd is
// called exactly once. A Reader-provided body is read exactly once, and if
// the Reader is also an io.Closer it is closed once after the read. If the
// body cannot be read or exceeds bodyLimit, an error is returned and no
// request must be sent for this Command.
func newRetrySnapshot(cmd Command, bodyLimit int64) (*retrySnapshot, error) {
	s := &retrySnapshot{}
	if hd, ok := cmd.(HeaderProvider); ok {
		h := hd.Header()
		s.header = make(http.Header, len(h))
		for k, vs := range h {
			s.header[k] = append([]string(nil), vs...)
		}
	}
	if ba, ok := cmd.(BasicAuthProvider); ok {
		s.user, s.pwd = ba.BasicAuth()
		s.hasAuth = true
	}
	if ck, ok := cmd.(CookiesProvider); ok {
		for _, c := range ck.Cookies() {
			if c == nil {
				continue
			}
			cp := *c
			s.cookies = append(s.cookies, &cp)
		}
	}
	// ReaderProvider has higher priority than ValuesProvider, as in
	// doRequest.
	if rd, ok := cmd.(ReaderProvider); ok {
		r := rd.Reader()
		body, err := readReplayBody(r, bodyLimit)
		if rc, ok := r.(io.Closer); ok {
			// The reader has been consumed by the snapshot, close it once.
			rc.Close()
		}
		if err != nil {
			return nil, err
		}
		s.body, s.hasBody = body, true
	} else if val, ok := cmd.(ValuesProvider); ok {
		enc := val.Values().Encode()
		if int64(len(enc)) > bodyLimit {
			return nil, ErrRetryBodyTooLarge
		}
		s.body, s.hasBody, s.formBody = []byte(enc), true, true
	}
	return s, nil
}

// readReplayBody reads r fully, once, returning an error if it holds more
// than limit bytes.
func readReplayBody(r io.Reader, limit int64) ([]byte, error) {
	if r == nil {
		return nil, nil
	}
	lr := &io.LimitedReader{R: r, N: limit + 1}
	body, err := ioutil.ReadAll(lr)
	if err != nil {
		return nil, err
	}
	if lr.N == 0 {
		return nil, ErrRetryBodyTooLarge
	}
	return body, nil
}

// doCommand executes the request for cmd, transparently retrying 429 and 503
// responses when the Command allows replay (see RetryableCommand) and the
// Fetcher is configured with RetryMax > 0. It returns the response and error
// to deliver to the terminal Handler. The third return value is false if the
// Queue was cancelled while waiting between attempts, in which case nothing
// must be delivered for this Command.
func (f *Fetcher) doCommand(cmd Command, crawlDelay time.Duration) (*http.Response, error, bool) {
	if f.RetryMax <= 0 {
		res, err := f.doRequest(cmd)
		return res, err, true
	}
	rc, ok := cmd.(RetryableCommand)
	if !ok || !rc.Retryable() {
		res, err := f.doRequest(cmd)
		return res, err, true
	}
	// The Command opted in to replays: take a snapshot of the request data
	// so that every attempt sends the exact same bytes without calling the
	// providers again.
	snap, err := newRetrySnapshot(cmd, f.RetryBodyLimit)
	if err != nil {
		// The body could not be captured for replay (read error or over the
		// RetryBodyLimit): no request is sent at all, the original error goes
		// to the terminal Handler.
		return nil, err, true
	}
	for attempt := 0; ; attempt++ {
		res, err := f.doSnapshotRequest(cmd, snap)
		if err != nil {
			// Transport errors are delivered as-is, they are not retried.
			return nil, err, true
		}
		if res.StatusCode != http.StatusTooManyRequests && res.StatusCode != http.StatusServiceUnavailable {
			return res, nil, true
		}
		if attempt >= f.RetryMax {
			// Out of extra attempts, deliver the last real response.
			return res, nil, true
		}
		// Intermediate response: it must not reach any Handler. Close its
		// body once before the next attempt, and measure the wait from the
		// moment Do returned.
		retryAfter := parseRetryAfter(res.Header, time.Now())
		if res.Body != nil {
			res.Body.Close()
		}
		wait := retryWait(crawlDelay, f.RetryDelay, f.RetryMaxDelay, retryAfter)
		if wait <= 0 {
			select {
			case <-f.q.cancelled:
				return nil, nil, false
			default:
			}
			continue
		}
		t := time.NewTimer(wait)
		select {
		case <-f.q.cancelled:
			// Cancelled while waiting: stop promptly, without delivering a
			// terminal callback for this Command.
			t.Stop()
			return nil, nil, false
		case <-t.C:
		}
	}
}

// doSnapshotRequest builds a fresh request from the snapshot and executes it.
// The original Command still provides the method and URL, and each attempt
// gets its own independent body Reader.
func (f *Fetcher) doSnapshotRequest(cmd Command, snap *retrySnapshot) (*http.Response, error) {
	var body io.Reader
	if snap.hasBody {
		body = bytes.NewReader(snap.body)
	}
	req, err := http.NewRequest(cmd.Method(), cmd.URL().String(), body)
	if err != nil {
		return nil, err
	}
	for k, vs := range snap.header {
		req.Header[k] = append([]string(nil), vs...)
	}
	// BasicAuth has higher priority than an Authorization header set by a
	// HeaderProvider.
	if snap.hasAuth {
		req.SetBasicAuth(snap.user, snap.pwd)
	}
	for _, c := range snap.cookies {
		req.AddCookie(c)
	}
	if snap.formBody && req.Header.Get("Content-Type") == "" {
		req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	}
	if req.Header.Get("User-Agent") == "" {
		req.Header.Set("User-Agent", f.UserAgent)
	}
	return f.HttpClient.Do(req)
}

// retryWait computes how long to wait before the next attempt: the largest of
// the host's crawl delay, RetryDelay and the Retry-After hint, with the
// RetryMaxDelay cap applied only to the two latter components so that it can
// never shorten the crawl delay.
func retryWait(crawlDelay, retryDelay, maxDelay, retryAfter time.Duration) time.Duration {
	wait := retryDelay
	if retryAfter > wait {
		wait = retryAfter
	}
	if wait > maxDelay {
		wait = maxDelay
	}
	if crawlDelay > wait {
		wait = crawlDelay
	}
	return wait
}

// retryAfterSeconds matches a non-negative decimal number of seconds.
var retryAfterSeconds = regexp.MustCompile(`^\d+(?:\.\d+)?$`)

// parseRetryAfter interprets the first Retry-After header field of the
// response. It supports non-negative decimal seconds and dates parseable by
// http.ParseTime. Invalid values, zero and past dates produce no additional
// wait, and out-of-range numbers saturate instead of overflowing.
func parseRetryAfter(h http.Header, now time.Time) time.Duration {
	v := strings.TrimSpace(h.Get("Retry-After"))
	if v == "" {
		return 0
	}
	if retryAfterSeconds.MatchString(v) {
		secs, err := strconv.ParseFloat(v, 64)
		if err != nil {
			// The value is syntactically valid, so this can only be a range
			// error: saturate, it will be capped by RetryMaxDelay anyway.
			return maxRetryWait
		}
		if secs <= 0 {
			return 0
		}
		if secs >= float64(maxRetryWait)/float64(time.Second) {
			return maxRetryWait
		}
		return time.Duration(secs * float64(time.Second))
	}
	if t, err := http.ParseTime(v); err == nil {
		if d := t.Sub(now); d > 0 {
			return d
		}
	}
	return 0
}
