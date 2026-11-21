package mp4

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"reflect"
	"testing"

	"github.com/alfg/mp4/atom"
	"github.com/alfg/mp4/task_support/taskcheck"
)

// --- box builders -----------------------------------------------------------

func cat(parts ...[]byte) []byte {
	var b bytes.Buffer
	for _, p := range parts {
		b.Write(p)
	}
	return b.Bytes()
}

func header32(typ string, size uint32) []byte {
	h := make([]byte, 8)
	binary.BigEndian.PutUint32(h[0:4], size)
	copy(h[4:8], typ)
	return h
}

func mkBox(typ string, payload []byte) []byte {
	return cat(header32(typ, uint32(8+len(payload))), payload)
}

func mkExtBox(typ string, payload []byte) []byte {
	h := make([]byte, 16)
	binary.BigEndian.PutUint32(h[0:4], 1)
	copy(h[4:8], typ)
	binary.BigEndian.PutUint64(h[8:16], uint64(16+len(payload)))
	return cat(h, payload)
}

func mkZeroBox(typ string, payload []byte) []byte {
	return cat(header32(typ, 0), payload)
}

// mk decides which boxes get an extended (size32 == 1) header while a movie is
// built, so every layout variant shares byte-identical payloads.
type mk struct {
	ext func(typ string) bool
}

func (k mk) box(typ string, payload []byte) []byte {
	if k.ext != nil && k.ext(typ) {
		return mkExtBox(typ, payload)
	}
	return mkBox(typ, payload)
}

func u32(v uint32) []byte { b := make([]byte, 4); binary.BigEndian.PutUint32(b, v); return b }
func u16(v uint16) []byte { b := make([]byte, 2); binary.BigEndian.PutUint16(b, v); return b }

func fullBox(version byte, flags uint32) []byte {
	b := u32(flags)
	b[0] = version
	return b
}

// --- movie fixture ----------------------------------------------------------

const ftypMinor = 512

func buildFtyp(k mk) []byte {
	payload := cat(
		[]byte("isom"), u32(ftypMinor),
		[]byte("isom"), []byte("iso2"), []byte("avc1"), []byte("mp41"),
	)
	return k.box("ftyp", payload)
}

func buildMoov(k mk) []byte {
	return k.box("moov", cat(
		k.box("mvhd", buildMvhd()),
		buildTrak(k, 1, "vide", "VideoHandler"),
		buildTrak(k, 2, "soun", "SoundHandler"),
	))
}

func buildMvhd() []byte {
	return cat(
		fullBox(0, 0),
		u32(11), u32(22), u32(1000), u32(5000),
		u32(0x00010000), // rate 1.0
		u16(0x0100),     // volume 1.0
		make([]byte, 10),
	)
}

const langUnd = uint16((21 << 10) | (14 << 5) | 4)

func buildTkhd() []byte {
	matrix := make([]byte, 36)
	binary.BigEndian.PutUint32(matrix[0:4], 0x00010000)
	binary.BigEndian.PutUint32(matrix[16:20], 0x00010000)
	binary.BigEndian.PutUint32(matrix[32:36], 0x40000000)
	return cat(
		fullBox(0, 7),
		u32(11), u32(22), u32(1), u32(0), u32(5000),
		make([]byte, 8),
		u16(3), u16(4), u16(0x0080), u16(0),
		matrix,
		u32(1920<<16), u32(1080<<16),
	)
}

func buildElst() []byte {
	return cat(fullBox(0, 0), u32(1), u32(5000), u32(0), u16(1), u16(0))
}

func buildMdhd() []byte {
	return cat(
		fullBox(0, 0),
		u32(11), u32(22),
		u32(90000), u32(450000),
		u16(langUnd), u16(0),
	)
}

func buildHdlr(handler, name string) []byte {
	return cat(fullBox(0, 0), u32(0), []byte(handler), make([]byte, 12), []byte(name), []byte{0})
}

func buildStsd(k mk, avcVersion byte) []byte {
	return cat(
		fullBox(7, 0), // Non-zero version proves the full-box prefix follows the real header.
		u32(1),
		k.box("avc1", buildAvc1(avcVersion)),
	)
}

