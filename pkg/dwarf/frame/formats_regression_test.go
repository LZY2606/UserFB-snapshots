package frame

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"reflect"
	"strings"
	"testing"

	"github.com/go-delve/delve/pkg/dwarf/leb128"
)

// The tests in this file build small .debug_frame / .eh_frame images by hand
// (following DWARF sections 6.4 and 7.24) and check the resulting frame rules
// at every PC covered by an FDE. Expected values are written out explicitly,
// never produced by another call to Parse.

// ---------------------------------------------------------------------------
// Small encoders and image builders.

func cat(parts ...[]byte) []byte {
	var out []byte
	for _, p := range parts {
		out = append(out, p...)
	}
	return out
}

func uleb(x uint64) []byte {
	var b bytes.Buffer
	leb128.EncodeUnsigned(&b, x)
	return b.Bytes()
}

func sleb(x int64) []byte {
	var b bytes.Buffer
	leb128.EncodeSigned(&b, x)
	return b.Bytes()
}

func fixed(b *bytes.Buffer, order binary.ByteOrder, size int, v uint64) {
	switch size {
	case 2:
		binary.Write(b, order, uint16(v))
	case 4:
		binary.Write(b, order, uint32(v))
	case 8:
		binary.Write(b, order, v)
	default:
		panic("bad fixed width")
	}
}

// testCIE describes the body of a .debug_frame CIE (everything after the
// length and CIE id fields).
type testCIE struct {
	version             uint8
	augmentation        string
	addressSize         uint8 // version >= 4 only
	segmentSelectorSize uint8 // version >= 4 only
	codeAlign           uint64
	dataAlign           int64
	retAddrReg          uint64
	initial             []byte
}

func (c testCIE) body() []byte {
	var b bytes.Buffer
	b.WriteByte(c.version)
	b.WriteString(c.augmentation)
	b.WriteByte(0)
	if c.version >= 4 {
		b.WriteByte(c.addressSize)
		b.WriteByte(c.segmentSelectorSize)
	}
	b.Write(uleb(c.codeAlign))
	b.Write(sleb(c.dataAlign))
	if c.version == 1 {
		b.WriteByte(byte(c.retAddrReg))
	} else {
		b.Write(uleb(c.retAddrReg))
	}
	b.Write(c.initial)
	return b.Bytes()
}

// frameImage accumulates a section of records.
type frameImage struct {
	order   binary.ByteOrder
	buf     []byte
	started bool
}

func (im *frameImage) writeHeader(dwarf64 bool, bodyLen int) {
	if dwarf64 {
		im.buf = append(im.buf, 0xff, 0xff, 0xff, 0xff)
		var l [8]byte
		im.order.PutUint64(l[:], uint64(bodyLen))
		im.buf = append(im.buf, l[:]...)
	} else {
		var l [4]byte
		im.order.PutUint32(l[:], uint32(bodyLen))
		im.buf = append(im.buf, l[:]...)
	}
}

func (im *frameImage) addRaw(dwarf64 bool, content []byte) int {
	start := len(im.buf)
	im.writeHeader(dwarf64, len(content))
	im.buf = append(im.buf, content...)
	return start
}

// addCIE appends a .debug_frame CIE and returns its section offset.
func (im *frameImage) addCIE(dwarf64 bool, c testCIE) int {
	var id []byte
	if dwarf64 {
		id = make([]byte, 8)
		im.order.PutUint64(id, ^uint64(0))
	} else {
		id = make([]byte, 4)
		im.order.PutUint32(id, ^uint32(0))
	}
	return im.addRaw(dwarf64, cat(id, c.body()))
}

func ciePointer(order binary.ByteOrder, dwarf64 bool, cieOffset int) []byte {
	if dwarf64 {
		var p [8]byte
		order.PutUint64(p[:], uint64(cieOffset))
		return p[:]
	}
	var p [4]byte
	order.PutUint32(p[:], uint32(cieOffset))
	return p[:]
}

// addFDE appends a .debug_frame FDE whose CIE is at cieOffset. begin and size
// are written with addrSize bytes, which is independent of the record format.
func (im *frameImage) addFDE(dwarf64 bool, cieOffset, addrSize int, begin, size uint64, instructions []byte) int {
	var body bytes.Buffer
	fixed(&body, im.order, addrSize, begin)
	fixed(&body, im.order, addrSize, size)
	body.Write(instructions)
	return im.addRaw(dwarf64, cat(ciePointer(im.order, dwarf64, cieOffset), body.Bytes()))
}

// addTerminator appends a 32-bit zero-length terminator.
func (im *frameImage) addTerminator() {
	im.buf = append(im.buf, 0, 0, 0, 0)
}

