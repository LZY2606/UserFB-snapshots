package op

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"strings"
	"testing"

	"github.com/go-delve/delve/pkg/dwarf/leb128"
	"github.com/go-delve/delve/task_support/taskcheck"
)

// ---- hand-written DWARF expression helpers ----

func uleb128(n uint64) []byte {
	var b bytes.Buffer
	leb128.EncodeUnsigned(&b, n)
	return b.Bytes()
}

func concat(parts ...[]byte) []byte {
	n := 0
	for _, p := range parts {
		n += len(p)
	}
	out := make([]byte, 0, n)
	for _, p := range parts {
		out = append(out, p...)
	}
	return out
}

// constAddr pushes addr through DW_OP_const4u.
func constAddr(addr uint32) []byte {
	return []byte{byte(DW_OP_const4u), byte(addr), byte(addr >> 8), byte(addr >> 16), byte(addr >> 24)}
}

// constAddr8 pushes addr through DW_OP_const8u.
func constAddr8(addr uint64) []byte {
	b := []byte{byte(DW_OP_const8u)}
	for i := 0; i < 8; i++ {
		b = append(b, byte(addr>>(8*i)))
	}
	return b
}

func bitPieceTerm(nbits, bitOffset uint64) []byte {
	return concat([]byte{byte(DW_OP_bit_piece)}, uleb128(nbits), uleb128(bitOffset))
}

func bytePieceTerm(size uint64) []byte {
	return concat([]byte{byte(DW_OP_piece)}, uleb128(size))
}

// regBitPiece is DW_OP_regN followed by a bit_piece terminator.
func regBitPiece(reg, nbits, bitOffset uint64) []byte {
	return concat([]byte{byte(DW_OP_reg0) + byte(reg)}, bitPieceTerm(nbits, bitOffset))
}

// memBitPiece is an address expression followed by a bit_piece terminator.
func memBitPiece(addr uint32, nbits, bitOffset uint64) []byte {
	return concat(constAddr(addr), bitPieceTerm(nbits, bitOffset))
}

func memBytePiece(addr uint32, size uint64) []byte {
	return concat(constAddr(addr), bytePieceTerm(size))
}

// ---- snapshot backends ----

type memBackend map[uint64]byte

func (m memBackend) read(buf []byte, addr uint64) (int, error) {
	for i := range buf {
		v, ok := m[addr+uint64(i)]
		if !ok {
			return i, fmt.Errorf("short read at %#x", addr+uint64(i))
		}
		buf[i] = v
	}
	return len(buf), nil
}

func (m memBackend) write(addr uint64, data []byte) (int, error) {
	for i, b := range data {
		m[addr+uint64(i)] = b
	}
	return len(data), nil
}

// countingWrite wraps a memory write callback counting calls and optionally
// failing / short-writing. When target is non-nil accepted bytes are stored
// there first.
type countingWrite struct {
	calls    int
	target   memBackend
	failAddr uint64
	failErr  error
	shortN   int // when > 0, replies with this many bytes instead of len(data)
}

func (cw *countingWrite) fn(addr uint64, data []byte) (int, error) {
	cw.calls++
	if cw.failErr != nil && addr == cw.failAddr {
		return 0, cw.failErr
	}
	if cw.shortN > 0 {
		n := cw.shortN
		if n > len(data) {
			n = len(data)
		}
		if cw.target != nil {
			cw.target.write(addr, data[:n])
		}
		return n, nil
	}
	if cw.target != nil {
		cw.target.write(addr, data)
	}
	return len(data), nil
}

func newTestRegs(regList ...*DwarfRegister) *DwarfRegisters {
	return NewDwarfRegisters(0, regList, binary.LittleEndian, 0, 0, 0, 0)
}

// applyChangeFunc returns a ChangeFunc that stores writes into applied and
// into the registers themselves, counting every invocation.
func applyChangeFunc(regs *DwarfRegisters, applied map[uint64]*DwarfRegister, calls *int) RegisterChangeFunc {
	return func(regNum uint64, reg *DwarfRegister) error {
		*calls++
		cp := DwarfRegisterFromBytes(append([]byte(nil), reg.Bytes...))
		applied[regNum] = cp
		regs.AddReg(regNum, cp)
		return nil
	}
}

func mustPlan(t *testing.T, c *taskcheck.C, regs DwarfRegisters, expr []byte, ptrSize int, bitSize uint64, data, mask []byte, rm ReadMemoryFunc) *LocationWritePlan {
	t.Helper()
	plan, err := PlanLocationWrite(regs, expr, ptrSize, bitSize, data, mask, rm)
	c.Check(err == nil, "PlanLocationWrite failed: %v", err)
	if err != nil {
		t.FailNow()
	}
	return plan
}

