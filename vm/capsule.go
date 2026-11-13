package vm

import (
	"fmt"
	"reflect"

	"github.com/expr-lang/expr/checker/nature"
)

// This file describes the relocation metadata recorded by the compiler and
// used by EncodeProgram/DecodeProgram to make compiled programs portable
// between processes. See docs/program-cache.md for the on-disk format.
//
// The metadata never contains Go code addresses, reflect pointers, builtin
// table indexes or struct field indexes computed from a specific struct
// layout. Everything is expressed in logical terms: field names as seen by
// expressions, access paths from the environment root, and function names
// with their declared signatures.

// ProvStepKind is the kind of a single step of a logical access path that
// starts at the environment root.
type ProvStepKind byte

const (
	// ProvField is a named field step: a struct field (resolved by the name
	// visible to expressions, including expr tags) or a key of a map
	// environment slot.
	ProvField ProvStepKind = iota
	// ProvElem is the element of a slice, array or map.
	ProvElem
	// ProvArrayOf wraps the type resolved so far into a slice. It is used to
	// describe the provenance of collections produced by predicates (map,
	// filter) so that a later ProvElem step reaches the element type.
	ProvArrayOf
)

// ProvStep is a single step of a logical access path from the environment
// root to the value a field access applies to.
type ProvStep struct {
	Kind ProvStepKind
	Name string // set for ProvField only
}

// FieldRef describes a single OpLoadField/OpFetchField instruction in a
// relocatable way. The same *runtime.Field constant may be shared by several
// instructions whose receivers come from different collections; FieldRef is
// recorded per instruction so each access keeps its own source.
type FieldRef struct {
	PC      int        // index of the instruction in Program.Bytecode
	Unknown bool       // provenance could not be determined; EncodeProgram rejects such programs
	Prov    []ProvStep // provenance of the receiver; empty means the environment itself
	Path    []string   // logical field path applied to the receiver
	Leaf    LeafShape  // static shape of the fetched value
}

// LogicalPath returns a human-readable logical path of the field access,
// e.g. "Left[].Value".
func (r FieldRef) LogicalPath() string {
	s := "$env"
	for _, p := range r.Prov {
		switch p.Kind {
		case ProvField:
			s += "." + p.Name
		case ProvElem:
			s += "[]"
		case ProvArrayOf:
			s += "[]"
		}
	}
	for _, p := range r.Path {
		s += "." + p
	}
	return s
}

// BuiltinRef describes an OpCallBuiltin1 instruction: a call of a fast
// builtin identified by name, never by its index in the builtin table.
type BuiltinRef struct {
	PC   int
	Name string
}

// SafeRef describes an OpPush instruction that pushes the Safe function of a
// builtin (used together with OpCallSafe), identified by builtin name.
type SafeRef struct {
	PC   int
	Name string
}

// FuncSignature is a relocatable declared signature of a function: the
// parameter and result type constraints as declared via expr.Function types
// or builtin declarations. Type references are logical names such as "int",
// "string" or "[]string"; see EncodeFuncTypes for the supported set.
type FuncSignature struct {
	In  []string
	Out []string
}

// FuncMeta describes one entry of the program's function table.
type FuncMeta struct {
	Name      string
	Builtin   bool // true for functions provided by the builtin table, false for expr.Function
	Overloads []FuncSignature
	// Unsupported is set when the declared signature uses types that cannot
	// be represented in a cache; EncodeProgram rejects such programs.
	Unsupported bool
}

// CapsuleMeta is the relocation metadata attached to a Program by the
// compiler. It is preserved by DecodeProgram so a loaded program can be
// encoded again byte-for-byte.
type CapsuleMeta struct {
	FieldRefs   []FieldRef   // ordered by PC
	BuiltinRefs []BuiltinRef // ordered by PC
	SafeRefs    []SafeRef    // ordered by PC
	Funcs       []FuncMeta   // parallel to the program's function table
}

