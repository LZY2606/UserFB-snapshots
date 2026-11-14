// Copyright 2014 Martin Angers and Contributors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package fetchbot

import (
	"io/ioutil"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"time"

	"github.com/temoto/robotstxt"
)

// clockNow and clockAfter are the clock primitives used by the robots.txt
// refresh scheduler. They are variables so that tests can substitute a
// controlled clock and advance time without sleeping.
var (
	clockNow   = time.Now
	clockAfter = time.After
)

// robotsRules is one published version of a host's robots.txt rules.
// A nil group with allowAll set represents a full-allow version, as published
// for a 404 or 410 response.
type robotsRules struct {
	group        *robotstxt.Group
	allowAll     bool
	etag         string
	lastModified string
}

// robotsOrigin holds the rules cache and the send schedule for a single
// origin. It outlives the host worker, so an idle-reclaimed worker can resume
// with the unexpired rules and the remaining wait.
type robotsOrigin struct {
	mu sync.Mutex

	// rules is the currently published version, nil if no usable version has
	// ever been obtained.
	rules *robotsRules

	// expires is the time at which the next refresh is due. On a failed
	// refresh it also serves as the retry time, whether or not rules exist.
	expires time.Time

	// delay is the effective delay between requests, determined by the
	// robots.txt Crawl-delay when present, otherwise by Fetcher.CrawlDelay.
	delay time.Duration

	// nextAllowed is the earliest time a request may be sent to this origin,
	// measured from the completion of the last real HTTP response.
	nextAllowed time.Time
}

// robotsSnapshot is a consistent view of the rules that apply to a command.
type robotsSnapshot struct {
	valid    bool
	allowAll bool
	group    *robotstxt.Group
}

// allowed reports whether the path may be fetched under this rule version.
func (s robotsSnapshot) allowed(path string) bool {
	if !s.valid {
		return false
	}
	if s.allowAll || s.group == nil {
		return true
	}
	return s.group.Test(path)
}

// robotsRefreshEnabled reports whether the TTL-based rules cache is active.
func (f *Fetcher) robotsRefreshEnabled() bool {
	return f.RobotsCacheTTL > 0 && !f.DisablePoliteness
}

// robotsOriginKey returns the cache and worker key for an origin: lower-cased
// scheme, hostname and effective port. The default port and an omitted port
// produce the same key.
func robotsOriginKey(u *url.URL) string {
	scheme := strings.ToLower(u.Scheme)
	host := strings.ToLower(u.Hostname())
	port := u.Port()
	if port == "" {
		switch scheme {
		case "http":
			port = "80"
		case "https":
			port = "443"
		}
	}
	if port == "" {
		return scheme + "://" + host
	}
	return scheme + "://" + host + ":" + port
}

// getRobotsOrigin returns the shared cache entry for an origin, creating it
// on first use.
func (f *Fetcher) getRobotsOrigin(key string) *robotsOrigin {
	f.robotsMu.Lock()
	defer f.robotsMu.Unlock()
	o := f.robots[key]
	if o == nil {
		o = &robotsOrigin{delay: f.CrawlDelay}
		f.robots[key] = o
	}
	return o
}

// scheduleLocked computes the next time a request may be sent, using delay
// for the interval since the response completion. The earliest send time
// already established by the previous rule version is never moved earlier.
func (o *robotsOrigin) scheduleLocked(done time.Time, delay time.Duration) {
	next := done.Add(delay)
	if next.Before(o.nextAllowed) {
		next = o.nextAllowed
	}
	o.nextAllowed = next
}

// waitUntil blocks until t, returning false if the queue was cancelled while
// waiting.
func (f *Fetcher) waitUntil(t time.Time) bool {
	d := t.Sub(clockNow())
	if d <= 0 {
		return true
	}
	select {
	case <-clockAfter(d):
		return true
	case <-f.q.cancelled:
		return false
	}
}

// ensureRobots returns the rule version to apply for the next command,
// refreshing it first if it is due. The origin's existing send schedule is
// respected by the refresh and by the request that follows it. The returned
// bool is false if the queue was cancelled while waiting.
func (f *Fetcher) ensureRobots(o *robotsOrigin, u *url.URL) (robotsSnapshot, bool) {
	for {
		now := clockNow()

		o.mu.Lock()
		due := o.expires.IsZero() || !now.Before(o.expires)
		snap := robotsSnapshot{}
		if o.rules != nil {
			snap.valid = true
			snap.allowAll = o.rules.allowAll
			snap.group = o.rules.group
		}
		next := o.nextAllowed
		o.mu.Unlock()

		if !due {
			// A rejected or unavailable page makes no request, so it neither
			// waits for nor clears the established send time.
			if !snap.valid || !snap.allowed(u.Path) {
				return snap, true
			}
			// A request must wait for the earliest send time established by
			// the previous rule version, and the request that follows a
			// refresh must wait for the interval established by the newest
			// real HTTP response (the robots.txt one included).
			if !f.waitUntil(next) {
				return snap, false
			}
			return snap, true
		}

		// Refresh when due, obeying the existing send schedule.
		if !f.waitUntil(next) {
			return snap, false
		}
		f.refreshRobots(o, u)
	}
}

