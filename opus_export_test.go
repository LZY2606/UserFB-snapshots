package mp4

import (
	"bytes"
	"encoding/binary"
	"errors"
	"io"
	"testing"

	"github.com/abema/go-mp4/task_support/taskcheck"
)

// This file builds MP4 inputs with the library's own box writer and reads
// the Ogg output back with an independent, self-contained page parser and
// CRC implementation. The exporter is never used as a reference.

// ---------------------------------------------------------------------------
// Independent Ogg parsing (RFC 3533 / RFC 7845), written locally on purpose.
// ---------------------------------------------------------------------------

var oggCRCTable [256]uint32

func init() {
	for i := 0; i < 256; i++ {
		r := uint32(i) << 24
		for j := 0; j < 8; j++ {
			if r&0x80000000 != 0 {
				r = (r << 1) ^ 0x04c11db7
			} else {
				r <<= 1
			}
		}
		oggCRCTable[i] = r
	}
}

func oggCRC(crc uint32, p []byte) uint32 {
	for _, b := range p {
		crc = (crc << 8) ^ oggCRCTable[byte(crc>>24)^b]
	}
	return crc
}

type parsedPage struct {
	flags    byte
	granule  uint64
	serial   uint32
	seq      uint32
	crc      uint32
	segments []byte
	packet   []byte
	raw      []byte
}

func parseOggPages(t *testing.T, data []byte) []parsedPage {
	t.Helper()
	var pages []parsedPage
	pos := 0
	for pos < len(data) {
		if len(data)-pos < 27 {
			t.Fatalf("truncated Ogg page header at %d", pos)
		}
		hdr := data[pos : pos+27]
		if string(hdr[0:4]) != "OggS" {
			t.Fatalf("bad capture pattern at %d", pos)
		}
		if hdr[4] != 0 {
			t.Fatalf("unsupported Ogg page version %d", hdr[4])
		}
		page := parsedPage{
			flags:   hdr[5],
			granule: binary.LittleEndian.Uint64(hdr[6:14]),
			serial:  binary.LittleEndian.Uint32(hdr[14:18]),
			seq:     binary.LittleEndian.Uint32(hdr[18:22]),
			crc:     binary.LittleEndian.Uint32(hdr[22:26]),
		}
		segCount := int(hdr[26])
		pageStart := pos
		pos += 27
		if len(data)-pos < segCount {
			t.Fatalf("truncated segment table")
		}
		page.segments = append([]byte(nil), data[pos:pos+segCount]...)
		pos += segCount
		size := 0
		for _, v := range page.segments {
			size += int(v)
		}
		if len(data)-pos < size {
			t.Fatalf("truncated packet payload")
		}
		page.packet = append([]byte(nil), data[pos:pos+size]...)
		pos += size
		page.raw = data[pageStart:pos]

		// Independently recompute the CRC with the checksum field zeroed.
		calc := make([]byte, len(page.raw))
		copy(calc, page.raw)
		binary.LittleEndian.PutUint32(calc[22:26], 0)
		got := oggCRC(0, calc)
		if got != page.crc {
			t.Fatalf("page %d CRC mismatch: header=%08x recomputed=%08x", page.seq, page.crc, got)
		}
		pages = append(pages, page)
	}
	return pages
}

// refTOCFrames mirrors the RFC 6716 TOC duration rules independently of the
// exporter implementation.
func refTOCFrames(packet []byte) uint32 {
	frameDur := [4]uint32{480, 960, 1920, 2880}
	toc := packet[0]
	cfg := toc >> 3
	var dur uint32
	switch {
	case cfg <= 11:
		dur = frameDur[cfg%4]
	case cfg <= 15:
		dur = [2]uint32{480, 960}[cfg%2]
	default:
		dur = frameDur[cfg%4]
	}
	var frames uint32
	switch toc & 3 {
	case 0:
		frames = 1
	case 1, 2:
		frames = 2
	case 3:
		frames = uint32(packet[1] & 0x3f)
	}
	return frames * dur
}

// ---------------------------------------------------------------------------
// MP4 fixture builder.
// ---------------------------------------------------------------------------

type exportFixture struct {
	movieTS     uint32
	trackID     uint32
	preSkip     uint16
	inputRate   uint32
	gain        int16
	mediaTime   int64
	segDurMovie uint64
	elstV1      bool
	mdhdDur     uint64 // zero means "use stts total"
	packets     [][]byte
	deltas      []uint32
	chunkOf     []int // chunk index per packet, decode order
	mdatOfChunk []int // mdat index per chunk
	gapBefore   map[int]int
	useCo64     bool
	uniformSize bool
	extraTrack  bool

	// error-injection knobs
	fragmentedTop  bool
	addMvex        bool
	addCtts        bool
	addSinf        bool
	stsdEntries    int
	dopsVersion    uint8
	dopsFamily     uint8
	dopsChannels   uint8
	mdhdTS         uint32
	entryChannels  uint16
	doubleElst     bool
	emptyElst      bool
	rateInteger    int16
	badChunkOffset int // chunk index whose offset is corrupted, -1 disables
	stszCountOver  int // nonzero overrides stsz sample_count
	code3Zero      int // packet index whose code-3 count byte is forced to zero
	code3Missing   int // packet index shortened to its TOC byte only
	badTOC         int // packet index given an over-long code-3 TOC
}

