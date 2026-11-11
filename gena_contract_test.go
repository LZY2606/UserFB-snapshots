package goupnp

import (
	"context"
	"encoding/xml"
	"fmt"
	"math"
	"net/http"
	"net/http/httptest"
	"net/url"
	"reflect"
	"strings"
	"testing"
	"time"
)

const genaServiceType = "urn:test:Meter:1"

// TestGenaContract exercises the GENA single-subscription contract using real
// SUBSCRIBE and NOTIFY HTTP requests between an httptest device and
// httptest-hosted callback handlers.
func TestGenaContract(t *testing.T) {
	t.Run("initial_event_before_subscribe_response", testGenaInitialBeforeResponse)
	t.Run("consecutive_updates_and_snapshot", testGenaConsecutiveUpdates)
	t.Run("sequence_uint32_wrap", testGenaSequenceWrap)
	t.Run("duplicate_and_out_of_order", testGenaDuplicatesAndOrdering)
	t.Run("corrupt_packet_then_continue", testGenaCorruptThenContinue)
	t.Run("two_subscriptions_interleaved", testGenaTwoSubscriptions)
	t.Run("failed_or_mismatched_handshake_does_not_publish", testGenaFailedHandshake)
}

// ---------------------------------------------------------------------------
// Fake device.

type genaSubscribeRequest struct {
	method   string
	callback string
	nt       string
	timeout  string
	path     string
}

type genaSubscribeResponse struct {
	status  int
	sid     string
	timeout string
}

type genaDevice struct {
	server    *httptest.Server
	requests  chan genaSubscribeRequest
	responses chan genaSubscribeResponse
}

const genaDeviceXML = `<root xmlns="urn:schemas-upnp-org:device-1-0">
  <specVersion><major>1</major><minor>0</minor></specVersion>
  <device>
    <deviceType>urn:test:MeterDevice:1</deviceType>
    <friendlyName>GENA test meter</friendlyName>
    <UDN>uuid:gena-test-device</UDN>
    <serviceList>
      <service>
        <serviceType>` + genaServiceType + `</serviceType>
        <serviceId>urn:test:service:Meter:1</serviceId>
        <SCPDURL>/scpd.xml</SCPDURL>
        <controlURL>/control</controlURL>
        <eventSubURL>/events</eventSubURL>
      </service>
    </serviceList>
  </device>
</root>`

const genaSCPDXML = `<scpd xmlns="urn:schemas-upnp-org:service-1-0">
  <specVersion><major>1</major><minor>0</minor></specVersion>
  <serviceStateTable>
    <stateVariable sendEvents="yes"><name>Value</name><dataType>string</dataType></stateVariable>
  </serviceStateTable>
</scpd>`

func newGenaDevice(t *testing.T) *genaDevice {
	t.Helper()
	d := &genaDevice{
		requests:  make(chan genaSubscribeRequest),
		responses: make(chan genaSubscribeResponse),
	}
	mux := http.NewServeMux()
	mux.HandleFunc("/device.xml", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/xml")
		fmt.Fprint(w, genaDeviceXML)
	})
	mux.HandleFunc("/scpd.xml", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/xml")
		fmt.Fprint(w, genaSCPDXML)
	})
	mux.HandleFunc("/events", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != "SUBSCRIBE" {
			w.WriteHeader(http.StatusMethodNotAllowed)
			return
		}
		d.requests <- genaSubscribeRequest{
			method:   r.Method,
			callback: r.Header.Get("CALLBACK"),
			nt:       r.Header.Get("NT"),
			timeout:  r.Header.Get("TIMEOUT"),
			path:     r.URL.Path,
		}
		resp := <-d.responses
		if resp.status != 0 && resp.status != http.StatusOK {
			w.WriteHeader(resp.status)
			return
		}
		if resp.sid != "" {
			w.Header().Set("SID", resp.sid)
		}
		if resp.timeout != "" {
			w.Header().Set("TIMEOUT", resp.timeout)
		}
		w.WriteHeader(http.StatusOK)
	})
	d.server = httptest.NewServer(mux)
	t.Cleanup(d.server.Close)
	return d
}

