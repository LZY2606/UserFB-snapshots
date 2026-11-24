package pe

import (
	"bytes"
	"encoding/binary"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"sort"
	"strconv"
	"strings"
	"testing"

	"github.com/mewmew/pe/task_support/fixtures"
	"github.com/mewmew/pe/task_support/taskcheck"
)

// --- independent fixture expectation builder ---------------------------------
//
// expectLine mirrors one source-line coverage derived without using any of the
// production embedded-CodeView types: the directory, segment map, module
// contributions, source files and line tables are walked directly from the
// payload bytes.

type expectLine struct {
	module                        uint16
	moduleName, file              string
	line                          uint16
	startRVA, endRVA              uint32
	subsection, fileRec, lineElem uint32
}

type expectEntry struct {
	kind, mod uint16
	off, size uint32
}

// independentExpectations re-derives source-line coverage straight from the
// CodeView payload and PE section headers. It is deliberately written as
// straight-line byte parsing separate from codeview_embedded.go.
func independentExpectations(t *testing.T, payload []byte, sects []SectionHeader) map[uint32][]expectLine {
	t.Helper()
	le := binary.LittleEndian
	dirOff := le.Uint32(payload[4:])
	cbDirHeader := le.Uint16(payload[dirOff:])
	cbDirEntry := le.Uint16(payload[dirOff+2:])
	cDir := le.Uint32(payload[dirOff+4:])
	if le.Uint32(payload[dirOff+8:]) != 0 {
		t.Fatalf("independent parser: nextDir must be zero")
	}
	var entries []expectEntry
	for i := uint32(0); i < cDir; i++ {
		e := uint32(dirOff) + uint32(cbDirHeader) + i*uint32(cbDirEntry)
		entries = append(entries, expectEntry{
			kind: le.Uint16(payload[e:]),
			mod:  le.Uint16(payload[e+2:]),
			off:  le.Uint32(payload[e+4:]),
			size: le.Uint32(payload[e+8:]),
		})
	}

	// Logical segment -> physical frame/offset.
	type segDesc struct {
		frame      uint16
		off, cbSeg uint32
	}
	segMap := map[uint16]segDesc{}
	type contrib struct {
		seg     uint16
		off, sz uint32
	}
	modContrib := map[uint16][]contrib{}
	modName := map[uint16]string{}
	type srcRef struct {
		mod                      uint16
		sub                      uint32
		modStart, modEnd, modSeg []uint32
		modSeg16                 []uint16
	}
	var srcs []srcRef

	for _, en := range entries {
		base := en.off
		switch en.kind {
		case 0x12d: // sstSegMap
			cSeg := le.Uint16(payload[base:])
			for i := uint16(0); i < cSeg; i++ {
				o := base + 4 + uint32(i)*20
				segMap[i+1] = segDesc{
					frame: le.Uint16(payload[o+6:]),
					off:   le.Uint32(payload[o+12:]),
					cbSeg: le.Uint32(payload[o+16:]),
				}
			}
		case 0x120: // sstModule
			cSeg := le.Uint16(payload[base+4:])
			for i := uint16(0); i < cSeg; i++ {
				o := base + 8 + uint32(i)*12
				modContrib[en.mod] = append(modContrib[en.mod], contrib{
					seg: le.Uint16(payload[o:]),
					off: le.Uint32(payload[o+4:]),
					sz:  le.Uint32(payload[o+8:]),
				})
			}
			nameOff := base + 8 + uint32(cSeg)*12
			n := uint32(payload[nameOff])
			modName[en.mod] = string(payload[nameOff+1 : nameOff+1+n])
		case 0x127: // sstSrcModule
			cFile := le.Uint16(payload[base:])
			cSeg := le.Uint16(payload[base+2:])
			sr := srcRef{mod: en.mod, sub: base}
			for i := uint16(0); i < cSeg; i++ {
				o := base + 4 + uint32(cFile)*4 + uint32(i)*8
				sr.modStart = append(sr.modStart, le.Uint32(payload[o:]))
				sr.modEnd = append(sr.modEnd, le.Uint32(payload[o+4:]))
				sr.modSeg16 = append(sr.modSeg16, le.Uint16(payload[base+4+uint32(cFile)*4+uint32(cSeg)*8+uint32(i)*2:]))
			}
			for _, s := range sr.modSeg16 {
				sr.modSeg = append(sr.modSeg, uint32(s))
			}
			srcs = append(srcs, sr)
		}
	}

	want := map[uint32][]expectLine{}
	add := func(l expectLine) {
		// Materialize the line over every RVA in its half-open interval.
		for rva := l.startRVA; rva < l.endRVA; rva++ {
			want[rva] = append(want[rva], l)
		}
	}

	for _, sr := range srcs {
		cFile := le.Uint16(payload[sr.sub:])
		for fi := uint16(0); fi < cFile; fi++ {
			fileRel := le.Uint32(payload[sr.sub+4+uint32(fi)*4:])
			rec := sr.sub + fileRel
			fileCSeg := le.Uint16(payload[rec:])
			nameRel := fileRel + 4 + uint32(fileCSeg)*4 + uint32(fileCSeg)*8
			n := uint32(payload[sr.sub+nameRel])
			fileName := string(payload[sr.sub+nameRel+1 : sr.sub+nameRel+1+n])
			for k := uint16(0); k < fileCSeg; k++ {
				// Source file segment closed interval.
				pair := rec + 4 + uint32(fileCSeg)*4 + uint32(k)*8
				fStart := le.Uint32(payload[pair:])
				fEnd := le.Uint32(payload[pair+4:])
				tblRel := le.Uint32(payload[rec+4+uint32(k)*4:])
				tbl := sr.sub + tblRel
				seg := le.Uint16(payload[tbl:])
				cLn := le.Uint16(payload[tbl+2:])
				offs := make([]uint32, cLn)
				lns := make([]uint16, cLn)
				elems := make([]uint32, cLn)
				for i := uint16(0); i < cLn; i++ {
					ep := tbl + 4 + uint32(i)*4
					elems[i] = ep
					offs[i] = le.Uint32(payload[ep:])
					lns[i] = le.Uint16(payload[tbl+4+uint32(cLn)*4+uint32(i)*2:])
				}
				desc := segMap[seg]
				sect := sects[desc.frame-1]
				baseRVA := sect.RelAddr + desc.off
				// Source module segment range for this logical segment.
				mLo, mHi := uint32(0), desc.cbSeg
				for i, s := range sr.modSeg16 {
					if s == seg {
						mLo, mHi = sr.modStart[i], sr.modEnd[i]+1
					}
				}
				for i := uint16(0); i < cLn; i++ {
					lineHi := fEnd + 1
					for j := i + 1; j < cLn; j++ {
						if offs[j] > offs[i] {
							lineHi = offs[j]
							break
						}
					}
					for _, cn := range modContrib[sr.mod] {
						if cn.seg != seg {
							continue
						}
						lo := max32i(mLo, fStart, offs[i], cn.off)
						hi := min32i(mHi, fEnd+1, lineHi, cn.off+cn.sz, desc.cbSeg,
							sect.VirtualSize-desc.off)
						if lo >= hi {
							continue
						}
						add(expectLine{
							module:     sr.mod,
							moduleName: modName[sr.mod],
							file:       fileName,
							line:       lns[i],
							startRVA:   baseRVA + lo,
							endRVA:     baseRVA + hi,
							subsection: sr.sub,
							fileRec:    rec,
							lineElem:   elems[i],
						})
					}
				}
			}
		}
	}
	for rva := range want {
		ls := want[rva]
		sort.SliceStable(ls, func(i, j int) bool {
			if ls[i].module != ls[j].module {
				return ls[i].module < ls[j].module
			}
			if ls[i].subsection != ls[j].subsection {
				return ls[i].subsection < ls[j].subsection
			}
			if ls[i].fileRec != ls[j].fileRec {
				return ls[i].fileRec < ls[j].fileRec
			}
			return ls[i].lineElem < ls[j].lineElem
		})
		want[rva] = ls
	}
	return want
}

