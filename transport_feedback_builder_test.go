// SPDX-FileCopyrightText: 2023 The Pion community <https://pion.ly>
// SPDX-License-Identifier: MIT

package rtcp

import (
	"bytes"
	"math/rand"
	"testing"
	"time"

	"github.com/pion/rtcp/task_support/taskcheck"
)

// ---------------------------------------------------------------------------
// Independent helpers. These deliberately do not reuse the builder's DP.
// ---------------------------------------------------------------------------

type fbExpanded struct {
	base       uint16
	count      int
	reference  uint32
	fbCount    uint8
	symbols    []uint16
	deltas     []int64
	chunkKinds []fbChunkKind
	raw        []byte
}

// expandPacket decodes a packet through the library's public Unmarshal.
func expandPacket(t *testing.T, raw []byte) fbExpanded {
	t.Helper()
	var packet TransportLayerCC
	if err := packet.Unmarshal(raw); err != nil {
		t.Fatalf("TransportLayerCC.Unmarshal(% x): %v", raw, err)
	}
	out := fbExpanded{
		base:      packet.BaseSequenceNumber,
		count:     int(packet.PacketStatusCount),
		reference: packet.ReferenceTime,
		fbCount:   packet.FbPktCount,
		raw:       raw,
	}
	for _, chunk := range packet.PacketChunks {
		switch c := chunk.(type) {
		case *RunLengthChunk:
			out.chunkKinds = append(out.chunkKinds, fbChunkRLE)
			for i := 0; i < int(c.RunLength); i++ {
				out.symbols = append(out.symbols, c.PacketStatusSymbol)
			}
		case *StatusVectorChunk:
			if c.SymbolSize == TypeTCCSymbolSizeOneBit {
				out.chunkKinds = append(out.chunkKinds, fbChunkOneBit)
			} else {
				out.chunkKinds = append(out.chunkKinds, fbChunkTwoBit)
			}
			out.symbols = append(out.symbols, c.SymbolList...)
		default:
			t.Fatalf("unexpected chunk type %T", chunk)
		}
	}
	for _, delta := range packet.RecvDeltas {
		out.deltas = append(out.deltas, delta.Delta)
	}
	return out
}

// structExpand decodes a packet purely from its wire bytes. It checks the
// header length, padding count, chunk fill rules and delta byte accounting.
func structExpand(t *testing.T, raw []byte) fbExpanded {
	t.Helper()
	if len(raw) < 20 || (raw[0]>>6) != 2 {
		t.Fatalf("not an RTCP packet: % x", raw)
	}
	if PacketType(raw[1]) != TypeTransportSpecificFeedback {
		t.Fatalf("unexpected packet type %d", raw[1])
	}
	if raw[0]&0x1F != FormatTCC {
		t.Fatalf("unexpected FMT %d", raw[0]&0x1F)
	}
	length := int(raw[2])<<8 | int(raw[3])
	if 4*(length+1) != len(raw) {
		t.Fatalf("length field %d does not match %d bytes", length, len(raw))
	}
	out := fbExpanded{
		base:      uint16(raw[12])<<8 | uint16(raw[13]),
		count:     int(raw[14])<<8 | int(raw[15]),
		reference: (uint32(raw[16])<<16 | uint32(raw[17])<<8 | uint32(raw[18])) & 0xFFFFFF,
		fbCount:   raw[19],
		raw:       raw,
	}
	payloadEnd := len(raw)
	if raw[0]&0x20 != 0 {
		pad := int(raw[len(raw)-1])
		if pad == 0 || pad > 3 {
			t.Fatalf("bad padding count %d in % x", pad, raw)
		}
		payloadEnd -= pad
	}
	chunkStart := headerLength + packetChunkOffset
	processed := 0
	for processed < out.count {
		if chunkStart+2 > payloadEnd {
			t.Fatalf("status chunks overrun packet: % x", raw)
		}
		word := uint16(raw[chunkStart])<<8 | uint16(raw[chunkStart+1])
		if word&0x8000 == 0 {
			run := int(word & 0x1FFF)
			symbol := uint16(word>>13) & 0x03
			if run == 0 {
				t.Fatalf("zero-length run chunk in % x", raw)
			}
			out.chunkKinds = append(out.chunkKinds, fbChunkRLE)
			used := out.count - processed
			if run < used {
				used = run
			}
			for i := 0; i < used; i++ {
				out.symbols = append(out.symbols, symbol)
			}
			processed += used
		} else {
			twoBit := word&0x4000 != 0
			n := 14
			if twoBit {
				n = 7
				out.chunkKinds = append(out.chunkKinds, fbChunkTwoBit)
			} else {
				out.chunkKinds = append(out.chunkKinds, fbChunkOneBit)
			}
			partial := processed+n > out.count
			if partial {
				// Only the final chunk may be short; unused positions must be
				// not-received padding.
				n = out.count - processed
				slots := 14
				if twoBit {
					slots = 7
				}
				for i := n; i < slots; i++ {
					var symbol uint16
					if twoBit {
						symbol = (word >> uint(12-2*i)) & 0x03
					} else {
						symbol = (word >> uint(13-i)) & 0x01
					}
					if symbol != TypeTCCPacketNotReceived {
						t.Fatalf("unused vector slot %d is not not-received", i)
					}
				}
			}
			for i := 0; i < n; i++ {
				var symbol uint16
				if twoBit {
					symbol = (word >> uint(12-2*i)) & 0x03
				} else {
					symbol = (word >> uint(13-i)) & 0x01
				}
				out.symbols = append(out.symbols, symbol)
			}
			processed += n
			if partial && processed != out.count {
				t.Fatalf("short vector chunk is not the final chunk")
			}
		}
		chunkStart += 2
	}

	var deltaBytes []byte
	for i, symbol := range out.symbols {
		start := chunkStart + len(deltaBytes)
		switch symbol {
		case TypeTCCPacketReceivedSmallDelta:
			if start >= payloadEnd {
				t.Fatalf("small delta %d overruns payload end %d", i, payloadEnd)
			}
			deltaBytes = append(deltaBytes, raw[start])
		case TypeTCCPacketReceivedLargeDelta:
			if start+1 >= payloadEnd {
				t.Fatalf("large delta %d overruns payload end %d", i, payloadEnd)
			}
			deltaBytes = append(deltaBytes, raw[start], raw[start+1])
		}
	}
	if chunkStart+len(deltaBytes) != payloadEnd {
		t.Fatalf("delta bytes end at %d, payload ends at %d", chunkStart+len(deltaBytes), payloadEnd)
	}
	x := 0
	for _, symbol := range out.symbols {
		switch symbol {
		case TypeTCCPacketReceivedSmallDelta:
			out.deltas = append(out.deltas, int64(deltaBytes[x])*250)
			x++
		case TypeTCCPacketReceivedLargeDelta:
			out.deltas = append(out.deltas, int64(int16(uint16(deltaBytes[x])<<8|uint16(deltaBytes[x+1])))*250)
			x += 2
		}
	}
	return out
}

