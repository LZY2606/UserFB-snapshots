package mp4_test

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"math"
	"testing"

	"github.com/Eyevinn/mp4ff/bits"
	"github.com/Eyevinn/mp4ff/mp4"
	"github.com/Eyevinn/mp4ff/task_support/taskcheck"
)

// The tests in this file exercise the optional continuous decode-time clock. The media is a real dual-track
// fragmented stream built with the mp4 encoders, encoded to bytes and decoded again: the fragment headers are
// then mutated to drop tfdt boxes, trun duration fields and tfhd defaults, and one fragment header is built from
// raw bytes without ever going through the encoder. An independent simulation of the clock rules, written in
// this file, is the oracle for every returned DecodeTime.

const (
	ccVideoTrack   uint32 = 1
	ccAudioTrack   uint32 = 2
	ccVideoScale   uint32 = 1000
	ccAudioScale   uint32 = 48000
	ccTrexAudioDur uint32 = 512
)

// ccTrunSpec describes one trun of a traf.
type ccTrunSpec struct {
	durs           []uint32 // per-sample durations used when noDurFlag is false
	sizes          []uint32 // per-sample media sizes
	ctos           []int32  // per-sample composition time offsets (signed, trun version 1)
	noDurFlag      bool     // strip the trun sample-duration-present flag
	tfhdDefaultDur *uint32  // when non-nil, set tfhd default-sample-duration with this value (zero is valid)
}

// ccTrafSpec describes one traf of a fragment.
type ccTrafSpec struct {
	trackID uint32
	tfdt    *uint64 // nil means the traf has no tfdt box
	truns   []ccTrunSpec
}

// ccFragSpec describes one fragment. rawNoTfdt builds its single video traf from raw bytes instead of the encoder.
type ccFragSpec struct {
	seq   uint32
	trafs []ccTrafSpec
	raw   bool
}

func ccU64(v uint64) *uint64 { return &v }
func ccU32(v uint32) *uint32 { return &v }

// ccFixture is the shared dual-track sequence:
//  1. both tracks, explicit tfdt, two truns on video including a negative composition offset
//  2. no tfdt anywhere, audio absent (missing track)
//  3. no tfdt, video tfhd default duration, audio trex fallback
//  4. no tfdt, explicit zero tfhd duration on video, an explicit zero trun duration on audio
//  5. empty traf on both tracks, no tfdt (clocks must not move)
//  6. tfdt re-anchors both tracks backwards
//  7. raw-bytes video-only fragment without tfdt (audio absent)
func ccFixture() []ccFragSpec {
	return []ccFragSpec{
		{seq: 1, trafs: []ccTrafSpec{
			{trackID: ccVideoTrack, tfdt: ccU64(1000), truns: []ccTrunSpec{
				{durs: []uint32{10, 20}, sizes: []uint32{2, 3}, ctos: []int32{-1, 0}},
				{durs: []uint32{30}, sizes: []uint32{1}, ctos: []int32{2}},
			}},
			{trackID: ccAudioTrack, tfdt: ccU64(8000), truns: []ccTrunSpec{
				{durs: []uint32{1024, 1024}, sizes: []uint32{4, 5}, ctos: []int32{0, 0}},
			}},
		}},
		{seq: 2, trafs: []ccTrafSpec{
			{trackID: ccVideoTrack, truns: []ccTrunSpec{
				{durs: []uint32{10, 15}, sizes: []uint32{2, 2}, ctos: []int32{0, 0}},
			}},
		}},
		{seq: 3, trafs: []ccTrafSpec{
			{trackID: ccVideoTrack, truns: []ccTrunSpec{
				{durs: nil, sizes: []uint32{2, 3}, ctos: []int32{0, 0}, noDurFlag: true, tfhdDefaultDur: ccU32(7)},
			}},
			{trackID: ccAudioTrack, truns: []ccTrunSpec{
				{durs: nil, sizes: []uint32{4, 4}, ctos: []int32{0, 0}, noDurFlag: true},
			}},
		}},
		{seq: 4, trafs: []ccTrafSpec{
			{trackID: ccVideoTrack, truns: []ccTrunSpec{
				{durs: nil, sizes: []uint32{2}, ctos: []int32{0}, noDurFlag: true, tfhdDefaultDur: ccU32(0)},
			}},
			{trackID: ccAudioTrack, truns: []ccTrunSpec{
				{durs: []uint32{0, 512}, sizes: []uint32{3, 3}, ctos: []int32{0, 0}},
			}},
		}},
		{seq: 5, trafs: []ccTrafSpec{
			{trackID: ccVideoTrack},
			{trackID: ccAudioTrack},
		}},
		{seq: 6, trafs: []ccTrafSpec{
			{trackID: ccVideoTrack, tfdt: ccU64(500), truns: []ccTrunSpec{
				{durs: []uint32{10}, sizes: []uint32{2}, ctos: []int32{0}},
			}},
			{trackID: ccAudioTrack, tfdt: ccU64(2000), truns: []ccTrunSpec{
				{durs: []uint32{512}, sizes: []uint32{4}, ctos: []int32{0}},
			}},
		}},
		{seq: 7, raw: true, trafs: []ccTrafSpec{
			{trackID: ccVideoTrack, truns: []ccTrunSpec{
				{durs: []uint32{10, 10}, sizes: []uint32{2, 2}, ctos: []int32{0, 0}},
			}},
		}},
		// Empty trafs with a tfdt: the clocks re-anchor but get no duration added.
		{seq: 8, trafs: []ccTrafSpec{
			{trackID: ccVideoTrack, tfdt: ccU64(300)},
			{trackID: ccAudioTrack, tfdt: ccU64(7000)},
		}},
		// No tfdt: the first samples continue exactly from the re-anchored empty trafs.
		{seq: 9, trafs: []ccTrafSpec{
			{trackID: ccVideoTrack, truns: []ccTrunSpec{
				{durs: []uint32{10}, sizes: []uint32{2}, ctos: []int32{0}},
			}},
			{trackID: ccAudioTrack, truns: []ccTrunSpec{
				{durs: []uint32{512}, sizes: []uint32{4}, ctos: []int32{0}},
			}},
		}},
	}
}

