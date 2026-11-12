// Copyright 2025 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package reassembly

import (
	"bytes"
	"net"
	"testing"
	"time"

	"github.com/gopacket/gopacket"
	"github.com/gopacket/gopacket/layers"
)

// overlapEvent is a retained copy of one OverlapDetected notification.
type overlapEvent struct {
	step     int
	dir      TCPFlowDirection
	seq      Sequence
	existing []byte
	incoming []byte
	ctx      AssemblerContext
}

type sgRecord struct {
	step  int
	data  []byte
	dir   TCPFlowDirection
	skip  int
	start bool
	end   bool
	ctx   AssemblerContext
}

// overlapTestStream is a Stream that also implements OverlapDetector.
type overlapTestStream struct {
	keepFrom   int             // KeepFrom offset, < 0 disables
	rejectSeqs map[uint32]bool // segments rejected by Accept
	completes  bool            // ReassemblyComplete return value

	step        int
	events      []overlapEvent
	sgs         []sgRecord
	mutate      bool           // corrupt notification slices on receipt
	mutateFetch bool           // corrupt the bytes returned by Fetch
	retain      []overlapEvent // slices kept across callbacks
}

func (s *overlapTestStream) New(_, _ gopacket.Flow, _ *layers.TCP, _ AssemblerContext) Stream {
	return s
}

func (s *overlapTestStream) Accept(tcp *layers.TCP, _ gopacket.CaptureInfo, _ TCPFlowDirection, _ Sequence, _ *bool, _ AssemblerContext) bool {
	return !s.rejectSeqs[tcp.Seq]
}

func (s *overlapTestStream) OverlapDetected(o TCPOverlap, ac AssemblerContext) {
	s.step++
	ev := overlapEvent{
		step:     s.step,
		dir:      o.Direction,
		seq:      o.Seq,
		existing: append([]byte(nil), o.Existing...),
		incoming: append([]byte(nil), o.Incoming...),
		ctx:      ac,
	}
	s.events = append(s.events, ev)
	s.retain = append(s.retain, ev)
	if s.mutate {
		// The library must hand over independent memory: scribbling over the
		// notification slices must not corrupt the history or later output.
		for i := range o.Existing {
			o.Existing[i] = 0
		}
		for i := range o.Incoming {
			o.Incoming[i] = 0
		}
	}
}

func (s *overlapTestStream) ReassembledSG(sg ScatterGather, ac AssemblerContext) {
	s.step++
	n, _ := sg.Lengths()
	dir, start, end, skip := sg.Info()
	data := append([]byte(nil), sg.Fetch(n)...)
	s.sgs = append(s.sgs, sgRecord{
		step:  s.step,
		data:  data,
		dir:   dir,
		skip:  skip,
		start: start,
		end:   end,
		ctx:   ac,
	})
	if s.mutateFetch {
		// The history was recorded before this callback: scribbling over the
		// Fetch memory must not rewrite what was already retained.
		fetched := sg.Fetch(n)
		for i := range fetched {
			fetched[i] = 0
		}
	}
	if s.keepFrom >= 0 && s.keepFrom < n {
		sg.KeepFrom(s.keepFrom)
	}
}

func (s *overlapTestStream) ReassemblyComplete(_ AssemblerContext) bool {
	return s.completes
}

func (s *overlapTestStream) delivered() []byte {
	var out []byte
	for _, r := range s.sgs {
		out = append(out, r.data...)
	}
	return out
}

// plainStream implements only Stream, without OverlapDetector.
type plainStream struct {
	data []byte
}

func (s *plainStream) New(_, _ gopacket.Flow, _ *layers.TCP, _ AssemblerContext) Stream {
	return s
}
func (s *plainStream) Accept(_ *layers.TCP, _ gopacket.CaptureInfo, _ TCPFlowDirection, _ Sequence, _ *bool, _ AssemblerContext) bool {
	return true
}
func (s *plainStream) ReassembledSG(sg ScatterGather, _ AssemblerContext) {
	n, _ := sg.Lengths()
	s.data = append(s.data, sg.Fetch(n)...)
}
func (s *plainStream) ReassemblyComplete(_ AssemblerContext) bool { return true }

// segmentContext is a recognizable AssemblerContext so tests can assert the
// notification carries the context of the triggering packet.
type segmentContext struct {
	id int
	ci gopacket.CaptureInfo
}

