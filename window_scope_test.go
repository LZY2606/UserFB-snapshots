package selenium_test

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"reflect"
	"strings"
	"sync"
	"testing"

	"github.com/tebeka/selenium"
)

// recordedRequest is a single HTTP request received by fakeWebDriver.
type recordedRequest struct {
	Method string
	Path   string
	Body   string
}

// fakeWebDriver is a minimal in-process WebDriver server. It tracks the
// current window handle and records every request, so tests can assert the
// exact request sequence produced by window operations.
type fakeWebDriver struct {
	mu sync.Mutex

	sessionID string
	w3c       bool
	current   string
	windows   map[string]bool
	requests  []recordedRequest

	// Failure injection.
	maximizeErr bool            // maximize requests fail with a marked error
	currentErr  bool            // reading the current handle fails
	badSwitches map[string]bool // handles that cannot be switched to
}

func newW3CFake() *fakeWebDriver {
	return &fakeWebDriver{
		sessionID:   "w3c-test",
		w3c:         true,
		current:     "A",
		windows:     map[string]bool{"A": true, "B": true},
		badSwitches: map[string]bool{},
	}
}

func newLegacyFake() *fakeWebDriver {
	return &fakeWebDriver{
		sessionID:   "legacy-test",
		current:     "A",
		windows:     map[string]bool{"A": true, "B": true},
		badSwitches: map[string]bool{},
	}
}

func (f *fakeWebDriver) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	body, _ := io.ReadAll(r.Body)
	r.Body.Close()

	f.mu.Lock()
	defer f.mu.Unlock()
	f.requests = append(f.requests, recordedRequest{r.Method, r.URL.Path, string(body)})

	w.Header().Set("Content-Type", "application/json")

	if r.Method == "POST" && r.URL.Path == "/session" {
		if f.w3c {
			fmt.Fprintf(w, `{"value":{"sessionId":%q,"capabilities":{"browserName":"fake","browserVersion":"1.0"}}}`, f.sessionID)
		} else {
			fmt.Fprintf(w, `{"status":0,"sessionId":%q,"value":{"browserName":"fake"}}`, f.sessionID)
		}
		return
	}

	rest := strings.TrimPrefix(r.URL.Path, "/session/"+f.sessionID)
	switch {
	case r.Method == "GET" && (rest == "/window" || rest == "/window_handle"):
		if f.currentErr {
			w.WriteHeader(http.StatusInternalServerError)
			fmt.Fprint(w, `{"value":{"error":"unknown error","message":"marker: current handle failed"}}`)
			return
		}
		fmt.Fprintf(w, `{"value":%q}`, f.current)
	case r.Method == "POST" && rest == "/window":
		var params map[string]string
		json.Unmarshal(body, &params)
		handle := params["handle"]
		if !f.w3c {
			handle = params["name"]
		}
		if f.badSwitches[handle] || !f.windows[handle] {
			w.WriteHeader(http.StatusNotFound)
			fmt.Fprintf(w, `{"value":{"error":"no such window","message":"marker: cannot switch to %s"}}`, handle)
			return
		}
		f.current = handle
		fmt.Fprint(w, `{"value":null}`)
	case r.Method == "DELETE" && rest == "/window":
		delete(f.windows, f.current)
		fmt.Fprint(w, `{"value":null}`)
	case r.Method == "POST" && strings.HasSuffix(rest, "/maximize"):
		if f.maximizeErr {
			w.WriteHeader(http.StatusInternalServerError)
			fmt.Fprint(w, `{"value":{"error":"unsupported operation","message":"marker: maximize failed"}}`)
			return
		}
		fmt.Fprint(w, `{"value":{"x":0,"y":0,"width":1280,"height":720}}`)
	case r.Method == "POST" && rest == "/window/rect":
		fmt.Fprint(w, `{"value":{"x":0,"y":0,"width":800,"height":600}}`)
	default:
		w.WriteHeader(http.StatusNotFound)
		fmt.Fprintf(w, `{"value":{"error":"unknown command","message":"unhandled: %s %s"}}`, r.Method, r.URL.Path)
	}
}

