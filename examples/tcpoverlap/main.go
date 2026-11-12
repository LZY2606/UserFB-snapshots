// Copyright 2025 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

// tcpoverlap demonstrates optional detection of inconsistent TCP overlaps on
// top of the standard reassembly pipeline. It replays hand-written segments
// (no capture device, no libpcap) through a real Assembler using a StreamPool
// configured with an 8-byte delivered-history, and checks every notification
// and the final reassembled output itself.
//
// Replay:
//
//	SYN, seq=1000
//	seq=1005 "ABCD"   (queued behind the 1001-1004 gap)
//	seq=1005 "AXCZ"   (overwrites the queued bytes with a conflicting payload)
//	seq=1001 "gap!"   (fills the gap, flushes "gap!AXCZ" to the stream)
//	seq=1005 "AXCZ"   (identical retransmission: no conflict)
//
// The conflicting queued overwrite raises two notifications (the equal bytes A
// and C split the two differing runs); the identical retransmission raises
// none.
package main

import (
	"fmt"
	"net"
	"os"

	"github.com/gopacket/gopacket"
	"github.com/gopacket/gopacket/layers"
	"github.com/gopacket/gopacket/reassembly"
)

type overlapRecord struct {
	seq      reassembly.Sequence
	existing []byte
	incoming []byte
	ctx      reassembly.AssemblerContext
}

type overlapStream struct {
	reassembly []byte
	conflicts  []overlapRecord
}

func (s *overlapStream) New(_, _ gopacket.Flow, _ *layers.TCP, _ reassembly.AssemblerContext) reassembly.Stream {
	return s
}

func (s *overlapStream) Accept(_ *layers.TCP, _ gopacket.CaptureInfo, _ reassembly.TCPFlowDirection, _ reassembly.Sequence, _ *bool, _ reassembly.AssemblerContext) bool {
	return true
}

// OverlapDetected is the optional conflict interface: the Stream interface
// itself is unchanged. The slices are owned by this callback.
func (s *overlapStream) OverlapDetected(o reassembly.TCPOverlap, ac reassembly.AssemblerContext) {
	s.conflicts = append(s.conflicts, overlapRecord{
		seq:      o.Seq,
		existing: append([]byte(nil), o.Existing...),
		incoming: append([]byte(nil), o.Incoming...),
		ctx:      ac,
	})
}

func (s *overlapStream) ReassembledSG(sg reassembly.ScatterGather, _ reassembly.AssemblerContext) {
	n, _ := sg.Lengths()
	s.reassembly = append(s.reassembly, sg.Fetch(n)...)
}

func (s *overlapStream) ReassemblyComplete(_ reassembly.AssemblerContext) bool { return true }

type packetContext struct {
	label string
	ci    gopacket.CaptureInfo
}

func (c *packetContext) GetCaptureInfo() gopacket.CaptureInfo { return c.ci }

func fail(format string, args ...interface{}) {
	fmt.Fprintf(os.Stderr, "tcpoverlap: "+format+"\n", args...)
	os.Exit(1)
}

func main() {
	stream := &overlapStream{}
	pool := reassembly.NewStreamPoolWithOptions(stream, reassembly.StreamPoolOptions{
		OverlapHistoryBytes: 8,
	})
	assembler := reassembly.NewAssembler(pool)

	netFlow, err := gopacket.FlowFromEndpoints(
		layers.NewIPEndpoint(net.IP{10, 0, 0, 1}),
		layers.NewIPEndpoint(net.IP{10, 0, 0, 2}))
	if err != nil {
		fail("building flow: %v", err)
	}

	send := func(label string, seq uint32, syn bool, payload []byte) *packetContext {
		ctx := &packetContext{label: label, ci: gopacket.CaptureInfo{}}
		tcp := &layers.TCP{
			SrcPort:   12345,
			DstPort:   80,
			Seq:       seq,
			SYN:       syn,
			BaseLayer: layers.BaseLayer{Payload: payload},
		}
		tcp.SetInternalPortsForTesting()
		assembler.AssembleWithContext(netFlow, tcp, ctx)
		return ctx
	}

	send("syn", 1000, true, nil)
	send("original", 1005, false, []byte("ABCD"))
	conflictCtx := send("overwrite", 1005, false, []byte("AXCZ"))
	send("gap-fill", 1001, false, []byte("gap!"))
	before := len(stream.conflicts)
	send("retransmit", 1005, false, []byte("AXCZ"))
	assembler.FlushAll()

	// The identical retransmission must not add a conflict.
	if len(stream.conflicts) != before {
		fail("identical retransmission reported %d spurious conflicts", len(stream.conflicts)-before)
	}
	// The queued overwrite is split into two maximal differing runs.
	want := []overlapRecord{
		{seq: 1006, existing: []byte("B"), incoming: []byte("X")},
		{seq: 1008, existing: []byte("D"), incoming: []byte("Z")},
	}
	if len(stream.conflicts) != len(want) {
		fail("conflict count: got %d want %d", len(stream.conflicts), len(want))
	}
	for i, w := range want {
		got := stream.conflicts[i]
		if got.seq != w.seq || string(got.existing) != string(w.existing) ||
			string(got.incoming) != string(w.incoming) {
			fail("conflict %d: got seq=%d old=%q new=%q, want seq=%d old=%q new=%q",
				i, got.seq, got.existing, got.incoming, w.seq, w.existing, w.incoming)
		}
		if len(got.existing) != len(got.incoming) {
			fail("conflict %d: slice lengths differ", i)
		}
		if got.ctx != conflictCtx {
			fail("conflict %d: context does not belong to the triggering packet", i)
		}
	}
	if got := string(stream.reassembly); got != "gap!AXCZ" {
		fail("reassembly: got %q want %q", got, "gap!AXCZ")
	}

	fmt.Printf("conflicts=%d reassembly=%s\n", len(stream.conflicts), stream.reassembly)
}