func (c *segmentContext) GetCaptureInfo() gopacket.CaptureInfo { return c.ci }

type overlapHarness struct {
	t        *testing.T
	pool     *StreamPool
	streams  []*overlapTestStream
	ctxID    int
	baseTime time.Time
}

func newOverlapHarness(t *testing.T, historyBytes int, streams ...*overlapTestStream) *overlapHarness {
	t.Helper()
	if streams == nil {
		streams = []*overlapTestStream{{keepFrom: -1, rejectSeqs: map[uint32]bool{}, completes: true}}
	}
	for _, s := range streams {
		if s.rejectSeqs == nil {
			s.rejectSeqs = map[uint32]bool{}
		}
	}
	factory := &multiStreamFactory{streams: streams}
	pool := NewStreamPoolWithOptions(factory, StreamPoolOptions{OverlapHistoryBytes: historyBytes})
	return &overlapHarness{
		t:        t,
		pool:     pool,
		streams:  streams,
		baseTime: time.Unix(1000, 0),
	}
}

type multiStreamFactory struct {
	streams []*overlapTestStream
}

func (f *multiStreamFactory) New(_, _ gopacket.Flow, _ *layers.TCP, _ AssemblerContext) Stream {
	return f.streams[0]
}

func (h *overlapHarness) flow() gopacket.Flow {
	f, _ := gopacket.FlowFromEndpoints(
		layers.NewIPEndpoint(net.IP{10, 0, 0, 1}),
		layers.NewIPEndpoint(net.IP{10, 0, 0, 2}))
	return f
}

func (h *overlapHarness) reverseFlow() gopacket.Flow {
	f, _ := gopacket.FlowFromEndpoints(
		layers.NewIPEndpoint(net.IP{10, 0, 0, 2}),
		layers.NewIPEndpoint(net.IP{10, 0, 0, 1}))
	return f
}

// send pushes one hand-written segment through the real AssembleWithContext
// entry point and returns the context it carried.
func (h *overlapHarness) send(a *Assembler, f gopacket.Flow, seq uint32, syn, fin, rst bool, payload []byte) *segmentContext {
	h.t.Helper()
	h.ctxID++
	ctx := &segmentContext{
		id: h.ctxID,
		ci: gopacket.CaptureInfo{Timestamp: h.baseTime.Add(time.Duration(h.ctxID) * time.Second)},
	}
	tcp := &layers.TCP{
		SrcPort:   12345,
		DstPort:   80,
		Seq:       seq,
		SYN:       syn,
		FIN:       fin,
		RST:       rst,
		BaseLayer: layers.BaseLayer{Payload: payload},
	}
	if f == h.reverseFlow() {
		tcp.SrcPort, tcp.DstPort = 80, 12345
	}
	tcp.SetInternalPortsForTesting()
	a.AssembleWithContext(f, tcp, ctx)
	return ctx
}

func expectEvent(t *testing.T, got overlapEvent, dir TCPFlowDirection, seq Sequence, existing, incoming string, ctx AssemblerContext) {
	t.Helper()
	if got.dir != dir {
		t.Fatalf("event direction: got %v want %v", got.dir, dir)
	}
	if got.seq != seq {
		t.Fatalf("event seq: got %d want %d", got.seq, seq)
	}
	if string(got.existing) != existing {
		t.Fatalf("event existing: got %q want %q", got.existing, existing)
	}
	if string(got.incoming) != incoming {
		t.Fatalf("event incoming: got %q want %q", got.incoming, incoming)
	}
	if len(got.existing) != len(got.incoming) {
		t.Fatalf("event slices differ in length: %d vs %d", len(got.existing), len(got.incoming))
	}
	for i := range got.existing {
		if got.existing[i] == got.incoming[i] {
			t.Fatalf("event position %d is equal (%q), runs must differ at every byte", i, got.existing[i])
		}
	}
	if ctx != nil && got.ctx != ctx {
		t.Fatalf("event context does not belong to the triggering packet")
	}
}

