// SPDX-FileCopyrightText: 2023 The Pion community <https://pion.ly>
// SPDX-License-Identifier: MIT

package rtcp

import (
	"fmt"
	"math"
)

const (
	// Transport feedback packet layout constants.
	feedbackPacketBaseLength = headerLength + packetChunkOffset // header + fixed TWCC fields
	feedbackMaxStatusCount   = 65535
	feedbackMaxSamples       = 131072
	feedbackMinBudget        = 24
	feedbackMaxBudget        = 65532

	// Reference clock: arrival times are expressed in 64ms ticks on the wire.
	feedbackReferenceClockRate = 64000

	// Small delta in units of TypeTCCDeltaScaleFactor is 0..255, large delta is
	// -32768..32767.
	feedbackSmallDeltaMax = math.MaxUint8
	feedbackLargeDeltaMin = math.MinInt16
	feedbackLargeDeltaMax = math.MaxInt16

	// Status chunk capacities.
	feedbackRLEMaxRun     = 8191
	feedbackOneBitSymbols = 14
	feedbackTwoBitSymbols = 7

	// feedbackInitialCapacity bounds per-fragment allocations so that a tiny
	// budget producing many short fragments stays linear in the input size.
	feedbackInitialCapacity = 64

	// Arrival times must fit in [0, 2^50) microseconds.
	feedbackMaxArrivalUS = int64(1) << 50
)

// chunkKind identifies one of the three full-chunk encodings used by the
// minimal-chunk dynamic program.
const (
	chunkKindRLE int8 = iota
	chunkKindVec1
	chunkKindVec2
)

// TransportFeedbackSample describes the reception state of one transport-wide
// sequence number. The position of a sample in the slice passed to
// BuildTransportFeedback is its sequence offset.
//
// Received=false marks a lost packet; ArrivalUS is ignored in that case.
// Received=true requires ArrivalUS to be in [0, 2^50) microseconds and to be a
// multiple of 250. Arrival times do not have to increase with the sequence
// number.
type TransportFeedbackSample struct {
	Received  bool
	ArrivalUS int64
}

// chunkChoice describes the final (possibly partial) status vector selected for
// a fragment.
type chunkChoice struct {
	partial bool
	vecKind int8 // chunkKindVec1 or chunkKindVec2
	length  int  // number of real statuses covered by the partial vector
}

// BuildTransportFeedback builds one or more reduced-size RTCP transport-wide
// congestion control feedback packets from an ordered run of sequence numbers.
//
// samples[i] corresponds to sequence number baseSequence+i (wrapping at 65536).
// Lost packets are represented with Received=false but still occupy a slot.
//
// Each returned packet is produced independently by TransportLayerCC.Marshal and
// is at most maxBytes long. maxBytes must be in [24, 65532] and len(samples) may
// not exceed 131072. The input slice and its fields are never modified.
func BuildTransportFeedback(
	senderSSRC, mediaSSRC uint32,
	baseSequence uint16,
	firstFeedback uint8,
	samples []TransportFeedbackSample,
	maxBytes int,
) ([][]byte, error) {
	if maxBytes < feedbackMinBudget || maxBytes > feedbackMaxBudget {
		return nil, errFeedbackBudget
	}
	if len(samples) > feedbackMaxSamples {
		return nil, errFeedbackSampleCount
	}

	// Validate every arrival time before any fragmentation happens.
	for i := range samples {
		if !samples[i].Received {
			continue
		}
		arrival := samples[i].ArrivalUS
		if arrival < 0 || arrival >= feedbackMaxArrivalUS || arrival%TypeTCCDeltaScaleFactor != 0 {
			return nil, fmt.Errorf("%w at index %d: %d", errFeedbackArrivalTime, i, arrival)
		}
	}

	if len(samples) == 0 {
		return nil, nil
	}

	packets := make([][]byte, 0)
	for start := 0; start < len(samples); {
		raw, consumed, err := buildTransportFeedbackFragment(
			senderSSRC, mediaSSRC, baseSequence, firstFeedback,
			start, len(packets), samples, maxBytes,
		)
		if err != nil {
			return nil, err
		}
		if consumed <= 0 || consumed > len(samples)-start {
			return nil, errWrongMarshalSize
		}
		packets = append(packets, raw)
		start += consumed
	}

	return packets, nil
}

