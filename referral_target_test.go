// Tests for automatic referral chasing (LDAPConnection.ReferralPolicy).
// Fake directory peers are driven over net.Pipe and decode the real BER
// search requests sent by the client.
package ldap

import (
	"errors"
	"fmt"
	"net"
	"net/url"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/mavricknz/asn1-ber"
	"github.com/mavricknz/ldap/task_support/taskcheck"
)

// recordedSearchRequest is what a fake directory decoded from the wire.
type recordedSearchRequest struct {
	messageID  uint64
	baseDN     string
	scope      int
	filter     string
	attributes []string
}

type directoryRecorder struct {
	mu       sync.Mutex
	requests []recordedSearchRequest
}

func (d *directoryRecorder) add(req recordedSearchRequest) {
	d.mu.Lock()
	defer d.mu.Unlock()
	d.requests = append(d.requests, req)
}

func (d *directoryRecorder) count() int {
	d.mu.Lock()
	defer d.mu.Unlock()
	return len(d.requests)
}

func (d *directoryRecorder) get(i int) recordedSearchRequest {
	d.mu.Lock()
	defer d.mu.Unlock()
	return d.requests[i]
}

// testReply is one LDAP response message: an operation packet plus optional
// response controls.
type testReply struct {
	op       *ber.Packet
	controls *ber.Packet
}

func testReplies(ops ...*ber.Packet) []testReply {
	replies := make([]testReply, 0, len(ops))
	for _, op := range ops {
		replies = append(replies, testReply{op: op})
	}
	return replies
}

func testEntryOp(dn string) *ber.Packet {
	op := ber.Encode(ber.ClassApplication, ber.TypeConstructed, SearchResultEntry, nil, "Search Result Entry")
	op.AppendChild(ber.NewString(ber.ClassUniversal, ber.TypePrimative, ber.TagOctetString, dn, "DN"))
	op.AppendChild(ber.Encode(ber.ClassUniversal, ber.TypeConstructed, ber.TagSequence, nil, "Attributes"))
	return op
}

func testReferenceOp(urls ...string) *ber.Packet {
	op := ber.Encode(ber.ClassApplication, ber.TypeConstructed, SearchResultReference, nil, "Search Result Reference")
	for _, u := range urls {
		op.AppendChild(ber.NewString(ber.ClassUniversal, ber.TypePrimative, ber.TagOctetString, u, "URI"))
	}
	return op
}

func testDoneOp(code uint64) *ber.Packet {
	op := ber.Encode(ber.ClassApplication, ber.TypeConstructed, SearchResultDone, nil, "Search Result Done")
	op.AppendChild(ber.NewInteger(ber.ClassUniversal, ber.TypePrimative, ber.TagEnumerated, code, "result code"))
	op.AppendChild(ber.NewString(ber.ClassUniversal, ber.TypePrimative, ber.TagOctetString, "", "matched DN"))
	op.AppendChild(ber.NewString(ber.ClassUniversal, ber.TypePrimative, ber.TagOctetString, "", "diagnostic"))
	return op
}

func decodeTestSearchRequest(packet *ber.Packet) (recordedSearchRequest, error) {
	var req recordedSearchRequest
	if len(packet.Children) < 2 || packet.Children[1].Tag != ApplicationSearchRequest {
		return req, fmt.Errorf("expected a SearchRequest, got tag %d", packet.Children[1].Tag)
	}
	messageID, ok := packet.Children[0].Value.(uint64)
	if !ok {
		return req, fmt.Errorf("missing message ID")
	}
	search := packet.Children[1]
	req.messageID = messageID
	req.baseDN = search.Children[0].ValueString()
	req.scope = int(search.Children[1].Value.(uint64))
	filter, err := DecompileFilter(search.Children[6])
	if err != nil {
		return req, err
	}
	req.filter = filter
	for _, attr := range search.Children[7].Children {
		req.attributes = append(req.attributes, attr.ValueString())
	}
	return req, nil
}

// runDirectoryServer serves search requests on conn until the peer goes away.
func runDirectoryServer(conn net.Conn, recorder *directoryRecorder, respond func(*recordedSearchRequest) []testReply) {
	defer conn.Close()
	for {
		packet, err := ber.ReadPacket(conn)
		if err != nil {
			return
		}
		req, err := decodeTestSearchRequest(packet)
		if err != nil {
			return
		}
		recorder.add(req)
		for _, reply := range respond(&req) {
			message := ber.Encode(ber.ClassUniversal, ber.TypeConstructed, ber.TagSequence, nil, "LDAP Response")
			message.AppendChild(ber.NewInteger(ber.ClassUniversal, ber.TypePrimative, ber.TagInteger, req.messageID, "Message ID"))
			message.AppendChild(reply.op)
			if reply.controls != nil {
				message.AppendChild(reply.controls)
			}
			if _, err := conn.Write(message.Bytes()); err != nil {
				return
			}
		}
	}
}

