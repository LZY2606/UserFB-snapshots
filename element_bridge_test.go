package agouti_test

import (
	"encoding/json"
	"fmt"
	"io/ioutil"
	"net/http"
	"reflect"
	"strings"
	"testing"

	"github.com/sclevine/agouti"
	"github.com/sclevine/agouti/api"
)

// recordingTransport is an in-memory http.RoundTripper that records every
// request body and responds with a canned payload. No browser is started and
// no network traffic occurs.
type recordingTransport struct {
	requests []recordedRequest
	respond  func(body string) string
}

type recordedRequest struct {
	method string
	url    string
	body   string
}

func (r *recordingTransport) RoundTrip(request *http.Request) (*http.Response, error) {
	var body []byte
	if request.Body != nil {
		var err error
		body, err = ioutil.ReadAll(request.Body)
		if err != nil {
			return nil, err
		}
		request.Body.Close()
	}
	r.requests = append(r.requests, recordedRequest{request.Method, request.URL.String(), string(body)})

	responseBody := `{"value": null}`
	if r.respond != nil {
		responseBody = r.respond(string(body))
	}
	return &http.Response{
		StatusCode: http.StatusOK,
		Header:     make(http.Header),
		Body:       ioutil.NopCloser(strings.NewReader(responseBody)),
		Request:    request,
	}, nil
}

func (r *recordingTransport) lastBody(t *testing.T) map[string]interface{} {
	t.Helper()
	if len(r.requests) == 0 {
		t.Fatal("expected at least one request, got none")
	}
	body := r.requests[len(r.requests)-1].body
	var decoded map[string]interface{}
	if err := json.Unmarshal([]byte(body), &decoded); err != nil {
		t.Fatalf("failed to decode request body %q: %v", body, err)
	}
	return decoded
}

func newSession(transport *recordingTransport) *api.Session {
	return api.NewWithClient("http://webdriver.example/session/abc123", &http.Client{Transport: transport})
}

const w3cKey = "element-6066-11e4-a52e-4f735466cecf"

// assertElementRef fails the test unless value is a JSON object containing
// exactly the legacy and W3C element reference keys, both set to id.
func assertElementRef(t *testing.T, value interface{}, id string) {
	t.Helper()
	expected := map[string]interface{}{"ELEMENT": id, w3cKey: id}
	if !reflect.DeepEqual(value, expected) {
		t.Fatalf("expected element reference %v, got %#v", expected, value)
	}
}

func TestExecuteEncodesSingleElement(t *testing.T) {
	transport := &recordingTransport{}
	session := newSession(transport)
	element := &api.Element{ID: "elem-1", Session: session}

	field := map[string]interface{}{"field": element}
	args := []interface{}{field}

	if err := session.Execute("return arguments[0];", args, nil); err != nil {
		t.Fatalf("expected no error, got: %v", err)
	}
	if len(transport.requests) != 1 {
		t.Fatalf("expected exactly one request, got %d", len(transport.requests))
	}

	body := transport.lastBody(t)
	sentArgs := body["args"].([]interface{})
	assertElementRef(t, sentArgs[0].(map[string]interface{})["field"], "elem-1")

	// The caller's containers must not be modified in place.
	if len(field) != 1 || field["field"] != interface{}(element) {
		t.Fatalf("original map was modified: %#v", field)
	}
	if _, ok := field["field"].(*api.Element); !ok {
		t.Fatalf("original element was replaced: %#v", field["field"])
	}
}

func TestExecuteEncodesNestedListsAndAliases(t *testing.T) {
	transport := &recordingTransport{}
	session := newSession(transport)
	element := &api.Element{ID: "nested-el", Session: session}

	shared := map[string]interface{}{"el": element, "n": 3}
	args := []interface{}{
		shared,
		[]interface{}{shared, []interface{}{map[string]interface{}{"deep": shared}}},
	}

	if err := session.Execute("return 1;", args, nil); err != nil {
		t.Fatalf("expected no error, got: %v", err)
	}

	sentArgs := transport.lastBody(t)["args"].([]interface{})

	// The same map appears in three positions; every occurrence is encoded.
	assertElementRef(t, sentArgs[0].(map[string]interface{})["el"], "nested-el")
	inner := sentArgs[1].([]interface{})
	assertElementRef(t, inner[0].(map[string]interface{})["el"], "nested-el")
	deep := inner[1].([]interface{})[0].(map[string]interface{})["deep"]
	assertElementRef(t, deep.(map[string]interface{})["el"], "nested-el")

	if n := sentArgs[0].(map[string]interface{})["n"]; n != 3.0 {
		t.Fatalf("expected scalar to pass through, got %#v", n)
	}

	// The shared map must remain untouched.
	if len(shared) != 2 || shared["el"] != interface{}(element) || shared["n"] != 3 {
		t.Fatalf("shared map was modified: %#v", shared)
	}
}

