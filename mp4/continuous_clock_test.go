package mp4_test

import (
	"bytes"
	"encoding/binary"
	"errors"
	"io"
	"math"
	"slices"
	"strings"
	"testing"

	"github.com/Eyevinn/mp4ff/bits"
	"github.com/Eyevinn/mp4ff/mp4"
	"github.com/Eyevinn/mp4ff/task_support/taskcheck"
)

// The tests here cover the optional continuous-clock mode of sequential fragment decoding, entered through
// FragmentDecoder.ContinuousClock or the stream option WithContinuousClock. The material is two-track
// fragments with several runs per track, really encoded and read back, combined with missing tracks,
// explicit zero tfhd defaults, trex fallback, empty trafs and re-anchoring tfdt boxes. One fragment header
// without tfdt is built as independent bytes, so that the mode is not only verified on self-written input.

// ccTrexDur1 and ccTrexDur2 are the trex default sample durations of tracks 1 and 2 of ccInit.
const (
	ccTrexDur1 = 40
	ccTrexDur2 = 20
)

// ccInit returns an init segment with a video track 1 and an audio track 2, both with a timescale of 1000,
// whose trex default sample durations are ccTrexDur1 and ccTrexDur2.
func ccInit(t testing.TB) *mp4.InitSegment {
	t.Helper()
	init := mp4.CreateEmptyInit()
	init.AddEmptyTrack(1000, "video", "en")
	init.AddEmptyTrack(1000, "audio", "en")
	init.Moov.Mvex.Trexs[0].DefaultSampleDuration = ccTrexDur1
	init.Moov.Mvex.Trexs[1].DefaultSampleDuration = ccTrexDur2
	return init
}

// ccU64 returns a pointer to v, for the tfdt field of ccTrackSpec.
func ccU64(v uint64) *uint64 { return &v }

// ccSample describes one sample of a test fragment. The data are recognizable payload bytes.
type ccSample struct {
	dur  uint32
	cto  int32
	data string
}

// ccDurSource selects where the durations of a track's samples come from.
type ccDurSource int

const (
	ccDurPerSample ccDurSource = iota // the trun per-sample duration fields
	ccDurTfhdZero                     // a tfhd default sample duration that is explicitly zero
	ccDurTrex                         // neither trun nor tfhd durations: the trex default
)

// ccTrackSpec describes one traf of a test fragment. A nil tfdt strips the tfdt box. Empty runs leave the
// traf without trun boxes.
type ccTrackSpec struct {
	trackID uint32
	tfdt    *uint64
	runs    [][]ccSample // every run becomes its own trun
	source  ccDurSource
}

// ccBuildFragment returns the encoded bytes of a fragment built from the specs. The runs of the tracks are
// added round-robin, so that every run becomes its own trun and the truns of the tracks interleave in the
// mdat. The fragment is really encoded, and the tests read the bytes back.
func ccBuildFragment(t testing.TB, seq uint32, specs ...ccTrackSpec) []byte {
	t.Helper()
	ids := make([]uint32, len(specs))
	for i, s := range specs {
		ids[i] = s.trackID
	}
	frag, err := mp4.CreateMultiTrackFragment(seq, ids)
	if err != nil {
		t.Fatal(err)
	}
	for r := 0; ; r++ {
		more := false
		for _, s := range specs {
			if r >= len(s.runs) {
				continue
			}
			more = true
			for _, smp := range s.runs[r] {
				fs := mp4.FullSample{
					Sample: mp4.Sample{
						Dur:                   smp.dur,
						Size:                  uint32(len(smp.data)),
						Flags:                 mp4.SyncSampleFlags,
						CompositionTimeOffset: smp.cto,
					},
					Data: []byte(smp.data),
				}
				if err := frag.AddFullSampleToTrack(fs, s.trackID); err != nil {
					t.Fatal(err)
				}
			}
		}
		if !more {
			break
		}
	}
	for i, s := range specs {
		traf := frag.Moof.Trafs[i]
		if s.tfdt == nil {
			children := traf.Children[:0]
			for _, b := range traf.Children {
				if b.Type() != "tfdt" {
					children = append(children, b)
				}
			}
			traf.Children = children
			traf.Tfdt = nil
		} else {
			traf.Tfdt.SetBaseMediaDecodeTime(*s.tfdt)
		}
		switch s.source {
		case ccDurTfhdZero:
			traf.Tfhd.Flags |= mp4.TfhdDefaultSampleDurationPresentFlag
			traf.Tfhd.DefaultSampleDuration = 0
			for _, trun := range traf.Truns {
				trun.Flags &^= mp4.TrunSampleDurationPresentFlag
			}
		case ccDurTrex:
			for _, trun := range traf.Truns {
				trun.Flags &^= mp4.TrunSampleDurationPresentFlag
			}
		}
	}
	var buf bytes.Buffer
	if err := frag.Encode(&buf); err != nil {
		t.Fatal(err)
	}
	return buf.Bytes()
}

