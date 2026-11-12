// SPDX-FileCopyrightText: 2025 The gopacket Authors
// SPDX-License-Identifier: BSD-3-Clause

package ip6defrag

import (
	"bytes"
	"encoding/binary"
	"net"
	"testing"
	"time"

	"github.com/gopacket/gopacket"
	"github.com/gopacket/gopacket/layers"
)

// Raw-entry-point contract tests. Every packet is built by hand so the
// assertions can compare whole output bytes, not just decoded fields.

var (
	rawSrc = net.ParseIP("2001:db8::1")
	rawDst = net.ParseIP("2001:db8::2")
	rawAlt = net.ParseIP("2001:db8::bad")
)

// rawTC packs a 6-bit DSCP and a 2-bit ECN into a Traffic Class byte.
func rawTC(dscp, ecn uint8) uint8 { return dscp<<2 | ecn }

// rawFixed builds a 40-byte IPv6 fixed header; Length is left zero and
// fixed per fragment by rawFragPacket.
func rawFixed(nh, tc, hop byte, src, dst net.IP) []byte {
	b := make([]byte, 40)
	b[0] = 0x60 | tc>>4
	b[1] = tc << 4
	b[6] = nh
	b[7] = hop
	copy(b[8:24], src.To16())
	copy(b[24:40], dst.To16())
	return b
}

// rawExt builds an 8n-byte Hop-by-Hop/Routing/Destination extension header.
// Routing/Destination bodies carry a PadN option so the TLV walk stays
// inside the header when the output is decoded again.
func rawExt(nh, hdrLen byte, body []byte) []byte {
	b := make([]byte, int(hdrLen)*8+8)
	b[0], b[1] = nh, hdrLen
	copy(b[2:], body)
	return b
}

// rawPadN is a Pad1/PadN option filling n bytes of option space.
func rawPadN(n int) []byte {
	if n == 1 {
		return []byte{0}
	}
	b := make([]byte, n)
	b[0], b[1] = 1, byte(n-2)
	return b
}

// rawFragHdr builds an 8-byte Fragment header. off is a byte offset.
func rawFragHdr(nh byte, id uint32, off int, more bool) []byte {
	b := make([]byte, 8)
	b[0] = nh
	v := uint16(off)
	if more {
		v |= 1
	}
	binary.BigEndian.PutUint16(b[2:4], v)
	binary.BigEndian.PutUint32(b[4:8], id)
	return b
}

// rawFragPacket assembles one fragment capture: a prefix ending in a
// Next Header value of 44, the Fragment header and its data. Length,
// Traffic Class and Hop Limit are written onto the fixed header.
func rawFragPacket(prefix []byte, tc, hop byte, fh, data []byte) []byte {
	b := make([]byte, len(prefix)+8+len(data))
	copy(b, prefix)
	copy(b[len(prefix):], fh)
	copy(b[len(prefix)+8:], data)
	b[0] = 0x60 | tc>>4
	b[1] = tc << 4
	b[7] = hop
	binary.BigEndian.PutUint16(b[4:6], uint16(len(b)-40))
	return b
}

// rawUDP is a small, valid UDP header plus a payload.
func rawUDP(sport, dport uint16, payload []byte) []byte {
	b := make([]byte, 8+len(payload))
	binary.BigEndian.PutUint16(b[0:2], sport)
	binary.BigEndian.PutUint16(b[2:4], dport)
	binary.BigEndian.PutUint16(b[4:6], uint16(len(b)))
	copy(b[8:], payload)
	return b
}

// rawDecode runs the output back through the normal decoder.
func rawDecode(t *testing.T, b []byte) gopacket.Packet {
	t.Helper()
	p := gopacket.NewPacket(b, layers.LayerTypeIPv6, gopacket.Default)
	if el := p.ErrorLayer(); el != nil {
		t.Fatalf("re-decode failed at %v: %v", el.LayerType(), el.Error())
	}
	return p
}

