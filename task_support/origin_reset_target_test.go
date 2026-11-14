package agouti_test

import (
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/sclevine/agouti"
	"github.com/sclevine/agouti/api"
	"github.com/sclevine/agouti/task_support/taskcheck"
)

// originPeer is a stateful in-process WebDriver JSON Wire peer used to verify
// origin-tracked reset behavior without starting a real browser.
type originPeer struct {
	t *testing.T

	mutex   sync.Mutex
	current string
	log     []string

	redirects      map[string]string // POST url target -> actual final URL
	actions        map[string]func(*originPeer)
	urlActions     map[string]func(*originPeer)
	failures       map[string]bool // request key -> respond 500
	failURLTargets map[string]bool // POST url target -> fail without changing document
	failClears     map[string]bool // "origin stage" -> fail that storage endpoint
	failScripts    map[string]bool // script substring -> fail execute

	scripts []string
	clears  []string

	pauseKey     string
	pauseEntered chan struct{}
	pauseRelease chan struct{}
	pauseActive  bool
	violations   []string
}

func newOriginPeer(t *testing.T) *originPeer {
	return &originPeer{
		t:              t,
		current:        "about:blank",
		redirects:      map[string]string{},
		actions:        map[string]func(*originPeer){},
		urlActions:     map[string]func(*originPeer){},
		failures:       map[string]bool{},
		failURLTargets: map[string]bool{},
		failClears:     map[string]bool{},
		failScripts:    map[string]bool{},
	}
}

const sessionPrefix = "/session/flow"

func originOf(rawurl string) string {
	u, _ := url.Parse(rawurl)
	return u.Scheme + "://" + u.Host
}

func (p *originPeer) setCurrent(rawurl string) {
	p.mutex.Lock()
	defer p.mutex.Unlock()
	p.current = rawurl
}

func (p *originPeer) setFailure(key string, fail bool) {
	p.mutex.Lock()
	defer p.mutex.Unlock()
	p.failures[key] = fail
}

func (p *originPeer) getURLKey() string { return "GET " + sessionPrefix + "/url" }

func (p *originPeer) entries() []string {
	p.mutex.Lock()
	defer p.mutex.Unlock()
	return append([]string(nil), p.log...)
}

func (p *originPeer) clearRecords() []string {
	p.mutex.Lock()
	defer p.mutex.Unlock()
	return append([]string(nil), p.clears...)
}

func (p *originPeer) executedScripts() []string {
	p.mutex.Lock()
	defer p.mutex.Unlock()
	return append([]string(nil), p.scripts...)
}

func (p *originPeer) currentURL() string {
	p.mutex.Lock()
	defer p.mutex.Unlock()
	return p.current
}

func (p *originPeer) pauseViolations() []string {
	p.mutex.Lock()
	defer p.mutex.Unlock()
	return append([]string(nil), p.violations...)
}

func countEntries(entries []string, want string) int {
	count := 0
	for _, entry := range entries {
		if entry == want {
			count++
		}
	}
	return count
}

func indexOfEntry(entries []string, want string) int {
	for i, entry := range entries {
		if entry == want {
			return i
		}
	}
	return -1
}

func lastIndexOfEntry(entries []string, want string) int {
	last := -1
	for i, entry := range entries {
		if entry == want {
			last = i
		}
	}
	return last
}

func countStrings(items []string, want string) int {
	count := 0
	for _, item := range items {
		if item == want {
			count++
		}
	}
	return count
}