// Capsule returns the relocation metadata recorded at compile time, or nil
// if the program was not compiled with relocation support.
func (program *Program) Capsule() *CapsuleMeta {
	return program.capsule
}

// SetCapsule attaches relocation metadata to the program. It is used by the
// compiler and by DecodeProgram.
func (program *Program) SetCapsule(meta *CapsuleMeta) {
	program.capsule = meta
}

// Functions returns the program's function table.
func (program *Program) Functions() []Function {
	return program.functions
}

// Variables returns the number of variable slots used by the program.
func (program *Program) Variables() int {
	return program.variables
}

// DebugInfo returns the program's debug info map.
func (program *Program) DebugInfo() map[string]string {
	return program.debugInfo
}

// ShapeKind is the kind of a LeafShape.
type ShapeKind byte

const (
	// ShapeUnknown means no static information was available at compile time;
	// such leaves keep their dynamic runtime behavior and are not checked.
	ShapeUnknown ShapeKind = iota
	// ShapeAny is a value statically typed as interface{}.
	ShapeAny
	// ShapeScalar is a bool, string or numeric scalar; Scalar holds the
	// reflect.Kind name (e.g. "bool", "int", "int64", "float64", "string").
	ShapeScalar
	// ShapePtr is a pointer; Elem describes the pointed value.
	ShapePtr
	// ShapeSlice is a slice; Elem describes the element.
	ShapeSlice
	// ShapeArray is an array; Elem describes the element.
	ShapeArray
	// ShapeMap is a map; Key holds the key kind name and Elem the value.
	ShapeMap
	// ShapeStruct is a struct value used as a leaf.
	ShapeStruct
	// ShapeUnsupported is any other type (func, chan, ...); programs using
	// such leaves are rejected by EncodeProgram.
	ShapeUnsupported
)

// LeafShape is the relocatable static shape of a value fetched by a field
// access. Anonymous embedding depth is not part of the shape; pointer and
// collection nesting is.
type LeafShape struct {
	Kind   ShapeKind
	Scalar string // kind name for ShapeScalar
	Key    string // key kind name for ShapeMap
	Elem   *LeafShape
}

const maxShapeDepth = 64

// LeafShapeOfNature computes the relocatable leaf shape of a static nature.
func LeafShapeOfNature(c *nature.Cache, n nature.Nature) LeafShape {
	if n.Nil || n.Type == nil {
		return LeafShape{Kind: ShapeUnknown}
	}
	return leafShapeOfType(n.Type, 0)
}

func leafShapeOfType(t reflect.Type, depth int) LeafShape {
	if depth > maxShapeDepth {
		return LeafShape{Kind: ShapeAny}
	}
	switch t.Kind() {
	case reflect.Bool, reflect.String,
		reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64,
		reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64, reflect.Uintptr,
		reflect.Float32, reflect.Float64:
		return LeafShape{Kind: ShapeScalar, Scalar: t.Kind().String()}
	case reflect.Ptr:
		elem := leafShapeOfType(t.Elem(), depth+1)
		return LeafShape{Kind: ShapePtr, Elem: &elem}
	case reflect.Slice:
		elem := leafShapeOfType(t.Elem(), depth+1)
		return LeafShape{Kind: ShapeSlice, Elem: &elem}
	case reflect.Array:
		elem := leafShapeOfType(t.Elem(), depth+1)
		return LeafShape{Kind: ShapeArray, Elem: &elem}
	case reflect.Map:
		elem := leafShapeOfType(t.Elem(), depth+1)
		return LeafShape{Kind: ShapeMap, Key: t.Key().Kind().String(), Elem: &elem}
	case reflect.Struct:
		return LeafShape{Kind: ShapeStruct}
	case reflect.Interface:
		return LeafShape{Kind: ShapeAny}
	default:
		return LeafShape{Kind: ShapeUnsupported}
	}
}

