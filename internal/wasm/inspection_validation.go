package wasm

import (
	"fmt"

	"github.com/tetratelabs/wazero/internal/leb128"
)

// Inspection-only instruction handling. The ordinary validation loop in
// func_validation.go calls these helpers when the module was decoded with
// DecodeModuleForInspection; every immediate is read from the same body cursor
// the validator is walking, so the recorded offsets describe real bytes.

// memoryIndexAlignBit is bit 6 of the encoded alignment, which marks the
// presence of an explicit memory index immediate under the multi-memory
// encoding.
const memoryIndexAlignBit = uint32(0x40)

// isVecMemoryInspectionOpcode reports whether a vector suffix opcode is a
// linear-memory access (loads, stores, splat/zero/lane load variants). SIMD
// memory access is rejected on the inspection path.
func isVecMemoryInspectionOpcode(vecOpcode byte) bool {
	switch {
	case vecOpcode <= 0x0b: // 0x00..0x0a loads and 0x0b v128.store
		return true
	case vecOpcode >= 0x54 && vecOpcode <= 0x5d: // lane load/store and load32/64_zero
		return true
	}
	return false
}

// readInspectionMemArg reads a multi-memory/memory64 memarg: an alignment
// whose bit 6 indicates an following memory index, then an unsigned LEB128
// offset kept at its full 64-bit width. The returned pc points at the last
// consumed immediate byte.
func readInspectionMemArg(pc uint64, body []byte) (memIndex uint32, align uint32, offset uint64, nextPC uint64, err error) {
	rawAlign, num, err := leb128.LoadUint32(body[pc:])
	if err != nil {
		err = fmt.Errorf("read memory align: %v", err)
		return
	}
	pc += uint64(num)
	if rawAlign&memoryIndexAlignBit != 0 {
		align = rawAlign &^ memoryIndexAlignBit
		memIndex, num, err = leb128.LoadUint32(body[pc:])
		if err != nil {
			err = fmt.Errorf("read memory index: %v", err)
			return
		}
		pc += uint64(num)
	} else {
		align = rawAlign
	}

	offset, num, err = leb128.LoadUint64(body[pc:])
	if err != nil {
		err = fmt.Errorf("read memory offset: %v", err)
		return
	}
	pc += uint64(num) - 1
	return memIndex, align, offset, pc, nil
}

// inspectionMemory resolves and range-checks a memory index against the
// combined declaration space, returning its address value type.
func (m *Module) inspectionMemory(memIndex uint32) (ValueType, error) {
	if m.inspection == nil || memIndex >= uint32(len(m.inspection.declarations)) {
		return 0, fmt.Errorf("unknown memory index %d", memIndex)
	}
	if m.inspection.declarations[memIndex].Is64 {
		return ValueTypeI64, nil
	}
	return ValueTypeI32, nil
}

// checkInspectionOffset verifies that a static offset fits the address width
// of the target memory. Offsets may legitimately exceed 4GiB on memory64.
func checkInspectionOffset(memIndex uint32, is64 bool, offset uint64) error {
	if !is64 && offset > uint64(0xFFFFFFFF) {
		return fmt.Errorf("memory[%d] is 32-bit but memarg offset %d exceeds 2^32-1", memIndex, offset)
	}
	return nil
}

func (m *Module) recordMemoryUse(funcIndex, bodyOffset uint32, opcode string, memoryIndices []uint32, offset uint64) {
	m.appendMemoryUse(MemoryUse{
		FunctionIndex: funcIndex,
		BodyOffset:    bodyOffset,
		Opcode:        opcode,
		MemoryIndices: append([]uint32(nil), memoryIndices...),
		Offset:        offset,
	})
}

// inspectScalarMemory handles OpcodeI32Load..OpcodeI64Store32 on the
// inspection path, including the full memarg decoding, address-type checking
// and the same alignment/stack effects as the ordinary path. pc points at the
// opcode byte; the returned pc points at the last consumed byte.
func (m *Module) inspectScalarMemory(
	op Opcode, pc uint64, body []byte, funcIndex Index, valueTypeStack *valueTypeStack,
) (uint64, error) {
	bodyOffset := uint32(pc)
	memIndex, align, offset, nextPC, err := readInspectionMemArg(pc+1, body)
	if err != nil {
		return pc, err
	}
	addrType, err := m.inspectionMemory(memIndex)
	if err != nil {
		return pc, err
	}
	is64 := addrType == ValueTypeI64
	if err = checkInspectionOffset(memIndex, is64, offset); err != nil {
		return pc, err
	}

	if err = m.scalarMemoryStackEffect(op, align, addrType, valueTypeStack); err != nil {
		return pc, err
	}

	m.recordMemoryUse(uint32(funcIndex), bodyOffset, InstructionName(op), []uint32{memIndex}, offset)
	return nextPC, nil
}