func (p *originPeer) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	key := r.Method + " " + r.URL.Path

	var target, script string
	switch key {
	case "POST " + sessionPrefix + "/url":
		var body struct {
			URL string `json:"url"`
		}
		if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
			p.t.Errorf("failed to decode navigate body: %s", err)
		}
		target = body.URL
	case "POST " + sessionPrefix + "/execute":
		var body struct {
			Script string `json:"script"`
		}
		if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
			p.t.Errorf("failed to decode execute body: %s", err)
		}
		script = body.Script
	}

	entry := strings.Replace(key, " "+sessionPrefix+"/", " ", 1)
	if target != "" {
		entry += " -> " + target
	}

	p.mutex.Lock()
	if p.pauseActive && key != p.pauseKey {
		p.violations = append(p.violations, entry)
	}
	p.log = append(p.log, entry)
	pause := p.pauseKey != "" && key == p.pauseKey && p.pauseEntered != nil
	if pause {
		p.pauseActive = true
	}
	p.mutex.Unlock()

	if pause {
		close(p.pauseEntered)
		<-p.pauseRelease
		p.mutex.Lock()
		p.pauseActive = false
		p.pauseEntered = nil
		p.mutex.Unlock()
	}

	p.mutex.Lock()
	fail := p.failures[key]
	action := p.actions[key]
	redirect, hasRedirect := p.redirects[target]
	urlAction := p.urlActions[target]
	failTarget := p.failURLTargets[target]
	if stage, ok := clearStages[key]; ok {
		if p.failClears[originOf(p.current)+" "+stage] {
			fail = true
		}
	}
	failScript := false
	for substring := range p.failScripts {
		if strings.Contains(script, substring) {
			failScript = true
		}
	}
	p.mutex.Unlock()

	// Navigation effects apply even when the command itself is rejected: the
	// document may already have changed.
	if key == "POST "+sessionPrefix+"/url" && target != "" && !failTarget {
		p.mutex.Lock()
		if hasRedirect {
			p.current = redirect
		} else {
			p.current = target
		}
		p.mutex.Unlock()
	}
	if action != nil {
		action(p)
	}
	if urlAction != nil {
		urlAction(p)
	}

	if fail || failTarget || failScript {
		http.Error(w, `{"value":{"message":"injected failure"}}`, http.StatusInternalServerError)
		return
	}

	value := interface{}(nil)
	switch key {
	case "POST /session":
		fmt.Fprint(w, `{"sessionId":"flow","value":{}}`)
		return
	case "GET " + sessionPrefix + "/url":
		p.mutex.Lock()
		value = p.current
		p.mutex.Unlock()
	case "POST " + sessionPrefix + "/execute":
		p.mutex.Lock()
		p.scripts = append(p.scripts, script)
		if strings.Contains(script, "localStorage.clear();") {
			p.clears = append(p.clears, originOf(p.current)+" local_storage(script)")
		}
		if strings.Contains(script, "sessionStorage.clear();") {
			p.clears = append(p.clears, originOf(p.current)+" session_storage(script)")
		}
		p.mutex.Unlock()
	case "DELETE " + sessionPrefix + "/cookie":
		p.recordClear("cookie")
	case "DELETE " + sessionPrefix + "/local_storage":
		p.recordClear("local_storage")
	case "DELETE " + sessionPrefix + "/session_storage":
		p.recordClear("session_storage")
	case "POST " + sessionPrefix + "/element":
		value = map[string]string{"ELEMENT": "field"}
	case "POST " + sessionPrefix + "/elements":
		value = []map[string]string{{"ELEMENT": "field"}}
	case "POST " + sessionPrefix + "/element/active":
		value = map[string]string{"ELEMENT": "active"}
	case "POST " + sessionPrefix + "/element/field/element":
		value = map[string]string{"ELEMENT": "child"}
	case "POST " + sessionPrefix + "/url",
		"POST " + sessionPrefix + "/forward",
		"POST " + sessionPrefix + "/back",
		"POST " + sessionPrefix + "/refresh",
		"POST " + sessionPrefix + "/accept_alert",
		"POST " + sessionPrefix + "/element/field/click",
		"POST " + sessionPrefix + "/element/field/submit",
		"POST " + sessionPrefix + "/element/field/clear",
		"POST " + sessionPrefix + "/element/field/value",
		"POST " + sessionPrefix + "/element/child/click",
		"POST " + sessionPrefix + "/element/child/submit",
		"POST " + sessionPrefix + "/element/active/click",
		"POST " + sessionPrefix + "/element/active/submit",
		"DELETE " + sessionPrefix:
	default:
		p.t.Errorf("unexpected request: %s", key)
		http.Error(w, `{"value":{"message":"unexpected endpoint"}}`, http.StatusNotFound)
		return
	}
	json.NewEncoder(w).Encode(map[string]interface{}{"value": value})
}

