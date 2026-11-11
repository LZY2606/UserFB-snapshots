package zeroconf

import (
	"context"
	"net"
	"testing"
	"time"

	"github.com/miekg/dns"
	"golang.org/x/net/ipv4"
)

const (
	contractService = "_meter._tcp"
	contractDomain  = "local"
)

// discoveryHarness runs the production mainloop over the same localhost UDP
// receive path used by the baseline test. Follow-up queries are captured
// through the client's query egress instead of being sent to the network.
type discoveryHarness struct {
	t       *testing.T
	client  *client
	sender  *net.UDPConn
	entries chan *ServiceEntry
	queries chan *dns.Msg
	cancel  context.CancelFunc
	done    chan struct{}
}

func newDiscoveryHarness(t *testing.T, instance, service, domain string, browsing bool, querySender func(*dns.Msg) error) *discoveryHarness {
	t.Helper()

	recv, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}

	h := &discoveryHarness{
		t:       t,
		entries: make(chan *ServiceEntry, 64),
		queries: make(chan *dns.Msg, 256),
		done:    make(chan struct{}),
	}
	h.client = &client{ipv4conn: ipv4.NewPacketConn(recv)}
	if querySender == nil {
		querySender = func(m *dns.Msg) error {
			h.queries <- m
			return nil
		}
	}
	h.client.querySender = querySender

	params := newLookupParams(instance, service, domain, browsing, h.entries)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	h.cancel = cancel

	go func() {
		h.client.mainloop(ctx, params)
		close(h.done)
	}()

	sender, err := net.DialUDP("udp4", nil, recv.LocalAddr().(*net.UDPAddr))
	if err != nil {
		cancel()
		t.Fatal(err)
	}
	h.sender = sender

	t.Cleanup(func() {
		closeQuietly(sender)
		cancel()
		select {
		case <-h.done:
		case <-time.After(time.Second):
		}
	})

	return h
}

func closeQuietly(c *net.UDPConn) {
	if c != nil {
		_ = c.Close()
	}
}

func (h *discoveryHarness) send(msg *dns.Msg) {
	h.t.Helper()
	buf, err := msg.Pack()
	if err != nil {
		h.t.Fatal(err)
	}
	if _, err := h.sender.Write(buf); err != nil {
		h.t.Fatal(err)
	}
}

func (h *discoveryHarness) sendRR(rr dns.RR) {
	h.t.Helper()
	h.send(&dns.Msg{MsgHdr: dns.MsgHdr{Response: true}, Answer: []dns.RR{rr}})
}

// collectUntil reads entries until the predicate observes the final sentinel
// packet. The sentinel proves that all packets queued before it were consumed;
// no sleep is used to guess whether an earlier result was missed.
func (h *discoveryHarness) collectUntil(sentinel func(*ServiceEntry) bool, label string) []*ServiceEntry {
	h.t.Helper()

	var got []*ServiceEntry
	deadline := time.After(3 * time.Second)
	for {
		select {
		case e, ok := <-h.entries:
			if !ok {
				h.t.Fatalf("%s: entries channel closed before sentinel", label)
			}
			if sentinel(e) {
				return got
			}
			got = append(got, e)
		case <-deadline:
			h.t.Fatalf("%s: sentinel not received", label)
		}
	}
}

// expectQueries waits for exactly want follow-up questions. A short quiet
// window afterwards verifies that repeated responses did not trigger extra
// queries; it does not influence result delivery.
func (h *discoveryHarness) expectQueries(want int, label string) []*dns.Msg {
	h.t.Helper()

	var got []*dns.Msg
	deadline := time.After(2 * time.Second)
	for len(got) < want {
		select {
		case q := <-h.queries:
			got = append(got, q)
		case <-deadline:
			h.t.Fatalf("%s: expected %d follow-up queries, got %d", label, want, len(got))
		}
	}

	select {
	case q := <-h.queries:
		h.t.Fatalf("%s: unexpected extra follow-up query %v", label, questionSummary(q))
	case <-time.After(100 * time.Millisecond):
	}
	return got
}

func questionSummary(m *dns.Msg) []string {
	var out []string
	for _, q := range m.Question {
		out = append(out, dns.TypeToString[q.Qtype]+" "+q.Name)
	}
	return out
}

