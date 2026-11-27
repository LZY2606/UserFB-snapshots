package macho

import (
	"bytes"
	"encoding/binary"
	"strings"
	"testing"

	"github.com/Binject/debug/task_support/taskcheck"
)

// The fixtures below are complete but inert Mach-O byte images assembled in
// Go. They are only parsed with NewFile and never loaded, mapped, or executed.

type testSeg struct {
	name                         string
	addr, memsz, fileoff, filesz uint64
}

type testStreams struct {
	normal, weak, lazy []byte
}

func alignTest(n, a int) int { return (n + a - 1) &^ (a - 1) }

// buildFixture assembles a little-endian Mach-O with the given segments and an
// optional DylinkInfo load command whose binding streams are placed after the
// load commands.
func buildFixture(wide bool, segs []testSeg, streams testStreams, withDylink bool) []byte {
	header := 28
	segCmd := 56
	if wide {
		header = 32
		segCmd = 72
	}
	cmdSize := len(segs) * segCmd
	ncmds := len(segs)
	if withDylink {
		cmdSize += 48
		ncmds++
	}

	streamStart := alignTest(header+cmdSize, 16)
	off := streamStart
	normalOff := off
	off += len(streams.normal)
	weakOff := off
	off += len(streams.weak)
	lazyOff := off
	off += len(streams.lazy)

	total := alignTest(off, 16)
	for _, s := range segs {
		if end := int(s.fileoff + s.filesz); end > total {
			total = end
		}
	}

	b := make([]byte, total)
	le := binary.LittleEndian
	magic, cpu := uint32(Magic32), uint32(Cpu386)
	if wide {
		magic, cpu = Magic64, uint32(CpuAmd64)
	}
	le.PutUint32(b[0:], magic)
	le.PutUint32(b[4:], cpu)
	le.PutUint32(b[8:], 3) // cpusubtype
	le.PutUint32(b[12:], uint32(TypeExec))
	le.PutUint32(b[16:], uint32(ncmds))
	le.PutUint32(b[20:], uint32(cmdSize))
	le.PutUint32(b[24:], 0) // flags

	o := header
	for _, s := range segs {
		if wide {
			le.PutUint32(b[o:], uint32(LoadCmdSegment64))
			le.PutUint32(b[o+4:], uint32(segCmd))
			copy(b[o+8:o+24], s.name)
			le.PutUint64(b[o+24:], s.addr)
			le.PutUint64(b[o+32:], s.memsz)
			le.PutUint64(b[o+40:], s.fileoff)
			le.PutUint64(b[o+48:], s.filesz)
		} else {
			le.PutUint32(b[o:], uint32(LoadCmdSegment))
			le.PutUint32(b[o+4:], uint32(segCmd))
			copy(b[o+8:o+24], s.name)
			le.PutUint32(b[o+24:], uint32(s.addr))
			le.PutUint32(b[o+28:], uint32(s.memsz))
			le.PutUint32(b[o+32:], uint32(s.fileoff))
			le.PutUint32(b[o+36:], uint32(s.filesz))
		}
		o += segCmd
	}

	if withDylink {
		le.PutUint32(b[o:], uint32(LoadCmdDylinkInfo))
		le.PutUint32(b[o+4:], 48)
		le.PutUint32(b[o+16:], uint32(normalOff))
		le.PutUint32(b[o+20:], uint32(len(streams.normal)))
		le.PutUint32(b[o+24:], uint32(weakOff))
		le.PutUint32(b[o+28:], uint32(len(streams.weak)))
		le.PutUint32(b[o+32:], uint32(lazyOff))
		le.PutUint32(b[o+36:], uint32(len(streams.lazy)))
		copy(b[normalOff:], streams.normal)
		copy(b[weakOff:], streams.weak)
		copy(b[lazyOff:], streams.lazy)
	}

	return b
}

func openFixture(c *taskcheck.C, wide bool, segs []testSeg, streams testStreams, withDylink bool) (*File, []byte) {
	raw := buildFixture(wide, segs, streams, withDylink)
	f, err := NewFile(bytes.NewReader(raw))
	c.Check(err == nil, "NewFile: %v", err)
	if err != nil {
		return nil, raw
	}
	return f, raw
}

