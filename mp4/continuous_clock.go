package mp4

import (
	"errors"
	"fmt"
	"math"
)

// Continuous-clock mode gives the samples of a fragmented stream decode times that continue from fragment to
// fragment on every track, including fragments whose traf has no tfdt box. Some recorders write a tfdt only at the
// start of a track or when they re-anchor the time, so the default decode, which gives such a traf decode times
// relative to zero, loses the continuation. The mode is optional and off by default, and it neither adds nor
// changes boxes: the tfdt boxes and the encoded media bytes stay exactly as decoded.
//
// Each track keeps its own clock in the ticks of its own timescale. The first traf of a track without a tfdt
// starts at zero. A traf with a tfdt re-anchors the clock to its 64-bit baseMediaDecodeTime, forwards or
// backwards. The clock then advances by the durations of all samples of all truns of the traf, in decode order.
// A traf absent from a fragment leaves its track's clock untouched. An empty traf without a tfdt does not move
// the clock, while an empty traf with a tfdt only re-anchors it.
//
// Sample durations come from the trun sample fields when present (an explicit zero counts), then from the
// tfhd default sample duration when its presence flag is set (an explicit zero counts there too), and then from
// the track's trex default. A non-empty trun without any of these sources is an error. All duration sums and the
// addition to the clock are checked against uint64 overflow. A fragment is evaluated for every track before any
// clock moves, so an error on a later track leaves every clock at its last successfully committed value.

// continuousClock is the decode-time clock of one track: next is the decode time of its next sample.
type continuousClock struct {
	next  uint64
	known bool // whether the clock has been initialized or re-anchored
}

// clockUpdate is the committed value of one track's clock after a fragment.
type clockUpdate struct {
	trackID uint32
	clock   continuousClock
}

// clockSession evaluates one fragment against the clocks without changing them, so that the clocks of all tracks
// advance together only once every traf has passed its duration and overflow checks.
type clockSession struct {
	clocks  map[uint32]*continuousClock
	seen    map[uint32]struct{}
	bases   map[uint32]uint64 // effective base decode time per track, recorded on the fragment
	updates []clockUpdate
}

func beginClockSession(clocks map[uint32]*continuousClock) *clockSession {
	return &clockSession{
		clocks: clocks,
		seen:   map[uint32]struct{}{},
		bases:  map[uint32]uint64{},
	}
}

// evaluateMoof evaluates every traf of moof. A moof with more than one traf of the same track is rejected.
func (s *clockSession) evaluateMoof(moof *MoofBox, trexFor func(uint32) *TrexBox) error {
	for _, traf := range moof.Trafs {
		trackID := traf.Tfhd.TrackID
		if _, ok := s.seen[trackID]; ok {
			return fmt.Errorf("continuous clock: moof has more than one traf for track %d", trackID)
		}
		s.seen[trackID] = struct{}{}
		current := continuousClock{}
		if c, ok := s.clocks[trackID]; ok {
			current = *c
		}
		base, updated, err := evaluateTraf(traf, current, trexFor(trackID))
		if err != nil {
			return fmt.Errorf("continuous clock, track %d: %w", trackID, err)
		}
		s.bases[trackID] = base
		s.updates = append(s.updates, clockUpdate{trackID: trackID, clock: updated})
	}
	return nil
}

// commit advances all clocks and returns the effective base times to record on the fragment.
func (s *clockSession) commit() map[uint32]uint64 {
	for _, u := range s.updates {
		c, ok := s.clocks[u.trackID]
		if !ok {
			c = &continuousClock{}
			s.clocks[u.trackID] = c
		}
		*c = u.clock
	}
	return s.bases
}

