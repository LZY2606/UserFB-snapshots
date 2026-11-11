// SPDX-FileCopyrightText: 2026 The Pion community <https://pion.ly>
// SPDX-License-Identifier: MIT

package codecs

// This file is an independent AV1 RTP contract regression. It deliberately
// does not use AV1Packet, AV1Depacketizer's parsing helpers, OBU header/size
// parsing from codecs/av1/obu, or the payloader's private length helpers to
// build or check expectations. The payloader and depacketizer are only used as
// the subjects under test. Every expected OBU type, extension identity
// (temporal_id/spatial_id) and payload below is written down independently.

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"
)

// Independent OBU type values (AV1 specification 5.3.1). They intentionally
// mirror the wire values instead of importing production constants.
const (
	refOBUSequenceHeader    byte = 1
	refOBUTemporalDelimiter byte = 2
	refOBUFrameHeader       byte = 3
	refOBUTileGroup         byte = 4
	refOBUMetadata          byte = 5
	refOBUFrame             byte = 6
	refOBUTileList          byte = 8
	refOBUPadding           byte = 15
)

// refOBU is an independently authored OBU description. Payload is the raw OBU
// payload, never a value parsed out of the payloader output.
type refOBU struct {
	Type          byte
	HasExtension  bool
	TemporalID    uint8
	SpatialID     uint8
	Reserved3Bits uint8
	Payload       []byte
	// SizeField controls how this OBU is written into the low-overhead input.
	// RTP elements always omit obu_size, independent of this flag.
	SizeField bool
}

func (o refOBU) headerByte(withSizeField bool) byte {
	header := (o.Type & 0x0f) << 3
	if o.HasExtension {
		header |= 0x04
	}
	if withSizeField {
		header |= 0x02
	}

	return header
}

// marshal writes an independently encoded low-overhead OBU. It is used both
// for hand-written input and for the expected depacketizer output.
func (o refOBU) marshal(withSizeField bool) []byte {
	out := []byte{o.headerByte(withSizeField)}
	if o.HasExtension {
		extension := (o.TemporalID << 5) | ((o.SpatialID & 0x03) << 3) | (o.Reserved3Bits & 0x07)
		out = append(out, extension)
	}
	if withSizeField {
		out = append(out, refLebEncode(uint(len(o.Payload)))...)
	}
	out = append(out, o.Payload...)

	return out
}

// refLebEncode is an independent LEB128 encoder.
func refLebEncode(value uint) []byte {
	var out []byte
	for {
		b := byte(value & 0x7f)
		value >>= 7
		if value != 0 {
			b |= 0x80
		}
		out = append(out, b)
		if value == 0 {
			return out
		}
	}
}

// refLebDecode is an independent LEB128 decoder. It returns value, bytes read
// and whether a terminated value was found.
func refLebDecode(in []byte) (uint, int, bool) {
	var value uint
	var shift uint
	for i := range in {
		value |= uint(in[i]&0x7f) << shift
		if in[i]&0x80 == 0 {
			return value, i + 1, true
		}
		shift += 7
	}

	return 0, 0, false
}

func refPayload(size int, seed byte) []byte {
	out := make([]byte, size)
	for i := range out {
		out[i] = byte(i) + seed
	}

	return out
}

// refAggPacket is the independently parsed RTP aggregation layout.
type refAggPacket struct {
	Z, Y, N bool
	W       byte
	// elements are OBU element bytes with RTP length fields removed.
	elements [][]byte
}

// refParseAggregation implements the AV1 RTP aggregation header and element
// length rules directly, without production parsing helpers.
func refParseAggregation(payload []byte) (refAggPacket, error) {
	var packet refAggPacket
	if len(payload) == 0 {
		return packet, errors.New("ASSERT[W] empty RTP payload")
	}
	header := payload[0]
	packet.Z = header&0x80 != 0
	packet.Y = header&0x40 != 0
	packet.N = header&0x08 != 0
	packet.W = (header & 0x30) >> 4

	offset := 1
	if packet.W == 0 {
		// W=0: every element is preceded by a LEB128 length.
		for offset < len(payload) {
			length, n, ok := refLebDecode(payload[offset:])
			if !ok {
				return packet, errors.New("ASSERT[W] unterminated element length LEB128")
			}
			offset += n
			if int(length) > len(payload)-offset {
				return packet, fmt.Errorf(
					"ASSERT[W] element length %d exceeds %d remaining bytes",
					length, len(payload)-offset,
				)
			}
			packet.elements = append(packet.elements, payload[offset:offset+int(length)])
			offset += int(length)
		}
	} else {
		// W=1,2,3: only the first W-1 elements have a LEB128 length. The
		// last element extends to the end of the RTP payload.
		for i := 0; i < int(packet.W); i++ {
			if i < int(packet.W)-1 {
				length, n, ok := refLebDecode(payload[offset:])
				if !ok {
					return packet, errors.New("ASSERT[W] unterminated element length LEB128")
				}
				offset += n
				if int(length) > len(payload)-offset {
					return packet, fmt.Errorf(
						"ASSERT[W] element length %d exceeds %d remaining bytes",
						length, len(payload)-offset,
					)
				}
				packet.elements = append(packet.elements, payload[offset:offset+int(length)])
				offset += int(length)

				continue
			}

			if offset > len(payload) {
				return packet, errors.New("ASSERT[W] missing final element")
			}
			packet.elements = append(packet.elements, payload[offset:])
			offset = len(payload)
		}
		if offset != len(payload) {
			return packet, errors.New("ASSERT[W] trailing bytes after last element")
		}
		if len(packet.elements) != int(packet.W) {
			return packet, fmt.Errorf(
				"ASSERT[W] W=%d but parsed %d elements", packet.W, len(packet.elements),
			)
		}
	}

	return packet, nil
}

