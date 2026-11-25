package mp4

import (
	"io"
	"math/big"
	"os"
	"testing"
	"time"

	"github.com/nareix/joy4/av"
	"github.com/nareix/joy4/codec/aacparser"
	"github.com/nareix/joy4/format/mp4/mp4io"
	"github.com/nareix/joy4/task_support/taskcheck"
)

// ---------------------------------------------------------------- helpers

func clockAAC(t *testing.T) av.CodecData {
	t.Helper()
	codec, err := aacparser.NewCodecDataFromMPEG4AudioConfigBytes([]byte{0x12, 0x10})
	if err != nil {
		t.Fatalf("aac codec: %v", err)
	}
	return codec
}

func newTempFile(t *testing.T) *os.File {
	t.Helper()
	f, err := os.CreateTemp(t.TempDir(), "clock-mp4-")
	if err != nil {
		t.Fatalf("temp file: %v", err)
	}
	return f
}

func mustReadMovie(t *testing.T, f *os.File) *mp4io.Movie {
	t.Helper()
	if _, err := f.Seek(0, 0); err != nil {
		t.Fatalf("seek: %v", err)
	}
	atoms, err := mp4io.ReadFileAtoms(f)
	if err != nil {
		t.Fatalf("read atoms: %v", err)
	}
	for _, a := range atoms {
		if m, ok := a.(*mp4io.Movie); ok {
			return m
		}
	}
	t.Fatalf("no moov atom")
	return nil
}

// rawMoov returns the raw moov payload straight from the file, without using
// mp4io.Unmarshal, so box versions and durations can be checked independently.
func rawMoov(t *testing.T, f *os.File) []byte {
	t.Helper()
	if _, err := f.Seek(0, 0); err != nil {
		t.Fatalf("seek: %v", err)
	}
	for {
		hdr := make([]byte, 8)
		if _, err := io.ReadFull(f, hdr); err != nil {
			if err == io.EOF {
				break
			}
			t.Fatalf("read top level: %v", err)
		}
		size := scanU32(hdr[0:])
		tag := mp4io.Tag(scanU32(hdr[4:]))
		if size < 8 {
			break
		}
		body := make([]byte, size-8)
		if _, err := io.ReadFull(f, body); err != nil {
			t.Fatalf("read top level body: %v", err)
		}
		if tag == mp4io.MOOV {
			return body
		}
	}
	t.Fatalf("no moov")
	return nil
}

func scanU32(b []byte) int {
	return int(uint32(b[0])<<24 | uint32(b[1])<<16 | uint32(b[2])<<8 | uint32(b[3]))
}

func scanU64(b []byte) uint64 {
	return uint64(scanU32(b[0:4]))<<32 | uint64(scanU32(b[4:8]))
}

// findBox scans b for the first box with the given tag and returns its payload.
func findBox(b []byte, tag mp4io.Tag) ([]byte, bool) {
	for n := 0; n+8 <= len(b); {
		size := scanU32(b[n:])
		t := mp4io.Tag(scanU32(b[n+4:]))
		if size < 8 || n+size > len(b) {
			return nil, false
		}
		if t == tag {
			return b[n+8 : n+size], true
		}
		n += size
	}
	return nil, false
}

func findPath(b []byte, tags ...mp4io.Tag) ([]byte, bool) {
	cur := b
	var ok bool
	for _, tag := range tags {
		if cur, ok = findBox(cur, tag); !ok {
			return nil, false
		}
	}
	return cur, true
}

func readMvhd(b []byte) (ver uint8, ts uint32, dur uint64, ok bool) {
	body, ok := findBox(b, mp4io.MVHD)
	if !ok || len(body) < 4 {
		return 0, 0, 0, false
	}
	ver = body[0]
	off := 4
	if ver == 0 {
		off += 8
	} else {
		off += 16
	}
	if len(body) < off+8 {
		return 0, 0, 0, false
	}
	ts = uint32(scanU32(body[off:]))
	off += 4
	if ver == 0 {
		dur = uint64(scanU32(body[off:]))
	} else {
		dur = scanU64(body[off:])
	}
	return ver, ts, dur, true
}

func readTkhd(b []byte) (ver uint8, dur uint64, ok bool) {
	body, ok := findPath(b, mp4io.TRAK, mp4io.TKHD)
	if !ok || len(body) < 4 {
		return 0, 0, false
	}
	ver = body[0]
	off := 4
	if ver == 0 {
		off += 8 + 4 + 4
		if len(body) < off+4 {
			return 0, 0, false
		}
		dur = uint64(scanU32(body[off:]))
	} else {
		off += 16 + 4 + 4
		if len(body) < off+8 {
			return 0, 0, false
		}
		dur = scanU64(body[off:])
	}
	return ver, dur, true
}

func readMdhd(b []byte) (ver uint8, ts uint32, dur uint64, ok bool) {
	body, ok := findPath(b, mp4io.TRAK, mp4io.MDIA, mp4io.MDHD)
	if !ok || len(body) < 4 {
		return 0, 0, 0, false
	}
	ver = body[0]
	off := 4
	if ver == 0 {
		off += 8
	} else {
		off += 16
	}
	if len(body) < off+8 {
		return 0, 0, 0, false
	}
	ts = uint32(scanU32(body[off:]))
	off += 4
	if ver == 0 {
		dur = uint64(scanU32(body[off:]))
	} else {
		dur = scanU64(body[off:])
	}
	return ver, ts, dur, true
}

