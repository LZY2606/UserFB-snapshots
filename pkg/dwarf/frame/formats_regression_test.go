package frame

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"strings"
	"testing"

	"github.com/go-delve/delve/pkg/dwarf"
)

// This file contains regression tests for parsing .debug_frame sections
// that mix the 32-bit and 64-bit DWARF formats, use either byte order and
// declare their own address size (CIE version 4). All byte streams are
// assembled by hand and all expected CFA/register rules are recorded
// manually, independently of the parser's own output.

// frameRec assembles a single .debug_frame record: the length prefix (a
// 4-byte length for DWARF32, or 0xffffffff followed by an 8-byte length for
// DWARF64) followed by body, which must already contain the CIE id or CIE
// pointer.
func frameRec(order binary.ByteOrder, dwarf64 bool, body []byte) []byte {
	var buf bytes.Buffer
	if dwarf64 {
		buf.Write([]byte{0xff, 0xff, 0xff, 0xff})
		binary.Write(&buf, order, uint64(len(body)))
	} else {
		binary.Write(&buf, order, uint32(len(body)))
	}
	buf.Write(body)
	return buf.Bytes()
}

// debugCIEBody assembles the body of a .debug_frame CIE record with an
// empty augmentation string, code alignment factor 1, data alignment
// factor -8 and return address register 16. addrSize is only written for
// version 4 CIEs.
func debugCIEBody(order binary.ByteOrder, dwarf64 bool, version, addrSize uint8, init []byte) []byte {
	var b bytes.Buffer
	if dwarf64 {
		binary.Write(&b, order, uint64(0xffffffffffffffff))
	} else {
		binary.Write(&b, order, uint32(0xffffffff))
	}
	b.WriteByte(version)
	b.WriteByte(0) // empty augmentation
	if version == 4 {
		b.WriteByte(addrSize)
		b.WriteByte(0) // segment_selector_size
	}
	b.WriteByte(1)    // code alignment factor (ULEB128)
	b.WriteByte(0x78) // data alignment factor -8 (SLEB128)
	b.WriteByte(16)   // return address register (1 byte in v1, ULEB128 otherwise; 16 encodes the same)
	b.Write(init)
	return b.Bytes()
}

// debugFDEBody assembles the body of a .debug_frame FDE record. cieOffset
// is the section offset of the CIE record, written as 4 or 8 bytes
// depending on the record format; begin and size are addrSize bytes wide.
func debugFDEBody(order binary.ByteOrder, dwarf64 bool, cieOffset, begin, size uint64, addrSize int, cfi []byte) []byte {
	var b bytes.Buffer
	if dwarf64 {
		binary.Write(&b, order, cieOffset)
	} else {
		binary.Write(&b, order, uint32(cieOffset))
	}
	dwarf.WriteUint(&b, order, addrSize, begin)
	dwarf.WriteUint(&b, order, addrSize, size)
	b.Write(cfi)
	return b.Bytes()
}

// setLocInsn assembles a DW_CFA_set_loc instruction with an addrSize-wide
// operand.
func setLocInsn(order binary.ByteOrder, addrSize int, addr uint64) []byte {
	var b bytes.Buffer
	b.WriteByte(0x01) // DW_CFA_set_loc
	dwarf.WriteUint(&b, order, addrSize, addr)
	return b.Bytes()
}

func concat(parts ...[]byte) []byte {
	return bytes.Join(parts, nil)
}

// wantFrame records the manually computed frame rules expected at a PC.
type wantFrame struct {
	cfaReg uint64
	cfaOff int64
	regs   map[uint64]DWRule
}