// refReassembler reconstructs complete low-overhead OBUs from RTP elements and
// checks Z/Y continuity while doing so.
type refReassembler struct {
	want          []refOBU
	next          int
	pending       []byte
	pendingActive bool
}

func (r *refReassembler) consume(packet refAggPacket) ([]int, error) {
	var touched []int

	if packet.Z && !r.pendingActive {
		return nil, errors.New("ASSERT[ZY] packet has Z but no fragment is pending")
	}
	if !packet.Z && r.pendingActive {
		return nil, errors.New("ASSERT[ZY] pending fragment was not continued with Z")
	}

	for i, element := range packet.elements {
		index := r.next
		touched = append(touched, index)
		if index >= len(r.want) {
			return nil, fmt.Errorf("ASSERT[OBUCOUNT] element belongs to unexpected OBU %d", index)
		}

		var current []byte
		if i == 0 && packet.Z {
			current = append(current, r.pending...)
		}
		current = append(current, element...)

		isLastElement := i == len(packet.elements)-1
		if isLastElement && packet.Y {
			r.pending = current
			r.pendingActive = true

			continue
		}

		expected := r.want[index].marshal(false) // RTP elements never carry obu_size
		if !bytes.Equal(current, expected) {
			return nil, fmt.Errorf(
				"ASSERT[payload] OBU %d bytes differ: got=%x want=%x",
				index, current, expected,
			)
		}
		r.next++
		r.pending = nil
		r.pendingActive = false
	}

	return touched, nil
}

// refRTPHeader independently encodes an aggregation header.
func refRTPHeader(z, y, n bool, w byte) byte {
	header := (w & 0x03) << 4
	if z {
		header |= 0x80
	}
	if y {
		header |= 0x40
	}
	if n {
		header |= 0x08
	}

	return header
}

// refRTPPacket independently encodes one hand-written RTP payload. When w is
// zero every element has a length field; otherwise the last element has none.
func refRTPPacket(z, y, n bool, w byte, elements ...[]byte) []byte {
	out := []byte{refRTPHeader(z, y, n, w)}
	for i, element := range elements {
		if w != 0 && i == len(elements)-1 {
			out = append(out, element...)

			continue
		}
		out = append(out, refLebEncode(uint(len(element)))...)
		out = append(out, element...)
	}

	return out
}

func refStream(obus []refOBU) []byte {
	out := make([]byte, 0, 64)
	for _, o := range obus {
		out = append(out, o.marshal(true)...)
	}

	return out
}

// senderCase gives one fixed-MTU payloader scenario. Input and expected OBUs
// are authored independently from one another.
type senderCase struct {
	name  string
	mtu   uint16
	input []refOBU
	want  []refOBU
}