// TestOverlapHistoryQueued covers conflicts against the out-of-order delivery
// queue: equal-byte run splitting, unknown-gap splitting, multi-page spans and
// the unchanged overwrite/delivery policy.
func TestOverlapHistoryQueued(t *testing.T) {
	stream := &overlapTestStream{keepFrom: -1, completes: true}
	h := newOverlapHarness(t, 16, stream)
	a := NewAssembler(h.pool)

	// Equal bytes split one conflict into two runs; nothing is delivered yet.
	h.send(a, h.flow(), 1000, true, false, false, nil)
	h.send(a, h.flow(), 1005, false, false, false, []byte("ABCD"))
	ctx := h.send(a, h.flow(), 1005, false, false, false, []byte("AXCZ"))
	if len(stream.events) != 2 {
		t.Fatalf("queued ABCD vs AXCZ: got %d notifications, want 2", len(stream.events))
	}
	expectEvent(t, stream.events[0], TCPDirClientToServer, 1006, "B", "X", ctx)
	expectEvent(t, stream.events[1], TCPDirClientToServer, 1008, "D", "Z", ctx)
	if len(stream.delivered()) != 0 {
		t.Fatalf("out-of-order bytes must not be delivered before the gap is filled")
	}

	// Filling the gap still delivers the overwriting payload, exactly as the
	// legacy policy does.
	h.send(a, h.flow(), 1001, false, false, false, []byte("gap!"))
	if got := string(stream.delivered()); got != "gap!AXCZ" {
		t.Fatalf("reassembly output changed: got %q want %q", got, "gap!AXCZ")
	}

	// A byte-identical retransmission is not a conflict.
	before := len(stream.events)
	h.send(a, h.flow(), 1005, false, false, false, []byte("AXCZ"))
	if len(stream.events) != before {
		t.Fatalf("identical retransmission reported %d spurious conflicts", len(stream.events)-before)
	}

	// A single differing byte against the now-delivered history is reported
	// with the history bytes.
	ctx = h.send(a, h.flow(), 1006, false, false, false, []byte("Y"))
	if len(stream.events) != before+1 {
		t.Fatalf("delivered-history conflict missing")
	}
	expectEvent(t, stream.events[before], TCPDirClientToServer, 1006, "X", "Y", ctx)

	// An unknown gap inside the segment splits the run into two events.
	stream2 := &overlapTestStream{keepFrom: -1, completes: true}
	h2 := newOverlapHarness(t, 16, stream2)
	a2 := NewAssembler(h2.pool)
	h2.send(a2, h2.flow(), 1000, true, false, false, nil)
	h2.send(a2, h2.flow(), 1005, false, false, false, []byte("ABCD")) // 1005-1008
	h2.send(a2, h2.flow(), 1011, false, false, false, []byte("GH"))   // 1009-1010 unknown
	ctx = h2.send(a2, h2.flow(), 1005, false, false, false, []byte("XBCDIJQH"))
	if len(stream2.events) != 2 {
		t.Fatalf("unknown-gap split: got %d events, want 2", len(stream2.events))
	}
	expectEvent(t, stream2.events[0], TCPDirClientToServer, 1005, "A", "X", ctx)
	expectEvent(t, stream2.events[1], TCPDirClientToServer, 1011, "G", "Q", ctx)

	// A difference spanning internal page boundaries stays one notification.
	stream3 := &overlapTestStream{keepFrom: -1, completes: true}
	h3 := newOverlapHarness(t, 64, stream3)
	a3 := NewAssembler(h3.pool)
	h3.send(a3, h3.flow(), 1000, true, false, false, nil)
	payload := bytes.Repeat([]byte{'A'}, 3900)
	h3.send(a3, h3.flow(), 1005, false, false, false, payload)
	ctx = h3.send(a3, h3.flow(), 2904, false, false, false, []byte("XYZ"))
	if len(stream3.events) != 1 {
		t.Fatalf("multi-page overlap: got %d events, want 1", len(stream3.events))
	}
	expectEvent(t, stream3.events[0], TCPDirClientToServer, 2904, "AAA", "XYZ", ctx)
	// The queued pages were overwritten and the eventual output matches the
	// baseline multi-page policy.
	h3.send(a3, h3.flow(), 1001, false, false, false, []byte("gap!"))
	want := append([]byte("gap!"), payload...)
	copy(want[1903:1906], "XYZ")
	if !bytes.Equal(stream3.delivered(), want) {
		t.Fatalf("multi-page reassembly output diverged from baseline")
	}
}

