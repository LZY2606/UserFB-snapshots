// SPDX-FileCopyrightText: 2023 The Pion community <https://pion.ly>
// SPDX-License-Identifier: MIT

package rtcp

import (
	"bytes"
	"math/rand"
	"reflect"
	"testing"
	"time"

	"github.com/pion/rtcp/task_support/taskcheck"
)

// TestFeedbackEncoding checks that generated packets are byte-correct, that the
// status chunks expand back to the intended reception pattern, and that delta
// values reproduce the input arrival times. Chunk minimality is cross-checked
// with an independent exhaustive solver.
func TestFeedbackEncoding(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// A hand-computed packet: sequence 100, fb count 7, statuses
		// small, lost, small, large -> one two-bit status vector.
		samples := []TransportFeedbackSample{
			{Received: true, ArrivalUS: 0},
			{Received: false},
			{Received: true, ArrivalUS: 500},
			{Received: true, ArrivalUS: 1000000},
		}
		packets, err := BuildTransportFeedback(0x11111111, 0x22222222, 100, 7, samples, 28)
		if !(err == nil) {
			c.Check(err == nil, "build failed: %v", err)
			return
		}
		if !(len(packets) == 1) {
			c.Check(len(packets) == 1, "expected one packet, got %d", len(packets))
			return
		}
		want := []byte{
			0xAF, 0xCD, 0x00, 0x06, // V=2 P=1 RC=15 PT=205 length=6
			0x11, 0x11, 0x11, 0x11, // sender SSRC
			0x22, 0x22, 0x22, 0x22, // media SSRC
			0x00, 0x64, 0x00, 0x04, // base seq 100, status count 4
			0x00, 0x00, 0x00, 0x07, // reference time 0, fb count 7
			0xD1, 0x80, // two-bit vector [1 0 1 2 0 0 0]
			0x00, 0x02, 0x0F, 0x9E, // small 0, small 2, large 3998
			0x00, 0x02, // two padding octets
		}
		c.Check(bytes.Equal(packets[0], want), "hand-written packet mismatch:\n got % X\nwant % X", packets[0], want)

		// Interleaved small/large/lost traffic: decode with the expander and
		// compare both statuses and arrival times.
		interleaved := []TransportFeedbackSample{
			{Received: true, ArrivalUS: 0},
			{Received: true, ArrivalUS: 250},
			{Received: false},
			{Received: true, ArrivalUS: 26000},
			{Received: true, ArrivalUS: 26000 + 250},
			{Received: false},
			{Received: false},
			{Received: true, ArrivalUS: 26000 + 250 + 100000},
			{Received: true, ArrivalUS: 26000 + 250 + 100000 + 500000},
		}
		packets, err = BuildTransportFeedback(7, 8, 0, 0, interleaved, 65532)
		if err == nil {
			c.Check(err == nil, "interleaved build failed: %v", err)
			p := tfCheckEnvelope(c, packets[0], 65532)
			if p != nil {
				covered := int(p.PacketStatusCount)
				c.Check(covered == len(interleaved), "covered %d want %d", covered, len(interleaved))
				sym, _, _, _ := tfEncodePrefix(interleaved)
				got := tfExpandChunks(p.PacketChunks)
				if len(got) >= covered {
					c.Check(len(got) >= covered, "chunk expansion short: %d < %d", len(got), covered)
					c.Check(reflect.DeepEqual(got[:covered], sym), "expanded statuses %v want %v", got[:covered], sym)
				}
				c.Check(len(p.PacketChunks) == tfBruteMinChunks(sym), "chunks %d want minimal %d", len(p.PacketChunks), tfBruteMinChunks(sym))
				wantArrivals := tfArrivalsFromPacket(p, sym)
				gotArrivals := tfExpectedArrivals(interleaved)
				c.Check(reflect.DeepEqual(wantArrivals, gotArrivals), "arrivals %v want %v", wantArrivals, gotArrivals)
			}
		}

		// An 8191-long identical run must be a single RLE chunk, not 8191
		// one-item chunks.
		longRun := make([]TransportFeedbackSample, 8191)
		packets, err = BuildTransportFeedback(1, 2, 0, 0, longRun, 65532)
		if err == nil {
			c.Check(err == nil, "long run build failed: %v", err)
			p := tfCheckEnvelope(c, packets[0], 65532)
			if p != nil {
				c.Check(len(p.PacketChunks) == 1, "8191-run chunks %d want 1", len(p.PacketChunks))
				if len(p.PacketChunks) == 1 {
					c.Check(len(p.PacketChunks) == 1, "need one chunk")
					rl, ok := p.PacketChunks[0].(*RunLengthChunk)
					c.Check(ok, "expected run-length chunk, got %T", p.PacketChunks[0])
					if ok {
						c.Check(rl.RunLength == 8191 && rl.PacketStatusSymbol == TypeTCCPacketNotReceived,
							"rle run length %d symbol %d", rl.RunLength, rl.PacketStatusSymbol)
					}
				}
			}
		}

		// 14 alternating statuses are cheapest as one full one-bit vector.
		alternating := make([]TransportFeedbackSample, 14)
		arrival := int64(0)
		for i := 0; i < 14; i += 2 {
			alternating[i] = TransportFeedbackSample{Received: true, ArrivalUS: arrival}
			arrival += 250
		}
		packets, err = BuildTransportFeedback(1, 2, 0, 0, alternating, 65532)
		if err == nil {
			c.Check(err == nil, "alternating build failed: %v", err)
			p := tfCheckEnvelope(c, packets[0], 65532)
			if p != nil {
				c.Check(len(p.PacketChunks) == 1, "alternating chunks %d want 1", len(p.PacketChunks))
			}
			if p != nil && len(p.PacketChunks) == 1 {
				sv, ok := p.PacketChunks[0].(*StatusVectorChunk)
				c.Check(ok, "expected status vector, got %T", p.PacketChunks[0])
				if ok {
					c.Check(sv.SymbolSize == TypeTCCSymbolSizeOneBit, "symbol size %d want one-bit", sv.SymbolSize)
					c.Check(len(sv.SymbolList) == 14, "symbol list len %d", len(sv.SymbolList))
				}
			}
		}

		// A status vector containing a large delta must use two-bit symbols.
		withLarge := []TransportFeedbackSample{
			{Received: true, ArrivalUS: 0},
			{Received: true, ArrivalUS: 100000},
			{Received: false}, {Received: false}, {Received: false}, {Received: false}, {Received: false},
		}
		packets, err = BuildTransportFeedback(1, 2, 0, 0, withLarge, 65532)
		if err == nil {
			c.Check(err == nil, "large-vector build failed: %v", err)
			p := tfCheckEnvelope(c, packets[0], 65532)
			if p != nil {
				c.Check(len(p.PacketChunks) == 1, "large-vector chunks %d want 1", len(p.PacketChunks))
			}
			if p != nil && len(p.PacketChunks) == 1 {
				sv, ok := p.PacketChunks[0].(*StatusVectorChunk)
				c.Check(ok, "expected status vector, got %T", p.PacketChunks[0])
				if ok {
					c.Check(sv.SymbolSize == TypeTCCSymbolSizeTwoBit, "symbol size %d want two-bit", sv.SymbolSize)
				}
			}
		}

		// Sequence-number and feedback-count wraparound across fragments.
		wrapSamples := make([]TransportFeedbackSample, 5)
		for i := range wrapSamples {
			wrapSamples[i] = TransportFeedbackSample{Received: true, ArrivalUS: int64(i) * 250}
		}
		packets, err = BuildTransportFeedback(9, 10, 65534, 254, wrapSamples, 24)
		if !(err == nil) {
			c.Check(err == nil, "wrap build failed: %v", err)
			return
		}
		c.Check(len(packets) == 3, "wrap packets %d want 3", len(packets))
		pos := 0
		for i, raw := range packets {
			p := tfCheckEnvelope(c, raw, 24)
			if p == nil {
				continue
			}
			c.Check(p.BaseSequenceNumber == uint16(65534+pos), "packet %d base %d want %d", i, p.BaseSequenceNumber, uint16(65534+pos))
			c.Check(p.FbPktCount == uint8(254+i), "packet %d fb count %d want %d", i, p.FbPktCount, uint8(254+i))
			pos += int(p.PacketStatusCount)
		}
		c.Check(pos == len(wrapSamples), "wrap covered %d want %d", pos, len(wrapSamples))

		// Negative delta: a later sequence number may arrive earlier.
		negative := []TransportFeedbackSample{
			{Received: true, ArrivalUS: 1000000},
			{Received: true, ArrivalUS: 50000},
			{Received: true, ArrivalUS: 250},
		}
		packets, err = BuildTransportFeedback(1, 2, 0, 0, negative, 65532)
		if err == nil {
			c.Check(err == nil, "negative build failed: %v", err)
			p := tfCheckEnvelope(c, packets[0], 65532)
			if p != nil {
				c.Check(len(p.RecvDeltas) == 3, "negative deltas %d want 3", len(p.RecvDeltas))
				if len(p.RecvDeltas) == 3 {
					c.Check(p.RecvDeltas[1].Delta == 50000-1000000, "second delta %d", p.RecvDeltas[1].Delta)
					c.Check(p.RecvDeltas[2].Delta == 250-50000, "third delta %d", p.RecvDeltas[2].Delta)
				}
				sym, _, _, _ := tfEncodePrefix(negative)
				c.Check(reflect.DeepEqual(tfArrivalsFromPacket(p, sym), tfExpectedArrivals(negative)), "negative arrivals mismatch")
			}
		}

		// 24-bit reference clock wraparound: the untruncated clock is the
		// baseline for the first delta, while only 24 bits go on the wire.
		base := int64(0x1000000+5) * 64000
		reference := []TransportFeedbackSample{
			{Received: true, ArrivalUS: base},
			{Received: true, ArrivalUS: base + 500},
		}
		packets, err = BuildTransportFeedback(1, 2, 0, 0, reference, 65532)
		if err == nil {
			c.Check(err == nil, "reference build failed: %v", err)
			p := tfCheckEnvelope(c, packets[0], 65532)
			if p != nil {
				c.Check(p.ReferenceTime == uint32(base/64000)&0x00FFFFFF, "reference time %#x want %#x",
					p.ReferenceTime, uint32(base/64000)&0x00FFFFFF)
				if len(p.RecvDeltas) == 2 {
					c.Check(len(p.RecvDeltas) == 2, "deltas %d want 2", len(p.RecvDeltas))
					c.Check(p.RecvDeltas[0].Delta == 0, "first delta %d want 0", p.RecvDeltas[0].Delta)
					c.Check(p.RecvDeltas[1].Delta == 500, "second delta %d want 500", p.RecvDeltas[1].Delta)
				}
			}
		}
	})
}

