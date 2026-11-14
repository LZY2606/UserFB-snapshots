package agouti_test

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strings"
	"sync"
	"testing"

	"github.com/sclevine/agouti"
	"github.com/sclevine/agouti/task_support/taskcheck"
)

// frameDriver is an in-memory JsonWireProtocol state machine. It implements
// http.RoundTripper so that agouti.JoinPage can drive it without binding a
// real TCP port. It records every request together with the frame the
// session was focused on when the request arrived.
type frameDriver struct {
	mu sync.Mutex

	frame    string
	values   map[string]string
	commands []string

	frameVersion int

	// failNilFrameAt counts down on every root-frame switch; the switch that
	// drives it to zero fails. 1 fails the next switch, 2 the one after, etc.
	failNilFrameAt int

	// failNextFrameSwitch makes the next non-nil frame switch fail.
	failNextFrameSwitch bool

	// valueFailIDs marks element IDs whose /value endpoint returns an error.
	valueFailIDs map[string]bool

	gate *requestGate
}

type requestGate struct {
	match   func(endpoint string) bool
	ch      chan struct{}
	reached chan struct{}
	once    sync.Once
}

func newFrameDriver() *frameDriver {
	return &frameDriver{
		frame:        "root",
		values:       map[string]string{"root-field": "root-text"},
		frameVersion: 1,
		valueFailIDs: map[string]bool{},
	}
}

func (d *frameDriver) logf(format string, args ...interface{}) {
	d.commands = append(d.commands, fmt.Sprintf(format, args...))
}

func (d *frameDriver) snapshot() []string {
	d.mu.Lock()
	defer d.mu.Unlock()
	return append([]string(nil), d.commands...)
}

func (d *frameDriver) len() int {
	d.mu.Lock()
	defer d.mu.Unlock()
	return len(d.commands)
}

func (d *frameDriver) replaceFrames() {
	d.mu.Lock()
	d.frameVersion++
	d.mu.Unlock()
}

func (d *frameDriver) hold(match func(string) bool) *requestGate {
	g := &requestGate{match: match, ch: make(chan struct{}), reached: make(chan struct{})}
	d.mu.Lock()
	d.gate = g
	d.mu.Unlock()
	return g
}

func (d *frameDriver) releaseGate() {
	d.mu.Lock()
	g := d.gate
	d.gate = nil
	d.mu.Unlock()
	if g != nil {
		close(g.ch)
	}
}

func (d *frameDriver) orderFrameID() string {
	return fmt.Sprintf("order-frame-%d", d.frameVersion)
}

func (d *frameDriver) contactFrameID() string {
	return fmt.Sprintf("contact-frame-%d", d.frameVersion)
}

func (d *frameDriver) resolve(css string) []string {
	switch d.frame {
	case "root":
		switch css {
		case "#order":
			return []string{d.orderFrameID()}
		case "#root-field":
			return []string{"root-field"}
		}
	case "outer":
		switch css {
		case "#contact":
			return []string{d.contactFrameID()}
		case "input.name":
			return []string{"outer-name"}
		case "input.mail":
			return []string{"outer-mail"}
		}
	case "contact":
		switch css {
		case "input.name":
			return []string{"contact-name"}
		case "input.cell":
			return []string{"contact-cell-1", "contact-cell-2"}
		}
	}
	return nil
}

func (d *frameDriver) respond(req *http.Request, status int, value interface{}) *http.Response {
	body, _ := json.Marshal(map[string]interface{}{"value": value})
	return &http.Response{
		StatusCode: status,
		Header:     http.Header{"Content-Type": []string{"application/json"}},
		Body:       io.NopCloser(bytes.NewReader(body)),
		Request:    req,
	}
}

func (d *frameDriver) fail(req *http.Request, message string) *http.Response {
	return d.respond(req, http.StatusInternalServerError,
		map[string]string{"message": fmt.Sprintf(`{"errorMessage":%q}`, message)})
}

