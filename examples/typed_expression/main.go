// Command typed_expression recovers variable values from typed DWARF
// location expressions in a DWARF5 ELF object and prints a TSV analysis
// table on stdout. It never runs the object and never reads target memory:
// address results are reported as addresses, not dereferenced.
//
// Usage: typed_expression <elf-file>
package main

import (
	"bytes"
	"debug/dwarf"
	"debug/elf"
	"fmt"
	"os"
	"strings"

	"github.com/go-delve/delve/pkg/dwarf/godwarf"
	"github.com/go-delve/delve/pkg/dwarf/leb128"
	"github.com/go-delve/delve/pkg/dwarf/op"
)

// dwarf5HeaderSize mirrors the header layout validated by
// godwarf.NewBaseTypeResolver (DWARF5, 32-bit DWARF).
const dwarf5HeaderSize = 12

type cuContext struct {
	headerOff dwarf.Offset
	resolve   op.BaseTypeResolver
}

func main() {
	if len(os.Args) != 2 {
		fmt.Fprintln(os.Stderr, "usage: typed_expression <elf-file>")
		os.Exit(2)
	}
	if err := run(os.Args[1]); err != nil {
		fmt.Fprintln(os.Stderr, "error:", err)
		os.Exit(1)
	}
}

func run(path string) error {
	f, err := elf.Open(path)
	if err != nil {
		return err
	}
	defer f.Close()
	if f.Class != elf.ELFCLASS64 {
		return fmt.Errorf("only 64-bit (ptrSize 8) objects are supported")
	}
	dw, err := f.DWARF()
	if err != nil {
		return err
	}
	sec := f.Section(".debug_info")
	if sec == nil {
		return fmt.Errorf("no .debug_info section")
	}
	debugInfo, err := sec.Data()
	if err != nil {
		return err
	}

	fmt.Println("cu_header_off\tvar_die_off\tvar_name\tlocation_hex\tlocation_pretty\ttype_rel_off\ttype_die_off\ttype_size\ttype_signed\tresult_kind\tvalue\tpiece_bytes")

	r := dw.Reader()
	var cu *cuContext
	for {
		e, err := r.Next()
		if err != nil {
			return err
		}
		if e == nil {
			break
		}
		switch e.Tag {
		case dwarf.TagCompileUnit:
			// The compilation unit entry follows its header directly.
			headerOff := e.Offset - dwarf5HeaderSize
			resolve, err := godwarf.NewBaseTypeResolver(dw, debugInfo, headerOff)
			if err != nil {
				return err
			}
			cu = &cuContext{headerOff: headerOff, resolve: resolve}
		case dwarf.TagVariable:
			if cu == nil {
				return fmt.Errorf("variable at %#x outside any compilation unit", e.Offset)
			}
			if err := processVariable(dw, e, cu); err != nil {
				return err
			}
		}
	}
	return nil
}

func processVariable(dw *dwarf.Data, e *dwarf.Entry, cu *cuContext) error {
	name, _ := e.Val(dwarf.AttrName).(string)
	loc, _ := e.Val(dwarf.AttrLocation).([]byte)

	var pretty strings.Builder
	op.PrettyPrint(&pretty, loc, nil)

	val, pieces, err := op.ExecuteStackProgramTyped(op.DwarfRegisters{}, loc, 8, nil, cu.resolve)
	if err != nil {
		return fmt.Errorf("variable %s at %#x: %w", name, e.Offset, err)
	}

	// Type referenced by the expression (the type of the final value), if any.
	var typeRel, typeDie, typeSize, typeSigned string
	if relOff, ok := finalTypeRef(loc); ok {
		bt, err := cu.resolve(relOff)
		if err != nil {
			return fmt.Errorf("variable %s at %#x: %w", name, e.Offset, err)
		}
		typeRel = fmt.Sprintf("%#x", relOff)
		typeDie = fmt.Sprintf("%#x", uint64(cu.headerOff)+relOff)
		typeSize = fmt.Sprintf("%d", bt.ByteSize)
		typeSigned = "unsigned"
		if bt.Signed {
			typeSigned = "signed"
		}
	}

	var kind, value, pieceBytes string
	switch {
	case pieces != nil:
		// The expression does not evaluate to a plain address.
		if len(pieces) == 1 && pieces[0].Kind == op.ImmPiece {
			kind = "immediate"
			p := pieces[0]
			if len(p.Bytes) > 0 {
				pieceBytes = fmt.Sprintf("%x", p.Bytes)
				if typeRel != "" {
					bt, _ := cu.resolve(mustTypeRef(loc))
					value = fmt.Sprintf("%d", interpretValue(p.Bytes, bt))
				} else {
					value = fmt.Sprintf("%d", int64(p.Val))
				}
			} else {
				value = fmt.Sprintf("%d", int64(p.Val))
			}
		} else if len(pieces) == 1 && pieces[0].Kind == op.RegPiece {
			kind = "register"
			value = fmt.Sprintf("%d", val)
		} else {
			kind = "pieces"
			value = fmt.Sprintf("%d", val)
		}
	default:
		// No DW_OP_stack_value: the result is the address of the variable,
		// not its value. Without a memory snapshot it is not dereferenced.
		kind = "address"
		value = fmt.Sprintf("%d", val)
	}

	fmt.Printf("%#x\t%#x\t%s\t%x\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n",
		uint64(cu.headerOff), uint64(e.Offset), name, loc, strings.TrimSpace(pretty.String()),
		typeRel, typeDie, typeSize, typeSigned, kind, value, pieceBytes)
	return nil
}