func checkPC(t *testing.T, fdes FrameDescriptionEntries, pc uint64, want wantFrame) {
	t.Helper()
	fde, err := fdes.FDEForPC(pc)
	if err != nil {
		t.Fatalf("FDEForPC(%#x): %v", pc, err)
	}
	fctx, err := fde.EstablishFrame(pc)
	if err != nil {
		t.Fatalf("EstablishFrame(%#x): %v", pc, err)
	}
	if fctx.CFA.Rule != RuleCFA || fctx.CFA.Reg != want.cfaReg || fctx.CFA.Offset != want.cfaOff {
		t.Errorf("pc %#x: CFA = {rule:%d reg:%d off:%d}, want {rule:%d reg:%d off:%d}",
			pc, fctx.CFA.Rule, fctx.CFA.Reg, fctx.CFA.Offset, RuleCFA, want.cfaReg, want.cfaOff)
	}
	for reg, wr := range want.regs {
		if got := fctx.Regs[reg]; got.Rule != wr.Rule || got.Offset != wr.Offset || got.Reg != wr.Reg {
			t.Errorf("pc %#x: reg %d = %+v, want %+v", pc, reg, got, wr)
		}
	}
	if fctx.RetAddrReg != 16 {
		t.Errorf("pc %#x: return address register = %d, want 16", pc, fctx.RetAddrReg)
	}
}

func checkNoFDE(t *testing.T, fdes FrameDescriptionEntries, pc uint64) {
	t.Helper()
	if fde, err := fdes.FDEForPC(pc); err == nil {
		t.Errorf("FDEForPC(%#x) = begin %#x, want ErrNoFDEForPC", pc, fde.Begin())
	}
}