func defaultFixture() *exportFixture {
	packets := [][]byte{
		{0x00, 0xAA, 0xBB},                       // cfg0 code0: 480
		{0x41, 0x11},                             // cfg8 code1: 2*480 = 960
		append([]byte{0x03, 0x03}, make253()...), // cfg0 code3 count3: 1440, 255 bytes
		{0x18, 0xCC, 0xDD},                       // cfg3 code0: 2880
		{0x40, 0x55},                             // cfg8 code0: 480
	}
	return &exportFixture{
		movieTS:        1000,
		trackID:        7,
		preSkip:        312, // deliberately different from S
		inputRate:      44100,
		gain:           -256,
		mediaTime:      480,
		segDurMovie:    100, // 100ms @1000 = 4800 frames -> E = 5280
		packets:        packets,
		deltas:         []uint32{480, 960, 1440, 2880, 480},
		chunkOf:        []int{0, 1, 1, 2, 2},
		mdatOfChunk:    []int{0, 1, 0},
		gapBefore:      map[int]int{2: 12},
		stsdEntries:    1,
		dopsVersion:    0,
		dopsFamily:     0,
		dopsChannels:   1,
		entryChannels:  1,
		rateInteger:    1,
		mdhdTS:         48000,
		badChunkOffset: -1,
		code3Zero:      -1,
		code3Missing:   -1,
		badTOC:         -1,
	}
}

func make253() []byte {
	b := make([]byte, 253)
	for i := range b {
		b[i] = byte(0x40 + i%64)
	}
	return b
}

type chunkRecord struct {
	offset uint64
}

func startBox(t *testing.T, w *Writer, typ BoxType) *BoxInfo {
	t.Helper()
	bi, err := w.StartBox(&BoxInfo{Type: typ})
	if err != nil {
		t.Fatal(err)
	}
	return bi
}

func endBox(t *testing.T, w *Writer) {
	t.Helper()
	if _, err := w.EndBox(); err != nil {
		t.Fatal(err)
	}
}

func emitBox(t *testing.T, w *Writer, b IImmutableBox) {
	t.Helper()
	startBox(t, w, b.GetType())
	if _, err := Marshal(w, b, Context{}); err != nil {
		t.Fatalf("marshal %s: %v", b.GetType(), err)
	}
	endBox(t, w)
}

func endContainers(t *testing.T, w *Writer, n int) {
	t.Helper()
	for i := 0; i < n; i++ {
		endBox(t, w)
	}
}

