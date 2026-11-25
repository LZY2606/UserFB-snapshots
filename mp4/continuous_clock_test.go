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

// The continuous-clock tests work on small two-track fragments with several truns, built with the library's
// encoder, written to bytes and parsed back, so that every decoded box has been through a real encode/decode
// cycle. One fragment header without tfdt is instead assembled byte by byte below, so that the path is not only
// exercised on the library's own output.

const (
	videoTrackID uint32 = 1
	audioTrackID uint32 = 2
	thirdTrackID uint32 = 3
)

// runSpec describes one trun of a test traf.
type runSpec struct {
	durs    []uint32 // nil means that the trun carries no per-sample duration field
	sizes   []uint32
	ctos    []int32 // nil means that the trun carries no composition offset field
	payload [][]byte
}

// trafSpec describes one traf of a test fragment.
type trafSpec struct {
	trackID        uint32
	tfdt           *uint64 // nil means that the traf has no tfdt box
	tfhdDefaultDur *uint32 // nil means that the tfhd carries no default duration; a pointer to zero sets it
	runs           []runSpec
}

// buildFragment assembles a fragment from traf specs. The truns get globally increasing write-order numbers in
// payload order, so that Fragment.SetTrunDataOffsets lays them out in the mdat in that order.
func buildFragment(t *testing.T, seqNr uint32, trafs []trafSpec) *mp4.Fragment {
	t.Helper()
	frag := mp4.NewFragment()
	moof := &mp4.MoofBox{}
	frag.AddChild(moof)
	if err := moof.AddChild(mp4.CreateMfhd(seqNr)); err != nil {
		t.Fatal(err)
	}
	var payload []byte
	order := uint32(1)
	for _, ts := range trafs {
		traf := &mp4.TrafBox{}
		tfhd := mp4.CreateTfhd(ts.trackID)
		if ts.tfhdDefaultDur != nil {
			tfhd.Flags |= mp4.TfhdDefaultSampleDurationPresentFlag
			tfhd.DefaultSampleDuration = *ts.tfhdDefaultDur
		}
		if err := traf.AddChild(tfhd); err != nil {
			t.Fatal(err)
		}
		if ts.tfdt != nil {
			if err := traf.AddChild(mp4.CreateTfdt(*ts.tfdt)); err != nil {
				t.Fatal(err)
			}
		}
		for _, rs := range ts.runs {
			trun := mp4.CreateTrun(order)
			order++
			if rs.durs == nil {
				trun.Flags &^= mp4.TrunSampleDurationPresentFlag
			}
			if rs.ctos == nil {
				trun.Flags &^= mp4.TrunSampleCompositionTimeOffsetPresentFlag
			}
			for i := range rs.sizes {
				if uint32(len(rs.payload[i])) != rs.sizes[i] {
					t.Fatalf("run sample %d: size %d does not match payload of %d bytes", i, rs.sizes[i], len(rs.payload[i]))
				}
				s := mp4.Sample{Flags: mp4.SyncSampleFlags, Size: rs.sizes[i]}
				if rs.durs != nil {
					s.Dur = rs.durs[i]
				}
				if rs.ctos != nil {
					s.CompositionTimeOffset = rs.ctos[i]
				}
				trun.AddSample(s)
				payload = append(payload, rs.payload[i]...)
			}
			if err := traf.AddChild(trun); err != nil {
				t.Fatal(err)
			}
		}
		if err := moof.AddChild(traf); err != nil {
			t.Fatal(err)
		}
	}
	mdat := &mp4.MdatBox{}
	mdat.SetData(payload)
	frag.AddChild(mdat)
	frag.SetTrunDataOffsets()
	return frag
}

// encodeClockFragment writes frag to bytes, the form that the decoders and the stream reader consume.
func encodeClockFragment(t *testing.T, frag *mp4.Fragment) []byte {
	t.Helper()
	var buf bytes.Buffer
	if err := frag.Encode(&buf); err != nil {
		t.Fatal(err)
	}
	return buf.Bytes()
}

// u64 returns a pointer to v, for the optional tfdt values of trafSpec.
func u64(v uint64) *uint64 { return &v }

// u32ptr returns a pointer to v, for the optional tfhd default duration of trafSpec.
func u32ptr(v uint32) *uint32 { return &v }

