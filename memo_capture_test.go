package shp

import (
	"archive/zip"
	"bytes"
	"encoding/binary"
	"errors"
	"io"
	"math"
	"os"
	"path/filepath"
	"testing"

	"github.com/jonas-p/go-shp/internal/checks"
)

// memoTestField builds a single DBF field descriptor for the tests.
func memoTestField(name string, typ byte, size uint8) Field {
	var f Field
	copy(f.Name[:], name)
	f.Fieldtype = typ
	f.Size = size
	return f
}

// memoTestFields is the two-field table (NAME C(10), MEMO M(4)) used by all
// memo tests.
var memoTestFields = []Field{
	memoTestField("NAME", 'C', 10),
	memoTestField("MEMO", 'M', 4),
}

// memoTestNames are the NAME values of the six physical rows. Row 1 carries
// the deletion flag but still occupies a physical row.
var memoTestNames = []string{"alpha", "gone", "gamma", "delta", "epsilon", "zeta"}

// memoTestCrossBlock is a 100 byte memo body that spans two FPT blocks of 64
// bytes. It contains an embedded NUL, an 0x1A byte and trailing spaces to
// make sure the body is neither truncated at markers nor trimmed.
var memoTestCrossBlock = func() []byte {
	b := make([]byte, 100)
	copy(b, "CROSS-BLOCK:")
	for i := 12; i < 96; i++ {
		b[i] = byte('A' + (i-12)%26)
	}
	copy(b[96:], []byte{' ', 0x00, 0x1a, ' '})
	return b
}()

var memoTestBinary = []byte{0xde, 0xad, 0xbe, 0xef}

// memoTestBlocks returns the FPT content of the shared fixture with a block
// size of 64 bytes: block 6 holds a short text, block 3 a body that spans
// into block 4, block 9 a zero length text and block 12 a binary memo.
func memoTestBlocks() map[uint32]memoTestBlock {
	return map[uint32]memoTestBlock{
		6:  {kind: MemoKindText, data: []byte("hello memo world")},
		3:  {kind: MemoKindText, data: memoTestCrossBlock},
		9:  {kind: MemoKindText, data: []byte{}},
		12: {kind: MemoKindBinary, data: memoTestBinary},
	}
}

type memoTestBlock struct {
	kind uint32
	data []byte
}

// memoTestPointers are the memo block numbers of the six physical rows. The
// references are deliberately not sorted (6, 3, ...) and block 6 is
// referenced twice.
var memoTestPointers = []uint32{6, 3, 6, 0, 9, 12}

// buildMemoSHP returns a minimal but valid POINT shapefile.
func buildMemoSHP(pts ...[2]float64) []byte {
	buf := new(bytes.Buffer)
	header := make([]byte, 100)
	binary.BigEndian.PutUint32(header[0:], 9994)
	binary.BigEndian.PutUint32(header[24:], uint32((100+len(pts)*28)/2))
	binary.LittleEndian.PutUint32(header[28:], 1000)
	binary.LittleEndian.PutUint32(header[32:], uint32(POINT))
	if len(pts) > 0 {
		minX, minY, maxX, maxY := pts[0][0], pts[0][1], pts[0][0], pts[0][1]
		for _, p := range pts[1:] {
			minX = math.Min(minX, p[0])
			minY = math.Min(minY, p[1])
			maxX = math.Max(maxX, p[0])
			maxY = math.Max(maxY, p[1])
		}
		binary.LittleEndian.PutUint64(header[36:], math.Float64bits(minX))
		binary.LittleEndian.PutUint64(header[44:], math.Float64bits(minY))
		binary.LittleEndian.PutUint64(header[52:], math.Float64bits(maxX))
		binary.LittleEndian.PutUint64(header[60:], math.Float64bits(maxY))
	}
	buf.Write(header)
	for i, p := range pts {
		binary.Write(buf, binary.BigEndian, int32(i+1))
		binary.Write(buf, binary.BigEndian, int32(10)) // 20 content bytes
		binary.Write(buf, binary.LittleEndian, int32(POINT))
		binary.Write(buf, binary.LittleEndian, math.Float64bits(p[0]))
		binary.Write(buf, binary.LittleEndian, math.Float64bits(p[1]))
	}
	return buf.Bytes()
}

