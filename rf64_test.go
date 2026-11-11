package wav

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"io"
	"math"
	"reflect"
	"strings"
	"testing"
	"time"

	"github.com/go-audio/audio"
)

// ---------------------------------------------------------------------------
// Independently assembled RF64 wire fixtures (never encoder output).
// ---------------------------------------------------------------------------

func rfLE(v interface{}) []byte {
	var b bytes.Buffer
	if err := binary.Write(&b, binary.LittleEndian, v); err != nil {
		panic(err)
	}
	return b.Bytes()
}

func rfBE(v interface{}) []byte {
	var b bytes.Buffer
	if err := binary.Write(&b, binary.BigEndian, v); err != nil {
		panic(err)
	}
	return b.Bytes()
}

func rfCat(parts ...[]byte) []byte {
	var out bytes.Buffer
	for _, p := range parts {
		out.Write(p)
	}
	return out.Bytes()
}

// rfWireChunk writes a chunk. A non-zero declared overrides the 32-bit size
// field; the payload length stays the real, physical one. Padding is based on
// the physical payload length, as required by RIFF/RF64.
func rfWireChunk(id string, declared uint32, payload []byte) []byte {
	var b bytes.Buffer
	b.WriteString(id)
	if declared == 0 {
		declared = uint32(len(payload))
	}
	b.Write(rfLE(declared))
	b.Write(payload)
	if len(payload)%2 == 1 {
		b.WriteByte(0)
	}
	return b.Bytes()
}

func rfFmtBytes(bits, chans int, rate uint32) []byte {
	blockAlign := chans * bits / 8
	var b bytes.Buffer
	b.Write(rfLE(uint16(1)))
	b.Write(rfLE(uint16(chans)))
	b.Write(rfLE(rate))
	b.Write(rfLE(rate * uint32(blockAlign)))
	b.Write(rfLE(uint16(blockAlign)))
	b.Write(rfLE(uint16(bits)))
	return b.Bytes()
}

func rfPCMBytes(bits, chans int, samples []int) []byte {
	var b bytes.Buffer
	for _, v := range samples {
		switch bits {
		case 8:
			b.WriteByte(byte(v))
		case 16:
			b.Write(rfLE(int16(v)))
		case 24:
			b.Write(audio.Int32toInt24LEBytes(int32(v)))
		case 32:
			b.Write(rfLE(int32(v)))
		default:
			panic("bad bit depth")
		}
	}
	return b.Bytes()
}

func rfINFO(entries ...[2]string) []byte {
	var b bytes.Buffer
	b.WriteString("INFO")
	for _, e := range entries {
		b.WriteString(e[0])
		b.Write(rfLE(uint32(len(e[1]) + 1)))
		b.WriteString(e[1])
		b.WriteByte(0)
	}
	return b.Bytes()
}

type rfTableEntry struct {
	id string
	sz uint64
}

func rfDS64Bytes(riffSize, dataSize, sampleCount uint64, table []rfTableEntry) []byte {
	var b bytes.Buffer
	b.Write(rfLE(riffSize))
	b.Write(rfLE(dataSize))
	b.Write(rfLE(sampleCount))
	b.Write(rfLE(uint32(len(table))))
	for _, e := range table {
		b.WriteString(e.id)
		b.Write(rfLE(e.sz))
	}
	return b.Bytes()
}

// buildRF64Wire assembles a complete RF64/WAVE file. riffSize may be
// overridden to create deliberately inconsistent containers.
func buildRF64Wire(riffSize *uint64, dataSize, sampleCount uint64, table []rfTableEntry, chunks ...[]byte) []byte {
	tail := rfCat(chunks...)
	ds := rfDS64Bytes(0, dataSize, sampleCount, table)
	dsChunk := rfWireChunk("ds64", 0, ds)
	rs := uint64(12+len(dsChunk)+len(tail)) - 8
	if riffSize != nil {
		rs = *riffSize
	}
	ds = rfDS64Bytes(rs, dataSize, sampleCount, table)
	var out bytes.Buffer
	out.WriteString("RF64")
	out.Write(rfLE(uint32(0xffffffff)))
	out.WriteString("WAVE")
	out.Write(rfWireChunk("ds64", 0, ds))
	out.Write(tail)
	return out.Bytes()
}

func frameCount(samples []int, chans int) uint64 {
	return uint64(len(samples) / chans)
}

func decodeAll(t *testing.T, wire []byte) *audio.IntBuffer {
	t.Helper()
	d := NewDecoder(bytes.NewReader(wire))
	buf, err := d.FullPCMBuffer()
	if err != nil {
		t.Fatalf("decoding RF64: %v", err)
	}
	return buf
}

func intBuffer(samples []int, chans, rate int) *audio.IntBuffer {
	return &audio.IntBuffer{
		Data:   append([]int(nil), samples...),
		Format: &audio.Format{NumChannels: chans, SampleRate: rate},
	}
}

// ---------------------------------------------------------------------------
// Sparse in-memory ReadWriteSeeker: keeps only written segments and a logical
// length, so several GiB of holes cost nothing and reads are budgeted.
// ---------------------------------------------------------------------------

type sparseSeg struct {
	off int64
	b   []byte
}

type sparseRW struct {
	segments []sparseSeg
	length   int64
	pos      int64
	reads    int64
	budget   int64 // >0: fail reads above this many bytes
}

func newSparseRW(budget int64) *sparseRW { return &sparseRW{budget: budget} }

func (s *sparseRW) Write(p []byte) (int, error) {
	if len(p) == 0 {
		return 0, nil
	}
	s.segments = append(s.segments, sparseSeg{off: s.pos, b: append([]byte(nil), p...)})
	s.pos += int64(len(p))
	if s.pos > s.length {
		s.length = s.pos
	}
	return len(p), nil
}

