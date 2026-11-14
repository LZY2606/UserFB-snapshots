package agouti_test

import (
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"reflect"
	"strings"
	"sync"
	"testing"

	"github.com/sclevine/agouti"
	"github.com/sclevine/agouti/task_support/taskcheck"
)

// frControl is one control (or option) in the fake WebDriver DOM.
type frControl struct {
	id       string
	tag      string // "input", "textarea", "select", "option", "div"
	typ      string // input type
	name     string
	value    string
	enabled  bool
	selected bool
	multiple bool
	options  []*frControl
}

// frForm is one <form> on the fake page.
type frForm struct {
	id       string
	selector string // the only CSS selector that matches this form
	controls []*frControl
}

// frSession is an independent WebDriver session: its own page, its own
// element IDs, its own request log. Element IDs of replaced forms are
// unregistered, so any request that still references them is rejected
// with a stale-element 404.
type frSession struct {
	id       string
	nextID   int
	forms    []*frForm
	extras   map[string]*frControl // selector -> non-form element
	live     map[string]interface{}
	log      []string
	onAction func(sess *frSession, action string, control *frControl)
}

// frServer is a stateful in-memory WebDriver serving any number of sessions.
type frServer struct {
	t        *testing.T
	mu       sync.Mutex
	sessions map[string]*frSession
}

func newFRServer(t *testing.T) *frServer {
	return &frServer{t: t, sessions: map[string]*frSession{}}
}

func (s *frServer) addSession(id string, forms ...*frForm) *frSession {
	s.mu.Lock()
	defer s.mu.Unlock()
	sess := &frSession{id: id, live: map[string]interface{}{}, extras: map[string]*frControl{}}
	for _, form := range forms {
		sess.forms = append(sess.forms, form)
		sess.registerForm(form)
	}
	s.sessions[id] = sess
	return sess
}

// joinPage points a real agouti.Page at this server through JoinPage, so the
// tests exercise the full public Selection -> WebDriver protocol chain.
func (s *frServer) joinPage(t *testing.T, sessionID string) *agouti.Page {
	server := httptest.NewServer(s)
	t.Cleanup(server.Close)
	return agouti.JoinPage(server.URL + "/session/" + sessionID)
}

func (sess *frSession) newID() string {
	sess.nextID++
	return fmt.Sprintf("%s-e%03d", sess.id, sess.nextID)
}

func (sess *frSession) registerForm(form *frForm) {
	form.id = sess.newID()
	sess.live[form.id] = form
	for _, control := range form.controls {
		sess.registerControl(control)
	}
}

func (sess *frSession) registerControl(control *frControl) {
	control.id = sess.newID()
	sess.live[control.id] = control
	for _, option := range control.options {
		sess.registerControl(option)
	}
}

func (sess *frSession) unregisterForm(form *frForm) {
	delete(sess.live, form.id)
	for _, control := range form.controls {
		sess.unregisterControl(control)
	}
}

func (sess *frSession) unregisterControl(control *frControl) {
	delete(sess.live, control.id)
	for _, option := range control.options {
		sess.unregisterControl(option)
	}
}

func (sess *frSession) addExtra(selector string, control *frControl) {
	control.id = sess.newID()
	sess.live[control.id] = control
	sess.extras[selector] = control
}

// replaceForm swaps a form for a fresh deep copy with all-new element IDs,
// preserving the current state. Every ID of the old generation goes stale.
func (sess *frSession) replaceForm(old *frForm) {
	if old == nil {
		return
	}
	replacement := &frForm{selector: old.selector}
	for _, control := range old.controls {
		replacement.controls = append(replacement.controls, frClone(control))
	}
	for i, form := range sess.forms {
		if form == old {
			sess.forms[i] = replacement
		}
	}
	sess.unregisterForm(old)
	sess.registerForm(replacement)
}

func frClone(control *frControl) *frControl {
	clone := *control
	clone.id = ""
	clone.options = nil
	for _, option := range control.options {
		clone.options = append(clone.options, frClone(option))
	}
	return &clone
}

func (sess *frSession) formOf(control *frControl) *frForm {
	for _, form := range sess.forms {
		for _, candidate := range form.controls {
			if frContains(candidate, control) {
				return form
			}
		}
	}
	return nil
}

func frContains(root, control *frControl) bool {
	if root == control {
		return true
	}
	for _, option := range root.options {
		if frContains(option, control) {
			return true
		}
	}
	return false
}

func (sess *frSession) selectOf(option *frControl) *frControl {
	for _, form := range sess.forms {
		for _, control := range form.controls {
			if control.tag != "select" {
				continue
			}
			for _, candidate := range control.options {
				if candidate == option {
					return control
				}
			}
		}
	}
	return nil
}

func (sess *frSession) click(control *frControl) {
	switch {
	case control.tag == "input" && control.typ == "checkbox":
		control.selected = !control.selected
	case control.tag == "input" && control.typ == "radio":
		if form := sess.formOf(control); form != nil {
			for _, sibling := range form.controls {
				if sibling.typ == "radio" && sibling.name == control.name {
					sibling.selected = false
				}
			}
		}
		control.selected = true
	case control.tag == "option":
		if parent := sess.selectOf(control); parent != nil {
			if parent.multiple {
				control.selected = !control.selected
			} else {
				for _, sibling := range parent.options {
					sibling.selected = false
				}
				control.selected = true
			}
		}
	}
}

