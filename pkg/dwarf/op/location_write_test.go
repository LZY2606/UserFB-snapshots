package op

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"strings"
	"testing"

	"github.com/go-delve/delve/task_support/taskcheck"
)

type memoryCall struct {
	addr uint64
	data []byte
}

type testMemory struct {
	data    map[uint64]byte
	reads   int
	writes  int
	calls   []memoryCall
	writeFn func(addr uint64, data []byte) (int, error)
}

func newTestMemory(values map[uint64]byte) *testMemory {
	data := make(map[uint64]byte, len(values))
	for k, v := range values {
		data[k] = v
	}
	return &testMemory{data: data}
}

func (m *testMemory) read(buf []byte, addr uint64) (int, error) {
	m.reads++
	for i := range buf {
		a := addr + uint64(i)
		b, ok := m.data[a]
		if !ok {
			return i, fmt.Errorf("test memory has no byte at %#x", a)
		}
		buf[i] = b
	}
	return len(buf), nil
}

func (m *testMemory) write(addr uint64, data []byte) (int, error) {
	m.writes++
	m.calls = append(m.calls, memoryCall{addr: addr, data: append([]byte(nil), data...)})
	if m.writeFn != nil {
		return m.writeFn(addr, data)
	}
	for i, b := range data {
		m.data[addr+uint64(i)] = b
	}
	return len(data), nil
}

func testRegisters(regs ...*DwarfRegister) DwarfRegisters {
	return DwarfRegisters{regs: regs, ByteOrder: binary.LittleEndian}
}

func byteRegister(values ...byte) *DwarfRegister {
	return DwarfRegisterFromBytes(append([]byte(nil), values...))
}

func testAddrExpr(addr uint64) []byte {
	out := []byte{byte(DW_OP_addr)}
	var raw [8]byte
	binary.LittleEndian.PutUint64(raw[:], addr)
	return append(out, raw[:]...)
}