// ccRawSample is one sample of ccHandBuiltFragment.
type ccRawSample struct {
	dur  uint32
	data []byte
}

// ccHandBuiltFragment returns the bytes of a one-track fragment without a tfdt box:
// moof(mfhd, traf(tfhd, trun)) + mdat. It is written byte by byte with encoding/binary, independently of
// the mp4 package encoders.
func ccHandBuiltFragment(seq, trackID uint32, samples []ccRawSample) []byte {
	put32 := func(buf *bytes.Buffer, v uint32) {
		_ = binary.Write(buf, binary.BigEndian, v)
	}
	box := func(typ string, payload []byte) []byte {
		var buf bytes.Buffer
		put32(&buf, uint32(8+len(payload)))
		buf.WriteString(typ)
		buf.Write(payload)
		return buf.Bytes()
	}
	var mfhd bytes.Buffer
	put32(&mfhd, 0) // version and flags
	put32(&mfhd, seq)
	var tfhd bytes.Buffer
	put32(&tfhd, 0x020000) // flags: default-base-is-moof
	put32(&tfhd, trackID)
	var payload []byte
	for _, s := range samples {
		payload = append(payload, s.data...)
	}
	// The trun data offset is the moof size plus the mdat header size.
	moofSize := 8 + (8 + mfhd.Len()) + 8 + (8 + tfhd.Len()) + (8 + 12 + 8*len(samples))
	var trun bytes.Buffer
	put32(&trun, 0x000301) // flags: data-offset, sample-duration and sample-size present
	put32(&trun, uint32(len(samples)))
	put32(&trun, uint32(moofSize+8)) // data offset from the moof start
	for _, s := range samples {
		put32(&trun, s.dur)
		put32(&trun, uint32(len(s.data)))
	}
	traf := box("traf", append(box("tfhd", tfhd.Bytes()), box("trun", trun.Bytes())...))
	moof := box("moof", append(box("mfhd", mfhd.Bytes()), traf...))
	return append(moof, box("mdat", payload)...)
}

// ccConcat returns the parts concatenated.
func ccConcat(parts ...[]byte) []byte {
	return bytes.Join(parts, nil)
}

// ccDecode decodes the next fragment from sr and reports a decode error as a failed check.
func ccDecode(c *taskcheck.C, d *mp4.FragmentDecoder, sr *bits.FixedSliceReader) *mp4.Fragment {
	f, err := d.DecodeSR(sr, uint64(sr.GetPos()))
	c.Check(err == nil, "decode fragment: %v", err)
	if err != nil {
		return nil
	}
	return f
}

// ccCheckDTS checks that the samples of the track of trex in f have the decode times want, and returns the
// samples for further checks.
func ccCheckDTS(c *taskcheck.C, f *mp4.Fragment, trex *mp4.TrexBox, want ...uint64) []mp4.FullSample {
	samples, err := f.GetFullSamples(trex)
	c.Check(err == nil, "GetFullSamples track %d: %v", trex.TrackID, err)
	if err != nil {
		return nil
	}
	c.Check(len(samples) == len(want), "track %d: %d samples, want %d", trex.TrackID, len(samples), len(want))
	if len(samples) != len(want) {
		return samples
	}
	for i, s := range samples {
		c.Check(s.DecodeTime == want[i], "track %d sample %d: decode time %d, want %d",
			trex.TrackID, i+1, s.DecodeTime, want[i])
	}
	return samples
}

// ccCheckSampleList checks the decode times and payload bytes of got against want.
func ccCheckSampleList(c *taskcheck.C, what string, trackID uint32, got []mp4.FullSample,
	wantDTS []uint64, wantData []string) {
	c.Check(len(got) == len(wantDTS), "%s track %d: %d samples, want %d", what, trackID, len(got), len(wantDTS))
	if len(got) != len(wantDTS) {
		return
	}
	for i, s := range got {
		c.Check(s.DecodeTime == wantDTS[i], "%s track %d sample %d: decode time %d, want %d",
			what, trackID, i+1, s.DecodeTime, wantDTS[i])
		c.Check(string(s.Data) == wantData[i], "%s track %d sample %d: data %q, want %q",
			what, trackID, i+1, s.Data, wantData[i])
	}
}