// ccClock is the test's own copy of the clock state.
type ccClock struct {
	next  uint64
	known bool
}

// ccExpectedDTS simulates the continuous-clock rules independently of the implementation and returns the
// expected DecodeTime of every sample of every track of every fragment.
func ccExpectedDTS(specs []ccFragSpec) []map[uint32][]uint64 {
	trexDur := map[uint32]uint32{ccAudioTrack: ccTrexAudioDur}
	clocks := map[uint32]ccClock{}
	out := make([]map[uint32][]uint64, len(specs))
	for fi, fs := range specs {
		out[fi] = map[uint32][]uint64{}
		for _, ts := range fs.trafs {
			cur := clocks[ts.trackID]
			var base uint64
			switch {
			case ts.tfdt != nil:
				base = *ts.tfdt
			case cur.known:
				base = cur.next
			default:
				base = 0
			}
			t := base
			var dts []uint64
			nrSamples := 0
			for _, r := range ts.truns {
				for j := range r.sizes {
					dts = append(dts, t)
					var d uint32
					switch {
					case !r.noDurFlag:
						d = r.durs[j]
					case r.tfhdDefaultDur != nil:
						d = *r.tfhdDefaultDur
					default:
						d = trexDur[ts.trackID]
					}
					t += uint64(d)
					nrSamples++
				}
			}
			out[fi][ts.trackID] = dts
			switch {
			case nrSamples == 0 && ts.tfdt != nil:
				clocks[ts.trackID] = ccClock{next: *ts.tfdt, known: true} // empty traf with tfdt re-anchors
			case nrSamples > 0:
				clocks[ts.trackID] = ccClock{next: t, known: true}
			default:
				// empty traf without tfdt: the clock stays untouched
			}
		}
	}
	return out
}

// ccSampleData fills a sample payload with bytes that identify the fragment, track, trun and sample by position.
func ccSampleData(fragSeq, trackID, trunNr, sampleNr, size int) []byte {
	data := make([]byte, size)
	for i := range data {
		data[i] = byte(0xA0 + ((fragSeq*7 + trackID*3 + trunNr*5 + sampleNr*11 + i) & 0x0f))
	}
	data[0] = byte(fragSeq)
	if size >= 2 {
		data[1] = byte(trackID)
	}
	return data
}

// ccRawBox encodes one box with the given type and payload.
func ccRawBox(typ string, payload []byte) []byte {
	b := make([]byte, 8+len(payload))
	binary.BigEndian.PutUint32(b, uint32(len(b)))
	copy(b[4:8], typ)
	copy(b[8:], payload)
	return b
}

// ccRawNoTfdtFragment builds a complete single-track fragment (moof + mdat) from raw bytes, with a traf that has
// tfhd and trun but no tfdt. It never uses the mp4 encoders, so decoding it proves the clock does not only work
// on files the library wrote itself.
func ccRawNoTfdtFragment(seq, trackID uint32, tr ccTrunSpec) []byte {
	n := len(tr.sizes)
	// trun: version 1, flags data-offset|sample-duration|sample-size|composition-offset (0x000b01)
	trunPayload := make([]byte, 12+12*n)
	binary.BigEndian.PutUint32(trunPayload[0:], (1<<24)|0x000b01)
	binary.BigEndian.PutUint32(trunPayload[4:], uint32(n))
	for j := 0; j < n; j++ {
		e := trunPayload[12+12*j:]
		binary.BigEndian.PutUint32(e[0:], tr.durs[j])
		binary.BigEndian.PutUint32(e[4:], tr.sizes[j])
		binary.BigEndian.PutUint32(e[8:], uint32(int32(tr.ctos[j])))
	}
	mfhd := ccRawBox("mfhd", ccU32BE(0, seq))
	tfhdPayload := make([]byte, 8)
	binary.BigEndian.PutUint32(tfhdPayload[0:], 0x020000) // default-base-is-moof
	binary.BigEndian.PutUint32(tfhdPayload[4:], trackID)
	tfhd := ccRawBox("tfhd", tfhdPayload)
	// The box sizes are fixed once the sample count is known, so the trun data offset (relative to the moof
	// start and pointing at the mdat payload) can be written before any box copies the payload.
	trunSize := 8 + len(trunPayload)
	trafSize := 8 + len(tfhd) + trunSize
	moofSize := 8 + len(mfhd) + trafSize
	binary.BigEndian.PutUint32(trunPayload[8:], uint32(moofSize+8))
	trun := ccRawBox("trun", trunPayload)
	traf := ccRawBox("traf", bytes.Join([][]byte{tfhd, trun}, nil))
	moof := ccRawBox("moof", bytes.Join([][]byte{mfhd, traf}, nil))
	var payload []byte
	for j := 0; j < n; j++ {
		payload = append(payload, ccSampleData(int(seq), int(trackID), 0, j, int(tr.sizes[j]))...)
	}
	mdat := ccRawBox("mdat", payload)
	return bytes.Join([][]byte{moof, mdat}, nil)
}