func TestRawIPv6Contract(t *testing.T) {
	base := time.Unix(1700000000, 0)

	t.Run("extensions", func(t *testing.T) {
		const id = uint32(11)
		udp := rawUDP(1234, 4321, []byte("upper-layer-payload")) // 8+19 = 27
		// A Destination header also exists behind the Fragment point, so
		// it must survive as an extension rather than being read as UDP.
		dest := rawExt(17, 0, rawPadN(6))    // NH = UDP
		fragmentable := cat(dest, udp)       // 8 + 27 = 35
		hbh := rawExt(43, 0, rawPadN(6))     // NH = Routing
		routing := rawExt(44, 0, rawPadN(6)) // NH = Fragment
		prefix := cat(rawFixed(0, 0, 64, rawSrc, rawDst), hbh, routing)

		// Tail fragment first: the zero-offset template arrives last.
		tail := rawFragPacket(prefix, 0, 64, rawFragHdr(60, id, 16, false), append([]byte(nil), fragmentable[16:]...))
		head := rawFragPacket(prefix, 0, 64, rawFragHdr(60, id, 0, true), append([]byte(nil), fragmentable[:16]...))
		// Trailing capture bytes must never enter the result.
		tail = append(tail, 0xAA, 0xBB, 0xCC)

		d := NewIPv6Defragmenter()
		out, err := d.DefragIPv6Bytes(tail, base)
		if err != nil || out != nil {
			t.Fatalf("tail fragment: got (%v, %v), want (nil, nil)", out, err)
		}
		out, err = d.DefragIPv6Bytes(head, base.Add(time.Second))
		if err != nil {
			t.Fatalf("head fragment: %v", err)
		}

		want := cat(
			rawFixed(0, 0, 64, rawSrc, rawDst),
			rawExt(43, 0, rawPadN(6)),
			rawExt(60, 0, rawPadN(6)), // NH back to Destination
			fragmentable,
		)
		binary.BigEndian.PutUint16(want[4:6], uint16(len(want)-40))
		if !bytes.Equal(out, want) {
			t.Fatalf("reassembled bytes mismatch\n got %x\nwant %x", out, want)
		}

		pkt := rawDecode(t, out)
		ip := pkt.Layer(layers.LayerTypeIPv6).(*layers.IPv6)
		if ip.Length != uint16(len(want)-40) || ip.NextHeader != layers.IPProtocolIPv6HopByHop {
			t.Fatalf("fixed header: length %d next %v", ip.Length, ip.NextHeader)
		}
		for _, lt := range []gopacket.LayerType{
			layers.LayerTypeIPv6HopByHop,
			layers.LayerTypeIPv6Routing,
			layers.LayerTypeIPv6Destination,
			layers.LayerTypeUDP,
		} {
			if pkt.Layer(lt) == nil {
				t.Fatalf("re-decoded packet is missing layer %v", lt)
			}
		}
		if pkt.Layer(layers.LayerTypeIPv6Fragment) != nil {
			t.Fatal("reassembled packet still carries a Fragment header")
		}
		if got := pkt.Layer(layers.LayerTypeUDP).(*layers.UDP).LayerPayload(); !bytes.Equal(got, []byte("upper-layer-payload")) {
			t.Fatalf("UDP payload %q", got)
		}

		// A packet without a Fragment header comes straight back as a
		// bounded, independent copy.
		plain := cat(rawFixed(17, 0, 64, rawSrc, rawDst), udp, []byte("trailer"))
		binary.BigEndian.PutUint16(plain[4:6], uint16(len(udp)))
		got, err := d.DefragIPv6Bytes(plain, base)
		if err != nil {
			t.Fatalf("plain packet: %v", err)
		}
		if len(got) != 40+len(udp) || !bytes.Equal(got, plain[:40+len(udp)]) {
			t.Fatalf("plain copy not bounded: %d bytes", len(got))
		}
		for i := range plain {
			plain[i] = 0
		}
		if !bytes.Equal(got[40:], udp) {
			t.Fatal("plain copy borrows the caller's buffer")
		}

		// Parse failures are errors, never a packet still waiting.
		for name, bad := range map[string][]byte{
			"truncated": func() []byte {
				b := rawFragPacket(prefix, 0, 64, rawFragHdr(60, id+9, 0, true), []byte("12345678"))
				return b[:len(b)-4]
			}(),
			"bad extension length": func() []byte {
				badHBH := rawExt(44, 0, rawPadN(6))
				badHBH[1] = 7 // claims 64 bytes while only 8 are present
				p := cat(rawFixed(0, 0, 64, rawSrc, rawDst), badHBH)
				return rawFragPacket(p, 0, 64, rawFragHdr(60, id+9, 0, true), []byte("12345678"))
			}(),
			"jumbogram": func() []byte {
				b := rawFragPacket(prefix, 0, 64, rawFragHdr(60, id+9, 0, true), []byte("12345678"))
				b[4], b[5] = 0, 0
				return b
			}(),
		} {
			if out, err := d.DefragIPv6Bytes(bad, base); err == nil || out != nil {
				t.Fatalf("%s packet: got (%v, %v), want an error", name, out, err)
			}
		}

		// A well-formed chain ending before any Fragment header, even one
		// starting with Hop-by-Hop, is a normal packet: bounded copy back.
		noFrag := func() []byte {
			hbhToUDP := rawExt(17, 0, rawPadN(6)) // NH = UDP, no Fragment
			p := cat(rawFixed(0, 0, 64, rawSrc, rawDst), hbhToUDP, udp, []byte("trailer"))
			binary.BigEndian.PutUint16(p[4:6], uint16(8+len(udp)))
			return p
		}()
		got, err = d.DefragIPv6Bytes(noFrag, base)
		if err != nil || len(got) != 40+8+len(udp) {
			t.Fatalf("extension chain without fragment: got %d bytes, err %v", len(got), err)
		}
		if pkt := rawDecode(t, got); pkt.Layer(layers.LayerTypeIPv6HopByHop) == nil || pkt.Layer(layers.LayerTypeUDP) == nil {
			t.Fatal("unfragmented extension chain was not returned intact")
		}

		// HopLimit is excluded from the prefix comparison, and the output
		// keeps the zero-offset fragment's value even when that fragment
		// arrives last.
		d2 := NewIPv6Defragmenter()
		tailHop := rawFragPacket(prefix, 0, 33, rawFragHdr(60, id+10, 16, false), append([]byte(nil), fragmentable[16:]...))
		headHop := rawFragPacket(prefix, 0, 64, rawFragHdr(60, id+10, 0, true), append([]byte(nil), fragmentable[:16]...))
		if _, err := d2.DefragIPv6Bytes(tailHop, base); err != nil {
			t.Fatal(err)
		}
		out, err = d2.DefragIPv6Bytes(headHop, base)
		if err != nil {
			t.Fatal(err)
		}
		if out[7] != 64 {
			t.Fatalf("HopLimit = %d, want the zero-offset fragment's 64", out[7])
		}
	})

	t.Run("ecn", func(t *testing.T) {
		const id = uint32(22)
		prefix := cat(rawFixed(44, 0, 64, rawSrc, rawDst))
		partA, partB := []byte("01234567"), []byte("abcdefgh")

		feed := func(d *IPv6Defragmenter, ecnA, ecnB, dscpA, dscpB uint8) ([]byte, error) {
			a := rawFragPacket(prefix, rawTC(dscpA, ecnA), 64, rawFragHdr(17, id, 0, true), partA)
			b := rawFragPacket(prefix, rawTC(dscpB, ecnB), 64, rawFragHdr(17, id, 8, false), partB)
			if out, err := d.DefragIPv6Bytes(a, base); err != nil || out != nil {
				return nil, err
			}
			return d.DefragIPv6Bytes(b, base.Add(time.Second))
		}
		wantPayload := cat(partA, partB)

		cases := []struct {
			name       string
			ecnA, ecnB uint8
			wantECN    uint8
		}{
			{"all not-ect", 0, 0, 0},
			{"only ect0", 1, 1, 1},
			{"only ect1", 2, 2, 2},
			{"mixed ect takes ect0", 1, 2, 1},
			{"ce with ect takes ce", 1, 3, 3},
		}
		for _, c := range cases {
			t.Run(c.name, func(t *testing.T) {
				d := NewIPv6Defragmenter()
				out, err := feed(d, c.ecnA, c.ecnB, 0, 0)
				if err != nil {
					t.Fatalf("reassembly: %v", err)
				}
				pkt := rawDecode(t, out)
				if got := pkt.Layer(layers.LayerTypeIPv6).(*layers.IPv6).TrafficClass & 0x03; got != c.wantECN {
					t.Fatalf("ECN = %d, want %d", got, c.wantECN)
				}
				if !bytes.Equal(out[40:], wantPayload) {
					t.Fatalf("payload %q", out[40:])
				}
			})
		}

		// Not-ECT mixed with any marked value discards the group.
		d := NewIPv6Defragmenter()
		if _, err := feed(d, 0, 2, 0, 0); err == nil {
			t.Fatal("Not-ECT mixed with ECT(1) was accepted")
		}
		// The group was cleared: a fresh, valid group with the same key
		// reassembles from scratch.
		if out, err := feed(d, 1, 1, 0, 0); err != nil || out == nil {
			t.Fatalf("group not cleared after ECN conflict: %v", err)
		}

		// DSCP comes from the zero-offset fragment and must match on all.
		d = NewIPv6Defragmenter()
		if _, err := feed(d, 1, 1, 0x05, 0x06); err == nil {
			t.Fatal("differing DSCP values were accepted")
		}

		// A different unfragmentable prefix under the same Identification
		// must not be stitched onto the group.
		d = NewIPv6Defragmenter()
		otherPrefix := cat(rawFixed(0, 0, 64, rawSrc, rawDst), rawExt(44, 0, rawPadN(6)))
		a := rawFragPacket(prefix, 0, 64, rawFragHdr(17, id, 0, true), partA)
		b := rawFragPacket(otherPrefix, 0, 64, rawFragHdr(17, id, 8, false), partB)
		if _, err := d.DefragIPv6Bytes(a, base); err != nil {
			t.Fatal(err)
		}
		if _, err := d.DefragIPv6Bytes(b, base); err == nil {
			t.Fatal("conflicting extension chains were accepted")
		}

		// Strict overlap rejection: an exact duplicate is an error, and a
		// bad fragment must never make a gapped group emit early.
		d = NewIPv6Defragmenter()
		tail := rawFragPacket(prefix, 0, 64, rawFragHdr(17, id, 8, false), partB)
		if out, err := d.DefragIPv6Bytes(tail, base); err != nil || out != nil {
			t.Fatalf("tail first: %v %v", out, err)
		}
		dup := rawFragPacket(prefix, 0, 64, rawFragHdr(17, id, 8, false), partB)
		if _, err := d.DefragIPv6Bytes(dup, base); err == nil {
			t.Fatal("duplicate fragment was accepted")
		}
		head := rawFragPacket(prefix, 0, 64, rawFragHdr(17, id, 0, true), partA)
		if out, err := d.DefragIPv6Bytes(head, base); err != nil || out != nil {
			t.Fatalf("cleared group must not output early: %v %v", out, err)
		}
		out, err := d.DefragIPv6Bytes(rawFragPacket(prefix, 0, 64, rawFragHdr(17, id, 8, false), partB), base)
		if err != nil || !bytes.Equal(out[40:], wantPayload) {
			t.Fatalf("group did not recover: %v", err)
		}
	})

	t.Run("atomic", func(t *testing.T) {
		const id = uint32(33)
		udp := rawUDP(9, 9, []byte("atomic-data"))
		prefix := cat(rawFixed(44, 0, 64, rawSrc, rawDst))

		// An atomic fragment strips its header immediately.
		atomic := rawFragPacket(prefix, 0, 64, rawFragHdr(17, id, 0, false), udp)
		d := NewIPv6Defragmenter()
		out, err := d.DefragIPv6Bytes(atomic, base)
		if err != nil {
			t.Fatal(err)
		}
		want := cat(rawFixed(17, 0, 64, rawSrc, rawDst), udp)
		binary.BigEndian.PutUint16(want[4:6], uint16(len(want)-40))
		if !bytes.Equal(out, want) {
			t.Fatalf("atomic output\n got %x\nwant %x", out, want)
		}
		pkt := rawDecode(t, out)
		if pkt.Layer(layers.LayerTypeIPv6Fragment) != nil || pkt.Layer(layers.LayerTypeUDP) == nil {
			t.Fatal("atomic fragment was not stripped correctly")
		}
		if n := d.DiscardOlderThan(base.Add(time.Hour)); n != 0 {
			t.Fatalf("atomic fragment left a group behind: %d", n)
		}

		// It neither joins nor deletes an ordinary group sharing the key.
		d = NewIPv6Defragmenter()
		head := rawFragPacket(prefix, 0, 64, rawFragHdr(17, id, 0, true), []byte("01234567"))
		if out, err := d.DefragIPv6Bytes(head, base); err != nil || out != nil {
			t.Fatalf("ordinary head: %v %v", out, err)
		}
		if out, err := d.DefragIPv6Bytes(atomic, base); err != nil || !bytes.Equal(out, want) {
			t.Fatalf("atomic inside an open group: %v %v", out, err)
		}
		tail := rawFragPacket(prefix, 0, 64, rawFragHdr(17, id, 8, false), []byte("abcdefgh"))
		out, err = d.DefragIPv6Bytes(tail, base)
		if err != nil || string(out[40:]) != "01234567abcdefgh" {
			t.Fatalf("ordinary group disturbed by atomic fragment: %v %q", err, out)
		}

		// Queued fragments and outputs never borrow the caller's buffer.
		d = NewIPv6Defragmenter()
		buf := rawFragPacket(prefix, 0, 64, rawFragHdr(17, id+1, 0, true), []byte("01234567"))
		if _, err := d.DefragIPv6Bytes(buf, base); err != nil {
			t.Fatal(err)
		}
		for i := range buf {
			buf[i] = 0
		}
		out, err = d.DefragIPv6Bytes(rawFragPacket(prefix, 0, 64, rawFragHdr(17, id+1, 8, false), []byte("abcdefgh")), base)
		if err != nil {
			t.Fatal(err)
		}
		if got := out[40:48]; !bytes.Equal(got, []byte("01234567")) {
			t.Fatalf("queued fragment aliased the input buffer: %q", got)
		}
		outSnap := append([]byte(nil), out...)
		for i := range buf {
			buf[i] = 0xFF
		}
		if !bytes.Equal(out, outSnap) {
			t.Fatal("output aliased the caller's buffer")
		}
	})

	t.Run("identity", func(t *testing.T) {
		const id = uint32(44)
		prefix := cat(rawFixed(44, 0, 64, rawSrc, rawDst))
		altPrefix := cat(rawFixed(44, 0, 64, rawAlt, rawDst))
		partA, partB := []byte("01234567"), []byte("abcdefgh")

		// Same Identification, different addresses: independent groups.
		d := NewIPv6Defragmenter()
		a1 := rawFragPacket(prefix, 0, 64, rawFragHdr(17, id, 0, true), partA)
		a2 := rawFragPacket(altPrefix, 0, 64, rawFragHdr(17, id, 0, true), []byte("XXXXXXXX"))
		if _, err := d.DefragIPv6Bytes(a1, base); err != nil {
			t.Fatal(err)
		}
		if _, err := d.DefragIPv6Bytes(a2, base); err != nil {
			t.Fatal(err)
		}
		out, err := d.DefragIPv6Bytes(rawFragPacket(prefix, 0, 64, rawFragHdr(17, id, 8, false), partB), base)
		if err != nil || string(out[40:]) != "01234567abcdefgh" {
			t.Fatalf("address-based grouping failed: %v %q", err, out)
		}
		out, err = d.DefragIPv6Bytes(rawFragPacket(altPrefix, 0, 64, rawFragHdr(17, id, 8, false), []byte("yyyyyyyy")), base)
		if err != nil || string(out[40:]) != "XXXXXXXXyyyyyyyy" {
			t.Fatalf("second group corrupted: %v %q", err, out)
		}

		// The next protocol named by the Fragment header is group-wide.
		d = NewIPv6Defragmenter()
		if _, err := d.DefragIPv6Bytes(rawFragPacket(prefix, 0, 64, rawFragHdr(17, id, 0, true), partA), base); err != nil {
			t.Fatal(err)
		}
		if _, err := d.DefragIPv6Bytes(rawFragPacket(prefix, 0, 64, rawFragHdr(6, id, 8, false), partB), base); err == nil {
			t.Fatal("differing Fragment Next Header values were accepted")
		}

		// The two entry points keep their half-assembled packets apart.
		d = NewIPv6Defragmenter()
		ip := &layers.IPv6{
			Version:    6,
			NextHeader: layers.IPProtocolIPv6Fragment,
			HopLimit:   64,
			SrcIP:      rawSrc,
			DstIP:      rawDst,
		}
		legacyFrag := func(off int, more bool, pl []byte) *layers.IPv6Fragment {
			f := &layers.IPv6Fragment{
				NextHeader:     layers.IPProtocolUDP,
				FragmentOffset: uint16(off / 8),
				MoreFragments:  more,
				Identification: id,
			}
			f.Payload = pl
			return f
		}
		if got := d.DefragIPv6(ip, legacyFrag(0, true, partA)); got != nil {
			t.Fatal("legacy fragment completed early")
		}
		out, err = d.DefragIPv6Bytes(rawFragPacket(prefix, 0, 64, rawFragHdr(17, id, 0, true), partA), base)
		if err != nil || out != nil {
			t.Fatalf("raw entry saw the legacy group: %v %v", out, err)
		}
		out, err = d.DefragIPv6Bytes(rawFragPacket(prefix, 0, 64, rawFragHdr(17, id, 8, false), partB), base)
		if err != nil || string(out[40:]) != "01234567abcdefgh" {
			t.Fatalf("raw group did not reassemble beside the legacy one: %v", err)
		}
		if got := d.DefragIPv6(ip, legacyFrag(8, false, partB)); got == nil || string(got.Payload) != "01234567abcdefgh" {
			t.Fatal("legacy group was disturbed by the raw entry")
		}

		// Ageing uses the first fragment's timestamp only, in both maps.
		d = NewIPv6Defragmenter()
		if _, err := d.DefragIPv6Bytes(rawFragPacket(prefix, 0, 64, rawFragHdr(17, id, 0, true), partA), base); err != nil {
			t.Fatal(err)
		}
		// A later fragment with a younger timestamp must not rejuvenate it.
		if _, err := d.DefragIPv6Bytes(rawFragPacket(prefix, 0, 64, rawFragHdr(17, id, 8, true), partB), base.Add(time.Hour)); err != nil {
			t.Fatal(err)
		}
		if n := d.DiscardOlderThan(base.Add(time.Minute)); n != 1 {
			t.Fatalf("DiscardOlderThan removed %d raw groups, want 1", n)
		}
		d = NewIPv6Defragmenter()
		if _, err := d.DefragIPv6Bytes(rawFragPacket(prefix, 0, 64, rawFragHdr(17, id, 0, true), partA), base.Add(time.Hour)); err != nil {
			t.Fatal(err)
		}
		// An older timestamp on a later fragment cannot age the group.
		if _, err := d.DefragIPv6Bytes(rawFragPacket(prefix, 0, 64, rawFragHdr(17, id, 8, true), partB), base); err != nil {
			t.Fatal(err)
		}
		if n := d.DiscardOlderThan(base.Add(time.Minute)); n != 0 {
			t.Fatalf("out-of-order timestamp changed the group age: removed %d", n)
		}
	})
}
