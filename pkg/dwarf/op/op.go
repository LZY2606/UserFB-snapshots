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

// ReadMemoryFunc reads into buf at addr and returns the number of bytes
// read. Location dereferences and write-plan reads treat a short read as an
// error.
type ReadMemoryFunc func([]byte, uint64) (int, error)

type context struct {
	buf     *bytes.Buffer
	prog    []byte
	stack   []int64
	pieces  []Piece
	locs    []locationFragment
	ptrSize int

	// planMode enables bit-piece evaluation and offset-annotated expression
	// errors for LocationWritePlan. The legacy ExecuteStackProgram path leaves
	// it false.
	planMode bool
	opOffset int

	DwarfRegisters
	readMemory ReadMemoryFunc
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
)

const arbitraryExecutionLimitFactor = 10

// ExecuteStackProgram executes a DWARF location expression and returns
// either an address (int64), or a slice of Pieces for location expressions
// that don't evaluate to an address (such as register and composite expressions).
func ExecuteStackProgram(regs DwarfRegisters, instructions []byte, ptrSize int, readMemory ReadMemoryFunc) (int64, []Piece, error) {
	ctxt, err := runStackProgram(regs, instructions, ptrSize, readMemory, false)
	if err != nil {
		return 0, nil, err
	}
	return ctxt.result()
}

func runStackProgram(regs DwarfRegisters, instructions []byte, ptrSize int, readMemory ReadMemoryFunc, planMode bool) (ctxt *context, err error) {
	ctxt = &context{
		buf:            bytes.NewBuffer(instructions),
		prog:           instructions,
		stack:          make([]int64, 0, 3),
		DwarfRegisters: regs,
		ptrSize:        ptrSize,
		readMemory:     readMemory,
		planMode:       planMode,
	}

	defer func() {
		if r := recover(); r != nil {
			if !planMode {
				panic(r)
			}
			err = ctxt.exprError(ctxt.opOffset, fmt.Errorf("corrupt DWARF expression: %v", r))
		}
	}()

	stepLimit := len(instructions) * arbitraryExecutionLimitFactor
	tick := 0
	for ; tick < stepLimit; tick++ {
		opOffset := len(ctxt.prog) - ctxt.buf.Len()
		opcodeByte, readErr := ctxt.buf.ReadByte()
		if readErr != nil {
			break
		}
		opcode := Opcode(opcodeByte)
		ctxt.opOffset = opOffset
		if opcode == DW_OP_nop {
			continue
		}
		fn, ok := oplut[opcode]
		if !ok {
			execErr := fmt.Errorf("invalid instruction %#v", opcode)
			return ctxt, ctxt.wrapExecError(execErr, opOffset)
		}

		execErr := fn(opcode, ctxt)
		if execErr != nil {
			return ctxt, ctxt.wrapExecError(execErr, opOffset)
		}
	}

	if planMode && tick >= stepLimit && ctxt.buf.Len() > 0 {
		offset := len(ctxt.prog) - ctxt.buf.Len()
		return ctxt, ctxt.exprError(offset, ErrExpressionStepsExhausted)
	}

	return ctxt, nil
}

func (ctxt *context) result() (int64, []Piece, error) {
	if ctxt.pieces != nil {
		if len(ctxt.pieces) == 1 && ctxt.pieces[0].Kind == RegPiece {
			return int64(ctxt.Uint64Val(ctxt.pieces[0].Val)), ctxt.pieces, nil
		}
		return 0, ctxt.pieces, nil
	}

	if len(ctxt.stack) == 0 {
		return 0, nil, errors.New("empty OP stack")
	}

	return ctxt.stack[len(ctxt.stack)-1], nil, nil
}

func (ctxt *context) wrapExecError(err error, offset int) error {
	if err == nil || !ctxt.planMode {
		return err
	}
	var exprErr *ExpressionError
	if errors.As(err, &exprErr) {
		return err
	}
	return ctxt.exprError(offset, err)
}

func (ctxt *context) exprError(offset int, err error) error {
	return &ExpressionError{Offset: offset, Err: err}
}

