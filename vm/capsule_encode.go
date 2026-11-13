package vm

import (
	"encoding/binary"
	"fmt"
	"math"
	"regexp"
	"sort"

	"github.com/expr-lang/expr/builtin"
	"github.com/expr-lang/expr/vm/runtime"
)

// This file implements EncodeProgram, the encoding half of the portable
// program cache. The format is documented in docs/program-cache.md.

const (
	capsuleMagic0 = 'E'
	capsuleMagic1 = 'X'
	capsuleMagic2 = 'P'
	capsuleMagic3 = 'C'

	// capsuleVersion is the current cache format version.
	capsuleVersion = 1

	// maxCapsuleSize is the maximum size in bytes of an encoded program
	// cache. DecodeProgram refuses larger inputs.
	maxCapsuleSize = 8 << 20
)

// opHasConstPayload reports whether an instruction's argument is an index
// into the constant table. Such arguments are not stored in the cache; the
// constant value is stored inline instead and the table is rebuilt on load.
func opHasConstPayload(op Opcode) bool {
	switch op {
	case OpPush, OpLoadConst, OpLoadField, OpLoadFast, OpFetchField, OpMatchesConst:
		return true
	}
	return false
}

// capsuleWriter is a deterministic byte writer.
type capsuleWriter struct {
	buf []byte
}

func (w *capsuleWriter) byte(b byte) {
	w.buf = append(w.buf, b)
}

func (w *capsuleWriter) bytes(b []byte) {
	w.buf = append(w.buf, b...)
}

func (w *capsuleWriter) uvarint(v uint64) {
	var tmp [binary.MaxVarintLen64]byte
	n := binary.PutUvarint(tmp[:], v)
	w.buf = append(w.buf, tmp[:n]...)
}

func (w *capsuleWriter) svarint(v int64) {
	var tmp [binary.MaxVarintLen64]byte
	n := binary.PutVarint(tmp[:], v)
	w.buf = append(w.buf, tmp[:n]...)
}

func (w *capsuleWriter) str(s string) {
	w.uvarint(uint64(len(s)))
	w.buf = append(w.buf, s...)
}

