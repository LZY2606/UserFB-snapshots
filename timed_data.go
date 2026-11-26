package astits

import (
	"fmt"
)

// TimedData wraps a DemuxerData with the program clock context that was in
// effect when the media started being received.
//
// All time values are expanded into continuous, per-epoch integers and are
// owned by the TimedData: later PCR or PMT updates never mutate them.
type TimedData struct {
	Data *DemuxerData
	// ProgramNumber the data belongs to, 0 when it cannot be derived from PAT/PMT yet.
	ProgramNumber uint16
	// ClockEpoch is the program clock epoch captured at the first TS packet of a PES.
	ClockEpoch uint64
	// PCR27MHz is the expanded anchor PCR (base*300+extension) in 27 MHz units.
	PCR27MHz *int64
	// PTS90k is the expanded PTS in 90 kHz units.
	PTS90k *int64
	// DTS90k is the expanded DTS in 90 kHz units.
	DTS90k *int64
}

const (
	// pcrBaseModulus is the 33 bit PCR/PTS/DTS base modulus.
	pcrBaseModulus int64 = 1 << 33
	// pcrModulus27MHz is the PCR modulus: 2^33 bases times 300 ticks per base.
	pcrModulus27MHz int64 = pcrBaseModulus * 300
	pcrHalfModulus  int64 = pcrModulus27MHz / 2
)

// timedSnapshot is the immutable program context captured when the first TS
// packet of a PES is received.
type timedSnapshot struct {
	programNumber uint16
	epoch         uint64
	hasAnchor     bool
	anchorPCR27   int64
}

// programClock holds the unwrapped PCR state of a single program epoch.
type programClock struct {
	pcrPID    uint16
	hasPCRPID bool

	// epoch starts at 0 and is incremented on PCR PID changes and discontinuities.
	epoch uint64

	// hasAnchor reports whether a PCR has been seen in the current epoch.
	hasAnchor bool
	lastRaw   int64 // raw 27 MHz value of the previous PCR
	last27    int64 // expanded continuous 27 MHz value of the previous PCR
}

// programClockRegistry tracks one independent clock per program.
type programClockRegistry struct {
	programs map[uint16]*programClock
}

func newProgramClockRegistry() *programClockRegistry {
	return &programClockRegistry{programs: map[uint16]*programClock{}}
}

// reset restores every program to its initial state after a Rewind.
func (r *programClockRegistry) reset() {
	r.programs = map[uint16]*programClock{}
}

func (r *programClockRegistry) getOrCreate(programNumber uint16) *programClock {
	pc, ok := r.programs[programNumber]
	if !ok {
		pc = &programClock{}
		r.programs[programNumber] = pc
	}
	return pc
}

// applyPMT updates the program's declared PCR PID. The initial declaration
// does not start a new epoch, and a PMT that keeps the same PCR PID (including
// descriptor-only changes or repeated PMTs) does not reset the clock.
func (r *programClockRegistry) applyPMT(pmt *PMTData) {
	if pmt == nil {
		return
	}
	pc := r.getOrCreate(pmt.ProgramNumber)
	if !pc.hasPCRPID {
		pc.hasPCRPID = true
		pc.pcrPID = pmt.PCRPID
		return
	}
	if pc.pcrPID == pmt.PCRPID {
		return
	}
	pc.pcrPID = pmt.PCRPID
	pc.epoch++
	pc.hasAnchor = false
	pc.lastRaw = 0
	pc.last27 = 0
}

