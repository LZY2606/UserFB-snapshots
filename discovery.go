package zeroconf

import (
	"net"
	"strings"

	"github.com/miekg/dns"
)

// discoveryState holds the records collected during a single Browse or Lookup
// invocation. DNS-SD responses may be split over several packets and records
// may arrive in any order, therefore partial records are kept until they can be
// assembled into a complete service entry.
//
// The state is only accessed from the mainloop goroutine. Follow-up queries are
// dispatched after the incoming message has been processed, never while holding
// a processing lock.
type discoveryState struct {
	params *lookupParams

	isLookup bool
	isEnum   bool

	// Canonical (lower-case, fully qualified) service names used for matching.
	serviceName string
	// Names a PTR record may use as its owner: the browsed service plus any
	// subtypes that were requested.
	ptrNames []string

	instances     map[string]*pendingInstance
	instanceOrder []string
	sent          map[string]bool

	hosts map[string]*pendingHost

	enum      map[string]*enumPending
	enumOrder []string
}

type pendingInstance struct {
	key      string // canonical full instance name
	fullName string // original spelling, used for results and follow-up queries

	// member is true once a PTR pointing at this instance has been seen (for
	// Browse) or immediately for an explicit Lookup.
	member bool

	haveSRV bool
	srvTTL  uint32
	port    int
	// targetKey is canonical, targetName keeps the spelling from the SRV.
	targetKey  string
	targetName string

	haveTXT bool
	text    []string

	// queried records whether the SRV/TXT follow-up for this instance has
	// already been sent for this call.
	queried bool
}

type pendingHost struct {
	key  string // canonical host name
	name string // original spelling from the SRV target

	addr4 []net.IP
	addr6 []net.IP

	queried4 bool
	queried6 bool
}

type enumPending struct {
	ptr string
	ttl uint32
}

func newDiscoveryState(params *lookupParams) *discoveryState {
	svc := canonicalName(params.ServiceName())
	st := &discoveryState{
		params:      params,
		serviceName: svc,
		ptrNames:    []string{svc},
		instances:   make(map[string]*pendingInstance),
		sent:        make(map[string]bool),
		hosts:       make(map[string]*pendingHost),
		enum:        make(map[string]*enumPending),
		isEnum:      equalName(params.ServiceTypeName(), params.ServiceName()),
	}

	// A subtype browse is answered with PTR records owned by the subtype name
	// as well as (in many announcements) the base service name.
	for _, sub := range params.Subtypes {
		st.ptrNames = append(st.ptrNames, canonicalName(sub))
	}

	if params.Instance != "" {
		st.isLookup = true
		// The instance was explicitly requested, so no PTR is required to
		// establish membership. The initial query already asks for SRV and TXT.
		inst := st.instance(params.ServiceInstanceName())
		inst.member = true
		inst.queried = true
	}

	return st
}

func (st *discoveryState) instance(name string) *pendingInstance {
	key := canonicalName(name)
	if inst, ok := st.instances[key]; ok {
		return inst
	}
	inst := &pendingInstance{key: key, fullName: name}
	st.instances[key] = inst
	st.instanceOrder = append(st.instanceOrder, key)
	return inst
}

func (st *discoveryState) host(name string) *pendingHost {
	key := canonicalName(name)
	if h, ok := st.hosts[key]; ok {
		return h
	}
	h := &pendingHost{key: key, name: name}
	st.hosts[key] = h
	return h
}

// processMessage consumes one DNS message and returns entries that became
// complete as well as follow-up queries needed to fill remaining gaps.
func (st *discoveryState) processMessage(msg *dns.Msg) ([]*ServiceEntry, []*dns.Msg) {
	sections := make([]dns.RR, 0, len(msg.Answer)+len(msg.Ns)+len(msg.Extra))
	sections = append(sections, msg.Answer...)
	sections = append(sections, msg.Ns...)
	sections = append(sections, msg.Extra...)

	for _, rr := range sections {
		switch rr := rr.(type) {
		case *dns.PTR:
			st.applyPTR(rr)
		case *dns.SRV:
			st.applySRV(rr)
		case *dns.TXT:
			st.applyTXT(rr)
		case *dns.A:
			st.applyAddr(rr.Hdr, rr.A)
		case *dns.AAAA:
			st.applyAddr(rr.Hdr, rr.AAAA)
		}
	}

	return st.completeEntries(), st.followUpQueries()
}

func (st *discoveryState) matchesPTROwner(name string) bool {
	for _, n := range st.ptrNames {
		if equalName(name, n) {
			return true
		}
	}
	return false
}