func TestFrameFormats(t *testing.T) {
	// def_cfa r7, 8 as CIE initial instructions.
	initCFA := []byte{0x0c, 0x07, 0x08}
	// r6 saved at cfa-16 (DW_CFA_offset r6, 2 with data alignment -8).
	r6at16 := map[uint64]DWRule{6: {Rule: RuleOffset, Offset: -16}}

	t.Run("debug32_be", func(t *testing.T) {
		order := binary.BigEndian
		data := concat(
			frameRec(order, false, debugCIEBody(order, false, 3, 0, initCFA)),
			frameRec(order, false, debugFDEBody(order, false, 0, 0x1000, 0x20, 8, []byte{
				0x86, 0x02, // DW_CFA_offset r6, 2
				0x02, 0x04, // DW_CFA_advance_loc1 4
				0x0e, 0x10, // DW_CFA_def_cfa_offset 16
			})),
		)
		fdes, err := Parse(data, order, 0, 8, 0)
		if err != nil {
			t.Fatalf("Parse: %v", err)
		}
		if len(fdes) != 1 {
			t.Fatalf("got %d FDEs, want 1", len(fdes))
		}
		if fdes[0].Begin() != 0x1000 || fdes[0].End() != 0x1020 {
			t.Fatalf("range = %#x..%#x, want 0x1000..0x1020", fdes[0].Begin(), fdes[0].End())
		}
		checkPC(t, fdes, 0x1000, wantFrame{7, 8, r6at16})
		checkPC(t, fdes, 0x1003, wantFrame{7, 8, r6at16})
		checkPC(t, fdes, 0x1004, wantFrame{7, 16, r6at16})
		checkPC(t, fdes, 0x101f, wantFrame{7, 16, r6at16})
		checkNoFDE(t, fdes, 0xfff)
		checkNoFDE(t, fdes, 0x1020)
	})

	t.Run("debug64_le", func(t *testing.T) {
		order := binary.LittleEndian
		data := concat(
			frameRec(order, true, debugCIEBody(order, true, 3, 0, initCFA)),
			frameRec(order, true, debugFDEBody(order, true, 0, 0x2000, 0x10, 8, []byte{
				0x41,       // DW_CFA_advance_loc 1
				0x0e, 0x18, // DW_CFA_def_cfa_offset 24
			})),
		)
		fdes, err := Parse(data, order, 0, 8, 0)
		if err != nil {
			t.Fatalf("Parse: %v", err)
		}
		if len(fdes) != 1 {
			t.Fatalf("got %d FDEs, want 1", len(fdes))
		}
		if fdes[0].Begin() != 0x2000 || fdes[0].End() != 0x2010 {
			t.Fatalf("range = %#x..%#x, want 0x2000..0x2010", fdes[0].Begin(), fdes[0].End())
		}
		checkPC(t, fdes, 0x2000, wantFrame{7, 8, nil})
		checkPC(t, fdes, 0x2001, wantFrame{7, 24, nil})
		checkPC(t, fdes, 0x200f, wantFrame{7, 24, nil})
		checkNoFDE(t, fdes, 0x1fff)
		checkNoFDE(t, fdes, 0x2010)
	})

	t.Run("debug64_be", func(t *testing.T) {
		// 64-bit DWARF format records with a 4-byte address size: the
		// record format must not be confused with the address width.
		order := binary.BigEndian
		data := concat(
			frameRec(order, true, debugCIEBody(order, true, 3, 0, initCFA)),
			frameRec(order, true, debugFDEBody(order, true, 0, 0x3000, 0x10, 4, []byte{
				0x43,       // DW_CFA_advance_loc 3
				0x0e, 0x10, // DW_CFA_def_cfa_offset 16
			})),
		)
		fdes, err := Parse(data, order, 0, 4, 0)
		if err != nil {
			t.Fatalf("Parse: %v", err)
		}
		if len(fdes) != 1 {
			t.Fatalf("got %d FDEs, want 1", len(fdes))
		}
		if fdes[0].CIE.addrSize != 4 {
			t.Fatalf("CIE address size = %d, want 4", fdes[0].CIE.addrSize)
		}
		if fdes[0].Begin() != 0x3000 || fdes[0].End() != 0x3010 {
			t.Fatalf("range = %#x..%#x, want 0x3000..0x3010", fdes[0].Begin(), fdes[0].End())
		}
		checkPC(t, fdes, 0x3000, wantFrame{7, 8, nil})
		checkPC(t, fdes, 0x3002, wantFrame{7, 8, nil})
		checkPC(t, fdes, 0x3003, wantFrame{7, 16, nil})
		checkPC(t, fdes, 0x300f, wantFrame{7, 16, nil})
		checkNoFDE(t, fdes, 0x3010)
	})

	t.Run("mixed_cie", func(t *testing.T) {
		// Interleave both length formats and both address sizes: a
		// DWARF32 CIE with 4-byte addresses referenced by a DWARF64 FDE,
		// and a DWARF64 CIE with 8-byte addresses referenced by a
		// DWARF32 FDE.
		order := binary.LittleEndian
		cieA := frameRec(order, false, debugCIEBody(order, false, 4, 4, initCFA))
		fde1 := frameRec(order, true, debugFDEBody(order, true, 0, 0x1000, 0x10, 4, []byte{
			0x02, 0x02, // DW_CFA_advance_loc1 2
			0x0e, 0x10, // DW_CFA_def_cfa_offset 16
		}))
		cieBoff := uint64(len(cieA) + len(fde1))
		cieB := frameRec(order, true, debugCIEBody(order, true, 4, 8, []byte{0x0c, 0x06, 0x20})) // def_cfa r6, 32
		fde2 := frameRec(order, false, debugFDEBody(order, false, cieBoff, 0x2000, 0x10, 8, []byte{
			0x02, 0x04, // DW_CFA_advance_loc1 4
			0x0e, 0x30, // DW_CFA_def_cfa_offset 48
		}))
		fdes, err := Parse(concat(cieA, fde1, cieB, fde2), order, 0, 8, 0)
		if err != nil {
			t.Fatalf("Parse: %v", err)
		}
		if len(fdes) != 2 {
			t.Fatalf("got %d FDEs, want 2", len(fdes))
		}
		if fdes[0].CIE.addrSize != 4 || fdes[1].CIE.addrSize != 8 {
			t.Fatalf("CIE address sizes = %d, %d, want 4, 8", fdes[0].CIE.addrSize, fdes[1].CIE.addrSize)
		}
		if fdes[0].Begin() != 0x1000 || fdes[0].End() != 0x1010 {
			t.Fatalf("FDE1 range = %#x..%#x, want 0x1000..0x1010", fdes[0].Begin(), fdes[0].End())
		}
		if fdes[1].Begin() != 0x2000 || fdes[1].End() != 0x2010 {
			t.Fatalf("FDE2 range = %#x..%#x, want 0x2000..0x2010", fdes[1].Begin(), fdes[1].End())
		}
		checkPC(t, fdes, 0x1000, wantFrame{7, 8, nil})
		checkPC(t, fdes, 0x1001, wantFrame{7, 8, nil})
		checkPC(t, fdes, 0x1002, wantFrame{7, 16, nil})
		checkPC(t, fdes, 0x100f, wantFrame{7, 16, nil})
		checkNoFDE(t, fdes, 0x1010)
		checkPC(t, fdes, 0x2000, wantFrame{6, 32, nil})
		checkPC(t, fdes, 0x2003, wantFrame{6, 32, nil})
		checkPC(t, fdes, 0x2004, wantFrame{6, 48, nil})
		checkPC(t, fdes, 0x200f, wantFrame{6, 48, nil})
		checkNoFDE(t, fdes, 0x2010)
	})

	t.Run("address_width", func(t *testing.T) {
		// Version 4 CIEs declaring 4-byte and 8-byte address sizes; the
		// declared size drives the FDE begin/size fields and the
		// DW_CFA_set_loc operand width.
		order := binary.BigEndian
		cie4 := frameRec(order, false, debugCIEBody(order, false, 4, 4, initCFA))
		fdeA := frameRec(order, false, debugFDEBody(order, false, 0, 0x4000, 0x20, 4, concat(
			[]byte{
				0x86, 0x02, // DW_CFA_offset r6, 2
				0x02, 0x08, // DW_CFA_advance_loc1 8
				0x0e, 0x10, // DW_CFA_def_cfa_offset 16
			},
			setLocInsn(order, 4, 0x4018),
			[]byte{0x0e, 0x20}, // DW_CFA_def_cfa_offset 32
		)))
		cie8off := uint64(len(cie4) + len(fdeA))
		cie8 := frameRec(order, false, debugCIEBody(order, false, 4, 8, initCFA))
		fdeB := frameRec(order, false, debugFDEBody(order, false, cie8off, 0x5000, 0x20, 8, concat(
			[]byte{
				0x02, 0x08, // DW_CFA_advance_loc1 8
				0x0e, 0x10, // DW_CFA_def_cfa_offset 16
			},
			setLocInsn(order, 8, 0x5018),
			[]byte{0x0e, 0x20}, // DW_CFA_def_cfa_offset 32
		)))
		fdes, err := Parse(concat(cie4, fdeA, cie8, fdeB), order, 0, 8, 0)
		if err != nil {
			t.Fatalf("Parse: %v", err)
		}
		if len(fdes) != 2 {
			t.Fatalf("got %d FDEs, want 2", len(fdes))
		}
		if fdes[0].CIE.addrSize != 4 || fdes[1].CIE.addrSize != 8 {
			t.Fatalf("CIE address sizes = %d, %d, want 4, 8", fdes[0].CIE.addrSize, fdes[1].CIE.addrSize)
		}
		if fdes[0].Begin() != 0x4000 || fdes[0].End() != 0x4020 {
			t.Fatalf("FDE A range = %#x..%#x, want 0x4000..0x4020", fdes[0].Begin(), fdes[0].End())
		}
		if fdes[1].Begin() != 0x5000 || fdes[1].End() != 0x5020 {
			t.Fatalf("FDE B range = %#x..%#x, want 0x5000..0x5020", fdes[1].Begin(), fdes[1].End())
		}
		checkPC(t, fdes, 0x4000, wantFrame{7, 8, r6at16})
		checkPC(t, fdes, 0x4007, wantFrame{7, 8, r6at16})
		checkPC(t, fdes, 0x4008, wantFrame{7, 16, r6at16})
		checkPC(t, fdes, 0x4017, wantFrame{7, 16, r6at16})
		checkPC(t, fdes, 0x4018, wantFrame{7, 32, r6at16})
		checkPC(t, fdes, 0x401f, wantFrame{7, 32, r6at16})
		checkNoFDE(t, fdes, 0x4020)
		checkPC(t, fdes, 0x5000, wantFrame{7, 8, nil})
		checkPC(t, fdes, 0x5008, wantFrame{7, 16, nil})
		checkPC(t, fdes, 0x5017, wantFrame{7, 16, nil})
		checkPC(t, fdes, 0x5018, wantFrame{7, 32, nil})
		checkPC(t, fdes, 0x501f, wantFrame{7, 32, nil})
		checkNoFDE(t, fdes, 0x5020)
	})

	t.Run("eh_compat", func(t *testing.T) {
		// Little-endian .eh_frame with a "zR" augmentation: the FDE
		// begin is a PC-relative signed 4-byte value and the range is a
		// signed 4-byte value.
		order := binary.LittleEndian
		const ehFrameAddr = uint64(0x400000)

		var cie bytes.Buffer
		binary.Write(&cie, order, uint32(0)) // CIE id
		cie.WriteByte(1)                     // version
		cie.WriteString("zR")
		cie.WriteByte(0)
		cie.WriteByte(1)    // code alignment factor
		cie.WriteByte(0x78) // data alignment factor -8
		cie.WriteByte(16)   // return address register (single byte in v1)
		cie.WriteByte(1)    // augmentation data length
		cie.WriteByte(0x1b) // 'R': ptrEncPCRel | ptrEncSdata4
		cie.Write(initCFA)
		cieRec := frameRec(order, false, cie.Bytes())

		const begin = uint64(0x3fff00)
		fdeOff := uint64(len(cieRec))
		// The begin field is mapped at ehFrameAddr+fdeOff+8 in memory;
		// encode the PC-relative displacement to 'begin' as sdata4.
		encBegin := int32(begin - (ehFrameAddr + fdeOff + 8))
		var fde bytes.Buffer
		binary.Write(&fde, order, uint32(fdeOff+4)) // CIE pointer: backward offset to the CIE
		binary.Write(&fde, order, encBegin)
		binary.Write(&fde, order, int32(0x30)) // range, sdata4
		fde.WriteByte(0)                       // augmentation data length
		fde.Write([]byte{
			0x41,       // DW_CFA_advance_loc 1
			0x0e, 0x10, // DW_CFA_def_cfa_offset 16
		})

		fdes, err := Parse(concat(cieRec, frameRec(order, false, fde.Bytes())), order, 0, 8, ehFrameAddr)
		if err != nil {
			t.Fatalf("Parse: %v", err)
		}
		if len(fdes) != 1 {
			t.Fatalf("got %d FDEs, want 1", len(fdes))
		}
		if fdes[0].CIE.Augmentation != "zR" {
			t.Fatalf("augmentation = %q, want %q", fdes[0].CIE.Augmentation, "zR")
		}
		if fdes[0].Begin() != begin || fdes[0].End() != begin+0x30 {
			t.Fatalf("range = %#x..%#x, want %#x..%#x", fdes[0].Begin(), fdes[0].End(), begin, begin+0x30)
		}
		checkPC(t, fdes, begin, wantFrame{7, 8, nil})
		checkPC(t, fdes, begin+1, wantFrame{7, 16, nil})
		checkPC(t, fdes, begin+0x2f, wantFrame{7, 16, nil})
		checkNoFDE(t, fdes, begin+0x30)
	})

	t.Run("malformed", func(t *testing.T) {
		order := binary.LittleEndian
		cie := frameRec(order, false, debugCIEBody(order, false, 3, 0, initCFA))
		fde := frameRec(order, false, debugFDEBody(order, false, 0, 0x1000, 0x10, 8, []byte{0x41}))
		valid := concat(cie, fde)

		// An FDE whose CIE pointer references the first FDE record
		// instead of the CIE.
		fdeBadRef := frameRec(order, false, debugFDEBody(order, false, uint64(len(cie)), 0x2000, 0x10, 8, []byte{0x41}))
		// An FDE whose CIE pointer references the interior of the CIE
		// record.
		fdeInteriorRef := frameRec(order, false, debugFDEBody(order, false, 2, 0x2000, 0x10, 8, []byte{0x41}))

		cases := []struct {
			name        string
			data        []byte
			ehFrameAddr uint64
			wantOff     int
		}{
			{"truncated-length", []byte{0x10, 0x00}, 0, 0},
			{"truncated-64bit-length", []byte{0xff, 0xff, 0xff, 0xff, 0x08, 0x00}, 0, 0},
			{"length-past-end", concat([]byte{0x40, 0, 0, 0}, []byte{0, 0, 0, 0}), 0, 0},
			{"eh-frame-64bit", frameRec(order, true, debugCIEBody(order, true, 3, 0, initCFA)), 0x400000, 0},
			{"cie-ref-to-fde", concat(cie, fde, fdeBadRef), 0, len(valid)},
			{"cie-ref-interior", concat(cie, fdeInteriorRef), 0, len(cie)},
			{"truncated-v4-header", frameRec(order, false, []byte{0xff, 0xff, 0xff, 0xff, 4, 0}), 0, 0},
			{"truncated-v4-operands", frameRec(order, false, []byte{0xff, 0xff, 0xff, 0xff, 4, 0, 8, 0}), 0, 0},
			{"truncated-leb128-operand", frameRec(order, false, []byte{0xff, 0xff, 0xff, 0xff, 3, 0, 0x80}), 0, 0},
			{"segment-selector", frameRec(order, false, []byte{0xff, 0xff, 0xff, 0xff, 4, 0, 8, 1, 1, 0x78, 16}), 0, 0},
			{"bad-address-size", frameRec(order, false, []byte{0xff, 0xff, 0xff, 0xff, 4, 0, 2, 0, 1, 0x78, 16}), 0, 0},
			{"valid-then-truncated", concat(valid, []byte{0x10, 0x00}), 0, len(valid)},
		}
		for _, tc := range cases {
			fdes, err := Parse(tc.data, order, 0, 8, tc.ehFrameAddr)
			if err == nil {
				t.Errorf("%s: expected error, got %d FDEs", tc.name, len(fdes))
				continue
			}
			if fdes != nil {
				t.Errorf("%s: got %d FDEs alongside error %v, want none", tc.name, len(fdes), err)
			}
			if wantOff := fmt.Sprintf("%#x", tc.wantOff); !strings.Contains(err.Error(), wantOff) {
				t.Errorf("%s: error %q does not mention record offset %s", tc.name, err, wantOff)
			}
		}

		// After the malformed inputs a valid section must still parse.
		fdes, err := Parse(valid, order, 0, 8, 0)
		if err != nil {
			t.Fatalf("Parse of valid input after malformed inputs: %v", err)
		}
		if len(fdes) != 1 || fdes[0].Begin() != 0x1000 || fdes[0].End() != 0x1010 {
			t.Fatalf("got %d FDEs, first range %#x..%#x", len(fdes), fdes[0].Begin(), fdes[0].End())
		}
		checkPC(t, fdes, 0x1000, wantFrame{7, 8, nil})
	})
}

// ExampleParse parses a hand-assembled .debug_frame section and looks up
// the frame rules for a program counter.
func ExampleParse() {
	order := binary.LittleEndian
	data := concat(
		// CIE: version 3, code alignment 1, data alignment -8, return
		// address register 16, initial instructions def_cfa r7, 8.
		frameRec(order, false, debugCIEBody(order, false, 3, 0, []byte{0x0c, 0x07, 0x08})),
		// FDE covering 0x1000..0x1020: advance one byte, then cfa = r7+16.
		frameRec(order, false, debugFDEBody(order, false, 0, 0x1000, 0x20, 8, []byte{0x41, 0x0e, 0x10})),
	)
	fdes, err := Parse(data, order, 0, 8, 0)
	if err != nil {
		panic(err)
	}
	fde, err := fdes.FDEForPC(0x1001)
	if err != nil {
		panic(err)
	}
	fctx, err := fde.EstablishFrame(0x1001)
	if err != nil {
		panic(err)
	}
	fmt.Printf("begin=%#x end=%#x cfa=r%d+%d\n", fde.Begin(), fde.End(), fctx.CFA.Reg, fctx.CFA.Offset)
	// Output: begin=0x1000 end=0x1020 cfa=r7+16
}