// TestOverlapHistoryDelivered covers the bounded delivered-history: wrap-around
// retransmits, partial comparability after eviction, gap invalidation and
// KeepFrom accounting.
func TestOverlapHistoryDelivered(t *testing.T) {
	// Cross-zero retransmission: differing runs are split by equal bytes and
	// the second run's sequence number wraps.
	stream := &overlapTestStream{keepFrom: -1, completes: true}
	h := newOverlapHarness(t, 16, stream)
	a := NewAssembler(h.pool)
	start := uint32(0xfffffffc)
	h.send(a, h.flow(), start-1, true, false, false, nil)
	h.send(a, h.flow(), start, false, false, false, []byte("ABCDEFGH"))
	if got := string(stream.delivered()); got != "ABCDEFGH" {
		t.Fatalf("initial delivery: got %q", got)
	}
	sgCount := len(stream.sgs)
	ctx := h.send(a, h.flow(), start+2, false, false, false, []byte("cDEf"))
	if len(stream.sgs) != sgCount {
		t.Fatalf("fully-retransmitted segment must not produce new data callbacks")
	}
	if len(stream.events) != 2 {
		t.Fatalf("wrap retransmit: got %d events, want 2", len(stream.events))
	}
	expectEvent(t, stream.events[0], TCPDirClientToServer, Sequence(start+2), "C", "c", ctx)
	expectEvent(t, stream.events[1], TCPDirClientToServer, Sequence(start+5), "F", "f", ctx)
	if uint32(stream.events[1].seq) != 1 {
		t.Fatalf("wrapped event seq: got %d want 1", uint32(stream.events[1].seq))
	}
	// Notifications strictly precede the SG of the packet that triggers one:
	// this retransmit conflicts on the delivered prefix and extends past it.
	h.send(a, h.flow(), start+2, false, false, false, []byte("cDEfghIJ"))
	if len(stream.events) != 4 {
		t.Fatalf("second wrap retransmit: got %d total events, want 4", len(stream.events))
	}
	if len(stream.sgs) != sgCount+1 {
		t.Fatalf("expected one new SG from the extending segment, got %d", len(stream.sgs)-sgCount)
	}
	if stream.events[len(stream.events)-1].step >= stream.sgs[sgCount].step {
		t.Fatalf("OverlapDetected must run before the packet's ReassembledSG")
	}
	if got := string(stream.sgs[sgCount].data); got != "IJ" {
		t.Fatalf("extending segment delivery: got %q want IJ", got)
	}

	// Eviction: only the suffix still present is comparable; earlier bytes of
	// the incoming segment find no history and produce nothing.
	stream2 := &overlapTestStream{keepFrom: -1, completes: true}
	h2 := newOverlapHarness(t, 4, stream2)
	a2 := NewAssembler(h2.pool)
	h2.send(a2, h2.flow(), 1000, true, false, false, nil)
	h2.send(a2, h2.flow(), 1001, false, false, false, []byte("ABCDEFGH"))
	conn := h2.pool.conns[h2.key()]
	if got := len(conn.c2s.hist.bytes); got != 4 {
		t.Fatalf("history bound not enforced: got %d bytes, want 4", got)
	}
	ctx = h2.send(a2, h2.flow(), 1003, false, false, false, []byte("cdefgh"))
	if len(stream2.events) != 1 {
		t.Fatalf("evicted overlap: got %d events, want 1", len(stream2.events))
	}
	expectEvent(t, stream2.events[0], TCPDirClientToServer, 1005, "EFGH", "efgh", ctx)

	// Delivering across a gap invalidates the old continuous interval: the
	// pre-gap bytes can no longer be compared, the new interval can.
	stream3 := &overlapTestStream{keepFrom: -1, completes: true}
	h3 := newOverlapHarness(t, 16, stream3)
	a3 := NewAssembler(h3.pool)
	h3.send(a3, h3.flow(), 1000, true, false, false, nil)
	h3.send(a3, h3.flow(), 1001, false, false, false, []byte("ab"))
	h3.send(a3, h3.flow(), 1005, false, false, false, []byte("XY")) // gap at 1003-1004
	flushed, _ := a3.FlushWithOptions(FlushOptions{T: h3.baseTime.Add(100 * time.Second)})
	if flushed != 1 {
		t.Fatalf("flush: got %d flushed connections, want 1", flushed)
	}
	if got := string(stream3.delivered()); got != "abXY" {
		t.Fatalf("gap delivery: got %q want abXY", got)
	}
	before := len(stream3.events)
	h3.send(a3, h3.flow(), 1001, false, false, false, []byte("AB"))
	if len(stream3.events) != before {
		t.Fatalf("pre-gap history was not invalidated: %d spurious events", len(stream3.events)-before)
	}
	ctx = h3.send(a3, h3.flow(), 1005, false, false, false, []byte("xY"))
	if len(stream3.events) != before+1 {
		t.Fatalf("post-gap interval conflict missing")
	}
	expectEvent(t, stream3.events[before], TCPDirClientToServer, 1005, "X", "x", ctx)

	// KeepFrom: saved bytes are recorded once, not re-recorded on the next SG,
	// yet remain comparable afterwards.
	stream4 := &overlapTestStream{keepFrom: 2, completes: true}
	h4 := newOverlapHarness(t, 8, stream4)
	a4 := NewAssembler(h4.pool)
	h4.send(a4, h4.flow(), 1000, true, false, false, nil)
	h4.send(a4, h4.flow(), 1001, false, false, false, []byte("ABCDEF"))
	h4.send(a4, h4.flow(), 1007, false, false, false, []byte("GH"))
	if got := string(stream4.delivered()); got != "ABCDEFCDEFGH" {
		t.Fatalf("KeepFrom delivery: got %q", got)
	}
	conn4 := h4.pool.conns[h4.key()]
	// SG1 recorded ABCDEF; SG2 skips the saved CDEF prefix and records GH,
	// so every byte appears exactly once and the bound holds. Re-recording
	// the saved prefix would instead restart the history at 1003 ("CDEFGH").
	if got := string(conn4.c2s.hist.bytes); got != "ABCDEFGH" {
		t.Fatalf("KeepFrom double-recorded saved bytes: history=%q want ABCDEFGH", got)
	}
	ctx = h4.send(a4, h4.flow(), 1003, false, false, false, []byte("cdef"))
	if len(stream4.events) != 1 {
		t.Fatalf("KeepFrom history conflict: got %d events, want 1", len(stream4.events))
	}
	expectEvent(t, stream4.events[0], TCPDirClientToServer, 1003, "CDEF", "cdef", ctx)
}