// buildMP4 serializes the fixture and returns the complete MP4 bytes.
func buildMP4(t *testing.T, f *exportFixture) []byte {
	t.Helper()

	// Apply packet-level corruptions on private copies.
	packets := make([][]byte, len(f.packets))
	for i, p := range f.packets {
		packets[i] = append([]byte(nil), p...)
	}
	if f.code3Zero >= 0 {
		packets[f.code3Zero][1] = 0x00
	}
	if f.code3Missing >= 0 {
		packets[f.code3Missing] = packets[f.code3Missing][:1]
	}
	if f.badTOC >= 0 {
		packets[f.badTOC] = []byte{0x1B, 0x07} // cfg3 code3, 7 frames -> 20160 > 5760
	}

	chunkCount := 0
	for _, c := range f.chunkOf {
		if c+1 > chunkCount {
			chunkCount = c + 1
		}
	}
	mdatCount := 0
	for _, m := range f.mdatOfChunk {
		if m+1 > mdatCount {
			mdatCount = m + 1
		}
	}
	chunkPackets := make([][]int, chunkCount)
	for pi, ci := range f.chunkOf {
		chunkPackets[ci] = append(chunkPackets[ci], pi)
	}

	ws := newTestWriteSeeker()
	w := NewWriter(ws)

	emitBox(t, w, &Ftyp{MajorBrand: BrandISOM(), CompatibleBrands: []CompatibleBrandElem{{CompatibleBrand: BrandISOM()}}})

	if f.fragmentedTop {
		emitBox(t, w, &Styp{MajorBrand: BrandISOM()})
	}

	// mdats (written before moov; physical order differs from decode order).
	chunks := make([]chunkRecord, chunkCount)
	for m := 0; m < mdatCount; m++ {
		startBox(t, w, BoxTypeMdat())
		if f.extraTrack && m == 0 {
			if _, err := w.Write(make([]byte, 8)); err != nil { // other-track bytes
				t.Fatal(err)
			}
		}
		for ci := 0; ci < chunkCount; ci++ {
			if f.mdatOfChunk[ci] != m {
				continue
			}
			if gap := f.gapBefore[ci]; gap > 0 {
				if _, err := w.Write(make([]byte, gap)); err != nil {
					t.Fatal(err)
				}
			}
			off, err := w.Seek(0, io.SeekCurrent)
			if err != nil {
				t.Fatal(err)
			}
			chunks[ci].offset = uint64(off)
			for _, pi := range chunkPackets[ci] {
				if _, err := w.Write(packets[pi]); err != nil {
					t.Fatal(err)
				}
			}
		}
		endBox(t, w)
	}

	// moov
	startBox(t, w, BoxTypeMoov())
	emitBox(t, w, &Mvhd{Timescale: f.movieTS, DurationV0: uint32(f.segDurMovie), NextTrackID: 9})
	if f.addMvex {
		emitBox(t, w, &Mvex{})
	}

	// selected trak
	startBox(t, w, BoxTypeTrak())
	emitBox(t, w, &Tkhd{TrackID: f.trackID, DurationV0: uint32(f.segDurMovie)})

	// edts/elst
	startBox(t, w, BoxTypeEdts())
	emitBox(t, w, buildElst(f))
	endBox(t, w)

	startBox(t, w, BoxTypeMdia())
	emitBox(t, w, &Mdhd{Timescale: f.mdhdTS, DurationV0: uint32(mediaDuration(f)), Language: [3]byte{'u', 'n', 'd'}})
	emitBox(t, w, &Hdlr{HandlerType: [4]byte{'s', 'o', 'u', 'n'}, Name: "soun"})
	startBox(t, w, BoxTypeMinf())
	emitBox(t, w, &Smhd{})
	startBox(t, w, BoxTypeStbl())

	// stsd
	startBox(t, w, BoxTypeStsd())
	if _, err := Marshal(w, &Stsd{EntryCount: uint32(f.stsdEntries)}, Context{}); err != nil {
		t.Fatal(err)
	}
	for e := 0; e < f.stsdEntries; e++ {
		ase := &AudioSampleEntry{
			SampleEntry:  SampleEntry{DataReferenceIndex: 1},
			ChannelCount: f.entryChannels,
			SampleSize:   16,
			SampleRate:   48000 << 16,
		}
		ase.SetType(BoxTypeOpus())
		startBox(t, w, BoxTypeOpus())
		if _, err := Marshal(w, ase, Context{}); err != nil {
			t.Fatal(err)
		}
		dops := &DOps{
			Version:              f.dopsVersion,
			OutputChannelCount:   f.dopsChannels,
			PreSkip:              f.preSkip,
			InputSampleRate:      f.inputRate,
			OutputGain:           f.gain,
			ChannelMappingFamily: f.dopsFamily,
		}
		if f.dopsFamily != 0 {
			dops.StreamCount = 1
			dops.ChannelMapping = make([]byte, f.dopsChannels)
		}
		emitBox(t, w, dops)
		if f.addSinf {
			emitBox(t, w, &Sinf{})
		}
		endBox(t, w) // Opus
	}
	endBox(t, w) // stsd

	emitBox(t, w, buildStts(f.deltas))

	// stsc: one explicit entry per chunk (varying samples per chunk).
	stscEntries := make([]StscEntry, chunkCount)
	for ci, ps := range chunkPackets {
		stscEntries[ci] = StscEntry{FirstChunk: uint32(ci + 1), SamplesPerChunk: uint32(len(ps)), SampleDescriptionIndex: 1}
	}
	emitBox(t, w, &Stsc{EntryCount: uint32(chunkCount), Entries: stscEntries})

	emitBox(t, w, buildStsz(f, packets))

	if f.addCtts {
		emitBox(t, w, &Ctts{EntryCount: 0})
	}

	// stco/co64
	if f.useCo64 {
		offsets := make([]uint64, chunkCount)
		for ci := range offsets {
			offsets[ci] = corruptedOffset(f, chunks[ci].offset, ci)
		}
		emitBox(t, w, &Co64{EntryCount: uint32(chunkCount), ChunkOffset: offsets})
	} else {
		offsets := make([]uint32, chunkCount)
		for ci := range offsets {
			off := corruptedOffset(f, chunks[ci].offset, ci)
			if off > 0xffffffff {
				t.Fatalf("test offset overflow for stco")
			}
			offsets[ci] = uint32(off)
		}
		emitBox(t, w, &Stco{EntryCount: uint32(chunkCount), ChunkOffset: offsets})
	}

	endContainers(t, w, 4) // stbl, minf, mdia, trak

	if f.extraTrack {
		buildDummyVideoTrak(t, w, chunks[0].offset)
	}

	endBox(t, w) // moov
	return ws.Bytes()
}

func corruptedOffset(f *exportFixture, off uint64, ci int) uint64 {
	if f.badChunkOffset == ci {
		return off + 1_000_000
	}
	return off
}

func mediaDuration(f *exportFixture) uint64 {
	if f.mdhdDur != 0 {
		return f.mdhdDur
	}
	var total uint64
	for _, d := range f.deltas {
		total += uint64(d)
	}
	return total
}

func buildElst(f *exportFixture) *Elst {
	rate := f.rateInteger
	if rate == 0 {
		rate = 1
	}
	build := func(segV0 uint32, segV1 uint64, mtV0 int32, mtV1 int64) ElstEntry {
		return ElstEntry{SegmentDurationV0: segV0, SegmentDurationV1: segV1, MediaTimeV0: mtV0, MediaTimeV1: mtV1, MediaRateInteger: rate}
	}
	var entries []ElstEntry
	entries = append(entries, build(uint32(f.segDurMovie), f.segDurMovie, int32(f.mediaTime), f.mediaTime))
	if f.doubleElst {
		entries = append(entries, build(1, 1, 0, 0))
	}
	if f.emptyElst {
		entries[0].MediaTimeV0 = -1
		entries[0].MediaTimeV1 = -1
	}
	elst := &Elst{EntryCount: uint32(len(entries)), Entries: entries}
	if f.elstV1 {
		elst.SetVersion(1)
	}
	return elst
}