// addEhCIE appends a .eh_frame CIE with the given augmentation data.
func (im *frameImage) addEhCIE(c testCIE, augData []byte) int {
	var id [4]byte // .eh_frame CIE id is 0
	var body bytes.Buffer
	body.WriteByte(c.version)
	body.WriteString(c.augmentation)
	body.WriteByte(0)
	body.Write(uleb(c.codeAlign))
	body.Write(sleb(c.dataAlign))
	if c.version == 1 {
		body.WriteByte(byte(c.retAddrReg))
	} else {
		body.Write(uleb(c.retAddrReg))
	}
	if c.augmentation != "" {
		body.Write(uleb(uint64(len(augData))))
		body.Write(augData)
	}
	body.Write(c.initial)
	return im.addRaw(false, cat(id[:], body.Bytes()))
}

// addEhFDE appends a .eh_frame FDE encoded with zR/sdata4 pointers. The begin
// field is PC-relative to ehFrameAddr, the range is a signed 4-byte value.
func (im *frameImage) addEhFDE(cieOffset int, ehFrameAddr, begin uint64, size int32, instructions []byte) int {
	start := len(im.buf)
	// The begin field starts after the length (4) and CIE pointer (4) fields.
	fieldAddr := ehFrameAddr + uint64(start+8)
	beginVal := int32(int64(begin) - int64(fieldAddr))

	var body bytes.Buffer
	binary.Write(&body, im.order, beginVal)
	binary.Write(&body, im.order, size)
	body.Write(uleb(0)) // FDE augmentation data length
	body.Write(instructions)

	// CIE pointer is relative to the pointer field itself.
	ciePtr := uint32(start + 4 - cieOffset)
	var ptr [4]byte
	im.order.PutUint32(ptr[:], ciePtr)
	return im.addRaw(false, cat(ptr[:], body.Bytes()))
}

func (im *frameImage) bytes() []byte { return im.buf }

// ---------------------------------------------------------------------------
// Expectations.

type wantRules struct {
	begin, end uint64
	cfaReg     uint64
	cfaOff     int64
	regs       map[uint64]DWRule
}

func (w wantRules) String() string {
	return fmt.Sprintf("%#x-%#x cfa=r%d%+d regs=%v", w.begin, w.end, w.cfaReg, w.cfaOff, w.regs)
}

// checkEveryPC verifies the CFA and register rules at every PC of every
// expected range. Q is the FDE used to establish frames.
func checkEveryPC(t *testing.T, fde *FrameDescriptionEntry, wants []wantRules) {
	t.Helper()
	for _, w := range wants {
		for pc := w.begin; pc < w.end; pc++ {
			fc, err := fde.EstablishFrame(pc)
			if err != nil {
				t.Fatalf("EstablishFrame(%#x): %v", pc, err)
			}
			if fc.CFA.Rule != RuleCFA || fc.CFA.Reg != w.cfaReg || fc.CFA.Offset != w.cfaOff {
				t.Fatalf("pc %#x: CFA = {%v r%d %+d}, want {RuleCFA r%d %+d} (%v)",
					pc, fc.CFA.Rule, fc.CFA.Reg, fc.CFA.Offset, w.cfaReg, w.cfaOff, w)
			}
			got := make(map[uint64]DWRule, len(fc.Regs))
			for reg, rule := range fc.Regs {
				if rule.Rule == RuleUndefined {
					continue
				}
				got[reg] = rule
			}
			if !reflect.DeepEqual(got, w.regs) {
				t.Fatalf("pc %#x: regs = %v, want %v (%v)", pc, got, w.regs, w)
			}
		}
	}
}

func cfaRule(reg uint64, off int64) wantRules {
	return wantRules{cfaReg: reg, cfaOff: off}
}

func (w wantRules) with(reg uint64, off int64) wantRules {
	regs := make(map[uint64]DWRule, len(w.regs)+1)
	for k, v := range w.regs {
		regs[k] = v
	}
	regs[reg] = DWRule{Rule: RuleOffset, Offset: off}
	w.regs = regs
	return w
}

func rangeOf(begin, end uint64, w wantRules) wantRules {
	w.begin, w.end = begin, end
	if w.regs == nil {
		w.regs = map[uint64]DWRule{}
	}
	return w
}

// ---------------------------------------------------------------------------
// Tests.

func TestFrameFormats(t *testing.T) {
	t.Run("debug32_be", testDebug32BE)
	t.Run("debug64_le", testDebug64LE)
	t.Run("debug64_be", testDebug64BE)
	t.Run("mixed_cie", testMixedCIE)
	t.Run("address_width", testAddressWidth)
	t.Run("eh_compat", testEhCompat)
	t.Run("malformed", testMalformed)
}

