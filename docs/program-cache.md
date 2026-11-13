# Program cache: `expr.EncodeProgram` / `expr.DecodeProgram`

Compiled expressions can be distributed to offline workers as a relocatable
byte cache. The compiling process encodes a program once; execution
processes load the cache and bind it to their own record types and extension
functions, without parsing or recompiling the expression.

```go
// Compiling side.
program, err := expr.Compile(rule, expr.Env(RecordV1{}), expr.Function("score", scoreFn))
data, err := expr.EncodeProgram(program)

// Execution side (another process, another layout of the record).
program, err := expr.DecodeProgram(data, expr.Env(RecordV2{}), expr.Function("score", scoreFn))
out, err := expr.Run(program, RecordV2{...})
```

`DecodeProgram` never calls `Compile` or `ParseCheck` and never regenerates
bytecode from a stored AST or the source text; the source is kept only to
render runtime error snippets. Loading does not call user functions and does
not read environment *values* to guess field origins; binding is driven
exclusively by type information (`reflect` types, struct tags, declared
function signatures), exactly the information `expr.Env` already exposes —
including the dynamic types of the slots of a `map[string]any` environment.

## What is stored

A cache stores the bytecode, the inline instruction arguments (variable
slots, jump offsets, call arities, cast kinds), the constants, the source
locations, the variable slot names and the source text. Process-specific
details are **never** stored:

- no Go function addresses or `reflect` pointers;
- no indexes into the current process's builtin table;
- no struct field indexes computed from a particular struct layout;
- no Go type names.

Instead, the cache records *logical* relocation metadata:

- **Field accesses** are stored per instruction as a logical path from the
  environment root: a *provenance* (which environment field, collection
  element or `let` alias the receiver comes from) plus the field names as
  visible to expressions (including `expr` tags), plus the static shape of
  the fetched value. Two accesses that shared one field descriptor at
  compile time (e.g. `#.Value` inside predicates over two different
  collections) keep their own sources and are bound independently.
- **Functions** are stored as a name, a kind (builtin or `expr.Function`
  extension) and the declared signature constraints. Builtins called through
  the fast form (`len`, `lower`, ...) or the safe form (`repeat`, ...) are
  stored by name as well.

## Binding rules

Loading re-resolves everything against the current configuration and fails
**before** returning a program when anything is incompatible:

- **Fields** are resolved by name against the current environment, following
  the recorded logical path. Struct names, Go field order and anonymous
  embedding depth may change; embedding depth does not count towards the
  logical path. Fields the expression does not use may be added or removed,
  and unreferenced recursive types do not block loading. A missing field, a
  field hidden with `expr:"-"`, or an ambiguous lookup is an error that
  names the logical field path (e.g. `$env.Left[].Value`).
- **Leaf types** of the values actually used must be compatible: scalar
  kinds are preserved (`bool`, `int`, `int64`, `float64`, `string` and the
  other numeric kinds), as are pointer and collection (slice/array/map)
  nesting. Values that were `any` or otherwise statically unknown at compile
  time keep their dynamic behavior and accept anything; a value that was
  statically typed may not become `any`.
- **Extension functions** (`expr.Function`) are bound by name to the
  functions registered at load time. Two closures sharing a code address but
  capturing different values are linked independently per `DecodeProgram`
  call; programs loaded from the same cache with different function sets can
  run interleaved and concurrently without interfering. If a signature was
  declared at compile time, the load-time declaration must match exactly;
  functions without declared types keep their dynamic behavior.
- **Builtins** are bound by name to the current builtin table. A builtin
  disabled for the current configuration (e.g. via `expr.DisableBuiltin`)
  makes loading fail, as does a missing extension function or an
  incompatible declared signature. A cached extension function is never
  bound to a same-named builtin and vice versa.

`map[string]any` environments and struct environments are both supported.
Members of a `map[string]any` environment that stay `any` (whose dynamic
type cannot be determined statically) keep their original runtime behavior;
they are not silently treated as statically bound.

## Rejection scope

`EncodeProgram` returns an error (dropping nothing silently) for programs
that cannot be relocated reliably:

- method calls (`OpLoadMethod`/`OpMethod`);
- environment function values called through a typed signature
  (`OpCallTyped`); dynamically called environment function values
  (`OpCall`/`OpCallFast`) keep working;
- programs compiled with profiling (`OpProfileStart`/`OpProfileEnd`);
- constants outside the supported set (see the tag table below), including
  `ConstExpr` results that are not representable;
- field accesses whose source cannot be determined statically (the error
  names the logical field path);
- declared function signatures using types outside the supported set (see
  below), for example variadic signatures, maps, channels or function
  parameters.