func (d *frameDriver) RoundTrip(req *http.Request) (*http.Response, error) {
	d.mu.Lock()
	defer d.mu.Unlock()

	endpoint := strings.TrimPrefix(req.URL.Path, "/session/s/")
	rawBody, _ := io.ReadAll(req.Body)

	if d.gate != nil && d.gate.match(endpoint) {
		gate := d.gate
		gate.once.Do(func() { close(gate.reached) })
		d.mu.Unlock()
		<-gate.ch
		d.mu.Lock()
	}

	switch {
	case req.Method == "POST" && strings.HasSuffix(endpoint, "/elements"):
		// Selector query scoped to a previously resolved element.
		var query struct {
			Using string `json:"using"`
			Value string `json:"value"`
		}
		_ = json.Unmarshal(rawBody, &query)
		ids := d.resolve(query.Value)
		d.logf("POST %s %q @%s", endpoint, query.Value, d.frame)
		results := make([]map[string]string, 0, len(ids))
		for _, id := range ids {
			results = append(results, map[string]string{"ELEMENT": id})
		}
		return d.respond(req, http.StatusOK, results), nil

	case req.Method == "POST" && endpoint == "element":
		var query struct {
			Using string `json:"using"`
			Value string `json:"value"`
		}
		_ = json.Unmarshal(rawBody, &query)
		ids := d.resolve(query.Value)
		d.logf("POST element %q @%s", query.Value, d.frame)
		if len(ids) == 0 {
			return d.fail(req, "element not found"), nil
		}
		return d.respond(req, http.StatusOK, map[string]string{"ELEMENT": ids[0]}), nil

	case req.Method == "POST" && endpoint == "elements":
		var query struct {
			Using string `json:"using"`
			Value string `json:"value"`
		}
		_ = json.Unmarshal(rawBody, &query)
		ids := d.resolve(query.Value)
		d.logf("POST elements %q @%s", query.Value, d.frame)
		results := make([]map[string]string, 0, len(ids))
		for _, id := range ids {
			results = append(results, map[string]string{"ELEMENT": id})
		}
		return d.respond(req, http.StatusOK, results), nil

	case req.Method == "POST" && endpoint == "frame":
		current := d.frame
		var body struct {
			ID *struct {
				Element string `json:"ELEMENT"`
			} `json:"id"`
		}
		_ = json.Unmarshal(rawBody, &body)
		if body.ID == nil {
			if d.failNilFrameAt > 0 {
				d.failNilFrameAt--
				if d.failNilFrameAt == 0 {
					d.logf("POST frame root @%s -> ERROR", current)
					return d.fail(req, "root frame switch failed"), nil
				}
			}
			d.frame = "root"
			d.logf("POST frame root @%s", current)
			return d.respond(req, http.StatusOK, nil), nil
		}
		if d.failNextFrameSwitch {
			d.failNextFrameSwitch = false
			d.logf("POST frame %s @%s -> ERROR", body.ID.Element, current)
			return d.fail(req, "frame switch failed"), nil
		}
		switch body.ID.Element {
		case d.orderFrameID():
			d.frame = "outer"
		case d.contactFrameID():
			d.frame = "contact"
		}
		d.logf("POST frame %s @%s", body.ID.Element, current)
		return d.respond(req, http.StatusOK, nil), nil

	case strings.HasSuffix(endpoint, "/text"):
		id := strings.TrimSuffix(strings.TrimPrefix(endpoint, "element/"), "/text")
		d.logf("GET %s @%s", endpoint, d.frame)
		return d.respond(req, http.StatusOK, d.values[id]), nil

	case strings.HasSuffix(endpoint, "/clear"):
		id := strings.TrimSuffix(strings.TrimPrefix(endpoint, "element/"), "/clear")
		d.values[id] = ""
		d.logf("POST %s @%s", endpoint, d.frame)
		return d.respond(req, http.StatusOK, nil), nil

	case strings.HasSuffix(endpoint, "/value"):
		id := strings.TrimSuffix(strings.TrimPrefix(endpoint, "element/"), "/value")
		if d.valueFailIDs[id] {
			d.logf("POST %s @%s -> ERROR", endpoint, d.frame)
			return d.fail(req, "value endpoint failed for "+id), nil
		}
		var body struct {
			Value []string `json:"value"`
		}
		_ = json.Unmarshal(rawBody, &body)
		d.values[id] = strings.Join(body.Value, "")
		d.logf("POST %s @%s", endpoint, d.frame)
		return d.respond(req, http.StatusOK, nil), nil

	case strings.HasSuffix(endpoint, "/click"):
		d.logf("POST %s @%s", endpoint, d.frame)
		return d.respond(req, http.StatusOK, nil), nil

	default:
		d.logf("%s %s @%s -> UNEXPECTED", req.Method, endpoint, d.frame)
		return d.fail(req, "unexpected endpoint "+endpoint), nil
	}
}

