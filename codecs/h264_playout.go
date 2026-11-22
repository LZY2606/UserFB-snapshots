// SPDX-FileCopyrightText: 2026 The Pion community <https://pion.ly>
// SPDX-License-Identifier: MIT

package codecs

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"sort"
	"time"

	"github.com/pion/rtp"
)

const (
	h264PlayoutClockRate = 90000
	h264PlayoutMinWindow = 2
	h264PlayoutMaxWindow = 512

	// maxH264AccessUnitSize bounds the Annex B representation of a single
	// access unit that the playout is willing to buffer and rebuild.
	maxH264AccessUnitSize = 1 << 20
)

// H264Frame is a complete H.264 access unit ready for playback.
type H264Frame struct {
	Timestamp uint32
	Due       time.Duration
	AnnexB    []byte
}

// h264BufferedPacket is an owned copy of a single RTP payload.
type h264BufferedPacket struct {
	payload []byte
	marker  bool
}

// h264AccessUnit groups the packets that share an RTP timestamp.
type h264AccessUnit struct {
	timestamp uint32
	offset    int32
	due       time.Duration

	packets map[uint16]h264BufferedPacket
	anchor  uint16

	// rebuildBytes tracks the exact Annex B size the packets would produce,
	// so oversized access units are rejected before allocating them.
	rebuildBytes int
}

// H264Playout reorders and schedules the access units belonging to a single
// H.264 RTP stream. It is not safe for concurrent use; Push and Advance must
// be called from the same goroutine and time must advance monotonically.
type H264Playout struct {
	ssrc          uint32
	pt            uint8
	baseTimestamp uint32
	baseTime      time.Duration
	delay         time.Duration
	window        int

	accessUnits map[uint32]*h264AccessUnit
	packetCount int

	// consumed remembers the timestamps of access units that Advance has
	// already resolved (delivered or discarded). Such units must never be
	// resurrected by a late packet.
	consumed map[uint32]time.Duration

	waitingForIDR bool
	haveProcessed bool
	prevEndSeq    uint16
}

// NewH264Playout returns a playout for one SSRC and payload type. Access unit
// deadlines are computed as
//
//	baseTime + delay + int64(int32(timestamp-baseTimestamp)) * time.Second / 90000
//
// window bounds the total number of buffered RTP packets.
func NewH264Playout(
	ssrc uint32,
	pt uint8,
	baseTimestamp uint32,
	baseTime time.Duration,
	delay time.Duration,
	window int,
) (*H264Playout, error) {
	if pt > 127 {
		return nil, fmt.Errorf("%w: %d", errH264PayloadTypeOutOfRange, pt)
	}
	if baseTime < 0 || delay < 0 {
		return nil, fmt.Errorf("%w: baseTime=%s delay=%s", errH264NegativeTime, baseTime, delay)
	}
	if window < h264PlayoutMinWindow || window > h264PlayoutMaxWindow {
		return nil, fmt.Errorf("%w: %d", errH264InvalidWindow, window)
	}

	return &H264Playout{
		ssrc:          ssrc,
		pt:            pt,
		baseTimestamp: baseTimestamp,
		baseTime:      baseTime,
		delay:         delay,
		window:        window,
		accessUnits:   make(map[uint32]*h264AccessUnit),
		consumed:      make(map[uint32]time.Duration),
		waitingForIDR: true,
	}, nil
}

