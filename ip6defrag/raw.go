// SPDX-FileCopyrightText: 2025 The gopacket Authors
// SPDX-License-Identifier: BSD-3-Clause

package ip6defrag

import (
	"encoding/binary"
	"errors"
	"fmt"
	"slices"
	"time"

	"github.com/gopacket/gopacket"
	"github.com/gopacket/gopacket/layers"
)

// IPv6 extension headers that may appear in the unfragmentable part of a
// packet, ahead of the Fragment header (RFC 8200 section 4.5).
var rawExtHeaders = map[layers.IPProtocol]bool{
	layers.IPProtocolIPv6HopByHop:    true,
	layers.IPProtocolIPv6Routing:     true,
	layers.IPProtocolIPv6Destination: true,
}

// rawParsed is one capture parsed by DefragIPv6Bytes down to its Fragment
// header. Everything it keeps is a private copy, so a caller reusing the
// capture buffer cannot mutate queued state.
type rawParsed struct {
	// wire is the bounded IPv6 packet: fixed header plus exactly Length bytes.
	wire []byte
	// fragOff is the offset of the Fragment header inside wire.
	fragOff int
	// nextHeaderPos is the byte position, inside wire, of the Next Header
	// field that points at the Fragment header: byte 6 of the fixed header
	// for a Fragment right after it, otherwise byte 0 of the preceding
	// extension header.
	nextHeaderPos int
	// nextHeader walks the extension chain while parsing and ends up
	// pointing at the Fragment header.
	nextHeader layers.IPProtocol
	offset     int
	more       bool
	id         uint32
	// ecn is the two-bit ECN field carried in the Traffic Class.
	ecn  uint8
	dscp uint8
}

// rawState is one fragment group fed through DefragIPv6Bytes. Its byte
// ranges are managed by the same fragmentList as the object API.
type rawState struct {
	list *fragmentList
	// time is when the first fragment of the group arrived; later
	// fragments, even out-of-order timestamps, do not rejuvenate it.
	time time.Time
	// template is the unfragmentable part of the zero-offset fragment,
	// compared against every later fragment with the Length, HopLimit and
	// ECN fields masked out. nil until the zero-offset fragment arrives.
	template []byte
	dscp     uint8
	// nextHeader is the protocol named by the Fragment header of the
	// zero-offset fragment; every other fragment must name the same one.
	nextHeader layers.IPProtocol
	// ecnSet collects the ECN value seen on each fragment, as a 4-bit mask.
	ecnSet uint8
}

// DefragIPv6Bytes feeds one captured IPv6 packet, starting at the 40-byte
// IPv6 fixed header, into the defragmenter. It returns nil, nil while the
// fragment group still has gaps and an independent copy of the reassembled
// IPv6 packet once it is complete. Packets without a Fragment header are
// returned immediately as a bounded copy: trailing capture bytes are not
// part of the result.
//
// The unfragmentable part (any Hop-by-Hop, Routing and Destination headers
// ahead of the Fragment header) is preserved verbatim; on completion the
// Fragment header is removed, the preceding Next Header field is rewritten
// to the protocol the Fragment header declared, and the IPv6 payload length
// is recomputed. Jumbograms are not handled.
func (d *IPv6Defragmenter) DefragIPv6Bytes(data []byte, timestamp time.Time) ([]byte, error) {
	wire, p, err := parseRawPacket(data)
	if err != nil {
		return nil, err
	}
	// A packet without a Fragment header is returned at once as a bounded,
	// independent copy; trailing capture bytes were dropped at parse time.
	if p == nil {
		return wire, nil
	}

	// An atomic fragment (offset 0, M = 0) is the whole packet: strip the
	// Fragment header right away without touching any ordinary group that
	// happens to share the same addresses and Identification (RFC 8200
	// section 4.5).
	if p.offset == 0 && !p.more {
		return p.assembleAtomic(), nil
	}

	d.lock.Lock()
	defer d.lock.Unlock()

	key := ipv6Key{ip6: gopacket.NewFlow(layers.EndpointIPv6, p.wire[8:24], p.wire[24:40]), id: p.id}
	st, ok := d.rawContainer[key]
	if !ok {
		st = &rawState{list: &fragmentList{}, time: timestamp}
	}

	if err := st.admit(p); err != nil {
		delete(d.rawContainer, key)
		return nil, err
	}
	if !st.list.complete() {
		d.rawContainer[key] = st
		return nil, nil
	}

	// The unfragmentable part counts toward the payload length too, so the
	// whole reassembled packet must still fit the 16-bit field (non-jumbo).
	if st.list.frags[0].raw.fragOff-40+st.list.end > maxPayload {
		delete(d.rawContainer, key)
		return nil, fmt.Errorf("ip6defrag: reassembled payload of %d bytes exceeds %d", st.list.frags[0].raw.fragOff-40+st.list.end, maxPayload)
	}

	// The group is complete; the same Identification may name a new packet
	// afterwards, so forget it before building.
	delete(d.rawContainer, key)
	return st.build(), nil
}