// TestFeedbackBudget checks the byte budget accounting: the longest prefix that
// fits, exact fits versus one byte short, coverage, and validation errors that
// leave the input untouched.
func TestFeedbackBudget(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		canonical := []TransportFeedbackSample{
			{Received: true, ArrivalUS: 0},
			{Received: false},
			{Received: true, ArrivalUS: 500},
			{Received: true, ArrivalUS: 1000000},
		}

		// Exactly enough for the whole fragment.
		packets, err := BuildTransportFeedback(1, 2, 0, 0, canonical, 28)
		if err == nil {
			c.Check(err == nil, "exact build failed: %v", err)
			c.Check(len(packets) == 1, "exact budget packets %d want 1", len(packets))
			if len(packets) == 1 {
				c.Check(len(packets[0]) == 28, "exact budget size %d want 28", len(packets[0]))
			}
		}

		// One byte short: the fragment must stop at three statuses and the
		// rest is carried in a second packet.
		packets, err = BuildTransportFeedback(1, 2, 0, 0, canonical, 27)
		if err == nil {
			c.Check(err == nil, "short build failed: %v", err)
			c.Check(len(packets) == 2, "one-short packets %d want 2", len(packets))
			covered := 0
			for _, raw := range packets {
				p := tfCheckEnvelope(c, raw, 27)
				if p != nil {
					covered += int(p.PacketStatusCount)
				}
			}
			c.Check(covered == len(canonical), "one-short covered %d want %d", covered, len(canonical))
			if len(packets) == 2 {
				p := tfCheckEnvelope(c, packets[0], 27)
				if p != nil {
					c.Check(int(p.PacketStatusCount) == 3, "first packet covered %d want 3", p.PacketStatusCount)
				}
			}
		}

		// Deterministic mixed inputs: every packet must be the brute-force
		// longest prefix, minimally chunked, and coverage must be exact.
		rng := rand.New(rand.NewSource(11))
		for trial := 0; trial < 120; trial++ {
			n := rng.Intn(50) + 1
			samples := tfRandomSamples(rng, n)
			budget := 24 + rng.Intn(40)
			packets, err := BuildTransportFeedback(3, 4, 65000, 100, samples, budget)
			if !(err == nil) {
				c.Check(err == nil, "trial %d build: %v", trial, err)
				continue
			}
			pos := 0
			for idx, raw := range packets {
				p := tfCheckEnvelope(c, raw, budget)
				if p == nil {
					continue
				}
				count := int(p.PacketStatusCount)
				wantN, wantRef := tfLongestPrefix(samples, pos, budget)
				c.Check(count == wantN, "trial %d pkt %d covered %d want %d", trial, idx, count, wantN)
				c.Check(p.ReferenceTime == uint32(wantRef)&0x00FFFFFF, "trial %d pkt %d reference %#x want %#x",
					trial, idx, p.ReferenceTime, uint32(wantRef)&0x00FFFFFF)
				c.Check(p.BaseSequenceNumber == uint16(65000+pos), "trial %d pkt %d base %d want %d",
					trial, idx, p.BaseSequenceNumber, uint16(65000+pos))
				sym, _, _, _ := tfEncodePrefix(samples[pos : pos+count])
				expanded := tfExpandChunks(p.PacketChunks)
				if len(expanded) >= count {
					c.Check(len(expanded) >= count, "trial %d pkt %d expansion short", trial, idx)
					c.Check(reflect.DeepEqual(expanded[:count], sym), "trial %d pkt %d statuses", trial, idx)
				}
				c.Check(len(p.PacketChunks) == tfBruteMinChunks(sym), "trial %d pkt %d chunks %d want %d",
					trial, idx, len(p.PacketChunks), tfBruteMinChunks(sym))
				pos += count
			}
			c.Check(pos == n, "trial %d covered %d want %d", trial, pos, n)
		}

		// Empty input with a valid budget yields zero packets.
		packets, err = BuildTransportFeedback(1, 2, 0, 0, nil, 24)
		c.Check(err == nil, "empty build: %v", err)
		c.Check(len(packets) == 0, "empty packets %d want 0", len(packets))

		// Invalid budgets and oversized input fail with nil results and do not
		// mutate the caller's slice.
		input := []TransportFeedbackSample{
			{Received: true, ArrivalUS: 250},
			{Received: false},
			{Received: true, ArrivalUS: 500},
		}
		original := append([]TransportFeedbackSample(nil), input...)
		for _, bad := range []int{0, 23, 65533, 1 << 20} {
			packets, err = BuildTransportFeedback(1, 2, 0, 0, input, bad)
			c.Check(err != nil, "budget %d should fail", bad)
			c.Check(packets == nil, "budget %d packets should be nil", bad)
		}
		huge := make([]TransportFeedbackSample, 131073)
		packets, err = BuildTransportFeedback(1, 2, 0, 0, huge, 1200)
		c.Check(err != nil, "oversized sample slice should fail")
		c.Check(packets == nil, "oversized packets should be nil")
		c.Check(reflect.DeepEqual(input, original), "input slice was modified by failures")
	})
}