func refSenderCases() []senderCase {
	seqPayload := refPayload(5, 0x10)
	framePayload := refPayload(7, 0x20)

	return []senderCase{
		{
			name: "sequence header followed by a frame",
			mtu:  1000,
			input: []refOBU{
				{Type: refOBUSequenceHeader, SizeField: true, Payload: seqPayload},
				{Type: refOBUFrame, SizeField: true, Payload: framePayload},
			},
			want: []refOBU{
				{Type: refOBUSequenceHeader, Payload: seqPayload},
				{Type: refOBUFrame, Payload: framePayload},
			},
		},
		{
			// Same input as TestBaselineLayerMix: temporal_id 0, 1, 2.
			name: "metadata layers temporal 0 1 2",
			mtu:  100,
			input: []refOBU{
				{
					Type: refOBUMetadata, HasExtension: true, TemporalID: 0, SpatialID: 0,
					SizeField: true, Payload: []byte{0x11},
				},
				{
					Type: refOBUMetadata, HasExtension: true, TemporalID: 1, SpatialID: 0,
					SizeField: true, Payload: []byte{0x22},
				},
				{
					Type: refOBUMetadata, HasExtension: true, TemporalID: 2, SpatialID: 0,
					SizeField: true, Payload: []byte{0x33},
				},
			},
			want: []refOBU{
				{Type: refOBUMetadata, HasExtension: true, TemporalID: 0, SpatialID: 0, Payload: []byte{0x11}},
				{Type: refOBUMetadata, HasExtension: true, TemporalID: 1, SpatialID: 0, Payload: []byte{0x22}},
				{Type: refOBUMetadata, HasExtension: true, TemporalID: 2, SpatialID: 0, Payload: []byte{0x33}},
			},
		},
		{
			name: "ignored temporal delimiter tile list and padding",
			mtu:  1000,
			input: []refOBU{
				{Type: refOBUSequenceHeader, SizeField: true, Payload: refPayload(4, 0x30)},
				{Type: refOBUTemporalDelimiter, SizeField: true, Payload: []byte{0x00}},
				{Type: refOBUFrame, SizeField: true, Payload: refPayload(6, 0x40)},
				{Type: refOBUTileList, SizeField: true, Payload: refPayload(3, 0x50)},
				{Type: refOBUPadding, SizeField: true, Payload: refPayload(5, 0x60)},
				{Type: refOBUMetadata, HasExtension: true, TemporalID: 3, SpatialID: 1, SizeField: true, Payload: []byte{0x70}},
			},
			want: []refOBU{
				{Type: refOBUSequenceHeader, Payload: refPayload(4, 0x30)},
				{Type: refOBUFrame, Payload: refPayload(6, 0x40)},
				{
					Type: refOBUMetadata, HasExtension: true, TemporalID: 3, SpatialID: 1,
					Payload: []byte{0x70},
				},
			},
		},
		{
			// Element lengths (OBU header includes payload) straddle the
			// one-byte/two-byte LEB128 boundary at 127/128.
			name: "element length boundaries 127 and 128",
			mtu:  1000,
			input: []refOBU{
				{Type: refOBUFrameHeader, SizeField: true, Payload: refPayload(126, 0x01)},
				{Type: refOBUFrameHeader, SizeField: true, Payload: refPayload(127, 0x02)},
				{Type: refOBUFrame, SizeField: true, Payload: refPayload(2, 0x03)},
			},
			want: []refOBU{
				{Type: refOBUFrameHeader, Payload: refPayload(126, 0x01)},
				{Type: refOBUFrameHeader, Payload: refPayload(127, 0x02)},
				{Type: refOBUFrame, Payload: refPayload(2, 0x03)},
			},
		},
		{
			// Element lengths straddle the two-byte/three-byte LEB128
			// boundary at 16383/16384.
			name: "element length boundaries 16383 and 16384",
			mtu:  40000,
			input: []refOBU{
				{Type: refOBUFrameHeader, SizeField: true, Payload: refPayload(16382, 0x04)},
				{Type: refOBUFrameHeader, SizeField: true, Payload: refPayload(16383, 0x05)},
				{Type: refOBUFrame, SizeField: true, Payload: refPayload(2, 0x06)},
			},
			want: []refOBU{
				{Type: refOBUFrameHeader, Payload: refPayload(16382, 0x04)},
				{Type: refOBUFrameHeader, Payload: refPayload(16383, 0x05)},
				{Type: refOBUFrame, Payload: refPayload(2, 0x06)},
			},
		},
		{
			// Fragments a large OBU and then switches to another temporal
			// layer, so Z/Y continuity and post-fragment layer separation
			// are both exercised.
			name: "fragmentation followed by layer switch",
			mtu:  100,
			input: []refOBU{
				{Type: refOBUSequenceHeader, SizeField: true, Payload: refPayload(3, 0x70)},
				{
					Type: refOBUFrame, HasExtension: true, TemporalID: 0, SpatialID: 0,
					SizeField: true, Payload: refPayload(250, 0x80),
				},
				{
					Type: refOBUMetadata, HasExtension: true, TemporalID: 1, SpatialID: 0,
					SizeField: true, Payload: []byte{0x90}},
			},
			want: []refOBU{
				{Type: refOBUSequenceHeader, Payload: refPayload(3, 0x70)},
				{
					Type: refOBUFrame, HasExtension: true, TemporalID: 0, SpatialID: 0,
					Payload: refPayload(250, 0x80),
				},
				{
					Type: refOBUMetadata, HasExtension: true, TemporalID: 1, SpatialID: 0,
					Payload: []byte{0x90},
				},
			},
		},
		{
			// The final OBU deliberately has no obu_size field; the
			// payloader must infer its size from the remaining input.
			name: "final OBU without size field",
			mtu:  1000,
			input: []refOBU{
				{Type: refOBUSequenceHeader, SizeField: true, Payload: refPayload(4, 0xA0)},
				{Type: refOBUMetadata, HasExtension: true, TemporalID: 1, SpatialID: 1, Payload: []byte{0xA1, 0xA2}},
			},
			want: []refOBU{
				{Type: refOBUSequenceHeader, Payload: refPayload(4, 0xA0)},
				{
					Type: refOBUMetadata, HasExtension: true, TemporalID: 1, SpatialID: 1,
					Payload: []byte{0xA1, 0xA2},
				},
			},
		},
	}
}

