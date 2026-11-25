// SPDX-FileCopyrightText: 2026 The Pion community <https://pion.ly>
// SPDX-License-Identifier: MIT

package rtcp

import (
	"errors"
	"sort"
	"time"
)

const (
	// receptionMinSequential counts the trial opener plus one consecutive
	// packet; probation tracks how many more consecutive packets are needed.
	receptionMinSequential   = 2
	receptionMaxDropout      = 3000
	receptionMaxMisorder     = 100
	receptionMaxReportBlocks = 31
	receptionSeqMod          = 1 << 16
	receptionBadSeqSentinel  = receptionSeqMod + 1
	receptionNTPMiddleMask   = uint64(0xFFFFFFFF)
	receptionDLSRScale       = 65536
)

var (
	errReceptionClockRateZero = errors.New("rtcp: reception session clock rate must be non-zero")
	errReceptionCNAMELength   = errors.New("rtcp: reception session CNAME must be 1 to 255 octets")
	errReceptionBudget        = errors.New("rtcp: compound report does not fit the byte budget")
)

// ReceptionSession maintains per-source RTP reception statistics and emits
// RTCP compound reports built from them. It is not safe for concurrent use.
type ReceptionSession struct {
	localSSRC uint32
	cname     string
	clockRate uint32
	sources   map[uint32]*receptionSource
	// cursor is the last SSRC reported in a successful Report; selection of
	// the next Report starts at its strict numeric successor.
	cursor    uint32
	hasCursor bool
}

// receptionSource holds the RFC 3550 appendix A state of one remote SSRC.
type receptionSource struct {
	ssrc uint32

	// Validation state (appendix A.1).
	probation int
	trialSeq  uint16
	maxSeq    uint16
	cycles    uint32
	badSeq    uint32
	started   bool // probation finished, statistics are valid

	// Statistics.
	baseSeq  uint16
	received uint32
	jitter   uint32

	// Jitter phase (appendix A.8).
	lastArrivalTicks int64
	lastRTPTime      uint32
	phaseSet         bool

	// Interval reference for appendix A.3 loss computation.
	intervalExtendedMax uint32
	intervalReceived    uint32
	hasInterval         bool

	// Latest sender report observed for this source.
	lsr       uint32
	srArrival time.Duration
	hasSR     bool
}

// NewReceptionSession creates a reception session that identifies itself with
// localSSRC and cname. clockRate is the RTP clock rate shared by every remote
// source and must be non-zero; cname must be 1-255 bytes long.
func NewReceptionSession(localSSRC uint32, cname string, clockRate uint32) (*ReceptionSession, error) {
	if clockRate == 0 {
		return nil, errReceptionClockRateZero
	}
	if len(cname) == 0 || len(cname) > sdesMaxOctetCount {
		return nil, errReceptionCNAMELength
	}

	return &ReceptionSession{
		localSSRC: localSSRC,
		cname:     cname,
		clockRate: clockRate,
		sources:   make(map[uint32]*receptionSource),
	}, nil
}

// ObserveRTP validates an incoming RTP packet following RFC 3550 appendix A.1
// and updates the reception statistics of its source when accepted. It
// reports whether the packet was accepted.
//
//nolint:cyclop
func (s *ReceptionSession) ObserveRTP(ssrc uint32, seq uint16, timestamp uint32, arrival time.Duration) bool {
	src := s.sources[ssrc]
	if src == nil {
		src = &receptionSource{ssrc: ssrc, badSeq: receptionBadSeqSentinel}
		s.sources[ssrc] = src
	}

	if !src.started {
		return s.observeRTPProbation(src, seq, timestamp, arrival)
	}

	udelta := seq - src.maxSeq
	switch {
	case udelta == 0:
		// Duplicate of the highest sequence number: accept it.
	case udelta < receptionMaxDropout:
		// In order, allowing a gap within MAX_DROPOUT.
		if seq < src.maxSeq {
			src.cycles += receptionSeqMod
		}
		src.maxSeq = seq
		src.badSeq = receptionBadSeqSentinel
	case udelta > receptionSeqMod-receptionMaxMisorder:
		// Late or reordered packet within MAX_MISORDER behind the top: count
		// it without advancing the cycle or the highest sequence number.
	default:
		// A very large jump: reject first, confirm a restart only once the
		// immediately following sequence number arrives.
		if src.badSeq != receptionBadSeqSentinel &&
			seq == uint16(src.badSeq) { //nolint:gosec // G115 badSeq is a mod-65536 sequence number
			// Restart confirmed: the confirming packet is the fresh base and
			// all previous cycles, counters, intervals, jitter and SRs drop.
			src.reset(seq)
			src.received++
			src.establishPhase(timestamp, s.arrivalTicks(arrival))
			return true
		}
		if src.badSeq == receptionBadSeqSentinel {
			src.badSeq = uint32(seq) + 1
		}
		return false
	}

	src.received++
	src.updateJitter(timestamp, s.arrivalTicks(arrival))
	return true
}