// TestFeedbackTiming covers delta limits, split behaviour when a difference is
// out of range, arrival-time validation, and the algorithmic scaling on a long
// input.
func TestFeedbackTiming(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// 255 units is the largest small delta; 256 becomes a large delta.
		edge := []TransportFeedbackSample{
			{Received: true, ArrivalUS: 0},
			{Received: true, ArrivalUS: 255 * 250},
			{Received: true, ArrivalUS: 255*250 + 256*250},
		}
		packets, err := BuildTransportFeedback(1, 2, 0, 0, edge, 65532)
		if err == nil {
			c.Check(err == nil, "edge build: %v", err)
			p := tfCheckEnvelope(c, packets[0], 65532)
			if p != nil {
				c.Check(len(p.RecvDeltas) == 3, "edge deltas %d", len(p.RecvDeltas))
				if len(p.RecvDeltas) == 3 {
					c.Check(p.RecvDeltas[1].Type == TypeTCCPacketReceivedSmallDelta, "255 units should be small")
					c.Check(p.RecvDeltas[2].Type == TypeTCCPacketReceivedLargeDelta, "256 units should be large")
					c.Check(p.RecvDeltas[2].Delta == 256*250, "large delta %d", p.RecvDeltas[2].Delta)
				}
				sym, _, _, _ := tfEncodePrefix(edge)
				c.Check(reflect.DeepEqual(tfArrivalsFromPacket(p, sym), tfExpectedArrivals(edge)), "edge arrivals")
			}
		}

		// +32767 units fits; +32768 must split into a new fragment with a
		// fresh reference clock.
		fitsPositive := []TransportFeedbackSample{
			{Received: true, ArrivalUS: 0},
			{Received: true, ArrivalUS: 32767 * 250},
		}
		packets, err = BuildTransportFeedback(1, 2, 0, 0, fitsPositive, 65532)
		if err == nil {
			c.Check(err == nil, "+32767 build: %v", err)
			c.Check(len(packets) == 1, "+32767 packets %d want 1", len(packets))
			if len(packets) == 1 {
				p := tfCheckEnvelope(c, packets[0], 65532)
				if p != nil {
					c.Check(len(p.RecvDeltas) == 2, "+32767 deltas %d", len(p.RecvDeltas))
				}
				if p != nil && len(p.RecvDeltas) == 2 {
					c.Check(p.RecvDeltas[1].Type == TypeTCCPacketReceivedLargeDelta, "+32767 should be large")
					c.Check(p.RecvDeltas[1].Delta == 32767*250, "+32767 delta %d", p.RecvDeltas[1].Delta)
				}
			}
		}

		tooLarge := []TransportFeedbackSample{
			{Received: true, ArrivalUS: 0},
			{Received: true, ArrivalUS: 32768 * 250},
		}
		packets, err = BuildTransportFeedback(1, 2, 0, 0, tooLarge, 65532)
		if err == nil {
			c.Check(err == nil, "+32768 build: %v", err)
			c.Check(len(packets) == 2, "+32768 packets %d want 2", len(packets))
			covered := 0
			for _, raw := range packets {
				p := tfCheckEnvelope(c, raw, 65532)
				if p != nil {
					covered += int(p.PacketStatusCount)
				}
			}
			c.Check(covered == 2, "+32768 covered %d want 2", covered)
			if len(packets) == 2 {
				if p := tfCheckEnvelope(c, packets[1], 65532); p != nil {
					c.Check(p.ReferenceTime == uint32((32768*250)/64000), "+32768 re-reference %d", p.ReferenceTime)
				}
			}
		}

		// -32768 units fits; -32769 splits.
		fitsNegative := []TransportFeedbackSample{
			{Received: true, ArrivalUS: 32768 * 250},
			{Received: true, ArrivalUS: 0},
		}
		packets, err = BuildTransportFeedback(1, 2, 0, 0, fitsNegative, 65532)
		if err == nil {
			c.Check(err == nil, "-32768 build: %v", err)
			c.Check(len(packets) == 1, "-32768 packets %d want 1", len(packets))
			if len(packets) == 1 {
				p := tfCheckEnvelope(c, packets[0], 65532)
				if p != nil {
					c.Check(len(p.RecvDeltas) == 2, "-32768 deltas %d", len(p.RecvDeltas))
				}
				if p != nil && len(p.RecvDeltas) == 2 {
					c.Check(p.RecvDeltas[1].Delta == -32768*250, "-32768 delta %d", p.RecvDeltas[1].Delta)
				}
			}
		}

		tooNegative := []TransportFeedbackSample{
			{Received: true, ArrivalUS: 32769 * 250},
			{Received: true, ArrivalUS: 0},
		}
		packets, err = BuildTransportFeedback(1, 2, 0, 0, tooNegative, 65532)
		if err == nil {
			c.Check(err == nil, "-32769 build: %v", err)
			c.Check(len(packets) == 2, "-32769 packets %d want 2", len(packets))
		}

		// Illegal arrival times are rejected before any fragmentation, and
		// the input is left untouched.
		invalid := [][]TransportFeedbackSample{
			{{Received: true, ArrivalUS: -250}},
			{{Received: true, ArrivalUS: 251}},
			{{Received: true, ArrivalUS: 250}, {Received: true, ArrivalUS: 500 + 1}},
			{{Received: true, ArrivalUS: int64(1) << 50}},
			{{Received: true, ArrivalUS: (int64(1) << 50) + 250}},
		}
		for i, samples := range invalid {
			snapshot := append([]TransportFeedbackSample(nil), samples...)
			packets, err = BuildTransportFeedback(1, 2, 0, 0, samples, 1200)
			c.Check(err != nil, "invalid case %d should fail", i)
			c.Check(packets == nil, "invalid case %d should return nil", i)
			c.Check(reflect.DeepEqual(samples, snapshot), "invalid case %d modified input", i)
		}

		// The largest legal arrival time is accepted and keeps only 24 bits of
		// the reference clock on the wire.
		const maxValid = int64(1125899906842500)
		if maxValid%250 == 0 && maxValid < (int64(1)<<50) {
			packets, err = BuildTransportFeedback(1, 2, 0, 0, []TransportFeedbackSample{{Received: true, ArrivalUS: maxValid}}, 24)
			if err == nil {
				c.Check(err == nil, "max valid arrival build: %v", err)
				if len(packets) == 1 {
					c.Check(len(packets) == 1, "max valid packets %d", len(packets))
					if p := tfCheckEnvelope(c, packets[0], 24); p != nil {
						wantRef := uint32((maxValid / 64000) & 0x00FFFFFF)
						c.Check(p.ReferenceTime == wantRef, "max valid reference %#x want %#x", p.ReferenceTime, wantRef)
					}
				}
			}
		}

		// Long, dense mixed input: coverage, budget, and a coarse scaling bound
		// guard against quadratic behaviour.
		const longCount = 131072
		rng := rand.New(rand.NewSource(23))
		long := make([]TransportFeedbackSample, longCount)
		cur := int64(0)
		for i := range long {
			if rng.Intn(3) == 0 {
				continue
			}
			long[i].Received = true
			cur += int64(rng.Intn(200)) * 250
			long[i].ArrivalUS = cur
		}
		started := time.Now()
		packets, err = BuildTransportFeedback(1, 2, 0, 0, long, 1200)
		elapsed := time.Since(started)
		if err == nil {
			c.Check(err == nil, "long build: %v", err)
			covered := 0
			for _, raw := range packets {
				if p := tfCheckEnvelope(c, raw, 1200); p != nil {
					covered += int(p.PacketStatusCount)
				}
			}
			c.Check(covered == longCount, "long covered %d want %d", covered, longCount)
			c.Check(elapsed < 5*time.Second, "long input took %v", elapsed)
		}

		// A long all-loss run must also complete quickly and use RLE chunks.
		loss := make([]TransportFeedbackSample, longCount)
		started = time.Now()
		packets, err = BuildTransportFeedback(1, 2, 0, 0, loss, 65532)
		elapsed = time.Since(started)
		if err == nil {
			c.Check(err == nil, "long loss build: %v", err)
			covered := 0
			for _, raw := range packets {
				if p := tfCheckEnvelope(c, raw, 65532); p != nil {
					covered += int(p.PacketStatusCount)
				}
			}
			c.Check(covered == longCount, "long loss covered %d want %d", covered, longCount)
			c.Check(elapsed < 5*time.Second, "long loss input took %v", elapsed)
		}
	})
}