// debug32_be: big-endian DWARF32 .debug_frame with a version 1 CIE (whose
// address width comes from the caller's ptrSize and whose return address
// register is a single byte). A zero-length terminator separates two FDEs.
func testDebug32BE(t *testing.T) {
	var order binary.ByteOrder = binary.BigEndian
	const ptrSize = 4

	im := &frameImage{order: order}
	cie := im.addCIE(false, testCIE{
		version:    1,
		codeAlign:  1,
		dataAlign:  -4,
		retAddrReg: 8,
		initial:    []byte{0x0c, 0x07, 0x08}, // DW_CFA_def_cfa r7, 8
	})

	// FDE 1: advance 0x10, offset r6 at cfa-8, advance 8, offset r5 at cfa-12.
	im.addFDE(false, cie, ptrSize, 0x1000, 0x20, []byte{
		0x50,       // advance_loc 0x10
		0x86, 0x02, // offset r6, 2 -> 2*-4
		0x48,       // advance_loc 8
		0x85, 0x03, // offset r5, 3 -> 3*-4
	})
	im.addTerminator()
	// FDE 2: def_cfa_register r6 applies from the start.
	im.addFDE(false, cie, ptrSize, 0x2000, 0x10, []byte{0x0d, 0x06})

	fdes, err := Parse(im.bytes(), order, 0, ptrSize, 0)
	if err != nil {
		t.Fatalf("Parse: %v", err)
	}
	if len(fdes) != 2 {
		t.Fatalf("got %d FDEs, want 2", len(fdes))
	}
	base := rangeOf(0x1000, 0x2000, cfaRule(7, 8))
	checkEveryPC(t, fdes[0], []wantRules{
		rangeOf(0x1000, 0x1010, base),
		rangeOf(0x1010, 0x1018, base.with(6, -8)),
		rangeOf(0x1018, 0x1020, base.with(6, -8).with(5, -12)),
	})
	checkEveryPC(t, fdes[1], []wantRules{
		rangeOf(0x2000, 0x2010, cfaRule(6, 8)),
	})
	if fdes[0].Begin() != 0x1000 || fdes[0].End() != 0x1020 {
		t.Fatalf("FDE 1 bounds = %#x-%#x", fdes[0].Begin(), fdes[0].End())
	}
	if fdes[0].CIE.ReturnAddressRegister != 8 {
		t.Fatalf("v1 return address register = %d, want 8", fdes[0].CIE.ReturnAddressRegister)
	}
	fde, err := fdes.FDEForPC(0x1010)
	if err != nil || fde != fdes[0] {
		t.Fatalf("FDEForPC(0x1010) = %v, %v", fde, err)
	}
}

// debug64_le: little-endian DWARF64 record with a version 4 CIE that declares
// 8-byte addresses. staticBase must be added exactly once (both to the FDE
// begin and to the set_loc operand).
func testDebug64LE(t *testing.T) {
	var order binary.ByteOrder = binary.LittleEndian
	const ptrSize = 8
	const staticBase = 0x4000

	im := &frameImage{order: order}
	cie := im.addCIE(true, testCIE{
		version:     4,
		addressSize: 8,
		codeAlign:   1,
		dataAlign:   -8,
		retAddrReg:  16,
		initial:     []byte{0x0c, 0x07, 0x10}, // def_cfa r7, 16
	})

	var instr bytes.Buffer
	instr.Write([]byte{0x03, 0x10, 0x00})             // advance_loc2 0x10
	instr.Write([]byte{0x83, 0x02})                   // offset r3, 2 -> -16
	instr.Write([]byte{0x04, 0x20, 0x00, 0x00, 0x00}) // advance_loc4 0x20
	instr.Write([]byte{0x84, 0x03})                   // offset r4, 3 -> -24
	instr.WriteByte(0x01)                             // set_loc
	fixed(&instr, order, 8, 0x1800)                   // staticBase is added to this once
	instr.Write([]byte{0x85, 0x01})                   // offset r5, 1 -> -8
	im.addFDE(true, cie, 8, 0x1000, 0x1000, instr.Bytes())

	fdes, err := Parse(im.bytes(), order, staticBase, ptrSize, 0)
	if err != nil {
		t.Fatalf("Parse: %v", err)
	}
	if len(fdes) != 1 {
		t.Fatalf("got %d FDEs, want 1", len(fdes))
	}
	fde := fdes[0]
	if fde.Begin() != 0x5000 || fde.End() != 0x6000 {
		t.Fatalf("FDE bounds = %#x-%#x, want 0x5000-0x6000", fde.Begin(), fde.End())
	}
	if !fde.IsDWARF64 || fde.CIE.AddressSize != 8 {
		t.Fatalf("FDE IsDWARF64=%v CIE address size=%d", fde.IsDWARF64, fde.CIE.AddressSize)
	}
	base := cfaRule(7, 16)
	checkEveryPC(t, fde, []wantRules{
		rangeOf(0x5000, 0x5010, base),
		rangeOf(0x5010, 0x5030, base.with(3, -16)),
		rangeOf(0x5030, 0x5800, base.with(3, -16).with(4, -24)),
		// set_loc 0x1800 + staticBase = 0x5800; staticBase is not added again
		// when the FDE begin was already relocated.
		rangeOf(0x5800, 0x6000, base.with(3, -16).with(4, -24).with(5, -8)),
	})
	if fde, _ := fdes.FDEForPC(0x5800); fde != fdes[0] {
		t.Fatalf("FDEForPC(0x5800) did not return the FDE")
	}
}