func runSenderCase(t *testing.T, testCase senderCase) {
	t.Helper()

	payloader := &AV1Payloader{}
	packets := payloader.Payload(testCase.mtu, refSenderInput(testCase))
	if len(packets) == 0 {
		t.Fatalf("ASSERT[packets] payloader returned no packets for %q", testCase.name)
	}

	reassembler := &refReassembler{want: testCase.want}
	previousY := false
	for packetIndex, packet := range packets {
		if len(packet) > int(testCase.mtu) {
			t.Fatalf(
				"ASSERT[MTU] packet %d length %d exceeds MTU %d",
				packetIndex, len(packet), testCase.mtu,
			)
		}

		aggregation, err := refParseAggregation(packet)
		if err != nil {
			t.Fatalf("packet %d: %v (%x)", packetIndex, err, packet)
		}
		if len(aggregation.elements) == 0 {
			t.Fatalf("ASSERT[W] packet %d has no OBU elements", packetIndex)
		}

		// Z on a packet must exactly continue a Y from its predecessor.
		if aggregation.Z != previousY {
			t.Fatalf(
				"ASSERT[ZY] packet %d Z=%v but previous packet Y=%v",
				packetIndex, aggregation.Z, previousY,
			)
		}

		// N is allowed only when a packet starts a new coded video
		// sequence, i.e. its first element is a sequence header OBU.
		expectedN := !aggregation.Z &&
			reassembler.next < len(testCase.want) &&
			testCase.want[reassembler.next].Type == refOBUSequenceHeader
		if aggregation.N != expectedN {
			t.Fatalf(
				"ASSERT[N] packet %d N=%v but new-sequence start=%v",
				packetIndex, aggregation.N, expectedN,
			)
		}

		touched, err := reassembler.consume(aggregation)
		if err != nil {
			t.Fatalf("packet %d: %v", packetIndex, err)
		}

		// OBUs carrying extension headers in one RTP packet must all have
		// the same temporal_id and spatial_id.
		layers := map[[2]uint8]struct{}{}
		for _, obuIndex := range touched {
			if obuIndex >= len(testCase.want) || !testCase.want[obuIndex].HasExtension {
				continue
			}
			layers[[2]uint8{
				testCase.want[obuIndex].TemporalID,
				testCase.want[obuIndex].SpatialID,
			}] = struct{}{}
		}
		if len(layers) > 1 {
			t.Fatalf("ASSERT[layer] packet %d mixes %d OBU layers", packetIndex, len(layers))
		}

		previousY = aggregation.Y
	}

	if previousY {
		t.Fatalf("ASSERT[ZY] final packet still has Y set")
	}
	if reassembler.pendingActive || reassembler.next != len(testCase.want) {
		t.Fatalf(
			"ASSERT[OBUCOUNT] reconstructed %d/%d OBUs (pending=%v)",
			reassembler.next, len(testCase.want), reassembler.pendingActive,
		)
	}
}

// receiverCall is one hand-written RTP payload and the low-overhead OBUs the
// depacketizer is expected to emit for it.
type receiverCall struct {
	payload []byte
	want    []refOBU
}

type receiverCase struct {
	name  string
	calls []receiverCall
}