// observe applies the adaptation field of a received TS packet to every program
// whose current PCR PID matches. Packets without payload are observed as well.
// The discontinuity indicator is applied first so the packet's own PCR seeds
// the fresh epoch.
func (r *programClockRegistry) observe(pid uint16, af *PacketAdaptationField) {
	if af == nil {
		return
	}
	for _, pc := range r.programs {
		// PCR PID 0x1fff explicitly means there is no PCR anchor.
		if !pc.hasPCRPID || pc.pcrPID == PIDNull || pc.pcrPID != pid {
			continue
		}

		if af.DiscontinuityIndicator {
			pc.epoch++
			pc.hasAnchor = false
			pc.lastRaw = 0
			pc.last27 = 0
		}

		if af.HasPCR && af.PCR != nil {
			raw := af.PCR.Base*300 + af.PCR.Extension
			if !pc.hasAnchor {
				// First PCR of an epoch: use the original raw value directly.
				pc.last27 = raw
			} else {
				// Accumulate the signed difference whose absolute value is
				// strictly smaller than half the modulus.
				d := raw - pc.lastRaw
				if d > pcrHalfModulus {
					d -= pcrModulus27MHz
				} else if d < -pcrHalfModulus {
					d += pcrModulus27MHz
				}
				pc.last27 += d
			}
			pc.lastRaw = raw
			pc.hasAnchor = true
		}
	}
}

// snapshot returns the immutable context for a PES starting now.
func (r *programClockRegistry) snapshot(programNumber uint16) timedSnapshot {
	pc := r.getOrCreate(programNumber)
	s := timedSnapshot{
		programNumber: programNumber,
		epoch:         pc.epoch,
	}
	if pc.hasAnchor {
		s.hasAnchor = true
		s.anchorPCR27 = pc.last27
	}
	return s
}

// programForPID resolves the program owning an elementary PID from PAT/PMT data.
func (m elementaryStreamMap) programForPID(pid uint16) (programNumber uint16, ok bool) {
	programNumber, ok = m.es[uint32(pid)]
	return
}

// floorDiv returns the floor of a/b for any signs, b being positive here.
func floorDiv(a, b int64) int64 {
	q := a / b
	if a%b != 0 && ((a%b < 0) != (b < 0)) {
		q--
	}
	return q
}

func absInt64(v int64) int64 {
	if v < 0 {
		return -v
	}
	return v
}

// unwrapTimestamp90k expands a raw 33 bit PTS/DTS value by selecting the
// congruent integer closest to anchorPCR27/300. Distances are compared in
// 27 MHz units so the PCR phase smaller than one 90 kHz tick is preserved.
// On an exact half-circle tie the smaller expanded value is selected.
func unwrapTimestamp90k(raw int64, anchorPCR27 int64) int64 {
	const m = pcrBaseModulus
	// k* = (anchorPCR27 - 300*raw) / (300*m) is the real-valued wrap count.
	k0 := floorDiv(anchorPCR27-300*raw, 300*m)
	c0 := raw + k0*m
	c1 := c0 + m
	d0 := absInt64(300*c0 - anchorPCR27)
	d1 := absInt64(300*c1 - anchorPCR27)
	if d1 < d0 {
		return c1
	}
	// Equal distances (exact half circle) keep c0, the smaller value.
	return c0
}