// closeCountingConn records how many times Close was called.
type closeCountingConn struct {
	net.Conn
	mu     sync.Mutex
	closes int
}

func (c *closeCountingConn) Close() error {
	c.mu.Lock()
	c.closes++
	c.mu.Unlock()
	return c.Conn.Close()
}

func (c *closeCountingConn) closeCount() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.closes
}

func waitForClose(c *closeCountingConn) bool {
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if c.closeCount() > 0 {
			return true
		}
		time.Sleep(5 * time.Millisecond)
	}
	return false
}

// referralRig stands up fake directories reachable through a ReferralPolicy.
type referralRig struct {
	mu          sync.Mutex
	directories map[string]*testDirectory
	dialErrs    map[string]error
	dialed      []*closeCountingConn
	dialedConns map[string][]*LDAPConnection
}

func newReferralRig() *referralRig {
	return &referralRig{
		directories: map[string]*testDirectory{},
		dialErrs:    map[string]error{},
		dialedConns: map[string][]*LDAPConnection{},
	}
}

func (rig *referralRig) policy() *ReferralPolicy {
	return &ReferralPolicy{Dial: rig.dial}
}

// registerDirectory installs a fake directory for host (lowercase) and
// returns its request recorder.
func (rig *referralRig) registerDirectory(host string, respond func(*recordedSearchRequest) []testReply) *directoryRecorder {
	recorder := &directoryRecorder{}
	rig.mu.Lock()
	rig.directories[host] = &testDirectory{recorder: recorder, respond: respond}
	rig.mu.Unlock()
	return recorder
}

func (rig *referralRig) dial(u *url.URL) (*LDAPConnection, error) {
	host := strings.ToLower(u.Hostname())
	rig.mu.Lock()
	dialErr := rig.dialErrs[host]
	dir := rig.directories[host]
	rig.mu.Unlock()
	if dialErr != nil {
		return nil, dialErr
	}
	if dir == nil {
		return nil, fmt.Errorf("no test directory registered for host %q", host)
	}
	client, server := net.Pipe()
	counted := &closeCountingConn{Conn: client}
	go runDirectoryServer(server, dir.recorder, dir.respond)
	port := uint16(389)
	if u.Scheme == "ldaps" {
		port = 636
	}
	conn := NewLDAPConnection(host, port)
	conn.Dialer = Dialer(func(_, _ string) (net.Conn, error) { return counted, nil })
	if err := conn.Connect(); err != nil {
		return nil, err
	}
	rig.mu.Lock()
	rig.dialed = append(rig.dialed, counted)
	rig.dialedConns[host] = append(rig.dialedConns[host], conn)
	rig.mu.Unlock()
	return conn, nil
}

func (rig *referralRig) dialedCount() int {
	rig.mu.Lock()
	defer rig.mu.Unlock()
	return len(rig.dialed)
}

func (rig *referralRig) lastDialedConn(host string) *LDAPConnection {
	rig.mu.Lock()
	defer rig.mu.Unlock()
	conns := rig.dialedConns[host]
	if len(conns) == 0 {
		return nil
	}
	return conns[len(conns)-1]
}

type testDirectory struct {
	recorder *directoryRecorder
	respond  func(*recordedSearchRequest) []testReply
}

// newTestRoot builds the borrowed root connection on a net.Pipe.
func newTestRoot(t *testing.T, respond func(*recordedSearchRequest) []testReply) (*LDAPConnection, *directoryRecorder, *closeCountingConn) {
	t.Helper()
	recorder := &directoryRecorder{}
	client, server := net.Pipe()
	counted := &closeCountingConn{Conn: client}
	go runDirectoryServer(server, recorder, respond)
	conn := NewLDAPConnection("root.example", 389)
	conn.Dialer = Dialer(func(_, _ string) (net.Conn, error) { return counted, nil })
	if err := conn.Connect(); err != nil {
		t.Fatalf("root connect: %v", err)
	}
	return conn, recorder, counted
}

func entryDNs(result *SearchResult) []string {
	dns := make([]string, 0, len(result.Entries))
	for _, entry := range result.Entries {
		dns = append(dns, entry.DN)
	}
	return dns
}

func equalStrings(a, b []string) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

// deliveryRecord captures one result seen by a recordingHandler.
type deliveryRecord struct {
	resultType uint8
	dn         string
	conn       *LDAPConnection
	messageID  uint64
	controls   []Control
}