// ccCheckAccessor exercises all five SampleAccessor methods for one track and checks that they report the
// same decode times and payload bytes.
func ccCheckAccessor(c *taskcheck.C, sa mp4.SampleAccessor, trackID uint32, wantDTS []uint64, wantData []string) {
	n := uint32(len(wantDTS))
	all, err := sa.GetSamples(trackID)
	c.Check(err == nil, "GetSamples track %d: %v", trackID, err)
	if err == nil {
		ccCheckSampleList(c, "GetSamples", trackID, all, wantDTS, wantData)
	}
	for i := uint32(1); i <= n; i++ {
		s, err := sa.GetSample(trackID, i)
		c.Check(err == nil, "GetSample track %d sample %d: %v", trackID, i, err)
		if err == nil {
			c.Check(s.DecodeTime == wantDTS[i-1], "GetSample track %d sample %d: decode time %d, want %d",
				trackID, i, s.DecodeTime, wantDTS[i-1])
		}
	}
	rng, err := sa.GetSampleRange(trackID, 1, n)
	c.Check(err == nil, "GetSampleRange track %d: %v", trackID, err)
	if err == nil {
		ccCheckSampleList(c, "GetSampleRange", trackID, rng, wantDTS, wantData)
	}
	app, buf, err := sa.AppendSamples(nil, nil, trackID)
	c.Check(err == nil, "AppendSamples track %d: %v", trackID, err)
	if err == nil {
		ccCheckSampleList(c, "AppendSamples", trackID, app, wantDTS, wantData)
		c.Check(string(buf) == strings.Join(wantData, ""), "AppendSamples track %d: buffer %q, want the sample data",
			trackID, buf)
	}
	ar, _, err := sa.AppendSampleRange(nil, nil, trackID, 1, n)
	c.Check(err == nil, "AppendSampleRange track %d: %v", trackID, err)
	if err == nil {
		ccCheckSampleList(c, "AppendSampleRange", trackID, ar, wantDTS, wantData)
	}
}

// ccStreamMaterial returns the init segment and the stream of three media fragments of the stream tests,
// and the decode times and payload bytes expected of every track of every fragment in continuous-clock
// mode.
func ccStreamMaterial(t testing.TB) (initBytes, stream []byte, wantDTS map[uint32][][]uint64,
	wantData map[uint32][][]string) {
	init := ccInit(t)
	var initBuf bytes.Buffer
	if err := init.Encode(&initBuf); err != nil {
		t.Fatal(err)
	}
	initBytes = initBuf.Bytes()
	frag1 := ccBuildFragment(t, 1,
		ccTrackSpec{trackID: 1, tfdt: ccU64(1000), runs: [][]ccSample{
			{{dur: 10, data: "V1"}, {dur: 10, cto: -3, data: "V2"}},
			{{dur: 30, cto: 5, data: "V3"}},
		}},
		ccTrackSpec{trackID: 2, tfdt: ccU64(500), runs: [][]ccSample{
			{{dur: 20, data: "a1"}, {dur: 20, data: "a2"}},
			{{dur: 20, data: "a3"}},
		}},
	)
	frag2 := ccBuildFragment(t, 2,
		ccTrackSpec{trackID: 1, runs: [][]ccSample{
			{{dur: 10, data: "W1"}},
		}},
		ccTrackSpec{trackID: 2, runs: [][]ccSample{
			{{dur: 20, data: "b1"}, {dur: 20, data: "b2"}},
		}},
	)
	frag3 := ccBuildFragment(t, 3,
		ccTrackSpec{trackID: 1, source: ccDurTrex, runs: [][]ccSample{
			{{data: "X1"}, {data: "X2"}},
		}},
		ccTrackSpec{trackID: 2, source: ccDurTfhdZero, runs: [][]ccSample{
			{{data: "c1"}},
		}},
	)
	stream = ccConcat(initBytes, frag1, frag2, frag3)
	wantDTS = map[uint32][][]uint64{
		1: {{1000, 1010, 1020}, {1050}, {1060, 1100}},
		2: {{500, 520, 540}, {560, 580}, {600}},
	}
	wantData = map[uint32][][]string{
		1: {{"V1", "V2", "V3"}, {"W1"}, {"X1", "X2"}},
		2: {{"a1", "a2", "a3"}, {"b1", "b2"}, {"c1"}},
	}
	return initBytes, stream, wantDTS, wantData
}