// debug64_be: big-endian DWARF64 record; every fixed-width field (length, CIE
// pointer, addresses, advance_loc2/4 deltas and set_loc) must be big-endian.
func testDebug64BE(t *testing.T) {
	var order binary.ByteOrder = binary.BigEndian
	const ptrSize = 8

	im := &frameImage{order: order}
	cie := im.addCIE(true, testCIE{
		version:     4,
		addressSize: 8,
		codeAlign:   1,
		dataAlign:   -8,
		retAddrReg:  16,
		initial:     []byte{0x0c, 0x07, 0x08}, // def_cfa r7, 8
	})

	var instr bytes.Buffer
	instr.Write([]byte{0x03, 0x00, 0x20})             // advance_loc2 0x20
	instr.Write([]byte{0x83, 0x02})                   // offset r3, 2 -> -16
	instr.Write([]byte{0x04, 0x00, 0x00, 0x00, 0x40}) // advance_loc4 0x40
	instr.WriteByte(0x01)                             // set_loc
	fixed(&instr, order, 8, 0x8070)
	instr.Write([]byte{0x84, 0x01}) // offset r4, 1 -> -8
	im.addFDE(true, cie, 8, 0x8000, 0x80, instr.Bytes())

	fdes, err := Parse(im.bytes(), order, 0, ptrSize, 0)
	if err != nil {
		t.Fatalf("Parse: %v", err)
	}
	if len(fdes) != 1 {
		t.Fatalf("got %d FDEs, want 1", len(fdes))
	}
	base := cfaRule(7, 8)
	checkEveryPC(t, fdes[0], []wantRules{
		rangeOf(0x8000, 0x8020, base),
		rangeOf(0x8020, 0x8060, base.with(3, -16)),
		rangeOf(0x8060, 0x8070, base.with(3, -16)),
		rangeOf(0x8070, 0x8080, base.with(3, -16).with(4, -8)),
	})
}

// mixed_cie interleaves both record formats and both address sizes. The first
// record is DWARF32, later records are DWARF64, then DWARF32 again. FDEs also
// reference CIEs of the other format, and the address width always follows the
// referenced CIE rather than the record format.
func testMixedCIE(t *testing.T) {
	var order binary.ByteOrder = binary.LittleEndian
	const ptrSize = 8

	im := &frameImage{order: order}
	cieA := im.addCIE(false, testCIE{ // DWARF32, 4-byte addresses
		version:     4,
		addressSize: 4,
		codeAlign:   1,
		dataAlign:   -4,
		retAddrReg:  8,
		initial:     []byte{0x0c, 0x07, 0x04}, // def_cfa r7, 4
	})
	// DWARF32 FDE referencing the DWARF32 CIE.
	im.addFDE(false, cieA, 4, 0x100, 0x20, []byte{0x83, 0x01}) // offset r3, 1 -> -4

	cieB := im.addCIE(true, testCIE{ // DWARF64, 8-byte addresses
		version:     4,
		addressSize: 8,
		codeAlign:   1,
		dataAlign:   -8,
		retAddrReg:  16,
		initial:     []byte{0x0c, 0x0c, 0x20}, // def_cfa r12, 32
	})
	// DWARF64 FDE referencing the DWARF64 CIE.
	im.addFDE(true, cieB, 8, 0x1000, 0x40, []byte{0x84, 0x01}) // offset r4, 1 -> -8

	// DWARF32 record referencing the DWARF64 CIE: 8-byte address fields.
	var b2 bytes.Buffer
	b2.WriteByte(0x01) // set_loc
	fixed(&b2, order, 8, 0x2020)
	b2.Write([]byte{0x85, 0x02}) // offset r5, 2 -> -16
	im.addFDE(false, cieB, 8, 0x2000, 0x40, b2.Bytes())

	// DWARF64 record referencing the DWARF32 CIE: 4-byte address fields.
	var a2 bytes.Buffer
	a2.WriteByte(0x01) // set_loc
	fixed(&a2, order, 4, 0x320)
	a2.Write([]byte{0x86, 0x02}) // offset r6, 2 -> -8
	im.addFDE(true, cieA, 4, 0x300, 0x40, a2.Bytes())

	fdes, err := Parse(im.bytes(), order, 0, ptrSize, 0)
	if err != nil {
		t.Fatalf("Parse: %v", err)
	}
	if len(fdes) != 4 {
		t.Fatalf("got %d FDEs, want 4", len(fdes))
	}
	if fdes[0].IsDWARF64 || !fdes[1].IsDWARF64 || fdes[2].IsDWARF64 || !fdes[3].IsDWARF64 {
		t.Fatalf("record formats = %v,%v,%v,%v", fdes[0].IsDWARF64, fdes[1].IsDWARF64, fdes[2].IsDWARF64, fdes[3].IsDWARF64)
	}
	if fdes[0].CIE != fdes[3].CIE || fdes[1].CIE != fdes[2].CIE {
		t.Fatalf("CIE references were not resolved to the right records")
	}
	baseA := cfaRule(7, 4)
	baseB := cfaRule(12, 32)
	checkEveryPC(t, fdes[0], []wantRules{
		rangeOf(0x100, 0x120, baseA.with(3, -4)),
	})
	checkEveryPC(t, fdes[1], []wantRules{
		rangeOf(0x1000, 0x1040, baseB.with(4, -8)),
	})
	checkEveryPC(t, fdes[2], []wantRules{
		rangeOf(0x2000, 0x2020, baseB),
		rangeOf(0x2020, 0x2040, baseB.with(5, -16)),
	})
	checkEveryPC(t, fdes[3], []wantRules{
		rangeOf(0x300, 0x320, baseA),
		rangeOf(0x320, 0x340, baseA.with(6, -8)),
	})
	if fdes[2].Begin() != 0x2000 || fdes[2].End() != 0x2040 || fdes[3].Begin() != 0x300 || fdes[3].End() != 0x340 {
		t.Fatalf("cross-format FDE bounds are wrong")
	}
}