func (s *sparseRW) Read(p []byte) (int, error) {
	if s.pos >= s.length {
		return 0, io.EOF
	}
	if int64(len(p)) > s.length-s.pos {
		p = p[:s.length-s.pos]
	}
	for i := range p {
		p[i] = 0
	}
	for _, seg := range s.segments {
		end := seg.off + int64(len(seg.b))
		lo := seg.off
		if lo < s.pos {
			lo = s.pos
		}
		hi := end
		if hi > s.pos+int64(len(p)) {
			hi = s.pos + int64(len(p))
		}
		if hi <= lo {
			continue
		}
		copy(p[lo-s.pos:hi-s.pos], seg.b[lo-seg.off:hi-seg.off])
	}
	s.reads += int64(len(p))
	if s.budget > 0 && s.reads > s.budget {
		return 0, fmt.Errorf("sparse read budget exceeded (%d bytes)", s.reads)
	}
	s.pos += int64(len(p))
	return len(p), nil
}

func (s *sparseRW) Seek(off int64, whence int) (int64, error) {
	switch whence {
	case io.SeekStart:
		s.pos = off
	case io.SeekCurrent:
		s.pos += off
	case io.SeekEnd:
		s.pos = s.length + off
	default:
		return 0, fmt.Errorf("bad whence %d", whence)
	}
	if s.pos < 0 {
		return 0, fmt.Errorf("negative position %d", s.pos)
	}
	return s.pos, nil
}

func (s *sparseRW) writeAt(off int64, p []byte) {
	if _, err := s.Seek(off, io.SeekStart); err != nil {
		panic(err)
	}
	if _, err := s.Write(p); err != nil {
		panic(err)
	}
}

// fullBytes materializes the whole logical stream; only use for small files.
func (s *sparseRW) fullBytes() []byte {
	buf := make([]byte, s.length)
	if _, err := s.Seek(0, io.SeekStart); err != nil {
		panic(err)
	}
	if _, err := io.ReadFull(s, buf); err != nil {
		panic(err)
	}
	return buf
}

// faultRW wraps sparseRW and arms seek/write failures for Close() testing.
type faultRW struct {
	base       *sparseRW
	armed      bool
	failSeekAt int64
	shortAt    int64
	errAt      int64
	errVal     error
}

func (f *faultRW) Write(p []byte) (int, error) {
	if f.armed {
		pos := f.base.pos
		if f.errAt >= 0 && pos >= f.errAt {
			return 0, f.errVal
		}
		if f.shortAt >= 0 && pos >= f.shortAt {
			n := len(p) - 1
			if n < 0 {
				n = 0
			}
			_, _ = f.base.Write(p[:n])
			return n, nil
		}
	}
	return f.base.Write(p)
}

func (f *faultRW) Seek(off int64, whence int) (int64, error) {
	if f.armed && f.failSeekAt >= 0 && whence == io.SeekStart && off == f.failSeekAt {
		return 0, fmt.Errorf("injected seek failure at %d", off)
	}
	return f.base.Seek(off, whence)
}

// sparseRF64File builds a logically multi-GiB RF64 file with an unknown
// chunk before fmt, the PCM data and a LIST/INFO trailer at the far end.
func sparseRF64File(t *testing.T, huge int64, bits, chans int, rate uint32, samples []int, info []byte) *sparseRW {
	t.Helper()
	pcm := rfPCMBytes(bits, chans, samples)
	table := []rfTableEntry{{id: "JUNK", sz: uint64(huge)}}
	tail := rfCat(
		rfWireChunk("fmt ", 0, rfFmtBytes(bits, chans, rate)),
		rfWireChunk("data", 0xffffffff, pcm),
		rfWireChunk("LIST", 0, info),
	)
	// header part: RF64 + size + WAVE + ds64 + JUNK header (payload omitted)
	dsLen := len(rfDS64Bytes(0, 0, 0, table))
	headLen := int64(12 + 8 + dsLen + 8)
	total := headLen + huge + int64(len(tail))
	ds := rfDS64Bytes(uint64(total)-8, uint64(len(pcm)), frameCount(samples, chans), table)
	var head bytes.Buffer
	head.WriteString("RF64")
	head.Write(rfLE(uint32(0xffffffff)))
	head.WriteString("WAVE")
	head.Write(rfWireChunk("ds64", 0, ds))
	head.WriteString("JUNK")
	head.Write(rfLE(uint32(0xffffffff)))
	if int64(head.Len()) != headLen {
		t.Fatalf("head layout %d != %d", head.Len(), headLen)
	}
	sp := newSparseRW(8 << 20)
	sp.writeAt(0, head.Bytes())
	sp.writeAt(headLen+huge, tail)
	if sp.length != total {
		t.Fatalf("sparse length %d != %d", sp.length, total)
	}
	return sp
}

// sparseRF64HugeData builds a logically huge RF64 file whose data chunk
// payload is never materialized: only the header bytes are written, plus one
// byte at the end so the sparse stream reports the full logical length.
func sparseRF64HugeData(t *testing.T, dataSize uint64) *sparseRW {
	t.Helper()
	fm := rfWireChunk("fmt ", 0, rfFmtBytes(8, 1, 8000))
	baseLen := int64(12 + 8 + 28 + len(fm) + 8) // RF64, ds64, fmt, data header
	total := baseLen + int64(dataSize)
	ds := rfDS64Bytes(uint64(total)-8, dataSize, 0, nil)
	head := rfCat(
		[]byte("RF64"), rfLE(uint32(0xffffffff)), []byte("WAVE"),
		rfWireChunk("ds64", 0, ds),
		fm,
		[]byte("data"), rfLE(uint32(0xffffffff)),
	)
	if int64(len(head)) != baseLen {
		t.Fatalf("head layout %d != %d", len(head), baseLen)
	}
	sp := newSparseRW(8 << 20)
	sp.writeAt(0, head)
	sp.writeAt(total-1, []byte{0})
	if sp.length != total {
		t.Fatalf("sparse length %d != %d", sp.length, total)
	}
	return sp
}

// ---------------------------------------------------------------------------
// TestRF64Read
// ---------------------------------------------------------------------------