// ccDupTrafFragment returns an encoded fragment whose moof has two trafs of track 1.
func ccDupTrafFragment(t testing.TB, seq uint32) []byte {
	t.Helper()
	frag, err := mp4.CreateMultiTrackFragment(seq, []uint32{1})
	if err != nil {
		t.Fatal(err)
	}
	err = frag.AddFullSampleToTrack(mp4.FullSample{
		Sample: mp4.Sample{Dur: 10, Size: 2, Flags: mp4.SyncSampleFlags},
		Data:   []byte("D1"),
	}, 1)
	if err != nil {
		t.Fatal(err)
	}
	traf2 := &mp4.TrafBox{}
	if err := traf2.AddChild(mp4.CreateTfhd(1)); err != nil {
		t.Fatal(err)
	}
	trun2 := mp4.CreateTrun(1)
	trun2.AddSample(mp4.Sample{Dur: 10, Size: 2, Flags: mp4.SyncSampleFlags})
	if err := traf2.AddChild(trun2); err != nil {
		t.Fatal(err)
	}
	if err := frag.Moof.AddChild(traf2); err != nil {
		t.Fatal(err)
	}
	frag.Mdat.AddSampleData([]byte("D2"))
	var buf bytes.Buffer
	if err := frag.Encode(&buf); err != nil {
		t.Fatal(err)
	}
	return buf.Bytes()
}