func (ctxt *context) appendLocation(piece Piece, loc locationFragment) {
	loc.piece = piece
	ctxt.pieces = append(ctxt.pieces, piece)
	ctxt.locs = append(ctxt.locs, loc)
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
		ctxt.appendLocation(piece, locationFragment{explicit: false, offset: ctxt.opOffset})
		return nil
	}

	// DWARF doesn't say what happens to the operand stack at the end of a
	// location expression, resetting it here.
	ctxt.stack = ctxt.stack[:0]

	terminatorOffset := len(ctxt.prog) - ctxt.buf.Len()
	b, err := ctxt.buf.ReadByte()
	if err != nil {
		return err
	}
	opcode := Opcode(b)

	switch opcode {
	case DW_OP_piece:
		sz, err := ctxt.decodeULEBLength()
		if err != nil {
			return ctxt.wrapLengthError(err, terminatorOffset)
		}
		bitLength, err := bytePieceBitLength(sz)
		if err != nil {
			return ctxt.wrapLengthError(err, terminatorOffset)
		}
		piece.Size = int(sz)
		ctxt.appendLocation(piece, locationFragment{
			explicit:  true,
			bitLength: bitLength,
			offset:    terminatorOffset,
		})
		return nil

	case DW_OP_bit_piece:
		if !ctxt.planMode {
			return fmt.Errorf("%w at expression offset %d", ErrBitPieceUnsupported, terminatorOffset)
		}
		bitLength, sourceBitOffset, err := ctxt.decodeBitPieceOperands()
		if err != nil {
			return ctxt.wrapLengthError(err, terminatorOffset)
		}
		piece.Size = 0
		ctxt.appendLocation(piece, locationFragment{
			explicit:        true,
			bitLength:       bitLength,
			sourceBitOffset: sourceBitOffset,
			offset:          terminatorOffset,
		})
		return nil
	default:
		err := fmt.Errorf("invalid instruction %#v after %#v", opcode, opcode0)
		if ctxt.planMode {
			return ctxt.exprError(terminatorOffset, err)
		}
		return err
	}
}

func (ctxt *context) decodeULEBLength() (uint64, error) {
	if ctxt.planMode {
		v, _, err := decodeULEB128Checked(ctxt.buf)
		return v, err
	}
	sz, _ := leb128.DecodeUnsigned(ctxt.buf)
	return sz, nil
}

func (ctxt *context) decodeBitPieceOperands() (bitLength, sourceBitOffset uint64, err error) {
	bitLength, _, err = decodeULEB128Checked(ctxt.buf)
	if err != nil {
		return 0, 0, fmt.Errorf("truncated DW_OP_bit_piece size: %w", err)
	}
	sourceBitOffset, _, err = decodeULEB128Checked(ctxt.buf)
	if err != nil {
		return 0, 0, fmt.Errorf("truncated DW_OP_bit_piece source offset: %w", err)
	}
	return bitLength, sourceBitOffset, nil
}

func (ctxt *context) wrapLengthError(err error, offset int) error {
	if !ctxt.planMode {
		return err
	}
	var exprErr *ExpressionError
	if errors.As(err, &exprErr) {
		return err
	}
	return ctxt.exprError(offset, err)
}

func callframecfa(opcode Opcode, ctxt *context) error {
	if ctxt.CFA == 0 {
		return errors.New("could not retrieve CFA for current PC")
	}
	ctxt.stack = append(ctxt.stack, ctxt.CFA)
	return nil
}

func addr(opcode Opcode, ctxt *context) error {
	buf := ctxt.buf.Next(ctxt.ptrSize)
	stack, err := dwarf.ReadUintRaw(bytes.NewReader(buf), binary.LittleEndian, ctxt.ptrSize)
	if err != nil {
		return err
	}
	ctxt.stack = append(ctxt.stack, int64(stack+ctxt.StaticBase))
	return nil
}

func plusuconsts(opcode Opcode, ctxt *context) error {
	slen := len(ctxt.stack)
	num, _ := leb128.DecodeUnsigned(ctxt.buf)
	ctxt.stack[slen-1] = ctxt.stack[slen-1] + int64(num)
	return nil
}

func consts(opcode Opcode, ctxt *context) error {
	num, _ := leb128.DecodeSigned(ctxt.buf)
	ctxt.stack = append(ctxt.stack, num)
	return nil
}