// ccU32BE encodes two uint32 values the way a full box starts plus its one field.
func ccU32BE(versionFlags, value uint32) []byte {
	b := make([]byte, 8)
	binary.BigEndian.PutUint32(b[0:], versionFlags)
	binary.BigEndian.PutUint32(b[4:], value)
	return b
}

// ccBuildFragment encodes one fragment spec with the real mp4 encoders and then applies the header mutations:
// tfdt boxes are removed where the spec says so, trun duration fields and tfhd defaults are changed in place.
func ccBuildFragment(t *testing.T, fs ccFragSpec) []byte {
	t.Helper()
	if fs.raw {
		if len(fs.trafs) != 1 || fs.trafs[0].tfdt != nil || len(fs.trafs[0].truns) != 1 {
			t.Fatalf("raw fragment %d needs one traf without tfdt and one trun", fs.seq)
		}
		return ccRawNoTfdtFragment(fs.seq, fs.trafs[0].trackID, fs.trafs[0].truns[0])
	}
	trackIDs := make([]uint32, 0, len(fs.trafs))
	for _, ts := range fs.trafs {
		trackIDs = append(trackIDs, ts.trackID)
	}
	frag, err := mp4.CreateMultiTrackFragment(fs.seq, trackIDs)
	if err != nil {
		t.Fatal(err)
	}
	writeOrder := uint32(0)
	for ti, ts := range fs.trafs {
		traf := frag.Moof.Trafs[ti]
		for ri, r := range ts.truns {
			trun := mp4.CreateTrun(writeOrder)
			writeOrder++
			if err := traf.AddChild(trun); err != nil {
				t.Fatal(err)
			}
			for si := range r.sizes {
				var dur uint32
				if !r.noDurFlag {
					dur = r.durs[si]
				}
				s := mp4.Sample{
					Flags:                 mp4.SyncSampleFlags,
					Dur:                   dur,
					Size:                  r.sizes[si],
					CompositionTimeOffset: r.ctos[si],
				}
				trun.AddSample(s)
				frag.Mdat.AddSampleData(ccSampleData(int(fs.seq), int(ts.trackID), ri, si, int(r.sizes[si])))
			}
			if r.noDurFlag {
				trun.Flags &^= mp4.TrunSampleDurationPresentFlag
			}
			if r.tfhdDefaultDur != nil {
				traf.Tfhd.Flags |= mp4.TfhdDefaultSampleDurationPresentFlag
				traf.Tfhd.DefaultSampleDuration = *r.tfhdDefaultDur
			}
		}
		if ts.tfdt == nil {
			children := traf.Children[:0]
			for _, b := range traf.Children {
				if b.Type() != "tfdt" {
					children = append(children, b)
				}
			}
			traf.Children = children
			traf.Tfdt = nil
		} else {
			traf.Tfdt.SetBaseMediaDecodeTime(*ts.tfdt)
		}
	}
	var buf bytes.Buffer
	if err := frag.Encode(&buf); err != nil {
		t.Fatal(err)
	}
	return buf.Bytes()
}

// ccBuildStream encodes every fragment of specs in order.
func ccBuildStream(t *testing.T, specs []ccFragSpec) [][]byte {
	t.Helper()
	out := make([][]byte, len(specs))
	for i, fs := range specs {
		out[i] = ccBuildFragment(t, fs)
	}
	return out
}

// ccInit builds (and encodes, when encoded is true) a two-track init segment whose audio trex carries the
// default sample duration used by truns without duration fields.
func ccInit(t *testing.T, encoded bool) (*mp4.InitSegment, []byte) {
	t.Helper()
	init := mp4.CreateEmptyInit()
	init.AddEmptyTrack(ccVideoScale, "video", "und")
	init.AddEmptyTrack(ccAudioScale, "soun", "und")
	trex, ok := init.Moov.Mvex.GetTrex(ccAudioTrack)
	if !ok {
		t.Fatal("no audio trex")
	}
	trex.DefaultSampleDuration = ccTrexAudioDur
	if !encoded {
		return init, nil
	}
	var buf bytes.Buffer
	if err := init.Encode(&buf); err != nil {
		t.Fatal(err)
	}
	return init, buf.Bytes()
}

// ccDTSMap collects DecodeTime values per track from full samples.
func ccDTSMap(ss []mp4.FullSample) []uint64 {
	out := make([]uint64, len(ss))
	for i, s := range ss {
		out[i] = s.DecodeTime
	}
	return out
}

// ccU64Equal compares two uint64 slices.
func ccU64Equal(a, b []uint64) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