type wantBind struct {
	streamOffset uint64
	symbol       string
	flags        uint8
	ordinal      int16
	addend       int64
	typ          uint8
	segIndex     int
	segOff       uint64
	addr         uint64
	hasFile      bool
	fileOff      uint64
}

func checkBindings(c *taskcheck.C, label string, got []Binding, want []wantBind) {
	c.Check(len(got) == len(want), "%s: got %d bindings, want %d", label, len(got), len(want))
	n := len(got)
	if len(want) < n {
		n = len(want)
	}
	for i := 0; i < n; i++ {
		g, w := got[i], want[i]
		c.Check(g.StreamOffset == w.streamOffset, "%s[%d]: StreamOffset=%d want %d", label, i, g.StreamOffset, w.streamOffset)
		c.Check(g.Symbol == w.symbol, "%s[%d]: Symbol=%q want %q", label, i, g.Symbol, w.symbol)
		c.Check(g.Flags == w.flags, "%s[%d]: Flags=%d want %d", label, i, g.Flags, w.flags)
		c.Check(g.Ordinal == w.ordinal, "%s[%d]: Ordinal=%d want %d", label, i, g.Ordinal, w.ordinal)
		c.Check(g.Addend == w.addend, "%s[%d]: Addend=%d want %d", label, i, g.Addend, w.addend)
		c.Check(g.Type == w.typ, "%s[%d]: Type=%d want %d", label, i, g.Type, w.typ)
		c.Check(g.SegmentIndex == w.segIndex, "%s[%d]: SegmentIndex=%d want %d", label, i, g.SegmentIndex, w.segIndex)
		c.Check(g.SegmentOffset == w.segOff, "%s[%d]: SegmentOffset=%d want %d", label, i, g.SegmentOffset, w.segOff)
		c.Check(g.Address == w.addr, "%s[%d]: Address=%#x want %#x", label, i, g.Address, w.addr)
		c.Check(g.HasFileBytes == w.hasFile, "%s[%d]: HasFileBytes=%v want %v", label, i, g.HasFileBytes, w.hasFile)
		c.Check(g.FileOffset == w.fileOff, "%s[%d]: FileOffset=%#x want %#x", label, i, g.FileOffset, w.fileOff)
	}
}