// address_width checks that begin, size and set_loc widths follow the CIE's
// address size, and that advance_loc1/2/4 work in the section's byte order.
func testAddressWidth(t *testing.T) {
	var order binary.ByteOrder = binary.LittleEndian
	const ptrSize = 8

	im := &frameImage{order: order}
	cie4 := im.addCIE(false, testCIE{
		version:     4,
		addressSize: 4,
		codeAlign:   1,
		dataAlign:   -4,
		retAddrReg:  8,
		initial:     []byte{0x0c, 0x07, 0x08}, // def_cfa r7, 8
	})
	cie8 := im.addCIE(false, testCIE{
		version:     4,
		addressSize: 8,
		codeAlign:   1,
		dataAlign:   -8,
		retAddrReg:  16,
		initial:     []byte{0x0c, 0x07, 0x08},
	})

	var i4 bytes.Buffer
	i4.Write([]byte{0x02, 0x08})       // advance_loc1 8
	i4.Write([]byte{0x83, 0x01})       // offset r3, 1 -> -4
	i4.WriteByte(0x01)                 // set_loc
	fixed(&i4, order, 4, 0x1080)       // 4-byte operand
	i4.Write([]byte{0x84, 0x02})       // offset r4, 2 -> -8
	i4.Write([]byte{0x03, 0x10, 0x00}) // advance_loc2 0x10
	i4.Write([]byte{0x85, 0x01})       // offset r5, 1 -> -4
	im.addFDE(false, cie4, 4, 0x1000, 0x100, i4.Bytes())

	var i8 bytes.Buffer
	i8.Write([]byte{0x02, 0x10}) // advance_loc1 0x10
	i8.Write([]byte{0x86, 0x01}) // offset r6, 1 -> -8
	i8.WriteByte(0x01)           // set_loc
	fixed(&i8, order, 8, 0x2080) // 8-byte operand
	i8.Write([]byte{0x87, 0x02}) // offset r7, 2 -> -16
	im.addFDE(false, cie8, 8, 0x2000, 0x100, i8.Bytes())

	fdes, err := Parse(im.bytes(), order, 0, ptrSize, 0)
	if err != nil {
		t.Fatalf("Parse: %v", err)
	}
	if len(fdes) != 2 {
		t.Fatalf("got %d FDEs, want 2", len(fdes))
	}
	base := cfaRule(7, 8)
	checkEveryPC(t, fdes[0], []wantRules{
		rangeOf(0x1000, 0x1008, base),
		rangeOf(0x1008, 0x1080, base.with(3, -4)),
		rangeOf(0x1080, 0x1090, base.with(3, -4).with(4, -8)),
		rangeOf(0x1090, 0x1100, base.with(3, -4).with(4, -8).with(5, -4)),
	})
	checkEveryPC(t, fdes[1], []wantRules{
		rangeOf(0x2000, 0x2010, base),
		rangeOf(0x2010, 0x2080, base.with(6, -8)),
		rangeOf(0x2080, 0x2100, base.with(6, -8).with(7, -16)),
	})
	if fde, _ := fdes.FDEForPC(0x20ff); fde != fdes[1] {
		t.Fatalf("FDEForPC(0x20ff) did not find the 8-byte-address FDE")
	}
}