func (d *genaDevice) service(t *testing.T) *Service {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	loc, err := url.Parse(d.server.URL + "/device.xml")
	if err != nil {
		t.Fatal(err)
	}
	root, err := DeviceByURLCtx(ctx, loc)
	if err != nil {
		t.Fatalf("DeviceByURLCtx: %v", err)
	}
	clients, err := NewServiceClientsFromRootDevice(root, loc, genaServiceType)
	if err != nil {
		t.Fatalf("NewServiceClientsFromRootDevice: %v", err)
	}
	if len(clients) != 1 {
		t.Fatalf("got %d service clients, want 1", len(clients))
	}
	svc := clients[0].Service
	if want := d.server.URL + "/events"; svc.EventSubURL.URL.String() != want {
		t.Fatalf("EventSubURL = %q, want %q", svc.EventSubURL.URL.String(), want)
	}
	return svc
}

// newGenaCallback creates an httptest server whose handler is the subscription
// itself, then builds the subscription using that server's URL. The closure
// lets the subscription and the server URL refer to each other.
func newGenaCallback(t *testing.T, svc *Service, handler EventStateHandler) (*EventSubscription, string) {
	t.Helper()
	var sub *EventSubscription
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		sub.ServeHTTP(w, r)
	}))
	t.Cleanup(server.Close)

	callback, err := url.Parse(server.URL + "/")
	if err != nil {
		t.Fatal(err)
	}
	sub = NewEventSubscription(svc, callback, handler)
	return sub, callback.String()
}

// ---------------------------------------------------------------------------
// Event recording and helpers.

type genaEvent struct {
	changed  map[string]string
	snapshot map[string]string
}

type genaRecorder struct {
	events chan genaEvent
}

func newGenaRecorder() *genaRecorder {
	return &genaRecorder{events: make(chan genaEvent, 32)}
}

func (r *genaRecorder) handler(changed, snapshot map[string]string) {
	r.events <- genaEvent{changed: changed, snapshot: snapshot}
}

func waitGenaEvent(t *testing.T, recorder *genaRecorder) genaEvent {
	t.Helper()
	select {
	case event := <-recorder.events:
		return event
	case <-time.After(3 * time.Second):
		t.Fatal("timed out waiting for event callback")
		return genaEvent{}
	}
}

func requireNoGenaEvent(t *testing.T, recorder *genaRecorder) {
	t.Helper()
	select {
	case event := <-recorder.events:
		t.Fatalf("unexpected event: changed=%v snapshot=%v", event.changed, event.snapshot)
	default:
	}
}

type genaStartResult struct {
	sid     string
	timeout time.Duration
	err     error
}

func startGena(sub *EventSubscription) <-chan genaStartResult {
	result := make(chan genaStartResult, 1)
	go func() {
		sid, timeout, err := sub.Start(context.Background())
		result <- genaStartResult{sid: sid, timeout: timeout, err: err}
	}()
	return result
}

func awaitGenaStart(t *testing.T, result <-chan genaStartResult) genaStartResult {
	t.Helper()
	select {
	case got := <-result:
		if got.err != nil {
			t.Fatalf("Start: %v", got.err)
		}
		return got
	case <-time.After(3 * time.Second):
		t.Fatal("timed out waiting for Start")
		return genaStartResult{}
	}
}

func checkSubscribeRequest(t *testing.T, request genaSubscribeRequest, callback string) {
	t.Helper()
	if request.method != "SUBSCRIBE" {
		t.Fatalf("request method = %q, want SUBSCRIBE", request.method)
	}
	if request.path != "/events" {
		t.Fatalf("subscribe path = %q, want /events", request.path)
	}
	if request.callback != "<"+callback+">" {
		t.Fatalf("CALLBACK = %q, want %q", request.callback, "<"+callback+">")
	}
	if !strings.EqualFold(request.nt, "upnp:event") {
		t.Fatalf("NT = %q, want upnp:event", request.nt)
	}
	if request.timeout != "Second-1800" {
		t.Fatalf("TIMEOUT = %q, want Second-1800", request.timeout)
	}
}

