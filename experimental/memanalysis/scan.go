package memanalysis

import (
	"bytes"
	"fmt"

	"github.com/tetratelabs/wazero/api"
	"github.com/tetratelabs/wazero/internal/leb128"
	"github.com/tetratelabs/wazero/internal/wasm"
)

// writeSite is a single store, memory.copy or memory.fill instruction found
// while scanning a function body.
type writeSite struct {
	function uint32
	bodyOff  uint32
	opcode   string
}

// scanBodies walks every instruction of every defined function, including
// instructions encoded in unreachable positions, and returns all write
// instruction sites. Anything outside the analyzer's supported instruction
// subset is rejected here, before a single sample call can execute.
func scanBodies(module *wasm.Module) ([]writeSite, error) {
	var sites []writeSite
	for funcIndex := range module.CodeSection {
		body := module.CodeSection[funcIndex].Body
		pc := 0
		for pc < len(body) {
			op := body[pc]
			bodyOff := uint32(pc)
			switch {
			case op == 0x00 || op == 0x01 || op == 0x05 || op == 0x0b || op == 0x0f || op == 0x1a || op == 0x1b:
				// unreachable, nop, else, end, return, drop, select: no immediates.
				pc++
			case op == 0x1c:
				// typed select: a one-element vector of value types.
				count, n, err := leb128.LoadUint32(body[pc+1:])
				if err != nil || count != 1 || pc+1+int(n) >= len(body) {
					return nil, fmt.Errorf("function %d: malformed typed select at body offset %d", funcIndex, bodyOff)
				}
				if body[pc+1+int(n)] != wasm.ValueTypeI32 {
					return nil, fmt.Errorf("function %d: only i32 typed select is supported at body offset %d", funcIndex, bodyOff)
				}
				pc += 1 + int(n) + 1
			case op == 0x02 || op == 0x03 || op == 0x04:
				// block, loop, if: decode the block type so type-index forms
				// are skipped correctly. The i32-only shape validation already
				// guarantees the referenced signature is void or single i32.
				r := bytes.NewReader(body[pc+1:])
				bt, num, decodeErr := wasm.DecodeBlockType(module.TypeSection, r, api.CoreFeaturesV2)
				if decodeErr != nil {
					return nil, fmt.Errorf("function %d: malformed block type at body offset %d: %w", funcIndex, bodyOff, decodeErr)
				}
				for _, p := range bt.Params {
					if p != wasm.ValueTypeI32 {
						return nil, fmt.Errorf("function %d: unsupported block parameter at body offset %d", funcIndex, bodyOff)
					}
				}
				for _, res := range bt.Results {
					if res != wasm.ValueTypeI32 {
						return nil, fmt.Errorf("function %d: unsupported block result at body offset %d", funcIndex, bodyOff)
					}
				}
				pc += 1 + int(num)
			case op == 0x0c || op == 0x0d:
				// br, br_if: label index.
				n, err := skipU32(body, pc+1)
				if err != nil {
					return nil, fmt.Errorf("function %d: malformed branch at body offset %d: %w", funcIndex, bodyOff, err)
				}
				pc = n
			case op == 0x0e:
				// br_table: a vector of label indices followed by the default.
				count, n, err := leb128.LoadUint32(body[pc+1:])
				if err != nil {
					return nil, fmt.Errorf("function %d: malformed br_table at body offset %d: %w", funcIndex, bodyOff, err)
				}
				pc = pc + 1 + int(n)
				for k := uint32(0); k <= count; k++ {
					pc, err = skipU32(body, pc)
					if err != nil {
						return nil, fmt.Errorf("function %d: malformed br_table label at body offset %d: %w", funcIndex, bodyOff, err)
					}
				}
			case op == 0x10:
				// direct call: function index.
				target, n, err := leb128.LoadUint32(body[pc+1:])
				if err != nil {
					return nil, fmt.Errorf("function %d: malformed call at body offset %d: %w", funcIndex, bodyOff, err)
				}
				if int(target) >= len(module.FunctionSection) {
					return nil, fmt.Errorf("function %d: call at body offset %d targets unknown function %d", funcIndex, bodyOff, target)
				}
				pc += 1 + int(n)
			case op == 0x20 || op == 0x21 || op == 0x22 || op == 0x23 || op == 0x24:
				// local.get/set/tee, global.get/set: one index.
				n, err := skipU32(body, pc+1)
				if err != nil {
					return nil, fmt.Errorf("function %d: malformed variable instruction at body offset %d: %w", funcIndex, bodyOff, err)
				}
				pc = n
			case op == 0x28 || op == 0x2c || op == 0x2d || op == 0x2e || op == 0x2f:
				// i32.load and the narrow i32 loads.
				n, err := skipMemArg(body, pc+1)
				if err != nil {
					return nil, fmt.Errorf("function %d: malformed load at body offset %d: %w", funcIndex, bodyOff, err)
				}
				pc = n
			case op == 0x36 || op == 0x3a || op == 0x3b:
				n, err := skipMemArg(body, pc+1)
				if err != nil {
					return nil, fmt.Errorf("function %d: malformed store at body offset %d: %w", funcIndex, bodyOff, err)
				}
				sites = append(sites, writeSite{
					function: uint32(funcIndex),
					bodyOff:  bodyOff,
					opcode:   storeOpcodeName(op),
				})
				pc = n
			case op == 0x3f || op == 0x40:
				// memory.size, memory.grow: one reserved zero byte.
				if pc+1 >= len(body) || body[pc+1] != 0x00 {
					return nil, fmt.Errorf("function %d: malformed memory instruction at body offset %d", funcIndex, bodyOff)
				}
				pc += 2
			case op == 0x41:
				// i32.const: signed LEB128.
				if _, n, err := leb128.LoadInt32(body[pc+1:]); err != nil {
					return nil, fmt.Errorf("function %d: malformed i32.const at body offset %d: %w", funcIndex, bodyOff, err)
				} else {
					pc += 1 + int(n)
				}
			case (op >= 0x45 && op <= 0x4f) || (op >= 0x67 && op <= 0x78):
				// The complete i32 numeric family: comparisons (0x45-0x4f),
				// unary integer ops (0x67-0x69) and arithmetic, bitwise,
				// shift and rotate instructions (0x6a-0x78). i64 and float
				// opcodes fall outside these ranges and are rejected.
				pc++
			case op == 0xfc:
				n, err := scanMisc(body, pc, funcIndex, &sites)
				if err != nil {
					return nil, err
				}
				pc = n
			default:
				return nil, fmt.Errorf("function %d: unsupported opcode 0x%02x at body offset %d", funcIndex, op, bodyOff)
			}
		}
	}
	return sites, nil
}

