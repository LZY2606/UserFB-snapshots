package op

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"io"

	"github.com/go-delve/delve/pkg/dwarf"
	"github.com/go-delve/delve/pkg/dwarf/leb128"
)

// Opcode represent a DWARF stack program instruction.
// See ./opcodes.go for a full list.
type Opcode byte

//go:generate go run ../../../_scripts/gen-opcodes.go opcodes.table opcodes.go

type stackfn func(Opcode, *context) error

type ReadMemoryFunc func([]byte, uint64) (int, error)

// BaseType describes a DWARF base type as far as the typed stack machine is
// concerned: its size in bytes and its signedness.
type BaseType struct {
	ByteSize int
	Signed   bool
}

// BaseTypeResolver resolves a CU-relative type DIE offset (as used by
// DW_OP_const_type, DW_OP_convert and DW_OP_reinterpret) to a BaseType.
// Offset 0 is never passed to the resolver, it denotes the generic type.
type BaseTypeResolver func(uint64) (BaseType, error)

// genericType is the type stack entry used by values that have no base type
// information (the "generic type" of the DWARF specification).
const genericType = 0

type context struct {
	buf     *bytes.Buffer
	prog    []byte
	stack   []int64
	types   []uint64 // parallel to stack, each entry is a CU-relative type DIE offset or genericType
	pieces  []Piece
	ptrSize int

	DwarfRegisters
	readMemory ReadMemoryFunc
	resolve    BaseTypeResolver
}

// Piece is a piece of memory stored either at an address or in a register.
type Piece struct {
	Size  int
	Kind  PieceKind
	Val   uint64
	Bytes []byte
}

// PieceKind describes the kind of a piece.
type PieceKind uint8

const (
	AddrPiece PieceKind = iota // The piece is stored in memory, Val is the address
	RegPiece                   // The piece is stored in a register, Val is the register number
	ImmPiece                   // The piece is an immediate value, Val or Bytes is the value
)

var (
	ErrStackUnderflow        = errors.New("DWARF stack underflow")
	ErrStackIndexOutOfBounds = errors.New("DWARF stack index out of bounds")
	ErrMemoryReadUnavailable = errors.New("memory read unavailable")
	ErrDivisionByZero        = errors.New("DWARF stack division by zero")
)

const arbitraryExecutionLimitFactor = 10

// ExecuteStackProgram executes a DWARF location expression and returns
// either an address (int64), or a slice of Pieces for location expressions
// that don't evaluate to an address (such as register and composite expressions).
func ExecuteStackProgram(regs DwarfRegisters, instructions []byte, ptrSize int, readMemory ReadMemoryFunc) (int64, []Piece, error) {
	return executeStackProgram(regs, instructions, ptrSize, readMemory, nil)
}

// ExecuteStackProgramTyped executes a DWARF location expression like
// ExecuteStackProgram but additionally tracks the base type of every value
// on the stack. resolve is used to resolve the CU-relative type DIE offsets
// of DW_OP_const_type, DW_OP_convert and DW_OP_reinterpret to base types;
// it may be nil, in which case only the generic type (offset 0) is available.
func ExecuteStackProgramTyped(regs DwarfRegisters, instructions []byte, ptrSize int, readMemory ReadMemoryFunc, resolve BaseTypeResolver) (int64, []Piece, error) {
	return executeStackProgram(regs, instructions, ptrSize, readMemory, resolve)
}