// TestLocationWriteAliases covers unaligned mixed sources and the physical
// alias rules: equal values collapse, conflicting values name both logical
// sources and reject the whole plan.
func TestLocationWriteAliases(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// --- unaligned mixed register / memory / vector sources ---
		// 16-bit object:
		//   piece 0: reg0 bits 0..3   <- logical bits 0..3
		//   piece 1: mem 0x1000 bits 4..7 <- logical bits 4..7
		//   piece 2: reg1 (16-byte vector) bits 3..10 <- logical bits 8..15
		regs := newTestRegs(
			DwarfRegisterFromBytes([]byte{0xA5}),
			DwarfRegisterFromBytes([]byte{0x00, 0xFF, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE}),
		)
		mem := memBackend{0x1000: 0x7E}
		expr := concat(
			regBitPiece(0, 4, 0),
			memBitPiece(0x1000, 4, 4),
			regBitPiece(1, 8, 3),
		)
		data := []byte{0x21, 0x3C}
		mask := []byte{0xFF, 0xFF}
		plan := mustPlan(t, c, *regs, expr, 8, 16, data, mask, mem.read)

		recs := plan.Records()
		c.Check(len(recs) == 3, "want 3 physical records, got %d", len(recs))

		// Record order: registers ascending, then memory.
		c.Check(recs[0].Kind == PhysicalRegister && recs[0].RegNum == 0, "record 0 must be register 0")
		c.Check(bytes.Equal(recs[0].Orig, []byte{0xA5}), "reg0 orig bytes = % x", recs[0].Orig)
		// low nibble becomes 0001, high nibble preserved: 1010_0001
		c.Check(bytes.Equal(recs[0].Data, []byte{0xA1}), "reg0 data bytes = % x", recs[0].Data)

		c.Check(recs[1].Kind == PhysicalRegister && recs[1].RegNum == 1, "record 1 must be register 1")
		wantReg1Orig := []byte{0x00, 0xFF, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE}
		wantReg1Data := []byte{0xE0, 0xF9, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE}
		c.Check(bytes.Equal(recs[1].Orig, wantReg1Orig), "reg1 orig = % x", recs[1].Orig)
		c.Check(bytes.Equal(recs[1].Data, wantReg1Data), "reg1 data = % x, want % x", recs[1].Data, wantReg1Data)

		c.Check(recs[2].Kind == PhysicalMemory && recs[2].Addr == 0x1000, "record 2 must be memory 0x1000")
		c.Check(bytes.Equal(recs[2].Orig, []byte{0x7E}), "mem orig = % x", recs[2].Orig)
		// bits 4..7 become 0010: 0010_1110
		c.Check(bytes.Equal(recs[2].Data, []byte{0x2E}), "mem data = % x", recs[2].Data)

		// --- two pieces writing the same register: both changes survive ---
		// 4-bit object, both pieces target reg0, disjoint bit ranges.
		regs2 := newTestRegs(DwarfRegisterFromBytes([]byte{0xFF}))
		expr2 := concat(
			regBitPiece(0, 2, 0), // logical 0..1 -> reg0 bits 0..1
			regBitPiece(0, 2, 4), // logical 2..3 -> reg0 bits 4..5
		)
		plan2 := mustPlan(t, c, *regs2, expr2, 8, 4, []byte{0x02}, []byte{0x0F}, nil)
		recs2 := plan2.Records()
		c.Check(len(recs2) == 1, "aliased register collapses into one record, got %d", len(recs2))
		// piece 0 clears bit 0, piece 1 clears bits 4..5: both changes
		// survive in a single full-width record (0xFF -> 0xCE).
		c.Check(bytes.Equal(recs2[0].Data, []byte{0xCE}), "aliased reg data = % x, want CE", recs2[0].Data)
		c.Check(recs2[0].Data[0]^recs2[0].Orig[0] == 0x31, "both piece modifications must be present, diff = %#x", recs2[0].Data[0]^recs2[0].Orig[0])

		// --- two different address expressions hitting overlapping bytes ---
		// 24-bit object: piece 0 covers 0x2000..0x2001, piece 1 covers
		// 0x2001 again. Equal requested values coalesce into one range.
		mem3 := memBackend{0x2000: 0x00, 0x2001: 0x00}
		expr3 := concat(
			memBytePiece(0x2000, 2),   // logical bits 0..15
			memBitPiece(0x2001, 8, 0), // logical bits 16..23, same physical byte
		)
		plan3 := mustPlan(t, c, DwarfRegisters{}, expr3, 8, 24,
			[]byte{0xAB, 0xCD, 0xCD}, []byte{0xFF, 0xFF, 0xFF}, mem3.read)
		recs3 := plan3.Records()
		c.Check(len(recs3) == 1 && recs3[0].Addr == 0x2000 && len(recs3[0].Data) == 2,
			"overlapping equal memory pieces coalesce, got %+v", recs3)

		// Conflicting values on the shared byte: whole plan rejected, and
		// the error names both logical sources.
		_, err := PlanLocationWrite(DwarfRegisters{}, expr3, 8, 24,
			[]byte{0xAB, 0xCD, 0xEF}, []byte{0xFF, 0xFF, 0xFF}, mem3.read)
		c.Check(err != nil, "conflicting memory alias must fail")
		if err != nil {
			msg := err.Error()
			// First mismatching bit is shared byte 0x2001 bit 1, which is
			// logical bit 9 (piece 0) and logical bit 17 (piece 1).
			c.Check(strings.Contains(msg, "logical bit 9") && strings.Contains(msg, "logical bit 17"),
				"conflict error must name both logical bits: %v", err)
			c.Check(strings.Contains(msg, "piece 0") && strings.Contains(msg, "piece 1"),
				"conflict error must name both pieces: %v", err)
		}

		// --- register alias conflict on the same physical bit ---
		expr4 := concat(
			regBitPiece(0, 1, 0), // logical bit 0 -> reg0 bit 0
			regBitPiece(0, 1, 0), // logical bit 1 -> reg0 bit 0 again
		)
		_, err = PlanLocationWrite(*newTestRegs(DwarfRegisterFromBytes([]byte{0x00})), expr4, 8, 2,
			[]byte{0x01}, []byte{0x03}, nil)
		c.Check(err != nil && strings.Contains(err.Error(), "register 0 bit 0"),
			"register alias conflict must be reported, got %v", err)

		// --- unselected logical bits get no constraint but still observe
		// the natural effect of a physical alias ---
		// Logical bits 0 and 1 both land on reg0 bit 0; only bit 0 is
		// selected and asks for 1. Planning succeeds and the single record
		// flips the shared bit, so bit 1's view changes as a side effect.
		regs5 := newTestRegs(DwarfRegisterFromBytes([]byte{0x00}))
		plan5 := mustPlan(t, c, *regs5, expr4, 8, 2, []byte{0x01}, []byte{0x01}, nil)
		recs5 := plan5.Records()
		c.Check(len(recs5) == 1 && recs5[0].Data[0] == 0x01, "unselected alias imposes no constraint: %+v", recs5)
		regs5.ChangeFunc = applyChangeFunc(regs5, map[uint64]*DwarfRegister{}, new(int))
		_, err = plan5.Commit(*regs5, nil, nil)
		c.Check(err == nil, "natural-alias plan commits: %v", err)
		c.Check(regs5.regs[0].Bytes[0] == 0x01, "both aliased views now read the written value")

		// --- memory ranges never extend across an unmodified byte ---
		// Two selected bytes with an unselected byte between them stay two
		// records even though their pieces are adjacent logically.
		memGap := memBackend{0xD000: 0x01, 0xD001: 0x02, 0xD002: 0x03}
		gapExpr := concat(
			memBitPiece(0xD000, 8, 0), // logical bits 0..7
			memBitPiece(0xD002, 8, 0), // logical bits 8..15
		)
		gapPlan := mustPlan(t, c, DwarfRegisters{}, gapExpr, 8, 16,
			[]byte{0xFF, 0xFF}, []byte{0xFF, 0xFF}, memGap.read)
		gapRecs := gapPlan.Records()
		c.Check(len(gapRecs) == 2, "gap splits memory into two records, got %d", len(gapRecs))
		if len(gapRecs) == 2 {
			c.Check(gapRecs[0].Addr == 0xD000 && len(gapRecs[0].Data) == 1, "first range does not expand: %+v", gapRecs[0])
			c.Check(gapRecs[1].Addr == 0xD002 && len(gapRecs[1].Data) == 1, "second range does not expand: %+v", gapRecs[1])
		}
	})
}

