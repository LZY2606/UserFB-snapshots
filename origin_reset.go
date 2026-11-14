package agouti

import (
	"fmt"
	"net/url"
	"strings"
	"sync"

	"github.com/sclevine/agouti/api"
)

// EnableOriginReset enables tracked-origin reset for this page's session.
//
// The current browser URL is observed and its origin recorded before tracking
// is enabled; if that observation fails, tracking is not enabled at all.
// Enabling more than once is safe and preserves previously recorded progress.
//
// Once enabled, navigation performed through Page.Navigate, Page.Forward,
// Page.Back, Page.Refresh, Selection Click and Submit (including elements
// retrieved before enabling), and the equivalent direct *api.Session methods
// (Session.SetURL, Session.Forward, Session.Back, Session.Refresh, element
// Click/Submit, or raw Send calls to those endpoints) records the origins the
// browser actually reports before and after each command, in first-observed
// order. Page.Reset then visits each recorded origin root and clears its
// cookies, local storage, and session storage instead of only clearing the
// current domain. Without EnableOriginReset, Reset keeps its original
// behavior.
//
// Note that resetting visits each recorded origin root (origin + "/"), which
// may execute page code and may fail or redirect; only origins already
// observed through the tracked commands above are cleanup targets.
func (p *Page) EnableOriginReset() error {
	tracker := originTrackerFor(p.Session())

	tracker.mutex.Lock()
	defer tracker.mutex.Unlock()

	if err := tracker.observeLocked(); err != nil {
		return fmt.Errorf("failed to enable origin reset: %s", err)
	}
	tracker.enabled = true
	return nil
}

// OriginResetPending returns the canonical origins still waiting to be
// cleaned by Page.Reset, in the order they were first observed. The returned
// slice is a copy: mutating it does not affect the tracker's progress. It
// returns nil when origin tracking has not been enabled.
func (p *Page) OriginResetPending() []string {
	tracker := lookupOriginTracker(p.Session())
	if tracker == nil {
		return nil
	}

	tracker.mutex.Lock()
	defer tracker.mutex.Unlock()

	return append([]string(nil), tracker.pending...)
}

// originTracker intercepts navigation commands sent through a session bus and
// records the canonical origins the browser reports around each command. It
// implements api.Bus so it can wrap a session's original bus; commands that
// are not tracked navigation pass straight through.
type originTracker struct {
	inner   api.Bus
	mutex   sync.Mutex
	enabled bool

	pending    []string
	pendingSet map[string]bool

	// observationGap is set when a command was attempted but the following
	// URL observation failed, leaving the final location unknown. The next
	// tracked Reset closes the gap by sampling the current URL before it
	// cleans anything.
	observationGap bool
}

var originTrackers = struct {
	sync.Mutex
	bySession map[*api.Session]*originTracker
}{bySession: map[*api.Session]*originTracker{}}

// originTrackerFor returns the tracker attached to the session, creating and
// installing one on first use. The session's bus is wrapped exactly once, so
// repeated enabling never stacks wrappers or loses progress.
func originTrackerFor(session *api.Session) *originTracker {
	originTrackers.Lock()
	defer originTrackers.Unlock()

	if tracker, ok := originTrackers.bySession[session]; ok {
		return tracker
	}
	tracker := &originTracker{
		inner:      session.Bus,
		pendingSet: map[string]bool{},
	}
	originTrackers.bySession[session] = tracker
	session.Bus = tracker
	return tracker
}

func lookupOriginTracker(session *api.Session) *originTracker {
	originTrackers.Lock()
	defer originTrackers.Unlock()
	return originTrackers.bySession[session]
}

func (t *originTracker) isEnabled() bool {
	t.mutex.Lock()
	defer t.mutex.Unlock()
	return t.enabled
}

// Send implements api.Bus. Once tracking is enabled, every command through
// the session is serialized under the tracker mutex, so the tracked Reset
// cleanup and each navigation's pre-observation, command, and post-observation
// form one session operation boundary. Tracked navigation commands are
// additionally bracketed by URL observations. While tracking is disabled,
// commands pass straight through without locking.
func (t *originTracker) Send(method, endpoint string, body, result interface{}) error {
	t.mutex.Lock()
	if !t.enabled {
		t.mutex.Unlock()
		return t.inner.Send(method, endpoint, body, result)
	}
	defer t.mutex.Unlock()

	if !isTrackedNavigation(method, endpoint) {
		return t.inner.Send(method, endpoint, body, result)
	}

	if err := t.observeLocked(); err != nil {
		return fmt.Errorf("failed to observe URL before %s %s: %s", method, endpoint, err)
	}

	commandErr := t.inner.Send(method, endpoint, body, result)
	observeErr := t.observeLocked()
	if observeErr != nil {
		t.observationGap = true
	}

	if commandErr != nil && observeErr != nil {
		return fmt.Errorf("failed to perform %s %s: %s; also failed to observe resulting URL: %s",
			method, endpoint, commandErr, observeErr)
	}
	if commandErr != nil {
		return commandErr
	}
	if observeErr != nil {
		return fmt.Errorf("%s %s was attempted but failed to observe resulting URL: %s",
			method, endpoint, observeErr)
	}
	return nil
}

// observeLocked samples the browser-reported URL through the inner bus and
// records its origin. The tracker mutex must already be held. Probes never
// recurse through the tracking boundary.
func (t *originTracker) observeLocked() error {
	var rawurl string
	if err := t.inner.Send("GET", "url", nil, &rawurl); err != nil {
		return err
	}
	origin, ok, err := canonicalOrigin(rawurl)
	if err != nil {
		return err
	}
	if ok {
		t.recordLocked(origin)
	}
	return nil
}

