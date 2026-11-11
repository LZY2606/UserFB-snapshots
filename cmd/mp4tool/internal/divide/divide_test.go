package divide

import (
	"errors"
	"fmt"
	"io"
	"io/ioutil"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	"github.com/abema/go-mp4"
)

// This test builds small fragmented MP4 files locally. It never decodes
// H.264/AAC and never depends on FFmpeg or any external download.

const (
	kindVideo = "video"
	kindAudio = "audio"
)

type inputTrack struct {
	id        uint32
	kind      string
	timescale uint32
	width     uint16
	height    uint16
	profile   uint8
	compat    uint8
	level     uint8
}

type inputFragment struct {
	trackID  uint32
	duration uint32
	payload  []byte
}

type inputFile struct {
	tracks    []inputTrack
	trakOrder []uint32
	fragments []inputFragment
}

// seekBuffer is an in-memory io.WriteSeeker used to assemble MP4 boxes.
type seekBuffer struct {
	buf []byte
	pos int64
}

func (m *seekBuffer) Write(p []byte) (int, error) {
	end := m.pos + int64(len(p))
	if end > int64(len(m.buf)) {
		grown := make([]byte, end)
		copy(grown, m.buf)
		m.buf = grown
	}
	copy(m.buf[m.pos:], p)
	m.pos = end
	return len(p), nil
}

func (m *seekBuffer) Seek(offset int64, whence int) (int64, error) {
	var p int64
	switch whence {
	case io.SeekStart:
		p = offset
	case io.SeekCurrent:
		p = m.pos + offset
	case io.SeekEnd:
		p = int64(len(m.buf)) + offset
	}
	if p < 0 {
		return 0, errors.New("negative seek position")
	}
	m.pos = p
	return p, nil
}

func (m *seekBuffer) Bytes() []byte { return m.buf }

func fullBoxFlags(flags uint32) [3]byte {
	return [3]byte{byte(flags >> 16), byte(flags >> 8), byte(flags)}
}

var identityMatrix = [9]int32{
	0x00010000, 0, 0,
	0, 0x00010000, 0,
	0, 0, 0x40000000,
}

func startBox(t *testing.T, w *mp4.Writer, typ mp4.BoxType) {
	t.Helper()
	if _, err := w.StartBox(&mp4.BoxInfo{Type: typ}); err != nil {
		t.Fatalf("StartBox %s: %v", typ, err)
	}
}

func endBox(t *testing.T, w *mp4.Writer) {
	t.Helper()
	if _, err := w.EndBox(); err != nil {
		t.Fatalf("EndBox: %v", err)
	}
}

func marshalBox(t *testing.T, w *mp4.Writer, b mp4.IImmutableBox) {
	t.Helper()
	if _, err := mp4.Marshal(w, b, mp4.Context{}); err != nil {
		t.Fatalf("Marshal %s: %v", b.GetType(), err)
	}
}

func buildFTYP(t *testing.T, w *mp4.Writer) {
	startBox(t, w, mp4.BoxTypeFtyp())
	marshalBox(t, w, &mp4.Ftyp{
		MajorBrand:   [4]byte{'i', 's', 'o', 'm'},
		MinorVersion: 0x200,
		CompatibleBrands: []mp4.CompatibleBrandElem{
			{CompatibleBrand: [4]byte{'i', 's', 'o', 'm'}},
			{CompatibleBrand: [4]byte{'a', 'v', 'c', '1'}},
		},
	})
	endBox(t, w)
}

func buildMVHD(t *testing.T, w *mp4.Writer) {
	startBox(t, w, mp4.BoxTypeMvhd())
	marshalBox(t, w, &mp4.Mvhd{
		FullBox:   mp4.FullBox{Version: 0},
		Timescale: 1000,
		Rate:      0x00010000,
		Volume:    0x0100,
		Matrix:    identityMatrix,
	})
	endBox(t, w)
}

func buildEmptyTable(t *testing.T, w *mp4.Writer, typ mp4.BoxType, box mp4.IImmutableBox) {
	startBox(t, w, typ)
	marshalBox(t, w, box)
	endBox(t, w)
}

