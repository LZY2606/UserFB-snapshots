package godwarf_test

import (
	"bytes"
	"debug/dwarf"
	"debug/elf"
	"fmt"
	"strings"
	"testing"

	"github.com/go-delve/delve/pkg/dwarf/godwarf"
	"github.com/go-delve/delve/pkg/dwarf/op"
	"github.com/go-delve/delve/task_support/taskcheck"
)

const fixturePath = "../../../task_support/fixtures/typed.elf"

type fixtureVar struct {
	name   string
	dieOff dwarf.Offset
	loc    []byte
}

type fixtureCU struct {
	headerOff dwarf.Offset
	resolve   op.BaseTypeResolver
	vars      []fixtureVar
}

// loadFixture reads the bundled ELF and returns its compilation units with
// per-unit base type resolvers and the real location expressions of every
// variable.
func loadFixture(t *testing.T) ([]byte, []fixtureCU) {
	t.Helper()
	f, err := elf.Open(fixturePath)
	if err != nil {
		t.Fatalf("open fixture: %v", err)
	}
	defer f.Close()
	dw, err := f.DWARF()
	if err != nil {
		t.Fatalf("DWARF: %v", err)
	}
	debugInfo, err := f.Section(".debug_info").Data()
	if err != nil {
		t.Fatalf("debug_info: %v", err)
	}

	var cus []fixtureCU
	r := dw.Reader()
	for {
		e, err := r.Next()
		if err != nil {
			t.Fatalf("read entry: %v", err)
		}
		if e == nil {
			break
		}
		switch e.Tag {
		case dwarf.TagCompileUnit:
			headerOff := e.Offset - 12 // DWARF5 32-bit header
			resolve, err := godwarf.NewBaseTypeResolver(dw, debugInfo, headerOff)
			if err != nil {
				t.Fatalf("resolver for CU at %#x: %v", headerOff, err)
			}
			cus = append(cus, fixtureCU{headerOff: headerOff, resolve: resolve})
		case dwarf.TagVariable:
			if len(cus) == 0 {
				t.Fatalf("variable outside CU")
			}
			name, _ := e.Val(dwarf.AttrName).(string)
			loc, _ := e.Val(dwarf.AttrLocation).([]byte)
			cu := &cus[len(cus)-1]
			cu.vars = append(cu.vars, fixtureVar{name: name, dieOff: e.Offset, loc: loc})
		}
	}
	return debugInfo, cus
}

func (cu fixtureCU) variable(name string) fixtureVar {
	for _, v := range cu.vars {
		if v.name == name {
			return v
		}
	}
	return fixtureVar{}
}

