package vm

import (
	"encoding/binary"
	"errors"
	"fmt"
	"math"
	"reflect"
	"regexp"

	"github.com/expr-lang/expr/builtin"
	"github.com/expr-lang/expr/checker/nature"
	"github.com/expr-lang/expr/conf"
	"github.com/expr-lang/expr/file"
	"github.com/expr-lang/expr/vm/runtime"
)

// This file implements DecodeProgram, the loading half of the portable
// program cache. It binds a relocatable encoded program to the current
// environment, extension functions and builtin configuration without
// recompiling anything: no parser, checker or compiler is involved, and the
// stored source text is only used to render runtime error snippets.

// capsuleReader is a bounds-checked deterministic reader. The first error
// is sticky.
type capsuleReader struct {
	data []byte
	off  int
	err  error
}

func (r *capsuleReader) fail(format string, args ...any) {
	if r.err == nil {
		r.err = fmt.Errorf(format, args...)
	}
}

func (r *capsuleReader) remaining() int {
	return len(r.data) - r.off
}

func (r *capsuleReader) byte() byte {
	if r.err != nil {
		return 0
	}
	if r.remaining() < 1 {
		r.fail("program cache is truncated")
		return 0
	}
	b := r.data[r.off]
	r.off++
	return b
}

func (r *capsuleReader) bytes(n uint64) []byte {
	if r.err != nil {
		return nil
	}
	if n > uint64(r.remaining()) {
		r.fail("program cache is truncated")
		return nil
	}
	out := make([]byte, n)
	copy(out, r.data[r.off:r.off+int(n)])
	r.off += int(n)
	return out
}

func (r *capsuleReader) uvarint() uint64 {
	if r.err != nil {
		return 0
	}
	v, n := binary.Uvarint(r.data[r.off:])
	if n <= 0 {
		r.fail("program cache contains a corrupt varint")
		return 0
	}
	r.off += n
	return v
}

func (r *capsuleReader) svarint() int64 {
	if r.err != nil {
		return 0
	}
	v, n := binary.Varint(r.data[r.off:])
	if n <= 0 {
		r.fail("program cache contains a corrupt varint")
		return 0
	}
	r.off += n
	return v
}

func (r *capsuleReader) str() string {
	n := r.uvarint()
	b := r.bytes(n)
	if r.err != nil {
		return ""
	}
	return string(b)
}

// count reads a collection size and refuses it before any allocation if the
// declared number of items cannot possibly fit in the remaining input.
func (r *capsuleReader) count(minPerItem uint64) int {
	n := r.uvarint()
	if r.err != nil {
		return 0
	}
	if n > uint64(r.remaining())/minPerItem {
		r.fail("program cache declares an impossibly large collection of %d items", n)
		return 0
	}
	return int(n)
}