func refReceiverCases() []receiverCase {
	obuWithExtension := refOBU{
		Type: refOBUFrameHeader, HasExtension: true, TemporalID: 1, SpatialID: 2,
		Payload: []byte{0xAA, 0xBB},
	}
	obuA := refOBU{Type: refOBUMetadata, HasExtension: true, TemporalID: 2, SpatialID: 1, Payload: []byte{0x01}}
	obuB := refOBU{Type: refOBUFrameHeader, Payload: []byte{0x02, 0x03}}
	obuC := refOBU{Type: refOBUFrame, Payload: []byte{0x04}}
	obuD := refOBU{Type: refOBUTileGroup, Payload: []byte{0x05, 0x06}}

	threePacketOBU := refOBU{Type: refOBUTileGroup, Payload: refPayload(9, 0x40)}
	fullThreePacketBytes := threePacketOBU.marshal(false)
	threePacketFragments := [3][]byte{
		fullThreePacketBytes[:3],
		fullThreePacketBytes[3:6],
		fullThreePacketBytes[6:],
	}

	firstFragment := refOBU{Type: refOBUFrameHeader, Payload: refPayload(6, 0x80)}
	firstFragmentBytes := firstFragment.marshal(false)

	residualFragment := refOBU{Type: refOBUFrame, Payload: refPayload(6, 0x90)}
	residualFragmentBytes := residualFragment.marshal(false)

	newSequenceOBU := refOBU{Type: refOBUFrameHeader, Payload: refPayload(5, 0xA0)}

	withSizeOBU := refOBU{Type: refOBUFrame, SizeField: true, Payload: refPayload(4, 0xB0)}

	return []receiverCase{
		{
			// W=0 (0x00): every element is length-prefixed, including a
			// zero-length element that receivers ignore.
			name: "W=0 with zero length element",
			calls: []receiverCall{
				{
					payload: refRTPPacket(false, false, false, 0,
						nil,
						obuA.marshal(false),
						obuB.marshal(false),
					),
					want: []refOBU{obuA, obuB},
				},
			},
		},
		{
			// W=1 (0x10), last element has no length; extension identity
			// must survive into the low-overhead output.
			name: "W=1 single OBU with extension",
			calls: []receiverCall{
				{
					payload: refRTPPacket(false, false, false, 1, obuWithExtension.marshal(false)),
					want:    []refOBU{obuWithExtension},
				},
			},
		},
		{
			// W=2 (0x20): first element length-prefixed, last is remainder.
			name: "W=2 two OBUs",
			calls: []receiverCall{
				{
					payload: refRTPPacket(false, false, false, 2, obuB.marshal(false), obuC.marshal(false)),
					want:    []refOBU{obuB, obuC},
				},
			},
		},
		{
			// W=3 (0x30): two length fields and an un-prefixed last element.
			name: "W=3 three OBUs",
			calls: []receiverCall{
				{
					payload: refRTPPacket(false, false, false, 3,
						obuB.marshal(false),
						obuC.marshal(false),
						obuD.marshal(false),
					),
					want: []refOBU{obuB, obuC, obuD},
				},
			},
		},
		{
			// Three packets, headers 0x50, 0xD0 and 0x90.
			// Z=0/Y=1,W=1 -> Z=1/Y=1,W=1 -> Z=1/Y=0,W=1.
			name: "three packets assemble one OBU",
			calls: []receiverCall{
				{
					payload: refRTPPacket(false, true, false, 1, threePacketFragments[0]),
					want:    nil,
				},
				{
					payload: refRTPPacket(true, true, false, 1, threePacketFragments[1]),
					want:    nil,
				},
				{
					payload: refRTPPacket(true, false, false, 1, threePacketFragments[2]),
					want:    []refOBU{threePacketOBU},
				},
			},
		},
		{
			// Z=0/Y=1 packet stores a fragment; an N=1 packet must clear
			// that residual and emit only its own complete OBU.
			name: "new sequence clears residual fragment",
			calls: []receiverCall{
				{
					payload: refRTPPacket(false, true, false, 1, residualFragmentBytes),
					want:    nil,
				},
				{
					payload: refRTPPacket(false, false, true, 1, newSequenceOBU.marshal(false)),
					want:    []refOBU{newSequenceOBU},
				},
			},
		},
		{
			// A Z=1 fragment with no buffered predecessor is dropped, and
			// the following complete OBU is recovered normally.
			name: "lost first fragment then complete OBU",
			calls: []receiverCall{
				{
					payload: refRTPPacket(true, false, false, 1, firstFragmentBytes[:2]),
					want:    nil,
				},
				{
					payload: refRTPPacket(false, false, false, 1, firstFragmentBytes),
					want:    []refOBU{firstFragment},
				},
			},
		},
		{
			// Input OBU already carries obu_size; the depacketizer keeps it
			// when its declared size matches the RTP element.
			name: "input OBU with matching size field",
			calls: []receiverCall{
				{
					payload: refRTPPacket(false, false, false, 1, withSizeOBU.marshal(true)),
					// withSizeOBU.SizeField only controls marshaling; its
					// expected output is the same low-overhead OBU.
					want: []refOBU{withSizeOBU},
				},
			},
		},
	}
}

// refParsedOBU is the independent result of reading a low-overhead OBU.
type refParsedOBU struct {
	Type         byte
	HasExtension bool
	TemporalID   uint8
	SpatialID    uint8
	Size         uint
	Payload      []byte
}

// refReadLowOverhead independently reads a low-overhead bitstream. It fails
// unless every OBU has obu_has_size_field set and a matching, valid obu_size.
func refReadLowOverhead(t *testing.T, payload []byte) []refParsedOBU {
	t.Helper()

	var out []refParsedOBU
	for offset := 0; offset < len(payload); {
		header := payload[offset]
		if header&0x80 != 0 {
			t.Fatalf("ASSERT[header] forbidden bit set at offset %d", offset)
		}
		obuType := (header >> 3) & 0x0f
		hasExtension := header&0x04 != 0
		hasSizeField := header&0x02 != 0
		offset++
		if !hasSizeField {
			t.Fatalf("ASSERT[HasSizeField] OBU at offset %d has no obu_size field", offset-1)
		}

		var temporalID, spatialID uint8
		if hasExtension {
			if offset >= len(payload) {
				t.Fatalf("ASSERT[extension] truncated extension header at offset %d", offset)
			}
			extension := payload[offset]
			offset++
			temporalID = extension >> 5
			spatialID = (extension >> 3) & 0x03
		}

		size, n, ok := refLebDecode(payload[offset:])
		if !ok {
			t.Fatalf("ASSERT[size] unterminated obu_size at offset %d", offset)
		}
		offset += n
		if int(size) > len(payload)-offset {
			t.Fatalf(
				"ASSERT[size] obu_size %d exceeds %d remaining bytes",
				size, len(payload)-offset,
			)
		}

		obuPayload := append([]byte(nil), payload[offset:offset+int(size)]...)
		offset += int(size)
		out = append(out, refParsedOBU{
			Type:         obuType,
			HasExtension: hasExtension,
			TemporalID:   temporalID,
			SpatialID:    spatialID,
			Size:         size,
			Payload:      obuPayload,
		})
	}

	return out
}