// memoCharValue returns s as a space padded C field value.
func memoCharValue(s string, size int) []byte {
	b := bytes.Repeat([]byte{' '}, size)
	copy(b, s)
	return b
}

// memoPtrValue returns the 4 byte little-endian M field value for block.
func memoPtrValue(block uint32) []byte {
	b := make([]byte, 4)
	binary.LittleEndian.PutUint32(b, block)
	return b
}

// memoRecord concatenates a deletion flag and field values into a DBF record.
func memoRecord(flag byte, values ...[]byte) []byte {
	rec := []byte{flag}
	for _, v := range values {
		rec = append(rec, v...)
	}
	return rec
}

// buildMemoDBF returns a DBF table with the given version byte, field
// descriptors and physical records. backlink extra bytes are placed between
// the 0x0D field descriptor terminator and the first record, mimicking the
// Visual FoxPro extended header; the header length covers them.
func buildMemoDBF(t *testing.T, version byte, fields []Field, records [][]byte, backlink int) []byte {
	t.Helper()
	recLen := 1
	for _, f := range fields {
		recLen += int(f.Size)
	}
	headerLen := 32 + 32*len(fields) + 1 + backlink
	buf := new(bytes.Buffer)
	hdr := make([]byte, 32)
	hdr[0] = version
	hdr[1], hdr[2], hdr[3] = 26, 11, 12 // last update 2026-11-12
	binary.LittleEndian.PutUint32(hdr[4:], uint32(len(records)))
	binary.LittleEndian.PutUint16(hdr[8:], uint16(headerLen))
	binary.LittleEndian.PutUint16(hdr[10:], uint16(recLen))
	buf.Write(hdr)
	for _, f := range fields {
		if err := binary.Write(buf, binary.LittleEndian, f); err != nil {
			t.Fatalf("cannot serialize field: %v", err)
		}
	}
	buf.WriteByte(0x0d)
	// garbage that must not be parsed as field descriptors
	buf.Write(bytes.Repeat([]byte{0xab}, backlink))
	for i, rec := range records {
		if len(rec) != recLen {
			t.Fatalf("record %d has %d bytes, want %d", i, len(rec), recLen)
		}
		buf.Write(rec)
	}
	buf.WriteByte(0x1a)
	return buf.Bytes()
}

// buildMemoFPT returns an FPT file with the given block size and blocks.
func buildMemoFPT(t *testing.T, blockSize uint16, blocks map[uint32]memoTestBlock) []byte {
	t.Helper()
	size := int64(fptHeaderSize)
	for n, b := range blocks {
		if end := int64(n)*int64(blockSize) + 8 + int64(len(b.data)); end > size {
			size = end
		}
	}
	buf := make([]byte, size)
	binary.BigEndian.PutUint16(buf[6:8], blockSize)
	for n, b := range blocks {
		off := int64(n) * int64(blockSize)
		binary.BigEndian.PutUint32(buf[off:], b.kind)
		binary.BigEndian.PutUint32(buf[off+4:], uint32(len(b.data)))
		copy(buf[off+8:], b.data)
	}
	return buf
}

// memoTestRecords builds the six physical DBF rows of the shared fixture.
func memoTestRecords() [][]byte {
	flags := []byte{0x20, 0x2a, 0x20, 0x20, 0x20, 0x20}
	records := make([][]byte, len(memoTestNames))
	for i, name := range memoTestNames {
		records[i] = memoRecord(flags[i],
			memoCharValue(name, 10),
			memoPtrValue(memoTestPointers[i]))
	}
	return records
}

// memoTestFixture returns SHP, DBF (Visual FoxPro, 0x30, with a 263 byte
// backlink area) and FPT bytes of the shared six-row fixture.
func memoTestFixture(t *testing.T) (shpBytes, dbfBytes, fptBytes []byte) {
	t.Helper()
	pts := make([][2]float64, len(memoTestNames))
	for i := range pts {
		pts[i] = [2]float64{float64(i), float64(i * 2)}
	}
	shpBytes = buildMemoSHP(pts...)
	dbfBytes = buildMemoDBF(t, 0x30, memoTestFields, memoTestRecords(), 263)
	fptBytes = buildMemoFPT(t, 64, memoTestBlocks())
	return shpBytes, dbfBytes, fptBytes
}

// wantMemo is the expected memo value of a row.
type wantMemo struct {
	present bool
	kind    uint32
	data    []byte
}

