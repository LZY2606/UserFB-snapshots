// SPDX-FileCopyrightText: 2026 The Pion community <https://pion.ly>
// SPDX-License-Identifier: MIT

package rtcp

import (
	"cmp"
	"errors"
	"math"
	"slices"
	"time"
)

const (
	// RFC 3550 appendix A.1 sequence number validation thresholds.
	minSequential = 2
	maxDropout    = 3000
	maxMisorder   = 100

	// At most 31 report blocks fit in a single Receiver Report header.
	maxReportBlocks = countMax

	// Fraction lost is an 8-bit fixed point field.
	maxFractionLost = 255

	// Cumulative packets lost is carried as a signed 24-bit value.
	maxSigned24Bit = (1 << 23) - 1
	minSigned24Bit = -(1 << 23)
	mask24         = (1 << 24) - 1

	// DLSR is measured in units of 1/65536 seconds.
	dlsrScale = 1 << 16
)

var (
	errZeroClockRate      = errors.New("rtcp: reception session clock rate must be non-zero")
	errInvalidCNAMELength = errors.New("rtcp: reception session CNAME must be 1 to 255 octets")
	errReportBudget       = errors.New("rtcp: report budget cannot hold a single report block")
	errInvalidMaxBytes    = errors.New("rtcp: maxBytes cannot hold the report header and CNAME")
)

// ReceptionSession maintains RTP reception statistics for several remote
// sources sharing one RTP clock rate and produces RTCP compound reports built
// from ReceiverReport and SourceDescription packets.
//
// All durations are non-negative monotonic offsets from a single origin; the
// caller is responsible for feeding observations in non-decreasing order.
type ReceptionSession struct {
	localSSRC uint32
	cname     string
	clockRate uint32

	sources map[uint32]*receptionSource
	// cursor is the SSRC after which report source selection starts; the first
	// report selects sources in ascending SSRC order.
	cursor    uint32
	hasCursor bool
}

// receptionSource holds the per-SSRC reception state, following the variables
// defined in RFC 3550 appendices A.1, A.3 and A.8.
type receptionSource struct {
	ssrc uint32

	// probation counts the consecutive sequence numbers still required before
	// the source is accepted; zero means the source has completed its trial.
	probation int
	// candidate is the low 16-bit sequence number the trial is anchored on.
	candidate uint32

	// Sequence number tracking (appendix A.1).
	baseSeq uint32
	maxSeq  uint32
	cycles  uint32

	// Total accepted packets, including late packets and duplicates.
	received uint32

	// Interarrival jitter in RTP timestamp units (appendix A.8).
	jitter      uint32
	lastTransit int64
	haveTransit bool

	// Interval loss baseline (appendix A.3).
	intervalExpected uint32
	intervalReceived uint32
	haveInterval     bool

	// Last sender report timing.
	lsr       uint32
	srArrival time.Duration
	haveSR    bool
}

// NewReceptionSession creates a ReceptionSession reporting as localSSRC with
// the given canonical name. clockRate is the shared RTP clock rate, in ticks
// per second, of every observed remote source and must be non-zero.
func NewReceptionSession(localSSRC uint32, cname string, clockRate uint32) (*ReceptionSession, error) {
	if clockRate == 0 {
		return nil, errZeroClockRate
	}
	if cnameLen := len([]byte(cname)); cnameLen < 1 || cnameLen > sdesMaxOctetCount {
		return nil, errInvalidCNAMELength
	}

	return &ReceptionSession{
		localSSRC: localSSRC,
		cname:     cname,
		clockRate: clockRate,
		sources:   make(map[uint32]*receptionSource),
	}, nil
}