// clockFixture builds the shared two-track, multi-run fragment sequence and the expected decode times.
//
// Video (track 1): F1 anchored at 1000 with an explicit two-sample run and a tfhd-default run, F2 without tfdt,
// F3 falling back to the trex duration, F4 empty, F5 re-anchored backwards. Audio (track 2): F1 anchored at
// 500 with a negative composition offset, absent from F2, explicit zero duration in F3, empty in F4,
// re-anchored forwards in F5.
func clockFixture(t *testing.T) (fragments [][]byte, want map[int]map[uint32][]mp4.FullSample) {
	t.Helper()
	f1 := buildFragment(t, 1, []trafSpec{
		{
			trackID: videoTrackID, tfdt: u64(1000), tfhdDefaultDur: u32ptr(300),
			runs: []runSpec{
				{durs: []uint32{100, 200}, sizes: []uint32{3, 4}, ctos: nil,
					payload: [][]byte{[]byte("V1a"), []byte("V1b.")}},
				{durs: nil, sizes: []uint32{5}, ctos: nil, payload: [][]byte{[]byte("V1c..")}},
			},
		},
		{
			trackID: audioTrackID, tfdt: u64(500),
			runs: []runSpec{
				{durs: []uint32{70, 80}, sizes: []uint32{2, 3}, ctos: []int32{-10, 5},
					payload: [][]byte{[]byte("A1"), []byte("A1b")}},
			},
		},
	})
	f3 := buildFragment(t, 3, []trafSpec{
		{
			trackID: videoTrackID, // no tfdt, trun without duration or tfhd default: trex fallback
			runs:    []runSpec{{durs: nil, sizes: []uint32{5}, ctos: nil, payload: [][]byte{[]byte("V3...")}}},
		},
		{
			trackID: audioTrackID, // explicit zero duration is valid
			runs:    []runSpec{{durs: []uint32{0}, sizes: []uint32{2}, ctos: nil, payload: [][]byte{[]byte("A3")}}},
		},
	})
	f4 := buildFragment(t, 4, []trafSpec{
		{trackID: videoTrackID}, // empty traf, no tfdt
		{trackID: audioTrackID},
	})
	f5 := buildFragment(t, 5, []trafSpec{
		{
			trackID: videoTrackID, tfdt: u64(4000), // backwards re-anchor from 4675
			runs: []runSpec{{durs: []uint32{100}, sizes: []uint32{3}, ctos: nil, payload: [][]byte{[]byte("V5.")}}},
		},
		{
			trackID: audioTrackID, tfdt: u64(900), // forwards re-anchor from 650
			runs: []runSpec{{durs: []uint32{10}, sizes: []uint32{2}, ctos: nil, payload: [][]byte{[]byte("A5")}}},
		},
	})
	f6 := buildFragment(t, 6, []trafSpec{
		{
			trackID: videoTrackID, // no tfdt: continues after the re-anchor
			runs:    []runSpec{{durs: []uint32{100}, sizes: []uint32{2}, ctos: nil, payload: [][]byte{[]byte("V6")}}},
		},
		{
			trackID: audioTrackID,
			runs:    []runSpec{{durs: []uint32{20}, sizes: []uint32{2}, ctos: nil, payload: [][]byte{[]byte("A6")}}},
		},
	})
	f2 := handBuiltNoTfdtFragment(t)
	fragments = [][]byte{
		encodeClockFragment(t, f1), f2, encodeClockFragment(t, f3),
		encodeClockFragment(t, f4), encodeClockFragment(t, f5), encodeClockFragment(t, f6),
	}
	want = map[int]map[uint32][]mp4.FullSample{
		1: {
			videoTrackID: {
				{Sample: mp4.Sample{Flags: mp4.SyncSampleFlags, Dur: 100, Size: 3}, DecodeTime: 1000, Data: []byte("V1a")},
				{Sample: mp4.Sample{Flags: mp4.SyncSampleFlags, Dur: 200, Size: 4}, DecodeTime: 1100, Data: []byte("V1b.")},
				{Sample: mp4.Sample{Flags: mp4.SyncSampleFlags, Dur: 300, Size: 5}, DecodeTime: 1300, Data: []byte("V1c..")},
			},
			audioTrackID: {
				{Sample: mp4.Sample{Flags: mp4.SyncSampleFlags, Dur: 70, Size: 2, CompositionTimeOffset: -10},
					DecodeTime: 500, Data: []byte("A1")},
				{Sample: mp4.Sample{Flags: mp4.SyncSampleFlags, Dur: 80, Size: 3, CompositionTimeOffset: 5},
					DecodeTime: 570, Data: []byte("A1b")},
			},
		},
		2: {
			videoTrackID: {
				{Sample: mp4.Sample{Dur: 50, Size: 3}, DecodeTime: 1600, Data: []byte("V2a")},
				{Sample: mp4.Sample{Dur: 25, Size: 3}, DecodeTime: 1650, Data: []byte("V2b")},
			},
		},
		3: {
			videoTrackID: {
				{Sample: mp4.Sample{Flags: mp4.SyncSampleFlags, Dur: 3000, Size: 5}, DecodeTime: 1675, Data: []byte("V3...")},
			},
			audioTrackID: {
				{Sample: mp4.Sample{Flags: mp4.SyncSampleFlags, Dur: 0, Size: 2}, DecodeTime: 650, Data: []byte("A3")},
			},
		},
		5: {
			videoTrackID: {
				{Sample: mp4.Sample{Flags: mp4.SyncSampleFlags, Dur: 100, Size: 3}, DecodeTime: 4000, Data: []byte("V5.")},
			},
			audioTrackID: {
				{Sample: mp4.Sample{Flags: mp4.SyncSampleFlags, Dur: 10, Size: 2}, DecodeTime: 900, Data: []byte("A5")},
			},
		},
		6: {
			videoTrackID: {
				{Sample: mp4.Sample{Flags: mp4.SyncSampleFlags, Dur: 100, Size: 2}, DecodeTime: 4100, Data: []byte("V6")},
			},
			audioTrackID: {
				{Sample: mp4.Sample{Flags: mp4.SyncSampleFlags, Dur: 20, Size: 2}, DecodeTime: 910, Data: []byte("A6")},
			},
		},
	}
	return fragments, want
}