func buildVideoTrak(t *testing.T, w *mp4.Writer, tr inputTrack) {
	startBox(t, w, mp4.BoxTypeTrak())

	startBox(t, w, mp4.BoxTypeTkhd())
	marshalBox(t, w, &mp4.Tkhd{
		FullBox: mp4.FullBox{Version: 0, Flags: fullBoxFlags(0x000007)},
		TrackID: tr.id,
		Matrix:  identityMatrix,
		Width:   uint32(tr.width) << 16,
		Height:  uint32(tr.height) << 16,
	})
	endBox(t, w)

	startBox(t, w, mp4.BoxTypeMdia())

	startBox(t, w, mp4.BoxTypeMdhd())
	marshalBox(t, w, &mp4.Mdhd{
		FullBox:   mp4.FullBox{Version: 0},
		Timescale: tr.timescale,
	})
	endBox(t, w)

	startBox(t, w, mp4.BoxTypeHdlr())
	marshalBox(t, w, &mp4.Hdlr{
		FullBox:     mp4.FullBox{},
		HandlerType: [4]byte{'v', 'i', 'd', 'e'},
		Name:        "VideoHandler",
	})
	endBox(t, w)

	startBox(t, w, mp4.BoxTypeMinf())

	startBox(t, w, mp4.BoxTypeVmhd())
	marshalBox(t, w, &mp4.Vmhd{FullBox: mp4.FullBox{Flags: fullBoxFlags(0x000001)}})
	endBox(t, w)

	startBox(t, w, mp4.BoxTypeDinf())
	startBox(t, w, mp4.BoxTypeDref())
	marshalBox(t, w, &mp4.Dref{FullBox: mp4.FullBox{}, EntryCount: 1})
	startBox(t, w, mp4.BoxTypeUrl())
	marshalBox(t, w, &mp4.Url{FullBox: mp4.FullBox{Flags: fullBoxFlags(mp4.UrlSelfContained)}})
	endBox(t, w)
	endBox(t, w)
	endBox(t, w)

	startBox(t, w, mp4.BoxTypeStbl())

	startBox(t, w, mp4.BoxTypeStsd())
	marshalBox(t, w, &mp4.Stsd{FullBox: mp4.FullBox{}, EntryCount: 1})
	startBox(t, w, mp4.BoxTypeAvc1())
	marshalBox(t, w, &mp4.VisualSampleEntry{
		SampleEntry: mp4.SampleEntry{
			AnyTypeBox:         mp4.AnyTypeBox{Type: mp4.BoxTypeAvc1()},
			DataReferenceIndex: 1,
		},
		Width:           tr.width,
		Height:          tr.height,
		Horizresolution: 0x00480000,
		Vertresolution:  0x00480000,
		FrameCount:      1,
		Depth:           0x0018,
		PreDefined3:     -1,
	})
	startBox(t, w, mp4.BoxTypeAvcC())
	marshalBox(t, w, &mp4.AVCDecoderConfiguration{
		AnyTypeBox:           mp4.AnyTypeBox{Type: mp4.BoxTypeAvcC()},
		ConfigurationVersion: 1,
		Profile:              tr.profile,
		ProfileCompatibility: tr.compat,
		Level:                tr.level,
		LengthSizeMinusOne:   3,
	})
	endBox(t, w)
	endBox(t, w) // avc1
	endBox(t, w) // stsd

	buildEmptyTable(t, w, mp4.BoxTypeStts(), &mp4.Stts{FullBox: mp4.FullBox{}})
	buildEmptyTable(t, w, mp4.BoxTypeStsc(), &mp4.Stsc{FullBox: mp4.FullBox{}})
	buildEmptyTable(t, w, mp4.BoxTypeStsz(), &mp4.Stsz{FullBox: mp4.FullBox{}})
	buildEmptyTable(t, w, mp4.BoxTypeStco(), &mp4.Stco{FullBox: mp4.FullBox{}})

	endBox(t, w) // stbl
	endBox(t, w) // minf
	endBox(t, w) // mdia
	endBox(t, w) // trak
}

func buildAudioTrak(t *testing.T, w *mp4.Writer, tr inputTrack) {
	startBox(t, w, mp4.BoxTypeTrak())

	startBox(t, w, mp4.BoxTypeTkhd())
	marshalBox(t, w, &mp4.Tkhd{
		FullBox: mp4.FullBox{Version: 0, Flags: fullBoxFlags(0x000007)},
		TrackID: tr.id,
		Volume:  0x0100,
		Matrix:  identityMatrix,
	})
	endBox(t, w)

	startBox(t, w, mp4.BoxTypeMdia())

	startBox(t, w, mp4.BoxTypeMdhd())
	marshalBox(t, w, &mp4.Mdhd{
		FullBox:   mp4.FullBox{Version: 0},
		Timescale: tr.timescale,
	})
	endBox(t, w)

	startBox(t, w, mp4.BoxTypeHdlr())
	marshalBox(t, w, &mp4.Hdlr{
		FullBox:     mp4.FullBox{},
		HandlerType: [4]byte{'s', 'o', 'u', 'n'},
		Name:        "SoundHandler",
	})
	endBox(t, w)

	startBox(t, w, mp4.BoxTypeMinf())

	startBox(t, w, mp4.BoxTypeSmhd())
	marshalBox(t, w, &mp4.Smhd{FullBox: mp4.FullBox{}})
	endBox(t, w)

	startBox(t, w, mp4.BoxTypeDinf())
	startBox(t, w, mp4.BoxTypeDref())
	marshalBox(t, w, &mp4.Dref{FullBox: mp4.FullBox{}, EntryCount: 1})
	startBox(t, w, mp4.BoxTypeUrl())
	marshalBox(t, w, &mp4.Url{FullBox: mp4.FullBox{Flags: fullBoxFlags(mp4.UrlSelfContained)}})
	endBox(t, w)
	endBox(t, w)
	endBox(t, w)

	startBox(t, w, mp4.BoxTypeStbl())

	startBox(t, w, mp4.BoxTypeStsd())
	marshalBox(t, w, &mp4.Stsd{FullBox: mp4.FullBox{}, EntryCount: 1})
	startBox(t, w, mp4.BoxTypeMp4a())
	marshalBox(t, w, &mp4.AudioSampleEntry{
		SampleEntry: mp4.SampleEntry{
			AnyTypeBox:         mp4.AnyTypeBox{Type: mp4.BoxTypeMp4a()},
			DataReferenceIndex: 1,
		},
		ChannelCount: 2,
		SampleSize:   16,
		SampleRate:   44100 << 16,
	})
	startBox(t, w, mp4.BoxTypeEsds())
	marshalBox(t, w, &mp4.Esds{
		FullBox: mp4.FullBox{},
		Descriptors: []mp4.Descriptor{
			{Tag: mp4.ESDescrTag, Size: 3, ESDescriptor: &mp4.ESDescriptor{ESID: 2}},
			{Tag: mp4.DecoderConfigDescrTag, Size: 13, DecoderConfigDescriptor: &mp4.DecoderConfigDescriptor{
				ObjectTypeIndication: 0x40,
				StreamType:           5,
				Reserved:             true,
				MaxBitrate:           96000,
				AvgBitrate:           96000,
			}},
			{Tag: mp4.DecSpecificInfoTag, Size: 2, Data: []byte{0x12, 0x10}},
			{Tag: mp4.SLConfigDescrTag, Size: 1, Data: []byte{0x02}},
		},
	})
	endBox(t, w) // esds
	endBox(t, w) // mp4a
	endBox(t, w) // stsd

	buildEmptyTable(t, w, mp4.BoxTypeStts(), &mp4.Stts{FullBox: mp4.FullBox{}})
	buildEmptyTable(t, w, mp4.BoxTypeStsc(), &mp4.Stsc{FullBox: mp4.FullBox{}})
	buildEmptyTable(t, w, mp4.BoxTypeStsz(), &mp4.Stsz{FullBox: mp4.FullBox{}})
	buildEmptyTable(t, w, mp4.BoxTypeStco(), &mp4.Stco{FullBox: mp4.FullBox{}})

	endBox(t, w) // stbl
	endBox(t, w) // minf
	endBox(t, w) // mdia
	endBox(t, w) // trak
}