// buildTransportFeedbackFragment encodes the longest possible prefix starting
// at start, and returns the marshaled packet and the number of input samples it
// covers.
func buildTransportFeedbackFragment(
	senderSSRC, mediaSSRC uint32,
	baseSequence uint16,
	firstFeedback uint8,
	start, packetIndex int,
	samples []TransportFeedbackSample,
	maxBytes int,
) ([]byte, int, error) { //nolint:gocognit,cyclop,funlen
	maxStatuses := len(samples) - start
	if maxStatuses > feedbackMaxStatusCount {
		maxStatuses = feedbackMaxStatusCount
	}

	// Start with a small capacity and let append grow it. Pre-allocating the
	// whole remaining run would be quadratic when a small budget forces many
	// one-sample fragments.
	initialCap := maxStatuses
	if initialCap > feedbackInitialCapacity {
		initialCap = feedbackInitialCapacity
	}

	symbols := make([]uint16, 0, initialCap)
	deltaUnits := make([]int64, 0, initialCap)
	h := make([]int32, 1, initialCap+1)
	kind := make([]int8, 1, initialCap+1)
	prev := make([]int32, 1, initialCap+1)
	deque := make([]int, 0, initialCap)
	dequeHead := 0

	var reference int64 // untruncated reference clock, in 64ms ticks
	var prevArrival int64
	hasReceived := false
	deltaBytes := 0
	nonLargeRun := 0
	accepted := 0

	for j := start; j < len(samples) && len(symbols) < feedbackMaxStatusCount; j++ {
		sample := samples[j]
		symbol := uint16(TypeTCCPacketNotReceived)
		units := int64(0)
		addedDeltaBytes := 0

		if sample.Received {
			if !hasReceived {
				reference = sample.ArrivalUS / feedbackReferenceClockRate
				prevArrival = reference * feedbackReferenceClockRate
			}

			units = (sample.ArrivalUS - prevArrival) / TypeTCCDeltaScaleFactor
			// A delta outside the large range ends the fragment before this
			// sample so that a new reference clock can be established.
			if hasReceived && (units < feedbackLargeDeltaMin || units > feedbackLargeDeltaMax) {
				break
			}

			if units >= 0 && units <= feedbackSmallDeltaMax {
				symbol = TypeTCCPacketReceivedSmallDelta
				addedDeltaBytes = 1
			} else {
				symbol = TypeTCCPacketReceivedLargeDelta
				addedDeltaBytes = 2
			}

			prevArrival = sample.ArrivalUS
			hasReceived = true
		}

		symbols = append(symbols, symbol)
		deltaUnits = append(deltaUnits, units)
		deltaBytes += addedDeltaBytes
		if symbol == TypeTCCPacketReceivedLargeDelta {
			nonLargeRun = 0
		} else {
			nonLargeRun++
		}

		m := len(symbols)
		h, kind, prev, deque, dequeHead = feedbackAppendChunkDP(
			symbols, m, nonLargeRun, h, kind, prev, deque, dequeHead,
		)
		best, _ := feedbackMinChunks(m, h, nonLargeRun)

		size := alignFeedbackSize(feedbackPacketBaseLength + 2*int(best) + deltaBytes)
		if size > maxBytes {
			// The candidate does not fit: drop it and end the fragment.
			symbols = symbols[:m-1]
			deltaUnits = deltaUnits[:m-1]
			deltaBytes -= addedDeltaBytes
			break
		}

		accepted = m
	}

	if accepted == 0 {
		return nil, 0, errWrongMarshalSize
	}

	_, choice := feedbackMinChunks(accepted, h, feedbackTrailingNonLarge(symbols, accepted))
	chunks := feedbackReconstructChunks(symbols, kind, prev, choice)

	recvDeltas := make([]*RecvDelta, 0, accepted)
	for i := 0; i < accepted; i++ {
		if symbols[i] == TypeTCCPacketReceivedSmallDelta || symbols[i] == TypeTCCPacketReceivedLargeDelta {
			recvDeltas = append(recvDeltas, &RecvDelta{
				Type:  symbols[i],
				Delta: deltaUnits[i] * TypeTCCDeltaScaleFactor,
			})
		}
	}

	packet := &TransportLayerCC{
		SenderSSRC:         senderSSRC,
		MediaSSRC:          mediaSSRC,
		BaseSequenceNumber: uint16(uint32(baseSequence) + uint32(start)),
		PacketStatusCount:  uint16(accepted),
		ReferenceTime:      uint32(reference) & 0x00FFFFFF,
		FbPktCount:         uint8(int(firstFeedback) + packetIndex),
		PacketChunks:       chunks,
		RecvDeltas:         recvDeltas,
	}

	size := packet.MarshalSize()
	packet.Header = Header{
		Padding: size != int(packet.packetLen()),
		Count:   FormatTCC,
		Type:    TypeTransportSpecificFeedback,
		Length:  uint16(size/4 - 1),
	}

	raw, err := packet.Marshal()
	if err != nil {
		return nil, 0, err
	}
	if len(raw) != size || len(raw) > maxBytes {
		return nil, 0, errWrongMarshalSize
	}

	return raw, accepted, nil
}