// scalarMemoryStackEffect reproduces the alignment checks and operand/result
// stack effects of the scalar load/store family, with a parameterized address
// type so memory64 uses i64 addresses.
func (m *Module) scalarMemoryStackEffect(
	op Opcode, align uint32, addrType ValueType, valueTypeStack *valueTypeStack,
) error {
	switch op {
	case OpcodeI32Load:
		if 1<<align > 32/8 {
			return fmt.Errorf("invalid memory alignment")
		}
		if err := valueTypeStack.popAndVerifyType(addrType); err != nil {
			return err
		}
		valueTypeStack.push(ValueTypeI32)
	case OpcodeF32Load:
		if 1<<align > 32/8 {
			return fmt.Errorf("invalid memory alignment")
		}
		if err := valueTypeStack.popAndVerifyType(addrType); err != nil {
			return err
		}
		valueTypeStack.push(ValueTypeF32)
	case OpcodeI32Store:
		if 1<<align > 32/8 {
			return fmt.Errorf("invalid memory alignment")
		}
		if err := valueTypeStack.popAndVerifyType(ValueTypeI32); err != nil {
			return err
		}
		if err := valueTypeStack.popAndVerifyType(addrType); err != nil {
			return err
		}
	case OpcodeF32Store:
		if 1<<align > 32/8 {
			return fmt.Errorf("invalid memory alignment")
		}
		if err := valueTypeStack.popAndVerifyType(ValueTypeF32); err != nil {
			return err
		}
		if err := valueTypeStack.popAndVerifyType(addrType); err != nil {
			return err
		}
	case OpcodeI64Load:
		if 1<<align > 64/8 {
			return fmt.Errorf("invalid memory alignment")
		}
		if err := valueTypeStack.popAndVerifyType(addrType); err != nil {
			return err
		}
		valueTypeStack.push(ValueTypeI64)
	case OpcodeF64Load:
		if 1<<align > 64/8 {
			return fmt.Errorf("invalid memory alignment")
		}
		if err := valueTypeStack.popAndVerifyType(addrType); err != nil {
			return err
		}
		valueTypeStack.push(ValueTypeF64)
	case OpcodeI64Store:
		if 1<<align > 64/8 {
			return fmt.Errorf("invalid memory alignment")
		}
		if err := valueTypeStack.popAndVerifyType(ValueTypeI64); err != nil {
			return err
		}
		if err := valueTypeStack.popAndVerifyType(addrType); err != nil {
			return err
		}
	case OpcodeF64Store:
		if 1<<align > 64/8 {
			return fmt.Errorf("invalid memory alignment")
		}
		if err := valueTypeStack.popAndVerifyType(ValueTypeF64); err != nil {
			return err
		}
		if err := valueTypeStack.popAndVerifyType(addrType); err != nil {
			return err
		}
	case OpcodeI32Load8S, OpcodeI32Load8U:
		if 1<<align > 1 {
			return fmt.Errorf("invalid memory alignment")
		}
		if err := valueTypeStack.popAndVerifyType(addrType); err != nil {
			return err
		}
		valueTypeStack.push(ValueTypeI32)
	case OpcodeI64Load8S, OpcodeI64Load8U:
		if 1<<align > 1 {
			return fmt.Errorf("invalid memory alignment")
		}
		if err := valueTypeStack.popAndVerifyType(addrType); err != nil {
			return err
		}
		valueTypeStack.push(ValueTypeI64)
	case OpcodeI32Store8:
		if 1<<align > 1 {
			return fmt.Errorf("invalid memory alignment")
		}
		if err := valueTypeStack.popAndVerifyType(ValueTypeI32); err != nil {
			return err
		}
		if err := valueTypeStack.popAndVerifyType(addrType); err != nil {
			return err
		}
	case OpcodeI64Store8:
		if 1<<align > 1 {
			return fmt.Errorf("invalid memory alignment")
		}
		if err := valueTypeStack.popAndVerifyType(ValueTypeI64); err != nil {
			return err
		}
		if err := valueTypeStack.popAndVerifyType(addrType); err != nil {
			return err
		}
	case OpcodeI32Load16S, OpcodeI32Load16U:
		if 1<<align > 16/8 {
			return fmt.Errorf("invalid memory alignment")
		}
		if err := valueTypeStack.popAndVerifyType(addrType); err != nil {
			return err
		}
		valueTypeStack.push(ValueTypeI32)
	case OpcodeI64Load16S, OpcodeI64Load16U:
		if 1<<align > 16/8 {
			return fmt.Errorf("invalid memory alignment")
		}
		if err := valueTypeStack.popAndVerifyType(addrType); err != nil {
			return err
		}
		valueTypeStack.push(ValueTypeI64)
	case OpcodeI32Store16:
		if 1<<align > 16/8 {
			return fmt.Errorf("invalid memory alignment")
		}
		if err := valueTypeStack.popAndVerifyType(ValueTypeI32); err != nil {
			return err
		}
		if err := valueTypeStack.popAndVerifyType(addrType); err != nil {
			return err
		}
	case OpcodeI64Store16:
		if 1<<align > 16/8 {
			return fmt.Errorf("invalid memory alignment")
		}
		if err := valueTypeStack.popAndVerifyType(ValueTypeI64); err != nil {
			return err
		}
		if err := valueTypeStack.popAndVerifyType(addrType); err != nil {
			return err
		}
	case OpcodeI64Load32S, OpcodeI64Load32U:
		if 1<<align > 32/8 {
			return fmt.Errorf("invalid memory alignment")
		}
		if err := valueTypeStack.popAndVerifyType(addrType); err != nil {
			return err
		}
		valueTypeStack.push(ValueTypeI64)
	case OpcodeI64Store32:
		if 1<<align > 32/8 {
			return fmt.Errorf("invalid memory alignment")
		}
		if err := valueTypeStack.popAndVerifyType(ValueTypeI64); err != nil {
			return err
		}
		if err := valueTypeStack.popAndVerifyType(addrType); err != nil {
			return err
		}
	default:
		return fmt.Errorf("invalid scalar memory instruction 0x%x", op)
	}
	return nil
}