// evaluateTraf returns the effective base decode time of traf and the clock value after it. current is the
// clock before the fragment, which is returned unchanged when the traf is empty and has no tfdt.
func evaluateTraf(traf *TrafBox, current continuousClock, trex *TrexBox) (uint64, continuousClock, error) {
	var total uint64
	nrSamples := 0
	for _, trun := range traf.Truns {
		d, err := trunContinuousDuration(trun, traf.Tfhd, trex)
		if err != nil {
			return 0, current, err
		}
		total, err = checkedAdd(total, d)
		if err != nil {
			return 0, current, err
		}
		nrSamples += int(trun.SampleCount())
	}
	anchored := traf.Tfdt != nil
	var base uint64
	switch {
	case anchored:
		base = traf.Tfdt.BaseMediaDecodeTime() // any 64-bit value, forwards or backwards, re-anchors
	case current.known:
		base = current.next
	default:
		base = 0 // first samples of a track the clock has not seen yet start at zero
	}
	if nrSamples == 0 {
		if !anchored {
			return base, current, nil // empty traf without tfdt: the clock does not move
		}
		return base, continuousClock{next: base, known: true}, nil // empty traf with tfdt: re-anchor only
	}
	end, err := checkedAdd(base, total)
	if err != nil {
		return 0, current, err
	}
	return base, continuousClock{next: end, known: true}, nil
}

// trunContinuousDuration sums the effective durations of the trun samples. The trun fields, the tfhd defaults
// behind their presence flags and the trex defaults apply in that order, and an explicit zero is a valid value.
// A non-empty trun without any duration source is an error.
func trunContinuousDuration(trun *TrunBox, tfhd *TfhdBox, trex *TrexBox) (uint64, error) {
	n := trun.SampleCount()
	if n == 0 {
		return 0, nil
	}
	if trun.HasSampleDuration() {
		var total uint64
		for _, s := range trun.Samples {
			var err error
			total, err = checkedAdd(total, uint64(s.Dur))
			if err != nil {
				return 0, err
			}
		}
		return total, nil
	}
	var dur uint32
	if tfhd.HasDefaultSampleDuration() {
		dur = tfhd.DefaultSampleDuration // presence flag set, so an explicit zero is valid
	} else if trex != nil {
		dur = trex.DefaultSampleDuration
	} else {
		return 0, fmt.Errorf("trun with %d samples has no sample duration source", n)
	}
	return checkedMul(uint64(dur), uint64(n))
}

func checkedAdd(a, b uint64) (uint64, error) {
	if b > math.MaxUint64-a {
		return 0, errors.New("sample duration overflows the uint64 decode-time clock")
	}
	return a + b, nil
}

func checkedMul(a uint64, n uint64) (uint64, error) {
	if n != 0 && a > math.MaxUint64/n {
		return 0, errors.New("sample duration overflows the uint64 decode-time clock")
	}
	return a * n, nil
}

// advanceContinuousClocks evaluates fragment f against clocks and, only when every traf passes, commits the new
// clock values and records the effective base decode times on f. A fragment that has already been advanced is not
// advanced again, so reading its samples more than once cannot move the clocks. It returns the clock map,
// allocated on first use.
func advanceContinuousClocks(clocks map[uint32]*continuousClock, f *Fragment,
	trexFor func(uint32) *TrexBox) (map[uint32]*continuousClock, error) {
	if f.clockBases != nil {
		return clocks, nil
	}
	if clocks == nil {
		clocks = map[uint32]*continuousClock{}
	}
	session := beginClockSession(clocks)
	if err := session.evaluateMoof(f.Moof, trexFor); err != nil {
		return clocks, err
	}
	f.clockBases = session.commit()
	return clocks, nil
}

// effectiveBaseTime returns the decode time of the first sample of traf: the base recorded by a continuous clock
// when the fragment was decoded with one, otherwise the tfdt value or zero, as without the mode.
func (f *Fragment) effectiveBaseTime(traf *TrafBox) uint64 {
	if f.clockBases != nil {
		if base, ok := f.clockBases[traf.Tfhd.TrackID]; ok {
			return base
		}
	}
	if traf.Tfdt != nil {
		return traf.Tfdt.BaseMediaDecodeTime()
	}
	return 0
}
