package mp4

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"testing"

	"github.com/abema/go-mp4/internal/oggopus"
	"github.com/abema/go-mp4/task_support/taskcheck"
	"github.com/orcaman/writerseeker"
)

// ---- independent Opus TOC reference (RFC 6716 section 3) ----

func refTocFrameDuration(config int) uint32 {
	switch {
	case config < 12:
		return [4]uint32{480, 960, 1920, 2880}[config%4]
	case config < 16:
		return [2]uint32{480, 960}[config%2]
	default:
		return [4]uint32{120, 240, 480, 960}[config%4]
	}
}

// refTocDuration independently computes the total 48 kHz frame count of a
// synthetic Opus packet from its TOC bytes.
func refTocDuration(packet []byte) (uint32, error) {
	if len(packet) == 0 {
		return 0, errors.New("empty packet")
	}
	config := int(packet[0] >> 3)
	code := packet[0] & 3
	var frames uint32
	switch code {
	case 0:
		frames = 1
	case 1, 2:
		frames = 2
	case 3:
		if len(packet) < 2 {
			return 0, errors.New("code 3 missing frame-count byte")
		}
		frames = uint32(packet[1] & 0x3f)
		if frames == 0 {
			return 0, errors.New("code 3 zero frame count")
		}
	}
	total := frames * refTocFrameDuration(config)
	if total > 5760 {
		return 0, fmt.Errorf("toc duration %d > 5760", total)
	}
	return total, nil
}

// opusPacket builds a packet with the requested TOC configuration and code,
// padded to size bytes. Payload bytes are deterministic but otherwise
// arbitrary: the exporter must not decode or rewrite them.
func opusPacket(config, code int, count byte, size int) []byte {
	if size < 1 {
		panic("packet size must be positive")
	}
	pkt := make([]byte, size)
	pkt[0] = byte(config<<3) | byte(code)
	if code == 3 {
		if size < 2 {
			panic("code 3 packet needs at least 2 bytes")
		}
		pkt[1] = count & 0x3f
	}
	for i := 1; i < size; i++ {
		if code == 3 && i == 1 {
			continue
		}
		pkt[i] = byte(0x40 + (i * 7))
	}
	if _, err := refTocDuration(pkt); err != nil {
		panic(fmt.Sprintf("synthetic packet invalid: %v", err))
	}
	return pkt
}

func makeOpusPacket(t *testing.T, config, code int, count byte, size int) []byte {
	t.Helper()
	return opusPacket(config, code, count, size)
}

func makePacketWithCode3(t *testing.T, config, count int, size int) []byte {
	t.Helper()
	pkt := make([]byte, size)
	pkt[0] = byte(config<<3) | 3
	pkt[1] = byte(count)
	for i := 2; i < size; i++ {
		pkt[i] = byte(i)
	}
	return pkt
}

// ---- synthetic MP4 builder ----

type testPacket struct {
	payload []byte
	delta   uint32 // stts delta; 0 means use the TOC duration
}

type mp4Spec struct {
	movieTimescale  uint32
	segmentMovieDur uint64 // 0 => derive exactly from E-S
	mediaTime       int64
	rateInteger     int16
	rateFraction    int16

	trackID       uint32
	channels      uint8
	entryChannels uint16 // 0 => channels
	preSkip       uint16
	inputRate     uint32
	outputGain    int16

	packets      []testPacket
	chunkPackets [][]int // packet indices per chunk, decode order
	chunkMdat    []int   // mdat index per chunk
	useCO64      bool
	uniformSize  bool

	padBetweenChunks int  // padding bytes between chunks inside an mdat
	reversePhysical  bool // lay chunk payloads out in reverse physical order
	extraTrack       bool

	// structural / table knobs for error cases
	mdhdTimescale     uint32 // 0 => 48000
	mdhdDurationOver  *uint64
	dopsVersion       uint8
	dopsFamily        uint8
	entryRate         uint32 // 0 => 48000<<16
	addCtts           bool
	addMvex           bool
	addMoof           bool
	addPssh           bool
	addSaio           bool
	sinfInOpus        bool
	encaEntry         bool
	stsdTwoEntries    bool
	missingDref       bool
	badDataRefURL     bool
	elstMissing       bool
	elstOverride      *Elst
	bothCO            bool
	neitherCO         bool
	stscDescIndex     uint32 // 0 => 1
	stscZeroSamples   bool
	stscBadFirstChunk bool
	stszCountOverride *uint32
	corruptOffset     *uint64 // replaces chunk 0 offset
	noMdat            bool
}

type builtMP4 struct {
	data         []byte
	packets      [][]byte
	chunkOffsets []uint64
}