func TestRF64Read(t *testing.T) {
	t.Run("basic", func(t *testing.T) {
		samples := []int{17, -23, 91, -101}
		pcm := rfPCMBytes(16, 2, samples)
		wire := buildRF64Wire(nil, uint64(len(pcm)), 2, nil,
			rfWireChunk("fmt ", 0, rfFmtBytes(16, 2, 8000)),
			rfWireChunk("data", 0xffffffff, pcm),
		)
		buf := decodeAll(t, wire)
		if !reflect.DeepEqual(buf.Data, samples) {
			t.Fatalf("samples: %v", buf.Data)
		}
		d := NewDecoder(bytes.NewReader(wire))
		if err := d.FwdToPCM(); err != nil {
			t.Fatal(err)
		}
		if d.PCMLen() != int64(len(pcm)) {
			t.Fatalf("PCMLen %d", d.PCMLen())
		}
		dur, err := d.Duration()
		if err != nil {
			t.Fatal(err)
		}
		if dur != 2*time.Second/8000 {
			t.Fatalf("duration %s", dur)
		}
	})

	t.Run("ordinary outer length wins over ds64 riffSize", func(t *testing.T) {
		samples := []int{3, -4}
		pcm := rfPCMBytes(16, 1, samples)
		wire := buildRF64Wire(nil, uint64(len(pcm)), 2, nil,
			rfWireChunk("fmt ", 0, rfFmtBytes(16, 1, 8000)),
			rfWireChunk("data", 0xffffffff, pcm),
		)
		// outer field not the sentinel: the 32-bit value is authoritative
		binary.LittleEndian.PutUint32(wire[4:8], uint32(len(wire)-8))
		binary.LittleEndian.PutUint64(wire[20:28], uint64(1)<<40)
		if buf := decodeAll(t, wire); !reflect.DeepEqual(buf.Data, samples) {
			t.Fatalf("samples: %v", buf.Data)
		}
		d := NewDecoder(bytes.NewReader(wire))
		if err := d.FwdToPCM(); err != nil {
			t.Fatal(err)
		}
		if d.PCMLen() != int64(len(pcm)) {
			t.Fatalf("PCMLen %d", d.PCMLen())
		}
	})

	t.Run("unknown chunks and repeated table entries", func(t *testing.T) {
		samples := []int{5, -5, 6, -6}
		pcm := rfPCMBytes(16, 2, samples)
		// The first ABCD carries an ordinary length and must not consume a
		// table entry; the next two sentinels take the entries in wire order.
		wire := buildRF64Wire(nil, uint64(len(pcm)), 2,
			[]rfTableEntry{{id: "ABCD", sz: 6}, {id: "ABCD", sz: 8}},
			rfWireChunk("XBEG", 0, []byte{9, 8, 7, 6, 5, 4}), // unknown before fmt
			rfWireChunk("fmt ", 0, rfFmtBytes(16, 2, 8000)),
			rfWireChunk("ABCD", 0, []byte{1, 2, 3, 4}), // ordinary
			rfWireChunk("ABCD", 0xffffffff, []byte{1, 2, 3, 4, 5, 6}),
			rfWireChunk("ABCD", 0xffffffff, []byte{1, 2, 3, 4, 5, 6, 7, 8}),
			rfWireChunk("data", 0xffffffff, pcm),
			rfWireChunk("LIST", 0, rfINFO([2]string{"IART", "tail artist"})),
		)
		// Decoding reaches data through the unknown chunks.
		if buf := decodeAll(t, wire); !reflect.DeepEqual(buf.Data, samples) {
			t.Fatalf("samples: %v", buf.Data)
		}

		// NextChunk exposes each chunk with its payload-only Size.
		d := NewDecoder(bytes.NewReader(wire))
		d.ReadInfo()
		var sizes []int
		for {
			ch, err := d.NextChunk()
			if err != nil {
				break
			}
			sizes = append(sizes, ch.Size)
			if len(sizes) >= 8 {
				break
			}
		}
		// Sequence from the first chunk after ds64 is:
		// XBEG(6), fmt(16), ABCD(4), ABCD(6), ABCD(8), data(...), LIST(...)
		infoLen := len(rfINFO([2]string{"IART", "tail artist"}))
		want := []int{6, 16, 4, 6, 8, len(pcm), infoLen}
		if len(sizes) != len(want) || !reflect.DeepEqual(sizes, want) {
			t.Fatalf("chunk sizes %v, want %v", sizes, want)
		}
	})

	t.Run("bit depths and channel counts", func(t *testing.T) {
		cases := []struct {
			bits, chans int
			samples     []int
		}{
			{8, 1, []int{0, 1, 254}}, // 3 frames, odd payload + padding
			{8, 2, []int{10, 20, 30, 40}},
			{16, 1, []int{-7, 8, -9}},
			{16, 2, []int{100, -100, 200, -200, 300, -300}},
			{24, 1, []int{-1000, 2000, -3000}},
			{24, 2, []int{1, -1, 2, -2, 3, -3}},
			{32, 1, []int{math.MinInt32, 0, math.MaxInt32}},
			{32, 2, []int{1 << 20, -(1 << 20), 2 << 20, -(2 << 20)}},
		}
		for _, c := range cases {
			name := fmt.Sprintf("%db_%dch", c.bits, c.chans)
			t.Run(name, func(t *testing.T) {
				pcm := rfPCMBytes(c.bits, c.chans, c.samples)
				frames := frameCount(c.samples, c.chans)
				wire := buildRF64Wire(nil, uint64(len(pcm)), frames, nil,
					rfWireChunk("fmt ", 0, rfFmtBytes(c.bits, c.chans, 44100)),
					rfWireChunk("data", 0xffffffff, pcm),
				)
				buf := decodeAll(t, wire)
				if !reflect.DeepEqual(buf.Data, c.samples) {
					t.Fatalf("samples: got %v want %v", buf.Data, c.samples)
				}
				d := NewDecoder(bytes.NewReader(wire))
				d.ReadInfo()
				d.FwdToPCM()
				if d.PCMLen() != int64(len(pcm)) {
					t.Fatalf("PCMLen %d want %d (padding must not count)", d.PCMLen(), len(pcm))
				}
				dur, err := d.Duration()
				if err != nil {
					t.Fatal(err)
				}
				if dur != time.Duration(frames)*time.Second/44100 {
					t.Fatalf("duration %s", dur)
				}
			})
		}
	})

	t.Run("large sparse skip", func(t *testing.T) {
		const huge = int64(4) << 30 // 4 GiB unknown chunk
		samples := []int{123, -456, 789, -1000, 2345, -6789}
		info := rfINFO([2]string{"IART", "sparse artist"}, [2]string{"INAM", "far title"})
		sp := sparseRF64File(t, huge, 16, 1, 8000, samples, info)
		if _, err := sp.Seek(0, io.SeekStart); err != nil {
			t.Fatal(err)
		}

		d := NewDecoder(sp)
		if err := d.FwdToPCM(); err != nil {
			t.Fatalf("FwdToPCM: %v", err)
		}
		if sp.reads > 1<<20 {
			t.Fatalf("decoder read %d bytes while skipping a 4 GiB chunk", sp.reads)
		}
		if d.PCMLen() != int64(2*len(samples)) {
			t.Fatalf("PCMLen %d", d.PCMLen())
		}
		// One PCM sample, then duration, metadata, then the following sample.
		one := &audio.IntBuffer{Data: make([]int, 1)}
		if n, err := d.PCMBuffer(one); err != nil || n != 1 || one.Data[0] != samples[0] {
			t.Fatalf("first sample: n=%d v=%v err=%v", n, one.Data, err)
		}
		dur, err := d.Duration()
		if err != nil || dur != time.Duration(len(samples))*time.Second/8000 {
			t.Fatalf("duration %s err=%v", dur, err)
		}
		d.ReadMetadata()
		if d.Err() != nil {
			t.Fatalf("metadata: %v", d.Err())
		}
		if d.Metadata == nil || d.Metadata.Artist != "sparse artist" || d.Metadata.Title != "far title" {
			t.Fatalf("metadata %#v", d.Metadata)
		}
		next := &audio.IntBuffer{Data: make([]int, 1)}
		if n, err := d.PCMBuffer(next); err != nil || n != 1 || next.Data[0] != samples[1] {
			t.Fatalf("next sample: n=%d v=%v err=%v", n, next.Data, err)
		}
		rest := &audio.IntBuffer{Data: make([]int, len(samples))}
		n, err := d.PCMBuffer(rest)
		if err != nil {
			t.Fatal(err)
		}
		if !reflect.DeepEqual(rest.Data[:n], samples[2:]) {
			t.Fatalf("remote PCM: %v", rest.Data[:n])
		}
		if sp.reads > 1<<20 {
			t.Fatalf("total reads %d", sp.reads)
		}
	})

	t.Run("chunk payload boundaries", func(t *testing.T) {
		pcm := rfPCMBytes(16, 1, []int{1, 2})
		info := rfINFO([2]string{"INAM", "x"})
		wire := buildRF64Wire(nil, uint64(len(pcm)), 2, nil,
			rfWireChunk("BEFO", 0, []byte{1, 2, 3, 4}),
			rfWireChunk("fmt ", 0, rfFmtBytes(16, 1, 8000)),
			rfWireChunk("odd!", 0, []byte{0xaa, 0xbb, 0xcc}), // 3 bytes + padding
			rfWireChunk("data", 0xffffffff, pcm),
			rfWireChunk("LIST", 0, info),
		)
		d := NewDecoder(bytes.NewReader(wire))
		d.ReadInfo()
		var seq []string
		for {
			ch, err := d.NextChunk()
			if err != nil {
				break
			}
			seq = append(seq, string(ch.ID[:]))
			if string(ch.ID[:]) == "odd!" {
				if ch.Size != 3 {
					t.Fatalf("odd! Size=%d, padding must not be exposed", ch.Size)
				}
				// The limited reader stops at the declared payload and never
				// exposes the padding byte or the following chunk.
				_, rerr := io.ReadFull(ch.R, make([]byte, 4))
				if rerr == nil || !strings.Contains(rerr.Error(), "unexpected EOF") {
					t.Fatalf("reading past odd payload: %v", rerr)
				}
			}
			if string(ch.ID[:]) == "data" {
				if ch.Size != len(pcm) {
					t.Fatalf("data Size=%d", ch.Size)
				}
			}
			if string(ch.ID[:]) == "LIST" {
				if ch.Size != len(info) {
					t.Fatalf("LIST Size=%d want %d", ch.Size, len(info))
				}
			}
		}
		want := []string{"BEFO", "fmt ", "odd!", "data", "LIST"}
		if !reflect.DeepEqual(seq, want) {
			t.Fatalf("chunks %v want %v", seq, want)
		}
	})
}