func executeStackProgram(regs DwarfRegisters, instructions []byte, ptrSize int, readMemory ReadMemoryFunc, resolve BaseTypeResolver) (int64, []Piece, error) {
	ctxt := &context{
		buf:            bytes.NewBuffer(instructions),
		prog:           instructions,
		stack:          make([]int64, 0, 3),
		types:          make([]uint64, 0, 3),
		DwarfRegisters: regs,
		ptrSize:        ptrSize,
		readMemory:     readMemory,
		resolve:        resolve,
	}

	for tick := 0; tick < len(instructions)*arbitraryExecutionLimitFactor; tick++ {
		opcodeByte, err := ctxt.buf.ReadByte()
		if err != nil {
			break
		}
		opcode := Opcode(opcodeByte)
		if opcode == DW_OP_nop {
			continue
		}
		fn, ok := oplut[opcode]
		if !ok {
			return 0, nil, fmt.Errorf("invalid instruction %#v", opcode)
		}

		err = fn(opcode, ctxt)
		if err != nil {
			return 0, nil, err
		}
	}

	if ctxt.pieces != nil {
		if len(ctxt.pieces) == 1 && ctxt.pieces[0].Kind == RegPiece {
			return int64(regs.Uint64Val(ctxt.pieces[0].Val)), ctxt.pieces, nil
		}
		return 0, ctxt.pieces, nil
	}

	if len(ctxt.stack) == 0 {
		return 0, nil, errors.New("empty OP stack")
	}

	return ctxt.stack[len(ctxt.stack)-1], nil, nil
}

// push pushes a value with its type on the stack.
func (ctxt *context) push(v int64, typ uint64) {
	ctxt.stack = append(ctxt.stack, v)
	ctxt.types = append(ctxt.types, typ)
}

// baseType resolves a (non-generic) type DIE offset to a base type and
// validates that the typed stack machine supports it.
func (ctxt *context) baseType(off uint64) (BaseType, error) {
	if ctxt.resolve == nil {
		return BaseType{}, errors.New("no base type resolver available")
	}
	bt, err := ctxt.resolve(off)
	if err != nil {
		return BaseType{}, err
	}
	switch bt.ByteSize {
	case 1, 2, 4, 8:
	default:
		return BaseType{}, fmt.Errorf("unsupported base type size %d", bt.ByteSize)
	}
	return bt, nil
}

// typeMask returns a mask for the low sz bytes of a value.
func typeMask(sz int) uint64 {
	if sz >= 8 {
		return ^uint64(0)
	}
	return (uint64(1) << (uint(sz) * 8)) - 1
}

// extendValue truncates v to sz bytes and extends it back to 64 bits
// according to the signedness of the type.
func extendValue(v int64, sz int, signed bool) int64 {
	if sz >= 8 {
		return v
	}
	mask := typeMask(sz)
	u := uint64(v) & mask
	if signed && u&(uint64(1)<<(uint(sz)*8-1)) != 0 {
		u |= ^mask
	}
	return int64(u)
}

// lowBytes returns the low sz bytes of v, little endian.
func lowBytes(v int64, sz int) []byte {
	var buf [8]byte
	binary.LittleEndian.PutUint64(buf[:], uint64(v))
	out := make([]byte, sz)
	copy(out, buf[:])
	return out
}

// decodeTypeOffset reads a CU-relative type DIE offset operand, returning an
// error on truncated input instead of panicking inside the leb128 decoder.
func decodeTypeOffset(buf *bytes.Buffer) (uint64, error) {
	data := buf.Bytes()
	i := 0
	for i < len(data) && data[i]&0x80 != 0 {
		i++
	}
	if i >= len(data) {
		return 0, errors.New("truncated type offset operand")
	}
	off, _ := leb128.DecodeUnsigned(buf)
	return off, nil
}