func completeGenaHandshake(t *testing.T, dev *genaDevice, sub *EventSubscription, callback string, response genaSubscribeResponse) genaStartResult {
	t.Helper()
	started := startGena(sub)
	request := <-dev.requests
	checkSubscribeRequest(t, request, callback)
	dev.responses <- response
	return awaitGenaStart(t, started)
}

type genaNotify struct {
	sid  string
	seq  string
	nt   string
	nts  string
	body string
}

func sendGenaNotify(t *testing.T, callbackURL string, notify genaNotify) int {
	t.Helper()
	req, err := http.NewRequest("NOTIFY", callbackURL, strings.NewReader(notify.body))
	if err != nil {
		t.Fatal(err)
	}
	if notify.sid != "" {
		req.Header.Set("SID", notify.sid)
	}
	if notify.seq != "" {
		req.Header.Set("SEQ", notify.seq)
	}
	nt := notify.nt
	if nt == "" {
		nt = "upnp:event"
	}
	nts := notify.nts
	if nts == "" {
		nts = "upnp:propchange"
	}
	req.Header.Set("NT", nt)
	req.Header.Set("NTS", nts)
	req.Header.Set("Content-Type", `text/xml; charset="utf-8"`)

	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	resp.Body.Close()
	return resp.StatusCode
}

func genaPropertySet(pairs ...string) string {
	var b strings.Builder
	b.WriteString(`<e:propertyset xmlns:e="` + EventXMLNamespace + `">`)
	for i := 0; i+1 < len(pairs); i += 2 {
		b.WriteString(`<e:property><` + pairs[i] + `>`)
		xml.EscapeText(&b, []byte(pairs[i+1]))
		b.WriteString(`</` + pairs[i] + `></e:property>`)
	}
	b.WriteString(`</e:propertyset>`)
	return b.String()
}

func requireStringMap(t *testing.T, what string, got, want map[string]string) {
	t.Helper()
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("%s = %v, want %v", what, got, want)
	}
}

// ---------------------------------------------------------------------------
// Scenarios.

func testGenaInitialBeforeResponse(t *testing.T) {
	dev := newGenaDevice(t)
	svc := dev.service(t)
	recorder := newGenaRecorder()
	sub, callback := newGenaCallback(t, svc, recorder.handler)

	started := startGena(sub)
	request := <-dev.requests
	checkSubscribeRequest(t, request, callback)

	const sid = "uuid:gena-initial"
	initialBody := genaPropertySet("Reading", "1", "State", "")

	// The device sends the initial event before releasing the SUBSCRIBE
	// response.
	if status := sendGenaNotify(t, callback, genaNotify{sid: sid, seq: "0", body: initialBody}); status != http.StatusOK {
		t.Fatalf("initial NOTIFY status = %d, want 200", status)
	}
	requireNoGenaEvent(t, recorder)

	// An identical retransmission is acknowledged but still not delivered.
	if status := sendGenaNotify(t, callback, genaNotify{sid: sid, seq: "0", body: initialBody}); status != http.StatusOK {
		t.Fatalf("duplicate initial NOTIFY status = %d, want 200", status)
	}
	requireNoGenaEvent(t, recorder)

	// A contradictory initial event must be rejected, leaving the first one
	// intact and still unpublished.
	if status := sendGenaNotify(t, callback, genaNotify{sid: sid, seq: "0", body: genaPropertySet("Reading", "2")}); status != http.StatusPreconditionFailed {
		t.Fatalf("contradictory initial NOTIFY status = %d, want 412", status)
	}
	requireNoGenaEvent(t, recorder)

	dev.responses <- genaSubscribeResponse{sid: sid, timeout: "Second-1800"}
	result := awaitGenaStart(t, started)
	if result.sid != sid {
		t.Fatalf("Start SID = %q, want %q", result.sid, sid)
	}
	if result.timeout != 30*time.Minute {
		t.Fatalf("Start timeout = %v, want 30m", result.timeout)
	}

	event := waitGenaEvent(t, recorder)
	requireStringMap(t, "changed", event.changed, map[string]string{"Reading": "1", "State": ""})
	requireStringMap(t, "snapshot", event.snapshot, map[string]string{"Reading": "1", "State": ""})
	requireNoGenaEvent(t, recorder)
}