// DecodeProgram loads a program cache produced by EncodeProgram and binds it
// to the given configuration: the environment declared with expr.Env, the
// extension functions declared with expr.Function and the current builtin
// configuration (including disabled builtins).
//
// The loader never calls Compile or ParseCheck and never evaluates user
// functions or environment data; binding uses type information only. Field
// accesses are re-resolved by logical path against the current environment
// and functions are re-linked by name. Any incompatibility (missing or
// hidden fields, changed leaf types, missing functions, disabled builtins,
// signature mismatches) is reported before the program is returned.
func DecodeProgram(data []byte, config *conf.Config) (*Program, error) {
	if len(data) > maxCapsuleSize {
		return nil, fmt.Errorf("program cache of %d bytes exceeds the %d bytes limit", len(data), maxCapsuleSize)
	}
	if config == nil {
		config = conf.CreateNew()
	}

	r := &capsuleReader{data: data}
	if r.byte() != capsuleMagic0 || r.byte() != capsuleMagic1 || r.byte() != capsuleMagic2 || r.byte() != capsuleMagic3 {
		return nil, fmt.Errorf("not a program cache: bad magic")
	}
	if v := r.uvarint(); v != capsuleVersion {
		return nil, fmt.Errorf("unsupported program cache version %d (expected %d)", v, capsuleVersion)
	}
	if r.err != nil {
		return nil, r.err
	}

	variables := r.count(1)
	codeLen := r.count(1)

	bytecode := make([]Opcode, codeLen)
	for i := range bytecode {
		op := Opcode(r.byte())
		if r.err == nil {
			if err := checkOpSupported(op); err != nil {
				r.fail("program cache: %v", err)
			}
		}
		bytecode[i] = op
	}

	arguments := make([]int, codeLen)
	for pc, op := range bytecode {
		if opHasConstPayload(op) || op == OpCallBuiltin1 {
			continue // resolved from payloads below
		}
		arguments[pc] = int(r.svarint())
	}

	constants := make([]any, 0, 8)
	var fieldRefs []FieldRef
	var builtinRefs []BuiltinRef
	var safeRefs []SafeRef
	for pc, op := range bytecode {
		switch {
		case opHasConstPayload(op):
			constant, ref, safe := r.readConstantPayload(op, pc)
			if r.err != nil {
				return nil, r.err
			}
			if ref != nil {
				fieldRefs = append(fieldRefs, *ref)
				continue // constant slot assigned during binding
			}
			if safe != nil {
				safeRefs = append(safeRefs, *safe)
				continue // constant slot assigned during binding
			}
			arguments[pc] = len(constants)
			constants = append(constants, constant)
		case op == OpCallBuiltin1:
			name := r.str()
			builtinRefs = append(builtinRefs, BuiltinRef{PC: pc, Name: name})
		}
	}

	numFuncs := r.count(1)
	funcMetas := make([]FuncMeta, 0, numFuncs)
	for i := 0; i < numFuncs; i++ {
		fn := FuncMeta{Name: r.str()}
		flags := r.byte()
		fn.Builtin = flags&1 != 0
		fn.Unsupported = flags&2 != 0
		numOverloads := r.count(1)
		for j := 0; j < numOverloads; j++ {
			var sig FuncSignature
			numIn := r.count(1)
			for k := 0; k < numIn; k++ {
				sig.In = append(sig.In, r.str())
			}
			numOut := r.count(1)
			for k := 0; k < numOut; k++ {
				sig.Out = append(sig.Out, r.str())
			}
			fn.Overloads = append(fn.Overloads, sig)
		}
		funcMetas = append(funcMetas, fn)
	}

	numVars := r.count(1)
	if r.err == nil && numVars != variables {
		r.fail("program cache declares %d variables but stores %d variable names", variables, numVars)
	}
	debugInfo := make(map[string]string, numVars+numFuncs)
	for i := 0; i < numVars; i++ {
		debugInfo[fmt.Sprintf("var_%d", i)] = r.str()
	}

	locations := make([]file.Location, codeLen)
	for i := range locations {
		from := r.uvarint()
		to := r.uvarint()
		locations[i] = file.Location{From: int(from), To: int(to)}
	}

	source := r.str()

	if r.err != nil {
		return nil, r.err
	}
	if r.off != len(r.data) {
		return nil, fmt.Errorf("program cache has %d trailing bytes", len(r.data)-r.off)
	}

	// Reference range checks that do not depend on the environment.
	for pc, op := range bytecode {
		arg := arguments[pc]
		switch op {
		case OpLoadFunc, OpCall0, OpCall1, OpCall2, OpCall3:
			if arg < 0 || arg >= len(funcMetas) {
				return nil, fmt.Errorf("program cache: function index %d out of range at instruction %d", arg, pc)
			}
		case OpStore, OpLoadVar:
			if arg < 0 || arg >= variables {
				return nil, fmt.Errorf("program cache: variable slot %d out of range at instruction %d", arg, pc)
			}
		}
	}

	// Binding phase: resolve everything against the current configuration.
	binder := &capsuleBinder{
		config:    config,
		cache:     &nature.Cache{},
		constants: constants,
		arguments: arguments,
		debugInfo: debugInfo,
	}
	if err := binder.bindFields(fieldRefs); err != nil {
		return nil, err
	}
	if err := binder.bindBuiltins(builtinRefs); err != nil {
		return nil, err
	}
	if err := binder.bindSafeFuncs(safeRefs); err != nil {
		return nil, err
	}
	functions, err := binder.bindFunctions(funcMetas)
	if err != nil {
		return nil, err
	}

	program := NewProgram(
		file.NewSource(source),
		nil,
		locations,
		variables,
		binder.constants,
		bytecode,
		arguments,
		functions,
		debugInfo,
		nil,
	)
	program.SetCapsule(&CapsuleMeta{
		FieldRefs:   fieldRefs,
		BuiltinRefs: builtinRefs,
		SafeRefs:    safeRefs,
		Funcs:       funcMetas,
	})
	return program, nil
}