// PrettyPrint prints the DWARF stack program instructions to `out`.
func PrettyPrint(out io.Writer, instructions []byte, regnumToName func(uint64) string) {
	in := bytes.NewBuffer(instructions)

	for {
		opcode, err := in.ReadByte()
		if err != nil {
			break
		}
		op := Opcode(opcode)
		if name, hasname := opcodeName[op]; hasname {
			io.WriteString(out, name)
			if regnumToName != nil {
				if op >= DW_OP_reg0 && op <= DW_OP_reg31 {
					fmt.Fprintf(out, "(%s)", regnumToName(uint64(op-DW_OP_reg0)))
				} else if op >= DW_OP_breg0 && op <= DW_OP_breg31 {
					fmt.Fprintf(out, "(%s)", regnumToName(uint64(op-DW_OP_breg0)))
				}
			}
			out.Write([]byte{' '})
		} else {
			fmt.Fprintf(out, "%#x ", opcode)
		}

		for i, arg := range opcodeArgs[Opcode(opcode)] {
			var regnum uint64
			switch arg {
			case 's':
				n, _ := leb128.DecodeSigned(in)
				regnum = uint64(n)
				fmt.Fprintf(out, "%#x ", n)
			case 'u':
				n, _ := leb128.DecodeUnsigned(in)
				regnum = n
				fmt.Fprintf(out, "%#x ", n)
			case '1':
				var x uint8
				binary.Read(in, binary.LittleEndian, &x)
				fmt.Fprintf(out, "%#x ", x)
			case '2':
				var x uint16
				binary.Read(in, binary.LittleEndian, &x)
				fmt.Fprintf(out, "%#x ", x)
			case '4':
				var x uint32
				binary.Read(in, binary.LittleEndian, &x)
				fmt.Fprintf(out, "%#x ", x)
			case '8':
				var x uint64
				binary.Read(in, binary.LittleEndian, &x)
				fmt.Fprintf(out, "%#x ", x)
			case 'B':
				sz, _ := leb128.DecodeUnsigned(in)
				data := make([]byte, sz)
				sz2, _ := in.Read(data)
				data = data[:sz2]
				fmt.Fprintf(out, "%d [%x] ", sz, data)
			case 'T':
				// type offset, one byte block length, block
				off, _ := leb128.DecodeUnsigned(in)
				var sz uint8
				binary.Read(in, binary.LittleEndian, &sz)
				data := make([]byte, sz)
				sz2, _ := in.Read(data)
				data = data[:sz2]
				fmt.Fprintf(out, "%#x %d [%x] ", off, sz, data)
			}
			if regnumToName != nil && i == 0 && (op == DW_OP_regx || op == DW_OP_bregx) {
				fmt.Fprintf(out, "(%s)", regnumToName(regnum))
			}
		}
	}
}

// closeLoc is called by opcodes that can only appear at the end of a
// location expression (DW_OP_regN, DW_OP_regx, DW_OP_stack_value...).
// It checks that we are at the end of the program or that the following
// opcode is DW_OP_piece or DW_OP_bit_piece and processes them.
func (ctxt *context) closeLoc(opcode0 Opcode, piece Piece) error {
	if ctxt.buf.Len() == 0 {
		ctxt.pieces = append(ctxt.pieces, piece)
		return nil
	}

	// DWARF doesn't say what happens to the operand stack at the end of a
	// location expression, resetting it here.
	ctxt.stack = ctxt.stack[:0]
	ctxt.types = ctxt.types[:0]

	b, err := ctxt.buf.ReadByte()
	if err != nil {
		return err
	}
	opcode := Opcode(b)

	switch opcode {
	case DW_OP_piece:
		sz, _ := leb128.DecodeUnsigned(ctxt.buf)
		piece.Size = int(sz)
		ctxt.pieces = append(ctxt.pieces, piece)
		return nil

	case DW_OP_bit_piece:
		// not supported
		return fmt.Errorf("invalid instruction %#v", opcode)
	default:
		return fmt.Errorf("invalid instruction %#v after %#v", opcode, opcode0)
	}
}

func callframecfa(opcode Opcode, ctxt *context) error {
	if ctxt.CFA == 0 {
		return errors.New("could not retrieve CFA for current PC")
	}
	ctxt.push(ctxt.CFA, genericType)
	return nil
}

func addr(opcode Opcode, ctxt *context) error {
	buf := ctxt.buf.Next(ctxt.ptrSize)
	stack, err := dwarf.ReadUintRaw(bytes.NewReader(buf), binary.LittleEndian, ctxt.ptrSize)
	if err != nil {
		return err
	}
	ctxt.push(int64(stack+ctxt.StaticBase), genericType)
	return nil
}