func buildMvex(t *testing.T, w *mp4.Writer, tracks []inputTrack) {
	startBox(t, w, mp4.BoxTypeMvex())
	for _, tr := range tracks {
		startBox(t, w, mp4.BoxTypeTrex())
		marshalBox(t, w, &mp4.Trex{
			FullBox:                       mp4.FullBox{},
			TrackID:                       tr.id,
			DefaultSampleDescriptionIndex: 1,
			DefaultSampleDuration:         0,
			DefaultSampleSize:             0,
			DefaultSampleFlags:            0,
		})
		endBox(t, w)
	}
	endBox(t, w)
}

// buildFragment writes one moof (single traf) immediately followed by one
// mdat carrying the recognizable payload.
func buildFragment(t *testing.T, w *mp4.Writer, seq uint32, baseTime uint64, f inputFragment) {
	startBox(t, w, mp4.BoxTypeMoof())

	startBox(t, w, mp4.BoxTypeMfhd())
	marshalBox(t, w, &mp4.Mfhd{FullBox: mp4.FullBox{}, SequenceNumber: seq})
	endBox(t, w)

	startBox(t, w, mp4.BoxTypeTraf())

	startBox(t, w, mp4.BoxTypeTfhd())
	marshalBox(t, w, &mp4.Tfhd{
		FullBox: mp4.FullBox{Flags: fullBoxFlags(0x020000)},
		TrackID: f.trackID,
	})
	endBox(t, w)

	startBox(t, w, mp4.BoxTypeTfdt())
	marshalBox(t, w, &mp4.Tfdt{
		FullBox:               mp4.FullBox{Version: 1},
		BaseMediaDecodeTimeV1: baseTime,
	})
	endBox(t, w)

	startBox(t, w, mp4.BoxTypeTrun())
	marshalBox(t, w, &mp4.Trun{
		FullBox:     mp4.FullBox{Flags: fullBoxFlags(0x000300)}, // duration + size
		SampleCount: 1,
		Entries: []mp4.TrunEntry{{
			SampleDuration: f.duration,
			SampleSize:     uint32(len(f.payload)),
		}},
	})
	endBox(t, w)

	endBox(t, w) // traf
	endBox(t, w) // moof

	startBox(t, w, mp4.BoxTypeMdat())
	if _, err := w.Write(f.payload); err != nil {
		t.Fatalf("write mdat: %v", err)
	}
	endBox(t, w)
}

func buildInput(t *testing.T, in inputFile) []byte {
	t.Helper()
	byID := make(map[uint32]inputTrack, len(in.tracks))
	for _, tr := range in.tracks {
		byID[tr.id] = tr
	}

	buf := new(seekBuffer)
	w := mp4.NewWriter(buf)

	buildFTYP(t, w)

	startBox(t, w, mp4.BoxTypeMoov())
	buildMVHD(t, w)
	for _, id := range in.trakOrder {
		tr := byID[id]
		if tr.kind == kindAudio {
			buildAudioTrak(t, w, tr)
		} else {
			buildVideoTrak(t, w, tr)
		}
	}
	buildMvex(t, w, in.tracks)
	endBox(t, w) // moov

	baseTime := make(map[uint32]uint64)
	for seq, f := range in.fragments {
		buildFragment(t, w, uint32(seq+1), baseTime[f.trackID], f)
		baseTime[f.trackID] += uint64(f.duration)
	}

	return buf.Bytes()
}

