package mp4

import (
	"encoding/binary"
	"errors"
	"io"
	"math"
	"testing"
	"time"

	"encoding/hex"

	"github.com/nareix/joy4/av"
	"github.com/nareix/joy4/codec/aacparser"
	"github.com/nareix/joy4/codec/h264parser"
	"github.com/nareix/joy4/format/mp4/mp4io"
	"github.com/nareix/joy4/task_support/taskcheck"
)

// memFile is an in-memory io.WriteSeeker so the tests never touch the disk.
type memFile struct {
	data []byte
	pos  int64
}

func (m *memFile) Write(p []byte) (int, error) {
	end := m.pos + int64(len(p))
	if end > int64(len(m.data)) {
		grown := make([]byte, end)
		copy(grown, m.data)
		m.data = grown
	}
	copy(m.data[m.pos:end], p)
	m.pos = end
	return len(p), nil
}

func (m *memFile) Seek(offset int64, whence int) (int64, error) {
	switch whence {
	case io.SeekStart:
		m.pos = offset
	case io.SeekCurrent:
		m.pos += offset
	case io.SeekEnd:
		m.pos = int64(len(m.data)) + offset
	}
	return m.pos, nil
}

func (m *memFile) Read(p []byte) (int, error) {
	if m.pos >= int64(len(m.data)) {
		return 0, io.EOF
	}
	n := copy(p, m.data[m.pos:])
	m.pos += int64(n)
	return n, nil
}

// rawBox is a box located by walking the wire bytes directly, without using
// mp4io.Unmarshal, so version/duration checks are independent of the library
// code under test.
type rawBox struct {
	tag     string
	payload []byte // bytes following the 8-byte size/tag header
}

var containerTags = map[string]bool{
	"moov": true, "trak": true, "mdia": true, "minf": true,
	"stbl": true, "dinf": true, "udta": true,
}

func walkRawBoxes(data []byte, out map[string][]rawBox) {
	for off := 0; off+8 <= len(data); {
		size := int(binary.BigEndian.Uint32(data[off:]))
		tag := string(data[off+4 : off+8])
		if size < 8 || off+size > len(data) {
			return
		}
		payload := data[off+8 : off+size]
		out[tag] = append(out[tag], rawBox{tag: tag, payload: payload})
		if containerTags[tag] {
			walkRawBoxes(payload, out)
		}
		off += size
	}
}

// headerVersionDuration reads the version byte and duration field of a raw
// mvhd/tkhd/mdhd box using the ISO layout, independently of mp4io.
func headerVersionDuration(b rawBox) (uint8, uint64) {
	ver := b.payload[0]
	switch b.tag {
	case "mvhd", "mdhd":
		if ver == 1 {
			return ver, binary.BigEndian.Uint64(b.payload[24:32])
		}
		return ver, uint64(binary.BigEndian.Uint32(b.payload[16:20]))
	case "tkhd":
		if ver == 1 {
			return ver, binary.BigEndian.Uint64(b.payload[28:36])
		}
		return ver, uint64(binary.BigEndian.Uint32(b.payload[20:24]))
	}
	return ver, 0
}

func rawSttsEntries(b rawBox) [][2]uint32 {
	// payload: version(1) flags(3) entry_count(4) entries{count(4),delta(4)}
	count := binary.BigEndian.Uint32(b.payload[4:8])
	entries := make([][2]uint32, 0, count)
	off := 8
	for i := uint32(0); i < count; i++ {
		entries = append(entries, [2]uint32{
			binary.BigEndian.Uint32(b.payload[off:]),
			binary.BigEndian.Uint32(b.payload[off+4:]),
		})
		off += 8
	}
	return entries
}

func mustAACCodec(t *testing.T, cfg []byte) av.CodecData {
	c, err := aacparser.NewCodecDataFromMPEG4AudioConfigBytes(cfg)
	if err != nil {
		t.Fatalf("aac codec: %v", err)
	}
	return c
}

func mustH264Codec(t *testing.T) av.CodecData {
	sps, _ := hex.DecodeString("6742000af841a2")
	pps, _ := hex.DecodeString("68ce3880")
	c, err := h264parser.NewCodecDataFromSPSAndPPS(sps, pps)
	if err != nil {
		t.Fatalf("h264 codec: %v", err)
	}
	return c
}

// ticksToTime mirrors the demuxer conversion and is computed independently in
// the test: whole seconds first, then the remainder, so no 32-bit wrap occurs.
func ticksToTime(ticks, scale uint64) time.Duration {
	secs := ticks / scale
	rem := ticks % scale
	return time.Duration(secs*1000000000 + rem*1000000000/scale)
}

func sttsTotal(entries []mp4io.TimeToSampleEntry) uint64 {
	var total uint64
	for _, e := range entries {
		total += uint64(e.Count) * uint64(e.Duration)
	}
	return total
}