// TestContinuousClockDecoder drives one FragmentDecoder in continuous-clock mode through the dual-track
// fixture and checks every sample access path, the missing-track, default, zero, trex, empty and re-anchor
// cases, payload and composition offsets, and that reading a fragment again does not move the clocks.
func TestContinuousClockDecoder(t *testing.T) {
	specs := ccFixture()
	fragments := ccBuildStream(t, specs)
	want := ccExpectedDTS(specs)
	init, _ := ccInit(t, false)

	var media bytes.Buffer
	for _, f := range fragments {
		media.Write(f)
	}

	taskcheck.Run(t, func(c *taskcheck.C) {
		d := &mp4.FragmentDecoder{Moov: init.Moov, ContinuousClock: true}
		sr := bits.NewFixedSliceReader(media.Bytes())
		trexByTrack := map[uint32]*mp4.TrexBox{}
		for _, trex := range init.Moov.Mvex.Trexs {
			trexByTrack[trex.TrackID] = trex
		}
		for fi := 0; ; fi++ {
			f, err := d.DecodeSR(sr, uint64(sr.GetPos()))
			if errors.Is(err, io.EOF) {
				c.Check(fi == len(specs), "%s", fmt.Sprintf("decoded %d fragments, want %d", fi, len(specs)))
				break
			}
			c.Check(err == nil, "%s", fmt.Sprintf("fragment %d decode: %v", fi+1, err))
			if err != nil {
				break
			}
			spec := specs[fi]
			c.Check(len(f.Moof.Trafs) == len(spec.trafs), "%s", fmt.Sprintf("fragment %d traf count", fi+1))
			for ti, ts := range spec.trafs {
				traf := f.Moof.Trafs[ti]
				c.Check(traf.Tfhd.TrackID == ts.trackID, "%s", fmt.Sprintf("fragment %d track id", fi+1))
				c.Check((traf.Tfdt != nil) == (ts.tfdt != nil), "%s",
					fmt.Sprintf("fragment %d track %d tfdt presence unchanged", fi+1, ts.trackID))
				trex := trexByTrack[ts.trackID]

				// GetFullSamples
				full, err := f.GetFullSamples(trex)
				c.Check(err == nil, "%s", fmt.Sprintf("fragment %d track %d GetFullSamples: %v", fi+1, ts.trackID, err))
				c.Check(ccU64Equal(ccDTSMap(full), want[fi][ts.trackID]), "%s",
					fmt.Sprintf("fragment %d track %d GetFullSamples DTS %v, want %v", fi+1, ts.trackID,
						ccDTSMap(full), want[fi][ts.trackID]))

				// AppendFullSamples with caller-provided storage
				var appended []mp4.FullSample
				appended, err = f.AppendFullSamples(appended, trex)
				c.Check(err == nil, "%s", fmt.Sprintf("AppendFullSamples: %v", err))
				c.Check(ccU64Equal(ccDTSMap(appended), want[fi][ts.trackID]), "AppendFullSamples DTS mismatch")

				// The Samples iterator must give the same decode times without building a slice.
				var iterDTS []uint64
				for s, err := range f.Samples(trex) {
					c.Check(err == nil, "%s", fmt.Sprintf("Samples iterator: %v", err))
					iterDTS = append(iterDTS, s.DecodeTime)
				}
				c.Check(ccU64Equal(iterDTS, want[fi][ts.trackID]), "Samples iterator DTS mismatch")

				// Repeated reads return identical values and cannot advance the clocks again.
				again, err := f.GetFullSamples(trex)
				c.Check(err == nil, "%s", fmt.Sprintf("re-read: %v", err))
				c.Check(ccU64Equal(ccDTSMap(again), want[fi][ts.trackID]), "re-read changed the DTS")

				// Payload bytes and composition offsets survive unchanged.
				sampleNr := 0
				for ri, r := range ts.truns {
					for si := range r.sizes {
						wantData := ccSampleData(int(spec.seq), int(ts.trackID), ri, si, int(r.sizes[si]))
						c.Check(bytes.Equal(full[sampleNr].Data, wantData), "%s",
							fmt.Sprintf("fragment %d track %d sample %d payload", fi+1, ts.trackID, sampleNr))
						c.Check(full[sampleNr].CompositionTimeOffset == r.ctos[si], "%s",
							fmt.Sprintf("fragment %d track %d sample %d composition offset", fi+1, ts.trackID, sampleNr))
						sampleNr++
					}
				}
			}
			// A track absent from this fragment has no clock base recorded on it.
			present := map[uint32]bool{}
			for _, ts := range spec.trafs {
				present[ts.trackID] = true
			}
			for _, trackID := range []uint32{ccVideoTrack, ccAudioTrack} {
				if !present[trackID] {
					trex := trexByTrack[trackID]
					ss, err := f.GetFullSamples(trex)
					c.Check(err == nil, "absent track lookup")
					c.Check(len(ss) == 0, "absent track appends no samples")
				}
			}

			// The clock only changes returned DecodeTime values: re-encoding the decoded fragment must give
			// the original bytes, with the tfdt boxes and every media byte untouched.
			var reEncoded bytes.Buffer
			c.Check(f.Encode(&reEncoded) == nil, "re-encode decoded fragment")
			c.Check(bytes.Equal(reEncoded.Bytes(), fragments[fi]), "%s",
				fmt.Sprintf("fragment %d re-encoded bytes differ from the input", fi+1))
		}

		// Hand-checked anchors along the way, including the backwards re-anchor and the raw fragment.
		d2 := &mp4.FragmentDecoder{Moov: init.Moov, ContinuousClock: true}
		sr2 := bits.NewFixedSliceReader(media.Bytes())
		var firstVideo, rolledBackVideo, rawVideoFirst, reanchoredVideo, reanchoredAudio []uint64
		for fi := 0; ; fi++ {
			f, err := d2.DecodeSR(sr2, uint64(sr2.GetPos()))
			if errors.Is(err, io.EOF) {
				break
			}
			c.Check(err == nil, "%s", fmt.Sprintf("second pass fragment %d: %v", fi+1, err))
			if err != nil {
				break
			}
			ss, err := f.GetFullSamples(trexByTrack[ccVideoTrack])
			c.Check(err == nil, "second pass samples")
			switch fi {
			case 0:
				firstVideo = ccDTSMap(ss)
			case 5:
				rolledBackVideo = ccDTSMap(ss)
			case 6:
				rawVideoFirst = ccDTSMap(ss)
			case 8: // first non-empty fragment after the empty tfdt re-anchors
				reanchoredVideo = ccDTSMap(ss)
				audio, err := f.GetFullSamples(trexByTrack[ccAudioTrack])
				c.Check(err == nil, "re-anchored audio samples")
				reanchoredAudio = ccDTSMap(audio)
			}
		}
		c.Check(ccU64Equal(firstVideo, []uint64{1000, 1010, 1030}), "first video DTS anchor")
		c.Check(ccU64Equal(rolledBackVideo, []uint64{500}), "backwards re-anchor")
		c.Check(ccU64Equal(rawVideoFirst, []uint64{510, 520}), "raw no-tfdt fragment continues the clock")
		c.Check(ccU64Equal(reanchoredVideo, []uint64{300}), "empty tfdt traf re-anchors the video clock")
		c.Check(ccU64Equal(reanchoredAudio, []uint64{7000}), "empty tfdt traf re-anchors the audio clock")

		// The negative composition offset of the very first sample is preserved and shifts presentation time.
		d3 := &mp4.FragmentDecoder{Moov: init.Moov, ContinuousClock: true}
		f0, err := d3.DecodeSR(bits.NewFixedSliceReader(fragments[0]), 0)
		c.Check(err == nil, "decode first fragment")
		s0, err := f0.GetFullSamples(trexByTrack[ccVideoTrack])
		c.Check(err == nil && len(s0) >= 1, "first sample available")
		if len(s0) >= 1 {
			c.Check(s0[0].CompositionTimeOffset == -1, "negative composition offset")
			c.Check(s0[0].PresentationTime() == int64(s0[0].DecodeTime)-1, "presentation time")
		}

		// Default mode stays unchanged: a traf without tfdt gives decode times relative to zero.
		def := &mp4.FragmentDecoder{Moov: init.Moov}
		f2, err := def.DecodeSR(bits.NewFixedSliceReader(fragments[1]), 0)
		c.Check(err == nil, "default-mode decode")
		defSamples, err := f2.GetFullSamples(trexByTrack[ccVideoTrack])
		c.Check(err == nil, "default-mode samples")
		c.Check(ccU64Equal(ccDTSMap(defSamples), []uint64{0, 10}), "default mode starts no-tfdt traf at zero")

		// A non-empty trun without any duration source is an error in continuous mode.
		noDur := ccBuildFragment(t, ccFragSpec{seq: 9, trafs: []ccTrafSpec{
			{trackID: ccVideoTrack, truns: []ccTrunSpec{
				{durs: nil, sizes: []uint32{1}, ctos: []int32{0}, noDurFlag: true},
			}},
		}})
		bad := &mp4.FragmentDecoder{ContinuousClock: true}
		_, err = bad.DecodeSR(bits.NewFixedSliceReader(noDur), 0)
		c.Check(err != nil, "trun without duration source must error")
	})
}

