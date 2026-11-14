package macho

import (
	"bytes"
	"encoding/binary"
	"testing"

	"github.com/Binject/debug/task_support/taskcheck"
)

// ---- fixture builders (Go standard library only, no host objects) ----

type testSeg struct {
	name                        string
	addr, memsz, offset, filesz uint64
}

type testStream struct {
	offset uint64
	data   []byte
}

// buildTestMachO synthesizes a complete, inert Mach-O image with the given
// segments and the three classic dyld binding streams.
func buildTestMachO(wide bool, segs []testSeg, normal, weak, lazy testStream) []byte {
	headerSize, segCmd, segLen := 28, uint32(LoadCmdSegment), 56
	if wide {
		headerSize, segCmd, segLen = 32, uint32(LoadCmdSegment64), 72
	}
	cmdsz := len(segs)*segLen + 48
	size := headerSize + cmdsz
	need := func(v uint64) {
		if int(v) > size {
			size = int(v)
		}
	}
	for _, s := range segs {
		need(s.offset + s.filesz)
	}
	for _, st := range []testStream{normal, weak, lazy} {
		need(st.offset + uint64(len(st.data)))
	}
	size = (size + 15) &^ 15

	b := make([]byte, size)
	put32 := func(o int, v uint32) { binary.LittleEndian.PutUint32(b[o:], v) }
	put64 := func(o int, v uint64) { binary.LittleEndian.PutUint64(b[o:], v) }

	magic, cpu := uint32(Magic32), uint32(Cpu386)
	if wide {
		magic, cpu = Magic64, uint32(CpuAmd64)
	}
	put32(0, magic)
	put32(4, cpu)
	put32(8, 3) // subcpu
	put32(12, uint32(TypeExec))
	put32(16, uint32(len(segs)+1))
	put32(20, uint32(cmdsz))

	o := headerSize
	for _, s := range segs {
		put32(o, segCmd)
		put32(o+4, uint32(segLen))
		copy(b[o+8:o+24], s.name)
		if wide {
			put64(o+24, s.addr)
			put64(o+32, s.memsz)
			put64(o+40, s.offset)
			put64(o+48, s.filesz)
			put32(o+56, 7)
			put32(o+60, 7)
		} else {
			put32(o+24, uint32(s.addr))
			put32(o+28, uint32(s.memsz))
			put32(o+32, uint32(s.offset))
			put32(o+36, uint32(s.filesz))
			put32(o+40, 7)
			put32(o+44, 7)
		}
		o += segLen
	}

	put32(o, uint32(LoadCmdDylinkInfo))
	put32(o+4, 48)
	put32(o+16, uint32(normal.offset))
	put32(o+20, uint32(len(normal.data)))
	put32(o+24, uint32(weak.offset))
	put32(o+28, uint32(len(weak.data)))
	put32(o+32, uint32(lazy.offset))
	put32(o+36, uint32(len(lazy.data)))

	for _, st := range []testStream{normal, weak, lazy} {
		copy(b[st.offset:], st.data)
	}
	return b
}

func newTestFile(t *testing.T, raw []byte) *File {
	t.Helper()
	f, err := NewFile(bytes.NewReader(raw))
	if err != nil {
		t.Fatalf("NewFile: %v", err)
	}
	return f
}

// require records a check and reports whether it passed, so callers can
// short-circuit before indexing a result.
func require(c *taskcheck.C, ok bool, message string, args ...interface{}) bool {
	c.Check(ok, message, args...)
	return ok
}

func lebs(v uint64) []byte {
	var out []byte
	for {
		c := byte(v & 0x7f)
		v >>= 7
		if v != 0 {
			c |= 0x80
		}
		out = append(out, c)
		if v == 0 {
			return out
		}
	}
}

func cat(parts ...[]byte) []byte {
	var out []byte
	for _, p := range parts {
		out = append(out, p...)
	}
	return out
}

func b1(v ...byte) []byte { return v }

// ---- 1. decoder state ----