func framebase(opcode Opcode, ctxt *context) error {
	num, _ := leb128.DecodeSigned(ctxt.buf)
	ctxt.stack = append(ctxt.stack, ctxt.FrameBase+num)
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
	ctxt.stack = append(ctxt.stack, int64(ctxt.Uint64Val(regnum))+offset)
	return nil
}

func piece(opcode Opcode, ctxt *context) error {
	sz, err := ctxt.decodeULEBLength()
	if err != nil {
		return fmt.Errorf("invalid DW_OP_piece length: %w", err)
	}
	bitLength, err := bytePieceBitLength(sz)
	if err != nil {
		return err
	}

	if len(ctxt.stack) == 0 {
		// nothing on the stack means this piece is unavailable (padding,
		// optimized away...), see DWARFv4 sec. 2.6.1.3 page 30.
		piece := Piece{Size: int(sz), Kind: ImmPiece, Val: 0}
		ctxt.appendLocation(piece, locationFragment{
			unavailable: true,
			explicit:    true,
			bitLength:   bitLength,
			offset:      ctxt.opOffset,
		})
		return nil
	}

	addr := ctxt.stack[len(ctxt.stack)-1]
	piece := Piece{Size: int(sz), Kind: AddrPiece, Val: uint64(addr)}
	ctxt.appendLocation(piece, locationFragment{
		explicit:  true,
		bitLength: bitLength,
		offset:    ctxt.opOffset,
	})
	ctxt.stack = ctxt.stack[:0]
	return nil
}

func bitpiece(opcode Opcode, ctxt *context) error {
	if !ctxt.planMode {
		return ErrBitPieceUnsupported
	}

	bitLength, sourceBitOffset, err := ctxt.decodeBitPieceOperands()
	if err != nil {
		return err
	}

	if len(ctxt.stack) == 0 {
		piece := Piece{Kind: ImmPiece, Val: 0}
		ctxt.appendLocation(piece, locationFragment{
			unavailable:     true,
			explicit:        true,
			bitLength:       bitLength,
			sourceBitOffset: sourceBitOffset,
			offset:          ctxt.opOffset,
		})
		return nil
	}

	addr := ctxt.stack[len(ctxt.stack)-1]
	piece := Piece{Kind: AddrPiece, Val: uint64(addr)}
	ctxt.appendLocation(piece, locationFragment{
		explicit:        true,
		bitLength:       bitLength,
		sourceBitOffset: sourceBitOffset,
		offset:          ctxt.opOffset,
	})
	ctxt.stack = ctxt.stack[:0]
	return nil
}

func literal(opcode Opcode, ctxt *context) error {
	ctxt.stack = append(ctxt.stack, int64(opcode-DW_OP_lit0))
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
	ctxt.stack = append(ctxt.stack, int64(n))
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
	ctxt.stack = append(ctxt.stack, int64(n))
	return nil
}

func constu(opcode Opcode, ctxt *context) error {
	num, _ := leb128.DecodeUnsigned(ctxt.buf)
	ctxt.stack = append(ctxt.stack, int64(num))
	return nil
}

func dup(_ Opcode, ctxt *context) error {
	if len(ctxt.stack) == 0 {
		return ErrStackUnderflow
	}
	ctxt.stack = append(ctxt.stack, ctxt.stack[len(ctxt.stack)-1])
	return nil
}

func drop(_ Opcode, ctxt *context) error {
	if len(ctxt.stack) == 0 {
		return ErrStackUnderflow
	}
	ctxt.stack = ctxt.stack[:len(ctxt.stack)-1]
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
	return nil
}

func swap(_ Opcode, ctxt *context) error {
	if len(ctxt.stack) < 2 {
		return ErrStackUnderflow
	}
	ctxt.stack[len(ctxt.stack)-1], ctxt.stack[len(ctxt.stack)-2] = ctxt.stack[len(ctxt.stack)-2], ctxt.stack[len(ctxt.stack)-1]
	return nil
}

func rot(_ Opcode, ctxt *context) error {
	if len(ctxt.stack) < 3 {
		return ErrStackUnderflow
	}
	ctxt.stack[len(ctxt.stack)-1], ctxt.stack[len(ctxt.stack)-2], ctxt.stack[len(ctxt.stack)-3] = ctxt.stack[len(ctxt.stack)-2], ctxt.stack[len(ctxt.stack)-3], ctxt.stack[len(ctxt.stack)-1]
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
	ctxt.stack[len(ctxt.stack)-1] = operand
	return nil
}