func buildOpusMP4(t *testing.T, spec *mp4Spec) *builtMP4 {
	t.Helper()

	if spec.trackID == 0 {
		spec.trackID = 1
	}
	if spec.channels == 0 {
		spec.channels = 2
	}
	if spec.inputRate == 0 {
		spec.inputRate = 48000
	}
	if spec.movieTimescale == 0 {
		spec.movieTimescale = 48000
	}
	mdhdTS := spec.mdhdTimescale
	if mdhdTS == 0 {
		mdhdTS = 48000
	}

	chunkCount := len(spec.chunkPackets)
	mdatCount := 1
	for _, mi := range spec.chunkMdat {
		if mi+1 > mdatCount {
			mdatCount = mi + 1
		}
	}

	// Assemble mdat payloads. chunkPos records the payload-relative offset
	// of every chunk, independent of physical write order.
	mdatBufs := make([]bytes.Buffer, mdatCount)
	chunkPos := make([]uint64, chunkCount)
	physOrder := make([]int, chunkCount)
	for i := range physOrder {
		physOrder[i] = i
	}
	if spec.reversePhysical {
		for i, j := 0, len(physOrder)-1; i < j; i, j = i+1, j-1 {
			physOrder[i], physOrder[j] = physOrder[j], physOrder[i]
		}
	}
	for _, ci := range physOrder {
		mi := spec.chunkMdat[ci]
		buf := &mdatBufs[mi]
		if buf.Len() > 0 && spec.padBetweenChunks > 0 {
			buf.Write(bytes.Repeat([]byte{0xDD}, spec.padBetweenChunks))
		}
		chunkPos[ci] = uint64(buf.Len())
		for _, pi := range spec.chunkPackets[ci] {
			buf.Write(spec.packets[pi].payload)
		}
	}
	var extraByteOffset uint64
	if spec.extraTrack {
		extraByteOffset = uint64(mdatBufs[0].Len())
		mdatBufs[0].WriteByte(0xEE)
	}

	ws := &writerseeker.WriterSeeker{}
	w := NewWriter(ws)
	emit := func(b IImmutableBox) {
		t.Helper()
		if _, err := w.StartBox(&BoxInfo{Type: b.GetType()}); err != nil {
			t.Fatal(err)
		}
		if _, err := Marshal(w, b, Context{}); err != nil {
			t.Fatal(err)
		}
		if _, err := w.EndBox(); err != nil {
			t.Fatal(err)
		}
	}
	begin := func(b IImmutableBox) *BoxInfo {
		t.Helper()
		bi, err := w.StartBox(&BoxInfo{Type: b.GetType()})
		if err != nil {
			t.Fatal(err)
		}
		if _, err = Marshal(w, b, Context{}); err != nil {
			t.Fatal(err)
		}
		return bi
	}
	end := func() {
		t.Helper()
		if _, err := w.EndBox(); err != nil {
			t.Fatal(err)
		}
	}
	emitRaw := func(typ BoxType, data []byte) {
		t.Helper()
		if _, err := w.StartBox(&BoxInfo{Type: typ}); err != nil {
			t.Fatal(err)
		}
		if _, err := w.Write(data); err != nil {
			t.Fatal(err)
		}
		end()
	}

	emit(&Ftyp{MajorBrand: BrandISOM(), CompatibleBrands: []CompatibleBrandElem{{CompatibleBrand: BrandISOM()}}})

	if spec.addMoof {
		begin(&Moof{})
		emit(&Mfhd{SequenceNumber: 1})
		end()
	}
	if spec.addPssh {
		emitRaw(BoxTypePssh(), []byte{0, 0, 0, 0})
	}

	chunkAbs := make([]uint64, chunkCount)
	mdatInfos := make([]*BoxInfo, mdatCount)
	if !spec.noMdat {
		for mi := 0; mi < mdatCount; mi++ {
			if mi > 0 {
				emitRaw(BoxTypeFree(), nil) // free box between mdats
			}
			bi := begin(&Mdat{})
			payloadStart := bi.Offset + bi.HeaderSize
			if _, err := w.Write(mdatBufs[mi].Bytes()); err != nil {
				t.Fatal(err)
			}
			end()
			mdatInfos[mi] = bi
			for ci := 0; ci < chunkCount; ci++ {
				if spec.chunkMdat[ci] == mi {
					chunkAbs[ci] = payloadStart + chunkPos[ci]
				}
			}
		}
	}
	if spec.corruptOffset != nil {
		chunkAbs[0] = *spec.corruptOffset
	}

	// stts entries and the stts total.
	var sttsEntries []SttsEntry
	var sttsTotal uint64
	for _, chunk := range spec.chunkPackets {
		for _, pi := range chunk {
			pkt := spec.packets[pi]
			delta := pkt.delta
			if delta == 0 {
				d, err := refTocDuration(pkt.payload)
				if err != nil {
					t.Fatal(err)
				}
				delta = d
			}
			sttsEntries = append(sttsEntries, SttsEntry{SampleCount: 1, SampleDelta: delta})
			sttsTotal += uint64(delta)
		}
	}
	mdhdDuration := sttsTotal
	if spec.mdhdDurationOver != nil {
		mdhdDuration = *spec.mdhdDurationOver
	}

	// edit list default: window from mediaTime to the stts media end. The
	// stts total is always well-defined even when a case deliberately builds
	// an invalid TOC packet.
	var tocEnd uint64
	for _, chunk := range spec.chunkPackets {
		for _, pi := range chunk {
			pkt := spec.packets[pi]
			delta := pkt.delta
			if delta == 0 {
				d, err := refTocDuration(pkt.payload)
				if err != nil {
					t.Fatal(err)
				}
				delta = d
			}
			tocEnd += uint64(delta)
		}
	}
	segmentDur := spec.segmentMovieDur
	if segmentDur == 0 {
		// exact conversion via the chosen movie timescale
		segmentDur = (tocEnd - uint64(spec.mediaTime)) * uint64(spec.movieTimescale) / 48000
	}

	begin(&Moov{})
	emit(&Mvhd{Timescale: spec.movieTimescale, DurationV0: uint32(segmentDur), NextTrackID: uint32(spec.trackID) + 101})
	if spec.addMvex {
		begin(&Mvex{})
		emit(&Trex{TrackID: spec.trackID, DefaultSampleDescriptionIndex: 1, DefaultSampleDuration: 960})
		end()
	}

	// Optional extra track; its single byte lives inside the first mdat and
	// must never be copied into the selected track's Ogg stream. Skipped when
	// the case deliberately removes every mdat.
	if spec.extraTrack && !spec.noMdat {
		begin(&Trak{})
		emit(&Tkhd{TrackID: spec.trackID + 100, DurationV0: uint32(segmentDur)})
		begin(&Mdia{})
		emit(&Mdhd{Timescale: 48000, DurationV0: 1024, Language: [3]byte{'u', 'n', 'd'}})
		begin(&Minf{})
		begin(&Stbl{})
		begin(&Stsd{EntryCount: 1})
		other := &AudioSampleEntry{SampleEntry: SampleEntry{DataReferenceIndex: 1}, ChannelCount: 1, SampleSize: 16, SampleRate: 48000 << 16}
		other.SetType(BoxTypeMp4a())
		emit(other)
		end()
		emit(&Stts{EntryCount: 1, Entries: []SttsEntry{{SampleCount: 1, SampleDelta: 1024}}})
		emit(&Stsc{EntryCount: 1, Entries: []StscEntry{{FirstChunk: 1, SamplesPerChunk: 1, SampleDescriptionIndex: 1}}})
		emit(&Stsz{SampleSize: 1, SampleCount: 1})
		off := mdatInfos[0].Offset + mdatInfos[0].HeaderSize + extraByteOffset
		emit(&Stco{EntryCount: 1, ChunkOffset: []uint32{uint32(off)}})
		end() // stbl
		end() // minf
		end() // mdia
		end() // trak
	}

	begin(&Trak{})
	emit(&Tkhd{TrackID: spec.trackID, DurationV0: uint32(segmentDur)})
	if !spec.elstMissing {
		begin(&Edts{})
		elst := spec.elstOverride
		if elst == nil {
			elst = &Elst{
				EntryCount: 1,
				Entries: []ElstEntry{{
					SegmentDurationV0: uint32(segmentDur),
					MediaTimeV0:       int32(spec.mediaTime),
					MediaRateInteger:  spec.rateIntegerOrOne(),
					MediaRateFraction: spec.rateFraction,
				}},
			}
		}
		emit(elst)
		end()
	}
	begin(&Mdia{})
	emit(&Mdhd{Timescale: mdhdTS, DurationV0: uint32(mdhdDuration), Language: [3]byte{'u', 'n', 'd'}})
	begin(&Minf{})
	begin(&Stbl{})

	entryCount := uint32(1)
	if spec.stsdTwoEntries || spec.encaEntry {
		entryCount = 2
	}
	begin(&Stsd{EntryCount: entryCount})
	ase := &AudioSampleEntry{
		SampleEntry:  SampleEntry{DataReferenceIndex: 1},
		ChannelCount: spec.entryChannelsOrZero(),
		SampleSize:   16,
		SampleRate:   spec.entryRateOr48k(),
	}
	ase.SetType(BoxTypeOpus())
	begin(ase)
	dops := &DOps{
		Version:              spec.dopsVersion,
		OutputChannelCount:   spec.channels,
		PreSkip:              spec.preSkip,
		InputSampleRate:      spec.inputRate,
		OutputGain:           spec.outputGain,
		ChannelMappingFamily: spec.dopsFamily,
	}
	if spec.dopsFamily != 0 {
		// Family 1: one coupled stereo stream (or a single mono stream).
		dops.StreamCount = 1
		if spec.channels == 2 {
			dops.CoupledCount = 1
		}
		dops.ChannelMapping = make([]byte, spec.channels)
		for i := range dops.ChannelMapping {
			dops.ChannelMapping[i] = byte(i)
		}
	}
	emit(dops)
	if spec.sinfInOpus {
		emitRaw(BoxTypeSinf(), nil)
	}
	end() // Opus entry
	if spec.encaEntry {
		enc := &AudioSampleEntry{SampleEntry: SampleEntry{DataReferenceIndex: 1}, ChannelCount: 2, SampleSize: 16, SampleRate: 48000 << 16}
		enc.SetType(BoxTypeEnca())
		emit(enc)
	}
	end() // stsd

	emit(&Stts{EntryCount: uint32(len(sttsEntries)), Entries: sttsEntries})

	var stscEntries []StscEntry
	for ci, chunk := range spec.chunkPackets {
		n := uint32(len(chunk))
		if spec.stscZeroSamples {
			n = 0
		}
		desc := spec.stscDescIndex
		if desc == 0 {
			desc = 1
		}
		first := uint32(ci + 1)
		if spec.stscBadFirstChunk && ci == 0 {
			first = 0
		}
		stscEntries = append(stscEntries, StscEntry{FirstChunk: first, SamplesPerChunk: n, SampleDescriptionIndex: desc})
	}
	emit(&Stsc{EntryCount: uint32(len(stscEntries)), Entries: stscEntries})

	if spec.uniformSize {
		first := spec.packets[spec.chunkPackets[0][0]].payload
		for _, pkt := range spec.packets {
			if len(pkt.payload) != len(first) {
				t.Fatalf("uniformSize requires equal packet sizes")
			}
		}
		emit(&Stsz{SampleSize: uint32(len(first)), SampleCount: uint32(len(spec.packets))})
	} else {
		sizes := make([]uint32, len(spec.packets))
		for i, pkt := range spec.packets {
			sizes[i] = uint32(len(pkt.payload))
		}
		if spec.stszCountOverride != nil {
			// Emit a deliberately inconsistent stsz without going through the
			// marshaller, which would reject the length mismatch.
			payload := make([]byte, 12+4*len(sizes))
			binary.BigEndian.PutUint32(payload[4:8], 0) // sample_size 0
			binary.BigEndian.PutUint32(payload[8:12], *spec.stszCountOverride)
			for i, size := range sizes {
				binary.BigEndian.PutUint32(payload[12+4*i:16+4*i], size)
			}
			emitRaw(BoxTypeStsz(), payload)
		} else {
			emit(&Stsz{SampleCount: uint32(len(sizes)), EntrySize: sizes})
		}
	}

	switch {
	case spec.bothCO:
		emit(&Stco{EntryCount: uint32(chunkCount), ChunkOffset: u32Offsets(chunkAbs)})
		emit(&Co64{EntryCount: uint32(chunkCount), ChunkOffset: append([]uint64(nil), chunkAbs...)})
	case spec.neitherCO:
		// emit neither table
	case spec.useCO64:
		emit(&Co64{EntryCount: uint32(chunkCount), ChunkOffset: append([]uint64(nil), chunkAbs...)})
	default:
		emit(&Stco{EntryCount: uint32(chunkCount), ChunkOffset: u32Offsets(chunkAbs)})
	}

	if spec.addCtts {
		emit(&Ctts{EntryCount: 1, Entries: []CttsEntry{{SampleCount: 1, SampleOffsetV0: 0}}})
	}
	if spec.addSaio {
		emit(&Saio{EntryCount: 0})
	}

	end() // stbl
	if !spec.missingDref {
		begin(&Dinf{})
		begin(&Dref{EntryCount: 1})
		url := &Url{}
		if !spec.badDataRefURL {
			url.SetFlags(UrlSelfContained)
		}
		emit(url)
		end() // dref
		end() // dinf
	}
	end() // minf
	end() // mdia
	end() // trak
	end() // moov

	packetBytes := make([][]byte, len(spec.packets))
	for i, pkt := range spec.packets {
		packetBytes[i] = append([]byte(nil), pkt.payload...)
	}
	data, err := io.ReadAll(ws.BytesReader())
	if err != nil {
		t.Fatal(err)
	}
	return &builtMP4{data: data, packets: packetBytes, chunkOffsets: chunkAbs}
}