// fetchRobots performs the robots.txt request, using a conditional request
// when a validator from the current rule version is available.
func (f *Fetcher) fetchRobots(u *url.URL, etag, lastModified string) (*http.Response, error) {
	robURL := u.ResolveReference(robotsTxtParsedPath)
	req, err := http.NewRequest("GET", robURL.String(), nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("User-Agent", f.UserAgent)
	if etag != "" {
		req.Header.Set("If-None-Match", etag)
	} else if lastModified != "" {
		req.Header.Set("If-Modified-Since", lastModified)
	}
	return f.HttpClient.Do(req)
}

// refreshRobots fetches and publishes a new robots.txt version if the
// response allows it. Failures never discard a usable current version.
func (f *Fetcher) refreshRobots(o *robotsOrigin, u *url.URL) {
	o.mu.Lock()
	etag, lastModified := "", ""
	if o.rules != nil {
		etag = o.rules.etag
		lastModified = o.rules.lastModified
	}
	o.mu.Unlock()

	res, err := f.fetchRobots(u, etag, lastModified)
	if err != nil {
		// A response may accompany an error (for example a redirect failure);
		// its body still belongs to the robots cache, not to the page handler.
		if res != nil && res.Body != nil {
			res.Body.Close()
		}
		// No usable response was received: do not change the send schedule.
		// Retry no sooner than one TTL from now, whether or not rules exist.
		o.mu.Lock()
		o.expires = clockNow().Add(f.RobotsCacheTTL)
		o.mu.Unlock()
		return
	}
	if res == nil {
		o.mu.Lock()
		o.expires = clockNow().Add(f.RobotsCacheTTL)
		o.mu.Unlock()
		return
	}
	if res.Body != nil {
		// The body is owned by the robots cache on every path and is never
		// passed to the page handler.
		defer res.Body.Close()
	}

	switch {
	case res.StatusCode == http.StatusNotModified:
		o.mu.Lock()
		// A 304 extends the current version's expiry only. It never publishes
		// an allow-all version, never adopts the validators it carries, and
		// without an existing version it leaves the origin unavailable. The
		// real response still counts toward the send schedule.
		o.scheduleLocked(clockNow(), o.delay)
		o.expires = clockNow().Add(f.RobotsCacheTTL)
		o.mu.Unlock()

	case res.StatusCode == http.StatusNotFound || res.StatusCode == http.StatusGone:
		o.mu.Lock()
		// Publish a full-allow version and drop the old validators.
		o.rules = &robotsRules{allowAll: true}
		o.delay = f.CrawlDelay
		o.scheduleLocked(clockNow(), o.delay)
		o.expires = clockNow().Add(f.RobotsCacheTTL)
		o.mu.Unlock()

	case res.StatusCode == http.StatusOK:
		if res.Body == nil {
			f.failRobotsRefresh(o, clockNow(), true)
			return
		}
		body, readErr := ioutil.ReadAll(res.Body)
		done := clockNow()
		if readErr != nil {
			f.failRobotsRefresh(o, done, true)
			return
		}
		data, parseErr := robotstxt.FromBytes(body)
		if parseErr != nil {
			f.failRobotsRefresh(o, done, true)
			return
		}
		group := data.FindGroup(f.UserAgent)
		delay := f.CrawlDelay
		if group != nil && group.CrawlDelay > 0 {
			delay = group.CrawlDelay
		}
		o.mu.Lock()
		// Only after a complete read and a successful parse are rules,
		// validators and expiry replaced together. Validators that are absent
		// from the new response are cleared.
		o.rules = &robotsRules{
			group:        group,
			etag:         res.Header.Get("ETag"),
			lastModified: res.Header.Get("Last-Modified"),
		}
		o.delay = delay
		o.scheduleLocked(done, delay)
		o.expires = done.Add(f.RobotsCacheTTL)
		o.mu.Unlock()

	default:
		// Any other status is a failed refresh: keep the old version.
		f.failRobotsRefresh(o, clockNow(), true)
	}
}

// failRobotsRefresh keeps the current rule version (if any) and schedules the
// next refresh attempt one TTL later. If the failure happened on a real HTTP
// response, the send schedule is advanced using the existing delay.
func (f *Fetcher) failRobotsRefresh(o *robotsOrigin, done time.Time, responseReceived bool) {
	o.mu.Lock()
	if responseReceived {
		o.scheduleLocked(done, o.delay)
	}
	o.expires = done.Add(f.RobotsCacheTTL)
	o.mu.Unlock()
}

// notePageResponse records the completion of a real page HTTP response and
// establishes the next send time using the current rule version's delay.
func (f *Fetcher) notePageResponse(o *robotsOrigin, done time.Time) {
	o.mu.Lock()
	o.scheduleLocked(done, o.delay)
	o.mu.Unlock()
}

// processCommandWithRefresh handles one command when the TTL-based rules cache
// is active: it refreshes the rules when due, then allows, rejects, or reports
// the command according to the published version. The command's own handler
// is always given the original Command value. It returns the effective delay
// for the origin.
func (f *Fetcher) processCommandWithRefresh(o *robotsOrigin, v Command) time.Duration {
	snap, ok := f.ensureRobots(o, v.URL())
	if !ok {
		// Queue cancelled while waiting; nothing is dispatched.
		return o.delayLocked()
	}

	switch {
	case !snap.valid:
		// No usable version has ever been obtained. Do not send the page
		// request; a later refresh is bound by the retry interval.
		f.visit(v, nil, ErrRobotsUnavailable)

	case !snap.allowed(v.URL().Path):
		// Path disallowed by the current version.
		f.visit(v, nil, ErrDisallowed)

	default:
		res, err := f.doRequest(v)
		if err == nil {
			f.notePageResponse(o, clockNow())
		}
		f.visit(v, res, err)
	}
	return o.delayLocked()
}

// delayLocked returns the origin's current effective delay.
func (o *robotsOrigin) delayLocked() time.Duration {
	o.mu.Lock()
	defer o.mu.Unlock()
	return o.delay
}