func binaryop(opcode Opcode, ctxt *context) error {
	if len(ctxt.stack) < 2 {
		return ErrStackUnderflow
	}
	second := ctxt.stack[len(ctxt.stack)-2]
	top := ctxt.stack[len(ctxt.stack)-1]
	var r int64
	ctxt.stack = ctxt.stack[:len(ctxt.stack)-2]
	switch opcode {
	case DW_OP_and:
		r = second & top
	case DW_OP_div:
		r = second / top
	case DW_OP_minus:
		r = second - top
	case DW_OP_mod:
		r = second % top
	case DW_OP_mul:
		r = second * top
	case DW_OP_or:
		r = second | top
	case DW_OP_plus:
		r = second + top
	case DW_OP_shl:
		r = second << uint64(top)
	case DW_OP_shr:
		r = int64(uint64(second) >> uint64(top))
	case DW_OP_shra:
		r = second >> uint64(top)
	case DW_OP_xor:
		r = second ^ top
	case DW_OP_le:
		r = bool2int(second <= top)
	case DW_OP_ge:
		r = bool2int(second >= top)
	case DW_OP_eq:
		r = bool2int(second == top)
	case DW_OP_lt:
		r = bool2int(second < top)
	case DW_OP_gt:
		r = bool2int(second > top)
	case DW_OP_ne:
		r = bool2int(second != top)
	default:
		panic("internal error")
	}
	ctxt.stack = append(ctxt.stack, r)
	return nil
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
	if err := binary.Read(ctxt.buf, binary.LittleEndian, &n); err != nil {
		return fmt.Errorf("truncated DW_OP_skip offset: %w", err)
	}
	return ctxt.jump(n)
}

func bra(_ Opcode, ctxt *context) error {
	var n int16
	if err := binary.Read(ctxt.buf, binary.LittleEndian, &n); err != nil {
		return fmt.Errorf("truncated DW_OP_bra offset: %w", err)
	}

	if len(ctxt.stack) < 1 {
		return ErrStackUnderflow
	}
	top := ctxt.stack[len(ctxt.stack)-1]
	ctxt.stack = ctxt.stack[:len(ctxt.stack)-1]
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
	ctxt.stack = ctxt.stack[:len(ctxt.stack)-1]
	return ctxt.closeLoc(DW_OP_stack_value, Piece{Kind: ImmPiece, Val: uint64(val)})
}

func implicitvalue(_ Opcode, ctxt *context) error {
	sz, err := ctxt.decodeULEBLength()
	if err != nil {
		return fmt.Errorf("invalid DW_OP_implicit_value length: %w", err)
	}
	if _, err := bytePieceBitLength(sz); err != nil {
		return err
	}
	if sz > uint64(ctxt.buf.Len()) {
		return fmt.Errorf("insufficient bytes read while reading DW_OP_implicit_value's block (expected: %d)", sz)
	}
	block := make([]byte, int(sz))
	n, err := io.ReadFull(ctxt.buf, block)
	if err != nil || uint64(n) != sz {
		return fmt.Errorf("insufficient bytes read while reading DW_OP_implicit_value's block %d (expected: %d): %w", n, sz, err)
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

	if op == DW_OP_xderef || op == DW_OP_xderef_size {
		if len(ctxt.stack) == 0 {
			return ErrStackUnderflow
		}
		// the second element on the stack is the "address space identifier" which we don't do anything with
		ctxt.stack = ctxt.stack[:len(ctxt.stack)-1]
	}

	buf := make([]byte, sz)
	n, err := ctxt.readMemory(buf, uint64(addr))
	if err != nil {
		return err
	}
	if n != sz {
		return fmt.Errorf("DW_OP_deref short read at %#x: read %d bytes, wanted %d", uint64(addr), n, sz)
	}

	x, err := dwarf.ReadUintRaw(bytes.NewReader(buf), binary.LittleEndian, sz)
	if err != nil {
		return err
	}

	ctxt.stack = append(ctxt.stack, int64(x))

	return nil
}
