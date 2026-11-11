// Package analyze turns a partitioned capture into reviewable evidence:
// feedback-to-sender causality candidates, RTT candidates, sender-report
// mappings and freeze intervals.
//
// The engine is deliberately non-committal about causality. When one feedback
// item could refer to more than one send event — an SSRC reused across
// generations, a transport sequence seen across overlapping TWCC messages —
// every candidate is retained. Wall-clock guessing is never used to collapse
// the set down to a single answer.
package analyze

import (
	"sort"

	"rtpfreeze/internal/identity"
	"rtpfreeze/internal/rtproto"
)

// Causality status values.
const (
	// StatusResolved: the reference maps to sent packet(s) unambiguously.
	StatusResolved = "resolved"
	// StatusAmbiguous: several generations or send events can explain it.
	StatusAmbiguous = "ambiguous"
	// StatusUnresolved: no sent packet matches the reference.
	StatusUnresolved = "unresolved"
	// StatusCrossEpoch: the only candidates live in another transport epoch
	// (for example a NACK for an SSRC that predates an ICE restart).
	StatusCrossEpoch = "cross_epoch"
)

// Feedback kinds.
const (
	KindNACK = "nack"
	KindPLI  = "pli"
	KindFIR  = "fir"
	KindTWCC = "twcc"
	KindRR   = "receiver_report"
	KindSR   = "sender_report"
)

// CausalityCandidate is one possible explanation of a feedback reference.
type CausalityCandidate struct {
	GenID            int    `json:"gen_id"`
	PacketIndex      int    `json:"packet_index"` // -1 when no sent packet matches
	TrackID          int    `json:"track_id"`
	TransportEpochID int    `json:"transport_epoch_id"`
	RawSeq           uint16 `json:"raw_seq"`
	ExtendedSeq      int64  `json:"extended_seq"` // -1 when unknown
	TransportSeq     uint16 `json:"transport_seq"`
	CrossEpoch       bool   `json:"cross_epoch"`
	Note             string `json:"note,omitempty"`
}

// FeedbackEvent is one RTCP feedback message with every candidate mapping the
// engine could derive.
type FeedbackEvent struct {
	PacketIndex      int                  `json:"packet_index"`
	MsgIndex         int                  `json:"msg_index"`
	TimeUS           int64                `json:"time_us"`
	Kind             string               `json:"kind"`
	SenderSSRC       uint32               `json:"sender_ssrc"`
	MediaSSRC        uint32               `json:"media_ssrc"`
	TransportEpochID int                  `json:"transport_epoch_id"`
	Status           string               `json:"status"`
	Candidates       []CausalityCandidate `json:"candidates"`
	// NACKSeqs holds the raw sequence numbers the feedback asserted as lost.
	NACKSeqs []uint16 `json:"nack_seqs,omitempty"`
	// TWCCSeqs holds transport sequences covered by the message.
	TWCCSeqs []uint16 `json:"twcc_seqs,omitempty"`
	// RTTUS/RTTNotes are aligned per report block. A zero RTT with a note
	// means the block could not be timed (LSR=0 or no matching SR).
	RTTUS    []int64  `json:"rtt_us,omitempty"`
	RTTNotes []string `json:"rtt_notes,omitempty"`
	Note     string   `json:"note,omitempty"`
}

// SRMapping is the NTP/RTP correspondence advertised by one sender report.
type SRMapping struct {
	PacketIndex  int    `json:"packet_index"`
	MsgIndex     int    `json:"msg_index"`
	GenID        int    `json:"gen_id"`
	SSRC         uint32 `json:"ssrc"`
	NTPMSW       uint32 `json:"ntp_msw"`
	NTPLSW       uint32 `json:"ntp_lsw"`
	NTPMiddle32  uint32 `json:"ntp_middle32"`
	NTPUS        int64  `json:"ntp_us"` // NTP instant in microseconds
	RTPTimestamp uint32 `json:"rtp_timestamp"`
	PacketCount  uint32 `json:"packet_count"`
	OctetCount   uint32 `json:"octet_count"`
}