func mustTypeRef(expr []byte) uint64 {
	off, _ := finalTypeRef(expr)
	return off
}

// interpretValue reads the raw low bytes of an immediate piece using the
// signedness of the final base type.
func interpretValue(b []byte, bt op.BaseType) int64 {
	var u uint64
	for i := 0; i < bt.ByteSize && i < len(b); i++ {
		u |= uint64(b[i]) << (8 * uint(i))
	}
	if bt.Signed && bt.ByteSize < 8 {
		sign := uint64(1) << (uint(bt.ByteSize)*8 - 1)
		if u&sign != 0 {
			u |= ^((uint64(1) << (uint(bt.ByteSize) * 8)) - 1)
		}
	}
	return int64(u)
}

// finalTypeRef decodes a location expression and returns the type offset
// referenced by the last DW_OP_const_type, DW_OP_convert or
// DW_OP_reinterpret instruction, which is the type of the final value.
func finalTypeRef(expr []byte) (uint64, bool) {
	var last uint64
	found := false
	buf := bytes.NewBuffer(expr)
	for buf.Len() > 0 {
		opc, err := buf.ReadByte()
		if err != nil {
			break
		}
		switch op.Opcode(opc) {
		case op.DW_OP_const_type:
			off, _ := leb128.DecodeUnsigned(buf)
			sz, err := buf.ReadByte()
			if err != nil {
				return last, found
			}
			buf.Next(int(sz))
			last, found = off, true
		case op.DW_OP_convert, op.DW_OP_reinterpret:
			off, _ := leb128.DecodeUnsigned(buf)
			last, found = off, true
		case op.DW_OP_addr:
			buf.Next(8)
		case op.DW_OP_const1u, op.DW_OP_const1s, op.DW_OP_pick, op.DW_OP_deref_size, op.DW_OP_xderef_size:
			buf.Next(1)
		case op.DW_OP_const2u, op.DW_OP_const2s, op.DW_OP_bra, op.DW_OP_skip, op.DW_OP_call2:
			buf.Next(2)
		case op.DW_OP_const4u, op.DW_OP_const4s, op.DW_OP_call4, op.DW_OP_call_ref:
			buf.Next(4)
		case op.DW_OP_const8u, op.DW_OP_const8s:
			buf.Next(8)
		case op.DW_OP_constu, op.DW_OP_plus_uconst, op.DW_OP_regx, op.DW_OP_piece:
			leb128.DecodeUnsigned(buf)
		case op.DW_OP_consts, op.DW_OP_fbreg:
			leb128.DecodeSigned(buf)
		case op.DW_OP_bregx:
			leb128.DecodeUnsigned(buf)
			leb128.DecodeSigned(buf)
		case op.DW_OP_implicit_value:
			sz, _ := leb128.DecodeUnsigned(buf)
			buf.Next(int(sz))
		case op.DW_OP_bit_piece:
			leb128.DecodeUnsigned(buf)
			leb128.DecodeUnsigned(buf)
		default:
			// opcodes without operands: lit0-31, reg0-31, breg0-31,
			// stack_value, dup/drop/over/swap/rot, arithmetic, nop...
			if op.Opcode(opc) >= op.DW_OP_breg0 && op.Opcode(opc) <= op.DW_OP_breg31 {
				leb128.DecodeSigned(buf)
			}
		}
	}
	return last, found
}