func equalPairs(a, b [][2]uint32) bool {
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

func equalU32(a, b []uint32) bool {
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

const ms = time.Millisecond

// TestClockTicks covers precise endpoint quantization at 44100/48000,
// including round-half-up, long uniform runs and interleaved A/V tracks.
func TestClockTicks(t *testing.T) {
	aac44 := mustAACCodec(t, []byte{0x12, 0x10})
	aac48 := mustAACCodec(t, []byte{0x11, 0x90})
	h264 := mustH264Codec(t)

	taskcheck.Run(t, func(c *taskcheck.C) {
		// --- 44100 Hz: 10 ms is exactly 441 ticks; 5 ms is 220.5 ticks and
		// must round up to 221. Endpoints are hand computed. ---
		f44 := &memFile{}
		m44 := NewClockMuxer(f44, []uint32{44100})
		c.Check(m44.WriteHeader([]av.CodecData{aac44}) == nil, "44.1k WriteHeader")
		type pkt44 struct {
			at   time.Duration
			tick int64
		}
		plan44 := []pkt44{{0, 0}, {5 * ms, 221}, {10 * ms, 441}}
		for j := 2; j <= 200; j++ {
			plan44 = append(plan44, pkt44{time.Duration(j) * 10 * ms, int64(j) * 441})
		}
		for i, p := range plan44 {
			err := m44.WritePacket(av.Packet{Idx: 0, Time: p.at, Data: []byte{byte(i), 0x44}})
			c.Check(err == nil, "44.1k write packet %d: %v", i, err)
		}
		// 2010 ms -> 88641 ticks; final sample therefore lasts 441 ticks.
		end44 := 2010 * ms
		c.Check(m44.WriteTrailerAt([]time.Duration{end44}) == nil, "44.1k trailer")

		boxes := map[string][]rawBox{}
		walkRawBoxes(f44.data, boxes)
		mdhd44 := boxes["mdhd"][0]
		ver, dur := headerVersionDuration(mdhd44)
		c.Check(ver == 0, "44.1k mdhd version 0, got %d", ver)
		c.Check(dur == 88641, "44.1k mdhd duration = 88641, got %d", dur)
		gotStts44 := rawSttsEntries(boxes["stts"][0])
		wantStts44 := [][2]uint32{{1, 221}, {1, 220}, {200, 441}}
		c.Check(len(gotStts44) == len(wantStts44), "44.1k stts run count %d", len(gotStts44))
		for i := range wantStts44 {
			c.Check(i < len(gotStts44) && gotStts44[i] == wantStts44[i],
				"44.1k stts[%d] = %v want %v", i, gotStts44, wantStts44)
		}
		mvVer, mvDur := headerVersionDuration(boxes["mvhd"][0])
		c.Check(mvVer == 0 && mvDur == 20100, "44.1k mvhd v0 dur 20100, got v%d d%d", mvVer, mvDur)
		tkVer, tkDur := headerVersionDuration(boxes["tkhd"][0])
		c.Check(tkVer == 0 && tkDur == 20100, "44.1k tkhd v0 dur 20100, got v%d d%d", tkVer, tkDur)

		// --- 48000 Hz: 1 ms is exactly 48 ticks; 31250 ns is 1.5 ticks and
		// must round up to 2. ---
		f48 := &memFile{}
		m48 := NewClockMuxer(f48, []uint32{48000})
		c.Check(m48.WriteHeader([]av.CodecData{aac48}) == nil, "48k WriteHeader")
		type pkt48 struct {
			at   time.Duration
			tick int64
		}
		plan48 := []pkt48{{0, 0}, {31250, 2}, {ms, 48}}
		for j := 2; j <= 1000; j++ {
			plan48 = append(plan48, pkt48{time.Duration(j) * ms, int64(j) * 48})
		}
		for i, p := range plan48 {
			err := m48.WritePacket(av.Packet{Idx: 0, Time: p.at, Data: []byte{byte(i), 0x48}})
			c.Check(err == nil, "48k write packet %d: %v", i, err)
		}
		c.Check(m48.WriteTrailerAt([]time.Duration{1001 * ms}) == nil, "48k trailer")
		boxes48 := map[string][]rawBox{}
		walkRawBoxes(f48.data, boxes48)
		ver48, dur48 := headerVersionDuration(boxes48["mdhd"][0])
		c.Check(ver48 == 0 && dur48 == 48048, "48k mdhd = 48048 got v%d d%d", ver48, dur48)
		gotStts48 := rawSttsEntries(boxes48["stts"][0])
		wantStts48 := [][2]uint32{{1, 2}, {1, 46}, {1000, 48}}
		c.Check(len(gotStts48) == len(wantStts48), "48k stts run count %d", len(gotStts48))
		for i := range wantStts48 {
			c.Check(i < len(gotStts48) && gotStts48[i] == wantStts48[i],
				"48k stts[%d] = %v want %v", i, gotStts48, wantStts48)
		}
		_, mv48 := headerVersionDuration(boxes48["mvhd"][0])
		c.Check(mv48 == 10010, "48k mvhd dur 10010 got %d", mv48)

		// --- Rejected packets are not consumed; legal packets still commit. ---
		fr := &memFile{}
		mr := NewClockMuxer(fr, []uint32{44100})
		c.Check(mr.WriteHeader([]av.CodecData{aac44}) == nil, "reject WriteHeader")
		sr := mr.streams[0]
		c.Check(errors.Is(mr.WritePacket(av.Packet{Idx: 0, Time: ms, Data: []byte{1}}), errClockBadFirstTime),
			"first packet must start at zero")
		c.Check(len(sr.sample.SampleSize.Entries) == 0, "rejected first packet consumed nothing")
		c.Check(mr.WritePacket(av.Packet{Idx: 0, Time: 0, Data: []byte{1}}) == nil, "zero packet accepted")
		// 1 ns quantizes to the same tick (0) as the previous endpoint.
		c.Check(errors.Is(mr.WritePacket(av.Packet{Idx: 0, Time: 1, Data: []byte{2}}), errClockSameTick),
			"same-tick packet rejected")
		c.Check(len(sr.sample.SampleSize.Entries) == 1, "same-tick rejection left one sample")
		c.Check(errors.Is(mr.WritePacket(av.Packet{Idx: 0, Time: 0, Data: []byte{3}}), errClockTimeNotStrict),
			"non-increasing time rejected")
		c.Check(len(sr.sample.SampleSize.Entries) == 1, "non-increasing rejection left one sample")
		c.Check(mr.WritePacket(av.Packet{Idx: 0, Time: 10 * ms, Data: []byte{4}}) == nil,
			"legal packet commits after rejections")
		c.Check(len(sr.sample.SampleSize.Entries) == 2, "legal packet committed after rejection")
		c.Check(mr.WriteTrailerAt([]time.Duration{20 * ms}) == nil, "reject-case trailer")

		// --- Interleaved A/V tracks on a shared 10 ms grid (441 ticks / 900 ticks). ---
		fi := &memFile{}
		mi := NewClockMuxer(fi, []uint32{44100, 90000})
		c.Check(mi.WriteHeader([]av.CodecData{aac44, h264}) == nil, "interleaved WriteHeader")
		// Submission order deliberately interleaves the two tracks on a
		// 10 ms grid (441 audio ticks / 900 video ticks per step).
		type job struct {
			idx int8
			at  time.Duration
			key bool
			ct  time.Duration
		}
		jobs := []job{
			{0, 0, false, 0}, {1, 0, true, 0},
			{1, 10 * ms, true, 0},
			{0, 20 * ms, false, 0},
			{1, 30 * ms, false, 10 * ms}, // composition offset stays in track ticks
			{0, 40 * ms, false, 0},
			{1, 50 * ms, true, 0},
		}
		for k, j := range jobs {
			err := mi.WritePacket(av.Packet{
				Idx: j.idx, Time: j.at, IsKeyFrame: j.key,
				CompositionTime: j.ct, Data: []byte{0xa0 + byte(k)},
			})
			c.Check(err == nil, "interleaved packet %d: %v", k, err)
		}
		c.Check(mi.WriteTrailerAt([]time.Duration{60 * ms, 60 * ms}) == nil, "interleaved trailer")
		ib := map[string][]rawBox{}
		walkRawBoxes(fi.data, ib)
		c.Check(len(ib["tkhd"]) == 2, "two tracks present, got %d", len(ib["tkhd"]))
		// Audio: endpoints 0,882,1764; end 2646 -> runs of 882, mdhd 2646.
		c.Check(rawSttsEntries(ib["stts"][0])[0] == [2]uint32{3, 882}, "audio stts 3x882: %v", rawSttsEntries(ib["stts"][0]))
		_, aMdhd := headerVersionDuration(ib["mdhd"][0])
		c.Check(aMdhd == 2646, "audio mdhd 2646 got %d", aMdhd)
		// Video: endpoints 0,900,2700,4500; end 5400 -> 900,1800,1800,900.
		c.Check(equalPairs(rawSttsEntries(ib["stts"][1]), [][2]uint32{{1, 900}, {2, 1800}, {1, 900}}),
			"video stts %v", rawSttsEntries(ib["stts"][1]))
		_, vMdhd := headerVersionDuration(ib["mdhd"][1])
		c.Check(vMdhd == 5400, "video mdhd 5400 got %d", vMdhd)
		// Key frames are samples 1, 2 and 4; the 30 ms packet is not a key frame.
		c.Check(equalU32(rawKeyframes(ib["stss"][0]), []uint32{1, 2, 4}), "stss keyframes %v", rawKeyframes(ib["stss"][0]))
		// Composition offset of 10 ms at 90 kHz is 900 ticks and is unchanged in meaning.
		ctts := ib["ctts"][0]
		count := binary.BigEndian.Uint32(ctts.payload[4:8])
		c.Check(count == 3, "video ctts run count %d", count)
		var saw900 bool
		for off, i := 8, uint32(0); i < count; i, off = i+1, off+8 {
			if binary.BigEndian.Uint32(ctts.payload[off+4:]) == 900 {
				saw900 = true
			}
		}
		c.Check(saw900, "ctts contains 900-tick composition offset")
		// Chunk offsets must follow the global submission order within each track.
		moov := mustParseMoov(t, fi.data)
		off0 := moov.Tracks[0].Media.Info.Sample.ChunkOffset.Entries
		off1 := moov.Tracks[1].Media.Info.Sample.ChunkOffset.Entries
		c.Check(len(off0) == 3 && len(off1) == 4, "chunk offset counts %d/%d", len(off0), len(off1))
		c.Check(off0[0] < off1[0] && off1[0] < off1[1] && off1[1] < off0[1] && off0[1] < off1[2],
			"interleaved chunk offsets preserve submission order: %v %v", off0, off1)

		// --- Overflow-safe conversion helpers. File-level samples cannot
		// exceed uint32 ticks each, so exercise the math/big fallback and the
		// int64 overflow error directly. ---
		// Half tick rounds up: 333333333 ns * 3 / 1e9 = 0.999999999 -> 1 tick.
		halfTick, herr := quantizeClockTicks(333333333, 3)
		c.Check(herr == nil && halfTick == 1, "half-tick rounds up to 1, got %d", halfTick)
		// One hour at 1 GHz ticks forces the big.Int path (ns*scale overflows
		// the uint64 fast path) and yields exactly 3.6e12 ticks.
		bigTick, berr := quantizeClockTicks(time.Hour, 1000000000)
		c.Check(berr == nil && bigTick == 3_600_000_000_000, "big-path 1h@1GHz ticks, got %d", bigTick)
		// mulDivFloor: ordinary floor and an overflowing product.
		small, merr := mulDivFloor(7, 3, 2)
		c.Check(merr == nil && small == 10, "mulDivFloor 7*3/2 = 10, got %d", small)
		wide, werr := mulDivFloor(1<<40, 1<<30, 1<<20)
		c.Check(werr == nil && wide == 1<<50, "mulDivFloor big path = 2^50, got %d", wide)
		// A movie duration whose scaled value exceeds uint64 is rejected.
		_, overflowErr := mulDivFloor(math.MaxInt64, 10000, 1)
		c.Check(errors.Is(overflowErr, errClockTicksOverflow), "mulDivFloor uint64 overflow reported")
	})
}

func rawKeyframes(b rawBox) []uint32 {
	count := binary.BigEndian.Uint32(b.payload[4:8])
	out := make([]uint32, 0, count)
	for i := uint32(0); i < count; i++ {
		out = append(out, binary.BigEndian.Uint32(b.payload[8+4*i:12+4*i]))
	}
	return out
}

func mustParseMoov(t *testing.T, data []byte) *mp4io.Movie {
	r := &memFile{data: data}
	atoms, err := mp4io.ReadFileAtoms(r)
	if err != nil {
		t.Fatalf("read atoms: %v", err)
	}
	for _, a := range atoms {
		if m, ok := a.(*mp4io.Movie); ok {
			return m
		}
	}
	t.Fatal("no moov")
	return nil
}

// TestClockLongHeaders drives a few widely spaced samples past the 32-bit
// duration boundary and independently exercises version 0/1 header boxes.
func TestClockLongHeaders(t *testing.T) {
	aac := mustAACCodec(t, []byte{0x12, 0x10})

	const (
		base32   = uint64(1) << 32
		tickP1   = int64(base32 - 50)    // 4294967246: fits uint32
		tickP2   = int64(base32*2 - 150) // 8589934446: mdhd is now 64-bit wide
		tickEnd  = tickP2 + 1000         // 8589935446
		movieEnd = uint64(tickEnd) * 10  // 85899354460 at the 10 kHz movie scale
	)

	taskcheck.Run(t, func(c *taskcheck.C) {
		f := &memFile{}
		m := NewClockMuxer(f, []uint32{1000})
		c.Check(m.WriteHeader([]av.CodecData{aac}) == nil, "long WriteHeader")
		s := m.streams[0]
		// A single gap wider than uint32 must be rejected without consuming
		// the packet.
		huge := time.Duration(base32+100) * time.Millisecond
		c.Check(m.WritePacket(av.Packet{Idx: 0, Time: 0, Data: []byte{0}}) == nil,
			"zero-time first packet accepted")
		c.Check(errors.Is(m.WritePacket(av.Packet{Idx: 0, Time: huge, Data: []byte{1}}), errClockDurationWide),
			"over-wide sample duration rejected")
		c.Check(len(s.sample.SampleSize.Entries) == 1, "rejected wide packet not consumed")
		// Legal wide-but-fitting spacing.
		c.Check(m.WritePacket(av.Packet{Idx: 0, Time: time.Duration(tickP1) * time.Millisecond, Data: []byte{2}}) == nil,
			"packet at 2^32-50 accepted")
		c.Check(m.WritePacket(av.Packet{Idx: 0, Time: time.Duration(tickP2) * time.Millisecond, Data: []byte{3}}) == nil,
			"packet beyond 2^32 accepted")
		c.Check(m.WriteTrailerAt([]time.Duration{time.Duration(tickEnd) * time.Millisecond}) == nil,
			"long trailer")

		boxes := map[string][]rawBox{}
		walkRawBoxes(f.data, boxes)
		mvVer, mvDur := headerVersionDuration(boxes["mvhd"][0])
		c.Check(mvVer == 1, "mvhd selected version 1, got %d", mvVer)
		c.Check(mvDur == movieEnd, "mvhd duration %d want %d", mvDur, movieEnd)
		tkVer, tkDur := headerVersionDuration(boxes["tkhd"][0])
		c.Check(tkVer == 1 && tkDur == movieEnd, "tkhd v1 duration %d", tkDur)
		mdVer, mdDur := headerVersionDuration(boxes["mdhd"][0])
		c.Check(mdVer == 1 && mdDur == uint64(tickEnd), "mdhd v1 duration %d", mdDur)
		wantStts := [][2]uint32{{1, uint32(tickP1)}, {1, uint32(tickP2 - tickP1)}, {1, 1000}}
		c.Check(equalPairs(rawSttsEntries(boxes["stts"][0]), wantStts), "long stts %v", rawSttsEntries(boxes["stts"][0]))

		// Cross-check the same values through mp4io unmarshal as well.
		moov := mustParseMoov(t, f.data)
		c.Check(moov.Header.Version == 1 && moov.Header.Duration == movieEnd, "parsed mvhd v1")
		c.Check(moov.Tracks[0].Header.Version == 1 && moov.Tracks[0].Header.Duration == movieEnd, "parsed tkhd v1")
		c.Check(moov.Tracks[0].Media.Header.Version == 1 && moov.Tracks[0].Media.Header.Duration == uint64(tickEnd),
			"parsed mdhd v1")
		c.Check(sttsTotal(moov.Tracks[0].Media.Info.Sample.TimeToSample.Entries) == uint64(tickEnd),
			"parsed stts total equals mdhd duration")

		// Mixed versions: at a 1 MHz track scale the mdhd duration passes 2^32
		// but the floor-converted 10 kHz movie duration stays under it, so
		// mdhd must be v1 while tkhd/mvhd stay v0 independently.
		fmix := &memFile{}
		mix := NewClockMuxer(fmix, []uint32{1000000})
		c.Check(mix.WriteHeader([]av.CodecData{aac}) == nil, "mixed WriteHeader")
		const (
			mixP    = int64(base32 - 100)  // 4294967196: fits uint32 as a duration
			mixEnd  = int64(base32 + 1e6)  // 4295967296: mdhd exceeds 2^32
			mixMovD = uint64(mixEnd) / 100 // floor at 10 kHz = 42959672 (< 2^32)
		)
		c.Check(mix.WritePacket(av.Packet{Idx: 0, Time: 0, Data: []byte{0}}) == nil, "mixed p0")
		c.Check(mix.WritePacket(av.Packet{Idx: 0, Time: time.Duration(mixP) * 1000, Data: []byte{1}}) == nil,
			"mixed p1")
		c.Check(mix.WriteTrailerAt([]time.Duration{time.Duration(mixEnd) * 1000}) == nil, "mixed trailer")
		mb := map[string][]rawBox{}
		walkRawBoxes(fmix.data, mb)
		mdMixedVer, mdMixedDur := headerVersionDuration(mb["mdhd"][0])
		c.Check(mdMixedVer == 1 && mdMixedDur == uint64(mixEnd), "mixed mdhd v1 dur %d", mdMixedDur)
		tkMixedVer, tkMixedDur := headerVersionDuration(mb["tkhd"][0])
		c.Check(tkMixedVer == 0 && tkMixedDur == mixMovD, "mixed tkhd v0 dur %d", tkMixedDur)
		mvMixedVer, mvMixedDur := headerVersionDuration(mb["mvhd"][0])
		c.Check(mvMixedVer == 0 && mvMixedDur == mixMovD, "mixed mvhd v0 dur %d", mvMixedDur)
		c.Check(equalPairs(rawSttsEntries(mb["stts"][0]), [][2]uint32{{1, uint32(mixP)}, {1, uint32(mixEnd - mixP)}}),
			"mixed stts %v", rawSttsEntries(mb["stts"][0]))

		// Trailer retry: a bad end time is rejected first, then the corrected
		// call finalizes the file; the failed attempt changed nothing.
		fr := &memFile{}
		mr := NewClockMuxer(fr, []uint32{1000, 44100})
		c.Check(mr.WriteHeader([]av.CodecData{aac, aac}) == nil, "retry WriteHeader")
		c.Check(mr.WritePacket(av.Packet{Idx: 0, Time: 0, Data: []byte{1}}) == nil, "retry p0")
		c.Check(mr.WritePacket(av.Packet{Idx: 0, Time: ms, Data: []byte{2}}) == nil, "retry p1")
		badEnds := []time.Duration{ms, 5 * ms} // empty track 1 with non-zero end
		c.Check(mr.WriteTrailerAt(badEnds) != nil, "empty track rejects non-zero end")
		c.Check(len(mr.streams[0].sample.TimeToSample.Entries) == 0, "failed trailer flushed no stts")
		c.Check(mr.WriteTrailerAt([]time.Duration{2 * ms, 0}) == nil, "corrected trailer succeeds")
		rb := map[string][]rawBox{}
		walkRawBoxes(fr.data, rb)
		_, emptyMdhd := headerVersionDuration(rb["mdhd"][1])
		c.Check(emptyMdhd == 0, "empty track mdhd duration zero, got %d", emptyMdhd)

		// Independently built version 0/1 header boxes: full marshal/Len and
		// unmarshal round trips plus truncation/version rejection.
		bigTime := time.Date(1904, time.January, 1, 0, 0, 0, 0, time.UTC).
			Add(time.Duration(base32*2) * time.Second)
		headers := []struct {
			name string
			make func(ver uint8, dur uint64) mp4io.Atom
		}{
			{"mvhd", func(ver uint8, dur uint64) mp4io.Atom {
				return &mp4io.MovieHeader{
					Version: ver, Duration: dur, TimeScale: 10000,
					CreateTime: bigTime, ModifyTime: bigTime,
					Matrix: [9]int32{0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000},
				}
			}},
			{"tkhd", func(ver uint8, dur uint64) mp4io.Atom {
				return &mp4io.TrackHeader{
					Version: ver, Duration: dur, TrackId: 1,
					CreateTime: bigTime, ModifyTime: bigTime,
					Matrix: [9]int32{0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000},
				}
			}},
			{"mdhd", func(ver uint8, dur uint64) mp4io.Atom {
				return &mp4io.MediaHeader{
					Version: ver, Duration: dur, TimeScale: 1000,
					CreateTime: bigTime, ModifyTime: bigTime,
				}
			}},
		}
		for _, h := range headers {
			v1 := h.make(1, base32+77)
			buf := make([]byte, v1.Len())
			n := v1.Marshal(buf)
			c.Check(n == len(buf), "%s Marshal length equals Len", h.name)
			rbx := rawBox{tag: h.name, payload: buf[8:]}
			gotVer, gotDur := headerVersionDuration(rbx)
			c.Check(gotVer == 1 && gotDur == base32+77, "%s raw v1 duration %d", h.name, gotDur)

			v0 := h.make(0, 1234)
			c.Check(v0.Len() == v1.Len()-12, "%s v1 is 12 bytes wider than v0 (%d vs %d)",
				h.name, v1.Len(), v0.Len())
			buf0 := make([]byte, v0.Len())
			v0.Marshal(buf0)
			gotVer0, gotDur0 := headerVersionDuration(rawBox{tag: h.name, payload: buf0[8:]})
			c.Check(gotVer0 == 0 && gotDur0 == 1234, "%s raw v0 duration", h.name)

			// Truncated fields must error.
			target := cloneAtom(v1)
			_, uerr := target.Unmarshal(buf[:len(buf)-1], 0)
			c.Check(uerr != nil, "%s truncated box errors", h.name)
			// Unsupported versions must error.
			bad := make([]byte, len(buf))
			copy(bad, buf)
			bad[8] = 2 // version byte sits right after the size/tag header
			_, verr := cloneAtom(v1).Unmarshal(bad, 0)
			c.Check(verr != nil, "%s version 2 errors", h.name)
		}

		// Times beyond 32 bits survive a version 1 unmarshal round trip intact.
		mv := headers[0].make(1, base32+77).(*mp4io.MovieHeader)
		buf := make([]byte, mv.Len())
		mv.Marshal(buf)
		parsed := &mp4io.MovieHeader{}
		_, perr := parsed.Unmarshal(buf, 0)
		c.Check(perr == nil, "mvhd unmarshal ok")
		c.Check(parsed.CreateTime.Equal(bigTime) && parsed.ModifyTime.Equal(bigTime),
			"64-bit create/modify times preserved")
	})
}

func cloneAtom(a mp4io.Atom) mp4io.Atom {
	switch v := a.(type) {
	case *mp4io.MovieHeader:
		clone := *v
		return &clone
	case *mp4io.TrackHeader:
		clone := *v
		return &clone
	case *mp4io.MediaHeader:
		clone := *v
		return &clone
	}
	return a
}

// TestClockReadback writes clock files and reads them back, including times
// beyond 32-bit ticks, round-trip seeks and a legacy short-file control.
func TestClockReadback(t *testing.T) {
	aac := mustAACCodec(t, []byte{0x12, 0x10})

	const (
		base32  = uint64(1) << 32
		tickP1  = int64(base32 - 50)
		tickP2  = int64(base32*2 - 150)
		tickEnd = tickP2 + 1000
	)

	taskcheck.Run(t, func(c *taskcheck.C) {
		// --- Long file: every endpoint is an exact microsecond, so readback
		// times are exact tick*1e6 ns values. ---
		f := &memFile{}
		m := NewClockMuxer(f, []uint32{1000})
		c.Check(m.WriteHeader([]av.CodecData{aac}) == nil, "readback WriteHeader")
		inputs := []struct {
			at   time.Duration
			tick int64
			data byte
		}{
			{0, 0, 1},
			{time.Duration(tickP1) * time.Millisecond, tickP1, 2},
			{time.Duration(tickP2) * time.Millisecond, tickP2, 3},
		}
		for _, in := range inputs {
			c.Check(m.WritePacket(av.Packet{Idx: 0, Time: in.at, Data: []byte{in.data}}) == nil,
				"readback write %d", in.data)
		}
		c.Check(m.WriteTrailerAt([]time.Duration{time.Duration(tickEnd) * time.Millisecond}) == nil,
			"readback trailer")

		d := NewDemuxer(&memFile{data: f.data})
		streams, err := d.Streams()
		c.Check(err == nil && len(streams) == 1, "demuxer Streams: %v", err)
		// ReadPacket advances the demux position, so CurrentTime reports the
		// boundary following the sample just returned (the original behavior).
		nextBoundaries := []int64{tickP1, tickP2, tickEnd}
		gotPackets := 0
		for {
			pkt, perr := d.ReadPacket()
			if perr == io.EOF {
				break
			}
			c.Check(perr == nil, "ReadPacket error %v", perr)
			want := inputs[gotPackets]
			c.Check(pkt.Data[0] == want.data, "payload %d preserved", gotPackets)
			wantTime := ticksToTime(uint64(want.tick), 1000)
			c.Check(pkt.Time == wantTime, "packet %d time %v want %v (no 32-bit wrap)",
				gotPackets, pkt.Time, wantTime)
			wantCurrent := ticksToTime(uint64(nextBoundaries[gotPackets]), 1000)
			c.Check(d.CurrentTime() == wantCurrent, "CurrentTime %v want %v (no wrap)",
				d.CurrentTime(), wantCurrent)
			gotPackets++
		}
		c.Check(gotPackets == 3, "all three packets read back, got %d", gotPackets)

		// Seek beyond the 32-bit boundary: target lands inside sample 1.
		target := ticksToTime(uint64(tickP1)+10, 1000)
		c.Check(d.SeekToTime(target) == nil, "SeekToTime past 2^32")
		// Immediately after the seek the position (dts) is sample 1's start.
		c.Check(d.CurrentTime() == ticksToTime(uint64(tickP1), 1000),
			"post-seek CurrentTime %v no wrap", d.CurrentTime())
		pkt, perr := d.ReadPacket()
		c.Check(perr == nil, "post-seek ReadPacket: %v", perr)
		c.Check(pkt.Time == ticksToTime(uint64(tickP1), 1000),
			"seek landed on sample 1 at %v, got %v", ticksToTime(uint64(tickP1), 1000), pkt.Time)
		// Seeking before the first sample clamps to the first sample.
		c.Check(d.SeekToTime(0) == nil, "SeekToTime zero")
		pkt, perr = d.ReadPacket()
		c.Check(perr == nil && pkt.Time == 0, "seek zero returns first sample")

		// --- Interleaved file readback: demuxer merges tracks on absolute time. ---
		fi := &memFile{}
		mi := NewClockMuxer(fi, []uint32{44100, 90000})
		h264 := mustH264Codec(t)
		c.Check(mi.WriteHeader([]av.CodecData{aac, h264}) == nil, "interleaved readback header")
		inter := []av.Packet{
			{Idx: 0, Time: 0, Data: []byte{0xa0}},
			{Idx: 1, Time: 0, IsKeyFrame: true, Data: []byte{0xb0}},
			{Idx: 1, Time: 10 * ms, IsKeyFrame: true, Data: []byte{0xb1}},
			{Idx: 0, Time: 20 * ms, Data: []byte{0xa1}},
			{Idx: 1, Time: 30 * ms, Data: []byte{0xb2}},
			{Idx: 0, Time: 40 * ms, Data: []byte{0xa2}},
			{Idx: 1, Time: 50 * ms, IsKeyFrame: true, Data: []byte{0xb3}},
		}
		for i := range inter {
			c.Check(mi.WritePacket(inter[i]) == nil, "inter write %d", i)
		}
		c.Check(mi.WriteTrailerAt([]time.Duration{60 * ms, 60 * ms}) == nil, "inter trailer")
		di := NewDemuxer(&memFile{data: fi.data})
		_, err = di.Streams()
		c.Check(err == nil, "inter Streams %v", err)
		wantOrder := []byte{0xa0, 0xb0, 0xb1, 0xa1, 0xb2, 0xa2, 0xb3}
		for i, wantByte := range wantOrder {
			p, perr := di.ReadPacket()
			c.Check(perr == nil, "inter ReadPacket %d: %v", i, perr)
			c.Check(p.Data[0] == wantByte, "inter order %d got %x want %x", i, p.Data[0], wantByte)
		}
		// Key frame rule is unchanged: a seek past the 30 ms non-key sample
		// rounds the video track back to its preceding sync sample (10 ms,
		// video sample index 1), and the audio track follows that time.
		c.Check(di.SeekToTime(35*ms) == nil, "video seek")
		video := di.streams[1]
		audio := di.streams[0]
		c.Check(video.sampleIndex == 1, "video seeked to sync sample index 1, got %d", video.sampleIndex)
		c.Check(video.tsToTime(video.dts) == 10*ms, "video dts at 10ms, got %v", video.tsToTime(video.dts))
		c.Check(video.sample.SyncSample.Entries[video.syncSampleIndex] == 2,
			"selected sync sample entry is 2 (index 1)")
		c.Check(audio.tsToTime(audio.dts) == 0, "audio clamped to first sample, got %v", audio.tsToTime(audio.dts))
		// Reading on from the seek position yields the 10 ms key frame next
		// on the video track, in absolute-time merge order.
		var sawVideo10ms bool
		for k := 0; k < 3; k++ {
			p, perr := di.ReadPacket()
			c.Check(perr == nil, "post-seek read %d: %v", k, perr)
			if p.Idx == 1 && p.IsKeyFrame && p.Time == 10*ms {
				sawVideo10ms = true
			}
		}
		c.Check(sawVideo10ms, "reached the 10ms video key frame after seek")

		// --- Mode cross-talk is rejected explicitly. ---
		clockMux := NewClockMuxer(&memFile{}, []uint32{44100})
		c.Check(clockMux.WriteHeader([]av.CodecData{aac}) == nil, "clock header")
		c.Check(clockMux.WriteTrailer() != nil, "clock muxer rejects legacy WriteTrailer")
		legacyMux := NewMuxer(&memFile{})
		c.Check(legacyMux.WriteHeader([]av.CodecData{aac}) == nil, "legacy header")
		c.Check(legacyMux.WriteTrailerAt([]time.Duration{ms}) != nil, "legacy muxer rejects WriteTrailerAt")

		// --- Header validation happens before any byte hits the writer. ---
		empty := &memFile{}
		bad := NewClockMuxer(empty, []uint32{44100, 90000})
		c.Check(bad.WriteHeader([]av.CodecData{aac}) != nil, "track count mismatch rejected")
		c.Check(len(empty.data) == 0, "no header byte written on count mismatch")
		for _, scale := range []uint32{0, 1000000001} {
			e := &memFile{}
			bm := NewClockMuxer(e, []uint32{scale})
			c.Check(bm.WriteHeader([]av.CodecData{aac}) != nil, "timescale %d rejected", scale)
			c.Check(len(e.data) == 0, "no header byte for timescale %d", scale)
		}
		for _, scale := range []uint32{1, 1000000000} {
			e := &memFile{}
			okm := NewClockMuxer(e, []uint32{scale})
			c.Check(okm.WriteHeader([]av.CodecData{aac}) == nil, "boundary timescale %d accepted", scale)
			c.Check(okm.WritePacket(av.Packet{Idx: 0, Time: 0, Data: []byte{9}}) == nil, "boundary packet")
			c.Check(okm.WriteTrailerAt([]time.Duration{time.Second}) == nil, "boundary trailer")
		}

		// --- Legacy short file control: version 0, 32-bit durations, same
		// 101-packet layout the baseline test exercises. ---
		lf := &memFile{}
		lm := NewMuxer(lf)
		c.Check(lm.WriteHeader([]av.CodecData{aac}) == nil, "legacy WriteHeader")
		for i := 0; i < 101; i++ {
			err := lm.WritePacket(av.Packet{
				Time: time.Duration(int64(i) * 1024 * 1000000000 / 44100),
				Data: []byte{byte(i), 0x55},
			})
			c.Check(err == nil, "legacy packet %d: %v", i, err)
		}
		c.Check(lm.WriteTrailer() == nil, "legacy trailer")
		lb := map[string][]rawBox{}
		walkRawBoxes(lf.data, lb)
		for _, tag := range []string{"mvhd", "tkhd", "mdhd"} {
			ver, d := headerVersionDuration(lb[tag][0])
			c.Check(ver == 0 && d <= uint64(^uint32(0)), "legacy %s stays v0 32-bit (v%d d%d)", tag, ver, d)
		}
		dl := NewDemuxer(&memFile{data: lf.data})
		_, err = dl.Streams()
		c.Check(err == nil, "legacy Streams %v", err)
		n := 0
		for {
			_, perr := dl.ReadPacket()
			if perr == io.EOF {
				break
			}
			c.Check(perr == nil, "legacy ReadPacket %v", perr)
			n++
		}
		c.Check(n == 101, "legacy readback count 101 got %d", n)
	})
}