func buildAvc1(version byte) []byte {
	p := make([]byte, 78)
	p[0] = version // reserved[0]; the parser exposes it as Avc1Box.Version.
	binary.BigEndian.PutUint16(p[6:8], 1)
	binary.BigEndian.PutUint16(p[24:26], 1920)
	binary.BigEndian.PutUint16(p[26:28], 1080)
	binary.BigEndian.PutUint32(p[28:32], 0x00480000)
	binary.BigEndian.PutUint32(p[32:36], 0x00480000)
	binary.BigEndian.PutUint16(p[40:42], 1)
	copy(p[42:74], []byte("layout-regression-compressor"))
	binary.BigEndian.PutUint16(p[74:76], 0x0018)
	binary.BigEndian.PutUint16(p[76:78], 0xFFFF)
	return p
}

func buildStts() []byte {
	return cat(fullBox(0, 0), u32(2), u32(100), u32(3000), u32(200), u32(1500))
}

func buildTrak(k mk, trackID uint32, handler, name string) []byte {
	tkhd := buildTkhd()
	binary.BigEndian.PutUint32(tkhd[12:16], trackID)

	vmhd := k.box("vmhd", cat(fullBox(0, 1), u16(0), u16(0), u16(0)))
	stbl := k.box("stbl", cat(
		k.box("stsd", buildStsd(k, 0x2A)),
		k.box("stts", buildStts()),
	))
	minf := k.box("minf", cat(vmhd, stbl))
	mdia := k.box("mdia", cat(
		k.box("mdhd", buildMdhd()),
		k.box("hdlr", buildHdlr(handler, name)),
		minf,
	))
	edts := k.box("edts", k.box("elst", buildElst()))
	return k.box("trak", cat(k.box("tkhd", tkhd), edts, mdia))
}

func movieBytes(k mk) []byte { return cat(buildFtyp(k), buildMoov(k)) }

// --- parsed summary ---------------------------------------------------------

type trackSummary struct {
	TrackID          uint32
	TkhdFlags        uint32
	TkhdDuration     uint32
	Layer            uint16
	AlternateGroup   uint16
	Volume           atom.Fixed16
	Width, Height    atom.Fixed32
	MdhdVersion      byte
	MdhdTimescale    uint32
	MdhdDuration     uint32
	Language         uint16
	LanguageString   string
	Handler          string
	HdlrName         string
	ElstVersion      uint32
	ElstCount        uint32
	ElstSegDurations []uint32
	SttsVersion      byte
	SttsFlags        uint32
	SttsCounts       []uint32
	SttsDeltas       []uint32
	StsdVersion      byte
	StsdFlags        uint32
	Avc1Version      byte
}

type movieSummary struct {
	FtypMajor     string
	FtypMinor     uint32
	FtypBrands    []string
	MvhdVersion   byte
	MvhdTimescale uint32
	MvhdDuration  uint32
	MvhdRate      atom.Fixed32
	MvhdVolume    atom.Fixed16
	Tracks        []trackSummary
	IsFragmented  bool
}