// TestBindingContract_State checks state carried across opcodes, the three
// ordinal forms, negative addends, the four DO_BIND-family progressions, and
// that a weak definition declaration alone produces no binding.
func TestBindingContract_State(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		segs := []testSeg{{name: "__DATA", addr: 0x1000, memsz: 0x1000, fileoff: 0x200, filesz: 0x100}}

		// Stream A: one state, four advance forms. Every bind reuses the
		// symbol/flags/ordinal/addend/type set at the top of the stream.
		// DO_BIND-family opcodes emit before advancing, so repeated-bind
		// records share a stream offset.
		normalA := []byte{
			0x11,                 // SET_DYLIB_ORDINAL_IMM 1
			0x42, 'a', 'b', 0x00, // SET_SYMBOL_TRAILING_FLAGS_IMM flags=2, "ab"
			0x51,       // SET_TYPE_IMM 1 (pointer)
			0x70, 0x10, // SET_SEGMENT_AND_OFFSET_ULEB seg0 offset 0x10
			0x60, 0x7f, // SET_ADDEND_SLEB -1
			0x90,       // DO_BIND @0x10
			0x90,       // DO_BIND @0x14
			0x80, 0x04, // ADD_ADDR_ULEB 4
			0x90,       // DO_BIND @0x1c
			0xa0, 0x02, // DO_BIND_ADD_ADDR_ULEB 2 -> bind @0x1c, +2+4
			0x90,             // DO_BIND @0x22
			0xb2,             // DO_BIND_ADD_ADDR_IMM_SCALED 2 -> bind @0x22, +2*4+4
			0x90,             // DO_BIND @0x2e
			0xc0, 0x02, 0x08, // DO_BIND_ULEB_TIMES_SKIPPING_ULEB 2,8
			0x00, // DONE
		}
		f, raw := openFixture(c, false, segs, testStreams{normal: normalA}, true)
		if f != nil {
			snapshot := append([]byte(nil), raw...)
			got, err := f.ReadBindings(BindNormal)
			c.Check(err == nil, "state A: ReadBindings: %v", err)
			want := []wantBind{
				{10, "ab", 2, 1, -1, 1, 0, 0x10, 0x1010, true, 0x210},
				{11, "ab", 2, 1, -1, 1, 0, 0x14, 0x1014, true, 0x214},
				{14, "ab", 2, 1, -1, 1, 0, 0x1c, 0x101c, true, 0x21c},
				{15, "ab", 2, 1, -1, 1, 0, 0x20, 0x1020, true, 0x220},
				{17, "ab", 2, 1, -1, 1, 0, 0x26, 0x1026, true, 0x226},
				{18, "ab", 2, 1, -1, 1, 0, 0x2a, 0x102a, true, 0x22a},
				{19, "ab", 2, 1, -1, 1, 0, 0x36, 0x1036, true, 0x236},
				{20, "ab", 2, 1, -1, 1, 0, 0x3a, 0x103a, true, 0x23a},
				{20, "ab", 2, 1, -1, 1, 0, 0x46, 0x1046, true, 0x246},
			}
			checkBindings(c, "state A", got, want)
			c.Check(bytes.Equal(raw, snapshot), "state A: input bytes changed by ReadBindings")
		}

		// Stream B: special (sign-extended) ordinal and a negative addend.
		normalB := []byte{
			0x3e,            // SET_DYLIB_SPECIAL_IMM 0xe -> -2
			0x40, 's', 0x00, // SET_SYMBOL_TRAILING_FLAGS_IMM flags=0, "s"
			0x52,       // SET_TYPE_IMM 2 (text_abs32)
			0x70, 0x00, // seg0 offset 0
			0x60, 0x7e, // SET_ADDEND_SLEB -2
			0x90, // DO_BIND
			0x00,
		}
		f, _ = openFixture(c, false, segs, testStreams{normal: normalB}, true)
		if f != nil {
			got, err := f.ReadBindings(BindNormal)
			c.Check(err == nil, "state B: ReadBindings: %v", err)
			checkBindings(c, "state B", got, []wantBind{
				{9, "s", 0, -2, -2, 2, 0, 0, 0x1000, true, 0x200},
			})
		}

		// Stream C: ULEB ordinal.
		normalC := []byte{
			0x20, 0x0f, // SET_DYLIB_ORDINAL_ULEB 15
			0x40, 'u', 0x00, // symbol "u"
			0x70, 0x04, // seg0 offset 4
			0x90, // DO_BIND
			0x00,
		}
		f, _ = openFixture(c, false, segs, testStreams{normal: normalC}, true)
		if f != nil {
			got, err := f.ReadBindings(BindNormal)
			c.Check(err == nil, "state C: ReadBindings: %v", err)
			checkBindings(c, "state C", got, []wantBind{
				{7, "u", 0, 15, 0, 1, 0, 4, 0x1004, true, 0x204},
			})
		}

		// A weak stream that only declares a symbol and has no DO_BIND must
		// not fabricate a binding.
		weakOnly := []byte{0x40, 'w', 0x00, 0x00}
		f, _ = openFixture(c, false, segs, testStreams{weak: weakOnly}, true)
		if f != nil {
			got, err := f.ReadBindings(BindWeak)
			c.Check(err == nil, "weak declaration: ReadBindings: %v", err)
			c.Check(len(got) == 0, "weak declaration: got %d bindings, want 0", len(got))
		}
	})
}