func buildStts(deltas []uint32) *Stts {
	type run struct {
		count uint32
		delta uint32
	}
	var runs []run
	for _, d := range deltas {
		if len(runs) > 0 && runs[len(runs)-1].delta == d {
			runs[len(runs)-1].count++
		} else {
			runs = append(runs, run{1, d})
		}
	}
	entries := make([]SttsEntry, len(runs))
	for i, r := range runs {
		entries[i] = SttsEntry{SampleCount: r.count, SampleDelta: r.delta}
	}
	return &Stts{EntryCount: uint32(len(entries)), Entries: entries}
}

func buildStsz(f *exportFixture, packets [][]byte) *Stsz {
	if f.uniformSize {
		size := uint32(len(packets[0]))
		for _, p := range packets {
			if uint32(len(p)) != size {
				size = 0 // uniform fixtures must use equal sizes
			}
		}
		stsz := &Stsz{SampleSize: size, SampleCount: uint32(len(packets))}
		if f.stszCountOver != 0 {
			stsz.SampleCount = uint32(f.stszCountOver)
		}
		return stsz
	}
	sizes := make([]uint32, len(packets))
	for i, p := range packets {
		sizes[i] = uint32(len(p))
	}
	stsz := &Stsz{SampleCount: uint32(len(packets)), EntrySize: sizes}
	if f.stszCountOver != 0 {
		stsz.SampleCount = uint32(f.stszCountOver)
	}
	return stsz
}

func buildDummyVideoTrak(t *testing.T, w *Writer, dataOffset uint64) {
	t.Helper()
	startBox(t, w, BoxTypeTrak())
	emitBox(t, w, &Tkhd{TrackID: 2, DurationV0: 1})
	startBox(t, w, BoxTypeMdia())
	emitBox(t, w, &Mdhd{Timescale: 48000, DurationV0: 1})
	emitBox(t, w, &Hdlr{HandlerType: [4]byte{'v', 'i', 'd', 'e'}, Name: "vide"})
	startBox(t, w, BoxTypeMinf())
	startBox(t, w, BoxTypeStbl())
	startBox(t, w, BoxTypeStsd())
	if _, err := Marshal(w, &Stsd{EntryCount: 1}, Context{}); err != nil {
		t.Fatal(err)
	}
	vse := &VisualSampleEntry{SampleEntry: SampleEntry{DataReferenceIndex: 1}, Width: 16, Height: 16}
	vse.SetType(BoxTypeMp4v())
	startBox(t, w, BoxTypeMp4v())
	if _, err := Marshal(w, vse, Context{}); err != nil {
		t.Fatal(err)
	}
	endBox(t, w) // mp4v
	endBox(t, w) // stsd
	emitBox(t, w, &Stts{EntryCount: 1, Entries: []SttsEntry{{SampleCount: 1, SampleDelta: 1}}})
	emitBox(t, w, &Stsc{EntryCount: 1, Entries: []StscEntry{{FirstChunk: 1, SamplesPerChunk: 1, SampleDescriptionIndex: 1}}})
	emitBox(t, w, &Stsz{SampleSize: 8, SampleCount: 1})
	emitBox(t, w, &Stco{EntryCount: 1, ChunkOffset: []uint32{uint32(dataOffset)}})
	endContainers(t, w, 4) // stbl, minf, mdia, trak
}

// testWriteSeeker is a local io.WriteSeeker backed by a byte buffer.
type testWriteSeeker struct {
	buf bytes.Buffer
	pos int64
}

func newTestWriteSeeker() *testWriteSeeker { return &testWriteSeeker{} }

func (t *testWriteSeeker) Write(p []byte) (int, error) {
	if extra := t.pos - int64(t.buf.Len()); extra > 0 {
		if _, err := t.buf.Write(make([]byte, extra)); err != nil {
			return 0, err
		}
	}
	if t.pos < int64(t.buf.Len()) {
		n := copy(t.buf.Bytes()[t.pos:], p)
		t.pos += int64(n)
		p = p[n:]
	}
	n, err := t.buf.Write(p)
	t.pos += int64(n)
	return n, err
}

func (t *testWriteSeeker) Seek(offset int64, whence int) (int64, error) {
	switch whence {
	case io.SeekStart:
		t.pos = offset
	case io.SeekCurrent:
		t.pos += offset
	case io.SeekEnd:
		t.pos = int64(t.buf.Len()) + offset
	}
	return t.pos, nil
}

func (t *testWriteSeeker) Bytes() []byte { return t.buf.Bytes() }

// ---------------------------------------------------------------------------
// Writers used for failure injection.
// ---------------------------------------------------------------------------

type countWriter struct {
	buf    bytes.Buffer
	writes int
}