func joinPageWithDriver(d *frameDriver) *agouti.Page {
	client := &http.Client{Transport: roundTripFunc(d.RoundTrip)}
	return agouti.JoinPage("http://frame.driver/session/s", agouti.HTTPClient(client))
}

type roundTripFunc func(*http.Request) (*http.Response, error)

func (f roundTripFunc) RoundTrip(req *http.Request) (*http.Response, error) {
	return f(req)
}

func joined(commands []string) string {
	return strings.Join(commands, "|")
}

func indexOf(commands []string, needle string) int {
	for i, command := range commands {
		if strings.Contains(command, needle) {
			return i
		}
	}
	return -1
}

func TestFrameScopePath(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		driver := newFrameDriver()
		page := joinPageWithDriver(driver)

		// A two-level path resolves each frame segment separately, from the
		// root frame down, and leaves the session back at the root.
		contactScope := page.InFrames("#order", "#contact")
		nameField := contactScope.Find("input.name")

		c.Check(nameField.Fill("林") == nil, "Fill through two frames failed")
		commands := driver.snapshot()
		log := joined(commands)
		c.Check(strings.Contains(log, `POST elements "#order" @root`), "outer iframe not resolved at root: %s", log)
		c.Check(strings.Contains(log, "POST frame order-frame-1 @root"), "did not switch into outer frame: %s", log)
		c.Check(strings.Contains(log, `POST elements "#contact" @outer`), "inner iframe not resolved in outer frame: %s", log)
		c.Check(strings.Contains(log, "POST frame contact-frame-1 @outer"), "did not switch into inner frame: %s", log)
		c.Check(strings.Contains(log, `POST elements "input.name" @contact`), "field not resolved in inner frame: %s", log)
		c.Check(strings.Contains(log, "POST element/contact-name/clear @contact"), "clear not sent in inner frame: %s", log)
		c.Check(strings.Contains(log, "POST element/contact-name/value @contact"), "value not sent in inner frame: %s", log)
		c.Check(strings.HasPrefix(commands[0], "POST frame root"), "operation did not start at root: %s", commands[0])
		c.Check(strings.HasPrefix(commands[len(commands)-1], "POST frame root"), "operation did not recover root: %s", commands[len(commands)-1])
		c.Check(!strings.Contains(log, "#order #contact"), "frame segments merged into descendant CSS: %s", log)
		c.Check(driver.values["contact-name"] == "林", "filled value not recorded, got %q", driver.values["contact-name"])

		text, err := contactScope.Find("input.name").Text()
		c.Check(err == nil && text == "林", "Text through frames = %q, %v", text, err)

		// Derived scopes append nested paths without mutating the parent.
		orderScope := page.InFrames("#order")
		derivedContact := orderScope.InFrames("#contact")
		derivedText, err := derivedContact.Find("input.name").Text()
		c.Check(err == nil && derivedText == "林", "derived scope Text = %q, %v", derivedText, err)

		mailField := orderScope.Find("input.mail")
		c.Check(mailField.Fill("a@b.c") == nil, "Fill through derived outer scope failed")
		c.Check(driver.values["outer-mail"] == "a@b.c", "outer frame fill not recorded")

		// Selection chains and MultiSelection.At carry the same frame path.
		cells := contactScope.All("input.cell")
		count, err := cells.Count()
		c.Check(err == nil && count == 2, "Count in inner frame = %d, %v", count, err)

		c.Check(cells.At(0).Click() == nil, "At(0).Click through frames failed")
		log = joined(driver.snapshot())
		c.Check(strings.Contains(log, "POST element/contact-cell-1/click @contact"), "chained At click not sent in inner frame: %s", log)

		chained := contactScope.Find("input.name").All("input.cell").At(1)
		c.Check(chained.Click() == nil, "long selector chain Click failed")
		log = joined(driver.snapshot())
		c.Check(strings.Contains(log, "POST element/contact-cell-2/click @contact"), "chained selection lost frame path: %s", log)

		// A replaced iframe is located by its new ID: no handle is cached.
		driver.replaceFrames()
		before := len(driver.snapshot())
		_, err = contactScope.Find("input.name").Text()
		c.Check(err == nil, "Text after frame replacement failed: %v", err)
		replacementLog := joined(driver.snapshot()[before:])
		c.Check(strings.Contains(replacementLog, "POST frame order-frame-2 @root"), "replaced outer frame not re-resolved: %s", replacementLog)
		c.Check(strings.Contains(replacementLog, "POST frame contact-frame-2 @outer"), "replaced inner frame not re-resolved: %s", replacementLog)
		c.Check(!strings.Contains(replacementLog, "order-frame-1"), "stale outer frame handle reused: %s", replacementLog)
		c.Check(!strings.Contains(replacementLog, "contact-frame-1"), "stale inner frame handle reused: %s", replacementLog)

		// A zero-segment path is the root page: no frame switch is sent.
		rootBefore := driver.len()
		rootText, err := page.InFrames().Find("#root-field").Text()
		c.Check(err == nil && rootText == "root-text", "zero-segment scope Text = %q, %v", rootText, err)
		rootLog := joined(driver.snapshot()[rootBefore:])
		c.Check(!strings.Contains(rootLog, "POST frame"), "zero-segment scope switched frames: %s", rootLog)

		// An empty CSS segment errors before any request is sent.
		emptyBefore := driver.len()
		_, err = page.InFrames("#order", "   ").Find("input.name").Text()
		c.Check(err != nil, "empty frame segment did not error")
		c.Check(driver.len() == emptyBefore, "requests sent for empty frame segment")

		// Root selections created before the scopes keep working and never
		// leave the root frame themselves.
		oldRoot := page.Find("#root-field")
		oldText, err := oldRoot.Text()
		c.Check(err == nil && oldText == "root-text", "pre-existing root selection Text = %q, %v", oldText, err)
	})
}

