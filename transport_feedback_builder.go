// SPDX-FileCopyrightText: 2023 The Pion community <https://pion.ly>
// SPDX-License-Identifier: MIT

package rtcp

import (
	"errors"
	"fmt"
)

// Limits for BuildTransportFeedback.
const (
	maxTransportFeedbackSamples = 131072
	minTransportFeedbackBytes   = 24
	maxTransportFeedbackBytes   = 65532
	maxTransportFeedbackStatus  = 65535
	maxTWCCRunLength            = 8191
	twccReferenceClockUS        = 64000
	twccStatusVectorOneBit      = 14
	twccStatusVectorTwoBit      = 7
)

// Errors returned by BuildTransportFeedback.
var (
	errFeedbackBudget   = errors.New("rtcp: transport feedback budget must be in [24, 65532]")
	errFeedbackTooLarge = errors.New("rtcp: transport feedback accepts at most 131072 samples")
)

// TransportFeedbackSample is the arrival state of one transport sequence number.
//
// The slice index passed to BuildTransportFeedback is the offset from the base
// sequence number: index 0 is baseSequence, index 1 is baseSequence+1 and so on,
// wrapping at 16 bits. Every index occupies one packet status, including the
// ones that were not received.
//
// ArrivalUS is only used when Received is true. It is the arrival time in
// microseconds, must be in [0, 2^50), and must be a multiple of 250us (the
// transport-wide-cc delta scale).
type TransportFeedbackSample struct {
	Received  bool
	ArrivalUS int64
}

// BuildTransportFeedback encodes an ordered run of transport sequence arrival
// records into one or more reduced-size RTCP transport layer CC packets.
//
// Each returned byte slice is a complete, standalone reduced-size RTCP packet
// marshaled by TransportLayerCC.Marshal. Samples are covered contiguously and
// in order, starting at baseSequence. FbPktCount starts at firstFeedback and is
// incremented for every emitted packet (wrapping at 8 bits).
//
// Every packet uses the minimum number of 2-byte packet status chunks for the
// longest prefix that fits in maxBytes. The first received sample of a packet
// establishes its reference clock (arrival/64000, truncated to 24 bits on the
// wire); subsequent received samples use deltas relative to the previous
// received sample. If a delta does not fit the large-delta range the current
// packet is closed before that sample and a new reference clock is established.
//
// maxBytes counts the complete packet including the RTCP header, chunks,
// deltas and 4-byte alignment padding, and must be in [24, 65532]. The input
// is not modified, and is validated before any splitting happens.
func BuildTransportFeedback(
	senderSSRC, mediaSSRC uint32,
	baseSequence uint16,
	firstFeedback uint8,
	samples []TransportFeedbackSample,
	maxBytes int,
) ([][]byte, error) {
	if maxBytes < minTransportFeedbackBytes || maxBytes > maxTransportFeedbackBytes {
		return nil, errFeedbackBudget
	}
	if len(samples) > maxTransportFeedbackSamples {
		return nil, errFeedbackTooLarge
	}
	for i := range samples {
		if !samples[i].Received {
			continue
		}
		time := samples[i].ArrivalUS
		if time < 0 || time >= int64(1)<<50 || time%TypeTCCDeltaScaleFactor != 0 {
			return nil, fmt.Errorf(
				"rtcp: sample %d has invalid arrival time %d (want [0, 2^50) microsecond multiple of 250)",
				i, time,
			)
		}
	}

	if len(samples) == 0 {
		return [][]byte{}, nil
	}

	packets := make([][]byte, 0, 1+len(samples)/256)
	fbPktCount := firstFeedback
	for offset := 0; offset < len(samples); {
		raw, consumed, err := encodeTransportFeedbackPacket(
			senderSSRC,
			mediaSSRC,
			baseSequence+uint16(offset),
			fbPktCount,
			samples[offset:],
			maxBytes,
		)
		if err != nil {
			return nil, err
		}
		if consumed <= 0 {
			return nil, errors.New("rtcp: transport feedback made no progress")
		}
		packets = append(packets, raw)
		offset += consumed
		fbPktCount++
	}

	return packets, nil
}

// fbChunkKind identifies the wire representation chosen for a status chunk.
type fbChunkKind uint8

const (
	fbChunkRLE fbChunkKind = iota
	fbChunkOneBit
	fbChunkTwoBit
)

type fbChunkDesc struct {
	kind   fbChunkKind
	start  int
	length int
}