// tfRandomSamples builds a deterministic mix of received and lost packets with
// valid arrival times, including occasional earlier arrivals.
func tfRandomSamples(rng *rand.Rand, n int) []TransportFeedbackSample {
	samples := make([]TransportFeedbackSample, n)
	cur := int64(rng.Intn(8)) * 250
	steps := []int64{-2000, -250, 0, 250, 500, 64000, 250000}
	for i := range samples {
		if rng.Intn(4) == 0 {
			continue
		}
		samples[i].Received = true
		cur += steps[rng.Intn(len(steps))]
		if cur < 0 {
			cur = 0
		}
		samples[i].ArrivalUS = cur
	}
	return samples
}

// tfEncodePrefix independently derives the status symbols and delta byte count
// for a run of samples, following the wire rules but not the builder code.
func tfEncodePrefix(samples []TransportFeedbackSample) ([]uint16, int, int64, bool) {
	symbols := make([]uint16, len(samples))
	deltaBytes := 0
	var reference, prev int64
	hasReceived := false
	for i := range samples {
		if !samples[i].Received {
			continue
		}
		if !hasReceived {
			reference = samples[i].ArrivalUS / 64000
			prev = reference * 64000
		}
		units := (samples[i].ArrivalUS - prev) / 250
		if hasReceived && (units < -32768 || units > 32767) {
			return nil, 0, 0, false
		}
		if units >= 0 && units <= 255 {
			symbols[i] = TypeTCCPacketReceivedSmallDelta
			deltaBytes++
		} else {
			symbols[i] = TypeTCCPacketReceivedLargeDelta
			deltaBytes += 2
		}
		prev = samples[i].ArrivalUS
		hasReceived = true
	}
	return symbols, deltaBytes, reference, true
}