func (sess *frSession) fire(action string, control *frControl) {
	if sess.onAction != nil {
		sess.onAction(sess, action, control)
	}
}

func (s *frServer) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	s.mu.Lock()
	defer s.mu.Unlock()
	w.Header().Set("Content-Type", "application/json")

	parts := strings.Split(strings.TrimPrefix(r.URL.Path, "/session/"), "/")
	sess, ok := s.sessions[parts[0]]
	if !ok {
		s.t.Errorf("request for unknown session %q: %s %s", parts[0], r.Method, r.URL.Path)
		w.WriteHeader(404)
		json.NewEncoder(w).Encode(map[string]interface{}{"value": map[string]string{"message": "no such session"}})
		return
	}
	sess.serve(s.t, w, r, parts[1:])
}

func (sess *frSession) respond(w http.ResponseWriter, value interface{}) {
	json.NewEncoder(w).Encode(map[string]interface{}{"value": value})
}

func frRefs(controls ...*frControl) []map[string]string {
	refs := []map[string]string{}
	for _, control := range controls {
		refs = append(refs, map[string]string{"ELEMENT": control.id})
	}
	return refs
}

func (sess *frSession) serve(t *testing.T, w http.ResponseWriter, r *http.Request, parts []string) {
	if len(parts) == 1 && parts[0] == "elements" && r.Method == "POST" {
		var req struct{ Using, Value string }
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			t.Errorf("bad elements request: %s", err)
		}
		sess.log = append(sess.log, "find:"+req.Value)
		refs := []map[string]string{}
		for _, form := range sess.forms {
			if form.selector == req.Value {
				refs = append(refs, map[string]string{"ELEMENT": form.id})
			}
		}
		if extra, ok := sess.extras[req.Value]; ok {
			refs = append(refs, map[string]string{"ELEMENT": extra.id})
		}
		sess.respond(w, refs)
		return
	}

	if len(parts) >= 2 && parts[0] == "element" {
		id := parts[1]
		target, ok := sess.live[id]
		if !ok {
			sess.log = append(sess.log, "stale!"+r.Method+":"+id+":"+strings.Join(parts[2:], "/"))
			w.WriteHeader(404)
			sess.respond(w, map[string]string{"message": "stale element reference: " + id})
			return
		}
		sess.serveElement(t, w, r, id, target, parts[2:])
		return
	}

	t.Errorf("unexpected request: %s %s", r.Method, r.URL.Path)
	w.WriteHeader(404)
	sess.respond(w, map[string]string{"message": "unexpected request"})
}

func (sess *frSession) serveElement(t *testing.T, w http.ResponseWriter, r *http.Request, id string, target interface{}, rest []string) {
	form, isForm := target.(*frForm)
	control, isControl := target.(*frControl)

	if len(rest) == 1 && rest[0] == "name" {
		sess.log = append(sess.log, "tag:"+id)
		if isForm {
			sess.respond(w, "form")
		} else {
			sess.respond(w, control.tag)
		}
		return
	}

	if len(rest) == 1 && rest[0] == "elements" && r.Method == "POST" {
		var req struct{ Using, Value string }
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			t.Errorf("bad scoped elements request: %s", err)
		}
		switch {
		case isForm && req.Value == ".//input | .//textarea | .//select":
			sess.log = append(sess.log, "find-in:"+id+":controls")
			sess.respond(w, frRefs(form.controls...))
		case isControl && control.tag == "select" && req.Value == ".//option":
			// .//option covers descendants, including options nested in <optgroup>
			sess.log = append(sess.log, "find-in:"+id+":options")
			sess.respond(w, frRefs(control.options...))
		default:
			t.Errorf("unexpected scoped query %q on element %s", req.Value, id)
			w.WriteHeader(404)
			sess.respond(w, map[string]string{"message": "unexpected scoped query"})
		}
		return
	}

	if !isControl {
		t.Errorf("unexpected form-scoped request: %s %s", r.Method, strings.Join(rest, "/"))
		w.WriteHeader(404)
		sess.respond(w, map[string]string{"message": "unexpected form-scoped request"})
		return
	}

	switch {
	case len(rest) == 2 && rest[0] == "attribute":
		sess.log = append(sess.log, "attr:"+id+":"+rest[1])
		switch rest[1] {
		case "name":
			sess.respond(w, control.name)
		case "type":
			sess.respond(w, control.typ)
		case "value":
			sess.respond(w, control.value)
		case "multiple":
			if control.multiple {
				sess.respond(w, "true")
			} else {
				sess.respond(w, "")
			}
		default:
			t.Errorf("unexpected attribute %q on element %s", rest[1], id)
			sess.respond(w, "")
		}
	case len(rest) == 1 && rest[0] == "enabled":
		sess.log = append(sess.log, "enabled:"+id)
		sess.respond(w, control.enabled)
	case len(rest) == 1 && rest[0] == "selected":
		sess.log = append(sess.log, "selected:"+id)
		sess.respond(w, control.selected)
	case len(rest) == 1 && rest[0] == "clear" && r.Method == "POST":
		sess.log = append(sess.log, "clear:"+id)
		control.value = ""
		sess.fire("clear", control)
		sess.respond(w, nil)
	case len(rest) == 1 && rest[0] == "value" && r.Method == "POST":
		var req struct{ Value []string }
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			t.Errorf("bad value request: %s", err)
		}
		text := strings.Join(req.Value, "")
		sess.log = append(sess.log, "value:"+id+":"+text)
		control.value = text
		sess.fire("value", control)
		sess.respond(w, nil)
	case len(rest) == 1 && rest[0] == "click" && r.Method == "POST":
		sess.log = append(sess.log, "click:"+id)
		sess.click(control)
		sess.fire("click", control)
		sess.respond(w, nil)
	default:
		t.Errorf("unexpected element request: %s %s", r.Method, strings.Join(rest, "/"))
		w.WriteHeader(404)
		sess.respond(w, map[string]string{"message": "unexpected element request"})
	}
}