func TestFrameScopeAtomic(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		driver := newFrameDriver()
		page := joinPageWithDriver(driver)

		orderScope := page.InFrames("#order")
		contactScope := page.InFrames("#order", "#contact")
		oldRoot := page.Find("#root-field")

		// Hold a Fill inside the outer frame at its Value request.
		gate := driver.hold(func(endpoint string) bool {
			return strings.Contains(endpoint, "outer-name/value")
		})
		fillErr := make(chan error, 1)
		go func() {
			fillErr <- orderScope.Find("input.name").Fill("林")
		}()
		<-gate.reached

		// While the Fill holds the Page boundary, a root selection and a
		// second scope cannot start any request.
		rootDone := make(chan string, 1)
		go func() {
			text, err := oldRoot.Text()
			rootDone <- fmt.Sprintf("%s:%v", text, err)
		}()
		contactDone := make(chan error, 1)
		go func() {
			_, err := contactScope.Find("input.name").Count()
			contactDone <- err
		}()

		// The Fill block is contiguous up to its held Value request.
		pendingLog := joined(driver.snapshot())
		c.Check(strings.Contains(pendingLog, "POST element/outer-name/clear @outer"), "Fill did not reach Clear before blocking: %s", pendingLog)
		firstRoot := indexOf(driver.snapshot(), "POST frame root")
		block := joined(driver.snapshot()[firstRoot:])
		c.Check(!strings.Contains(block, "root-field"), "another selection sent requests inside the Fill boundary: %s", block)
		c.Check(!strings.Contains(block, "contact-frame"), "second scope sent requests inside the Fill boundary: %s", block)

		driver.releaseGate()
		c.Check(<-fillErr == nil, "blocked Fill returned an error")
		rootResult := <-rootDone
		c.Check(rootResult == "root-text:<nil>", "root selection result after boundary release wrong: %s", rootResult)
		c.Check(<-contactDone == nil, "second scope operation failed")

		// Every scoped operation forms one contiguous block bracketed by
		// root-frame switches, with its field requests at the deepest frame.
		commands := driver.snapshot()
		log := joined(commands)
		c.Check(strings.Contains(log, "POST element/outer-name/value @outer"), "Fill Value missing: %s", log)
		fillStart := indexOf(commands, "POST frame root")
		fillValue := indexOf(commands, "outer-name/value")
		fillRecovery := -1
		for i := fillValue + 1; i < len(commands); i++ {
			if strings.HasPrefix(commands[i], "POST frame root") {
				fillRecovery = i
				break
			}
		}
		c.Check(fillRecovery > fillValue, "Fill did not recover root after Value: %s", log)
		for i := fillStart; i <= fillRecovery; i++ {
			c.Check(!strings.Contains(commands[i], "root-field") && !strings.Contains(commands[i], "contact-frame"),
				"Fill block interleaved with another selection: %s", commands[i])
		}

		contactStart := indexOf(commands, "contact-frame-1 @outer")
		c.Check(contactStart > fillRecovery, "second scope started before Fill released its boundary")
		contactRecovery := -1
		for i := contactStart + 1; i < len(commands); i++ {
			if strings.HasPrefix(commands[i], "POST frame root") {
				contactRecovery = i
				break
			}
		}
		c.Check(contactRecovery > contactStart, "second scope did not recover root")
		c.Check(strings.Contains(joined(commands[contactStart:contactRecovery]), `POST elements "input.name" @contact`),
			"second scope field not resolved in inner frame")

		// Fill uses one element snapshot: one elements query, then Clear and
		// Value per element in order. A Value failure stops without re-Clear.
		driver2 := newFrameDriver()
		page2 := joinPageWithDriver(driver2)
		driver2.valueFailIDs["contact-cell-2"] = true
		cells := page2.InFrames("#order", "#contact").All("input.cell")
		err := cells.Fill("x")
		c.Check(err != nil && strings.Contains(err.Error(), "failed to enter text"), "Value failure not propagated: %v", err)
		block2 := joined(driver2.snapshot())
		c.Check(strings.Contains(block2, "POST element/contact-cell-1/clear @contact"), "first Clear missing: %s", block2)
		c.Check(strings.Contains(block2, "POST element/contact-cell-1/value @contact"), "first Value missing: %s", block2)
		c.Check(strings.Contains(block2, "POST element/contact-cell-2/clear @contact"), "second Clear missing: %s", block2)
		c.Check(strings.Contains(block2, "POST element/contact-cell-2/value @contact"), "failed Value missing: %s", block2)
		clearCount := strings.Count(block2, "/clear @")
		valueCount := strings.Count(block2, "/value @")
		c.Check(clearCount == 2 && valueCount == 2, "expected one Clear and one Value per element, got %d clears %d values: %s", clearCount, valueCount, block2)
		c.Check(indexOf(driver2.snapshot(), "contact-cell-2/value") > indexOf(driver2.snapshot(), "contact-cell-2/clear"),
			"Value did not follow Clear for second element")

		// Different Pages do not block each other.
		otherDriver := newFrameDriver()
		otherPage := joinPageWithDriver(otherDriver)
		gate2 := driver.hold(func(endpoint string) bool {
			return strings.Contains(endpoint, "outer-name/value")
		})
		blockedErr := make(chan error, 1)
		go func() {
			blockedErr <- orderScope.Find("input.name").Fill("held")
		}()
		<-gate2.reached

		otherText, otherErr := otherPage.InFrames("#order").Find("input.name").Text()
		c.Check(otherErr == nil && otherText == "", "other Page should not block: %q %v", otherText, otherErr)
		otherLog := joined(otherDriver.snapshot())
		c.Check(strings.Contains(otherLog, "POST frame order-frame-1 @root"), "other Page could not run its own frame path: %s", otherLog)

		select {
		case err := <-blockedErr:
			c.Check(false, "blocked Fill completed while gate closed: %v", err)
		default:
			c.Check(true, "")
		}
		driver.releaseGate()
		c.Check(<-blockedErr == nil, "held Fill failed after release")
	})
}