// bruteFullMin is the minimum number of chunks covering symbols[:n] when every
// vector chunk must be full. Plain O(n^2) enumeration for small inputs.
func bruteFullMin(symbols []uint16, n int) int {
	const inf = 1 << 20
	cost := make([]int, n+1)
	for i := 1; i <= n; i++ {
		cost[i] = inf
	}
	for i := 1; i <= n; i++ {
		for j := 0; j < i; j++ {
			if cost[j] == inf {
				continue
			}
			length := i - j
			ok := false
			if length <= 8191 && allEqual(symbols[j:i]) {
				ok = true
			}
			if length == 7 {
				ok = true
			}
			if length == 14 && allVectorOneBit(symbols[j:i]) {
				ok = true
			}
			if ok && cost[j]+1 < cost[i] {
				cost[i] = cost[j] + 1
			}
		}
	}
	return cost[n]
}

// bruteEndMin allows only the final chunk to be a partially used vector.
// Every preceding chunk must be full, so predecessors are scored with
// bruteFullMin's cost table, computed here once for the whole prefix.
func bruteEndMin(symbols []uint16, n int) int {
	const inf = 1 << 20
	full := make([]int, n+1)
	for i := 1; i <= n; i++ {
		full[i] = inf
	}
	for i := 1; i <= n; i++ {
		for j := 0; j < i; j++ {
			if full[j] == inf {
				continue
			}
			length := i - j
			ok := (length <= 8191 && allEqual(symbols[j:i])) ||
				length == 7 ||
				(length == 14 && allVectorOneBit(symbols[j:i]))
			if ok && full[j]+1 < full[i] {
				full[i] = full[j] + 1
			}
		}
	}
	cost := make([]int, n+1)
	for i := 1; i <= n; i++ {
		cost[i] = inf
	}
	for i := 1; i <= n; i++ {
		for j := 0; j < i; j++ {
			if full[j] == inf {
				continue
			}
			length := i - j
			ok := false
			if length <= 8191 && allEqual(symbols[j:i]) {
				ok = true
			}
			if length >= 1 && length <= 7 {
				ok = true
			}
			if length >= 1 && length <= 14 && allVectorOneBit(symbols[j:i]) {
				ok = true
			}
			if ok && full[j]+1 < cost[i] {
				cost[i] = full[j] + 1
			}
		}
	}
	return cost[n]
}

func allEqual(symbols []uint16) bool {
	for i := 1; i < len(symbols); i++ {
		if symbols[i] != symbols[0] {
			return false
		}
	}
	return true
}

func allVectorOneBit(symbols []uint16) bool {
	for _, s := range symbols {
		if s > TypeTCCPacketReceivedSmallDelta {
			return false
		}
	}
	return true
}

