// Package identity partitions a capture into tracks, SSRC generations and
// transport epochs.
//
// The central rule is that an SSRC value alone is NOT a stream identity. The
// same 32-bit value can be reborn after BYE, reused after an ICE restart, or
// reused on a different track during a collision; the encoder may also keep
// the same payload type while switching simulcast layers. Every such event is
// a hard boundary, and evidence on opposite sides of a boundary must never be
// treated as belonging to the same flow.
package identity

import (
	"fmt"

	"rtpfreeze/internal/rtproto"
)

// Boundary reasons recorded on a generation.
const (
	BoundaryInitial     = "initial"
	BoundaryCollision   = "ssrc_collision"
	BoundaryBYERebirth  = "bye_rebirth"
	BoundaryLayerSwitch = "layer_switch"
	BoundaryICERestart  = "ice_restart"

	CloseBYE         = "bye"
	CloseCollision   = "ssrc_collision"
	CloseLayerSwitch = "layer_switch"
	CloseICERestart  = "ice_restart"
)

// Track is one negotiated media line/layer, identified by its RTP extension
// headers. Payload type is deliberately NOT part of the identity because the
// encoder keeps reusing it across layer switches.
type Track struct {
	ID          int    `json:"id"`
	MID         string `json:"mid"`
	RID         string `json:"rid"`
	PayloadType int    `json:"payload_type"`
	Synthetic   bool   `json:"synthetic"`
}

// Generation is one contiguous epoch in which an SSRC feeds one track. It is
// the unit against which sequence continuity and freeze evidence are scoped.
type Generation struct {
	ID           int    `json:"id"`
	TrackID      int    `json:"track_id"`
	SSRC         uint32 `json:"ssrc"`
	GenerationNo int    `json:"generation_no"` // per-SSRC reuse counter, 1-based
	StartIndex   int    `json:"start_index"`
	EndIndex     int    `json:"end_index"` // -1 while open
	StartUS      int64  `json:"start_us"`
	EndUS        int64  `json:"end_us"`
	OpenReason   string `json:"open_reason"`
	CloseReason  string `json:"close_reason"` // "" while open
	PayloadType  int    `json:"payload_type"`
}

// Closed reports whether the generation has ended.
func (g *Generation) Closed() bool { return g.EndIndex >= 0 }

// TransportEpoch groups packets between ICE restarts. The transport-wide
// sequence space resets on a restart, so TWCC evidence may never cross epochs.
type TransportEpoch struct {
	ID         int    `json:"id"`
	StartIndex int    `json:"start_index"`
	EndIndex   int    `json:"end_index"`
	StartUS    int64  `json:"start_us"`
	EndUS      int64  `json:"end_us"`
	Reason     string `json:"reason"`
}

// Event is an out-of-band capture marker (currently only ICE restarts).
type Event struct {
	Index  int    `json:"index"`
	TimeUS int64  `json:"time_us"`
	Type   string `json:"type"`
}

// Assignment attaches identity results to every RTP packet.
type Assignment struct {
	TrackID          int   `json:"track_id"`
	GenerationID     int   `json:"generation_id"`
	TransportEpochID int   `json:"transport_epoch_id"`
	ExtendedSeq      int64 `json:"extended_seq"`
}

// Result is the full partition of a capture.
type Result struct {
	Tracks          []Track
	Generations     []Generation
	TransportEpochs []TransportEpoch
	Events          []Event
	// RTP is keyed by capture packet index; non-RTP packets have no entry.
	RTP map[int]*Assignment
}

type ssrcState struct {
	currentGenID int   // open generation id, or -1
	allGenIDs    []int // every generation that ever used this SSRC
	uses         int
}

type trackState struct {
	trackID    int
	activeSSRC uint32 // SSRC currently feeding the track, 0 if none
}

// Builder consumes packets in capture order and emits the partition.
type Builder struct {
	tracks     []Track
	trackByKey map[string]int
	gens       []Generation
	epochs     []TransportEpoch
	ssrc       map[uint32]*ssrcState
	trackState map[int]*trackState
	rtp        map[int]*Assignment
	unwrappers map[int]*rtproto.SeqUnwrapper // per generation
	events     []Event
	curEpochID int
}

// NewBuilder starts an empty partition.
func NewBuilder() *Builder {
	return &Builder{
		trackByKey: map[string]int{},
		ssrc:       map[uint32]*ssrcState{},
		trackState: map[int]*trackState{},
		rtp:        map[int]*Assignment{},
		unwrappers: map[int]*rtproto.SeqUnwrapper{},
		curEpochID: -1,
	}
}

func (b *Builder) openEpoch(idx int, timeUS int64, reason string) int {
	id := len(b.epochs)
	b.epochs = append(b.epochs, TransportEpoch{
		ID: id, StartIndex: idx, EndIndex: -1, StartUS: timeUS, Reason: reason,
	})
	b.curEpochID = id
	return id
}