// parseRawPacket validates one capture. It returns the bounded packet and a
// parsed fragment; p is nil when the packet carries no Fragment header at
// all. Failures are reported with the byte position inside the capture,
// never as a packet still waiting for more fragments.
func parseRawPacket(data []byte) (wire []byte, p *rawParsed, err error) {
	if len(data) < 40 {
		return nil, nil, fmt.Errorf("ip6defrag: offset %d: IPv6 header truncated: %d bytes", len(data), len(data))
	}
	if data[0]>>4 != 6 {
		return nil, nil, fmt.Errorf("ip6defrag: offset 0: not an IPv6 packet (version %d)", data[0]>>4)
	}
	length := int(binary.BigEndian.Uint16(data[4:6]))
	if length == 0 {
		return nil, nil, errors.New("ip6defrag: offset 4: jumbogram (payload length 0) is not supported")
	}
	if 40+length > len(data) {
		return nil, nil, fmt.Errorf("ip6defrag: offset 4: payload length %d exceeds the %d captured payload bytes", length, len(data)-40)
	}

	// Keep a private, bounded copy: trailing capture bytes stay out, and a
	// caller reusing data cannot reach queued fragments.
	wire = slices.Clone(data[:40+length])
	p = &rawParsed{
		wire:          wire,
		nextHeaderPos: 6,
		nextHeader:    layers.IPProtocol(wire[6]),
		// ECN is the low two bits of the Traffic Class; the class's low
		// nibble is the high nibble of the second header byte.
		ecn:  (wire[1] >> 4) & 0x03,
		dscp: (wire[0]&0x0f)<<2 | wire[1]>>6,
	}

	pos := 40
	for p.nextHeader != layers.IPProtocolIPv6Fragment {
		if !rawExtHeaders[p.nextHeader] {
			// A well-formed chain that reaches any terminal protocol
			// without a Fragment header is simply an unfragmented packet,
			// whether the terminal header sits right behind the fixed
			// header or after Hop-by-Hop/Routing/Destination headers.
			return wire, nil, nil
		}
		if pos+2 > len(wire) {
			return nil, nil, fmt.Errorf("ip6defrag: offset %d: extension header truncated", pos)
		}
		extLen := int(wire[pos+1])*8 + 8
		if pos+extLen > len(wire) {
			return nil, nil, fmt.Errorf("ip6defrag: offset %d: extension header length %d runs past the packet", pos+1, extLen)
		}
		p.nextHeaderPos = pos
		p.nextHeader = layers.IPProtocol(wire[pos])
		pos += extLen
	}
	if pos+8 > len(wire) {
		return nil, nil, fmt.Errorf("ip6defrag: offset %d: fragment header truncated", pos)
	}

	p.fragOff = pos
	frag := wire[pos:]
	offM := binary.BigEndian.Uint16(frag[2:4])
	p.offset = int(offM>>3) * fragmentUnit
	p.more = offM&0x0001 != 0
	p.id = binary.BigEndian.Uint32(frag[4:8])

	dataLen := len(frag) - 8
	if dataLen == 0 {
		return nil, nil, fmt.Errorf("ip6defrag: offset %d: fragment carries no data", pos)
	}
	if p.more && dataLen%fragmentUnit != 0 {
		return nil, nil, fmt.Errorf("ip6defrag: offset %d: non-final fragment of %d bytes is not a multiple of 8", pos, dataLen)
	}
	if p.offset+dataLen > maxPayload {
		return nil, nil, fmt.Errorf("ip6defrag: offset %d: reassembled packet would exceed %d bytes", pos, maxPayload)
	}
	return wire, p, nil
}