// ---------------------------------------------------------------------------
// TestRF64Metadata
// ---------------------------------------------------------------------------

func TestRF64Metadata(t *testing.T) {
	samples := []int{11, -22, 33, -44, 55, -66}
	pcm := rfPCMBytes(16, 1, samples)
	info := rfINFO(
		[2]string{"IART", "rf64 artist"},
		[2]string{"INAM", "rf64 title"},
		[2]string{"ICMT", "interleaved"},
	)
	wire := buildRF64Wire(nil, uint64(len(pcm)), frameCount(samples, 1), nil,
		rfWireChunk("fmt ", 0, rfFmtBytes(16, 1, 8000)),
		rfWireChunk("data", 0xffffffff, pcm),
		rfWireChunk("LIST", 0, info),
	)
	d := NewDecoder(bytes.NewReader(wire))

	// Read one frame (one sample in mono), then query duration.
	first := &audio.IntBuffer{Data: make([]int, 1)}
	if n, err := d.PCMBuffer(first); err != nil || n != 1 || first.Data[0] != samples[0] {
		t.Fatalf("first: n=%d %v %v", n, first.Data, err)
	}
	dur, err := d.Duration()
	if err != nil {
		t.Fatal(err)
	}
	if dur != 6*time.Second/8000 {
		t.Fatalf("duration %s (container length must not be used)", dur)
	}
	if d.PCMLen() != int64(len(pcm)) {
		t.Fatalf("PCMLen %d", d.PCMLen())
	}

	// Reading footer metadata must not disturb the PCM cursor.
	d.ReadMetadata()
	if d.Err() != nil {
		t.Fatalf("ReadMetadata: %v", d.Err())
	}
	if d.Metadata == nil || d.Metadata.Artist != "rf64 artist" || d.Metadata.Title != "rf64 title" || d.Metadata.Comments != "interleaved" {
		t.Fatalf("metadata %#v", d.Metadata)
	}
	second := &audio.IntBuffer{Data: make([]int, 1)}
	if n, err := d.PCMBuffer(second); err != nil || n != 1 || second.Data[0] != samples[1] {
		t.Fatalf("second: n=%d %v %v", n, second.Data, err)
	}

	// Continue to the end and confirm all samples.
	rest := &audio.IntBuffer{Data: make([]int, len(samples))}
	n, err := d.PCMBuffer(rest)
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(rest.Data[:n], samples[2:]) {
		t.Fatalf("rest: %v", rest.Data[:n])
	}

	// Rewind restarts the ds64 table from the beginning and yields the same
	// audio in the same order.
	if err := d.Rewind(); err != nil {
		t.Fatalf("Rewind: %v", err)
	}
	all := &audio.IntBuffer{Data: make([]int, len(samples))}
	n, err = d.PCMBuffer(all)
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(all.Data[:n], samples) {
		t.Fatalf("after rewind: %v", all.Data[:n])
	}

	// FullPCMBuffer works on a fresh reader too.
	if buf := decodeAll(t, wire); !reflect.DeepEqual(buf.Data, samples) {
		t.Fatalf("FullPCMBuffer: %v", buf.Data)
	}
}