func plusuconsts(opcode Opcode, ctxt *context) error {
	slen := len(ctxt.stack)
	num, _ := leb128.DecodeUnsigned(ctxt.buf)
	v := ctxt.stack[slen-1] + int64(num)
	if typ := ctxt.types[slen-1]; typ != genericType {
		// DW_OP_plus_uconst preserves the type of its left operand.
		bt, err := ctxt.baseType(typ)
		if err != nil {
			return err
		}
		v = extendValue(v, bt.ByteSize, bt.Signed)
	}
	ctxt.stack[slen-1] = v
	return nil
}

func consts(opcode Opcode, ctxt *context) error {
	num, _ := leb128.DecodeSigned(ctxt.buf)
	ctxt.push(num, genericType)
	return nil
}

func framebase(opcode Opcode, ctxt *context) error {
	num, _ := leb128.DecodeSigned(ctxt.buf)
	ctxt.push(ctxt.FrameBase+num, genericType)
	return nil
}

func register(opcode Opcode, ctxt *context) error {
	var regnum uint64
	if opcode == DW_OP_regx {
		regnum, _ = leb128.DecodeUnsigned(ctxt.buf)
	} else {
		regnum = uint64(opcode - DW_OP_reg0)
	}
	return ctxt.closeLoc(opcode, Piece{Kind: RegPiece, Val: regnum})
}

func bregister(opcode Opcode, ctxt *context) error {
	var regnum uint64
	if opcode == DW_OP_bregx {
		regnum, _ = leb128.DecodeUnsigned(ctxt.buf)
	} else {
		regnum = uint64(opcode - DW_OP_breg0)
	}
	offset, _ := leb128.DecodeSigned(ctxt.buf)
	if ctxt.Reg(regnum) == nil {
		return fmt.Errorf("register %d not available", regnum)
	}
	ctxt.push(int64(ctxt.Uint64Val(regnum))+offset, genericType)
	return nil
}

func piece(opcode Opcode, ctxt *context) error {
	sz, _ := leb128.DecodeUnsigned(ctxt.buf)

	if len(ctxt.stack) == 0 {
		// nothing on the stack means this piece is unavailable (padding,
		// optimized away...), see DWARFv4 sec. 2.6.1.3 page 30.
		ctxt.pieces = append(ctxt.pieces, Piece{Size: int(sz), Kind: ImmPiece, Val: 0})
		return nil
	}

	addr := ctxt.stack[len(ctxt.stack)-1]
	ctxt.pieces = append(ctxt.pieces, Piece{Size: int(sz), Kind: AddrPiece, Val: uint64(addr)})
	ctxt.stack = ctxt.stack[:0]
	ctxt.types = ctxt.types[:0]
	return nil
}

func literal(opcode Opcode, ctxt *context) error {
	ctxt.push(int64(opcode-DW_OP_lit0), genericType)
	return nil
}

func constnu(opcode Opcode, ctxt *context) error {
	var (
		n   uint64
		err error
	)
	switch opcode {
	case DW_OP_const1u:
		var b uint8
		b, err = ctxt.buf.ReadByte()
		n = uint64(b)
	case DW_OP_const2u:
		n, err = dwarf.ReadUintRaw(ctxt.buf, binary.LittleEndian, 2)
	case DW_OP_const4u:
		n, err = dwarf.ReadUintRaw(ctxt.buf, binary.LittleEndian, 4)
	case DW_OP_const8u:
		n, err = dwarf.ReadUintRaw(ctxt.buf, binary.LittleEndian, 8)
	default:
		panic("internal error")
	}
	if err != nil {
		return err
	}
	ctxt.push(int64(n), genericType)
	return nil
}

func constns(opcode Opcode, ctxt *context) error {
	var (
		n   uint64
		err error
	)
	switch opcode {
	case DW_OP_const1s:
		var b uint8
		b, err = ctxt.buf.ReadByte()
		n = uint64(int64(int8(b)))
	case DW_OP_const2s:
		n, err = dwarf.ReadUintRaw(ctxt.buf, binary.LittleEndian, 2)
		n = uint64(int64(int16(n)))
	case DW_OP_const4s:
		n, err = dwarf.ReadUintRaw(ctxt.buf, binary.LittleEndian, 4)
		n = uint64(int64(int32(n)))
	case DW_OP_const8s:
		n, err = dwarf.ReadUintRaw(ctxt.buf, binary.LittleEndian, 8)
	default:
		panic("internal error")
	}
	if err != nil {
		return err
	}
	ctxt.push(int64(n), genericType)
	return nil
}