// FreezeInterval is one stretch where a track had no decodable output.
type FreezeInterval struct {
	ID              int    `json:"id"`
	TrackID         int    `json:"track_id"`
	GenID           int    `json:"gen_id"`
	StartIndex      int    `json:"start_index"` // last packet before the freeze
	EndIndex        int    `json:"end_index"`   // first packet after it, -1 if open
	StartUS         int64  `json:"start_us"`
	EndUS           int64  `json:"end_us"`
	DurationUS      int64  `json:"duration_us"`
	Kind            string `json:"kind"` // loss_gap | silence
	FirstMissingExt int64  `json:"first_missing_ext"`
	LastMissingExt  int64  `json:"last_missing_ext"`
	MissingCount    int    `json:"missing_count"`
	// RelatedFeedback are positions in Analysis.Feedback that point at this
	// generation (or its SSRC) inside the freeze window.
	RelatedFeedback []int `json:"related_feedback"`
}

// Analysis is the complete engine output.
type Analysis struct {
	Feedback []FeedbackEvent  `json:"feedback"`
	SRs      []SRMapping      `json:"sender_reports"`
	Freezes  []FreezeInterval `json:"freezes"`
}

// FreezeParams controls freeze detection.
type FreezeParams struct {
	// MinMissing is the smallest sequence gap reported as a loss freeze.
	MinMissing int
	// MinSilenceUS is the minimum time gap between consecutive packets (even
	// with continuous sequence numbers) reported as a silence freeze.
	MinSilenceUS int64
}

// DefaultFreezeParams matches typical video frame cadence: two missing
// packets, or 250 ms without any packet on the generation.
func DefaultFreezeParams() FreezeParams {
	return FreezeParams{MinMissing: 2, MinSilenceUS: 250_000}
}

type sentPacket struct {
	idx          int
	genID        int
	trackID      int
	epochID      int
	ssrc         uint32
	rawSeq       uint16
	extSeq       int64
	transportSeq uint16
	hasTransport bool
	timeUS       int64
}

// Engine holds the indexes built once for the whole capture. Every lookup
// table stores indices into the flat `sent` slice.
type Engine struct {
	packets []rtproto.Packet
	id      *identity.Result
	params  FreezeParams

	sent []sentPacket
	// byGen: generation id -> sent indices in capture order.
	byGen map[int][]int
	// genRawIndex: generation -> raw 16-bit seq -> sent index.
	genRawIndex map[int]map[uint16]int
	// epochTCC: transport epoch -> transport seq -> sent indices. Overlapping
	// TWCC messages share entries; several sends can share a transport seq
	// only across an epoch boundary, which the key prevents.
	epochTCC   map[int]map[uint16][]int
	genBySSRC  map[uint32][]int
	srByMiddle map[uint32][]int // NTP middle 32 -> SR positions in Analysis.SRs
}

// NewEngine builds the lookup indexes from the partition result.
func NewEngine(packets []rtproto.Packet, id *identity.Result, params FreezeParams) *Engine {
	e := &Engine{
		packets:     packets,
		id:          id,
		params:      params,
		byGen:       map[int][]int{},
		genRawIndex: map[int]map[uint16]int{},
		epochTCC:    map[int]map[uint16][]int{},
		genBySSRC:   map[uint32][]int{},
		srByMiddle:  map[uint32][]int{},
	}
	for i := range packets {
		p := &packets[i]
		if p.Kind != rtproto.KindRTP || p.RTP == nil {
			continue
		}
		a := id.RTP[p.Index]
		if a == nil {
			continue
		}
		sp := sentPacket{
			idx:     p.Index,
			genID:   a.GenerationID,
			trackID: a.TrackID,
			epochID: a.TransportEpochID,
			ssrc:    p.RTP.SSRC,
			rawSeq:  p.RTP.Sequence,
			extSeq:  a.ExtendedSeq,
			timeUS:  p.TimeUS,
		}
		if p.TransportSeq != nil {
			sp.hasTransport = true
			sp.transportSeq = *p.TransportSeq
		}
		si := len(e.sent)
		e.sent = append(e.sent, sp)
		e.byGen[sp.genID] = append(e.byGen[sp.genID], si)
		if e.genRawIndex[sp.genID] == nil {
			e.genRawIndex[sp.genID] = map[uint16]int{}
		}
		e.genRawIndex[sp.genID][sp.rawSeq] = si
		e.genBySSRC[sp.ssrc] = appendUniqueGen(e.genBySSRC[sp.ssrc], sp.genID)
		if sp.hasTransport {
			if e.epochTCC[sp.epochID] == nil {
				e.epochTCC[sp.epochID] = map[uint16][]int{}
			}
			e.epochTCC[sp.epochID][sp.transportSeq] =
				append(e.epochTCC[sp.epochID][sp.transportSeq], si)
		}
	}
	return e
}