// interpretPiece reads the raw low bytes of an immediate piece using the
// signedness of the final base type.
func interpretPiece(b []byte, bt op.BaseType) int64 {
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

// evalTyped evaluates a single-value typed program ending in
// DW_OP_stack_value and returns the piece bytes.
func evalTyped(resolve op.BaseTypeResolver, prog []byte) (int64, []op.Piece, error) {
	return op.ExecuteStackProgramTyped(op.DwarfRegisters{}, prog, 8, nil, resolve)
}

// TestTypedExpressionValues recovers the variables of the bundled ELF through
// the typed executor and checks the recovered values, piece bytes, type DIE
// identities and the printed instruction stream.
func TestTypedExpressionValues(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		_, cus := loadFixture(t)
		c.Check(len(cus) == 2, "expected 2 compilation units, got %d", len(cus))
		if len(cus) != 2 {
			return
		}

		wantValue := []int64{-2, 254}
		wantBytes := [][]byte{{0xfe, 0xff}, {0xfe, 0x00}}
		for i, cu := range cus {
			// plain: DW_OP_lit7 evaluates to the address 7, there is no
			// DW_OP_stack_value so nothing is dereferenced.
			plain := cu.variable("plain")
			c.Check(bytes.Equal(plain.loc, []byte{0x37}), "CU %d plain location %x", i, plain.loc)
			val, pieces, err := evalTyped(cu.resolve, plain.loc)
			c.Check(err == nil, "CU %d plain eval: %v", i, err)
			c.Check(val == 7 && pieces == nil, "CU %d plain = address %d pieces %v", i, val, pieces)

			// narrow: same expression bytes in both CUs, different results.
			narrow := cu.variable("narrow")
			val, pieces, err = evalTyped(cu.resolve, narrow.loc)
			c.Check(err == nil, "CU %d narrow eval: %v", i, err)
			if err != nil || len(pieces) != 1 {
				c.Check(false, "CU %d narrow pieces %v err %v", i, pieces, err)
				continue
			}
			p := pieces[0]
			c.Check(p.Kind == op.ImmPiece, "CU %d narrow piece kind %v", i, p.Kind)
			c.Check(bytes.Equal(p.Bytes, wantBytes[i]), "CU %d narrow bytes %x want %x", i, p.Bytes, wantBytes[i])
			bt, err := cu.resolve(0x15) // final type after DW_OP_convert
			c.Check(err == nil, "CU %d resolve final type: %v", i, err)
			c.Check(bt.ByteSize == 2 && bt.Signed, "CU %d final type %+v", i, bt)
			got := interpretPiece(p.Bytes, bt)
			c.Check(got == wantValue[i], "CU %d narrow value %d want %d", i, got, wantValue[i])

			// The printed instruction stream must show the type offset and
			// the constant bytes and must not lose the instructions after
			// the constant block.
			var pp strings.Builder
			op.PrettyPrint(&pp, narrow.loc, nil)
			out := pp.String()
			c.Check(strings.Contains(out, "DW_OP_const_type 0xd 1 [fe]"), "CU %d pretty %q", i, out)
			c.Check(strings.Contains(out, "DW_OP_convert 0x15"), "CU %d pretty %q", i, out)
			c.Check(strings.Contains(out, "DW_OP_stack_value"), "CU %d pretty %q", i, out)
		}

		// Type DIE identity: both CUs use the same CU-relative offsets but
		// they are different DIEs at different global offsets with different
		// signedness for "byte".
		bt0, err0 := cus[0].resolve(0xd)
		bt1, err1 := cus[1].resolve(0xd)
		c.Check(err0 == nil && err1 == nil, "resolve byte: %v %v", err0, err1)
		c.Check(bt0.Signed && !bt1.Signed, "byte signedness CU0=%v CU1=%v", bt0.Signed, bt1.Signed)
		c.Check(cus[0].headerOff != cus[1].headerOff, "CU header offsets must differ")
		c.Check(cus[0].headerOff+0xd != cus[1].headerOff+0xd, "global byte DIE offsets must differ")

		// Reordering after conversion: the type follows the value through
		// swap/drop/rot and a second piece does not inherit it.
		prog := append([]byte{0xa4, 0x0d, 1, 0xfe, 0xa8, 0x15}, // const_type byte fe; convert word
			0x33, 0x35, // lit3 lit5
			0x17, // rot
			0x13, // drop
			0x16, // swap
			0x13, // drop
			0x9f) // stack_value
		_, pieces, err := evalTyped(cus[0].resolve, prog)
		c.Check(err == nil, "reorder eval: %v", err)
		c.Check(len(pieces) == 1 && bytes.Equal(pieces[0].Bytes, []byte{0xfe, 0xff}), "reorder pieces %v", pieces)

		// dup keeps the type: -2 + -2 in signed16 truncates to -4.
		prog = []byte{0xa4, 0x0d, 1, 0xfe, 0xa8, 0x15, 0x12, 0x22, 0x9f}
		_, pieces, err = evalTyped(cus[0].resolve, prog)
		c.Check(err == nil, "dup/plus eval: %v", err)
		c.Check(len(pieces) == 1 && bytes.Equal(pieces[0].Bytes, []byte{0xfc, 0xff}), "dup/plus pieces %v", pieces)

		// Errors do not poison later evaluations with the same resolver.
		bad := []byte{0xa4, 0x0d, 1, 0xfe, 0xa4, 0x15, 2, 1, 0, 0x22} // byte + word: type mismatch
		_, _, err = evalTyped(cus[0].resolve, bad)
		c.Check(err != nil, "type mismatch must fail")
		bad = []byte{0xa4, 0x0d, 1, 0xfe, 0xa4, 0x0d, 1, 0x00, 0x1b} // signed8 div by zero
		_, _, err = evalTyped(cus[0].resolve, bad)
		c.Check(err == op.ErrDivisionByZero, "div by zero err %v", err)
		bad = []byte{0xa4, 0x0d, 2, 0xfe, 0xff} // block size 2 != type size 1
		_, _, err = evalTyped(cus[0].resolve, bad)
		c.Check(err != nil, "const_type size mismatch must fail")
		bad = []byte{0xa4, 0x0d, 1, 0xfe, 0xa9, 0x15} // reinterpret 1 byte as 2 bytes
		_, _, err = evalTyped(cus[0].resolve, bad)
		c.Check(err != nil, "reinterpret width mismatch must fail")
		narrow := cus[0].variable("narrow")
		_, pieces, err = evalTyped(cus[0].resolve, narrow.loc)
		c.Check(err == nil && len(pieces) == 1 && bytes.Equal(pieces[0].Bytes, []byte{0xfe, 0xff}),
			"evaluation after errors: pieces %v err %v", pieces, err)
	})
}