func (s *mp4Spec) rateIntegerOrOne() int16 {
	if s.rateInteger == 0 {
		return 1
	}
	return s.rateInteger
}

func (s *mp4Spec) entryChannelsOrZero() uint16 {
	if s.entryChannels != 0 {
		return s.entryChannels
	}
	return uint16(s.channels)
}

func (s *mp4Spec) entryRateOr48k() uint32 {
	if s.entryRate != 0 {
		return s.entryRate
	}
	return 48000 << 16
}

func u32Offsets(in []uint64) []uint32 {
	out := make([]uint32, len(in))
	for i, v := range in {
		out[i] = uint32(v)
	}
	return out
}

// ---- independent Ogg page parser and CRC check ----

var oggCRCTable [256]uint32

func init() {
	for n := 0; n < 256; n++ {
		c := uint32(n) << 24
		for k := 0; k < 8; k++ {
			if c&0x80000000 != 0 {
				c = c<<1 ^ 0x04c11db7
			} else {
				c <<= 1
			}
		}
		oggCRCTable[n] = c
	}
}

func oggCRC(data []byte) uint32 {
	var crc uint32
	for _, b := range data {
		crc = (crc << 8) ^ oggCRCTable[byte(crc>>24)^b]
	}
	return crc
}

type oggPage struct {
	headerType byte
	granule    int64
	serial     uint32
	seq        uint32
	crc        uint32
	segments   []byte
	packet     []byte
}