func sttsSum(t *testing.T, tr *mp4io.Track) uint64 {
	t.Helper()
	var n uint64
	for _, e := range tr.Media.Info.Sample.TimeToSample.Entries {
		n += uint64(e.Count) * uint64(e.Duration)
	}
	return n
}

func sttsDurations(tr *mp4io.Track) []uint32 {
	var out []uint32
	for _, e := range tr.Media.Info.Sample.TimeToSample.Entries {
		for i := uint32(0); i < e.Count; i++ {
			out = append(out, e.Duration)
		}
	}
	return out
}

// roundTicks is an independent reimplementation of the documented rule:
// round(ns * timescale / 1e9), ties upward, using exact integers.
func roundTicks(ns int64, ts uint32) uint64 {
	num := new(big.Int).Mul(big.NewInt(ns), big.NewInt(int64(ts)))
	num.Add(num, big.NewInt(500000000))
	return num.Quo(num, big.NewInt(1000000000)).Uint64()
}

func floorDivU64(a, b uint64) uint64 {
	if b == 0 {
		return 0
	}
	return new(big.Int).Quo(new(big.Int).SetUint64(a), new(big.Int).SetUint64(b)).Uint64()
}

// endpointDurations returns the expected sample durations for a strictly
// increasing endpoint list, derived only from independent endpoint rounding.
func endpointDurations(ends []time.Duration, ts uint32) []uint32 {
	out := make([]uint32, 0, len(ends)-1)
	for i := 0; i+1 < len(ends); i++ {
		a := roundTicks(int64(ends[i]), ts)
		b := roundTicks(int64(ends[i+1]), ts)
		out = append(out, uint32(b-a))
	}
	return out
}