func TestExecuteRejectsCircularReference(t *testing.T) {
	transport := &recordingTransport{}
	session := newSession(transport)

	cyclic := map[string]interface{}{}
	cyclic["self"] = cyclic

	err := session.Execute("return 1;", []interface{}{cyclic}, nil)
	if err == nil || !strings.Contains(err.Error(), "circular reference at /0/self") {
		t.Fatalf("expected circular reference error at /0/self, got: %v", err)
	}
	if len(transport.requests) != 0 {
		t.Fatalf("expected no requests, got %d", len(transport.requests))
	}

	// A slice that contains itself is also rejected locally.
	cyclicSlice := make([]interface{}, 1)
	cyclicSlice[0] = cyclicSlice
	err = session.Execute("return 1;", []interface{}{cyclicSlice}, nil)
	if err == nil || !strings.Contains(err.Error(), "circular reference at /0/0") {
		t.Fatalf("expected circular reference error at /0/0, got: %v", err)
	}
	if len(transport.requests) != 0 {
		t.Fatalf("expected no requests, got %d", len(transport.requests))
	}
}

func TestExecuteRejectsInvalidElements(t *testing.T) {
	transport := &recordingTransport{}
	session := newSession(transport)
	sameURLSession := newSession(transport)
	otherSession := api.NewWithClient("http://webdriver.example/session/other", &http.Client{Transport: transport})

	cases := []struct {
		name    string
		element *api.Element
		path    string
	}{
		{"element from another session", &api.Element{ID: "x", Session: otherSession}, "/0/field"},
		{"element from same-URL session", &api.Element{ID: "x", Session: sameURLSession}, "/0/field"},
		{"element with empty ID", &api.Element{ID: "", Session: session}, "/0/field"},
		{"element without session", &api.Element{ID: "x"}, "/0/field"},
	}

	for _, testCase := range cases {
		before := len(transport.requests)
		args := []interface{}{map[string]interface{}{"field": testCase.element}}
		err := session.Execute("return 1;", args, nil)
		if err == nil {
			t.Fatalf("%s: expected error, got nil", testCase.name)
		}
		if !strings.Contains(err.Error(), testCase.path) {
			t.Fatalf("%s: expected error path %q, got: %v", testCase.name, testCase.path, err)
		}
		if len(transport.requests) != before {
			t.Fatalf("%s: expected no requests, got %d", testCase.name, len(transport.requests)-before)
		}
	}
}

func TestExecuteErrorPathsAreDeterministicAndEscaped(t *testing.T) {
	transport := &recordingTransport{}
	session := newSession(transport)
	other := &api.Element{ID: "x", Session: newSession(transport)}

	// Two invalid elements: the lexicographically first key is reported.
	args := []interface{}{map[string]interface{}{"b": other, "a": other}}
	err := session.Execute("return 1;", args, nil)
	if err == nil || !strings.Contains(err.Error(), "/0/a") {
		t.Fatalf("expected first error at /0/a, got: %v", err)
	}

	// Tilde and slash in keys are escaped per JSON Pointer (RFC 6901).
	args = []interface{}{map[string]interface{}{"a/b~c": other}}
	err = session.Execute("return 1;", args, nil)
	if err == nil || !strings.Contains(err.Error(), "/0/a~1b~0c") {
		t.Fatalf("expected escaped path /0/a~1b~0c, got: %v", err)
	}

	if len(transport.requests) != 0 {
		t.Fatalf("expected no requests, got %d", len(transport.requests))
	}
}

func TestExecuteEncodesNilElementAsNull(t *testing.T) {
	transport := &recordingTransport{}
	session := newSession(transport)

	var nilElement *api.Element
	args := []interface{}{nilElement, map[string]interface{}{"el": nilElement}}

	if err := session.Execute("return 1;", args, nil); err != nil {
		t.Fatalf("expected no error, got: %v", err)
	}

	sentArgs := transport.lastBody(t)["args"].([]interface{})
	if sentArgs[0] != nil {
		t.Fatalf("expected nil element to encode as null, got %#v", sentArgs[0])
	}
	if value := sentArgs[1].(map[string]interface{})["el"]; value != nil {
		t.Fatalf("expected nested nil element to encode as null, got %#v", value)
	}
}

