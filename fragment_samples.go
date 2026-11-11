package mp4

import (
	"fmt"
	"io"
	"math"
)

// FragmentSample describes a single sample of a fragmented MP4 file.
// All timestamps are expressed in the raw ticks of the owning track;
// neither edit lists nor movie-time mapping are applied.
type FragmentSample struct {
	// TrackID is the track ID taken from the tfhd box.
	TrackID uint32

	// MoofOffset is the file offset of the moof box containing the sample.
	MoofOffset uint64

	// TrafIndex is the zero-based index of the traf box within the moof box.
	TrafIndex uint32

	// TrunIndex is the zero-based index of the trun box within the traf box.
	TrunIndex uint32

	// SampleIndex is the zero-based index of the sample within the trun box.
	SampleIndex uint32

	// Offset is the absolute file offset of the first byte of the sample.
	Offset uint64

	// Size is the sample size in bytes. It may be zero; in that case the
	// sample occupies no bytes and Offset may point to the end of an mdat
	// payload.
	Size uint32

	// DTS is the decode time of the sample, accumulated from the tfdt box
	// of the containing traf.
	DTS uint64

	// PTS is DTS plus the composition time offset. It is negative when the
	// composition time offset is negative and larger than DTS.
	PTS int64

	// Duration is the sample duration in track ticks.
	Duration uint32

	// SampleFlags is the effective sample flags value, resolved from the
	// trun entry, first_sample_flags, the tfhd default, or the trex default.
	SampleFlags uint32
}

// trun flags (ISO/IEC 14496-12)
const (
	trunDataOffsetPresent                  = 0x000001
	trunFirstSampleFlagsPresent            = 0x000004
	trunSampleDurationPresent              = 0x000100
	trunSampleSizePresent                  = 0x000200
	trunSampleFlagsPresent                 = 0x000400
	trunSampleCompositionTimeOffsetPresent = 0x000800
)

// ProbeFragmentSamples probes a fragmented MP4 file and returns an index of
// every sample, ordered by the appearance of moof, traf, trun and sample in
// the file (not sorted by PTS).
//
// The file must be unencrypted, every traf must carry an explicit tfdt box,
// and every traf must declare its data base explicitly via the tfhd
// base_data_offset field or the default-base-is-moof flag; the legacy
// implicit base is not supported.
//
// The reader position is restored to where it was before the call, both on
// success and on failure. On error, no partial index is returned.
func ProbeFragmentSamples(r io.ReadSeeker) ([]*FragmentSample, error) {
	startPos, err := r.Seek(0, io.SeekCurrent)
	if err != nil {
		return nil, err
	}
	defer func() {
		_, _ = r.Seek(startPos, io.SeekStart)
	}()

	// collect trex defaults from moov/mvex
	trexBips, err := ExtractBoxesWithPayload(r, nil, []BoxPath{
		{BoxTypeMoov(), BoxTypeMvex(), BoxTypeTrex()},
	})
	if err != nil {
		return nil, err
	}
	trexByTrack := make(map[uint32]*Trex, len(trexBips))
	for _, bip := range trexBips {
		trex, ok := bip.Payload.(*Trex)
		if !ok {
			continue
		}
		if _, dup := trexByTrack[trex.TrackID]; dup {
			return nil, fmt.Errorf("probeFragmentSamples: duplicate trex box for track ID %d", trex.TrackID)
		}
		trexByTrack[trex.TrackID] = trex
	}

	// collect top-level moof and mdat boxes in file order
	bis, err := ExtractBoxes(r, nil, []BoxPath{
		{BoxTypeMoof()},
		{BoxTypeMdat()},
	})
	if err != nil {
		return nil, err
	}
	type payloadRange struct{ start, end uint64 } // [start, end)
	mdats := make([]payloadRange, 0, 8)
	moofs := make([]*BoxInfo, 0, 8)
	for _, bi := range bis {
		switch bi.Type {
		case BoxTypeMdat():
			mdats = append(mdats, payloadRange{start: bi.Offset + bi.HeaderSize, end: bi.Offset + bi.Size})
		case BoxTypeMoof():
			moofs = append(moofs, bi)
		}
	}

	// inMdat reports whether the byte range [offset, offset+size) lies
	// entirely within the payload of a single top-level mdat box.
	// A zero-size range only needs to touch a payload, including its right
	// endpoint.
	inMdat := func(offset uint64, size uint64) bool {
		for _, m := range mdats {
			if offset < m.start || offset > m.end {
				continue
			}
			if size <= m.end-offset {
				return true
			}
		}
		return false
	}

	samples := make([]*FragmentSample, 0, 64)
	for _, moof := range moofs {
		trafs, err := ExtractBox(r, moof, BoxPath{BoxTypeTraf()})
		if err != nil {
			return nil, err
		}
		for trafIdx, traf := range trafs {
			trafSamples, err := probeFragmentTrafSamples(r, moof, uint32(trafIdx), traf, trexByTrack, inMdat)
			if err != nil {
				return nil, err
			}
			samples = append(samples, trafSamples...)
		}
	}
	return samples, nil
}