// tfLongestPrefix returns the longest prefix that fits, computed by trying every
// prefix and using the exhaustive chunk solver.
func tfLongestPrefix(samples []TransportFeedbackSample, start, maxBytes int) (int, int64) {
	maxN := len(samples) - start
	if maxN > 65535 {
		maxN = 65535
	}
	accepted := 0
	var reference int64
	for n := 1; n <= maxN; n++ {
		symbols, deltaBytes, ref, ok := tfEncodePrefix(samples[start : start+n])
		if !ok {
			break
		}
		size := 20 + 2*tfBruteMinChunks(symbols) + deltaBytes
		size = (size + 3) &^ 3
		if size > maxBytes {
			break
		}
		accepted = n
		reference = ref
	}
	return accepted, reference
}

// tfBruteMinChunks exhaustively finds the minimal number of status chunks for
// a symbol sequence. Only the final status vector may be partial.
func tfBruteMinChunks(symbols []uint16) int {
	m := len(symbols)
	const inf = 1 << 30
	memo := make([]int, m+1)
	for i := range memo {
		memo[i] = -1
	}
	var solve func(pos int) int
	solve = func(pos int) int {
		if pos == m {
			return 0
		}
		if memo[pos] >= 0 {
			return memo[pos]
		}
		best := inf
		for l := 1; l <= 8191 && pos+l <= m; l++ {
			if symbols[pos+l-1] != symbols[pos] {
				break
			}
			if v := 1 + solve(pos+l); v < best {
				best = v
			}
		}
		ok1 := true
		for l := 1; l <= 14 && pos+l <= m; l++ {
			if symbols[pos+l-1] > 1 {
				ok1 = false
			}
			if !ok1 {
				break
			}
			if l == 14 {
				if v := 1 + solve(pos+14); v < best {
					best = v
				}
			} else if pos+l == m {
				if best > 1 {
					best = 1
				}
			}
		}
		for l := 1; l <= 7 && pos+l <= m; l++ {
			if symbols[pos+l-1] > 2 {
				break
			}
			if l == 7 {
				if v := 1 + solve(pos+7); v < best {
					best = v
				}
			} else if pos+l == m {
				if best > 1 {
					best = 1
				}
			}
		}
		memo[pos] = best
		return best
	}
	return solve(0)
}