func eqDurations(a, b []uint32) bool {
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

// ---------------------------------------------------------------- TestClockTicks

func TestClockTicks(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const n = 50000
		for _, ts := range []uint32{44100, 48000} {
			f := newTempFile(t)
			m := NewClockMuxer(f, []uint32{ts})
			if err := m.WriteHeader([]av.CodecData{clockAAC(t)}); err != nil {
				t.Fatalf("WriteHeader ts=%d: %v", ts, err)
			}
			// Endpoints at exactly 10ms intervals: 441/480 ticks per sample.
			ends := make([]time.Duration, n)
			for i := 0; i < n; i++ {
				ends[i] = time.Duration(i) * 10 * time.Millisecond
				if err := m.WritePacket(av.Packet{Idx: 0, Time: ends[i], Data: []byte{byte(i), 0x11}}); err != nil {
					t.Fatalf("WritePacket %d ts=%d: %v", i, ts, err)
				}
			}
			finalEnd := time.Duration(n) * 10 * time.Millisecond
			if err := m.WriteTrailerAt([]time.Duration{finalEnd}); err != nil {
				t.Fatalf("WriteTrailerAt ts=%d: %v", ts, err)
			}

			allEnds := append(append([]time.Duration(nil), ends...), finalEnd)
			want := endpointDurations(allEnds, ts)

			moov := mustReadMovie(t, f)
			if len(moov.Tracks) != 1 {
				t.Fatalf("ts=%d track count %d", ts, len(moov.Tracks))
			}
			tr := moov.Tracks[0]
			got := sttsDurations(tr)
			c.Check(len(got) == n, "ts=%d stts sample count got=%d want=%d", ts, len(got), n)
			c.Check(eqDurations(got, want), "ts=%d stts durations differ from endpoint differences", ts)
			if len(got) == n {
				tick := uint32(ts) / 100
				c.Check(got[0] == tick && got[n-1] == tick,
					"ts=%d expected every duration=%d got first=%d last=%d", ts, tick, got[0], got[n-1])
				c.Check(len(tr.Media.Info.Sample.TimeToSample.Entries) == 1,
					"ts=%d expected one stts run, got %d", ts, len(tr.Media.Info.Sample.TimeToSample.Entries))
				wantTotal := roundTicks(int64(finalEnd), ts)
				c.Check(sttsSum(t, tr) == wantTotal,
					"ts=%d stts sum got=%d want=%d", ts, sttsSum(t, tr), wantTotal)
				c.Check(tr.Media.Header.Duration == wantTotal,
					"ts=%d mdhd duration got=%d want=%d", ts, tr.Media.Header.Duration, wantTotal)
				c.Check(tr.Media.Header.TimeScale == int32(ts), "ts=%d mdhd timescale", ts)
				c.Check(tr.Header.Duration == floorDivU64(wantTotal*10000, uint64(ts)),
					"ts=%d tkhd duration %d from %d ticks", ts, tr.Header.Duration, wantTotal)
			}

			// Independent raw check: box versions and durations straight from
			// the encoded bytes, without mp4io.
			raw := rawMoov(t, f)
			mdhdVer, mdhdTs, mdhdDur, ok := readMdhd(raw)
			c.Check(ok, "ts=%d raw mdhd present", ts)
			c.Check(mdhdVer == 0, "ts=%d raw mdhd version got=%d want=0", ts, mdhdVer)
			c.Check(mdhdTs == ts, "ts=%d raw mdhd timescale got=%d want=%d", ts, mdhdTs, ts)
			c.Check(mdhdDur == roundTicks(int64(finalEnd), ts),
				"ts=%d raw mdhd duration got=%d want=%d", ts, mdhdDur, roundTicks(int64(finalEnd), ts))
			mvhdVer, mvhdTs, mvhdDur, ok := readMvhd(raw)
			c.Check(ok, "ts=%d raw mvhd present", ts)
			c.Check(mvhdVer == 0, "ts=%d raw mvhd version got=%d want=0", ts, mvhdVer)
			c.Check(mvhdTs == 10000, "ts=%d raw mvhd timescale got=%d want=10000", ts, mvhdTs)
			c.Check(mvhdDur == floorDivU64(roundTicks(int64(finalEnd), ts)*10000, uint64(ts)),
				"ts=%d raw mvhd duration got=%d want=%d", ts, mvhdDur,
				floorDivU64(roundTicks(int64(finalEnd), ts)*10000, uint64(ts)))
			tkhdVer, tkhdDur, ok := readTkhd(raw)
			c.Check(ok, "ts=%d raw tkhd present", ts)
			c.Check(tkhdVer == 0, "ts=%d raw tkhd version got=%d want=0", ts, tkhdVer)
			c.Check(tkhdDur == floorDivU64(roundTicks(int64(finalEnd), ts)*10000, uint64(ts)),
				"ts=%d raw tkhd duration got=%d", ts, tkhdDur)
		}

		// Interleaved tracks sharing one movie timebase, with distinct
		// timescales and distinct endpoint sets.
		f := newTempFile(t)
		m := NewClockMuxer(f, []uint32{44100, 48000})
		if err := m.WriteHeader([]av.CodecData{clockAAC(t), clockAAC(t)}); err != nil {
			t.Fatalf("interleaved WriteHeader: %v", err)
		}
		const steps = 600
		track1Time := func(i int) time.Duration {
			if i == 0 {
				return 0 // clock mode requires the first packet at time zero
			}
			return time.Duration(i)*10*time.Millisecond + 3*time.Millisecond
		}
		for i := 0; i < steps; i++ {
			when := time.Duration(i) * 10 * time.Millisecond
			if err := m.WritePacket(av.Packet{Idx: 0, Time: when, Data: []byte{byte(i)}}); err != nil {
				t.Fatalf("interleaved a %d: %v", i, err)
			}
			if err := m.WritePacket(av.Packet{Idx: 1, Time: track1Time(i), Data: []byte{byte(i)}}); err != nil {
				t.Fatalf("interleaved b %d: %v", i, err)
			}
		}
		end0 := time.Duration(steps) * 10 * time.Millisecond
		end1 := track1Time(steps-1) + 10*time.Millisecond
		if err := m.WriteTrailerAt([]time.Duration{end0, end1}); err != nil {
			t.Fatalf("interleaved WriteTrailerAt: %v", err)
		}
		moov := mustReadMovie(t, f)
		c.Check(len(moov.Tracks) == 2, "interleaved track count got=%d want=2", len(moov.Tracks))
		if len(moov.Tracks) == 2 {
			c.Check(moov.Tracks[0].Media.Header.TimeScale == 44100, "interleaved track0 timescale")
			c.Check(moov.Tracks[1].Media.Header.TimeScale == 48000, "interleaved track1 timescale")
			s0 := sttsSum(t, moov.Tracks[0])
			s1 := sttsSum(t, moov.Tracks[1])
			c.Check(moov.Tracks[0].Media.Header.Duration == s0, "interleaved t0 mdhd==stts")
			c.Check(moov.Tracks[1].Media.Header.Duration == s1, "interleaved t1 mdhd==stts")
			wantMov0 := floorDivU64(s0*10000, 44100)
			wantMov1 := floorDivU64(s1*10000, 48000)
			c.Check(moov.Tracks[0].Header.Duration == wantMov0,
				"interleaved t0 tkhd got=%d want=%d", moov.Tracks[0].Header.Duration, wantMov0)
			c.Check(moov.Tracks[1].Header.Duration == wantMov1,
				"interleaved t1 tkhd got=%d want=%d", moov.Tracks[1].Header.Duration, wantMov1)
			max := wantMov0
			if wantMov1 > max {
				max = wantMov1
			}
			c.Check(moov.Header.Duration == max, "interleaved mvhd got=%d want=%d", moov.Header.Duration, max)
			c.Check(moov.Header.TimeScale == 10000, "interleaved movie timescale")
			e0 := make([]time.Duration, 0, steps+1)
			e1 := make([]time.Duration, 0, steps+1)
			for i := 0; i < steps; i++ {
				e0 = append(e0, time.Duration(i)*10*time.Millisecond)
				e1 = append(e1, track1Time(i))
			}
			e0 = append(e0, end0)
			e1 = append(e1, end1)
			c.Check(eqDurations(sttsDurations(moov.Tracks[0]), endpointDurations(e0, 44100)),
				"interleaved t0 durations from endpoints")
			c.Check(eqDurations(sttsDurations(moov.Tracks[1]), endpointDurations(e1, 48000)),
				"interleaved t1 durations from endpoints")
		}
	})
}

// ---------------------------------------------------------------- TestClockLongHeaders

