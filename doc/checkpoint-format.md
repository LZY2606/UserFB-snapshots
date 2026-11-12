# SPIRV-VM checkpoint binary format (version 1)

The checkpoint format serializes the numeric execution scene of a single
`spvm_state_t` so that it can be restored into another state attached to a
reload of the **same** SPIR-V module. No shader instruction is executed and no
host callback is invoked while saving or restoring.

All multi-byte integers are stored **little-endian**, written field by field.
The blob never contains a host struct image, a host pointer, padding bytes, or
uninitialized stack capacity. Equal numeric scenes therefore produce equal
bytes regardless of allocation addresses.

## Scope

Only plain numeric Logical/GLSL shaders are eligible. A module is rejected
(save and restore both return 0) when it uses any of:

- image / sampler / sampled-image types or instructions, including sparse
  image instructions;
- `OpTypeRuntimeArray`;
- physical addressing / physical storage pointers;
- `Workgroup` / `CrossWorkgroup` / `AtomicCounter` / `StorageBuffer` /
  `PushConstant` / generic pointer storage classes;
- atomic, barrier and named-barrier instructions;
- geometry stream output (`OpEmitVertex`, `OpEndPrimitive`, ...);
- derivative groups.

Supported storage classes for pointer variables: `Function`, `Private`,
`Input`, `Output`, `Uniform`. Host bindings (`user_data`, analyzer,
callbacks, extension function tables) are never stored; the target keeps its
own bindings.

## Top-level layout

```
u32  magic      = 0x4350564D ("CPVM")
u32  version    = 1
section MODULE  (tag 0x31444F4D "MOD1")
section EXEC    (tag 0x31434558 "XEC1")
section VALUES  (tag 0x314C4156 "VAL1")
u32  end_tag    = 0x31444E45 ("END1")
```

Every section starts with its 4-byte tag followed by a `u32` payload size in
bytes (the bytes following that size field). A reader must reject blobs that
end before a declared payload, have trailing bytes after `END1`, use an
unknown tag/version, or contain any count that cannot fit inside the bytes
actually present.

## MODULE section — module identity

```
u32  bound          // SPIR-V IdBound
u32  code_words     // instruction word count (excludes the 5-word header)
u64  hash_a         // FNV-1a over bound, code length and every code word
u64  hash_b         // second checksum, words consumed in reverse byte order
```

Identity is a strong checksum over the **complete instruction content**, not
just `bound`, length or entry-point names. A reload with identical content is
accepted; any differing content changes at least one hash and is rejected.
`hash_a` folds `bound` then `code_words` then every word in natural byte order;
`hash_b` folds them in the opposite order (`code_words` then `bound`, each word
byte-reversed) so byte-reordering corruption cannot defeat both.

## EXEC section — execution position

```
u32  flags          // bit 0 = TERMINAL (function has returned)
```

### Active scene (`TERMINAL` unset)

```
u32  pc                 // instruction word offset of the current PC
u32  instruction_count
u32  current_function   // result id of the current function
u32  return_id          // result id, or 0xFFFFFFFF if none
u32  current_parameter
u32  line               // source line, or 0xFFFFFFFF
u32  column             // source column, or 0xFFFFFFFF
u32  file               // OpString result id, or 0xFFFFFFFF
u8   did_jump
u8   discarded
u32  depth              // number of valid call frames, >= 1
frame[depth]
```

Each frame holds:

```
u32  pc          // resume position; entry position for the deepest frame
u32  function    // function result id
u32  return_slot // caller's result id; 0xFFFFFFFF for frame 0
u32  cfg         // current block label id, 0 if none
u32  cfg_parent  // predecessor block label id, 0 if none
```

The executor parks a caller frame on the instruction following the
`OpFunctionCall` that opened the frame below it. Validation reconstructs the
preceding instruction from the module's instruction-boundary map and requires
it to be that `OpFunctionCall`, with matching call result id and callee id.
The current PC must be an instruction boundary inside the current function
body (between its `OpFunction` and `OpFunctionEnd`).

### Terminal scene (`TERMINAL` set)

```
u32  instruction_count
u32  return_id
u32  line
u32  column
u32  file
u8   did_jump
u8   discarded
```

On restore the target's `code_current` becomes `NULL`, its current function
becomes `NULL`, and the frame depth is reset to -1, matching the state produced
by the final `OpReturn` / `OpReturnValue`.

## VALUES section — numeric trees and pointer aliases

```
u32  root_count
root[root_count]
u32  alias_count
alias[alias_count]
```

### Owning trees (roots)

A root is a result that owns a member allocation: an `OpVariable` or an
`OpConstant` (including `OpUndef`) with members. Every owning tree of the
target must be present; an uncaptured tree is a validation failure.

```
u32  result_id
tree            // recursive value tree matching the result's static type
```

A tree node is:

- scalar leaf: `u8 kind = 1`, `u8 width` (1 = bool, 4 = 32-bit, 8 = 64-bit),
  `u64 value` stored bit-for-bit (floats and doubles keep their exact bit
  pattern, including negative zero and NaN payloads);
- composite: `u8 kind` (2 vector, 3 matrix, 4 array, 5 struct),
  `u32 child_count`, followed by one child node per element. Struct children
  use the member types in declaration order; vectors/matrices/arrays use the
  repeated element type. The encoded shape must match the target's existing
  allocation exactly.

Values are written **into** the target state's own existing member tree;
allocations are never replaced, so host-held member addresses stay valid.

### Alias records

Access-chain results and function parameters do not own storage; their member
pointer names a location inside one owning tree. Each record stores that
location as an index path, never as an address:

```
u32  result_id
u32  root_id        // owning variable/constant result id
u32  path_len      // >= 1
u32  index[path_len]
```

The path is interpreted against the target's own tree. During validation every
index is checked against the physical bounds of the target allocation, and the
result id must be produced by an `OpAccessChain`-family instruction or an
`OpFunctionParameter` in the module. On commit the alias member pointer is
rebuilt with the same rule as `OpAccessChain`: a path ending on a composite
member aliases that member's child array, otherwise it aliases the scalar slot
itself. Two aliases that name the same member therefore stay shared, and
adjacent members stay distinct. No pointer into the source state survives.

## Restore ordering

1. Verify header, version and section tags; confirm the module checksums.
2. Build an instruction-boundary map and the aliasable-id set from the module.
3. Validate the execution section (PC, frames, labels, call chain, debug ids).
4. Validate every value tree against the target type shape and every alias
   path against the target allocation bounds. All counts are checked against
   the available bytes and module bounds before any allocation.
5. Require `END1` with no trailing bytes.
6. Only then commit: write values into existing trees, rebuild alias pointers,
   and install the execution scene and call frames.

Any failure returns 0 without changing the target's values, aliases, execution
position or host bindings.