// TestContinuousClockDecoder - continuous-clock mode of FragmentDecoder: per-track clocks that continue
// across fragments without tfdt, re-anchor on tfdt, and leave absent tracks alone, with the effective base
// reaching the Fragment sample methods. The default mode is unchanged.
func TestContinuousClockDecoder(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		init := ccInit(t)
		trex1, trex2 := init.Moov.Mvex.Trexs[0], init.Moov.Mvex.Trexs[1]
		trex3 := &mp4.TrexBox{TrackID: 3}

		// frag1: both tracks with tfdt, two runs each, per-sample durations, a negative composition
		// time offset. frag2: hand-built bytes, track 1 only, no tfdt. frag3: track 2 only, no tfdt.
		// frag4: track 1 with an explicit zero tfhd default, track 2 on the trex default, no tfdt.
		// frag5: empty traf of track 1 with tfdt 7777. frag6: empty traf of track 1 without tfdt.
		// frag7: track 1 without tfdt.
		frag1 := ccBuildFragment(t, 1,
			ccTrackSpec{trackID: 1, tfdt: ccU64(1000), runs: [][]ccSample{
				{{dur: 10, data: "V1"}, {dur: 10, cto: -3, data: "V2"}},
				{{dur: 30, cto: 5, data: "V3"}},
			}},
			ccTrackSpec{trackID: 2, tfdt: ccU64(500), runs: [][]ccSample{
				{{dur: 20, data: "a1"}, {dur: 20, data: "a2"}},
				{{dur: 20, data: "a3"}},
			}},
		)
		frag2 := ccHandBuiltFragment(2, 1, []ccRawSample{
			{dur: 25, data: []byte("W1")}, {dur: 25, data: []byte("W2")},
		})
		frag3 := ccBuildFragment(t, 3, ccTrackSpec{trackID: 2, runs: [][]ccSample{
			{{dur: 20, data: "b1"}, {dur: 20, data: "b2"}},
		}})
		frag4 := ccBuildFragment(t, 4,
			ccTrackSpec{trackID: 1, source: ccDurTfhdZero, runs: [][]ccSample{
				{{data: "X1"}, {data: "X2"}},
			}},
			ccTrackSpec{trackID: 2, source: ccDurTrex, runs: [][]ccSample{
				{{data: "c1"}, {data: "c2"}},
			}},
		)
		frag5 := ccBuildFragment(t, 5, ccTrackSpec{trackID: 1, tfdt: ccU64(7777)})
		frag6 := ccBuildFragment(t, 6, ccTrackSpec{trackID: 1})
		frag7 := ccBuildFragment(t, 7, ccTrackSpec{trackID: 1, runs: [][]ccSample{
			{{dur: 10, data: "Y1"}},
		}})
		stream := ccConcat(frag1, frag2, frag3, frag4, frag5, frag6, frag7)

		d := mp4.FragmentDecoder{Moov: init.Moov, ContinuousClock: true}
		sr := bits.NewFixedSliceReader(stream)

		f1 := ccDecode(c, &d, sr)
		if f1 == nil {
			return
		}
		samples := ccCheckDTS(c, f1, trex1, 1000, 1010, 1020)
		if len(samples) == 3 {
			c.Check(string(samples[0].Data) == "V1" && string(samples[1].Data) == "V2" && string(samples[2].Data) == "V3",
				"track 1 payload: %q, %q, %q", samples[0].Data, samples[1].Data, samples[2].Data)
			c.Check(samples[1].CompositionTimeOffset == -3,
				"sample 2 composition offset %d, want -3", samples[1].CompositionTimeOffset)
			c.Check(samples[2].CompositionTimeOffset == 5,
				"sample 3 composition offset %d, want 5", samples[2].CompositionTimeOffset)
		}
		ccCheckDTS(c, f1, trex2, 500, 520, 540)
		// AppendFullSamples and the Samples iterator report the same decode times as GetFullSamples.
		app, err := f1.AppendFullSamples(nil, trex1)
		c.Check(err == nil, "AppendFullSamples track 1: %v", err)
		c.Check(len(app) == 3, "AppendFullSamples track 1: %d samples, want 3", len(app))
		if len(app) == 3 {
			c.Check(app[0].DecodeTime == 1000 && app[1].DecodeTime == 1010 && app[2].DecodeTime == 1020,
				"AppendFullSamples track 1: decode times %d, %d, %d",
				app[0].DecodeTime, app[1].DecodeTime, app[2].DecodeTime)
		}
		var iterDTS []uint64
		for s, err := range f1.Samples(trex1) {
			if err != nil {
				c.Check(false, "Samples track 1: %v", err)
				break
			}
			iterDTS = append(iterDTS, s.DecodeTime)
		}
		c.Check(slices.Equal(iterDTS, []uint64{1000, 1010, 1020}), "Samples track 1: decode times %v", iterDTS)
		// Reading the samples of a fragment repeatedly does not move the clocks again.
		ccCheckDTS(c, f1, trex1, 1000, 1010, 1020)
		// Re-encoding the decoded fragment keeps the time fields and media bytes of the original.
		var enc1 bytes.Buffer
		err = f1.Encode(&enc1)
		c.Check(err == nil, "re-encode fragment 1: %v", err)
		c.Check(bytes.Equal(enc1.Bytes(), frag1), "re-encoded fragment 1 differs from the original bytes")

		// The hand-built fragment without tfdt continues the clock of track 1; track 2 is absent and
		// its clock does not move.
		f2 := ccDecode(c, &d, sr)
		if f2 == nil {
			return
		}
		samples2 := ccCheckDTS(c, f2, trex1, 1050, 1075)
		if len(samples2) == 2 {
			c.Check(string(samples2[0].Data) == "W1" && string(samples2[1].Data) == "W2",
				"hand-built fragment payload: %q, %q", samples2[0].Data, samples2[1].Data)
		}
		ccCheckDTS(c, f2, trex1, 1050, 1075) // repeated read of the same fragment
		var enc2 bytes.Buffer
		err = f2.Encode(&enc2)
		c.Check(err == nil, "re-encode hand-built fragment: %v", err)
		c.Check(bytes.Equal(enc2.Bytes(), frag2), "re-encoded hand-built fragment differs from its bytes")

		// Track 2 continues where fragment 1 ended, although fragment 2 had no traf of it.
		f3 := ccDecode(c, &d, sr)
		if f3 == nil {
			return
		}
		ccCheckDTS(c, f3, trex2, 560, 580)

		// The explicit zero tfhd default of track 1 is a valid duration source and does not move the
		// clock; track 2 takes its durations from its trex.
		f4 := ccDecode(c, &d, sr)
		if f4 == nil {
			return
		}
		ccCheckDTS(c, f4, trex1, 1100, 1100)
		ccCheckDTS(c, f4, trex2, 600, 620)

		// The empty traf of fragment 5 only re-anchors track 1 to 7777, and the empty traf without
		// tfdt of fragment 6 does not move the clock.
		f5 := ccDecode(c, &d, sr)
		if f5 == nil {
			return
		}
		c.Check(len(f5.Moof.Trafs) == 1 && len(f5.Moof.Traf.Truns) == 0,
			"fragment 5: %d trafs, %d truns, want an empty traf", len(f5.Moof.Trafs), len(f5.Moof.Traf.Truns))
		f6 := ccDecode(c, &d, sr)
		if f6 == nil {
			return
		}
		f7 := ccDecode(c, &d, sr)
		if f7 == nil {
			return
		}
		ccCheckDTS(c, f7, trex1, 7777)

		// A moof with two trafs of one track is rejected, and the clocks stay where they were.
		dup := ccDupTrafFragment(t, 8)
		_, err = d.DecodeSR(bits.NewFixedSliceReader(dup), 0)
		c.Check(err != nil && strings.Contains(err.Error(), "more than one traf"),
			"two trafs of track 1: error %v", err)
		frag9 := ccBuildFragment(t, 9, ccTrackSpec{trackID: 1, runs: [][]ccSample{
			{{dur: 10, data: "E1"}},
		}})
		f9 := ccDecode(c, &d, bits.NewFixedSliceReader(frag9))
		if f9 == nil {
			return
		}
		ccCheckDTS(c, f9, trex1, 7787)

		// A non-empty run without a duration source (no trun or tfhd duration, no trex of the track)
		// is an error, and does not move any clock.
		frag10 := ccBuildFragment(t, 10, ccTrackSpec{trackID: 3, tfdt: ccU64(100), source: ccDurTrex,
			runs: [][]ccSample{
				{{data: "N1"}, {data: "N2"}},
			}})
		_, err = d.DecodeSR(bits.NewFixedSliceReader(frag10), 0)
		c.Check(err != nil && strings.Contains(err.Error(), "no duration source"),
			"track 3 without duration source: error %v", err)

		// Track 3 appears for the first time in a later fragment, without tfdt, and starts from 0.
		frag11 := ccBuildFragment(t, 11,
			ccTrackSpec{trackID: 1, runs: [][]ccSample{
				{{dur: 10, data: "F1"}},
			}},
			ccTrackSpec{trackID: 3, runs: [][]ccSample{
				{{dur: 15, data: "G1"}, {dur: 15, data: "G2"}},
			}},
		)
		f11 := ccDecode(c, &d, bits.NewFixedSliceReader(frag11))
		if f11 == nil {
			return
		}
		ccCheckDTS(c, f11, trex1, 7797)
		ccCheckDTS(c, f11, trex3, 0, 15)

		// The default decoder is unchanged: fragments without tfdt start at 0, and the hand-built
		// bytes parse as a standard fragment.
		dd := mp4.FragmentDecoder{Moov: init.Moov}
		dsr := bits.NewFixedSliceReader(stream)
		df1 := ccDecode(c, &dd, dsr)
		if df1 == nil {
			return
		}
		ccCheckDTS(c, df1, trex1, 1000, 1010, 1020) // tfdt present: as in continuous-clock mode
		df2 := ccDecode(c, &dd, dsr)
		if df2 == nil {
			return
		}
		dsamples2 := ccCheckDTS(c, df2, trex1, 0, 25)
		if len(dsamples2) == 2 {
			c.Check(string(dsamples2[0].Data) == "W1" && string(dsamples2[1].Data) == "W2",
				"default decode of hand-built fragment: %q, %q", dsamples2[0].Data, dsamples2[1].Data)
		}
		df3 := ccDecode(c, &dd, dsr)
		if df3 == nil {
			return
		}
		ccCheckDTS(c, df3, trex2, 0, 20)
		for i := 0; i < 3; i++ { // fragments 4-6
			if ccDecode(c, &dd, dsr) == nil {
				return
			}
		}
		df7 := ccDecode(c, &dd, dsr)
		if df7 == nil {
			return
		}
		ccCheckDTS(c, df7, trex1, 0)
	})
}