func max32i(xs ...uint32) uint32 {
	m := xs[0]
	for _, x := range xs[1:] {
		if x > m {
			m = x
		}
	}
	return m
}

func min32i(xs ...uint32) uint32 {
	m := xs[0]
	for _, x := range xs[1:] {
		if x < m {
			m = x
		}
	}
	return m
}

func payloadOf(t *testing.T, content []byte) []byte {
	t.Helper()
	// Debug directory entry lives at file offset 0xc00 in both fixture and
	// custom images; read its file pointer and size independently.
	le := binary.LittleEndian
	size := le.Uint32(content[0xc00+16:])
	off := le.Uint32(content[0xc00+24:])
	return content[off : off+size]
}

func linesEqual(c *taskcheck.C, got []CodeViewSourceLine, want []expectLine, ctx string) {
	c.Check(len(got) == len(want), "%s: got %d lines, want %d", ctx, len(got), len(want))
	n := len(got)
	if len(want) < n {
		n = len(want)
	}
	for i := 0; i < n; i++ {
		g, w := got[i], want[i]
		c.Check(g.Module == w.module, "%s[%d]: module got %d want %d", ctx, i, g.Module, w.module)
		c.Check(g.ModuleName == w.moduleName, "%s[%d]: module name got %q want %q", ctx, i, g.ModuleName, w.moduleName)
		c.Check(g.File == w.file, "%s[%d]: file got %q want %q", ctx, i, g.File, w.file)
		c.Check(g.Line == w.line, "%s[%d]: line got %d want %d", ctx, i, g.Line, w.line)
		c.Check(g.StartRVA == w.startRVA, "%s[%d]: start got %#x want %#x", ctx, i, g.StartRVA, w.startRVA)
		c.Check(g.EndRVA == w.endRVA, "%s[%d]: end got %#x want %#x", ctx, i, g.EndRVA, w.endRVA)
		c.Check(g.SubsectionOffset == w.subsection, "%s[%d]: subsection offset got %#x want %#x", ctx, i, g.SubsectionOffset, w.subsection)
		c.Check(g.FileOffset == w.fileRec, "%s[%d]: file offset got %#x want %#x", ctx, i, g.FileOffset, w.fileRec)
		c.Check(g.LineOffset == w.lineElem, "%s[%d]: line offset got %#x want %#x", ctx, i, g.LineOffset, w.lineElem)
	}
}

// --- custom PE/OMF builder ---------------------------------------------------

type customSect struct {
	name             string
	rva, virtualSize uint32
	raw, rawSize     uint32
}

// buildCustomPE wraps a hand-built CodeView payload in a minimal PE32+ image.
// The layout intentionally differs from task_support/fixtures.
func buildCustomPE(payload []byte) []byte {
	sects := []customSect{
		{".a", 0x2000, 0x200, 0x400, 0x200},
		{".b", 0x4000, 0x200, 0x800, 0x200},
		{".c", 0x6000, 0x200, 0xc00, 0x200},
		{".d", 0x8000, 0x900, 0x1000, 0x900},
	}
	b := make([]byte, 0x1a00)
	le := binary.LittleEndian
	copy(b, "MZ")
	le.PutUint32(b[0x3c:], 0x80)
	copy(b[0x80:], "PE\x00\x00")
	le.PutUint16(b[0x84:], 0x8664)
	le.PutUint16(b[0x86:], uint16(len(sects)))
	le.PutUint16(b[0x94:], 240)
	le.PutUint16(b[0x96:], 0x2022)
	o := 0x98
	le.PutUint16(b[o:], 0x20b)
	le.PutUint64(b[o+24:], 0x180000000) // distinct ImageBase: must never enter RVAs
	le.PutUint32(b[o+32:], 0x1000)
	le.PutUint32(b[o+56:], 0x9000)
	le.PutUint32(b[o+60:], 0x400)
	le.PutUint16(b[o+68:], uint16(len(sects)))
	le.PutUint32(b[o+108:], 16)
	// Debug data directory entry (index 6).
	le.PutUint32(b[o+112+6*8:], 0x8000)
	le.PutUint32(b[o+112+6*8+4:], 28)
	for i, s := range sects {
		h := o + 240 + i*40
		copy(b[h:], s.name)
		le.PutUint32(b[h+8:], s.virtualSize)
		le.PutUint32(b[h+12:], s.rva)
		le.PutUint32(b[h+16:], s.rawSize)
		le.PutUint32(b[h+20:], s.raw)
		le.PutUint32(b[h+36:], 0x40000040)
	}
	// Debug directory at raw 0x1000 (RVA 0x8000); payload at raw 0x1100.
	le.PutUint32(b[0xc00+12:], 2) // not used; keep the fixture-style slot sane
	d := 0x1000
	le.PutUint32(b[d+12:], 2)
	le.PutUint32(b[d+16:], uint32(len(payload)))
	le.PutUint32(b[d+20:], 0x8100)
	le.PutUint32(b[d+24:], 0x1100)
	copy(b[0x1100:], payload)
	return b
}

