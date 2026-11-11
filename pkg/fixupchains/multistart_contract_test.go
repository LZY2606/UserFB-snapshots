package fixupchains

// TestMultiStartContract pins the contract shared by the three chained-fixups
// read paths (ParseStarts, Parse, and GetFixupAtOffset):
//
// Alignment: a format's chain stride and its pointer width are independent.
// DYLD_CHAINED_PTR_64 stores 8-byte pointers but its "next" field counts
// 4-byte steps, so a legal slot can start at any 4-byte-aligned offset;
// DYLD_CHAINED_PTR_ARM64E_USERLAND slots advance in 8-byte steps. Per-slot
// queries therefore accept any slot reachable from one of the page's chain
// starts instead of demanding pointer-width alignment.
//
// Size vs PageCount: dyld_chained_starts_in_segment.size covers the whole
// record — the 22-byte header, page_start[page_count] and any chain_starts
// overflow slots used by DYLD_CHAINED_PTR_START_MULTI pages. page_count
// counts only the per-page entries; overflow slots live after them, are
// indexed (by MULTI page_start values) into the combined array, and must
// never be interpreted as extra pages. A record must be read exactly up to
// its declared size, never padded by bytes belonging to the next segment's
// record.

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"strings"
	"testing"

	"github.com/blacktop/go-macho/types"
)

const contractPageSize = 0x1000

// segLayout describes one dyld_chained_starts_in_segment record plus where its
// content lives in the fixture's shared memory image.
type segLayout struct {
	segOffset     uint64
	pointerFormat DCPtrKind
	pageStarts    []uint16
	chainStarts   []uint16

	sizeOverride      uint32 // when non-zero, written as the record's Size field
	pageCountOverride int    // when > 0, written as the record's PageCount field
}

func (s segLayout) recordSize() uint32 {
	if s.sizeOverride != 0 {
		return s.sizeOverride
	}
	return uint32(22 + 2*(len(s.pageStarts)+len(s.chainStarts)))
}

func (s segLayout) pageCount() uint16 {
	if s.pageCountOverride > 0 {
		return uint16(s.pageCountOverride)
	}
	return uint16(len(s.pageStarts))
}

// chainedFixture is an in-memory LC_DYLD_CHAINED_FIXUPS payload (lc) plus the
// segment content it points at (mem).
type chainedFixture struct {
	lc  []byte
	mem []byte
}

func buildChainedFixture(t *testing.T, segs []segLayout, memSize int, imports []string) chainedFixture {
	t.Helper()

	const headerSize = 28
	startsOffset := uint32(headerSize)
	imageHeaderSize := 4 + 4*len(segs)

	var records []byte
	segInfoOffsets := make([]uint32, len(segs))
	for i, s := range segs {
		segInfoOffsets[i] = uint32(imageHeaderSize + len(records))
		size := s.recordSize()
		// Corrupt-size fixtures may declare less than a header; build in a
		// buffer large enough for the header and truncate to the declared size.
		alloc := int(size)
		if alloc < 22 {
			alloc = 22
		}
		rec := make([]byte, alloc)
		binary.LittleEndian.PutUint32(rec[0:4], size)
		binary.LittleEndian.PutUint16(rec[4:6], contractPageSize)
		binary.LittleEndian.PutUint16(rec[6:8], uint16(s.pointerFormat))
		binary.LittleEndian.PutUint64(rec[8:16], s.segOffset)
		binary.LittleEndian.PutUint16(rec[20:22], s.pageCount())
		off := 22
		for _, entry := range append(append([]uint16{}, s.pageStarts...), s.chainStarts...) {
			if off+2 > int(size) {
				break // corrupt-size fixtures: only write what the record holds
			}
			binary.LittleEndian.PutUint16(rec[off:off+2], entry)
			off += 2
		}
		records = append(records, rec[:size]...)
	}

	importsOffset := startsOffset + uint32(imageHeaderSize+len(records))
	symbolsOffset := importsOffset + uint32(4*len(imports))

	var b bytes.Buffer
	put := func(v any) {
		if err := binary.Write(&b, binary.LittleEndian, v); err != nil {
			t.Fatal(err)
		}
	}

	put(uint32(0)) // fixups version
	put(startsOffset)
	put(importsOffset)
	put(symbolsOffset)
	put(uint32(len(imports)))
	put(uint32(DC_IMPORT))
	put(uint32(0)) // symbols format: uncompressed

	put(uint32(len(segs)))
	for _, off := range segInfoOffsets {
		put(off)
	}
	b.Write(records)

	nameOffset := uint32(0)
	for _, name := range imports {
		put(nameOffset<<9 | 1) // lib ordinal 1, not weak
		nameOffset += uint32(len(name) + 1)
	}
	for _, name := range imports {
		b.WriteString(name)
		b.WriteByte(0)
	}

	return chainedFixture{lc: b.Bytes(), mem: make([]byte, memSize)}
}