func TestLocationWriteAliases(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Two unaligned bit pieces reach different bits of the same vector
		// register. Both changes must survive in one complete register image.
		regOrig := []byte{0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc}
		regs := testRegisters(byteRegister(regOrig...))
		expr := []byte{
			byte(DW_OP_reg0), byte(DW_OP_bit_piece), 4, 2,
			byte(DW_OP_reg0), byte(DW_OP_bit_piece), 4, 10,
		}
		plan, err := PlanLocationWrite(regs, expr, 8, 8, []byte{0xca}, []byte{0xff}, nil)
		c.Check(err == nil, "unaligned same-register plan: %v", err)
		if check(c, err == nil && plan != nil, "same-register plan exists") {
			rws := plan.RegisterWrites()
			c.Check(len(rws) == 1, "wanted one register record, got %d", len(rws))
			if check(c, len(rws) == 1, "register record available") {
				c.Check(rws[0].RegNum == 0, "register number = %d", rws[0].RegNum)
				wantData := []byte{0x69, 0x72, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc}
				c.Check(bytes.Equal(rws[0].Data, wantData), "same-register data = % x, want % x", rws[0].Data, wantData)
				c.Check(bytes.Equal(rws[0].Original, regOrig), "same-register original = % x", rws[0].Original)

				rws[0].Data[0] = 0xff
				again := plan.RegisterWrites()
				c.Check(again[0].Data[0] == 0x69, "returned register records must be copies, got %#02x", again[0].Data[0])
			}
			c.Check(len(plan.MemoryWrites()) == 0, "register-only plan has memory records")
		}

		// A mixed, unaligned composite: one register nibble, a ten-bit memory
		// run crossing a byte boundary, and a matching stack_value constant.
		mem := newTestMemory(map[uint64]byte{0x4000: 0x07, 0x4001: 0xff})
		mixedRegs := testRegisters(nil, nil, nil, nil, byteRegister(0xa5, 0x3c, 0, 0, 0, 0, 0, 0))
		mixedExpr := []byte{byte(DW_OP_reg4), byte(DW_OP_bit_piece), 4, 2}
		mixedExpr = append(mixedExpr, testAddrExpr(0x4000)...)
		mixedExpr = append(mixedExpr, byte(DW_OP_bit_piece), 10, 3)
		mixedExpr = append(mixedExpr,
			byte(DW_OP_lit2), byte(DW_OP_stack_value),
			byte(DW_OP_bit_piece), 2, 0)
		mixedPlan, err := PlanLocationWrite(mixedRegs, mixedExpr, 8, 16,
			[]byte{0xd2, 0xa5}, []byte{0xff, 0xff}, mem.read)
		c.Check(err == nil, "mixed-source plan: %v", err)
		if check(c, err == nil && mixedPlan != nil, "mixed plan exists") {
			rws := mixedPlan.RegisterWrites()
			if check(c, len(rws) == 1, "mixed register records = %d", len(rws)) {
				wantReg := []byte{0x89, 0x3c, 0, 0, 0, 0, 0, 0}
				c.Check(bytes.Equal(rws[0].Data, wantReg), "mixed register data = % x, want % x", rws[0].Data, wantReg)
			}
			mws := mixedPlan.MemoryWrites()
			if check(c, len(mws) == 1, "mixed memory records = %d", len(mws)) {
				c.Check(mws[0].Address == 0x4000, "mixed memory address = %#x", mws[0].Address)
				c.Check(bytes.Equal(mws[0].Original, []byte{0x07, 0xff}), "mixed original = % x", mws[0].Original)
				c.Check(bytes.Equal(mws[0].Data, []byte{0xef, 0xf2}), "mixed memory data = % x, want ef f2", mws[0].Data)
			}
		}

		// Two different address expressions reach the same physical byte.
		overlapMem := newTestMemory(map[uint64]byte{0x1000: 0x00})
		overlapExpr := append(testAddrExpr(0x1000), byte(DW_OP_bit_piece), 4, 4)
		overlapExpr = append(overlapExpr, byte(DW_OP_addr), 0xff, 0x0f, 0, 0, 0, 0, 0, 0)
		overlapExpr = append(overlapExpr, byte(DW_OP_lit1), byte(DW_OP_plus))
		overlapExpr = append(overlapExpr, byte(DW_OP_bit_piece), 4, 0)
		overlapPlan, err := PlanLocationWrite(DwarfRegisters{ByteOrder: binary.LittleEndian}, overlapExpr, 8, 8,
			[]byte{0xca}, []byte{0xff}, overlapMem.read)
		c.Check(err == nil, "overlapping address plan: %v", err)
		if check(c, err == nil && overlapPlan != nil, "overlap plan exists") {
			mws := overlapPlan.MemoryWrites()
			if check(c, len(mws) == 1, "overlap memory records = %d", len(mws)) {
				c.Check(mws[0].Address == 0x1000, "overlap address = %#x", mws[0].Address)
				c.Check(bytes.Equal(mws[0].Data, []byte{0xac}), "overlap byte = %#02x, want ac", mws[0].Data)
			}
		}

		// A one-byte gap must produce two records, not one widened range.
		gapExpr := append(testAddrExpr(0x1000), byte(DW_OP_piece), 1)
		gapExpr = append(gapExpr, testAddrExpr(0x1002)...)
		gapExpr = append(gapExpr, byte(DW_OP_piece), 1)
		gapMem := newTestMemory(map[uint64]byte{0x1000: 0, 0x1002: 0})
		gapPlan, err := PlanLocationWrite(DwarfRegisters{ByteOrder: binary.LittleEndian}, gapExpr, 8, 16,
			[]byte{0x11, 0x22}, []byte{0xff, 0xff}, gapMem.read)
		c.Check(err == nil, "gapped memory plan: %v", err)
		if check(c, err == nil && gapPlan != nil, "gapped memory plan exists") {
			mws := gapPlan.MemoryWrites()
			if check(c, len(mws) == 2, "gapped memory records = %d", len(mws)) {
				c.Check(mws[0].Address == 0x1000 && len(mws[0].Data) == 1, "first gap record = %+v", mws[0])
				c.Check(mws[1].Address == 0x1002 && len(mws[1].Data) == 1, "second gap record = %+v", mws[1])
			}
		}

		// Opposite requests for the same register bit identify both logical
		// sources and reject the whole plan.
		conflictRegs := testRegisters(nil, nil, byteRegister(0, 0, 0, 0, 0, 0, 0, 0))
		conflictExpr := []byte{
			byte(DW_OP_reg2), byte(DW_OP_bit_piece), 1, 3,
			byte(DW_OP_reg2), byte(DW_OP_bit_piece), 1, 3,
		}
		badPlan, err := PlanLocationWrite(conflictRegs, conflictExpr, 8, 2,
			[]byte{0x01}, []byte{0x03}, nil)
		c.Check(err != nil, "register alias conflict must be rejected")
		c.Check(badPlan == nil, "conflict must not return a half plan")
		var aliasErr *AliasConflictError
		if check(c, errors.As(err, &aliasErr), "register conflict has structured AliasConflictError: %v", err) {
			c.Check(aliasErr.Target.Kind == PhysicalRegisterWrite && aliasErr.Target.RegNum == 2 && aliasErr.Target.Bit == 3,
				"register conflict target = %+v", aliasErr.Target)
			c.Check(aliasErr.First.LogicalBit == 0 && aliasErr.Second.LogicalBit == 1,
				"conflict logical sources = %d and %d", aliasErr.First.LogicalBit, aliasErr.Second.LogicalBit)
			c.Check(aliasErr.First.Fragment == 0 && aliasErr.Second.Fragment == 1,
				"conflict fragments = %d and %d", aliasErr.First.Fragment, aliasErr.Second.Fragment)
			c.Check(strings.Contains(err.Error(), "logical bit 0") && strings.Contains(err.Error(), "logical bit 1"),
				"conflict error reports both sources: %v", err)
		}

		// An unselected aliased request imposes no constraint, even if its
		// data bit would have opposed the selected physical update.
		unselectedAliasPlan, err := PlanLocationWrite(conflictRegs, conflictExpr, 8, 2,
			[]byte{0x01}, []byte{0x01}, nil)
		c.Check(err == nil, "unselected alias should not conflict: %v", err)
		if check(c, unselectedAliasPlan != nil, "unselected alias plan exists") {
			rws := unselectedAliasPlan.RegisterWrites()
			if check(c, len(rws) == 1, "unselected alias records = %d", len(rws)) {
				c.Check(rws[0].Data[0] == 0x08, "selected alias bit = %#02x", rws[0].Data[0])
			}
		}

		// Equal requests for an alias collapse to one physical update.
		samePlan, err := PlanLocationWrite(conflictRegs, conflictExpr, 8, 2,
			[]byte{0x03}, []byte{0x03}, nil)
		c.Check(err == nil, "same-value alias plan: %v", err)
		if check(c, err == nil && samePlan != nil, "same-value alias plan exists") {
			rws := samePlan.RegisterWrites()
			if check(c, len(rws) == 1, "same-value alias records = %d", len(rws)) {
				c.Check(rws[0].Data[0] == 0x08, "aliased bit data = %#02x, want 08", rws[0].Data[0])
			}
		}

		// Memory aliases are detected through the source bit offset too.
		memAliasExpr := append(testAddrExpr(0x2000), byte(DW_OP_bit_piece), 1, 0)
		memAliasExpr = append(memAliasExpr, testAddrExpr(0x1fff)...)
		memAliasExpr = append(memAliasExpr, byte(DW_OP_bit_piece), 1, 8)
		memAliasMem := newTestMemory(map[uint64]byte{0x2000: 0})
		_, err = PlanLocationWrite(DwarfRegisters{ByteOrder: binary.LittleEndian}, memAliasExpr, 8, 2,
			[]byte{0x01}, []byte{0x03}, memAliasMem.read)
		var memAliasErr *AliasConflictError
		if check(c, errors.As(err, &memAliasErr), "memory alias conflict: %v", err) {
			c.Check(memAliasErr.Target.Kind == PhysicalMemoryWrite && memAliasErr.Target.Address == 0x2000 && memAliasErr.Target.Bit == 0,
				"memory alias target = %+v", memAliasErr.Target)
		}

		// stack_value is an immutable source: equal bits are accepted without
		// writes, changed bits are rejected.
		constExpr := []byte{byte(DW_OP_lit7), byte(DW_OP_stack_value)}
		constPlan, err := PlanLocationWrite(DwarfRegisters{}, constExpr, 8, 3,
			[]byte{0x07}, []byte{0x07}, nil)
		c.Check(err == nil, "matching stack_value constant: %v", err)
		if check(c, constPlan != nil, "constant plan exists") {
			c.Check(len(constPlan.Writes()) == 0, "constant source produced %d writes", len(constPlan.Writes()))
		}
		_, err = PlanLocationWrite(DwarfRegisters{}, constExpr, 8, 3,
			[]byte{0x06}, []byte{0x07}, nil)
		var constErr *ConstantMismatchError
		c.Check(errors.As(err, &constErr), "changed constant must be rejected: %v", err)
	})
}