// fbIndexDeque keeps the candidate start indices for a run-length chunk.
// Candidates are pushed in increasing index order and the deque is monotonic on
// the cost, so the front always holds the cheapest candidate.
type fbIndexDeque struct {
	buf  []int32
	head int
}

func (d *fbIndexDeque) reset() {
	d.buf = d.buf[:0]
	d.head = 0
}

func (d *fbIndexDeque) empty() bool {
	return d.head >= len(d.buf)
}

func (d *fbIndexDeque) front() int32 {
	return d.buf[d.head]
}

func (d *fbIndexDeque) expire(minIndex int32) {
	for d.head < len(d.buf) && d.buf[d.head] < minIndex {
		d.head++
	}
}

func (d *fbIndexDeque) push(index int32, cost []int32) {
	for len(d.buf) > d.head && cost[d.buf[len(d.buf)-1]] >= cost[index] {
		d.buf = d.buf[:len(d.buf)-1]
	}
	d.buf = append(d.buf, index)
	if d.head > 4096 && d.head*2 > len(d.buf) {
		n := copy(d.buf, d.buf[d.head:])
		d.buf = d.buf[:n]
		d.head = 0
	}
}

const (
	fbChunkCostInf = int32(1 << 30)
	// RTCP header + sender/media SSRCs + base sequence/count + reference
	// time/fb packet count: everything before the first packet status chunk.
	fbFixedHeaderSize = headerLength + packetChunkOffset
)