// refAssertLowOverhead independently reads out and checks it against the
// expected OBUs, including exact bytes (not just matching lengths).
func refAssertLowOverhead(t *testing.T, label string, out []byte, want []refOBU) {
	t.Helper()

	got := refReadLowOverhead(t, out)
	if len(got) != len(want) {
		t.Fatalf(
			"ASSERT[OBUCOUNT] %s emitted %d OBUs, want %d: %x",
			label, len(got), len(want), out,
		)
	}
	for i, expected := range want {
		parsed := got[i]
		if parsed.Type != expected.Type {
			t.Fatalf(
				"ASSERT[OBUOrder] %s OBU %d type %d, want %d",
				label, i, parsed.Type, expected.Type,
			)
		}
		if parsed.HasExtension != expected.HasExtension {
			t.Fatalf(
				"ASSERT[extension] %s OBU %d extension flag %v, want %v",
				label, i, parsed.HasExtension, expected.HasExtension,
			)
		}
		if expected.HasExtension &&
			(parsed.TemporalID != expected.TemporalID || parsed.SpatialID != expected.SpatialID) {
			t.Fatalf(
				"ASSERT[extension] %s OBU %d temporal_id/spatial_id %d/%d, want %d/%d",
				label, i, parsed.TemporalID, parsed.SpatialID,
				expected.TemporalID, expected.SpatialID,
			)
		}
		if parsed.Size != uint(len(expected.Payload)) {
			t.Fatalf(
				"ASSERT[size] %s OBU %d obu_size %d, want %d",
				label, i, parsed.Size, len(expected.Payload),
			)
		}
		if !bytes.Equal(parsed.Payload, expected.Payload) {
			t.Fatalf(
				"ASSERT[payload] %s OBU %d payload got=%x want=%x",
				label, i, parsed.Payload, expected.Payload,
			)
		}
	}

	expectedBytes := refStream(want)
	if !bytes.Equal(out, expectedBytes) {
		t.Fatalf(
			"ASSERT[bytes] %s low-overhead bytes got=%x want=%x",
			label, out, expectedBytes,
		)
	}
}

func runReceiverCase(t *testing.T, testCase receiverCase) {
	t.Helper()

	depacketizer := &AV1Depacketizer{}
	for callIndex, call := range testCase.calls {
		out, err := depacketizer.Unmarshal(call.payload)
		if err != nil {
			t.Fatalf("ASSERT[valid] call %d returned error: %v", callIndex, err)
		}
		refAssertLowOverhead(t, fmt.Sprintf("call %d", callIndex), out, call.want)
	}
}

// refSenderInput independently marshals the low-overhead input for a sender
// case.
func refSenderInput(testCase senderCase) []byte {
	var input []byte
	for _, o := range testCase.input {
		input = append(input, o.marshal(o.SizeField)...)
	}

	return input
}

// runPayloaderDepacketizerRoundTrip links the low-overhead input, the actual
// RTP layout produced by AV1Payloader, and AV1Depacketizer reconstruction.
func runPayloaderDepacketizerRoundTrip(t *testing.T, testCase senderCase) {
	t.Helper()

	payloader := &AV1Payloader{}
	packets := payloader.Payload(testCase.mtu, refSenderInput(testCase))
	if len(packets) == 0 {
		t.Fatalf("ASSERT[packets] payloader returned no packets for %q", testCase.name)
	}

	depacketizer := &AV1Depacketizer{}
	var stream []byte
	for packetIndex, packet := range packets {
		out, err := depacketizer.Unmarshal(packet)
		if err != nil {
			t.Fatalf(
				"ASSERT[valid] depacketizer rejected packet %d: %v",
				packetIndex, err,
			)
		}
		stream = append(stream, out...)
	}

	refAssertLowOverhead(t, "round trip", stream, testCase.want)
}

func refInvalidReceiverCases() []struct {
	name    string
	payload []byte
} {
	oneElement := refOBU{Type: refOBUTileGroup, Payload: []byte{0x01, 0x02, 0x03}}.marshal(false)

	return []struct {
		name    string
		payload []byte
	}{
		{
			name:    "truncated element length",
			payload: []byte{0x00, 0x05, 0x01, 0x02},
		},
		{
			name:    "unterminated LEB128 length",
			payload: []byte{0x00, 0x80},
		},
		{
			name:    "W=2 length exceeds payload",
			payload: []byte{0x20, 0x05, 0x22, 0x01},
		},
		{
			name:    "W=3 element count mismatch",
			payload: refRTPPacket(false, false, false, 3, oneElement),
		},
	}
}