func (st *discoveryState) applyPTR(rr *dns.PTR) {
	if !st.matchesPTROwner(rr.Hdr.Name) {
		return
	}

	if st.isEnum {
		key := "ptr:" + canonicalName(rr.Ptr)
		if rr.Hdr.Ttl == 0 {
			delete(st.enum, key)
			return
		}
		if _, ok := st.enum[key]; !ok {
			st.enum[key] = &enumPending{ptr: rr.Ptr, ttl: rr.Hdr.Ttl}
			st.enumOrder = append(st.enumOrder, key)
		}
		return
	}

	// An explicit Lookup is already a member; PTR records do not add anything
	// and must not be a prerequisite.
	if st.isLookup {
		return
	}

	// A PTR for the browsed service must point at an instance of that service.
	// The suffix must match on a label boundary: "other_meter._tcp.local." is
	// not an instance of "_meter._tcp.local.".
	if !nameHasSuffix(rr.Ptr, st.params.ServiceName()) || equalName(rr.Ptr, st.params.ServiceName()) {
		return
	}

	inst := st.instance(rr.Ptr)
	inst.fullName = rr.Ptr

	if rr.Hdr.Ttl == 0 {
		// Revoke this instance's PTR membership. Addresses are kept in the
		// host cache because other instances may share the same target host.
		delete(st.instances, inst.key)
		return
	}
	inst.member = true
}

func validInstanceRecordOwner(st *discoveryState, owner string) bool {
	if st.isLookup {
		return equalName(owner, st.params.ServiceInstanceName())
	}
	if st.isEnum {
		return false
	}
	return nameHasSuffix(owner, st.params.ServiceName()) &&
		!equalName(owner, st.params.ServiceName())
}

func (st *discoveryState) applySRV(rr *dns.SRV) {
	if !validInstanceRecordOwner(st, rr.Hdr.Name) {
		return
	}
	inst := st.instance(rr.Hdr.Name)

	if rr.Hdr.Ttl == 0 {
		// A revocation is not evidence that the relationship is complete.
		inst.haveSRV = false
		inst.srvTTL = 0
		inst.port = 0
		inst.targetKey = ""
		inst.targetName = ""
		return
	}

	inst.haveSRV = true
	inst.srvTTL = rr.Hdr.Ttl
	inst.port = int(rr.Port)
	inst.targetName = rr.Target
	inst.targetKey = canonicalName(rr.Target)
	if inst.targetKey != "" {
		st.host(rr.Target)
	}
}

func (st *discoveryState) applyTXT(rr *dns.TXT) {
	if !validInstanceRecordOwner(st, rr.Hdr.Name) {
		return
	}
	inst := st.instance(rr.Hdr.Name)

	if rr.Hdr.Ttl == 0 {
		inst.haveTXT = false
		inst.text = nil
		return
	}

	// Presence of the TXT record is what matters. An empty TXT array is a
	// received (and complete) TXT record, not a missing one.
	inst.haveTXT = true
	inst.text = cloneStrings(rr.Txt)
}

func (st *discoveryState) applyAddr(hdr dns.RR_Header, ip net.IP) {
	h := st.host(hdr.Name)

	if hdr.Ttl == 0 {
		if hdr.Rrtype == dns.TypeA {
			h.addr4 = removeIP(h.addr4, ip)
		} else {
			h.addr6 = removeIP(h.addr6, ip)
		}
		return
	}

	if hdr.Rrtype == dns.TypeA {
		if !containsIP(h.addr4, ip) {
			h.addr4 = append(h.addr4, cloneIP(ip))
		}
	} else {
		if !containsIP(h.addr6, ip) {
			h.addr6 = append(h.addr6, cloneIP(ip))
		}
	}
}

func (st *discoveryState) completeEntries() []*ServiceEntry {
	var out []*ServiceEntry

	for _, key := range st.instanceOrder {
		inst, ok := st.instances[key]
		if !ok || st.sent[key] {
			continue
		}
		// For Browse, membership is established by a PTR. Lookup members are
		// seeded in newDiscoveryState.
		if !inst.member || !inst.haveSRV || !inst.haveTXT {
			continue
		}
		h := st.hosts[inst.targetKey]
		if h == nil || (len(h.addr4) == 0 && len(h.addr6) == 0) {
			continue
		}

		e := NewServiceEntry(instanceLabel(inst.fullName, st.params.ServiceName()), st.params.Service, st.params.Domain)
		e.HostName = inst.targetName
		e.Port = inst.port
		e.TTL = inst.srvTTL
		e.Text = cloneStrings(inst.text)
		// Published values must not alias the pending state: a caller may
		// mutate its entry and must not affect another instance sharing a host.
		e.AddrIPv4 = cloneIPs(h.addr4)
		e.AddrIPv6 = cloneIPs(h.addr6)

		st.sent[key] = true
		out = append(out, e)
	}

	// Service type enumeration only yields PTR records and has no SRV/TXT/A
	// completeness requirements.
	if st.isEnum {
		for _, key := range st.enumOrder {
			ep, ok := st.enum[key]
			if !ok || st.sent[key] {
				continue
			}
			e := NewServiceEntry(trimDot(ep.ptr), st.params.Service, st.params.Domain)
			e.TTL = ep.ttl
			st.sent[key] = true
			out = append(out, e)
		}
	}

	return out
}