// EncodeProgram encodes a compiled program into a relocatable byte form
// suitable for distribution to offline workers. The returned bytes can be
// loaded again with DecodeProgram (see the expr package wrappers).
//
// The encoding is deterministic: encoding the same program twice, or
// encoding a program produced by a successful DecodeProgram, yields
// identical bytes.
//
// Programs using features that cannot be relocated reliably (methods,
// profiled programs, typed calls of environment function values, constants
// outside the supported set, field accesses whose source cannot be
// determined) are rejected with an error; nothing is silently dropped.
func EncodeProgram(program *Program) ([]byte, error) {
	if program == nil {
		return nil, fmt.Errorf("cannot encode nil program")
	}
	meta := program.Capsule()
	if meta == nil {
		return nil, fmt.Errorf("program does not carry relocation metadata; recompile it with this version of expr")
	}
	if len(program.Bytecode) != len(program.Arguments) {
		return nil, fmt.Errorf("invalid program: bytecode and arguments length mismatch")
	}
	if len(program.Bytecode) != len(program.Locations()) {
		return nil, fmt.Errorf("invalid program: bytecode and locations length mismatch")
	}
	if len(meta.Funcs) != len(program.Functions()) {
		return nil, fmt.Errorf("invalid program: function metadata mismatch")
	}

	fieldRefs := make(map[int]FieldRef, len(meta.FieldRefs))
	for _, ref := range meta.FieldRefs {
		fieldRefs[ref.PC] = ref
	}
	builtinRefs := make(map[int]BuiltinRef, len(meta.BuiltinRefs))
	for _, ref := range meta.BuiltinRefs {
		builtinRefs[ref.PC] = ref
	}
	safeRefs := make(map[int]SafeRef, len(meta.SafeRefs))
	for _, ref := range meta.SafeRefs {
		safeRefs[ref.PC] = ref
	}

	w := &capsuleWriter{}
	w.byte(capsuleMagic0)
	w.byte(capsuleMagic1)
	w.byte(capsuleMagic2)
	w.byte(capsuleMagic3)
	w.uvarint(capsuleVersion)

	w.uvarint(uint64(program.Variables()))

	// Instruction stream.
	w.uvarint(uint64(len(program.Bytecode)))
	for _, op := range program.Bytecode {
		if err := checkOpSupported(op); err != nil {
			return nil, err
		}
		w.byte(byte(op))
	}
	functions := program.Functions()
	for pc, op := range program.Bytecode {
		arg := program.Arguments[pc]
		switch {
		case opHasConstPayload(op):
			if arg < 0 || arg >= len(program.Constants) {
				return nil, fmt.Errorf("invalid program: constant index %d out of range at instruction %d", arg, pc)
			}
		case op == OpCallBuiltin1:
			if arg < 0 || arg >= len(builtin.Builtins) {
				return nil, fmt.Errorf("invalid program: builtin index %d out of range at instruction %d", arg, pc)
			}
		default:
			switch op {
			case OpLoadFunc, OpCall0, OpCall1, OpCall2, OpCall3:
				if arg < 0 || arg >= len(functions) {
					return nil, fmt.Errorf("invalid program: function index %d out of range at instruction %d", arg, pc)
				}
			}
			w.svarint(int64(arg))
		}
	}

	// Constant and builtin payloads, in instruction order.
	for pc, op := range program.Bytecode {
		arg := program.Arguments[pc]
		switch {
		case opHasConstPayload(op):
			if err := w.encodeConstant(program, meta, safeRefs, fieldRefs, pc, op, program.Constants[arg]); err != nil {
				return nil, err
			}
		case op == OpCallBuiltin1:
			ref, ok := builtinRefs[pc]
			if !ok {
				return nil, fmt.Errorf("missing relocation metadata for builtin call at instruction %d", pc)
			}
			if got := builtin.Builtins[arg].Name; got != ref.Name {
				return nil, fmt.Errorf("builtin metadata mismatch at instruction %d: %q vs %q", pc, got, ref.Name)
			}
			w.str(ref.Name)
		}
	}

	// Function table.
	w.uvarint(uint64(len(meta.Funcs)))
	for _, fn := range meta.Funcs {
		if fn.Unsupported {
			return nil, fmt.Errorf("function %q declares a signature that cannot be stored in a program cache", fn.Name)
		}
		w.str(fn.Name)
		var flags byte
		if fn.Builtin {
			flags |= 1
		}
		w.byte(flags)
		w.uvarint(uint64(len(fn.Overloads)))
		for _, sig := range fn.Overloads {
			w.uvarint(uint64(len(sig.In)))
			for _, in := range sig.In {
				w.str(in)
			}
			w.uvarint(uint64(len(sig.Out)))
			for _, out := range sig.Out {
				w.str(out)
			}
		}
	}

	// Variable slot names (debug info), ordered by slot.
	debugInfo := program.DebugInfo()
	w.uvarint(uint64(program.Variables()))
	for i := 0; i < program.Variables(); i++ {
		w.str(debugInfo[fmt.Sprintf("var_%d", i)])
	}

	// Source locations.
	for _, loc := range program.Locations() {
		w.uvarint(uint64(loc.From))
		w.uvarint(uint64(loc.To))
	}

	// Source text, kept only to render error snippets at load/run time.
	w.str(program.Source().String())

	return w.buf, nil
}

func checkOpSupported(op Opcode) error {
	switch op {
	case OpLoadMethod, OpMethod:
		return fmt.Errorf("programs calling methods cannot be stored in a program cache")
	case OpCallTyped:
		return fmt.Errorf("programs calling typed environment function values cannot be stored in a program cache")
	case OpProfileStart, OpProfileEnd:
		return fmt.Errorf("programs compiled with profiling cannot be stored in a program cache")
	}
	if op > OpEnd {
		return fmt.Errorf("invalid program: unknown opcode %#x", byte(op))
	}
	return nil
}