func (t *originTracker) recordLocked(origin string) {
	if t.pendingSet[origin] {
		return
	}
	t.pendingSet[origin] = true
	t.pending = append(t.pending, origin)
}

// isTrackedNavigation reports whether a bus-level command is one of the
// navigation operations tracked after enabling: POST url, forward, back,
// refresh, and element click/submit.
func isTrackedNavigation(method, endpoint string) bool {
	if method != "POST" {
		return false
	}
	switch endpoint {
	case "url", "forward", "back", "refresh":
		return true
	}
	parts := strings.Split(endpoint, "/")
	return len(parts) == 3 && parts[0] == "element" &&
		(parts[2] == "click" || parts[2] == "submit")
}

// canonicalOrigin normalizes an http or https URL into its canonical origin:
// lowercase scheme and hostname, no userinfo, path, query, or fragment, no
// default port, and bracketed IPv6 hosts. Valid non-http(s) URLs such as
// about:blank are reported with ok == false; malformed URLs are errors.
func canonicalOrigin(rawurl string) (origin string, ok bool, err error) {
	parsed, err := url.Parse(rawurl)
	if err != nil {
		return "", false, fmt.Errorf("malformed URL %q: %s", rawurl, err)
	}

	scheme := strings.ToLower(parsed.Scheme)
	if scheme != "http" && scheme != "https" {
		return "", false, nil
	}

	host := strings.ToLower(parsed.Hostname())
	if host == "" {
		return "", false, fmt.Errorf("malformed URL %q: missing host", rawurl)
	}
	if strings.Contains(host, ":") {
		host = "[" + host + "]"
	}

	port := parsed.Port()
	if (scheme == "http" && port == "80") || (scheme == "https" && port == "443") {
		port = ""
	}

	origin = scheme + "://" + host
	if port != "" {
		origin += ":" + port
	}
	return origin, true, nil
}

// runScriptLocked executes JavaScript through the inner bus with the same
// body wrapping as Page.RunScript, so the storage-clearing fallback issues
// the exact request the original Reset compatibility path produces. The
// tracker mutex must already be held.
func (t *originTracker) runScriptLocked(body string) error {
	cleanBody := fmt.Sprintf("return (function() { %s; }).apply(this, arguments);", body)
	request := struct {
		Script string        `json:"script"`
		Args   []interface{} `json:"args"`
	}{cleanBody, []interface{}{}}
	return t.inner.Send("POST", "execute", request, nil)
}

// trackedReset performs origin-tracked cleanup: every pending origin is
// visited in first-observed order and cleared of cookies, local storage, and
// session storage, and the browser is finally navigated to about:blank. The
// whole cleanup holds the tracker mutex, so it is serialized with every
// tracked command boundary on the same session. Cleanup navigation and probes
// go straight to the inner bus and never enroll origins.
func (tracker *originTracker) trackedReset() error {
	tracker.mutex.Lock()
	defer tracker.mutex.Unlock()

	// Retain the original Reset behavior of confirming (and ignoring errors
	// from) an open popup before cleaning.
	tracker.inner.Send("POST", "accept_alert", nil, nil)

	if tracker.observationGap {
		if err := tracker.observeLocked(); err != nil {
			return fmt.Errorf("failed to observe current URL before reset: %s", err)
		}
		tracker.observationGap = false
	}

	for len(tracker.pending) > 0 {
		origin := tracker.pending[0]

		if err := tracker.inner.Send("POST", "url", struct {
			URL string `json:"url"`
		}{origin + "/"}, nil); err != nil {
			return fmt.Errorf("failed to visit origin %s for cleanup: %s", origin, err)
		}

		var rawurl string
		if err := tracker.inner.Send("GET", "url", nil, &rawurl); err != nil {
			return fmt.Errorf("failed to verify arrival at origin %s: %s", origin, err)
		}
		actual, _, err := canonicalOrigin(rawurl)
		if err != nil {
			return fmt.Errorf("failed to verify arrival at origin %s: %s", origin, err)
		}
		if actual != origin {
			return fmt.Errorf("cleanup of origin %s redirected to %s: refusing to clear unexpected origin", origin, rawurl)
		}

		if err := tracker.inner.Send("DELETE", "cookie", nil, nil); err != nil {
			return fmt.Errorf("failed to clear cookies for origin %s: %s", origin, err)
		}

		if err := tracker.inner.Send("DELETE", "local_storage", nil, nil); err != nil {
			if scriptErr := tracker.runScriptLocked("localStorage.clear();"); scriptErr != nil {
				return fmt.Errorf("failed to clear local storage for origin %s: %s; script fallback also failed: %s", origin, err, scriptErr)
			}
		}

		if err := tracker.inner.Send("DELETE", "session_storage", nil, nil); err != nil {
			if scriptErr := tracker.runScriptLocked("sessionStorage.clear();"); scriptErr != nil {
				return fmt.Errorf("failed to clear session storage for origin %s: %s; script fallback also failed: %s", origin, err, scriptErr)
			}
		}

		tracker.pending = tracker.pending[1:]
		delete(tracker.pendingSet, origin)
	}

	if err := tracker.inner.Send("POST", "url", struct {
		URL string `json:"url"`
	}{"about:blank"}, nil); err != nil {
		return fmt.Errorf("failed to navigate to about:blank after cleanup: %s", err)
	}
	return nil
}