// tfExpandChunks flattens decoded chunks back into a status symbol sequence.
func tfExpandChunks(chunks []PacketStatusChunk) []uint16 {
	var out []uint16
	for _, chunk := range chunks {
		switch v := chunk.(type) {
		case *RunLengthChunk:
			for i := 0; i < int(v.RunLength); i++ {
				out = append(out, v.PacketStatusSymbol)
			}
		case *StatusVectorChunk:
			out = append(out, v.SymbolList...)
		}
	}
	return out
}

// tfArrivalsFromPacket reconstructs arrival times from the wire reference time
// and deltas. This is only meaningful while the reference clock fits in 24 bits.
func tfArrivalsFromPacket(p *TransportLayerCC, symbols []uint16) []int64 {
	var arrivals []int64
	deltaIndex := 0
	var prev int64
	hasReceived := false
	for _, symbol := range symbols {
		if symbol == TypeTCCPacketNotReceived {
			continue
		}
		if deltaIndex >= len(p.RecvDeltas) {
			return arrivals
		}
		delta := p.RecvDeltas[deltaIndex].Delta
		deltaIndex++
		if !hasReceived {
			prev = int64(p.ReferenceTime) * 64000
		}
		prev += delta
		arrivals = append(arrivals, prev)
		hasReceived = true
	}
	return arrivals
}