// box writes a box of type typ with body into buf.
func box(buf *bytes.Buffer, typ string, body []byte) {
	hdr := make([]byte, 8)
	binary.BigEndian.PutUint32(hdr, uint32(8+len(body)))
	copy(hdr[4:], typ)
	buf.Write(hdr)
	buf.Write(body)
}

// handBuiltNoTfdtFragment independently assembles a one-track fragment whose traf has no tfdt box, without using
// the mp4ff encoder. It is sequence number 2 and carries two samples, matching the second fragment of the fixture.
func handBuiltNoTfdtFragment(t *testing.T) []byte {
	t.Helper()
	var trun bytes.Buffer
	// version 0, flags: data offset (0x001), sample duration (0x100) and sample size (0x200) present.
	binary.Write(&trun, binary.BigEndian, uint32(0x00000301))
	binary.Write(&trun, binary.BigEndian, uint32(2)) // sample count
	trun.Write(make([]byte, 4))                      // data offset, patched once the moof size is known
	binary.Write(&trun, binary.BigEndian, uint32(50))
	binary.Write(&trun, binary.BigEndian, uint32(3))
	binary.Write(&trun, binary.BigEndian, uint32(25))
	binary.Write(&trun, binary.BigEndian, uint32(3))
	trunBytes := append([]byte(nil), trun.Bytes()...)

	var tfhd bytes.Buffer
	binary.Write(&tfhd, binary.BigEndian, uint32(0x00020000)) // default-base-is-moof
	binary.Write(&tfhd, binary.BigEndian, videoTrackID)

	var traf bytes.Buffer
	box(&traf, "tfhd", tfhd.Bytes())
	box(&traf, "trun", trunBytes)

	var mfhd bytes.Buffer
	binary.Write(&mfhd, binary.BigEndian, uint32(0))
	binary.Write(&mfhd, binary.BigEndian, uint32(2))

	var moof bytes.Buffer
	box(&moof, "mfhd", mfhd.Bytes())
	box(&moof, "traf", traf.Bytes())
	moofSize := uint32(8 + moof.Len())

	// With default-base-is-moof, the absolute offset is the moof start plus the trun data offset.
	binary.BigEndian.PutUint32(trunBytes[8:], moofSize+8) // mdat header is 8 bytes, then the payload starts
	traf.Reset()
	box(&traf, "tfhd", tfhd.Bytes())
	box(&traf, "trun", trunBytes)
	moof.Reset()
	box(&moof, "mfhd", mfhd.Bytes())
	box(&moof, "traf", traf.Bytes())

	var frag bytes.Buffer
	box(&frag, "moof", moof.Bytes())
	box(&frag, "mdat", []byte("V2aV2bX"))
	return frag.Bytes()
}

// continuousInit returns an encoded two-track init segment whose trex boxes provide fallback durations and sizes.
func continuousInit(t *testing.T) []byte {
	t.Helper()
	init := mp4.CreateEmptyInit()
	init.AddEmptyTrack(90000, "video", "und")
	init.AddEmptyTrack(48000, "audio", "und")
	defaults := map[uint32][2]uint32{
		videoTrackID: {3000, 5},
		audioTrackID: {1024, 2},
	}
	for _, trex := range init.Moov.Mvex.Trexs {
		d := defaults[trex.TrackID]
		trex.DefaultSampleDuration = d[0]
		trex.DefaultSampleSize = d[1]
	}
	var buf bytes.Buffer
	if err := init.Encode(&buf); err != nil {
		t.Fatal(err)
	}
	return buf.Bytes()
}

// trexFor returns the trex box of trackID of the encoded init segment.
func trexFor(t *testing.T, initData []byte, trackID uint32) *mp4.TrexBox {
	t.Helper()
	f, err := mp4.DecodeFile(bytes.NewReader(initData))
	if err != nil {
		t.Fatal(err)
	}
	trex, ok := f.Init.Moov.Mvex.GetTrex(trackID)
	if !ok {
		t.Fatalf("no trex for track %d", trackID)
	}
	return trex
}

// checkSamples compares every field of got and want, including the payload bytes and the composition offset.
func checkSamples(c *taskcheck.C, name string, got, want []mp4.FullSample) {
	c.Check(len(got) == len(want), "%s: got %d samples, want %d", name, len(got), len(want))
	for i := 0; i < len(got) && i < len(want); i++ {
		c.Check(got[i].Sample == want[i].Sample, "%s sample %d: got %+v, want %+v", name, i+1, got[i].Sample, want[i].Sample)
		c.Check(got[i].DecodeTime == want[i].DecodeTime, "%s sample %d: got DTS %d, want %d",
			name, i+1, got[i].DecodeTime, want[i].DecodeTime)
		c.Check(bytes.Equal(got[i].Data, want[i].Data), "%s sample %d: payload got %q, want %q",
			name, i+1, got[i].Data, want[i].Data)
	}
}

// decodeOne decodes the next fragment from data with d and returns it.
func decodeOne(t *testing.T, d *mp4.FragmentDecoder, data []byte, pos uint64) *mp4.Fragment {
	t.Helper()
	f, err := d.DecodeSR(bits.NewFixedSliceReader(data), pos)
	if err != nil {
		t.Fatalf("decode at %d: %v", pos, err)
	}
	return f
}