// observeRTPProbation drives the MIN_SEQUENTIAL validation trial for a source
// that has not finished probation. The trial opener is rejected and only the
// immediately following consecutive packet establishes the statistics base.
func (s *ReceptionSession) observeRTPProbation(
	src *receptionSource, seq uint16, timestamp uint32, arrival time.Duration,
) bool {
	if src.probation == 0 {
		src.probation = receptionMinSequential
		src.trialSeq = seq
		return false
	}
	if seq != src.trialSeq+1 {
		// The trial broke; this packet starts a new one.
		src.trialSeq = seq
		return false
	}

	// The second of two consecutive packets is the statistics base and is
	// the first packet counted as received.
	src.reset(seq)
	src.received++
	src.establishPhase(timestamp, s.arrivalTicks(arrival))
	return true
}

// reset clears every statistic and makes seq the new base sequence number.
func (src *receptionSource) reset(seq uint16) {
	src.probation = 0
	src.trialSeq = 0
	src.baseSeq = seq
	src.maxSeq = seq
	src.cycles = 0
	src.badSeq = receptionBadSeqSentinel
	src.received = 0
	src.jitter = 0
	src.lastArrivalTicks = 0
	src.lastRTPTime = 0
	src.phaseSet = false
	src.hasInterval = false
	src.intervalExtendedMax = 0
	src.intervalReceived = 0
	src.lsr = 0
	src.srArrival = 0
	src.hasSR = false
	src.started = true
}

// arrivalTicks converts a monotone arrival duration into RTP clock ticks.
func (s *ReceptionSession) arrivalTicks(arrival time.Duration) int64 {
	return arrival.Nanoseconds() * int64(s.clockRate) / int64(time.Second)
}

// establishPhase stores the phase of the first accepted packet of a cycle;
// jitter itself remains zero until the next accepted packet.
func (src *receptionSource) establishPhase(timestamp uint32, arrivalTicks int64) {
	src.lastRTPTime = timestamp
	src.lastArrivalTicks = arrivalTicks
	src.phaseSet = true
}

// updateJitter applies the RFC 3550 appendix A.8 Q4 integer estimate. Both
// the RTP timestamp difference and the phase difference use the shortest
// signed difference modulo 2^32.
func (src *receptionSource) updateJitter(timestamp uint32, arrivalTicks int64) {
	if !src.phaseSet {
		src.establishPhase(timestamp, arrivalTicks)
		return
	}

	rtpDiff := int64(int32(timestamp - src.lastRTPTime))
	arrivalDiff := arrivalTicks - src.lastArrivalTicks
	diff := arrivalDiff - rtpDiff
	if diff < 0 {
		diff = -diff
	}

	// Integer division truncates toward zero, matching the reference C code.
	src.jitter = uint32(int64(src.jitter) + (diff-int64(src.jitter))/16) //nolint:gosec // G115 result stays non-negative
	src.lastRTPTime = timestamp
	src.lastArrivalTicks = arrivalTicks
}

// extendedMax returns the extended highest sequence number received.
func (src *receptionSource) extendedMax() uint32 {
	return src.cycles + uint32(src.maxSeq)
}

// ObserveRTCP parses a complete RTCP datagram and applies its SR and BYE
// packets in order. Unmarshal is all-or-nothing, so a parse error never
// partially updates the session; other packet types are ignored.
func (s *ReceptionSession) ObserveRTCP(raw []byte, arrival time.Duration) error {
	packets, err := Unmarshal(raw)
	if err != nil {
		return err
	}

	for _, pkt := range packets {
		switch p := pkt.(type) {
		case *SenderReport:
			src := s.sources[p.SSRC]
			// SRs only attach to known sources that finished probation.
			if src == nil || !src.started {
				continue
			}
			src.lsr = uint32((p.NTPTime >> 16) & receptionNTPMiddleMask) //nolint:gosec // G115
			src.srArrival = arrival
			src.hasSR = true
		case *Goodbye:
			for _, ssrc := range p.Sources {
				delete(s.sources, ssrc)
			}
		default:
			// Other packet types are ignored.
		}
	}
	return nil
}