// Push adds one RTP datagram to the playout buffer. The datagram is validated
// and copied; later changes to raw have no effect on buffered media.
func (p *H264Playout) Push(raw []byte, now time.Duration) error {
	var packet rtp.Packet
	if err := packet.Unmarshal(raw); err != nil {
		return err
	}
	if packet.Version != 2 {
		return fmt.Errorf("%w: %d", errH264WrongRTPVersion, packet.Version)
	}
	if packet.SSRC != p.ssrc {
		return fmt.Errorf("%w: got %d want %d", errH264WrongSSRC, packet.SSRC, p.ssrc)
	}
	if packet.PayloadType != p.pt {
		return fmt.Errorf("%w: got %d want %d", errH264WrongPayloadType, packet.PayloadType, p.pt)
	}

	offset := int32(packet.Timestamp - p.baseTimestamp) //nolint:gosec // defined conversion
	if offset < 0 {
		return fmt.Errorf("%w: %d", errH264NegativeMediaOffset, offset)
	}
	due := p.baseTime + p.delay + time.Duration(offset)*time.Second/h264PlayoutClockRate

	if _, resolved := p.consumed[packet.Timestamp]; resolved {
		return fmt.Errorf("%w: timestamp %d", errH264StalePacket, packet.Timestamp)
	}
	if now > due {
		return fmt.Errorf("%w: timestamp %d due %s now %s", errH264LatePacket, packet.Timestamp, due, now)
	}

	// Validate the payload's local structure before touching any state.
	if err := validateH264PayloadStructure(packet.Payload); err != nil {
		return err
	}

	accessUnit := p.accessUnits[packet.Timestamp]
	if accessUnit != nil {
		if existing, ok := accessUnit.packets[packet.SequenceNumber]; ok {
			if existing.marker == packet.Marker && bytes.Equal(existing.payload, packet.Payload) {
				return nil
			}

			return fmt.Errorf("%w: sequence %d", errH264ConflictingDuplicate, packet.SequenceNumber)
		}
	}

	if p.packetCount >= p.window {
		return fmt.Errorf("%w: %d packets", errH264WindowFull, p.packetCount)
	}

	contribution := h264PayloadAnnexBSize(packet.Payload)
	if accessUnit != nil && accessUnit.rebuildBytes+contribution > maxH264AccessUnitSize {
		return fmt.Errorf("%w: %d > %d", errH264AccessUnitTooLarge,
			accessUnit.rebuildBytes+contribution, maxH264AccessUnitSize)
	}
	if accessUnit == nil && contribution > maxH264AccessUnitSize {
		return fmt.Errorf("%w: %d > %d", errH264AccessUnitTooLarge, contribution, maxH264AccessUnitSize)
	}

	payload := make([]byte, len(packet.Payload))
	copy(payload, packet.Payload)

	if accessUnit == nil {
		accessUnit = &h264AccessUnit{
			timestamp: packet.Timestamp,
			offset:    offset,
			due:       due,
			packets:   make(map[uint16]h264BufferedPacket),
			anchor:    packet.SequenceNumber,
		}
		p.accessUnits[packet.Timestamp] = accessUnit
	}
	accessUnit.packets[packet.SequenceNumber] = h264BufferedPacket{
		payload: payload,
		marker:  packet.Marker,
	}
	accessUnit.rebuildBytes += contribution
	p.packetCount++

	return nil
}

// Advance resolves all buffered access units that are due at now and returns
// the frames that should be played, in ascending media time order.
func (p *H264Playout) Advance(now time.Duration) []H264Frame {
	due := make([]*h264AccessUnit, 0, len(p.accessUnits))
	for _, accessUnit := range p.accessUnits {
		if accessUnit.due <= now {
			due = append(due, accessUnit)
		}
	}
	sort.Slice(due, func(i, j int) bool { return due[i].offset < due[j].offset })

	var frames []H264Frame //nolint:prealloc // only some access units are delivered

	for _, accessUnit := range due {
		packets := accessUnit.rtpPackets()
		annexB, err := (&H264Packet{}).UnmarshalAccessUnit(packets)

		delete(p.accessUnits, accessUnit.timestamp)
		p.packetCount -= len(accessUnit.packets)
		p.consumed[accessUnit.timestamp] = accessUnit.due

		startSeq, endSeq := accessUnit.sequenceRange()
		if p.haveProcessed && startSeq != p.prevEndSeq+1 {
			// A gap means the decoder may have missed a reference frame.
			p.waitingForIDR = true
		}
		p.prevEndSeq = endSeq
		p.haveProcessed = true

		if err != nil {
			// Missing fragments, a missing marker or a malformed access unit
			// are all unresolvable at the deadline.
			p.waitingForIDR = true

			continue
		}

		hasVCL, hasIDR := h264AccessUnitPictureTypes(packets)
		if p.waitingForIDR {
			if hasIDR {
				frames = append(frames, H264Frame{
					Timestamp: accessUnit.timestamp,
					Due:       accessUnit.due,
					AnnexB:    annexB,
				})
				p.waitingForIDR = false
			}

			continue
		}
		if hasVCL {
			frames = append(frames, H264Frame{
				Timestamp: accessUnit.timestamp,
				Due:       accessUnit.due,
				AnnexB:    annexB,
			})
		}
	}

	// Anything strictly older than now can no longer be pushed: Push checks
	// now > due, and now is monotonic. Keeping older entries would just grow
	// the map. Entries exactly at now must stay so a packet pushed at the
	// deadline cannot resurrect an already-processed access unit.
	for timestamp, due := range p.consumed {
		if due < now {
			delete(p.consumed, timestamp)
		}
	}

	return frames
}