// TestContinuousClockDecoder drives one FragmentDecoder in continuous-clock mode through the fixture and checks
// every sample-facing API, the default-mode control, empty and absent tracks, defaults and byte preservation.
func TestContinuousClockDecoder(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		fragments, want := clockFixture(t)
		initData := continuousInit(t)
		trex1, trex2 := trexFor(t, initData, videoTrackID), trexFor(t, initData, audioTrackID)
		trexs := map[uint32]*mp4.TrexBox{videoTrackID: trex1, audioTrackID: trex2}

		d := &mp4.FragmentDecoder{Moov: decodeInitMoov(t, initData), ContinuousClock: true}
		var pos uint64
		for idx, data := range fragments {
			frag := decodeOne(t, d, data, pos)
			pos += uint64(len(data))
			fragNr := idx + 1

			// The hand-built fragment really has no tfdt box.
			if fragNr == 2 {
				c.Check(frag.Moof.Traf.Tfdt == nil, "fragment 2 traf has a tfdt box")
			}
			for trackID, wantSamples := range want[fragNr] {
				got, err := frag.GetFullSamples(trexs[trackID])
				c.Check(err == nil, "fragment %d track %d GetFullSamples: %v", fragNr, trackID, err)
				checkSamples(c, fmt.Sprintf("F%d/t%d GetFullSamples", fragNr, trackID), got, wantSamples)

				// AppendFullSamples with caller-provided storage gives the same values.
				var appended []mp4.FullSample
				appended, err = frag.AppendFullSamples(appended, trexs[trackID])
				c.Check(err == nil, "fragment %d track %d AppendFullSamples: %v", fragNr, trackID, err)
				checkSamples(c, fmt.Sprintf("F%d/t%d AppendFullSamples", fragNr, trackID), appended, wantSamples)

				// The Samples iterator gives the same values, including DecodeTime and payload.
				var iterSamples []mp4.FullSample
				for s, err := range frag.Samples(trexs[trackID]) {
					c.Check(err == nil, "fragment %d track %d Samples: %v", fragNr, trackID, err)
					iterSamples = append(iterSamples, s)
				}
				checkSamples(c, fmt.Sprintf("F%d/t%d Samples", fragNr, trackID), iterSamples, wantSamples)

				// Reading the same fragment again must not move the clock a second time.
				again, err := frag.GetFullSamples(trexs[trackID])
				c.Check(err == nil, "fragment %d track %d re-read: %v", fragNr, trackID, err)
				checkSamples(c, fmt.Sprintf("F%d/t%d re-read", fragNr, trackID), again, wantSamples)
			}
			// Empty fragment 4 has no samples on either track.
			if fragNr == 4 {
				for _, trackID := range []uint32{videoTrackID, audioTrackID} {
					got, err := frag.GetFullSamples(trexs[trackID])
					c.Check(err == nil && len(got) == 0, "fragment 4 track %d: got %d samples, error %v",
						trackID, len(got), err)
				}
			}
			// Re-encoding leaves every time field and media byte untouched.
			c.Check(bytes.Equal(encodeClockFragment(t, frag), data),
				"fragment %d re-encoded bytes differ from the input", fragNr)
		}

		// The negative composition offset of the first audio sample is preserved.
		f1 := decodeOne(t, d, fragments[0], 0)
		audio, err := f1.GetFullSamples(trex2)
		c.Check(err == nil, "audio F1 after loop: %v", err)
		if len(audio) == 2 {
			c.Check(audio[0].PresentationTime() == 490, "got presentation time %d, want 490", audio[0].PresentationTime())
			c.Check(audio[1].PresentationTime() == 575, "got presentation time %d, want 575", audio[1].PresentationTime())
		}

		// io.EOF does not clear a clock: an empty reader reports EOF and the next no-tfdt fragment continues the
		// clock of the last successful decode. F1 was decoded again above, so the video clock stands at 1600.
		_, err = d.DecodeSR(bits.NewFixedSliceReader(nil), pos)
		c.Check(errors.Is(err, io.EOF), "empty reader: got %v, want io.EOF", err)
		continued := decodeOne(t, d, fragments[1], pos) // the hand-built no-tfdt fragment, replayed at the end
		contSamples, err := continued.GetFullSamples(trex1)
		c.Check(err == nil, "no-tfdt fragment after EOF: %v", err)
		c.Check(len(contSamples) == 2 && contSamples[0].DecodeTime == 1600 && contSamples[1].DecodeTime == 1650,
			"clock after EOF: got DTS %d and %d, want 1600 and 1650",
			contSamples[0].DecodeTime, contSamples[1].DecodeTime)

		// A default-mode decoder keeps the old behavior: no-tfdt fragments start at zero.
		def := &mp4.FragmentDecoder{}
		defF2 := decodeOne(t, def, fragments[1], 0)
		defSamples, err := defF2.GetFullSamples(trex1)
		c.Check(err == nil, "default F2: %v", err)
		c.Check(len(defSamples) == 2 && defSamples[0].DecodeTime == 0 && defSamples[1].DecodeTime == 50,
			"default mode F2 times: got %d and %d, want 0 and 50", defSamples[0].DecodeTime, defSamples[1].DecodeTime)

		// A non-empty trun without any duration source is an error in continuous mode, but not in default mode.
		noDur := buildFragment(t, 9, []trafSpec{{
			trackID: videoTrackID,
			runs:    []runSpec{{durs: nil, sizes: []uint32{2}, ctos: nil, payload: [][]byte{[]byte("XX")}}},
		}})
		noDurData := encodeClockFragment(t, noDur)
		noClock := &mp4.FragmentDecoder{ContinuousClock: true} // no Moov, so there is no trex either
		_, err = noClock.DecodeSR(bits.NewFixedSliceReader(noDurData), 0)
		c.Check(err != nil, "continuous mode without duration source: expected an error, got nil")
		defFrag, err := (&mp4.FragmentDecoder{}).DecodeSR(bits.NewFixedSliceReader(noDurData), 0)
		c.Check(err == nil, "default mode without duration source: %v", err)
		defNoDur, err := defFrag.GetFullSamples(nil)
		c.Check(err == nil && len(defNoDur) == 1 && defNoDur[0].Dur == 0,
			"default mode missing duration: got %+v, error %v", defNoDur, err)

		// Two trafs of the same track in one moof are rejected in continuous mode, accepted in default mode.
		doubleTraf := buildFragment(t, 10, []trafSpec{
			{trackID: videoTrackID, runs: []runSpec{{durs: []uint32{10}, sizes: []uint32{1},
				payload: [][]byte{[]byte("Y")}}}},
			{trackID: videoTrackID, runs: []runSpec{{durs: []uint32{10}, sizes: []uint32{1},
				payload: [][]byte{[]byte("Z")}}}},
		})
		doubleData := encodeClockFragment(t, doubleTraf)
		_, err = (&mp4.FragmentDecoder{ContinuousClock: true}).DecodeSR(bits.NewFixedSliceReader(doubleData), 0)
		c.Check(err != nil, "two trafs of one track in a moof: expected an error, got nil")
		_, err = (&mp4.FragmentDecoder{}).DecodeSR(bits.NewFixedSliceReader(doubleData), 0)
		c.Check(err == nil, "default mode rejects a double-traf moof: %v", err)

		// A tfhd default duration of zero overrides the non-zero trex default, so the traf has zero duration.
		zeroOverride := buildFragment(t, 15, []trafSpec{
			{trackID: videoTrackID, tfhdDefaultDur: u32ptr(0), runs: []runSpec{
				{durs: nil, sizes: []uint32{1}, ctos: nil, payload: [][]byte{[]byte("0")}},
				{durs: nil, sizes: []uint32{1}, ctos: nil, payload: [][]byte{[]byte("1")}},
			}},
		})
		zeroClock := &mp4.FragmentDecoder{Moov: decodeInitMoov(t, initData), ContinuousClock: true}
		zeroFrag := decodeOne(t, zeroClock, encodeClockFragment(t, zeroOverride), 0)
		zeroSamples, err := zeroFrag.GetFullSamples(trex1)
		c.Check(err == nil, "tfhd zero override: %v", err)
		c.Check(len(zeroSamples) == 2 && zeroSamples[0].Dur == 0 && zeroSamples[1].Dur == 0,
			"tfhd zero override: got durations %d and %d, want 0 and 0",
			zeroSamples[0].Dur, zeroSamples[1].Dur)
		// The zero-duration traf leaves the clock where it was, so the next no-tfdt traf starts again at zero.
		zeroFollow := buildFragment(t, 16, []trafSpec{
			{trackID: videoTrackID, runs: []runSpec{{durs: []uint32{9}, sizes: []uint32{1},
				payload: [][]byte{[]byte("2")}}}},
		})
		zeroFollowFrag := decodeOne(t, zeroClock, encodeClockFragment(t, zeroFollow), 0)
		zeroFollowSamples, err := zeroFollowFrag.GetFullSamples(trex1)
		c.Check(err == nil, "fragment after zero-duration traf: %v", err)
		c.Check(len(zeroFollowSamples) == 1 && zeroFollowSamples[0].DecodeTime == 0,
			"clock after zero-duration traf: got DTS %d, want 0", zeroFollowSamples[0].DecodeTime)
	})
}