// TestContinuousClockStream - continuous-clock mode of StreamFile: the effective base reaches the
// streaming SampleAccessor, the clocks of all tracks advance whether or not a callback reads their
// samples, and the sliding window does not disturb them.
func TestContinuousClockStream(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		initBytes, stream, wantDTS, wantData := ccStreamMaterial(t)

		// Every accessor method of every track of every fragment reports the continuous decode times.
		sf, err := mp4.InitDecodeStream(bytes.NewReader(stream),
			mp4.WithContinuousClock(),
			mp4.WithFragmentCallback(func(f *mp4.Fragment, sa mp4.SampleAccessor) error {
				fragNr := int(f.Moof.Mfhd.SequenceNumber) - 1
				for _, trackID := range []uint32{1, 2} {
					ccCheckAccessor(c, sa, trackID, wantDTS[trackID][fragNr], wantData[trackID][fragNr])
				}
				if fragNr == 0 {
					s, err := sa.GetSample(1, 2)
					c.Check(err == nil, "GetSample track 1 sample 2: %v", err)
					if err == nil {
						c.Check(s.CompositionTimeOffset == -3,
							"stream sample 2 composition offset %d, want -3", s.CompositionTimeOffset)
					}
				}
				return nil
			}))
		c.Check(err == nil, "InitDecodeStream: %v", err)
		if err != nil {
			return
		}
		err = sf.ProcessFragments()
		c.Check(err == nil, "ProcessFragments: %v", err)

		// A callback that reads only track 1: the clock of track 2 still advances, as the decode
		// times of the last fragment show.
		fragNr := 0
		sf2, err := mp4.InitDecodeStream(bytes.NewReader(stream),
			mp4.WithContinuousClock(),
			mp4.WithFragmentCallback(func(f *mp4.Fragment, sa mp4.SampleAccessor) error {
				fragNr++
				if fragNr < 3 {
					_, err := sa.GetSamples(1) // only track 1
					return err
				}
				samples, err := sa.GetSamples(2)
				c.Check(err == nil, "last fragment GetSamples track 2: %v", err)
				if err == nil {
					ccCheckSampleList(c, "GetSamples", 2, samples, wantDTS[2][2], wantData[2][2])
				}
				return nil
			}))
		c.Check(err == nil, "InitDecodeStream one-track callback: %v", err)
		if err != nil {
			return
		}
		err = sf2.ProcessFragments()
		c.Check(err == nil, "ProcessFragments one-track callback: %v", err)

		// No callback at all: the clocks advance, as the retained fragments show. The samples have
		// zero size, so that their decode times can be read from the retained fragments whose mdat
		// data is no longer in memory.
		z1 := ccBuildFragment(t, 1, ccTrackSpec{trackID: 1, tfdt: ccU64(700), runs: [][]ccSample{
			{{dur: 10}, {dur: 10}},
		}})
		z2 := ccBuildFragment(t, 2, ccTrackSpec{trackID: 1, runs: [][]ccSample{
			{{dur: 10}},
		}})
		sf3, err := mp4.InitDecodeStream(bytes.NewReader(ccConcat(initBytes, z1, z2)), mp4.WithContinuousClock())
		c.Check(err == nil, "InitDecodeStream without callback: %v", err)
		if err != nil {
			return
		}
		err = sf3.ProcessFragments()
		c.Check(err == nil, "ProcessFragments without callback: %v", err)
		if err != nil {
			return
		}
		trex1, _ := sf3.Moov.Mvex.GetTrex(1)
		frags := sf3.GetActiveFragments()
		c.Check(len(frags) == 2, "retained fragments: %d, want 2", len(frags))
		if len(frags) == 2 {
			ccCheckDTS(c, frags[0], trex1, 700, 710)
			ccCheckDTS(c, frags[1], trex1, 720)
			var iterDTS []uint64
			for s, err := range frags[1].Samples(trex1) {
				if err != nil {
					c.Check(false, "Samples of retained fragment: %v", err)
					break
				}
				iterDTS = append(iterDTS, s.DecodeTime)
			}
			c.Check(slices.Equal(iterDTS, []uint64{720}), "Samples of retained fragment: decode times %v", iterDTS)
		}

		// A sliding window of one fragment: the evicted fragments do not take their clocks with
		// them, and the decode times stay continuous.
		var gotDTS []uint64
		sf4, err := mp4.InitDecodeStream(bytes.NewReader(stream),
			mp4.WithContinuousClock(),
			mp4.WithMaxFragments(1),
			mp4.WithFragmentCallback(func(f *mp4.Fragment, sa mp4.SampleAccessor) error {
				samples, err := sa.GetSamples(1)
				if err != nil {
					return err
				}
				for _, s := range samples {
					gotDTS = append(gotDTS, s.DecodeTime)
				}
				return nil
			}))
		c.Check(err == nil, "InitDecodeStream sliding window: %v", err)
		if err != nil {
			return
		}
		err = sf4.ProcessFragments()
		c.Check(err == nil, "ProcessFragments sliding window: %v", err)
		c.Check(slices.Equal(gotDTS, []uint64{1000, 1010, 1020, 1050, 1060, 1100}),
			"sliding window track 1 decode times: %v", gotDTS)
		active := sf4.GetActiveFragments()
		c.Check(len(active) == 1, "sliding window retained %d fragments, want 1", len(active))
		if len(active) == 1 {
			c.Check(active[0].Moof.Mfhd.SequenceNumber == 3,
				"sliding window retained fragment %d, want 3", active[0].Moof.Mfhd.SequenceNumber)
		}

		// The default stream is unchanged: the fragments without tfdt start at 0.
		var defaultDTS []uint64
		sf5, err := mp4.InitDecodeStream(bytes.NewReader(stream),
			mp4.WithFragmentCallback(func(f *mp4.Fragment, sa mp4.SampleAccessor) error {
				samples, err := sa.GetSamples(1)
				if err != nil {
					return err
				}
				for _, s := range samples {
					defaultDTS = append(defaultDTS, s.DecodeTime)
				}
				return nil
			}))
		c.Check(err == nil, "InitDecodeStream default: %v", err)
		if err != nil {
			return
		}
		err = sf5.ProcessFragments()
		c.Check(err == nil, "ProcessFragments default: %v", err)
		c.Check(slices.Equal(defaultDTS, []uint64{1000, 1010, 1020, 0, 0, 40}),
			"default stream track 1 decode times: %v", defaultDTS)
	})
}