func probeFragmentTrafSamples(
	r io.ReadSeeker,
	moof *BoxInfo,
	trafIdx uint32,
	traf *BoxInfo,
	trexByTrack map[uint32]*Trex,
	inMdat func(offset, size uint64) bool,
) ([]*FragmentSample, error) {
	bips, err := ExtractBoxesWithPayload(r, traf, []BoxPath{
		{BoxTypeTfhd()},
		{BoxTypeTfdt()},
		{BoxTypeTrun()},
	})
	if err != nil {
		return nil, err
	}

	var tfhd *Tfhd
	var tfdt *Tfdt
	var truns []*Trun
	for _, bip := range bips {
		switch bip.Info.Type {
		case BoxTypeTfhd():
			tfhd = bip.Payload.(*Tfhd)
		case BoxTypeTfdt():
			tfdt = bip.Payload.(*Tfdt)
		case BoxTypeTrun():
			truns = append(truns, bip.Payload.(*Trun))
		}
	}

	where := fmt.Sprintf("moof at offset %d, traf %d", moof.Offset, trafIdx)
	if tfhd == nil {
		return nil, fmt.Errorf("probeFragmentSamples: %s: tfhd box not found", where)
	}
	trackID := tfhd.TrackID
	where = fmt.Sprintf("%s (track ID %d)", where, trackID)

	if tfdt == nil {
		return nil, fmt.Errorf("probeFragmentSamples: %s: tfdt box not found", where)
	}

	// the data base must be declared explicitly; the legacy implicit base
	// (previous moof/mdat end) is not supported
	var base uint64
	switch {
	case tfhd.CheckFlag(TfhdBaseDataOffsetPresent):
		base = tfhd.BaseDataOffset
	case tfhd.CheckFlag(TfhdDefaultBaseIsMoof):
		base = moof.Offset
	default:
		return nil, fmt.Errorf("probeFragmentSamples: %s: neither base_data_offset nor default-base-is-moof is set; implicit base data offset is not supported", where)
	}

	trex := trexByTrack[trackID]
	resolveDefault := func(tfhdPresent bool, tfhdValue uint32, trexValue func(*Trex) uint32, what string) (uint32, error) {
		if tfhdPresent {
			return tfhdValue, nil
		}
		if trex != nil {
			return trexValue(trex), nil
		}
		return 0, fmt.Errorf("probeFragmentSamples: %s: missing default sample %s: not present in tfhd and no trex box for track ID %d", where, what, trackID)
	}

	samples := make([]*FragmentSample, 0, 16)
	dts := tfdt.GetBaseMediaDecodeTime()
	runEnd := base
	for trunIdx, trun := range truns {
		trunWhere := fmt.Sprintf("%s, trun %d", where, trunIdx)

		if trun.CheckFlag(trunFirstSampleFlagsPresent) && trun.CheckFlag(trunSampleFlagsPresent) {
			return nil, fmt.Errorf("probeFragmentSamples: %s: first_sample_flags and per-sample flags must not both be present", trunWhere)
		}

		// locate the first byte of this run
		var runStart uint64
		switch {
		case trun.CheckFlag(trunDataOffsetPresent):
			offset := int64(trun.DataOffset)
			if offset >= 0 {
				if uint64(offset) > math.MaxUint64-base {
					return nil, fmt.Errorf("probeFragmentSamples: %s: base %d + data_offset %d overflows", trunWhere, base, offset)
				}
				runStart = base + uint64(offset)
			} else {
				if uint64(-offset) > base {
					return nil, fmt.Errorf("probeFragmentSamples: %s: base %d + data_offset %d is negative", trunWhere, base, offset)
				}
				runStart = base - uint64(-offset)
			}
		case trunIdx == 0:
			runStart = base
		default:
			runStart = runEnd
		}

		if uint64(len(trun.Entries)) < uint64(trun.SampleCount) {
			return nil, fmt.Errorf("probeFragmentSamples: %s: trun declares %d samples but only %d entries were read", trunWhere, trun.SampleCount, len(trun.Entries))
		}

		pos := runStart
		for sampleIdx := uint32(0); sampleIdx < trun.SampleCount; sampleIdx++ {
			entry := &trun.Entries[sampleIdx]
			sampleWhere := fmt.Sprintf("%s, sample %d", trunWhere, sampleIdx)

			var duration uint32
			if trun.CheckFlag(trunSampleDurationPresent) {
				duration = entry.SampleDuration
			} else if duration, err = resolveDefault(
				tfhd.CheckFlag(TfhdDefaultSampleDurationPresent),
				tfhd.DefaultSampleDuration,
				func(t *Trex) uint32 { return t.DefaultSampleDuration },
				"duration",
			); err != nil {
				return nil, err
			}

			var size uint32
			if trun.CheckFlag(trunSampleSizePresent) {
				size = entry.SampleSize
			} else if size, err = resolveDefault(
				tfhd.CheckFlag(TfhdDefaultSampleSizePresent),
				tfhd.DefaultSampleSize,
				func(t *Trex) uint32 { return t.DefaultSampleSize },
				"size",
			); err != nil {
				return nil, err
			}

			var flags uint32
			switch {
			case trun.CheckFlag(trunSampleFlagsPresent):
				flags = entry.SampleFlags
			case sampleIdx == 0 && trun.CheckFlag(trunFirstSampleFlagsPresent):
				flags = trun.FirstSampleFlags
			default:
				if flags, err = resolveDefault(
					tfhd.CheckFlag(TfhdDefaultSampleFlagsPresent),
					tfhd.DefaultSampleFlags,
					func(t *Trex) uint32 { return t.DefaultSampleFlags },
					"flags",
				); err != nil {
					return nil, err
				}
			}

			if !inMdat(pos, uint64(size)) {
				return nil, fmt.Errorf("probeFragmentSamples: %s: sample byte range [%d, %d) is not contained in the payload of any top-level mdat box", sampleWhere, pos, pos+uint64(size))
			}

			var cto int64
			if trun.CheckFlag(trunSampleCompositionTimeOffsetPresent) {
				// version 0: unsigned, version 1: signed
				cto = trun.GetSampleCompositionTimeOffset(int(sampleIdx))
			}
			if dts > math.MaxInt64 {
				return nil, fmt.Errorf("probeFragmentSamples: %s: DTS %d exceeds int64 range", sampleWhere, dts)
			}
			pts := int64(dts) + cto
			if (cto > 0 && pts < int64(dts)) || (cto < 0 && pts > int64(dts)) {
				return nil, fmt.Errorf("probeFragmentSamples: %s: PTS computation overflows (DTS=%d, composition offset=%d)", sampleWhere, dts, cto)
			}

			samples = append(samples, &FragmentSample{
				TrackID:     trackID,
				MoofOffset:  moof.Offset,
				TrafIndex:   trafIdx,
				TrunIndex:   uint32(trunIdx),
				SampleIndex: sampleIdx,
				Offset:      pos,
				Size:        size,
				DTS:         dts,
				PTS:         pts,
				Duration:    duration,
				SampleFlags: flags,
			})

			if uint64(duration) > math.MaxUint64-dts {
				return nil, fmt.Errorf("probeFragmentSamples: %s: DTS accumulation overflows (DTS=%d, duration=%d)", sampleWhere, dts, duration)
			}
			dts += uint64(duration)
			if uint64(size) > math.MaxUint64-pos {
				return nil, fmt.Errorf("probeFragmentSamples: %s: data position overflows (offset=%d, size=%d)", sampleWhere, pos, size)
			}
			pos += uint64(size)
		}
		runEnd = pos
	}
	return samples, nil
}