type customBuilder struct {
	b      []byte
	dirOff uint32
}

func newCustomBuilder() *customBuilder {
	return &customBuilder{b: make([]byte, 0x500), dirOff: 0x400}
}

func (cb *customBuilder) w16(off uint32, x uint16) { binary.LittleEndian.PutUint16(cb.b[off:], x) }
func (cb *customBuilder) w32(off uint32, x uint32) { binary.LittleEndian.PutUint32(cb.b[off:], x) }
func (cb *customBuilder) pstr(off uint32, s string) {
	cb.b[off] = byte(len(s))
	copy(cb.b[off+1:], s)
}

// buildCustomPayload constructs scattered OMF records for:
//   - iMod 3 "gamma.obj" on logical segment 3 (physical section 1) with one
//     contribution [0x30,0x50) and same-address lines;
//   - iMod 42 "delta.obj" on logical segment 1 (physical section 3) with two
//     contributions separated by a gap.
//
// The directory uses extended header/entry strides and shuffled entry order.
func (cb *customBuilder) build(signature string) []byte {
	copy(cb.b, signature)
	cb.w32(4, cb.dirOff)

	// sstSegMap at 0x40: three logical segments, deliberately non-identity.
	segBase := uint32(0x40)
	cb.w16(segBase, 3)
	cb.w16(segBase+2, 3)
	descs := []struct {
		frame      uint16
		off, cbSeg uint32
	}{
		{3, 0x10, 0x80}, // logical 1 -> PE section 3
		{2, 0x00, 0x40}, // logical 2 -> PE section 2
		{1, 0x20, 0x80}, // logical 3 -> PE section 1
	}
	for i, d := range descs {
		o := segBase + 4 + uint32(i)*20
		cb.w16(o, 0x109)
		cb.w16(o+6, d.frame)
		cb.w16(o+8, 0xffff)
		cb.w16(o+10, 0xffff)
		cb.w32(o+12, d.off)
		cb.w32(o+16, d.cbSeg)
	}

	// sstModule gamma (iMod 3) at 0x90.
	cb.w16(0x90+4, 1)
	copy(cb.b[0x90+6:], "CV")
	cb.w16(0x90+8, 3)
	cb.w32(0x90+12, 0x30)
	cb.w32(0x90+16, 0x20)
	cb.pstr(0x90+20, "gamma.obj")

	// sstModule delta (iMod 42) at 0xd0: two contributions on logical seg 1.
	cb.w16(0xd0+4, 2)
	copy(cb.b[0xd0+6:], "CV")
	cb.w16(0xd0+8, 1)
	cb.w32(0xd0+12, 0x10)
	cb.w32(0xd0+16, 0x10)
	cb.w16(0xd0+20, 1)
	cb.w32(0xd0+24, 0x40)
	cb.w32(0xd0+28, 0x10)
	cb.pstr(0xd0+32, "delta.obj")

	// Unrecognized subsection at 0x180: must be skipped, not parsed.
	cb.w16(0x180, 0x12f)

	// gamma sstSrcModule at 0x200; file record at 0x260; line table at 0x2c0.
	cb.w16(0x200, 1)
	cb.w16(0x202, 1)
	cb.w32(0x204, 0x60)
	cb.w32(0x208, 0x28)
	cb.w32(0x20c, 0x5f)
	cb.w16(0x210, 3)
	cb.w16(0x260, 1)
	cb.w32(0x264, 0xc0) // baseSrcLn relative to sstSrcModule start 0x200
	cb.w32(0x268, 0x28)
	cb.w32(0x26c, 0x5f)
	cb.pstr(0x270, "gamma.c")
	cb.w16(0x2c0, 3)
	cb.w16(0x2c2, 4)
	for i, x := range []uint32{0x28, 0x28, 0x40, 0x55} {
		cb.w32(0x2c4+uint32(i)*4, x)
	}
	for i, x := range []uint16{10, 11, 12, 13} {
		cb.w16(0x2d4+uint32(i)*2, x)
	}

	// delta sstSrcModule at 0x320; file record at 0x370; line table at 0x3c0.
	cb.w16(0x320, 1)
	cb.w16(0x322, 1)
	cb.w32(0x324, 0x50)
	cb.w32(0x328, 0x10)
	cb.w32(0x32c, 0x50)
	cb.w16(0x330, 1)
	cb.w16(0x370, 1)
	cb.w32(0x374, 0xa0)
	cb.w32(0x378, 0x10)
	cb.w32(0x37c, 0x50)
	cb.pstr(0x380, "delta.c")
	cb.w16(0x3c0, 1)
	cb.w16(0x3c2, 3)
	for i, x := range []uint32{0x10, 0x18, 0x48} {
		cb.w32(0x3c4+uint32(i)*4, x)
	}
	for i, x := range []uint16{5, 6, 7} {
		cb.w16(0x3d0+uint32(i)*2, x)
	}

	// Directory at 0x400 with 4 bytes of trailing header/entry extension.
	cb.w16(cb.dirOff, 20)
	cb.w16(cb.dirOff+2, 16)
	cb.w32(cb.dirOff+4, 6)
	cb.w32(cb.dirOff+8, 0)
	cb.w32(cb.dirOff+12, 0)
	type dEntry struct {
		kind, mod uint16
		off, size uint32
	}
	entries := []dEntry{
		{0x12d, 0xffff, 0x40, 64},
		{0x127, 42, 0x320, 0xb6},
		{0x120, 3, 0x90, 0x2a},
		{0x12f, 3, 0x180, 16},
		{0x127, 3, 0x200, 0xe0},
		{0x120, 42, 0xd0, 0x2a},
	}
	for i, e := range entries {
		p := cb.dirOff + 20 + uint32(i)*16
		cb.w16(p, e.kind)
		cb.w16(p+2, e.mod)
		cb.w32(p+4, e.off)
		cb.w32(p+8, e.size)
	}
	return cb.b
}

