package mp4_test

import (
	"bytes"
	"os"
	"testing"

	"github.com/Eyevinn/mp4ff/mp4"
)

// seekSpec defines a tiny progressive MP4 built entirely in memory for the
// seek tests. Every sample nr (1-based) is filled with its own one-byte value,
// so the bytes read back from a plan reveal which samples were fetched and in
// which order.
type seekSpec struct {
	movieTS  uint32
	mediaTS  uint32
	sizes    []uint32 // one entry per sample
	durs     []uint32 // one duration per sample, in the media timescale
	cto      []int32  // one composition offset per sample; nil means no ctts box
	sync     []uint32 // sync sample numbers; nil means no stss box (all sync)
	chunks   []uint32 // samples per chunk
	gaps     []uint32 // filler bytes after each chunk but the last
	elstVer  byte
	edits    []mp4.ElstEntry // nil means no edts box
	movieDur uint64          // mvhd/tkhd duration in movie timescale
	mediaDur uint64          // mdhd duration in media timescale
}

// buildSeekMP4 encodes a file from spec and decodes it back with DecodeFile,
// returning the decoded file and its raw bytes.
func buildSeekMP4(t *testing.T, spec seekSpec) (*mp4.File, []byte) {
	t.Helper()
	nrSamples := uint32(len(spec.sizes))
	if len(spec.durs) != int(nrSamples) {
		t.Fatalf("got %d sizes but %d durations", nrSamples, len(spec.durs))
	}
	stts := &mp4.SttsBox{}
	for i := uint32(0); i < nrSamples; {
		delta := spec.durs[i]
		count := uint32(1)
		for i+count < nrSamples && spec.durs[i+count] == delta {
			count++
		}
		stts.SampleCount = append(stts.SampleCount, count)
		stts.SampleTimeDelta = append(stts.SampleTimeDelta, delta)
		i += count
	}
	stsz := &mp4.StszBox{SampleNumber: nrSamples, SampleSize: append([]uint32(nil), spec.sizes...)}
	stsc := &mp4.StscBox{}
	stco := &mp4.StcoBox{}
	var payload []byte
	payloadPos := uint64(0)
	firstSample := uint32(1)
	for ci, perChunk := range spec.chunks {
		if err := stsc.AddEntry(uint32(ci+1), perChunk, 1); err != nil {
			t.Fatalf("stsc.AddEntry: %v", err)
		}
		// ftyp (20 bytes) comes first, then the 8-byte mdat header.
		stco.ChunkOffset = append(stco.ChunkOffset, uint32(28+payloadPos))
		for k := uint32(0); k < perChunk; k++ {
			size := spec.sizes[firstSample-1]
			payload = append(payload, bytes.Repeat([]byte{byte(firstSample)}, int(size))...)
			payloadPos += uint64(size)
			firstSample++
		}
		if ci < len(spec.chunks)-1 {
			var gap uint32
			if ci < len(spec.gaps) {
				gap = spec.gaps[ci]
			}
			payload = append(payload, make([]byte, gap)...)
			payloadPos += uint64(gap)
		}
	}
	if firstSample-1 != nrSamples {
		t.Fatalf("chunks hold %d samples, want %d", firstSample-1, nrSamples)
	}
	stblChildren := []mp4.Box{}
	trak := mp4.CreateEmptyTrak(1, spec.mediaTS, "video", "und")
	stbl := trak.Mdia.Minf.Stbl
	stbl.Stts = stts
	stblChildren = append(stblChildren, stbl.Stsd, stts)
	if spec.cto != nil {
		if len(spec.cto) != int(nrSamples) {
			t.Fatalf("got %d ctts offsets for %d samples", len(spec.cto), nrSamples)
		}
		ctts := &mp4.CttsBox{}
		counts := make([]uint32, nrSamples)
		for i := range counts {
			counts[i] = 1
		}
		if err := ctts.AddSampleCountsAndOffset(counts, spec.cto); err != nil {
			t.Fatal(err)
		}
		for _, off := range spec.cto {
			if off < 0 {
				ctts.Version = 1 // signed composition offsets need version 1
				break
			}
		}
		stbl.Ctts = ctts
		stblChildren = append(stblChildren, ctts)
	}
	if spec.sync != nil {
		stss := &mp4.StssBox{SampleNumber: append([]uint32(nil), spec.sync...)}
		stbl.Stss = stss
		stblChildren = append(stblChildren, stss)
	}
	stbl.Stsc = stsc
	stbl.Stsz = stsz
	stbl.Stco = stco
	stblChildren = append(stblChildren, stsc, stsz, stco)
	stbl.Children = stblChildren

	if len(spec.edits) > 0 {
		elst := &mp4.ElstBox{Version: spec.elstVer, Entries: spec.edits}
		edts := &mp4.EdtsBox{}
		edts.AddChild(elst)
		trak.AddChild(edts)
		// Keep the conventional order tkhd, edts, mdia.
		children := trak.Children
		trak.Children = append([]mp4.Box{children[0], children[len(children)-1]},
			children[1:len(children)-1]...)
	}
	trak.Mdia.Mdhd.Duration = spec.mediaDur
	trak.Tkhd.Duration = spec.movieDur

	moov := mp4.NewMoovBox()
	mvhd := mp4.CreateMvhd()
	mvhd.Timescale = spec.movieTS
	mvhd.Duration = spec.movieDur
	moov.AddChild(mvhd)
	moov.AddChild(trak)

	ftyp := mp4.NewFtyp("isom", 0, []string{"isom"})
	if ftyp.Size() != 20 {
		t.Fatalf("ftyp size %d, want 20", ftyp.Size())
	}
	mdat := &mp4.MdatBox{Data: payload}
	f := mp4.NewFile()
	f.AddChild(ftyp, 0)
	f.AddChild(mdat, ftyp.Size())
	f.AddChild(moov, ftyp.Size()+mdat.Size())

	var buf bytes.Buffer
	if err := f.Encode(&buf); err != nil {
		t.Fatal(err)
	}
	decoded, err := mp4.DecodeFile(bytes.NewReader(buf.Bytes()))
	if err != nil {
		t.Fatal(err)
	}
	return decoded, buf.Bytes()
}