type recordingHandler struct {
	records []deliveryRecord
}

func (h *recordingHandler) ProcessDiscreteResult(dsr *DiscreteSearchResult, info *ConnectionInfo) (bool, error) {
	record := deliveryRecord{resultType: dsr.SearchResultType, controls: dsr.Controls}
	if dsr.Entry != nil {
		record.dn = dsr.Entry.DN
	}
	if info != nil {
		record.conn = info.Conn
		record.messageID = info.MessageID
	}
	h.records = append(h.records, record)
	return false, nil
}

func (h *recordingHandler) dns() []string {
	var dns []string
	for _, record := range h.records {
		if record.resultType == SearchResultEntry {
			dns = append(dns, record.dn)
		}
	}
	return dns
}

func (h *recordingHandler) countOf(resultType uint8) int {
	n := 0
	for _, record := range h.records {
		if record.resultType == resultType {
			n++
		}
	}
	return n
}

func TestReferralAlternatives(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Scenario 1: the first candidate returns entries and then an
		// LDAP error, the second candidate's own successor has no usable
		// address; the third candidate must be selected and only its
		// entries may be delivered.
		rig := newReferralRig()
		rig.dialErrs["missing.example"] = fmt.Errorf("cannot connect")
		alt1 := rig.registerDirectory("alt1.example", func(req *recordedSearchRequest) []testReply {
			return testReplies(
				testEntryOp("cn=a1,dc=people,dc=example"),
				testDoneOp(LDAPResultUnavailable),
			)
		})
		alt2 := rig.registerDirectory("alt2.example", func(req *recordedSearchRequest) []testReply {
			return testReplies(
				testEntryOp("cn=a2,dc=people,dc=example"),
				testReferenceOp("ldap://missing.example/dc=people,dc=example??sub?(uid=*)"),
				testDoneOp(LDAPResultSuccess),
			)
		})
		alt3 := rig.registerDirectory("alt3.example", func(req *recordedSearchRequest) []testReply {
			return testReplies(
				testEntryOp("cn=a3,dc=people,dc=example"),
				testDoneOp(LDAPResultSuccess),
			)
		})

		root, rootRec, _ := newTestRoot(t, func(req *recordedSearchRequest) []testReply {
			return testReplies(
				testEntryOp("cn=root,dc=example"),
				testReferenceOp(
					"ldap://alt1.example/dc=people,dc=example??sub?(uid=*)",
					"ldap://alt2.example/dc=people,dc=example??sub?(uid=*)",
					"ldap://alt3.example/dc=people,dc=example??sub?(uid=*)",
				),
				testDoneOp(LDAPResultSuccess),
			)
		})
		root.ReferralPolicy = rig.policy()

		result, err := root.Search(NewSimpleSearchRequest("dc=example", ScopeWholeSubtree, "(objectClass=*)", []string{"cn"}))
		c.Check(err == nil, "scenario 1: search failed: %v", err)
		c.Check(equalStrings(entryDNs(result), []string{"cn=root,dc=example", "cn=a3,dc=people,dc=example"}),
			"scenario 1: wrong entries delivered: %v", entryDNs(result))
		c.Check(len(result.Referrals) == 0, "scenario 1: referrals not empty: %v", result.Referrals)
		c.Check(alt1.count() == 1, "scenario 1: alt1 requests = %d, want 1", alt1.count())
		c.Check(alt2.count() == 1, "scenario 1: alt2 requests = %d, want 1", alt2.count())
		c.Check(alt3.count() == 1, "scenario 1: alt3 requests = %d, want 1", alt3.count())
		c.Check(rootRec.count() == 1, "scenario 1: root requests = %d, want 1", rootRec.count())
		root.Close()

		// Scenario 2: every candidate fails, the whole call errors and
		// Search returns no partial entries.
		rig2 := newReferralRig()
		bad1 := rig2.registerDirectory("bad1.example", func(req *recordedSearchRequest) []testReply {
			return testReplies(testDoneOp(LDAPResultNoSuchObject))
		})
		bad2 := rig2.registerDirectory("bad2.example", func(req *recordedSearchRequest) []testReply {
			return testReplies(
				testEntryOp("cn=bad2,dc=people,dc=example"),
				testDoneOp(LDAPResultOperationsError),
			)
		})

		root2, _, _ := newTestRoot(t, func(req *recordedSearchRequest) []testReply {
			return testReplies(
				testEntryOp("cn=root,dc=example"),
				testReferenceOp(
					"ldap://bad1.example/dc=people,dc=example??sub?(uid=*)",
					"ldap://bad2.example/dc=people,dc=example??sub?(uid=*)",
				),
				testDoneOp(LDAPResultSuccess),
			)
		})
		root2.ReferralPolicy = rig2.policy()

		result2, err2 := root2.Search(NewSimpleSearchRequest("dc=example", ScopeWholeSubtree, "(objectClass=*)", []string{"cn"}))
		c.Check(err2 != nil, "scenario 2: expected an error when all candidates fail")
		c.Check(len(result2.Entries) == 0, "scenario 2: partial entries leaked: %v", entryDNs(result2))
		c.Check(bad1.count() == 1, "scenario 2: bad1 requests = %d, want 1", bad1.count())
		c.Check(bad2.count() == 1, "scenario 2: bad2 requests = %d, want 1", bad2.count())
		root2.Close()

		// Scenario 3: invalid addresses (scheme, userinfo, fragment,
		// extensions, missing DN/host) are unusable and skipped inside
		// their group.
		rig3 := newReferralRig()
		ok := rig3.registerDirectory("ok.example", func(req *recordedSearchRequest) []testReply {
			return testReplies(
				testEntryOp("cn=ok,dc=people,dc=example"),
				testDoneOp(LDAPResultSuccess),
			)
		})

		root3, _, _ := newTestRoot(t, func(req *recordedSearchRequest) []testReply {
			return testReplies(
				testEntryOp("cn=root,dc=example"),
				testReferenceOp(
					"http://notldap.example/dc=people,dc=example",
					"ldap://user@ok.example/dc=people,dc=example",
					"ldap://ok.example/dc=people,dc=example#fragment",
					"ldap://ok.example/dc=people,dc=example?cn?sub?(uid=*)?x-ext",
					"ldap://ok.example",
					"ldap:///dc=people,dc=example",
					"ldap://ok.example/dc=people,dc=example??sub?(uid=*)",
				),
				testDoneOp(LDAPResultSuccess),
			)
		})
		root3.ReferralPolicy = rig3.policy()

		result3, err3 := root3.Search(NewSimpleSearchRequest("dc=example", ScopeWholeSubtree, "(objectClass=*)", []string{"cn"}))
		c.Check(err3 == nil, "scenario 3: search failed: %v", err3)
		c.Check(equalStrings(entryDNs(result3), []string{"cn=root,dc=example", "cn=ok,dc=people,dc=example"}),
			"scenario 3: wrong entries delivered: %v", entryDNs(result3))
		c.Check(ok.count() == 1, "scenario 3: ok requests = %d, want 1", ok.count())
		root3.Close()

		// Scenario 4: a SearchRequest with Controls errors out before any
		// networking happens.
		rig4 := newReferralRig()
		root4, root4Rec, _ := newTestRoot(t, func(req *recordedSearchRequest) []testReply {
			return testReplies(testDoneOp(LDAPResultSuccess))
		})
		root4.ReferralPolicy = rig4.policy()
		reqWithControls := NewSimpleSearchRequest("dc=example", ScopeWholeSubtree, "(objectClass=*)", []string{"cn"})
		reqWithControls.AddControl(NewControlPaging(10))
		_, err4 := root4.Search(reqWithControls)
		c.Check(err4 != nil, "scenario 4: expected an error for a request with controls")
		c.Check(root4Rec.count() == 0, "scenario 4: root received %d requests, want 0", root4Rec.count())
		c.Check(rig4.dialedCount() == 0, "scenario 4: %d referral connections dialed, want 0", rig4.dialedCount())
		root4.Close()
	})
}