func summarize(t *testing.T, m *atom.Mp4Reader) movieSummary {
	t.Helper()
	if m == nil || m.Ftyp == nil || m.Moov == nil || m.Moov.Mvhd == nil {
		t.Fatalf("movie not parsed: %#v", m)
	}
	s := movieSummary{
		FtypMajor:     m.Ftyp.MajorBrand,
		FtypMinor:     m.Ftyp.MinorVersion,
		FtypBrands:    m.Ftyp.CompatibleBrands,
		MvhdVersion:   m.Moov.Mvhd.Version,
		MvhdTimescale: m.Moov.Mvhd.Timescale,
		MvhdDuration:  m.Moov.Mvhd.Duration,
		MvhdRate:      m.Moov.Mvhd.Rate,
		MvhdVolume:    m.Moov.Mvhd.Volume,
		IsFragmented:  m.IsFragmented,
	}
	for _, tr := range m.Moov.Traks {
		ts := trackSummary{}
		if tr.Tkhd == nil {
			t.Fatalf("track %d: missing tkhd", len(s.Tracks))
		}
		ts.TrackID = tr.Tkhd.TrackID
		ts.TkhdFlags = tr.Tkhd.Flags
		ts.TkhdDuration = tr.Tkhd.Duration
		ts.Layer = tr.Tkhd.Layer
		ts.AlternateGroup = tr.Tkhd.AlternateGroup
		ts.Volume = tr.Tkhd.Volume
		ts.Width, ts.Height = tr.Tkhd.Width, tr.Tkhd.Height

		if tr.Edts == nil || tr.Edts.Elst == nil {
			t.Fatalf("track %d: missing edts/elst", len(s.Tracks))
		}
		ts.ElstVersion = tr.Edts.Elst.Version
		ts.ElstCount = tr.Edts.Elst.EntryCount
		for _, e := range tr.Edts.Elst.Entries {
			ts.ElstSegDurations = append(ts.ElstSegDurations, e.SegmentDuration)
		}

		if tr.Mdia == nil || tr.Mdia.Mdhd == nil || tr.Mdia.Hdlr == nil || tr.Mdia.Minf == nil {
			t.Fatalf("track %d: incomplete mdia", len(s.Tracks))
		}
		ts.MdhdVersion = tr.Mdia.Mdhd.Version
		ts.MdhdTimescale = tr.Mdia.Mdhd.Timescale
		ts.MdhdDuration = tr.Mdia.Mdhd.Duration
		ts.Language = tr.Mdia.Mdhd.Language
		ts.LanguageString = tr.Mdia.Mdhd.LanguageString
		ts.Handler = tr.Mdia.Hdlr.Handler
		ts.HdlrName = tr.Mdia.Hdlr.Name

		st := tr.Mdia.Minf.Stbl
		if st == nil || st.Stsd == nil || st.Stts == nil || st.Stsd.Avc1 == nil {
			t.Fatalf("track %d: incomplete stbl", len(s.Tracks))
		}
		ts.StsdVersion = st.Stsd.Version
		ts.StsdFlags = st.Stsd.Flags
		ts.Avc1Version = st.Stsd.Avc1.Version
		ts.SttsVersion = st.Stts.Version
		ts.SttsFlags = st.Stts.Flags
		ts.SttsCounts = st.Stts.SampleCounts
		ts.SttsDeltas = st.Stts.SampleDeltas

		s.Tracks = append(s.Tracks, ts)
	}
	return s
}

func expectedSummary() movieSummary {
	track := func(id uint32, handler, name string) trackSummary {
		return trackSummary{
			TrackID: id, TkhdFlags: 7, TkhdDuration: 5000, Layer: 3, AlternateGroup: 4,
			Volume: 0x0080, Width: 1920 << 16, Height: 1080 << 16,
			MdhdVersion: 0, MdhdTimescale: 90000, MdhdDuration: 450000,
			Language: langUnd, LanguageString: "und",
			Handler: handler, HdlrName: name + "\x00",
			ElstVersion: 0, ElstCount: 1, ElstSegDurations: []uint32{5000},
			SttsVersion: 0, SttsFlags: 0, SttsCounts: []uint32{100, 200}, SttsDeltas: []uint32{3000, 1500},
			StsdVersion: 7, StsdFlags: 0x07000000, Avc1Version: 0x2A,
		}
	}
	return movieSummary{
		FtypMajor: "isom", FtypMinor: ftypMinor,
		FtypBrands:    []string{"isom", "iso2", "avc1", "mp41"},
		MvhdVersion:   0,
		MvhdTimescale: 1000,
		MvhdDuration:  5000,
		MvhdRate:      1 << 16,
		MvhdVolume:    0x0100,
		Tracks: []trackSummary{
			track(1, "vide", "VideoHandler"),
			track(2, "soun", "SoundHandler"),
		},
	}
}

// boxHeaderSizeOf reports the header size a layout gives to a box type.
func boxHeaderSizeOf(k mk, typ string) int64 {
	if k.ext != nil && k.ext(typ) {
		return atom.LargeBoxHeaderSize
	}
	return atom.BoxHeaderSize
}