// TestLocationWritePreflight checks snapshot verification and commit
// accounting, including zero-write rejection when the last record drifts.
func TestLocationWritePreflight(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// --- happy path: preflight passes, records commit in order ---
		regs := newTestRegs(DwarfRegisterFromBytes([]byte{0xA5}))
		mem := memBackend{0x5000: 0x7E, 0x5001: 0x12}
		expr := concat(
			regBitPiece(0, 4, 0),
			memBytePiece(0x5000, 2),
		)
		plan := mustPlan(t, c, *regs, expr, 8, 20,
			[]byte{0x01, 0x34, 0x56}, []byte{0x0F, 0xFF, 0x0F}, mem.read)

		applied := map[uint64]*DwarfRegister{}
		regCalls := 0
		regs.ChangeFunc = applyChangeFunc(regs, applied, &regCalls)
		cw := &countingWrite{target: mem}
		res, err := plan.Commit(*regs, mem.read, cw.fn)
		c.Check(err == nil, "happy commit failed: %v", err)
		c.Check(res != nil && res.FirstError == nil && res.Current == nil, "result must be clean: %+v", res)
		c.Check(len(res.Completed) == 2, "both records completed: %d", len(res.Completed))
		c.Check(regCalls == 1 && cw.calls == 1, "exactly one reg and one mem callback: %d/%d", regCalls, cw.calls)
		c.Check(applied[0].Bytes[0] == 0xA1, "register committed value = %#x", applied[0].Bytes[0])
		// Piece 1 starts at memory bit 0 while logical bit 4: 0x5000 keeps
		// its unselected low nibble (E) and takes data[1]'s low nibble (4);
		// 0x5001 is fully selected from the two adjacent data nibbles.
		c.Check(mem[0x5000] == 0x4E && mem[0x5001] == 0x63, "memory committed bytes = %#x %#x", mem[0x5000], mem[0x5001])

		// A plan can only be committed once.
		_, err = plan.Commit(*regs, mem.read, cw.fn)
		c.Check(err != nil && strings.Contains(err.Error(), "already committed"), "re-commit must fail: %v", err)

		// --- last record drifts: preflight rejects before any write ---
		regs2 := newTestRegs(DwarfRegisterFromBytes([]byte{0xA5}))
		mem2 := memBackend{0x5000: 0x7E}
		expr2 := concat(regBitPiece(0, 4, 0), memBitPiece(0x5000, 4, 0))
		plan2 := mustPlan(t, c, *regs2, expr2, 8, 8, []byte{0x01}, []byte{0xFF}, mem2.read)
		regCalls2, memCalls2 := 0, 0
		regs2.ChangeFunc = func(uint64, *DwarfRegister) error { regCalls2++; return nil }
		write2 := func(addr uint64, data []byte) (int, error) { memCalls2++; return len(data), nil }
		mem2[0x5000] = 0x00 // snapshot changed after planning: last record fails
		_, err = plan2.Commit(*regs2, mem2.read, write2)
		c.Check(err != nil && strings.Contains(err.Error(), "preflight"), "drift must fail preflight: %v", err)
		c.Check(regCalls2 == 0 && memCalls2 == 0, "zero writes when preflight fails: %d/%d", regCalls2, memCalls2)

		// register drift is caught the same way
		regs3 := newTestRegs(DwarfRegisterFromBytes([]byte{0xA5}))
		plan3 := mustPlan(t, c, *regs3, regBitPiece(0, 4, 0), 8, 4, []byte{0x01}, []byte{0x0F}, nil)
		regs3.AddReg(0, DwarfRegisterFromBytes([]byte{0x5A}))
		regs3.ChangeFunc = func(uint64, *DwarfRegister) error { t.Fatal("no register write allowed after drift"); return nil }
		_, err = plan3.Commit(*regs3, nil, nil)
		c.Check(err != nil && strings.Contains(err.Error(), "preflight"), "register drift must fail preflight: %v", err)

		// --- short write: result names the current record and byte count ---
		regs4 := newTestRegs(DwarfRegisterFromBytes([]byte{0xA5}))
		mem4 := memBackend{0x6000: 0x00, 0x6001: 0x00}
		plan4 := mustPlan(t, c, *regs4, concat(regBitPiece(0, 4, 0), memBytePiece(0x6000, 2)),
			8, 20, []byte{0x01, 0x34, 0x56}, []byte{0x0F, 0xFF, 0x0F}, mem4.read)
		regs4.ChangeFunc = applyChangeFunc(regs4, map[uint64]*DwarfRegister{}, new(int))
		cw4 := &countingWrite{shortN: 1}
		res4, err4 := plan4.Commit(*regs4, mem4.read, cw4.fn)
		c.Check(err4 != nil && res4 != nil, "short write must return a result and error")
		c.Check(res4 != nil && res4.Current != nil && res4.Current.Addr == 0x6000, "current record is the memory one")
		c.Check(res4 != nil && res4.CurrentWritten == 1, "one byte actually written, got %d", res4.CurrentWritten)
		c.Check(res4 != nil && len(res4.Completed) == 1, "the register record completed first, got %d", len(res4.Completed))
		c.Check(res4 != nil && res4.FirstError != nil, "first error recorded")

		// --- callback error stops the commit ---
		regs5 := newTestRegs(DwarfRegisterFromBytes([]byte{0xA5}))
		mem5 := memBackend{0x7000: 0x00}
		plan5 := mustPlan(t, c, *regs5, memBitPiece(0x7000, 8, 0), 8, 8, []byte{0x42}, []byte{0xFF}, mem5.read)
		sentinel := errors.New("boom")
		cw5 := &countingWrite{failAddr: 0x7000, failErr: sentinel}
		res5, err5 := plan5.Commit(*regs5, mem5.read, cw5.fn)
		c.Check(errors.Is(err5, sentinel), "callback error is propagated: %v", err5)
		c.Check(res5 != nil && res5.Current != nil && res5.CurrentWritten == 0, "failed record wrote nothing")

		// --- missing callbacks are rejected ---
		regs6 := newTestRegs(DwarfRegisterFromBytes([]byte{0xA5}))
		plan6 := mustPlan(t, c, *regs6, regBitPiece(0, 4, 0), 8, 4, []byte{0x01}, []byte{0x0F}, nil)
		_, err = plan6.Commit(*regs6, nil, nil)
		c.Check(err != nil && strings.Contains(err.Error(), "ChangeFunc"), "missing ChangeFunc rejected: %v", err)
		mem7 := memBackend{0x8000: 0x00}
		plan7 := mustPlan(t, c, DwarfRegisters{}, memBitPiece(0x8000, 8, 0), 8, 8, []byte{0x01}, []byte{0xFF}, mem7.read)
		_, err = plan7.Commit(DwarfRegisters{}, mem7.read, nil)
		c.Check(err != nil && strings.Contains(err.Error(), "write callback"), "missing write callback rejected: %v", err)

		// --- publicly visible records cannot tamper with the commit ---
		regs8 := newTestRegs(DwarfRegisterFromBytes([]byte{0xA5}))
		plan8 := mustPlan(t, c, *regs8, regBitPiece(0, 4, 0), 8, 4, []byte{0x01}, []byte{0x0F}, nil)
		view := plan8.Records()
		view[0].Data[0] = 0x00
		again := plan8.Records()
		c.Check(again[0].Data[0] == 0xA1, "Records() returns independent copies, got %#x", again[0].Data[0])
		regs8.ChangeFunc = applyChangeFunc(regs8, map[uint64]*DwarfRegister{}, new(int))
		_, err = plan8.Commit(*regs8, nil, nil)
		c.Check(err == nil, "commit uses the plan-owned bytes, not the mutated view: %v", err)

		// --- empty mask selects nothing: no sources are read and the plan
		// commits with zero records, needing no callbacks ---
		plan9 := mustPlan(t, c, DwarfRegisters{}, memBitPiece(0x8100, 8, 0), 8, 8,
			[]byte{0x00}, []byte{0x00}, func(buf []byte, addr uint64) (int, error) {
				t.Fatal("unselected pieces must not read their source")
				return 0, nil
			})
		c.Check(len(plan9.Records()) == 0, "masked-out plan has no records")
		res9, err9 := plan9.Commit(DwarfRegisters{}, nil, nil)
		c.Check(err9 == nil && len(res9.Completed) == 0, "empty plan commits cleanly: %v %+v", err9, res9)

		// --- missing registers are fetched through the lazy load callback ---
		lazy := NewDwarfRegisters(0, make([]*DwarfRegister, 1), binary.LittleEndian, 0, 0, 0, 0)
		loaded := 0
		lazy.SetLoadMoreCallback(func() {
			loaded++
			lazy.AddReg(0, DwarfRegisterFromBytes([]byte{0xF0}))
		})
		plan10 := mustPlan(t, c, *lazy, regBitPiece(0, 4, 0), 8, 4, []byte{0x01}, []byte{0x0F}, nil)
		c.Check(loaded == 1, "lazy load fired once, got %d", loaded)
		c.Check(bytes.Equal(plan10.Records()[0].Orig, []byte{0xF0}), "snapshot came from lazy-loaded register")

		// --- short reads and read errors during planning fail the plan ---
		shortRead := func(buf []byte, addr uint64) (int, error) { return 0, nil }
		_, err = PlanLocationWrite(DwarfRegisters{}, memBitPiece(0x8200, 8, 0), 8, 8,
			[]byte{0x01}, []byte{0xFF}, shortRead)
		c.Check(err != nil && strings.Contains(err.Error(), "short read"), "short read fails planning: %v", err)
		readErr := func(buf []byte, addr uint64) (int, error) { return 0, errors.New("no memory") }
		_, err = PlanLocationWrite(DwarfRegisters{}, memBitPiece(0x8200, 8, 0), 8, 8,
			[]byte{0x01}, []byte{0xFF}, readErr)
		c.Check(err != nil && strings.Contains(err.Error(), "no memory"), "read error fails planning: %v", err)

		// --- address overflow is rejected rather than wrapping ---
		huge := memBackend{}
		// Source bit offset 8 makes the first selected bit land one byte
		// past the maximum address, before any memory read is attempted.
		overExpr := concat(constAddr8(^uint64(0)), bitPieceTerm(8, 8))
		_, err = PlanLocationWrite(DwarfRegisters{}, overExpr, 8, 8, []byte{0xFF}, []byte{0xFF}, huge.read)
		c.Check(err != nil && strings.Contains(err.Error(), "overflows the address space"), "address overflow fails: %v", err)
	})
}