// ---------------------------------------------------------------------------
// TestRF64AutoPromote
// ---------------------------------------------------------------------------

func TestRF64AutoPromote(t *testing.T) {
	t.Run("small file keeps RIFF and JUNK", func(t *testing.T) {
		sp := newSparseRW(0)
		e := NewEncoder(sp, 8000, 16, 2, 1)
		e.AutoRF64 = true
		e.Metadata = &Metadata{Artist: "encoded artist", Title: "encoded title"}
		if err := e.Write(intBuffer([]int{1, -1, 2, -2}, 2, 8000)); err != nil {
			t.Fatal(err)
		}
		// Changing AutoRF64 after the first write must have no effect.
		e.AutoRF64 = false
		if err := e.WriteFrame(uint32(0xfffd0003)); err != nil { // samples 3, -3
			t.Fatal(err)
		}
		if err := e.Write(intBuffer([]int{4, -4}, 2, 8000)); err != nil {
			t.Fatal(err)
		}
		if err := e.Close(); err != nil {
			t.Fatal(err)
		}
		raw := sp.fullBytes()

		// Independent header inspection.
		if string(raw[0:4]) != "RIFF" {
			t.Fatalf("small file promoted unexpectedly: %q", raw[0:4])
		}
		if got := binary.LittleEndian.Uint32(raw[4:8]); got != uint32(len(raw)-8) {
			t.Fatalf("riff size %d want %d", got, len(raw)-8)
		}
		if string(raw[8:12]) != "WAVE" || string(raw[12:16]) != "JUNK" {
			t.Fatalf("header %q %q", raw[8:12], raw[12:16])
		}
		if got := binary.LittleEndian.Uint32(raw[16:20]); got != 28 {
			t.Fatalf("JUNK size %d", got)
		}
		// fmt starts after the 36 byte JUNK chunk, data after the 24 byte fmt.
		if string(raw[48:52]) != "fmt " || string(raw[72:76]) != "data" {
			t.Fatalf("fmt/data IDs %q %q", raw[48:52], raw[72:76])
		}
		if got := binary.LittleEndian.Uint32(raw[76:80]); got != 16 {
			t.Fatalf("data size %d", got)
		}
		want := []int{1, -1, 2, -2, 3, -3, 4, -4}
		if buf := decodeAll(t, raw); !reflect.DeepEqual(buf.Data, want) {
			t.Fatalf("decoded %v", buf.Data)
		}
		d := NewDecoder(bytes.NewReader(raw))
		d.ReadMetadata()
		if d.Metadata == nil || d.Metadata.Artist != "encoded artist" {
			t.Fatalf("metadata %#v", d.Metadata)
		}
	})

	// helper: encode an auto header and leave the data chunk size reserved.
	prepare := func(t *testing.T, sp *sparseRW, bits, chans int) *Encoder {
		t.Helper()
		e := NewEncoder(sp, 8000, bits, chans, 1)
		e.AutoRF64 = true
		e.lockMode()
		if err := e.writeHeader(); err != nil {
			t.Fatal(err)
		}
		if err := e.startPCMChunk(); err != nil {
			t.Fatal(err)
		}
		return e
	}
	const base = 80 // RF64 header, JUNK, fmt and data header

	t.Run("below both thresholds stays RIFF", func(t *testing.T) {
		sp := newSparseRW(0)
		e := prepare(t, sp, 16, 1)
		x := int64(0xffffff00)
		e.pcmBytes = x
		e.frames = int(x / 2)
		e.WrittenBytes = base + int(x)
		if _, err := sp.Seek(int64(e.WrittenBytes), io.SeekStart); err != nil {
			t.Fatal(err)
		}
		if err := e.WriteFrame(int16(0x1234)); err != nil {
			t.Fatal(err)
		}
		if err := e.Close(); err != nil {
			t.Fatal(err)
		}
		hdr := readSparseHeader(t, sp, 80)
		if string(hdr[0:4]) != "RIFF" || string(hdr[12:16]) != "JUNK" {
			t.Fatalf("expected RIFF/JUNK, got %q %q", hdr[0:4], hdr[12:16])
		}
		if got := binary.LittleEndian.Uint32(hdr[4:8]); got != uint32(sp.length-8) {
			t.Fatalf("riff size %d want %d", got, sp.length-8)
		}
		if got := binary.LittleEndian.Uint32(hdr[76:80]); got != uint32(e.pcmBytes) {
			t.Fatalf("data size %d want %d", got, e.pcmBytes)
		}
	})

	t.Run("data size reaching the sentinel promotes", func(t *testing.T) {
		sp := newSparseRW(0)
		e := prepare(t, sp, 16, 1)
		x := int64(0xfffffffe)
		e.pcmBytes = x
		e.frames = int(x / 2)
		e.WrittenBytes = base + int(x)
		if _, err := sp.Seek(int64(e.WrittenBytes), io.SeekStart); err != nil {
			t.Fatal(err)
		}
		// real Write at the boundary: one more 16-bit mono frame
		if err := e.Write(intBuffer([]int{0x1234}, 1, 8000)); err != nil {
			t.Fatal(err)
		}
		if err := e.Close(); err != nil {
			t.Fatal(err)
		}
		checkPromoted(t, sp, uint64(sp.length)-8, uint64(e.pcmBytes), uint64(e.frames))
	})

	t.Run("only container length reaching the sentinel promotes", func(t *testing.T) {
		sp := newSparseRW(0)
		e := prepare(t, sp, 16, 1)
		e.pcmBytes = 8
		e.frames = 4
		e.WrittenBytes = 0x100000006 // two bytes below; WriteFrame pushes it over
		if _, err := sp.Seek(int64(e.WrittenBytes), io.SeekStart); err != nil {
			t.Fatal(err)
		}
		if err := e.WriteFrame(int16(7)); err != nil {
			t.Fatal(err)
		}
		if err := e.Close(); err != nil {
			t.Fatal(err)
		}
		if uint64(e.pcmBytes) >= uint64(rf64LengthSentinel) {
			t.Fatal("audio size unexpectedly at the sentinel")
		}
		checkPromoted(t, sp, uint64(sp.length)-8, uint64(e.pcmBytes), uint64(e.frames))
	})

	t.Run("metadata and odd padding count for riffSize only", func(t *testing.T) {
		sp := newSparseRW(0)
		e := prepare(t, sp, 8, 1)
		// Odd audio size forces the alignment byte at Close.
		e.pcmBytes = int64(0xffffffff)
		e.frames = int(e.pcmBytes) // blockAlign == 1
		e.WrittenBytes = base + int(e.pcmBytes)
		if _, err := sp.Seek(int64(e.WrittenBytes), io.SeekStart); err != nil {
			t.Fatal(err)
		}
		e.Metadata = &Metadata{Artist: "a", Title: "t"}
		if err := e.Close(); err != nil {
			t.Fatal(err)
		}
		if !e.pcmPadded {
			t.Fatal("expected the odd PCM alignment byte")
		}
		checkPromoted(t, sp, uint64(sp.length)-8, uint64(e.pcmBytes), uint64(e.frames))
		if uint64(e.pcmBytes) == uint64(sp.length)-8 {
			t.Fatal("container size must exceed data size after metadata/padding")
		}
	})

	t.Run("WriteFrame validation happens before writing", func(t *testing.T) {
		sp := newSparseRW(0)
		e := NewEncoder(sp, 8000, 16, 2, 1)
		e.AutoRF64 = true
		if err := e.WriteFrame(int16(1)); err == nil || !strings.Contains(err.Error(), "blockAlign") {
			t.Fatalf("short frame: %v", err)
		}
		if e.WrittenBytes != 0 {
			t.Fatalf("%d bytes written before the rejected frame", e.WrittenBytes)
		}
		if err := e.WriteFrame(make([]byte, 2)); err == nil {
			t.Fatal("2-byte slice accepted for a 4-byte frame")
		}
		if e.WrittenBytes != 0 {
			t.Fatalf("%d bytes written before the rejected slice", e.WrittenBytes)
		}
		if err := e.WriteFrame(7); err == nil {
			t.Fatal("variable-sized int accepted in AutoRF64 mode")
		}
		if e.WrittenBytes != 0 {
			t.Fatalf("%d bytes written before the rejected int", e.WrittenBytes)
		}
		if err := e.WriteFrame([4]byte{1, 0, 2, 0}); err != nil {
			t.Fatalf("valid fixed-size frame rejected: %v", err)
		}
		if err := e.WriteFrame([]byte{3, 0, 4, 0}); err != nil {
			t.Fatalf("valid byte-slice frame rejected: %v", err)
		}
		if err := e.Close(); err != nil {
			t.Fatal(err)
		}
		if e.pcmBytes != 8 || e.frames != 2 {
			t.Fatalf("pcmBytes=%d frames=%d", e.pcmBytes, e.frames)
		}
		raw := sp.fullBytes()
		if got := binary.LittleEndian.Uint32(raw[76:80]); got != 8 {
			t.Fatalf("data size %d", got)
		}
		if buf := decodeAll(t, raw); !reflect.DeepEqual(buf.Data, []int{1, 2, 3, 4}) {
			t.Fatalf("decoded %v", buf.Data)
		}
	})

	t.Run("mixed Write and WriteFrame counts", func(t *testing.T) {
		sp := newSparseRW(0)
		e := NewEncoder(sp, 8000, 8, 1, 1)
		e.AutoRF64 = true
		if err := e.Write(intBuffer([]int{1, 2}, 1, 8000)); err != nil {
			t.Fatal(err)
		}
		if err := e.WriteFrame(uint8(3)); err != nil {
			t.Fatal(err)
		}
		if err := e.WriteFrame(uint8(4)); err != nil {
			t.Fatal(err)
		}
		if err := e.Write(intBuffer([]int{5, 6}, 1, 8000)); err != nil {
			t.Fatal(err)
		}
		if err := e.Close(); err != nil {
			t.Fatal(err)
		}
		if e.pcmBytes != 6 || e.frames != 6 {
			t.Fatalf("pcmBytes=%d frames=%d", e.pcmBytes, e.frames)
		}
		raw := sp.fullBytes()
		if got := binary.LittleEndian.Uint32(raw[76:80]); got != 6 {
			t.Fatalf("data size %d", got)
		}
		if buf := decodeAll(t, raw); !reflect.DeepEqual(buf.Data, []int{1, 2, 3, 4, 5, 6}) {
			t.Fatalf("decoded %v", buf.Data)
		}
	})

	t.Run("non AutoRF64 mode keeps old behavior", func(t *testing.T) {
		sp := newSparseRW(0)
		e := NewEncoder(sp, 8000, 16, 1, 1)
		if err := e.WriteFrame(int16(5)); err != nil {
			t.Fatalf("RIFF WriteFrame: %v", err)
		}
		if err := e.Close(); err != nil {
			t.Fatal(err)
		}
		raw := sp.fullBytes()
		if string(raw[0:4]) != "RIFF" || string(raw[12:16]) != "fmt " {
			t.Fatalf("unexpected header %q %q", raw[0:4], raw[12:16])
		}
		if buf := decodeAll(t, raw); !reflect.DeepEqual(buf.Data, []int{5}) {
			t.Fatalf("decoded %v", buf.Data)
		}
	})

	t.Run("Close reports seek, short write and write failures", func(t *testing.T) {
		newAuto := func() (*faultRW, *Encoder) {
			sp := newSparseRW(0)
			f := &faultRW{base: sp, failSeekAt: -1, shortAt: -1, errAt: -1}
			e := NewEncoder(f, 8000, 16, 1, 1)
			e.AutoRF64 = true
			if err := e.Write(intBuffer([]int{1, 2}, 1, 8000)); err != nil {
				panic(err)
			}
			return f, e
		}

		f, e := newAuto()
		f.armed = true
		f.failSeekAt = 4
		if err := e.Close(); err == nil || !strings.Contains(err.Error(), "seek") {
			t.Fatalf("seek failure not reported: %v", err)
		}

		f, e = newAuto()
		f.armed = true
		f.shortAt = 4
		if err := e.Close(); err == nil || !strings.Contains(err.Error(), "short write") {
			t.Fatalf("short write not reported: %v", err)
		}

		f, e = newAuto()
		f.armed = true
		f.errAt = 4
		f.errVal = fmt.Errorf("disk on fire")
		if err := e.Close(); err == nil || !strings.Contains(err.Error(), "disk on fire") {
			t.Fatalf("write failure not reported: %v", err)
		}
	})
}