// TestContinuousClockStream drives StreamFile with WithContinuousClock through the same fixture, exercises
// every SampleAccessor method, the sliding window, a callback that ignores one track, no-read callbacks,
// callback and reader failures, and the default-mode control.
func TestContinuousClockStream(t *testing.T) {
	specs := ccFixture()
	fragments := ccBuildStream(t, specs)
	want := ccExpectedDTS(specs)
	_, initBytes := ccInit(t, true)

	var media bytes.Buffer
	media.Write(initBytes)
	for _, f := range fragments {
		media.Write(f)
	}
	all := media.Bytes()

	taskcheck.Run(t, func(c *taskcheck.C) {
		// Per-track DTS collected through the different accessor methods, plus payload and offset spot checks.
		got := make([]map[uint32][]uint64, len(specs))
		var dst []mp4.FullSample
		var buf []byte
		sf, err := mp4.InitDecodeStream(bytes.NewReader(all),
			mp4.WithContinuousClock(),
			mp4.WithMaxFragments(1), // sliding window keeps only the latest fragment
			mp4.WithFragmentCallback(func(f *mp4.Fragment, sa mp4.SampleAccessor) error {
				fi := int(f.Moof.Mfhd.SequenceNumber) - 1
				got[fi] = map[uint32][]uint64{}
				for ti, traf := range f.Moof.Trafs {
					trackID := traf.Tfhd.TrackID
					switch {
					case fi == 2: // read only the video track on this fragment
						if trackID != ccVideoTrack {
							continue
						}
						one, err := sa.GetSample(trackID, 1)
						if err != nil {
							return err
						}
						got[fi][trackID] = []uint64{one.DecodeTime}
					case fi == 3: // range and append-range access
						rng, err := sa.GetSampleRange(trackID, 1, 1)
						if err != nil {
							return err
						}
						for _, s := range rng {
							got[fi][trackID] = append(got[fi][trackID], s.DecodeTime)
						}
						ap, _, err := sa.AppendSampleRange(dst[:0], buf[:0], trackID, 1, math.MaxUint32)
						if err != nil {
							return err
						}
						for _, s := range ap {
							got[fi][trackID] = append(got[fi][trackID], s.DecodeTime)
						}
					case fi == 4: // no accessor calls at all on this empty fragment
					case fi%2 == 0:
						all, err := sa.GetSamples(trackID)
						if err != nil {
							return err
						}
						for _, s := range all {
							got[fi][trackID] = append(got[fi][trackID], s.DecodeTime)
						}
					default:
						ap, b, err := sa.AppendSamples(dst[:0], buf[:0], trackID)
						buf = b
						if err != nil {
							return err
						}
						for _, s := range ap {
							got[fi][trackID] = append(got[fi][trackID], s.DecodeTime)
						}
					}
					// Repeated access on the first fragment's first track returns identical values.
					if fi == 0 && trackID == ccVideoTrack {
						again, err := sa.GetSamples(trackID)
						if err != nil {
							return err
						}
						c.Check(ccU64Equal(ccDTSMap(again), want[0][ccVideoTrack]), "stream repeated read DTS")
						c.Check(again[0].CompositionTimeOffset == -1, "stream negative composition offset")
						wantData := ccSampleData(int(specs[fi].seq), int(trackID), 0, 0,
							int(specs[fi].trafs[ti].truns[0].sizes[0]))
						c.Check(bytes.Equal(again[0].Data, wantData), "stream payload bytes")
					}
				}
				return nil
			}),
		)
		c.Check(err == nil, "%s", fmt.Sprintf("InitDecodeStream: %v", err))
		c.Check(sf.ProcessFragments() == nil, "ProcessFragments")

		for fi := range specs {
			for _, ts := range specs[fi].trafs {
				nrSamples := 0
				for _, r := range ts.truns {
					nrSamples += len(r.sizes)
				}
				if nrSamples == 0 {
					continue // empty fragment: nothing was read in the callback
				}
				gotDTS, ok := got[fi][ts.trackID]
				if fi == 2 && ts.trackID == ccAudioTrack {
					c.Check(!ok, "audio track was not read on fragment 3")
					continue // its clock still ticked, checked on the next audio fragment
				}
				wantDTS := want[fi][ts.trackID]
				switch {
				case fi == 2 && ts.trackID == ccVideoTrack:
					wantDTS = wantDTS[:1] // only GetSample(1) was called
				case fi == 3:
					// GetSampleRange(1,1) gives the first sample, then AppendSampleRange over the whole trun.
					wantDTS = append([]uint64{wantDTS[0]}, wantDTS...)
				}
				c.Check(ok, "%s", fmt.Sprintf("fragment %d track %d was read", fi+1, ts.trackID))
				c.Check(ccU64Equal(gotDTS, wantDTS), "%s",
					fmt.Sprintf("stream fragment %d track %d DTS %v, want %v", fi+1, ts.trackID, gotDTS, wantDTS))
			}
		}
		// The audio track skipped on fragment 3 keeps ticking: fragment 3 adds two trex durations (1536 ticks),
		// so on fragment 4 its first sample continues from 9536 + 1536 = 11072, even though no accessor call
		// touched the track on fragment 3. The range read returns the first sample, then the full-range read
		// returns both; the first trun duration is an explicit zero, so both samples share the decode time.
		c.Check(ccU64Equal(got[3][ccAudioTrack], []uint64{11072, 11072, 11072}), "%s",
			fmt.Sprintf("skipped audio track keeps ticking: got %v", got[3][ccAudioTrack]))
		// The sliding window retained at most one fragment, but the clocks survived in the StreamFile.
		active := sf.GetActiveFragments()
		c.Check(len(active) <= 1, "%s", fmt.Sprintf("sliding window retained %d fragments", len(active)))

		// A callback error ends this run; the fragment was already committed, so its DTS values are correct.
		var failedAt []uint64
		sfErr, err := mp4.InitDecodeStream(bytes.NewReader(all), mp4.WithContinuousClock(),
			mp4.WithFragmentCallback(func(f *mp4.Fragment, sa mp4.SampleAccessor) error {
				trackID := f.Moof.Trafs[0].Tfhd.TrackID
				s, err := sa.GetSample(trackID, 1)
				if err != nil {
					return err
				}
				failedAt = append(failedAt, s.DecodeTime)
				if f.Moof.Mfhd.SequenceNumber == 3 {
					return errors.New("callback stop")
				}
				return nil
			}))
		c.Check(err == nil, "error-stream init")
		c.Check(sfErr.ProcessFragments() != nil, "callback error stops ProcessFragments")
		c.Check(ccU64Equal(failedAt, []uint64{1000, 1060, 1085}), "committed DTS up to the callback failure")

		// A truncated media stream ends with an error, not a panic, and no requirement to resume the reader.
		truncated := make([]byte, len(initBytes)+len(fragments[0])+len(fragments[1])/2)
		copy(truncated, all[:len(truncated)])
		sfTrunc, err := mp4.InitDecodeStream(bytes.NewReader(truncated), mp4.WithContinuousClock())
		c.Check(err == nil, "truncated init")
		c.Check(sfTrunc.ProcessFragments() != nil, "truncated media returns an error")

		// Default mode without the option keeps the old behavior on a no-tfdt fragment.
		var defaultDTS []uint64
		sfDef, err := mp4.InitDecodeStream(bytes.NewReader(all),
			mp4.WithFragmentCallback(func(f *mp4.Fragment, sa mp4.SampleAccessor) error {
				if f.Moof.Mfhd.SequenceNumber != 2 {
					return nil
				}
				s, err := sa.GetSample(ccVideoTrack, 1)
				if err != nil {
					return err
				}
				defaultDTS = append(defaultDTS, s.DecodeTime)
				return nil
			}))
		c.Check(err == nil, "default stream init")
		c.Check(sfDef.ProcessFragments() == nil, "default stream process")
		c.Check(ccU64Equal(defaultDTS, []uint64{0}), "default stream gives zero-based DTS without tfdt")

		// The clocks advance for every track even when the callback never reads a sample. The callback makes no
		// accessor calls at all until the last fragment, where both tracks are read: their decode times then
		// continue across the unread fragments instead of restarting from the tfdt or zero.
		var lastDTS map[uint32][]uint64
		sfNoCb, err := mp4.InitDecodeStream(bytes.NewReader(all), mp4.WithContinuousClock(),
			mp4.WithFragmentCallback(func(f *mp4.Fragment, sa mp4.SampleAccessor) error {
				if f.Moof.Mfhd.SequenceNumber != uint32(len(specs)) {
					return nil // deliberately read nothing
				}
				lastDTS = map[uint32][]uint64{}
				for _, trackID := range []uint32{ccVideoTrack, ccAudioTrack} {
					ss, err := sa.GetSamples(trackID)
					if err != nil {
						return err
					}
					lastDTS[trackID] = ccDTSMap(ss)
				}
				return nil
			}))
		c.Check(err == nil, "no-read init")
		c.Check(sfNoCb.ProcessFragments() == nil, "no-read process")
		for _, trackID := range []uint32{ccVideoTrack, ccAudioTrack} {
			c.Check(ccU64Equal(lastDTS[trackID], want[len(want)-1][trackID]),
				"unread fragments still advanced the clock before the last fragment")
		}
	})
}