// TestTypedExpressionUnits checks the per-CU base type resolvers and the
// conversion/reinterpretation semantics of the typed executor.
func TestTypedExpressionUnits(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		debugInfo, cus := loadFixture(t)
		c.Check(len(cus) == 2, "expected 2 compilation units, got %d", len(cus))
		if len(cus) != 2 {
			return
		}

		// The same CU-relative offset in interleaved resolver calls must not
		// share cached results across units.
		for round := 0; round < 2; round++ {
			bt0, err0 := cus[0].resolve(0xd)
			bt1, err1 := cus[1].resolve(0xd)
			c.Check(err0 == nil && err1 == nil, "interleaved resolve: %v %v", err0, err1)
			c.Check(bt0 == (op.BaseType{ByteSize: 1, Signed: true}), "CU0 byte %+v", bt0)
			c.Check(bt1 == (op.BaseType{ByteSize: 1, Signed: false}), "CU1 byte %+v", bt1)
		}
		for i, cu := range cus {
			w, err := cu.resolve(0x15)
			c.Check(err == nil && w == (op.BaseType{ByteSize: 2, Signed: true}), "CU %d word %+v %v", i, w, err)
			w, err = cu.resolve(0x1d)
			c.Check(err == nil && w == (op.BaseType{ByteSize: 8, Signed: false}), "CU %d wide %+v %v", i, w, err)
		}

		// Resolver rejections: offsets outside the CU, non-DIE starts and
		// non-base-type DIEs.
		_, err := cus[0].resolve(0)
		c.Check(err != nil, "offset 0 must be rejected")
		_, err = cus[0].resolve(0x7fff)
		c.Check(err != nil, "offset outside CU must be rejected")
		narrow := cus[0].variable("narrow")
		varRel := uint64(narrow.dieOff - cus[0].headerOff)
		_, err = cus[0].resolve(varRel)
		c.Check(err != nil, "variable DIE offset must be rejected")
		_, err = cus[0].resolve(varRel + 1)
		c.Check(err != nil, "non-DIE-start offset must be rejected")
		_, err = godwarf.NewBaseTypeResolver(mustDWARF(t), debugInfo, dwarf.Offset(len(debugInfo)))
		c.Check(err != nil, "CU offset out of range must be rejected")
		_, err = godwarf.NewBaseTypeResolver(mustDWARF(t), debugInfo, narrow.dieOff)
		c.Check(err != nil, "CU offset not at a unit header must be rejected")

		// Hand-built resolver covering every supported width and signedness.
		types := map[uint64]op.BaseType{
			1: {ByteSize: 1, Signed: true},
			2: {ByteSize: 1, Signed: false},
			3: {ByteSize: 2, Signed: true},
			4: {ByteSize: 2, Signed: false},
			5: {ByteSize: 4, Signed: true},
			6: {ByteSize: 8, Signed: false},
			7: {ByteSize: 1, Signed: true}, // same shape as 1, different identity
		}
		resolve := func(off uint64) (op.BaseType, error) {
			bt, ok := types[off]
			if !ok {
				return op.BaseType{}, fmt.Errorf("unknown type offset %#x", off)
			}
			return bt, nil
		}

		// signed8 0xfe converted to signed16 sign-extends: 0xfffe = -2.
		_, pieces, err := evalTyped(resolve, []byte{0xa4, 1, 1, 0xfe, 0xa8, 3, 0x9f})
		c.Check(err == nil, "signed convert: %v", err)
		c.Check(len(pieces) == 1 && bytes.Equal(pieces[0].Bytes, []byte{0xfe, 0xff}), "signed convert pieces %v", pieces)

		// Reinterpreted as unsigned8 first, the same byte converts to 0x00fe = 254.
		_, pieces, err = evalTyped(resolve, []byte{0xa4, 1, 1, 0xfe, 0xa9, 2, 0xa8, 3, 0x9f})
		c.Check(err == nil, "reinterpret+convert: %v", err)
		c.Check(len(pieces) == 1 && bytes.Equal(pieces[0].Bytes, []byte{0xfe, 0x00}), "reinterpret+convert pieces %v", pieces)

		// Division observes signedness: signed8 -2/2 = -1, unsigned8 254/2 = 127.
		val, pieces, err := evalTyped(resolve, []byte{0xa4, 1, 1, 0xfe, 0xa4, 1, 1, 0x02, 0x1b, 0x9f})
		c.Check(err == nil && len(pieces) == 1 && bytes.Equal(pieces[0].Bytes, []byte{0xff}), "signed div %v %v", pieces, err)
		_ = val
		_, pieces, err = evalTyped(resolve, []byte{0xa4, 2, 1, 0xfe, 0xa4, 2, 1, 0x02, 0x1b, 0x9f})
		c.Check(err == nil && len(pieces) == 1 && bytes.Equal(pieces[0].Bytes, []byte{0x7f}), "unsigned div %v %v", pieces, err)

		// Comparisons observe signedness and produce a generic 0 or 1.
		val, _, err = evalTyped(resolve, []byte{0xa4, 1, 1, 0xfe, 0xa4, 1, 1, 0x01, 0x2d}) // -2 < 1
		c.Check(err == nil && val == 1, "signed lt = %d, %v", val, err)
		val, _, err = evalTyped(resolve, []byte{0xa4, 2, 1, 0xfe, 0xa4, 2, 1, 0x01, 0x2d}) // 254 < 1
		c.Check(err == nil && val == 0, "unsigned lt = %d, %v", val, err)
		// The generic comparison result cannot be mixed with a typed operand.
		_, _, err = evalTyped(resolve, []byte{0xa4, 1, 1, 0xfe, 0xa4, 1, 1, 0x01, 0x2d, 0xa4, 1, 1, 0x01, 0x22})
		c.Check(err != nil, "generic comparison result mixed with typed operand must fail")

		// Same-shape types with different identities are not interchangeable.
		_, _, err = evalTyped(resolve, []byte{0xa4, 1, 1, 0xfe, 0xa4, 7, 1, 0x01, 0x22})
		c.Check(err != nil, "same-shape different-identity plus must fail")

		// Arithmetic truncates to the type width: signed8 127+1 = -128.
		_, pieces, err = evalTyped(resolve, []byte{0xa4, 1, 1, 0x7f, 0xa4, 1, 1, 0x01, 0x22, 0x9f})
		c.Check(err == nil && len(pieces) == 1 && bytes.Equal(pieces[0].Bytes, []byte{0x80}), "truncating plus %v %v", pieces, err)

		// Shifts: generic non-negative count, shr zero-fills, shra sign-extends.
		_, pieces, err = evalTyped(resolve, []byte{0xa4, 1, 1, 0xfe, 0x31, 0x26, 0x9f}) // signed8 shra 1
		c.Check(err == nil && len(pieces) == 1 && bytes.Equal(pieces[0].Bytes, []byte{0xff}), "shra %v %v", pieces, err)
		_, pieces, err = evalTyped(resolve, []byte{0xa4, 1, 1, 0xfe, 0x31, 0x25, 0x9f}) // signed8 shr 1
		c.Check(err == nil && len(pieces) == 1 && bytes.Equal(pieces[0].Bytes, []byte{0x7f}), "shr %v %v", pieces, err)
		_, pieces, err = evalTyped(resolve, []byte{0xa4, 2, 1, 0xfe, 0x31, 0x26, 0x9f}) // unsigned8 shra 1
		c.Check(err == nil && len(pieces) == 1 && bytes.Equal(pieces[0].Bytes, []byte{0x7f}), "unsigned shra %v %v", pieces, err)
		_, _, err = evalTyped(resolve, []byte{0xa4, 1, 1, 0xfe, 0xa4, 1, 1, 0x01, 0x24}) // typed shift count
		c.Check(err != nil, "typed shift count must fail")
		_, _, err = evalTyped(resolve, []byte{0xa4, 1, 1, 0xfe, 0x11, 0x7f, 0x25}) // consts -1; shr
		c.Check(err != nil, "negative shift count must fail")

		// plus_uconst preserves the left type and truncates: signed8 127+1 = -128.
		_, pieces, err = evalTyped(resolve, []byte{0xa4, 1, 1, 0x7f, 0x23, 0x01, 0x9f})
		c.Check(err == nil && len(pieces) == 1 && bytes.Equal(pieces[0].Bytes, []byte{0x80}), "plus_uconst %v %v", pieces, err)

		// Unary ops keep the type: signed8 neg 127 = -127, not 127 = -128.
		_, pieces, err = evalTyped(resolve, []byte{0xa4, 1, 1, 0x7f, 0x1f, 0x9f})
		c.Check(err == nil && len(pieces) == 1 && bytes.Equal(pieces[0].Bytes, []byte{0x81}), "neg %v %v", pieces, err)
		_, pieces, err = evalTyped(resolve, []byte{0xa4, 1, 1, 0x7f, 0x20, 0x9f})
		c.Check(err == nil && len(pieces) == 1 && bytes.Equal(pieces[0].Bytes, []byte{0x80}), "not %v %v", pieces, err)

		// convert 0 produces the generic type, which mixes with generic values.
		val, _, err = evalTyped(resolve, []byte{0xa4, 1, 1, 0xfe, 0xa8, 0x00, 0x09, 0xfe, 0x22}) // convert 0; const1s -2; plus
		c.Check(err == nil && val == -4, "convert to generic = %d, %v", val, err)

		// Resolution failure is reported.
		_, _, err = evalTyped(resolve, []byte{0xa4, 0x7f, 1, 0xfe})
		c.Check(err != nil, "unknown type offset must fail")
	})
}