// readConstantPayload reads the inline payload of a constant-referencing
// instruction. Field accesses and builtin safe functions are returned as
// references to be resolved during the binding phase.
func (r *capsuleReader) readConstantPayload(op Opcode, pc int) (constant any, field *FieldRef, safe *SafeRef) {
	switch op {
	case OpLoadConst, OpLoadFast:
		return r.str(), nil, nil

	case OpLoadField, OpFetchField:
		ref := FieldRef{PC: pc}
		numSteps := r.count(1)
		for i := 0; i < numSteps; i++ {
			kind := ProvStepKind(r.byte())
			step := ProvStep{Kind: kind}
			switch kind {
			case ProvField:
				step.Name = r.str()
			case ProvElem, ProvArrayOf:
			default:
				r.fail("program cache contains a corrupt provenance step")
				return nil, nil, nil
			}
			ref.Prov = append(ref.Prov, step)
		}
		numPath := r.count(1)
		for i := 0; i < numPath; i++ {
			ref.Path = append(ref.Path, r.str())
		}
		if r.err == nil && len(ref.Path) == 0 {
			r.fail("program cache contains a field access with an empty path")
		}
		ref.Leaf = r.readShape(0)
		return nil, &ref, nil

	case OpMatchesConst:
		pattern := r.str()
		if r.err != nil {
			return nil, nil, nil
		}
		re, err := regexp.Compile(pattern)
		if err != nil {
			r.fail("program cache contains an invalid regular expression: %v", err)
			return nil, nil, nil
		}
		return re, nil, nil

	case OpPush:
		v, safeName, isSafe := r.readValue(pc, 0)
		if r.err != nil {
			return nil, nil, nil
		}
		if isSafe {
			return nil, nil, &SafeRef{PC: pc, Name: safeName}
		}
		return v, nil, nil
	}
	r.fail("program cache: opcode %d does not take a constant payload", op)
	return nil, nil, nil
}

func (r *capsuleReader) readShape(depth int) LeafShape {
	if depth > maxShapeDepth {
		r.fail("program cache contains a type shape nested too deeply")
		return LeafShape{}
	}
	kind := ShapeKind(r.byte())
	if r.err != nil {
		return LeafShape{}
	}
	switch kind {
	case ShapeUnknown, ShapeAny, ShapeStruct, ShapeUnsupported:
		return LeafShape{Kind: kind}
	case ShapeScalar:
		return LeafShape{Kind: kind, Scalar: r.str()}
	case ShapePtr, ShapeSlice, ShapeArray:
		elem := r.readShape(depth + 1)
		return LeafShape{Kind: kind, Elem: &elem}
	case ShapeMap:
		key := r.str()
		elem := r.readShape(depth + 1)
		return LeafShape{Kind: kind, Key: key, Elem: &elem}
	}
	r.fail("program cache contains a corrupt type shape")
	return LeafShape{}
}