// decodeInitMoov decodes the moov box kept by an encoded init segment.
func decodeInitMoov(t *testing.T, initData []byte) *mp4.MoovBox {
	t.Helper()
	f, err := mp4.DecodeFile(bytes.NewReader(initData))
	if err != nil {
		t.Fatal(err)
	}
	return f.Moov
}

// TestContinuousClockStream checks the StreamFile entry point: the option, every SampleAccessor method, clocks
// that move without being read, the sliding window, read and callback failures, and the default control.
func TestContinuousClockStream(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		fragments, want := clockFixture(t)
		initData := continuousInit(t)
		media := bytes.Join(fragments, nil)

		// Full read with a sliding window that keeps only one fragment and checks every accessor method.
		sf, err := mp4.InitDecodeStream(bytes.NewReader(append(append([]byte{}, initData...), media...)),
			mp4.WithContinuousClock(), mp4.WithMaxFragments(1),
			mp4.WithFragmentCallback(func(frag *mp4.Fragment, sa mp4.SampleAccessor) error {
				seq := int(frag.Moof.Mfhd.SequenceNumber)
				for trackID, wantSamples := range want[seq] {
					all, err := sa.GetSamples(trackID)
					if err != nil {
						return err
					}
					checkSamples(c, fmt.Sprintf("stream F%d/t%d GetSamples", seq, trackID), all, wantSamples)

					first, err := sa.GetSample(trackID, 1)
					c.Check(err == nil, "F%d/t%d GetSample: %v", seq, trackID, err)
					if err == nil {
						checkSamples(c, fmt.Sprintf("stream F%d/t%d GetSample", seq, trackID),
							[]mp4.FullSample{*first}, wantSamples[:1])
					}

					end := uint32(len(wantSamples))
					rng, err := sa.GetSampleRange(trackID, 1, end)
					c.Check(err == nil, "F%d/t%d GetSampleRange: %v", seq, trackID, err)
					checkSamples(c, fmt.Sprintf("stream F%d/t%d GetSampleRange", seq, trackID), rng, wantSamples)

					var dst []mp4.FullSample
					var buf []byte
					dst, buf, err = sa.AppendSamples(dst[:0], buf[:0], trackID)
					c.Check(err == nil, "F%d/t%d AppendSamples: %v", seq, trackID, err)
					checkSamples(c, fmt.Sprintf("stream F%d/t%d AppendSamples", seq, trackID), dst, wantSamples)

					dst, buf, err = sa.AppendSampleRange(dst[:0], buf[:0], trackID, 1, end)
					c.Check(err == nil, "F%d/t%d AppendSampleRange: %v", seq, trackID, err)
					checkSamples(c, fmt.Sprintf("stream F%d/t%d AppendSampleRange", seq, trackID), dst, wantSamples)
				}
				return nil
			}))
		c.Check(err == nil, "InitDecodeStream: %v", err)
		c.Check(sf.ProcessFragments() == nil, "ProcessFragments: %v", err)
		active := sf.GetActiveFragments()
		c.Check(len(active) == 1, "sliding window keeps %d fragments, want 1", len(active))

		// A stream whose callback never reads a sample still advances every clock.
		noRead, err := mp4.InitDecodeStream(bytes.NewReader(append(append([]byte{}, initData...), media...)),
			mp4.WithContinuousClock(),
			mp4.WithFragmentCallback(func(frag *mp4.Fragment, sa mp4.SampleAccessor) error {
				return nil // deliberately touch nothing
			}))
		c.Check(err == nil, "no-read InitDecodeStream: %v", err)
		c.Check(noRead.ProcessFragments() == nil, "no-read ProcessFragments: %v", err)

		// With no callback at all, processing still succeeds and every fragment is retained.
		noCallback, err := mp4.InitDecodeStream(bytes.NewReader(append(append([]byte{}, initData...), media...)),
			mp4.WithContinuousClock(), mp4.WithMaxFragments(0))
		c.Check(err == nil, "no-callback InitDecodeStream: %v", err)
		c.Check(noCallback.ProcessFragments() == nil, "no-callback ProcessFragments: %v", err)
		retained := noCallback.GetActiveFragments()
		c.Check(len(retained) == len(fragments), "no-callback stream retained %d fragments, want %d",
			len(retained), len(fragments))

		// A callback that reads only the last fragment still sees continuous times: every earlier fragment,
		// none of whose samples was read, must have advanced both clocks.
		lastOnly, err := mp4.InitDecodeStream(bytes.NewReader(append(append([]byte{}, initData...), media...)),
			mp4.WithContinuousClock(),
			mp4.WithFragmentCallback(func(frag *mp4.Fragment, sa mp4.SampleAccessor) error {
				if frag.Moof.Mfhd.SequenceNumber != 6 {
					return nil
				}
				for trackID, wantSamples := range want[6] {
					samples, err := sa.GetSamples(trackID)
					c.Check(err == nil, "last-only F6/t%d: %v", trackID, err)
					checkSamples(c, fmt.Sprintf("last-only F6/t%d", trackID), samples, wantSamples)
				}
				return nil
			}))
		c.Check(err == nil, "last-only InitDecodeStream: %v", err)
		c.Check(lastOnly.ProcessFragments() == nil, "last-only ProcessFragments: %v", err)

		// Reading only one track still lets the other track keep time. Fragment 3 is the first fragment after
		// the audio gap, and its audio sample must continue at 650, not restart at zero.
		oneTrack, err := mp4.InitDecodeStream(bytes.NewReader(append(append([]byte{}, initData...), media...)),
			mp4.WithContinuousClock(),
			mp4.WithFragmentCallback(func(frag *mp4.Fragment, sa mp4.SampleAccessor) error {
				if _, err := sa.GetSamples(videoTrackID); err != nil {
					return err
				}
				if frag.Moof.Mfhd.SequenceNumber == 3 {
					audio, err := sa.GetSamples(audioTrackID)
					c.Check(err == nil, "F3 audio GetSamples: %v", err)
					checkSamples(c, "stream F3 audio despite no earlier reads", audio, want[3][audioTrackID])
				}
				return nil
			}))
		c.Check(err == nil, "one-track InitDecodeStream: %v", err)
		c.Check(oneTrack.ProcessFragments() == nil, "one-track ProcessFragments: %v", err)

		// The default stream keeps zero-based times for trafs without tfdt.
		def, err := mp4.InitDecodeStream(bytes.NewReader(append(append([]byte{}, initData...), media...)),
			mp4.WithFragmentCallback(func(frag *mp4.Fragment, sa mp4.SampleAccessor) error {
				if frag.Moof.Mfhd.SequenceNumber == 2 {
					s, err := sa.GetSample(videoTrackID, 1)
					c.Check(err == nil, "default F2 GetSample: %v", err)
					c.Check(s != nil && s.DecodeTime == 0, "default stream F2 DTS: got %d, want 0", s.DecodeTime)
				}
				return nil
			}))
		c.Check(err == nil, "default InitDecodeStream: %v", err)
		c.Check(def.ProcessFragments() == nil, "default ProcessFragments: %v", err)

		// A media read failure inside a fragment ends processing with an error.
		full := append(append([]byte{}, initData...), media...)
		cut := len(initData) + len(fragments[0]) + len(fragments[1])/2
		sfCut, err := mp4.InitDecodeStream(io.LimitReader(bytes.NewReader(full), int64(cut)))
		c.Check(err == nil, "cut InitDecodeStream: %v", err)
		c.Check(sfCut.ProcessFragments() != nil, "cut stream: expected an error, got nil")

		// A callback failure likewise ends processing with an error. The failed fragment is not kept and its
		// clock is not committed, so only the fragment before it remains.
		var processed uint32
		sfCb, err := mp4.InitDecodeStream(bytes.NewReader(full),
			mp4.WithContinuousClock(),
			mp4.WithFragmentCallback(func(frag *mp4.Fragment, sa mp4.SampleAccessor) error {
				processed++
				if frag.Moof.Mfhd.SequenceNumber == 2 {
					return errors.New("callback stops")
				}
				return nil
			}))
		c.Check(err == nil, "callback InitDecodeStream: %v", err)
		err = sfCb.ProcessFragments()
		c.Check(err != nil && err.Error() != "", "callback stream: expected an error, got %v", err)
		c.Check(processed == 2, "callback failure: processed %d fragments, want 2", processed)
		retainedAfterCb := sfCb.GetActiveFragments()
		c.Check(len(retainedAfterCb) == 1, "callback failure: retained %d fragments, want 1", len(retainedAfterCb))
		if len(retainedAfterCb) == 1 {
			c.Check(retainedAfterCb[0].Moof.Mfhd.SequenceNumber == 1,
				"callback failure: retained fragment %d, want 1", retainedAfterCb[0].Moof.Mfhd.SequenceNumber)
		}
	})
}