var clearStages = map[string]string{
	"DELETE " + sessionPrefix + "/cookie":          "cookie",
	"DELETE " + sessionPrefix + "/local_storage":   "local_storage",
	"DELETE " + sessionPrefix + "/session_storage": "session_storage",
}

func (p *originPeer) recordClear(stage string) {
	p.mutex.Lock()
	defer p.mutex.Unlock()
	p.clears = append(p.clears, originOf(p.current)+" "+stage)
}

func newTestPage(t *testing.T, peer *originPeer) (*agouti.Page, func()) {
	server := httptest.NewServer(peer)
	page, err := agouti.NewPage(server.URL)
	if err != nil {
		server.Close()
		t.Fatalf("failed to open page: %s", err)
	}
	return page, server.Close
}

func checkPending(c *taskcheck.C, page *agouti.Page, expected ...string) {
	pending := page.OriginResetPending()
	c.Check(len(pending) == len(expected), "pending=%v, expected %v", pending, expected)
	if len(pending) != len(expected) {
		return
	}
	for i, origin := range expected {
		c.Check(pending[i] == origin, "pending[%d]=%s, expected %s (full: %v)", i, pending[i], origin, pending)
	}
}

func TestOriginObservation(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		peer := newOriginPeer(t)
		page, cleanup := newTestPage(t, peer)
		defer cleanup()

		// Default behavior: no tracking, no extra URL probes, original Reset.
		c.Check(page.Navigate("https://pre.invalid/start") == nil, "navigate failed")
		oldElement, err := page.Session().GetElement(api.Selector{Using: "css selector", Value: "#old"})
		c.Check(err == nil, "get old element: %s", err)
		oldSelection := page.Find("#oldsel")

		c.Check(page.Reset() == nil, "default reset failed")
		c.Check(countEntries(peer.entries(), "GET url") == 1, "default reset should probe URL once: %v", peer.entries())
		c.Check(countEntries(peer.entries(), "POST url -> about:blank") == 1, "default reset should end blank: %v", peer.entries())
		c.Check(strings.Join(peer.clearRecords(), ",") == "https://pre.invalid cookie,https://pre.invalid local_storage,https://pre.invalid session_storage",
			"default reset clears: %v", peer.clearRecords())
		c.Check(len(page.OriginResetPending()) == 0, "untracked page must not report pending: %v", page.OriginResetPending())

		// A failed enable observation must not partially enable tracking.
		c.Check(page.Navigate("https://one.invalid/start") == nil, "navigate failed")
		peer.setFailure(peer.getURLKey(), true)
		c.Check(page.EnableOriginReset() != nil, "enable should fail when URL observation fails")
		c.Check(page.Navigate("https://one.invalid/untracked") == nil, "navigate failed")
		c.Check(countEntries(peer.entries(), "GET url") == 2, "failed enable must not add probes: %v", peer.entries())
		peer.setFailure(peer.getURLKey(), false)

		// A malformed reported URL is an enable error as well.
		c.Check(page.Navigate("http://exa mple.invalid/") == nil, "navigate failed")
		c.Check(page.EnableOriginReset() != nil, "enable should fail on malformed reported URL")
		c.Check(page.Navigate("https://one.invalid/start") == nil, "navigate failed")

		c.Check(page.EnableOriginReset() == nil, "enable failed")
		checkPending(c, page, "https://one.invalid")
		c.Check(page.EnableOriginReset() == nil, "re-enable failed")
		checkPending(c, page, "https://one.invalid")

		// Canonical duplicates: scheme/host case, default ports, userinfo,
		// path, query and fragment do not create new entries.
		c.Check(page.Navigate("HTTPS://ONE.INVALID:443/other?x=1#frag") == nil, "navigate failed")
		c.Check(page.Navigate("https://user:secret@two.invalid/form") == nil, "navigate failed")
		c.Check(page.Navigate("http://two.invalid:8080/path") == nil, "navigate failed")
		c.Check(page.Navigate("http://[::1]:8080/ipv6") == nil, "navigate failed")
		c.Check(page.Navigate("http://one.invalid:80/a") == nil, "navigate failed")
		c.Check(page.Navigate("https://one.invalid/another/path") == nil, "navigate failed")
		c.Check(page.Navigate("about:blank") == nil, "navigate failed")
		c.Check(page.Navigate("https://one.invalid/resume") == nil, "navigate failed")
		checkPending(c, page,
			"https://one.invalid",
			"https://two.invalid",
			"http://two.invalid:8080",
			"http://[::1]:8080",
			"http://one.invalid")

		// Returned pending slices are independent copies.
		mutated := page.OriginResetPending()
		c.Check(len(mutated) == 5, "expected 5 pending, got %v", mutated)
		mutated[0] = "https://mutated.invalid"
		c.Check(page.OriginResetPending()[0] == "https://one.invalid", "pending slice mutation leaked: %v", page.OriginResetPending())

		// Redirect destinations are recorded, not the requested URL.
		peer.redirects["https://redir.invalid/go"] = "https://dest.invalid/landing"
		c.Check(page.Navigate("https://redir.invalid/go") == nil, "navigate failed")
		checkPending(c, page,
			"https://one.invalid", "https://two.invalid", "http://two.invalid:8080",
			"http://[::1]:8080", "http://one.invalid", "https://dest.invalid")

		// History navigation and refresh go through the same recording path.
		peer.actions["POST "+sessionPrefix+"/back"] = func(p *originPeer) { p.setCurrent("https://back.invalid/b") }
		peer.actions["POST "+sessionPrefix+"/forward"] = func(p *originPeer) { p.setCurrent("https://forward.invalid/f") }
		c.Check(page.Back() == nil, "back failed")
		c.Check(page.Forward() == nil, "forward failed")
		c.Check(page.Refresh() == nil, "refresh failed")
		checkPending(c, page,
			"https://one.invalid", "https://two.invalid", "http://two.invalid:8080",
			"http://[::1]:8080", "http://one.invalid", "https://dest.invalid",
			"https://back.invalid", "https://forward.invalid")

		// Selection Click/Submit, elements retrieved before enabling, nested
		// elements and the active element are all tracked.
		peer.actions["POST "+sessionPrefix+"/element/field/click"] = func(p *originPeer) { p.setCurrent("https://clicked.invalid/c") }
		c.Check(page.Find("#go").Click() == nil, "selection click failed")
		peer.actions["POST "+sessionPrefix+"/element/field/click"] = func(p *originPeer) { p.setCurrent("https://old.invalid/o") }
		c.Check(oldElement.Click() == nil, "pre-enable element click failed")
		child, err := oldElement.GetElement(api.Selector{Using: "css selector", Value: "#child"})
		c.Check(err == nil, "nested get element: %s", err)
		peer.actions["POST "+sessionPrefix+"/element/child/submit"] = func(p *originPeer) { p.setCurrent("https://nested.invalid/n") }
		c.Check(child.Submit() == nil, "nested element submit failed")
		peer.actions["POST "+sessionPrefix+"/element/field/submit"] = func(p *originPeer) { p.setCurrent("https://submitted.invalid/s") }
		c.Check(oldSelection.Submit() == nil, "pre-enable selection submit failed")
		active, err := page.Session().GetActiveElement()
		c.Check(err == nil, "get active element: %s", err)
		peer.actions["POST "+sessionPrefix+"/element/active/click"] = func(p *originPeer) { p.setCurrent("https://active.invalid/a") }
		c.Check(active.Click() == nil, "active element click failed")

		// Direct Session calls and the raw Send equivalent are tracked too.
		c.Check(page.Session().SetURL("https://direct.invalid/d") == nil, "direct SetURL failed")
		c.Check(page.Session().Refresh() == nil, "direct Refresh failed")
		c.Check(page.Session().Send("POST", "url", map[string]string{"url": "https://raw.invalid/r"}, nil) == nil, "raw Send navigate failed")
		peer.actions["POST "+sessionPrefix+"/element/field/click"] = func(p *originPeer) { p.setCurrent("https://rawclick.invalid/rc") }
		c.Check(page.Session().Send("POST", "element/field/click", nil, nil) == nil, "raw Send click failed")

		expected := []string{
			"https://one.invalid", "https://two.invalid", "http://two.invalid:8080",
			"http://[::1]:8080", "http://one.invalid", "https://dest.invalid",
			"https://back.invalid", "https://forward.invalid", "https://clicked.invalid",
			"https://old.invalid", "https://nested.invalid", "https://submitted.invalid",
			"https://active.invalid", "https://direct.invalid", "https://raw.invalid",
			"https://rawclick.invalid",
		}
		checkPending(c, page, expected...)

		// A rejected command still leaves the document changed: the resulting
		// URL is observed, and the command is never replayed.
		peer.failURLTargets["https://broken.invalid/go"] = true
		peer.urlActions["https://broken.invalid/go"] = func(p *originPeer) { p.setCurrent("https://broken.invalid/after") }
		c.Check(page.Navigate("https://broken.invalid/go") != nil, "navigate should fail")
		c.Check(countEntries(peer.entries(), "POST url -> https://broken.invalid/go") == 1, "command must not be replayed: %v", peer.entries())
		checkPending(c, page, append(append([]string(nil), expected...), "https://broken.invalid")...)
		expected = append(expected, "https://broken.invalid")

		// A failed pre-observation prevents the command from being sent.
		peer.setFailure(peer.getURLKey(), true)
		c.Check(page.Navigate("https://blocked.invalid/x") != nil, "navigate should fail on pre-observation")
		c.Check(countEntries(peer.entries(), "POST url -> https://blocked.invalid/x") == 0, "command must not be sent: %v", peer.entries())
		peer.setFailure(peer.getURLKey(), false)

		// A failed post-observation reports the attempt and leaves a gap that
		// Reset closes before cleaning anything.
		peer.urlActions["https://gap.invalid/go"] = func(p *originPeer) { p.setFailure(p.getURLKey(), true) }
		gapErr := page.Navigate("https://gap.invalid/go")
		c.Check(gapErr != nil && strings.Contains(gapErr.Error(), "attempted"), "gap error should report attempt: %v", gapErr)
		c.Check(countEntries(peer.entries(), "POST url -> https://gap.invalid/go") == 1, "gap command sent once: %v", peer.entries())
		peer.setFailure(peer.getURLKey(), false)
		checkPending(c, page, expected...)

		c.Check(page.Reset() == nil, "tracked reset failed")
		checkPending(c, page)
		c.Check(peer.currentURL() == "about:blank", "reset should end at about:blank, got %s", peer.currentURL())
		c.Check(countStrings(peer.clearRecords(), "https://gap.invalid cookie") == 1, "gap origin must be cleaned: %v", peer.clearRecords())
		c.Check(countStrings(peer.clearRecords(), "https://one.invalid cookie") == 1, "first origin must be cleaned: %v", peer.clearRecords())
		c.Check(countStrings(peer.clearRecords(), "https://rawclick.invalid session_storage") == 1, "last origin must be cleaned: %v", peer.clearRecords())

		// Tracking stays enabled and re-enrolls cleaned origins.
		c.Check(page.Navigate("https://one.invalid/again") == nil, "navigate failed")
		checkPending(c, page, "https://one.invalid")
		c.Check(page.Reset() == nil, "second reset failed")
		checkPending(c, page)

		// A separate session stays independent and untracked.
		peer2 := newOriginPeer(t)
		page2, cleanup2 := newTestPage(t, peer2)
		defer cleanup2()
		c.Check(page2.Navigate("https://other.invalid/x") == nil, "page2 navigate failed")
		c.Check(countEntries(peer2.entries(), "GET url") == 0, "untracked session must not be probed: %v", peer2.entries())
		c.Check(len(page2.OriginResetPending()) == 0, "page2 must not share progress: %v", page2.OriginResetPending())
		c.Check(page2.Destroy() == nil, "page2 destroy failed")

		c.Check(page.Destroy() == nil, "destroy failed")
	})
}