// ObserveRTP validates and records one received RTP packet following the
// sequence number rules of RFC 3550 appendix A.1 and reports whether the
// packet was accepted into the reception statistics.
func (s *ReceptionSession) ObserveRTP(ssrc uint32, seq uint16, timestamp uint32, arrival time.Duration) bool {
	source := s.sources[ssrc]
	if source == nil {
		// The first packet anchors the trial and is never itself accepted.
		s.sources[ssrc] = newReceptionSource(ssrc, seq)
		return false
	}

	if source.probation > 0 {
		return source.observeProbation(seq, timestamp, arrival, s.clockRate)
	}

	// Signed 16-bit modular difference, so a 16-bit wrap yields a small delta:
	// the first sequence after the wrap looks +1 ahead, while a late packet
	// from the previous cycle looks a small negative number behind.
	udelta := int32(int16(seq - uint16(source.maxSeq))) //nolint:gosec // G115

	switch {
	case udelta < -maxMisorder:
		// A modular jump too far backwards to be reordering: the source may
		// have restarted, so reopen the trial.
		source.beginJumpTrial(seq)
		return false
	case udelta < 0:
		// Late packet inside the misorder window: accepted without advancing
		// maxSeq or the cycle counter. A zero delta is a duplicate and takes
		// the in-order branch below, where nothing moves anyway.
	case udelta <= maxDropout:
		if seq < uint16(source.maxSeq) {
			source.cycles += 1 << 16
		}
		source.maxSeq = uint32(seq)
	default:
		// The forward jump is too large to trust: reject it and reopen the
		// trial without touching any accumulated statistics.
		source.beginJumpTrial(seq)
		return false
	}

	source.received++
	source.updateJitter(timestamp, arrival, s.clockRate)

	return true
}

// senderReportInfo captures the LSR and arrival time taken from one SR.
type senderReportInfo struct {
	lsr     uint32
	arrival time.Duration
}

// ObserveRTCP parses one complete RTCP datagram and applies its SenderReport
// and Goodbye packets in order. Any parse failure leaves the session untouched.
func (s *ReceptionSession) ObserveRTCP(raw []byte, arrival time.Duration) error {
	packets, err := Unmarshal(raw)
	if err != nil {
		return err
	}

	// The datagram is fully parsed above, so applying the collected effects can
	// no longer fail and leave the session partially updated.
	srUpdates, byeSources := collectRTCPEffects(packets, arrival)

	// Apply SR updates before removals so an SR followed by a BYE for the same
	// SSRC ends with the source gone, as the packet order dictates.
	for ssrc, info := range srUpdates {
		source := s.sources[ssrc]
		if source == nil || source.probation > 0 {
			continue
		}
		source.lsr = info.lsr
		source.srArrival = info.arrival
		source.haveSR = true
	}
	for _, ssrc := range byeSources {
		delete(s.sources, ssrc)
	}

	return nil
}

// collectRTCPEffects walks the parsed packets in order. A later BYE for a SSRC
// cancels an earlier SR for it, matching the on-the-wire packet order.
func collectRTCPEffects(packets []Packet, arrival time.Duration) (map[uint32]senderReportInfo, []uint32) {
	srUpdates := make(map[uint32]senderReportInfo)
	byeSources := make([]uint32, 0)
	haveBye := make(map[uint32]struct{})

	for _, packet := range packets {
		switch p := packet.(type) {
		case *SenderReport:
			if _, gone := haveBye[p.SSRC]; gone {
				continue
			}
			srUpdates[p.SSRC] = senderReportInfo{
				lsr:     uint32(p.NTPTime >> 16), //nolint:gosec // G115 middle 32 of 64 bits
				arrival: arrival,
			}
		case *Goodbye:
			for _, ssrc := range p.Sources {
				delete(srUpdates, ssrc)
				if _, seen := haveBye[ssrc]; !seen {
					haveBye[ssrc] = struct{}{}
					byeSources = append(byeSources, ssrc)
				}
			}
		}
	}

	return srUpdates, byeSources
}