func (w *countWriter) Write(p []byte) (int, error) {
	w.writes++
	return w.buf.Write(p)
}

type failWriter struct {
	err error
}

func (w *failWriter) Write([]byte) (int, error) { return 0, w.err }

// ---------------------------------------------------------------------------
// Test groups.
// ---------------------------------------------------------------------------

func TestOpusExportTables(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		f := defaultFixture()
		f.extraTrack = true
		data := buildMP4(t, f)
		r := bytes.NewReader(data)

		// A non-zero initial position must be restored afterwards.
		origin := int64(37)
		if _, err := r.Seek(origin, io.SeekStart); err != nil {
			t.Fatal(err)
		}
		out := &countWriter{}
		serial := uint32(0xDEADBEEF)
		if err := ExportOpusOgg(r, out, f.trackID, serial); err != nil {
			t.Fatalf("export failed: %v", err)
		}
		pos, err := r.Seek(0, io.SeekCurrent)
		if err != nil {
			t.Fatal(err)
		}
		c.Check(pos == origin, "source position restored: got %d want %d", pos, origin)

		pages := parseOggPages(t, out.buf.Bytes())
		// Window [480,5280): packet end times are 480,1440,2880,5760,...;
		// packet 3 is the first covering E, so 2 header pages + 4 audio.
		c.Check(len(pages) == 6, "page count = %d, want 6", len(pages))

		// BOS page / OpusHead.
		bos := pages[0]
		c.Check(bos.flags == 0x02, "BOS flag on first page: %#x", bos.flags)
		c.Check(bos.seq == 0 && bos.granule == 0 && bos.serial == serial, "BOS header fields seq=%d granule=%d serial=%#x", bos.seq, bos.granule, bos.serial)
		c.Check(len(bos.packet) == 19, "OpusHead length %d", len(bos.packet))
		c.Check(string(bos.packet[0:8]) == "OpusHead", "OpusHead magic")
		c.Check(bos.packet[8] == 1, "OpusHead version %d", bos.packet[8])
		c.Check(bos.packet[9] == f.dopsChannels, "OpusHead channels %d", bos.packet[9])
		preSkip := binary.LittleEndian.Uint16(bos.packet[10:12])
		c.Check(preSkip == 480, "pre_skip = S = %d (dOps.PreSkip was %d)", preSkip, f.preSkip)
		c.Check(binary.LittleEndian.Uint32(bos.packet[12:16]) == f.inputRate, "input sample rate LE")
		c.Check(int16(binary.LittleEndian.Uint16(bos.packet[16:18])) == f.gain, "output gain LE")
		c.Check(bos.packet[18] == 0, "mapping family 0")

		// Tags page.
		tags := pages[1]
		c.Check(tags.flags == 0 && tags.seq == 1 && tags.granule == 0, "tags page header")
		c.Check(string(tags.packet[0:8]) == "OpusTags", "OpusTags magic")
		vendorLen := binary.LittleEndian.Uint32(tags.packet[8:12])
		c.Check(vendorLen == 6, "vendor length %d", vendorLen)
		c.Check(string(tags.packet[12:18]) == "go-mp4", "vendor string")
		c.Check(binary.LittleEndian.Uint32(tags.packet[18:22]) == 0, "comment count zero")

		// Audio pages: sequence, flags, raw bytes and hand-computed granules.
		kept := f.packets[:4]
		var cumulative uint64
		for i, pkt := range kept {
			page := pages[2+i]
			c.Check(page.seq == uint32(2+i), "audio page seq %d", page.seq)
			c.Check(page.serial == serial, "audio page serial")
			c.Check(bytes.Equal(page.packet, pkt), "page %d packet bytes identical to source packet", i)
			cumulative += uint64(refTOCFrames(pkt))
			if i == len(kept)-1 {
				c.Check(page.flags == 0x04, "EOS only on final audio page, flags=%#x", page.flags)
				c.Check(page.granule == 5280, "EOS granule = E = 5280, got %d", page.granule)
			} else {
				c.Check(page.flags == 0, "non-final audio page flags=%#x", page.flags)
				c.Check(page.granule == cumulative, "page %d raw granule %d want %d", i, page.granule, cumulative)
			}
		}
		for i, p := range pages[:len(pages)-1] {
			c.Check(p.flags&0x04 == 0, "page %d must not carry EOS", i)
		}

		// The 255-byte packet is audio page index 2: lacing must be 255
		// followed by a zero terminator.
		p255 := pages[4]
		c.Check(len(p255.segments) == 2 && p255.segments[0] == 255 && p255.segments[1] == 0,
			"255-multiple lacing: %v", p255.segments)
		c.Check(len(p255.packet) == 255, "255-byte packet preserved")

		// co64 + uniform stsz variant of the same structural checks.
		f2 := defaultFixture()
		f2.useCo64 = true
		f2.uniformSize = true
		for i := range f2.packets {
			orig := f2.packets[i]
			f2.packets[i] = make([]byte, 4)
			f2.packets[i][0] = orig[0]
			if orig[0]&3 == 3 {
				f2.packets[i][1] = orig[1] // code-3 count byte must survive
			}
		}
		data2 := buildMP4(t, f2)
		out2 := &countWriter{}
		if err := ExportOpusOgg(bytes.NewReader(data2), out2, f2.trackID, 42); err != nil {
			t.Fatalf("co64 export failed: %v", err)
		}
		pages2 := parseOggPages(t, out2.buf.Bytes())
		c.Check(len(pages2) == 6, "co64 page count %d", len(pages2))
		c.Check(pages2[0].serial == 42, "co64 serial")
		for i, pkt := range f2.packets[:4] {
			c.Check(bytes.Equal(pages2[2+i].packet, pkt), "co64 page %d packet match", i)
			c.Check(len(pages2[2+i].segments) == 1, "small packet single lacing value")
		}

		// Multi-255 lacing: a 511-byte packet needs 255,255,1 and a
		// 510-byte packet needs 255,255,0.
		for _, size := range []int{510, 511} {
			f3 := defaultFixture()
			big := make([]byte, size)
			big[0] = f3.packets[0][0] // keep the same TOC configuration
			f3.packets[0] = big
			out3 := &countWriter{}
			if err := ExportOpusOgg(bytes.NewReader(buildMP4(t, f3)), out3, f3.trackID, 43); err != nil {
				t.Fatalf("large-packet export failed: %v", err)
			}
			pages3 := parseOggPages(t, out3.buf.Bytes())
			page0 := pages3[2]
			c.Check(len(page0.packet) == size, "packet size %d preserved", size)
			if size == 510 {
				c.Check(len(page0.segments) == 3 && page0.segments[0] == 255 && page0.segments[1] == 255 && page0.segments[2] == 0,
					"510-byte lacing: %v", page0.segments)
			} else {
				c.Check(len(page0.segments) == 3 && page0.segments[0] == 255 && page0.segments[1] == 255 && page0.segments[2] == 1,
					"511-byte lacing: %v", page0.segments)
			}
		}
	})
}