func TestAV1Contract(t *testing.T) {
	t.Run("SenderReference", func(t *testing.T) {
		for _, testCase := range refSenderCases() {
			testCase := testCase
			t.Run(testCase.name, func(t *testing.T) {
				runSenderCase(t, testCase)
			})
		}
	})

	t.Run("ReceiverReference", func(t *testing.T) {
		for _, testCase := range refReceiverCases() {
			testCase := testCase
			t.Run(testCase.name, func(t *testing.T) {
				runReceiverCase(t, testCase)
			})
		}

		// More than the payloader's own output is needed, but that output
		// is still part of the chain: payloader -> RTP layout -> receiver.
		t.Run("PayloaderRoundTrip", func(t *testing.T) {
			for _, testCase := range refSenderCases() {
				testCase := testCase
				t.Run(testCase.name, func(t *testing.T) {
					runPayloaderDepacketizerRoundTrip(t, testCase)
				})
			}
		})

		t.Run("invalid packets rejected", func(t *testing.T) {
			for _, testCase := range refInvalidReceiverCases() {
				testCase := testCase
				t.Run(testCase.name, func(t *testing.T) {
					depacketizer := &AV1Depacketizer{}
					_, err := depacketizer.Unmarshal(testCase.payload)
					if err == nil {
						t.Fatalf("ASSERT[invalid] invalid payload was accepted: %x", testCase.payload)
					}
				})
			}
		})
	})

	// The mutation self-check recursively builds temporary copies and runs
	// this same test there. The environment flag stops the subprocess from
	// starting another self-check.
	if os.Getenv("AV1_CONTRACT_SELFCHECK_CHILD") == "" {
		t.Run("MutationSelfCheck", func(t *testing.T) {
			runMutationSelfCheck(t)
		})
	}
}

type mutation struct {
	name   string
	file   string
	old    string
	new    string
	marker string
}

func refMutations() []mutation {
	return []mutation{
		{
			name: "MUTATION_N",
			file: filepath.Join("codecs", "av1_packet.go"),
			old: `		if isNewVideoSequence {
			payload[0] |= 1 << av1NBitshift
		}`,
			new: `		if isNewVideoSequence {
			// self-check mutation: N bit intentionally omitted
		}`,
			marker: "ASSERT[N]",
		},
		{
			name: "MUTATION_Z",
			file: filepath.Join("codecs", "av1_packet.go"),
			old: `			payloads[currentPayload-1][0] |= av1YMask
			payloads[currentPayload][0] |= av1ZMask`,
			new: `			payloads[currentPayload-1][0] |= av1YMask
			// self-check mutation: Z bit intentionally omitted`,
			marker: "ASSERT[ZY]",
		},
		{
			name: "MUTATION_HASSIZE",
			file: filepath.Join("codecs", "av1_depacketizer.go"),
			old: `			obuHeader.HasSizeField = true
			buff = d.appendOBUWithCalculatedSize(buff, obuHeader, obuBuffer, obuHeader.Size())`,
			new: `			// self-check mutation: HasSizeField intentionally not set
			buff = d.appendOBUWithCalculatedSize(buff, obuHeader, obuBuffer, obuHeader.Size())`,
			marker: "ASSERT[HasSizeField]",
		},
	}
}

func moduleRootDir(t *testing.T) string {
	t.Helper()

	_, file, _, ok := runtime.Caller(0)
	if !ok {
		t.Fatal("SELFCHECK_SCRIPT: runtime.Caller failed")
	}

	return filepath.Dir(filepath.Dir(file))
}

func copyFileTo(t *testing.T, src, dst string) {
	t.Helper()

	data, err := os.ReadFile(src)
	if err != nil {
		t.Fatalf("SELFCHECK_SCRIPT: read %s: %v", src, err)
	}
	if err := os.WriteFile(dst, data, 0o644); err != nil { //nolint:gosec
		t.Fatalf("SELFCHECK_SCRIPT: write %s: %v", dst, err)
	}
}

func copyDirTo(t *testing.T, src, dst string) {
	t.Helper()

	entries, err := os.ReadDir(src)
	if err != nil {
		t.Fatalf("SELFCHECK_SCRIPT: read dir %s: %v", src, err)
	}
	if err := os.MkdirAll(dst, 0o755); err != nil { //nolint:gosec
		t.Fatalf("SELFCHECK_SCRIPT: mkdir %s: %v", dst, err)
	}

	for _, entry := range entries {
		srcPath := filepath.Join(src, entry.Name())
		dstPath := filepath.Join(dst, entry.Name())
		if entry.IsDir() {
			copyDirTo(t, srcPath, dstPath)

			continue
		}
		copyFileTo(t, srcPath, dstPath)
	}
}