func TestReferralQueryGraph(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Scenario 1: two references naming the same query (hostname case
		// and default port differ) execute once and merge once.
		rig := newReferralRig()
		shared := rig.registerDirectory("shared.example", func(req *recordedSearchRequest) []testReply {
			return testReplies(
				testEntryOp("cn=shared,dc=shared"),
				testDoneOp(LDAPResultSuccess),
			)
		})

		root, _, _ := newTestRoot(t, func(req *recordedSearchRequest) []testReply {
			return testReplies(
				testEntryOp("cn=root,dc=example"),
				testReferenceOp("ldap://shared.example/dc=shared??sub?(objectClass=*)"),
				testReferenceOp("ldap://SHARED.example:389/dc=shared??sub?(objectClass=*)"),
				testDoneOp(LDAPResultSuccess),
			)
		})
		root.ReferralPolicy = rig.policy()

		result, err := root.Search(NewSimpleSearchRequest("dc=example", ScopeWholeSubtree, "(objectClass=*)", []string{"cn"}))
		c.Check(err == nil, "scenario 1: search failed: %v", err)
		c.Check(equalStrings(entryDNs(result), []string{"cn=root,dc=example", "cn=shared,dc=shared"}),
			"scenario 1: wrong entries delivered: %v", entryDNs(result))
		c.Check(shared.count() == 1, "scenario 1: shared requests = %d, want 1", shared.count())
		root.Close()

		// Scenario 2: the same server with different DNs, and different
		// servers with the same DN, are all distinct queries.
		rig2 := newReferralRig()
		multi := rig2.registerDirectory("multi.example", func(req *recordedSearchRequest) []testReply {
			return testReplies(
				testEntryOp("cn=from-multi-"+req.baseDN),
				testDoneOp(LDAPResultSuccess),
			)
		})
		one := rig2.registerDirectory("one.example", func(req *recordedSearchRequest) []testReply {
			return testReplies(testEntryOp("cn=one,dc=a"), testDoneOp(LDAPResultSuccess))
		})
		two := rig2.registerDirectory("two.example", func(req *recordedSearchRequest) []testReply {
			return testReplies(testEntryOp("cn=two,dc=a"), testDoneOp(LDAPResultSuccess))
		})

		root2, _, _ := newTestRoot(t, func(req *recordedSearchRequest) []testReply {
			return testReplies(
				testEntryOp("cn=root,dc=example"),
				testReferenceOp("ldap://multi.example/dc=a??sub?(x=*)"),
				testReferenceOp("ldap://multi.example/dc=b??sub?(x=*)"),
				testReferenceOp("ldap://one.example/dc=a??sub?(x=*)"),
				testReferenceOp("ldap://two.example/dc=a??sub?(x=*)"),
				testDoneOp(LDAPResultSuccess),
			)
		})
		root2.ReferralPolicy = rig2.policy()

		result2, err2 := root2.Search(NewSimpleSearchRequest("dc=example", ScopeWholeSubtree, "(objectClass=*)", []string{"cn"}))
		c.Check(err2 == nil, "scenario 2: search failed: %v", err2)
		c.Check(equalStrings(entryDNs(result2), []string{
			"cn=root,dc=example",
			"cn=from-multi-dc=a",
			"cn=from-multi-dc=b",
			"cn=one,dc=a",
			"cn=two,dc=a",
		}), "scenario 2: wrong entries delivered: %v", entryDNs(result2))
		c.Check(multi.count() == 2, "scenario 2: multi requests = %d, want 2", multi.count())
		c.Check(one.count() == 1, "scenario 2: one requests = %d, want 1", one.count())
		c.Check(two.count() == 1, "scenario 2: two requests = %d, want 1", two.count())
		root2.Close()

		// Scenario 3: a reference group whose only candidate loops back to
		// the active root query fails the call with ErrReferralLoop.
		rig3 := newReferralRig()
		root3, _, _ := newTestRoot(t, func(req *recordedSearchRequest) []testReply {
			return testReplies(
				testEntryOp("cn=root,dc=loop"),
				testReferenceOp("ldap://ROOT.example/dc=loop??sub?(objectClass=*)"),
				testDoneOp(LDAPResultSuccess),
			)
		})
		root3.ReferralPolicy = rig3.policy()

		result3, err3 := root3.Search(NewSimpleSearchRequest("dc=loop", ScopeWholeSubtree, "(objectClass=*)", []string{"cn"}))
		c.Check(err3 == ErrReferralLoop, "scenario 3: error = %v, want ErrReferralLoop", err3)
		c.Check(len(result3.Entries) == 0, "scenario 3: partial entries leaked: %v", entryDNs(result3))
		c.Check(rig3.dialedCount() == 0, "scenario 3: %d connections dialed for a loop, want 0", rig3.dialedCount())
		root3.Close()

		// Scenario 4: a loop candidate is skipped and the next
		// alternative of the same group is used.
		rig4 := newReferralRig()
		good := rig4.registerDirectory("good.example", func(req *recordedSearchRequest) []testReply {
			return testReplies(testEntryOp("cn=good,dc=good"), testDoneOp(LDAPResultSuccess))
		})

		root4, _, _ := newTestRoot(t, func(req *recordedSearchRequest) []testReply {
			return testReplies(
				testEntryOp("cn=root,dc=rootloop"),
				testReferenceOp(
					"ldap://ROOT.example:389/dc=rootloop?cn?sub?(objectClass=*)",
					"ldap://good.example/dc=good??sub?(objectClass=*)",
				),
				testDoneOp(LDAPResultSuccess),
			)
		})
		root4.ReferralPolicy = rig4.policy()

		result4, err4 := root4.Search(NewSimpleSearchRequest("dc=rootloop", ScopeWholeSubtree, "(objectClass=*)", []string{"cn"}))
		c.Check(err4 == nil, "scenario 4: search failed: %v", err4)
		c.Check(equalStrings(entryDNs(result4), []string{"cn=root,dc=rootloop", "cn=good,dc=good"}),
			"scenario 4: wrong entries delivered: %v", entryDNs(result4))
		c.Check(good.count() == 1, "scenario 4: good requests = %d, want 1", good.count())
		root4.Close()

		// Scenario 5: twelve levels of nesting are followed depth-first.
		rig5 := newReferralRig()
		const depth = 12
		hosts := []string{}
		recorders := map[string]*directoryRecorder{}
		for i := 1; i <= depth; i++ {
			i := i
			host := fmt.Sprintf("s%d.example", i)
			hosts = append(hosts, host)
			recorders[host] = rig5.registerDirectory(host, func(req *recordedSearchRequest) []testReply {
				replies := testReplies(testEntryOp(fmt.Sprintf("cn=s%d,dc=n%d", i, i)))
				if i < depth {
					replies = append(replies, testReply{op: testReferenceOp(
						fmt.Sprintf("ldap://s%d.example/dc=n%d??sub?(objectClass=*)", i+1, i+1))})
				}
				return append(replies, testReply{op: testDoneOp(LDAPResultSuccess)})
			})
		}

		root5, _, _ := newTestRoot(t, func(req *recordedSearchRequest) []testReply {
			return testReplies(
				testEntryOp("cn=root,dc=example"),
				testReferenceOp("ldap://s1.example/dc=n1??sub?(objectClass=*)"),
				testDoneOp(LDAPResultSuccess),
			)
		})
		root5.ReferralPolicy = rig5.policy()

		wantDNs := []string{"cn=root,dc=example"}
		for i := 1; i <= depth; i++ {
			wantDNs = append(wantDNs, fmt.Sprintf("cn=s%d,dc=n%d", i, i))
		}
		result5, err5 := root5.Search(NewSimpleSearchRequest("dc=example", ScopeWholeSubtree, "(objectClass=*)", []string{"cn"}))
		c.Check(err5 == nil, "scenario 5: search failed: %v", err5)
		c.Check(equalStrings(entryDNs(result5), wantDNs),
			"scenario 5: wrong entries delivered: %v", entryDNs(result5))
		allServed := true
		for _, host := range hosts {
			if recorders[host].count() != 1 {
				allServed = false
			}
		}
		c.Check(allServed, "scenario 5: every nested directory should have been queried exactly once")
		root5.Close()
	})
}

