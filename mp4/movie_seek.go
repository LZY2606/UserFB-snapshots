package mp4

import (
	"fmt"
	"math"
	mbits "math/bits"
	"sort"
)

// MovieSeekPlan is the result of [TrakBox.PlanMovieSeek]. A movie time falls in
// either an empty edit, which gives an [EmptyMovieSeekPlan], or an edit that
// presents media, which gives a [MediaMovieSeekPlan]. Type-switch on the
// concrete type to tell the two apart.
type MovieSeekPlan interface {
	movieSeekPlan()
}

// EmptyMovieSeekPlan means that movieTime falls in an empty edit
// (an elst entry with media_time == -1), so no media is presented there.
type EmptyMovieSeekPlan struct {
	// EditNr is the 1-based number of the edit that movieTime falls in.
	EditNr uint32
}

func (*EmptyMovieSeekPlan) movieSeekPlan() {}

// MediaMovieSeekPlan means that movieTime falls in an edit that presents media.
// It carries everything a file reader needs to fetch and decode from the
// preceding sync sample up to the sample presented at movieTime.
type MediaMovieSeekPlan struct {
	// EditNr is the 1-based number of the edit that movieTime falls in.
	EditNr uint32
	// TargetSampleNr is the 1-based sample number presented at movieTime.
	TargetSampleNr uint32
	// DecodeSampleNr is the 1-based sample number where decoding must start:
	// the target sample itself, or the nearest preceding sync sample (stss).
	DecodeSampleNr uint32
	// DataRanges are the file ranges of the samples from DecodeSampleNr to
	// TargetSampleNr, inclusive, in decode order. The offsets are absolute
	// file positions, exactly as [TrakBox.GetRangesForSampleInterval] returns.
	DataRanges []DataRange
}

func (*MediaMovieSeekPlan) movieSeekPlan() {}

// PlanMovieSeek plans which samples a reader must fetch to present the movie at
// movieTime, counted in movieTimescale (the mvhd timescale), for a track of a
// progressive (non-fragmented) file. It only reads the complete sample table of
// the moov box: it performs no network access and no decoding, and it neither
// changes the track boxes nor the sample tables.
//
// The edit list (elst) maps the movie timeline to the media timeline. Both
// rate-1 media edits and empty edits (media_time == -1) are supported, including
// edits that repeat the same media fragment. Without an elst, the media is mapped
// from time zero. A time exactly on an edit boundary belongs to the later edit,
// and a time at or past the end of the last edit is out of range. Time-scale
// conversions use exact integer arithmetic, so non-integral time-base ratios are
// compared without rounding to milliseconds or floats.
//
// The presented (target) sample is the one whose composition interval covers the
// mapped media time. Each interval is half-open [DTS+compositionOffset,
// DTS+compositionOffset+duration), so B-frame display reordering does not affect
// the byte ranges, which stay in decode order. When several intervals cover the
// same time, the one with the largest PTS wins, and the smallest sample number
// wins a PTS tie.
//
// An empty edit gives an [EmptyMovieSeekPlan]. A zero timescale, a non-unit edit
// rate, an unsupported or inconsistent sample table, an edit that no sample
// presents, a target with no sync sample to decode from, or an arithmetic
// overflow gives an error.
func (t *TrakBox) PlanMovieSeek(movieTime uint64, movieTimescale uint32) (MovieSeekPlan, error) {
	const ctx = "PlanMovieSeek"
	if movieTimescale == 0 {
		return nil, fmt.Errorf("%s: movie timescale is 0", ctx)
	}
	stbl, err := t.getSampleTable()
	if err != nil {
		return nil, fmt.Errorf("%s: %w", ctx, err)
	}
	switch {
	case stbl.Stts == nil:
		return nil, fmt.Errorf("%s: track has no stts box", ctx)
	case stbl.Stsz == nil:
		return nil, fmt.Errorf("%s: track has no stsz box", ctx)
	case stbl.Stsc == nil:
		return nil, fmt.Errorf("%s: track has no stsc box", ctx)
	case stbl.Stco == nil && stbl.Co64 == nil:
		return nil, fmt.Errorf("%s: track has no stco or co64 box", ctx)
	}
	if t.Mdia == nil || t.Mdia.Mdhd == nil {
		return nil, fmt.Errorf("%s: track has no mdhd box", ctx)
	}
	mediaTimescale := t.Mdia.Mdhd.Timescale
	if mediaTimescale == 0 {
		return nil, fmt.Errorf("%s: media timescale is 0", ctx)
	}
	nrSamples := stbl.Stsz.GetNrSamples()
	if nrSamples == 0 {
		return nil, fmt.Errorf("%s: sample table has no samples", ctx)
	}
	// GetDecodeTime on the last sample both validates that stts covers every
	// declared sample and gives the decode time and duration of the last one.
	lastDTS, lastDur, err := stbl.Stts.GetDecodeTime(nrSamples)
	if err != nil {
		return nil, fmt.Errorf("%s: bad stts table: %w", ctx, err)
	}
	mediaDuration, ok := addChecked(lastDTS, uint64(lastDur))
	if !ok {
		return nil, fmt.Errorf("%s: media duration overflows the time base", ctx)
	}

	edit, err := selectSeekEdit(t, movieTime, uint64(movieTimescale), uint64(mediaTimescale), mediaDuration)
	if err != nil {
		return nil, fmt.Errorf("%s: %w", ctx, err)
	}
	if edit.empty {
		return &EmptyMovieSeekPlan{EditNr: edit.nr}, nil
	}

	target, err := selectTargetSample(stbl, nrSamples, edit)
	if err != nil {
		return nil, fmt.Errorf("%s: %w", ctx, err)
	}
	decodeStart, err := findDecodeStart(stbl.Stss, nrSamples, target, edit.nr)
	if err != nil {
		return nil, fmt.Errorf("%s: %w", ctx, err)
	}
	ranges, err := t.GetRangesForSampleInterval(decodeStart, target)
	if err != nil {
		return nil, fmt.Errorf("%s: ranges for samples %d-%d: %w", ctx, decodeStart, target, err)
	}
	return &MediaMovieSeekPlan{
		EditNr:         edit.nr,
		TargetSampleNr: target,
		DecodeSampleNr: decodeStart,
		DataRanges:     ranges,
	}, nil
}

