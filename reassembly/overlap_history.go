// Copyright 2025 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package reassembly

// StreamPoolOptions configures optional behavior of a StreamPool.
//
// The zero value keeps the historical behavior of NewStreamPool: every option
// is disabled.
type StreamPoolOptions struct {
	// OverlapHistoryBytes, when strictly positive, enables detection of
	// inconsistent TCP segment overlaps for every connection of the pool.
	//
	// For each direction of a connection, the assembler keeps at most
	// OverlapHistoryBytes bytes that were actually delivered to the Stream
	// (the most recent continuous suffix), in addition to comparing against
	// bytes still waiting in the out-of-order delivery queue. When a packet
	// accepted by Accept carries bytes that differ, at the same sequence
	// number, from bytes still known in either place, the Stream is notified
	// through the optional OverlapDetector interface before the packet's own
	// ReassembledSG callback runs.
	//
	// Lifetime of the retained history:
	//   - it is attached to one direction of one connection held by the pool,
	//     so every Assembler sharing the pool sees the same history;
	//   - the two directions keep independent histories and are never compared
	//     with each other, even at identical sequence numbers;
	//   - delivering across a sequence gap discards the previous continuous
	//     interval and the history restarts from the new interval;
	//   - closing the half-connection releases it, and recycling the
	//     connection object through the pool starts from an empty history.
	//
	// Limitation: only bytes still present can be compared. Bytes of the
	// delivered suffix older than the last OverlapHistoryBytes bytes are
	// evicted, and bytes before a gap are invalidated: an inconsistent
	// retransmission of such bytes is not reported. A zero or negative value
	// disables the feature and retains all legacy behavior and performance
	// characteristics.
	OverlapHistoryBytes int
}

// TCPOverlap describes one maximal run of bytes on which an incoming TCP
// segment disagrees with the bytes already known at the same sequence
// numbers.
//
// Existing and Incoming always have the same length and differ at every
// position; equal surrounding bytes and unknown (never received or already
// evicted) positions split runs into separate notifications. Both slices are
// independent copies owned by the receiver: keeping or mutating them affects
// neither the packet, the internal page cache nor the retained history.
type TCPOverlap struct {
	// Direction is the half-connection the conflicting segment belongs to.
	Direction TCPFlowDirection
	// Seq is the TCP sequence number of the first byte of the run.
	Seq Sequence
	// Existing are the bytes previously queued or delivered, in sequence
	// order.
	Existing []byte
	// Incoming are the bytes carried by the current packet, in sequence
	// order.
	Incoming []byte
}

// OverlapDetector is the optional interface a Stream can implement to be
// notified of inconsistent TCP overlaps. The Stream interface itself is
// unchanged: streams that do not implement it keep working exactly as before.
//
// OverlapDetected is called while the triggering packet is being processed,
// after Accept accepted it and before any ReassembledSG call triggered by that
// same packet. The AssemblerContext belongs to the triggering packet.
type OverlapDetector interface {
	OverlapDetected(overlap TCPOverlap, ac AssemblerContext)
}

// overlapHistory stores a bounded, continuous suffix of the bytes that were
// actually delivered to a Stream in one direction. The slice is owned by the
// history and never aliases packet payloads or page buffers.
type overlapHistory struct {
	limit int
	base  Sequence // sequence number of bytes[0]; meaningful when len > 0
	bytes []byte
}

func newOverlapHistory(limit int) *overlapHistory {
	if limit <= 0 {
		return nil
	}
	return &overlapHistory{limit: limit}
}

// release drops every retained byte. It is safe to call repeatedly.
func (h *overlapHistory) release() {
	if h == nil {
		return
	}
	h.bytes = nil
}

// delivered records data newly delivered to the Stream, starting at seq. If
// data does not continue the stored interval (first delivery, or a gap lies
// between the two), the old interval is discarded and the history restarts
// with data. Only the last h.limit bytes are retained.
func (h *overlapHistory) delivered(seq Sequence, data []byte) {
	if h == nil || len(data) == 0 {
		return
	}
	if len(h.bytes) == 0 || h.base.Add(len(h.bytes)).Difference(seq) != 0 {
		// Discontinuity (first delivery, or a gap): invalidate the old
		// interval and drop its backing memory, then restart with data.
		h.base = seq
		h.bytes = nil
	}
	if len(data) >= h.limit {
		// Only the suffix fits: restart the backing array so the history
		// never grows beyond the configured bound, even transiently.
		tail := data[len(data)-h.limit:]
		h.bytes = append(h.bytes[:0], tail...)
		h.base = seq.Add(len(data) - h.limit)
		return
	}
	h.bytes = append(h.bytes, data...)
	if excess := len(h.bytes) - h.limit; excess > 0 {
		// Compact in place: evicted bytes must not linger beyond the bound.
		copy(h.bytes, h.bytes[excess:])
		h.bytes = h.bytes[:h.limit]
		h.base = h.base.Add(excess)
	}
}