func (st *discoveryState) followUpQueries() []*dns.Msg {
	if st.isEnum {
		return nil
	}

	var queries []*dns.Msg
	for _, key := range st.instanceOrder {
		inst, ok := st.instances[key]
		if !ok || st.sent[key] || !inst.member {
			// No follow-up is needed for an instance that is already published.
			// Before Browse membership is known there is nothing to complete.
			continue
		}

		// Missing SRV or TXT: ask the instance for both. An empty TXT record
		// sets haveTXT, so it does not trigger this query.
		if (!inst.haveSRV || !inst.haveTXT) && !inst.queried {
			inst.queried = true
			queries = append(queries, newQuery(inst.fullName, dns.TypeSRV, dns.TypeTXT))
		}

		if !inst.haveSRV || inst.targetKey == "" {
			continue
		}

		// After the SRV is known, fill in the address families that are
		// missing. One family is enough to publish; the other is still queried
		// once for completeness. Instances sharing a host share this query.
		h := st.hosts[inst.targetKey]
		if h == nil {
			h = st.host(inst.targetName)
		}
		need4 := len(h.addr4) == 0 && !h.queried4
		need6 := len(h.addr6) == 0 && !h.queried6
		if !need4 && !need6 {
			continue
		}
		if need4 {
			h.queried4 = true
		}
		if need6 {
			h.queried6 = true
		}

		var types []uint16
		if need4 {
			types = append(types, dns.TypeA)
		}
		if need6 {
			types = append(types, dns.TypeAAAA)
		}
		queries = append(queries, newQuery(inst.targetName, types...))
	}

	return queries
}

func newQuery(name string, qtypes ...uint16) *dns.Msg {
	m := new(dns.Msg)
	for _, t := range qtypes {
		m.Question = append(m.Question, dns.Question{
			Name:   name,
			Qtype:  t,
			Qclass: dns.ClassINET,
		})
	}
	m.RecursionDesired = false
	return m
}

// canonicalName applies DNS name rules for comparisons: names are
// case-insensitive and the trailing dot is insignificant.
func canonicalName(name string) string {
	return strings.ToLower(dns.Fqdn(name))
}

func equalName(a, b string) bool {
	return canonicalName(a) == canonicalName(b)
}

// nameHasSuffix reports whether name ends with suffix on a DNS label boundary.
// Both names are compared case-insensitively.
func nameHasSuffix(name, suffix string) bool {
	n := canonicalName(name)
	s := canonicalName(suffix)
	if n == s {
		return true
	}
	// canonicalName always includes a trailing dot, so "."+s enforces that the
	// match begins at a label boundary.
	return strings.HasSuffix(n, "."+s)
}

// instanceLabel extracts the instance part from a full instance name such as
// "sensor._meter._tcp.local." for service "_meter._tcp.local.".
func instanceLabel(fullName, serviceName string) string {
	full := trimDot(fullName)
	svc := trimDot(serviceName)
	if len(full) >= len(svc) && strings.EqualFold(full[len(full)-len(svc):], svc) {
		prefix := full[:len(full)-len(svc)]
		return trimDot(strings.TrimSuffix(prefix, "."))
	}
	return trimDot(fullName)
}

func cloneStrings(in []string) []string {
	out := make([]string, len(in))
	copy(out, in)
	return out
}

func cloneIP(ip net.IP) net.IP {
	out := make(net.IP, len(ip))
	copy(out, ip)
	return out
}

func cloneIPs(in []net.IP) []net.IP {
	out := make([]net.IP, len(in))
	for i, ip := range in {
		out[i] = cloneIP(ip)
	}
	return out
}

func containsIP(list []net.IP, ip net.IP) bool {
	for _, candidate := range list {
		if candidate.Equal(ip) {
			return true
		}
	}
	return false
}

func removeIP(list []net.IP, ip net.IP) []net.IP {
	out := list[:0]
	for _, candidate := range list {
		if !candidate.Equal(ip) {
			out = append(out, candidate)
		}
	}
	return out
}