func TestOpusExportTiming(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Non-48k movie timescale (44100) and a cross-packet first hop:
		// S=1440 lands inside packet 2 (span 1440..2880). A 1920-frame
		// window converts exactly to 1764 movie units at 44100 Hz.
		f := defaultFixture()
		f.movieTS = 44100
		f.mediaTime = 1440
		f.segDurMovie = 1764
		data := buildMP4(t, f)
		out := &countWriter{}
		if err := ExportOpusOgg(bytes.NewReader(data), out, f.trackID, 77); err != nil {
			t.Fatalf("export failed: %v", err)
		}
		pages := parseOggPages(t, out.buf.Bytes())
		// E = 1440 + 1920 = 3360, first covered by packet 3 -> 4 audio pages.
		c.Check(len(pages) == 6, "timing page count %d", len(pages))
		c.Check(binary.LittleEndian.Uint16(pages[0].packet[10:12]) == 1440, "pre_skip = S = 1440")
		var cumulative uint64
		for i, pkt := range f.packets[:4] {
			page := pages[2+i]
			cumulative += uint64(refTOCFrames(pkt))
			if i == 3 {
				c.Check(page.granule == 3360, "EOS granule E=3360 got %d", page.granule)
				c.Check(page.flags == 0x04, "EOS flag")
			} else {
				c.Check(page.granule == cumulative, "raw cumulative granule page %d = %d", i, cumulative)
			}
		}
		// History packets starting before S are still present byte-for-byte.
		c.Check(bytes.Equal(pages[2].packet, f.packets[0]), "pre-hop history packet retained")
		c.Check(bytes.Equal(pages[3].packet, f.packets[1]), "pre-hop history packet retained")

		// In-packet tail trim: the final packet has a shorter positive stts
		// duration while its TOC still declares 480 frames.
		g := defaultFixture()
		g.deltas = []uint32{480, 960, 1440, 2880, 240}
		g.mediaTime = 0
		g.segDurMovie = 125 // 125ms @1000 = 6000 frames, equal to the trimmed stts total
		g.preSkip = 0
		dataG := buildMP4(t, g)
		outG := &countWriter{}
		if err := ExportOpusOgg(bytes.NewReader(dataG), outG, g.trackID, 78); err != nil {
			t.Fatalf("trim export failed: %v", err)
		}
		pagesG := parseOggPages(t, outG.buf.Bytes())
		c.Check(len(pagesG) == 7, "all five packets kept, pages=%d", len(pagesG))
		last := pagesG[len(pagesG)-1]
		c.Check(last.flags == 0x04 && last.granule == 6000, "trimmed EOS granule %d", last.granule)
		c.Check(bytes.Equal(last.packet, g.packets[4]), "trimmed packet payload untouched")
		c.Check(binary.LittleEndian.Uint16(pagesG[0].packet[10:12]) == 0, "pre_skip zero")
		c.Check(last.granule-uint64(binary.LittleEndian.Uint16(pagesG[0].packet[10:12])) == 6000, "audible length 6000")
		// Earlier pages still carry raw TOC granules; p3 ends at 5760.
		c.Check(pagesG[len(pagesG)-2].granule == 5760, "penultimate raw granule %d", pagesG[len(pagesG)-2].granule)

		// Window ending strictly inside the last packet: the covering
		// packet is kept but no whole packet after E is written.
		h := defaultFixture()
		h.mediaTime = 0
		h.segDurMovie = 122 // E = 5856, inside packet 4 stts span [5760,6000)
		dataH := buildMP4(t, h)
		outH := &countWriter{}
		if err := ExportOpusOgg(bytes.NewReader(dataH), outH, h.trackID, 79); err != nil {
			t.Fatalf("mid-packet window export failed: %v", err)
		}
		pagesH := parseOggPages(t, outH.buf.Bytes())
		c.Check(len(pagesH) == 7, "five audio packets up to covering packet, pages=%d", len(pagesH))
		c.Check(pagesH[len(pagesH)-1].granule == 5856 && pagesH[len(pagesH)-1].flags == 0x04,
			"mid-packet EOS granule %d", pagesH[len(pagesH)-1].granule)

		// The same window expressed with a version-1 (64-bit) edit list
		// must produce an identical layout and window.
		v1 := defaultFixture()
		v1.elstV1 = true
		outV1 := &countWriter{}
		if err := ExportOpusOgg(bytes.NewReader(buildMP4(t, v1)), outV1, v1.trackID, 80); err != nil {
			t.Fatalf("elst v1 export failed: %v", err)
		}
		pagesV1 := parseOggPages(t, outV1.buf.Bytes())
		c.Check(len(pagesV1) == 6, "elst v1 page count %d", len(pagesV1))
		c.Check(binary.LittleEndian.Uint16(pagesV1[0].packet[10:12]) == 480, "elst v1 pre_skip = S")
		c.Check(pagesV1[len(pagesV1)-1].granule == 5280 && pagesV1[len(pagesV1)-1].flags == 0x04,
			"elst v1 EOS granule %d", pagesV1[len(pagesV1)-1].granule)
	})
}