// TestOverlapHistoryLifecycle covers configuration gates, rejection, direction
// isolation, shared-pool assemblers, memory independence, half-close release
// and connection-object recycling.
func TestOverlapHistoryLifecycle(t *testing.T) {
	// Zero configuration: detection stays off even with an OverlapDetector.
	stream := &overlapTestStream{keepFrom: -1, completes: true}
	pool := NewStreamPool(stream)
	a := NewAssembler(pool)
	f, _ := gopacket.FlowFromEndpoints(
		layers.NewIPEndpoint(net.IP{10, 0, 0, 1}),
		layers.NewIPEndpoint(net.IP{10, 0, 0, 2}))
	send := func(seq uint32, syn bool, payload []byte) {
		tcp := &layers.TCP{SrcPort: 12345, DstPort: 80, Seq: seq, SYN: syn,
			BaseLayer: layers.BaseLayer{Payload: payload}}
		tcp.SetInternalPortsForTesting()
		ctx := &segmentContext{ci: gopacket.CaptureInfo{Timestamp: time.Now()}}
		a.AssembleWithContext(f, tcp, ctx)
	}
	send(1000, true, nil)
	send(1005, false, []byte("ABCD"))
	send(1005, false, []byte("AXCZ"))
	send(1001, false, []byte("gap!"))
	if len(stream.events) != 0 {
		t.Fatalf("zero-config pool emitted %d overlap events", len(stream.events))
	}
	if got := string(stream.delivered()); got != "gap!AXCZ" {
		t.Fatalf("zero-config output: got %q", got)
	}
	keyTCP := &layers.TCP{SrcPort: 12345, DstPort: 80}
	keyTCP.SetInternalPortsForTesting()
	if pool.conns[key{f, keyTCP.TransportFlow()}].c2s.hist != nil {
		t.Fatalf("zero-config pool allocated overlap history")
	}

	// A Stream without OverlapDetector keeps working with detection enabled.
	plain := &plainStream{}
	pool2 := NewStreamPoolWithOptions(plainFactory{plain}, StreamPoolOptions{OverlapHistoryBytes: 8})
	a2 := NewAssembler(pool2)
	send2 := func(seq uint32, syn bool, payload []byte) {
		tcp := &layers.TCP{SrcPort: 12345, DstPort: 80, Seq: seq, SYN: syn,
			BaseLayer: layers.BaseLayer{Payload: payload}}
		tcp.SetInternalPortsForTesting()
		ctx := &segmentContext{ci: gopacket.CaptureInfo{Timestamp: time.Now()}}
		a2.AssembleWithContext(f, tcp, ctx)
	}
	send2(1000, true, nil)
	send2(1005, false, []byte("ABCD"))
	send2(1005, false, []byte("AXCZ"))
	send2(1001, false, []byte("gap!"))
	if got := string(plain.data); got != "gap!AXCZ" {
		t.Fatalf("plain stream output: got %q", got)
	}

	// Rejected packets never reach comparison or delivery.
	stream3 := &overlapTestStream{keepFrom: -1, completes: true, rejectSeqs: map[uint32]bool{1005: true}}
	h3 := newOverlapHarness(t, 8, stream3)
	a3 := NewAssembler(h3.pool)
	h3.send(a3, h3.flow(), 1000, true, false, false, nil)
	h3.send(a3, h3.flow(), 1005, false, false, false, []byte("ABCD"))
	h3.send(a3, h3.flow(), 1005, false, false, false, []byte("AXCZ"))
	if len(stream3.events) != 0 || len(stream3.delivered()) != 0 {
		t.Fatalf("rejected segment produced events=%d data=%q", len(stream3.events), stream3.delivered())
	}

	// Direction isolation: identical sequence numbers on the reverse half
	// never compare against the forward half.
	stream4 := &overlapTestStream{keepFrom: -1, completes: true}
	h4 := newOverlapHarness(t, 16, stream4)
	a4 := NewAssembler(h4.pool)
	h4.send(a4, h4.flow(), 1000, true, false, false, nil)
	h4.send(a4, h4.flow(), 1001, false, false, false, []byte("AAAA"))
	h4.send(a4, h4.reverseFlow(), 1000, true, false, false, nil)
	h4.send(a4, h4.reverseFlow(), 1001, false, false, false, []byte("BBBB"))
	ctx := h4.send(a4, h4.flow(), 1001, false, false, false, []byte("CCCC"))
	if len(stream4.events) != 1 {
		t.Fatalf("bidirectional: got %d events, want 1", len(stream4.events))
	}
	expectEvent(t, stream4.events[0], TCPDirClientToServer, 1001, "AAAA", "CCCC", ctx)
	ctx = h4.send(a4, h4.reverseFlow(), 1001, false, false, false, []byte("DDDD"))
	if len(stream4.events) != 2 {
		t.Fatalf("reverse direction conflict missing")
	}
	expectEvent(t, stream4.events[1], TCPDirServerToClient, 1001, "BBBB", "DDDD", ctx)

	// Two Assemblers sharing one pool see the same history, alternating.
	stream5 := &overlapTestStream{keepFrom: -1, mutate: true, completes: true}
	h5 := newOverlapHarness(t, 16, stream5)
	a5 := NewAssembler(h5.pool)
	b5 := NewAssembler(h5.pool)
	h5.send(a5, h5.flow(), 1000, true, false, false, nil)
	h5.send(a5, h5.flow(), 1001, false, false, false, []byte("ABCDEFGH"))
	ctx = h5.send(b5, h5.flow(), 1003, false, false, false, []byte("cdef"))
	if len(stream5.events) != 1 {
		t.Fatalf("shared-pool conflict: got %d events, want 1", len(stream5.events))
	}
	expectEvent(t, stream5.events[0], TCPDirClientToServer, 1003, "CDEF", "cdef", ctx)
	// The callback mutated and retained the slices; a later comparison must
	// still observe the untouched history, and reassembly stays intact.
	ctx = h5.send(a5, h5.flow(), 1003, false, false, false, []byte("XDEF"))
	if len(stream5.events) != 2 {
		t.Fatalf("post-mutation conflict: got %d events, want 2", len(stream5.events))
	}
	expectEvent(t, stream5.events[1], TCPDirClientToServer, 1003, "C", "X", ctx)
	for _, ev := range stream5.retain {
		if !bytes.Equal(ev.existing, []byte("CDEF")) && !bytes.Equal(ev.existing, []byte("C")) {
			t.Fatalf("retained event memory changed after return: %q", ev.existing)
		}
	}

	// A Stream that mutates the Fetch memory must not rewrite the history.
	streamFetch := &overlapTestStream{keepFrom: -1, mutateFetch: true, completes: true}
	hf := newOverlapHarness(t, 16, streamFetch)
	af := NewAssembler(hf.pool)
	hf.send(af, hf.flow(), 1000, true, false, false, nil)
	hf.send(af, hf.flow(), 1001, false, false, false, []byte("ABCDEFGH"))
	connFetch := hf.pool.conns[hf.key()]
	if got := string(connFetch.c2s.hist.bytes); got != "ABCDEFGH" {
		t.Fatalf("Fetch mutation rewrote the history: got %q want ABCDEFGH", got)
	}
	// The retained history still drives a correct comparison afterwards.
	ctx = hf.send(af, hf.flow(), 1003, false, false, false, []byte("XDEF"))
	if len(streamFetch.events) != 1 {
		t.Fatalf("post-Fetch-mutation conflict: got %d events, want 1", len(streamFetch.events))
	}
	expectEvent(t, streamFetch.events[0], TCPDirClientToServer, 1003, "C", "X", ctx)

	// Half-close releases the history of that half only; the other half keeps
	// its evidence. After full close and pool reuse, no evidence survives.
	stream6 := &overlapTestStream{keepFrom: -1, completes: true}
	h6 := newOverlapHarness(t, 16, stream6)
	a6 := NewAssembler(h6.pool)
	h6.send(a6, h6.flow(), 1000, true, false, false, nil)
	h6.send(a6, h6.flow(), 1001, false, false, false, []byte("AAAA"))
	h6.send(a6, h6.reverseFlow(), 1000, true, false, false, nil)
	h6.send(a6, h6.reverseFlow(), 1001, false, false, false, []byte("BBBB"))
	conn6 := h6.pool.conns[h6.key()]
	oldHist := conn6.c2s.hist
	h6.send(a6, h6.flow(), 1005, false, true, false, nil) // FIN closes c2s
	if conn6.c2s.hist != nil {
		t.Fatalf("half-close did not release its history")
	}
	before := len(stream6.events)
	ctx = h6.send(a6, h6.reverseFlow(), 1001, false, false, false, []byte("bBBB"))
	if len(stream6.events) != before+1 {
		t.Fatalf("closing one half must not drop the other half's history")
	}
	expectEvent(t, stream6.events[before], TCPDirServerToClient, 1001, "B", "b", ctx)
	h6.send(a6, h6.reverseFlow(), 1005, false, true, false, nil) // FIN closes s2c -> remove
	if _, ok := h6.pool.conns[h6.key()]; ok {
		t.Fatalf("closed connection still held by pool")
	}
	// A new incarnation on the same 4-tuple recycles the connection object.
	h6.send(a6, h6.flow(), 2000, true, false, false, nil)
	h6.send(a6, h6.flow(), 2001, false, false, false, []byte("ABCD"))
	conn7 := h6.pool.conns[h6.key()]
	if conn7.c2s.hist == oldHist {
		t.Fatalf("recycled connection reused the old history object")
	}
	// The fresh history holds the new incarnation's bytes at its own sequence
	// numbers; nothing from the closed connection (seq 1001) survived.
	if got := string(conn7.c2s.hist.bytes); got != "ABCD" || conn7.c2s.hist.base != 2001 {
		t.Fatalf("recycled connection history: base=%d bytes=%q", conn7.c2s.hist.base, got)
	}
	recycledBefore := len(stream6.events)
	ctx = h6.send(a6, h6.flow(), 2001, false, false, false, []byte("AXCZ"))
	if len(stream6.events) != recycledBefore+2 {
		t.Fatalf("recycled connection does not compare against its own fresh history")
	}
	expectEvent(t, stream6.events[recycledBefore+0], TCPDirClientToServer, 2002, "B", "X", ctx)
	expectEvent(t, stream6.events[recycledBefore+1], TCPDirClientToServer, 2004, "D", "Z", ctx)
}

// key builds the pool key of the harness flow.
func (h *overlapHarness) key() key {
	tcp := &layers.TCP{SrcPort: 12345, DstPort: 80}
	tcp.SetInternalPortsForTesting()
	return key{h.flow(), tcp.TransportFlow()}
}

type plainFactory struct {
	stream *plainStream
}

func (f plainFactory) New(_, _ gopacket.Flow, _ *layers.TCP, _ AssemblerContext) Stream {
	return f.stream
}