// encodeConstant writes the payload of a constant-referencing instruction.
func (w *capsuleWriter) encodeConstant(program *Program, meta *CapsuleMeta, safeRefs map[int]SafeRef, fieldRefs map[int]FieldRef, pc int, op Opcode, constant any) error {
	switch op {
	case OpLoadConst, OpLoadFast:
		name, ok := constant.(string)
		if !ok {
			return fmt.Errorf("invalid program: unexpected constant %T for opcode %d at instruction %d", constant, op, pc)
		}
		w.str(name)
		return nil

	case OpLoadField, OpFetchField:
		ref, ok := fieldRefs[pc]
		if !ok {
			return fmt.Errorf("missing relocation metadata for field access at instruction %d", pc)
		}
		if ref.Unknown {
			return fmt.Errorf("cannot determine the source of field access %s; this expression cannot be stored in a program cache", ref.LogicalPath())
		}
		if ref.Leaf.Kind == ShapeUnsupported {
			return fmt.Errorf("field access %s fetches a value of a type that cannot be stored in a program cache", ref.LogicalPath())
		}
		if field, ok := constant.(*runtime.Field); ok {
			if !equalStringSlice(field.Path, ref.Path) {
				return fmt.Errorf("field metadata mismatch at instruction %d", pc)
			}
		} else {
			return fmt.Errorf("invalid program: unexpected constant %T for field access at instruction %d", constant, pc)
		}
		w.encodeFieldRef(ref)
		return nil

	case OpMatchesConst:
		re, ok := constant.(*regexp.Regexp)
		if !ok {
			return fmt.Errorf("invalid program: unexpected constant %T for matches at instruction %d", constant, pc)
		}
		w.str(re.String())
		return nil

	case OpPush:
		return w.encodeValue(program, meta, safeRefs, pc, constant, 0)
	}
	return fmt.Errorf("invalid program: opcode %d does not take a constant payload", op)
}

func (w *capsuleWriter) encodeFieldRef(ref FieldRef) {
	w.uvarint(uint64(len(ref.Prov)))
	for _, step := range ref.Prov {
		w.byte(byte(step.Kind))
		if step.Kind == ProvField {
			w.str(step.Name)
		}
	}
	w.uvarint(uint64(len(ref.Path)))
	for _, name := range ref.Path {
		w.str(name)
	}
	w.encodeShape(ref.Leaf)
}

func (w *capsuleWriter) encodeShape(shape LeafShape) {
	w.byte(byte(shape.Kind))
	switch shape.Kind {
	case ShapeScalar:
		w.str(shape.Scalar)
	case ShapePtr, ShapeSlice, ShapeArray:
		w.encodeShape(*shape.Elem)
	case ShapeMap:
		w.str(shape.Key)
		w.encodeShape(*shape.Elem)
	}
}

// Constant value tags. See docs/program-cache.md.
const (
	tagNil = iota
	tagBool
	tagInt
	tagInt8
	tagInt16
	tagInt32
	tagInt64
	tagUint
	tagUint8
	tagUint16
	tagUint32
	tagUint64
	tagFloat32
	tagFloat64
	tagString
	tagBytes
	tagSliceAny
	tagSliceString
	tagSliceInt
	tagSliceInt64
	tagSliceFloat64
	tagSliceBool
	tagMapStringAny
	tagMapStringStruct
	tagMapIntStruct
	tagRegexp
	tagError
	tagBuiltinSafe
)

const maxConstantDepth = 100