func parseOggPages(t *testing.T, data []byte) []oggPage {
	t.Helper()
	var pages []oggPage
	pos := 0
	for pos < len(data) {
		if len(data)-pos < 27 {
			t.Fatalf("truncated page header at %d", pos)
		}
		hdr := data[pos : pos+27]
		if !bytes.Equal(hdr[0:4], []byte("OggS")) {
			t.Fatalf("bad capture pattern at %d", pos)
		}
		if hdr[4] != 0 {
			t.Fatalf("unsupported Ogg version %d", hdr[4])
		}
		segCount := int(hdr[26])
		segStart := pos + 27
		if len(data) < segStart+segCount {
			t.Fatalf("truncated segment table")
		}
		segments := append([]byte(nil), data[segStart:segStart+segCount]...)
		packetLen := 0
		for _, s := range segments {
			packetLen += int(s)
		}
		pktStart := segStart + segCount
		if len(data) < pktStart+packetLen {
			t.Fatalf("truncated packet data")
		}
		packet := append([]byte(nil), data[pktStart:pktStart+packetLen]...)

		stored := binary.LittleEndian.Uint32(hdr[22:26])
		zeroed := append([]byte(nil), data[pos:pktStart+packetLen]...)
		binary.LittleEndian.PutUint32(zeroed[22:26], 0)
		calc := oggCRC(zeroed)
		if stored != calc {
			t.Fatalf("page %d CRC mismatch: stored=%08x calc=%08x", len(pages), stored, calc)
		}

		pages = append(pages, oggPage{
			headerType: hdr[5],
			granule:    int64(binary.LittleEndian.Uint64(hdr[6:14])),
			serial:     binary.LittleEndian.Uint32(hdr[14:18]),
			seq:        binary.LittleEndian.Uint32(hdr[18:22]),
			crc:        stored,
			segments:   segments,
			packet:     packet,
		})
		pos = pktStart + packetLen
	}
	if pos != len(data) {
		t.Fatalf("trailing bytes after Ogg stream")
	}
	return pages
}

// ---- shared export helper ----

func exportBytes(t *testing.T, mp4Data []byte, trackID, serial uint32) []byte {
	t.Helper()
	r := bytes.NewReader(mp4Data)
	var out bytes.Buffer
	if err := ExportOpusOgg(r, &out, trackID, serial); err != nil {
		t.Fatalf("ExportOpusOgg failed: %v", err)
	}
	return out.Bytes()
}

func richSpec() *mp4Spec {
	// Durations: mix configs 0..4, 12, 16, 31 and all four TOC codes; sizes
	// include multiples of 255 to exercise the zero lacing terminator.
	pkts := []testPacket{
		{payload: opusPacket(0, 0, 0, 10)},                     // 480
		{payload: opusPacket(1, 1, 0, 255)},                    // 960*2, 255 multiple
		{payload: opusPacket(2, 2, 0, 256)},                    // 1920*2
		{payload: opusPacket(3, 3, 2, 300)},                    // 2880*2
		{payload: opusPacket(4, 0, 0, 510)},                    // 960, 510 = 2*255
		{payload: opusPacket(16, 3, 3, 77)},                    // 120*3
		{payload: opusPacket(31, 0, 0, 1)},                     // 960
		{payload: opusPacket(12, 1, 0, oggopus.MaxPacketSize)}, // 480*2, max size
	}
	return &mp4Spec{
		movieTimescale:   48000,
		mediaTime:        0,
		trackID:          7,
		channels:         2,
		preSkip:          3840, // deliberately different from S=0
		inputRate:        44100,
		outputGain:       -256,
		packets:          pkts,
		chunkPackets:     [][]int{{0, 1}, {2}, {3, 4, 5}, {6, 7}},
		chunkMdat:        []int{0, 0, 1, 1},
		useCO64:          true,
		padBetweenChunks: 16,
		extraTrack:       true,
	}
}