func testGenaConsecutiveUpdates(t *testing.T) {
	dev := newGenaDevice(t)
	svc := dev.service(t)
	recorder := newGenaRecorder()
	sub, callback := newGenaCallback(t, svc, recorder.handler)

	// Querying the subscription from inside the handler must not deadlock:
	// the handler does not run while the subscription's state lock is held.
	sub.Handler = func(changed, snapshot map[string]string) {
		_ = sub.Snapshot()
		recorder.handler(changed, snapshot)
	}

	const sid = "uuid:gena-consecutive"
	result := completeGenaHandshake(t, dev, sub, callback, genaSubscribeResponse{sid: sid, timeout: "Second-1800"})
	_ = result

	// First event initialises the snapshot, including an empty value.
	if status := sendGenaNotify(t, callback, genaNotify{sid: sid, seq: "0", body: genaPropertySet("Reading", "1", "State", "")}); status != http.StatusOK {
		t.Fatalf("SEQ 0 status = %d, want 200", status)
	}
	first := waitGenaEvent(t, recorder)
	requireStringMap(t, "first changed", first.changed, map[string]string{"Reading": "1", "State": ""})
	requireStringMap(t, "first snapshot", first.snapshot, map[string]string{"Reading": "1", "State": ""})

	// Handler-visible snapshot and caller mutations must be independent of
	// subscription state.
	first.changed["Reading"] = "mutated"
	first.snapshot["State"] = "mutated"
	snapshotFromOutside := sub.Snapshot()
	snapshotFromOutside["Reading"] = "mutated"

	if status := sendGenaNotify(t, callback, genaNotify{sid: sid, seq: "1", body: genaPropertySet("Reading", "2", "Extra", "x")}); status != http.StatusOK {
		t.Fatalf("SEQ 1 status = %d, want 200", status)
	}
	second := waitGenaEvent(t, recorder)
	requireStringMap(t, "second changed", second.changed, map[string]string{"Reading": "2", "Extra": "x"})
	requireStringMap(t, "second snapshot", second.snapshot, map[string]string{
		"Reading": "2",
		"State":   "",
		"Extra":   "x",
	})

	if status := sendGenaNotify(t, callback, genaNotify{sid: sid, seq: "2", body: genaPropertySet("State", "on", "Reading", "<&")}); status != http.StatusOK {
		t.Fatalf("SEQ 2 status = %d, want 200", status)
	}
	third := waitGenaEvent(t, recorder)
	requireStringMap(t, "third changed", third.changed, map[string]string{"State": "on", "Reading": "<&"})
	requireStringMap(t, "third snapshot", third.snapshot, map[string]string{
		"Reading": "<&",
		"State":   "on",
		"Extra":   "x",
	})
}

