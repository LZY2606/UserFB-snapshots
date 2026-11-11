package ssdp

import (
	"context"
	"errors"
	"net/http"
	"testing"
	"time"

	"github.com/huin/goupnp/httpu"
)

// stubHTTPUCtx implements HTTPUClientCtx with canned results.
type stubHTTPUCtx struct {
	responses []*http.Response
	err       error
}

func (s *stubHTTPUCtx) DoWithContext(req *http.Request, numSends int) ([]*http.Response, error) {
	return s.responses, s.err
}

// stubHTTPU implements the legacy HTTPUClient interface with canned results.
type stubHTTPU struct {
	responses []*http.Response
	err       error
}

func (s *stubHTTPU) Do(req *http.Request, timeout time.Duration, numSends int) ([]*http.Response, error) {
	return s.responses, s.err
}

func makeSearchResponse(st, usn, location, localAddr string) *http.Response {
	header := http.Header{}
	header.Set("ST", st)
	header.Set("USN", usn)
	header.Set("Location", location)
	if localAddr != "" {
		header.Set(httpu.LocalAddressHeader, localAddr)
	}
	return &http.Response{
		Status:     "200 OK",
		StatusCode: 200,
		Header:     header,
	}
}

// TestCollectorPartialDiscoveryRawSearchPartialError checks that RawSearch
// filters and returns the responses that were received even when the
// transport reports an error at the same time.
func TestCollectorPartialDiscoveryRawSearchPartialError(t *testing.T) {
	errSend := errors.New("one interface down")
	good := makeSearchResponse(UPNPRootDevice, "uuid:1::upnp:rootdevice", "http://192.0.2.10:5000/root.xml", "192.0.2.10")
	stub := &stubHTTPUCtx{responses: []*http.Response{good}, err: errSend}

	responses, err := RawSearch(context.Background(), stub, UPNPRootDevice, 1)
	if !errors.Is(err, errSend) {
		t.Fatalf("error %v does not unwrap to %v", err, errSend)
	}
	if len(responses) != 1 || responses[0] != good {
		t.Fatalf("responses = %v, want the valid response kept alongside the error", responses)
	}
}

// TestCollectorPartialDiscoveryLegacyRawSearchPartialError checks the same
// partial-result behavior for the legacy SSDPRawSearchCtx entry point.
func TestCollectorPartialDiscoveryLegacyRawSearchPartialError(t *testing.T) {
	errSend := errors.New("one interface down")
	good := makeSearchResponse(UPNPRootDevice, "uuid:1::upnp:rootdevice", "http://192.0.2.10:5000/root.xml", "192.0.2.10")
	stub := &stubHTTPU{responses: []*http.Response{good}, err: errSend}

	responses, err := SSDPRawSearchCtx(context.Background(), stub, UPNPRootDevice, 1, 1)
	if !errors.Is(err, errSend) {
		t.Fatalf("error %v does not unwrap to %v", err, errSend)
	}
	if len(responses) != 1 || responses[0] != good {
		t.Fatalf("responses = %v, want the valid response kept alongside the error", responses)
	}
}

// TestCollectorPartialDiscoveryDualNICSameDevice checks that the same device
// (USN and Location) discovered through two different local addresses is
// reported once per address.
func TestCollectorPartialDiscoveryDualNICSameDevice(t *testing.T) {
	usn := "uuid:abc::upnp:rootdevice"
	loc := "http://192.0.2.10:5000/root.xml"
	viaFirst := makeSearchResponse(UPNPRootDevice, usn, loc, "192.0.2.10")
	viaSecond := makeSearchResponse(UPNPRootDevice, usn, loc, "192.0.2.11")
	stub := &stubHTTPUCtx{responses: []*http.Response{viaFirst, viaSecond}}

	responses, err := RawSearch(context.Background(), stub, UPNPRootDevice, 1)
	if err != nil {
		t.Fatal(err)
	}
	if len(responses) != 2 || responses[0] != viaFirst || responses[1] != viaSecond {
		t.Fatalf("responses = %v, want both per-NIC sightings kept in order", responses)
	}
}

// TestCollectorPartialDiscoveryDuplicateResponses checks the deduplication
// rules: identical sightings through the same local address collapse to the
// first one, local addresses are normalized, responses with an invalid local
// address are discarded, and responses without any local address share a
// single unattributed bucket.
func TestCollectorPartialDiscoveryDuplicateResponses(t *testing.T) {
	usn := "uuid:abc::upnp:rootdevice"
	loc := "http://192.0.2.10:5000/root.xml"

	first := makeSearchResponse(UPNPRootDevice, usn, loc, "192.0.2.10")
	first.Header.Set("X-Seq", "1")
	dup := makeSearchResponse(UPNPRootDevice, usn, loc, "192.0.2.10")
	dup.Header.Set("X-Seq", "2")
	// The IPv4-mapped form normalizes to the same address as "192.0.2.10".
	mapped := makeSearchResponse(UPNPRootDevice, usn, loc, "::ffff:192.0.2.10")
	// A non-empty but invalid local address marks a bad response.
	invalid := makeSearchResponse(UPNPRootDevice, usn, loc, "not-an-ip")
	// Responses without a local address share one unattributed bucket.
	unattributed := makeSearchResponse(UPNPRootDevice, usn, loc, "")
	unattributedDup := makeSearchResponse(UPNPRootDevice, usn, loc, "")

	stub := &stubHTTPUCtx{responses: []*http.Response{first, dup, mapped, invalid, unattributed, unattributedDup}}
	responses, err := RawSearch(context.Background(), stub, UPNPRootDevice, 1)
	if err != nil {
		t.Fatal(err)
	}
	if len(responses) != 2 || responses[0] != first || responses[1] != unattributed {
		t.Fatalf("responses = %v, want the first sighting per address plus one unattributed", responses)
	}
	if got := responses[0].Header.Get("X-Seq"); got != "1" {
		t.Fatalf("kept response has X-Seq %q, want the first duplicate", got)
	}
}