func appendUniqueGen(gens []int, gen int) []int {
	for _, g := range gens {
		if g == gen {
			return gens
		}
	}
	return append(gens, gen)
}

// candidateGens returns every generation that ever used ssrc. An SSRC value
// reused across BYE/rebirth, collision or ICE restart yields several
// generations, and callers keep all of them.
func (e *Engine) candidateGens(ssrc uint32) []int {
	return e.genBySSRC[ssrc]
}

// candidateGensAt narrows the generations for ssrc to those that already
// existed when the feedback packet at fbPacketIndex was observed. A generation
// that starts later in capture order cannot possibly explain an earlier
// reference; this is an ordering constraint, not a wall-clock guess.
func (e *Engine) candidateGensAt(ssrc uint32, fbPacketIndex int) []int {
	var out []int
	for _, genID := range e.genBySSRC[ssrc] {
		if e.id.Generations[genID].StartIndex <= fbPacketIndex {
			out = append(out, genID)
		}
	}
	return out
}

// extSeqFor projects a raw 16-bit sequence into a generation's extended
// sequence space even when that exact packet was never observed (a genuine
// loss). It anchors on any observed packet of the generation and applies the
// modular forward distance, so a NACK for a missing packet still carries its
// expected extended sequence.
func (e *Engine) extSeqFor(genID int, rawSeq uint16) (int64, bool) {
	index := e.genRawIndex[genID]
	if len(index) == 0 {
		return -1, false
	}
	for anchorRaw, si := range index {
		sp := e.sent[si]
		return sp.extSeq + int64(rtproto.SeqDelta(anchorRaw, rawSeq)), true
	}
	return -1, false
}

func (e *Engine) epochOfGen(genID int) int {
	if list := e.byGen[genID]; len(list) > 0 {
		return e.sent[list[0]].epochID
	}
	for _, ep := range e.id.TransportEpochs {
		start := e.id.Generations[genID].StartIndex
		if ep.StartIndex <= start && (ep.EndIndex < 0 || ep.EndIndex >= start) {
			return ep.ID
		}
	}
	return 0
}

func (e *Engine) candidateFromSent(si int, fbEpoch int) CausalityCandidate {
	sp := e.sent[si]
	c := CausalityCandidate{
		GenID:            sp.genID,
		PacketIndex:      sp.idx,
		TrackID:          sp.trackID,
		TransportEpochID: sp.epochID,
		RawSeq:           sp.rawSeq,
		ExtendedSeq:      sp.extSeq,
		TransportSeq:     sp.transportSeq,
	}
	if sp.epochID != fbEpoch {
		c.CrossEpoch = true
		c.Note = "feedback transport epoch does not match the candidate generation"
	}
	return c
}

func (e *Engine) bareCandidate(genID int, rawSeq uint16, fbEpoch int, note string) CausalityCandidate {
	c := CausalityCandidate{
		GenID:            genID,
		PacketIndex:      -1,
		TrackID:          e.id.Generations[genID].TrackID,
		TransportEpochID: e.epochOfGen(genID),
		RawSeq:           rawSeq,
		ExtendedSeq:      -1,
		Note:             note,
	}
	if c.TransportEpochID != fbEpoch {
		c.CrossEpoch = true
		if c.Note == "" {
			c.Note = "feedback transport epoch does not match the candidate generation"
		}
	}
	return c
}