// buildMultiDebugPE places CodeView, FPO and Misc debug directories side by
// side so the legacy FPO/Misc readers can be regression-checked alongside the
// new embedded CodeView path.
func buildMultiDebugPE(cvPayload []byte) []byte {
	b := buildCustomPE(cvPayload)
	le := binary.LittleEndian
	// Three 28-byte debug directory entries at raw 0x1000 (RVA 0x8000).
	d := 0x1000
	le.PutUint32(b[d+12:], 2) // entry 0: CodeView (already populated)
	// entry 1: FPO, one 16-byte record at raw 0x1600 (RVA 0x8600).
	fpo := d + 28
	le.PutUint32(b[fpo+12:], 3)
	le.PutUint32(b[fpo+16:], 16)
	le.PutUint32(b[fpo+20:], 0x8600)
	le.PutUint32(b[fpo+24:], 0x1600)
	le.PutUint32(b[0x1600:], 0x111) // StartOffset
	le.PutUint32(b[0x1604:], 0x22)  // FuncSize
	le.PutUint32(b[0x1608:], 4)     // NLocals /4
	le.PutUint16(b[0x160c:], 8)     // Params /4
	b[0x160e] = 3                   // Prolog
	b[0x160f] = 0
	// entry 2: Misc raw content at raw 0x1700 (RVA 0x8700).
	misc := d + 56
	le.PutUint32(b[misc+12:], 4)
	le.PutUint32(b[misc+16:], 8)
	le.PutUint32(b[misc+20:], 0x8700)
	le.PutUint32(b[misc+24:], 0x1700)
	copy(b[0x1700:], []byte("MISCDATA"))
	// Grow data directory size to three entries.
	le.PutUint32(b[0x98+112+6*8+4:], 3*28)
	return b
}

// --- tests -------------------------------------------------------------------

// buildOverlapPE constructs two modules whose contributions on one logical
// segment overlap, so a single RVA can legitimately resolve to both modules.
func buildOverlapPE(signature string) []byte {
	cb := newCustomBuilder()
	copy(cb.b, signature)
	cb.w32(4, 0x400)

	// One logical segment: array position 1 -> physical frame 1 + 0x20.
	segBase := uint32(0x40)
	cb.w16(segBase, 1)
	cb.w16(segBase+2, 1)
	d := segBase + 4
	cb.w16(d, 0x109)     // flags
	cb.w16(d+2, 0)       // overlay
	cb.w16(d+4, 0)       // group
	cb.w16(d+6, 1)       // frame: PE section 1
	cb.w16(d+8, 0xffff)  // iSegName
	cb.w16(d+10, 0xffff) // iClassName
	cb.w32(d+12, 0x20)   // offset
	cb.w32(d+16, 0x80)   // cbSeg

	// iMod 5 "eps.obj" contribution logical seg 1 [0x10,0x30); iMod 6
	// "zeta.obj" contribution seg 1 [0x20,0x40); overlap [0x20,0x30).
	module := func(off, mod, start, size uint32, name string) {
		cb.w16(off+4, 1)
		copy(cb.b[off+6:], "CV")
		cb.w16(off+8, 1)
		cb.w32(off+12, start)
		cb.w32(off+16, size)
		cb.pstr(off+20, name)
	}
	module(0x80, 5, 0x10, 0x20, "eps.obj")
	module(0xb0, 6, 0x20, 0x20, "zeta.obj")

	// eps sstSrcModule 0x180, file record 0x1c0, line table 0x1e0.
	cb.w16(0x180, 1)
	cb.w16(0x182, 1)
	cb.w32(0x184, 0x40)
	cb.w32(0x188, 0x10)
	cb.w32(0x18c, 0x2f)
	cb.w16(0x190, 1)
	cb.w16(0x1c0, 1)
	cb.w32(0x1c4, 0x60)
	cb.w32(0x1c8, 0x10)
	cb.w32(0x1cc, 0x2f)
	cb.pstr(0x1d0, "eps.c")
	cb.w16(0x1e0, 1)
	cb.w16(0x1e2, 2)
	cb.w32(0x1e4, 0x10)
	cb.w32(0x1e8, 0x28)
	cb.w16(0x1ec, 21)
	cb.w16(0x1ee, 22)

	// zeta sstSrcModule 0x280, file record 0x2c0, line table 0x2e0.
	cb.w16(0x280, 1)
	cb.w16(0x282, 1)
	cb.w32(0x284, 0x40)
	cb.w32(0x288, 0x18)
	cb.w32(0x28c, 0x3f)
	cb.w16(0x290, 1)
	cb.w16(0x2c0, 1)
	cb.w32(0x2c4, 0x60)
	cb.w32(0x2c8, 0x18)
	cb.w32(0x2cc, 0x3f)
	cb.pstr(0x2d0, "zeta.c")
	cb.w16(0x2e0, 1)
	cb.w16(0x2e2, 2)
	cb.w32(0x2e4, 0x18)
	cb.w32(0x2e8, 0x24)
	cb.w16(0x2ec, 31)
	cb.w16(0x2ee, 32)

	// Directory at 0x400, entries shuffled.
	cb.w16(0x400, 16)
	cb.w16(0x402, 12)
	cb.w32(0x404, 5)
	entries := []struct {
		kind, mod uint16
		off, size uint32
	}{
		{0x120, 6, 0xb0, 0x2a},
		{0x127, 5, 0x180, 0x70},
		{0x12d, 0xffff, 0x40, 24},
		{0x120, 5, 0x80, 0x2a},
		{0x127, 6, 0x280, 0x76},
	}
	for i, e := range entries {
		p := uint32(0x410 + i*12)
		cb.w16(p, e.kind)
		cb.w16(p+2, e.mod)
		cb.w32(p+4, e.off)
		cb.w32(p+8, e.size)
	}
	return buildCustomPE(cb.b)
}