// TestContinuousClockRollback - re-anchoring backward, atomic rejection of a fragment whose later track
// overflows, continuing from the last good fragment after a truncated one, io.EOF keeping the clocks, and
// clearing them.
func TestContinuousClockRollback(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		init := ccInit(t)
		trex1 := init.Moov.Mvex.Trexs[0]
		d := mp4.FragmentDecoder{Moov: init.Moov, ContinuousClock: true}

		fragA := ccBuildFragment(t, 1, ccTrackSpec{trackID: 1, tfdt: ccU64(100), runs: [][]ccSample{
			{{dur: 10, data: "R1"}},
		}})
		fragB := ccBuildFragment(t, 2, ccTrackSpec{trackID: 1, tfdt: ccU64(30), runs: [][]ccSample{
			{{dur: 10, data: "R2"}},
		}})
		fragC := ccBuildFragment(t, 3,
			ccTrackSpec{trackID: 1, tfdt: ccU64(1000), runs: [][]ccSample{
				{{dur: 10, data: "C1"}},
			}},
			ccTrackSpec{trackID: 2, tfdt: ccU64(math.MaxUint64 - 5), runs: [][]ccSample{
				{{dur: 10, data: "C2"}},
			}},
		)
		fragD := ccBuildFragment(t, 4, ccTrackSpec{trackID: 1, runs: [][]ccSample{
			{{dur: 10, data: "R3"}},
		}})
		fragE := ccBuildFragment(t, 5, ccTrackSpec{trackID: 1, runs: [][]ccSample{
			{{dur: 10, data: "R4"}},
		}})
		fragF := ccBuildFragment(t, 6, ccTrackSpec{trackID: 1, runs: [][]ccSample{
			{{dur: 10, data: "R5"}},
		}})
		fragG := ccBuildFragment(t, 7, ccTrackSpec{trackID: 1, runs: [][]ccSample{
			{{dur: 10, data: "R6"}},
		}})

		decodeOne := func(data []byte) *mp4.Fragment {
			f, err := d.DecodeSR(bits.NewFixedSliceReader(data), 0)
			c.Check(err == nil, "decode fragment: %v", err)
			if err != nil {
				return nil
			}
			return f
		}

		f := decodeOne(fragA)
		if f == nil {
			return
		}
		ccCheckDTS(c, f, trex1, 100)

		// A tfdt that moves the clock backward re-anchors the track.
		f = decodeOne(fragB)
		if f == nil {
			return
		}
		ccCheckDTS(c, f, trex1, 30)

		// Track 2 of fragC overflows uint64, so the whole fragment is rejected: the re-anchor of
		// track 1 is not committed either, and fragD continues from fragB.
		_, err := d.DecodeSR(bits.NewFixedSliceReader(fragC), 0)
		c.Check(err != nil && strings.Contains(err.Error(), "overflow"),
			"fragment with overflowing track 2: error %v", err)
		f = decodeOne(fragD)
		if f == nil {
			return
		}
		ccCheckDTS(c, f, trex1, 40)

		// A truncated fragment leaves the clocks where the last good one put them, and the same
		// decoder object decodes the complete fragment.
		_, err = d.DecodeSR(bits.NewFixedSliceReader(fragE[:len(fragE)/2]), 0)
		c.Check(errors.Is(err, io.ErrUnexpectedEOF), "truncated fragment: error %v, want io.ErrUnexpectedEOF", err)
		f = decodeOne(fragE)
		if f == nil {
			return
		}
		ccCheckDTS(c, f, trex1, 50)

		// io.EOF does not clear the clocks.
		_, err = d.DecodeSR(bits.NewFixedSliceReader(nil), 0)
		c.Check(errors.Is(err, io.EOF), "empty input: error %v, want io.EOF", err)
		f = decodeOne(fragF)
		if f == nil {
			return
		}
		ccCheckDTS(c, f, trex1, 60)

		// Clearing the clocks starts the track from 0 again.
		d.ClearTrackClocks()
		f = decodeOne(fragG)
		if f == nil {
			return
		}
		ccCheckDTS(c, f, trex1, 0)
	})
}