// bruteCovered returns the longest prefix a packet may take: symbols are
// derived with the split rule, then each prefix is scored with the minimal
// chunk count.
func bruteCovered(samples []TransportFeedbackSample, maxBytes int) int {
	symbols := make([]uint16, 0, len(samples))
	var first bool
	var last int64
	for _, s := range samples {
		var symbol uint16
		if s.Received {
			if !first {
				first = true
				last = s.ArrivalUS
				symbol = TypeTCCPacketReceivedSmallDelta
			} else {
				units := (s.ArrivalUS - last) / 250
				if units < -32768 || units > 32767 {
					break
				}
				last = s.ArrivalUS
				if units >= 0 && units <= 255 {
					symbol = TypeTCCPacketReceivedSmallDelta
				} else {
					symbol = TypeTCCPacketReceivedLargeDelta
				}
			}
		}
		symbols = append(symbols, symbol)
	}
	best := 0
	for n := 1; n <= len(symbols); n++ {
		deltaBytes := 0
		for _, s := range symbols[:n] {
			if s == TypeTCCPacketReceivedSmallDelta {
				deltaBytes++
			} else if s == TypeTCCPacketReceivedLargeDelta {
				deltaBytes += 2
			}
		}
		if recordSize(bruteEndMin(symbols, n), deltaBytes) > maxBytes {
			continue
		}
		best = n
	}
	return best
}

func recordSize(chunks, deltaBytes int) int {
	return roundUpTo4(headerLength + packetChunkOffset + chunks*packetStatusChunkLength + deltaBytes)
}

// ---------------------------------------------------------------------------