func queryHas(qs []*dns.Msg, name string, qtype uint16) bool {
	for _, q := range qs {
		for _, question := range q.Question {
			if equalName(question.Name, name) && question.Qtype == qtype {
				return true
			}
		}
	}
	return false
}

func queryCount(qs []*dns.Msg, name string, qtype uint16) int {
	count := 0
	for _, q := range qs {
		for _, question := range q.Question {
			if equalName(question.Name, name) && question.Qtype == qtype {
				count++
			}
		}
	}
	return count
}

func serviceFQDN(service, domain string) string {
	return trimDot(service+"."+trimDot(domain)) + "."
}

func instanceFQDN(instance, service, domain string) string {
	return trimDot(instance) + "." + serviceFQDN(service, domain)
}

func ptrRR(owner, target string, ttl uint32) *dns.PTR {
	return &dns.PTR{Hdr: dns.RR_Header{Name: owner, Rrtype: dns.TypePTR, Class: dns.ClassINET, Ttl: ttl}, Ptr: target}
}

func srvRR(name, target string, port int, ttl uint32) *dns.SRV {
	return &dns.SRV{Hdr: dns.RR_Header{Name: name, Rrtype: dns.TypeSRV, Class: dns.ClassINET, Ttl: ttl}, Port: uint16(port), Target: target}
}

func txtRR(name string, text []string, ttl uint32) *dns.TXT {
	return &dns.TXT{Hdr: dns.RR_Header{Name: name, Rrtype: dns.TypeTXT, Class: dns.ClassINET, Ttl: ttl}, Txt: text}
}

func aRR(host string, ip net.IP, ttl uint32) *dns.A {
	return &dns.A{Hdr: dns.RR_Header{Name: host, Rrtype: dns.TypeA, Class: dns.ClassINET, Ttl: ttl}, A: ip}
}

func aaaaRR(host string, ip net.IP, ttl uint32) *dns.AAAA {
	return &dns.AAAA{Hdr: dns.RR_Header{Name: host, Rrtype: dns.TypeAAAA, Class: dns.ClassINET, Ttl: ttl}, AAAA: ip}
}

func ip4(a, b, c, d byte) net.IP {
	return net.IPv4(a, b, c, d).To4()
}

func findEntry(entries []*ServiceEntry, instance string) *ServiceEntry {
	for _, e := range entries {
		if e.Instance == instance {
			return e
		}
	}
	return nil
}

func hasIPv4(e *ServiceEntry, ip net.IP) bool {
	for _, candidate := range e.AddrIPv4 {
		if candidate.Equal(ip) {
			return true
		}
	}
	return false
}

// sentinelMsg is a complete service response used as the final packet. Its
// distinct instance/host means a missing sentinel cannot be confused with the
// split service under test.
func sentinelMsg(instance string) *dns.Msg {
	name := instanceFQDN(instance, contractService, contractDomain)
	return &dns.Msg{MsgHdr: dns.MsgHdr{Response: true}, Answer: []dns.RR{
		ptrRR(serviceFQDN(contractService, contractDomain), name, 120),
		srvRR(name, "sentinel.local.", 65535, 120),
		txtRR(name, []string{"sentinel=1"}, 120),
		aRR("sentinel.local.", ip4(192, 0, 2, 250), 120),
	}}
}

func sentinelEntry(e *ServiceEntry) bool { return e.Instance == "sentinel" }