func (fx chainedFixture) dcf() *DyldChainedFixups {
	var r types.MachoReader = types.NewCustomSectionReader(bytes.NewReader(fx.mem), nil, 0, int64(len(fx.mem)))
	return NewChainedFixups(bytes.NewReader(fx.lc), &r, binary.LittleEndian)
}

func (fx chainedFixture) dcfWithReader(r types.MachoReader) *DyldChainedFixups {
	return NewChainedFixups(bytes.NewReader(fx.lc), &r, binary.LittleEndian)
}

func (fx chainedFixture) put64(off uint64, bits uint64) {
	binary.LittleEndian.PutUint64(fx.mem[off:], bits)
}

// ptr64RebaseBits encodes a DYLD_CHAINED_PTR_64 rebase (36-bit target).
func ptr64RebaseBits(target uint64, next uint16) uint64 {
	return (target & 0xFFFFFFFFF) | (uint64(next) << 51)
}

// ptr64BindBits encodes a DYLD_CHAINED_PTR_64 bind (24-bit ordinal, 8-bit addend).
func ptr64BindBits(ordinal uint32, addend uint8, next uint16) uint64 {
	return uint64(ordinal)&0xFFFFFF | uint64(addend)<<24 | uint64(next)<<51 | 1<<63
}

// arm64eRebaseBits encodes a DYLD_CHAINED_PTR_ARM64E* plain rebase (43-bit target).
func arm64eRebaseBits(target uint64, next uint16) uint64 {
	return (target & 0x7FFFFFFFFFF) | (uint64(next) << 51)
}

func eagerFixups(t *testing.T, dcf *DyldChainedFixups) map[uint64]Fixup {
	t.Helper()
	if _, err := dcf.Parse(); err != nil {
		t.Fatalf("Parse() error = %v", err)
	}
	out := make(map[uint64]Fixup)
	for _, seg := range dcf.Starts {
		for _, f := range seg.Fixups {
			if _, dup := out[f.Offset()]; dup {
				t.Fatalf("Parse() appended a duplicate fixup at %#x", f.Offset())
			}
			out[f.Offset()] = f
		}
	}
	return out
}