func TestOpusExportErrors(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		cases := []struct {
			name string
			mut  func(*exportFixture)
		}{
			{"fragmented top-level", func(f *exportFixture) { f.fragmentedTop = true }},
			{"mvex box", func(f *exportFixture) { f.addMvex = true }},
			{"ctts box", func(f *exportFixture) { f.addCtts = true }},
			{"sinf encryption", func(f *exportFixture) { f.addSinf = true }},
			{"multiple descriptions", func(f *exportFixture) { f.stsdEntries = 2 }},
			{"dOps version 1", func(f *exportFixture) { f.dopsVersion = 1 }},
			{"mapping family 1", func(f *exportFixture) { f.dopsFamily = 1; f.dopsChannels = 1 }},
			{"channel mismatch", func(f *exportFixture) { f.entryChannels = 2 }},
			{"non-48k mdhd", func(f *exportFixture) { f.mdhdTS = 24000 }},
			{"two elst entries", func(f *exportFixture) { f.doubleElst = true }},
			{"empty edit", func(f *exportFixture) { f.emptyElst = true }},
			{"rate not one", func(f *exportFixture) { f.rateInteger = 2 }},
			{"inexact edit divisor", func(f *exportFixture) { f.movieTS = 44100; f.segDurMovie = 1 }},
			{"edit beyond media", func(f *exportFixture) { f.mediaTime = 6000; f.segDurMovie = 96 }},
			{"mdhd stts mismatch", func(f *exportFixture) { f.mdhdDur = 9999 }},
			{"stts toc mismatch", func(f *exportFixture) { f.deltas[1] = 1000 }},
			{"code3 zero count", func(f *exportFixture) { f.code3Zero = 2 }},
			{"code3 missing byte", func(f *exportFixture) { f.code3Missing = 2 }},
			{"toc over 5760", func(f *exportFixture) { f.badTOC = 0 }},
			{"bad toc after window still rejected", func(f *exportFixture) {
				// Packet 4 lies wholly after E (5280); its malformed TOC
				// must still be detected before any page is written.
				f.packets[4] = []byte{0x43, 0x00} // code 3 with zero frames
			}},
			{"chunk outside mdat", func(f *exportFixture) { f.badChunkOffset = 1 }},
			{"zero size packet", func(f *exportFixture) { f.packets[0] = []byte{} }},
			{"oversize packet", func(f *exportFixture) { f.packets[0] = make([]byte, 65025) }},
			{"stsz count mismatch", func(f *exportFixture) { f.stszCountOver = 3 }},
		}
		for _, tc := range cases {
			f := defaultFixture()
			tc.mut(f)
			data := buildMP4(t, f)
			out := &countWriter{}
			err := ExportOpusOgg(bytes.NewReader(data), out, f.trackID, 1)
			c.Check(err != nil, "%s: expected error", tc.name)
			if err == nil {
				continue
			}
			c.Check(out.writes == 0, "%s: no bytes written before error, writes=%d", tc.name, out.writes)
		}

		// A dref/url entry that is not self-contained.
		extData := buildMP4WithExternalURL(t, defaultFixture())
		extOut := &countWriter{}
		err := ExportOpusOgg(bytes.NewReader(extData), extOut, defaultFixture().trackID, 1)
		c.Check(err != nil, "external data reference: expected error")
		c.Check(extOut.writes == 0, "external data reference writes nothing")

		// Unknown track ID.
		f := defaultFixture()
		data := buildMP4(t, f)
		out := &countWriter{}
		err = ExportOpusOgg(bytes.NewReader(data), out, 999, 1)
		c.Check(err != nil, "unknown trackID errors")
		c.Check(out.writes == 0, "unknown trackID writes nothing")

		// Write failure after successful validation: the error is returned
		// as-is and the source position is still restored.
		sentinel := errors.New("disk full")
		r := bytes.NewReader(buildMP4(t, defaultFixture()))
		if _, err := r.Seek(11, io.SeekStart); err != nil {
			t.Fatal(err)
		}
		fw := &failWriter{err: sentinel}
		err = ExportOpusOgg(r, fw, f.trackID, 5)
		c.Check(errors.Is(err, sentinel), "write error returned as-is: %v", err)
		pos, err := r.Seek(0, io.SeekCurrent)
		if err != nil {
			t.Fatal(err)
		}
		c.Check(pos == 11, "position restored after write failure: %d", pos)

		// Position restored after a validation failure as well.
		bad := defaultFixture()
		bad.mdhdTS = 24000
		r2 := bytes.NewReader(buildMP4(t, bad))
		if _, err := r2.Seek(23, io.SeekStart); err != nil {
			t.Fatal(err)
		}
		err = ExportOpusOgg(r2, &countWriter{}, f.trackID, 5)
		c.Check(err != nil, "bad timescale rejected")
		pos2, err := r2.Seek(0, io.SeekCurrent)
		if err != nil {
			t.Fatal(err)
		}
		c.Check(pos2 == 23, "position restored after validation failure: %d", pos2)
	})
}