// feedbackAppendChunkDP computes the minimal number of full chunks needed to
// cover the first m statuses and appends it to the DP tables. All chunks used by
// h[] are exactly full: RLE chunks have their exact run length and status
// vectors hold 14 or 7 symbols.
//
// h[m] is derived online from h[m-1] (RLE), h[m-14] (one-bit vector) and h[m-7]
// (two-bit vector). A monotonic deque keeps the minimum h over the current run,
// so long runs are processed in amortized constant time.
func feedbackAppendChunkDP(
	symbols []uint16,
	m, nonLargeRun int,
	h []int32, kind []int8, prev []int32,
	deque []int, dequeHead int,
) ([]int32, []int8, []int32, []int, int) {
	if m >= 2 && symbols[m-1] != symbols[m-2] {
		deque = deque[:0]
		dequeHead = 0
	}

	value := h[m-1]
	for len(deque) > dequeHead && h[deque[len(deque)-1]] >= value {
		deque = deque[:len(deque)-1]
	}
	deque = append(deque, m-1)

	minIndex := m - feedbackRLEMaxRun
	for dequeHead < len(deque) && deque[dequeHead] < minIndex {
		dequeHead++
	}

	best := int32(math.MaxInt32)
	bestKind := chunkKindRLE
	bestPrev := int32(-1)

	if dequeHead < len(deque) {
		s := deque[dequeHead]
		best = h[s] + 1
		bestKind = chunkKindRLE
		bestPrev = int32(s)
	}

	if m >= feedbackOneBitSymbols && nonLargeRun >= feedbackOneBitSymbols {
		if candidate := h[m-feedbackOneBitSymbols] + 1; candidate < best {
			best = candidate
			bestKind = chunkKindVec1
			bestPrev = int32(m - feedbackOneBitSymbols)
		}
	}

	if m >= feedbackTwoBitSymbols {
		if candidate := h[m-feedbackTwoBitSymbols] + 1; candidate < best {
			best = candidate
			bestKind = chunkKindVec2
			bestPrev = int32(m - feedbackTwoBitSymbols)
		}
	}

	h = append(h, best)
	kind = append(kind, bestKind)
	prev = append(prev, bestPrev)

	return h, kind, prev, deque, dequeHead
}