// memoTestWants are the expected memo values of the six physical rows.
func memoTestWants() []wantMemo {
	return []wantMemo{
		{true, MemoKindText, []byte("hello memo world")},
		{true, MemoKindText, memoTestCrossBlock},
		{true, MemoKindText, []byte("hello memo world")},
		{false, 0, nil},
		{true, MemoKindText, []byte{}},
		{true, MemoKindBinary, memoTestBinary},
	}
}

func checkMemoValue(t *testing.T, row int, got MemoValue, err error, want wantMemo) {
	t.Helper()
	if err != nil {
		t.Fatalf("row %d: unexpected memo error: %v", row, err)
	}
	if got.Present != want.present || got.Kind != want.kind || !bytes.Equal(got.Data, want.data) {
		t.Fatalf("row %d: got %+v, want present=%v kind=%d data=%q",
			row, got, want.present, want.kind, want.data)
	}
}

// shortReadCloser wraps r and limits every Read to max bytes to simulate a
// stream that returns short reads.
type shortReadCloser struct {
	r   io.Reader
	max int
}

func (s *shortReadCloser) Read(p []byte) (int, error) {
	if len(p) > s.max {
		p = p[:s.max]
	}
	return s.r.Read(p)
}

func (s *shortReadCloser) Close() error { return nil }

// TestMemoFiles reads the fixture from files on disk through Reader,
// covering the extended Visual FoxPro header, cross-block bodies, zero
// length memos, reversed and duplicate block references and deleted rows.
func TestMemoFiles(t *testing.T) {
	shpBytes, dbfBytes, fptBytes := memoTestFixture(t)
	dir := t.TempDir()
	base := filepath.Join(dir, "points")
	for ext, data := range map[string][]byte{
		".shp": shpBytes, ".dbf": dbfBytes, ".fpt": fptBytes,
	} {
		if err := os.WriteFile(base+ext, data, 0644); err != nil {
			t.Fatal(err)
		}
	}

	r, err := Open(base + ".shp")
	if err != nil {
		t.Fatal(err)
	}
	defer r.Close()

	fields := r.Fields()
	// the 263 backlink bytes between the terminator and the first record
	// must not be counted as fields
	checks.Check(t, len(fields) == 2 &&
		fields[0].String() == "NAME" && fields[0].Fieldtype == 'C' &&
		fields[1].String() == "MEMO" && fields[1].Fieldtype == 'M' && fields[1].Size == 4)
	checks.Check(t, r.GeometryType == POINT)
	checks.Check(t, r.AttributeCount() == len(memoTestNames))

	wants := memoTestWants()
	n := 0
	for r.Next() {
		idx, shape := r.Shape()
		checks.Check(t, idx == n && shape.BBox().MinX == float64(n))
		// deleted rows occupy a physical row and stay aligned
		checks.Check(t, r.ReadAttribute(n, 0) == memoTestNames[n])
		m, err := r.ReadMemo(n, 1)
		checkMemoValue(t, n, m, err, wants[n])
		checks.Check(t, true)
		n++
	}
	if err := r.Err(); err != nil {
		t.Fatal(err)
	}
	checks.Check(t, n == len(memoTestNames))
}

// TestMemoStream reads the fixture through SequentialReaderFromExtWithMemo
// with streams that only return a few bytes per Read.
func TestMemoStream(t *testing.T) {
	shpBytes, dbfBytes, fptBytes := memoTestFixture(t)
	sr := SequentialReaderFromExtWithMemo(
		&shortReadCloser{bytes.NewReader(shpBytes), 3},
		&shortReadCloser{bytes.NewReader(dbfBytes), 3},
		bytes.NewReader(fptBytes), int64(len(fptBytes)))
	defer sr.Close()

	// the new interface still satisfies the old one
	var plain SequentialReader = sr
	checks.Check(t, len(plain.Fields()) == 2 && plain.Fields()[1].Fieldtype == 'M')

	wants := memoTestWants()
	n := 0
	for sr.Next() {
		idx, _ := sr.Shape()
		checks.Check(t, idx == n)
		checks.Check(t, sr.Attribute(0) == memoTestNames[n])
		if n == 0 {
			// a memo error must not disturb the iteration
			if _, err := sr.Memo(0); !errors.Is(err, ErrMemoField) {
				t.Fatalf("expected ErrMemoField, got %v", err)
			}
			checks.Check(t, true)
		}
		m, err := sr.Memo(1)
		checkMemoValue(t, n, m, err, wants[n])
		checks.Check(t, true)
		n++
	}
	if err := sr.Err(); err != nil {
		t.Fatal(err)
	}
	checks.Check(t, n == len(memoTestNames))

	// the old constructor keeps working without memo support
	shpBytes2, dbfBytes2, _ := memoTestFixture(t)
	old := SequentialReaderFromExt(
		&shortReadCloser{bytes.NewReader(shpBytes2), 5},
		&shortReadCloser{bytes.NewReader(dbfBytes2), 5})
	defer old.Close()
	count := 0
	for old.Next() {
		count++
	}
	checks.Check(t, count == len(memoTestNames) && old.Err() == nil)
}