func prepareTempModule(t *testing.T) string {
	t.Helper()

	root := moduleRootDir(t)
	dst := filepath.Join(t.TempDir(), "module")
	if err := os.MkdirAll(dst, 0o755); err != nil { //nolint:gosec
		t.Fatalf("SELFCHECK_SCRIPT: mkdir module: %v", err)
	}

	for _, name := range []string{"go.mod", "go.sum"} {
		copyFileTo(t, filepath.Join(root, name), filepath.Join(dst, name))
	}
	copyDirTo(t, filepath.Join(root, "codecs"), filepath.Join(dst, "codecs"))

	return dst
}

func applyMutation(t *testing.T, moduleDir string, mut mutation) {
	t.Helper()

	path := filepath.Join(moduleDir, mut.file)
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("SELFCHECK_SCRIPT: read mutation target %s: %v", path, err)
	}
	text := string(data)
	if strings.Count(text, mut.old) != 1 {
		t.Fatalf(
			"SELFCHECK_SCRIPT: mutation %s target found %d times in %s",
			mut.name, strings.Count(text, mut.old), mut.file,
		)
	}
	text = strings.Replace(text, mut.old, mut.new, 1)
	if err := os.WriteFile(path, []byte(text), 0o644); err != nil { //nolint:gosec
		t.Fatalf("SELFCHECK_SCRIPT: write mutation %s: %v", mut.name, err)
	}
}

func runContractInModule(t *testing.T, moduleDir string) (string, error) {
	t.Helper()

	ctx, cancel := context.WithTimeout(context.Background(), 120*time.Second)
	defer cancel()

	cmd := exec.CommandContext( //nolint:gosec
		ctx, "go", "test", "-count=1", "-v", "-run", "^TestAV1Contract$", "./codecs",
	)
	cmd.Dir = moduleDir
	cmd.Env = append(os.Environ(),
		"GOWORK=off",
		// The parent test build already populated GOMODCACHE, so the
		// mutation copies do not need network access.
		"GOPROXY=off",
		"GOTOOLCHAIN=local",
		"AV1_CONTRACT_SELFCHECK_CHILD=1",
	)

	var output bytes.Buffer
	cmd.Stdout = &output
	cmd.Stderr = &output
	err := cmd.Run()
	if ctx.Err() == context.DeadlineExceeded {
		t.Fatalf("SELFCHECK_SCRIPT: child go test timed out")
	}

	return output.String(), err
}

func firstLineContaining(text, marker string) string {
	for _, line := range strings.Split(text, "\n") {
		if strings.Contains(line, marker) {
			return strings.TrimSpace(line)
		}
	}

	return ""
}

func runMutationSelfCheck(t *testing.T) {
	t.Helper()

	// Control: an unmodified temporary copy must pass the same regression.
	controlDir := prepareTempModule(t)
	controlOutput, controlErr := runContractInModule(t, controlDir)
	if controlErr != nil {
		t.Fatalf(
			"SELFCHECK_SCRIPT: unmodified copy failed: %v\n%s",
			controlErr, controlOutput,
		)
	}
	if !strings.Contains(controlOutput, "--- PASS: TestAV1Contract") {
		t.Fatalf(
			"SELFCHECK_SCRIPT: unmodified copy did not report PASS:\n%s",
			controlOutput,
		)
	}
	t.Logf("SELFCHECK_CONTROL_OK: unmodified temporary copy passed TestAV1Contract")

	for _, mut := range refMutations() {
		moduleDir := prepareTempModule(t)
		applyMutation(t, moduleDir, mut)

		output, err := runContractInModule(t, moduleDir)
		if err == nil {
			t.Fatalf("SELFCHECK_SCRIPT: mutation %s did not fail", mut.name)
		}
		// A compilation failure, a missing test, or a harness failure is
		// not a behavioral detection: the expected assertion must be
		// present and the test must have actually run and failed.
		if strings.Contains(output, "[build failed]") || strings.Contains(output, "build failed") {
			t.Fatalf("SELFCHECK_SCRIPT: mutation %s failed to compile:\n%s", mut.name, output)
		}
		if strings.Contains(output, "no tests to run") {
			t.Fatalf("SELFCHECK_SCRIPT: mutation %s did not match TestAV1Contract:\n%s", mut.name, output)
		}
		if !strings.Contains(output, "=== RUN   TestAV1Contract") ||
			!strings.Contains(output, "--- FAIL: TestAV1Contract") {
			t.Fatalf(
				"SELFCHECK_SCRIPT: mutation %s did not run and fail TestAV1Contract:\n%s",
				mut.name, output,
			)
		}

		failureLine := firstLineContaining(output, mut.marker)
		if failureLine == "" {
			t.Fatalf(
				"SELFCHECK_SCRIPT: mutation %s produced no %s assertion:\n%s",
				mut.name, mut.marker, output,
			)
		}
		t.Logf("SELFCHECK[%s] behavioral failure detected: %s", mut.name, failureLine)
	}

	t.Logf("SELFCHECK_OK: all three mutations were detected by behavioral assertions")
}