func (r *capsuleReader) readValue(pc int, depth int) (v any, safeName string, isSafe bool) {
	if depth > maxConstantDepth {
		r.fail("program cache contains a constant nested too deeply")
		return nil, "", false
	}
	tag := r.byte()
	if r.err != nil {
		return nil, "", false
	}
	switch tag {
	case tagNil:
		return nil, "", false
	case tagBool:
		return r.byte() == 1, "", false
	case tagInt:
		return int(r.svarint()), "", false
	case tagInt8:
		return int8(r.svarint()), "", false
	case tagInt16:
		return int16(r.svarint()), "", false
	case tagInt32:
		return int32(r.svarint()), "", false
	case tagInt64:
		return r.svarint(), "", false
	case tagUint:
		return uint(r.uvarint()), "", false
	case tagUint8:
		return uint8(r.uvarint()), "", false
	case tagUint16:
		return uint16(r.uvarint()), "", false
	case tagUint32:
		return uint32(r.uvarint()), "", false
	case tagUint64:
		return r.uvarint(), "", false
	case tagFloat32:
		return math.Float32frombits(uint32(r.uvarint())), "", false
	case tagFloat64:
		return math.Float64frombits(r.uvarint()), "", false
	case tagString:
		return r.str(), "", false
	case tagBytes:
		n := r.count(1)
		return r.bytes(uint64(n)), "", false
	case tagSliceAny:
		n := r.count(1)
		out := make([]any, 0, n)
		for i := 0; i < n; i++ {
			item, safe, isSafe := r.readValue(pc, depth+1)
			if isSafe {
				r.fail("program cache contains a misplaced builtin function reference")
				return nil, "", false
			}
			_ = safe
			out = append(out, item)
		}
		return out, "", false
	case tagSliceString:
		n := r.count(1)
		out := make([]string, 0, n)
		for i := 0; i < n; i++ {
			out = append(out, r.str())
		}
		return out, "", false
	case tagSliceInt:
		n := r.count(1)
		out := make([]int, 0, n)
		for i := 0; i < n; i++ {
			out = append(out, int(r.svarint()))
		}
		return out, "", false
	case tagSliceInt64:
		n := r.count(1)
		out := make([]int64, 0, n)
		for i := 0; i < n; i++ {
			out = append(out, r.svarint())
		}
		return out, "", false
	case tagSliceFloat64:
		n := r.count(1)
		out := make([]float64, 0, n)
		for i := 0; i < n; i++ {
			out = append(out, math.Float64frombits(r.uvarint()))
		}
		return out, "", false
	case tagSliceBool:
		n := r.count(1)
		out := make([]bool, 0, n)
		for i := 0; i < n; i++ {
			out = append(out, r.byte() == 1)
		}
		return out, "", false
	case tagMapStringAny:
		n := r.count(2)
		out := make(map[string]any, n)
		for i := 0; i < n; i++ {
			key := r.str()
			item, _, isSafe := r.readValue(pc, depth+1)
			if isSafe {
				r.fail("program cache contains a misplaced builtin function reference")
				return nil, "", false
			}
			out[key] = item
		}
		return out, "", false
	case tagMapStringStruct:
		n := r.count(1)
		out := make(map[string]struct{}, n)
		for i := 0; i < n; i++ {
			out[r.str()] = struct{}{}
		}
		return out, "", false
	case tagMapIntStruct:
		n := r.count(1)
		out := make(map[int]struct{}, n)
		for i := 0; i < n; i++ {
			out[int(r.svarint())] = struct{}{}
		}
		return out, "", false
	case tagRegexp:
		pattern := r.str()
		if r.err != nil {
			return nil, "", false
		}
		re, err := regexp.Compile(pattern)
		if err != nil {
			r.fail("program cache contains an invalid regular expression: %v", err)
			return nil, "", false
		}
		return re, "", false
	case tagError:
		return errors.New(r.str()), "", false
	case tagBuiltinSafe:
		return nil, r.str(), true
	}
	r.fail("program cache contains an unknown constant tag %d", tag)
	return nil, "", false
}

// capsuleBinder binds a parsed program cache to the current configuration.
type capsuleBinder struct {
	config    *conf.Config
	cache     *nature.Cache
	constants []any
	arguments []int
	debugInfo map[string]string
}

// bindFields relocates every recorded field access against the current
// environment and rewrites the corresponding constant slots.
func (b *capsuleBinder) bindFields(refs []FieldRef) error {
	slots := make(map[string]int)
	for _, ref := range refs {
		key := fieldRefKey(ref)
		slot, ok := slots[key]
		if !ok {
			field, err := b.resolveFieldRef(ref)
			if err != nil {
				return err
			}
			slot = len(b.constants)
			b.constants = append(b.constants, field)
			slots[key] = slot
		}
		b.arguments[ref.PC] = slot
	}
	return nil
}

func fieldRefKey(ref FieldRef) string {
	key := ""
	for _, step := range ref.Prov {
		switch step.Kind {
		case ProvField:
			key += "." + step.Name
		case ProvElem:
			key += "[]"
		case ProvArrayOf:
			key += "{}"
		}
	}
	for _, name := range ref.Path {
		key += "/" + name
	}
	// The leaf shape is part of the key so that every distinct cached shape
	// is checked against the current environment, even for identical paths.
	return key + "|" + shapeString(ref.Leaf)
}