// buildMP4WithExternalURL builds a minimal valid track whose dref points at a
// url entry without the self-contained flag.
func buildMP4WithExternalURL(t *testing.T, f *exportFixture) []byte {
	t.Helper()
	ws := newTestWriteSeeker()
	w := NewWriter(ws)

	emitBox(t, w, &Ftyp{MajorBrand: BrandISOM(), CompatibleBrands: []CompatibleBrandElem{{CompatibleBrand: BrandISOM()}}})

	mbi := startBox(t, w, BoxTypeMdat())
	off, err := w.Seek(0, io.SeekCurrent)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := w.Write(f.packets[0]); err != nil {
		t.Fatal(err)
	}
	endBox(t, w)
	_ = mbi

	startBox(t, w, BoxTypeMoov())
	emitBox(t, w, &Mvhd{Timescale: f.movieTS, DurationV0: uint32(f.segDurMovie), NextTrackID: 2})
	startBox(t, w, BoxTypeTrak())
	emitBox(t, w, &Tkhd{TrackID: f.trackID, DurationV0: uint32(f.segDurMovie)})
	startBox(t, w, BoxTypeEdts())
	emitBox(t, w, buildElst(f))
	endBox(t, w)
	startBox(t, w, BoxTypeMdia())
	emitBox(t, w, &Mdhd{Timescale: 48000, DurationV0: uint32(mediaDuration(f)), Language: [3]byte{'u', 'n', 'd'}})
	emitBox(t, w, &Hdlr{HandlerType: [4]byte{'s', 'o', 'u', 'n'}, Name: "soun"})
	startBox(t, w, BoxTypeMinf())
	emitBox(t, w, &Smhd{})

	startBox(t, w, BoxTypeDinf())
	startBox(t, w, BoxTypeDref())
	if _, err := Marshal(w, &Dref{EntryCount: 1}, Context{}); err != nil {
		t.Fatal(err)
	}
	emitBox(t, w, &Url{Location: "http://example.invalid/a"}) // flags zero
	endBox(t, w)                                              // dref
	endBox(t, w)                                              // dinf

	startBox(t, w, BoxTypeStbl())
	startBox(t, w, BoxTypeStsd())
	if _, err := Marshal(w, &Stsd{EntryCount: 1}, Context{}); err != nil {
		t.Fatal(err)
	}
	ase := &AudioSampleEntry{SampleEntry: SampleEntry{DataReferenceIndex: 1}, ChannelCount: 1, SampleSize: 16, SampleRate: 48000 << 16}
	ase.SetType(BoxTypeOpus())
	startBox(t, w, BoxTypeOpus())
	if _, err := Marshal(w, ase, Context{}); err != nil {
		t.Fatal(err)
	}
	emitBox(t, w, &DOps{OutputChannelCount: 1, PreSkip: f.preSkip, InputSampleRate: f.inputRate, OutputGain: f.gain})
	endBox(t, w) // Opus
	endBox(t, w) // stsd
	emitBox(t, w, buildStts(f.deltas))
	emitBox(t, w, &Stsc{EntryCount: 1, Entries: []StscEntry{{FirstChunk: 1, SamplesPerChunk: 1, SampleDescriptionIndex: 1}}})
	emitBox(t, w, &Stsz{SampleSize: uint32(len(f.packets[0])), SampleCount: 1})
	emitBox(t, w, &Stco{EntryCount: 1, ChunkOffset: []uint32{uint32(off)}})
	endContainers(t, w, 4) // stbl, minf, mdia, trak
	endBox(t, w)           // moov
	return ws.Bytes()
}