// inspectMemorySizeGrow handles memory.size and memory.grow with a full
// unsigned LEB128 memory index immediate. The operand/result type follows the
// selected memory: i32 for memory32, i64 for memory64.
func (m *Module) inspectMemorySizeGrow(
	op Opcode, pc uint64, body []byte, funcIndex Index, valueTypeStack *valueTypeStack,
) (uint64, error) {
	bodyOffset := uint32(pc)
	memIndex, num, err := leb128.LoadUint32(body[pc+1:])
	if err != nil {
		return pc, fmt.Errorf("read immediate: %v", err)
	}
	addrType, err := m.inspectionMemory(memIndex)
	if err != nil {
		return pc, err
	}
	switch op {
	case OpcodeMemoryGrow:
		if err = valueTypeStack.popAndVerifyType(addrType); err != nil {
			return pc, err
		}
		valueTypeStack.push(addrType)
	case OpcodeMemorySize:
		valueTypeStack.push(addrType)
	}
	m.recordMemoryUse(uint32(funcIndex), bodyOffset, InstructionName(op), []uint32{memIndex}, 0)
	return pc + uint64(num), nil
}

// inspectBulkMemory handles memory.init, memory.copy and memory.fill with full
// memory index immediates and memory64-aware operand types. pc points at the
// misc opcode byte following the 0xfc prefix, and the returned pc points at
// the last consumed byte.
func (m *Module) inspectBulkMemory(
	miscOpcode OpcodeMisc, pc uint64, body []byte, funcIndex Index, valueTypeStack *valueTypeStack,
) (uint64, error) {
	bodyOffset := uint32(pc - 1) // the 0xfc prefix is the instruction's first byte
	cursor := pc + 1             // first immediate byte

	switch miscOpcode {
	case OpcodeMiscMemoryInit:
		if m.DataCountSection == nil {
			return pc, fmt.Errorf("%s requires data count section", MiscInstructionName(miscOpcode))
		}
		dataIndex, num, err := leb128.LoadUint32(body[cursor:])
		if err != nil {
			return pc, fmt.Errorf("failed to read data segment index for %s: %v", MiscInstructionName(miscOpcode), err)
		}
		if int(dataIndex) >= len(m.DataSection) {
			return pc, fmt.Errorf("index %d out of range of data section(len=%d)", dataIndex, len(m.DataSection))
		}
		cursor += uint64(num)

		memIndex, num, err := leb128.LoadUint32(body[cursor:])
		if err != nil {
			return pc, fmt.Errorf("failed to read memory index for %s: %v", MiscInstructionName(miscOpcode), err)
		}
		cursor += uint64(num)
		addrType, err := m.inspectionMemory(memIndex)
		if err != nil {
			return pc, err
		}
		// Destination address follows the target memory; source offset and size stay i32.
		if err = valueTypeStack.popAndVerifyType(ValueTypeI32); err != nil { // size n
			return pc, err
		}
		if err = valueTypeStack.popAndVerifyType(ValueTypeI32); err != nil { // source offset s
			return pc, err
		}
		if err = valueTypeStack.popAndVerifyType(addrType); err != nil { // destination address
			return pc, err
		}
		m.recordMemoryUse(uint32(funcIndex), bodyOffset, MiscInstructionName(miscOpcode), []uint32{memIndex}, 0)
		return cursor - 1, nil

	case OpcodeMiscMemoryCopy:
		dstIndex, num, err := leb128.LoadUint32(body[cursor:])
		if err != nil {
			return pc, fmt.Errorf("failed to read destination memory index for %s: %v", MiscInstructionName(miscOpcode), err)
		}
		cursor += uint64(num)
		dstType, err := m.inspectionMemory(dstIndex)
		if err != nil {
			return pc, err
		}
		srcIndex, num, err := leb128.LoadUint32(body[cursor:])
		if err != nil {
			return pc, fmt.Errorf("failed to read source memory index for %s: %v", MiscInstructionName(miscOpcode), err)
		}
		cursor += uint64(num)
		srcType, err := m.inspectionMemory(srcIndex)
		if err != nil {
			return pc, err
		}
		// The copy length uses the narrower of the two address types: i32 if
		// either memory is 32-bit, i64 only when both are 64-bit.
		lengthType := ValueTypeI64
		if dstType == ValueTypeI32 || srcType == ValueTypeI32 {
			lengthType = ValueTypeI32
		}
		if err = valueTypeStack.popAndVerifyType(lengthType); err != nil { // size n
			return pc, err
		}
		if err = valueTypeStack.popAndVerifyType(srcType); err != nil { // source address
			return pc, err
		}
		if err = valueTypeStack.popAndVerifyType(dstType); err != nil { // destination address
			return pc, err
		}
		m.recordMemoryUse(uint32(funcIndex), bodyOffset, MiscInstructionName(miscOpcode),
			[]uint32{dstIndex, srcIndex}, 0)
		return cursor - 1, nil

	case OpcodeMiscMemoryFill:
		memIndex, num, err := leb128.LoadUint32(body[cursor:])
		if err != nil {
			return pc, fmt.Errorf("failed to read memory index for %s: %v", MiscInstructionName(miscOpcode), err)
		}
		cursor += uint64(num)
		addrType, err := m.inspectionMemory(memIndex)
		if err != nil {
			return pc, err
		}
		// The fill value stays i32; address and length follow the target memory.
		if err = valueTypeStack.popAndVerifyType(addrType); err != nil { // size n
			return pc, err
		}
		if err = valueTypeStack.popAndVerifyType(ValueTypeI32); err != nil { // fill value
			return pc, err
		}
		if err = valueTypeStack.popAndVerifyType(addrType); err != nil { // destination address
			return pc, err
		}
		m.recordMemoryUse(uint32(funcIndex), bodyOffset, MiscInstructionName(miscOpcode), []uint32{memIndex}, 0)
		return cursor - 1, nil

	default:
		return pc, fmt.Errorf("invalid bulk memory opcode %#x", miscOpcode)
	}
}