func TestBindingContract_State(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// One segment with address 0x2000, fully backed by file bytes at 0x400.
		seg := testSeg{"__DATA", 0x2000, 0x1000, 0x400, 0x1000}
		normal := cat(
			b1(0x11),                     // SET_DYLIB_ORDINAL_IMM 1
			b1(0x45), []byte("sym1\x00"), // SET_SYMBOL_TRAILING_FLAGS_IMM flags=5
			b1(0x51),                     // SET_TYPE_IMM pointer
			b1(0x60, 0x78),               // SET_ADDEND_SLEB -8
			b1(0x70, 0x10),               // SET_SEGMENT_AND_OFFSET_ULEB seg0 +0x10
			b1(0x90),                     // DO_BIND @0x10
			b1(0x42), []byte("sym2\x00"), // flags=2, symbol sym2; inherit ordinal/type/addend
			b1(0x90),            // DO_BIND @0x14
			b1(0x3F),            // SET_DYLIB_SPECIAL_IMM 15 -> -1
			b1(0x90),            // DO_BIND @0x18
			b1(0x20), lebs(300), // SET_DYLIB_ORDINAL_ULEB 300
			b1(0x90),          // DO_BIND @0x1C
			b1(0x80), lebs(8), // ADD_ADDR_ULEB 8 -> 0x24
			b1(0x90), // DO_BIND @0x24
			b1(0x00), // DONE
		)
		// The weak stream first declares a symbol with no DO_BIND (which must
		// not synthesize a binding), then performs one real bind.
		weakStream := cat(b1(0x40), []byte("decl\x00"),
			b1(0x12), b1(0x40), []byte("wk\x00"),
			b1(0x70, 0x00), b1(0x90), b1(0x00))

		raw := buildTestMachO(false, []testSeg{seg},
			testStream{0x2000, normal}, testStream{0x2200, weakStream}, testStream{})
		pristine := append([]byte(nil), raw...)
		f := newTestFile(t, raw)

		got, err := f.ReadBindings(BindNormal)
		if !require(c, err == nil, "normal ReadBindings error: %v", err) {
			return
		}
		if !require(c, len(got) == 5, "normal bindings: got %d want 5", len(got)) {
			return
		}

		type want struct {
			off       uint64
			streamOff int
			symbol    string
			flags     uint8
			ordinal   int
			addend    int64
			typ       uint8
		}
		wants := []want{
			{0x10, 12, "sym1", 5, 1, -8, BindTypePointer},
			{0x14, 19, "sym2", 2, 1, -8, BindTypePointer},
			{0x18, 21, "sym2", 2, -1, -8, BindTypePointer},
			{0x1C, 25, "sym2", 2, 300, -8, BindTypePointer},
			{0x28, 28, "sym2", 2, 300, -8, BindTypePointer},
		}
		for i, w := range wants {
			g := got[i]
			require(c, g.Kind == BindNormal, "binding %d kind = %v", i, g.Kind)
			require(c, g.StreamOffset == w.streamOff, "binding %d stream offset = %d want %d", i, g.StreamOffset, w.streamOff)
			require(c, g.SegmentOffset == w.off, "binding %d segment offset = 0x%x want 0x%x", i, g.SegmentOffset, w.off)
			require(c, g.Symbol == w.symbol, "binding %d symbol = %q want %q", i, g.Symbol, w.symbol)
			require(c, g.Flags == w.flags, "binding %d flags = %d want %d", i, g.Flags, w.flags)
			require(c, g.DylibOrdinal == w.ordinal, "binding %d ordinal = %d want %d", i, g.DylibOrdinal, w.ordinal)
			require(c, g.Addend == w.addend, "binding %d addend = %d want %d", i, g.Addend, w.addend)
			require(c, g.Type == w.typ, "binding %d type = %d want %d", i, g.Type, w.typ)
			require(c, g.SegmentIndex == 0, "binding %d segment index = %d", i, g.SegmentIndex)
			require(c, g.SegmentName == "__DATA", "binding %d segment name = %q", i, g.SegmentName)
			require(c, g.VirtualAddress == 0x2000+w.off, "binding %d va = 0x%x want 0x%x", i, g.VirtualAddress, 0x2000+w.off)
			require(c, g.HasFileBytes, "binding %d should have file bytes", i)
			require(c, g.FileOffset == 0x400+w.off, "binding %d file offset = 0x%x want 0x%x", i, g.FileOffset, 0x400+w.off)
		}

		// Weak: a bare symbol declaration is not a binding.
		weak, err := f.ReadBindings(BindWeak)
		if require(c, err == nil, "weak ReadBindings: %v", err) {
			require(c, len(weak) == 1, "weak bindings: got %d want 1", len(weak))
			if len(weak) == 1 {
				require(c, weak[0].Kind == BindWeak, "weak binding kind = %v", weak[0].Kind)
				require(c, weak[0].Symbol == "wk", "weak symbol = %q", weak[0].Symbol)
				require(c, weak[0].DylibOrdinal == 2, "weak ordinal = %d", weak[0].DylibOrdinal)
			}
		}

		// Invalid kind and missing dyld info.
		_, err = f.ReadBindings(BindKind(7))
		require(c, err != nil, "invalid kind should error")

		empty := &File{}
		got, err = empty.ReadBindings(BindNormal)
		require(c, err == nil && len(got) == 0, "no DylinkInfo should yield empty (got %d, err %v)", len(got), err)

		// Empty stream returns empty.
		emptyStream := &File{DylinkInfo: &DylinkInfo{}}
		got, err = emptyStream.ReadBindings(BindLazy)
		require(c, err == nil && len(got) == 0, "empty stream should yield empty (got %d, err %v)", len(got), err)

		// DO_BIND before any segment selection is an error, not a panic.
		badFile := newTestFile(t, buildTestMachO(false, []testSeg{seg},
			testStream{0x2000, b1(0x90)}, testStream{}, testStream{}))
		_, err = badFile.ReadBindings(BindNormal)
		require(c, err != nil, "DO_BIND without segment should error")
		if be, ok := err.(*BindingError); require(c, ok, "expected *BindingError, got %T", err) {
			require(c, be.Kind == BindNormal, "error kind = %v", be.Kind)
			require(c, be.Offset == 0, "error offset = %d", be.Offset)
		}

		// Corrupted stream: unknown opcode. Then a clean call on the same File
		// must succeed and the source bytes must be untouched.
		origDat := f.DylinkInfo.BindingInfoDat
		origLen := f.DylinkInfo.BindingInfoLen
		f.DylinkInfo.BindingInfoDat = []byte{0xD0}
		f.DylinkInfo.BindingInfoLen = 1
		_, err = f.ReadBindings(BindNormal)
		if require(c, err != nil, "threaded/unknown opcode should error") {
			be, ok := err.(*BindingError)
			require(c, ok, "expected *BindingError, got %T", err)
			if ok {
				require(c, be.Kind == BindNormal, "error kind = %v", be.Kind)
				require(c, be.Offset == 0, "error offset = %d", be.Offset)
				require(c, bytes.Contains([]byte(be.Error()), []byte("normal")), "error text lacks kind: %v", be)
			}
		}
		// Declared-length mismatch.
		f.DylinkInfo.BindingInfoDat = origDat
		f.DylinkInfo.BindingInfoLen = origLen + 1
		_, err = f.ReadBindings(BindNormal)
		require(c, err != nil, "length mismatch should error")

		// Restore; the File recovers and the input is unchanged.
		f.DylinkInfo.BindingInfoLen = origLen
		again, err := f.ReadBindings(BindNormal)
		require(c, err == nil, "read after corruption: %v", err)
		require(c, len(again) == 5, "read after corruption: got %d", len(again))
		require(c, bytes.Equal(raw, pristine), "input bytes changed")

		// Non-zero bytes after DONE in a normal stream are rejected.
		trailing := newTestFile(t, buildTestMachO(false, []testSeg{seg},
			testStream{0x2000, b1(0x00, 0x11)}, testStream{}, testStream{}))
		_, err = trailing.ReadBindings(BindNormal)
		require(c, err != nil, "non-zero after DONE should error")
	})
}