func writeInput(t *testing.T, name string, data []byte) string {
	t.Helper()
	dir, err := ioutil.TempDir("", "divide-"+name+"-")
	if err != nil {
		t.Fatalf("TempDir: %v", err)
	}
	t.Cleanup(func() { os.RemoveAll(dir) })
	p := filepath.Join(dir, "input.mp4")
	if err := ioutil.WriteFile(p, data, 0666); err != nil {
		t.Fatalf("WriteFile: %v", err)
	}
	return p
}

func runDivide(t *testing.T, input string) string {
	t.Helper()
	out := filepath.Join(filepath.Dir(input), "out")
	if err := divide(input, out); err != nil {
		t.Fatalf("divide failed: %v", err)
	}
	return out
}

func openFile(t *testing.T, p string) *os.File {
	t.Helper()
	f, err := os.Open(p)
	if err != nil {
		t.Fatalf("open %s: %v", p, err)
	}
	return f
}

func extractBoxes(t *testing.T, f io.ReadSeeker, path mp4.BoxPath) []*mp4.BoxInfo {
	t.Helper()
	bis, err := mp4.ExtractBox(f, nil, path)
	if err != nil {
		t.Fatalf("ExtractBox %v: %v", path, err)
	}
	return bis
}

func extractPayloads(t *testing.T, f io.ReadSeeker, path mp4.BoxPath) []*mp4.BoxInfoWithPayload {
	t.Helper()
	bps, err := mp4.ExtractBoxWithPayload(f, nil, path)
	if err != nil {
		t.Fatalf("ExtractBoxWithPayload %v: %v", path, err)
	}
	return bps
}

func readTkhd(t *testing.T, f io.ReadSeeker) *mp4.Tkhd {
	bps := extractPayloads(t, f, mp4.BoxPath{mp4.BoxTypeMoov(), mp4.BoxTypeTrak(), mp4.BoxTypeTkhd()})
	if len(bps) != 1 {
		t.Fatalf("expected exactly one tkhd, got %d", len(bps))
	}
	return bps[0].Payload.(*mp4.Tkhd)
}

func readTrexIDs(t *testing.T, f io.ReadSeeker) []uint32 {
	bps := extractPayloads(t, f, mp4.BoxPath{mp4.BoxTypeMoov(), mp4.BoxTypeMvex(), mp4.BoxTypeTrex()})
	ids := make([]uint32, 0, len(bps))
	for _, bp := range bps {
		ids = append(ids, bp.Payload.(*mp4.Trex).TrackID)
	}
	return ids
}

func readAVC(t *testing.T, f io.ReadSeeker) (*mp4.VisualSampleEntry, *mp4.AVCDecoderConfiguration) {
	vps := extractPayloads(t, f, mp4.BoxPath{
		mp4.BoxTypeMoov(), mp4.BoxTypeTrak(), mp4.BoxTypeMdia(), mp4.BoxTypeMinf(),
		mp4.BoxTypeStbl(), mp4.BoxTypeStsd(), mp4.BoxTypeAvc1(),
	})
	if len(vps) != 1 {
		t.Fatalf("expected one avc1, got %d", len(vps))
	}
	avc1 := vps[0].Payload.(*mp4.VisualSampleEntry)

	cps := extractPayloads(t, f, mp4.BoxPath{
		mp4.BoxTypeMoov(), mp4.BoxTypeTrak(), mp4.BoxTypeMdia(), mp4.BoxTypeMinf(),
		mp4.BoxTypeStbl(), mp4.BoxTypeStsd(), mp4.BoxTypeAvc1(), mp4.BoxTypeAvcC(),
	})
	if len(cps) != 1 {
		t.Fatalf("expected one avcC, got %d", len(cps))
	}
	return avc1, cps[0].Payload.(*mp4.AVCDecoderConfiguration)
}

func hasMp4a(t *testing.T, f io.ReadSeeker) bool {
	return len(extractBoxes(t, f, mp4.BoxPath{
		mp4.BoxTypeMoov(), mp4.BoxTypeTrak(), mp4.BoxTypeMdia(), mp4.BoxTypeMinf(),
		mp4.BoxTypeStbl(), mp4.BoxTypeStsd(), mp4.BoxTypeMp4a(),
	})) == 1
}

func readTfhd(t *testing.T, f io.ReadSeeker) *mp4.Tfhd {
	bps := extractPayloads(t, f, mp4.BoxPath{mp4.BoxTypeMoof(), mp4.BoxTypeTraf(), mp4.BoxTypeTfhd()})
	if len(bps) != 1 {
		t.Fatalf("expected one tfhd, got %d", len(bps))
	}
	return bps[0].Payload.(*mp4.Tfhd)
}