// Run performs the full analysis pass.
func (e *Engine) Run() *Analysis {
	out := &Analysis{}
	for i := range e.packets {
		p := &e.packets[i]
		if p.Kind != rtproto.KindRTCP {
			continue
		}
		fbEpoch := e.epochAt(p.Index)
		for mi, m := range p.RTCP {
			switch {
			case m.PacketType == rtproto.PTFB && m.FMT == rtproto.FMTNACK:
				out.Feedback = append(out.Feedback, e.analyzeNACK(p, mi, m, fbEpoch))
			case m.PacketType == rtproto.PTPSFB && m.FMT == rtproto.FMTPLI:
				out.Feedback = append(out.Feedback, e.analyzeKeyframeRequest(p, mi, m, fbEpoch, KindPLI))
			case m.PacketType == rtproto.PTPSFB && m.FMT == rtproto.FMTFIR:
				out.Feedback = append(out.Feedback, e.analyzeKeyframeRequest(p, mi, m, fbEpoch, KindFIR))
			case m.PacketType == rtproto.PTFB && m.FMT == rtproto.FMTTWCC:
				out.Feedback = append(out.Feedback, e.analyzeTWCC(p, mi, m, fbEpoch))
			case m.PacketType == rtproto.PTRR:
				out.Feedback = append(out.Feedback, e.analyzeRR(p, mi, m, fbEpoch))
			case m.PacketType == rtproto.PTSR:
				out.SRs = append(out.SRs, e.analyzeSR(p, mi, m))
			}
		}
	}
	for i := range out.SRs {
		e.srByMiddle[out.SRs[i].NTPMiddle32] =
			append(e.srByMiddle[out.SRs[i].NTPMiddle32], i)
	}
	// Re-run RR timing now that every SR is indexed.
	for fi := range out.Feedback {
		if out.Feedback[fi].Kind == KindRR {
			e.fillRTT(&out.Feedback[fi], out.SRs)
		}
	}
	e.indexFreezes(out)
	e.linkFreezeFeedback(out)
	return out
}

func (e *Engine) epochAt(packetIndex int) int {
	cur := 0
	for _, ep := range e.id.TransportEpochs {
		if ep.StartIndex <= packetIndex && (ep.EndIndex < 0 || ep.EndIndex > packetIndex) {
			cur = ep.ID
		}
	}
	return cur
}

func (e *Engine) analyzeNACK(p *rtproto.Packet, mi int, m rtproto.RTCPMessage, fbEpoch int) FeedbackEvent {
	ev := FeedbackEvent{
		PacketIndex: p.Index, MsgIndex: mi, TimeUS: p.TimeUS, Kind: KindNACK,
		SenderSSRC: m.SSRC, MediaSSRC: m.MediaSSRC, TransportEpochID: fbEpoch,
	}
	seqs := sortedSeqs(rtproto.NACKSet(m.NACK))
	ev.NACKSeqs = seqs
	gens := e.candidateGensAt(m.MediaSSRC, p.Index)
	if len(gens) == 0 {
		ev.Status = StatusUnresolved
		ev.Note = "no generation that predates this feedback ever used the NACK media SSRC"
		for _, s := range seqs {
			ev.Candidates = append(ev.Candidates, CausalityCandidate{
				PacketIndex: -1, RawSeq: s, ExtendedSeq: -1,
				TransportEpochID: fbEpoch, Note: ev.Note,
			})
		}
		return ev
	}
	ev.Status = StatusResolved
	if len(gens) > 1 {
		ev.Status = StatusAmbiguous
		ev.Note = "SSRC was reused across generations; all candidates retained"
	}
	matchedAny := false
	crossOnly := true
	for _, s := range seqs {
		for _, genID := range gens {
			si, ok := e.genRawIndex[genID][s]
			if !ok {
				// The sequence was never observed on this generation, so the
				// loss reference cannot be tied to a send event here. The
				// candidate stays because the SSRC itself matches, and its
				// expected extended sequence is still projected.
				c := e.bareCandidate(genID, s, fbEpoch, "sequence not observed on this generation")
				if ext, ok := e.extSeqFor(genID, s); ok {
					c.ExtendedSeq = ext
				}
				ev.Candidates = append(ev.Candidates, c)
				continue
			}
			matchedAny = true
			c := e.candidateFromSent(si, fbEpoch)
			if !c.CrossEpoch {
				crossOnly = false
			}
			ev.Candidates = append(ev.Candidates, c)
		}
	}
	if !matchedAny {
		ev.Status = StatusUnresolved
	} else if crossOnly {
		// Every concrete send event lives in another transport epoch.
		ev.Status = StatusCrossEpoch
	}
	return ev
}