// admit stores one fragment in the group, enforcing the group-wide rules:
// one declared next protocol, one unfragmentable prefix, matching DSCP, a
// legal ECN set and strictly non-overlapping byte ranges.
func (st *rawState) admit(p *rawParsed) error {
	next := layers.IPProtocol(p.wire[p.fragOff])

	// ECN is decided from the set over the whole group. Not-ECT mixed with
	// any marked value makes the set unusable (RFC 3168).
	newSet := st.ecnSet | 1<<p.ecn
	if newSet&1 != 0 && newSet&^1 != 0 {
		return fmt.Errorf("ip6defrag: offset 1: conflicting ECN values (Not-ECT mixed with %#x)", p.ecn)
	}

	if st.template != nil {
		if err := st.checkAgainstTemplate(p, next); err != nil {
			return err
		}
	}

	in := fragment{
		start: p.offset,
		end:   p.offset + len(p.wire) - p.fragOff - 8,
		// wire is already a private copy made at parse time, so the
		// payload slice cannot alias the caller's buffer.
		payload: p.wire[p.fragOff+8:],
		more:    p.more,
		raw:     p,
	}
	switch st.list.insert(in) {
	case ignored:
		// The raw API rejects every overlap, an exact repeat included.
		return fmt.Errorf("ip6defrag: offset %d: fragment repeats bytes already held", p.fragOff+2)
	case dropPacket:
		return fmt.Errorf("ip6defrag: offset %d: overlapping or inconsistent fragment", p.fragOff+2)
	}
	st.ecnSet = newSet

	if p.offset == 0 {
		// The zero-offset fragment supplies the final template and may
		// arrive last, so check the fragments already queued against it.
		st.template = p.prefix()
		st.dscp = p.dscp
		st.nextHeader = next
		for _, f := range st.list.frags {
			if f.start == 0 {
				continue
			}
			if err := st.checkAgainstTemplate(f.raw, layers.IPProtocol(f.raw.wire[f.raw.fragOff])); err != nil {
				return err
			}
		}
	}
	return nil
}

// checkAgainstTemplate verifies a later fragment against the unfragmentable
// part of the zero-offset fragment: same declared next protocol, same DSCP
// and identical prefix apart from the masked fields.
func (st *rawState) checkAgainstTemplate(p *rawParsed, next layers.IPProtocol) error {
	if next != st.nextHeader {
		return fmt.Errorf("ip6defrag: offset %d: next header %d differs from the zero-offset fragment's %d", p.fragOff, next, st.nextHeader)
	}
	if p.dscp != st.dscp {
		return fmt.Errorf("ip6defrag: offset 1: DSCP %#x differs from the zero-offset fragment DSCP %#x", p.dscp, st.dscp)
	}
	got := p.prefix()
	if len(got) != len(st.template) {
		return fmt.Errorf("ip6defrag: offset %d: unfragmentable part of %d bytes differs from the zero-offset fragment's %d", p.fragOff, len(got), len(st.template))
	}
	for i := range got {
		if got[i] != st.template[i] {
			return fmt.Errorf("ip6defrag: offset %d: unfragmentable part differs from the zero-offset fragment at byte %d", p.fragOff, i)
		}
	}
	return nil
}

// prefix returns the unfragmentable part of a fragment packet, the bytes
// every fragment of the group must carry identically apart from the Length,
// HopLimit and ECN fields.
func (p *rawParsed) prefix() []byte {
	b := slices.Clone(p.wire[:p.fragOff])
	b[4], b[5] = 0, 0 // payload length
	b[7] = 0          // hop limit
	b[1] &^= 0x30     // ECN
	return b
}

// ecnResult reduces the ECN set of the whole group: Not-ECT stays Not-ECT,
// a single ECT value is kept, ECT(0)+ECT(1) becomes ECT(0), and any CE
// mixed with ECT becomes CE.
func (st *rawState) ecnResult() uint8 {
	switch {
	case st.ecnSet == 1<<0:
		return 0 // Not-ECT
	case st.ecnSet&(1<<3) != 0 && st.ecnSet&(1<<1|1<<2) != 0:
		return 3 // CE mixed with ECT
	case st.ecnSet&(1<<1) != 0 && st.ecnSet&(1<<2) != 0:
		return 1 // ECT(0) and ECT(1) together
	default:
		for i := uint8(1); i < 4; i++ {
			if st.ecnSet&(1<<i) != 0 {
				return i
			}
		}
		return 0
	}
}

// build reassembles the complete group into one independent IPv6 packet.
func (st *rawState) build() []byte {
	first := st.list.frags[0].raw
	fragOff := first.fragOff

	b := make([]byte, 0, fragOff+st.list.end)
	b = append(b, first.wire[:fragOff]...)
	for _, f := range st.list.frags {
		b = append(b, f.payload...)
	}

	// Drop the Fragment header: the preceding Next Header field names the
	// protocol the Fragment header declared.
	b[first.nextHeaderPos] = first.wire[fragOff]
	// Recompute the payload length: everything after the fixed header.
	binary.BigEndian.PutUint16(b[4:6], uint16(len(b)-40))
	// ECN is the group-wide result; DSCP already comes from the template.
	b[1] = (b[1] &^ 0x30) | st.ecnResult()<<4
	return b
}

// assembleAtomic strips the Fragment header of an atomic fragment without
// consulting any queued group.
func (p *rawParsed) assembleAtomic() []byte {
	b := make([]byte, 0, len(p.wire)-8)
	b = append(b, p.wire[:p.fragOff]...)
	b = append(b, p.wire[p.fragOff+8:]...)
	b[p.nextHeaderPos] = p.wire[p.fragOff]
	binary.BigEndian.PutUint16(b[4:6], uint16(len(b)-40))
	return b
}