// Report builds a compound RTCP packet containing exactly one ReceiverReport
// followed by a SourceDescription carrying the local CNAME. Sources are
// picked round-robin among those that finished probation, within maxBytes and
// the 31-block limit. Failure never advances the cursor or any interval.
func (s *ReceptionSession) Report(now time.Duration, maxBytes int) ([]byte, error) {
	sdes := NewCNAMESourceDescription(s.localSSRC, s.cname)
	baseSize := (&ReceiverReport{SSRC: s.localSSRC}).MarshalSize() + sdes.MarshalSize()

	ready := s.readySSRCs()
	if len(ready) == 0 {
		// An empty RR is a valid report when nothing is ready yet.
		if maxBytes < baseSize {
			return nil, errReceptionBudget
		}
		return s.emit(nil, sdes, nil)
	}

	if maxBytes < baseSize+receptionReportLength {
		// Sources are waiting but even one block cannot fit.
		return nil, errReceptionBudget
	}

	limit := (maxBytes - baseSize) / receptionReportLength
	if limit > receptionMaxReportBlocks {
		limit = receptionMaxReportBlocks
	}
	selected := s.selectSSRCs(ready, limit)

	blocks := make([]ReceptionReport, 0, len(selected))
	for _, ssrc := range selected {
		blocks = append(blocks, s.sources[ssrc].buildReportBlock(now))
	}

	return s.emit(selected, sdes, blocks)
}

// readySSRCs returns the SSRCs that finished probation, in ascending order.
func (s *ReceptionSession) readySSRCs() []uint32 {
	out := make([]uint32, 0, len(s.sources))
	for ssrc, src := range s.sources {
		if src.started {
			out = append(out, ssrc)
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i] < out[j] })
	return out
}

// selectSSRCs walks the sorted ready sources starting strictly after the
// cursor and cycles back to the beginning, never repeating a source.
func (s *ReceptionSession) selectSSRCs(ready []uint32, limit int) []uint32 {
	start := 0
	if s.hasCursor {
		start = sort.Search(len(ready), func(i int) bool { return ready[i] > s.cursor })
	}
	selected := make([]uint32, 0, limit)
	for offset := 0; offset < len(ready) && len(selected) < limit; offset++ {
		selected = append(selected, ready[(start+offset)%len(ready)])
	}
	return selected
}

// emit marshals the compound packet and only advances state on success.
func (s *ReceptionSession) emit(selected []uint32, sdes *SourceDescription, blocks []ReceptionReport) ([]byte, error) {
	rr := &ReceiverReport{SSRC: s.localSSRC, Reports: blocks}
	data, err := CompoundPacket{rr, sdes}.Marshal()
	if err != nil {
		return nil, err
	}

	for i, ssrc := range selected {
		src := s.sources[ssrc]
		src.intervalExtendedMax = src.extendedMax()
		src.intervalReceived = src.received
		src.hasInterval = true
		if i == len(selected)-1 {
			s.cursor = ssrc
			s.hasCursor = true
		}
	}
	return data, nil
}

// buildReportBlock computes the reception report block for one source,
// including the appendix A.3 loss figures and the SR timing fields.
func (src *receptionSource) buildReportBlock(now time.Duration) ReceptionReport {
	extended := src.extendedMax()

	fractionLost := uint8(0)
	if src.hasInterval {
		expectedInterval := extended - src.intervalExtendedMax
		receivedInterval := src.received - src.intervalReceived
		lostInterval := int64(expectedInterval) - int64(receivedInterval)
		if expectedInterval > 0 && lostInterval > 0 {
			fraction := lostInterval * 256 / int64(expectedInterval)
			if fraction > 255 {
				fraction = 255
			}
			fractionLost = uint8(fraction) //nolint:gosec // G115 clamped to 255 above
		}
	}

	// Cumulative lost since the base, clamped to signed 24-bit and stored as
	// the low 24 bits of its two's complement representation.
	totalLost := int64(extended) - int64(src.baseSeq) + 1 - int64(src.received)
	const maxSigned24 = 1<<23 - 1
	const minSigned24 = -(1 << 23)
	if totalLost > maxSigned24 {
		totalLost = maxSigned24
	} else if totalLost < minSigned24 {
		totalLost = minSigned24
	}

	block := ReceptionReport{
		SSRC:               src.ssrc,
		FractionLost:       fractionLost,
		TotalLost:          uint32(int32(totalLost)) & 0xFFFFFF, //nolint:gosec // G115
		LastSequenceNumber: extended,
		Jitter:             src.jitter,
	}

	if src.hasSR {
		// A zero LSR is still a valid SR: DLSR must be timed regardless.
		block.LastSenderReport = src.lsr
		scaled := (now - src.srArrival).Nanoseconds() * receptionDLSRScale / int64(time.Second)
		if scaled >= int64(^uint32(0)) {
			block.Delay = ^uint32(0)
		} else if scaled > 0 {
			block.Delay = uint32(scaled) //nolint:gosec // G115 saturation checked above
		}
	}
	return block
}