func TestLocationWritePreflight(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// A successful mixed commit checks record order and result contents.
		regs := testRegisters(DwarfRegisterFromUint64(0))
		mem := newTestMemory(map[uint64]byte{0x1000: 0})
		expr := []byte{byte(DW_OP_reg0), byte(DW_OP_piece), 1}
		expr = append(expr, testAddrExpr(0x1000)...)
		expr = append(expr, byte(DW_OP_piece), 1)
		plan, err := PlanLocationWrite(regs, expr, 8, 16,
			[]byte{0x34, 0x12}, []byte{0xff, 0xff}, mem.read)
		c.Check(err == nil, "successful commit plan: %v", err)
		if check(c, err == nil && plan != nil, "successful plan exists") {
			var regCalls []regWriteCall
			current := regs
			current.ChangeFunc = func(num uint64, reg *DwarfRegister) error {
				regCalls = append(regCalls, regWriteCall{regNum: num, data: append([]byte(nil), reg.Bytes...)})
				return nil
			}
			result, commitErr := plan.Commit(current, mem.read, mem.write)
			c.Check(commitErr == nil, "commit: %v", commitErr)
			c.Check(result != nil && result.Succeeded(), "result succeeded: %+v", result)
			if check(c, result != nil, "commit result present") {
				c.Check(intsEqual(result.CompletedRecords, []int{0, 1}),
					"completed records = %v", result.CompletedRecords)
				c.Check(result.FailedIndex == -1, "failed index = %d", result.FailedIndex)
			}
			c.Check(len(regCalls) == 1 && regCalls[0].regNum == 0 && regCalls[0].data[0] == 0x34,
				"register ChangeFunc calls = %+v", regCalls)
			c.Check(len(mem.calls) == 1 && mem.calls[0].addr == 0x1000 && bytes.Equal(mem.calls[0].data, []byte{0x12}),
				"memory write calls = %+v", mem.calls)
		}

		// A register snapshot change is caught before ChangeFunc is called.
		changedRegs := testRegisters(byteRegister(0, 0, 0, 0, 0, 0, 0, 0))
		changedPlan, err := PlanLocationWrite(changedRegs,
			[]byte{byte(DW_OP_reg0)}, 8, 8, []byte{0xaa}, []byte{0xff}, nil)
		c.Check(err == nil, "changed-register plan: %v", err)
		if check(c, changedPlan != nil, "changed-register plan exists") {
			var calls int
			changedRegs.ChangeFunc = func(uint64, *DwarfRegister) error { calls++; return nil }
			changedRegs.Reg(0).Bytes[0] = 0xff
			result, commitErr := changedPlan.Commit(changedRegs, nil, nil)
			c.Check(commitErr != nil && errors.Is(commitErr, ErrPreflight), "register preflight error: %v", commitErr)
			c.Check(calls == 0, "ChangeFunc called %d times after snapshot change", calls)
			if check(c, result != nil, "register preflight result present") {
				c.Check(result.Stage == CommitPreflight && result.FailedIndex == 0, "preflight result = %+v", result)
				c.Check(len(result.CompletedRecords) == 0, "completed after preflight failure: %v", result.CompletedRecords)
			}
			_, secondErr := changedPlan.Commit(changedRegs, nil, nil)
			c.Check(errors.Is(secondErr, ErrPlanAlreadyCommitted), "second commit = %v", secondErr)
		}

		// When the last preflight record changed, no write may have started.
		twoMemExpr := append(testAddrExpr(0x1000), byte(DW_OP_piece), 1)
		twoMemExpr = append(twoMemExpr, testAddrExpr(0x2000)...)
		twoMemExpr = append(twoMemExpr, byte(DW_OP_piece), 1)
		twoMemPlan, err := PlanLocationWrite(DwarfRegisters{ByteOrder: binary.LittleEndian}, twoMemExpr, 8, 16,
			[]byte{0x11, 0x22}, []byte{0xff, 0xff},
			newTestMemory(map[uint64]byte{0x1000: 0, 0x2000: 0}).read)
		c.Check(err == nil, "two-memory plan: %v", err)
		if check(c, twoMemPlan != nil, "two-memory plan exists") {
			preflightMem := newTestMemory(map[uint64]byte{0x1000: 0, 0x2000: 0xff})
			writeCalls := 0
			write := func(uint64, []byte) (int, error) { writeCalls++; return 1, nil }
			result, commitErr := twoMemPlan.Commit(DwarfRegisters{}, preflightMem.read, write)
			c.Check(commitErr != nil && errors.Is(commitErr, ErrPreflight), "last-record preflight: %v", commitErr)
			c.Check(writeCalls == 0, "write callback count after failed final preflight = %d", writeCalls)
			if check(c, result != nil, "last preflight result present") {
				c.Check(result.FailedIndex == 1, "failed preflight index = %d", result.FailedIndex)
				c.Check(len(result.CompletedRecords) == 0, "records completed before preflight: %v", result.CompletedRecords)
			}
		}

		// Current memory read failure is also preflight-only.
		readFailPlan, err := PlanLocationWrite(DwarfRegisters{ByteOrder: binary.LittleEndian},
			append(testAddrExpr(0x1800), byte(DW_OP_piece), 1), 8, 8,
			[]byte{1}, []byte{1}, newTestMemory(map[uint64]byte{0x1800: 0}).read)
		c.Check(err == nil, "read-failure plan: %v", err)
		if readFailPlan != nil {
			writeCalls := 0
			read := func([]byte, uint64) (int, error) { return 0, errors.New("snapshot read failed") }
			result, commitErr := readFailPlan.Commit(DwarfRegisters{}, read, func(uint64, []byte) (int, error) {
				writeCalls++
				return 1, nil
			})
			c.Check(commitErr != nil && errors.Is(commitErr, ErrPreflight), "read failure preflight: %v", commitErr)
			c.Check(writeCalls == 0, "writes after read failure = %d", writeCalls)
			c.Check(result != nil && result.FailedIndex == 0, "read failure result = %+v", result)
		}

		// Required callbacks are checked.
		memOnlyPlan, err := PlanLocationWrite(DwarfRegisters{ByteOrder: binary.LittleEndian},
			append(testAddrExpr(0x1900), byte(DW_OP_piece), 1), 8, 8,
			[]byte{1}, []byte{1}, newTestMemory(map[uint64]byte{0x1900: 0}).read)
		c.Check(err == nil, "callback plan: %v", err)
		if memOnlyPlan != nil {
			result, commitErr := memOnlyPlan.Commit(DwarfRegisters{}, newTestMemory(nil).read, nil)
			c.Check(commitErr != nil, "nil memory write callback must fail")
			c.Check(result != nil && result.FailedIndex == 0, "nil callback result = %+v", result)
		}
		regOnlyPlan, err := PlanLocationWrite(testRegisters(DwarfRegisterFromUint64(0)),
			[]byte{byte(DW_OP_reg0)}, 8, 8, []byte{1}, []byte{1}, nil)
		c.Check(err == nil, "register callback plan: %v", err)
		if regOnlyPlan != nil {
			result, commitErr := regOnlyPlan.Commit(DwarfRegisters{}, nil, nil)
			c.Check(commitErr != nil, "nil ChangeFunc must fail")
			c.Check(result != nil && result.FailedIndex == 0, "nil ChangeFunc result = %+v", result)
		}

		// Public record copies cannot change the bytes passed to Commit.
		tamperMem := newTestMemory(map[uint64]byte{0x1a00: 0})
		tamperPlan, err := PlanLocationWrite(DwarfRegisters{ByteOrder: binary.LittleEndian},
			append(testAddrExpr(0x1a00), byte(DW_OP_piece), 1), 8, 8,
			[]byte{0x5a}, []byte{0xff}, tamperMem.read)
		c.Check(err == nil, "tamper plan: %v", err)
		if tamperPlan != nil {
			recs := tamperPlan.MemoryWrites()
			recs[0].Data[0] = 0xff
			recs[0].Original[0] = 0xee
			writes := tamperPlan.Writes()
			writes[0].Data[0] = 0xaa
			var received []byte
			_, commitErr := tamperPlan.Commit(DwarfRegisters{}, tamperMem.read, func(_ uint64, data []byte) (int, error) {
				received = append([]byte(nil), data...)
				return len(data), nil
			})
			c.Check(commitErr == nil, "tamper commit: %v", commitErr)
			c.Check(bytes.Equal(received, []byte{0x5a}), "commit data after tampering = % x", received)
		}

		// A callback error reports completed records and the current count.
		failExpr := append(testAddrExpr(0x1b00), byte(DW_OP_piece), 1)
		failExpr = append(failExpr, testAddrExpr(0x1c00)...)
		failExpr = append(failExpr, byte(DW_OP_piece), 1)
		failPlan, err := PlanLocationWrite(DwarfRegisters{ByteOrder: binary.LittleEndian}, failExpr, 8, 16,
			[]byte{0x11, 0x22}, []byte{0xff, 0xff},
			newTestMemory(map[uint64]byte{0x1b00: 0, 0x1c00: 0}).read)
		c.Check(err == nil, "write-failure plan: %v", err)
		if failPlan != nil {
			writeCalls := 0
			result, commitErr := failPlan.Commit(DwarfRegisters{},
				newTestMemory(map[uint64]byte{0x1b00: 0, 0x1c00: 0}).read,
				func(addr uint64, _ []byte) (int, error) {
					writeCalls++
					if addr == 0x1c00 {
						return 1, errors.New("boom")
					}
					return 1, nil
				})
			c.Check(commitErr != nil && strings.Contains(commitErr.Error(), "boom"), "write error: %v", commitErr)
			c.Check(writeCalls == 2, "write calls before stop = %d", writeCalls)
			if check(c, result != nil, "write failure result present") {
				c.Check(result.Stage == CommitWriting && result.FailedIndex == 1, "write result = %+v", result)
				c.Check(result.BytesWritten == 1, "bytes written on failed call = %d", result.BytesWritten)
				c.Check(intsEqual(result.CompletedRecords, []int{0}), "completed = %v", result.CompletedRecords)
			}
			_, secondErr := failPlan.Commit(DwarfRegisters{}, nil, nil)
			c.Check(errors.Is(secondErr, ErrPlanAlreadyCommitted), "failed plan reused: %v", secondErr)
			c.Check(writeCalls == 2, "reused plan issued another write")
		}

		// A short write with nil error is also a stopping failure.
		shortExpr := append(testAddrExpr(0x1d00), byte(DW_OP_piece), 1)
		shortExpr = append(shortExpr, testAddrExpr(0x1e00)...)
		shortExpr = append(shortExpr, byte(DW_OP_piece), 2)
		shortPlan, err := PlanLocationWrite(DwarfRegisters{ByteOrder: binary.LittleEndian}, shortExpr, 8, 24,
			[]byte{0x11, 0x22, 0x33}, []byte{0xff, 0xff, 0xff},
			newTestMemory(map[uint64]byte{0x1d00: 0, 0x1e00: 0, 0x1e01: 0}).read)
		c.Check(err == nil, "short-write plan: %v", err)
		if shortPlan != nil {
			result, commitErr := shortPlan.Commit(DwarfRegisters{},
				newTestMemory(map[uint64]byte{0x1d00: 0, 0x1e00: 0, 0x1e01: 0}).read,
				func(addr uint64, data []byte) (int, error) {
					if addr == 0x1e00 {
						return 1, nil
					}
					return len(data), nil
				})
			c.Check(commitErr != nil && errors.Is(commitErr, ErrShortWrite), "short write error: %v", commitErr)
			c.Check(result != nil && result.FailedIndex == 1 && result.BytesWritten == 1 && result.RecordLength == 2,
				"short write result = %+v", result)
		}

		// Commit does not re-evaluate the address expression.
		addrRegs := testRegisters(nil, DwarfRegisterFromUint64(0x2000))
		addrPlan, err := PlanLocationWrite(addrRegs,
			[]byte{byte(DW_OP_breg1), 0x10}, 8, 8, []byte{0x77}, []byte{0xff},
			newTestMemory(map[uint64]byte{0x2010: 0}).read)
		c.Check(err == nil, "fixed-address plan: %v", err)
		if addrPlan != nil {
			changed := addrRegs
			changed.Reg(1).Uint64Val = 0x9999
			var gotAddr uint64
			result, commitErr := addrPlan.Commit(changed,
				newTestMemory(map[uint64]byte{0x2010: 0}).read,
				func(addr uint64, data []byte) (int, error) {
					gotAddr = addr
					return len(data), nil
				})
			c.Check(commitErr == nil, "fixed-address commit: %v", commitErr)
			c.Check(result != nil && result.Succeeded(), "fixed-address result = %+v", result)
			c.Check(gotAddr == 0x2010, "commit re-evaluated or changed address to %#x", gotAddr)
		}
	})
}