// recorded returns a copy of the requests received so far.
func (f *fakeWebDriver) recorded() []recordedRequest {
	f.mu.Lock()
	defer f.mu.Unlock()
	return append([]recordedRequest(nil), f.requests...)
}

// reset clears the recorded requests.
func (f *fakeWebDriver) reset() {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.requests = nil
}

// switchAttempts counts the window switch requests targeting handle.
func (f *fakeWebDriver) switchAttempts(handle string) int {
	f.mu.Lock()
	defer f.mu.Unlock()
	n := 0
	for _, r := range f.requests {
		if r.Method == "POST" && r.Path == "/session/"+f.sessionID+"/window" && strings.Contains(r.Body, fmt.Sprintf("%q", handle)) {
			n++
		}
	}
	return n
}

func (f *fakeWebDriver) currentWindow() string {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.current
}

func (f *fakeWebDriver) hasWindow(name string) bool {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.windows[name]
}

// newFakeSession starts the fake server and returns a client with an established
// session. Requests made during session creation are discarded.
func newFakeSession(t *testing.T, f *fakeWebDriver) selenium.WebDriver {
	t.Helper()
	server := httptest.NewServer(f)
	t.Cleanup(server.Close)
	wd, err := selenium.NewRemote(selenium.Capabilities{"browserName": "fake"}, server.URL)
	if err != nil {
		t.Fatalf("NewRemote: %v", err)
	}
	f.reset()
	return wd
}

func assertRequests(t *testing.T, f *fakeWebDriver, want []recordedRequest) {
	t.Helper()
	if got := f.recorded(); !reflect.DeepEqual(got, want) {
		t.Errorf("requests mismatch:\n got: %+v\nwant: %+v", got, want)
	}
}

// A failing operation on B must still restore A before returning.
func TestWindowOperationFailureRestoresOriginalWindow(t *testing.T) {
	f := newW3CFake()
	f.maximizeErr = true
	wd := newFakeSession(t, f)

	err := wd.MaximizeWindow("B")
	if err == nil {
		t.Fatal("MaximizeWindow: expected an error, got nil")
	}
	var selErr *selenium.Error
	if !errors.As(err, &selErr) {
		t.Fatalf("MaximizeWindow error has type %T, want *selenium.Error", err)
	}
	if !strings.Contains(selErr.Message, "marker: maximize failed") {
		t.Errorf("error message %q does not contain the maximize marker", selErr.Message)
	}

	assertRequests(t, f, []recordedRequest{
		{"GET", "/session/w3c-test/window", ""},
		{"POST", "/session/w3c-test/window", `{"handle":"B"}`},
		{"POST", "/session/w3c-test/window/maximize", "{}"},
		{"POST", "/session/w3c-test/window", `{"handle":"A"}`},
	})
	if n := f.switchAttempts("A"); n != 1 {
		t.Errorf("restore switch to A attempted %d times, want exactly 1", n)
	}
	if got := f.currentWindow(); got != "A" {
		t.Errorf("server current window = %q, want %q", got, "A")
	}

	// Subsequent commands must still target A.
	handle, err := wd.CurrentWindowHandle()
	if err != nil {
		t.Fatalf("CurrentWindowHandle: %v", err)
	}
	if handle != "A" {
		t.Errorf("CurrentWindowHandle = %q, want %q", handle, "A")
	}
}