func readSingleMdatPayload(t *testing.T, f io.ReadSeeker) []byte {
	t.Helper()
	bis := extractBoxes(t, f, mp4.BoxPath{mp4.BoxTypeMdat()})
	if len(bis) != 1 {
		t.Fatalf("expected one mdat, got %d", len(bis))
	}
	bi := bis[0]
	if _, err := bi.SeekToPayload(f); err != nil {
		t.Fatalf("seek mdat: %v", err)
	}
	data := make([]byte, bi.Size-bi.HeaderSize)
	if _, err := io.ReadFull(f, data); err != nil {
		t.Fatalf("read mdat payload: %v", err)
	}
	return data
}

func readPlaylist(t *testing.T, p string) string {
	t.Helper()
	b, err := ioutil.ReadFile(p)
	if err != nil {
		t.Fatalf("read playlist %s: %v", p, err)
	}
	return string(b)
}

func extinfDurations(t *testing.T, playlist string) []float64 {
	t.Helper()
	var out []float64
	for _, line := range strings.Split(playlist, "\n") {
		if strings.HasPrefix(line, "#EXTINF:") {
			num := strings.TrimSuffix(strings.TrimPrefix(line, "#EXTINF:"), ",")
			v, err := strconv.ParseFloat(num, 64)
			if err != nil {
				t.Fatalf("parse EXTINF %q: %v", line, err)
			}
			out = append(out, v)
		}
	}
	return out
}

func approxEqual(a, b float64) bool {
	d := a - b
	if d < 0 {
		d = -d
	}
	// playlists are serialized with %f (six decimal places)
	return d < 1e-6
}

func repeatByte(b byte, n int) []byte {
	out := make([]byte, n)
	for i := range out {
		out[i] = b
	}
	return out
}

// twoVideoInputSpec builds the shared two-video specification (trackID 3 and
// 9), with different dimensions, timescales and recognizable payloads.
func twoVideoInputSpec(trakOrder []uint32) inputFile {
	tracks := []inputTrack{
		{id: 3, kind: kindVideo, timescale: 90000, width: 320, height: 180, profile: 0x42, compat: 0xC0, level: 0x1E},
		{id: 9, kind: kindVideo, timescale: 30000, width: 640, height: 360, profile: 0x64, compat: 0x00, level: 0x28},
	}
	// moof order is interleaved and deliberately not aligned with trak order.
	fragments := []inputFragment{
		{trackID: 9, duration: 1000, payload: repeatByte(0x39, 20)},
		{trackID: 3, duration: 3000, payload: repeatByte(0x33, 16)},
		{trackID: 9, duration: 2000, payload: repeatByte(0x39, 28)},
		{trackID: 3, duration: 4500, payload: repeatByte(0x33, 24)},
	}
	return inputFile{tracks: tracks, trakOrder: trakOrder, fragments: fragments}
}

// expectedPayloads maps trackID to the per-segment mdat payload in segment
// order, independent of moof interleaving.
func expectedPayloads(in inputFile) map[uint32][][]byte {
	out := make(map[uint32][][]byte)
	for _, f := range in.fragments {
		out[f.trackID] = append(out[f.trackID], f.payload)
	}
	return out
}