func TestOriginCleanup(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		peer := newOriginPeer(t)
		page, cleanup := newTestPage(t, peer)
		defer cleanup()

		c.Check(page.Navigate("https://alpha.invalid/order") == nil, "navigate failed")
		c.Check(page.EnableOriginReset() == nil, "enable failed")

		// Non-navigation commands pass through without URL probes.
		probes := countEntries(peer.entries(), "GET url")
		c.Check(page.Find("#order").Fill("订单42") == nil, "fill failed")
		c.Check(countEntries(peer.entries(), "GET url") == probes, "fill must not add probes: %v", peer.entries())

		peer.actions["POST "+sessionPrefix+"/element/field/click"] = func(p *originPeer) { p.setCurrent("https://beta.invalid/pay") }
		c.Check(page.Find("#pay").Click() == nil, "click failed")
		peer.actions["POST "+sessionPrefix+"/element/field/submit"] = func(p *originPeer) { p.setCurrent("https://gamma.invalid/confirm") }
		c.Check(page.Find("#confirm").Submit() == nil, "submit failed")
		c.Check(page.Navigate("https://beta.invalid/receipt") == nil, "navigate failed")
		checkPending(c, page, "https://alpha.invalid", "https://beta.invalid", "https://gamma.invalid")

		// Reset cleans every observed origin in first-observed order.
		c.Check(page.Reset() == nil, "reset failed")
		c.Check(strings.Join(peer.clearRecords(), ",") == strings.Join([]string{
			"https://alpha.invalid cookie", "https://alpha.invalid local_storage", "https://alpha.invalid session_storage",
			"https://beta.invalid cookie", "https://beta.invalid local_storage", "https://beta.invalid session_storage",
			"https://gamma.invalid cookie", "https://gamma.invalid local_storage", "https://gamma.invalid session_storage",
		}, ","), "cleanup order: %v", peer.clearRecords())
		entries := peer.entries()
		c.Check(indexOfEntry(entries, "POST url -> https://alpha.invalid/") < indexOfEntry(entries, "POST url -> https://beta.invalid/"),
			"cleanup visits out of order: %v", entries)
		c.Check(indexOfEntry(entries, "POST url -> https://beta.invalid/") < indexOfEntry(entries, "POST url -> https://gamma.invalid/"),
			"cleanup visits out of order: %v", entries)
		c.Check(lastIndexOfEntry(entries, "POST url -> about:blank") > indexOfEntry(entries, "POST url -> https://gamma.invalid/"),
			"blank navigation must be last: %v", entries)
		checkPending(c, page)
		c.Check(peer.currentURL() == "about:blank", "reset should end blank, got %s", peer.currentURL())

		// Storage endpoint failures fall back to scripts; a failing script
		// fallback reports both reasons and stops at that origin.
		c.Check(page.Navigate("https://delta.invalid/x") == nil, "navigate failed")
		peer.failClears["https://delta.invalid local_storage"] = true
		peer.failClears["https://delta.invalid session_storage"] = true
		peer.failScripts["sessionStorage.clear();"] = true
		fallbackErr := page.Reset()
		c.Check(fallbackErr != nil, "reset should fail when session storage fallback fails")
		c.Check(strings.Contains(fallbackErr.Error(), "session storage") && strings.Contains(fallbackErr.Error(), "script fallback"),
			"error should carry both reasons: %v", fallbackErr)
		c.Check(countStrings(peer.executedScripts(), "return (function() { localStorage.clear();; }).apply(this, arguments);") == 1,
			"local storage script fallback expected: %v", peer.executedScripts())
		c.Check(countStrings(peer.clearRecords(), "https://delta.invalid cookie") == 1,
			"delta cookies must be cleared once: %v", peer.clearRecords())
		c.Check(countStrings(peer.clearRecords(), "https://delta.invalid local_storage(script)") == 1,
			"delta local storage must fall back to script: %v", peer.clearRecords())
		c.Check(countStrings(peer.clearRecords(), "https://delta.invalid session_storage") == 0 &&
			countStrings(peer.clearRecords(), "https://delta.invalid session_storage(script)") == 0,
			"delta session storage must remain uncleared: %v", peer.clearRecords())
		checkPending(c, page, "https://delta.invalid")
		peer.failClears = map[string]bool{}
		peer.failScripts = map[string]bool{}

		// A cleanup redirect is an error and clears nothing for that visit.
		c.Check(page.Navigate("https://epsilon.invalid/y") == nil, "navigate failed")
		peer.redirects["https://delta.invalid/"] = "https://evil.invalid/"
		redirectErr := page.Reset()
		c.Check(redirectErr != nil && strings.Contains(redirectErr.Error(), "redirected"), "cleanup redirect must fail: %v", redirectErr)
		checkPending(c, page, "https://delta.invalid", "https://epsilon.invalid")
		c.Check(countStrings(peer.clearRecords(), "https://delta.invalid cookie") == 1, "redirected origin must not be re-cleared: %v", peer.clearRecords())
		for _, clear := range peer.clearRecords() {
			c.Check(!strings.HasPrefix(clear, "https://evil.invalid"), "unexpected origin must not be cleared: %v", peer.clearRecords())
		}
		delete(peer.redirects, "https://delta.invalid/")

		// Retrying resumes at the failed origin from cookie deletion.
		c.Check(page.Reset() == nil, "retry reset failed")
		c.Check(countStrings(peer.clearRecords(), "https://delta.invalid cookie") == 2, "failed origin must retry from cookies: %v", peer.clearRecords())
		c.Check(countStrings(peer.clearRecords(), "https://delta.invalid session_storage") == 1, "retry must finish storage: %v", peer.clearRecords())
		c.Check(countStrings(peer.clearRecords(), "https://epsilon.invalid cookie") == 1, "later origin must be cleaned: %v", peer.clearRecords())
		checkPending(c, page)

		// Cleanup itself never enrolls origins; business navigation does.
		c.Check(page.Navigate("https://alpha.invalid/new") == nil, "navigate failed")
		checkPending(c, page, "https://alpha.invalid")
		c.Check(page.Reset() == nil, "final reset failed")
		checkPending(c, page)

		c.Check(page.Destroy() == nil, "destroy failed")
	})
}