// TestBindingContract_Stride checks that the post-bind stride is always the
// pointer width, while a text_abs32/text_pcrel32 field is four bytes for
// file-byte mapping and the scaled immediate uses the pointer width.
func TestBindingContract_Stride(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// 64-bit: pointer stride 8; text_abs32 field size 4.
		// Filesz=38 makes a 4-byte field at segment offset 32 have file bytes
		// while an 8-byte field there would not.
		segs64 := []testSeg{{name: "__DATA", addr: 0x10000, memsz: 0x1000, fileoff: 0x200, filesz: 38}}
		normal64 := []byte{
			0x52,            // SET_TYPE_IMM 2 (text_abs32)
			0x40, 'q', 0x00, // symbol "q"
			0x70, 0x00, // seg0 offset 0
			0x90, // DO_BIND @0, stride 8 -> 8
			0x90, // DO_BIND @8, stride 8 -> 16
			0xb1, // scaled imm 1: bind @16, advance 1*8+8=16 -> 32
			0x90, // DO_BIND @32, stride 8 -> 40
			0x51, // SET_TYPE_IMM 1 (pointer)
			0x90, // DO_BIND @40, stride 8 -> 48
			0x00,
		}
		f, _ := openFixture(c, true, segs64, testStreams{normal: normal64}, true)
		if f != nil {
			got, err := f.ReadBindings(BindNormal)
			c.Check(err == nil, "stride64: ReadBindings: %v", err)
			checkBindings(c, "stride64", got, []wantBind{
				{6, "q", 0, 0, 0, 2, 0, 0, 0x10000, true, 0x200},
				{7, "q", 0, 0, 0, 2, 0, 8, 0x10008, true, 0x208},
				{8, "q", 0, 0, 0, 2, 0, 16, 0x10010, true, 0x210},
				{9, "q", 0, 0, 0, 2, 0, 32, 0x10020, true, 0x220},
				{11, "q", 0, 0, 0, 1, 0, 40, 0x10028, false, 0},
			})
		}

		// 32-bit: pointer stride 4, and a text_abs32 field is also 4 bytes.
		segs32 := []testSeg{{name: "__DATA", addr: 0x2000, memsz: 0x1000, fileoff: 0x300, filesz: 0x100}}
		normal32 := []byte{
			0x51,            // SET_TYPE_IMM 1 (pointer)
			0x40, 'r', 0x00, // symbol "r"
			0x70, 0x00, // seg0 offset 0
			0x90, // DO_BIND @0, stride 4 -> 4
			0x52, // SET_TYPE_IMM 2 (text_abs32)
			0x90, // DO_BIND @4, stride 4 -> 8
			0x00,
		}
		f, _ = openFixture(c, false, segs32, testStreams{normal: normal32}, true)
		if f != nil {
			got, err := f.ReadBindings(BindNormal)
			c.Check(err == nil, "stride32: ReadBindings: %v", err)
			checkBindings(c, "stride32", got, []wantBind{
				{6, "r", 0, 0, 0, 1, 0, 0, 0x2000, true, 0x300},
				{8, "r", 0, 0, 0, 2, 0, 4, 0x2004, true, 0x304},
			})
		}
	})
}

// TestBindingContract_Lazy checks that in the lazy stream DONE ends one record
// and resets all state, that consecutive zeros are empty records, and that a
// later record inherits nothing from an earlier one.
func TestBindingContract_Lazy(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		segs := []testSeg{{name: "__DATA", addr: 0x3000, memsz: 0x1000, fileoff: 0x200, filesz: 0x100}}
		lazy := []byte{
			0x13,                      // SET_DYLIB_ORDINAL_IMM 3
			0x40, 'o', 'n', 'e', 0x00, // symbol "one"
			0x51,       // SET_TYPE_IMM 1
			0x60, 0x05, // SET_ADDEND_SLEB 5
			0x70, 0x10, // seg0 offset 0x10
			0x90,                      // DO_BIND @0x10
			0x00,                      // DONE: end of first record
			0x00,                      // empty record
			0x00,                      // empty record
			0x40, 't', 'w', 'o', 0x00, // second record: only symbol "two"
			0x70, 0x20, // seg0 offset 0x20
			0x90, // DO_BIND @0x20
			0x00, // DONE: end of second record
		}
		f, _ := openFixture(c, false, segs, testStreams{lazy: lazy}, true)
		if f != nil {
			got, err := f.ReadBindings(BindLazy)
			c.Check(err == nil, "lazy: ReadBindings: %v", err)
			// The second binding must not inherit ordinal 3, addend 5, type 1
			// from "one" except the pointer default; ordinal/addend are zero
			// and flags zero.
			checkBindings(c, "lazy", got, []wantBind{
				{11, "one", 0, 3, 5, 1, 0, 0x10, 0x3010, true, 0x210},
				{22, "two", 0, 0, 0, 1, 0, 0x20, 0x3020, true, 0x220},
			})
		}
	})
}