func TestReferralDelivery(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Scenario 1: delivery order, per-result origin, inherited and
		// overridden URL query parts, preserved root controls.
		rig := newReferralRig()
		bRec := rig.registerDirectory("b.example", func(req *recordedSearchRequest) []testReply {
			return testReplies(
				testEntryOp("cn=b1,dc=branch,dc=example"),
				testReferenceOp("ldap://d.example/dc=leaf,dc=example??one"),
				testDoneOp(LDAPResultSuccess),
			)
		})
		dRec := rig.registerDirectory("d.example", func(req *recordedSearchRequest) []testReply {
			return testReplies(
				testEntryOp("cn=d1,dc=leaf,dc=example"),
				testDoneOp(LDAPResultSuccess),
			)
		})
		cRec := rig.registerDirectory("c.example", func(req *recordedSearchRequest) []testReply {
			return testReplies(
				testEntryOp("cn=c1,dc=other,dc=example"),
				testDoneOp(LDAPResultSuccess),
			)
		})

		pagingControls, ctlErr := encodeControls([]Control{NewControlPaging(100)})
		c.Check(ctlErr == nil, "scenario 1: could not encode controls: %v", ctlErr)
		root, rootRec, rootCounted := newTestRoot(t, func(req *recordedSearchRequest) []testReply {
			return []testReply{
				{op: testEntryOp("cn=r1,dc=example")},
				{op: testEntryOp("cn=r2,dc=example")},
				{op: testReferenceOp("ldap://b.example/dc=branch,dc=example")},
				{op: testReferenceOp("ldap://c.example/dc=other,dc=example?uid?base?(uid%3Da+b)")},
				{op: testDoneOp(LDAPResultSuccess), controls: pagingControls},
			}
		})
		root.ReferralPolicy = rig.policy()

		attrs := []string{"cn", "sn"}
		req := NewSimpleSearchRequest("dc=example", ScopeWholeSubtree, "(objectClass=*)", attrs)

		// Entry point 1: Search.
		result, err := root.Search(req)
		c.Check(err == nil, "scenario 1: search failed: %v", err)
		c.Check(equalStrings(entryDNs(result),
			[]string{"cn=r1,dc=example", "cn=r2,dc=example", "cn=b1,dc=branch,dc=example", "cn=d1,dc=leaf,dc=example", "cn=c1,dc=other,dc=example"}),
			"scenario 1: wrong Search entries: %v", entryDNs(result))
		c.Check(len(result.Referrals) == 0, "scenario 1: Search referrals not empty: %v", result.Referrals)
		_, paging := FindControl(result.Controls, ControlTypePaging)
		c.Check(paging != nil, "scenario 1: root response controls were not preserved")

		// Entry point 2: SearchWithHandler with a recording handler.
		handler := &recordingHandler{}
		err = root.SearchWithHandler(req, handler, nil)
		c.Check(err == nil, "scenario 1: SearchWithHandler failed: %v", err)
		c.Check(equalStrings(handler.dns(),
			[]string{"cn=r1,dc=example", "cn=r2,dc=example", "cn=b1,dc=branch,dc=example", "cn=d1,dc=leaf,dc=example", "cn=c1,dc=other,dc=example"}),
			"scenario 1: wrong handler delivery order: %v", handler.dns())
		c.Check(handler.countOf(SearchResultDone) == 1, "scenario 1: handler saw %d Done results, want 1", handler.countOf(SearchResultDone))
		c.Check(handler.countOf(SearchResultReference) == 0, "scenario 1: handler saw %d Reference results, want 0", handler.countOf(SearchResultReference))
		c.Check(len(handler.records) == 6, "scenario 1: handler got %d deliveries, want 6", len(handler.records))
		if len(handler.records) == 6 {
			last := handler.records[len(handler.records)-1]
			c.Check(last.resultType == SearchResultDone, "scenario 1: last delivery is not the root Done")
			c.Check(len(last.controls) > 0, "scenario 1: root Done controls missing in handler delivery")
			c.Check(last.conn == root, "scenario 1: root Done came from the wrong connection")
		}

		// Per-result origin: real Conn and MessageID of each branch.
		bConn := rig.lastDialedConn("b.example")
		dConn := rig.lastDialedConn("d.example")
		cConn := rig.lastDialedConn("c.example")
		c.Check(bConn != nil && dConn != nil && cConn != nil, "scenario 1: referral connections were not dialed")
		byDN := map[string]deliveryRecord{}
		for _, record := range handler.records {
			if record.resultType == SearchResultEntry {
				byDN[record.dn] = record
			}
		}
		c.Check(byDN["cn=r1,dc=example"].conn == root, "scenario 1: r1 came from the wrong connection")
		c.Check(byDN["cn=b1,dc=branch,dc=example"].conn == bConn, "scenario 1: b1 came from the wrong connection")
		c.Check(byDN["cn=d1,dc=leaf,dc=example"].conn == dConn, "scenario 1: d1 came from the wrong connection")
		c.Check(byDN["cn=c1,dc=other,dc=example"].conn == cConn, "scenario 1: c1 came from the wrong connection")
		c.Check(byDN["cn=r1,dc=example"].messageID == rootRec.get(1).messageID,
			"scenario 1: r1 message ID %d, want %d", byDN["cn=r1,dc=example"].messageID, rootRec.get(1).messageID)
		c.Check(byDN["cn=b1,dc=branch,dc=example"].messageID == bRec.get(1).messageID,
			"scenario 1: b1 message ID %d, want %d", byDN["cn=b1,dc=branch,dc=example"].messageID, bRec.get(1).messageID)
		c.Check(byDN["cn=d1,dc=leaf,dc=example"].messageID == dRec.get(1).messageID,
			"scenario 1: d1 message ID %d, want %d", byDN["cn=d1,dc=leaf,dc=example"].messageID, dRec.get(1).messageID)

		// URL query inheritance and overrides as decoded on the wire.
		bReq := bRec.get(1)
		c.Check(bReq.baseDN == "dc=branch,dc=example", "scenario 1: b base DN = %q", bReq.baseDN)
		c.Check(bReq.scope == ScopeWholeSubtree, "scenario 1: b scope = %d, want inherited sub", bReq.scope)
		c.Check(bReq.filter == "(objectClass=*)", "scenario 1: b filter = %q, want inherited", bReq.filter)
		c.Check(equalStrings(bReq.attributes, []string{"cn", "sn"}), "scenario 1: b attributes = %v, want inherited", bReq.attributes)
		dReq := dRec.get(1)
		c.Check(dReq.scope == ScopeSingleLevel, "scenario 1: d scope = %d, want explicit one", dReq.scope)
		c.Check(dReq.filter == "(objectClass=*)" && equalStrings(dReq.attributes, []string{"cn", "sn"}),
			"scenario 1: d should inherit filter and attributes, got %q %v", dReq.filter, dReq.attributes)
		cReq := cRec.get(1)
		c.Check(cReq.baseDN == "dc=other,dc=example", "scenario 1: c base DN = %q", cReq.baseDN)
		c.Check(cReq.scope == ScopeBaseObject, "scenario 1: c scope = %d, want explicit base", cReq.scope)
		c.Check(cReq.filter == "(uid=a+b)", "scenario 1: c filter = %q, want (uid=a+b) decoded once", cReq.filter)
		c.Check(equalStrings(cReq.attributes, []string{"uid"}), "scenario 1: c attributes = %v, want [uid]", cReq.attributes)

		// The caller's request was never modified.
		c.Check(req.BaseDN == "dc=example" && equalStrings(req.Attributes, []string{"cn", "sn"}) && len(req.Controls) == 0,
			"scenario 1: caller's SearchRequest was modified")

		// The borrowed root connection still works and nil policy keeps
		// the legacy one-by-one delivery.
		root.ReferralPolicy = nil
		rawResult, rawErr := root.Search(req)
		c.Check(rawErr == nil, "scenario 1: raw search after referral call failed: %v", rawErr)
		c.Check(equalStrings(entryDNs(rawResult), []string{"cn=r1,dc=example", "cn=r2,dc=example"}),
			"scenario 1: raw entries wrong: %v", entryDNs(rawResult))
		c.Check(len(rawResult.Referrals) == 2, "scenario 1: raw referrals = %v, want 2", rawResult.Referrals)

		// Every dialed connection is closed exactly once by the call, the
		// borrowed root connection is not closed for us.
		allClosed := true
		exactlyOnce := true
		for _, dialed := range rig.dialed {
			if !waitForClose(dialed) {
				allClosed = false
			}
			if dialed.closeCount() != 1 {
				exactlyOnce = false
			}
		}
		c.Check(allClosed, "scenario 1: some dialed connections were never closed")
		c.Check(exactlyOnce, "scenario 1: some dialed connections were closed more than once")
		c.Check(rootCounted.closeCount() == 0, "scenario 1: borrowed root connection was closed by the call")
		root.Close()

		// Scenario 2: handler stop and handler error keep the original
		// conventions; errorChan is notified exactly once.
		root2, _, _ := newTestRoot(t, func(req *recordedSearchRequest) []testReply {
			return testReplies(
				testEntryOp("cn=e1,dc=example"),
				testEntryOp("cn=e2,dc=example"),
				testEntryOp("cn=e3,dc=example"),
				testDoneOp(LDAPResultSuccess),
			)
		})
		root2.ReferralPolicy = newReferralRig().policy()
		simpleReq := NewSimpleSearchRequest("dc=example", ScopeWholeSubtree, "(objectClass=*)", []string{"cn"})

		stopHandler := &stopAfterHandler{stopAfter: 1}
		stopErr := root2.SearchWithHandler(simpleReq, stopHandler, nil)
		c.Check(stopErr == nil, "scenario 2: stop returned error: %v", stopErr)
		c.Check(stopHandler.seen == 1, "scenario 2: handler saw %d results after stop, want 1", stopHandler.seen)

		boom := errors.New("boom")
		errHandler := &errorAfterHandler{errAfter: 1, err: boom}
		handlerErr := root2.SearchWithHandler(simpleReq, errHandler, nil)
		c.Check(handlerErr == boom, "scenario 2: handler error = %v, want boom", handlerErr)
		c.Check(errHandler.seen == 1, "scenario 2: handler saw %d results before error, want 1", errHandler.seen)

		errChan := make(chan error, 2)
		chanErr := root2.SearchWithHandler(simpleReq, &recordingHandler{}, errChan)
		c.Check(chanErr == nil, "scenario 2: errorChan call returned: %v", chanErr)
		notified := <-errChan
		c.Check(notified == nil, "scenario 2: errorChan notification = %v, want nil", notified)
		select {
		case extra := <-errChan:
			c.Check(false, "scenario 2: errorChan notified more than once: %v", extra)
		case <-time.After(100 * time.Millisecond):
		}
		root2.Close()
	})
}

type stopAfterHandler struct {
	stopAfter int
	seen      int
}

func (h *stopAfterHandler) ProcessDiscreteResult(dsr *DiscreteSearchResult, info *ConnectionInfo) (bool, error) {
	h.seen++
	return h.seen >= h.stopAfter, nil
}

type errorAfterHandler struct {
	errAfter int
	err      error
	seen     int
}

func (h *errorAfterHandler) ProcessDiscreteResult(dsr *DiscreteSearchResult, info *ConnectionInfo) (bool, error) {
	h.seen++
	if h.seen >= h.errAfter {
		return false, h.err
	}
	return false, nil
}