// findBox returns the absolute offset of the first size32==1 header of typ in
// data. It is only used for fixtures that build typ in extended form.
func findExtBox(t *testing.T, data []byte, typ string) int64 {
	t.Helper()
	tb := []byte(typ)
	for i := 4; i+4 <= len(data); i++ {
		if bytes.Equal(data[i:i+4], tb) && binary.BigEndian.Uint32(data[i-4:i]) == 1 {
			return int64(i - 4)
		}
	}
	t.Fatalf("extended box %q not found", typ)
	return -1
}

// findBoxAfter returns the absolute offset of the first header of typ at or
// after start. Use it when the type string may also appear in an earlier box
// payload (for example the "avc1" compatible brand inside ftyp).
func findBoxAfter(t *testing.T, data []byte, typ string, start int64) int64 {
	t.Helper()
	i := bytes.Index(data[start:], []byte(typ))
	if i < 4 {
		t.Fatalf("box %q not found after offset %d", typ, start)
	}
	return start + int64(i-4)
}

// findBox returns any box header of typ, regular or extended.
func findBox(t *testing.T, data []byte, typ string) int64 {
	t.Helper()
	tb := []byte(typ)
	for i := 4; i+4 <= len(data); i++ {
		if bytes.Equal(data[i:i+4], tb) {
			return int64(i - 4)
		}
	}
	t.Fatalf("box %q not found", typ)
	return -1
}

// --- TestLayoutTree ---------------------------------------------------------