// Reset clears buffered packets, deadlines and reference state while keeping
// the constructor parameters.
func (p *H264Playout) Reset() {
	p.accessUnits = make(map[uint32]*h264AccessUnit)
	p.packetCount = 0
	p.consumed = make(map[uint32]time.Duration)
	p.waitingForIDR = true
	p.haveProcessed = false
	p.prevEndSeq = 0
}

// sequenceRange returns the first and last sequence numbers of the buffered
// packets in stream order, tolerating sequence number wraparound.
func (a *h264AccessUnit) sequenceRange() (uint16, uint16) {
	sequenceNumbers := make([]uint16, 0, len(a.packets))
	for sequenceNumber := range a.packets {
		sequenceNumbers = append(sequenceNumbers, sequenceNumber)
	}
	orderSequences(sequenceNumbers, a.anchor)

	return sequenceNumbers[0], sequenceNumbers[len(sequenceNumbers)-1]
}

// rtpPackets returns the buffered packets in stream order.
func (a *h264AccessUnit) rtpPackets() []*rtp.Packet {
	sequenceNumbers := make([]uint16, 0, len(a.packets))
	for sequenceNumber := range a.packets {
		sequenceNumbers = append(sequenceNumbers, sequenceNumber)
	}
	orderSequences(sequenceNumbers, a.anchor)

	packets := make([]*rtp.Packet, 0, len(sequenceNumbers))
	for _, sequenceNumber := range sequenceNumbers {
		buffered := a.packets[sequenceNumber]
		packets = append(packets, &rtp.Packet{
			Header: rtp.Header{
				Version:        2,
				Marker:         buffered.marker,
				SequenceNumber: sequenceNumber,
				Timestamp:      a.timestamp,
			},
			Payload: buffered.payload,
		})
	}

	return packets
}

// orderSequences sorts sequence numbers relative to anchor, relying on the
// guarantee that the active sequence span is smaller than 32768.
func orderSequences(sequenceNumbers []uint16, anchor uint16) {
	offsets := make(map[uint16]int, len(sequenceNumbers))
	for _, sequenceNumber := range sequenceNumbers {
		offsets[sequenceNumber] = int(int16(sequenceNumber - anchor))
	}
	sort.Slice(sequenceNumbers, func(i, j int) bool {
		return offsets[sequenceNumbers[i]] < offsets[sequenceNumbers[j]]
	})
}

// validateH264PayloadStructure checks everything that can be decided from a
// single RTP payload, without knowledge of neighbouring packets.
func validateH264PayloadStructure(payload []byte) error { //nolint:cyclop
	if len(payload) == 0 {
		return fmt.Errorf("%w: empty payload", errShortPacket)
	}

	naluType := payload[0] & naluTypeBitmask
	switch {
	case naluType >= 1 && naluType <= 23:
		return nil

	case naluType == stapaNALUType:
		return validateStapAPayload(payload)

	case naluType == fuaNALUType:
		if len(payload) < fuaHeaderSize {
			return fmt.Errorf("%w: FU-A header", errShortPacket)
		}
		fragmentedType := payload[1] & naluTypeBitmask
		if err := validateFragmentedType(fragmentedType); err != nil {
			return err
		}

		return nil

	default:
		return fmt.Errorf("%w: %d", errUnhandledNALUType, naluType)
	}
}

func validateStapAPayload(payload []byte) error {
	if len(payload) <= stapaHeaderSize {
		return fmt.Errorf("%w: empty STAP-A", errInvalidAccessUnit)
	}

	offset := stapaHeaderSize
	for offset < len(payload) {
		if len(payload)-offset < stapaNALULengthSize {
			return fmt.Errorf("%w: truncated STAP-A length", errShortPacket)
		}
		naluSize := int(binary.BigEndian.Uint16(payload[offset:]))
		offset += stapaNALULengthSize

		if naluSize == 0 {
			return fmt.Errorf("%w: empty STAP-A entry", errInvalidAccessUnit)
		}
		if naluSize > len(payload)-offset {
			return fmt.Errorf("%w: STAP-A entry %d > %d", errShortPacket, naluSize, len(payload)-offset)
		}

		entryType := payload[offset] & naluTypeBitmask
		// An AUD must never hide inside an aggregation packet, and nested
		// aggregation/fragmentation packets are not valid STAP-A entries.
		if entryType == 0 || entryType == audNALUType ||
			entryType == stapaNALUType || entryType == fuaNALUType || entryType == fubNALUType ||
			entryType > 23 {
			return fmt.Errorf("%w: STAP-A entry type %d", errInvalidAccessUnit, entryType)
		}

		offset += naluSize
	}

	return nil
}