// --- fixture builders ---

func frText(name, value string) *frControl {
	return &frControl{tag: "input", typ: "text", name: name, value: value, enabled: true}
}

func frTextarea(name, value string) *frControl {
	return &frControl{tag: "textarea", name: name, value: value, enabled: true}
}

func frCheckbox(name, value string, selected bool) *frControl {
	return &frControl{tag: "input", typ: "checkbox", name: name, value: value, selected: selected, enabled: true}
}

func frRadio(name, value string, selected bool) *frControl {
	return &frControl{tag: "input", typ: "radio", name: name, value: value, selected: selected, enabled: true}
}

func frOption(value string, selected bool) *frControl {
	return &frControl{tag: "option", value: value, selected: selected, enabled: true}
}

func frSelect(name string, multiple bool, options ...*frControl) *frControl {
	return &frControl{tag: "select", name: name, multiple: multiple, enabled: true, options: options}
}

func frFormDef(selector string, controls ...*frControl) *frForm {
	return &frForm{selector: selector, controls: controls}
}

// --- assertion helpers ---

// frActions filters a session log down to the sent Clear/Value/Click actions.
func frActions(log []string) []string {
	actions := []string{}
	for _, entry := range log {
		for _, prefix := range []string{"clear:", "value:", "click:"} {
			if strings.HasPrefix(entry, prefix) {
				actions = append(actions, entry)
			}
		}
	}
	return actions
}

// frStale collects requests that referenced stale (replaced) element IDs.
func frStale(log []string) []string {
	stale := []string{}
	for _, entry := range log {
		if strings.HasPrefix(entry, "stale!") {
			stale = append(stale, entry)
		}
	}
	return stale
}

// frFormGenerations lists the distinct form element IDs that scoped control
// queries were issued against, in order of first use.
func frFormGenerations(log []string) []string {
	generations := []string{}
	for _, entry := range log {
		if !strings.HasPrefix(entry, "find-in:") || !strings.HasSuffix(entry, ":controls") {
			continue
		}
		id := strings.TrimSuffix(strings.TrimPrefix(entry, "find-in:"), ":controls")
		found := false
		for _, generation := range generations {
			if generation == id {
				found = true
			}
		}
		if !found {
			generations = append(generations, id)
		}
	}
	return generations
}

func frSelectedValues(controls ...*frControl) []string {
	selected := []string{}
	for _, control := range controls {
		if control.selected {
			selected = append(selected, control.value)
		}
	}
	return selected
}

func frControlsNamed(form *frForm, name string) []*frControl {
	controls := []*frControl{}
	for _, control := range form.controls {
		if control.name == name {
			controls = append(controls, control)
		}
	}
	return controls
}