func TestFeedbackEncoding(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// 1. Interleaved small/large deltas with losses, checked against
		// hand-written bytes, the independent expander and the library
		// Unmarshal.
		samples := []TransportFeedbackSample{
			{Received: true, ArrivalUS: 64000 * 3},
			{Received: false},
			{Received: true, ArrivalUS: 64000*3 + 2*250},
			{Received: true, ArrivalUS: 64000*3 + 300*250},
			{Received: false},
			{Received: true, ArrivalUS: 64000*3 + 300*250 - 250},
		}
		packets, err := BuildTransportFeedback(0xAABBCCDD, 0x11223344, 1000, 9, samples, 65532)
		c.Check(err == nil, "unexpected error: %v", err)
		c.Check(len(packets) == 1, "expected a single packet, got %d", len(packets))
		if len(packets) == 1 {
			raw := packets[0]
			// Header: V=2, P=0, FMT=15, PT=205, length 6 (28 bytes).
			c.Check(bytes.Equal(raw[:4], []byte{0x8F, 0xCD, 0x00, 0x06}), "header bytes % x", raw[:4])
			// One full two-bit vector: 0x8000|0x4000 then symbols
			// 1,0,1,2,0,2 and a not-received filler.
			c.Check(bytes.Equal(raw[20:22], []byte{0xD1, 0x88}), "status chunk bytes % x", raw[20:22])
			// Deltas: 0 (small), 500 (small), 74500 (large), -250 (large).
			c.Check(bytes.Equal(raw[22:28], []byte{0x00, 0x02, 0x01, 0x2A, 0xFF, 0xFF}), "delta bytes % x", raw[22:28])
			c.Check(len(raw) == 28, "raw length %d", len(raw))

			got := structExpand(t, raw)
			c.Check(got.base == 1000, "base %d", got.base)
			c.Check(got.count == 6, "count %d", got.count)
			c.Check(got.reference == 3, "reference %d", got.reference)
			c.Check(got.fbCount == 9, "fb count %d", got.fbCount)
			wantSymbols := []uint16{
				TypeTCCPacketReceivedSmallDelta, TypeTCCPacketNotReceived,
				TypeTCCPacketReceivedSmallDelta, TypeTCCPacketReceivedLargeDelta,
				TypeTCCPacketNotReceived, TypeTCCPacketReceivedLargeDelta,
			}
			c.Check(equalSymbols(got.symbols, wantSymbols), "symbols %v", got.symbols)
			wantDeltas := []int64{0, 500, 74500, -250}
			c.Check(equalDeltas(got.deltas, wantDeltas), "deltas %v", got.deltas)

			lib := expandPacket(t, raw)
			c.Check(equalDeltas(lib.deltas, wantDeltas), "library deltas %v", lib.deltas)
			c.Check(lib.reference == got.reference && lib.count == got.count && lib.fbCount == got.fbCount,
				"library header mismatch")
		}

		// 2. 8191-long run of one state must be a single RLE chunk.
		run := make([]TransportFeedbackSample, 8191)
		packets, err = BuildTransportFeedback(1, 2, 0, 0, run, 65532)
		c.Check(err == nil, "8191 run error: %v", err)
		c.Check(len(packets) == 1, "8191 run packets %d", len(packets))
		if len(packets) == 1 {
			exp := structExpand(t, packets[0])
			c.Check(exp.count == 8191, "8191 count %d", exp.count)
			c.Check(equalKinds(exp.chunkKinds, []fbChunkKind{fbChunkRLE}), "8191 chunks %v", exp.chunkKinds)
			c.Check(bytes.Equal(exp.raw[20:22], []byte{0x1F, 0xFF}), "8191 RLE bytes % x", exp.raw[20:22])
			c.Check(exp.reference == 0, "all-lost reference %d", exp.reference)
			c.Check(len(exp.deltas) == 0, "all-lost deltas %d", len(exp.deltas))
			lib := expandPacket(t, packets[0])
			c.Check(lib.count == 8191 && len(lib.deltas) == 0, "library 8191 readback")
		}

		// 3. 8192-long run must use two chunks.
		run = make([]TransportFeedbackSample, 8192)
		packets, err = BuildTransportFeedback(1, 2, 0, 0, run, 65532)
		c.Check(err == nil, "8192 run error: %v", err)
		c.Check(len(packets) == 1, "8192 run packets %d", len(packets))
		if len(packets) == 1 {
			exp := structExpand(t, packets[0])
			c.Check(exp.count == 8192, "8192 count %d", exp.count)
			c.Check(len(exp.chunkKinds) == 2, "8192 chunks %v", exp.chunkKinds)
		}

		// 4. One-bit vectors win for alternating small/not-received.
		oneBit := make([]TransportFeedbackSample, 14)
		for i := range oneBit {
			if i%2 == 0 {
				oneBit[i] = TransportFeedbackSample{Received: true, ArrivalUS: int64(i/2) * 250}
			}
		}
		packets, err = BuildTransportFeedback(1, 2, 0, 0, oneBit, 65532)
		c.Check(err == nil, "one-bit case: %v", err)
		if err == nil && len(packets) == 1 {
			exp := structExpand(t, packets[0])
			c.Check(exp.count == 14, "one-bit count %d", exp.count)
			c.Check(equalKinds(exp.chunkKinds, []fbChunkKind{fbChunkOneBit}), "one-bit chunks %v", exp.chunkKinds)
			c.Check(len(exp.chunkKinds) == bruteEndMin(exp.symbols, exp.count), "one-bit not minimal")
			// The single chunk is a full 14-symbol vector, so it must also
			// match the full-chunk enumeration.
			c.Check(len(exp.chunkKinds) == bruteFullMin(exp.symbols, exp.count), "one-bit not full-minimal")
		}

		// 5. Two-bit vectors win for alternating small/large.
		twoBit := make([]TransportFeedbackSample, 7)
		for i := range twoBit {
			// Alternating 0 and 300 units: 0/300 => small, then large.
			arrival := int64(0)
			if i > 0 {
				if i%2 == 1 {
					arrival = int64(i) * 300 * 250
				} else {
					arrival = int64(i-1)*300*250 + 250
				}
			}
			twoBit[i] = TransportFeedbackSample{Received: true, ArrivalUS: arrival}
		}
		packets, err = BuildTransportFeedback(1, 2, 0, 0, twoBit, 65532)
		c.Check(err == nil, "two-bit case: %v", err)
		if err == nil && len(packets) == 1 {
			exp := structExpand(t, packets[0])
			c.Check(exp.count == 7, "two-bit count %d", exp.count)
			c.Check(equalKinds(exp.chunkKinds, []fbChunkKind{fbChunkTwoBit}), "two-bit chunks %v", exp.chunkKinds)
		}

		// 6. Dense mixed states: far fewer chunks than statuses, and the
		// minimal count matches the independent enumeration.
		mixed := make([]TransportFeedbackSample, 60)
		current := int64(0)
		for i := range mixed {
			if i%5 == 3 {
				continue
			}
			current += 250
			mixed[i] = TransportFeedbackSample{Received: true, ArrivalUS: current}
		}
		packets, err = BuildTransportFeedback(1, 2, 0, 0, mixed, 65532)
		c.Check(err == nil, "mixed error: %v", err)
		covered := 0
		var kinds []fbChunkKind
		for _, p := range packets {
			exp := structExpand(t, p)
			covered += exp.count
			kinds = append(kinds, exp.chunkKinds...)
		}
		c.Check(covered == 60, "mixed covered %d", covered)
		c.Check(len(kinds) < 60, "mixed used %d chunks for 60 statuses", len(kinds))
		// Score the whole 60-symbol stream with the independent brute force.
		wantKinds := bruteEndMin(symbolsOf(mixed), 60)
		c.Check(len(kinds) == wantKinds, "mixed chunks %d want %d", len(kinds), wantKinds)

		// 7. Sequence number and feedback count wraparound across packets.
		wrapSamples := make([]TransportFeedbackSample, 131072)
		packets, err = BuildTransportFeedback(1, 2, 0xFFF0, 0xFE, wrapSamples, 1000)
		c.Check(err == nil, "wrap error: %v", err)
		c.Check(len(packets) > 1, "wrap produced %d packets", len(packets))
		seq16 := uint16(0xFFF0)
		total := 0
		for i, p := range packets {
			exp := structExpand(t, p)
			c.Check(exp.base == seq16, "packet %d base %d want %d", i, exp.base, seq16)
			c.Check(exp.fbCount == uint8(0xFE+i), "packet %d fb count %d want %d", i, exp.fbCount, uint8(0xFE+i))
			c.Check(exp.count > 0, "packet %d empty", i)
			seq16 += uint16(exp.count)
			total += exp.count
		}
		c.Check(total == len(wrapSamples), "wrap covered %d want %d", total, len(wrapSamples))
		c.Check(len(packets) == 3, "wrap packets %d want 3", len(packets))
		c.Check(packets[len(packets)-1][19] == 0x00, "feedback count did not wrap: %x", packets[len(packets)-1][19])

		// 8. 24-bit reference clock wraparound: truncate on the wire, keep the
		// untruncated clock for the first delta.
		refWrap := []TransportFeedbackSample{
			{Received: true, ArrivalUS: (int64(1) << 24) * 64000},
			{Received: true, ArrivalUS: (int64(1)<<24)*64000 + 250},
			{Received: true, ArrivalUS: (int64(1)<<24)*64000 + 2*250},
		}
		packets, err = BuildTransportFeedback(1, 2, 7, 0, refWrap, 65532)
		c.Check(err == nil, "reference wrap error: %v", err)
		c.Check(len(packets) == 1, "reference wrap packets %d", len(packets))
		if len(packets) == 1 {
			exp := structExpand(t, packets[0])
			c.Check(exp.reference == 0, "truncated reference %d", exp.reference)
			c.Check(equalDeltas(exp.deltas, []int64{0, 250, 250}), "reference wrap deltas %v", exp.deltas)
		}

		// 9. A delta outside the large range forces a split before that
		// sample; the new packet re-establishes the reference clock.
		far := []TransportFeedbackSample{
			{Received: true, ArrivalUS: 100000},
			{Received: true, ArrivalUS: 100000 + 32767*250},
			{Received: true, ArrivalUS: 100000 + 32767*250 + 32768*250},
			{Received: true, ArrivalUS: 100000 + 32767*250 + 32768*250 + 250},
		}
		packets, err = BuildTransportFeedback(1, 2, 0, 0, far, 65532)
		c.Check(err == nil, "far delta error: %v", err)
		c.Check(len(packets) == 2, "far delta packets %d", len(packets))
		if len(packets) == 2 {
			first := structExpand(t, packets[0])
			second := structExpand(t, packets[1])
			c.Check(first.count == 2, "far first count %d", first.count)
			c.Check(second.count == 2, "far second count %d", second.count)
			c.Check(second.base == 2, "far second base %d", second.base)
			c.Check(second.reference == uint32((100000+32767*250+32768*250)/64000), "far second reference %d", second.reference)
			c.Check(second.deltas[0] == 35750, "far second first delta %d", second.deltas[0])
			c.Check(equalDeltas(first.deltas, []int64{36000, 32767 * 250}), "far first deltas %v", first.deltas)
		}

		// 10. All-lost packet uses a zero reference clock and no deltas.
		allLost := make([]TransportFeedbackSample, 5)
		packets, err = BuildTransportFeedback(1, 2, 0, 0, allLost, 65532)
		c.Check(err == nil, "all lost error: %v", err)
		if err == nil {
			c.Check(len(packets) == 1, "all lost packets %d", len(packets))
			if len(packets) == 1 {
				exp := structExpand(t, packets[0])
				c.Check(exp.reference == 0 && len(exp.deltas) == 0,
					"all lost reference %d deltas %d", exp.reference, len(exp.deltas))
			}
		}
	})
}