// Report builds one compound RTCP packet containing a single ReceiverReport and
// a SourceDescription carrying the local CNAME, never exceeding maxBytes.
// Interval baselines and the selection cursor only advance once the full
// compound has been encoded successfully.
func (s *ReceptionSession) Report(now time.Duration, maxBytes int) ([]byte, error) {
	sdes := NewCNAMESourceDescription(s.localSSRC, s.cname)
	headerSize := (&ReceiverReport{SSRC: s.localSSRC}).MarshalSize() + sdes.MarshalSize()
	if maxBytes < headerSize {
		return nil, errInvalidMaxBytes
	}

	ready := s.readySources()
	if len(ready) == 0 {
		return s.marshalReport(nil, now)
	}

	selected := make([]*receptionSource, 0, maxReportBlocks)
	for _, source := range ready {
		if len(selected) == maxReportBlocks {
			break
		}
		if headerSize+(len(selected)+1)*receptionReportLength > maxBytes {
			break
		}
		selected = append(selected, source)
	}
	if len(selected) == 0 {
		return nil, errReportBudget
	}

	return s.marshalReport(selected, now)
}

// marshalReport encodes the compound and, only on success, commits the
// interval baselines and the rotation cursor.
func (s *ReceptionSession) marshalReport(selected []*receptionSource, now time.Duration) ([]byte, error) {
	reports := make([]ReceptionReport, len(selected))
	for i, source := range selected {
		reports[i] = source.buildReport(now)
	}

	rr := &ReceiverReport{SSRC: s.localSSRC, Reports: reports}
	sdes := NewCNAMESourceDescription(s.localSSRC, s.cname)
	compound := CompoundPacket{rr, sdes}

	raw, err := compound.Marshal()
	if err != nil {
		return nil, err
	}

	for _, source := range selected {
		source.commitInterval()
	}
	if len(selected) > 0 {
		s.cursor = selected[len(selected)-1].ssrc
		s.hasCursor = true
	}

	return raw, nil
}

// readySources returns the validated sources in selection order: starting just
// after the cursor, wrapping around, sorted by SSRC within each wrap.
func (s *ReceptionSession) readySources() []*receptionSource {
	ready := make([]*receptionSource, 0, len(s.sources))
	for _, source := range s.sources {
		if source.probation == 0 {
			ready = append(ready, source)
		}
	}
	slices.SortFunc(ready, func(a, b *receptionSource) int {
		return cmp.Compare(a.ssrc, b.ssrc)
	})

	if !s.hasCursor {
		return ready
	}

	first, _ := slices.BinarySearchFunc(ready, s.cursor, func(source *receptionSource, cursor uint32) int {
		return cmp.Compare(source.ssrc, cursor+1)
	})
	if first == len(ready) {
		first = 0
	}

	return append(ready[first:], ready[:first]...)
}

// newReceptionSource opens a trial for a SSRC seen for the first time. The
// second of two consecutive sequence numbers becomes the statistics base.
func newReceptionSource(ssrc uint32, seq uint16) *receptionSource {
	return &receptionSource{
		ssrc:      ssrc,
		probation: minSequential - 1,
		candidate: uint32(seq),
	}
}

// beginJumpTrial rejects an out-of-range sequence number and reopens the
// probation trial anchored on it, leaving live statistics untouched until the
// next sequence number confirms a restart.
func (r *receptionSource) beginJumpTrial(seq uint16) {
	r.probation = minSequential - 1
	r.candidate = uint32(seq)
}

// observeProbation handles packets while the source still has to prove itself
// with consecutive sequence numbers, and confirms a detected restart.
func (r *receptionSource) observeProbation(
	seq uint16,
	timestamp uint32,
	arrival time.Duration,
	clockRate uint32,
) bool {
	if seq != uint16(r.candidate)+1 {
		// The trial broke: start over from this packet.
		r.probation = minSequential - 1
		r.candidate = uint32(seq)
		return false
	}

	r.probation--
	r.candidate = uint32(seq)
	if r.probation > 0 {
		return false
	}

	// This confirming packet is the new statistics base point.
	r.confirm(seq, timestamp, arrival, clockRate)
	return true
}