func TestLayoutTree(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		want := expectedSummary()

		// The plain 32-bit layout must keep producing exactly what it always did.
		normalBytes := movieBytes(mk{})
		normal, err := OpenFromBytes(normalBytes)
		c.Check(err == nil, "normal layout: unexpected error: %v", err)
		if err == nil {
			got := summarize(t, normal)
			c.Check(reflect.DeepEqual(got, want), "normal layout summary mismatch:\n got: %#v\nwant: %#v", got, want)
		}

		// Every layout mix must yield the same movie and track description.
		layouts := []struct {
			name string
			k    mk
		}{
			{"all-extended", mk{func(string) bool { return true }}},
			{"moov-extended", mk{func(typ string) bool { return typ == "moov" }}},
			{"trak-extended", mk{func(typ string) bool { return typ == "trak" }}},
			{"mdia-extended", mk{func(typ string) bool { return typ == "mdia" }}},
			{"minf-stbl-extended", mk{func(typ string) bool { return typ == "minf" || typ == "stbl" }}},
			{"stsd-avc1-extended", mk{func(typ string) bool { return typ == "stsd" || typ == "avc1" }}},
			{"leaf-extended", mk{func(typ string) bool {
				switch typ {
				case "mvhd", "tkhd", "elst", "mdhd", "hdlr", "vmhd", "stts":
					return true
				}
				return false
			}}},
		}
		for _, lay := range layouts {
			lay := lay
			t.Run(lay.name, func(t *testing.T) {
				buf := movieBytes(lay.k)
				m, err := OpenFromBytes(buf)
				c.Check(err == nil, "%s: unexpected error: %v", lay.name, err)
				if err != nil {
					return
				}
				got := summarize(t, m)
				c.Check(reflect.DeepEqual(got, want), "%s: summary mismatch:\n got: %#v\nwant: %#v", lay.name, got, want)

				// Payload offsets really do move with the extended header.
				c.Check(m.Moov.HeaderSize == boxHeaderSizeOf(lay.k, "moov"), "%s: moov header size = %d", lay.name, m.Moov.HeaderSize)
				c.Check(m.Moov.Traks[0].Mdia.Minf.Stbl.Stsd.HeaderSize == boxHeaderSizeOf(lay.k, "stsd"), "%s: stsd header size", lay.name)
				c.Check(m.Moov.Traks[0].Mdia.Minf.Stbl.Stsd.Avc1.HeaderSize == boxHeaderSizeOf(lay.k, "avc1"), "%s: avc1 header size", lay.name)
			})
		}

		// The three public entry points must agree for an extended layout.
		extBytes := movieBytes(mk{func(string) bool { return true }})
		fromBytes, errB := OpenFromBytes(extBytes)
		c.Check(errB == nil, "OpenFromBytes extended: %v", errB)
		fromReader, errR := OpenFromReader(bytes.NewReader(extBytes), int64(len(extBytes)))
		c.Check(errR == nil, "OpenFromReader extended: %v", errR)

		dir := t.TempDir()
		path := filepath.Join(dir, "extended.mp4")
		c.Check(os.WriteFile(path, extBytes, 0644) == nil, "write fixture")
		fromFile, errF := Open(path)
		c.Check(errF == nil, "Open extended: %v", errF)
		if fromBytes != nil && fromReader != nil && fromFile != nil {
			a := summarize(t, fromBytes)
			c.Check(reflect.DeepEqual(summarize(t, fromReader), a), "OpenFromReader differs from OpenFromBytes")
			c.Check(reflect.DeepEqual(summarize(t, fromFile), a), "Open differs from OpenFromBytes")
		}

		// Unknown boxes are skipped by their real length, in either header form.
		moovPayload := cat(
			mkBox("free", bytes.Repeat([]byte{0xAB}, 13)),
			mkBox("mvhd", buildMvhd()),
			mkExtBox("wxyz", bytes.Repeat([]byte{0xCD}, 9)),
			buildTrak(mk{}, 1, "vide", "VideoHandler"),
			buildTrak(mk{}, 2, "soun", "SoundHandler"),
		)
		withUnknown := cat(buildFtyp(mk{}), mkBox("moov", moovPayload))
		m, err := OpenFromBytes(withUnknown)
		c.Check(err == nil, "unknown-box layout: %v", err)
		if err == nil {
			c.Check(len(m.Moov.Traks) == 2, "unknown boxes: got %d tracks, want 2", len(m.Moov.Traks))
			c.Check(m.Moov.Mvhd != nil && m.Moov.Mvhd.Timescale == 1000, "unknown boxes: mvhd not parsed")
		}

		// size32 == 0 at the end of the file fills the remaining extent.
		ftyp := buildFtyp(mk{})
		zeroMoov := cat(ftyp, mkZeroBox("moov", cat(
			mkBox("mvhd", buildMvhd()),
			buildTrak(mk{}, 1, "vide", "VideoHandler"),
		)))
		mz, err := OpenFromBytes(zeroMoov)
		c.Check(err == nil, "zero-sized top-level moov: %v", err)
		if err == nil && mz.Moov != nil {
			c.Check(len(mz.Moov.Traks) == 1, "zero-sized top-level moov not parsed")
			c.Check(mz.Moov.Size == int64(len(zeroMoov)-len(ftyp)), "zero-sized moov should fill the remainder, size=%d", mz.Moov.Size)
			c.Check(mz.Moov.Start == int64(len(ftyp)), "zero-sized moov start=%d, want %d", mz.Moov.Start, len(ftyp))
		} else {
			c.Check(false, "zero-sized top-level moov: moov missing")
		}

		// A zero-sized child is valid when its parent ends at EOF.
		innerZero := mkBox("moov", cat(
			mkBox("mvhd", buildMvhd()),
			buildTrak(mk{}, 1, "vide", "VideoHandler"),
			mkZeroBox("udta", []byte("tail")),
		))
		mzi, err := OpenFromBytes(cat(ftyp, innerZero))
		c.Check(err == nil, "zero-sized nested box ending at EOF: %v", err)
		if err == nil {
			c.Check(len(mzi.Moov.Traks) == 1, "zero-sized nested box broke parsing")
		}

		// ReadBoxAt keeps its old uint32 contract and reads an extended mdat header.
		mdatPayload := bytes.Repeat([]byte{0x5A}, 64)
		extMdat := cat(mkExtBox("mdat", mdatPayload), mkBox("moov", cat(
			mkBox("mvhd", buildMvhd()),
			buildTrak(mk{}, 1, "vide", "VideoHandler"),
		)))
		mdatFixture := cat(ftyp, extMdat)
		mm, err := OpenFromBytes(mdatFixture)
		c.Check(err == nil, "extended mdat: %v", err)
		if err == nil {
			c.Check(mm.Mdat != nil && mm.Mdat.Size == int64(16+len(mdatPayload)), "extended mdat size = %v", mm.Mdat)
			off := findExtBox(t, mdatFixture, "mdat")
			size, typ := mm.ReadBoxAt(off)
			c.Check(size == 1, "ReadBoxAt size32 = %d, want 1", size)
			c.Check(typ == "mdat", "ReadBoxAt type = %q, want mdat", typ)
		}
	})
}