// TestContinuousClockRollback focuses on clock survival: truncated fragments, later-track failures, explicit
// resets, EOF, object reuse and tracks that appear late.
func TestContinuousClockRollback(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		fragments, want := clockFixture(t)
		initData := continuousInit(t)
		moov := decodeInitMoov(t, initData)
		trex1 := trexFor(t, initData, videoTrackID)

		// A truncated fragment moves no clock, and the same decoder continues with the complete fragment.
		d := &mp4.FragmentDecoder{Moov: moov, ContinuousClock: true}
		f1 := decodeOne(t, d, fragments[0], 0)
		c.Check(f1 != nil, "F1 decode")
		truncated := fragments[1][:len(fragments[1])/2]
		_, err := d.DecodeSR(bits.NewFixedSliceReader(truncated), uint64(len(fragments[0])))
		c.Check(errors.Is(err, io.ErrUnexpectedEOF), "truncated F2: got %v, want io.ErrUnexpectedEOF", err)
		var pos uint64 = uint64(len(fragments[0]))
		f2 := decodeOne(t, d, fragments[1], pos) // retry with the complete, hand-built fragment
		got, err := f2.GetFullSamples(trex1)
		c.Check(err == nil, "retried F2: %v", err)
		checkSamples(c, "retried F2", got, want[2][videoTrackID])

		// A failure on the second track of a fragment leaves the first track's clock in place.
		badTfdt := u64(math.MaxUint64) // one more tick overflows
		bad := buildFragment(t, 7, []trafSpec{
			{trackID: videoTrackID, runs: []runSpec{{durs: []uint32{100}, sizes: []uint32{2},
				payload: [][]byte{[]byte("VB")}}}},
			{trackID: audioTrackID, tfdt: badTfdt, runs: []runSpec{{durs: []uint32{1}, sizes: []uint32{2},
				payload: [][]byte{[]byte("AB")}}}},
		})
		badData := encodeClockFragment(t, bad)
		_, err = d.DecodeSR(bits.NewFixedSliceReader(badData), 0)
		c.Check(err != nil, "overflowing fragment: expected an error, got nil")
		// The default decoder accepts the same bytes: overflow protection belongs to continuous mode.
		_, err = (&mp4.FragmentDecoder{}).DecodeSR(bits.NewFixedSliceReader(badData), 0)
		c.Check(err == nil, "default decoder rejects overflowing fragment: %v", err)

		// Retrying with a good fragment continues from the clocks of the last successful fragment (F5 is next
		// in this decoder's sequence).
		pos = 0
		for _, data := range fragments[2:5] { // F3, F4, F5
			decodeOne(t, d, data, pos)
			pos += uint64(len(data))
		}
		good := buildFragment(t, 8, []trafSpec{
			{trackID: videoTrackID, runs: []runSpec{{durs: []uint32{100}, sizes: []uint32{2},
				payload: [][]byte{[]byte("VG")}}}},
			{trackID: audioTrackID, runs: []runSpec{{durs: []uint32{10}, sizes: []uint32{2},
				payload: [][]byte{[]byte("AG")}}}},
		})
		goodData := encodeClockFragment(t, good)
		goodFrag := decodeOne(t, d, goodData, 0)
		goodVideo, err := goodFrag.GetFullSamples(trex1)
		c.Check(err == nil, "good retry video: %v", err)
		c.Check(len(goodVideo) == 1 && goodVideo[0].DecodeTime == 4100,
			"video after failed fragment: got DTS %d, want 4100", goodVideo[0].DecodeTime)

		// Explicitly clearing the clocks restarts every track at zero without a tfdt.
		d.ResetClocks()
		f3 := decodeOne(t, d, fragments[2], 0)
		resetVideo, err := f3.GetFullSamples(trex1)
		c.Check(err == nil, "reset F3 video: %v", err)
		c.Check(len(resetVideo) == 1 && resetVideo[0].DecodeTime == 0,
			"video after ResetClocks: got DTS %d, want 0", resetVideo[0].DecodeTime)

		// Two decoders with object reuse do not share clocks.
		dA := &mp4.FragmentDecoder{Moov: moov, ContinuousClock: true}
		dB := &mp4.FragmentDecoder{Moov: moov, ContinuousClock: true}
		decodeOne(t, dA, fragments[0], 0)
		decodeOne(t, dB, fragments[0], 0)
		decodeOne(t, dA, fragments[1], 0) // A advances; B must not notice
		bF2 := decodeOne(t, dB, fragments[1], 0)
		bSamples, err := bF2.GetFullSamples(trex1)
		c.Check(err == nil, "decoder B F2: %v", err)
		checkSamples(c, "independent decoder B", bSamples, want[2][videoTrackID])

		// A track that first appears in a later fragment starts at zero, and then continues.
		late1 := buildFragment(t, 11, []trafSpec{
			{trackID: thirdTrackID, runs: []runSpec{{durs: []uint32{7}, sizes: []uint32{1},
				payload: [][]byte{[]byte("T")}}}},
		})
		late2 := buildFragment(t, 12, []trafSpec{
			{trackID: thirdTrackID, runs: []runSpec{{durs: []uint32{3}, sizes: []uint32{1},
				payload: [][]byte{[]byte("U")}}}},
		})
		lateData1, lateData2 := encodeClockFragment(t, late1), encodeClockFragment(t, late2)
		lateFrag1 := decodeOne(t, d, lateData1, 0)
		lateSamples1, err := lateFrag1.GetFullSamples(nil)
		c.Check(err == nil, "late track first: %v", err)
		c.Check(len(lateSamples1) == 1 && lateSamples1[0].DecodeTime == 0,
			"late track first DTS: got %d, want 0", lateSamples1[0].DecodeTime)
		lateFrag2 := decodeOne(t, d, lateData2, 0)
		lateSamples2, err := lateFrag2.GetFullSamples(nil)
		c.Check(err == nil, "late track second: %v", err)
		c.Check(len(lateSamples2) == 1 && lateSamples2[0].DecodeTime == 7,
			"late track second DTS: got %d, want 7", lateSamples2[0].DecodeTime)

		// ResetClocks in default mode and on a fresh decoder is harmless.
		(&mp4.FragmentDecoder{}).ResetClocks()
		d.ResetClocks()

		// An empty traf with a tfdt only re-anchors: the next no-tfdt fragment starts at the tfdt value.
		reAnchor := buildFragment(t, 13, []trafSpec{
			{trackID: videoTrackID, tfdt: u64(7000)}, // empty traf, no truns
		})
		follow := buildFragment(t, 14, []trafSpec{
			{trackID: videoTrackID, runs: []runSpec{{durs: []uint32{40}, sizes: []uint32{1},
				payload: [][]byte{[]byte("W")}}}},
		})
		fresh := &mp4.FragmentDecoder{Moov: moov, ContinuousClock: true}
		emptyFrag := decodeOne(t, fresh, encodeClockFragment(t, reAnchor), 0)
		emptySamples, err := emptyFrag.GetFullSamples(trex1)
		c.Check(err == nil && len(emptySamples) == 0, "empty re-anchor traf: got %d samples, error %v",
			len(emptySamples), err)
		followFrag := decodeOne(t, fresh, encodeClockFragment(t, follow), 0)
		followSamples, err := followFrag.GetFullSamples(trex1)
		c.Check(err == nil, "fragment after empty re-anchor: %v", err)
		c.Check(len(followSamples) == 1 && followSamples[0].DecodeTime == 7000,
			"re-anchored clock: got DTS %d, want 7000", followSamples[0].DecodeTime)
	})
}
