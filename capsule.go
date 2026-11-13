package expr

import (
	"github.com/expr-lang/expr/conf"
	"github.com/expr-lang/expr/vm"
)

// EncodeProgram encodes a compiled program into a relocatable byte form
// that can be shipped to offline workers and loaded with DecodeProgram.
//
// The encoded form is independent of the compiling process: it contains no
// Go function addresses, no builtin table indexes and no struct field
// indexes computed from a particular layout. Field accesses are stored as
// logical paths from the environment root and functions as names with their
// declared signatures. Encoding the same program twice yields the same
// bytes.
//
// Programs using features that cannot be relocated reliably (methods,
// environment function values called through typed signatures, profiling,
// ConstExpr results or constants outside the supported set, field accesses
// whose source cannot be determined statically) are rejected with an error.
// See docs/program-cache.md for the format and the exact rejection scope.
func EncodeProgram(program *vm.Program) ([]byte, error) {
	return vm.EncodeProgram(program)
}

// DecodeProgram loads a program cache produced by EncodeProgram and binds it
// to the current environment, without calling Compile or ParseCheck and
// without regenerating bytecode from the stored source.
//
// The given options bind the program to the current process:
//   - expr.Env declares the environment the field accesses are resolved
//     against. The visible field names, expr tags and the types of the
//     values actually used by the expression must be compatible with the
//     compiling environment; struct names, field order and anonymous
//     embedding depth may differ.
//   - expr.Function provides the extension functions the program is linked
//     against. Functions are bound by name; declared signatures must match
//     the ones recorded at compile time.
//   - expr.DisableBuiltin and friends define which builtins are available;
//     a program using a disabled builtin fails to load.
//
// Any incompatibility is reported as an error before the program is
// returned; a successfully loaded program can be run with expr.Run and
// encoded again with EncodeProgram, yielding the original bytes.
func DecodeProgram(data []byte, options ...Option) (*vm.Program, error) {
	config := conf.CreateNew()
	for _, op := range options {
		op(config)
	}
	for name := range config.Disabled {
		delete(config.Builtins, name)
	}
	return vm.DecodeProgram(data, config)
}