func constu(opcode Opcode, ctxt *context) error {
	num, _ := leb128.DecodeUnsigned(ctxt.buf)
	ctxt.push(int64(num), genericType)
	return nil
}

// consttype implements DW_OP_const_type: it reads a CU-relative type offset,
// a one byte block length and the raw (little endian) constant bytes. The
// block length must match the size of the referenced base type.
func consttype(opcode Opcode, ctxt *context) error {
	off, err := decodeTypeOffset(ctxt.buf)
	if err != nil {
		return err
	}
	if off == genericType {
		return errors.New("DW_OP_const_type requires a named base type, not the generic type")
	}
	bt, err := ctxt.baseType(off)
	if err != nil {
		return err
	}
	sz, err := ctxt.buf.ReadByte()
	if err != nil {
		return fmt.Errorf("truncated DW_OP_const_type operand: %w", err)
	}
	if int(sz) != bt.ByteSize {
		return fmt.Errorf("DW_OP_const_type block size %d does not match base type size %d", sz, bt.ByteSize)
	}
	raw := ctxt.buf.Next(bt.ByteSize)
	if len(raw) != bt.ByteSize {
		return errors.New("truncated DW_OP_const_type constant block")
	}
	var n uint64
	for i, b := range raw {
		n |= uint64(b) << (8 * uint(i))
	}
	ctxt.push(extendValue(int64(n), bt.ByteSize, bt.Signed), off)
	return nil
}

// convert implements DW_OP_convert: it converts the top of the stack by
// value to the referenced type, sign extending or truncating as needed.
// A zero offset converts the value to the generic type.
func convert(opcode Opcode, ctxt *context) error {
	off, err := decodeTypeOffset(ctxt.buf)
	if err != nil {
		return err
	}
	if len(ctxt.stack) < 1 {
		return ErrStackUnderflow
	}
	i := len(ctxt.stack) - 1
	if off == genericType {
		ctxt.types[i] = genericType
		return nil
	}
	bt, err := ctxt.baseType(off)
	if err != nil {
		return err
	}
	ctxt.stack[i] = extendValue(ctxt.stack[i], bt.ByteSize, bt.Signed)
	ctxt.types[i] = off
	return nil
}

// reinterpret implements DW_OP_reinterpret: it reinterprets the raw bits of
// the top of the stack using the referenced type. Both types must have the
// same size. A zero offset reinterprets the value as the generic type.
func reinterpret(opcode Opcode, ctxt *context) error {
	off, err := decodeTypeOffset(ctxt.buf)
	if err != nil {
		return err
	}
	if len(ctxt.stack) < 1 {
		return ErrStackUnderflow
	}
	i := len(ctxt.stack) - 1
	if off == genericType {
		ctxt.types[i] = genericType
		return nil
	}
	bt, err := ctxt.baseType(off)
	if err != nil {
		return err
	}
	srcSize := ctxt.ptrSize
	if typ := ctxt.types[i]; typ != genericType {
		src, err := ctxt.baseType(typ)
		if err != nil {
			return err
		}
		srcSize = src.ByteSize
	}
	if srcSize != bt.ByteSize {
		return fmt.Errorf("DW_OP_reinterpret requires equal sizes, have %d and %d", srcSize, bt.ByteSize)
	}
	ctxt.stack[i] = extendValue(ctxt.stack[i], bt.ByteSize, bt.Signed)
	ctxt.types[i] = off
	return nil
}