// confirm resets every statistic when a source completes its trial, including
// a restart confirmed after an oversized sequence jump.
func (r *receptionSource) confirm(seq uint16, timestamp uint32, arrival time.Duration, clockRate uint32) {
	r.baseSeq = uint32(seq)
	r.maxSeq = uint32(seq)
	r.cycles = 0
	r.received = 1
	r.jitter = 0
	r.haveTransit = false
	r.updateJitter(timestamp, arrival, clockRate)
	r.intervalExpected = 1
	r.intervalReceived = 1
	r.haveInterval = true
	r.lsr = 0
	r.haveSR = false
	r.srArrival = 0
}

// updateJitter applies the Q4 fixed point interarrival jitter estimate from
// RFC 3550 appendix A.8. The first accepted packet only establishes the phase.
func (r *receptionSource) updateJitter(timestamp uint32, arrival time.Duration, clockRate uint32) {
	transit := durationToTicks(arrival, clockRate) - int64(int32(timestamp)) //nolint:gosec // G115
	if !r.haveTransit {
		r.lastTransit = transit
		r.haveTransit = true
		return
	}

	delta := transit - r.lastTransit
	if delta < 0 {
		delta = -delta
	}
	r.jitter += uint32((delta - int64(r.jitter)) / 16) //nolint:gosec // G115
	r.lastTransit = transit
}

// durationToTicks converts a monotonic duration to RTP clock ticks without
// overflowing the intermediate product.
func durationToTicks(arrival time.Duration, clockRate uint32) int64 {
	whole := int64(arrival / time.Second)
	fraction := arrival % time.Second

	return whole*int64(clockRate) + fraction.Nanoseconds()*int64(clockRate)/int64(time.Second)
}

// extendedHighest returns the extended highest sequence number (A.3).
func (r *receptionSource) extendedHighest() uint32 {
	return r.cycles + r.maxSeq
}

// expected returns the number of packets expected since reception began.
func (r *receptionSource) expected() uint32 {
	return r.extendedHighest() - r.baseSeq + 1
}

// buildReport assembles one reception report block from the current state.
func (r *receptionSource) buildReport(now time.Duration) ReceptionReport {
	expected := r.expected()
	totalLost := clamp24(int64(expected) - int64(r.received))

	report := ReceptionReport{
		SSRC:               r.ssrc,
		TotalLost:          packSigned24(int32(totalLost)), //nolint:gosec // G115
		LastSequenceNumber: r.extendedHighest(),
		Jitter:             r.jitter,
	}

	if r.haveInterval {
		expectedInterval := int64(expected) - int64(r.intervalExpected)
		receivedInterval := int64(r.received) - int64(r.intervalReceived)
		lostInterval := expectedInterval - receivedInterval
		if expectedInterval > 0 && lostInterval > 0 {
			fraction := lostInterval * maxFractionLost / expectedInterval
			if fraction > maxFractionLost {
				fraction = maxFractionLost
			}
			report.FractionLost = uint8(fraction) //nolint:gosec // G115
		}
	}

	if r.haveSR {
		report.LastSenderReport = r.lsr
		report.Delay = dlsr(now - r.srArrival)
	}

	return report
}

// commitInterval advances the appendix A.3 interval baseline after a report
// block for this source has been sent successfully.
func (r *receptionSource) commitInterval() {
	r.intervalExpected = r.expected()
	r.intervalReceived = r.received
	r.haveInterval = true
}

// dlsr converts the delay since the last sender report into 1/65536 second
// units, saturating at the uint32 maximum.
func dlsr(delay time.Duration) uint32 {
	if delay < 0 {
		delay = 0
	}
	scaled := delay.Nanoseconds() * dlsrScale / int64(time.Second)
	if scaled > math.MaxUint32 {
		return math.MaxUint32
	}

	return uint32(scaled) //nolint:gosec // G115
}

// clamp24 constrains cumulative loss to the signed 24-bit range.
func clamp24(value int64) int64 {
	if value > maxSigned24Bit {
		return maxSigned24Bit
	}
	if value < minSigned24Bit {
		return minSigned24Bit
	}

	return value
}

// packSigned24 encodes a signed 24-bit value as its two's complement low bits.
func packSigned24(value int32) uint32 {
	return uint32(value) & mask24 //nolint:gosec // G115
}