// eh_compat keeps the existing little-endian .eh_frame support working: zR
// augmentation, PC-relative begin pointers and a signed 4-byte range.
func testEhCompat(t *testing.T) {
	var order binary.ByteOrder = binary.LittleEndian
	const ptrSize = 8
	const ehFrameAddr = 0x400000

	im := &frameImage{order: order}
	cie := im.addEhCIE(testCIE{
		version:      1,
		augmentation: "zR",
		codeAlign:    1,
		dataAlign:    -8,
		retAddrReg:   16,
		initial:      []byte{0x0c, 0x07, 0x08}, // def_cfa r7, 8
	}, []byte{byte(ptrEncPCRel | ptrEncSdata4)})

	im.addEhFDE(cie, ehFrameAddr, 0x410000, 0x40, []byte{0x83, 0x01}) // offset r3, 1 -> -8

	fdes, err := Parse(im.bytes(), order, 0, ptrSize, ehFrameAddr)
	if err != nil {
		t.Fatalf("Parse: %v", err)
	}
	if len(fdes) != 1 {
		t.Fatalf("got %d FDEs, want 1", len(fdes))
	}
	fde := fdes[0]
	if fde.Begin() != 0x410000 || fde.End() != 0x410040 {
		t.Fatalf("FDE bounds = %#x-%#x, want 0x410000-0x410040", fde.Begin(), fde.End())
	}
	checkEveryPC(t, fde, []wantRules{
		rangeOf(0x410000, 0x410040, cfaRule(7, 8).with(3, -8)),
	})
	if got, _ := fdes.FDEForPC(0x410020); got != fde {
		t.Fatalf("FDEForPC(0x410020) did not return the .eh_frame FDE")
	}
}

// With returns a copy of im with one byte changed.
func mutated(im []byte, off int, b byte) []byte {
	out := append([]byte(nil), im...)
	out[off] = b
	return out
}

// validImage is a minimal, well-formed .debug_frame used to prove that
// malformed input doesn't poison later parsing.
func validImage() []byte {
	im := &frameImage{order: binary.LittleEndian}
	cie := im.addCIE(false, testCIE{version: 3, codeAlign: 1, dataAlign: -8, retAddrReg: 16, initial: []byte{0x0c, 0x07, 0x08}})
	im.addFDE(false, cie, 8, 0x1000, 0x20, []byte{0x83, 0x01})
	return im.bytes()
}

func checkValidAfterwards(t *testing.T) {
	t.Helper()
	fdes, err := Parse(validImage(), binary.LittleEndian, 0, 8, 0)
	if err != nil {
		t.Fatalf("valid input after malformed input: %v", err)
	}
	if len(fdes) != 1 {
		t.Fatalf("valid input after malformed input: got %d FDEs", len(fdes))
	}
	if _, err := fdes[0].EstablishFrame(0x1000); err != nil {
		t.Fatalf("valid input after malformed input: %v", err)
	}
}