// ---- 2. stride and field width ----

func TestBindingContract_Stride(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		seg := testSeg{"__DATA", 0x1000, 0x800, 0x400, 0x400}
		stream := cat(
			b1(0x52),                  // SET_TYPE_IMM 2 (text_abs32, 4-byte field)
			b1(0x11),                  // ordinal 1
			b1(0x40), []byte("s\x00"), // symbol s
			b1(0x70, 0x00),       // seg0 +0
			b1(0x90),             // DO_BIND: stride must be pointer width
			b1(0xB3),             // DO_BIND_ADD_ADDR_IMM_SCALED 3
			b1(0xA0), lebs(0x10), // DO_BIND_ADD_ADDR_ULEB 0x10
			b1(0xC0), lebs(3), lebs(8), // DO_BIND_ULEB_TIMES_SKIPPING count 3 skip 8
			b1(0x70), lebs(0x3FC), // seg0 +0x3FC (4 bytes left before Filesz)
			b1(0x90), // DO_BIND
			b1(0x00),
		)

		// The four advancing opcodes each carry the pointer-width stride,
		// regardless of the 4-byte text_abs32 field width.
		cases := []struct {
			name string
			wide bool
			offs []uint64
		}{
			{"64bit", true, []uint64{0x00, 0x08, 0x28, 0x40, 0x50, 0x60, 0x3FC}},
			{"32bit", false, []uint64{0x00, 0x04, 0x14, 0x28, 0x34, 0x40, 0x3FC}},
		}
		for _, tc := range cases {
			raw := buildTestMachO(tc.wide, []testSeg{seg},
				testStream{0x2000, stream}, testStream{}, testStream{})
			f := newTestFile(t, raw)
			got, err := f.ReadBindings(BindNormal)
			if !require(c, err == nil, "%s: %v", tc.name, err) {
				continue
			}
			if !require(c, len(got) == len(tc.offs), "%s count = %d want %d", tc.name, len(got), len(tc.offs)) {
				continue
			}
			for i, off := range tc.offs {
				require(c, got[i].SegmentOffset == off, "%s binding %d offset = 0x%x want 0x%x", tc.name, i, got[i].SegmentOffset, off)
				require(c, got[i].Type == 2, "%s binding %d type = %d", tc.name, i, got[i].Type)
				require(c, got[i].Kind == BindNormal, "%s binding %d kind = %v", tc.name, i, got[i].Kind)
			}
			if tc.wide {
				// A 4-byte field at Filesz-4 fits entirely in the file.
				last := got[len(got)-1]
				require(c, last.HasFileBytes, "%s: 4-byte field at Filesz-4 should have file bytes", tc.name)
				require(c, last.FileOffset == 0x400+0x3FC, "%s: file offset = 0x%x", tc.name, last.FileOffset)
			}
		}

		// A pointer-sized field at Filesz-4 crosses the file end but stays
		// inside Memsz: valid virtual reference, no file bytes.
		ptrStream := cat(
			b1(0x51), // type pointer
			b1(0x11),
			b1(0x40), []byte("p\x00"),
			b1(0x70), lebs(0x3FC),
			b1(0x90),
			b1(0x00),
		)
		raw := buildTestMachO(true, []testSeg{seg},
			testStream{0x2000, ptrStream}, testStream{}, testStream{})
		got, err := newTestFile(t, raw).ReadBindings(BindNormal)
		if require(c, err == nil, "pointer field: %v", err) && require(c, len(got) == 1, "pointer field count = %d", len(got)) {
			require(c, !got[0].HasFileBytes, "8-byte field crossing Filesz must not have file bytes")
			require(c, got[0].FileOffset == 0, "FileOffset must be zero without file bytes, got 0x%x", got[0].FileOffset)
			require(c, got[0].VirtualAddress == 0x1000+0x3FC, "va = 0x%x", got[0].VirtualAddress)
			require(c, got[0].SegmentOffset == 0x3FC, "segment offset = 0x%x", got[0].SegmentOffset)
		}

		// The same offset with a 4-byte field in the 64-bit file does have
		// file bytes, which shows the field width drives has-file-bytes.
		fourByte := cat(b1(0x52), b1(0x11), b1(0x40), []byte("p\x00"),
			b1(0x70), lebs(0x3FC), b1(0x90), b1(0x00))
		raw = buildTestMachO(true, []testSeg{seg},
			testStream{0x2000, fourByte}, testStream{}, testStream{})
		got, err = newTestFile(t, raw).ReadBindings(BindNormal)
		if require(c, err == nil, "4-byte field at 0x3FC: %v", err) && require(c, len(got) == 1, "4-byte field count = %d", len(got)) {
			require(c, got[0].HasFileBytes, "4-byte field at Filesz-4 should have file bytes")
			require(c, got[0].FileOffset == 0x400+0x3FC, "file offset = 0x%x", got[0].FileOffset)
		}
	})
}