func TestFeedbackBudget(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Empty input is valid and yields no packets.
		packets, err := BuildTransportFeedback(1, 2, 0, 0, nil, 24)
		c.Check(err == nil && len(packets) == 0, "empty input: err %v packets %d", err, len(packets))

		// Budget bounds.
		for _, budget := range []int{-1, 0, 23, 65533, 1 << 20} {
			_, err := BuildTransportFeedback(1, 2, 0, 0, nil, budget)
			c.Check(err != nil, "budget %d accepted", budget)
		}
		for _, budget := range []int{24, 25, 65531, 65532} {
			_, err := BuildTransportFeedback(1, 2, 0, 0, nil, budget)
			c.Check(err == nil, "budget %d rejected: %v", budget, err)
		}

		// Sample count bound.
		tooMany := make([]TransportFeedbackSample, 131073)
		_, err = BuildTransportFeedback(1, 2, 0, 0, tooMany, 65532)
		c.Check(err != nil, "131073 samples accepted")
		limit := make([]TransportFeedbackSample, 131072)
		_, err = BuildTransportFeedback(1, 2, 0, 0, limit, 65532)
		c.Check(err == nil, "131072 samples rejected: %v", err)

		// Invalid arrival times are rejected; Received=false ignores the time.
		for _, bad := range []int64{-250, -1, 1 << 50, (1 << 50) + 250, 251, 125, 1} {
			badSamples := []TransportFeedbackSample{
				{Received: true, ArrivalUS: 0},
				{Received: true, ArrivalUS: bad},
				{Received: false},
			}
			_, err := BuildTransportFeedback(1, 2, 0, 0, badSamples, 65532)
			c.Check(err != nil, "arrival time %d accepted", bad)
		}
		ignored := []TransportFeedbackSample{
			{Received: true, ArrivalUS: 0},
			{Received: false, ArrivalUS: -1},
			{Received: false, ArrivalUS: 1 << 60},
			{Received: true, ArrivalUS: 250},
		}
		_, err = BuildTransportFeedback(1, 2, 0, 0, ignored, 65532)
		c.Check(err == nil, "unused arrival time rejected: %v", err)

		// Failed calls must not modify the input.
		original := []TransportFeedbackSample{
			{Received: true, ArrivalUS: 1000},
			{Received: false, ArrivalUS: 12345},
			{Received: true, ArrivalUS: 2001},
		}
		originalCopy := append([]TransportFeedbackSample(nil), original...)
		_, err = BuildTransportFeedback(1, 2, 0, 0, original, 65532)
		c.Check(err != nil, "expected invalid time error")
		c.Check(equalSamples(original, originalCopy), "input mutated on failure")
		_, err = BuildTransportFeedback(1, 2, 0, 0, original, 23)
		c.Check(err != nil, "expected budget error")
		c.Check(equalSamples(original, originalCopy), "input mutated on budget failure")
		// A successful call must not modify the input either.
		_, err = BuildTransportFeedback(1, 2, 0, 0, ignored, 65532)
		c.Check(err == nil && equalSamples(ignored, []TransportFeedbackSample{
			{Received: true, ArrivalUS: 0},
			{Received: false, ArrivalUS: -1},
			{Received: false, ArrivalUS: 1 << 60},
			{Received: true, ArrivalUS: 250},
		}), "input mutated on success")

		// Budget exactly enough vs one byte short.
		// One small delta: 20 + 2 + 1 = 23 -> 24 bytes.
		oneSmall := []TransportFeedbackSample{{Received: true, ArrivalUS: 0}}
		packets, err = BuildTransportFeedback(1, 2, 0, 0, oneSmall, 24)
		c.Check(err == nil && len(packets) == 1 && len(packets[0]) == 24, "one small at 24: err %v", err)

		// Six received samples: 20 + 2 (one vector) + 6 = 28 bytes exactly.
		sixRecv := make([]TransportFeedbackSample, 6)
		for i := range sixRecv {
			sixRecv[i] = TransportFeedbackSample{Received: true, ArrivalUS: int64(i) * 250}
		}
		packets, err = BuildTransportFeedback(1, 2, 0, 0, sixRecv, 28)
		c.Check(err == nil && len(packets) == 1 && len(packets[0]) == 28, "six at 28: err %v len %d", err, firstLen(packets))
		if err == nil && len(packets) == 1 {
			c.Check(bytes.Equal(packets[0][:4], []byte{0x8F, 0xCD, 0x00, 0x06}), "six header % x", packets[0][:4])
		}
		// One byte short must split; every packet stays within 27 bytes and
		// the union covers all six in order.
		packets, err = BuildTransportFeedback(1, 2, 0, 0, sixRecv, 27)
		c.Check(err == nil && len(packets) > 1, "six at 27: err %v packets %d", err, len(packets))
		if err == nil {
			total := 0
			for _, p := range packets {
				c.Check(len(p) <= 27, "packet length %d over budget", len(p))
				total += structExpand(t, p).count
			}
			c.Check(total == 6, "covered %d", total)
		}

		// 15 statuses need two chunks: 20 + 4 = 24 exactly.
		fifteen := make([]TransportFeedbackSample, 15)
		packets, err = BuildTransportFeedback(1, 2, 0, 0, fifteen, 24)
		c.Check(err == nil && len(packets) == 1 && len(packets[0]) == 24, "15 at 24: err %v", err)

		// 16 statuses with one received need 20+4+1 = 25 -> 28 bytes, so a
		// 24-byte budget splits after 14 statuses.
		sixteen := make([]TransportFeedbackSample, 16)
		sixteen[0] = TransportFeedbackSample{Received: true, ArrivalUS: 0}
		packets, err = BuildTransportFeedback(1, 2, 0, 0, sixteen, 24)
		c.Check(err == nil, "16 at 24: %v", err)
		if err == nil {
			c.Check(len(packets) == 2, "16 at 24 packets %d", len(packets))
			total := 0
			for _, p := range packets {
				c.Check(len(p) <= 24, "packet length %d over budget", len(p))
				total += structExpand(t, p).count
			}
			c.Check(total == 16, "covered %d", total)
			c.Check(structExpand(t, packets[0]).count == 14, "first prefix %d want 14", structExpand(t, packets[0]).count)
		}

		// A prefix whose minimal encoding temporarily needs an extra chunk
		// must not hide a longer fitting prefix: 8 statuses (one small delta
		// plus 7 lost) need two chunks and 28 bytes, while 14 statuses (one
		// small plus 13 lost) fit a single one-bit vector in 24 bytes.
		nonMonotonic := make([]TransportFeedbackSample, 14)
		nonMonotonic[0] = TransportFeedbackSample{Received: true, ArrivalUS: 0}
		packets, err = BuildTransportFeedback(1, 2, 0, 0, nonMonotonic, 24)
		c.Check(err == nil, "non-monotonic prefix: %v", err)
		if err == nil {
			c.Check(len(packets) == 1, "non-monotonic packets %d", len(packets))
			if len(packets) == 1 {
				c.Check(structExpand(t, packets[0]).count == 14, "non-monotonic prefix %d want 14", structExpand(t, packets[0]).count)
			}
		}

		// Padding accounting: 14 large deltas plus one small = 20+2+29 = 51
		// -> 52 bytes. Confirm the wire length and length field agree.
		padded := make([]TransportFeedbackSample, 15)
		for i := range padded {
			padded[i] = TransportFeedbackSample{Received: true, ArrivalUS: int64(i) * 2000 * 250}
		}
		packets, err = BuildTransportFeedback(1, 2, 0, 0, padded, 65532)
		c.Check(err == nil, "padding case: %v", err)
		if err == nil && len(packets) == 1 {
			raw := packets[0]
			c.Check(len(raw)%4 == 0, "packet not 4-byte aligned: %d", len(raw))
			c.Check(len(raw) == 56, "padding packet length %d want 56", len(raw))
			c.Check(raw[0]&0x20 != 0, "padding bit not set for %d bytes", len(raw))
			c.Check(int(raw[len(raw)-1]) == 3, "padding count %d want 3", raw[len(raw)-1])
			c.Check(int(raw[2])<<8|int(raw[3]) == len(raw)/4-1, "length field mismatch")
			c.Check(structExpand(t, raw).count == 15, "padding statuses")
		}
	})
}