func TestCodeViewDispatch(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		for _, sig := range []string{"NB09", "NB11"} {
			raw := fixtures.Embedded(sig)
			snapshot := append([]byte(nil), raw...)

			// ParseBytes entry point.
			f, err := ParseBytes(raw)
			c.Check(err == nil, "%s ParseBytes: %v", sig, err)
			cv := f.DbgData[0].(*DebugCodeView)
			c.Check(cv.Embedded != nil, "%s: Embedded must be non-nil", sig)
			c.Check(cv.Age == 0 && cv.PDBPath == "" && cv.Date.IsZero(),
				"%s: embedded payload must not expose external PDB identity", sig)
			c.Check(cv.Offset == 0x300, "%s: directory offset got %#x", sig, cv.Offset)
			c.Check(string([]byte{byte(cv.Signature), byte(cv.Signature >> 8), byte(cv.Signature >> 16), byte(cv.Signature >> 24)}) == sig,
				"%s: signature round trip", sig)
			c.Check(len(cv.Embedded.Entries) == 5, "%s: directory entries got %d", sig, len(cv.Embedded.Entries))
			c.Check(len(cv.Embedded.Modules) == 2, "%s: modules got %d", sig, len(cv.Embedded.Modules))
			c.Check(cv.Embedded.Modules[0].Module == 7 && cv.Embedded.Modules[1].Module == 9,
				"%s: modules sorted by iMod", sig)

			// Parse entry point (io.Reader).
			f2, err := Parse(bytes.NewReader(raw))
			c.Check(err == nil, "%s Parse: %v", sig, err)
			cv2 := f2.DbgData[0].(*DebugCodeView)
			c.Check(cv2.Embedded != nil && reflect.DeepEqual(cv2.Embedded.Entries, cv.Embedded.Entries),
				"%s: Parse and ParseBytes agree", sig)

			// ParseFile entry point.
			path := filepath.Join(t.TempDir(), sig+".pe")
			c.Check(os.WriteFile(path, raw, 0600) == nil, "%s: write sample", sig)
			f3, err := ParseFile(path)
			c.Check(err == nil, "%s ParseFile: %v", sig, err)
			cv3 := f3.DbgData[0].(*DebugCodeView)
			c.Check(cv3.Embedded != nil && cv3.Embedded.DirectoryOffset == 0x300,
				"%s: ParseFile restores embedded directory", sig)

			// Queries must not mutate the input bytes.
			for _, rva := range []uint32{0x1044, 0x30c0, 0x1060, 0x105b} {
				_ = cv.SourceLines(rva)
				_ = cv3.SourceLines(rva)
			}
			c.Check(bytes.Equal(raw, snapshot), "%s: input bytes unchanged after queries", sig)
		}

		// Legacy NB10 external PDB reference stays on the old code path.
		nb10, err := ParseBytes(fixtures.NB10())
		c.Check(err == nil, "NB10 parse: %v", err)
		legacy := nb10.DbgData[0].(*DebugCodeView)
		c.Check(legacy.Embedded == nil, "NB10: Embedded must be nil")
		c.Check(legacy.PDBPath == "offline-demo.pdb" && legacy.Age == 3 && legacy.Offset == 0,
			"NB10: legacy fields preserved")
		c.Check(len(legacy.SourceLines(0x1044)) == 0, "NB10: no embedded source lines")

		// Legacy FPO and Misc debug directories keep working next to an
		// embedded CodeView directory in the same image.
		multi := buildMultiDebugPE(newCustomBuilder().build("NB09"))
		mf, err := ParseBytes(multi)
		c.Check(err == nil, "multi-debug parse: %v", err)
		var sawFPO, sawMisc, sawEmbeddedCV bool
		for _, d := range mf.DbgData {
			switch v := d.(type) {
			case *DebugFPO:
				sawFPO = true
				c.Check(len(v.FPOData) == 1 && v.FPOData[0].StartOffset == 0x111 &&
					v.FPOData[0].FuncSize == 0x22 && v.FPOData[0].Prolog == 3,
					"FPO record preserved")
			case *DebugMisc:
				sawMisc = true
				c.Check(string(v.Content) == "MISCDATA", "Misc content preserved")
			case *DebugCodeView:
				sawEmbeddedCV = v.Embedded != nil
			}
		}
		c.Check(sawFPO && sawMisc && sawEmbeddedCV,
			"FPO=%v Misc=%v embeddedCV=%v all dispatched", sawFPO, sawMisc, sawEmbeddedCV)

		// Unknown signature falls through to the legacy NB10-style parser.
		payload := make([]byte, 40)
		copy(payload, "NBxx")
		copy(payload[16:], "other.pdb\x00")
		unk, err := ParseBytes(buildCustomPE(payload))
		c.Check(err == nil, "unknown signature parse: %v", err)
		ucv := unk.DbgData[0].(*DebugCodeView)
		c.Check(ucv.Embedded == nil && ucv.PDBPath == "other.pdb",
			"unknown signature: legacy PDB path parsing")

		// Corrupt full-length payloads: malformed headers must produce
		// positioned errors rather than panics; the file itself stays intact so
		// readDebugData never slices past its content.
		full := fixtures.Embedded("NB09")
		payloadAt := 0xd00
		corrupt := func(fn func([]byte)) []byte {
			out := append([]byte(nil), full...)
			fn(out[payloadAt:])
			return out
		}
		// Directory offset points past the payload.
		_, err = ParseBytes(corrupt(func(p []byte) { binary.LittleEndian.PutUint32(p[4:], 0x900) }))
		c.Check(err != nil && strings.Contains(err.Error(), "0x"),
			"bad directory offset: positioned error, got %v", err)
		// Declared directory entry stride overruns the payload.
		_, err = ParseBytes(corrupt(func(p []byte) { binary.LittleEndian.PutUint16(p[0x302:], 0x400) }))
		c.Check(err != nil && strings.Contains(err.Error(), "directory"),
			"oversized directory stride: positioned error, got %v", err)
		// A directory entry references a subsection outside the payload.
		_, err = ParseBytes(corrupt(func(p []byte) { binary.LittleEndian.PutUint32(p[0x314:], 0x900) }))
		c.Check(err != nil && strings.Contains(err.Error(), "subsection"),
			"out-of-range subsection: positioned error, got %v", err)
		// A genuinely short payload built through the constructor path.
		shortPayload := make([]byte, 8)
		copy(shortPayload, "NB09")
		binary.LittleEndian.PutUint32(shortPayload[4:], 0x300)
		_, err = ParseBytes(buildCustomPE(shortPayload))
		c.Check(err != nil && strings.Contains(err.Error(), "0x300"),
			"short payload: positioned directory error, got %v", err)

		// Unsupported: non-zero next directory.
		withNextDir := append([]byte(nil), full...)
		binary.LittleEndian.PutUint32(withNextDir[0xd00+0x300+8:], 0x123)
		_, err = ParseBytes(withNextDir)
		c.Check(err != nil && strings.Contains(err.Error(), "not supported"),
			"nextDir unsupported error, got %v", err)
	})
}