// ---- 3. lazy records ----

func TestBindingContract_Lazy(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		segs := []testSeg{
			{"__TEXT", 0x1000, 0x1000, 0x400, 0x1000},
			{"__DATA", 0x3000, 0x1000, 0x1400, 0x1000},
		}
		lazy := cat(
			// record 1: full state
			b1(0x12),                    // ordinal 2
			b1(0x4A), []byte("lz1\x00"), // flags 0xA
			b1(0x53),       // type 3
			b1(0x60, 0x70), // addend -16 (SLEB)
			b1(0x70, 0x20), // seg0 +0x20
			b1(0x90),       // DO_BIND
			b1(0x00),       // DONE: end of record
			// record 2: must start from scratch, so ordinal/type/addend reset
			b1(0x40), []byte("lz2\x00"), // symbol only, flags 0
			b1(0x71, 0x10), // seg1 +0x10
			b1(0x90),       // DO_BIND
			b1(0x00),       // DONE
			b1(0x00, 0x00), // empty records
		)
		raw := buildTestMachO(true, segs, testStream{}, testStream{}, testStream{0x2000, lazy})
		f := newTestFile(t, raw)

		got, err := f.ReadBindings(BindLazy)
		if !require(c, err == nil, "lazy: %v", err) {
			return
		}
		if !require(c, len(got) == 2, "lazy count = %d want 2", len(got)) {
			return
		}
		// Record 1.
		require(c, got[0].Kind == BindLazy, "record1 kind = %v", got[0].Kind)
		require(c, got[0].Symbol == "lz1", "record1 symbol = %q", got[0].Symbol)
		require(c, got[0].Flags == 0xA, "record1 flags = %d", got[0].Flags)
		require(c, got[0].DylibOrdinal == 2, "record1 ordinal = %d", got[0].DylibOrdinal)
		require(c, got[0].Addend == -16, "record1 addend = %d", got[0].Addend)
		require(c, got[0].Type == 3, "record1 type = %d", got[0].Type)
		require(c, got[0].SegmentIndex == 0, "record1 segment = %d", got[0].SegmentIndex)
		require(c, got[0].SegmentOffset == 0x20, "record1 offset = 0x%x", got[0].SegmentOffset)
		require(c, got[0].VirtualAddress == 0x1020, "record1 va = 0x%x", got[0].VirtualAddress)
		require(c, got[0].StreamOffset == 11, "record1 stream offset = %d", got[0].StreamOffset)

		// Record 2 must not inherit anything from record 1.
		require(c, got[1].Symbol == "lz2", "record2 symbol = %q", got[1].Symbol)
		require(c, got[1].Flags == 0, "record2 flags = %d", got[1].Flags)
		require(c, got[1].DylibOrdinal == 0, "record2 ordinal = %d (must reset)", got[1].DylibOrdinal)
		require(c, got[1].Addend == 0, "record2 addend = %d (must reset)", got[1].Addend)
		require(c, got[1].Type == BindTypePointer, "record2 type = %d (must reset)", got[1].Type)
		require(c, got[1].SegmentIndex == 1, "record2 segment = %d", got[1].SegmentIndex)
		require(c, got[1].SegmentOffset == 0x10, "record2 offset = 0x%x", got[1].SegmentOffset)
		require(c, got[1].VirtualAddress == 0x3010, "record2 va = 0x%x", got[1].VirtualAddress)
		require(c, got[1].StreamOffset == 20, "record2 stream offset = %d", got[1].StreamOffset)
	})
}