func TestOriginCleanupRecovery(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		peer := newOriginPeer(t)
		page, cleanup := newTestPage(t, peer)
		defer cleanup()

		c.Check(page.Navigate("https://a.invalid/1") == nil, "navigate failed")
		c.Check(page.EnableOriginReset() == nil, "enable failed")
		peer.actions["POST "+sessionPrefix+"/element/field/click"] = func(p *originPeer) { p.setCurrent("https://b.invalid/2") }
		c.Check(page.Find("#next").Click() == nil, "click failed")
		c.Check(page.Navigate("https://c.invalid/3") == nil, "navigate failed")
		checkPending(c, page, "https://a.invalid", "https://b.invalid", "https://c.invalid")

		// Cleanup stops at the first failed origin and keeps it and the rest.
		peer.failClears["https://b.invalid local_storage"] = true
		peer.failScripts["localStorage.clear();"] = true
		failErr := page.Reset()
		c.Check(failErr != nil, "reset should fail")
		c.Check(strings.Contains(failErr.Error(), "local storage") && strings.Contains(failErr.Error(), "script fallback"),
			"error should carry both reasons: %v", failErr)
		checkPending(c, page, "https://b.invalid", "https://c.invalid")
		c.Check(strings.Join(peer.clearRecords(), ",") == strings.Join([]string{
			"https://a.invalid cookie", "https://a.invalid local_storage", "https://a.invalid session_storage",
			"https://b.invalid cookie",
		}, ","), "partial progress: %v", peer.clearRecords())

		// The next Reset retries the failed origin from cookie deletion and
		// does not revisit completed origins.
		peer.failClears = map[string]bool{}
		peer.failScripts = map[string]bool{}
		c.Check(page.Reset() == nil, "retry reset failed")
		checkPending(c, page)
		c.Check(countStrings(peer.clearRecords(), "https://a.invalid cookie") == 1, "completed origin must not be revisited: %v", peer.clearRecords())
		c.Check(countStrings(peer.clearRecords(), "https://b.invalid cookie") == 2, "failed origin must retry from cookies: %v", peer.clearRecords())
		c.Check(countStrings(peer.clearRecords(), "https://b.invalid local_storage") == 1, "retry must clear storage: %v", peer.clearRecords())
		c.Check(countStrings(peer.clearRecords(), "https://c.invalid session_storage") == 1, "later origin must be cleaned: %v", peer.clearRecords())
		c.Check(peer.currentURL() == "about:blank", "reset should end blank, got %s", peer.currentURL())

		// A failed final blank navigation reports an error with empty pending;
		// the next Reset only retries that navigation.
		c.Check(page.Navigate("https://d.invalid/x") == nil, "navigate failed")
		peer.failURLTargets["about:blank"] = true
		blankErr := page.Reset()
		c.Check(blankErr != nil, "reset should fail on final blank navigation")
		checkPending(c, page)
		c.Check(countStrings(peer.clearRecords(), "https://d.invalid cookie") == 1, "d must be cleaned before blank: %v", peer.clearRecords())
		c.Check(peer.currentURL() == "https://d.invalid/", "failed blank navigation must not move: %s", peer.currentURL())
		clearsBefore := len(peer.clearRecords())
		delete(peer.failURLTargets, "about:blank")
		c.Check(page.Reset() == nil, "blank-only retry failed")
		c.Check(len(peer.clearRecords()) == clearsBefore, "completed origins must not be re-cleared: %v", peer.clearRecords())
		c.Check(peer.currentURL() == "about:blank", "retry should end blank, got %s", peer.currentURL())
		checkPending(c, page)

		// A concurrent navigation must wait for a paused cleanup to finish.
		c.Check(page.Navigate("https://e.invalid/1") == nil, "navigate failed")
		c.Check(page.Navigate("https://f.invalid/2") == nil, "navigate failed")
		checkPending(c, page, "https://e.invalid", "https://f.invalid")

		peer.pauseKey = "DELETE " + sessionPrefix + "/local_storage"
		peer.pauseEntered = make(chan struct{})
		peer.pauseRelease = make(chan struct{})

		resetDone := make(chan error, 1)
		go func() { resetDone <- page.Reset() }()

		select {
		case <-peer.pauseEntered:
		case <-time.After(10 * time.Second):
			c.Check(false, "cleanup did not reach the pause point")
			close(peer.pauseRelease)
			return
		}

		navStarted := make(chan struct{})
		navDone := make(chan error, 1)
		go func() {
			close(navStarted)
			navDone <- page.Session().SetURL("https://nav.invalid/concurrent")
		}()
		select {
		case <-navStarted:
		case <-time.After(10 * time.Second):
			c.Check(false, "concurrent navigation did not start")
			close(peer.pauseRelease)
			return
		}
		close(peer.pauseRelease)

		select {
		case err := <-resetDone:
			c.Check(err == nil, "paused reset failed: %v", err)
		case <-time.After(10 * time.Second):
			c.Check(false, "paused reset did not finish")
			return
		}
		select {
		case err := <-navDone:
			c.Check(err == nil, "concurrent navigation failed: %v", err)
		case <-time.After(10 * time.Second):
			c.Check(false, "concurrent navigation did not finish")
			return
		}

		c.Check(len(peer.pauseViolations()) == 0, "requests sent while cleanup paused: %v", peer.pauseViolations())
		entries := peer.entries()
		blankIndex := lastIndexOfEntry(entries, "POST url -> about:blank")
		navIndex := indexOfEntry(entries, "POST url -> https://nav.invalid/concurrent")
		c.Check(blankIndex != -1 && navIndex != -1 && blankIndex < navIndex,
			"concurrent navigation must wait for cleanup: %v", entries)
		c.Check(countStrings(peer.clearRecords(), "https://e.invalid cookie") == 1, "e must be cleaned: %v", peer.clearRecords())
		c.Check(countStrings(peer.clearRecords(), "https://f.invalid session_storage") == 1, "f must be cleaned: %v", peer.clearRecords())
		checkPending(c, page, "https://nav.invalid")

		c.Check(page.Reset() == nil, "final reset failed")
		checkPending(c, page)
		c.Check(page.Destroy() == nil, "destroy failed")
	})
}