// tfExpectedArrivals collects the arrival times of all received samples.
func tfExpectedArrivals(samples []TransportFeedbackSample) []int64 {
	var arrivals []int64
	for _, sample := range samples {
		if sample.Received {
			arrivals = append(arrivals, sample.ArrivalUS)
		}
	}
	return arrivals
}

// tfCheckEnvelope validates the common packet invariants and returns the decoded
// packet (or nil after recording a failure).
func tfCheckEnvelope(c *taskcheck.C, raw []byte, maxBytes int) *TransportLayerCC {
	var p TransportLayerCC
	err := p.Unmarshal(raw)
	c.Check(err == nil, "unmarshal packet: %v", err)
	if err != nil {
		return nil
	}
	c.Check(len(raw) <= maxBytes, "packet size %d exceeds budget %d", len(raw), maxBytes)
	c.Check(len(raw)%4 == 0, "packet size %d is not 32-bit aligned", len(raw))
	c.Check(int(p.Header.Length+1)*4 == len(raw), "header length %d implies %d bytes, got %d", p.Header.Length, int(p.Header.Length+1)*4, len(raw))
	c.Check(p.Header.Count == FormatTCC && p.Header.Type == TypeTransportSpecificFeedback, "unexpected header %+v", p.Header)

	used := 20 + len(p.PacketChunks)*2
	for _, delta := range p.RecvDeltas {
		if delta.Type == TypeTCCPacketReceivedSmallDelta {
			used++
		} else {
			used += 2
		}
	}
	if p.Header.Padding {
		c.Check(raw[len(raw)-1] == byte(len(raw)-used), "padding byte %d, want %d", raw[len(raw)-1], len(raw)-used)
	} else {
		c.Check(used == len(raw), "padding bit clear but used %d of %d", used, len(raw))
	}
	return &p
}