// resolveFieldRef resolves one field access against the current environment
// and returns the relocated field descriptor.
func (b *capsuleBinder) resolveFieldRef(ref FieldRef) (*runtime.Field, error) {
	base, err := b.resolveProv(ref)
	if err != nil {
		return nil, err
	}
	index, leaf, err := b.resolveFieldPath(base, ref)
	if err != nil {
		return nil, err
	}
	newShape := LeafShapeOfNature(b.cache, leaf)
	if !ref.Leaf.compatible(newShape) {
		return nil, fmt.Errorf("field access %s: type of the fetched value is not compatible with the cached rule (was %s, now %s)",
			ref.LogicalPath(), shapeString(ref.Leaf), shapeString(newShape))
	}
	return &runtime.Field{Index: index, Path: append([]string(nil), ref.Path...)}, nil
}

// resolveProv resolves the provenance path of a field access receiver
// against the current environment.
func (b *capsuleBinder) resolveProv(ref FieldRef) (nature.Nature, error) {
	n := b.config.Env
	for _, step := range ref.Prov {
		switch step.Kind {
		case ProvField:
			next, err := b.fieldStep(n, step.Name)
			if err != nil {
				return nature.Nature{}, fmt.Errorf("field access %s: %w", ref.LogicalPath(), err)
			}
			n = next
		case ProvElem:
			d := n.Deref(b.cache)
			switch d.Kind {
			case reflect.Slice, reflect.Array, reflect.Map, reflect.Ptr:
				n = d.Elem(b.cache)
				if n.Type == nil && !n.Nil {
					return nature.Nature{}, fmt.Errorf("field access %s: cannot determine the element type statically", ref.LogicalPath())
				}
			default:
				return nature.Nature{}, fmt.Errorf("field access %s: %s is not a collection in the current environment", ref.LogicalPath(), natureString(n))
			}
		case ProvArrayOf:
			if n.Type == nil {
				return nature.Nature{}, fmt.Errorf("field access %s: cannot determine the collection type statically", ref.LogicalPath())
			}
			n = b.cache.FromType(reflect.SliceOf(n.Type))
		}
	}
	return n, nil
}

// fieldStep resolves a single named step: a struct field or a map key.
func (b *capsuleBinder) fieldStep(n nature.Nature, name string) (nature.Nature, error) {
	d := n.Deref(b.cache)
	switch d.Kind {
	case reflect.Struct:
		if f, ok := d.FieldByName(b.cache, name); ok {
			return f, nil
		}
		return nature.Nature{}, fmt.Errorf("field %q is not present or not visible in %s", name, natureString(d))
	case reflect.Map:
		if d.TypeData != nil && d.Fields != nil {
			if f, ok := d.Fields[name]; ok {
				return f, nil
			}
			return nature.Nature{}, fmt.Errorf("key %q is not present in the map environment", name)
		}
		if d.Type != nil && d.Type.Key().Kind() == reflect.String {
			return b.cache.FromType(d.Type.Elem()), nil
		}
		return nature.Nature{}, fmt.Errorf("cannot resolve %q through %s", name, natureString(d))
	}
	if n.Type == nil {
		return nature.Nature{}, fmt.Errorf("cannot resolve %q: no static environment type (was expr.Env given?)", name)
	}
	return nature.Nature{}, fmt.Errorf("cannot resolve %q through %s", name, natureString(d))
}

// resolveFieldPath resolves the logical field path of a field access against
// the receiver nature, returning the relocated field index path and the
// nature of the fetched value.
func (b *capsuleBinder) resolveFieldPath(base nature.Nature, ref FieldRef) ([]int, nature.Nature, error) {
	n := base
	var index []int
	for _, name := range ref.Path {
		d := n.Deref(b.cache)
		if d.Kind != reflect.Struct {
			return nil, nature.Nature{}, fmt.Errorf("field access %s: cannot resolve field %q through %s in the current environment",
				ref.LogicalPath(), name, natureString(d))
		}
		idx, ok := d.FieldIndex(b.cache, name)
		if !ok {
			return nil, nature.Nature{}, fmt.Errorf("field access %s: field %q is not present, not visible or ambiguous in %s",
				ref.LogicalPath(), name, natureString(d))
		}
		index = append(index, idx...)
		field, _ := d.FieldByName(b.cache, name)
		n = field
	}
	return index, n, nil
}