// samplePayload returns the bytes of the 1-based samples from..to, inclusive.
func samplePayload(sizes []uint32, from, to uint32) []byte {
	var out []byte
	for nr := from; nr <= to; nr++ {
		out = append(out, bytes.Repeat([]byte{byte(nr)}, int(sizes[nr-1]))...)
	}
	return out
}

// readRanges reads every DataRange from data and concatenates the bytes.
func readRanges(t *testing.T, data []byte, ranges []mp4.DataRange) []byte {
	t.Helper()
	r := bytes.NewReader(data)
	var out []byte
	for _, dr := range ranges {
		buf := make([]byte, dr.Size)
		n, err := r.ReadAt(buf, int64(dr.Offset))
		if err != nil || n != len(buf) {
			t.Fatalf("ReadAt at %d for %d bytes: n=%d err=%v", dr.Offset, dr.Size, n, err)
		}
		out = append(out, buf...)
	}
	return out
}

// mediaPlan fails the test unless plan is a MediaMovieSeekPlan with the wanted
// sample numbers, and returns it.
func mediaPlan(t *testing.T, plan mp4.MovieSeekPlan, wantTarget, wantDecode, wantEdit uint32) *mp4.MediaMovieSeekPlan {
	t.Helper()
	media, ok := plan.(*mp4.MediaMovieSeekPlan)
	if !ok {
		t.Fatalf("got %T, want *MediaMovieSeekPlan", plan)
	}
	if media.EditNr != wantEdit {
		t.Errorf("EditNr = %d, want %d", media.EditNr, wantEdit)
	}
	if media.TargetSampleNr != wantTarget {
		t.Errorf("TargetSampleNr = %d, want %d", media.TargetSampleNr, wantTarget)
	}
	if media.DecodeSampleNr != wantDecode {
		t.Errorf("DecodeSampleNr = %d, want %d", media.DecodeSampleNr, wantDecode)
	}
	if len(media.DataRanges) == 0 {
		t.Error("media plan has no DataRanges")
	}
	return media
}