// compatible reports whether a value of the new shape may be used where the
// old (cached) shape was expected. Scalar kinds, pointer and collection
// nesting must be preserved; leaves that were dynamic (unknown or any) at
// compile time accept anything, while leaves that become dynamic are
// rejected because the compiled bytecode relies on the static type.
func (old LeafShape) compatible(new LeafShape) bool {
	switch old.Kind {
	case ShapeUnknown, ShapeAny:
		return true
	case ShapeUnsupported:
		return false
	}
	switch new.Kind {
	case ShapeUnknown, ShapeAny, ShapeUnsupported:
		return false
	}
	if old.Kind == new.Kind {
		switch old.Kind {
		case ShapeScalar:
			return old.Scalar == new.Scalar
		case ShapePtr, ShapeSlice, ShapeArray:
			return old.Elem.compatible(*new.Elem)
		case ShapeMap:
			return old.Key == new.Key && old.Elem.compatible(*new.Elem)
		case ShapeStruct:
			return true
		}
		return false
	}
	// Slices and arrays are interchangeable for the VM.
	if (old.Kind == ShapeSlice || old.Kind == ShapeArray) &&
		(new.Kind == ShapeSlice || new.Kind == ShapeArray) {
		return old.Elem.compatible(*new.Elem)
	}
	return false
}

var (
	anyType          = reflect.TypeOf((*any)(nil)).Elem()
	errorTypeCapsule = reflect.TypeOf((*error)(nil)).Elem()
)

// EncodeFuncTypes converts declared function types (as passed to
// expr.Function or declared by builtins) into relocatable signatures. It
// reports false if any declaration uses types outside the supported set:
// any, error, bool, int, int64, float64, string and slices of those
// (including []any).
func EncodeFuncTypes(types []reflect.Type) ([]FuncSignature, bool) {
	sigs := make([]FuncSignature, 0, len(types))
	for _, t := range types {
		if t == nil || t.Kind() != reflect.Func || t.IsVariadic() {
			return nil, false
		}
		sig := FuncSignature{
			In:  make([]string, 0, t.NumIn()),
			Out: make([]string, 0, t.NumOut()),
		}
		for i := 0; i < t.NumIn(); i++ {
			ref, ok := typeRefOf(t.In(i))
			if !ok {
				return nil, false
			}
			sig.In = append(sig.In, ref)
		}
		for i := 0; i < t.NumOut(); i++ {
			ref, ok := typeRefOf(t.Out(i))
			if !ok {
				return nil, false
			}
			sig.Out = append(sig.Out, ref)
		}
		sigs = append(sigs, sig)
	}
	return sigs, true
}

func typeRefOf(t reflect.Type) (string, bool) {
	if t == anyType {
		return "any", true
	}
	if t == errorTypeCapsule {
		return "error", true
	}
	switch t.Kind() {
	case reflect.Bool:
		return "bool", true
	case reflect.Int:
		return "int", true
	case reflect.Int64:
		return "int64", true
	case reflect.Float64:
		return "float64", true
	case reflect.String:
		return "string", true
	case reflect.Slice:
		elem, ok := typeRefOf(t.Elem())
		if !ok {
			return "", false
		}
		return "[]" + elem, true
	}
	return "", false
}

// equalSignatures reports whether two declared signature sets are identical.
func equalSignatures(a, b []FuncSignature) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if !equalStringSlice(a[i].In, b[i].In) || !equalStringSlice(a[i].Out, b[i].Out) {
			return false
		}
	}
	return true
}

func equalStringSlice(a, b []string) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

// formatSignature renders a signature set for error messages.
func formatSignature(sigs []FuncSignature) string {
	if len(sigs) == 0 {
		return "func(...any) (any, error) (dynamic)"
	}
	s := ""
	for i, sig := range sigs {
		if i > 0 {
			s += " or "
		}
		s += fmt.Sprintf("func(%v) %v", sig.In, sig.Out)
	}
	return s
}