// seekEdit describes the edit that a requested movie time falls in, and the
// media-time window [loMedia, hiNum/hiDen) that this edit presents. loMedia is
// an exact media time; the edit end is kept rational so that time bases with a
// non-integral ratio are compared exactly.
type seekEdit struct {
	nr          uint32 // 1-based edit number
	empty       bool
	loMedia     uint64 // first media time presented by the edit
	hiNum       uint64 // numerator of the exclusive media-time end of the edit
	hiDen       uint64 // denominator of the edit end
	targetMedia uint64 // mapped media time presented at the requested movie time
}

// selectSeekEdit walks the edit list and returns the edit containing movieTime,
// or a virtual edit spanning the whole media when the track has no elst.
func selectSeekEdit(t *TrakBox, movieTime, movieTS, mediaTS, mediaDuration uint64) (*seekEdit, error) {
	if t.Edts == nil || len(t.Edts.Elst) == 0 {
		// No edit list: the whole media is presented from media time zero.
		hiNum, ok := mulChecked(mediaDuration, movieTS)
		if !ok {
			return nil, fmt.Errorf("media duration overflows the movie time base")
		}
		if cmpRational(movieTime, movieTS, hiNum, movieTS) >= 0 {
			return nil, fmt.Errorf("movieTime %d is at or past the track duration", movieTime)
		}
		target, ok := scaleToTimeBase(movieTime, mediaTS, movieTS)
		if !ok {
			return nil, fmt.Errorf("movieTime %d overflows the media time base", movieTime)
		}
		return &seekEdit{nr: 1, loMedia: 0, hiNum: hiNum, hiDen: movieTS, targetMedia: target}, nil
	}
	if len(t.Edts.Elst) > 1 {
		return nil, fmt.Errorf("track has %d elst boxes, but only one is supported", len(t.Edts.Elst))
	}
	entries := t.Edts.Elst[0].Entries
	if len(entries) == 0 {
		return nil, fmt.Errorf("elst box has no entries")
	}
	var editStart uint64
	for i := range entries {
		entry := &entries[i]
		segEnd, ok := addChecked(editStart, entry.SegmentDuration)
		if !ok {
			return nil, fmt.Errorf("elst entry %d: edit timeline overflows", i+1)
		}
		if movieTime >= segEnd {
			editStart = segEnd
			continue
		}
		// movieTime is in this half-open edit [editStart, segEnd).
		if entry.MediaTime == -1 {
			if !isUnitRate(entry) {
				return nil, fmt.Errorf("elst entry %d is an empty edit with a non-unit rate %d.%d",
					i+1, entry.MediaRateInteger, entry.MediaRateFraction)
			}
			return &seekEdit{nr: uint32(i + 1), empty: true}, nil
		}
		if !isUnitRate(entry) {
			return nil, fmt.Errorf("elst entry %d has non-unit media rate %d.%d, only rate 1 is supported",
				i+1, entry.MediaRateInteger, entry.MediaRateFraction)
		}
		if entry.MediaTime < 0 {
			return nil, fmt.Errorf("elst entry %d has unsupported negative mediaTime %d",
				i+1, entry.MediaTime)
		}
		mediaTime := uint64(entry.MediaTime)
		offsetInEdit, ok := scaleToTimeBase(movieTime-editStart, mediaTS, movieTS)
		if !ok {
			return nil, fmt.Errorf("elst entry %d: media time overflows the media time base", i+1)
		}
		target, ok := addChecked(mediaTime, offsetInEdit)
		if !ok {
			return nil, fmt.Errorf("elst entry %d: media time overflows the media time base", i+1)
		}
		loScaled, ok := mulChecked(mediaTime, movieTS)
		if !ok {
			return nil, fmt.Errorf("elst entry %d: media time overflows the movie time base", i+1)
		}
		durScaled, ok := mulChecked(entry.SegmentDuration, mediaTS)
		if !ok {
			return nil, fmt.Errorf("elst entry %d: segment duration overflows the time bases", i+1)
		}
		hiNum, ok := addChecked(loScaled, durScaled)
		if !ok {
			return nil, fmt.Errorf("elst entry %d: edit end overflows the time bases", i+1)
		}
		return &seekEdit{
			nr:          uint32(i + 1),
			loMedia:     mediaTime,
			hiNum:       hiNum,
			hiDen:       movieTS,
			targetMedia: target,
		}, nil
	}
	return nil, fmt.Errorf("movieTime %d is at or past the edit list duration", movieTime)
}