// encodeTransportFeedbackPacket encodes the longest prefix of samples that fits
// in maxBytes and returns the marshaled packet together with the number of
// samples it covers.
func encodeTransportFeedbackPacket(
	senderSSRC, mediaSSRC uint32,
	baseSequence uint16,
	fbPktCount uint8,
	samples []TransportFeedbackSample,
	maxBytes int,
) ([]byte, int, error) {
	maxCount := len(samples)
	if maxCount > maxTransportFeedbackStatus {
		maxCount = maxTransportFeedbackStatus
	}

	// Dynamic programming over the packet prefix.
	//
	// symbols[i] is the wire symbol of status i: 0 = not received,
	// 1 = small delta, 2 = large delta.
	//
	// fullCost[i] is the minimum number of chunks for statuses [0,i) when
	// every vector chunk is completely filled (run-length chunks have no fill
	// requirement). Only the final chunk of a packet may be a partially filled
	// vector, so endCost[i] is the minimum number of chunks for [0,i) when that
	// allowance is applied to the last chunk.
	//
	// Full prefixes only need to be anchored at the start of the final chunk,
	// which keeps the recurrence local plus one sliding-window minimum for the
	// run-length candidates. This avoids enumerating every partition of every
	// prefix.
	initialCap := maxCount
	if initialCap > 1024 {
		initialCap = 1024
	}
	symbols := make([]uint8, 0, initialCap)
	deltaUnits := make([]int32, 0, initialCap)
	cumulativeDelta := make([]int32, 1, initialCap+1)
	fullCost := make([]int32, 1, initialCap+1)
	fullPrev := make([]int32, 1, initialCap+1)
	fullKind := make([]fbChunkKind, 1, initialCap+1)
	endCost := make([]int32, 1, initialCap+1)
	endPrev := make([]int32, 1, initialCap+1)
	endKind := make([]fbChunkKind, 1, initialCap+1)

	fullCost[0] = 0
	endCost[0] = 0

	var (
		firstReceived bool
		referenceUS   int64
		lastArrivalUS int64
		deque         fbIndexDeque
		runStart      int32
		tailEncodable int
		covered       int
	)

	for i := 1; i <= maxCount; i++ {
		pos := i - 1
		sample := &samples[pos]

		var symbol uint8
		if sample.Received {
			time := sample.ArrivalUS
			if !firstReceived {
				firstReceived = true
				referenceUS = time / twccReferenceClockUS
				deltaUnits = append(deltaUnits, int32((time-referenceUS*twccReferenceClockUS)/TypeTCCDeltaScaleFactor))
				symbol = uint8(TypeTCCPacketReceivedSmallDelta)
			} else {
				units := (time - lastArrivalUS) / TypeTCCDeltaScaleFactor
				if units < -32768 || units > 32767 {
					// The delta is outside the large-delta range: close the
					// packet in front of this sample. The next packet starts a
					// new reference clock here.
					break
				}
				deltaUnits = append(deltaUnits, int32(units))
				if units >= 0 && units <= 255 {
					symbol = uint8(TypeTCCPacketReceivedSmallDelta)
				} else {
					symbol = uint8(TypeTCCPacketReceivedLargeDelta)
				}
			}
			lastArrivalUS = time
		} else {
			deltaUnits = append(deltaUnits, 0)
		}
		symbols = append(symbols, symbol)

		switch symbol {
		case uint8(TypeTCCPacketReceivedSmallDelta):
			cumulativeDelta = append(cumulativeDelta, cumulativeDelta[i-1]+1)
		case uint8(TypeTCCPacketReceivedLargeDelta):
			cumulativeDelta = append(cumulativeDelta, cumulativeDelta[i-1]+2)
		default:
			cumulativeDelta = append(cumulativeDelta, cumulativeDelta[i-1])
		}

		// Maintain the window of run-length chunk candidates: a chunk may
		// only cover a run of equal symbols, at most 8191 statuses long.
		if pos == 0 || symbols[pos] != symbols[pos-1] {
			runStart = int32(pos)
			deque.reset()
		}
		if symbols[pos] <= uint8(TypeTCCPacketReceivedSmallDelta) {
			tailEncodable++
		} else {
			tailEncodable = 0
		}
		deque.push(int32(pos), fullCost)
		minCandidate := int32(i) - maxTWCCRunLength
		if runStart > minCandidate {
			minCandidate = runStart
		}
		deque.expire(minCandidate)

		// fullCost[i]: every chunk is full (or a run-length chunk).
		bestCost := fbChunkCostInf
		bestPrev := int32(0)
		bestKind := fbChunkRLE

		if i >= twccStatusVectorTwoBit {
			if cost := fullCost[i-twccStatusVectorTwoBit] + 1; cost < bestCost {
				bestCost = cost
				bestPrev = int32(i - twccStatusVectorTwoBit)
				bestKind = fbChunkTwoBit
			}
		}
		if i >= twccStatusVectorOneBit && tailEncodable >= twccStatusVectorOneBit {
			if cost := fullCost[i-twccStatusVectorOneBit] + 1; cost < bestCost {
				bestCost = cost
				bestPrev = int32(i - twccStatusVectorOneBit)
				bestKind = fbChunkOneBit
			}
		}
		if !deque.empty() {
			if cost := fullCost[deque.front()] + 1; cost < bestCost {
				bestCost = cost
				bestPrev = deque.front()
				bestKind = fbChunkRLE
			}
		}
		fullCost = append(fullCost, bestCost)
		fullPrev = append(fullPrev, bestPrev)
		fullKind = append(fullKind, bestKind)

		// endCost[i]: only the final chunk may be a partially filled vector.
		bestCost = fbChunkCostInf
		bestPrev = 0
		bestKind = fbChunkRLE

		maxTwoBit := twccStatusVectorTwoBit
		if i < maxTwoBit {
			maxTwoBit = i
		}
		for length := 1; length <= maxTwoBit; length++ {
			if cost := fullCost[i-length] + 1; cost < bestCost {
				bestCost = cost
				bestPrev = int32(i - length)
				bestKind = fbChunkTwoBit
			}
		}
		maxOneBit := twccStatusVectorOneBit
		if i < maxOneBit {
			maxOneBit = i
		}
		if tailEncodable < maxOneBit {
			maxOneBit = tailEncodable
		}
		// Lengths up to 7 are already covered by a (full) two-bit vector with
		// the same chunk count; longer one-bit vectors are the partial case.
		for length := twccStatusVectorTwoBit + 1; length <= maxOneBit; length++ {
			if cost := fullCost[i-length] + 1; cost < bestCost {
				bestCost = cost
				bestPrev = int32(i - length)
				bestKind = fbChunkOneBit
			}
		}
		if !deque.empty() {
			if cost := fullCost[deque.front()] + 1; cost < bestCost {
				bestCost = cost
				bestPrev = deque.front()
				bestKind = fbChunkRLE
			}
		}
		endCost = append(endCost, bestCost)
		endPrev = append(endPrev, bestPrev)
		endKind = append(endKind, bestKind)

		marshaledSize := roundUpTo4(
			fbFixedHeaderSize + int(endCost[i])*packetStatusChunkLength + int(cumulativeDelta[i]),
		)
		if marshaledSize <= maxBytes {
			covered = i
			continue
		}
		// A longer prefix is not necessarily larger: it can use fewer chunks
		// (for example a full one-bit vector instead of two chunks). Keep
		// scanning, but stop once the absolute lower bound (a single chunk)
		// cannot fit any longer prefix either. cumulativeDelta is monotonic,
		// so the bound stays true from here on.
		lowerBound := roundUpTo4(fbFixedHeaderSize + packetStatusChunkLength + int(cumulativeDelta[i]))
		if lowerBound > maxBytes {
			break
		}
	}

	if covered == 0 {
		return nil, 0, errors.New("rtcp: transport feedback sample does not fit in maxBytes")
	}

	chunks := make([]fbChunkDesc, 0, endCost[covered])
	index := covered
	kind := endKind[index]
	prev := int(endPrev[index])
	for index > 0 {
		chunks = append(chunks, fbChunkDesc{kind: kind, start: prev, length: index - prev})
		index = prev
		if index > 0 {
			kind = fullKind[index]
			prev = int(fullPrev[index])
		}
	}
	for left, right := 0, len(chunks)-1; left < right; left, right = left+1, right-1 {
		chunks[left], chunks[right] = chunks[right], chunks[left]
	}

	packetChunks := make([]PacketStatusChunk, 0, len(chunks))
	for _, chunk := range chunks {
		switch chunk.kind {
		case fbChunkRLE:
			packetChunks = append(packetChunks, &RunLengthChunk{
				Type:               TypeTCCRunLengthChunk,
				PacketStatusSymbol: uint16(symbols[chunk.start]),
				RunLength:          uint16(chunk.length),
			})
		case fbChunkOneBit:
			symbolList := make([]uint16, chunk.length)
			for i := range symbolList {
				symbolList[i] = uint16(symbols[chunk.start+i])
			}
			packetChunks = append(packetChunks, &StatusVectorChunk{
				Type:       TypeTCCStatusVectorChunk,
				SymbolSize: TypeTCCSymbolSizeOneBit,
				SymbolList: symbolList,
			})
		case fbChunkTwoBit:
			symbolList := make([]uint16, chunk.length)
			for i := range symbolList {
				symbolList[i] = uint16(symbols[chunk.start+i])
			}
			packetChunks = append(packetChunks, &StatusVectorChunk{
				Type:       TypeTCCStatusVectorChunk,
				SymbolSize: TypeTCCSymbolSizeTwoBit,
				SymbolList: symbolList,
			})
		}
	}

	recvDeltas := make([]*RecvDelta, 0, cumulativeDelta[covered])
	received := false
	for pos := 0; pos < covered; pos++ {
		if symbols[pos] != uint8(TypeTCCPacketReceivedSmallDelta) &&
			symbols[pos] != uint8(TypeTCCPacketReceivedLargeDelta) {
			continue
		}
		received = true
		deltaType := TypeTCCPacketReceivedSmallDelta
		if symbols[pos] == uint8(TypeTCCPacketReceivedLargeDelta) {
			deltaType = TypeTCCPacketReceivedLargeDelta
		}
		recvDeltas = append(recvDeltas, &RecvDelta{
			Type:  deltaType,
			Delta: int64(deltaUnits[pos]) * TypeTCCDeltaScaleFactor,
		})
	}

	var packetReference uint32
	if received {
		packetReference = uint32(referenceUS) & 0x00FFFFFF
	}

	innerLen := fbFixedHeaderSize + len(packetChunks)*packetStatusChunkLength + int(cumulativeDelta[covered])
	marshalSize := roundUpTo4(innerLen)
	if marshalSize > maxBytes {
		return nil, 0, errors.New("rtcp: transport feedback size accounting mismatch")
	}

	packet := TransportLayerCC{
		Header: Header{
			Padding: marshalSize != innerLen,
			Count:   FormatTCC,
			Type:    TypeTransportSpecificFeedback,
			Length:  uint16(marshalSize/4 - 1),
		},
		SenderSSRC:         senderSSRC,
		MediaSSRC:          mediaSSRC,
		BaseSequenceNumber: baseSequence,
		PacketStatusCount:  uint16(covered),
		ReferenceTime:      packetReference,
		FbPktCount:         fbPktCount,
		PacketChunks:       packetChunks,
		RecvDeltas:         recvDeltas,
	}

	raw, err := packet.Marshal()
	if err != nil {
		return nil, 0, err
	}
	if len(raw) != marshalSize {
		return nil, 0, errors.New("rtcp: transport feedback marshal size mismatch")
	}

	return raw, covered, nil
}

func roundUpTo4(n int) int {
	return (n + 3) &^ 3
}