func testGenaSequenceWrap(t *testing.T) {
	dev := newGenaDevice(t)
	svc := dev.service(t)
	recorder := newGenaRecorder()
	sub, callback := newGenaCallback(t, svc, recorder.handler)

	const sid = "uuid:gena-wrap"
	completeGenaHandshake(t, dev, sub, callback, genaSubscribeResponse{sid: sid, timeout: "Second-1800"})

	if status := sendGenaNotify(t, callback, genaNotify{sid: sid, seq: "0", body: genaPropertySet("Count", "start")}); status != http.StatusOK {
		t.Fatalf("SEQ 0 status = %d, want 200", status)
	}
	waitGenaEvent(t, recorder)

	// Pretend a very long-running subscription has reached the end of the
	// uint32 sequence space. This is a package-internal shortcut; no billions
	// of notifications are sent.
	sub.mu.Lock()
	sub.lastSeq = math.MaxUint32
	sub.mu.Unlock()

	if status := sendGenaNotify(t, callback, genaNotify{sid: sid, seq: "1", body: genaPropertySet("Count", "wrapped")}); status != http.StatusOK {
		t.Fatalf("post-wrap SEQ 1 status = %d, want 200", status)
	}
	wrapped := waitGenaEvent(t, recorder)
	requireStringMap(t, "wrapped changed", wrapped.changed, map[string]string{"Count": "wrapped"})

	// Zero is never a legal successor, even after wrapping.
	if status := sendGenaNotify(t, callback, genaNotify{sid: sid, seq: "0", body: genaPropertySet("Count", "zero-again")}); status != http.StatusPreconditionFailed {
		t.Fatalf("SEQ 0 after wrap status = %d, want 412", status)
	}
	requireNoGenaEvent(t, recorder)

	if status := sendGenaNotify(t, callback, genaNotify{sid: sid, seq: "2", body: genaPropertySet("Count", "next")}); status != http.StatusOK {
		t.Fatalf("SEQ 2 after wrap status = %d, want 200", status)
	}
	next := waitGenaEvent(t, recorder)
	requireStringMap(t, "next changed", next.changed, map[string]string{"Count": "next"})
}

func testGenaDuplicatesAndOrdering(t *testing.T) {
	dev := newGenaDevice(t)
	svc := dev.service(t)
	recorder := newGenaRecorder()
	sub, callback := newGenaCallback(t, svc, recorder.handler)

	const sid = "uuid:gena-order"
	completeGenaHandshake(t, dev, sub, callback, genaSubscribeResponse{sid: sid, timeout: "Second-1800"})

	if status := sendGenaNotify(t, callback, genaNotify{sid: sid, seq: "0", body: genaPropertySet("Value", "zero")}); status != http.StatusOK {
		t.Fatalf("SEQ 0 status = %d, want 200", status)
	}
	waitGenaEvent(t, recorder)

	if status := sendGenaNotify(t, callback, genaNotify{sid: sid, seq: "1", body: genaPropertySet("Value", "one")}); status != http.StatusOK {
		t.Fatalf("SEQ 1 status = %d, want 200", status)
	}
	waitGenaEvent(t, recorder)

	// An exact duplicate of the previous packet is acknowledged but not
	// delivered again.
	if status := sendGenaNotify(t, callback, genaNotify{sid: sid, seq: "1", body: genaPropertySet("Value", "one")}); status != http.StatusOK {
		t.Fatalf("duplicate SEQ 1 status = %d, want 200", status)
	}
	requireNoGenaEvent(t, recorder)

	// The same sequence number with different contents is rejected.
	if status := sendGenaNotify(t, callback, genaNotify{sid: sid, seq: "1", body: genaPropertySet("Value", "changed")}); status != http.StatusPreconditionFailed {
		t.Fatalf("different SEQ 1 status = %d, want 412", status)
	}
	requireNoGenaEvent(t, recorder)

	// A skipped sequence number is rejected.
	if status := sendGenaNotify(t, callback, genaNotify{sid: sid, seq: "3", body: genaPropertySet("Value", "three")}); status != http.StatusPreconditionFailed {
		t.Fatalf("skipped SEQ 3 status = %d, want 412", status)
	}
	requireNoGenaEvent(t, recorder)

	// The correct successor still works after the rejections.
	if status := sendGenaNotify(t, callback, genaNotify{sid: sid, seq: "2", body: genaPropertySet("Value", "two")}); status != http.StatusOK {
		t.Fatalf("SEQ 2 status = %d, want 200", status)
	}
	event := waitGenaEvent(t, recorder)
	requireStringMap(t, "changed", event.changed, map[string]string{"Value": "two"})
	requireStringMap(t, "snapshot", event.snapshot, map[string]string{"Value": "two"})
}