`DecodeProgram` returns an error for: inputs larger than 8 MiB, a bad magic
header, an unknown version, truncated input, trailing content, corrupt
lengths or indexes (including declarations too large to fit the remaining
input, rejected before allocation), unknown opcodes or constant tags, and
all binding incompatibilities listed above. The cache format is meant for
caches produced by a trusted compiling side: the loader validates the
format, reference ranges and bindings, but does not attempt to prove
arbitrary malicious bytecode safe or to sandbox its execution.

## Determinism

Encoding the same program twice yields the same bytes, and encoding a
program produced by a successful `DecodeProgram` yields the original bytes:
constant table indexes and builtin table indexes are not part of the format
(they are rebuilt on load), maps are encoded with sorted keys, and the
execution-side layout and function addresses are never written back into the
cache. A loaded program does not borrow the caller's `data` slice.

## Format (version 1)

All multi-byte integers are varints as produced by `encoding/binary`:
`uvarint` for unsigned values, `svarint` (zig-zag) for signed ones. Strings
are a `uvarint` length followed by the bytes.

```
cache       := magic version variables code
magic       := 'E' 'X' 'P' 'C'
version     := uvarint                     # currently 1
variables   := uvarint                     # number of variable slots
code        := uvarint opcodes arguments payloads functions vars locations source
opcodes     := byte{n}                     # one opcode per instruction
arguments   := svarint*                    # one per instruction, except
                                           # constant-payload and OpCallBuiltin1
                                           # instructions (see below)
payloads    := payload*                    # one per constant-payload or
                                           # OpCallBuiltin1 instruction, in order
functions   := uvarint function*
function    := string flags overloads
flags       := byte                        # bit 0: builtin, bit 1: unsupported
overloads   := uvarint (params results)*
params      := uvarint string*
results     := uvarint string*
vars        := uvarint string*             # variable slot names, in slot order
locations   := (uvarint uvarint){n}        # From/To per instruction
source      := string
```

Constant-payload instructions are `OpPush`, `OpLoadConst`, `OpLoadFast`,
`OpLoadField`, `OpFetchField` and `OpMatchesConst`. Their constant table
index is not stored; the constant value is stored inline and the table is
rebuilt on load. `OpCallBuiltin1` stores the builtin name as a string
instead of its table index.

Payloads by instruction:

- `OpLoadConst`, `OpLoadFast`: a string (the identifier or map key).
- `OpMatchesConst`: a string (the regular expression pattern).
- `OpCallBuiltin1`: a string (the builtin name).
- `OpLoadField`, `OpFetchField`: a field reference —
  `uvarint steps* uvarint path* shape` where each step is a kind byte
  (`0` = field, followed by a name string; `1` = collection element;
  `2` = slice-of wrapper) and each path element is a name string.
- `OpPush`: a tagged value (see below).

### Type shapes

A field reference ends with the static shape of the fetched value: a kind
byte — `0` unknown, `1` any, `2` scalar (followed by the `reflect.Kind` name
string, e.g. `"bool"`, `"int"`, `"int64"`, `"float64"`, `"string"`),
`3` pointer, `4` slice, `5` array (each followed by the element shape),
`6` map (followed by the key kind name string and the value shape),
`7` struct, `8` unsupported.

### Constant value tags

`OpPush` payloads start with a tag byte:

| tag | type      | payload                                   |
|-----|-----------|-------------------------------------------|
| 0   | nil       | —                                         |
| 1   | bool      | byte                                      |
| 2   | int       | svarint                                   |
| 3–6 | int8–int64| svarint                                   |
| 7–11| uint–uint64| uvarint                                  |
| 12  | float32   | uvarint (IEEE-754 bits)                   |
| 13  | float64   | uvarint (IEEE-754 bits)                   |
| 14  | string    | string                                    |
| 15  | []byte    | uvarint, bytes                            |
| 16  | []any     | uvarint, tagged values                    |
| 17  | []string  | uvarint, strings                          |
| 18  | []int     | uvarint, svarints                         |
| 19  | []int64   | uvarint, svarints                         |
| 20  | []float64 | uvarint, uvarints                         |
| 21  | []bool    | uvarint, bytes                            |
| 22  | map[string]any | uvarint, (string, tagged value) pairs, keys sorted |
| 23  | map[string]struct{} | uvarint, strings, keys sorted      |
| 24  | map[int]struct{}    | uvarint, svarints, keys sorted     |
| 25  | *regexp.Regexp | string (pattern, recompiled on load)     |
| 26  | error     | string (message, rebuilt with errors.New) |
| 27  | builtin safe function | string (builtin name)         |

### Declared signature type references

Function signature constraints use these type reference strings: `any`,
`error`, `bool`, `int`, `int64`, `float64`, `string`, and slices of those
(`[]any`, `[]bool`, `[]int`, `[]int64`, `[]float64`, `[]string`). A function
with no declared types is stored with zero overloads and keeps its dynamic
behavior.

### Limits

The encoded cache is at most 8 MiB. Every collection count is checked
against the remaining input before any allocation happens. Constants nest at
most 100 levels deep, type shapes at most 64.