// selectTargetSample returns the 1-based sample presented at the edit's target
// media time. The scan is limited to samples whose composition interval
// intersects the edit's own media window, so adjacent edits never fill a hole.
func selectTargetSample(stbl *StblBox, nrSamples uint32, edit *seekEdit) (uint32, error) {
	if edit.targetMedia > math.MaxInt64 || edit.loMedia > math.MaxInt64 {
		return 0, fmt.Errorf("elst entry %d: media time is outside the supported time range", edit.nr)
	}
	target := int64(edit.targetMedia)
	lo := int64(edit.loMedia)
	var (
		bestNr  uint32
		bestPTS int64
		dts     uint64
	)
	ctts := stbl.Ctts
	for nr := uint32(1); nr <= nrSamples; nr++ {
		dur := uint64(stbl.Stts.GetDur(nr))
		if dts > math.MaxInt64 {
			return 0, fmt.Errorf("elst entry %d: decode time of sample %d is outside the supported time range",
				edit.nr, nr)
		}
		var cto int32
		if ctts != nil {
			cto = ctts.GetCompositionTimeOffset(nr)
		}
		ptsStart, ok := addInt64Checked(int64(dts), int64(cto))
		if !ok {
			return 0, fmt.Errorf("elst entry %d: composition time of sample %d overflows", edit.nr, nr)
		}
		ptsEnd, ok := addInt64Checked(ptsStart, int64(dur))
		if !ok {
			return 0, fmt.Errorf("elst entry %d: composition interval of sample %d overflows", edit.nr, nr)
		}
		// Keep the sample inside the edit window, so samples of an adjacent
		// edit cannot fill a hole: the composition interval must intersect
		// [loMedia, hiNum/hiDen) and contain the target media time.
		inWindow := ptsEnd > lo &&
			(ptsStart < 0 || cmpRational(uint64(ptsStart), 1, edit.hiNum, edit.hiDen) < 0)
		if inWindow && ptsStart <= target && ptsEnd > target {
			if bestNr == 0 || ptsStart > bestPTS || (ptsStart == bestPTS && nr < bestNr) {
				bestNr = nr
				bestPTS = ptsStart
			}
		}
		next, ok := addChecked(dts, dur)
		if !ok {
			return 0, fmt.Errorf("elst entry %d: decode time of sample %d overflows", edit.nr, nr+1)
		}
		dts = next
	}
	if bestNr == 0 {
		return 0, fmt.Errorf("elst entry %d: no sample presents media time %d", edit.nr, edit.targetMedia)
	}
	return bestNr, nil
}