func testGenaCorruptThenContinue(t *testing.T) {
	dev := newGenaDevice(t)
	svc := dev.service(t)
	recorder := newGenaRecorder()
	sub, callback := newGenaCallback(t, svc, recorder.handler)

	const sid = "uuid:gena-corrupt"
	completeGenaHandshake(t, dev, sub, callback, genaSubscribeResponse{sid: sid, timeout: "Second-1800"})

	if status := sendGenaNotify(t, callback, genaNotify{sid: sid, seq: "0", body: genaPropertySet("Value", "zero")}); status != http.StatusOK {
		t.Fatalf("SEQ 0 status = %d, want 200", status)
	}
	waitGenaEvent(t, recorder)

	corrupt := []string{
		// Truncated / unclosed XML.
		`<e:propertyset xmlns:e="` + EventXMLNamespace + `"><e:property><A>1</e:property>`,
		// Duplicate variable in the same notification.
		`<e:propertyset xmlns:e="` + EventXMLNamespace + `"><e:property><A>1</A></e:property><e:property><A>2</A></e:property></e:propertyset>`,
		// Nested value.
		`<e:propertyset xmlns:e="` + EventXMLNamespace + `"><e:property><A><B>1</B></A></e:property></e:propertyset>`,
		// A valid first property followed by a damaged one: the whole packet
		// must be rejected without publishing the first half.
		`<e:propertyset xmlns:e="` + EventXMLNamespace + `"><e:property><Partial>must-not-leak</Partial></e:property><e:property><B>2</`,
	}
	for i, body := range corrupt {
		if status := sendGenaNotify(t, callback, genaNotify{sid: sid, seq: "1", body: body}); status != http.StatusBadRequest {
			t.Fatalf("corrupt packet %d status = %d, want 400", i, status)
		}
		requireNoGenaEvent(t, recorder)
	}

	// State is unchanged; the correct SEQ 1 is still accepted, and no value
	// from the partially valid corrupt packet leaked into it.
	if status := sendGenaNotify(t, callback, genaNotify{sid: sid, seq: "1", body: genaPropertySet("B", "2")}); status != http.StatusOK {
		t.Fatalf("valid SEQ 1 status = %d, want 200", status)
	}
	event := waitGenaEvent(t, recorder)
	requireStringMap(t, "changed", event.changed, map[string]string{"B": "2"})
	requireStringMap(t, "snapshot", event.snapshot, map[string]string{"Value": "zero", "B": "2"})

	if status := sendGenaNotify(t, callback, genaNotify{sid: sid, seq: "2", body: genaPropertySet("C", "3")}); status != http.StatusOK {
		t.Fatalf("SEQ 2 status = %d, want 200", status)
	}
	next := waitGenaEvent(t, recorder)
	requireStringMap(t, "continued snapshot", next.snapshot, map[string]string{"Value": "zero", "B": "2", "C": "3"})
}