func (w *capsuleWriter) encodeValue(program *Program, meta *CapsuleMeta, safeRefs map[int]SafeRef, pc int, v any, depth int) error {
	if depth > maxConstantDepth {
		return fmt.Errorf("constant at instruction %d is nested too deeply", pc)
	}
	switch c := v.(type) {
	case nil:
		w.byte(tagNil)
	case bool:
		w.byte(tagBool)
		if c {
			w.byte(1)
		} else {
			w.byte(0)
		}
	case int:
		w.byte(tagInt)
		w.svarint(int64(c))
	case int8:
		w.byte(tagInt8)
		w.svarint(int64(c))
	case int16:
		w.byte(tagInt16)
		w.svarint(int64(c))
	case int32:
		w.byte(tagInt32)
		w.svarint(int64(c))
	case int64:
		w.byte(tagInt64)
		w.svarint(c)
	case uint:
		w.byte(tagUint)
		w.uvarint(uint64(c))
	case uint8:
		w.byte(tagUint8)
		w.uvarint(uint64(c))
	case uint16:
		w.byte(tagUint16)
		w.uvarint(uint64(c))
	case uint32:
		w.byte(tagUint32)
		w.uvarint(uint64(c))
	case uint64:
		w.byte(tagUint64)
		w.uvarint(c)
	case float32:
		w.byte(tagFloat32)
		w.uvarint(uint64(math.Float32bits(c)))
	case float64:
		w.byte(tagFloat64)
		w.uvarint(math.Float64bits(c))
	case string:
		w.byte(tagString)
		w.str(c)
	case []byte:
		w.byte(tagBytes)
		w.uvarint(uint64(len(c)))
		w.bytes(c)
	case []any:
		w.byte(tagSliceAny)
		w.uvarint(uint64(len(c)))
		for _, item := range c {
			if err := w.encodeValue(program, meta, safeRefs, pc, item, depth+1); err != nil {
				return err
			}
		}
	case []string:
		w.byte(tagSliceString)
		w.uvarint(uint64(len(c)))
		for _, item := range c {
			w.str(item)
		}
	case []int:
		w.byte(tagSliceInt)
		w.uvarint(uint64(len(c)))
		for _, item := range c {
			w.svarint(int64(item))
		}
	case []int64:
		w.byte(tagSliceInt64)
		w.uvarint(uint64(len(c)))
		for _, item := range c {
			w.svarint(item)
		}
	case []float64:
		w.byte(tagSliceFloat64)
		w.uvarint(uint64(len(c)))
		for _, item := range c {
			w.uvarint(math.Float64bits(item))
		}
	case []bool:
		w.byte(tagSliceBool)
		w.uvarint(uint64(len(c)))
		for _, item := range c {
			if item {
				w.byte(1)
			} else {
				w.byte(0)
			}
		}
	case map[string]any:
		w.byte(tagMapStringAny)
		keys := make([]string, 0, len(c))
		for k := range c {
			keys = append(keys, k)
		}
		sort.Strings(keys)
		w.uvarint(uint64(len(keys)))
		for _, k := range keys {
			w.str(k)
			if err := w.encodeValue(program, meta, safeRefs, pc, c[k], depth+1); err != nil {
				return err
			}
		}
	case map[string]struct{}:
		w.byte(tagMapStringStruct)
		keys := make([]string, 0, len(c))
		for k := range c {
			keys = append(keys, k)
		}
		sort.Strings(keys)
		w.uvarint(uint64(len(keys)))
		for _, k := range keys {
			w.str(k)
		}
	case map[int]struct{}:
		w.byte(tagMapIntStruct)
		keys := make([]int, 0, len(c))
		for k := range c {
			keys = append(keys, k)
		}
		sort.Ints(keys)
		w.uvarint(uint64(len(keys)))
		for _, k := range keys {
			w.svarint(int64(k))
		}
	case *regexp.Regexp:
		w.byte(tagRegexp)
		w.str(c.String())
	case error:
		w.byte(tagError)
		w.str(c.Error())
	case SafeFunction:
		ref, ok := safeRefs[pc]
		if !ok {
			return fmt.Errorf("missing relocation metadata for builtin safe function at instruction %d", pc)
		}
		w.byte(tagBuiltinSafe)
		w.str(ref.Name)
	case *Span:
		return fmt.Errorf("programs compiled with profiling cannot be stored in a program cache")
	case *runtime.Method:
		return fmt.Errorf("programs calling methods cannot be stored in a program cache")
	case *runtime.Field:
		return fmt.Errorf("invalid program: unexpected field constant at instruction %d", pc)
	default:
		return fmt.Errorf("constant of type %T at instruction %d cannot be stored in a program cache", v, pc)
	}
	return nil
}