func TestFormValueSets(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// One ordered pass over text, checkbox group, radio group, and a
		// multiple select whose y/z options live inside an <optgroup>.
		server := newFRServer(t)
		city := frText("city", "old")
		svcA := frCheckbox("svc", "a", true)
		svcB := frCheckbox("svc", "b", false)
		svcC := frCheckbox("svc", "c", false)
		basic := frRadio("plan", "basic", true)
		pro := frRadio("plan", "pro", false)
		optX := frOption("x", true)
		optY := frOption("y", false) // nested under <optgroup label="extras">
		optZ := frOption("z", false) // nested under <optgroup label="extras">
		toppings := frSelect("toppings", true, optX, optY, optZ)
		sess := server.addSession("sets", frFormDef("#order", city, svcA, svcB, svcC, basic, pro, toppings))
		page := server.joinPage(t, "sets")

		result, err := page.Find("#order").ApplyForm([]agouti.FormStep{
			{Name: "svc", Values: []string{"b", "c"}},
			{Name: "city", Values: []string{"new"}},
			{Name: "plan", Values: []string{"pro"}},
			{Name: "toppings", Values: []string{"y", "z"}},
		}, 20)
		c.Check(err == nil, "ApplyForm over mixed kinds failed: %v", err)
		c.Check(result.Completed == 4, "Completed=%d, want 4", result.Completed)
		c.Check(result.Actions == 9, "Actions=%d, want 9", result.Actions)
		wantActions := []string{
			"click:" + svcA.id, "click:" + svcB.id, "click:" + svcC.id,
			"clear:" + city.id, "value:" + city.id + ":new",
			"click:" + pro.id,
			"click:" + optX.id, "click:" + optY.id, "click:" + optZ.id,
		}
		c.Check(reflect.DeepEqual(frActions(sess.log), wantActions),
			"actions=%v, want %v", frActions(sess.log), wantActions)
		c.Check(len(frStale(sess.log)) == 0, "stale requests: %v", frStale(sess.log))
		c.Check(sess.log[0] == "find:#order", "first request=%q, want form resolution", sess.log[0])
		c.Check(city.value == "new", "city=%q, want %q", city.value, "new")
		c.Check(reflect.DeepEqual(frSelectedValues(svcA, svcB, svcC), []string{"b", "c"}),
			"svc selected=%v, want [b c]", frSelectedValues(svcA, svcB, svcC))
		c.Check(pro.selected && !basic.selected, "plan: pro=%v basic=%v", pro.selected, basic.selected)
		c.Check(reflect.DeepEqual(frSelectedValues(toppings.options...), []string{"y", "z"}),
			"toppings selected=%v, want [y z]", frSelectedValues(toppings.options...))
		optionsQuery := "find-in:" + toppings.id + ":options"
		foundOptionsQuery := false
		for _, entry := range sess.log {
			if entry == optionsQuery {
				foundOptionsQuery = true
			}
		}
		c.Check(foundOptionsQuery, "options were not queried under the select element %s", toppings.id)

		// Steps that already match their target are confirmed read-only.
		server2 := newFRServer(t)
		city2 := frText("city", "new")
		svc2 := frCheckbox("svc", "a", true)
		sess2 := server2.addSession("satisfied", frFormDef("#order", city2, svc2))
		page2 := server2.joinPage(t, "satisfied")

		single, err := page2.Find("#order").ApplyForm([]agouti.FormStep{
			{Name: "city", Values: []string{"new"}},
		}, 5)
		c.Check(err == nil, "satisfied single step failed: %v", err)
		c.Check(single.Completed == 1 && single.Actions == 0,
			"satisfied single step=%+v, want Completed=1 Actions=0", single)

		both, err := page2.Find("#order").ApplyForm([]agouti.FormStep{
			{Name: "city", Values: []string{"new"}},
			{Name: "svc", Values: []string{"a"}},
		}, 5)
		c.Check(err == nil, "satisfied prefix failed: %v", err)
		c.Check(both.Completed == 2 && both.Actions == 0,
			"satisfied prefix=%+v, want Completed=2 Actions=0", both)
		c.Check(len(frActions(sess2.log)) == 0, "satisfied steps sent actions: %v", frActions(sess2.log))

		// Single select, textarea, and a field name containing quotes.
		server3 := newFRServer(t)
		countryUS := frOption("us", true)
		countryJP := frOption("jp", false)
		country := frSelect("country", false, countryUS, countryJP)
		note := frTextarea("note", "")
		quoted := frText(`q"u'o`, "x")
		sess3 := server3.addSession("misc", frFormDef("#profile", country, note, quoted))
		page3 := server3.joinPage(t, "misc")

		result3, err := page3.Find("#profile").ApplyForm([]agouti.FormStep{
			{Name: "country", Values: []string{"jp"}},
			{Name: "note", Values: []string{"hello"}},
			{Name: `q"u'o`, Values: []string{"y"}},
		}, 10)
		c.Check(err == nil, "ApplyForm with select/textarea/quoted name failed: %v", err)
		c.Check(result3.Completed == 3 && result3.Actions == 5,
			"result=%+v, want Completed=3 Actions=5", result3)
		wantActions3 := []string{
			"click:" + countryJP.id,
			"clear:" + note.id, "value:" + note.id + ":hello",
			"clear:" + quoted.id, "value:" + quoted.id + ":y",
		}
		c.Check(reflect.DeepEqual(frActions(sess3.log), wantActions3),
			"actions=%v, want %v", frActions(sess3.log), wantActions3)
		c.Check(countryJP.selected && !countryUS.selected, "country: jp=%v us=%v", countryJP.selected, countryUS.selected)
		c.Check(note.value == "hello", "note=%q, want %q", note.value, "hello")
		c.Check(quoted.value == "y", "quoted field=%q, want %q", quoted.value, "y")

		// Same field name in two forms: only the resolved form is touched.
		server4 := newFRServer(t)
		orderSvc := frCheckbox("svc", "a", false)
		otherSvc := frCheckbox("svc", "a", false)
		orderForm := frFormDef("#order", orderSvc)
		otherForm := frFormDef("#other", otherSvc)
		sess4 := server4.addSession("twoforms", orderForm, otherForm)
		page4 := server4.joinPage(t, "twoforms")

		result4, err := page4.Find("#order").ApplyForm([]agouti.FormStep{
			{Name: "svc", Values: []string{"a"}},
		}, 5)
		c.Check(err == nil, "ApplyForm on #order failed: %v", err)
		c.Check(result4.Completed == 1 && result4.Actions == 1,
			"result=%+v, want Completed=1 Actions=1", result4)
		c.Check(reflect.DeepEqual(frActions(sess4.log), []string{"click:" + orderSvc.id}),
			"actions=%v, want only the #order checkbox", frActions(sess4.log))
		c.Check(orderSvc.selected && !otherSvc.selected,
			"order svc=%v, other svc=%v", orderSvc.selected, otherSvc.selected)
		for _, entry := range sess4.log {
			c.Check(!strings.Contains(entry, otherSvc.id) && !strings.Contains(entry, otherForm.id),
				"request %q leaked into the #other form", entry)
		}

		// A checkbox group can be reconciled to the empty set.
		server5 := newFRServer(t)
		emptyA := frCheckbox("svc", "a", true)
		emptyB := frCheckbox("svc", "b", true)
		sess5 := server5.addSession("emptyset", frFormDef("#order", emptyA, emptyB))
		page5 := server5.joinPage(t, "emptyset")

		result5, err := page5.Find("#order").ApplyForm([]agouti.FormStep{
			{Name: "svc", Values: []string{}},
		}, 5)
		c.Check(err == nil, "ApplyForm to empty set failed: %v", err)
		c.Check(result5.Completed == 1 && result5.Actions == 2,
			"result=%+v, want Completed=1 Actions=2", result5)
		c.Check(reflect.DeepEqual(frActions(sess5.log), []string{"click:" + emptyA.id, "click:" + emptyB.id}),
			"actions=%v, want unchecking a then b", frActions(sess5.log))
		c.Check(len(frSelectedValues(emptyA, emptyB)) == 0,
			"svc still selected: %v", frSelectedValues(emptyA, emptyB))

		// Invalid groups and targets are rejected per step, before any action.
		type errorCase struct {
			label    string
			controls []*frControl
			step     agouti.FormStep
			want     string
		}
		disabledCheckbox := frCheckbox("svc", "a", false)
		disabledCheckbox.enabled = false
		disabledOption := frOption("x", false)
		disabledOption.enabled = false
		errorCases := []errorCase{
			{"mixed types", []*frControl{frText("mix", ""), frCheckbox("mix", "a", false)},
				agouti.FormStep{Name: "mix", Values: []string{"a"}}, "mixed"},
			{"missing target value", []*frControl{frCheckbox("svc", "a", false)},
				agouti.FormStep{Name: "svc", Values: []string{"zzz"}}, `"zzz"`},
			{"empty group", []*frControl{frText("city", "")},
				agouti.FormStep{Name: "ghost", Values: []string{"x"}}, `"ghost"`},
			{"multiple text fields", []*frControl{frText("dup", ""), frText("dup", "")},
				agouti.FormStep{Name: "dup", Values: []string{"x"}}, "multiple text fields"},
			{"disabled checkbox", []*frControl{disabledCheckbox},
				agouti.FormStep{Name: "svc", Values: []string{"a"}}, "disabled"},
			{"disabled option", []*frControl{frSelect("country", false, frOption("us", true), disabledOption)},
				agouti.FormStep{Name: "country", Values: []string{"us"}}, "disabled"},
			{"hidden input", []*frControl{{tag: "input", typ: "hidden", name: "hid", value: "1", enabled: true}},
				agouti.FormStep{Name: "hid", Values: []string{"2"}}, "unsupported"},
			{"file input", []*frControl{{tag: "input", typ: "file", name: "doc", enabled: true}},
				agouti.FormStep{Name: "doc", Values: []string{"f"}}, "unsupported"},
			{"submit input", []*frControl{{tag: "input", typ: "submit", name: "go", enabled: true}},
				agouti.FormStep{Name: "go", Values: []string{"x"}}, "unsupported"},
			{"duplicate checkbox values", []*frControl{frCheckbox("svc", "a", false), frCheckbox("svc", "a", false)},
				agouti.FormStep{Name: "svc", Values: []string{"a"}}, "duplicate"},
			{"duplicate option values", []*frControl{frSelect("country", false, frOption("x", false), frOption("x", false))},
				agouti.FormStep{Name: "country", Values: []string{"x"}}, "duplicate"},
			{"radio with two targets", []*frControl{frRadio("plan", "basic", true), frRadio("plan", "pro", false)},
				agouti.FormStep{Name: "plan", Values: []string{"basic", "pro"}}, "exactly one"},
			{"text with two targets", []*frControl{frText("city", "")},
				agouti.FormStep{Name: "city", Values: []string{"a", "b"}}, "exactly one"},
			{"single select with two targets", []*frControl{frSelect("country", false, frOption("us", true), frOption("jp", false))},
				agouti.FormStep{Name: "country", Values: []string{"us", "jp"}}, "exactly one"},
		}
		for i, tc := range errorCases {
			errServer := newFRServer(t)
			errSess := errServer.addSession(fmt.Sprintf("err-%d", i), frFormDef("#order", tc.controls...))
			errPage := errServer.joinPage(t, errSess.id)
			errResult, err := errPage.Find("#order").ApplyForm([]agouti.FormStep{tc.step}, 10)
			c.Check(err != nil, "%s: expected an error", tc.label)
			if err != nil {
				c.Check(strings.Contains(err.Error(), `"`+tc.step.Name+`"`),
					"%s: error %q does not name the step", tc.label, err)
				c.Check(strings.Contains(err.Error(), tc.want),
					"%s: error %q does not mention %s", tc.label, err, tc.want)
			}
			c.Check(errResult.Completed == 0 && errResult.Actions == 0,
				"%s: result=%+v, want Completed=0 Actions=0", tc.label, errResult)
			c.Check(len(frActions(errSess.log)) == 0,
				"%s: actions were sent: %v", tc.label, frActions(errSess.log))
		}

		// The selection must resolve to exactly one <form>.
		serverNF := newFRServer(t)
		sessNF := serverNF.addSession("nonform", frFormDef("#order", frText("city", "")))
		sessNF.addExtra("#panel", &frControl{tag: "div", enabled: true})
		pageNF := serverNF.joinPage(t, "nonform")
		_, err = pageNF.Find("#panel").ApplyForm([]agouti.FormStep{
			{Name: "city", Values: []string{"x"}},
		}, 5)
		c.Check(err != nil && strings.Contains(err.Error(), "<form>"),
			"non-form selection: err=%v, want a <form> error", err)

		serverAmb := newFRServer(t)
		serverAmb.addSession("ambiguous",
			frFormDef("#dup", frText("city", "")), frFormDef("#dup", frText("city", "")))
		pageAmb := serverAmb.joinPage(t, "ambiguous")
		ambResult, err := pageAmb.Find("#dup").ApplyForm([]agouti.FormStep{
			{Name: "city", Values: []string{"x"}},
		}, 5)
		c.Check(err != nil, "ambiguous form selection: expected an error")
		c.Check(ambResult.Completed == 0 && ambResult.Actions == 0,
			"ambiguous form selection: result=%+v", ambResult)
	})
}