func (b *Builder) closeEpoch(idx int, timeUS int64) {
	if b.curEpochID < 0 {
		return
	}
	b.epochs[b.curEpochID].EndIndex = idx
	b.epochs[b.curEpochID].EndUS = timeUS
}

func (b *Builder) getSSRC(ssrc uint32) *ssrcState {
	s := b.ssrc[ssrc]
	if s == nil {
		s = &ssrcState{currentGenID: -1}
		b.ssrc[ssrc] = s
	}
	return s
}

// findTrack resolves (or creates) the track for an RTP packet. Packets whose
// capture carried no MID/RID get a synthetic, payload-type+SSRC-scoped track.
func (b *Builder) findTrack(p *rtproto.Packet) int {
	pt := 0
	if p.RTP != nil {
		pt = p.RTP.PayloadType
	}
	var key string
	synthetic := p.MID == ""
	if synthetic {
		key = fmt.Sprintf("synthetic:pt=%d:ssrc=%08x", pt, p.RTP.SSRC)
	} else {
		key = "mid=" + p.MID + "|rid=" + p.RID
	}
	if id, ok := b.trackByKey[key]; ok {
		return id
	}
	id := len(b.tracks)
	b.tracks = append(b.tracks, Track{
		ID: id, MID: p.MID, RID: p.RID, PayloadType: pt, Synthetic: synthetic,
	})
	b.trackByKey[key] = id
	b.trackState[id] = &trackState{trackID: id}
	return id
}

func (b *Builder) closeGen(genID, idx int, timeUS int64, reason string) {
	g := &b.gens[genID]
	if g.Closed() {
		return
	}
	g.EndIndex = idx
	g.EndUS = timeUS
	g.CloseReason = reason
	if st := b.ssrc[g.SSRC]; st != nil && st.currentGenID == genID {
		st.currentGenID = -1
	}
	if ts := b.trackState[g.TrackID]; ts != nil && ts.activeSSRC == g.SSRC {
		ts.activeSSRC = 0
	}
}

func (b *Builder) openGen(trackID int, ssrc uint32, pt int, idx int, timeUS int64, reason string) int {
	st := b.getSSRC(ssrc)
	st.uses++
	id := len(b.gens)
	b.gens = append(b.gens, Generation{
		ID:           id,
		TrackID:      trackID,
		SSRC:         ssrc,
		GenerationNo: st.uses,
		StartIndex:   idx,
		EndIndex:     -1,
		StartUS:      timeUS,
		OpenReason:   reason,
		PayloadType:  pt,
	})
	st.currentGenID = id
	st.allGenIDs = append(st.allGenIDs, id)
	b.trackState[trackID].activeSSRC = ssrc
	b.unwrappers[id] = rtproto.NewSeqUnwrapper()
	return id
}

// HandleEvent applies an out-of-band marker. An ICE restart closes every open
// generation and the transport epoch: SSRC values and transport sequence
// numbers after it start in a fresh namespace.
func (b *Builder) HandleEvent(ev Event) {
	b.events = append(b.events, ev)
	if ev.Type == "ice_restart" {
		for i := range b.gens {
			b.closeGen(i, ev.Index, ev.TimeUS, CloseICERestart)
		}
		b.closeEpoch(ev.Index, ev.TimeUS)
		b.openEpoch(ev.Index, ev.TimeUS, BoundaryICERestart)
	}
}

// HandleRTP assigns (or creates) the generation for one RTP packet.
func (b *Builder) HandleRTP(p *rtproto.Packet) *Assignment {
	if b.curEpochID < 0 {
		b.openEpoch(p.Index, p.TimeUS, BoundaryInitial)
	}
	h := p.RTP
	trackID := b.findTrack(p)
	st := b.getSSRC(h.SSRC)
	ts := b.trackState[trackID]

	genID := st.currentGenID

	if genID >= 0 && b.gens[genID].TrackID != trackID {
		// A live SSRC value is now feeding a different track identity: an
		// RFC 3550 collision. Hard boundary on both sides.
		b.closeGen(genID, p.Index, p.TimeUS, CloseCollision)
		b.closeActiveTrackPeer(trackID, h.SSRC, p.Index, p.TimeUS, CloseCollision)
		genID = b.openGen(trackID, h.SSRC, h.PayloadType, p.Index, p.TimeUS, BoundaryCollision)
	} else if genID < 0 {
		reason := BoundaryInitial
		if len(st.allGenIDs) > 0 {
			last := b.gens[st.allGenIDs[len(st.allGenIDs)-1]]
			if last.TrackID != trackID {
				reason = BoundaryCollision
			} else {
				reason = reasonFromClose(last.CloseReason)
			}
		}

		// Simulcast layer switch: a sibling track (same MID, different RID) is
		// still live, or the same track is swapping its sending SSRC. The
		// payload type is intentionally ignored — the encoder reuses it.
		if sibling := b.openSiblingOnSameMID(trackID); sibling >= 0 {
			b.closeGen(sibling, p.Index, p.TimeUS, CloseLayerSwitch)
			if reason == BoundaryInitial {
				reason = BoundaryLayerSwitch
			}
		} else if ts.activeSSRC != 0 && ts.activeSSRC != h.SSRC {
			if other := b.ssrc[ts.activeSSRC]; other != nil && other.currentGenID >= 0 {
				b.closeGen(other.currentGenID, p.Index, p.TimeUS, CloseLayerSwitch)
			}
			if reason == BoundaryInitial {
				reason = BoundaryLayerSwitch
			}
		}
		genID = b.openGen(trackID, h.SSRC, h.PayloadType, p.Index, p.TimeUS, reason)
	}

	ext := b.unwrappers[genID].Extend(h.Sequence)
	a := &Assignment{
		TrackID:          trackID,
		GenerationID:     genID,
		TransportEpochID: b.curEpochID,
		ExtendedSeq:      ext,
	}
	b.rtp[p.Index] = a
	return a
}