func dup(_ Opcode, ctxt *context) error {
	if len(ctxt.stack) == 0 {
		return ErrStackUnderflow
	}
	ctxt.stack = append(ctxt.stack, ctxt.stack[len(ctxt.stack)-1])
	ctxt.types = append(ctxt.types, ctxt.types[len(ctxt.types)-1])
	return nil
}

func drop(_ Opcode, ctxt *context) error {
	if len(ctxt.stack) == 0 {
		return ErrStackUnderflow
	}
	ctxt.stack = ctxt.stack[:len(ctxt.stack)-1]
	ctxt.types = ctxt.types[:len(ctxt.types)-1]
	return nil
}

func pick(opcode Opcode, ctxt *context) error {
	var n byte
	switch opcode {
	case DW_OP_pick:
		n, _ = ctxt.buf.ReadByte()
	case DW_OP_over:
		n = 1
	default:
		panic("internal error")
	}
	idx := len(ctxt.stack) - 1 - int(n)
	if idx < 0 || idx >= len(ctxt.stack) {
		return ErrStackIndexOutOfBounds
	}
	ctxt.stack = append(ctxt.stack, ctxt.stack[idx])
	ctxt.types = append(ctxt.types, ctxt.types[idx])
	return nil
}

func swap(_ Opcode, ctxt *context) error {
	if len(ctxt.stack) < 2 {
		return ErrStackUnderflow
	}
	ctxt.stack[len(ctxt.stack)-1], ctxt.stack[len(ctxt.stack)-2] = ctxt.stack[len(ctxt.stack)-2], ctxt.stack[len(ctxt.stack)-1]
	ctxt.types[len(ctxt.types)-1], ctxt.types[len(ctxt.types)-2] = ctxt.types[len(ctxt.types)-2], ctxt.types[len(ctxt.types)-1]
	return nil
}

func rot(_ Opcode, ctxt *context) error {
	if len(ctxt.stack) < 3 {
		return ErrStackUnderflow
	}
	ctxt.stack[len(ctxt.stack)-1], ctxt.stack[len(ctxt.stack)-2], ctxt.stack[len(ctxt.stack)-3] = ctxt.stack[len(ctxt.stack)-2], ctxt.stack[len(ctxt.stack)-3], ctxt.stack[len(ctxt.stack)-1]
	ctxt.types[len(ctxt.types)-1], ctxt.types[len(ctxt.types)-2], ctxt.types[len(ctxt.types)-3] = ctxt.types[len(ctxt.types)-2], ctxt.types[len(ctxt.types)-3], ctxt.types[len(ctxt.types)-1]
	return nil
}

func unaryop(opcode Opcode, ctxt *context) error {
	if len(ctxt.stack) < 1 {
		return ErrStackUnderflow
	}
	operand := ctxt.stack[len(ctxt.stack)-1]
	switch opcode {
	case DW_OP_abs:
		if operand < 0 {
			operand = -operand
		}
	case DW_OP_neg:
		operand = -operand
	case DW_OP_not:
		operand = ^operand
	default:
		panic("internal error")
	}
	if typ := ctxt.types[len(ctxt.types)-1]; typ != genericType {
		bt, err := ctxt.baseType(typ)
		if err != nil {
			return err
		}
		operand = extendValue(operand, bt.ByteSize, bt.Signed)
	}
	ctxt.stack[len(ctxt.stack)-1] = operand
	return nil
}