// testMalformed checks that each kind of corruption returns an error (without
// panicking or hanging), never leaves a partially parsed FDE behind, and that
// a subsequent well-formed section still parses.
func testMalformed(t *testing.T) {
	le := binary.LittleEndian

	t.Run("truncated_extended_length", func(t *testing.T) {
		data := []byte{0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0}
		if fdes, err := Parse(data, le, 0, 8, 0); err == nil {
			t.Fatalf("expected error, got %d FDEs", len(fdes))
		} else if !strings.Contains(err.Error(), "at 0x0") {
			t.Fatalf("error does not mention the record offset: %v", err)
		}
		checkValidAfterwards(t)
	})

	t.Run("length_exceeds_section", func(t *testing.T) {
		var b bytes.Buffer
		binary.Write(&b, le, uint32(0x100))
		b.Write([]byte{0, 0})
		if fdes, err := Parse(b.Bytes(), le, 0, 8, 0); err == nil {
			t.Fatalf("expected error, got %d FDEs", len(fdes))
		}
		checkValidAfterwards(t)
	})

	t.Run("record_too_short_for_cie_id", func(t *testing.T) {
		var b bytes.Buffer
		binary.Write(&b, le, uint32(2))
		b.Write([]byte{0, 0})
		if fdes, err := Parse(b.Bytes(), le, 0, 8, 0); err == nil {
			t.Fatalf("expected error, got %d FDEs", len(fdes))
		}
		checkValidAfterwards(t)
	})

	// buildFDEWithPointer builds a CIE followed by an FDE whose CIE pointer is
	// the supplied absolute section offset.
	buildFDEWithPointer := func(ptr uint32, addrSize int, instr []byte) []byte {
		im := &frameImage{order: le}
		im.addCIE(false, testCIE{version: 3, codeAlign: 1, dataAlign: -8, retAddrReg: 16})
		var body bytes.Buffer
		fixed(&body, le, addrSize, 0x1000)
		fixed(&body, le, addrSize, 0x10)
		body.Write(instr)
		var p [4]byte
		le.PutUint32(p[:], ptr)
		im.addRaw(false, cat(p[:], body.Bytes()))
		return im.bytes()
	}

	t.Run("cie_pointer_to_fde", func(t *testing.T) {
		im := &frameImage{order: le}
		cie := im.addCIE(false, testCIE{version: 3, codeAlign: 1, dataAlign: -8, retAddrReg: 16})
		firstFDE := im.addFDE(false, cie, 8, 0x1000, 0x10, nil)
		// Point the next FDE at the first FDE record, not at a CIE.
		var body bytes.Buffer
		fixed(&body, le, 8, 0x2000)
		fixed(&body, le, 8, 0x10)
		im.addRaw(false, cat(ciePointer(le, false, firstFDE), body.Bytes()))

		fdes, err := Parse(im.bytes(), le, 0, 8, 0)
		if err == nil {
			t.Fatalf("expected error, got %d FDEs", len(fdes))
		}
		if !strings.Contains(err.Error(), fmt.Sprintf("%#x", firstFDE)) {
			t.Fatalf("error does not mention the invalid target: %v", err)
		}
		checkValidAfterwards(t)
	})

	t.Run("cie_pointer_inside_record", func(t *testing.T) {
		if _, err := Parse(buildFDEWithPointer(2, 8, nil), le, 0, 8, 0); err == nil {
			t.Fatal("expected error for a CIE pointer into the middle of a record")
		}
		checkValidAfterwards(t)
	})

	t.Run("cie_pointer_beyond_section", func(t *testing.T) {
		if _, err := Parse(buildFDEWithPointer(0x10000, 8, nil), le, 0, 8, 0); err == nil {
			t.Fatal("expected error for a CIE pointer beyond the section")
		}
		checkValidAfterwards(t)
	})

	// buildCIE builds a section containing a single CIE with the given body.
	buildCIE := func(body []byte) []byte {
		im := &frameImage{order: le}
		var id [4]byte
		le.PutUint32(id[:], ^uint32(0))
		im.addRaw(false, cat(id[:], body))
		return im.bytes()
	}

	t.Run("truncated_v4_header", func(t *testing.T) {
		// version 4, empty augmentation, but no address/segment size bytes.
		if _, err := Parse(buildCIE([]byte{4, 0}), le, 0, 8, 0); err == nil {
			t.Fatal("expected error for a truncated v4 header")
		}
		checkValidAfterwards(t)
	})

	t.Run("v4_bad_address_size", func(t *testing.T) {
		body := cat([]byte{4, 0, 3, 0}, uleb(1), sleb(-8), uleb(16))
		if _, err := Parse(buildCIE(body), le, 0, 8, 0); err == nil {
			t.Fatal("expected error for address size 3")
		}
		checkValidAfterwards(t)
	})

	t.Run("v4_segment_selector", func(t *testing.T) {
		body := cat([]byte{4, 0, 8, 1}, uleb(1), sleb(-8), uleb(16))
		if _, err := Parse(buildCIE(body), le, 0, 8, 0); err == nil {
			t.Fatal("expected error for a non-zero segment selector size")
		}
		checkValidAfterwards(t)
	})

	t.Run("truncated_cie_operand", func(t *testing.T) {
		// The CIE claims a version and augmentation but ends before the
		// alignment factors; the following valid record must not be used to
		// complete it.
		im := &frameImage{order: le}
		var id [4]byte
		le.PutUint32(id[:], ^uint32(0))
		im.addRaw(false, cat(id[:], []byte{3, 0}))
		im.buf = append(im.buf, validImage()...)
		if _, err := Parse(im.bytes(), le, 0, 8, 0); err == nil {
			t.Fatal("expected error for a CIE truncated before its operands")
		}
		checkValidAfterwards(t)
	})

	t.Run("truncated_fde_body", func(t *testing.T) {
		im := &frameImage{order: le}
		cie := im.addCIE(false, testCIE{version: 3, codeAlign: 1, dataAlign: -8, retAddrReg: 16})
		// An 8-byte-address FDE whose record ends before the size field.
		var p [4]byte
		le.PutUint32(p[:], uint32(cie))
		im.addRaw(false, cat(p[:], []byte{0, 0, 0, 0, 0, 0, 0, 0}))
		if _, err := Parse(im.bytes(), le, 0, 8, 0); err == nil {
			t.Fatal("expected error for a truncated FDE body")
		}
		checkValidAfterwards(t)
	})

	// Truncated CFI operands are detected at parse time, so an FDE with a
	// cut-off instruction stream is never returned.
	t.Run("truncated_operand_advance_loc2", func(t *testing.T) {
		im := &frameImage{order: le}
		cie := im.addCIE(false, testCIE{version: 3, codeAlign: 1, dataAlign: -8, retAddrReg: 16, initial: []byte{0x0c, 0x07, 0x08}})
		im.addFDE(false, cie, 8, 0x1000, 0x10, []byte{0x03, 0x10}) // advance_loc2 needs 2 operand bytes
		if fdes, err := Parse(im.bytes(), le, 0, 8, 0); err == nil {
			t.Fatalf("expected error for a truncated advance_loc2 operand, got %d FDEs", len(fdes))
		}
		checkValidAfterwards(t)
	})

	t.Run("truncated_operand_set_loc", func(t *testing.T) {
		im := &frameImage{order: le}
		cie := im.addCIE(false, testCIE{version: 4, addressSize: 8, codeAlign: 1, dataAlign: -8, retAddrReg: 16, initial: []byte{0x0c, 0x07, 0x08}})
		im.addFDE(false, cie, 8, 0x1000, 0x10, []byte{0x01, 0x00, 0x00}) // set_loc needs 8 operand bytes
		if fdes, err := Parse(im.bytes(), le, 0, 8, 0); err == nil {
			t.Fatalf("expected error for a truncated set_loc operand, got %d FDEs", len(fdes))
		}
		checkValidAfterwards(t)
	})

	t.Run("truncated_operand_in_cie", func(t *testing.T) {
		// The CIE's initial instructions end with a set_loc that is missing its
		// 4-byte operand (address size 4 from the v4 header).
		im := &frameImage{order: le}
		im.addCIE(false, testCIE{
			version: 4, addressSize: 4, codeAlign: 1, dataAlign: -8, retAddrReg: 16,
			initial: []byte{0x0c, 0x07, 0x08, 0x01, 0x00, 0x00},
		})
		if fdes, err := Parse(im.bytes(), le, 0, 8, 0); err == nil {
			t.Fatalf("expected error for a truncated CIE instruction, got %d FDEs", len(fdes))
		}
		checkValidAfterwards(t)
	})

	t.Run("truncated_operand_offset", func(t *testing.T) {
		im := &frameImage{order: le}
		cie := im.addCIE(false, testCIE{version: 3, codeAlign: 1, dataAlign: -8, retAddrReg: 16})
		im.addFDE(false, cie, 8, 0x1000, 0x10, []byte{0x83}) // DW_CFA_offset needs a ULEB128 offset
		if fdes, err := Parse(im.bytes(), le, 0, 8, 0); err == nil {
			t.Fatalf("expected error for a truncated DW_CFA_offset operand, got %d FDEs", len(fdes))
		}
		checkValidAfterwards(t)
	})

	// The .eh_frame path must reject truncation too (even though it stays
	// 32-bit only).
	t.Run("eh_frame_truncated_range", func(t *testing.T) {
		im := &frameImage{order: le}
		im.addEhCIE(testCIE{
			version: 1, augmentation: "zR", codeAlign: 1, dataAlign: -8, retAddrReg: 16,
		}, []byte{byte(ptrEncPCRel | ptrEncSdata4)})
		// A begin pointer but no range field: the FDE body is cut short.
		var body bytes.Buffer
		binary.Write(&body, le, int32(-8))
		var ptr [4]byte
		le.PutUint32(ptr[:], uint32(0))
		im.addRaw(false, cat(ptr[:], body.Bytes()))
		if fdes, err := Parse(im.bytes(), le, 0, 8, 0x400000); err == nil {
			t.Fatalf("expected error for a truncated .eh_frame range, got %d FDEs", len(fdes))
		}
	})

	// A corrupted length must not let one record consume the next.
	t.Run("length_borrows_next_record", func(t *testing.T) {
		// Truncated CIE whose declared length runs past its own body: the
		// parser must reject it rather than splicing in the following bytes.
		data := mutated(buildCIE([]byte{3, 0}), 0, 0xff)
		if _, err := Parse(data, le, 0, 8, 0); err == nil {
			t.Fatal("expected error for an over-long record length")
		}
		checkValidAfterwards(t)
	})
}