// byteInterval is a contiguous run of bytes starting at seq. Intervals are
// disjoint and ordered by sequence number.
type byteInterval struct {
	seq   Sequence
	bytes []byte
}

// detectOverlaps compares the assembler's cached incoming segment with every
// byte still known for the direction: the bounded delivered history followed
// by the pages of the out-of-order queue. It reports, in payload order, the
// maximal runs of differing bytes.
//
// Equal bytes and positions covered by neither interval (never received, or
// already evicted) end a run; boundaries between adjacent internal pages do
// not, so a difference spanning several pages stays a single notification.
// It must run before checkOverlap/overlapExisting mutate the queue or trim
// the incoming segment.
func (a *Assembler) detectOverlaps(half *halfconnection, ac AssemblerContext) {
	h := half.hist
	if h == nil {
		return
	}
	detector, ok := half.stream.(OverlapDetector)
	if !ok {
		return
	}
	in := a.cacheLP.bytes
	if len(in) == 0 {
		return
	}
	inSeq := a.cacheLP.seq

	// Delivered history and queued pages are disjoint and in sequence order.
	intervals := make([]byteInterval, 0, 8)
	if len(h.bytes) > 0 {
		intervals = append(intervals, byteInterval{h.base, h.bytes})
	}
	for p := half.first; p != nil; p = p.next {
		intervals = append(intervals, byteInterval{p.seq, p.bytes})
	}
	if len(intervals) == 0 {
		// Nothing delivered or queued yet: an incoming segment cannot
		// conflict with anything known.
		return
	}

	runStart := -1
	runCursor := 0
	cursor := 0
	emit := func(end int) {
		// Independent copies: the receiver may keep and mutate them without
		// touching the packet, the page cache or the retained history.
		n := end - runStart
		old := make([]byte, n)
		fresh := make([]byte, n)
		ivCursor := runCursor
		for k := runStart; k < end; k++ {
			pos := inSeq.Add(k)
			for ivCursor < len(intervals) &&
				intervals[ivCursor].seq.Add(len(intervals[ivCursor].bytes)).Difference(pos) >= 0 {
				ivCursor++
			}
			iv := intervals[ivCursor]
			off := iv.seq.Difference(pos)
			old[k-runStart] = iv.bytes[off]
			fresh[k-runStart] = in[k]
		}
		detector.OverlapDetected(TCPOverlap{
			Direction: half.dir,
			Seq:       inSeq.Add(runStart),
			Existing:  old,
			Incoming:  fresh,
		}, ac)
	}
	for i := 0; i < len(in); i++ {
		pos := inSeq.Add(i)
		for cursor < len(intervals) &&
			intervals[cursor].seq.Add(len(intervals[cursor].bytes)).Difference(pos) >= 0 {
			cursor++
		}
		different := false
		if cursor < len(intervals) {
			iv := intervals[cursor]
			if off := iv.seq.Difference(pos); off >= 0 && off < len(iv.bytes) {
				different = iv.bytes[off] != in[i]
			}
		}
		switch {
		case different && runStart < 0:
			runStart = i
			runCursor = cursor
		case !different && runStart >= 0:
			emit(i)
			runStart = -1
		}
	}
	if runStart >= 0 {
		emit(len(in))
	}
}

// recordDelivered stores the bytes of the ScatterGather just passed to
// ReassembledSG that were delivered for the first time.
//
// The SG starts with the pages kept by an earlier KeepFrom (Lengths' saved
// count): re-sending them must not record them a second time, so recording
// starts after the saved prefix. Bytes kept again by this SG's KeepFrom are
// still delivered now: they are recorded here, and when a later SG re-sends
// them they belong to that SG's saved prefix and are skipped, which also
// keeps them from consuming the bounded capacity a second time.
func (a *Assembler) recordDelivered(half *halfconnection) {
	h := half.hist
	if h == nil {
		return
	}
	ro := &a.cacheSG
	total, saved := ro.Lengths()
	if total <= saved {
		return
	}
	pos := 0
	for _, bc := range ro.all {
		next := pos + bc.length()
		if next > saved && pos < total {
			lo := pos
			if lo < saved {
				lo = saved
			}
			hi := next
			if hi > total {
				hi = total
			}
			chunk := bc.getBytes()[lo-pos : hi-pos]
			h.delivered(bc.getSeq().Add(lo-pos), chunk)
		}
		pos = next
	}
}