// NextTimedData retrieves the next parsed data, like NextData, and attaches the
// program clock context captured when the media started being received.
//
// NextTimedData must not be mixed with NextPacket or NextData on the same
// Demuxer: pick one reading entry point for the Demuxer's lifetime. Rewind can
// be used to restart a timed read and rebuilds program clocks.
func (dmx *Demuxer) NextTimedData() (td *TimedData, err error) {
	// Check timed data buffer
	if len(dmx.timedBuffer) > 0 {
		td = dmx.timedBuffer[0]
		dmx.timedBuffer = dmx.timedBuffer[1:]
		return
	}

	// Loop through packets
	var p *Packet
	var ps []*Packet
	var ds []*DemuxerData
	for {
		// Get next packet
		if p, err = dmx.NextPacket(); err != nil {
			// If the end of the stream has been reached, we dump the packet pool
			if err == ErrNoMorePackets {
				for {
					// Dump packet pool
					if ps = dmx.packetPool.dumpUnlocked(); len(ps) == 0 {
						break
					}

					// Snapshot was taken on the first packet of the payload
					snap := dmx.consumeTimedSnapshot(ps)

					// Parse data
					var errParseData error
					if ds, errParseData = parseData(ps, dmx.optPacketsParser, dmx.programMap, dmx.streamMap); errParseData != nil {
						// Log error as there may be some incomplete data here
						// We still want to try to parse all packets, in case final data is complete
						dmx.l.Error(fmt.Errorf("astits: parsing data failed: %w", errParseData))
						continue
					}

					// Build timed data
					if td = dmx.buildTimedData(ds, snap); td != nil {
						err = nil
						return
					}
				}
				return
			}
			err = fmt.Errorf("astits: fetching next packet failed: %w", err)
			return
		}

		// Update program clocks from every packet, including packets that only
		// carry an adaptation field (the packet pool ignores those).
		dmx.clocks.observe(p.Header.PID, p.AdaptationField)

		// Freeze the program context on the first TS packet of a PES. The
		// packet's own PCR/discontinuity has already been applied above.
		if p.Header.PayloadUnitStartIndicator && isPESPayload(p.Payload) {
			snap := timedSnapshot{}
			if pn, ok := dmx.streamMap.programForPID(p.Header.PID); ok {
				snap = dmx.clocks.snapshot(pn)
			}
			dmx.timedSnapshots[p] = snap
		}

		// Add packet to the pool
		if ps = dmx.packetPool.addUnlocked(p); len(ps) == 0 {
			continue
		}

		// Recover the context frozen on the first packet of the assembled payload
		snap := dmx.consumeTimedSnapshot(ps)

		// Parse data
		if ds, err = parseData(ps, dmx.optPacketsParser, dmx.programMap, dmx.streamMap); err != nil {
			err = fmt.Errorf("astits: building new data failed: %w", err)
			return
		}

		// Build timed data
		if td = dmx.buildTimedData(ds, snap); td != nil {
			return
		}
	}
}

// consumeTimedSnapshot returns and removes the snapshot attached to the first
// packet of an assembled payload.
func (dmx *Demuxer) consumeTimedSnapshot(ps []*Packet) (snap timedSnapshot) {
	if len(ps) == 0 {
		return
	}
	if s, ok := dmx.timedSnapshots[ps[0]]; ok {
		snap = s
		delete(dmx.timedSnapshots, ps[0])
	}
	return
}

// buildTimedData updates program tables and builds TimedData for every parsed
// data, buffering all but the first one.
func (dmx *Demuxer) buildTimedData(ds []*DemuxerData, snap timedSnapshot) (td *TimedData) {
	// Check whether there is data to be processed
	if len(ds) == 0 {
		return
	}

	// Update program map and elementary stream map
	dmx.updateProgramMaps(ds)

	// Track PCR PID changes per program
	for _, v := range ds {
		if v.PMT != nil {
			dmx.clocks.applyPMT(v.PMT)
		}
	}

	// Build timed data
	tps := make([]*TimedData, 0, len(ds))
	for _, d := range ds {
		t := &TimedData{Data: d}
		if d.PMT != nil {
			// PMT belongs to its signalled program even though it carries no time
			t.ProgramNumber = d.PMT.ProgramNumber
		} else if d.PES != nil {
			t.applySnapshot(snap, d.PES)
		}
		tps = append(tps, t)
	}

	// Buffer extra data and return the first one
	td = tps[0]
	dmx.timedBuffer = append(dmx.timedBuffer, tps[1:]...)
	return
}

// applySnapshot fills PES time pointers from an immutable snapshot.
func (td *TimedData) applySnapshot(snap timedSnapshot, pes *PESData) {
	td.ProgramNumber = snap.programNumber
	td.ClockEpoch = snap.epoch
	if !snap.hasAnchor {
		return
	}

	// Own the value so later clock updates cannot reach it
	pcr := snap.anchorPCR27
	td.PCR27MHz = &pcr

	if pes.Header == nil || pes.Header.OptionalHeader == nil {
		return
	}
	oh := pes.Header.OptionalHeader
	if oh.PTS != nil {
		v := unwrapTimestamp90k(oh.PTS.Base, pcr)
		td.PTS90k = &v
	}
	if oh.DTS != nil {
		v := unwrapTimestamp90k(oh.DTS.Base, pcr)
		td.DTS90k = &v
	}
}
