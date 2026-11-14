package astits

// TimedData represents a parsed data accompanied by the program clock context
// that was in effect when the first TS packet of the underlying data was received.
//
// The time values are owned by the TimedData: they are snapshots and never change
// when later packets update the program clock.
type TimedData struct {
	// Data is the parsed data, identical to what NextData delivers.
	Data *DemuxerData
	// ProgramNumber is the program the PES belongs to, derived from the received
	// PAT/PMT. It is zero when no known program owns the PID.
	ProgramNumber uint16
	// ClockEpoch identifies the continuous clock of the program. It starts at 0
	// and is incremented on PCR PID discontinuities and PCR PID changes.
	ClockEpoch uint64
	// PCR27MHz is the unwrapped program clock reference, in 27 MHz units,
	// captured when the PES started. It is nil for non-PES data and whenever the
	// program has no PCR anchor in the current epoch.
	PCR27MHz *int64
	// PTS90k is the unwrapped presentation timestamp, in 90 kHz units, selected
	// as the 33-bit expansion closest to the anchor PCR/300. It is nil when the
	// PES has no PTS or when there is no PCR anchor.
	PTS90k *int64
	// DTS90k is the unwrapped decoding timestamp, in 90 kHz units. It is nil
	// when the PES only carries a PTS, has no timestamp at all, or when there
	// is no PCR anchor.
	DTS90k *int64
}

// PCR is encoded as base*300 + extension over a 33-bit base, so the modulus of
// the raw 27 MHz value is 2^33 * 300. PTS/DTS are plain 33-bit 90 kHz values.
const (
	pcr27Modulus     = int64(1) << 33 * 300
	pcr27HalfModulus = pcr27Modulus / 2
	pts90Modulus     = int64(1) << 33
	pts90HalfModulus = pts90Modulus / 2
)

// clockSnapshot is the immutable program clock context captured when a PES
// starts. Every value it exposes is copied from the live program state, so
// later PCR or PMT updates cannot affect an in-flight PES.
type clockSnapshot struct {
	programNumber uint16
	epoch         uint64
	pcr27         *int64
}

// programClockState holds the clock of a single program.
type programClockState struct {
	// known reports whether a PMT has been seen for this program.
	known bool
	// pcrPID is the PCR PID currently declared by the program's PMT.
	pcrPID uint16
	// epoch is the current clock epoch of the program, starting at 0.
	epoch uint64
	// anchor is the latest unwrapped PCR in the epoch, in 27 MHz units.
	// It is nil until the first PCR of the epoch has been received.
	anchor *int64
}

// programClocks tracks the per-program clock state derived from PAT/PMT and
// adaptation fields, and the per-PID snapshots of PES packets currently held
// by the packet pool.
type programClocks struct {
	programs  map[uint16]*programClockState
	streams   map[uint32]uint16              // elementary PID -> program number
	pcrOwners map[uint32]map[uint16]struct{} // PCR PID -> programs declaring it
	anchors   map[uint32]*clockSnapshot      // PID -> snapshot of the PES currently queued
}

func newProgramClocks() *programClocks {
	return &programClocks{
		programs:  make(map[uint16]*programClockState),
		streams:   make(map[uint32]uint16),
		pcrOwners: make(map[uint32]map[uint16]struct{}),
		anchors:   make(map[uint32]*clockSnapshot),
	}
}

// reset restores the state a freshly created demuxer starts with. It is used
// by Rewind so that re-reading the same stream gives the same results.
func (c *programClocks) reset() {
	c.programs = make(map[uint16]*programClockState)
	c.streams = make(map[uint32]uint16)
	c.pcrOwners = make(map[uint32]map[uint16]struct{})
	c.anchors = make(map[uint32]*clockSnapshot)
}

// handlePacket applies the adaptation field effects of a packet in reception
// order, before the packet enters the packet pool. A discontinuity indicator
// on a program's current PCR PID starts a new epoch and clears the anchor,
// even when the packet carries no payload. A PCR is then applied to every
// program declaring that PID as its PCR PID.
func (c *programClocks) handlePacket(p *Packet) {
	if !p.Header.HasAdaptationField || p.AdaptationField == nil {
		return
	}

	pid := uint32(p.Header.PID)

	if p.AdaptationField.DiscontinuityIndicator {
		for pnr := range c.pcrOwners[pid] {
			st := c.programs[pnr]
			st.epoch++
			st.anchor = nil
		}
	}

	if p.AdaptationField.HasPCR && p.AdaptationField.PCR != nil {
		if owners, ok := c.pcrOwners[pid]; ok {
			raw := p.AdaptationField.PCR.Base*300 + p.AdaptationField.PCR.Extension
			for pnr := range owners {
				st := c.programs[pnr]
				v := c.unwrapPCR(raw, st.anchor)
				st.anchor = &v
			}
		}
	}
}