func TestClockLongHeaders(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Nanosecond ticks: each 4s sample is 4e9 ticks (under uint32), but the
		// running total crosses 2^32, so mdhd needs version 1 while tkhd/mvhd,
		// converted to the 10000 timebase, still fit version 0.
		const ts = uint32(1000000000)
		f := newTempFile(t)
		m := NewClockMuxer(f, []uint32{ts})
		if err := m.WriteHeader([]av.CodecData{clockAAC(t)}); err != nil {
			t.Fatalf("long header WriteHeader: %v", err)
		}
		ends := []time.Duration{
			0,
			4 * time.Second,
			8 * time.Second,
			12 * time.Second,
			16 * time.Second,
		}
		for i, e := range ends {
			if err := m.WritePacket(av.Packet{Idx: 0, Time: e, Data: []byte{byte(i)}}); err != nil {
				t.Fatalf("long WritePacket %d: %v", i, err)
			}
		}
		finalEnd := 20 * time.Second
		if err := m.WriteTrailerAt([]time.Duration{finalEnd}); err != nil {
			t.Fatalf("long WriteTrailerAt: %v", err)
		}

		moov := mustReadMovie(t, f)
		tr := moov.Tracks[0]
		wantDurs := []uint32{4000000000, 4000000000, 4000000000, 4000000000, 4000000000}
		got := sttsDurations(tr)
		c.Check(eqDurations(got, wantDurs), "long durations got=%v want=%v", got, wantDurs)
		sum := sttsSum(t, tr)
		c.Check(sum == 20000000000, "long stts sum got=%d want=20000000000", sum)
		c.Check(tr.Media.Header.Duration == 20000000000, "long mdhd duration got=%d", tr.Media.Header.Duration)
		c.Check(tr.Media.Header.Version == 1, "long mdhd version got=%d want=1", tr.Media.Header.Version)
		wantTkhd := floorDivU64(20000000000*10000, 1000000000)
		c.Check(wantTkhd == 200000, "long expected movie duration %d", wantTkhd)
		c.Check(tr.Header.Duration == wantTkhd, "long tkhd duration got=%d want=%d", tr.Header.Duration, wantTkhd)
		c.Check(tr.Header.Version == 0, "long tkhd version got=%d want=0 (fits 32 bits)", tr.Header.Version)
		c.Check(moov.Header.Duration == wantTkhd, "long mvhd duration got=%d want=%d", moov.Header.Duration, wantTkhd)
		c.Check(moov.Header.Version == 0, "long mvhd version got=%d want=0", moov.Header.Version)

		// Independent raw inspection of the encoded boxes.
		raw := rawMoov(t, f)
		mdhdVer, mdhdTs, mdhdDur, ok := readMdhd(raw)
		c.Check(ok, "long raw mdhd present")
		c.Check(mdhdVer == 1, "long raw mdhd version got=%d want=1", mdhdVer)
		c.Check(mdhdTs == ts, "long raw mdhd timescale got=%d", mdhdTs)
		c.Check(mdhdDur == 20000000000, "long raw mdhd duration got=%d", mdhdDur)
		tkhdVer, tkhdDur, ok := readTkhd(raw)
		c.Check(ok, "long raw tkhd present")
		c.Check(tkhdVer == 0, "long raw tkhd version got=%d want=0", tkhdVer)
		c.Check(tkhdDur == wantTkhd, "long raw tkhd duration got=%d want=%d", tkhdDur, wantTkhd)
		mvhdVer, mvhdTs, mvhdDur, ok := readMvhd(raw)
		c.Check(ok, "long raw mvhd present")
		c.Check(mvhdVer == 0, "long raw mvhd version got=%d", mvhdVer)
		c.Check(mvhdTs == 10000, "long raw mvhd timescale got=%d", mvhdTs)
		c.Check(mvhdDur == wantTkhd, "long raw mvhd duration got=%d want=%d", mvhdDur, wantTkhd)

		// Independently constructed version-1 boxes: Len must equal the number
		// of bytes Marshal writes, and every versioned field must round-trip.
		longDur := uint64(0x1_0000_0001)
		mvhd := &mp4io.MovieHeader{
			Version:       1,
			CreateTime:    time.Date(2000, 1, 2, 3, 4, 5, 0, time.UTC),
			ModifyTime:    time.Date(2030, 5, 6, 7, 8, 9, 0, time.UTC),
			TimeScale:     10000,
			Duration:      longDur,
			NextTrackId:   3,
			PreferredRate: 1,
		}
		mb := make([]byte, mvhd.Len())
		n := mvhd.Marshal(mb)
		c.Check(n == mvhd.Len(), "v1 mvhd Len=%d Marshal=%d", mvhd.Len(), n)
		c.Check(scanU32(mb[0:]) == n, "v1 mvhd size field %d matches Len %d", scanU32(mb[0:]), n)
		rver, rts, rdur, ok := readMvhd(mb)
		c.Check(ok, "v1 mvhd independent read ok")
		c.Check(rver == 1, "v1 mvhd version got=%d", rver)
		c.Check(rts == 10000, "v1 mvhd timescale got=%d", rts)
		c.Check(rdur == longDur, "v1 mvhd duration got=%d want=%d", rdur, longDur)
		var mvhd2 mp4io.MovieHeader
		un, err := mvhd2.Unmarshal(mb, 0)
		c.Check(err == nil, "v1 mvhd unmarshal err: %v", err)
		c.Check(un == len(mb), "v1 mvhd unmarshal length got=%d want=%d", un, len(mb))
		c.Check(mvhd2.Duration == longDur, "v1 mvhd roundtrip duration got=%d want=%d", mvhd2.Duration, longDur)
		c.Check(mvhd2.CreateTime.Equal(mvhd.CreateTime), "v1 mvhd create time roundtrip got=%v want=%v", mvhd2.CreateTime, mvhd.CreateTime)
		c.Check(mvhd2.ModifyTime.Equal(mvhd.ModifyTime), "v1 mvhd modify time roundtrip got=%v want=%v", mvhd2.ModifyTime, mvhd.ModifyTime)

		tkhd := &mp4io.TrackHeader{
			Version:     1,
			CreateTime:  time.Date(1999, 2, 3, 4, 5, 6, 0, time.UTC),
			ModifyTime:  time.Date(2031, 6, 7, 8, 9, 10, 0, time.UTC),
			TrackId:     1,
			Duration:    longDur,
			Matrix:      [9]int32{0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000},
			TrackWidth:  640,
			TrackHeight: 480,
		}
		tb := make([]byte, tkhd.Len())
		tn := tkhd.Marshal(tb)
		c.Check(tn == tkhd.Len(), "v1 tkhd Len=%d Marshal=%d", tkhd.Len(), tn)
		c.Check(scanU32(tb[0:]) == tn, "v1 tkhd size field matches Len")
		var tkhd2 mp4io.TrackHeader
		tun, terr := tkhd2.Unmarshal(tb, 0)
		c.Check(terr == nil, "v1 tkhd unmarshal err: %v", terr)
		c.Check(tun == len(tb), "v1 tkhd unmarshal length got=%d", tun)
		c.Check(tkhd2.Duration == longDur, "v1 tkhd roundtrip duration got=%d", tkhd2.Duration)
		c.Check(tkhd2.CreateTime.Equal(tkhd.CreateTime), "v1 tkhd create time roundtrip")
		c.Check(tkhd2.ModifyTime.Equal(tkhd.ModifyTime), "v1 tkhd modify time roundtrip")

		mdhd := &mp4io.MediaHeader{
			Version:    1,
			CreateTime: time.Date(1998, 1, 1, 0, 0, 0, 0, time.UTC),
			ModifyTime: time.Date(2032, 2, 2, 0, 0, 0, 0, time.UTC),
			TimeScale:  90000,
			Duration:   longDur,
			Language:   21956,
		}
		db := make([]byte, mdhd.Len())
		dn := mdhd.Marshal(db)
		c.Check(dn == mdhd.Len(), "v1 mdhd Len=%d Marshal=%d", mdhd.Len(), dn)
		c.Check(scanU32(db[0:]) == dn, "v1 mdhd size field matches Len")
		var mdhd2 mp4io.MediaHeader
		dun, derr := mdhd2.Unmarshal(db, 0)
		c.Check(derr == nil, "v1 mdhd unmarshal err: %v", derr)
		c.Check(dun == len(db), "v1 mdhd unmarshal length got=%d", dun)
		c.Check(mdhd2.Duration == longDur, "v1 mdhd roundtrip duration got=%d", mdhd2.Duration)
		c.Check(mdhd2.CreateTime.Equal(mdhd.CreateTime), "v1 mdhd create time roundtrip")
		c.Check(mdhd2.ModifyTime.Equal(mdhd.ModifyTime), "v1 mdhd modify time roundtrip")

		// Truncated fields and unsupported versions must return errors.
		short := append([]byte(nil), mb[:20]...)
		var truncatedMvhd mp4io.MovieHeader
		_, terr = truncatedMvhd.Unmarshal(short, 0)
		c.Check(terr != nil, "truncated v1 mvhd should error")

		badVer := append([]byte(nil), mb...)
		badVer[8] = 2 // version byte follows the 8-byte size/tag header
		var badMvhd mp4io.MovieHeader
		_, berr := badMvhd.Unmarshal(badVer, 0)
		c.Check(berr != nil, "unsupported mvhd version should error")

		shortTkhd := append([]byte(nil), tb[:24]...)
		var truncatedTkhd mp4io.TrackHeader
		_, terr = truncatedTkhd.Unmarshal(shortTkhd, 0)
		c.Check(terr != nil, "truncated v1 tkhd should error")

		shortMdhd := append([]byte(nil), db[:18]...)
		var truncatedMdhd mp4io.MediaHeader
		_, derr = truncatedMdhd.Unmarshal(shortMdhd, 0)
		c.Check(derr != nil, "truncated v1 mdhd should error")

		// Old short file comparison: version 0 keeps the historical layout.
		shortMvhd := &mp4io.MovieHeader{TimeScale: 10000, Duration: 1234, NextTrackId: 2, PreferredRate: 1}
		sb := make([]byte, shortMvhd.Len())
		sn := shortMvhd.Marshal(sb)
		c.Check(sn == shortMvhd.Len(), "v0 mvhd Len=%d Marshal=%d", shortMvhd.Len(), sn)
		c.Check(shortMvhd.Len() == 108, "v0 mvhd Len got=%d want=108", shortMvhd.Len())
		ver0, ts0, dur0, ok0 := readMvhd(sb)
		c.Check(ok0 && ver0 == 0 && ts0 == 10000 && dur0 == 1234,
			"v0 mvhd layout ver=%d ts=%d dur=%d", ver0, ts0, dur0)
		var mvhd0 mp4io.MovieHeader
		un0, err0 := mvhd0.Unmarshal(sb, 0)
		c.Check(err0 == nil && un0 == len(sb), "v0 mvhd unmarshal len=%d err=%v", un0, err0)
		c.Check(mvhd0.Duration == 1234, "v0 mvhd duration got=%d", mvhd0.Duration)
	})
}