func TestOMFSourceRelations(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		for _, sig := range []string{"NB09", "NB11"} {
			raw := buildCustomPE(newCustomBuilder().build(sig))
			snapshot := append([]byte(nil), raw...)
			f, err := ParseBytes(raw)
			c.Check(err == nil, "%s custom parse: %v", sig, err)
			cv := f.DbgData[0].(*DebugCodeView)
			c.Check(cv.Embedded != nil, "%s custom: embedded present", sig)

			// Directory identities: shuffled order, extended strides, unknown
			// subsection retained in the raw directory.
			kinds := map[uint16]int{}
			for _, e := range cv.Embedded.Entries {
				kinds[e.Subsection]++
			}
			c.Check(kinds[0x120] == 2 && kinds[0x127] == 2 && kinds[0x12d] == 1 && kinds[0x12f] == 1,
				"%s: directory entry kinds %v", sig, kinds)
			c.Check(cv.Embedded.Entries[0].Subsection == 0x12d, "%s: shuffled entry order retained", sig)

			// Module identities and contributions.
			mods := map[uint16]*EmbeddedModule{}
			for _, m := range cv.Embedded.Modules {
				mods[m.Module] = m
			}
			c.Check(mods[3].Name == "gamma.obj" && mods[42].Name == "delta.obj",
				"%s: module names via iMod", sig)
			c.Check(len(mods[3].Contributions) == 1 && mods[3].Contributions[0].Seg == 3,
				"%s: gamma contribution", sig)
			c.Check(len(mods[42].Contributions) == 2 && mods[42].Contributions[0].Off == 0x10,
				"%s: delta split contributions", sig)

			// Logical-to-physical segment map is not the identity.
			c.Check(cv.Embedded.Segments[0].Frame == 3 && cv.Embedded.Segments[2].Frame == 1,
				"%s: reversed frame mapping", sig)

			// gamma: same-address lines 10/11, contribution clipping, line 13
			// dropped because it starts past the contribution.
			got := cv.SourceLines(0x2050) // local 0x30 on logical seg 3
			linesEqual(c, got, []expectLine{
				{3, "gamma.obj", "gamma.c", 10, 0x2050, 0x2060, 0x200, 0x260, 0x2c4},
				{3, "gamma.obj", "gamma.c", 11, 0x2050, 0x2060, 0x200, 0x260, 0x2c8},
			}, sig+" gamma same-address")
			got = cv.SourceLines(0x2060) // local 0x40
			linesEqual(c, got, []expectLine{
				{3, "gamma.obj", "gamma.c", 12, 0x2060, 0x2070, 0x200, 0x260, 0x2cc},
			}, sig+" gamma second line")
			c.Check(len(cv.SourceLines(0x2048)) == 0, sig+": source line before contribution must not expand it (0x2048)")
			c.Check(len(cv.SourceLines(0x2070)) == 0, sig+": contribution end is exclusive (0x2070)")
			c.Check(len(cv.SourceLines(0x2075)) == 0, sig+": line beyond contribution dropped (0x2075)")

			// delta: gap between contributions; one line clipped into two
			// pieces sharing one LineOffset.
			got = cv.SourceLines(0x6020) // local 0x10
			linesEqual(c, got, []expectLine{
				{42, "delta.obj", "delta.c", 5, 0x6020, 0x6028, 0x320, 0x370, 0x3c4},
			}, sig+" delta first line")
			got = cv.SourceLines(0x602f) // local 0x1f, first contribution
			linesEqual(c, got, []expectLine{
				{42, "delta.obj", "delta.c", 6, 0x6028, 0x6030, 0x320, 0x370, 0x3c8},
			}, sig+" delta line 6 piece 1")
			c.Check(len(cv.SourceLines(0x6040)) == 0, sig+": contribution hole stays uncovered (0x6040)")
			got = cv.SourceLines(0x6050) // local 0x40, second contribution
			linesEqual(c, got, []expectLine{
				{42, "delta.obj", "delta.c", 6, 0x6050, 0x6058, 0x320, 0x370, 0x3c8},
			}, sig+" delta line 6 piece 2")
			got = cv.SourceLines(0x605f) // local 0x4f, last line to closed end+1
			linesEqual(c, got, []expectLine{
				{42, "delta.obj", "delta.c", 7, 0x6058, 0x6060, 0x320, 0x370, 0x3cc},
			}, sig+" delta last line")

			// Repeat and reverse-order queries are identical.
			query := []uint32{0x2050, 0x602f, 0x6040, 0x2060, 0x605f}
			first := make([][]CodeViewSourceLine, len(query))
			for i, rva := range query {
				first[i] = cv.SourceLines(rva)
			}
			for i := len(query) - 1; i >= 0; i-- {
				c.Check(reflect.DeepEqual(cv.SourceLines(query[i]), first[i]),
					"%s: deterministic query at %#x", sig, query[i])
			}
			c.Check(bytes.Equal(raw, snapshot), "%s: custom input bytes unchanged", sig)
		}

		// Full independent sweep over both shipped fixtures.
		sweep := []uint32{0x1000, 0x1044, 0x104f, 0x1050, 0x105a, 0x105b, 0x1060,
			0x3000, 0x30c0, 0x30cf, 0x30d0, 0x30df, 0x30e0, 0x4000}
		for _, sig := range []string{"NB09", "NB11"} {
			raw := fixtures.Embedded(sig)
			f, err := ParseBytes(raw)
			c.Check(err == nil, "fixture %s parse: %v", sig, err)
			cv := f.DbgData[0].(*DebugCodeView)
			want := independentExpectations(t, payloadOf(t, raw), f.SectHdrs)
			for _, rva := range sweep {
				linesEqual(c, cv.SourceLines(rva), want[rva], sig+" sweep "+strconv.FormatUint(uint64(rva), 16))
			}
			// Explicit half-open edge semantics.
			c.Check(len(cv.SourceLines(0x105b)) == 0, "%s: right edge 0x105b not hit", sig)
			c.Check(len(cv.SourceLines(0x105a)) == 1, "%s: byte before edge 0x105a hit", sig)
			c.Check(len(cv.SourceLines(0x30e0)) == 0, "%s: right edge 0x30e0 not hit", sig)
		}

		// Two modules with overlapping contributions: the shared RVA must
		// resolve to both, ordered by iMod; module-private RVAs resolve to one.
		// Logical segment 1 -> frame 1 (.a, RelAddr 0x2000) + 0x20, so local x
		// maps to RVA 0x2020+x.
		for _, sig := range []string{"NB09", "NB11"} {
			ov, err := ParseBytes(buildOverlapPE(sig))
			c.Check(err == nil, "overlap %s parse: %v", sig, err)
			ocv := ov.DbgData[0].(*DebugCodeView)

			got := ocv.SourceLines(0x2048) // local 0x28: inside both modules
			linesEqual(c, got, []expectLine{
				{5, "eps.obj", "eps.c", 22, 0x2048, 0x2050, 0x180, 0x1c0, 0x1e8},
				{6, "zeta.obj", "zeta.c", 32, 0x2044, 0x2060, 0x280, 0x2c0, 0x2e8},
			}, sig+" overlap shared RVA")
			c.Check(got[0].Module == 5 && got[1].Module == 6,
				"%s: overlap candidates sorted by iMod", sig)

			got = ocv.SourceLines(0x2030) // local 0x10: eps only
			linesEqual(c, got, []expectLine{
				{5, "eps.obj", "eps.c", 21, 0x2030, 0x2048, 0x180, 0x1c0, 0x1e4},
			}, sig+" overlap eps-only")
			// zeta line 31 is clipped to the contribution start: its surviving
			// piece is local [0x20,0x24) -> RVA [0x2040,0x2044); eps line 21
			// covers the same RVA, so both are returned in iMod order.
			got = ocv.SourceLines(0x2040)
			linesEqual(c, got, []expectLine{
				{5, "eps.obj", "eps.c", 21, 0x2030, 0x2048, 0x180, 0x1c0, 0x1e4},
				{6, "zeta.obj", "zeta.c", 31, 0x2040, 0x2044, 0x280, 0x2c0, 0x2e4},
			}, sig+" overlap zeta clipped head")
			got = ocv.SourceLines(0x2054) // local 0x34: zeta only
			linesEqual(c, got, []expectLine{
				{6, "zeta.obj", "zeta.c", 32, 0x2044, 0x2060, 0x280, 0x2c0, 0x2e8},
			}, sig+" overlap zeta-only")
			c.Check(len(ocv.SourceLines(0x2070)) == 0,
				"%s: beyond both contributions uncovered", sig)
		}

		// Malformed full-length variants: positioned errors for truncation
		// (declared via the directory or an interior reference) and explicit
		// unsupported errors for segmented/overlay modes.
		good := buildCustomPE(newCustomBuilder().build("NB09"))
		payloadBase := 0x1100
		patch := func(rel int, fn func([]byte)) []byte {
			out := append([]byte(nil), good...)
			fn(out[payloadBase+rel:])
			return out
		}
		// Shrink the sstSegMap directory entry below its header.
		_, err := ParseBytes(patch(0x41c, func(b []byte) { binary.LittleEndian.PutUint32(b, 2) }))
		c.Check(err != nil && strings.Contains(err.Error(), "sstSegMap"), "segmap truncation: %v", err)
		// Point gamma's baseSrcLn outside the sstSrcModule subsection.
		_, err = ParseBytes(patch(0x264, func(b []byte) { binary.LittleEndian.PutUint32(b, 0x300) }))
		c.Check(err != nil && strings.Contains(err.Error(), "line table"), "line table truncation: %v", err)
		// cSeg != cSegLog.
		_, err = ParseBytes(patch(0x42, func(b []byte) { binary.LittleEndian.PutUint16(b, 1) }))
		c.Check(err != nil && strings.Contains(err.Error(), "not supported"), "non-flat segmap: %v", err)
		// Non-flat descriptor flags.
		_, err = ParseBytes(patch(0x40, func(b []byte) { binary.LittleEndian.PutUint16(b, 0x108) }))
		c.Check(err != nil && strings.Contains(err.Error(), "not supported"), "segmented descriptor: %v", err)
		// Overlay module.
		_, err = ParseBytes(patch(0x90, func(b []byte) { binary.LittleEndian.PutUint16(b, 1) }))
		c.Check(err != nil && strings.Contains(err.Error(), "not supported"), "overlay module: %v", err)
		// Unsupported style.
		badStyle := patch(0x96, func(b []byte) { copy(b, "XX") })
		_, err = ParseBytes(badStyle)
		c.Check(err != nil && strings.Contains(err.Error(), "style"), "bad style: %v", err)
	})
}