func TestDiscoveryContract(t *testing.T) {
	t.Run("split responses across packets and sections", func(t *testing.T) {
		h := newDiscoveryHarness(t, "", contractService, contractDomain, true, nil)
		name := instanceFQDN("sensor", contractService, contractDomain)
		svc := serviceFQDN(contractService, contractDomain)

		// PTR in Answer.
		h.send(&dns.Msg{MsgHdr: dns.MsgHdr{Response: true}, Answer: []dns.RR{
			ptrRR(svc, name, 120),
		}})
		// SRV in Ns.
		h.send(&dns.Msg{MsgHdr: dns.MsgHdr{Response: true}, Ns: []dns.RR{
			srvRR(name, "hub.local.", 8123, 120),
		}})
		// TXT in Extra (empty-array semantics are covered separately).
		h.send(&dns.Msg{MsgHdr: dns.MsgHdr{Response: true}, Extra: []dns.RR{
			txtRR(name, []string{"unit=kW"}, 120),
		}})
		// A in Answer.
		h.sendRR(aRR("hub.local.", ip4(192, 0, 2, 10), 120))
		h.send(sentinelMsg("sentinel"))

		got := h.collectUntil(sentinelEntry, "split")
		if len(got) != 1 {
			t.Fatalf("expected one service entry, got %+v", got)
		}
		e := got[0]
		if e.Instance != "sensor" || e.HostName != "hub.local." || e.Port != 8123 {
			t.Fatalf("unexpected sensor entry: %+v", e)
		}
		if len(e.Text) != 1 || e.Text[0] != "unit=kW" {
			t.Fatalf("unexpected TXT: %#v", e.Text)
		}
		if !hasIPv4(e, ip4(192, 0, 2, 10)) {
			t.Fatalf("missing address: %+v", e.AddrIPv4)
		}

		qs := h.expectQueries(2, "split")
		if !queryHas(qs, name, dns.TypeSRV) || !queryHas(qs, name, dns.TypeTXT) {
			t.Fatalf("missing instance SRV/TXT follow-up: %v", qs)
		}
		if !queryHas(qs, "hub.local.", dns.TypeA) || !queryHas(qs, "hub.local.", dns.TypeAAAA) {
			t.Fatalf("missing host A/AAAA follow-up: %v", qs)
		}
	})

	t.Run("two instances share one host", func(t *testing.T) {
		h := newDiscoveryHarness(t, "", contractService, contractDomain, true, nil)
		svc := serviceFQDN(contractService, contractDomain)
		alpha := instanceFQDN("alpha", contractService, contractDomain)
		beta := instanceFQDN("beta", contractService, contractDomain)
		hub := "hub.local."

		h.sendRR(ptrRR(svc, alpha, 120))
		h.sendRR(ptrRR(svc, beta, 120))
		h.sendRR(srvRR(alpha, hub, 8200, 120))
		h.sendRR(srvRR(beta, hub, 8300, 120))
		h.sendRR(txtRR(alpha, []string{"id=alpha"}, 120))
		h.sendRR(txtRR(beta, []string{"id=beta"}, 120))
		h.sendRR(aRR(hub, ip4(192, 0, 2, 20), 120))
		h.send(sentinelMsg("sentinel"))

		got := h.collectUntil(sentinelEntry, "shared host")
		if len(got) != 2 {
			t.Fatalf("expected two entries, got %+v", got)
		}
		a := findEntry(got, "alpha")
		b := findEntry(got, "beta")
		if a == nil || b == nil {
			t.Fatalf("missing shared-host entries: %+v", got)
		}
		if a.Port != 8200 || b.Port != 8300 {
			t.Fatalf("unexpected ports: %d %d", a.Port, b.Port)
		}
		if !hasIPv4(a, ip4(192, 0, 2, 20)) || !hasIPv4(b, ip4(192, 0, 2, 20)) {
			t.Fatalf("shared host address missing: %+v / %+v", a.AddrIPv4, b.AddrIPv4)
		}

		// One resolution query for each instance, one shared query for the host.
		qs := h.expectQueries(3, "shared host")
		if queryCount(qs, alpha, dns.TypeSRV) != 1 || queryCount(qs, beta, dns.TypeSRV) != 1 {
			t.Fatalf("expected one instance query each: %v", qs)
		}
		if queryCount(qs, hub, dns.TypeA)+queryCount(qs, hub, dns.TypeAAAA) != 2 {
			t.Fatalf("expected one shared host query (A+AAAA): %v", qs)
		}
	})

	t.Run("SRV before PTR still resolves", func(t *testing.T) {
		h := newDiscoveryHarness(t, "", contractService, contractDomain, true, nil)
		svc := serviceFQDN(contractService, contractDomain)
		name := instanceFQDN("sensor", contractService, contractDomain)

		// Records arrive before membership is established.
		h.sendRR(srvRR(name, "hub.local.", 8123, 120))
		h.sendRR(txtRR(name, []string{"unit=kW"}, 120))
		h.sendRR(ptrRR(svc, name, 120))
		h.sendRR(aRR("hub.local.", ip4(192, 0, 2, 11), 120))
		h.send(sentinelMsg("sentinel"))

		got := h.collectUntil(sentinelEntry, "srv before ptr")
		e := findEntry(got, "sensor")
		if e == nil || e.Port != 8123 || !hasIPv4(e, ip4(192, 0, 2, 11)) {
			t.Fatalf("sensor not assembled when SRV preceded PTR: %+v", got)
		}

		// Membership was learned last, so the host address query is issued once
		// the PTR arrives; no duplicate instance query is needed.
		qs := h.expectQueries(1, "srv before ptr")
		if !queryHas(qs, "hub.local.", dns.TypeA) || !queryHas(qs, "hub.local.", dns.TypeAAAA) {
			t.Fatalf("missing host follow-up: %v", qs)
		}
	})

	t.Run("target change uses only the new host", func(t *testing.T) {
		h := newDiscoveryHarness(t, "", contractService, contractDomain, true, nil)
		svc := serviceFQDN(contractService, contractDomain)
		name := instanceFQDN("sensor", contractService, contractDomain)

		h.sendRR(ptrRR(svc, name, 120))
		h.sendRR(srvRR(name, "oldhub.local.", 8000, 120))
		h.sendRR(aRR("oldhub.local.", ip4(192, 0, 2, 30), 120))
		h.sendRR(srvRR(name, "newhub.local.", 9000, 120))
		h.sendRR(aRR("newhub.local.", ip4(192, 0, 2, 31), 120))
		// TXT arrives last, after the SRV target changed but before publication.
		h.sendRR(txtRR(name, []string{"unit=kW"}, 120))
		h.send(sentinelMsg("sentinel"))

		got := h.collectUntil(sentinelEntry, "target change")
		if len(got) != 1 {
			t.Fatalf("expected one entry, got %+v", got)
		}
		e := got[0]
		if e.HostName != "newhub.local." || e.Port != 9000 {
			t.Fatalf("new SRV not used: %+v", e)
		}
		if hasIPv4(e, ip4(192, 0, 2, 30)) || !hasIPv4(e, ip4(192, 0, 2, 31)) {
			t.Fatalf("old host address leaked into result: %+v", e.AddrIPv4)
		}

		qs := h.expectQueries(3, "target change")
		if !queryHas(qs, "oldhub.local.", dns.TypeA) || !queryHas(qs, "newhub.local.", dns.TypeA) {
			t.Fatalf("missing per-target host queries: %v", qs)
		}
	})

	t.Run("target change does not reuse old host address", func(t *testing.T) {
		h := newDiscoveryHarness(t, "", contractService, contractDomain, true, nil)
		svc := serviceFQDN(contractService, contractDomain)
		name := instanceFQDN("sensor", contractService, contractDomain)

		h.sendRR(ptrRR(svc, name, 120))
		h.sendRR(srvRR(name, "oldhub.local.", 8000, 120))
		h.sendRR(aRR("oldhub.local.", ip4(192, 0, 2, 30), 120))
		h.sendRR(srvRR(name, "newhub.local.", 9000, 120))
		h.sendRR(txtRR(name, []string{"unit=kW"}, 120))
		h.send(sentinelMsg("sentinel"))

		got := h.collectUntil(sentinelEntry, "stale target")
		if findEntry(got, "sensor") != nil {
			t.Fatalf("sensor published using stale host address: %+v", got)
		}
		h.expectQueries(3, "stale target")
	})

	t.Run("one address family is enough", func(t *testing.T) {
		h := newDiscoveryHarness(t, "", contractService, contractDomain, true, nil)
		svc := serviceFQDN(contractService, contractDomain)
		name := instanceFQDN("sensor", contractService, contractDomain)
		ip6 := net.ParseIP("2001:db8::8")

		h.sendRR(ptrRR(svc, name, 120))
		h.sendRR(srvRR(name, "hub.local.", 8123, 120))
		h.sendRR(txtRR(name, []string{"unit=kW"}, 120))
		h.sendRR(aaaaRR("hub.local.", ip6, 120))
		h.send(sentinelMsg("sentinel"))

		got := h.collectUntil(sentinelEntry, "ipv6")
		e := findEntry(got, "sensor")
		if e == nil {
			t.Fatalf("AAAA-only response did not complete the service: %+v", got)
		}
		if len(e.AddrIPv4) != 0 || len(e.AddrIPv6) != 1 || !e.AddrIPv6[0].Equal(ip6) {
			t.Fatalf("unexpected address families: v4=%+v v6=%+v", e.AddrIPv4, e.AddrIPv6)
		}
		h.expectQueries(2, "ipv6")
	})

	t.Run("empty TXT is received while missing TXT is not", func(t *testing.T) {
		t.Run("missing TXT", func(t *testing.T) {
			h := newDiscoveryHarness(t, "", contractService, contractDomain, true, nil)
			svc := serviceFQDN(contractService, contractDomain)
			name := instanceFQDN("sensor", contractService, contractDomain)

			h.sendRR(ptrRR(svc, name, 120))
			h.sendRR(srvRR(name, "hub.local.", 8123, 120))
			h.sendRR(aRR("hub.local.", ip4(192, 0, 2, 12), 120))
			h.send(sentinelMsg("sentinel"))

			got := h.collectUntil(sentinelEntry, "missing txt")
			if findEntry(got, "sensor") != nil {
				t.Fatalf("sensor published without TXT: %+v", got)
			}
			h.expectQueries(2, "missing txt")
		})

		t.Run("empty TXT", func(t *testing.T) {
			h := newDiscoveryHarness(t, "", contractService, contractDomain, true, nil)
			svc := serviceFQDN(contractService, contractDomain)
			name := instanceFQDN("sensor", contractService, contractDomain)

			h.sendRR(ptrRR(svc, name, 120))
			h.sendRR(srvRR(name, "hub.local.", 8123, 120))
			h.sendRR(txtRR(name, []string{}, 120))
			h.sendRR(aRR("hub.local.", ip4(192, 0, 2, 12), 120))
			h.send(sentinelMsg("sentinel"))

			got := h.collectUntil(sentinelEntry, "empty txt")
			e := findEntry(got, "sensor")
			if e == nil {
				t.Fatalf("empty TXT did not complete the service: %+v", got)
			}
			if e.Text == nil || len(e.Text) != 0 {
				t.Fatalf("empty TXT should be delivered as an empty slice, got %#v", e.Text)
			}
			h.expectQueries(2, "empty txt")
		})
	})

	t.Run("service type enumeration returns PTR only", func(t *testing.T) {
		h := newDiscoveryHarness(t, "", "_services._dns-sd._udp", contractDomain, true, nil)
		meta := serviceFQDN("_services._dns-sd._udp", contractDomain)

		h.sendRR(ptrRR(meta, "_meter._tcp.local.", 120))
		h.sendRR(ptrRR(meta, "_camera._tcp.local.", 120))
		h.sendRR(ptrRR(meta, "_sentinel._tcp.local.", 120))

		got := h.collectUntil(func(e *ServiceEntry) bool {
			return e.Instance == "_sentinel._tcp.local"
		}, "enumeration")
		if len(got) != 2 {
			t.Fatalf("expected two PTR-only entries, got %+v", got)
		}
		if e := findEntry(got, "_meter._tcp.local"); e == nil {
			t.Fatalf("missing _meter._tcp PTR entry: %+v", got)
		}
		if e := findEntry(got, "_camera._tcp.local"); e == nil {
			t.Fatalf("missing _camera._tcp PTR entry: %+v", got)
		}
		// Enumeration needs no SRV/TXT/A completeness and sends no follow-ups.
		h.expectQueries(0, "enumeration")
	})

	t.Run("names are case insensitive with label boundaries", func(t *testing.T) {
		h := newDiscoveryHarness(t, "", contractService, contractDomain, true, nil)
		name := instanceFQDN("sensor", contractService, contractDomain)

		// Suffix-similar names must not enter the _meter._tcp.local. membership.
		h.sendRR(ptrRR(serviceFQDN("other_meter._tcp", contractDomain), "ghost.other_meter._tcp.local.", 120))
		h.sendRR(srvRR("ghost.other_meter._tcp.local.", "ghost.local.", 1, 120))

		h.sendRR(ptrRR(serviceFQDN("_METER._TCP", "LOCAL"), "SENSOR._METER._TCP.LOCAL.", 120))
		h.sendRR(srvRR(name, "HUB.LOCAL.", 8123, 120))
		h.sendRR(txtRR("SENSOR._Meter._Tcp.Local.", []string{"unit=kW"}, 120))
		h.sendRR(aRR("hub.local.", ip4(192, 0, 2, 13), 120))
		h.send(sentinelMsg("sentinel"))

		got := h.collectUntil(sentinelEntry, "case")
		if findEntry(got, "ghost.other_meter._tcp.local.") != nil {
			t.Fatalf("suffix-similar instance was accepted: %+v", got)
		}
		if len(got) != 1 {
			t.Fatalf("expected one entry, got %+v", got)
		}
		e := got[0]
		if e.Instance != "SENSOR" || e.HostName != "HUB.LOCAL." || !hasIPv4(e, ip4(192, 0, 2, 13)) {
			t.Fatalf("case-insensitive assembly failed: %+v", e)
		}

		qs := h.expectQueries(2, "case")
		if !queryHas(qs, "SENSOR._METER._TCP.LOCAL.", dns.TypeSRV) {
			t.Fatalf("missing case-insensitive instance follow-up: %v", qs)
		}
	})

	t.Run("PTR revocation preserves another instance's shared host", func(t *testing.T) {
		h := newDiscoveryHarness(t, "", contractService, contractDomain, true, nil)
		svc := serviceFQDN(contractService, contractDomain)
		alpha := instanceFQDN("alpha", contractService, contractDomain)
		beta := instanceFQDN("beta", contractService, contractDomain)
		hub := "hub.local."

		h.sendRR(ptrRR(svc, alpha, 120))
		h.sendRR(ptrRR(svc, beta, 120))
		h.sendRR(srvRR(alpha, hub, 8200, 120))
		h.sendRR(srvRR(beta, hub, 8300, 120))
		h.sendRR(aRR(hub, ip4(192, 0, 2, 21), 120))
		// Revoke alpha before it is complete. The shared address must remain.
		h.sendRR(ptrRR(svc, alpha, 0))
		h.sendRR(txtRR(beta, []string{"id=beta"}, 120))
		h.send(sentinelMsg("sentinel"))

		got := h.collectUntil(sentinelEntry, "revocation")
		if findEntry(got, "alpha") != nil {
			t.Fatalf("revoked instance was published: %+v", got)
		}
		b := findEntry(got, "beta")
		if b == nil || b.Port != 8300 || !hasIPv4(b, ip4(192, 0, 2, 21)) {
			t.Fatalf("beta lost the shared host address after alpha revocation: %+v", got)
		}

		qs := h.expectQueries(3, "revocation")
		if queryCount(qs, alpha, dns.TypeSRV) != 1 || queryCount(qs, beta, dns.TypeSRV) != 1 ||
			queryCount(qs, hub, dns.TypeA) != 1 {
			t.Fatalf("unexpected query set: %v", qs)
		}
	})

	t.Run("repeated responses deliver and query once", func(t *testing.T) {
		h := newDiscoveryHarness(t, "", contractService, contractDomain, true, nil)
		svc := serviceFQDN(contractService, contractDomain)
		name := instanceFQDN("sensor", contractService, contractDomain)

		for i := 0; i < 2; i++ {
			h.sendRR(ptrRR(svc, name, 120))
			h.sendRR(srvRR(name, "hub.local.", 8123, 120))
			h.sendRR(txtRR(name, []string{"unit=kW"}, 120))
			h.sendRR(aRR("hub.local.", ip4(192, 0, 2, 14), 120))
		}
		h.send(sentinelMsg("sentinel"))

		got := h.collectUntil(sentinelEntry, "repeated")
		if len(got) != 1 {
			t.Fatalf("expected at most one delivery, got %+v", got)
		}
		if len(got[0].AddrIPv4) != 1 {
			t.Fatalf("duplicate address records were accumulated: %+v", got[0].AddrIPv4)
		}

		// One instance query and one shared host query despite two responses.
		qs := h.expectQueries(2, "repeated")
		if queryCount(qs, name, dns.TypeSRV) != 1 || queryCount(qs, "hub.local.", dns.TypeA) != 1 {
			t.Fatalf("gaps were queried more than once: %v", qs)
		}
	})

	t.Run("lookup does not require PTR", func(t *testing.T) {
		h := newDiscoveryHarness(t, "sensor", contractService, contractDomain, false, nil)
		name := instanceFQDN("sensor", contractService, contractDomain)

		h.sendRR(srvRR(name, "hub.local.", 8123, 120))
		h.sendRR(aRR("hub.local.", ip4(192, 0, 2, 15), 120))
		// TXT is the final gap; seeing the entry proves the earlier packets
		// were consumed even though no PTR was supplied. It is also the
		// sentinel, so it is returned as the result rather than discarded.
		h.sendRR(txtRR(name, []string{"unit=kW"}, 120))

		var result *ServiceEntry
		deadline := time.After(3 * time.Second)
		for result == nil {
			select {
			case e, ok := <-h.entries:
				if !ok {
					t.Fatal("entries closed before lookup result")
				}
				if e.Instance == "sensor" {
					result = e
				} else {
					t.Fatalf("unexpected lookup entry: %+v", e)
				}
			case <-deadline:
				t.Fatal("lookup result not received")
			}
		}
		if result.Port != 8123 || !hasIPv4(result, ip4(192, 0, 2, 15)) {
			t.Fatalf("unexpected lookup result: %+v", result)
		}

		qs := h.expectQueries(1, "lookup")
		if queryCount(qs, "hub.local.", dns.TypeA) != 1 || queryHas(qs, name, dns.TypeSRV) {
			t.Fatalf("lookup follow-up should only query the host: %v", qs)
		}
	})

	t.Run("published values do not alias pending state", func(t *testing.T) {
		h := newDiscoveryHarness(t, "", contractService, contractDomain, true, nil)
		svc := serviceFQDN(contractService, contractDomain)
		alpha := instanceFQDN("alpha", contractService, contractDomain)
		beta := instanceFQDN("beta", contractService, contractDomain)
		hub := "hub.local."

		h.sendRR(ptrRR(svc, alpha, 120))
		h.sendRR(ptrRR(svc, beta, 120))
		h.sendRR(srvRR(alpha, hub, 8200, 120))
		h.sendRR(srvRR(beta, hub, 8300, 120))
		h.sendRR(txtRR(alpha, []string{"id=shared"}, 120))
		h.sendRR(txtRR(beta, []string{"id=shared"}, 120))
		h.sendRR(aRR(hub, ip4(192, 0, 2, 16), 120))
		h.send(sentinelMsg("sentinel"))

		got := h.collectUntil(sentinelEntry, "copy isolation")
		a := findEntry(got, "alpha")
		b := findEntry(got, "beta")
		if a == nil || b == nil {
			t.Fatalf("missing entries: %+v", got)
		}

		a.Text[0] = "mutated"
		a.AddrIPv4[0][0] = 203
		if b.Text[0] != "id=shared" {
			t.Fatalf("TXT slice was shared between entries: %#v", b.Text)
		}
		if !hasIPv4(b, ip4(192, 0, 2, 16)) {
			t.Fatalf("IP bytes were shared between entries: %+v", b.AddrIPv4)
		}

		h.expectQueries(3, "copy isolation")
	})

	t.Run("follow-up egress cannot block result delivery", func(t *testing.T) {
		release := make(chan struct{})
		h := newDiscoveryHarness(t, "", contractService, contractDomain, true, func(*dns.Msg) error {
			<-release
			return nil
		})
		svc := serviceFQDN(contractService, contractDomain)
		name := instanceFQDN("sensor", contractService, contractDomain)

		// This PTR triggers a follow-up query whose egress blocks.
		h.sendRR(ptrRR(svc, name, 120))
		// The final complete sentinel must still be read and delivered.
		h.send(sentinelMsg("sentinel"))

		got := h.collectUntil(sentinelEntry, "blocking egress")
		if len(got) != 0 {
			t.Fatalf("unexpected partial entries: %+v", got)
		}
		close(release)
	})
}