func binaryop(opcode Opcode, ctxt *context) error {
	if len(ctxt.stack) < 2 {
		return ErrStackUnderflow
	}
	second := ctxt.stack[len(ctxt.stack)-2]
	top := ctxt.stack[len(ctxt.stack)-1]
	secondTyp := ctxt.types[len(ctxt.types)-2]
	topTyp := ctxt.types[len(ctxt.types)-1]
	ctxt.stack = ctxt.stack[:len(ctxt.stack)-2]
	ctxt.types = ctxt.types[:len(ctxt.types)-2]

	switch opcode {
	case DW_OP_shl, DW_OP_shr, DW_OP_shra:
		// The shift count must be a non-negative value of the generic type.
		if topTyp != genericType {
			return fmt.Errorf("%s requires a generic type shift count", opcodeName[opcode])
		}
		if top < 0 {
			return fmt.Errorf("%s with negative shift count %d", opcodeName[opcode], top)
		}
		r, err := ctxt.shiftop(opcode, second, secondTyp, uint64(top))
		if err != nil {
			return err
		}
		ctxt.push(r, secondTyp)
		return nil
	}

	if secondTyp != topTyp {
		return fmt.Errorf("type mismatch in %s: operands have different types", opcodeName[opcode])
	}

	var (
		bt  BaseType
		err error
	)
	signed := true // the generic type is signed
	if secondTyp != genericType {
		bt, err = ctxt.baseType(secondTyp)
		if err != nil {
			return err
		}
		signed = bt.Signed
	}

	var r int64
	resultTyp := secondTyp
	switch opcode {
	case DW_OP_and:
		r = second & top
	case DW_OP_div:
		if top == 0 {
			return ErrDivisionByZero
		}
		if signed {
			r = second / top
		} else {
			r = int64(uint64(second) / uint64(top))
		}
	case DW_OP_minus:
		r = second - top
	case DW_OP_mod:
		if top == 0 {
			return ErrDivisionByZero
		}
		if signed {
			r = second % top
		} else {
			r = int64(uint64(second) % uint64(top))
		}
	case DW_OP_mul:
		r = second * top
	case DW_OP_or:
		r = second | top
	case DW_OP_plus:
		r = second + top
	case DW_OP_xor:
		r = second ^ top
	case DW_OP_le, DW_OP_ge, DW_OP_eq, DW_OP_lt, DW_OP_gt, DW_OP_ne:
		// Comparisons observe the signedness of the operands and produce a
		// generic type 0 or 1.
		var b bool
		if signed {
			switch opcode {
			case DW_OP_le:
				b = second <= top
			case DW_OP_ge:
				b = second >= top
			case DW_OP_eq:
				b = second == top
			case DW_OP_lt:
				b = second < top
			case DW_OP_gt:
				b = second > top
			case DW_OP_ne:
				b = second != top
			}
		} else {
			switch opcode {
			case DW_OP_le:
				b = uint64(second) <= uint64(top)
			case DW_OP_ge:
				b = uint64(second) >= uint64(top)
			case DW_OP_eq:
				b = second == top
			case DW_OP_lt:
				b = uint64(second) < uint64(top)
			case DW_OP_gt:
				b = uint64(second) > uint64(top)
			case DW_OP_ne:
				b = second != top
			}
		}
		r = bool2int(b)
		resultTyp = genericType
	default:
		panic("internal error")
	}
	if resultTyp != genericType {
		// Arithmetic results are truncated to the width of the type.
		r = extendValue(r, bt.ByteSize, bt.Signed)
	}
	ctxt.push(r, resultTyp)
	return nil
}

// shiftop implements DW_OP_shl, DW_OP_shr and DW_OP_shra. The result has the
// type of the shifted (left) operand, truncated to its width.
func (ctxt *context) shiftop(opcode Opcode, v int64, typ uint64, n uint64) (int64, error) {
	mask := typeMask(ctxt.ptrSize)
	signed := true
	sz := ctxt.ptrSize
	if typ != genericType {
		bt, err := ctxt.baseType(typ)
		if err != nil {
			return 0, err
		}
		mask = typeMask(bt.ByteSize)
		signed = bt.Signed
		sz = bt.ByteSize
	}
	var r int64
	switch opcode {
	case DW_OP_shl:
		r = v << n
	case DW_OP_shr:
		// logical shift, zero fill
		r = int64((uint64(v) & mask) >> n)
	case DW_OP_shra:
		// arithmetic shift, sign extends for signed types
		if signed {
			r = v >> n
		} else {
			r = int64((uint64(v) & mask) >> n)
		}
	default:
		panic("internal error")
	}
	return extendValue(r, sz, signed), nil
}

func bool2int(b bool) int64 {
	if b {
		return 1
	}
	return 0
}