func readSparseHeader(t *testing.T, sp *sparseRW, n int) []byte {
	t.Helper()
	hdr := make([]byte, n)
	if _, err := sp.Seek(0, io.SeekStart); err != nil {
		t.Fatal(err)
	}
	if _, err := io.ReadFull(sp, hdr); err != nil {
		t.Fatal(err)
	}
	return hdr
}

func checkPromoted(t *testing.T, sp *sparseRW, riffSize, dataSize, sampleCount uint64) {
	t.Helper()
	// read only the small header, never the sparse payload
	hdr := readSparseHeader(t, sp, 80)
	if string(hdr[0:4]) != "RF64" {
		t.Fatalf("outer ID %q", hdr[0:4])
	}
	if got := binary.LittleEndian.Uint32(hdr[4:8]); got != 0xffffffff {
		t.Fatalf("outer size %#x", got)
	}
	if string(hdr[8:12]) != "WAVE" || string(hdr[12:16]) != "ds64" {
		t.Fatalf("header %q %q", hdr[8:12], hdr[12:16])
	}
	if got := binary.LittleEndian.Uint32(hdr[16:20]); got != 28 {
		t.Fatalf("ds64 size %d", got)
	}
	if got := binary.LittleEndian.Uint64(hdr[20:28]); got != riffSize {
		t.Fatalf("ds64 riffSize %d want %d", got, riffSize)
	}
	if got := binary.LittleEndian.Uint64(hdr[28:36]); got != dataSize {
		t.Fatalf("ds64 dataSize %d want %d", got, dataSize)
	}
	if got := binary.LittleEndian.Uint64(hdr[36:44]); got != sampleCount {
		t.Fatalf("ds64 sampleCount %d want %d", got, sampleCount)
	}
	if got := binary.LittleEndian.Uint32(hdr[44:48]); got != 0 {
		t.Fatalf("ds64 tableLength %d", got)
	}
	if got := binary.LittleEndian.Uint32(hdr[76:80]); got != 0xffffffff {
		t.Fatalf("data chunk size %#x", got)
	}
}