func assertSameFixup(t *testing.T, eager, lazy Fixup) {
	t.Helper()
	if fmt.Sprintf("%T", eager) != fmt.Sprintf("%T", lazy) {
		t.Errorf("fixup type mismatch: eager %T, lazy %T", eager, lazy)
	}
	if eager.Offset() != lazy.Offset() {
		t.Errorf("offset mismatch: eager %#x, lazy %#x", eager.Offset(), lazy.Offset())
	}
	if eager.Raw() != lazy.Raw() {
		t.Errorf("raw mismatch at %#x: eager %#x, lazy %#x", eager.Offset(), eager.Raw(), lazy.Raw())
	}
	if eager.IsBind() != lazy.IsBind() || eager.IsRebase() != lazy.IsRebase() {
		t.Errorf("kind mismatch at %#x: eager bind=%t rebase=%t, lazy bind=%t rebase=%t",
			eager.Offset(), eager.IsBind(), eager.IsRebase(), lazy.IsBind(), lazy.IsRebase())
	}
	if eb, ok := eager.(Bind); ok {
		lb, ok := lazy.(Bind)
		if !ok {
			t.Fatalf("eager fixup at %#x is a Bind but lazy is %T", eager.Offset(), lazy)
		}
		if eb.Name() != lb.Name() || eb.Ordinal() != lb.Ordinal() || eb.Addend() != lb.Addend() {
			t.Errorf("bind mismatch at %#x: eager (name=%q ordinal=%d addend=%d), lazy (name=%q ordinal=%d addend=%d)",
				eager.Offset(), eb.Name(), eb.Ordinal(), eb.Addend(), lb.Name(), lb.Ordinal(), lb.Addend())
		}
	}
	if er, ok := eager.(Rebase); ok {
		lr, ok := lazy.(Rebase)
		if !ok {
			t.Fatalf("eager fixup at %#x is a Rebase but lazy is %T", eager.Offset(), lazy)
		}
		if er.Target() != lr.Target() {
			t.Errorf("rebase target mismatch at %#x: eager %#x, lazy %#x", eager.Offset(), er.Target(), lr.Target())
		}
	}
}

// assertEagerMatchesLazy parses the whole fixture eagerly and then checks that
// every expected slot is found by the lazy per-offset path with identical
// contents, and that no extra fixups appear.
func assertEagerMatchesLazy(t *testing.T, fx chainedFixture, offsets ...uint64) {
	t.Helper()
	eager := eagerFixups(t, fx.dcf())
	if len(eager) != len(offsets) {
		t.Errorf("eager parse produced %d fixups, want %d", len(eager), len(offsets))
	}
	lazyDCF := fx.dcf()
	for _, off := range offsets {
		e, ok := eager[off]
		if !ok {
			t.Errorf("eager parse missing fixup at %#x", off)
			continue
		}
		l, err := lazyDCF.GetFixupAtOffset(off)
		if err != nil {
			t.Errorf("GetFixupAtOffset(%#x) error = %v", off, err)
			continue
		}
		assertSameFixup(t, e, l)
	}
}

func requireErrContains(t *testing.T, err error, subs ...string) {
	t.Helper()
	if err == nil {
		t.Fatalf("expected an error containing %q, got nil", subs)
	}
	for _, s := range subs {
		if !strings.Contains(err.Error(), s) {
			t.Fatalf("error %q does not contain %q", err.Error(), s)
		}
	}
}

// recordingReader wraps a MachoReader and logs every ReadAt offset.
type recordingReader struct {
	base  types.MachoReader
	reads []int64
}

func (r *recordingReader) Read(p []byte) (int, error)           { return r.base.Read(p) }
func (r *recordingReader) Seek(off int64, w int) (int64, error) { return r.base.Seek(off, w) }
func (r *recordingReader) SeekToAddr(addr uint64) error         { return r.base.SeekToAddr(addr) }
func (r *recordingReader) ReadAtAddr(buf []byte, addr uint64) (int, error) {
	return r.base.ReadAtAddr(buf, addr)
}
func (r *recordingReader) ReadAt(p []byte, off int64) (int, error) {
	r.reads = append(r.reads, off)
	return r.base.ReadAt(p, off)
}