func testGenaTwoSubscriptions(t *testing.T) {
	dev := newGenaDevice(t)
	svc := dev.service(t)

	recorderA := newGenaRecorder()
	subA, callbackA := newGenaCallback(t, svc, recorderA.handler)
	recorderB := newGenaRecorder()
	subB, callbackB := newGenaCallback(t, svc, recorderB.handler)

	// Both subscriptions deliberately use the same Service (and thus the same
	// event subscription address).
	if subA.Service.EventSubURL.URL.String() != subB.Service.EventSubURL.URL.String() {
		t.Fatal("test setup: subscriptions do not share the service event URL")
	}

	const (
		sidA = "uuid:gena-sub-a"
		sidB = "uuid:gena-sub-b"
	)
	completeGenaHandshake(t, dev, subA, callbackA, genaSubscribeResponse{sid: sidA, timeout: "Second-1800"})
	completeGenaHandshake(t, dev, subB, callbackB, genaSubscribeResponse{sid: sidB, timeout: "Second-1800"})

	if status := sendGenaNotify(t, callbackA, genaNotify{sid: sidA, seq: "0", body: genaPropertySet("A", "a0")}); status != http.StatusOK {
		t.Fatalf("A SEQ 0 status = %d, want 200", status)
	}
	if status := sendGenaNotify(t, callbackB, genaNotify{sid: sidB, seq: "0", body: genaPropertySet("B", "b0")}); status != http.StatusOK {
		t.Fatalf("B SEQ 0 status = %d, want 200", status)
	}
	eventA := waitGenaEvent(t, recorderA)
	eventB := waitGenaEvent(t, recorderB)
	requireStringMap(t, "A initial snapshot", eventA.snapshot, map[string]string{"A": "a0"})
	requireStringMap(t, "B initial snapshot", eventB.snapshot, map[string]string{"B": "b0"})

	// Interleave updates, each with its own SID and snapshot.
	if status := sendGenaNotify(t, callbackA, genaNotify{sid: sidA, seq: "1", body: genaPropertySet("A", "a1")}); status != http.StatusOK {
		t.Fatalf("A SEQ 1 status = %d, want 200", status)
	}
	if status := sendGenaNotify(t, callbackB, genaNotify{sid: sidB, seq: "1", body: genaPropertySet("B", "b1")}); status != http.StatusOK {
		t.Fatalf("B SEQ 1 status = %d, want 200", status)
	}
	if status := sendGenaNotify(t, callbackA, genaNotify{sid: sidA, seq: "2", body: genaPropertySet("Extra", "xa")}); status != http.StatusOK {
		t.Fatalf("A SEQ 2 status = %d, want 200", status)
	}

	eventA = waitGenaEvent(t, recorderA)
	requireStringMap(t, "A SEQ 1 snapshot", eventA.snapshot, map[string]string{"A": "a1"})
	eventB = waitGenaEvent(t, recorderB)
	requireStringMap(t, "B SEQ 1 snapshot", eventB.snapshot, map[string]string{"B": "b1"})
	eventA = waitGenaEvent(t, recorderA)
	requireStringMap(t, "A SEQ 2 snapshot", eventA.snapshot, map[string]string{"A": "a1", "Extra": "xa"})

	// Events carrying the other subscription's SID must not be accepted.
	if status := sendGenaNotify(t, callbackA, genaNotify{sid: sidB, seq: "3", body: genaPropertySet("A", "stolen")}); status != http.StatusPreconditionFailed {
		t.Fatalf("cross-SID NOTIFY to A status = %d, want 412", status)
	}
	requireNoGenaEvent(t, recorderA)
	if status := sendGenaNotify(t, callbackB, genaNotify{sid: sidA, seq: "2", body: genaPropertySet("B", "stolen")}); status != http.StatusPreconditionFailed {
		t.Fatalf("cross-SID NOTIFY to B status = %d, want 412", status)
	}
	requireNoGenaEvent(t, recorderB)

	// Wrong eventing headers are association errors.
	if status := sendGenaNotify(t, callbackA, genaNotify{sid: sidA, seq: "3", nt: "upnp:event", nts: "wrong", body: genaPropertySet("A", "bad-nts")}); status != http.StatusPreconditionFailed {
		t.Fatalf("bad NTS status = %d, want 412", status)
	}
	requireNoGenaEvent(t, recorderA)

	if status := sendGenaNotify(t, callbackA, genaNotify{sid: sidA, seq: "3", body: genaPropertySet("A", "a3")}); status != http.StatusOK {
		t.Fatalf("A SEQ 3 status = %d, want 200", status)
	}
	eventA = waitGenaEvent(t, recorderA)
	requireStringMap(t, "A final snapshot", eventA.snapshot, map[string]string{"A": "a3", "Extra": "xa"})
}