// When both the operation and the restore fail, the returned error must be a
// *WindowOperationError exposing both underlying errors.
func TestWindowOperationAndRestoreBothFail(t *testing.T) {
	f := newW3CFake()
	f.maximizeErr = true
	f.badSwitches["A"] = true // the original window can no longer be switched to
	wd := newFakeSession(t, f)

	err := wd.MaximizeWindow("B")
	if err == nil {
		t.Fatal("MaximizeWindow: expected an error, got nil")
	}
	var woe *selenium.WindowOperationError
	if !errors.As(err, &woe) {
		t.Fatalf("error has type %T (%v), want *selenium.WindowOperationError", err, err)
	}

	var opErr *selenium.Error
	if !errors.As(woe.OperationErr, &opErr) {
		t.Fatalf("OperationErr has type %T, want *selenium.Error", woe.OperationErr)
	}
	if !strings.Contains(opErr.Message, "marker: maximize failed") {
		t.Errorf("OperationErr message %q does not contain the maximize marker", opErr.Message)
	}

	var restoreErr *selenium.Error
	if !errors.As(woe.RestoreErr, &restoreErr) {
		t.Fatalf("RestoreErr has type %T, want *selenium.Error", woe.RestoreErr)
	}
	if !strings.Contains(restoreErr.Message, "marker: cannot switch to A") {
		t.Errorf("RestoreErr message %q does not contain the restore marker", restoreErr.Message)
	}

	// The combined message must distinguish the two failures.
	if text := err.Error(); !strings.Contains(text, "marker: maximize failed") || !strings.Contains(text, "marker: cannot switch to A") {
		t.Errorf("Error() = %q, want both failure markers", text)
	}

	if n := f.switchAttempts("A"); n != 1 {
		t.Errorf("restore switch to A attempted %d times, want exactly 1", n)
	}
}

// An empty name operates on the current window directly: no handle lookup and
// no switching.
func TestEmptyNameOperatesOnCurrentWindow(t *testing.T) {
	f := newW3CFake()
	wd := newFakeSession(t, f)

	if err := wd.MaximizeWindow(""); err != nil {
		t.Fatalf("MaximizeWindow: %v", err)
	}

	assertRequests(t, f, []recordedRequest{
		{"POST", "/session/w3c-test/window/maximize", "{}"},
	})
}

// Naming the current window performs the operation without switching.
func TestSameNameSkipsSwitching(t *testing.T) {
	f := newW3CFake()
	wd := newFakeSession(t, f)

	if err := wd.MaximizeWindow("A"); err != nil {
		t.Fatalf("MaximizeWindow: %v", err)
	}

	assertRequests(t, f, []recordedRequest{
		{"GET", "/session/w3c-test/window", ""},
		{"POST", "/session/w3c-test/window/maximize", "{}"},
	})
}

// Closing B while A is current switches back to A afterwards.
func TestCloseOtherWindowRestoresOriginal(t *testing.T) {
	f := newW3CFake()
	wd := newFakeSession(t, f)

	if err := wd.CloseWindow("B"); err != nil {
		t.Fatalf("CloseWindow: %v", err)
	}

	assertRequests(t, f, []recordedRequest{
		{"GET", "/session/w3c-test/window", ""},
		{"POST", "/session/w3c-test/window", `{"handle":"B"}`},
		{"DELETE", "/session/w3c-test/window", ""},
		{"POST", "/session/w3c-test/window", `{"handle":"A"}`},
	})
	if f.hasWindow("B") {
		t.Error("window B was not closed on the server")
	}
	if got := f.currentWindow(); got != "A" {
		t.Errorf("server current window = %q, want %q", got, "A")
	}

	handle, err := wd.CurrentWindowHandle()
	if err != nil {
		t.Fatalf("CurrentWindowHandle: %v", err)
	}
	if handle != "A" {
		t.Errorf("CurrentWindowHandle = %q, want %q", handle, "A")
	}
}

// Closing the current window itself must not trigger a restore of the just
// closed window.
func TestCloseCurrentWindowDoesNotRestore(t *testing.T) {
	f := newW3CFake()
	wd := newFakeSession(t, f)

	if err := wd.CloseWindow("A"); err != nil {
		t.Fatalf("CloseWindow: %v", err)
	}

	assertRequests(t, f, []recordedRequest{
		{"GET", "/session/w3c-test/window", ""},
		{"DELETE", "/session/w3c-test/window", ""},
	})
}