// closeActiveTrackPeer closes the generation currently feeding trackID when
// it belongs to a different SSRC than the incoming packet.
func (b *Builder) closeActiveTrackPeer(trackID int, incoming uint32, idx int, timeUS int64, reason string) {
	ts := b.trackState[trackID]
	if ts == nil || ts.activeSSRC == 0 || ts.activeSSRC == incoming {
		return
	}
	if other := b.ssrc[ts.activeSSRC]; other != nil && other.currentGenID >= 0 {
		b.closeGen(other.currentGenID, idx, timeUS, reason)
	}
}

// reasonFromClose maps how a generation ended onto the reason its successor
// on the same flow opens.
func reasonFromClose(closeReason string) string {
	switch closeReason {
	case CloseBYE:
		return BoundaryBYERebirth
	case CloseLayerSwitch:
		return BoundaryLayerSwitch
	case CloseICERestart:
		return BoundaryICERestart
	case CloseCollision:
		return BoundaryCollision
	default:
		return BoundaryICERestart
	}
}

// openSiblingOnSameMID returns the id of an open generation feeding another
// track that shares the new packet's MID but uses a different RID — i.e. the
// simulcast layer this packet is switching away from.
func (b *Builder) openSiblingOnSameMID(trackID int) int {
	target := b.tracks[trackID]
	if target.MID == "" {
		return -1
	}
	for otherID, ts := range b.trackState {
		if otherID == trackID || ts.activeSSRC == 0 {
			continue
		}
		if b.tracks[otherID].MID == target.MID && b.tracks[otherID].RID != target.RID {
			if st := b.ssrc[ts.activeSSRC]; st != nil && st.currentGenID >= 0 {
				return st.currentGenID
			}
		}
	}
	return -1
}

// HandleBYE closes the generation of the BYE packet's SSRC.
func (b *Builder) HandleBYE(p *rtproto.Packet, m rtproto.RTCPMessage) {
	if st := b.ssrc[m.SSRC]; st != nil && st.currentGenID >= 0 {
		b.closeGen(st.currentGenID, p.Index, p.TimeUS, CloseBYE)
	}
}

// Build partitions the full packet stream. Out-of-band events are applied at
// their capture index, interleaved with the packets rather than all up front.
func Build(packets []rtproto.Packet, events []Event) *Result {
	b := NewBuilder()
	eventAt := map[int][]Event{}
	for _, ev := range events {
		eventAt[ev.Index] = append(eventAt[ev.Index], ev)
	}
	for i := range packets {
		p := &packets[i]
		for _, ev := range eventAt[p.Index] {
			b.HandleEvent(ev)
		}
		switch p.Kind {
		case rtproto.KindRTP:
			if p.RTP != nil {
				b.HandleRTP(p)
			}
		case rtproto.KindRTCP:
			for _, m := range p.RTCP {
				if m.PacketType == rtproto.PTBYE {
					b.HandleBYE(p, m)
				}
			}
		}
	}
	b.closeEpoch(len(packets), 0)
	return &Result{
		Tracks:          b.tracks,
		Generations:     b.gens,
		TransportEpochs: b.epochs,
		Events:          append([]Event(nil), b.events...),
		RTP:             b.rtp,
	}
}

// GenerationsForSSRC returns every generation that ever used the SSRC value,
// in capture order. Feedback targeting a reused SSRC must consider all of
// them rather than guessing which epoch the sender belonged to.
func (r *Result) GenerationsForSSRC(ssrc uint32) []int {
	var out []int
	for i := range r.Generations {
		if r.Generations[i].SSRC == ssrc {
			out = append(out, i)
		}
	}
	return out
}