// writeMemoZip writes entries to a new ZIP archive and returns its path.
func writeMemoZip(t *testing.T, entries map[string][]byte) string {
	t.Helper()
	name := filepath.Join(t.TempDir(), "data.zip")
	f, err := os.Create(name)
	if err != nil {
		t.Fatal(err)
	}
	zw := zip.NewWriter(f)
	for entry, data := range entries {
		w, err := zw.Create(entry)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := w.Write(data); err != nil {
			t.Fatal(err)
		}
	}
	if err := zw.Close(); err != nil {
		t.Fatal(err)
	}
	if err := f.Close(); err != nil {
		t.Fatal(err)
	}
	return name
}

// TestMemoZip reads memos from ZIP archives through ZipReader.
func TestMemoZip(t *testing.T) {
	shpBytes, dbfBytes, fptBytes := memoTestFixture(t)

	// single shapefile group at the archive root, opened with OpenZip
	single := writeMemoZip(t, map[string][]byte{
		"points.shp": shpBytes,
		"points.dbf": dbfBytes,
		"points.fpt": fptBytes,
	})
	zr, err := OpenZip(single)
	if err != nil {
		t.Fatal(err)
	}
	wants := memoTestWants()
	n := 0
	for zr.Next() {
		checks.Check(t, zr.Attribute(0) == memoTestNames[n])
		m, err := zr.Memo(1)
		checkMemoValue(t, n, m, err, wants[n])
		checks.Check(t, true)
		n++
	}
	checks.Check(t, n == len(memoTestNames) && zr.Err() == nil)
	// Memo still sees the physical row of the last successful Next
	m, err := zr.Memo(1)
	checkMemoValue(t, len(wants)-1, m, err, wants[len(wants)-1])
	checks.Check(t, true)
	if err := zr.Close(); err != nil {
		t.Fatal(err)
	}

	// a shapefile group inside a directory; the decoy files with the same
	// basename in another directory must not be picked up
	decoyFpt := buildMemoFPT(t, 64, map[uint32]memoTestBlock{
		6: {kind: MemoKindText, data: []byte("DECOY-DECOY-DECOY")},
	})
	nested := writeMemoZip(t, map[string][]byte{
		"a/points.shp": shpBytes,
		"a/points.dbf": dbfBytes,
		"a/points.fpt": fptBytes,
		"b/points.dbf": dbfBytes,
		"b/points.fpt": decoyFpt,
	})
	zr2, err := OpenShapeFromZip(nested, "a/points.shp")
	if err != nil {
		t.Fatal(err)
	}
	defer zr2.Close()
	if !zr2.Next() {
		t.Fatal("expected first feature")
	}
	m, err = zr2.Memo(1)
	checkMemoValue(t, 0, m, err, wants[0])
	checks.Check(t, zr2.Attribute(0) == "alpha")
}