func TestFrameScopeRecovery(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		driver := newFrameDriver()
		page := joinPageWithDriver(driver)
		orderScope := page.InFrames("#order")
		oldRoot := page.Find("#root-field")

		// Failure while entering a frame: the operation still returns to the
		// root frame and the Page context remains usable.
		driver.failNextFrameSwitch = true
		_, err := orderScope.Find("input.name").Text()
		c.Check(err != nil && strings.Contains(err.Error(), "failed to switch to frame"), "frame entry failure not returned: %v", err)
		log := joined(driver.snapshot())
		c.Check(strings.Contains(log, "POST frame order-frame-1 @root -> ERROR"), "frame switch failure not recorded: %s", log)
		c.Check(strings.HasSuffix(log, "POST frame root @root"), "root not recovered after frame-entry failure: %s", log)
		_, err = oldRoot.Text()
		c.Check(err == nil, "root selection blocked after recoverable frame failure: %v", err)

		// A missing iframe selector is the other frame-entry failure mode.
		missingBefore := driver.len()
		_, err = page.InFrames("#does-not-exist").Find("input.name").Text()
		c.Check(err != nil && strings.Contains(err.Error(), "element not found"), "missing iframe error wrong: %v", err)
		missingLog := joined(driver.snapshot()[missingBefore:])
		c.Check(strings.Contains(missingLog, `POST elements "#does-not-exist" @root`), "missing iframe not queried: %s", missingLog)
		c.Check(strings.HasSuffix(missingLog, "POST frame root @root"), "root not recovered after missing iframe: %s", missingLog)

		// Failure of the initial return-to-root aborts before path resolution
		// and does not poison the Page context.
		driver.failNilFrameAt = 1
		before := driver.len()
		_, err = orderScope.Find("input.name").Text()
		c.Check(err != nil, "initial root failure did not return an error")
		initialLog := joined(driver.snapshot()[before:])
		c.Check(!strings.Contains(initialLog, "#order"), "path resolved after initial root failure: %s", initialLog)
		driver.failNilFrameAt = 0
		_, err = oldRoot.Text()
		c.Check(err == nil, "Page context poisoned by initial root failure: %v", err)

		// Both the field failure and the failed root recovery are retained,
		// and the Page context becomes uncertain.
		driver.failNilFrameAt = 2
		driver.valueFailIDs["outer-name"] = true
		err = orderScope.Find("input.name").Fill("林")
		c.Check(err != nil, "combined failure did not return an error")
		c.Check(errors.Is(err, agouti.ErrFrameContextUnknown), "combined failure not recognized as ErrFrameContextUnknown: %v", err)
		c.Check(strings.Contains(err.Error(), "failed to enter text"), "field error lost: %v", err)
		c.Check(strings.Contains(err.Error(), "failed to return to root frame"), "recovery error lost: %v", err)

		// All four entries on old root selections and new scopes now fail
		// with the sentinel before any request leaves the process.
		for _, op := range []struct {
			name string
			call func() error
		}{
			{"root Count", func() error { _, e := oldRoot.Count(); return e }},
			{"root Text", func() error { _, e := oldRoot.Text(); return e }},
			{"root Click", func() error { return oldRoot.Click() }},
			{"root Fill", func() error { return oldRoot.Fill("x") }},
			{"scope Count", func() error { _, e := orderScope.All("input.name").Count(); return e }},
			{"scope Text", func() error { _, e := orderScope.Find("input.name").Text(); return e }},
			{"scope Click", func() error { return orderScope.Find("input.name").Click() }},
			{"scope Fill", func() error { return orderScope.Find("input.name").Fill("x") }},
		} {
			blockedAt := driver.len()
			err := op.call()
			c.Check(errors.Is(err, agouti.ErrFrameContextUnknown), "%s did not return ErrFrameContextUnknown: %v", op.name, err)
			c.Check(driver.len() == blockedAt, "%s sent a request while context unknown", op.name)
		}

		// Only an explicit successful SwitchToRootFrame clears the state.
		driver.failNilFrameAt = 1
		err = page.SwitchToRootFrame()
		c.Check(err != nil, "SwitchToRootFrame unexpectedly succeeded while failing")
		_, err = oldRoot.Text()
		c.Check(errors.Is(err, agouti.ErrFrameContextUnknown), "failed recovery cleared the uncertain state: %v", err)

		driver.failNilFrameAt = 0
		driver.valueFailIDs["outer-name"] = false
		c.Check(page.SwitchToRootFrame() == nil, "explicit root recovery failed")

		rootText, err := oldRoot.Text()
		c.Check(err == nil && rootText == "root-text", "old root selection unusable after recovery: %q %v", rootText, err)
		scopeText, err := orderScope.Find("input.name").Text()
		c.Check(err == nil, "frame scope unusable after recovery: %v", err)
		c.Check(scopeText == "", "cleared field should be empty, got %q", scopeText)
	})
}