func TestOpusExportTables(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		spec := richSpec()
		built := buildOpusMP4(t, spec)
		serial := uint32(0xdeadbeef)
		oggData := exportBytes(t, built.data, spec.trackID, serial)
		pages := parseOggPages(t, oggData)

		// 2 header pages + one page per packet.
		c.Check(len(pages) == 2+len(spec.packets), "page count = %d, want %d", len(pages), 2+len(spec.packets))

		// BOS page: OpusHead alone.
		bos := pages[0]
		c.Check(bos.headerType == 0x02, "BOS header type = %#x", bos.headerType)
		c.Check(bos.granule == 0, "BOS granule = %d", bos.granule)
		c.Check(bos.seq == 0 && bos.serial == serial, "BOS seq/serial = %d/%#x", bos.seq, bos.serial)
		c.Check(len(bos.packet) == 19, "OpusHead size = %d", len(bos.packet))
		c.Check(bytes.Equal(bos.packet[0:8], []byte("OpusHead")), "OpusHead magic")
		c.Check(bos.packet[8] == 1, "OpusHead version = %d", bos.packet[8])
		c.Check(bos.packet[9] == spec.channels, "OpusHead channels = %d", bos.packet[9])
		preSkip := binary.LittleEndian.Uint16(bos.packet[10:12])
		c.Check(preSkip == uint16(spec.mediaTime), "OpusHead pre_skip = %d, want S=%d (dOps preSkip=%d)", preSkip, spec.mediaTime, spec.preSkip)
		c.Check(preSkip != spec.preSkip, "pre_skip must follow edit S, not dOps.PreSkip")
		c.Check(binary.LittleEndian.Uint32(bos.packet[12:16]) == spec.inputRate, "OpusHead input sample rate")
		c.Check(int16(binary.LittleEndian.Uint16(bos.packet[16:18])) == spec.outputGain, "OpusHead output gain")
		c.Check(bos.packet[18] == 0, "OpusHead mapping family = %d", bos.packet[18])

		// Tags page: vendor go-mp4, zero comments.
		tags := pages[1]
		c.Check(tags.headerType == 0x00, "tags header type = %#x", tags.headerType)
		c.Check(tags.granule == 0 && tags.seq == 1, "tags granule/seq")
		c.Check(bytes.Equal(tags.packet[0:8], []byte("OpusTags")), "OpusTags magic")
		vendorLen := binary.LittleEndian.Uint32(tags.packet[8:12])
		c.Check(vendorLen == 6, "vendor length = %d", vendorLen)
		c.Check(string(tags.packet[12:18]) == "go-mp4", "vendor = %q", string(tags.packet[12:18]))
		c.Check(binary.LittleEndian.Uint32(tags.packet[18:22]) == 0, "comment count must be zero")

		// Audio pages: packet bytes verbatim, hand-computed granules.
		var cumulative uint64
		var E uint64
		for _, pkt := range spec.packets {
			d, err := refTocDuration(pkt.payload)
			if err != nil {
				t.Fatal(err)
			}
			E += uint64(d)
		}
		for i, pkt := range spec.packets {
			page := pages[2+i]
			c.Check(page.seq == uint32(2+i), "audio page %d seq = %d", i, page.seq)
			c.Check(page.serial == serial, "audio page %d serial", i)
			c.Check(bytes.Equal(page.packet, built.packets[i]), "page %d packet bytes differ from source", i)

			d, err := refTocDuration(pkt.payload)
			if err != nil {
				t.Fatal(err)
			}
			cumulative += uint64(d)
			isLast := i == len(spec.packets)-1
			wantHeader := byte(0x00)
			wantGranule := int64(cumulative)
			if isLast {
				wantHeader = 0x04
				wantGranule = int64(E)
			}
			c.Check(page.headerType == wantHeader, "page %d header type = %#x", i, page.headerType)
			c.Check(page.granule == wantGranule, "page %d granule = %d, want %d", i, page.granule, wantGranule)
			c.Check((page.headerType&0x04 != 0) == isLast, "EOS only on final page (page %d)", i)
		}

		// 255-multiple packets keep a zero terminating lacing value.
		for _, idx := range []int{1, 4} {
			segs := pages[2+idx].segments
			c.Check(len(segs) > 0 && segs[len(segs)-1] == 0, "packet %d (255 multiple) needs zero lacing terminator", idx)
			c.Check(len(pages[2+idx].packet) == len(built.packets[idx]), "packet %d reassembled length", idx)
		}
		// Max-size packet needs 255 segments and no continuation page.
		c.Check(len(pages[2+7].segments) == 255, "max packet segment count = %d", len(pages[2+7].segments))

		// Decode order follows chunk order, not physical addresses: rebuild
		// with reversed physical layout and compare the packet sequence.
		revSpec := *richSpec()
		revSpec.reversePhysical = true
		revBuilt := buildOpusMP4(t, &revSpec)
		revPages := parseOggPages(t, exportBytes(t, revBuilt.data, revSpec.trackID, serial))
		physicalReordered := false
		for ci := 1; ci < len(revBuilt.chunkOffsets); ci++ {
			if revBuilt.chunkOffsets[ci] < revBuilt.chunkOffsets[ci-1] {
				physicalReordered = true
			}
		}
		c.Check(physicalReordered, "reversed layout must actually reorder physical offsets")
		for i := range spec.packets {
			c.Check(bytes.Equal(revPages[2+i].packet, built.packets[i]), "reverse-physical page %d keeps decode order", i)
		}

		// Uniform stsz + plain stco variant.
		uniSpec := richSpec()
		uniSpec.useCO64 = false
		uniSpec.uniformSize = true
		for i := range uniSpec.packets {
			uniSpec.packets[i].payload = makeOpusPacket(t, 1, 0, 0, 64)
		}
		uniBuilt := buildOpusMP4(t, uniSpec)
		uniPages := parseOggPages(t, exportBytes(t, uniBuilt.data, uniSpec.trackID, 99))
		c.Check(len(uniPages) == 2+len(uniSpec.packets), "uniform stsz page count")
		for i := range uniSpec.packets {
			c.Check(bytes.Equal(uniPages[2+i].packet, uniBuilt.packets[i]), "uniform stsz packet %d bytes", i)
		}
	})
}