func verifyVideoTrackOutput(t *testing.T, out string, tr inputTrack, expected [][]byte, expectedDur []float64) uint64 {
	t.Helper()
	dir := filepath.Join(out, fmt.Sprintf("video_%d", tr.id))

	// ---- init.mp4: one trak, correct tkhd, own trex, own avc1/avcC ----
	initF := openFile(t, filepath.Join(dir, "init.mp4"))
	defer initF.Close()

	if traks := extractBoxes(t, initF, mp4.BoxPath{mp4.BoxTypeMoov(), mp4.BoxTypeTrak()}); len(traks) != 1 {
		t.Fatalf("track %d init: expected 1 trak, got %d", tr.id, len(traks))
	}
	tkhd := readTkhd(t, initF)
	if tkhd.TrackID != tr.id {
		t.Fatalf("track %d init: tkhd TrackID=%d", tr.id, tkhd.TrackID)
	}
	if tkhd.GetWidthInt() != tr.width || tkhd.GetHeightInt() != tr.height {
		t.Fatalf("track %d init: tkhd size=%dx%d want %dx%d", tr.id, tkhd.GetWidthInt(), tkhd.GetHeightInt(), tr.width, tr.height)
	}

	trexIDs := readTrexIDs(t, initF)
	if len(trexIDs) != 1 || trexIDs[0] != tr.id {
		t.Fatalf("track %d init: trex IDs=%v, want only [%d]", tr.id, trexIDs, tr.id)
	}

	avc1, avcC := readAVC(t, initF)
	if avc1.Width != tr.width || avc1.Height != tr.height {
		t.Fatalf("track %d init: avc1 size=%dx%d want %dx%d", tr.id, avc1.Width, avc1.Height, tr.width, tr.height)
	}
	if avcC.Profile != tr.profile || avcC.ProfileCompatibility != tr.compat || avcC.Level != tr.level {
		t.Fatalf("track %d init: avcC=%02x%02x%02x want %02x%02x%02x",
			tr.id, avcC.Profile, avcC.ProfileCompatibility, avcC.Level, tr.profile, tr.compat, tr.level)
	}
	if hasMp4a(t, initF) {
		t.Fatalf("track %d init must not contain mp4a", tr.id)
	}

	// ---- media segments: own tfhd and byte-identical mdat payload ----
	var maxBandwidth uint64
	for i, wantPayload := range expected {
		segPath := filepath.Join(dir, segmentFileName(i))
		segF := openFile(t, segPath)
		if moofs := extractBoxes(t, segF, mp4.BoxPath{mp4.BoxTypeMoof()}); len(moofs) != 1 {
			t.Fatalf("track %d segment %d: expected 1 moof, got %d", tr.id, i, len(moofs))
		}
		if trafs := extractBoxes(t, segF, mp4.BoxPath{mp4.BoxTypeMoof(), mp4.BoxTypeTraf()}); len(trafs) != 1 {
			t.Fatalf("track %d segment %d: expected 1 traf, got %d", tr.id, i, len(trafs))
		}
		tfhd := readTfhd(t, segF)
		if tfhd.TrackID != tr.id {
			t.Fatalf("track %d segment %d: tfhd TrackID=%d", tr.id, i, tfhd.TrackID)
		}
		gotPayload := readSingleMdatPayload(t, segF)
		if string(gotPayload) != string(wantPayload) {
			t.Fatalf("track %d segment %d: mdat payload mismatch", tr.id, i)
		}
		// bandwidth is computed from this track's own fragment size/duration
		mdatBis := extractBoxes(t, segF, mp4.BoxPath{mp4.BoxTypeMdat()})
		mdatSize := mdatBis[0].Size
		bw := uint64(float64(mdatSize) * 8 / expectedDur[i])
		if bw > maxBandwidth {
			maxBandwidth = bw
		}
		segF.Close()
	}

	// ---- media playlist: contiguous numbering from 0 and own durations ----
	pl := readPlaylist(t, filepath.Join(dir, "playlist.m3u8"))
	if !strings.Contains(pl, "#EXT-X-MAP:URI=\"init.mp4\"") {
		t.Fatalf("track %d playlist missing EXT-X-MAP", tr.id)
	}
	gotDur := extinfDurations(t, pl)
	if len(gotDur) != len(expectedDur) {
		t.Fatalf("track %d playlist: got %d EXTINF, want %d", tr.id, len(gotDur), len(expectedDur))
	}
	for i := range gotDur {
		if !approxEqual(gotDur[i], expectedDur[i]) {
			t.Fatalf("track %d playlist segment %d duration=%v want %v", tr.id, i, gotDur[i], expectedDur[i])
		}
	}
	for i := range expected {
		if !strings.Contains(pl, segmentFileName(i)+"\n") {
			t.Fatalf("track %d playlist missing segment %d", tr.id, i)
		}
	}
	if !strings.HasSuffix(strings.TrimSpace(pl), "#EXT-X-ENDLIST") {
		t.Fatalf("track %d playlist missing ENDLIST", tr.id)
	}
	return maxBandwidth
}