// TestLocationWriteCompatibility verifies that planning shares the existing
// opcode interpreter and terminator logic, that legacy byte-only behavior is
// unchanged, and that bit pieces stay rejected on the legacy path.
func TestLocationWriteCompatibility(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		regs := newTestRegs(DwarfRegisterFromBytes([]byte{0xA5}))

		// Legacy byte pieces keep their old shape (Size in bytes, no bit
		// fields populated).
		legacy := concat(
			[]byte{byte(DW_OP_reg0)}, bytePieceTerm(1),
			memBytePiece(0x1000, 1),
		)
		_, pieces, err := ExecuteStackProgram(*regs, legacy, 8, nil)
		c.Check(err == nil && len(pieces) == 2, "legacy pieces evaluate: %v %d", err, len(pieces))
		c.Check(pieces[0].Kind == RegPiece && pieces[0].Size == 1 && pieces[0].BitSize == 0 && pieces[0].BitOffset == 0,
			"legacy piece keeps byte semantics: %+v", pieces[0])
		c.Check(pieces[1].Kind == AddrPiece && pieces[1].Val == 0x1000 && pieces[1].Size == 1, "legacy addr piece: %+v", pieces[1])

		// Legacy path still rejects bit pieces explicitly.
		_, _, err = ExecuteStackProgram(*regs, concat([]byte{byte(DW_OP_reg0)}, bitPieceTerm(3, 1)), 8, nil)
		c.Check(err != nil && strings.Contains(err.Error(), "0x9d"), "legacy bit_piece rejected: %v", err)
		_, _, err = ExecuteStackProgram(DwarfRegisters{}, memBitPiece(0x1000, 3, 1), 8, nil)
		c.Check(err != nil, "standalone bit_piece rejected on legacy path: %v", err)

		// Shared evaluation: frame-base addressing gives the same address
		// to both paths.
		fbregs := NewDwarfRegisters(0, nil, binary.LittleEndian, 0, 0, 0, 0)
		fbregs.FrameBase = 0x9000
		fbExpr := concat([]byte{byte(DW_OP_fbreg)}, uleb128(8))
		addr, _, err := ExecuteStackProgram(*fbregs, fbExpr, 8, nil)
		c.Check(err == nil && addr == 0x9008, "fbreg address via shared interpreter: %#x %v", addr, err)
		fbmem := memBackend{0x9008: 0x00}
		fbplan := mustPlan(t, c, *fbregs, fbExpr, 8, 8, []byte{0x77}, []byte{0xFF}, fbmem.read)
		fbrecs := fbplan.Records()
		c.Check(len(fbrecs) == 1 && fbrecs[0].Addr == 0x9008 && fbrecs[0].Data[0] == 0x77,
			"plan reuses fbreg evaluation: %+v", fbrecs)

		// Shared deref path: const4u(0x3000) deref -> 0x4000.
		derefMem := memBackend{}
		for i := uint64(0); i < 8; i++ {
			derefMem[0x3000+i] = byte((uint64(0x4000) >> (8 * i)) & 0xFF)
		}
		derefMem[0x4000] = 0x00
		derefExpr := concat(constAddr(0x3000), []byte{byte(DW_OP_deref)})
		daddr, _, err := ExecuteStackProgram(DwarfRegisters{}, derefExpr, 8, derefMem.read)
		c.Check(err == nil && daddr == 0x4000, "legacy deref: %#x %v", daddr, err)
		dplan := mustPlan(t, c, DwarfRegisters{}, derefExpr, 8, 8, []byte{0x99}, []byte{0xFF}, derefMem.read)
		drecs := dplan.Records()
		c.Check(len(drecs) == 1 && drecs[0].Addr == 0x4000, "plan follows the deref address: %+v", drecs)

		// A zero-length bit_piece still consumes both operands and clears
		// the (empty) stack; the following piece remains usable.
		zeroExpr := concat(
			regBitPiece(0, 0, 0),
			memBitPiece(0xA000, 8, 0),
		)
		zmem := memBackend{0xA000: 0x00}
		zplan := mustPlan(t, c, *regs, zeroExpr, 8, 8, []byte{0x33}, []byte{0xFF}, zmem.read)
		zrecs := zplan.Records()
		c.Check(len(zrecs) == 1 && zrecs[0].Addr == 0xA000 && zrecs[0].Data[0] == 0x33,
			"zero-length piece contributes no physical record: %+v", zrecs)

		// Unavailable pieces: selected bits fail, unselected bits do not
		// touch any data source.
		unavailExpr := concat(
			bytePieceTerm(1), // empty stack -> unavailable, 8 logical bits
			memBitPiece(0xB000, 8, 0),
		)
		readCalls := 0
		noRead := func(buf []byte, addr uint64) (int, error) {
			readCalls++
			return (memBackend{0xB000: 0x00}).read(buf, addr)
		}
		_, err = PlanLocationWrite(DwarfRegisters{}, unavailExpr, 8, 16,
			[]byte{0xFF, 0x00}, []byte{0xFF, 0x00}, noRead)
		c.Check(err != nil && strings.Contains(err.Error(), "unavailable"), "selecting unavailable bits fails: %v", err)
		uplan, err := PlanLocationWrite(DwarfRegisters{}, unavailExpr, 8, 16,
			[]byte{0x00, 0x44}, []byte{0x00, 0xFF}, noRead)
		c.Check(err == nil, "masking unavailable bits out is allowed: %v", err)
		c.Check(readCalls == 1, "only the selected memory piece is read: %d calls", readCalls)
		urecs := uplan.Records()
		c.Check(len(urecs) == 1 && urecs[0].Addr == 0xB000 && urecs[0].Data[0] == 0x44, "memory piece still planned: %+v", urecs)

		// Constants accept only their own value and produce no writes.
		immExpr := concat([]byte{byte(DW_OP_implicit_value), 0x01, 0x0A}, bitPieceTerm(4, 0))
		_, err = PlanLocationWrite(DwarfRegisters{}, immExpr, 8, 4, []byte{0x0A}, []byte{0x0F}, nil)
		c.Check(err == nil, "matching constant request accepted: %v", err)
		cp, err := PlanLocationWrite(DwarfRegisters{}, immExpr, 8, 4, []byte{0x0B}, []byte{0x0F}, nil)
		c.Check(err != nil && strings.Contains(err.Error(), "constant"), "mismatched constant rejected: %v", err)
		c.Check(cp == nil, "failed planning returns no plan")
		stackExpr := concat([]byte{byte(DW_OP_lit5), byte(DW_OP_stack_value)}, bitPieceTerm(3, 0))
		_, err = PlanLocationWrite(DwarfRegisters{}, stackExpr, 8, 3, []byte{0x05}, []byte{0x07}, nil)
		c.Check(err == nil, "stack_value constant matched: %v", err)
		wideExpr := concat([]byte{byte(DW_OP_implicit_value), 0x01, 0x0A}, bitPieceTerm(8, 4))
		_, err = PlanLocationWrite(DwarfRegisters{}, wideExpr, 8, 8, []byte{0x00}, []byte{0xFF}, nil)
		c.Check(err != nil && strings.Contains(err.Error(), "beyond the constant source width"), "out-of-width constant bit fails: %v", err)

		// Missing terminator on the last piece of a composite is invalid.
		badTerm := concat(regBitPiece(0, 8, 0), constAddr(0xC000))
		_, err = PlanLocationWrite(*regs, badTerm, 8, 16, []byte{0x00, 0x00}, []byte{0x00, 0x00}, nil)
		c.Check(err != nil && strings.Contains(err.Error(), "missing a piece terminator"), "unterminated last piece fails: %v", err)

		// The byte terminator DW_OP_piece works on the planning path too:
		// one register byte then one memory byte, 16-bit object.
		bpRegs := newTestRegs(DwarfRegisterFromBytes([]byte{0x0F}))
		bpMem := memBackend{0xC100: 0xF0}
		bpExpr := concat(
			[]byte{byte(DW_OP_reg0)}, bytePieceTerm(1),
			memBytePiece(0xC100, 1),
		)
		bpPlan := mustPlan(t, c, *bpRegs, bpExpr, 8, 16,
			[]byte{0xF0, 0x0F}, []byte{0xFF, 0xFF}, bpMem.read)
		bpRecs := bpPlan.Records()
		c.Check(len(bpRecs) == 2, "byte terminators yield a reg and a mem record: %d", len(bpRecs))
		if len(bpRecs) == 2 {
			c.Check(bpRecs[0].Kind == PhysicalRegister && bytes.Equal(bpRecs[0].Data, []byte{0xF0}), "reg byte planned: %+v", bpRecs[0])
			c.Check(bpRecs[1].Kind == PhysicalMemory && bytes.Equal(bpRecs[1].Data, []byte{0x0F}), "mem byte planned: %+v", bpRecs[1])
		}

		// A composite longer than the object is rejected.
		longExpr := concat(
			[]byte{byte(DW_OP_reg0)}, bytePieceTerm(1),
			memBytePiece(0xC200, 2),
		)
		_, err = PlanLocationWrite(*bpRegs, longExpr, 8, 16,
			[]byte{0x00, 0x00}, []byte{0x00, 0x00}, bpMem.read)
		c.Check(err != nil && strings.Contains(err.Error(), "extends past"), "over-long composite fails: %v", err)

		// Error messages carry the expression offset.
		_, _, err = ExecuteStackProgram(DwarfRegisters{}, []byte{byte(DW_OP_drop)}, 8, nil)
		c.Check(err != nil && strings.Contains(err.Error(), "offset 0"), "stack underflow reports offset: %v", err)
		_, err = PlanLocationWrite(*regs, concat([]byte{byte(DW_OP_reg0)}, []byte{byte(DW_OP_bit_piece), 0x04}), 8, 4,
			[]byte{0x00}, []byte{0x0F}, nil)
		c.Check(err != nil && strings.Contains(err.Error(), "offset"), "truncated terminator reports offset: %v", err)
		loopExpr := []byte{byte(DW_OP_skip), 0xFD, 0xFF} // jump back to offset 0 forever
		_, err = PlanLocationWrite(DwarfRegisters{}, loopExpr, 8, 8, []byte{0x00}, []byte{0x00}, nil)
		c.Check(err != nil && strings.Contains(err.Error(), "offset") && strings.Contains(err.Error(), "step limit"),
			"step exhaustion reports offset: %v", err)

		// Input validation: ptrSize, endianness, mask padding, limits.
		_, err = PlanLocationWrite(*regs, regBitPiece(0, 4, 0), 2, 4, []byte{0x01}, []byte{0x0F}, nil)
		c.Check(err != nil, "ptrSize 2 rejected: %v", err)
		beRegs := NewDwarfRegisters(0, []*DwarfRegister{DwarfRegisterFromBytes([]byte{0xA5})}, binary.BigEndian, 0, 0, 0, 0)
		_, err = PlanLocationWrite(*beRegs, regBitPiece(0, 4, 0), 8, 4, []byte{0x01}, []byte{0x0F}, nil)
		c.Check(err != nil && strings.Contains(err.Error(), "little-endian"), "big-endian rejected: %v", err)
		_, err = PlanLocationWrite(*regs, regBitPiece(0, 4, 0), 8, 5, []byte{0x01}, []byte{0xFF}, nil)
		c.Check(err != nil && strings.Contains(err.Error(), "beyond bitSize"), "mask padding rejected: %v", err)
		_, err = PlanLocationWrite(*regs, regBitPiece(0, 4, 0), 8, 0, nil, nil, nil)
		c.Check(err != nil, "zero bitSize rejected: %v", err)
		_, err = PlanLocationWrite(*regs, regBitPiece(0, 4, 0), 4, 4, []byte{0x01}, []byte{0x0F}, nil)
		c.Check(err == nil, "ptrSize 4 supported: %v", err)

		// Planning never mutates the caller's registers: a Uint64Val-only
		// register keeps a nil Bytes after planning.
		lazyRegs := newTestRegs(DwarfRegisterFromUint64(0xA5))
		c.Check(lazyRegs.regs[0].Bytes == nil, "test register starts without Bytes")
		lplan := mustPlan(t, c, *lazyRegs, regBitPiece(0, 4, 0), 8, 4, []byte{0x01}, []byte{0x0F}, nil)
		c.Check(lazyRegs.regs[0].Bytes == nil, "planning must not fill the input register's Bytes")
		c.Check(bytes.Equal(lplan.Records()[0].Orig, []byte{0xA5, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}),
			"Uint64Val falls back to an eight-byte snapshot")
	})
}

// ExamplePlanLocationWrite shows the prepare-then-commit flow for a single
// masked bit flip in a register.
func ExamplePlanLocationWrite() {
	regs := NewDwarfRegisters(0,
		[]*DwarfRegister{DwarfRegisterFromBytes([]byte{0b00000001})},
		binary.LittleEndian, 0, 0, 0, 0)
	regs.ChangeFunc = func(regNum uint64, reg *DwarfRegister) error {
		fmt.Printf("register %d now %08b\n", regNum, reg.Bytes[0])
		return nil
	}

	// One-byte object living entirely in register 0.
	expr := []byte{byte(DW_OP_reg0)}
	// Flip only bit 1; all other mask bits are zero.
	plan, err := PlanLocationWrite(*regs, expr, 8, 8,
		[]byte{0b00000010}, []byte{0b00000010}, nil)
	if err != nil {
		fmt.Println("plan error:", err)
		return
	}
	fmt.Println("prepared records:", len(plan.Records()))
	if _, err := plan.Commit(*regs, nil, nil); err != nil {
		fmt.Println("commit error:", err)
		return
	}
	// Output:
	// prepared records: 1
	// register 0 now 00000011
}