func TestExecutePlainArgumentsRegression(t *testing.T) {
	transport := &recordingTransport{
		respond: func(body string) string { return `{"value": 42}` },
	}
	session := newSession(transport)

	args := []interface{}{1, "two", true, nil, 2.5, map[string]interface{}{"k": "v"}, []interface{}{1, 2}}
	var result int
	if err := session.Execute("return 42;", args, &result); err != nil {
		t.Fatalf("expected no error, got: %v", err)
	}
	if result != 42 {
		t.Fatalf("expected result 42, got %d", result)
	}

	body := transport.lastBody(t)
	if body["script"] != "return 42;" {
		t.Fatalf("unexpected script: %#v", body["script"])
	}
	expected := []interface{}{1.0, "two", true, nil, 2.5, map[string]interface{}{"k": "v"}, []interface{}{1.0, 2.0}}
	if !reflect.DeepEqual(body["args"], expected) {
		t.Fatalf("expected args %v, got %#v", expected, body["args"])
	}
}

func TestPageRunScriptWithElement(t *testing.T) {
	transport := &recordingTransport{
		respond: func(body string) string { return `{"value": 7}` },
	}
	client := &http.Client{Transport: transport}
	page := agouti.JoinPage("http://webdriver.example/session/abc123", agouti.HTTPClient(client))
	element := &api.Element{ID: "page-el", Session: page.Session()}

	var result int
	err := page.RunScript("return field;", map[string]interface{}{"field": element}, &result)
	if err != nil {
		t.Fatalf("expected no error, got: %v", err)
	}
	if result != 7 {
		t.Fatalf("expected result 7, got %d", result)
	}

	body := transport.lastBody(t)
	script, _ := body["script"].(string)
	if !strings.Contains(script, "function(field)") {
		t.Fatalf("expected argument name in script wrapper, got: %s", script)
	}
	assertElementRef(t, body["args"].([]interface{})[0], "page-el")
}

func TestPageRunScriptRejectsForeignElement(t *testing.T) {
	transport := &recordingTransport{}
	page := agouti.JoinPage("http://webdriver.example/session/abc123", agouti.HTTPClient(&http.Client{Transport: transport}))
	foreign := &api.Element{ID: "x", Session: newSession(transport)}

	err := page.RunScript("return field;", map[string]interface{}{"field": foreign}, nil)
	if err == nil || !strings.Contains(err.Error(), "different session") {
		t.Fatalf("expected foreign session error, got: %v", err)
	}
	if len(transport.requests) != 0 {
		t.Fatalf("expected no requests, got %d", len(transport.requests))
	}
}

func TestFrameElementOwnership(t *testing.T) {
	transport := &recordingTransport{}
	session := newSession(transport)

	// Frame(nil) switches back to the root frame.
	if err := session.Frame(nil); err != nil {
		t.Fatalf("expected no error, got: %v", err)
	}
	if id, present := transport.lastBody(t)["id"]; !present || id != nil {
		t.Fatalf("expected {\"id\": null}, got: %v", transport.lastBody(t))
	}

	// Frame(element) sends the dual-key element reference.
	element := &api.Element{ID: "frame-el", Session: session}
	if err := session.Frame(element); err != nil {
		t.Fatalf("expected no error, got: %v", err)
	}
	assertElementRef(t, transport.lastBody(t)["id"], "frame-el")

	// A foreign or session-less element is rejected before any request.
	for name, bad := range map[string]*api.Element{
		"foreign session": {ID: "x", Session: newSession(transport)},
		"no session":      {ID: "x"},
		"empty ID":        {ID: "", Session: session},
	} {
		before := len(transport.requests)
		if err := session.Frame(bad); err == nil {
			t.Fatalf("%s: expected error, got nil", name)
		}
		if len(transport.requests) != before {
			t.Fatalf("%s: expected no requests, got %d", name, len(transport.requests)-before)
		}
	}
}