// TestContinuousClockRollback checks that only fully validated fragments commit the clocks: truncated fragments,
// later-track overflow, io.EOF and explicit resets all leave the clocks at the last successful position, while
// the reused decoder objects never mix tracks. It also rejects two trafs of the same track in one moof.
func TestContinuousClockRollback(t *testing.T) {
	specs := ccFixture()
	fragments := ccBuildStream(t, specs)
	want := ccExpectedDTS(specs)
	init, _ := ccInit(t, false)

	taskcheck.Run(t, func(c *taskcheck.C) {
		d := &mp4.FragmentDecoder{Moov: init.Moov, ContinuousClock: true}

		// Decode the first fragment successfully.
		f0, err := d.DecodeSR(bits.NewFixedSliceReader(fragments[0]), 0)
		c.Check(err == nil, "first fragment")
		s0, err := f0.GetFullSamples(init.Moov.Mvex.Trexs[0])
		c.Check(err == nil && ccU64Equal(ccDTSMap(s0), want[0][ccVideoTrack]), "first fragment DTS")

		// A truncated second fragment fails and must not move any clock.
		cut := fragments[1][:len(fragments[1])/2]
		_, err = d.DecodeSR(bits.NewFixedSliceReader(cut), 0)
		c.Check(errors.Is(err, io.ErrUnexpectedEOF), "%s", fmt.Sprintf("truncated fragment: %v", err))

		// Feeding the same decoder the complete fragment afterwards continues from the last success.
		f1, err := d.DecodeSR(bits.NewFixedSliceReader(fragments[1]), 0)
		c.Check(err == nil, "retry complete fragment")
		s1, err := f1.GetFullSamples(init.Moov.Mvex.Trexs[0])
		c.Check(err == nil && ccU64Equal(ccDTSMap(s1), want[1][ccVideoTrack]), "%s",
			fmt.Sprintf("retry DTS %v want %v", ccDTSMap(s1), want[1][ccVideoTrack]))

		// io.EOF on an empty reader leaves the clocks untouched too.
		_, err = d.DecodeSR(bits.NewFixedSliceReader(nil), 0)
		c.Check(errors.Is(err, io.EOF), "empty reader gives EOF")
		f2, err := d.DecodeSR(bits.NewFixedSliceReader(fragments[2]), 0)
		c.Check(err == nil, "fragment after EOF")
		s2, err := f2.GetFullSamples(init.Moov.Mvex.Trexs[0])
		c.Check(err == nil && ccU64Equal(ccDTSMap(s2), want[2][ccVideoTrack]), "%s",
			fmt.Sprintf("after-EOF DTS %v want %v", ccDTSMap(s2), want[2][ccVideoTrack]))

		// Later-track overflow: the video traf is fine, the audio traf re-anchors near MaxUint64 and overflows.
		overflow := ccBuildOverflowFragment(t)
		_, err = d.DecodeSR(bits.NewFixedSliceReader(overflow), 0)
		c.Check(err != nil, "overflow fragment rejected")
		// The video clock did not commit: the next video-only no-tfdt fragment starts where fragment 3 ended.
		fAfter, err := d.DecodeSR(bits.NewFixedSliceReader(fragments[3]), 0)
		c.Check(err == nil, "fragment after overflow")
		sAfter, err := fAfter.GetFullSamples(init.Moov.Mvex.Trexs[0])
		c.Check(err == nil && ccU64Equal(ccDTSMap(sAfter), want[3][ccVideoTrack]), "%s",
			fmt.Sprintf("post-overflow video DTS %v want %v", ccDTSMap(sAfter), want[3][ccVideoTrack]))

		// Two trafs with the same track ID in one moof are rejected, while default mode accepts the bytes.
		dup := ccBuildDuplicateTrackFragment(t)
		dupDec := &mp4.FragmentDecoder{Moov: init.Moov, ContinuousClock: true}
		_, err = dupDec.DecodeSR(bits.NewFixedSliceReader(dup), 0)
		c.Check(err != nil, "duplicate-track moof rejected")
		defDec := &mp4.FragmentDecoder{Moov: init.Moov}
		fDef, err := defDec.DecodeSR(bits.NewFixedSliceReader(dup), 0)
		c.Check(err == nil, "default mode accepts duplicate-track bytes")
		if err == nil {
			ss, err := fDef.GetFullSamples(init.Moov.Mvex.Trexs[0])
			c.Check(err == nil && len(ss) == 1, "default mode reads the first traf")
		}

		// Explicit reset: the next no-tfdt video fragment starts at zero again.
		d.ResetContinuousClocks()
		fReset, err := d.DecodeSR(bits.NewFixedSliceReader(fragments[1]), 0)
		c.Check(err == nil, "fragment after reset")
		sReset, err := fReset.GetFullSamples(init.Moov.Mvex.Trexs[0])
		c.Check(err == nil && ccU64Equal(ccDTSMap(sReset), []uint64{0, 10}), "reset restarts the clock at zero")

		// A fresh decoder on the same object-reuse pattern keeps tracks independent: the audio-only fragment
		// cannot revive an old video base.
		d2 := &mp4.FragmentDecoder{Moov: init.Moov, ContinuousClock: true}
		audioOnly := ccBuildFragment(t, ccFragSpec{seq: 10, trafs: []ccTrafSpec{
			{trackID: ccAudioTrack, tfdt: ccU64(9000), truns: []ccTrunSpec{
				{durs: []uint32{512}, sizes: []uint32{4}, ctos: []int32{0}},
			}},
		}})
		fA, err := d2.DecodeSR(bits.NewFixedSliceReader(audioOnly), 0)
		c.Check(err == nil, "audio-only fragment")
		missing, err := fA.GetFullSamples(init.Moov.Mvex.Trexs[0])
		c.Check(err == nil && len(missing) == 0, "video absent from audio-only fragment")
		fV, err := d2.DecodeSR(bits.NewFixedSliceReader(fragments[1]), 0)
		c.Check(err == nil, "video fragment after audio-only")
		sV, err := fV.GetFullSamples(init.Moov.Mvex.Trexs[0])
		c.Check(err == nil && ccU64Equal(ccDTSMap(sV), []uint64{0, 10}),
			"video first seen later starts at zero, not at the audio base")
	})
}