// ---------------------------------------------------------------- TestClockReadback

func TestClockReadback(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Nanosecond ticks with 4s samples: the running time passes 2^32 ticks,
		// so ReadPacket, CurrentTime and SeekToTime must not wrap.
		const ts = uint32(1000000000)
		f := newTempFile(t)
		m := NewClockMuxer(f, []uint32{ts})
		if err := m.WriteHeader([]av.CodecData{clockAAC(t)}); err != nil {
			t.Fatalf("readback WriteHeader: %v", err)
		}
		ends := []time.Duration{
			0,
			4 * time.Second,
			8 * time.Second,
			12 * time.Second,
			16 * time.Second,
		}
		for i, e := range ends {
			if err := m.WritePacket(av.Packet{Idx: 0, Time: e, Data: []byte{byte(i), 0x22}}); err != nil {
				t.Fatalf("readback WritePacket %d: %v", i, err)
			}
		}
		finalEnd := 20 * time.Second
		if err := m.WriteTrailerAt([]time.Duration{finalEnd}); err != nil {
			t.Fatalf("readback WriteTrailerAt: %v", err)
		}

		if _, err := f.Seek(0, 0); err != nil {
			t.Fatalf("seek: %v", err)
		}
		dm := NewDemuxer(f)
		if _, err := dm.Streams(); err != nil {
			t.Fatalf("readback Streams: %v", err)
		}
		var readTimes []time.Duration
		for {
			pkt, err := dm.ReadPacket()
			if err == io.EOF {
				break
			}
			if err != nil {
				t.Fatalf("readback ReadPacket: %v", err)
			}
			if len(pkt.Data) != 2 {
				t.Fatalf("readback payload len=%d", len(pkt.Data))
			}
			readTimes = append(readTimes, pkt.Time)
		}
		c.Check(len(readTimes) == len(ends), "readback packet count got=%d want=%d", len(readTimes), len(ends))
		for i := 0; i < len(readTimes) && i < len(ends); i++ {
			wantTicks := roundTicks(int64(ends[i]), ts)
			wantNs := time.Duration(wantTicks * uint64(time.Second) / uint64(ts))
			c.Check(readTimes[i] == wantNs,
				"readback packet %d time got=%v want=%v (ticks=%d)", i, readTimes[i], wantNs, wantTicks)
			c.Check(readTimes[i] >= 0 && readTimes[i] < 30*time.Second,
				"readback packet %d time=%v did not wrap", i, readTimes[i])
		}

		ct := dm.CurrentTime()
		c.Check(ct == 20*time.Second, "readback CurrentTime got=%v want=20s", ct)

		// SeekToTime beyond 32-bit ticks: landing sample must contain the target
		// and reported time must agree with the next packet.
		for _, target := range []time.Duration{
			5 * time.Second,
			10 * time.Second,
			18 * time.Second,
		} {
			if err := dm.SeekToTime(target); err != nil {
				t.Fatalf("readback SeekToTime %v: %v", target, err)
			}
			// Seeking lands on the sample whose span contains target.
			wantStart := (target / (4 * time.Second)) * (4 * time.Second)
			got := dm.CurrentTime()
			c.Check(got == wantStart, "readback seek %v position got=%v want=%v", target, got, wantStart)
			pkt, err := dm.ReadPacket()
			if err != nil {
				t.Fatalf("readback seek %v ReadPacket: %v", target, err)
			}
			c.Check(pkt.Time == wantStart, "readback seek %v packet time got=%v want=%v", target, pkt.Time, wantStart)
		}

		// Rejection then continued submission: a rejected packet is not
		// consumed, and a valid one afterwards still lands in the file.
		rf := newTempFile(t)
		rm := NewClockMuxer(rf, []uint32{44100})
		if err := rm.WriteHeader([]av.CodecData{clockAAC(t)}); err != nil {
			t.Fatalf("reject WriteHeader: %v", err)
		}
		c.Check(rm.WritePacket(av.Packet{Idx: 0, Time: time.Millisecond, Data: []byte{9}}) != nil,
			"first packet with nonzero time must be rejected")
		c.Check(rm.streams[0].sampleIndex == 0 && rm.wpos == 8,
			"rejected packet must leave muxer state untouched")
		c.Check(rm.WritePacket(av.Packet{Idx: 0, Time: 0, Data: []byte{1}}) == nil,
			"zero-time first packet after rejection must be accepted")
		c.Check(rm.WritePacket(av.Packet{Idx: 0, Time: 0, Data: []byte{2}}) != nil,
			"non-increasing time must be rejected")
		c.Check(rm.WritePacket(av.Packet{Idx: 0, Time: 5 * time.Millisecond, Data: []byte{3}}) == nil,
			"valid packet after rejection must be accepted")
		c.Check(rm.WritePacket(av.Packet{Idx: 0, Time: 10 * time.Millisecond, Data: []byte{4}}) == nil,
			"second valid packet must be accepted")
		if err := rm.WriteTrailerAt([]time.Duration{15 * time.Millisecond}); err != nil {
			t.Fatalf("reject trailer: %v", err)
		}
		rtr := mustReadMovie(t, rf).Tracks[0]
		c.Check(eqDurations(sttsDurations(rtr), []uint32{221, 220, 221}),
			"reject/continue durations got=%v", sttsDurations(rtr))

		// Same-tick endpoints: rejected without changing state, then a later
		// valid endpoint still commits the pending sample.
		tiny := newTempFile(t)
		tm := NewClockMuxer(tiny, []uint32{1})
		if err := tm.WriteHeader([]av.CodecData{clockAAC(t)}); err != nil {
			t.Fatalf("tiny WriteHeader: %v", err)
		}
		c.Check(tm.WritePacket(av.Packet{Idx: 0, Time: 0, Data: []byte{1}}) == nil, "tiny first packet")
		c.Check(tm.WritePacket(av.Packet{Idx: 0, Time: 900 * time.Millisecond, Data: []byte{2}}) == nil,
			"tiny 900ms packet accepted")
		idxBefore := tm.streams[0].sampleIndex
		wposBefore := tm.wpos
		c.Check(tm.WritePacket(av.Packet{Idx: 0, Time: 1000 * time.Millisecond, Data: []byte{3}}) != nil,
			"same-tick endpoints must be rejected")
		c.Check(tm.streams[0].sampleIndex == idxBefore && tm.wpos == wposBefore,
			"same-tick rejection must not consume the packet or change samples")
		c.Check(tm.WritePacket(av.Packet{Idx: 0, Time: 1500 * time.Millisecond, Data: []byte{4}}) == nil,
			"valid packet after same-tick rejection")

		// A single sample duration above uint32 ticks is rejected; the pending
		// sample survives and can be committed with a smaller end.
		huge := newTempFile(t)
		hm := NewClockMuxer(huge, []uint32{1000000000})
		if err := hm.WriteHeader([]av.CodecData{clockAAC(t)}); err != nil {
			t.Fatalf("huge WriteHeader: %v", err)
		}
		c.Check(hm.WritePacket(av.Packet{Idx: 0, Time: 0, Data: []byte{1}}) == nil, "huge first packet")
		c.Check(hm.WritePacket(av.Packet{Idx: 0, Time: 5 * time.Second, Data: []byte{2}}) != nil,
			"uint32-overflow duration must be rejected")
		c.Check(hm.WritePacket(av.Packet{Idx: 0, Time: 2 * time.Second, Data: []byte{3}}) == nil,
			"valid packet after uint32 overflow rejection")
		if err := hm.WriteTrailerAt([]time.Duration{3 * time.Second}); err != nil {
			t.Fatalf("huge trailer: %v", err)
		}
		htr := mustReadMovie(t, huge).Tracks[0]
		c.Check(eqDurations(sttsDurations(htr), []uint32{2000000000, 1000000000}),
			"uint32 overflow recovery durations got=%v", sttsDurations(htr))

		// Trailer retry: a bad end on one track fails all tracks; after fixing it
		// the write succeeds without duplicating samples.
		tf := newTempFile(t)
		tmm := NewClockMuxer(tf, []uint32{44100, 48000})
		if err := tmm.WriteHeader([]av.CodecData{clockAAC(t), clockAAC(t)}); err != nil {
			t.Fatalf("trailer retry WriteHeader: %v", err)
		}
		for i := 0; i < 10; i++ {
			when := time.Duration(i) * 10 * time.Millisecond
			if err := tmm.WritePacket(av.Packet{Idx: 0, Time: when, Data: []byte{byte(i)}}); err != nil {
				t.Fatalf("retry t0 %d: %v", i, err)
			}
			if err := tmm.WritePacket(av.Packet{Idx: 1, Time: when, Data: []byte{byte(i)}}); err != nil {
				t.Fatalf("retry t1 %d: %v", i, err)
			}
		}
		last := 9 * 10 * time.Millisecond
		c.Check(tmm.WriteTrailerAt([]time.Duration{last + 10*time.Millisecond, last}) != nil,
			"trailer end not later than last packet must be rejected")
		c.Check(tmm.WriteTrailerAt([]time.Duration{time.Second}) != nil,
			"trailer end count mismatch must be rejected")
		c.Check(tmm.WriteTrailerAt([]time.Duration{last + 10*time.Millisecond, last + 10*time.Millisecond}) == nil,
			"corrected trailer must succeed")
		moov := mustReadMovie(t, tf)
		if len(moov.Tracks) == 2 {
			c.Check(sttsSum(t, moov.Tracks[0]) == 4410,
				"retry t0 stts sum got=%d want=4410", sttsSum(t, moov.Tracks[0]))
			c.Check(sttsSum(t, moov.Tracks[1]) == 4800,
				"retry t1 stts sum got=%d want=4800", sttsSum(t, moov.Tracks[1]))
			c.Check(len(sttsDurations(moov.Tracks[0])) == 10, "retry t0 sample count")
			c.Check(len(sttsDurations(moov.Tracks[1])) == 10, "retry t1 sample count")
		}

		// Empty track only accepts zero, and ends are copied before checking.
		ef := newTempFile(t)
		em := NewClockMuxer(ef, []uint32{44100, 48000})
		if err := em.WriteHeader([]av.CodecData{clockAAC(t), clockAAC(t)}); err != nil {
			t.Fatalf("empty WriteHeader: %v", err)
		}
		if err := em.WritePacket(av.Packet{Idx: 0, Time: 0, Data: []byte{1}}); err != nil {
			t.Fatalf("empty track packet: %v", err)
		}
		if err := em.WritePacket(av.Packet{Idx: 0, Time: 10 * time.Millisecond, Data: []byte{2}}); err != nil {
			t.Fatalf("empty track packet 2: %v", err)
		}
		c.Check(em.WriteTrailerAt([]time.Duration{20 * time.Millisecond, time.Millisecond}) != nil,
			"empty track with nonzero end must be rejected")
		c.Check(em.WriteTrailerAt([]time.Duration{20 * time.Millisecond, 0}) == nil,
			"empty track with zero end must succeed")

		// Mode mismatches: clock mode rejects the old trailer, legacy mode
		// rejects the new one, and clock WriteHeader rejects bad timescales
		// before writing a byte.
		mf := newTempFile(t)
		mm := NewClockMuxer(mf, []uint32{44100})
		if err := mm.WriteHeader([]av.CodecData{clockAAC(t)}); err != nil {
			t.Fatalf("mode WriteHeader: %v", err)
		}
		c.Check(mm.WriteTrailer() != nil, "clock mode WriteTrailer must error")

		lf := newTempFile(t)
		lm := NewMuxer(lf)
		if err := lm.WriteHeader([]av.CodecData{clockAAC(t)}); err != nil {
			t.Fatalf("legacy WriteHeader: %v", err)
		}
		c.Check(lm.WriteTrailerAt([]time.Duration{time.Second}) != nil, "legacy WriteTrailerAt must error")

		verifyNoBytes := func(label string, scales []uint32, codecs []av.CodecData) {
			hf := newTempFile(t)
			hm := NewClockMuxer(hf, scales)
			err := hm.WriteHeader(codecs)
			c.Check(err != nil, "%s: WriteHeader should fail", label)
			if st, serr := hf.Seek(0, io.SeekEnd); serr == nil {
				c.Check(st == 0, "%s: WriteHeader wrote %d bytes before failing", label, st)
			}
		}
		verifyNoBytes("fewer timescales", []uint32{44100}, []av.CodecData{clockAAC(t), clockAAC(t)})
		verifyNoBytes("more timescales", []uint32{44100, 48000}, []av.CodecData{clockAAC(t)})
		verifyNoBytes("zero timescale", []uint32{0}, []av.CodecData{clockAAC(t)})
		verifyNoBytes("too large timescale", []uint32{1000000001}, []av.CodecData{clockAAC(t)})

		// The timescale slice is copied by the constructor.
		scales := []uint32{44100}
		cf := newTempFile(t)
		cm := NewClockMuxer(cf, scales)
		scales[0] = 0
		c.Check(cm.WriteHeader([]av.CodecData{clockAAC(t)}) == nil, "timescales must be copied by NewClockMuxer")

		// Old short-file path still works: the legacy muxer with 101 packets
		// reads back with its historical timing behaviour.
		legacy := newTempFile(t)
		lm2 := NewMuxer(legacy)
		if err := lm2.WriteHeader([]av.CodecData{clockAAC(t)}); err != nil {
			t.Fatalf("legacy WriteHeader: %v", err)
		}
		const n = 101
		for i := 0; i < n; i++ {
			tm := time.Duration(int64(i) * 1024 * int64(time.Second) / 44100)
			if err := lm2.WritePacket(av.Packet{Idx: 0, Time: tm, Data: []byte{byte(i), 0x55}}); err != nil {
				t.Fatalf("legacy WritePacket %d: %v", i, err)
			}
		}
		if err := lm2.WriteTrailer(); err != nil {
			t.Fatalf("legacy WriteTrailer: %v", err)
		}
		lmoov := mustReadMovie(t, legacy)
		ltr := lmoov.Tracks[0]
		c.Check(len(sttsDurations(ltr)) == n, "legacy sample count got=%d want=%d", len(sttsDurations(ltr)), n)
		c.Check(sttsSum(t, ltr) == 208900, "legacy stts ticks got=%d want=208900", sttsSum(t, ltr))
		c.Check(ltr.Media.Header.TimeScale == 90000, "legacy mdhd timescale got=%d", ltr.Media.Header.TimeScale)
		c.Check(ltr.Media.Header.Version == 0 && ltr.Header.Version == 0 && lmoov.Header.Version == 0,
			"legacy boxes stay version 0")

		if _, err := legacy.Seek(0, 0); err != nil {
			t.Fatalf("legacy seek: %v", err)
		}
		ldm := NewDemuxer(legacy)
		if _, err := ldm.Streams(); err != nil {
			t.Fatalf("legacy Streams: %v", err)
		}
		legacyTimes := make([]time.Duration, 0, n)
		for {
			pkt, err := ldm.ReadPacket()
			if err == io.EOF {
				break
			}
			if err != nil {
				t.Fatalf("legacy ReadPacket: %v", err)
			}
			legacyTimes = append(legacyTimes, pkt.Time)
		}
		c.Check(len(legacyTimes) == n, "legacy readback count got=%d want=%d", len(legacyTimes), n)
		if len(legacyTimes) == n {
			c.Check(legacyTimes[0] == 0, "legacy first time got=%v", legacyTimes[0])
			c.Check(legacyTimes[n-1] < 3*time.Second, "legacy last time got=%v", legacyTimes[n-1])
			monotonic := true
			for i := 1; i < n; i++ {
				if legacyTimes[i] < legacyTimes[i-1] {
					monotonic = false
				}
			}
			c.Check(monotonic, "legacy readback times must be monotonic")
		}

		// The old trailer keeps failing even after a successful clock trailer:
		// mode never flips back.
		vm := NewClockMuxer(newTempFile(t), []uint32{90000})
		if err := vm.WriteHeader([]av.CodecData{clockAAC(t)}); err != nil {
			t.Fatalf("mode-after-trailer WriteHeader: %v", err)
		}
		if err := vm.WritePacket(av.Packet{Idx: 0, Time: 0, Data: []byte{1}}); err != nil {
			t.Fatalf("mode-after-trailer packet: %v", err)
		}
		if err := vm.WriteTrailerAt([]time.Duration{40 * time.Millisecond}); err != nil {
			t.Fatalf("mode-after-trailer WriteTrailerAt: %v", err)
		}
		c.Check(vm.WriteTrailer() != nil, "clock WriteTrailer must keep failing after WriteTrailerAt")
	})
}