// ---- 4. segment/address mapping ----

func TestBindingContract_Mapping(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		segs := []testSeg{
			{"__PAGEZERO", 0x0000, 0x1000, 0x0000, 0x0000}, // index 0
			{"__DATA", 0x1000, 0x3000, 0x0400, 0x1000},     // index 1
			{"__DATA", 0x5000, 0x2000, 0x0000, 0x0000},     // index 2, duplicate name, zero-filled
			{"__TEXT", 0x8000, 0x1000, 0x1400, 0x0800},     // index 3
		}
		stream := cat(
			b1(0x51), b1(0x11),
			b1(0x40), []byte("m\x00"),
			b1(0x70), lebs(0x100), // __PAGEZERO +0x100: virtual only
			b1(0x90),
			b1(0x71), lebs(0x10), // __DATA(index1) +0x10: file backed
			b1(0x90),
			b1(0x71), lebs(0xFF8), // last full 8-byte field in Filesz
			b1(0x90),
			b1(0x71), lebs(0xFFC), // crosses Filesz but inside Memsz
			b1(0x90),
			b1(0x72), lebs(0x00), // duplicate-name __DATA(index2): zero-filled
			b1(0x90),
			b1(0x73), lebs(0x7FC), // __TEXT, crosses Filesz
			b1(0x90),
			b1(0x71), lebs(0x10), // repeat the same location: not merged
			b1(0x90),
			b1(0x00),
		)
		raw := buildTestMachO(true, segs, testStream{0x2000, stream}, testStream{}, testStream{})
		pristine := append([]byte(nil), raw...)
		f := newTestFile(t, raw)

		got, err := f.ReadBindings(BindNormal)
		if !require(c, err == nil, "mapping: %v", err) {
			return
		}
		if !require(c, len(got) == 7, "mapping count = %d want 7", len(got)) {
			return
		}

		type want struct {
			segIdx  int
			segName string
			segOff  uint64
			va      uint64
			hasFile bool
			fileOff uint64
		}
		wants := []want{
			{0, "__PAGEZERO", 0x100, 0x100, false, 0},
			{1, "__DATA", 0x10, 0x1010, true, 0x410},
			{1, "__DATA", 0xFF8, 0x1FF8, true, 0x13F8},
			{1, "__DATA", 0xFFC, 0x1FFC, false, 0},
			{2, "__DATA", 0x00, 0x5000, false, 0},
			{3, "__TEXT", 0x7FC, 0x87FC, false, 0},
			{1, "__DATA", 0x10, 0x1010, true, 0x410}, // same location as #1, separate entry
		}
		for i, w := range wants {
			g := got[i]
			require(c, g.SegmentIndex == w.segIdx, "binding %d segment index = %d want %d", i, g.SegmentIndex, w.segIdx)
			require(c, g.SegmentName == w.segName, "binding %d segment name = %q want %q", i, g.SegmentName, w.segName)
			require(c, g.SegmentOffset == w.segOff, "binding %d segment offset = 0x%x want 0x%x", i, g.SegmentOffset, w.segOff)
			require(c, g.VirtualAddress == w.va, "binding %d va = 0x%x want 0x%x", i, g.VirtualAddress, w.va)
			require(c, g.HasFileBytes == w.hasFile, "binding %d hasFile = %v want %v", i, g.HasFileBytes, w.hasFile)
			require(c, g.FileOffset == w.fileOff, "binding %d file offset = 0x%x want 0x%x", i, g.FileOffset, w.fileOff)
		}

		// A field past Memsz is rejected with kind and stream offset.
		pastMem := cat(b1(0x51), b1(0x11), b1(0x70), lebs(0x3000), b1(0x90), b1(0x00))
		bad := newTestFile(t, buildTestMachO(true, segs, testStream{0x2000, pastMem}, testStream{}, testStream{}))
		_, err = bad.ReadBindings(BindNormal)
		if require(c, err != nil, "past Memsz should error") {
			be, ok := err.(*BindingError)
			require(c, ok, "expected *BindingError, got %T", err)
			if ok {
				require(c, be.Offset == 5, "error offset = %d want 5", be.Offset)
			}
		}

		// Segment index out of range.
		badSeg := cat(b1(0x77), lebs(0x00), b1(0x90), b1(0x00))
		bad2 := newTestFile(t, buildTestMachO(true, segs, testStream{0x2000, badSeg}, testStream{}, testStream{}))
		_, err = bad2.ReadBindings(BindNormal)
		require(c, err != nil, "out-of-range segment should error")

		// Truncated ULEB and overflowing ULEB are reported, not panicked.
		trunc := cat(b1(0x11), b1(0x70), []byte{0x80, 0x80})
		bad3 := newTestFile(t, buildTestMachO(true, segs, testStream{0x2000, trunc}, testStream{}, testStream{}))
		_, err = bad3.ReadBindings(BindNormal)
		require(c, err != nil, "truncated ULEB should error")

		over := cat(b1(0x11), b1(0x70), bytes.Repeat([]byte{0xFF}, 10), b1(0x90), b1(0x00))
		bad4 := newTestFile(t, buildTestMachO(true, segs, testStream{0x2000, over}, testStream{}, testStream{}))
		_, err = bad4.ReadBindings(BindNormal)
		require(c, err != nil, "overflowing ULEB should error")

		// Expansion limit is enforced.
		huge := cat(b1(0x11), b1(0x40), []byte("h\x00"), b1(0x70), lebs(0),
			b1(0xC0), lebs(MaxBindings+1), lebs(0), b1(0x00))
		bad5 := newTestFile(t, buildTestMachO(true, segs, testStream{0x2000, huge}, testStream{}, testStream{}))
		_, err = bad5.ReadBindings(BindNormal)
		require(c, err != nil, "expansion limit should error")

		// The bad calls did not touch the input.
		require(c, bytes.Equal(raw, pristine), "input bytes changed after bad streams")
	})
}