// TestTypedExpressionCompatibility checks that the legacy untyped entry point
// keeps its behavior, accepts the generic-type forms of the new opcodes, and
// that the register and memory paths still work through both entries.
func TestTypedExpressionCompatibility(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Untyped programs behave exactly as before.
		val, pieces, err := op.ExecuteStackProgram(op.DwarfRegisters{}, []byte{0x11, 0x1c, 0x11, 0x1c, 0x22}, 8, nil)
		c.Check(err == nil && val == 56 && pieces == nil, "legacy consts/plus = %d, %v", val, err)
		val, _, err = op.ExecuteStackProgram(op.DwarfRegisters{}, []byte{0x37, 0x39, 0x22}, 8, nil)
		c.Check(err == nil && val == 16, "legacy lit7+lit9 = %d, %v", val, err)
		val, _, err = op.ExecuteStackProgram(op.DwarfRegisters{}, []byte{0x09, 0x88}, 8, nil)
		c.Check(err == nil && val == -120, "legacy const1s = %d, %v", val, err)
		val, _, err = op.ExecuteStackProgram(op.DwarfRegisters{}, []byte{0x31, 0x30, 0x15, 0x01}, 8, nil)
		c.Check(err == nil && val == 1, "legacy pick = %d, %v", val, err)

		// The legacy entry directly accepts convert/reinterpret of the
		// generic type (offset 0).
		val, _, err = op.ExecuteStackProgram(op.DwarfRegisters{}, []byte{0x35, 0xa8, 0x00}, 8, nil)
		c.Check(err == nil && val == 5, "legacy convert 0 = %d, %v", val, err)
		val, _, err = op.ExecuteStackProgram(op.DwarfRegisters{}, []byte{0x35, 0xa9, 0x00}, 8, nil)
		c.Check(err == nil && val == 5, "legacy reinterpret 0 = %d, %v", val, err)

		// Named types need a resolver; the legacy entry reports an error
		// instead of failing silently.
		_, _, err = op.ExecuteStackProgram(op.DwarfRegisters{}, []byte{0xa4, 0x01, 1, 0xfe}, 8, nil)
		c.Check(err != nil, "const_type without resolver must fail")
		_, _, err = op.ExecuteStackProgram(op.DwarfRegisters{}, []byte{0x35, 0xa8, 0x01}, 8, nil)
		c.Check(err != nil, "convert to named type without resolver must fail")

		// The typed entry still runs plain untyped programs.
		val, _, err = op.ExecuteStackProgramTyped(op.DwarfRegisters{}, []byte{0x32, 0x33, 0x22}, 8, nil, nil)
		c.Check(err == nil && val == 5, "typed entry generic program = %d, %v", val, err)

		// Division by zero is an error, not a panic, through both entries.
		_, _, err = op.ExecuteStackProgram(op.DwarfRegisters{}, []byte{0x34, 0x30, 0x1b}, 8, nil)
		c.Check(err == op.ErrDivisionByZero, "legacy div by zero err %v", err)

		// Register path: CFA based expressions keep working.
		regs := op.DwarfRegisters{CFA: 0x1000}
		val, _, err = op.ExecuteStackProgram(regs, []byte{0x9c, 0x38, 0x22}, 8, nil)
		c.Check(err == nil && val == 0x1008, "legacy CFA = %#x, %v", uint64(val), err)
		val, _, err = op.ExecuteStackProgramTyped(regs, []byte{0x9c, 0x38, 0x22}, 8, nil, nil)
		c.Check(err == nil && val == 0x1008, "typed CFA = %#x, %v", uint64(val), err)

		// Memory path: deref still uses the supplied readMemory callback.
		readMem := func(buf []byte, addr uint64) (int, error) {
			if addr != 0x10 || len(buf) != 4 {
				return 0, fmt.Errorf("unexpected read of %d bytes at %#x", len(buf), addr)
			}
			return copy(buf, []byte{0x78, 0x56, 0x34, 0x12}), nil
		}
		val, _, err = op.ExecuteStackProgram(op.DwarfRegisters{}, []byte{0x08, 0x10, 0x94, 0x04}, 8, readMem)
		c.Check(err == nil && val == 0x12345678, "legacy deref = %#x, %v", uint64(val), err)
		val, _, err = op.ExecuteStackProgramTyped(op.DwarfRegisters{}, []byte{0x08, 0x10, 0x94, 0x04}, 8, readMem, nil)
		c.Check(err == nil && val == 0x12345678, "typed deref = %#x, %v", uint64(val), err)
	})
}

func mustDWARF(t *testing.T) *dwarf.Data {
	t.Helper()
	f, err := elf.Open(fixturePath)
	if err != nil {
		t.Fatalf("open fixture: %v", err)
	}
	defer f.Close()
	dw, err := f.DWARF()
	if err != nil {
		t.Fatalf("DWARF: %v", err)
	}
	return dw
}