func TestFormReplacement(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// The page replaces the whole form as soon as Clear succeeds; the
		// follow-up Value must reach the replacement text field, never the
		// stale one.
		server := newFRServer(t)
		city := frText("city", "old")
		orderForm := frFormDef("#order", city)
		sess := server.addSession("remount", orderForm)
		sess.onAction = func(sess *frSession, action string, control *frControl) {
			if action == "clear" {
				sess.replaceForm(sess.formOf(control))
			}
		}
		page := server.joinPage(t, "remount")

		result, err := page.Find("#order").ApplyForm([]agouti.FormStep{
			{Name: "city", Values: []string{"new"}},
		}, 5)
		c.Check(err == nil, "ApplyForm across clear-remount failed: %v", err)
		c.Check(result.Completed == 1 && result.Actions == 2,
			"result=%+v, want Completed=1 Actions=2", result)

		currentForm := sess.forms[0]
		c.Check(currentForm != orderForm && currentForm.id != orderForm.id,
			"form was not replaced: %s", currentForm.id)
		currentCity := frControlsNamed(currentForm, "city")[0]
		c.Check(currentCity.id != city.id, "text field was not replaced: %s", currentCity.id)
		wantActions := []string{"clear:" + city.id, "value:" + currentCity.id + ":new"}
		c.Check(reflect.DeepEqual(frActions(sess.log), wantActions),
			"actions=%v, want %v (Value on the replacement field)", frActions(sess.log), wantActions)
		c.Check(len(frStale(sess.log)) == 0, "stale requests: %v", frStale(sess.log))
		c.Check(currentCity.value == "new", "city=%q, want %q", currentCity.value, "new")
		generations := frFormGenerations(sess.log)
		c.Check(len(generations) == 2 && generations[0] == orderForm.id && generations[1] == currentForm.id,
			"form generations queried=%v, want [%s %s]", generations, orderForm.id, currentForm.id)

		// Every click replaces the whole form, so each click of a checkbox
		// group lands on a fresh generation of element IDs.
		server2 := newFRServer(t)
		svcA := frCheckbox("svc", "a", false)
		svcB := frCheckbox("svc", "b", false)
		orderForm2 := frFormDef("#order", svcA, svcB)
		sess2 := server2.addSession("click-remount", orderForm2)
		sess2.onAction = func(sess *frSession, action string, control *frControl) {
			if action == "click" {
				sess.replaceForm(sess.formOf(control))
			}
		}
		page2 := server2.joinPage(t, "click-remount")

		result2, err := page2.Find("#order").ApplyForm([]agouti.FormStep{
			{Name: "svc", Values: []string{"a", "b"}},
		}, 5)
		c.Check(err == nil, "ApplyForm across click-remount failed: %v", err)
		c.Check(result2.Completed == 1 && result2.Actions == 2,
			"result=%+v, want Completed=1 Actions=2", result2)
		actions2 := frActions(sess2.log)
		c.Check(len(actions2) == 2 && actions2[0] == "click:"+svcA.id && actions2[1] != actions2[0],
			"actions=%v, want clicks on two different generations", actions2)
		currentForm2 := sess2.forms[0]
		c.Check(reflect.DeepEqual(frSelectedValues(frControlsNamed(currentForm2, "svc")...), []string{"a", "b"}),
			"svc selected=%v, want [a b]", frSelectedValues(frControlsNamed(currentForm2, "svc")...))
		c.Check(len(frStale(sess2.log)) == 0, "stale requests: %v", frStale(sess2.log))
		c.Check(len(frFormGenerations(sess2.log)) == 3,
			"form generations queried=%v, want 3 generations", frFormGenerations(sess2.log))

		// A replacement triggered by Value is picked up naturally by the
		// next step: no element ID crosses the step boundary.
		server3 := newFRServer(t)
		city3 := frText("city", "old")
		svc3 := frCheckbox("svc", "a", false)
		orderForm3 := frFormDef("#order", city3, svc3)
		sess3 := server3.addSession("step-remount", orderForm3)
		sess3.onAction = func(sess *frSession, action string, control *frControl) {
			if action == "value" {
				sess.replaceForm(sess.formOf(control))
			}
		}
		page3 := server3.joinPage(t, "step-remount")

		result3, err := page3.Find("#order").ApplyForm([]agouti.FormStep{
			{Name: "city", Values: []string{"new"}},
			{Name: "svc", Values: []string{"a"}},
		}, 5)
		c.Check(err == nil, "ApplyForm across step-remount failed: %v", err)
		c.Check(result3.Completed == 2 && result3.Actions == 3,
			"result=%+v, want Completed=2 Actions=3", result3)
		currentForm3 := sess3.forms[0]
		currentSvc := frControlsNamed(currentForm3, "svc")[0]
		wantActions3 := []string{
			"clear:" + city3.id, "value:" + city3.id + ":new", "click:" + currentSvc.id,
		}
		c.Check(reflect.DeepEqual(frActions(sess3.log), wantActions3),
			"actions=%v, want %v", frActions(sess3.log), wantActions3)
		c.Check(currentSvc.id != svc3.id, "checkbox was not replaced: %s", currentSvc.id)
		c.Check(currentSvc.selected, "svc not selected in the replacement form")
		c.Check(frControlsNamed(currentForm3, "city")[0].value == "new",
			"city=%q, want %q", frControlsNamed(currentForm3, "city")[0].value, "new")
		c.Check(len(frStale(sess3.log)) == 0, "stale requests: %v", frStale(sess3.log))
	})
}