// TestBindingDocExample keeps the offline byte example in docs/bindings.md
// honest: the stream shown there is decoded through the real File path.
func TestBindingDocExample(t *testing.T) {
	seg := testSeg{"__DATA", 0x1000, 0x1000, 0x400, 0x400}
	stream := []byte{0x11, 0x45, 's', 'y', 'm', 0x00, 0x51, 0x60, 0x78, 0x70, 0x08, 0x90, 0x90, 0x00}
	raw := buildTestMachO(false, []testSeg{seg}, testStream{0x2000, stream}, testStream{}, testStream{})
	f, err := NewFile(bytes.NewReader(raw))
	if err != nil {
		t.Fatalf("NewFile: %v", err)
	}
	got, err := f.ReadBindings(BindNormal)
	if err != nil {
		t.Fatalf("ReadBindings: %v", err)
	}
	if len(got) != 2 {
		t.Fatalf("got %d bindings, want 2", len(got))
	}
	for i, b := range got {
		wantOff := uint64(0x08 + 4*i)
		wantStream := 11 + i
		if b.StreamOffset != wantStream || b.SegmentOffset != wantOff ||
			b.VirtualAddress != 0x1000+wantOff || b.FileOffset != 0x400+wantOff ||
			!b.HasFileBytes {
			t.Errorf("binding %d = %+v, want stream offset %d, segment offset 0x%x", i, b, wantStream, wantOff)
		}
		if b.Symbol != "sym" || b.Flags != 5 || b.DylibOrdinal != 1 || b.Addend != -8 || b.Type != BindTypePointer {
			t.Errorf("binding %d state = %+v", i, b)
		}
	}
}