// TestMovieSeekPlan covers normal seeks, edit-list boundaries and the rejection
// paths of TrakBox.PlanMovieSeek on tiny files that are encoded and decoded
// back, plus a compatibility check on bbb_prog_10s.mp4.
func TestMovieSeekPlan(t *testing.T) {
	// Full-featured file: movie timescale 600 against media timescale 1000 (a
	// non-integral 5/3 ratio), negative composition offsets with display
	// reordering, an stss box, non-contiguous chunks with gaps, an empty edit
	// and edits that repeat the same media. The composition intervals tile the
	// media timeline [0,8000), with samples 5 and 6 sharing [4000,5000).
	fullSpec := seekSpec{
		movieTS: 600,
		mediaTS: 1000,
		sizes:   []uint32{10, 12, 8, 14, 9, 11, 7, 13},
		durs:    []uint32{1000, 1000, 1000, 1000, 1000, 2000, 1000, 1000},
		// DTS:    0     1000  2000  3000  4000  5000      7000  8000
		// PTS:    0     2000  1000  3000  4000  4000      6000  7000
		cto:      []int32{0, 1000, -1000, 0, 0, -1000, -1000, -1000},
		sync:     []uint32{1, 4, 7},
		chunks:   []uint32{3, 2, 3},
		gaps:     []uint32{7, 11},
		elstVer:  0,
		movieDur: 6600,
		mediaDur: 9000,
		edits: []mp4.ElstEntry{
			{SegmentDuration: 600, MediaTime: -1, MediaRateInteger: 1},    // empty edit
			{SegmentDuration: 2400, MediaTime: 0, MediaRateInteger: 1},    // media 0..4000
			{SegmentDuration: 1200, MediaTime: 1000, MediaRateInteger: 1}, // repeats media 1000..3000
			{SegmentDuration: 2400, MediaTime: 4000, MediaRateInteger: 1}, // media 4000..8000
		},
	}
	fullFile, fullData := buildSeekMP4(t, fullSpec)
	fullTrak := fullFile.Moov.Trak

	t.Run("empty edit gives an empty plan", func(t *testing.T) {
		for _, movieTime := range []uint64{0, 300, 599} {
			plan, err := fullTrak.PlanMovieSeek(movieTime, fullSpec.movieTS)
			if err != nil {
				t.Fatalf("movieTime %d: %v", movieTime, err)
			}
			empty, ok := plan.(*mp4.EmptyMovieSeekPlan)
			if !ok {
				t.Fatalf("movieTime %d: got %T, want *EmptyMovieSeekPlan", movieTime, plan)
			}
			if empty.EditNr != 1 {
				t.Errorf("movieTime %d: EditNr = %d, want 1", movieTime, empty.EditNr)
			}
		}
	})

	t.Run("edit boundary belongs to the later edit", func(t *testing.T) {
		// movieTime 600 is the first time of edit 2, which presents media time 0.
		plan, err := fullTrak.PlanMovieSeek(600, fullSpec.movieTS)
		if err != nil {
			t.Fatal(err)
		}
		mediaPlan(t, plan, 1, 1, 2)
		// movieTime 3000 starts edit 3 at repeated media time 1000 (sample 3).
		plan, err = fullTrak.PlanMovieSeek(3000, fullSpec.movieTS)
		if err != nil {
			t.Fatal(err)
		}
		mediaPlan(t, plan, 3, 1, 3)
		// movieTime 4200 starts edit 4 at repeated media time 4000 (sample 5).
		plan, err = fullTrak.PlanMovieSeek(4200, fullSpec.movieTS)
		if err != nil {
			t.Fatal(err)
		}
		mediaPlan(t, plan, 5, 4, 4)
	})

	t.Run("PTS tie picks the smaller sample number", func(t *testing.T) {
		// Media times [4000,5000) are covered by samples 5 and 6 at the same
		// PTS, so sample 5 must win. It is not a sync sample, so decoding
		// starts at the preceding sync sample 4.
		plan, err := fullTrak.PlanMovieSeek(4500, fullSpec.movieTS)
		if err != nil {
			t.Fatal(err)
		}
		mediaPlan(t, plan, 5, 4, 4)
		// The same media fragment reached through the repeated edit 3.
		plan, err = fullTrak.PlanMovieSeek(3300, fullSpec.movieTS) // edit 3, media 1500 -> sample 3
		if err != nil {
			t.Fatal(err)
		}
		mediaPlan(t, plan, 3, 1, 3)
	})

	t.Run("sync sample and preroll selection", func(t *testing.T) {
		cases := []struct {
			movieTime              uint64
			wantTarget, wantDecode uint32
			wantEdit               uint32
		}{
			{4500, 5, 4, 4}, // tie at PTS 4000: sample 5 wins, preroll to sync 4
			{5100, 6, 4, 4}, // sample 6 [4000,6000) needs preroll to sync sample 4
			{5400, 7, 7, 4}, // sample 7 is itself a sync sample
			{6000, 8, 7, 4}, // last sample, preroll to sync sample 7
		}
		for _, c := range cases {
			plan, err := fullTrak.PlanMovieSeek(c.movieTime, fullSpec.movieTS)
			if err != nil {
				t.Fatalf("movieTime %d: %v", c.movieTime, err)
			}
			mediaPlan(t, plan, c.wantTarget, c.wantDecode, c.wantEdit)
		}
	})

	t.Run("ranges are read in decode order across non-contiguous chunks", func(t *testing.T) {
		// Sample 6 is decoded after sample 5 but presented within sample 6's
		// long composition interval. Decoding starts at sync sample 4, so the
		// ranges must give samples 4, 5 and 6 in decode order and skip the gap
		// between chunks 2 and 3.
		plan, err := fullTrak.PlanMovieSeek(5100, fullSpec.movieTS)
		if err != nil {
			t.Fatal(err)
		}
		media := mediaPlan(t, plan, 6, 4, 4)
		got := readRanges(t, fullData, media.DataRanges)
		want := samplePayload(fullSpec.sizes, 4, 6)
		if !bytes.Equal(got, want) {
			t.Fatalf("read %v, want samples 4-6 in decode order %v", got, want)
		}
		// The plan must match the existing sample-range interface exactly.
		wantRanges, err := fullTrak.GetRangesForSampleInterval(4, 6)
		if err != nil {
			t.Fatal(err)
		}
		if diff := compareRanges(media.DataRanges, wantRanges); diff != "" {
			t.Errorf("DataRanges differ from GetRangesForSampleInterval: %s", diff)
		}
		// A plan ending at a sync sample reads exactly that one sample.
		plan, err = fullTrak.PlanMovieSeek(5400, fullSpec.movieTS)
		if err != nil {
			t.Fatal(err)
		}
		media = mediaPlan(t, plan, 7, 7, 4)
		got = readRanges(t, fullData, media.DataRanges)
		if !bytes.Equal(got, samplePayload(fullSpec.sizes, 7, 7)) {
			t.Fatalf("read %v, want sample 7 only", got)
		}
	})

	t.Run("adjacent edits do not fill a hole", func(t *testing.T) {
		// Edit 3 presents media [1000,3000). Sample 1 ends at media time 1000,
		// so the first media time of the edit is presented by sample 3, whose
		// preroll reaches back to sync sample 1, without borrowing any sample
		// beyond the edit window.
		plan, err := fullTrak.PlanMovieSeek(3000, fullSpec.movieTS)
		if err != nil {
			t.Fatal(err)
		}
		media := mediaPlan(t, plan, 3, 1, 3)
		got := readRanges(t, fullData, media.DataRanges)
		if !bytes.Equal(got, samplePayload(fullSpec.sizes, 1, 3)) {
			t.Fatalf("read %v, want samples 1-3", got)
		}
	})

	t.Run("end of timeline is out of range", func(t *testing.T) {
		for _, movieTime := range []uint64{6600, 7000, ^uint64(0)} {
			if _, err := fullTrak.PlanMovieSeek(movieTime, fullSpec.movieTS); err == nil {
				t.Errorf("movieTime %d: expected an out-of-range error", movieTime)
			}
		}
	})

	t.Run("planning does not modify the boxes or tables", func(t *testing.T) {
		var before bytes.Buffer
		if err := fullFile.Moov.Encode(&before); err != nil {
			t.Fatal(err)
		}
		for _, movieTime := range []uint64{0, 600, 1200, 3000, 4500, 6000, 6599} {
			if _, err := fullTrak.PlanMovieSeek(movieTime, fullSpec.movieTS); err != nil {
				t.Fatalf("movieTime %d: %v", movieTime, err)
			}
		}
		var after bytes.Buffer
		if err := fullFile.Moov.Encode(&after); err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(before.Bytes(), after.Bytes()) {
			t.Error("moov box changed while planning seeks")
		}
	})

	// Simple file without an edit list or stss: every sample is a sync sample,
	// the media is mapped from time zero, and movie/media timescales are 1000/3000.
	plainSpec := seekSpec{
		movieTS:  1000,
		mediaTS:  3000,
		sizes:    []uint32{5, 6, 7, 8},
		durs:     []uint32{3000, 3000, 3000, 3000},
		chunks:   []uint32{1, 1, 1, 1},
		movieDur: 4000,
		mediaDur: 12000,
	}
	plainFile, plainData := buildSeekMP4(t, plainSpec)
	plainTrak := plainFile.Moov.Trak

	t.Run("no elst maps from media zero and every sample is sync", func(t *testing.T) {
		cases := []struct {
			movieTime  uint64
			wantSample uint32
		}{
			{0, 1},
			{999, 1},
			{1000, 2},
			{2500, 3},
			{3999, 4},
		}
		for _, c := range cases {
			plan, err := plainTrak.PlanMovieSeek(c.movieTime, plainSpec.movieTS)
			if err != nil {
				t.Fatalf("movieTime %d: %v", c.movieTime, err)
			}
			media := mediaPlan(t, plan, c.wantSample, c.wantSample, 1)
			got := readRanges(t, plainData, media.DataRanges)
			want := samplePayload(plainSpec.sizes, c.wantSample, c.wantSample)
			if !bytes.Equal(got, want) {
				t.Errorf("movieTime %d: read %v, want sample %d", c.movieTime, got, c.wantSample)
			}
		}
		if _, err := plainTrak.PlanMovieSeek(4000, plainSpec.movieTS); err == nil {
			t.Error("movieTime at the track duration should be out of range")
		}
	})

	// An stss whose first sync sample is sample 3: targets before it cannot be
	// decoded, which must be an error rather than a fake plan.
	stssSpec := seekSpec{
		movieTS:  1000,
		mediaTS:  1000,
		sizes:    []uint32{4, 4, 4},
		durs:     []uint32{1000, 1000, 1000},
		sync:     []uint32{3},
		chunks:   []uint32{3},
		movieDur: 3000,
		mediaDur: 3000,
	}
	stssFile, _ := buildSeekMP4(t, stssSpec)
	t.Run("target without a preceding sync sample is rejected", func(t *testing.T) {
		for _, movieTime := range []uint64{0, 500, 1500} {
			if _, err := stssFile.Moov.Trak.PlanMovieSeek(movieTime, 1000); err == nil {
				t.Errorf("movieTime %d: expected a no-sync-sample error", movieTime)
			}
		}
		plan, err := stssFile.Moov.Trak.PlanMovieSeek(2500, 1000) // sample 3 is sync
		if err != nil {
			t.Fatal(err)
		}
		mediaPlan(t, plan, 3, 3, 1)
	})

	t.Run("rejection paths", func(t *testing.T) {
		baseFile, _ := buildSeekMP4(t, plainSpec)
		baseTrak := baseFile.Moov.Trak
		if _, err := baseTrak.PlanMovieSeek(0, 0); err == nil {
			t.Error("zero movie timescale should be rejected")
		}
		zeroMedia := *baseTrak
		zeroMedia.Mdia = cloneTrakMedia(t, baseTrak)
		zeroMedia.Mdia.Mdhd.Timescale = 0
		if _, err := zeroMedia.PlanMovieSeek(0, 1000); err == nil {
			t.Error("zero media timescale should be rejected")
		}
		// Non-unit media rate.
		rateSpec := plainSpec
		rateSpec.edits = []mp4.ElstEntry{{SegmentDuration: 4000, MediaTime: 0, MediaRateInteger: 2}}
		rateFile, _ := buildSeekMP4(t, rateSpec)
		if _, err := rateFile.Moov.Trak.PlanMovieSeek(0, 1000); err == nil {
			t.Error("non-unit media rate should be rejected")
		}
		// An stts that covers fewer samples than the stsz.
		badSpec := plainSpec
		badSpec.durs = []uint32{3000, 3000, 3000, 1}
		badFile, _ := buildSeekMP4(t, badSpec)
		badStbl := badFile.Moov.Trak.Mdia.Minf.Stbl
		badStbl.Stts = &mp4.SttsBox{SampleCount: []uint32{3}, SampleTimeDelta: []uint32{3000}}
		badStbl.Children = replaceChild(badStbl.Children, badStbl.Stts)
		if _, err := badFile.Moov.Trak.PlanMovieSeek(0, 1000); err == nil {
			t.Error("stts shorter than stsz should be rejected")
		}
		// A missing table box.
		badStbl.Stts = nil
		if _, err := badFile.Moov.Trak.PlanMovieSeek(0, 1000); err == nil {
			t.Error("missing stts should be rejected")
		}
		// An stss entry outside the declared samples is a bad sample table.
		badStssFile, _ := buildSeekMP4(t, stssSpec)
		badStssFile.Moov.Trak.Mdia.Minf.Stbl.Stss.SampleNumber[0] = 99
		if _, err := badStssFile.Moov.Trak.PlanMovieSeek(2500, 1000); err == nil {
			t.Error("stss entry outside the samples should be rejected")
		}
		// A negative mediaTime other than the empty-edit value -1 is unsupported.
		negSpec := plainSpec
		negSpec.edits = []mp4.ElstEntry{{SegmentDuration: 4000, MediaTime: -2, MediaRateInteger: 1}}
		negFile, _ := buildSeekMP4(t, negSpec)
		if _, err := negFile.Moov.Trak.PlanMovieSeek(0, 1000); err == nil {
			t.Error("negative mediaTime other than -1 should be rejected")
		}
		// Overflow while walking the edit list (version 1 entries).
		overflowSpec := seekSpec{
			movieTS: 1000, mediaTS: 1000,
			sizes: []uint32{4}, durs: []uint32{1000}, chunks: []uint32{1},
			elstVer: 1, movieDur: 1000, mediaDur: 1000,
			edits: []mp4.ElstEntry{
				{SegmentDuration: ^uint64(0) - 1, MediaTime: -1, MediaRateInteger: 1},
				{SegmentDuration: 100, MediaTime: -1, MediaRateInteger: 1},
			},
		}
		overflowFile, _ := buildSeekMP4(t, overflowSpec)
		if _, err := overflowFile.Moov.Trak.PlanMovieSeek(^uint64(0), 1000); err == nil {
			t.Error("edit-list timeline overflow should be rejected")
		}
	})

	t.Run("bbb_prog_10s.mp4 compatibility", func(t *testing.T) {
		data, err := os.ReadFile("testdata/bbb_prog_10s.mp4")
		if err != nil {
			t.Fatal(err)
		}
		f, err := mp4.DecodeFile(bytes.NewReader(data))
		if err != nil {
			t.Fatal(err)
		}
		const movieTS uint32 = 1000
		for trakIdx, trak := range f.Moov.Traks {
			nrSamples := trak.GetNrSamples()
			for _, movieTime := range []uint64{0, 1, 5000, 9000} {
				plan, err := trak.PlanMovieSeek(movieTime, movieTS)
				if err != nil {
					t.Fatalf("track %d movieTime %d: %v", trakIdx+1, movieTime, err)
				}
				media, ok := plan.(*mp4.MediaMovieSeekPlan)
				if !ok {
					t.Fatalf("track %d: got %T, want media plan", trakIdx+1, plan)
				}
				if media.TargetSampleNr < 1 || media.TargetSampleNr > nrSamples {
					t.Fatalf("track %d: target %d outside 1-%d", trakIdx+1, media.TargetSampleNr, nrSamples)
				}
				if media.DecodeSampleNr < 1 || media.DecodeSampleNr > media.TargetSampleNr {
					t.Fatalf("track %d: decode start %d not in 1-%d", trakIdx+1, media.DecodeSampleNr, media.TargetSampleNr)
				}
				got := readRanges(t, data, media.DataRanges)
				samples, err := trak.GetSampleData(media.DecodeSampleNr, media.TargetSampleNr)
				if err != nil {
					t.Fatal(err)
				}
				var wantSize uint64
				for _, s := range samples {
					wantSize += uint64(s.Size)
				}
				if uint64(len(got)) != wantSize {
					t.Errorf("track %d movieTime %d: read %d bytes, want %d",
						trakIdx+1, movieTime, len(got), wantSize)
				}
			}
			// The mvhd duration is the exclusive end of the timeline.
			dur := f.Moov.Mvhd.Duration
			if _, err := trak.PlanMovieSeek(dur, movieTS); err == nil {
				t.Errorf("track %d: seek at duration %d should be out of range", trakIdx+1, dur)
			}
		}
	})
}

// compareRanges returns an empty string when a and b hold the same ranges, and
// a description of the first difference otherwise.
func compareRanges(a, b []mp4.DataRange) string {
	if len(a) != len(b) {
		return "different number of ranges"
	}
	for i := range a {
		if a[i] != b[i] {
			return "range content differs"
		}
	}
	return ""
}

// replaceChild builds a child list where the box of the same type as replacement
// is replaced by it, keeping the order of the others.
func replaceChild(children []mp4.Box, replacement mp4.Box) []mp4.Box {
	out := make([]mp4.Box, 0, len(children))
	for _, c := range children {
		if c.Type() == replacement.Type() {
			out = append(out, replacement)
		} else {
			out = append(out, c)
		}
	}
	return out
}

// cloneTrakMedia returns a shallow-copy mdia box of trak with its own mdhd, so a
// test can mutate the timescale without touching the original track.
func cloneTrakMedia(t *testing.T, trak *mp4.TrakBox) *mp4.MdiaBox {
	t.Helper()
	mdia := *trak.Mdia
	mdhd := *trak.Mdia.Mdhd
	mdia.Mdhd = &mdhd
	return &mdia
}