// --- sparse reader ----------------------------------------------------------

var errMediaRead = errors.New("media payload must not be read")

type sparsePart struct {
	start int64
	data  []byte
}

type sparseReader struct {
	total  int64
	parts  []sparsePart
	poison [2]int64 // any read overlapping this interval is an error
	reads  int
}

func (s *sparseReader) ReadAt(p []byte, off int64) (int, error) {
	s.reads++
	if off < 0 {
		return 0, fmt.Errorf("negative offset %d", off)
	}
	for n := 0; n < len(p); {
		abs := off + int64(n)
		if abs >= s.total {
			return n, io.EOF
		}
		if abs >= s.poison[0] && abs < s.poison[1] {
			return n, errMediaRead
		}
		var b byte
		found := false
		for _, part := range s.parts {
			if abs >= part.start && abs < part.start+int64(len(part.data)) {
				b = part.data[abs-part.start]
				found = true
				break
			}
		}
		if !found {
			return n, fmt.Errorf("unmapped sparse offset %d", abs)
		}
		p[n] = b
		n++
	}
	return len(p), nil
}

func TestSparseMedia(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		want := expectedSummary()

		const mediaPayload = int64(4<<30) + 12345 // Just past 4 GiB.
		ftyp := buildFtyp(mk{})
		moov := buildMoov(mk{})
		mdatStart := int64(len(ftyp))
		largeSize := uint64(atom.LargeBoxHeaderSize) + uint64(mediaPayload)
		moovStart := mdatStart + int64(largeSize)
		total := moovStart + int64(len(moov))

		c.Check(total > 4<<30, "logical file size %d should exceed 4 GiB", total)

		hdr := make([]byte, atom.LargeBoxHeaderSize)
		binary.BigEndian.PutUint32(hdr[0:4], 1)
		copy(hdr[4:8], "mdat")
		binary.BigEndian.PutUint64(hdr[8:16], largeSize)

		reader := &sparseReader{
			total:  total,
			poison: [2]int64{mdatStart + atom.LargeBoxHeaderSize, moovStart},
			parts: []sparsePart{
				{start: 0, data: cat(ftyp, hdr)},
				{start: moovStart, data: moov},
			},
		}

		m, err := OpenFromReader(reader, total)
		c.Check(err == nil, "sparse OpenFromReader: %v", err)
		if err != nil {
			return
		}
		got := summarize(t, m)
		c.Check(reflect.DeepEqual(got, want), "sparse media summary mismatch:\n got: %#v\nwant: %#v", got, want)
		c.Check(m.Size == total, "reader size = %d, want %d", m.Size, total)
		c.Check(m.Mdat != nil && m.Mdat.Size == int64(largeSize), "mdat box size = %v, want %d", m.Mdat, largeSize)
		c.Check(m.Mdat != nil && m.Mdat.Start == mdatStart, "mdat start = %v, want %d", m.Mdat, mdatStart)
		c.Check(m.Moov != nil && m.Moov.Start == moovStart, "moov start = %v, want %d", m.Moov, moovStart)
		c.Check(reader.reads > 0, "reader was never used")

		// Same layout as a real sparse file, opened through Open().
		path := filepath.Join(t.TempDir(), "sparse.mp4")
		f, err := os.Create(path)
		c.Check(err == nil, "create sparse file: %v", err)
		if err == nil {
			_, err = f.WriteAt(cat(ftyp, hdr), 0)
			c.Check(err == nil, "write sparse prefix: %v", err)
			_, err = f.WriteAt(moov, moovStart)
			c.Check(err == nil, "write sparse moov: %v", err)
			c.Check(f.Truncate(total) == nil, "truncate sparse file")
			_ = f.Close()

			fm, err := Open(path)
			c.Check(err == nil, "sparse Open: %v", err)
			if err == nil {
				c.Check(reflect.DeepEqual(summarize(t, fm), want), "sparse Open summary mismatch")
				c.Check(fm.Size == total, "sparse Open size = %d, want %d", fm.Size, total)
			}
		}

		// A zero-sized mdat at the end of the file covers the whole tail.
		zeroMdat := cat(ftyp, mkZeroBox("mdat", nil))
		zm, err := OpenFromBytes(zeroMdat)
		c.Check(err == nil, "zero-sized mdat: %v", err)
		if err == nil {
			c.Check(zm.Mdat != nil && zm.Mdat.Size == int64(len(zeroMdat)-len(ftyp)), "zero-sized mdat size = %v", zm.Mdat)
			c.Check(zm.Moov == nil, "zero-sized mdat should leave no moov")
		}
	})
}