// bindBuiltins re-links fast builtin calls (OpCallBuiltin1) by name.
func (b *capsuleBinder) bindBuiltins(refs []BuiltinRef) error {
	for _, ref := range refs {
		idx, ok := builtin.Index[ref.Name]
		if !ok {
			return fmt.Errorf("program cache: unknown builtin %q", ref.Name)
		}
		if _, ok := b.config.Builtins[ref.Name]; !ok {
			return fmt.Errorf("program cache: builtin %q is disabled in the current configuration", ref.Name)
		}
		if builtin.Builtins[idx].Fast == nil {
			return fmt.Errorf("program cache: builtin %q no longer supports fast calls", ref.Name)
		}
		b.arguments[ref.PC] = idx
	}
	return nil
}

// bindSafeFuncs re-links builtin safe functions (used with OpCallSafe) by
// name and appends them to the constant table.
func (b *capsuleBinder) bindSafeFuncs(refs []SafeRef) error {
	for _, ref := range refs {
		fn, ok := b.config.Builtins[ref.Name]
		if !ok {
			return fmt.Errorf("program cache: builtin %q is disabled or unknown in the current configuration", ref.Name)
		}
		if fn.Safe == nil {
			return fmt.Errorf("program cache: builtin %q no longer supports safe calls", ref.Name)
		}
		slot := len(b.constants)
		b.constants = append(b.constants, fn.Safe)
		b.arguments[ref.PC] = slot
		b.debugInfo[fmt.Sprintf("const_%d", slot)] = ref.Name
	}
	return nil
}

// bindFunctions re-links the program's function table by name.
func (b *capsuleBinder) bindFunctions(metas []FuncMeta) ([]Function, error) {
	functions := make([]Function, len(metas))
	for i, meta := range metas {
		if meta.Unsupported {
			return nil, fmt.Errorf("program cache: function %q declares a signature that cannot be relocated", meta.Name)
		}
		if meta.Builtin {
			fn, ok := b.config.Builtins[meta.Name]
			if !ok {
				return nil, fmt.Errorf("program cache: builtin %q is disabled or unknown in the current configuration", meta.Name)
			}
			if fn.Func == nil {
				return nil, fmt.Errorf("program cache: builtin %q cannot be called the way the cached rule uses it", meta.Name)
			}
			sigs, ok := EncodeFuncTypes(fn.Types)
			if !ok || !equalSignatures(sigs, meta.Overloads) {
				return nil, fmt.Errorf("program cache: builtin %q has an incompatible signature in the current configuration", meta.Name)
			}
			functions[i] = fn.Func
		} else {
			fn, ok := b.config.Functions[meta.Name]
			if !ok || fn == nil || fn.Func == nil {
				return nil, fmt.Errorf("program cache: function %q is not defined in the current configuration", meta.Name)
			}
			if len(meta.Overloads) > 0 {
				sigs, ok := EncodeFuncTypes(fn.Types)
				if !ok || !equalSignatures(sigs, meta.Overloads) {
					return nil, fmt.Errorf("program cache: function %q declares an incompatible signature (cached rule expects %s)",
						meta.Name, formatSignature(meta.Overloads))
				}
			}
			functions[i] = fn.Func
		}
		b.debugInfo[fmt.Sprintf("func_%d", i)] = meta.Name
	}
	return functions, nil
}

func natureString(n nature.Nature) string {
	if n.Type == nil {
		return "an untyped environment"
	}
	return n.Type.String()
}

func shapeString(s LeafShape) string {
	switch s.Kind {
	case ShapeUnknown:
		return "unknown"
	case ShapeAny:
		return "any"
	case ShapeScalar:
		return s.Scalar
	case ShapePtr:
		return "*" + shapeString(*s.Elem)
	case ShapeSlice:
		return "[]" + shapeString(*s.Elem)
	case ShapeArray:
		return "array of " + shapeString(*s.Elem)
	case ShapeMap:
		return "map[" + s.Key + "]" + shapeString(*s.Elem)
	case ShapeStruct:
		return "struct"
	default:
		return "unsupported"
	}
}