func validateFragmentedType(fragmentedType uint8) error {
	if fragmentedType == 0 || fragmentedType == audNALUType ||
		fragmentedType >= stapaNALUType {
		return fmt.Errorf("%w: FU-A NALU type %d", errInvalidAccessUnit, fragmentedType)
	}

	return nil
}

// h264PayloadAnnexBSize reports how many bytes this payload contributes to the
// Annex B representation of its access unit.
func h264PayloadAnnexBSize(payload []byte) int {
	naluType := payload[0] & naluTypeBitmask
	switch naluType {
	case stapaNALUType:
		size := 0
		offset := stapaHeaderSize
		for offset < len(payload) {
			if len(payload)-offset < stapaNALULengthSize {
				break
			}
			naluSize := int(binary.BigEndian.Uint16(payload[offset:]))
			offset += stapaNALULengthSize
			if naluSize > len(payload)-offset {
				break
			}
			size += len(annexbNALUStartCode) + naluSize
			offset += naluSize
		}

		return size

	case fuaNALUType:
		size := len(payload) - fuaHeaderSize
		if payload[1]&fuStartBitmask != 0 {
			// The reconstructed NAL header and its Annex B start code.
			size += len(annexbNALUStartCode) + 1
		}

		return size

	default:
		return len(annexbNALUStartCode) + len(payload)
	}
}

// h264UniquePacket is a deduplicated packet inside one access unit.
type h264UniquePacket struct {
	sequenceNumber uint16
	payload        []byte
	marker         bool
}

// UnmarshalAccessUnit validates the complete, ordered access unit in packets
// and rebuilds it according to IsAVC. Packets may be supplied in any order;
// duplicates with identical contents are tolerated. The receiver's streaming
// FU-A buffer is never touched, so a failed call cannot corrupt Unmarshal
// state.
func (p *H264Packet) UnmarshalAccessUnit(packets []*rtp.Packet) ([]byte, error) { //nolint:cyclop,gocognit
	if len(packets) == 0 {
		return nil, fmt.Errorf("%w: no packets", errInvalidAccessUnit)
	}

	var (
		ssrc      uint32
		payloadTy uint8
		timestamp uint32
	)
	for i, packet := range packets {
		if packet == nil {
			return nil, fmt.Errorf("%w: nil packet", errInvalidAccessUnit)
		}
		if err := validateH264PayloadStructure(packet.Payload); err != nil {
			return nil, err
		}
		if i == 0 {
			ssrc = packet.SSRC
			payloadTy = packet.PayloadType
			timestamp = packet.Timestamp
		} else if packet.SSRC != ssrc || packet.PayloadType != payloadTy || packet.Timestamp != timestamp {
			return nil, fmt.Errorf("%w: inconsistent RTP fields", errInvalidAccessUnit)
		}
	}

	// Deduplicate by sequence number, rejecting conflicting duplicates.
	unique := make(map[uint16]h264UniquePacket, len(packets))
	for _, packet := range packets {
		existing, ok := unique[packet.SequenceNumber]
		if ok {
			if existing.marker != packet.Marker || !bytes.Equal(existing.payload, packet.Payload) {
				return nil, fmt.Errorf("%w: conflicting duplicate sequence %d",
					errInvalidAccessUnit, packet.SequenceNumber)
			}

			continue
		}
		unique[packet.SequenceNumber] = h264UniquePacket{
			sequenceNumber: packet.SequenceNumber,
			payload:        packet.Payload,
			marker:         packet.Marker,
		}
	}

	ordered := make([]h264UniquePacket, 0, len(unique))
	anchor := packets[0].SequenceNumber
	for _, packet := range unique {
		ordered = append(ordered, packet)
	}
	sort.Slice(ordered, func(i, j int) bool {
		return int(int16(ordered[i].sequenceNumber-anchor)) < int(int16(ordered[j].sequenceNumber-anchor))
	})

	// The sequence numbers must be exactly contiguous after sorting.
	firstOffset := int(int16(ordered[0].sequenceNumber - anchor))
	for i, packet := range ordered {
		if int(int16(packet.sequenceNumber-anchor))-firstOffset != i {
			return nil, fmt.Errorf("%w: non-contiguous sequence at %d", errInvalidAccessUnit, packet.sequenceNumber)
		}
	}

	if err := validateOrderedAccessUnit(ordered); err != nil {
		return nil, err
	}

	// Reuse the production depacketizer. A fresh instance guarantees that a
	// failed batch never disturbs the caller's streaming FU-A buffer.
	isAVC := p != nil && p.IsAVC
	depacketizer := &H264Packet{IsAVC: isAVC}
	var annexB []byte
	for _, packet := range ordered {
		chunk, err := depacketizer.Unmarshal(packet.payload)
		if err != nil {
			return nil, fmt.Errorf("%w: %v", errInvalidAccessUnit, err)
		}
		if len(annexB)+len(chunk) > maxH264AccessUnitSize {
			return nil, fmt.Errorf("%w: > %d bytes", errH264AccessUnitTooLarge, maxH264AccessUnitSize)
		}
		annexB = append(annexB, chunk...)
	}

	return annexB, nil
}