func TestFeedbackTiming(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Independent brute-force cross-check over randomly generated inputs.
		rng := rand.New(rand.NewSource(20261114))
		for trial := 0; trial < 60; trial++ {
			n := 1 + rng.Intn(45)
			samples := make([]TransportFeedbackSample, n)
			current := int64(rng.Intn(1 << 20))
			current -= current % 250
			for i := range samples {
				if rng.Intn(4) == 0 {
					samples[i] = TransportFeedbackSample{Received: false, ArrivalUS: -1}
					continue
				}
				step := int64(rng.Intn(4)) * 250
				if rng.Intn(8) == 0 {
					step = int64(rng.Intn(400)) * 250
				}
				current += step
				samples[i] = TransportFeedbackSample{Received: true, ArrivalUS: current}
			}
			maxBytes := 24 + rng.Intn(200)
			packets, err := BuildTransportFeedback(5, 6, uint16(rng.Intn(65536)), uint8(rng.Intn(256)), samples, maxBytes)
			c.Check(err == nil, "trial %d error: %v", trial, err)
			if err != nil {
				continue
			}
			pos := 0
			for _, raw := range packets {
				c.Check(len(raw) <= maxBytes, "trial %d packet length %d", trial, len(raw))
				exp := structExpand(t, raw)
				wantCovered := bruteCovered(samples[pos:], maxBytes)
				c.Check(exp.count == wantCovered, "trial %d packet count %d want %d", trial, exp.count, wantCovered)
				wantChunks := bruteEndMin(exp.symbols, exp.count)
				c.Check(len(exp.chunkKinds) == wantChunks, "trial %d chunks %d want %d", trial, len(exp.chunkKinds), wantChunks)
				// Independent delta expansion from the input arrivals.
				var wantDeltas []int64
				var prev int64
				first := true
				for i := 0; i < exp.count; i++ {
					s := samples[pos+i]
					if !s.Received {
						continue
					}
					if first {
						wantDeltas = append(wantDeltas, s.ArrivalUS-(s.ArrivalUS/64000)*64000)
						first = false
					} else {
						wantDeltas = append(wantDeltas, s.ArrivalUS-prev)
					}
					prev = s.ArrivalUS
				}
				c.Check(equalDeltas(exp.deltas, wantDeltas), "trial %d deltas %v want %v", trial, exp.deltas, wantDeltas)
				pos += exp.count
			}
			c.Check(pos == n, "trial %d covered %d want %d", trial, pos, n)
		}

		// Negative deltas, including a large negative one.
		samples := []TransportFeedbackSample{
			{Received: true, ArrivalUS: 1000*64000 + 1000*250},
			{Received: true, ArrivalUS: 1000*64000 + 1000*250 - 1000*250},
			{Received: true, ArrivalUS: 1000*64000 + 1000*250 - 1000*250 + 32767*250},
			{Received: true, ArrivalUS: 1000*64000 + 1000*250 - 1000*250 + 32767*250 - 2*250},
		}
		packets, err := BuildTransportFeedback(1, 2, 0, 0, samples, 65532)
		c.Check(err == nil, "negative delta: %v", err)
		if err == nil {
			c.Check(len(packets) == 1, "negative delta packets %d", len(packets))
			if len(packets) == 1 {
				exp := structExpand(t, packets[0])
				c.Check(equalDeltas(exp.deltas, []int64{58000, -250000, 32767 * 250, -500}), "negative deltas %v", exp.deltas)
				c.Check(len(exp.chunkKinds) == bruteEndMin(exp.symbols, exp.count), "negative delta chunks not minimal")
				lib := expandPacket(t, packets[0])
				c.Check(equalDeltas(lib.deltas, exp.deltas), "library negative deltas %v", lib.deltas)
			}
		}

		// Out-of-range positive and negative deltas split and restart.
		for _, delta := range []int64{32768 * 250, -32769 * 250} {
			pair := []TransportFeedbackSample{
				{Received: true, ArrivalUS: 1000 * 1000},
				{Received: true, ArrivalUS: 1000*1000 + delta},
			}
			if pair[1].ArrivalUS < 0 {
				shift := int64(1<<40/250) * 250
				pair[1].ArrivalUS += shift
				pair[0].ArrivalUS += shift
			}
			packets, err := BuildTransportFeedback(1, 2, 0, 0, pair, 65532)
			c.Check(err == nil, "out-of-range delta %d: %v", delta, err)
			if err == nil {
				c.Check(len(packets) == 2, "out-of-range delta %d packets %d", delta, len(packets))
				if len(packets) == 2 {
					c.Check(structExpand(t, packets[0]).count == 1, "out-of-range first count")
					c.Check(structExpand(t, packets[1]).count == 1, "out-of-range second count")
					second := structExpand(t, packets[1])
					want := pair[1].ArrivalUS - (pair[1].ArrivalUS/64000)*64000
					c.Check(second.deltas[0] == want, "out-of-range restart delta %d want %d", second.deltas[0], want)
				}
			}
		}

		// 24-bit reference clock wrap; untruncated clock drives the delta.
		wrap := []TransportFeedbackSample{
			{Received: true, ArrivalUS: 0xFFFFFF*64000 + 32000},
			{Received: true, ArrivalUS: 0xFFFFFF*64000 + 32000 + 500},
		}
		packets, err = BuildTransportFeedback(1, 2, 0, 0, wrap, 65532)
		c.Check(err == nil, "clock wrap: %v", err)
		if err == nil && len(packets) == 1 {
			exp := structExpand(t, packets[0])
			c.Check(exp.reference == 0xFFFFFF, "reference %d", exp.reference)
			c.Check(equalDeltas(exp.deltas, []int64{32000, 500}), "clock wrap deltas %v", exp.deltas)
		}

		// Reference clock derived from the first received item even when
		// leading statuses are lost.
		leadingLost := []TransportFeedbackSample{
			{Received: false},
			{Received: false},
			{Received: true, ArrivalUS: 64000*5 + 3*250},
			{Received: true, ArrivalUS: 64000*5 + 4*250},
		}
		packets, err = BuildTransportFeedback(1, 2, 0, 0, leadingLost, 65532)
		c.Check(err == nil, "leading lost: %v", err)
		if err == nil && len(packets) == 1 {
			exp := structExpand(t, packets[0])
			c.Check(exp.reference == 5, "leading lost reference %d", exp.reference)
			c.Check(equalDeltas(exp.deltas, []int64{750, 250}), "leading lost deltas %v", exp.deltas)
		}

		// Long input scale check: dense mixed stream and long all-lost run.
		longMixed := make([]TransportFeedbackSample, 131072)
		current := int64(0)
		for i := range longMixed {
			if i%7 == 5 {
				longMixed[i] = TransportFeedbackSample{Received: false}
				continue
			}
			current += 250
			longMixed[i] = TransportFeedbackSample{Received: true, ArrivalUS: current}
		}
		start := time.Now()
		packets, err = BuildTransportFeedback(1, 2, 9, 0, longMixed, 1200)
		elapsed := time.Since(start)
		c.Check(err == nil, "long mixed: %v", err)
		c.Check(len(packets) > 0, "long mixed packets %d", len(packets))
		total := 0
		for _, p := range packets {
			total += structExpand(t, p).count
		}
		c.Check(total == len(longMixed), "long mixed covered %d", total)
		c.Check(elapsed < 2*time.Second, "long mixed took %v", elapsed)

		longLost := make([]TransportFeedbackSample, 131072)
		start = time.Now()
		packets, err = BuildTransportFeedback(1, 2, 123, 0, longLost, 1200)
		elapsed = time.Since(start)
		c.Check(err == nil, "long lost: %v", err)
		c.Check(len(packets) > 0, "long lost packets %d", len(packets))
		total = 0
		for _, p := range packets {
			total += structExpand(t, p).count
		}
		c.Check(total == len(longLost), "long lost covered %d", total)
		c.Check(elapsed < 2*time.Second, "long lost took %v", elapsed)
	})
}

// --- small assertion helpers ---

func symbolsOf(samples []TransportFeedbackSample) []uint16 {
	symbols := make([]uint16, 0, len(samples))
	var first bool
	var last int64
	for _, s := range samples {
		var symbol uint16
		if s.Received {
			if !first {
				first = true
				last = s.ArrivalUS
				symbol = TypeTCCPacketReceivedSmallDelta
			} else {
				units := (s.ArrivalUS - last) / 250
				last = s.ArrivalUS
				if units >= 0 && units <= 255 {
					symbol = TypeTCCPacketReceivedSmallDelta
				} else {
					symbol = TypeTCCPacketReceivedLargeDelta
				}
			}
		}
		symbols = append(symbols, symbol)
	}
	return symbols
}

func equalSymbols(a, b []uint16) bool {
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

func equalKinds(a, b []fbChunkKind) bool {
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

func equalDeltas(a, b []int64) bool {
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

func equalSamples(a, b []TransportFeedbackSample) bool {
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

func firstLen(packets [][]byte) int {
	if len(packets) == 0 {
		return -1
	}
	return len(packets[0])
}
