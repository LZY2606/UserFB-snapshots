package mp4

import (
	"fmt"
	"math"
)

// continuousClock keeps the decode-time clock of each track of a fragmented stream, in the ticks of that track,
// for the optional continuous-clock mode. Some recorders write a tfdt only at the start of a track or when they
// re-synchronize the clock, and let later fragments continue from the end of the previous one. Without this mode,
// the samples of such a fragment get decode times that start at zero. In continuous-clock mode each track
// remembers the decode time of its next sample, so the times carry on across fragments until a tfdt re-anchors
// them, forwards or backwards.
//
// A clock is advanced only after the whole fragment, all its tracks, has been parsed and the additions checked,
// so that a fragment that fails to parse never moves a clock. Nothing here changes the boxes of a fragment:
// no tfdt is added or modified, and the encoded bytes stay as they were read.
type continuousClock struct {
	// bases maps a track ID to the decode time of its next sample. A track without an entry has not appeared
	// since the clock was created or cleared, so its first fragment starts at zero, unless a tfdt anchors it.
	bases map[uint32]uint64
}

// newContinuousClock creates a clock with no track times.
func newContinuousClock() *continuousClock {
	return &continuousClock{bases: make(map[uint32]uint64)}
}

// Reset clears the clocks of every track. The next fragment of a track then starts again from zero, unless its
// traf has a tfdt, which anchors it as for a first appearance.
func (c *continuousClock) Reset() {
	clear(c.bases)
}

// baseDecodeTime returns the decode time of the first sample of traf: its tfdt value when the traf has one,
// otherwise the remembered clock of the track, or zero when the track appears for the first time.
func (c *continuousClock) baseDecodeTime(traf *TrafBox) uint64 {
	if traf.Tfdt != nil {
		return traf.Tfdt.BaseMediaDecodeTime()
	}
	return c.bases[traf.Tfhd.TrackID] // zero value for a track that has not appeared
}

// fragmentClock holds the clock values of one fragment, so that they are committed together. times captures the
// base decode time of each track of the fragment at prepare time, before anything is committed, so that reading
// the fragment's samples keeps giving those times however far the shared clock has moved since.
type fragmentClock struct {
	clock   *continuousClock
	times   map[uint32]uint64 // base decode time of each track in this fragment
	bases   map[uint32]uint64 // next clock value of each track, installed by commit
	checked bool
}

// prepare works out the base decode time of every track of the fragment and the total duration of each traf.
// It must be called once per fragment, before any sample is read, so that all tracks keep time whether or not
// the caller takes their samples or invokes a callback. The duration of a sample is taken from its trun field,
// then the tfhd default of the traf, then the trex default of the track; an explicit zero duration is valid. A
// non-empty trun without any duration source is an error. An empty traf without tfdt does not move its clock,
// and one with a tfdt only re-anchors it. A track that is absent from the fragment keeps its clock.
func (c *continuousClock) prepare(moof *MoofBox, trexs map[uint32]*TrexBox) (*fragmentClock, error) {
	fc := &fragmentClock{
		clock: c,
		times: make(map[uint32]uint64),
		bases: make(map[uint32]uint64),
	}
	seen := make(map[uint32]bool)
	for _, traf := range moof.Trafs {
		trackID := traf.Tfhd.TrackID
		if seen[trackID] {
			return nil, fmt.Errorf("track %d has more than one traf in a moof", trackID)
		}
		seen[trackID] = true
		base := c.baseDecodeTime(traf)
		fc.times[trackID] = base
		var totalDur uint64
		for nr, trun := range traf.Truns {
			dur, err := trunDuration(trun, traf.Tfhd, trexs[trackID])
			if err != nil {
				return nil, fmt.Errorf("track %d trun %d: %w", trackID, nr+1, err)
			}
			totalDur, err = addChecked(totalDur, dur)
			if err != nil {
				return nil, fmt.Errorf("track %d: %w", trackID, err)
			}
		}
		next, err := addChecked(base, totalDur)
		if err != nil {
			return nil, fmt.Errorf("track %d: %w", trackID, err)
		}
		fc.bases[trackID] = next
	}
	return fc, nil
}

// commit installs the clock values of the fragment, together, after the fragment and all its tracks have been
// parsed and checked. It is effective at most once: reading the same fragment again does not move the clock a
// second time.
func (fc *fragmentClock) commit() {
	if fc == nil || fc.checked {
		return
	}
	fc.checked = true
	for trackID, base := range fc.bases {
		fc.clock.bases[trackID] = base
	}
}

// baseTime returns the base decode time captured for traf in this fragment, or its tfdt value, or zero without
// tfdt, when fc is nil as in the default mode.
func (fc *fragmentClock) baseTime(traf *TrafBox) uint64 {
	if fc == nil {
		if traf.Tfdt != nil {
			return traf.Tfdt.BaseMediaDecodeTime()
		}
		return 0
	}
	return fc.times[traf.Tfhd.TrackID]
}

// trunDuration returns the total duration of the samples of trun, resolving each sample's duration from its trun
// field, then the tfhd default and then the trex default. AddSampleDefaultValues performs the same resolution as
// a side effect on the samples, so it is used directly and the total it returns is taken. A trun with samples but
// no duration source anywhere is an error. An explicit zero in a trun field or a tfhd default is valid.
func trunDuration(trun *TrunBox, tfhd *TfhdBox, trex *TrexBox) (uint64, error) {
	if trun.SampleCount() > 0 && !trun.HasSampleDuration() &&
		!tfhd.HasDefaultSampleDuration() && (trex == nil || trex.DefaultSampleDuration == 0) {
		return 0, fmt.Errorf("no sample duration source for %d samples", trun.SampleCount())
	}
	return trun.AddSampleDefaultValues(tfhd, trex), nil
}

// addChecked returns a+b, or an error if the addition overflows uint64.
func addChecked(a, b uint64) (uint64, error) {
	if b > math.MaxUint64-a {
		return 0, fmt.Errorf("decode time overflow: %d + %d", a, b)
	}
	return a + b, nil
}