// TestMemoFailures checks that the different memo failure modes produce
// distinguishable errors and that a failed memo read does not disturb the
// geometry or DBF position.
func TestMemoFailures(t *testing.T) {
	shpBytes, dbfBytes, fptBytes := memoTestFixture(t)

	writeGroup := func(dbf, fpt []byte, withFpt bool) string {
		dir := t.TempDir()
		base := filepath.Join(dir, "points")
		if err := os.WriteFile(base+".shp", shpBytes, 0644); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(base+".dbf", dbf, 0644); err != nil {
			t.Fatal(err)
		}
		if withFpt {
			if err := os.WriteFile(base+".fpt", fpt, 0644); err != nil {
				t.Fatal(err)
			}
		}
		return base
	}

	// invalid fields: wrong type and out of range index
	base := writeGroup(dbfBytes, fptBytes, true)
	r, err := Open(base + ".shp")
	if err != nil {
		t.Fatal(err)
	}
	defer r.Close()
	if _, err := r.ReadMemo(0, 0); !errors.Is(err, ErrMemoField) {
		t.Fatalf("C field: expected ErrMemoField, got %v", err)
	}
	if _, err := r.ReadMemo(0, 7); !errors.Is(err, ErrMemoField) {
		t.Fatalf("bad index: expected ErrMemoField, got %v", err)
	}
	checks.Check(t, true)

	// missing FPT: geometry and plain attributes keep working, only memo
	// reads fail
	base = writeGroup(dbfBytes, nil, false)
	rNoFpt, err := Open(base + ".shp")
	if err != nil {
		t.Fatal(err)
	}
	defer rNoFpt.Close()
	checks.Check(t, rNoFpt.Next() && rNoFpt.ReadAttribute(0, 0) == "alpha")
	if _, err := rNoFpt.ReadMemo(0, 1); !errors.Is(err, ErrMemoUnavailable) {
		t.Fatalf("expected ErrMemoUnavailable, got %v", err)
	}
	checks.Check(t, true)

	// truncated memo pointer: the DBF ends in the middle of the M field
	headerLen := 32 + 32*2 + 1 + 263
	truncated := dbfBytes[:headerLen+1+10+2]
	base = writeGroup(truncated, fptBytes, true)
	rTrunc, err := Open(base + ".shp")
	if err != nil {
		t.Fatal(err)
	}
	defer rTrunc.Close()
	if _, err := rTrunc.ReadMemo(0, 1); !errors.Is(err, ErrMemoPointer) {
		t.Fatalf("expected ErrMemoPointer, got %v", err)
	}
	checks.Check(t, true)

	// helper for the FPT-level failures: a one-row stream reader whose
	// single M field points at block 1
	oneRow := func(fpt []byte, fptSize int64) SequentialReaderWithMemo {
		dbf := buildMemoDBF(t, 0x30, memoTestFields, [][]byte{
			memoRecord(0x20, memoCharValue("solo", 10), memoPtrValue(1)),
		}, 0)
		return SequentialReaderFromExtWithMemo(
			io.NopCloser(bytes.NewReader(buildMemoSHP([2]float64{1, 2}))),
			io.NopCloser(bytes.NewReader(dbf)),
			bytes.NewReader(fpt), fptSize)
	}
	nextMemo := func(sr SequentialReaderWithMemo) error {
		if !sr.Next() {
			t.Fatalf("cannot advance: %v", sr.Err())
		}
		_, err := sr.Memo(1)
		return err
	}

	// memo block beyond the end of the FPT file
	far := buildMemoFPT(t, 64, map[uint32]memoTestBlock{
		1: {kind: MemoKindText, data: []byte("ok")},
	})
	dbfFar := buildMemoDBF(t, 0x30, memoTestFields, [][]byte{
		memoRecord(0x20, memoCharValue("solo", 10), memoPtrValue(1000)),
	}, 0)
	srFar := SequentialReaderFromExtWithMemo(
		io.NopCloser(bytes.NewReader(buildMemoSHP([2]float64{1, 2}))),
		io.NopCloser(bytes.NewReader(dbfFar)),
		bytes.NewReader(far), int64(len(far)))
	if err := nextMemo(srFar); !errors.Is(err, ErrMemoBlockHeader) {
		t.Fatalf("expected ErrMemoBlockHeader, got %v", err)
	}
	checks.Check(t, true)

	// FPT file smaller than its 512 byte header
	if err := nextMemo(oneRow(make([]byte, 100), 100)); !errors.Is(err, ErrMemoBlockHeader) {
		t.Fatalf("expected ErrMemoBlockHeader for short FPT, got %v", err)
	}
	checks.Check(t, true)

	// unsupported memo block type
	typed := buildMemoFPT(t, 64, map[uint32]memoTestBlock{
		1: {kind: 7, data: []byte("xx")},
	})
	if err := nextMemo(oneRow(typed, int64(len(typed)))); !errors.Is(err, ErrMemoType) {
		t.Fatalf("expected ErrMemoType, got %v", err)
	}
	checks.Check(t, true)

	// declared memo length exceeds the remaining FPT data
	oversized := buildMemoFPT(t, 64, map[uint32]memoTestBlock{
		1: {kind: MemoKindText, data: []byte("ok")},
	})
	binary.BigEndian.PutUint32(oversized[64+4:], 0x00ffffff)
	if err := nextMemo(oneRow(oversized, int64(len(oversized)))); !errors.Is(err, ErrMemoLength) {
		t.Fatalf("expected ErrMemoLength, got %v", err)
	}
	checks.Check(t, true)

	// the FPT source is shorter than its declared size, so the body read
	// comes up short
	short := buildMemoFPT(t, 64, map[uint32]memoTestBlock{
		1: {kind: MemoKindText, data: []byte("ok")},
	})
	binary.BigEndian.PutUint32(short[64+4:], 1000)
	if err := nextMemo(oneRow(short, 10000)); !errors.Is(err, ErrMemoBody) {
		t.Fatalf("expected ErrMemoBody, got %v", err)
	}
	checks.Check(t, true)

	// a failed memo read does not move the geometry or DBF position: the
	// reader above failed on row 0, a fresh failing read on the file based
	// reader must be followed by fully aligned rows
	if _, err := r.ReadMemo(0, 0); !errors.Is(err, ErrMemoField) {
		t.Fatalf("expected ErrMemoField, got %v", err)
	}
	n := 0
	for r.Next() {
		checks.Check(t, r.ReadAttribute(n, 0) == memoTestNames[n])
		n++
	}
	checks.Check(t, n == len(memoTestNames) && r.Err() == nil)
}

