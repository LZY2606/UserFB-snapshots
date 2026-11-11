package goupnp

import (
	"context"
	"errors"
	"io"
	"net/http"
	"strings"
	"testing"

	"github.com/huin/goupnp/httpu"
	"github.com/huin/goupnp/ssdp"
)

// stubDiscoveryClient implements httpu.ClientInterfaceCtx with canned
// results, standing in for the per-interface HTTPU clients.
type stubDiscoveryClient struct {
	responses []*http.Response
	err       error
}

func (s *stubDiscoveryClient) DoWithContext(req *http.Request, numSends int) ([]*http.Response, error) {
	return s.responses, s.err
}

// roundTripFunc is an in-memory http.RoundTripper, so tests never touch a
// real device.
type roundTripFunc func(*http.Request) (*http.Response, error)

func (f roundTripFunc) RoundTrip(req *http.Request) (*http.Response, error) {
	return f(req)
}

func discoveryResponse(usn, location, localAddr string) *http.Response {
	header := http.Header{}
	header.Set("ST", ssdp.UPNPRootDevice)
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

const testRootDeviceXML = `<?xml version="1.0"?>
<root xmlns="urn:schemas-upnp-org:device-1-0">
  <specVersion><major>1</major><minor>0</minor></specVersion>
  <device>
    <deviceType>urn:schemas-upnp-org:device:test:1</deviceType>
    <friendlyName>Test Device</friendlyName>
    <UDN>uuid:test-device</UDN>
  </device>
</root>`

func xmlHTTPResponse(body string) *http.Response {
	return &http.Response{
		Status:     "200 OK",
		StatusCode: 200,
		Header:     http.Header{"Content-Type": []string{"text/xml"}},
		Body:       io.NopCloser(strings.NewReader(body)),
	}
}

func withDescriptionClient(t *testing.T, rt http.RoundTripper) {
	t.Helper()
	old := HTTPClientDefault
	HTTPClientDefault = &http.Client{Transport: rt}
	t.Cleanup(func() { HTTPClientDefault = old })
}

// TestCollectorPartialDiscoveryDescriptionFetchFailure checks that a partial
// discovery (responses plus a discovery-level error) still probes every
// discovered device, and that a failing description fetch is recorded on that
// device only, without masking the discovery error or stopping later devices.
func TestCollectorPartialDiscoveryDescriptionFetchFailure(t *testing.T) {
	errNIC := errors.New("nic 2 down")
	errFetch := errors.New("connection refused")

	client := &stubDiscoveryClient{
		responses: []*http.Response{
			discoveryResponse("uuid:1::upnp:rootdevice", "http://192.0.2.10:5000/good.xml", "192.0.2.10"),
			discoveryResponse("uuid:2::upnp:rootdevice", "http://192.0.2.11:5000/bad.xml", "192.0.2.11"),
			discoveryResponse("uuid:3::upnp:rootdevice", "http://192.0.2.10:5000/after.xml", "192.0.2.10"),
		},
		err: errNIC,
	}
	withDescriptionClient(t, roundTripFunc(func(req *http.Request) (*http.Response, error) {
		if strings.Contains(req.URL.String(), "bad.xml") {
			return nil, errFetch
		}
		return xmlHTTPResponse(testRootDeviceXML), nil
	}))

	devices, err := DiscoverDevicesWithClientCtx(context.Background(), ssdp.UPNPRootDevice, client, 1)

	// The discovery-level error is reported, but the partial results survive.
	if !errors.Is(err, errNIC) {
		t.Fatalf("discovery error %v does not unwrap to %v", err, errNIC)
	}
	if len(devices) != 3 {
		t.Fatalf("got %d devices, want 3", len(devices))
	}

	if devices[0].Err != nil || devices[0].Root == nil {
		t.Fatalf("devices[0] = %+v, want a probed root device", devices[0])
	}
	if devices[0].USN != "uuid:1::upnp:rootdevice" {
		t.Fatalf("devices[0].USN = %q", devices[0].USN)
	}
	if devices[0].Location == nil || devices[0].Location.String() != "http://192.0.2.10:5000/good.xml" {
		t.Fatalf("devices[0].Location = %v", devices[0].Location)
	}
	if got := devices[0].LocalAddr.String(); got != "192.0.2.10" {
		t.Fatalf("devices[0].LocalAddr = %v", got)
	}

	// The failing description fetch is recorded on that device only, and does
	// not mask the discovery-level error.
	if devices[1].Root != nil || devices[1].Err == nil {
		t.Fatalf("devices[1] = %+v, want Err set and no root device", devices[1])
	}
	if !errors.Is(err, errNIC) || errors.Is(devices[1].Err, errNIC) {
		t.Fatalf("description error %v must not replace discovery error %v", devices[1].Err, err)
	}
	if devices[1].Location == nil || devices[1].USN != "uuid:2::upnp:rootdevice" {
		t.Fatalf("devices[1] lost its discovery identity: %+v", devices[1])
	}

	// Devices after the failure are still probed.
	if devices[2].Err != nil || devices[2].Root == nil {
		t.Fatalf("devices[2] = %+v, want a probed root device", devices[2])
	}
}

// TestCollectorPartialDiscoveryParentCancel checks that once the parent
// context is canceled, no further description fetches are started and the
// devices not yet probed carry the context error instead of disappearing.
func TestCollectorPartialDiscoveryParentCancel(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	client := &stubDiscoveryClient{
		responses: []*http.Response{
			discoveryResponse("uuid:1::upnp:rootdevice", "http://192.0.2.10:5000/first.xml", "192.0.2.10"),
			discoveryResponse("uuid:2::upnp:rootdevice", "http://192.0.2.10:5000/second.xml", "192.0.2.10"),
			discoveryResponse("uuid:3::upnp:rootdevice", "http://192.0.2.10:5000/third.xml", "192.0.2.10"),
		},
	}
	fetches := 0
	withDescriptionClient(t, roundTripFunc(func(req *http.Request) (*http.Response, error) {
		fetches++
		cancel() // the parent context goes away during the first fetch
		return xmlHTTPResponse(testRootDeviceXML), nil
	}))

	devices, err := DiscoverDevicesWithClientCtx(ctx, ssdp.UPNPRootDevice, client, 1)
	if err != nil {
		t.Fatalf("discovery itself succeeded, got error %v", err)
	}
	if len(devices) != 3 {
		t.Fatalf("got %d devices, want all 3 discovered devices kept", len(devices))
	}
	if fetches != 1 {
		t.Fatalf("made %d description fetches after cancellation, want 1", fetches)
	}
	if devices[0].Err != nil || devices[0].Root == nil {
		t.Fatalf("devices[0] = %+v, want a probed root device", devices[0])
	}
	for i := 1; i < 3; i++ {
		if devices[i].Root != nil {
			t.Fatalf("devices[%d] unexpectedly probed after cancellation", i)
		}
		if !errors.Is(devices[i].Err, context.Canceled) {
			t.Fatalf("devices[%d].Err = %v, want context.Canceled", i, devices[i].Err)
		}
		if devices[i].Location == nil || devices[i].USN == "" {
			t.Fatalf("devices[%d] lost its discovery identity: %+v", i, devices[i])
		}
	}
}