// findDecodeStart returns the sample where decoding must start to present
// targetSampleNr: target itself or the nearest preceding stss sync sample.
// Without an stss box every sample is a sync sample. An stss entry outside the
// declared samples is an inconsistent sample table.
func findDecodeStart(stss *StssBox, nrSamples, targetSampleNr, editNr uint32) (uint32, error) {
	if stss == nil || len(stss.SampleNumber) == 0 {
		return targetSampleNr, nil
	}
	// The binary search below requires strictly increasing entries, and every
	// entry must name one of the declared samples.
	prev := uint32(0)
	for _, nr := range stss.SampleNumber {
		if nr < 1 || nr > nrSamples || nr <= prev {
			return 0, fmt.Errorf("elst entry %d: stss sync sample %d is invalid for the %d declared samples",
				editNr, nr, nrSamples)
		}
		prev = nr
	}
	// First entry strictly greater than the target, so index-1 is the last one <= target.
	idx := sort.Search(len(stss.SampleNumber), func(i int) bool {
		return stss.SampleNumber[i] > targetSampleNr
	})
	if idx == 0 {
		return 0, fmt.Errorf("elst entry %d: no sync sample at or before target sample %d",
			editNr, targetSampleNr)
	}
	return stss.SampleNumber[idx-1], nil
}

// isUnitRate reports whether the edit media rate is exactly 1.0.
func isUnitRate(entry *ElstEntry) bool {
	return entry.MediaRateInteger == 1 && entry.MediaRateFraction == 0
}

// scaleToTimeBase converts value counted in fromTimeBase to the toTimeBase
// time base with a floor division, reporting an overflow.
func scaleToTimeBase(value, toTimeBase, fromTimeBase uint64) (uint64, bool) {
	scaled, ok := mulChecked(value, toTimeBase)
	if !ok {
		return 0, false
	}
	return scaled / fromTimeBase, true
}

// cmpRational compares aNum/aDen with bNum/bDen exactly, returning -1, 0 or 1.
// The cross products are evaluated as unsigned 128-bit values, so no time base
// ratio has to be rounded to milliseconds or a float. Both denominators must be
// non-zero.
func cmpRational(aNum, aDen, bNum, bDen uint64) int {
	lhi, llo := mbits.Mul64(aNum, bDen)
	rhi, rlo := mbits.Mul64(bNum, aDen)
	if lhi != rhi {
		if lhi < rhi {
			return -1
		}
		return 1
	}
	if llo < rlo {
		return -1
	}
	if llo > rlo {
		return 1
	}
	return 0
}

// mulChecked returns a*b and false if the multiplication overflows uint64.
func mulChecked(a, b uint64) (uint64, bool) {
	hi, lo := mbits.Mul64(a, b)
	if hi != 0 {
		return 0, false
	}
	return lo, true
}

// addChecked returns a+b and false if the addition overflows uint64.
func addChecked(a, b uint64) (uint64, bool) {
	if b > math.MaxUint64-a {
		return 0, false
	}
	return a + b, true
}

// addInt64Checked returns a+b and false if the addition overflows int64.
func addInt64Checked(a, b int64) (int64, bool) {
	sum := a + b
	if (b > 0 && sum < a) || (b < 0 && sum > a) {
		return 0, false
	}
	return sum, true
}