// TestMemoOwnership verifies that memo results of repeated reads, of
// different rows referencing the same block and of different calls never
// share writable Data buffers.
func TestMemoOwnership(t *testing.T) {
	shpBytes, dbfBytes, fptBytes := memoTestFixture(t)
	dir := t.TempDir()
	base := filepath.Join(dir, "points")
	for ext, data := range map[string][]byte{
		".shp": shpBytes, ".dbf": dbfBytes, ".fpt": fptBytes,
	} {
		if err := os.WriteFile(base+ext, data, 0644); err != nil {
			t.Fatal(err)
		}
	}
	r, err := Open(base + ".shp")
	if err != nil {
		t.Fatal(err)
	}
	defer r.Close()

	// rows 0 and 2 reference the same block 6
	first, err := r.ReadMemo(0, 1)
	if err != nil {
		t.Fatal(err)
	}
	second, err := r.ReadMemo(2, 1)
	if err != nil {
		t.Fatal(err)
	}
	checks.Check(t, bytes.Equal(first.Data, second.Data))
	first.Data[0] = 'X'
	checks.Check(t, string(second.Data) == "hello memo world")
	third, err := r.ReadMemo(0, 1)
	if err != nil {
		t.Fatal(err)
	}
	checks.Check(t, string(third.Data) == "hello memo world")

	// same for the streaming reader: two calls for the current row and the
	// same block referenced from a later row must not alias
	sr := SequentialReaderFromExtWithMemo(
		io.NopCloser(bytes.NewReader(shpBytes)),
		io.NopCloser(bytes.NewReader(dbfBytes)),
		bytes.NewReader(fptBytes), int64(len(fptBytes)))
	defer sr.Close()
	if !sr.Next() {
		t.Fatal("expected first row")
	}
	a, err := sr.Memo(1)
	if err != nil {
		t.Fatal(err)
	}
	b, err := sr.Memo(1)
	if err != nil {
		t.Fatal(err)
	}
	a.Data[0] = 'Y'
	checks.Check(t, string(b.Data) == "hello memo world")
	if !sr.Next() || !sr.Next() { // deleted row, then row 2 (same block)
		t.Fatal("expected more rows")
	}
	c, err := sr.Memo(1)
	if err != nil {
		t.Fatal(err)
	}
	checks.Check(t, string(c.Data) == "hello memo world")

	// a zero length memo is present but empty
	empty, err := r.ReadMemo(4, 1)
	if err != nil {
		t.Fatal(err)
	}
	checks.Check(t, empty.Present && len(empty.Data) == 0)
}