func testGenaFailedHandshake(t *testing.T) {
	t.Run("pending_dropped_on_sid_mismatch", func(t *testing.T) {
		dev := newGenaDevice(t)
		svc := dev.service(t)
		recorder := newGenaRecorder()
		sub, callback := newGenaCallback(t, svc, recorder.handler)

		started := startGena(sub)
		request := <-dev.requests
		checkSubscribeRequest(t, request, callback)

		// The held initial event's SID does not match the eventual response.
		if status := sendGenaNotify(t, callback, genaNotify{sid: "uuid:wrong", seq: "0", body: genaPropertySet("Value", "held")}); status != http.StatusOK {
			t.Fatalf("initial NOTIFY status = %d, want 200", status)
		}
		requireNoGenaEvent(t, recorder)

		dev.responses <- genaSubscribeResponse{sid: "uuid:right", timeout: "Second-1800"}
		result := awaitGenaStart(t, started)
		if result.sid != "uuid:right" {
			t.Fatalf("Start SID = %q, want uuid:right", result.sid)
		}
		requireNoGenaEvent(t, recorder)

		// The subscription itself is still usable; the first accepted event
		// is the real SEQ 0.
		if status := sendGenaNotify(t, callback, genaNotify{sid: "uuid:right", seq: "0", body: genaPropertySet("Value", "real")}); status != http.StatusOK {
			t.Fatalf("real SEQ 0 status = %d, want 200", status)
		}
		event := waitGenaEvent(t, recorder)
		requireStringMap(t, "snapshot", event.snapshot, map[string]string{"Value": "real"})
	})

	t.Run("missing_sid_response_discards_pending", func(t *testing.T) {
		dev := newGenaDevice(t)
		svc := dev.service(t)
		recorder := newGenaRecorder()
		sub, callback := newGenaCallback(t, svc, recorder.handler)

		started := startGena(sub)
		request := <-dev.requests
		checkSubscribeRequest(t, request, callback)
		if status := sendGenaNotify(t, callback, genaNotify{sid: "uuid:never-confirmed", seq: "0", body: genaPropertySet("Value", "held")}); status != http.StatusOK {
			t.Fatalf("initial NOTIFY status = %d, want 200", status)
		}
		requireNoGenaEvent(t, recorder)

		dev.responses <- genaSubscribeResponse{timeout: "Second-1800"}
		select {
		case got := <-started:
			if got.err == nil {
				t.Fatal("Start succeeded without a SID")
			}
		case <-time.After(3 * time.Second):
			t.Fatal("timed out waiting for failed Start")
		}
		requireNoGenaEvent(t, recorder)
	})

	t.Run("invalid_timeout_response", func(t *testing.T) {
		dev := newGenaDevice(t)
		svc := dev.service(t)
		recorder := newGenaRecorder()
		sub, callback := newGenaCallback(t, svc, recorder.handler)

		started := startGena(sub)
		request := <-dev.requests
		checkSubscribeRequest(t, request, callback)
		dev.responses <- genaSubscribeResponse{sid: "uuid:bad-timeout", timeout: "Second-bogus"}
		select {
		case got := <-started:
			if got.err == nil {
				t.Fatal("Start succeeded with an invalid TIMEOUT")
			}
		case <-time.After(3 * time.Second):
			t.Fatal("timed out waiting for failed Start")
		}
		requireNoGenaEvent(t, recorder)
	})

	t.Run("relative_callback_url", func(t *testing.T) {
		dev := newGenaDevice(t)
		svc := dev.service(t)

		relative, err := url.Parse("/callback")
		if err != nil {
			t.Fatal(err)
		}
		sub := NewEventSubscription(svc, relative, nil)
		if _, _, err := sub.Start(context.Background()); err == nil {
			t.Fatal("Start succeeded with a relative callback URL")
		}
	})
}