func TestMultiStartContract(t *testing.T) {
	t.Run("single chain with stride-4 slot off pointer-width alignment", func(t *testing.T) {
		// DYLD_CHAINED_PTR_64: 8-byte pointers, 4-byte chain stride. A slot at
		// 0x14 is legal even though it is not 8-byte aligned.
		fx := buildChainedFixture(t, []segLayout{{
			segOffset:     0,
			pointerFormat: DYLD_CHAINED_PTR_64,
			pageStarts:    []uint16{0x14},
		}}, contractPageSize, nil)
		fx.put64(0x14, ptr64RebaseBits(0x1234, 0))

		assertEagerMatchesLazy(t, fx, 0x14)

		// In-page offsets that no chain reaches stay ErrNoFixupAtOffset.
		dcf := fx.dcf()
		for _, off := range []uint64{0x10, 0x18, 0x15} {
			if _, err := dcf.GetFixupAtOffset(off); !errors.Is(err, ErrNoFixupAtOffset) {
				t.Errorf("GetFixupAtOffset(%#x) error = %v, want ErrNoFixupAtOffset", off, err)
			}
		}
	})

	t.Run("multi start page with two independent chains", func(t *testing.T) {
		// One page, page_start carries MULTI with an index into the combined
		// page_start+chain_starts array; the second overflow entry has LAST.
		fx := buildChainedFixture(t, []segLayout{{
			segOffset:     0,
			pointerFormat: DYLD_CHAINED_PTR_64,
			pageStarts:    []uint16{uint16(DYLD_CHAINED_PTR_START_MULTI) | 1},
			chainStarts:   []uint16{0x10, uint16(DYLD_CHAINED_PTR_START_LAST) | 0x30},
		}}, contractPageSize, nil)
		fx.put64(0x10, ptr64RebaseBits(0xAAAA0, 0))
		fx.put64(0x30, ptr64RebaseBits(0xBBBB0, 0))
		// Pointer-looking bytes that no chain start reaches: not a fixup.
		fx.put64(0x20, ptr64RebaseBits(0xDEAD0, 0))

		assertEagerMatchesLazy(t, fx, 0x10, 0x30)

		dcf := fx.dcf()
		if _, err := dcf.GetFixupAtOffset(0x20); !errors.Is(err, ErrNoFixupAtOffset) {
			t.Errorf("GetFixupAtOffset(0x20) error = %v, want ErrNoFixupAtOffset for unreachable pointer-looking bytes", err)
		}
	})

	t.Run("pages share overflow storage and LAST is per page", func(t *testing.T) {
		// page 0: START_NONE; page 1: single start; pages 2 and 3: MULTI starts
		// whose overflow lists share one chain_starts pool after the page table.
		fx := buildChainedFixture(t, []segLayout{{
			segOffset:     0,
			pointerFormat: DYLD_CHAINED_PTR_64,
			pageStarts: []uint16{
				uint16(DYLD_CHAINED_PTR_START_NONE),
				0x40,
				uint16(DYLD_CHAINED_PTR_START_MULTI) | 4,
				uint16(DYLD_CHAINED_PTR_START_MULTI) | 6,
			},
			chainStarts: []uint16{
				0x50, uint16(DYLD_CHAINED_PTR_START_LAST) | 0x58, // page 2's list
				0x60, uint16(DYLD_CHAINED_PTR_START_LAST) | 0x68, // page 3's list
			},
		}}, 4*contractPageSize, nil)
		fx.put64(1*contractPageSize+0x40, ptr64RebaseBits(0x1040, 0))
		fx.put64(2*contractPageSize+0x50, ptr64RebaseBits(0x2050, 0))
		fx.put64(2*contractPageSize+0x58, ptr64RebaseBits(0x2058, 0))
		fx.put64(3*contractPageSize+0x60, ptr64RebaseBits(0x3060, 0))
		fx.put64(3*contractPageSize+0x68, ptr64RebaseBits(0x3068, 0))

		assertEagerMatchesLazy(t, fx,
			1*contractPageSize+0x40,
			2*contractPageSize+0x50, 2*contractPageSize+0x58,
			3*contractPageSize+0x60, 3*contractPageSize+0x68)

		dcf := fx.dcf()
		if err := dcf.ParseStarts(); err != nil {
			t.Fatalf("ParseStarts() error = %v", err)
		}
		if got := len(dcf.Starts[0].PageStarts); got != 4 {
			t.Errorf("PageStarts has %d entries, want 4 (overflow slots are not pages)", got)
		}
		if got := len(dcf.Starts[0].ChainStarts); got != 4 {
			t.Errorf("ChainStarts has %d entries, want 4", got)
		}
		// The overflow slots must not extend the segment's page coverage.
		if _, err := dcf.GetFixupAtOffset(4 * contractPageSize); err == nil || errors.Is(err, ErrNoFixupAtOffset) {
			t.Errorf("GetFixupAtOffset(past last page) error = %v, want a coverage error", err)
		}
	})

	t.Run("bind chain with import table", func(t *testing.T) {
		fx := buildChainedFixture(t, []segLayout{{
			segOffset:     0,
			pointerFormat: DYLD_CHAINED_PTR_64,
			pageStarts:    []uint16{0x10},
		}}, contractPageSize, []string{"_printf"})
		fx.put64(0x10, ptr64RebaseBits(0x5000, 2)) // next: 2*4 = +8
		fx.put64(0x18, ptr64BindBits(0, 7, 0))     // ordinal 0, addend 7, chain end

		assertEagerMatchesLazy(t, fx, 0x10, 0x18)

		dcf := fx.dcf()
		f, err := dcf.GetFixupAtOffset(0x18)
		if err != nil {
			t.Fatalf("GetFixupAtOffset(0x18) error = %v", err)
		}
		b, ok := f.(Bind)
		if !ok {
			t.Fatalf("GetFixupAtOffset(0x18) = %T, want a bind", f)
		}
		if b.Name() != "_printf" || b.Ordinal() != 0 || b.Addend() != 7 {
			t.Errorf("bind = (name=%q ordinal=%d addend=%d), want (_printf, 0, 7)",
				b.Name(), b.Ordinal(), b.Addend())
		}
	})

	t.Run("segments with mixed pointer formats", func(t *testing.T) {
		fx := buildChainedFixture(t, []segLayout{
			{
				segOffset:     0,
				pointerFormat: DYLD_CHAINED_PTR_64, // stride 4
				pageStarts:    []uint16{0x14},
			},
			{
				segOffset:     0x1000,
				pointerFormat: DYLD_CHAINED_PTR_ARM64E_USERLAND, // stride 8
				pageStarts:    []uint16{0x18},
			},
		}, 2*contractPageSize, nil)
		fx.put64(0x14, ptr64RebaseBits(0x1234, 0))
		fx.put64(0x1000+0x18, arm64eRebaseBits(0x9876, 0))

		assertEagerMatchesLazy(t, fx, 0x14, 0x1000+0x18)

		// The arm64e segment uses an 8-byte stride: 0x14 is not a legal slot there.
		dcf := fx.dcf()
		if _, err := dcf.GetFixupAtOffset(0x1000 + 0x14); !errors.Is(err, ErrNoFixupAtOffset) {
			t.Errorf("GetFixupAtOffset(arm64e page + 0x14) error = %v, want ErrNoFixupAtOffset", err)
		}
	})

	t.Run("repeated calls do not duplicate fixups", func(t *testing.T) {
		fx := buildChainedFixture(t, []segLayout{{
			segOffset:     0,
			pointerFormat: DYLD_CHAINED_PTR_64,
			pageStarts:    []uint16{uint16(DYLD_CHAINED_PTR_START_MULTI) | 1},
			chainStarts:   []uint16{0x10, uint16(DYLD_CHAINED_PTR_START_LAST) | 0x30},
		}}, contractPageSize, nil)
		fx.put64(0x10, ptr64RebaseBits(0xAAAA0, 0))
		fx.put64(0x30, ptr64RebaseBits(0xBBBB0, 0))

		dcf := fx.dcf()
		if _, err := dcf.Parse(); err != nil {
			t.Fatalf("Parse() error = %v", err)
		}
		first := len(dcf.Starts[0].Fixups)
		if first != 2 {
			t.Fatalf("Parse() produced %d fixups, want 2", first)
		}
		if _, err := dcf.Parse(); err != nil {
			t.Fatalf("second Parse() error = %v", err)
		}
		if got := len(dcf.Starts[0].Fixups); got != first {
			t.Errorf("after second Parse(): %d fixups, want %d (no duplicate appends)", got, first)
		}
		if _, err := dcf.GetFixupAtOffset(0x10); err != nil {
			t.Fatalf("GetFixupAtOffset(0x10) error = %v", err)
		}
		if got := len(dcf.Starts[0].Fixups); got != first {
			t.Errorf("after lazy lookup: %d fixups, want %d (lazy path must not append)", got, first)
		}

		// A purely lazy dcf never accumulates fixups either.
		lazyDCF := fx.dcf()
		for i := 0; i < 3; i++ {
			if _, err := lazyDCF.GetFixupAtOffset(0x30); err != nil {
				t.Fatalf("GetFixupAtOffset(0x30) error = %v", err)
			}
		}
		if got := len(lazyDCF.Starts[0].Fixups); got != 0 {
			t.Errorf("lazy-only dcf accumulated %d fixups, want 0", got)
		}
	})

	t.Run("lazy lookup only reads its own page", func(t *testing.T) {
		fx := buildChainedFixture(t, []segLayout{{
			segOffset:     0,
			pointerFormat: DYLD_CHAINED_PTR_64,
			pageStarts: []uint16{
				uint16(DYLD_CHAINED_PTR_START_NONE),
				uint16(DYLD_CHAINED_PTR_START_NONE),
				0x30,
			},
		}}, 3*contractPageSize, nil)
		fx.put64(2*contractPageSize+0x30, ptr64RebaseBits(0x777, 0))

		rec := &recordingReader{base: types.MachoReader(
			types.NewCustomSectionReader(bytes.NewReader(fx.mem), nil, 0, int64(len(fx.mem))))}
		dcf := fx.dcfWithReader(rec)
		if _, err := dcf.GetFixupAtOffset(2*contractPageSize + 0x30); err != nil {
			t.Fatalf("GetFixupAtOffset() error = %v", err)
		}
		if len(rec.reads) == 0 {
			t.Fatal("lazy lookup performed no reads; expected it to read its own page")
		}
		for _, off := range rec.reads {
			if off < 2*contractPageSize || off >= 3*contractPageSize {
				t.Errorf("lazy lookup read offset %#x outside page 2 [%#x, %#x)",
					off, 2*contractPageSize, 3*contractPageSize)
			}
		}
	})

	t.Run("corrupt record size", func(t *testing.T) {
		// Size smaller than the record header.
		fx := buildChainedFixture(t, []segLayout{{
			segOffset:     0,
			pointerFormat: DYLD_CHAINED_PTR_64,
			pageStarts:    []uint16{0x10},
			sizeOverride:  20,
		}}, contractPageSize, nil)
		_, err := fx.dcf().Parse()
		requireErrContains(t, err, "segment 0")
		_, err = fx.dcf().GetFixupAtOffset(0x10)
		requireErrContains(t, err, "segment 0")

		// Size that does not cover a whole number of uint16 entries.
		fx = buildChainedFixture(t, []segLayout{{
			segOffset:     0,
			pointerFormat: DYLD_CHAINED_PTR_64,
			pageStarts:    []uint16{0x10},
			sizeOverride:  25,
		}}, contractPageSize, nil)
		_, err = fx.dcf().Parse()
		requireErrContains(t, err, "segment 0")

		// Size covers fewer entries than page_count; the following segment's
		// record must not be mined for the missing page_start entries.
		fx = buildChainedFixture(t, []segLayout{
			{
				segOffset:         0,
				pointerFormat:     DYLD_CHAINED_PTR_64,
				pageStarts:        []uint16{0x10},
				sizeOverride:      24, // room for one entry only
				pageCountOverride: 2,
			},
			{
				segOffset:     0x2000,
				pointerFormat: DYLD_CHAINED_PTR_64,
				pageStarts:    []uint16{0x20},
			},
		}, 3*contractPageSize, nil)
		_, err = fx.dcf().Parse()
		requireErrContains(t, err, "segment 0")
	})

	t.Run("overflow index into page table", func(t *testing.T) {
		fx := buildChainedFixture(t, []segLayout{{
			segOffset:     0,
			pointerFormat: DYLD_CHAINED_PTR_64,
			pageStarts: []uint16{
				uint16(DYLD_CHAINED_PTR_START_MULTI) | 1, // index 1 < page_count 2
				0x40,
			},
			chainStarts: []uint16{uint16(DYLD_CHAINED_PTR_START_LAST) | 0x50},
		}}, 2*contractPageSize, nil)

		_, err := fx.dcf().Parse()
		requireErrContains(t, err, "segment 0", "page 0")
		_, err = fx.dcf().GetFixupAtOffset(0x40)
		requireErrContains(t, err, "segment 0", "page 0")
	})

	t.Run("overflow index beyond record", func(t *testing.T) {
		fx := buildChainedFixture(t, []segLayout{{
			segOffset:     0,
			pointerFormat: DYLD_CHAINED_PTR_64,
			pageStarts:    []uint16{uint16(DYLD_CHAINED_PTR_START_MULTI) | 5},
			chainStarts:   []uint16{uint16(DYLD_CHAINED_PTR_START_LAST) | 0x10},
		}}, contractPageSize, nil)

		_, err := fx.dcf().Parse()
		requireErrContains(t, err, "segment 0", "page 0")
		_, err = fx.dcf().GetFixupAtOffset(0x10)
		requireErrContains(t, err, "segment 0", "page 0")
	})

	t.Run("missing LAST terminator", func(t *testing.T) {
		fx := buildChainedFixture(t, []segLayout{{
			segOffset:     0,
			pointerFormat: DYLD_CHAINED_PTR_64,
			pageStarts:    []uint16{uint16(DYLD_CHAINED_PTR_START_MULTI) | 1},
			chainStarts:   []uint16{0x10, 0x30}, // neither has LAST
		}}, contractPageSize, nil)

		_, err := fx.dcf().Parse()
		requireErrContains(t, err, "segment 0", "page 0", "LAST")
		_, err = fx.dcf().GetFixupAtOffset(0x10)
		requireErrContains(t, err, "segment 0", "page 0", "LAST")
	})

	t.Run("chain next escapes the page", func(t *testing.T) {
		fx := buildChainedFixture(t, []segLayout{{
			segOffset:     0,
			pointerFormat: DYLD_CHAINED_PTR_64,
			pageStarts:    []uint16{0x10},
		}}, contractPageSize, nil)
		fx.put64(0x10, ptr64RebaseBits(0x1234, 0x400)) // next lands at 0x1010

		_, err := fx.dcf().Parse()
		requireErrContains(t, err, "segment 0", "page 0")

		// The first slot itself is still reachable...
		dcf := fx.dcf()
		if _, err := dcf.GetFixupAtOffset(0x10); err != nil {
			t.Errorf("GetFixupAtOffset(0x10) error = %v, want the valid first slot", err)
		}
		// ...but following the chain off the page is a locatable error.
		_, err = dcf.GetFixupAtOffset(0x40)
		requireErrContains(t, err, "segment 0", "page 0")
	})

	t.Run("half pointer at page tail", func(t *testing.T) {
		fx := buildChainedFixture(t, []segLayout{{
			segOffset:     0,
			pointerFormat: DYLD_CHAINED_PTR_64, // 8-byte pointers
			pageStarts:    []uint16{0xFFC},     // only 4 bytes left in the page
		}}, contractPageSize, nil)

		_, err := fx.dcf().Parse()
		requireErrContains(t, err, "segment 0", "page 0")
		_, err = fx.dcf().GetFixupAtOffset(0xFFC)
		requireErrContains(t, err, "segment 0", "page 0")
	})

	t.Run("truncated reader", func(t *testing.T) {
		fx := buildChainedFixture(t, []segLayout{{
			segOffset:     0,
			pointerFormat: DYLD_CHAINED_PTR_64,
			pageStarts:    []uint16{0x10},
		}}, 0x18, nil)
		fx.put64(0x10, ptr64RebaseBits(0x1234, 0))
		fx.mem = fx.mem[:0x14] // memory ends in the middle of the slot's pointer

		_, err := fx.dcf().Parse()
		requireErrContains(t, err, "segment 0", "page 0")
		_, err = fx.dcf().GetFixupAtOffset(0x10)
		requireErrContains(t, err, "segment 0", "page 0")
	})
}
