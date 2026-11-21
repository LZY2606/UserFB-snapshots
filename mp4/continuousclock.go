package mp4

import "fmt"

// This file implements the optional continuous-clock mode of sequential fragment decoding, for fragmented
// streams in which a track has a tfdt box only in its first fragment or when the recorder re-synchronizes,
// and the later fragments continue from the end of the previous one. The mode is enabled with
// FragmentDecoder.ContinuousClock or the stream option WithContinuousClock. It covers unencrypted input
// with one mdat after each moof and at most one traf per track in a moof; it does no index seeking, edit
// list mapping or timescale conversion.
//
// The mode keeps one clock per track: the decode time of the next sample of the track, in the track's own
// ticks. A traf with a tfdt box re-anchors its track to the 64-bit tfdt value, forward or backward; a traf
// without tfdt starts where the previous fragment of the track ended, or at 0 if the track has not been
// seen. The clock of a track advances by the durations of the samples of its traf, resolved as the sample
// accessors resolve them: the trun per-sample field, then the tfhd default if its present flag is set (an
// explicit zero counts), then the trex default of the track. A trun with samples but no duration source is
// an error. A track that is absent from a fragment keeps its clock; an empty traf without tfdt does not
// move it, and one with tfdt only re-anchors it. The clocks of all tracks of a fragment are committed
// together, once every traf is parsed and the duration sums and the uint64 additions are checked, so a
// truncated or rejected fragment leaves every clock at the last good fragment.
//
// The boxes are never modified and none are added: the effective base of a traf is kept in the unexported
// traf fields below, so the tfdt box, the media bytes and the time fields of re-encoded output are
// untouched. Fragment.GetFullSamples, Fragment.AppendFullSamples, Fragment.Samples and the streaming
// SampleAccessor methods report the effective base as DecodeTime, as TrafBox.baseMediaDecodeTime gives it.

// trackClocks holds the continuous-clock state of a sequence of fragments: for each track, the decode time
// of its next sample in the track's ticks.
type trackClocks struct {
	next map[uint32]uint64
}

// clear drops the clocks of all tracks, so that the next fragment without tfdt starts from 0 again.
func (c *trackClocks) clear() {
	clear(c.next)
}

// clockUpdate is the pending state of one traf of a fragment, committed only when every traf is parsed and
// checked.
type clockUpdate struct {
	traf    *TrafBox
	trackID uint32
	base    uint64 // decode time of the first sample of the traf
	next    uint64 // decode time after the last sample of the traf
}

// maxClockTrafs bounds the trafs whose updates advance prepares without allocating.
const maxClockTrafs = 8

// advance computes the effective base of every traf of moof and moves the clocks of their tracks past the
// last sample of the traf. The bases are stored in the trafs and the clocks are committed only after every
// traf is parsed and its durations and additions are checked, so an error leaves the clocks and the trafs
// unchanged. mvex gives the trex defaults of the tracks and may be nil.
func (c *trackClocks) advance(moof *MoofBox, mvex *MvexBox) error {
	trafs := moof.Trafs
	var fixed [maxClockTrafs]clockUpdate
	updates := fixed[:0]
	if len(trafs) > maxClockTrafs {
		updates = make([]clockUpdate, 0, len(trafs))
	}
	for _, traf := range trafs {
		tfhd := traf.Tfhd
		trackID := tfhd.TrackID
		for _, u := range updates {
			if u.trackID == trackID {
				return fmt.Errorf("continuous clock: moof has more than one traf of track %d", trackID)
			}
		}
		var base uint64
		if traf.Tfdt != nil {
			base = traf.Tfdt.BaseMediaDecodeTime()
		} else {
			base = c.next[trackID] // 0 for a track that has not been seen
		}
		var total uint64
		for _, trun := range traf.Truns {
			dur, err := trunTotalDuration(trun, tfhd, mvexTrex(mvex, trackID))
			if err != nil {
				return err
			}
			if total += dur; total < dur {
				return fmt.Errorf("continuous clock: sample durations of track %d overflow uint64", trackID)
			}
		}
		if next := base + total; next >= base {
			updates = append(updates, clockUpdate{traf: traf, trackID: trackID, base: base, next: next})
		} else {
			return fmt.Errorf("continuous clock: decode time of track %d overflows uint64", trackID)
		}
	}
	if c.next == nil && len(updates) > 0 {
		c.next = make(map[uint32]uint64, len(updates))
	}
	for _, u := range updates {
		u.traf.clockBase, u.traf.hasClockBase = u.base, true
		c.next[u.trackID] = u.next
	}
	return nil
}

// trunTotalDuration returns the sum of the durations of the samples of trun. The duration of a sample is
// the trun per-sample field if present, else the tfhd default if its present flag is set (an explicit zero
// counts), else the trex default. A trun with samples but no duration source is an error; an empty trun
// needs no source.
func trunTotalDuration(trun *TrunBox, tfhd *TfhdBox, trex *TrexBox) (uint64, error) {
	n := uint64(len(trun.Samples))
	if n == 0 {
		return 0, nil
	}
	switch {
	case trun.HasSampleDuration():
		var total uint64
		for _, s := range trun.Samples {
			total += uint64(s.Dur) // n and Dur are 32-bit, so the sum cannot overflow
		}
		return total, nil
	case tfhd.HasDefaultSampleDuration():
		return n * uint64(tfhd.DefaultSampleDuration), nil // n and the default are 32-bit
	case trex != nil:
		return n * uint64(trex.DefaultSampleDuration), nil
	default:
		return 0, fmt.Errorf("continuous clock: trun of track %d has samples but no duration source", tfhd.TrackID)
	}
}

// mvexTrex returns the trex of trackID, or nil if mvex is nil or has none.
func mvexTrex(mvex *MvexBox, trackID uint32) *TrexBox {
	if mvex == nil {
		return nil
	}
	trex, _ := mvex.GetTrex(trackID)
	return trex
}