func TestOpusExportTiming(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Six 960-frame packets; the edit starts inside packet 2 (pre-roll
		// across packets) and ends inside the last packet (tail trim).
		pkts := make([]testPacket, 6)
		for i := range pkts {
			pkts[i] = testPacket{payload: makeOpusPacket(t, 1, 0, 0, 20+i)}
		}
		S := uint64(2*960 + 480) // 2400, inside packet 2
		E := uint64(5*960 + 240) // 5040, inside packet 5
		pkts[5].delta = 240      // last packet: positive shorter effective duration
		spec := &mp4Spec{
			movieTimescale:  90000, // non-48k movie timescale
			mediaTime:       int64(S),
			preSkip:         312, // dOps.PreSkip differs from S
			channels:        1,
			inputRate:       48000,
			packets:         pkts,
			chunkPackets:    [][]int{{0, 1}, {2, 3}, {4, 5}},
			chunkMdat:       []int{0, 0, 1},
			segmentMovieDur: (E - S) * 90000 / 48000, // exact integer conversion
		}
		built := buildOpusMP4(t, spec)
		oggData := exportBytes(t, built.data, spec.trackID, 4242)
		pages := parseOggPages(t, oggData)

		// All six packets survive (pre-roll history through the first jump),
		// even though the window only becomes audible inside packet 2.
		c.Check(len(pages) == 2+6, "all pre-roll packets exported, pages=%d", len(pages))
		head := pages[0].packet
		c.Check(binary.LittleEndian.Uint16(head[10:12]) == uint16(S), "pre_skip = S = %d", S)
		c.Check(head[9] == 1, "mono channels")

		var cumulative uint64
		for i := 0; i < 6; i++ {
			d, err := refTocDuration(pkts[i].payload)
			if err != nil {
				t.Fatal(err)
			}
			cumulative += uint64(d)
			page := pages[2+i]
			isLast := i == 5
			want := int64(cumulative)
			if isLast {
				want = int64(E)
			}
			c.Check(page.granule == want, "page %d granule = %d want %d", i, page.granule, want)
			if isLast {
				c.Check(page.headerType&0x04 != 0, "last page EOS")
				c.Check(page.granule == int64(E), "EOS granule trimmed to E")
			} else {
				c.Check(page.headerType&0x04 == 0, "non-last page must not be EOS")
			}
		}
		// Audible length is exactly E-S on the EOS page.
		c.Check(pages[len(pages)-1].granule-int64(S) == int64(E-S), "audible length E-S = %d", E-S)
		// The trimmed last packet is still the full verbatim payload.
		c.Check(bytes.Equal(pages[len(pages)-1].packet, built.packets[5]), "tail-trimmed packet bytes unchanged")

		// Non-divisible segment duration in the movie timescale is rejected.
		bad := *spec
		bad.segmentMovieDur = (E-S)*90000/48000 + 1
		err := ExportOpusOgg(bytes.NewReader(buildOpusMP4(t, &bad).data), io.Discard, spec.trackID, 1)
		c.Check(errors.Is(err, ErrOpusExport), "non-divisible segment rejected: %v", err)

		// Edit end beyond mdhd/stts duration is rejected (48k movie time scale
		// keeps the conversion exact so the end check itself is exercised).
		endOver := *spec
		endOver.movieTimescale = 48000
		endOver.segmentMovieDur = uint64(6*960) - S
		err = ExportOpusOgg(bytes.NewReader(buildOpusMP4(t, &endOver).data), io.Discard, spec.trackID, 1)
		c.Check(errors.Is(err, ErrOpusExport), "edit end beyond media rejected: %v", err)

		// mdhd duration differing from the stts total is rejected.
		mdhdBad := *spec
		mdhdBad.mdhdDurationOver = ptrU64(sttsTotalOf(t, spec) + 1)
		err = ExportOpusOgg(bytes.NewReader(buildOpusMP4(t, &mdhdBad).data), io.Discard, spec.trackID, 1)
		c.Check(errors.Is(err, ErrOpusExport), "mdhd/stts mismatch rejected: %v", err)

		// A non-last packet whose stts delta differs from its TOC is rejected.
		deltaBad := *spec
		deltaBad.packets = append([]testPacket(nil), pkts...)
		deltaBad.packets[2] = testPacket{payload: pkts[2].payload, delta: 480}
		err = ExportOpusOgg(bytes.NewReader(buildOpusMP4(t, &deltaBad).data), io.Discard, spec.trackID, 1)
		c.Check(errors.Is(err, ErrOpusExport), "non-last stts/TOC mismatch rejected: %v", err)

		// S that does not fit the 16-bit OpusHead pre_skip field. Use an
		// exactly convertible one-frame window so divisibility passes first.
		bigS := *spec
		bigS.mediaTime = 0x10000
		bigS.segmentMovieDur = 90000 // 48000 frames at 48 kHz
		err = ExportOpusOgg(bytes.NewReader(buildOpusMP4(t, &bigS).data), io.Discard, spec.trackID, 1)
		c.Check(errors.Is(err, ErrOpusExport), "S > 65535 rejected: %v", err)

		// Window ending on a packet boundary: whole packets at/after E are
		// dropped, pre-roll before S is kept, and the EOS page is the last
		// retained packet with granule exactly E.
		midPkts := make([]testPacket, 8)
		for i := range midPkts {
			midPkts[i] = testPacket{payload: makeOpusPacket(t, 1, 0, 0, 12+i)}
		}
		midS := uint64(960 + 480) // 1440, inside packet 1
		midE := uint64(4 * 960)   // exact boundary after packet 3
		midSpec := &mp4Spec{
			movieTimescale:  24000,
			mediaTime:       int64(midS),
			preSkip:         512,
			channels:        2,
			packets:         midPkts,
			chunkPackets:    [][]int{{0, 1, 2}, {3, 4}, {5, 6, 7}},
			chunkMdat:       []int{0, 0, 1},
			segmentMovieDur: (midE - midS) * 24000 / 48000,
		}
		midPages := parseOggPages(t, exportBytes(t, buildOpusMP4(t, midSpec).data, midSpec.trackID, 55))
		c.Check(len(midPages) == 2+4, "mid window keeps packets 0..3, pages=%d", len(midPages))
		c.Check(binary.LittleEndian.Uint16(midPages[0].packet[10:12]) == uint16(midS), "mid window pre_skip = S")
		for i := 0; i < 4; i++ {
			want := int64(uint64(i+1) * 960)
			isLast := i == 3
			if isLast {
				want = int64(midE)
			}
			c.Check(midPages[2+i].granule == want, "mid window page %d granule = %d want %d", i, midPages[2+i].granule, want)
			c.Check((midPages[2+i].headerType&0x04 != 0) == isLast, "mid window EOS placement (page %d)", i)
		}
	})
}