func TestFormProgress(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// A reactive page reverts every click; the shared budget stops the
		// reconciliation instead of looping forever.
		server := newFRServer(t)
		svc := frCheckbox("svc", "a", false)
		sess := server.addSession("pendulum", frFormDef("#order", svc))
		sess.onAction = func(sess *frSession, action string, control *frControl) {
			if action == "click" {
				control.selected = !control.selected // page script flips it back
			}
		}
		page := server.joinPage(t, "pendulum")

		result, err := page.Find("#order").ApplyForm([]agouti.FormStep{
			{Name: "svc", Values: []string{"a"}},
		}, 3)
		c.Check(err != nil, "reactive page: expected a budget error")
		if err != nil {
			c.Check(strings.Contains(err.Error(), `"svc"`), "error %q does not name the step", err)
			c.Check(strings.Contains(err.Error(), "budget"), "error %q does not mention the budget", err)
		}
		c.Check(result.Completed == 0 && result.Actions == 3,
			"result=%+v, want Completed=0 Actions=3", result)
		wantClicks := []string{"click:" + svc.id, "click:" + svc.id, "click:" + svc.id}
		c.Check(reflect.DeepEqual(frActions(sess.log), wantClicks),
			"actions=%v, want exactly 3 clicks and no fourth", frActions(sess.log))

		// A failing later step preserves the completed prefix and the
		// actions already spent; the failed write is never replayed.
		server2 := newFRServer(t)
		city2 := frText("city", "old")
		svc2 := frCheckbox("svc", "a", false)
		sess2 := server2.addSession("prefix", frFormDef("#order", city2, svc2))
		page2 := server2.joinPage(t, "prefix")

		result2, err := page2.Find("#order").ApplyForm([]agouti.FormStep{
			{Name: "city", Values: []string{"new"}},
			{Name: "svc", Values: []string{"zzz"}},
		}, 10)
		c.Check(err != nil, "later failing step: expected an error")
		if err != nil {
			c.Check(strings.Contains(err.Error(), `"svc"`), "error %q does not name the failing step", err)
			c.Check(strings.Contains(err.Error(), `"zzz"`), "error %q does not mention the missing value", err)
		}
		c.Check(result2.Completed == 1 && result2.Actions == 2,
			"result=%+v, want Completed=1 Actions=2", result2)
		wantActions2 := []string{"clear:" + city2.id, "value:" + city2.id + ":new"}
		c.Check(reflect.DeepEqual(frActions(sess2.log), wantActions2),
			"actions=%v, want only the completed prefix", frActions(sess2.log))
		c.Check(city2.value == "new", "city=%q, want %q", city2.value, "new")
		c.Check(!svc2.selected, "svc must remain untouched")

		// Zero-action steps count toward the completed prefix.
		server3 := newFRServer(t)
		city3 := frText("city", "new")
		svc3 := frCheckbox("svc", "a", true)
		note3 := frText("note", "x")
		sess3 := server3.addSession("zeroprefix", frFormDef("#order", city3, svc3, note3))
		page3 := server3.joinPage(t, "zeroprefix")

		result3, err := page3.Find("#order").ApplyForm([]agouti.FormStep{
			{Name: "city", Values: []string{"new"}},
			{Name: "svc", Values: []string{"a"}},
			{Name: "note", Values: []string{"y"}},
		}, 5)
		c.Check(err == nil, "zero-action prefix failed: %v", err)
		c.Check(result3.Completed == 3 && result3.Actions == 2,
			"result=%+v, want Completed=3 Actions=2", result3)
		wantActions3 := []string{"clear:" + note3.id, "value:" + note3.id + ":y"}
		c.Check(reflect.DeepEqual(frActions(sess3.log), wantActions3),
			"actions=%v, want only the note fill", frActions(sess3.log))

		// Invalid plans are rejected before any remote request, and the
		// caller's slices are never mutated.
		server4 := newFRServer(t)
		sess4 := server4.addSession("validate", frFormDef("#order", frText("city", "")))
		page4 := server4.joinPage(t, "validate")

		_, err = page4.Find("#order").ApplyForm([]agouti.FormStep{
			{Name: "city", Values: []string{"x"}},
		}, 0)
		c.Check(err != nil && strings.Contains(err.Error(), "budget"),
			"non-positive budget: err=%v", err)
		_, err = page4.Find("#order").ApplyForm([]agouti.FormStep{
			{Name: "", Values: []string{"x"}},
		}, 5)
		c.Check(err != nil, "empty step name: expected an error")
		_, err = page4.Find("#order").ApplyForm([]agouti.FormStep{
			{Name: "city", Values: []string{"a"}},
			{Name: "city", Values: []string{"b"}},
		}, 5)
		c.Check(err != nil && strings.Contains(err.Error(), "duplicate"),
			"duplicate step names: err=%v", err)
		_, err = page4.Find("#order").ApplyForm([]agouti.FormStep{
			{Name: "city", Values: []string{"a", "a"}},
		}, 5)
		c.Check(err != nil && strings.Contains(err.Error(), "duplicate"),
			"duplicate values in one step: err=%v", err)
		c.Check(len(sess4.log) == 0,
			"invalid plans triggered remote requests: %v", sess4.log)

		server5 := newFRServer(t)
		sess5 := server5.addSession("immutable", frFormDef("#order", frCheckbox("svc", "a", false)))
		page5 := server5.joinPage(t, "immutable")
		callerValues := []string{"a", "b"}
		callerSteps := []agouti.FormStep{{Name: "svc", Values: callerValues}}
		_, err = page5.Find("#order").ApplyForm(callerSteps, 5)
		c.Check(err != nil && strings.Contains(err.Error(), `"b"`),
			"unknown target value: err=%v", err)
		c.Check(reflect.DeepEqual(callerValues, []string{"a", "b"}),
			"caller values mutated: %v", callerValues)
		c.Check(reflect.DeepEqual(callerSteps[0].Values, []string{"a", "b"}),
			"caller step values mutated: %v", callerSteps[0].Values)
		c.Check(len(frActions(sess5.log)) == 0,
			"rejected step sent actions: %v", frActions(sess5.log))

		// Two sessions interleaved: independent state, independent logs, no
		// global progress shared between pages.
		server6 := newFRServer(t)
		cityA := frText("city", "a")
		cityB := frText("city", "x")
		sessA := server6.addSession("s1", frFormDef("#order", cityA))
		sessB := server6.addSession("s2", frFormDef("#order", cityB))
		pageA := server6.joinPage(t, "s1")
		pageB := server6.joinPage(t, "s2")

		inter1, interErr1 := pageA.Find("#order").ApplyForm([]agouti.FormStep{
			{Name: "city", Values: []string{"b"}},
		}, 5)
		inter2, interErr2 := pageB.Find("#order").ApplyForm([]agouti.FormStep{
			{Name: "city", Values: []string{"y"}},
		}, 5)
		inter3, interErr3 := pageA.Find("#order").ApplyForm([]agouti.FormStep{
			{Name: "city", Values: []string{"c"}},
		}, 5)
		c.Check(interErr1 == nil && interErr2 == nil && interErr3 == nil,
			"interleaved sessions failed: %v %v %v", interErr1, interErr2, interErr3)
		c.Check(inter1.Completed == 1 && inter1.Actions == 2, "s1 first=%+v", inter1)
		c.Check(inter2.Completed == 1 && inter2.Actions == 2, "s2=%+v", inter2)
		c.Check(inter3.Completed == 1 && inter3.Actions == 2, "s1 second=%+v", inter3)
		c.Check(cityA.value == "c" && cityB.value == "y",
			"final values: s1=%q s2=%q, want c and y", cityA.value, cityB.value)
		wantActionsA := []string{
			"clear:" + cityA.id, "value:" + cityA.id + ":b",
			"clear:" + cityA.id, "value:" + cityA.id + ":c",
		}
		c.Check(reflect.DeepEqual(frActions(sessA.log), wantActionsA),
			"s1 actions=%v, want %v", frActions(sessA.log), wantActionsA)
		wantActionsB := []string{"clear:" + cityB.id, "value:" + cityB.id + ":y"}
		c.Check(reflect.DeepEqual(frActions(sessB.log), wantActionsB),
			"s2 actions=%v, want %v", frActions(sessB.log), wantActionsB)
		c.Check(!strings.Contains(strings.Join(sessA.log, "\n"), "s2-"),
			"s1 log references s2 elements")
		c.Check(!strings.Contains(strings.Join(sessB.log, "\n"), "s1-"),
			"s2 log references s1 elements")
	})
}