func TestOMFAddressCoverage(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		dir := t.TempDir()
		// Save fixture constructors as on-disk samples using Go only.
		samplePath := map[string]string{}
		for _, sig := range []string{"NB09", "NB11"} {
			p := filepath.Join(dir, strings.ToLower(sig)+".pe")
			c.Check(os.WriteFile(p, fixtures.Embedded(sig), 0600) == nil, "write %s sample", sig)
			samplePath[sig] = p
		}

		// Build the example through os/exec so the analysis artifact and the
		// regression share the exact production ParseFile/SourceLines entry.
		bin := filepath.Join(dir, "codeview_lines")
		build := exec.Command("go", "build", "-o", bin, "./examples/codeview_lines")
		build.Env = os.Environ()
		if out, err := build.CombinedOutput(); err != nil {
			t.Fatalf("build example: %v\n%s", err, out)
		}

		queries := []string{"0x1044", "0x105a", "0x105b", "0x30c0", "0x1060"}
		type wantRow struct {
			rva                                          string
			module, name, file, line                     string
			start, end, sub, foff, loff, subPE, fPE, lPE string
			uncovered                                    bool
		}
		wants := map[string][]wantRow{
			"NB09": {
				{"0x1044", "7", "alpha.obj", "alpha.c", "7", "0x1044", "0x1050", "0xc0", "0xf0", "0x134", "0xdc0", "0xdf0", "0xe34", false},
				{"0x105a", "7", "alpha.obj", "alpha.c", "9", "0x1050", "0x105b", "0xc0", "0xf0", "0x138", "0xdc0", "0xdf0", "0xe38", false},
				{"0x105b", "", "", "", "", "", "", "", "", "", "", "", "", true},
				{"0x30c0", "9", "beta.obj", "beta.c", "41", "0x30c0", "0x30d0", "0x180", "0x1b0", "0x1f4", "0xe80", "0xeb0", "0xef4", false},
				{"0x1060", "", "", "", "", "", "", "", "", "", "", "", "", true},
			},
		}
		wants["NB11"] = wants["NB09"]

		for _, sig := range []string{"NB09", "NB11"} {
			args := append([]string{samplePath[sig]}, queries...)
			run := exec.Command(bin, args...)
			out, err := run.CombinedOutput()
			c.Check(err == nil, "%s example run: %v\n%s", sig, err, out)
			rows := strings.Split(strings.TrimRight(string(out), "\n"), "\n")
			c.Check(len(rows) == 1+len(queries), "%s: header plus one row per query, got %d", sig, len(rows))
			c.Check(strings.HasPrefix(rows[0], "sample\tsignature\trva\tmodule\t"), "%s: TSV header", sig)

			for i, w := range wants[sig] {
				cols := strings.Split(rows[i+1], "\t")
				c.Check(len(cols) == 15, "%s row %d: 15 TSV columns got %d", sig, i, len(cols))
				c.Check(cols[0] == samplePath[sig], "%s row %d: sample path", sig, i)
				c.Check(cols[1] == sig, "%s row %d: signature", sig, i)
				c.Check(cols[2] == w.rva, "%s row %d: rva echo", sig, i)
				if w.uncovered {
					c.Check(cols[3] == "UNCOVERED", "%s row %d: UNCOVERED marker", sig, i)
					c.Check(strings.Join(cols[4:], "") == "", "%s row %d: uncovered row has empty fields", sig, i)
					continue
				}
				got := wantRow{
					rva: cols[2], module: cols[3], name: cols[4], file: cols[5], line: cols[6],
					start: cols[7], end: cols[8], sub: cols[9], foff: cols[10], loff: cols[11],
					subPE: cols[12], fPE: cols[13], lPE: cols[14],
				}
				c.Check(reflect.DeepEqual(got, w), "%s row %d: TSV fields\ngot  %+v\nwant %+v", sig, i, got, w)
			}

			// Cross-check the same bytes through the independent expectation
			// builder for every covered RVA emitted by the example.
			raw, err := os.ReadFile(samplePath[sig])
			c.Check(err == nil, "%s readback: %v", sig, err)
			f, err := ParseBytes(raw)
			c.Check(err == nil, "%s parse readback: %v", sig, err)
			want := independentExpectations(t, payloadOf(t, raw), f.SectHdrs)
			for _, q := range []uint32{0x1044, 0x105a, 0x30c0} {
				linesEqual(c, f.DbgData[0].(*DebugCodeView).SourceLines(q), want[q], sig+" exec cross-check")
			}
			c.Check(len(want[0x105b]) == 0 && len(want[0x1060]) == 0,
				"%s: independent builder also marks queries uncovered", sig)

			// The example must not treat RVA as file coordinates: PE offsets
			// equal payload base 0xd00 plus payload-relative offsets.
			run = exec.Command(bin, samplePath[sig], "0x1044")
			out, err = run.Output()
			c.Check(err == nil, "%s single query: %v", sig, err)
			dataLine := strings.SplitN(strings.TrimRight(string(out), "\n"), "\n", 2)[1]
			cols := strings.Split(dataLine, "\t")
			c.Check(cols[12] == "0xdc0" && cols[13] == "0xdf0" && cols[14] == "0xe34",
				"%s: PE file offsets translated from payload base 0xd00", sig)
		}

		// Byte-for-byte input stability around the whole ParseFile workflow.
		for _, sig := range []string{"NB09", "NB11"} {
			raw, err := os.ReadFile(samplePath[sig])
			c.Check(err == nil, "%s sample read: %v", sig, err)
			before := append([]byte(nil), raw...)
			f, err := ParseFile(samplePath[sig])
			c.Check(err == nil, "%s ParseFile: %v", sig, err)
			for _, rva := range []uint32{0x1044, 0x105b, 0x30c0, 0x1060, 0x2050} {
				_ = f.DbgData[0].(*DebugCodeView).SourceLines(rva)
			}
			c.Check(bytes.Equal(raw, before), "%s: on-disk sample unchanged", sig)
		}
	})
}