// feedbackMinChunks returns the minimal number of chunks for the first m
// statuses, allowing the final chunk to be a partial status vector. The returned
// choice describes that final partial vector when one is used.
func feedbackMinChunks(
	m int,
	h []int32,
	nonLargeRun int,
) (int32, chunkChoice) {
	best := h[m]
	choice := chunkChoice{}

	// Two-bit vectors can hold any of our three status symbols.
	limit := feedbackTwoBitSymbols
	if m < limit {
		limit = m
	}
	for k := 1; k <= limit; k++ {
		if candidate := h[m-k] + 1; candidate < best {
			best = candidate
			choice = chunkChoice{partial: true, vecKind: chunkKindVec2, length: k}
		}
	}

	// One-bit vectors can only hold not-received and small-delta statuses.
	limit = feedbackOneBitSymbols
	if m < limit {
		limit = m
	}
	if nonLargeRun < limit {
		limit = nonLargeRun
	}
	for k := 1; k <= limit; k++ {
		if candidate := h[m-k] + 1; candidate < best {
			best = candidate
			choice = chunkChoice{partial: true, vecKind: chunkKindVec1, length: k}
		}
	}

	return best, choice
}

// feedbackTrailingNonLarge returns the number of trailing statuses that are not
// large deltas.
func feedbackTrailingNonLarge(symbols []uint16, length int) int {
	count := 0
	for i := length - 1; i >= 0; i-- {
		if symbols[i] == TypeTCCPacketReceivedLargeDelta {
			break
		}
		count++
	}
	return count
}

// feedbackReconstructChunks materializes the minimal chunk sequence selected by
// the dynamic program.
func feedbackReconstructChunks(
	symbols []uint16,
	kind []int8,
	prev []int32,
	choice chunkChoice,
) []PacketStatusChunk {
	reversed := make([]PacketStatusChunk, 0, len(symbols)/feedbackTwoBitSymbols+1)

	pos := len(symbols)
	if choice.partial {
		start := pos - choice.length
		reversed = append(reversed, feedbackNewVectorChunk(symbols[start:pos], choice.vecKind))
		pos = start
	}

	for pos > 0 {
		start := int(prev[pos])
		switch kind[pos] {
		case chunkKindRLE:
			reversed = append(reversed, &RunLengthChunk{
				Type:               TypeTCCRunLengthChunk,
				PacketStatusSymbol: symbols[start],
				RunLength:          uint16(pos - start),
			})
		case chunkKindVec1:
			reversed = append(reversed, feedbackNewVectorChunk(symbols[start:pos], chunkKindVec1))
		case chunkKindVec2:
			reversed = append(reversed, feedbackNewVectorChunk(symbols[start:pos], chunkKindVec2))
		}
		pos = start
	}

	chunks := make([]PacketStatusChunk, len(reversed))
	for i := range reversed {
		chunks[len(reversed)-1-i] = reversed[i]
	}
	return chunks
}

// feedbackNewVectorChunk builds a status vector chunk, padding unused symbols
// with not-received.
func feedbackNewVectorChunk(symbols []uint16, vecKind int8) *StatusVectorChunk {
	symbolSize := uint16(TypeTCCSymbolSizeTwoBit)
	length := feedbackTwoBitSymbols
	if vecKind == chunkKindVec1 {
		symbolSize = TypeTCCSymbolSizeOneBit
		length = feedbackOneBitSymbols
	}

	list := make([]uint16, length)
	copy(list, symbols)
	for i := len(symbols); i < length; i++ {
		list[i] = TypeTCCPacketNotReceived
	}

	return &StatusVectorChunk{
		Type:       TypeTCCStatusVectorChunk,
		SymbolSize: symbolSize,
		SymbolList: list,
	}
}

// alignFeedbackSize rounds a packet length up to the RTCP 32-bit boundary.
func alignFeedbackSize(n int) int {
	return (n + 3) &^ 3
}