func (e *Engine) analyzeKeyframeRequest(p *rtproto.Packet, mi int, m rtproto.RTCPMessage, fbEpoch int, kind string) FeedbackEvent {
	ev := FeedbackEvent{
		PacketIndex: p.Index, MsgIndex: mi, TimeUS: p.TimeUS, Kind: kind,
		SenderSSRC: m.SSRC, MediaSSRC: m.MediaSSRC, TransportEpochID: fbEpoch,
	}
	gens := e.candidateGensAt(m.MediaSSRC, p.Index)
	switch len(gens) {
	case 0:
		ev.Status = StatusUnresolved
		ev.Note = "no generation that predates this feedback used the media SSRC"
	case 1:
		ev.Status = StatusResolved
	default:
		ev.Status = StatusAmbiguous
		ev.Note = "SSRC was reused across generations; all candidates retained"
	}
	for _, genID := range gens {
		c := e.bareCandidate(genID, 0, fbEpoch, "")
		if c.CrossEpoch && ev.Status == StatusResolved {
			ev.Status = StatusCrossEpoch
		}
		ev.Candidates = append(ev.Candidates, c)
	}
	return ev
}

func (e *Engine) analyzeTWCC(p *rtproto.Packet, mi int, m rtproto.RTCPMessage, fbEpoch int) FeedbackEvent {
	ev := FeedbackEvent{
		PacketIndex: p.Index, MsgIndex: mi, TimeUS: p.TimeUS, Kind: KindTWCC,
		SenderSSRC: m.SSRC, TransportEpochID: fbEpoch,
	}
	if m.TWCC == nil {
		ev.Status = StatusUnresolved
		ev.Note = "transport-wide feedback carried no parsed payload"
		return ev
	}
	seqs := rtproto.TWCCTransportSeqs(m.TWCC)
	ev.TWCCSeqs = seqs
	index := e.epochTCC[fbEpoch]
	resolved := 0
	for _, tseq := range seqs {
		sentIdx := index[tseq]
		if len(sentIdx) == 0 {
			// Retain as unresolved; overlapping feedback intervals mean later
			// messages may still explain the same transport sequence.
			ev.Candidates = append(ev.Candidates, CausalityCandidate{
				PacketIndex: -1, TransportSeq: tseq, ExtendedSeq: -1,
				TransportEpochID: fbEpoch,
				Note:             "transport sequence not observed in this epoch",
			})
			continue
		}
		resolved++
		for _, si := range sentIdx {
			c := e.candidateFromSent(si, fbEpoch)
			c.TransportSeq = tseq
			ev.Candidates = append(ev.Candidates, c)
		}
	}
	switch {
	case resolved == 0:
		ev.Status = StatusUnresolved
	case resolved < len(seqs):
		ev.Status = StatusAmbiguous
		ev.Note = "part of the feedback window has no matching send event"
	default:
		ev.Status = StatusResolved
	}
	return ev
}

func (e *Engine) analyzeRR(p *rtproto.Packet, mi int, m rtproto.RTCPMessage, fbEpoch int) FeedbackEvent {
	ev := FeedbackEvent{
		PacketIndex: p.Index, MsgIndex: mi, TimeUS: p.TimeUS, Kind: KindRR,
		SenderSSRC: m.SSRC, TransportEpochID: fbEpoch,
	}
	if len(m.ReportBlocks) == 0 {
		ev.Status = StatusUnresolved
		ev.Note = "receiver report carried no report blocks"
		return ev
	}
	ev.MediaSSRC = m.ReportBlocks[0].SSRC
	for _, blk := range m.ReportBlocks {
		gens := e.candidateGensAt(blk.SSRC, p.Index)
		if len(gens) == 0 {
			ev.Candidates = append(ev.Candidates, CausalityCandidate{
				PacketIndex: -1, TransportEpochID: fbEpoch, ExtendedSeq: -1,
				Note: "report block SSRC unknown",
			})
			ev.RTTUS = append(ev.RTTUS, 0)
			ev.RTTNotes = append(ev.RTTNotes, "no generation for reported SSRC")
			continue
		}
		for _, genID := range gens {
			c := e.bareCandidate(genID, 0, fbEpoch, "")
			ev.Candidates = append(ev.Candidates, c)
		}
		if len(gens) > 1 {
			ev.Status = StatusAmbiguous
		} else if ev.Status == "" {
			ev.Status = StatusResolved
		}
		// RTT is filled in fillRTT once every SR is indexed. Placeholder
		// slots keep block/result alignment.
		ev.RTTUS = append(ev.RTTUS, 0)
		if blk.LSR == 0 {
			ev.RTTNotes = append(ev.RTTNotes, "LSR=0: receiver never got a sender report")
		} else {
			ev.RTTNotes = append(ev.RTTNotes, "pending SR match")
		}
	}
	if ev.Status == "" {
		ev.Status = StatusUnresolved
	}
	return ev
}