func TestElementLookupKeyVariants(t *testing.T) {
	cases := []struct {
		name    string
		value   string
		wantID  string
		wantErr string
	}{
		{"legacy key", `{"ELEMENT": "legacy-id"}`, "legacy-id", ""},
		{"w3c key", fmt.Sprintf(`{%q: "w3c-id"}`, w3cKey), "w3c-id", ""},
		{"matching dual keys", fmt.Sprintf(`{"ELEMENT": "same-id", %q: "same-id"}`, w3cKey), "same-id", ""},
		{"conflicting keys", fmt.Sprintf(`{"ELEMENT": "one", %q: "two"}`, w3cKey), "", "conflicting"},
		{"empty id", `{"ELEMENT": ""}`, "", "no element ID"},
		{"missing keys", `{}`, "", "no element ID"},
		{"id with spaces preserved", `{"ELEMENT": "  spaced id  "}`, "  spaced id  ", ""},
	}

	lookups := map[string]func(*api.Session) (*api.Element, error){
		"Session.GetElement": func(s *api.Session) (*api.Element, error) {
			return s.GetElement(api.Selector{Using: "css selector", Value: "#x"})
		},
		"Session.GetActiveElement": func(s *api.Session) (*api.Element, error) {
			return s.GetActiveElement()
		},
		"Element.GetElement": func(s *api.Session) (*api.Element, error) {
			parent := &api.Element{ID: "parent", Session: s}
			return parent.GetElement(api.Selector{Using: "css selector", Value: "#x"})
		},
	}

	for lookupName, lookup := range lookups {
		for _, testCase := range cases {
			transport := &recordingTransport{
				respond: func(body string) string { return `{"value": ` + testCase.value + `}` },
			}
			element, err := lookup(newSession(transport))
			prefix := lookupName + "/" + testCase.name
			if testCase.wantErr != "" {
				if err == nil || !strings.Contains(err.Error(), testCase.wantErr) {
					t.Fatalf("%s: expected error containing %q, got: %v", prefix, testCase.wantErr, err)
				}
				if element != nil {
					t.Fatalf("%s: expected no element handle, got %#v", prefix, element)
				}
				continue
			}
			if err != nil {
				t.Fatalf("%s: expected no error, got: %v", prefix, err)
			}
			if element.ID != testCase.wantID {
				t.Fatalf("%s: expected ID %q, got %q", prefix, testCase.wantID, element.ID)
			}
		}
	}
}

func TestGetElementsKeyVariantsAndAtomicity(t *testing.T) {
	w3cOnly := fmt.Sprintf(`{%q: "b"}`, w3cKey)
	dual := fmt.Sprintf(`{"ELEMENT": "c", %q: "c"}`, w3cKey)

	// A fully valid list using a mix of key styles succeeds.
	transport := &recordingTransport{
		respond: func(body string) string {
			return `{"value": [{"ELEMENT": "a"}, ` + w3cOnly + `, ` + dual + `]}`
		},
	}
	elements, err := newSession(transport).GetElements(api.Selector{Using: "css selector", Value: "#x"})
	if err != nil {
		t.Fatalf("expected no error, got: %v", err)
	}
	ids := []string{elements[0].ID, elements[1].ID, elements[2].ID}
	if !reflect.DeepEqual(ids, []string{"a", "b", "c"}) {
		t.Fatalf("expected IDs [a b c], got %v", ids)
	}

	// A single bad reference fails the whole list: nil elements, no partials.
	badLists := map[string]string{
		"missing id":       `{"value": [{"ELEMENT": "a"}, {}]}`,
		"empty id":         `{"value": [{"ELEMENT": "a"}, {"ELEMENT": ""}]}`,
		"conflicting keys": fmt.Sprintf(`{"value": [{"ELEMENT": "a"}, {"ELEMENT": "b", %q: "c"}]}`, w3cKey),
	}
	for name, list := range badLists {
		transport := &recordingTransport{
			respond: func(body string) string { return list },
		}
		elements, err := newSession(transport).GetElements(api.Selector{Using: "css selector", Value: "#x"})
		if err == nil {
			t.Fatalf("%s: expected error, got nil", name)
		}
		if elements != nil {
			t.Fatalf("%s: expected nil elements, got %#v", name, elements)
		}
	}

	// Element-scoped lookups follow the same rules.
	transport = &recordingTransport{
		respond: func(body string) string { return `{"value": [{"ELEMENT": "a"}, {}]}` },
	}
	session := newSession(transport)
	parent := &api.Element{ID: "parent", Session: session}
	elements, err = parent.GetElements(api.Selector{Using: "css selector", Value: "#x"})
	if err == nil || elements != nil {
		t.Fatalf("Element.GetElements: expected nil elements and error, got %#v, %v", elements, err)
	}

	transport = &recordingTransport{
		respond: func(body string) string { return `{"value": {"ELEMENT": "child"}}` },
	}
	session = newSession(transport)
	parent = &api.Element{ID: "parent", Session: session}
	child, err := parent.GetElement(api.Selector{Using: "css selector", Value: "#x"})
	if err != nil || child.ID != "child" || child.Session != session {
		t.Fatalf("Element.GetElement: expected child element, got %#v, %v", child, err)
	}
}