func TestDivideTrackIsolation(t *testing.T) {
	t.Run("two video tracks are isolated by trackID regardless of order", func(t *testing.T) {
		spec := twoVideoInputSpec(nil)

		orderA := []uint32{9, 3} // trak order swapped vs ascending trackID
		orderB := []uint32{3, 9}
		inputA := writeInput(t, "a", buildInput(t, inputFile{
			tracks: spec.tracks, trakOrder: orderA, fragments: spec.fragments,
		}))
		inputB := writeInput(t, "b", buildInput(t, inputFile{
			tracks: spec.tracks, trakOrder: orderB, fragments: spec.fragments,
		}))
		outA := runDivide(t, inputA)
		outB := runDivide(t, inputB)

		// only video_3 and video_9 must exist
		entries, err := ioutil.ReadDir(outA)
		if err != nil {
			t.Fatalf("ReadDir: %v", err)
		}
		var names []string
		for _, e := range entries {
			names = append(names, e.Name())
		}
		sortStrings(names)
		if strings.Join(names, ",") != "playlist.m3u8,video_3,video_9" {
			t.Fatalf("unexpected top-level entries: %v", names)
		}

		want := expectedPayloads(spec)
		bw3 := verifyVideoTrackOutput(t, outA, spec.tracks[0], want[3],
			[]float64{float64(3000) / 90000, float64(4500) / 90000})
		bw9 := verifyVideoTrackOutput(t, outA, spec.tracks[1], want[9],
			[]float64{float64(1000) / 30000, float64(2000) / 30000})

		// swapped trak order must produce identical ownership and outputs
		for _, id := range []uint32{3, 9} {
			dir := fmt.Sprintf("video_%d", id)
			for _, seg := range []string{"init.mp4", "0.mp4", "1.mp4", "playlist.m3u8"} {
				a, errA := ioutil.ReadFile(filepath.Join(outA, dir, seg))
				b, errB := ioutil.ReadFile(filepath.Join(outB, dir, seg))
				if errA != nil || errB != nil {
					t.Fatalf("read %s/%s: %v %v", dir, seg, errA, errB)
				}
				if string(a) != string(b) {
					t.Fatalf("%s/%s differs between trak orders", dir, seg)
				}
			}
		}

		// ---- master playlist: ascending trackID, own codec/resolution, no mp4a ----
		master := readPlaylist(t, filepath.Join(outA, "playlist.m3u8"))
		if strings.Contains(master, "mp4a") {
			t.Fatalf("master playlist must not advertise mp4a without audio: %s", master)
		}
		if strings.Contains(master, "EXT-X-MEDIA:TYPE=AUDIO") {
			t.Fatalf("master playlist must not list an audio group")
		}
		idx3 := strings.Index(master, "video_3/playlist.m3u8")
		idx9 := strings.Index(master, "video_9/playlist.m3u8")
		if idx3 < 0 || idx9 < 0 || idx3 >= idx9 {
			t.Fatalf("master playlist must list video_3 before video_9: %s", master)
		}
		if !strings.Contains(master, `CODECS="avc1.42C01E",RESOLUTION=320x180`) {
			t.Fatalf("master playlist missing track 3 codec/resolution: %s", master)
		}
		if !strings.Contains(master, `CODECS="avc1.640028",RESOLUTION=640x360`) {
			t.Fatalf("master playlist missing track 9 codec/resolution: %s", master)
		}
		// each variant must advertise its own peak bandwidth, computed from
		// that track's fragment sizes and timescale only.
		if bw3 == bw9 {
			t.Fatalf("tracks must have distinct bandwidths, got %d for both", bw3)
		}
		if !strings.Contains(master, fmt.Sprintf("BANDWIDTH=%d,", bw3)) {
			t.Fatalf("master playlist missing track 3 BANDWIDTH=%d: %s", bw3, master)
		}
		if !strings.Contains(master, fmt.Sprintf("BANDWIDTH=%d,", bw9)) {
			t.Fatalf("master playlist missing track 9 BANDWIDTH=%d: %s", bw9, master)
		}
	})

	t.Run("single video with one audio keeps legacy layout", func(t *testing.T) {
		tracks := []inputTrack{
			{id: 1, kind: kindVideo, timescale: 24000, width: 160, height: 120, profile: 0x4D, compat: 0x40, level: 0x1F},
			{id: 2, kind: kindAudio, timescale: 44100},
		}
		fragments := []inputFragment{
			{trackID: 1, duration: 1000, payload: repeatByte(0x31, 18)},
			{trackID: 2, duration: 1024, payload: repeatByte(0xAA, 12)},
			{trackID: 1, duration: 2000, payload: repeatByte(0x31, 22)},
			{trackID: 2, duration: 1024, payload: repeatByte(0xAA, 12)},
		}
		in := inputFile{tracks: tracks, trakOrder: []uint32{1, 2}, fragments: fragments}
		out := runDivide(t, writeInput(t, "legacy", buildInput(t, in)))

		entries, err := ioutil.ReadDir(out)
		if err != nil {
			t.Fatalf("ReadDir: %v", err)
		}
		var names []string
		for _, e := range entries {
			names = append(names, e.Name())
		}
		sortStrings(names)
		if strings.Join(names, ",") != "audio,playlist.m3u8,video" {
			t.Fatalf("legacy layout must use video/ and audio/: %v", names)
		}

		// video init carries avc1 only; audio init carries mp4a only
		vInit := openFile(t, filepath.Join(out, "video", "init.mp4"))
		if len(extractBoxes(t, vInit, mp4.BoxPath{mp4.BoxTypeMoov(), mp4.BoxTypeTrak()})) != 1 {
			t.Fatalf("video init must have one trak")
		}
		if readTkhd(t, vInit).TrackID != 1 {
			t.Fatalf("video init tkhd must be track 1")
		}
		if hasMp4a(t, vInit) {
			t.Fatalf("video init must not contain mp4a")
		}
		vInit.Close()

		aInit := openFile(t, filepath.Join(out, "audio", "init.mp4"))
		if readTkhd(t, aInit).TrackID != 2 {
			t.Fatalf("audio init tkhd must be track 2")
		}
		if !hasMp4a(t, aInit) {
			t.Fatalf("audio init must contain mp4a")
		}
		if len(extractBoxes(t, aInit, mp4.BoxPath{
			mp4.BoxTypeMoov(), mp4.BoxTypeTrak(), mp4.BoxTypeMdia(), mp4.BoxTypeMinf(),
			mp4.BoxTypeStbl(), mp4.BoxTypeStsd(), mp4.BoxTypeAvc1(),
		})) != 0 {
			t.Fatalf("audio init must not contain avc1")
		}
		aInit.Close()

		// audio segments keep their own tfhd and payload
		for i := 0; i < 2; i++ {
			segF := openFile(t, filepath.Join(out, "audio", segmentFileName(i)))
			if readTfhd(t, segF).TrackID != 2 {
				t.Fatalf("audio segment %d tfhd must be track 2", i)
			}
			if string(readSingleMdatPayload(t, segF)) != string(repeatByte(0xAA, 12)) {
				t.Fatalf("audio segment %d payload mismatch", i)
			}
			segF.Close()
		}
		audioPL := readPlaylist(t, filepath.Join(out, "audio", "playlist.m3u8"))
		gotDur := extinfDurations(t, audioPL)
		wantDur := float64(1024) / 44100
		if len(gotDur) != 2 || !approxEqual(gotDur[0], wantDur) || !approxEqual(gotDur[1], wantDur) {
			t.Fatalf("audio playlist durations=%v, want two of %v", gotDur, wantDur)
		}

		master := readPlaylist(t, filepath.Join(out, "playlist.m3u8"))
		if !strings.Contains(master, "#EXT-X-MEDIA:TYPE=AUDIO,URI=\"audio/playlist.m3u8\"") {
			t.Fatalf("master playlist must reference the audio playlist: %s", master)
		}
		if !strings.Contains(master, `CODECS="avc1.4D401F,mp4a.40.2"`) {
			t.Fatalf("master playlist must advertise avc1 and mp4a: %s", master)
		}
		if !strings.Contains(master, ",AUDIO=\"audio\"") {
			t.Fatalf("master playlist must associate the audio group: %s", master)
		}
		if !strings.Contains(master, "video/playlist.m3u8") {
			t.Fatalf("master playlist must reference the legacy video dir: %s", master)
		}
	})

	t.Run("duplicate trackID is rejected before creating outputs", func(t *testing.T) {
		tracks := []inputTrack{
			{id: 7, kind: kindVideo, timescale: 30000, width: 100, height: 100, profile: 0x42, compat: 0xC0, level: 0x1E},
			{id: 7, kind: kindVideo, timescale: 60000, width: 200, height: 200, profile: 0x64, compat: 0x00, level: 0x28},
		}
		fragments := []inputFragment{
			{trackID: 7, duration: 1000, payload: repeatByte(0x37, 10)},
		}
		input := writeInput(t, "dup", buildInput(t, inputFile{
			tracks: tracks, trakOrder: []uint32{7, 7}, fragments: fragments,
		}))

		out := filepath.Join(filepath.Dir(input), "out")
		if err := os.MkdirAll(out, 0777); err != nil {
			t.Fatalf("MkdirAll: %v", err)
		}
		sentinel := filepath.Join(out, "sentinel.txt")
		if err := ioutil.WriteFile(sentinel, []byte("keep-me"), 0666); err != nil {
			t.Fatalf("write sentinel: %v", err)
		}

		err := divide(input, out)
		if err == nil {
			t.Fatal("expected error for duplicated trackID")
		}
		if !strings.Contains(err.Error(), "duplicated trackID") {
			t.Fatalf("error must mention duplicated trackID, got: %v", err)
		}

		// pre-existing output must be untouched and no divide output created
		got, err := ioutil.ReadFile(sentinel)
		if err != nil {
			t.Fatalf("sentinel missing after failure: %v", err)
		}
		if string(got) != "keep-me" {
			t.Fatalf("sentinel was overwritten: %q", got)
		}
		entries, err := ioutil.ReadDir(out)
		if err != nil {
			t.Fatalf("ReadDir: %v", err)
		}
		if len(entries) != 1 || entries[0].Name() != "sentinel.txt" {
			t.Fatalf("no output files may be created before validation, got: %v", entries)
		}
	})

	t.Run("multiple audio tracks are rejected before creating outputs", func(t *testing.T) {
		tracks := []inputTrack{
			{id: 1, kind: kindVideo, timescale: 24000, width: 160, height: 120, profile: 0x42, compat: 0xC0, level: 0x1E},
			{id: 4, kind: kindAudio, timescale: 44100},
			{id: 8, kind: kindAudio, timescale: 48000},
		}
		fragments := []inputFragment{
			{trackID: 4, duration: 1024, payload: repeatByte(0xA4, 8)},
			{trackID: 8, duration: 1024, payload: repeatByte(0xA8, 8)},
		}
		input := writeInput(t, "multiaudio", buildInput(t, inputFile{
			tracks: tracks, trakOrder: []uint32{1, 4, 8}, fragments: fragments,
		}))

		out := filepath.Join(filepath.Dir(input), "out")
		if err := os.MkdirAll(out, 0777); err != nil {
			t.Fatalf("MkdirAll: %v", err)
		}
		sentinel := filepath.Join(out, "sentinel.txt")
		if err := ioutil.WriteFile(sentinel, []byte("keep-me"), 0666); err != nil {
			t.Fatalf("write sentinel: %v", err)
		}

		err := divide(input, out)
		if err == nil {
			t.Fatal("expected error for multiple audio tracks")
		}
		if !strings.Contains(err.Error(), "single mp4a audio track") {
			t.Fatalf("error must mention the single audio track limit, got: %v", err)
		}

		// pre-existing output must be untouched and no divide output created
		got, err := ioutil.ReadFile(sentinel)
		if err != nil {
			t.Fatalf("sentinel missing after failure: %v", err)
		}
		if string(got) != "keep-me" {
			t.Fatalf("sentinel was overwritten: %q", got)
		}
		entries, err := ioutil.ReadDir(out)
		if err != nil {
			t.Fatalf("ReadDir: %v", err)
		}
		if len(entries) != 1 || entries[0].Name() != "sentinel.txt" {
			t.Fatalf("no output files may be created before validation, got: %v", entries)
		}
	})
}

func sortStrings(s []string) {
	for i := 1; i < len(s); i++ {
		for j := i; j > 0 && s[j-1] > s[j]; j-- {
			s[j-1], s[j] = s[j], s[j-1]
		}
	}
}
