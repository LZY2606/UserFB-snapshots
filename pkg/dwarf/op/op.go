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

type context struct {
	buf     *bytes.Buffer
	prog    []byte
	stack   []int64
	pieces  []Piece
	ptrSize int

	DwarfRegisters
	readMemory ReadMemoryFunc

	// bitPieces selects the bit-piece-aware interpretation of DW_OP_piece
	// and DW_OP_bit_piece used by the location write planner; when false the
	// historical byte-only behavior is preserved.
	bitPieces bool
}

// Piece is a piece of memory stored either at an address or in a register.
type Piece struct {
	Size  int
	Kind  PieceKind
	Val   uint64
	Bytes []byte

	// BitSize and BitOffset are populated only by the bit-piece-aware
	// evaluation path (see PlanLocationWrite). BitSize is the length of the
	// piece in bits and BitOffset is the source bit offset (the second
	// ULEB128 operand of DW_OP_bit_piece). Both are zero on pieces produced
	// by the legacy ExecuteStackProgram path, which keeps reporting byte
	// sizes through Size.
	BitSize   uint64
	BitOffset uint64

	// bitSized reports whether a piece terminator explicitly gave the piece
	// a length, including a zero-length one. A false value means the piece
	// is a single location expression without any piece terminator.
	bitSized bool

	// unavailable marks a synthetic trailing piece that fills the part of
	// the logical object not covered by the location expression. Selecting
	// any of its bits fails planning.
	unavailable bool
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
	ctxt := &context{
		buf:            bytes.NewBuffer(instructions),
		prog:           instructions,
		stack:          make([]int64, 0, 3),
		DwarfRegisters: regs,
		ptrSize:        ptrSize,
		readMemory:     readMemory,
	}

	if err := ctxt.run(); err != nil {
		return 0, nil, err
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

// run drives the shared opcode interpreter. Both ExecuteStackProgram and the
// location write planner go through this loop and the same opcode handlers,
// including the piece/bit_piece terminator logic in closeLoc; the two paths
// only differ in how terminators are interpreted (ctxt.bitPieces).
func (ctxt *context) run() error {
	limit := len(ctxt.prog) * arbitraryExecutionLimitFactor
	for tick := 0; tick < limit; tick++ {
		off := ctxt.offset()
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
			return ctxt.exprErrorf(off, "invalid instruction %#v", opcode)
		}

		if err = fn(opcode, ctxt); err != nil {
			return ctxt.exprError(off, err)
		}
	}
	if len(ctxt.prog) > 0 && ctxt.offset() < len(ctxt.prog) {
		// The only way to leave unread instructions is to exhaust the
		// execution step budget (jumps keep the buffer moving otherwise).
		return ctxt.exprErrorf(ctxt.offset(), "execution step limit exhausted")
	}
	return nil
}

// offset is the byte position in the location expression at which the next
// opcode would be read.
func (ctxt *context) offset() int {
	return len(ctxt.prog) - ctxt.buf.Len()
}

// exprError wraps err with the expression offset at which the failing opcode
// started, so callers can locate truncated or malformed expressions.
func (ctxt *context) exprError(off int, err error) error {
	if err == nil {
		return nil
	}
	return fmt.Errorf("DWARF expression error at offset %d: %w", off, err)
}

func (ctxt *context) exprErrorf(off int, format string, args ...interface{}) error {
	return ctxt.exprError(off, fmt.Errorf(format, args...))
}

// mul8Checked converts a byte length to a bit length without overflowing.
func mul8Checked(bytes uint64) (uint64, bool) {
	if bytes > (^uint64(0))/8 {
		return 0, true
	}
	return bytes * 8, false
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
		ctxt.pieces = append(ctxt.pieces, piece)
		return nil
	}

	// DWARF doesn't say what happens to the operand stack at the end of a
	// location expression, resetting it here.
	ctxt.stack = ctxt.stack[:0]

	b, err := ctxt.buf.ReadByte()
	if err != nil {
		return err
	}
	opcode := Opcode(b)

	switch opcode {
	case DW_OP_piece:
		sz, err := ctxt.decodePieceSize(opcode)
		if err != nil {
			return err
		}
		if ctxt.bitPieces {
			bits, overflow := mul8Checked(sz)
			if overflow {
				return fmt.Errorf("%#v size overflows uint64 bits", opcode)
			}
			piece.BitSize = bits
			piece.bitSized = true
		} else {
			piece.Size = int(sz)
		}
		ctxt.pieces = append(ctxt.pieces, piece)
		return nil

	case DW_OP_bit_piece:
		if !ctxt.bitPieces {
			// The legacy byte-only consumer cannot interpret bit pieces,
			// reject them explicitly rather than silently dropping bits.
			return fmt.Errorf("invalid instruction %#v", opcode)
		}
		nbits, err := ctxt.decodePieceSize(opcode)
		if err != nil {
			return err
		}
		boff, err := ctxt.decodePieceSize(opcode)
		if err != nil {
			return err
		}
		piece.BitSize = nbits
		piece.BitOffset = boff
		piece.bitSized = true
		ctxt.pieces = append(ctxt.pieces, piece)
		return nil
	default:
		return fmt.Errorf("invalid instruction %#v after %#v", opcode, opcode0)
	}
}