// scanMisc handles the multi-byte 0xfc prefix, accepting only memory.copy and
// memory.fill, both of which are write instructions.
func scanMisc(body []byte, pc, funcIndex int, sites *[]writeSite) (int, error) {
	bodyOff := uint32(pc)
	sub, n, err := leb128.LoadUint32(body[pc+1:])
	if err != nil {
		return 0, fmt.Errorf("function %d: malformed misc prefix at body offset %d: %w", funcIndex, bodyOff, err)
	}
	next := pc + 1 + int(n)
	switch sub {
	case 0x0a: // memory.copy: two reserved zero bytes.
		if next+2 > len(body) || body[next] != 0 || body[next+1] != 0 {
			return 0, fmt.Errorf("function %d: malformed memory.copy at body offset %d", funcIndex, bodyOff)
		}
		*sites = append(*sites, writeSite{function: uint32(funcIndex), bodyOff: bodyOff, opcode: wasm.OpcodeMemoryCopyName})
		return next + 2, nil
	case 0x0b: // memory.fill: one reserved zero byte.
		if next+1 > len(body) || body[next] != 0 {
			return 0, fmt.Errorf("function %d: malformed memory.fill at body offset %d", funcIndex, bodyOff)
		}
		*sites = append(*sites, writeSite{function: uint32(funcIndex), bodyOff: bodyOff, opcode: wasm.OpcodeMemoryFillName})
		return next + 1, nil
	default:
		return 0, fmt.Errorf("function %d: unsupported misc opcode 0x%x at body offset %d", funcIndex, sub, bodyOff)
	}
}

func skipU32(body []byte, pc int) (int, error) {
	_, n, err := leb128.LoadUint32(body[pc:])
	if err != nil {
		return 0, err
	}
	return pc + int(n), nil
}

func skipMemArg(body []byte, pc int) (int, error) {
	var err error
	if pc, err = skipU32(body, pc); err != nil {
		return 0, err
	}
	return skipU32(body, pc)
}

func storeOpcodeName(op byte) string {
	switch op {
	case 0x36:
		return wasm.OpcodeI32StoreName
	case 0x3a:
		return wasm.OpcodeI32Store8Name
	case 0x3b:
		return wasm.OpcodeI32Store16Name
	default:
		panic("unreachable")
	}
}