func (ctxt *context) jump(n int16) error {
	i := len(ctxt.prog) - ctxt.buf.Len() + int(n)
	if i < 0 {
		return ErrStackUnderflow
	}
	if i >= len(ctxt.prog) {
		i = len(ctxt.prog)
	}
	ctxt.buf = bytes.NewBuffer(ctxt.prog[i:])
	return nil
}

func skip(_ Opcode, ctxt *context) error {
	var n int16
	binary.Read(ctxt.buf, binary.LittleEndian, &n)
	return ctxt.jump(n)
}

func bra(_ Opcode, ctxt *context) error {
	var n int16
	binary.Read(ctxt.buf, binary.LittleEndian, &n)

	if len(ctxt.stack) < 1 {
		return ErrStackUnderflow
	}
	top := ctxt.stack[len(ctxt.stack)-1]
	ctxt.stack = ctxt.stack[:len(ctxt.stack)-1]
	ctxt.types = ctxt.types[:len(ctxt.types)-1]
	if top != 0 {
		return ctxt.jump(n)
	}
	return nil
}

func stackvalue(_ Opcode, ctxt *context) error {
	if len(ctxt.stack) < 1 {
		return ErrStackUnderflow
	}
	val := ctxt.stack[len(ctxt.stack)-1]
	typ := ctxt.types[len(ctxt.types)-1]
	ctxt.stack = ctxt.stack[:len(ctxt.stack)-1]
	ctxt.types = ctxt.types[:len(ctxt.types)-1]
	if typ != genericType {
		// A typed value becomes an immediate piece holding the raw low
		// bytes of the value; the type stack is reset so the next piece
		// does not inherit it.
		bt, err := ctxt.baseType(typ)
		if err != nil {
			return err
		}
		return ctxt.closeLoc(DW_OP_stack_value, Piece{Kind: ImmPiece, Bytes: lowBytes(val, bt.ByteSize), Size: bt.ByteSize})
	}
	return ctxt.closeLoc(DW_OP_stack_value, Piece{Kind: ImmPiece, Val: uint64(val)})
}

func implicitvalue(_ Opcode, ctxt *context) error {
	sz, _ := leb128.DecodeUnsigned(ctxt.buf)
	block := make([]byte, sz)
	n, _ := ctxt.buf.Read(block)
	if uint64(n) != sz {
		return fmt.Errorf("insufficient bytes read while reading DW_OP_implicit_value's block %d (expected: %d)", n, sz)
	}
	return ctxt.closeLoc(DW_OP_implicit_value, Piece{Kind: ImmPiece, Bytes: block, Size: int(sz)})
}

func deref(op Opcode, ctxt *context) error {
	if ctxt.readMemory == nil {
		return ErrMemoryReadUnavailable
	}

	sz := ctxt.ptrSize
	if op == DW_OP_deref_size || op == DW_OP_xderef_size {
		n, err := ctxt.buf.ReadByte()
		if err != nil {
			return err
		}
		sz = int(n)
	}

	if len(ctxt.stack) == 0 {
		return ErrStackUnderflow
	}

	addr := ctxt.stack[len(ctxt.stack)-1]
	ctxt.stack = ctxt.stack[:len(ctxt.stack)-1]
	ctxt.types = ctxt.types[:len(ctxt.types)-1]

	if op == DW_OP_xderef || op == DW_OP_xderef_size {
		if len(ctxt.stack) == 0 {
			return ErrStackUnderflow
		}
		// the second element on the stack is the "address space identifier" which we don't do anything with
		ctxt.stack = ctxt.stack[:len(ctxt.stack)-1]
		ctxt.types = ctxt.types[:len(ctxt.types)-1]
	}

	buf := make([]byte, sz)
	_, err := ctxt.readMemory(buf, uint64(addr))
	if err != nil {
		return err
	}

	x, err := dwarf.ReadUintRaw(bytes.NewReader(buf), binary.LittleEndian, sz)
	if err != nil {
		return err
	}

	ctxt.push(int64(x), genericType)

	return nil
}