// ---------------------------------------------------------------------------
// TestRF64Limits
// ---------------------------------------------------------------------------

func decodeRF64Err(t *testing.T, wire []byte, want string) {
	t.Helper()
	d := NewDecoder(bytes.NewReader(wire))
	err := d.FwdToPCM()
	if err == nil {
		_, err = d.FullPCMBuffer()
	}
	if err == nil {
		err = d.Err()
	}
	if err == nil {
		t.Fatalf("expected an error containing %q, decoding succeeded", want)
	}
	if !strings.Contains(err.Error(), want) {
		t.Fatalf("error %q does not mention %q", err, want)
	}
}

func TestRF64Limits(t *testing.T) {
	pcm16 := rfPCMBytes(16, 1, []int{1, 2})
	full := func(extra ...[]byte) []byte {
		chunks := append([][]byte{
			rfWireChunk("fmt ", 0, rfFmtBytes(16, 1, 8000)),
			rfWireChunk("data", 0xffffffff, pcm16),
		}, extra...)
		return buildRF64Wire(nil, uint64(len(pcm16)), 2, nil, chunks...)
	}

	t.Run("missing ds64", func(t *testing.T) {
		wire := rfCat(
			[]byte("RF64"), rfLE(uint32(0xffffffff)), []byte("WAVE"),
			rfWireChunk("fmt ", 0, rfFmtBytes(16, 1, 8000)),
			rfWireChunk("data", 0xffffffff, pcm16),
		)
		decodeRF64Err(t, wire, "ds64")
	})

	t.Run("truncated ds64", func(t *testing.T) {
		wire := rfCat(
			[]byte("RF64"), rfLE(uint32(0xffffffff)), []byte("WAVE"),
			[]byte("ds64"), rfLE(uint32(28)), make([]byte, 8),
		)
		decodeRF64Err(t, wire, "truncated")
	})

	t.Run("table entries beyond ds64 payload", func(t *testing.T) {
		ds := rfCat(rfLE(uint64(0)), rfLE(uint64(0)), rfLE(uint64(0)), rfLE(uint32(2)))
		wire := rfCat(
			[]byte("RF64"), rfLE(uint32(0xffffffff)), []byte("WAVE"),
			rfWireChunk("ds64", 0, ds),
			rfWireChunk("fmt ", 0, rfFmtBytes(16, 1, 8000)),
			rfWireChunk("data", 0xffffffff, pcm16),
		)
		decodeRF64Err(t, wire, "table entries")
	})

	t.Run("missing length substitution", func(t *testing.T) {
		wire := buildRF64Wire(nil, uint64(len(pcm16)), 2, nil,
			rfWireChunk("fmt ", 0, rfFmtBytes(16, 1, 8000)),
			rfWireChunk("ABCD", 0xffffffff, []byte{1, 2}),
			rfWireChunk("data", 0xffffffff, pcm16),
		)
		decodeRF64Err(t, wire, "ABCD")
	})

	t.Run("chunk past the logical container end", func(t *testing.T) {
		wire := full()
		// shrink the declared container so the data chunk overruns it
		binary.LittleEndian.PutUint64(wire[20:28], uint64(len(wire)-8-2))
		decodeRF64Err(t, wire, "past the container end")
	})

	t.Run("chunk past the physical file end", func(t *testing.T) {
		wire := full()
		wire = wire[:len(wire)-2] // cut into the data payload
		decodeRF64Err(t, wire, "file is only")
	})

	t.Run("unrepresentable chunk length", func(t *testing.T) {
		wire := buildRF64Wire(nil, uint64(len(pcm16)), 2,
			[]rfTableEntry{{id: "ABCD", sz: math.MaxUint64}},
			rfWireChunk("fmt ", 0, rfFmtBytes(16, 1, 8000)),
			rfWireChunk("ABCD", 0xffffffff, []byte{1, 2}),
			rfWireChunk("data", 0xffffffff, pcm16),
		)
		decodeRF64Err(t, wire, "unrepresentable")
	})

	t.Run("unrepresentable duration", func(t *testing.T) {
		sp := sparseRF64HugeData(t, 1<<40)
		if _, err := sp.Seek(0, io.SeekStart); err != nil {
			t.Fatal(err)
		}
		d := NewDecoder(sp)
		if _, err := d.Duration(); err == nil || !strings.Contains(err.Error(), "duration") {
			t.Fatalf("duration error: %v", err)
		}
	})

	t.Run("data not a whole frame", func(t *testing.T) {
		odd := []byte{1, 2, 3}
		wire := buildRF64Wire(nil, uint64(len(odd)), 0, nil,
			rfWireChunk("fmt ", 0, rfFmtBytes(16, 1, 8000)),
			rfWireChunk("data", 0xffffffff, odd),
		)
		decodeRF64Err(t, wire, "frames")
	})

	t.Run("sample count mismatch", func(t *testing.T) {
		wire := buildRF64Wire(nil, uint64(len(pcm16)), 3, nil, // 2 frames declared, 3 claimed
			rfWireChunk("fmt ", 0, rfFmtBytes(16, 1, 8000)),
			rfWireChunk("data", 0xffffffff, pcm16),
		)
		decodeRF64Err(t, wire, "sample count")
	})

	t.Run("sample count zero means not provided", func(t *testing.T) {
		wire := buildRF64Wire(nil, uint64(len(pcm16)), 0, nil,
			rfWireChunk("fmt ", 0, rfFmtBytes(16, 1, 8000)),
			rfWireChunk("data", 0xffffffff, pcm16),
		)
		if buf := decodeAll(t, wire); !reflect.DeepEqual(buf.Data, []int{1, 2}) {
			t.Fatalf("decoded %v", buf.Data)
		}
	})

	t.Run("second data chunk", func(t *testing.T) {
		wire := full(rfWireChunk("data", 0xffffffff, pcm16))
		d := NewDecoder(bytes.NewReader(wire))
		if err := d.FwdToPCM(); err != nil {
			t.Fatal(err)
		}
		if _, err := d.NextChunk(); err == nil || !strings.Contains(err.Error(), "second data") {
			t.Fatalf("second data error: %v", err)
		}
	})

	t.Run("second ds64 chunk", func(t *testing.T) {
		ds := rfDS64Bytes(0, 0, 0, nil)
		wire := buildRF64Wire(nil, uint64(len(pcm16)), 2, nil,
			rfWireChunk("fmt ", 0, rfFmtBytes(16, 1, 8000)),
			rfWireChunk("ds64", 0, ds),
			rfWireChunk("data", 0xffffffff, pcm16),
		)
		d := NewDecoder(bytes.NewReader(wire))
		err := d.FwdToPCM()
		if err == nil || !strings.Contains(err.Error(), "second ds64") {
			t.Fatalf("second ds64 error: %v", err)
		}
	})
}

// ---------------------------------------------------------------------------
// Example
// ---------------------------------------------------------------------------

func ExampleEncoder_AutoRF64() {
	sp := newSparseRW(0)
	e := NewEncoder(sp, 8000, 16, 1, 1)
	e.AutoRF64 = true
	if err := e.WriteFrame(int16(42)); err != nil {
		panic(err)
	}
	if err := e.Close(); err != nil {
		panic(err)
	}
	raw := sp.fullBytes()
	d := NewDecoder(bytes.NewReader(raw))
	buf, err := d.FullPCMBuffer()
	if err != nil {
		panic(err)
	}
	fmt.Printf("container=%s samples=%v pcm=%d\n", string(raw[0:4]), buf.Data, d.PCMLen())
	// Output: container=RIFF samples=[42] pcm=2
}