func validateOrderedAccessUnit(packets []h264UniquePacket) error {
	last := len(packets) - 1

	firstType := packets[0].payload[0] & naluTypeBitmask
	if firstType != audNALUType {
		return fmt.Errorf("%w: access unit does not start with AUD", errInvalidAccessUnit)
	}

	openFU := false
	var fuType uint8
	markerCount := 0

	for i, packet := range packets {
		if packet.marker {
			markerCount++
			if i != last {
				return fmt.Errorf("%w: marker before end of access unit", errInvalidAccessUnit)
			}
		}

		naluType := packet.payload[0] & naluTypeBitmask
		switch naluType {
		case audNALUType:
			if i != 0 {
				return fmt.Errorf("%w: multiple AUD NAL units", errInvalidAccessUnit)
			}

		case fuaNALUType:
			startBit := packet.payload[1]&fuStartBitmask != 0
			endBit := packet.payload[1]&fuEndBitmask != 0

			if openFU {
				if startBit {
					return fmt.Errorf("%w: overlapping FU-A fragments", errInvalidAccessUnit)
				}
				if fragmentedType := packet.payload[1] & naluTypeBitmask; fragmentedType != fuType {
					return fmt.Errorf("%w: FU-A type changed %d -> %d",
						errInvalidAccessUnit, fuType, fragmentedType)
				}
				if endBit {
					openFU = false
				}

				continue
			}

			if !startBit {
				return fmt.Errorf("%w: FU-A continuation without start", errInvalidAccessUnit)
			}
			fuType = packet.payload[1] & naluTypeBitmask
			openFU = !endBit

		default:
			if openFU {
				return fmt.Errorf("%w: non-FU-A packet inside FU-A", errInvalidAccessUnit)
			}
		}
	}

	if openFU {
		return fmt.Errorf("%w: unterminated FU-A", errInvalidAccessUnit)
	}
	if markerCount != 1 || !packets[last].marker {
		return fmt.Errorf("%w: missing end marker", errInvalidAccessUnit)
	}

	return nil
}

// h264AccessUnitPictureTypes reports whether the access unit carries a VCL
// NAL and whether it carries an IDR picture (VCL type 5).
func h264AccessUnitPictureTypes(packets []*rtp.Packet) (bool, bool) {
	hasVCL := false
	hasIDR := false

	for _, packet := range packets {
		payload := packet.Payload
		naluType := payload[0] & naluTypeBitmask

		switch naluType {
		case stapaNALUType:
			offset := stapaHeaderSize
			for offset < len(payload) {
				if len(payload)-offset < stapaNALULengthSize {
					break
				}
				naluSize := int(binary.BigEndian.Uint16(payload[offset:]))
				offset += stapaNALULengthSize
				if naluSize > len(payload)-offset {
					break
				}
				entryType := payload[offset] & naluTypeBitmask
				if entryType >= 1 && entryType <= 5 {
					hasVCL = true
					if entryType == 5 {
						hasIDR = true
					}
				}
				offset += naluSize
			}

		case fuaNALUType:
			if len(payload) >= fuaHeaderSize {
				fragmentedType := payload[1] & naluTypeBitmask
				if fragmentedType >= 1 && fragmentedType <= 5 {
					hasVCL = true
					if fragmentedType == 5 {
						hasIDR = true
					}
				}
			}

		default:
			if naluType >= 1 && naluType <= 5 {
				hasVCL = true
				if naluType == 5 {
					hasIDR = true
				}
			}
		}
	}

	return hasVCL, hasIDR
}