// ccBuildOverflowFragment builds a fragment whose video traf is valid while its audio traf re-anchors close to
// MaxUint64 and adds a duration that overflows the uint64 clock.
func ccBuildOverflowFragment(t *testing.T) []byte {
	t.Helper()
	frag, err := mp4.CreateMultiTrackFragment(8, []uint32{ccVideoTrack, ccAudioTrack})
	if err != nil {
		t.Fatal(err)
	}
	video := mp4.Sample{Flags: mp4.SyncSampleFlags, Dur: 10, Size: 1}
	if err := frag.AddFullSampleToTrack(mp4.FullSample{Sample: video, DecodeTime: 0,
		Data: []byte{0x01}}, ccVideoTrack); err != nil {
		t.Fatal(err)
	}
	audio := mp4.Sample{Flags: mp4.SyncSampleFlags, Dur: 100, Size: 1}
	if err := frag.AddFullSampleToTrack(mp4.FullSample{Sample: audio, DecodeTime: math.MaxUint64 - 10,
		Data: []byte{0x02}}, ccAudioTrack); err != nil {
		t.Fatal(err)
	}
	// AddFullSampleToTrack writes the decode time into the tfdt, which is the near-MaxUint64 re-anchor we want.
	var buf bytes.Buffer
	if err := frag.Encode(&buf); err != nil {
		t.Fatal(err)
	}
	return buf.Bytes()
}

// ccBuildDuplicateTrackFragment builds valid bytes with two trafs carrying the same track ID.
func ccBuildDuplicateTrackFragment(t *testing.T) []byte {
	t.Helper()
	frag, err := mp4.CreateMultiTrackFragment(11, []uint32{ccVideoTrack, ccVideoTrack})
	if err != nil {
		t.Fatal(err)
	}
	for i, traf := range frag.Moof.Trafs {
		trun := mp4.CreateTrun(uint32(i))
		if err := traf.AddChild(trun); err != nil {
			t.Fatal(err)
		}
		trun.AddSample(mp4.Sample{Flags: mp4.SyncSampleFlags, Dur: 5, Size: 1})
		frag.Mdat.AddSampleData([]byte{byte(0x10 + i)})
	}
	var buf bytes.Buffer
	if err := frag.Encode(&buf); err != nil {
		t.Fatal(err)
	}
	return buf.Bytes()
}