// --- TestLayoutErrors -------------------------------------------------------

// errReader fails every read with a sentinel error.
type errReader struct{ err error }

func (r errReader) ReadAt(p []byte, off int64) (int, error) { return 0, r.err }

// errAfterReader serves prefix and then fails with a sentinel error.
type errAfterReader struct {
	prefix []byte
	err    error
}

func (r errAfterReader) ReadAt(p []byte, off int64) (int, error) {
	if off >= int64(len(r.prefix)) {
		return 0, r.err
	}
	n := copy(p, r.prefix[off:])
	if n < len(p) {
		return n, r.err
	}
	return n, nil
}

func wantBoxError(t *testing.T, c *taskcheck.C, err error, typ string, offset int64) {
	t.Helper()
	c.Check(err != nil, "expected an error for box %q at %d, got nil", typ, offset)
	if err == nil {
		return
	}
	var be *atom.BoxError
	c.Check(errors.As(err, &be), "error %v is not a *atom.BoxError", err)
	if errors.As(err, &be) {
		c.Check(be.Type == typ, "error box type = %q, want %q (err: %v)", be.Type, typ, err)
		c.Check(be.Offset == offset, "error offset = %d, want %d (err: %v)", be.Offset, offset, err)
	}
}

func TestLayoutErrors(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		ftyp := buildFtyp(mk{})
		ftypLen := int64(len(ftyp))

		// Extended header with fewer than 8 largesize bytes available.
		truncated := cat(ftyp, header32("moov", 1), []byte{0, 0, 0})
		_, err := OpenFromBytes(truncated)
		wantBoxError(t, c, err, "moov", ftypLen)

		// Extended size smaller than the 16 byte header itself.
		tooSmall := cat(ftyp)
		h := make([]byte, 16)
		binary.BigEndian.PutUint32(h[0:4], 1)
		copy(h[4:8], "moov")
		binary.BigEndian.PutUint64(h[8:16], 8)
		tooSmall = cat(tooSmall, h)
		_, err = OpenFromBytes(tooSmall)
		wantBoxError(t, c, err, "moov", ftypLen)

		// Extended size that cannot be represented as a signed 64-bit length.
		overflow := cat(ftyp)
		h = make([]byte, 16)
		binary.BigEndian.PutUint32(h[0:4], 1)
		copy(h[4:8], "moov")
		binary.BigEndian.PutUint64(h[8:16], 1<<63)
		overflow = cat(overflow, h)
		_, err = OpenFromBytes(overflow)
		wantBoxError(t, c, err, "moov", ftypLen)

		// Extended size that runs past the end of the file.
		overrun := cat(ftyp)
		h = make([]byte, 16)
		binary.BigEndian.PutUint32(h[0:4], 1)
		copy(h[4:8], "moov")
		binary.BigEndian.PutUint64(h[8:16], 1<<20)
		overrun = cat(overrun, h, make([]byte, 32))
		_, err = OpenFromBytes(overrun)
		wantBoxError(t, c, err, "moov", ftypLen)

		// A regular child whose size exceeds its parent container.
		mvhd := mkBox("mvhd", buildMvhd())
		badMoovPayload := cat(mvhd, header32("trak", uint32(len(mvhd)+4096)))
		badMoov := cat(ftyp, mkBox("moov", badMoovPayload))
		trakOff := ftypLen + int64(atom.BoxHeaderSize) + int64(len(mvhd))
		_, err = OpenFromBytes(badMoov)
		wantBoxError(t, c, err, "trak", trakOff)

		// A nested extended child whose largesize exceeds the parent container.
		extTrak := func() []byte {
			x := make([]byte, 16)
			binary.BigEndian.PutUint32(x[0:4], 1)
			copy(x[4:8], "trak")
			binary.BigEndian.PutUint64(x[8:16], 1<<20)
			return x
		}()
		nestedBad := cat(ftyp, mkBox("moov", cat(mvhd, extTrak)))
		_, err = OpenFromBytes(nestedBad)
		wantBoxError(t, c, err, "trak", trakOff)

		// A supported description box truncated inside its own payload.
		shortMvhd := cat(ftyp, mkBox("moov", mkBox("mvhd", []byte{0, 0, 0, 0})))
		_, err = OpenFromBytes(shortMvhd)
		wantBoxError(t, c, err, "mvhd", ftypLen+int64(atom.BoxHeaderSize))

		shortStsd := cat(ftyp, mkBox("moov", cat(
			mkBox("mvhd", buildMvhd()),
			mkBox("trak", cat(
				mkBox("tkhd", buildTkhd()),
				mkBox("mdia", cat(
					mkBox("mdhd", buildMdhd()),
					mkBox("hdlr", buildHdlr("vide", "v")),
					mkBox("minf", mkBox("stbl", mkBox("stsd", []byte{0, 0, 0, 0}))),
				)),
			)),
		)))
		stsdOff := findBox(t, shortStsd, "stsd")
		_, err = OpenFromBytes(shortStsd)
		wantBoxError(t, c, err, "stsd", stsdOff)

		// Truncated avc1 payload inside stsd.
		shortAvc := cat(ftyp, mkBox("moov", cat(
			mkBox("mvhd", buildMvhd()),
			mkBox("trak", cat(
				mkBox("tkhd", buildTkhd()),
				mkBox("mdia", cat(
					mkBox("mdhd", buildMdhd()),
					mkBox("hdlr", buildHdlr("vide", "v")),
					mkBox("minf", mkBox("stbl", mkBox("stsd", cat(fullBox(0, 0), u32(1), mkBox("avc1", nil))))),
				)),
			)),
		)))
		avcOff := findBoxAfter(t, shortAvc, "avc1", ftypLen)
		_, err = OpenFromBytes(shortAvc)
		wantBoxError(t, c, err, "avc1", avcOff)

		// A zero-sized box inside a container that does not end at EOF.
		zeroInMiddle := cat(
			ftyp,
			mkBox("moov", cat(mkBox("mvhd", buildMvhd()), mkZeroBox("udta", []byte("x")))),
			mkBox("free", nil),
		)
		udtaOff := findBox(t, zeroInMiddle, "udta")
		_, err = OpenFromBytes(zeroInMiddle)
		wantBoxError(t, c, err, "udta", udtaOff)

		// A zero-sized box in a nested container that does not itself end at
		// the file end must also be rejected.
		nestedZero := cat(
			ftyp,
			mkBox("moov", cat(
				mkBox("mvhd", buildMvhd()),
				mkBox("trak", cat(mkZeroBox("tkhd", bytes.Repeat([]byte{0}, 84)), mkBox("free", []byte("z")))),
			)),
			mkBox("free", nil),
		)
		_, err = OpenFromBytes(nestedZero)
		wantBoxError(t, c, err, "tkhd", ftypLen+2*int64(atom.BoxHeaderSize)+int64(len(mvhd)))

		// ReaderAt failures must be discoverable with errors.Is.
		sentinel := errors.New("sentinel read failure")
		_, err = OpenFromReader(errReader{err: sentinel}, 1024)
		c.Check(err != nil, "reader error: expected failure")
		c.Check(errors.Is(err, sentinel), "reader error %v should wrap the sentinel", err)

		_, err = OpenFromReader(errAfterReader{prefix: cat(ftyp, header32("moov", 64)), err: sentinel}, 1024)
		c.Check(err != nil, "mid-read error: expected failure")
		c.Check(errors.Is(err, sentinel), "mid-read error %v should wrap the sentinel", err)

		// A size that implies content beyond the reader must fail rather than
		// silently return an empty movie.
		_, err = OpenFromReader(bytes.NewReader(ftyp), 1<<20)
		c.Check(err != nil, "short reader: expected failure")
	})
}