type regWriteCall struct {
	regNum uint64
	data   []byte
}

func check(c *taskcheck.C, ok bool, message string, args ...interface{}) bool {
	c.Check(ok, message, args...)
	return ok
}

func intsEqual(got, want []int) bool {
	if len(got) != len(want) {
		return false
	}
	for i := range got {
		if got[i] != want[i] {
			return false
		}
	}
	return true
}

func TestLocationWriteCompatibility(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// The original signature is part of compatibility.
		var _ func(DwarfRegisters, []byte, int, ReadMemoryFunc) (int64, []Piece, error) = ExecuteStackProgram

		regs := NewDwarfRegisters(0, []*DwarfRegister{DwarfRegisterFromUint64(0x1234)}, binary.LittleEndian, 0, 0, 0, 0)
		expr := []byte{byte(DW_OP_reg0), byte(DW_OP_piece), 1}
		expr = append(expr, testAddrExpr(0x1000)...)
		expr = append(expr, byte(DW_OP_piece), 1)
		_, pieces, err := ExecuteStackProgram(*regs, expr, 8, nil)
		c.Check(err == nil, "legacy byte pieces: %v", err)
		if check(c, len(pieces) == 2, "legacy piece count = %d", len(pieces)) {
			c.Check(pieces[0].Kind == RegPiece && pieces[0].Val == 0 && pieces[0].Size == 1,
				"legacy register piece = %+v", pieces[0])
			c.Check(pieces[1].Kind == AddrPiece && pieces[1].Val == 0x1000 && pieces[1].Size == 1,
				"legacy memory piece = %+v", pieces[1])
		}

		_, gap, err := ExecuteStackProgram(DwarfRegisters{}, []byte{byte(DW_OP_piece), 1}, 8, nil)
		c.Check(err == nil && len(gap) == 1 && gap[0].Kind == ImmPiece && gap[0].Val == 0,
			"legacy empty piece behavior changed: %+v %v", gap, err)

		_, _, err = ExecuteStackProgram(*regs,
			[]byte{byte(DW_OP_reg0), byte(DW_OP_bit_piece), 3, 1}, 8, nil)
		c.Check(err != nil && errors.Is(err, ErrBitPieceUnsupported), "legacy register bit_piece must be explicitly rejected: %v", err)
		c.Check(strings.Contains(err.Error(), "DW_OP_bit_piece"), "legacy rejection names bit_piece: %v", err)

		_, _, err = ExecuteStackProgram(DwarfRegisters{},
			append(testAddrExpr(0x1000), byte(DW_OP_bit_piece), 3, 0), 8, nil)
		c.Check(err != nil && errors.Is(err, ErrBitPieceUnsupported), "legacy address bit_piece must be rejected: %v", err)

		// Existing address arithmetic, CFA, frame base, and deref paths feed
		// the same planner/evaluator.
		bregRegs := testRegisters(nil, DwarfRegisterFromUint64(0x2000))
		bregPlan, err := PlanLocationWrite(bregRegs, []byte{byte(DW_OP_breg1), 0x10}, 8, 8,
			[]byte{1}, []byte{0xff}, newTestMemory(map[uint64]byte{0x2010: 0}).read)
		c.Check(err == nil, "breg plan: %v", err)
		if bregPlan != nil {
			mws := bregPlan.MemoryWrites()
			c.Check(len(mws) == 1 && mws[0].Address == 0x2010, "breg address records = %+v", mws)
		}

		frameRegs := DwarfRegisters{FrameBase: 0x4000, ByteOrder: binary.LittleEndian}
		framePlan, err := PlanLocationWrite(frameRegs, []byte{byte(DW_OP_fbreg), 8}, 8, 8,
			[]byte{1}, []byte{0xff}, newTestMemory(map[uint64]byte{0x4008: 0}).read)
		c.Check(err == nil, "framebase plan: %v", err)
		if framePlan != nil {
			mws := framePlan.MemoryWrites()
			c.Check(len(mws) == 1 && mws[0].Address == 0x4008, "framebase address = %+v", mws)
		}

		cfaRegs := DwarfRegisters{CFA: 0x7000, ByteOrder: binary.LittleEndian}
		cfaPlan, err := PlanLocationWrite(cfaRegs, []byte{byte(DW_OP_call_frame_cfa)}, 8, 8,
			[]byte{1}, []byte{0xff}, newTestMemory(map[uint64]byte{0x7000: 0}).read)
		c.Check(err == nil, "CFA plan: %v", err)
		if cfaPlan != nil {
			mws := cfaPlan.MemoryWrites()
			c.Check(len(mws) == 1 && mws[0].Address == 0x7000, "CFA address = %+v", mws)
		}

		derefRead := func(buf []byte, addr uint64) (int, error) {
			if addr == 0x5000 && len(buf) == 8 {
				binary.LittleEndian.PutUint64(buf, 0x6000)
				return 8, nil
			}
			if addr == 0x6000 && len(buf) == 1 {
				buf[0] = 0
				return 1, nil
			}
			return 0, fmt.Errorf("unexpected deref read at %#x len %d", addr, len(buf))
		}
		derefPlan, err := PlanLocationWrite(DwarfRegisters{ByteOrder: binary.LittleEndian},
			append(testAddrExpr(0x5000), byte(DW_OP_deref)), 8, 8,
			[]byte{0x80}, []byte{0xff}, derefRead)
		c.Check(err == nil, "deref plan: %v", err)
		if derefPlan != nil {
			mws := derefPlan.MemoryWrites()
			if check(c, len(mws) == 1, "deref records = %d", len(mws)) {
				c.Check(mws[0].Address == 0x6000 && mws[0].Data[0] == 0x80, "deref write = %+v", mws[0])
			}
		}

		// Expression errors carry byte offsets.
		checkOffset := func(name string, expr []byte, want int) {
			_, planErr := PlanLocationWrite(DwarfRegisters{ByteOrder: binary.LittleEndian}, expr, 8, 8,
				[]byte{0}, []byte{1}, nil)
			off, ok := ExpressionOffset(planErr)
			c.Check(planErr != nil && ok && off == want, "%s error=%v offset=%d ok=%v", name, planErr, off, ok)
		}
		checkOffset("stack underflow", []byte{byte(DW_OP_dup)}, 0)
		checkOffset("truncated piece length", []byte{byte(DW_OP_piece), 0x80}, 0)
		overflowULEB := append([]byte{byte(DW_OP_piece)}, bytes.Repeat([]byte{0xff}, 9)...)
		overflowULEB = append(overflowULEB, 0x02)
		_, overflowErr := PlanLocationWrite(DwarfRegisters{}, overflowULEB, 8, 8, []byte{0}, []byte{1}, nil)
		c.Check(overflowErr != nil && errors.Is(overflowErr, ErrLengthOverflow), "length overflow error: %v", overflowErr)
		off, ok := ExpressionOffset(overflowErr)
		c.Check(ok && off == 0, "overflow offset = %d (%v)", off, overflowErr)
		checkOffset("illegal terminator", []byte{byte(DW_OP_reg0), 0x01}, 1)
		checkOffset("truncated bit-piece operands", []byte{byte(DW_OP_reg0), byte(DW_OP_bit_piece), 3, 0x80}, 1)
		checkOffset("step limit", []byte{byte(DW_OP_skip), 0xfd, 0xff}, 0)
		checkOffset("composite final length", []byte{byte(DW_OP_reg0), byte(DW_OP_piece), 1, byte(DW_OP_reg1)}, 3)

		// Zero-length pieces consume their operand and clear the stack.
		zeroExpr := []byte{byte(DW_OP_lit5), byte(DW_OP_piece), 0, byte(DW_OP_dup)}
		_, err = PlanLocationWrite(DwarfRegisters{}, zeroExpr, 8, 8, []byte{0}, []byte{1}, nil)
		c.Check(err != nil && errors.Is(err, ErrStackUnderflow), "zero piece must clear stack: %v", err)
		off, ok = ExpressionOffset(err)
		c.Check(ok && off == 3, "zero-piece underflow offset = %d (%v)", off, err)

		// Empty locations remain unavailable only when selected.
		_, err = PlanLocationWrite(DwarfRegisters{}, []byte{byte(DW_OP_piece), 1}, 8, 8,
			[]byte{0}, []byte{1}, nil)
		var unavailable *UnavailableBitError
		c.Check(errors.As(err, &unavailable), "selected empty piece: %v", err)
		emptyPlan, err := PlanLocationWrite(DwarfRegisters{}, []byte{byte(DW_OP_piece), 1}, 8, 8,
			[]byte{0}, []byte{0}, nil)
		c.Check(err == nil && emptyPlan != nil && len(emptyPlan.Writes()) == 0, "unselected empty piece: %v", err)

		emptyExprPlan, err := PlanLocationWrite(DwarfRegisters{}, nil, 8, 8,
			[]byte{0}, []byte{0}, nil)
		c.Check(err == nil && emptyExprPlan != nil && len(emptyExprPlan.Writes()) == 0,
			"empty expression with no selected bits should be an unavailable empty plan: %v", err)
		_, err = PlanLocationWrite(DwarfRegisters{}, nil, 8, 8,
			[]byte{0}, []byte{1}, nil)
		c.Check(errors.As(err, &unavailable), "selected empty expression: %v", err)

		// Short composites leave an unavailable tail; oversized pieces fail.
		_, err = PlanLocationWrite(testRegisters(DwarfRegisterFromUint64(0)),
			[]byte{byte(DW_OP_reg0), byte(DW_OP_piece), 1}, 8, 16,
			[]byte{0, 0}, []byte{0, 1}, nil)
		c.Check(err != nil, "selected tail must fail")
		shortPlan, err := PlanLocationWrite(testRegisters(DwarfRegisterFromUint64(0)),
			[]byte{byte(DW_OP_reg0), byte(DW_OP_piece), 1}, 8, 16,
			[]byte{0xaa, 0}, []byte{0xff, 0}, nil)
		c.Check(err == nil && shortPlan != nil && len(shortPlan.RegisterWrites()) == 1, "unselected tail plan: %v", err)
		_, err = PlanLocationWrite(testRegisters(DwarfRegisterFromUint64(0)),
			[]byte{byte(DW_OP_reg0), byte(DW_OP_piece), 2}, 8, 8,
			[]byte{0}, []byte{1}, nil)
		c.Check(err != nil, "oversized piece must fail")

		// Unselected missing sources are not loaded.
		missingExpr := []byte{byte(DW_OP_reg0), byte(DW_OP_bit_piece), 8, 0}
		missingExpr = append(missingExpr, byte(DW_OP_reg9), byte(DW_OP_bit_piece), 8, 0)
		missingPlan, err := PlanLocationWrite(testRegisters(DwarfRegisterFromUint64(0)), missingExpr, 8, 16,
			[]byte{0xaa, 0}, []byte{0xff, 0x00}, nil)
		c.Check(err == nil, "unselected missing register fragment: %v", err)
		if missingPlan != nil {
			c.Check(len(missingPlan.RegisterWrites()) == 1 && missingPlan.RegisterWrites()[0].RegNum == 0,
				"unselected fragment caused source access: %+v", missingPlan.RegisterWrites())
		}

		// Missing registers use the existing lazy-load callback.
		lazySlice := []*DwarfRegister{DwarfRegisterFromUint64(0), nil, nil, nil, nil}
		lazyRegs := NewDwarfRegisters(0, lazySlice, binary.LittleEndian, 0, 0, 0, 0)
		lazyLoads := 0
		lazyRegs.SetLoadMoreCallback(func() {
			lazyLoads++
			lazyRegs.AddReg(4, byteRegister(0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0))
		})
		lazyPlan, err := PlanLocationWrite(*lazyRegs,
			[]byte{byte(DW_OP_reg4), byte(DW_OP_piece), 1}, 8, 8,
			[]byte{0x55}, []byte{0xff}, nil)
		c.Check(err == nil, "lazy register plan: %v", err)
		c.Check(lazyLoads > 0, "lazy register loader was not used")
		if lazyPlan != nil {
			rws := lazyPlan.RegisterWrites()
			if check(c, len(rws) == 1, "lazy register records = %d", len(rws)) {
				c.Check(rws[0].RegNum == 4 && rws[0].Data[0] == 0x55 && rws[0].Data[1] == 0x34,
					"lazy register data = % x", rws[0].Data)
			}
		}

		// A selected missing register is still an error.
		_, err = PlanLocationWrite(DwarfRegisters{}, []byte{byte(DW_OP_reg3)}, 8, 8,
			[]byte{0}, []byte{0xff}, nil)
		var missingReg *MissingRegisterError
		c.Check(errors.As(err, &missingReg), "selected missing register: %v", err)

		// Unselected memory fragments do not issue preparation reads.
		unselectedMemExpr := []byte{byte(DW_OP_reg0), byte(DW_OP_piece), 1}
		unselectedMemExpr = append(unselectedMemExpr, testAddrExpr(0x4444)...)
		unselectedMemExpr = append(unselectedMemExpr, byte(DW_OP_piece), 1)
		noRead := func([]byte, uint64) (int, error) {
			return 0, errors.New("unselected memory source was read")
		}
		unselectedMemPlan, err := PlanLocationWrite(testRegisters(DwarfRegisterFromUint64(0)), unselectedMemExpr, 8, 16,
			[]byte{0xaa, 0}, []byte{0xff, 0x00}, noRead)
		c.Check(err == nil && unselectedMemPlan != nil, "unselected memory fragment: %v", err)

		prepShortRead := func([]byte, uint64) (int, error) { return 0, nil }
		_, err = PlanLocationWrite(DwarfRegisters{ByteOrder: binary.LittleEndian},
			append(testAddrExpr(0x4545), byte(DW_OP_piece), 1), 8, 8,
			[]byte{1}, []byte{1}, prepShortRead)
		c.Check(err != nil && strings.Contains(err.Error(), "short read preparing memory"),
			"preparation short read: %v", err)

		// ptrSize 4 is supported for DW_OP_addr.
		addr32Expr := []byte{byte(DW_OP_addr), 0x00, 0x20, 0x00, 0x00, byte(DW_OP_piece), 1}
		plan32, err := PlanLocationWrite(DwarfRegisters{ByteOrder: binary.LittleEndian}, addr32Expr, 4, 8,
			[]byte{0x5d}, []byte{0xff}, newTestMemory(map[uint64]byte{0x2000: 0}).read)
		c.Check(err == nil, "ptrSize 4 plan: %v", err)
		if plan32 != nil {
			mws := plan32.MemoryWrites()
			c.Check(len(mws) == 1 && mws[0].Address == 0x2000, "ptrSize 4 records = %+v", mws)
		}

		// implicit_value is checked as an immutable LSB-packed constant.
		implicitExpr := []byte{byte(DW_OP_implicit_value), 2, 0xca, 0xfe}
		implicitPlan, err := PlanLocationWrite(DwarfRegisters{}, implicitExpr, 8, 16,
			[]byte{0xca, 0xfe}, []byte{0xff, 0xff}, nil)
		c.Check(err == nil && implicitPlan != nil && len(implicitPlan.Writes()) == 0,
			"matching implicit_value: %v", err)
		_, err = PlanLocationWrite(DwarfRegisters{}, implicitExpr, 8, 16,
			[]byte{0xcb, 0xfe}, []byte{0xff, 0xff}, nil)
		var implicitErr *ConstantMismatchError
		c.Check(errors.As(err, &implicitErr), "changed implicit_value must be rejected: %v", err)
		implicitOverExpr := []byte{byte(DW_OP_implicit_value), 2, 0xca, 0xfe,
			byte(DW_OP_bit_piece), 1, 16}
		_, err = PlanLocationWrite(DwarfRegisters{}, implicitOverExpr, 8, 1,
			[]byte{1}, []byte{1}, nil)
		c.Check(err != nil, "implicit_value source beyond width must fail")

		// Dereference short reads are location errors and carry an offset.
		shortDerefRead := func(buf []byte, addr uint64) (int, error) {
			if addr == 0x5000 && len(buf) == 8 {
				return 3, nil
			}
			return 0, fmt.Errorf("unexpected read %#x/%d", addr, len(buf))
		}
		_, err = PlanLocationWrite(DwarfRegisters{}, append(testAddrExpr(0x5000), byte(DW_OP_deref)), 8, 8,
			[]byte{1}, []byte{1}, shortDerefRead)
		c.Check(err != nil && strings.Contains(err.Error(), "short read"), "short deref read: %v", err)
		off, ok = ExpressionOffset(err)
		c.Check(ok && off == len(testAddrExpr(0)), "short deref offset=%d ok=%v err=%v", off, ok, err)

		// Address arithmetic overflow is rejected during preparation.
		maxAddr := bytes.Repeat([]byte{0xff}, 8)
		overflowAddrExpr := append([]byte{byte(DW_OP_addr)}, maxAddr...)
		overflowAddrExpr = append(overflowAddrExpr, byte(DW_OP_bit_piece), 1, 8)
		_, err = PlanLocationWrite(DwarfRegisters{}, overflowAddrExpr, 8, 1,
			[]byte{0}, []byte{1}, nil)
		c.Check(err != nil && strings.Contains(err.Error(), "address overflow"), "address overflow: %v", err)

		// Input register Bytes are not synthesized/cached by planning.
		uintRegs := testRegisters(DwarfRegisterFromUint64(0x1122334455667788))
		c.Check(uintRegs.Reg(0).Bytes == nil, "test setup should have nil Bytes")
		_, err = PlanLocationWrite(uintRegs, []byte{byte(DW_OP_reg0), byte(DW_OP_bit_piece), 16, 0}, 8, 16,
			[]byte{0x34, 0x12}, []byte{0xff, 0xff}, nil)
		c.Check(err == nil, "uint64 register plan: %v", err)
		c.Check(uintRegs.Reg(0).Bytes == nil, "Plan mutated input register Bytes")

		// Argument validation and supported range.
		_, err = PlanLocationWrite(DwarfRegisters{}, nil, 2, 8, []byte{0}, []byte{1}, nil)
		c.Check(err != nil, "ptrSize 2 must be rejected")
		_, err = PlanLocationWrite(DwarfRegisters{}, nil, 8, MaxLocationWriteBits+1, nil, nil, nil)
		c.Check(err != nil, "bitSize over maximum must be rejected")
		_, err = PlanLocationWrite(DwarfRegisters{}, nil, 8, 8, []byte{}, []byte{1}, nil)
		c.Check(err != nil, "short data must be rejected")
		_, err = PlanLocationWrite(DwarfRegisters{}, nil, 8, 9, []byte{0, 0}, []byte{0, 0x02}, nil)
		c.Check(err != nil, "nonzero mask padding must be rejected")
		zeroPlan, err := PlanLocationWrite(DwarfRegisters{}, []byte{byte(DW_OP_reg0)}, 8, 0, nil, nil, nil)
		c.Check(err == nil && zeroPlan != nil && len(zeroPlan.Writes()) == 0, "zero-bit plan: %v", err)
	})
}