// Resizing another window also restores the original window.
func TestResizeOtherWindowRestoresOriginal(t *testing.T) {
	f := newW3CFake()
	wd := newFakeSession(t, f)

	if err := wd.ResizeWindow("B", 800, 600); err != nil {
		t.Fatalf("ResizeWindow: %v", err)
	}

	assertRequests(t, f, []recordedRequest{
		{"GET", "/session/w3c-test/window", ""},
		{"POST", "/session/w3c-test/window", `{"handle":"B"}`},
		{"POST", "/session/w3c-test/window/rect", `{"height":600,"width":800}`},
		{"POST", "/session/w3c-test/window", `{"handle":"A"}`},
	})
}

// If only the restore fails (the original window went away), that error is
// returned as-is, not wrapped in a WindowOperationError.
func TestRestoreFailureOnly(t *testing.T) {
	f := newW3CFake()
	f.badSwitches["A"] = true
	wd := newFakeSession(t, f)

	err := wd.MaximizeWindow("B")
	if err == nil {
		t.Fatal("MaximizeWindow: expected an error, got nil")
	}
	var woe *selenium.WindowOperationError
	if errors.As(err, &woe) {
		t.Fatalf("error is a *WindowOperationError (%v), want the plain restore error", err)
	}
	var selErr *selenium.Error
	if !errors.As(err, &selErr) {
		t.Fatalf("error has type %T, want *selenium.Error", err)
	}
	if !strings.Contains(selErr.Message, "marker: cannot switch to A") {
		t.Errorf("error message %q does not contain the restore marker", selErr.Message)
	}
	if n := f.switchAttempts("A"); n != 1 {
		t.Errorf("restore switch to A attempted %d times, want exactly 1", n)
	}
}

// If reading the current handle fails, the operation is not performed and no
// switching happens.
func TestCurrentHandleReadFailureSkipsOperation(t *testing.T) {
	f := newW3CFake()
	f.currentErr = true
	wd := newFakeSession(t, f)

	err := wd.MaximizeWindow("B")
	if err == nil {
		t.Fatal("MaximizeWindow: expected an error, got nil")
	}
	if !strings.Contains(err.Error(), "marker: current handle failed") {
		t.Errorf("error %q does not contain the current-handle marker", err)
	}

	assertRequests(t, f, []recordedRequest{
		{"GET", "/session/w3c-test/window", ""},
	})
}

// If the first switch fails, the operation is not performed and no further
// switching is attempted.
func TestInitialSwitchFailureSkipsOperation(t *testing.T) {
	f := newW3CFake()
	f.badSwitches["B"] = true
	wd := newFakeSession(t, f)

	err := wd.MaximizeWindow("B")
	if err == nil {
		t.Fatal("MaximizeWindow: expected an error, got nil")
	}
	if !strings.Contains(err.Error(), "marker: cannot switch to B") {
		t.Errorf("error %q does not contain the switch marker", err)
	}

	assertRequests(t, f, []recordedRequest{
		{"GET", "/session/w3c-test/window", ""},
		{"POST", "/session/w3c-test/window", `{"handle":"B"}`},
	})
}

// Legacy (JSON Wire) sessions keep the original request shapes and never use
// the W3C temporary-switch dance.
func TestLegacyProtocolRequestsUnchanged(t *testing.T) {
	f := newLegacyFake()
	wd := newFakeSession(t, f)

	if err := wd.MaximizeWindow("B"); err != nil {
		t.Fatalf("MaximizeWindow: %v", err)
	}
	assertRequests(t, f, []recordedRequest{
		{"GET", "/session/legacy-test/window_handle", ""},
		{"POST", "/session/legacy-test/window/A/maximize", ""},
	})

	f.reset()
	if err := wd.CloseWindow("B"); err != nil {
		t.Fatalf("CloseWindow: %v", err)
	}
	assertRequests(t, f, []recordedRequest{
		{"DELETE", "/session/legacy-test/window", ""},
	})

	// No W3C-style switch request may have been issued at all.
	if n := f.switchAttempts("A") + f.switchAttempts("B"); n != 0 {
		t.Errorf("legacy session issued %d switch requests, want 0", n)
	}
}