// TestBindingContract_Mapping checks segment-index mapping (including
// __PAGEZERO and duplicate segment names), file-byte vs zero-filled mapping,
// and the read-only error behaviour on damaged streams.
func TestBindingContract_Mapping(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		segs := []testSeg{
			{name: "__PAGEZERO", addr: 0, memsz: 0x1000, fileoff: 0, filesz: 0},
			{name: "__DATA", addr: 0x1000, memsz: 0x1000, fileoff: 0x200, filesz: 0x100},
			{name: "__DATA", addr: 0x3000, memsz: 0x1000, fileoff: 0x400, filesz: 0x100},
		}
		normal := []byte{
			0x51,            // SET_TYPE_IMM 1 (pointer)
			0x40, 'm', 0x00, // symbol "m"
			0x71, 0x70, // seg1 offset 0x70
			0x90,             // bind @0x70
			0x71, 0xfc, 0x01, // seg1 offset 0xfc (crosses Filesz)
			0x90,             // bind @0xfc
			0x71, 0x80, 0x04, // seg1 offset 0x200 (zero-filled tail)
			0x90,       // bind @0x200
			0x72, 0x10, // seg2 (same name) offset 0x10
			0x90,       // bind @0x10
			0x70, 0x00, // seg0 (__PAGEZERO) offset 0
			0x90, // bind @0
			0x00,
		}
		f, raw := openFixture(c, true, segs, testStreams{normal: normal}, true)
		if f != nil {
			snapshot := append([]byte(nil), raw...)
			got, err := f.ReadBindings(BindNormal)
			c.Check(err == nil, "mapping: ReadBindings: %v", err)
			checkBindings(c, "mapping", got, []wantBind{
				{6, "m", 0, 0, 0, 1, 1, 0x70, 0x1070, true, 0x270},
				{10, "m", 0, 0, 0, 1, 1, 0xfc, 0x10fc, false, 0},
				{14, "m", 0, 0, 0, 1, 1, 0x200, 0x1200, false, 0},
				{17, "m", 0, 0, 0, 1, 2, 0x10, 0x3010, true, 0x410},
				{20, "m", 0, 0, 0, 1, 0, 0, 0, false, 0},
			})
			c.Check(bytes.Equal(raw, snapshot), "mapping: input bytes changed")
		}

		// A damaged normal stream must not affect a later valid call on the
		// same File, nor mutate the input bytes or decoded stream slice.
		badNormal := []byte{0xe0} // unknown opcode
		goodLazy := []byte{0x40, 'z', 0x00, 0x70, 0x00, 0x90, 0x00}
		f, raw = openFixture(c, true, segs, testStreams{normal: badNormal, lazy: goodLazy}, true)
		if f != nil {
			snapshot := append([]byte(nil), raw...)
			streamCopy := append([]byte(nil), f.DylinkInfo.BindingInfoDat...)
			got, err := f.ReadBindings(BindNormal)
			c.Check(err != nil, "bad normal: expected error")
			c.Check(got == nil, "bad normal: expected nil list, got %d", len(got))
			if err != nil {
				c.Check(strings.Contains(err.Error(), "normal"), "bad normal: error should name kind: %v", err)
				c.Check(strings.Contains(err.Error(), "stream offset"), "bad normal: error should name stream offset: %v", err)
			}
			got, err = f.ReadBindings(BindLazy)
			c.Check(err == nil, "good lazy after bad normal: %v", err)
			checkBindings(c, "good lazy after bad normal", got, []wantBind{
				{5, "z", 0, 0, 0, 1, 0, 0, 0, false, 0},
			})
			c.Check(bytes.Equal(raw, snapshot), "bad normal: input bytes changed")
			c.Check(bytes.Equal(f.DylinkInfo.BindingInfoDat, streamCopy), "bad normal: decoded stream bytes changed")
		}

		// Threaded binding is explicitly unsupported.
		f, _ = openFixture(c, true, segs, testStreams{normal: []byte{0xd0}}, true)
		if f != nil {
			_, err := f.ReadBindings(BindNormal)
			c.Check(err != nil && strings.Contains(err.Error(), "threaded"), "threaded: want unsupported error, got %v", err)
		}

		// Non-zero data after DONE in a normal stream is corruption.
		f, _ = openFixture(c, true, segs, testStreams{normal: []byte{0x00, 0x01}}, true)
		if f != nil {
			_, err := f.ReadBindings(BindNormal)
			c.Check(err != nil && strings.Contains(err.Error(), "after DONE"), "nonzero after DONE: got %v", err)
		}

		// Declared length must match the slice length.
		f, _ = openFixture(c, true, segs, testStreams{normal: normal}, true)
		if f != nil {
			declared := f.DylinkInfo.BindingInfoLen
			f.DylinkInfo.BindingInfoLen = declared + 1
			_, err := f.ReadBindings(BindNormal)
			c.Check(err != nil, "length mismatch: expected error")
			f.DylinkInfo.BindingInfoLen = declared
			_, err = f.ReadBindings(BindNormal)
			c.Check(err == nil, "length mismatch: restored call failed: %v", err)
		}

		// Invalid kind.
		f, _ = openFixture(c, true, segs, testStreams{normal: normal}, true)
		if f != nil {
			_, err := f.ReadBindings(BindKind(99))
			c.Check(err != nil, "invalid kind: expected error")
		}

		// No DylinkInfo at all returns an empty result.
		f, _ = openFixture(c, true, segs, testStreams{}, false)
		if f != nil {
			c.Check(f.DylinkInfo == nil, "no dylink: DylinkInfo unexpectedly present")
			got, err := f.ReadBindings(BindNormal)
			c.Check(err == nil, "no dylink: %v", err)
			c.Check(len(got) == 0, "no dylink: got %d bindings", len(got))
		}

		// An empty stream returns an empty result.
		f, _ = openFixture(c, true, segs, testStreams{}, true)
		if f != nil {
			for _, kind := range []BindKind{BindNormal, BindWeak, BindLazy} {
				got, err := f.ReadBindings(kind)
				c.Check(err == nil, "empty stream kind %d: %v", kind, err)
				c.Check(len(got) == 0, "empty stream kind %d: got %d bindings", kind, len(got))
			}
		}

		// A field beyond Memsz is rejected.
		f, _ = openFixture(c, true, segs, testStreams{normal: []byte{0x71, 0x80, 0x20, 0x90}}, true) // seg1 offset 0x1000
		if f != nil {
			_, err := f.ReadBindings(BindNormal)
			c.Check(err != nil && strings.Contains(err.Error(), "segment memory"), "memsz: got %v", err)
		}

		// A segment index past the available segments is rejected, not capped.
		f, _ = openFixture(c, true, segs, testStreams{normal: []byte{0x75, 0x00}}, true)
		if f != nil {
			_, err := f.ReadBindings(BindNormal)
			c.Check(err != nil && strings.Contains(err.Error(), "segment index"), "segment index: got %v", err)
		}

		// ULEB overflow and truncation are reported, not wrapped.
		over := append([]byte{0x80}, bytes.Repeat([]byte{0x80}, 9)...)
		over = append(over, 0x02)
		f, _ = openFixture(c, true, segs, testStreams{normal: over}, true)
		if f != nil {
			_, err := f.ReadBindings(BindNormal)
			c.Check(err != nil && strings.Contains(err.Error(), "ULEB128 overflow"), "uleb overflow: got %v", err)
		}
		f, _ = openFixture(c, true, segs, testStreams{normal: []byte{0x80, 0x80}}, true)
		if f != nil {
			_, err := f.ReadBindings(BindNormal)
			c.Check(err != nil && strings.Contains(err.Error(), "truncated"), "uleb truncated: got %v", err)
		}
	})
}