// fillRTT resolves LSR references against observed sender reports. Every
// matching SR produces a separate RTT candidate rather than picking one; when
// no SR matches, the block stays un-timed and the event is marked unresolved.
func (e *Engine) fillRTT(ev *FeedbackEvent, srs []SRMapping) {
	p := e.packets[ev.PacketIndex]
	blocks := p.RTCP[ev.MsgIndex].ReportBlocks
	block := 0
	for _, blk := range blocks {
		if blk.LSR != 0 {
			matches := e.srByMiddle[blk.LSR]
			if len(matches) == 0 {
				ev.RTTNotes[block] = "LSR matches no observed sender report"
				if ev.Status == StatusResolved {
					ev.Status = StatusUnresolved
				}
			}
			for _, srPos := range matches {
				sr := srs[srPos]
				// DLSR is expressed in 1/65536 s; keep the multiply first to
				// avoid truncating to whole seconds.
				dlsrUS := int64(blk.DLSR) * 1_000_000 / 65536
				rtt := p.TimeUS - sr.NTPUS - dlsrUS
				ev.RTTUS[block] = rtt
				ev.RTTNotes[block] = "rtt candidate from SR packet index " + itoa(sr.PacketIndex)
				if len(matches) > 1 {
					ev.Status = StatusAmbiguous
				}
			}
		}
		block++
	}
}

func (e *Engine) analyzeSR(p *rtproto.Packet, mi int, m rtproto.RTCPMessage) SRMapping {
	sr := SRMapping{
		PacketIndex: p.Index, MsgIndex: mi, SSRC: m.SSRC,
		NTPMSW: m.SenderInfo.NTPMSW, NTPLSW: m.SenderInfo.NTPLSW,
		RTPTimestamp: m.SenderInfo.RTPTimestamp,
		PacketCount:  m.SenderInfo.PacketCount, OctetCount: m.SenderInfo.OctetCount,
		NTPUS: ntpToUS(m.SenderInfo.NTPMSW, m.SenderInfo.NTPLSW),
	}
	sr.NTPMiddle32 = (sr.NTPMSW << 16) | (sr.NTPLSW >> 16)
	// Attribute the SR to the generation of its SSRC that was live when the
	// report was sent, not to whatever generation happens to be last after
	// later rebirths/collisions.
	sr.GenID = -1
	for _, genID := range e.candidateGensAt(m.SSRC, p.Index) {
		g := e.id.Generations[genID]
		if g.StartIndex <= p.Index && (!g.Closed() || g.EndIndex >= p.Index) {
			sr.GenID = genID
		}
	}
	return sr
}

// ntpToUS converts a 64-bit NTP timestamp to microseconds since the NTP
// epoch. The fractional second is scaled with integer rounding rather than
// float conversion so round-tripping an exact microsecond anchor stays exact.
// The synthetic capture anchors SR clocks to the capture clock so RTT
// arithmetic against capture time stays meaningful.
func ntpToUS(msw, lsw uint32) int64 {
	const half = uint64(1) << 31 // 0.5 in NTP fractional units, scaled to us
	fracUS := int64((uint64(lsw)*1_000_000 + half) >> 32)
	return int64(msw)*1_000_000 + fracUS
}

func sortedSeqs(set map[uint16]bool) []uint16 {
	out := make([]uint16, 0, len(set))
	for s := range set {
		out = append(out, s)
	}
	sort.Slice(out, func(i, j int) bool { return out[i] < out[j] })
	return out
}

func itoa(v int) string {
	if v == 0 {
		return "0"
	}
	neg := v < 0
	if neg {
		v = -v
	}
	var b [24]byte
	i := len(b)
	for v > 0 {
		i--
		b[i] = byte('0' + v%10)
		v /= 10
	}
	if neg {
		i--
		b[i] = '-'
	}
	return string(b[i:])
}