func sttsTotalOf(t *testing.T, spec *mp4Spec) uint64 {
	t.Helper()
	var total uint64
	for _, pkt := range spec.packets {
		delta := pkt.delta
		if delta == 0 {
			d, err := refTocDuration(pkt.payload)
			if err != nil {
				t.Fatal(err)
			}
			delta = d
		}
		total += uint64(delta)
	}
	return total
}

func ptrU64(v uint64) *uint64 { return &v }
func ptrU32(v uint32) *uint32 { return &v }

// failWriter fails once failAt bytes have been written.
type failWriter struct {
	failAt int
	n      int
}

func (f *failWriter) Write(p []byte) (int, error) {
	if f.n >= f.failAt {
		return 0, errors.New("injected write failure")
	}
	f.n += len(p)
	return len(p), nil
}

// countingWriter records how many bytes reached it.
type countingWriter struct{ n int }

func (c *countingWriter) Write(p []byte) (int, error) {
	c.n += len(p)
	return len(p), nil
}

func TestOpusExportErrors(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		base := func() *mp4Spec {
			s := richSpec()
			s.preSkip = 100
			return s
		}
		export := func(spec *mp4Spec) error {
			data := buildOpusMP4(t, spec).data
			return ExportOpusOgg(bytes.NewReader(data), io.Discard, spec.trackID, 1)
		}
		expectReject := func(name string, mutate func(*mp4Spec)) {
			t.Helper()
			spec := base()
			mutate(spec)
			err := export(spec)
			c.Check(errors.Is(err, ErrOpusExport), "%s: expected ErrOpusExport, got %v", name, err)
		}

		expectReject("fragmented moof", func(s *mp4Spec) { s.addMoof = true })
		expectReject("fragmented mvex", func(s *mp4Spec) { s.addMvex = true })
		expectReject("pssh encryption", func(s *mp4Spec) { s.addPssh = true })
		expectReject("saio encryption", func(s *mp4Spec) { s.addSaio = true })
		expectReject("sinf in opus entry", func(s *mp4Spec) { s.sinfInOpus = true })
		expectReject("enca sample entry", func(s *mp4Spec) { s.encaEntry = true })
		expectReject("ctts present", func(s *mp4Spec) { s.addCtts = true })
		expectReject("two stsd entries", func(s *mp4Spec) { s.stsdTwoEntries = true })
		expectReject("dops version 1", func(s *mp4Spec) { s.dopsVersion = 1 })
		expectReject("mapping family 1", func(s *mp4Spec) { s.dopsFamily = 1 })
		expectReject("channel mismatch", func(s *mp4Spec) { s.entryChannels = 1 })
		expectReject("wrong sample rate", func(s *mp4Spec) { s.entryRate = 44100 << 16 })
		expectReject("mdhd timescale", func(s *mp4Spec) { s.mdhdTimescale = 44100 })
		expectReject("missing elst", func(s *mp4Spec) { s.elstMissing = true })
		expectReject("rate not 1", func(s *mp4Spec) { s.rateInteger = 2 })
		expectReject("negative media time", func(s *mp4Spec) {
			s.mediaTime = -1
			s.elstOverride = &Elst{EntryCount: 1, Entries: []ElstEntry{{
				SegmentDurationV0: 1000, MediaTimeV0: -1, MediaRateInteger: 1,
			}}}
		})
		expectReject("zero segment duration", func(s *mp4Spec) {
			s.elstOverride = &Elst{EntryCount: 1, Entries: []ElstEntry{{
				SegmentDurationV0: 0, MediaTimeV0: 0, MediaRateInteger: 1,
			}}}
		})
		expectReject("two edit entries", func(s *mp4Spec) {
			s.elstOverride = &Elst{EntryCount: 2, Entries: []ElstEntry{
				{SegmentDurationV0: 10, MediaTimeV0: 0, MediaRateInteger: 1},
				{SegmentDurationV0: 10, MediaTimeV0: 10, MediaRateInteger: 1},
			}}
		})
		expectReject("both stco and co64", func(s *mp4Spec) { s.bothCO = true })
		expectReject("neither stco nor co64", func(s *mp4Spec) { s.neitherCO = true })
		expectReject("stsc description index 2", func(s *mp4Spec) { s.stscDescIndex = 2 })
		expectReject("stsc zero samples", func(s *mp4Spec) { s.stscZeroSamples = true })
		expectReject("stsc bad first chunk", func(s *mp4Spec) { s.stscBadFirstChunk = true })
		expectReject("stsz count mismatch", func(s *mp4Spec) { s.stszCountOverride = ptrU32(uint32(len(s.packets)) - 1) })
		expectReject("non-self-contained data ref", func(s *mp4Spec) { s.badDataRefURL = true })
		expectReject("missing dref", func(s *mp4Spec) { s.missingDref = true })
		expectReject("no mdat", func(s *mp4Spec) { s.noMdat = true })

		// Bad byte range: chunk 0 points outside every mdat.
		good := buildOpusMP4(t, base())
		badOffset := uint64(len(good.data) + 100)
		rangeSpec := base()
		rangeSpec.corruptOffset = &badOffset
		err := export(rangeSpec)
		c.Check(errors.Is(err, ErrOpusExport), "out-of-mdat offset rejected: %v", err)

		// Bad offset pointing at the free box between the two mdats.
		freeSpec := base()
		freeBuilt := buildOpusMP4(t, freeSpec)
		freeOffset := freeBuilt.chunkOffsets[2] - 8 // the 8-byte free box header
		freeSpec.corruptOffset = &freeOffset
		err = export(freeSpec)
		c.Check(errors.Is(err, ErrOpusExport), "free-box offset rejected: %v", err)

		// Unknown track ID.
		err = ExportOpusOgg(bytes.NewReader(good.data), io.Discard, 999, 1)
		c.Check(errors.Is(err, ErrOpusExport), "unknown trackID rejected: %v", err)

		// TOC code 3 with missing count byte.
		tocSpec := base()
		tocSpec.packets = []testPacket{{payload: []byte{0x1b}, delta: 960}} // config 3, code 3, no second byte
		tocSpec.chunkPackets = [][]int{{0}}
		tocSpec.chunkMdat = []int{0}
		err = export(tocSpec)
		c.Check(errors.Is(err, ErrOpusExport), "code 3 missing count byte rejected: %v", err)

		// TOC code 3 with zero count.
		tocSpec2 := base()
		tocSpec2.packets = []testPacket{{payload: []byte{0x1b, 0x00, 0xaa}, delta: 960}}
		tocSpec2.chunkPackets = [][]int{{0}}
		tocSpec2.chunkMdat = []int{0}
		err = export(tocSpec2)
		c.Check(errors.Is(err, ErrOpusExport), "code 3 zero count rejected: %v", err)

		// TOC total over 5760: config 3 (2880/frame) with 3 frames.
		tocSpec3 := base()
		tocSpec3.packets = []testPacket{{payload: makePacketWithCode3(t, 3, 3, 32), delta: 960}}
		tocSpec3.chunkPackets = [][]int{{0}}
		tocSpec3.chunkMdat = []int{0}
		err = export(tocSpec3)
		c.Check(errors.Is(err, ErrOpusExport), "TOC > 5760 rejected: %v", err)

		// Packet size boundaries: 65024 accepted, 65025 rejected.
		sizeSpec := base()
		sizeSpec.packets = []testPacket{{payload: makeOpusPacket(t, 1, 0, 0, oggopus.MaxPacketSize)}}
		sizeSpec.chunkPackets = [][]int{{0}}
		sizeSpec.chunkMdat = []int{0}
		c.Check(export(sizeSpec) == nil, "65024-byte packet accepted")
		sizeSpec.packets[0].payload = append(sizeSpec.packets[0].payload, 0)
		err = export(sizeSpec)
		c.Check(errors.Is(err, ErrOpusExport), "65025-byte packet rejected: %v", err)

		// Rejected input must not produce a single output byte.
		cw := &countingWriter{}
		rejSpec := base()
		rejSpec.addCtts = true
		err = ExportOpusOgg(bytes.NewReader(buildOpusMP4(t, rejSpec).data), cw, rejSpec.trackID, 1)
		c.Check(errors.Is(err, ErrOpusExport), "ctts rejection error")
		c.Check(cw.n == 0, "no bytes written before validation completes, got %d", cw.n)

		// Write failure during the page-write phase is propagated as-is.
		fw := &failWriter{failAt: 40}
		err = ExportOpusOgg(bytes.NewReader(good.data), fw, base().trackID, 1)
		c.Check(err != nil && !errors.Is(err, ErrOpusExport), "write failure propagated, got %v", err)

		// Reader position is restored on success and on every failure.
		origin := int64(123)
		r := bytes.NewReader(good.data)
		if _, err := r.Seek(origin, io.SeekStart); err != nil {
			t.Fatal(err)
		}
		if err := ExportOpusOgg(r, io.Discard, base().trackID, 77); err != nil {
			t.Fatal(err)
		}
		pos, _ := r.Seek(0, io.SeekCurrent)
		c.Check(pos == origin, "reader restored after success, pos=%d", pos)

		r2 := bytes.NewReader(good.data)
		if _, err := r2.Seek(origin, io.SeekStart); err != nil {
			t.Fatal(err)
		}
		_ = ExportOpusOgg(r2, io.Discard, 999, 77) // unknown track, fails
		pos2, _ := r2.Seek(0, io.SeekCurrent)
		c.Check(pos2 == origin, "reader restored after failure, pos=%d", pos2)

		r3 := bytes.NewReader(good.data)
		if _, err := r3.Seek(origin, io.SeekStart); err != nil {
			t.Fatal(err)
		}
		_ = ExportOpusOgg(r3, &failWriter{failAt: 5}, base().trackID, 77) // write failure
		pos3, _ := r3.Seek(0, io.SeekCurrent)
		c.Check(pos3 == origin, "reader restored after write failure, pos=%d", pos3)
	})
}