// unwrapPCR extends a raw 27 MHz PCR onto the continuous epoch clock. The
// first PCR of an epoch is used as is. Afterwards the signed difference whose
// absolute value is smaller than half the modulus is accumulated.
func (c *programClocks) unwrapPCR(raw int64, prev *int64) int64 {
	if prev == nil {
		return raw
	}

	prevMod := *prev % pcr27Modulus
	if prevMod < 0 {
		prevMod += pcr27Modulus
	}

	d := raw - prevMod
	for d > pcr27HalfModulus {
		d -= pcr27Modulus
	}
	for d < -pcr27HalfModulus {
		d += pcr27Modulus
	}
	return *prev + d
}

// applyPMT updates the program state from a current PMT section. Changing the
// declared PCR PID starts a new epoch; an identical PMT or one that only
// changes descriptors leaves the clock untouched. A PCR PID of 0x1fff means
// the program has no clock anchor.
func (c *programClocks) applyPMT(pmt *PMTData) {
	st, ok := c.programs[pmt.ProgramNumber]
	if !ok {
		st = &programClockState{pcrPID: PIDNull}
		c.programs[pmt.ProgramNumber] = st
	}

	if st.known && st.pcrPID != pmt.PCRPID {
		st.epoch++
		st.anchor = nil
	}

	if st.known {
		c.removePCROwner(st.pcrPID, pmt.ProgramNumber)
	}
	st.known = true
	st.pcrPID = pmt.PCRPID
	if pmt.PCRPID != PIDNull {
		pid := uint32(pmt.PCRPID)
		owners, ok := c.pcrOwners[pid]
		if !ok {
			owners = make(map[uint16]struct{})
			c.pcrOwners[pid] = owners
		}
		owners[pmt.ProgramNumber] = struct{}{}
	}

	for _, es := range pmt.ElementaryStreams {
		c.streams[uint32(es.ElementaryPID)] = pmt.ProgramNumber
	}
}

func (c *programClocks) removePCROwner(pid uint16, pnr uint16) {
	key := uint32(pid)
	owners, ok := c.pcrOwners[key]
	if !ok {
		return
	}
	delete(owners, pnr)
	if len(owners) == 0 {
		delete(c.pcrOwners, key)
	}
}

// snapshot captures the program clock context in effect after the packet's own
// adaptation field has been processed. PES whose PID is unknown to the
// received PMTs get no program number and no time pointers.
func (c *programClocks) snapshot(p *Packet) *clockSnapshot {
	s := &clockSnapshot{}
	pnr, ok := c.streams[uint32(p.Header.PID)]
	if !ok {
		return s
	}

	s.programNumber = pnr
	st, ok := c.programs[pnr]
	if !ok {
		return s
	}
	s.epoch = st.epoch
	if st.anchor != nil {
		v := *st.anchor
		s.pcr27 = &v
	}
	return s
}

// timedData wraps a parsed data with the snapshot captured when its PES (if
// any) started. Non-PES data and PES without an anchor are delivered with nil
// time pointers.
func (c *programClocks) timedData(d *DemuxerData, s *clockSnapshot) *TimedData {
	td := &TimedData{Data: d}
	if s == nil {
		return td
	}

	td.ProgramNumber = s.programNumber
	td.ClockEpoch = s.epoch
	if s.pcr27 == nil || d.PES == nil || d.PES.Header == nil || d.PES.Header.OptionalHeader == nil {
		return td
	}

	pcr := *s.pcr27
	v := pcr
	td.PCR27MHz = &v

	anchor90 := pcr / 300
	oh := d.PES.Header.OptionalHeader
	if oh.PTS != nil {
		pts := unwrapTimestamp90k(oh.PTS.Base, anchor90)
		td.PTS90k = &pts
	}
	if oh.DTS != nil {
		dts := unwrapTimestamp90k(oh.DTS.Base, anchor90)
		td.DTS90k = &dts
	}
	return td
}

// unwrapTimestamp90k selects the expansion of a raw 33-bit 90 kHz timestamp
// that is closest to the anchor PCR/300 using integer distance. The sub-90kHz
// PCR phase is kept by the anchor; on an exact half-circle tie the smaller
// value wins.
func unwrapTimestamp90k(raw, anchor90 int64) int64 {
	d := (raw - anchor90) % pts90Modulus
	if d >= pts90HalfModulus {
		d -= pts90Modulus
	}
	if d < -pts90HalfModulus {
		d += pts90Modulus
	}
	return anchor90 + d
}