// decodePieceSize reads one ULEB128 operand of a piece terminator, turning
// truncated encodings and values that do not fit in uint64 into errors
// instead of the panic/zero that a bare leb128 read yields on a short
// buffer.
func (ctxt *context) decodePieceSize(opcode Opcode) (uint64, error) {
	var (
		result uint64
		shift  uint
		n      uint32
	)
	for {
		b, err := ctxt.buf.ReadByte()
		if err != nil {
			return 0, fmt.Errorf("truncated %#v operand", opcode)
		}
		n++
		if shift < 64 {
			if shift == 63 && b&0x7e != 0 {
				return 0, fmt.Errorf("%#v operand overflows uint64", opcode)
			}
			result |= uint64(b&0x7f) << shift
		}
		if b&0x80 == 0 {
			break
		}
		shift += 7
		if shift >= 64 {
			// Keep consuming the rest of the malformed encoding so the
			// buffer position stays consistent, then reject it.
			for {
				b, err := ctxt.buf.ReadByte()
				if err != nil {
					return 0, fmt.Errorf("truncated %#v operand", opcode)
				}
				if b&0x80 == 0 {
					return 0, fmt.Errorf("%#v operand overflows uint64", opcode)
				}
			}
		}
	}
	if n > 10 {
		return 0, fmt.Errorf("%#v operand overflows uint64", opcode)
	}
	return result, nil
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
	sz, err := ctxt.decodePieceSize(opcode)
	if err != nil {
		return err
	}

	if len(ctxt.stack) == 0 {
		// nothing on the stack means this piece is unavailable (padding,
		// optimized away...), see DWARFv4 sec. 2.6.1.3 page 30.
		p := Piece{Kind: ImmPiece, Val: 0}
		if ctxt.bitPieces {
			// A terminator is always consumed, even for a zero-length
			// piece; the (already empty) operand stack is cleared below.
			bits, overflow := mul8Checked(sz)
			if overflow {
				return fmt.Errorf("%#v size overflows uint64 bits", opcode)
			}
			p.BitSize = bits
			p.bitSized = true
			p.unavailable = true
		} else {
			p.Size = int(sz)
		}
		ctxt.pieces = append(ctxt.pieces, p)
		ctxt.stack = ctxt.stack[:0]
		return nil
	}

	addr := ctxt.stack[len(ctxt.stack)-1]
	p := Piece{Kind: AddrPiece, Val: uint64(addr)}
	if ctxt.bitPieces {
		bits, overflow := mul8Checked(sz)
		if overflow {
			return fmt.Errorf("%#v size overflows uint64 bits", opcode)
		}
		p.BitSize = bits
		p.bitSized = true
	} else {
		p.Size = int(sz)
	}
	ctxt.pieces = append(ctxt.pieces, p)
	ctxt.stack = ctxt.stack[:0]
	return nil
}

// bitpiece handles a DW_OP_bit_piece terminator following an address
// expression (the register/immediate/stack_value forms go through
// closeLoc). Like piece(), an empty operand stack means an unavailable
// piece; zero-length pieces still consume both operands.
func bitpiece(opcode Opcode, ctxt *context) error {
	nbits, err := ctxt.decodePieceSize(opcode)
	if err != nil {
		return err
	}
	boff, err := ctxt.decodePieceSize(opcode)
	if err != nil {
		return err
	}
	if !ctxt.bitPieces {
		// Keep the legacy byte-only path rejecting bit pieces even when
		// the terminator follows an address expression.
		return fmt.Errorf("invalid instruction %#v", opcode)
	}

	if len(ctxt.stack) == 0 {
		// Unavailable piece (padding, optimized away...), see DWARFv4
		// sec. 2.6.1.3. The terminator was consumed above and the
		// (already empty) stack is cleared.
		ctxt.pieces = append(ctxt.pieces, Piece{Kind: ImmPiece, Val: 0, BitSize: nbits, BitOffset: boff, bitSized: true, unavailable: true})
		ctxt.stack = ctxt.stack[:0]
		return nil
	}

	addr := ctxt.stack[len(ctxt.stack)-1]
	ctxt.pieces = append(ctxt.pieces, Piece{Kind: AddrPiece, Val: uint64(addr), BitSize: nbits, BitOffset: boff, bitSized: true})
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

	if op == DW_OP_xderef || op == DW_OP_xderef_size {
		if len(ctxt.stack) == 0 {
			return ErrStackUnderflow
		}
		// the second element on the stack is the "address space identifier" which we don't do anything with
		ctxt.stack = ctxt.stack[:len(ctxt.stack)-1]
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

	ctxt.stack = append(ctxt.stack, int64(x))

	return nil
}
