# Offline variable location and snapshot analysis

This document describes the support added for reconstructing where a
variable's value comes from entirely offline: from an ELF file's DWARF
data plus a register/memory snapshot, without executing the ELF and
without reading the analyzing process's own memory or registers.

## What is supported

### Location resolution (`dwarf::at_location`)

```cpp
dwarf::expr_result dwarf::at_location(const dwarf::die &variable,
                                      const dwarf::die &function,
                                      dwarf::taddr pc,
                                      dwarf::expr_context *ctx);
```

The caller supplies the variable DIE, the DIE of its enclosing
function, the program counter and an expression context. The library
does not search for variables or enumerate lexical scopes.

`DW_AT_location` and `DW_AT_frame_base` are resolved with the same
inheritance order as `dwarf::die::resolve`: the DIE itself first, then
its abstract origin, then its specification. The following encodings
work:

* Direct `DW_FORM_exprloc` expressions.
* Legacy block forms (`DW_FORM_block`, `block1`, `block2`, `block4`),
  which are interpreted as expressions.
* DWARF 2–4 location lists in `.debug_loc` (`DW_FORM_sec_offset` and
  the older `data4`/`data8` section-offset forms).

For a location list the entry whose half-open range `[low, high)`
contains `pc` is selected; if several entries match, the first entry
in the original table wins. A gap between entries, or a selected entry
whose expression is empty, yields an `empty` location result. The
whole list is validated through its end-of-list terminator, but
expressions of unselected entries are never evaluated.

Location lists are interpreted using the address size, byte order and
`DW_AT_low_pc` of the CU that owns the referring attribute, including
base-address selection entries (`max_address, new_base`). Truncation, a
missing terminator, a reversed range and base-address wraparound raise
`dwarf::format_error` naming `.debug_loc` and the offending entry
offset; they are never reported as an unavailable variable.

### Expression evaluation

Expressions remember their owning CU and read their bytes from the
correct section: `.debug_info` for ordinary attributes and
`.debug_loc` for location list entries. The previously supported
register, base-register, address, constant and stack-value operations
keep working, and the following are now implemented:

* `DW_OP_fbreg` — evaluates the function's `DW_AT_frame_base` at the
  same `pc` and adds the signed operand.
* `DW_OP_call_frame_cfa` — calls `expr_context::cfa()`.

`dwarf::expr_context` gains two virtual methods. Their default
implementations throw `dwarf::expr_error`:

* `cfa()` returns the canonical frame address. The value comes from
  the caller's snapshot; the library performs no stack unwinding.
* `frame_base()` returns the frame base.

Frame bases are only computed when an expression actually uses
`DW_OP_fbreg`. The three meaningful frame-base results are handled as
follows:

* a register result reads that register and uses the register value as
  the frame base;
* an address result is used directly as the frame base;
* a literal (`DW_OP_stack_value`) result is used directly.

Address and literal frame bases are never dereferenced. A
`DW_AT_frame_base` expression that itself uses `DW_OP_fbreg` raises an
expression error for recursive evaluation. If the function has no
`DW_AT_frame_base` at all, a `dwarf::frame_base_missing` (an
`expr_error`) is raised. Plain register locations never consult the
CFA or the frame base.

Operations the engine still does not implement (for example
`DW_OP_piece`, `DW_OP_call*`, `DW_OP_push_object_address`) raise
`dwarf::expr_unsupported`, an `expr_error` subtype, so they can be
distinguished from malformed data.

## The offline snapshot interface (`dwarf/variable.hh`)

### Snapshot

`dwarf::variable_snapshot` is an owning description of target state:

* the target byte order (which must match the ELF's byte order);
* a register-number to value table;
* an optional CFA;
* non-overlapping memory byte segments addressed by target address.

Target addresses are stored as integers and are never converted to
native pointers. Segments are checked for overlap and address-space
wraparound when added; a read whose address plus width wraps is
rejected. Memory reads concatenate adjacent segments, but if any single
byte is missing the whole read fails with no partial result.

### Analysis

```cpp
dwarf::variable_result dwarf::inspect_variable(
        const dwarf::die &variable, const dwarf::die &function,
        dwarf::taddr pc, unsigned width,
        const dwarf::variable_snapshot &snapshot);
```

`width` is the value width in bytes (1–8) and is always given
explicitly by the caller; there is no full type system. The result is
owning (scalars, strings and byte vectors only) and stays valid after
the ELF loader, snapshot and DIE arguments are released or modified.

The status distinguishes:

| status             | meaning                                                  |
| ------------------ | -------------------------------------------------------- |
| `value`            | a value was recovered                                    |
| `unavailable`      | no location at this pc (gap, empty expression, no frame base) |
| `missing_register` | a needed register is absent from the snapshot            |
| `missing_cfa`      | `DW_OP_call_frame_cfa` was needed but no CFA was given    |
| `missing_memory`   | a needed target byte is absent                           |
| `unsupported`      | the location expression uses an unimplemented operation  |
| `malformed`        | the DWARF data or expression is malformed                |

Values are assembled as unsigned integers in the ELF byte order and the
original bytes are retained in `raw_bytes`. Register locations read the
register and keep the low `width` bytes; literal locations use the
value directly; address locations read only from the snapshot's memory
segments. Missing snapshot context is never filled with zeroes.

### Evidence

A `variable_result` records only the context that was actually used:

* the DIE that supplied `DW_AT_location` and, when relevant, the DIE
  that supplied `DW_AT_frame_base`;
* whether a location list was used and the selected range;
* the section and offset of the evaluated expression;
* the registers read, whether the CFA was read (and its value), the
  final value address and the raw memory bytes.

Registers, the CFA and memory that were not accessed never appear in
the evidence.

## Example

`examples/inspect-variable.cc` ties the pieces together. It takes an
ELF path, CU/function/variable DIE offsets, a PC, a width, the target
byte order and snapshot contents, then prints the value and its
provenance. The controlled snapshots for
`test/golden-gcc-4.9.2/example` recover `fib`'s parameter `x` (CU
offset 0, function DIE `0x2b`, parameter DIE `0x4c`, PC `0x4004bd`,
width 4):

```sh
# CFA 0x7000; the four bytes at CFA-36 (0x6fdc) are 2a000000 -> 42
examples/inspect-variable test/golden-gcc-4.9.2/example \
    0 0x2b 0x4c 0x4004bd 4 endian=little \
    cfa=0x7000 mem=0x6fdc:2a000000

# Shifted CFA and bytes, value 17000000 -> 23
examples/inspect-variable test/golden-gcc-4.9.2/example \
    0 0x2b 0x4c 0x4004bd 4 endian=little \
    cfa=0x8000 mem=0x7fdc:17000000
```

Removing any required byte changes the outcome to `missing_memory`.

## Tests

`tests/variable_snapshot_contract.cc` registers four groups through
`task_support/check.h`:

* `elf` — the real golden ELF and both controlled snapshots, including
  the exact value, status and provenance and a missing-byte case;
* `locations` — synthetic in-memory ELF/DWARF covering cross-CU
  attributes, base-address changes, gaps, first-match selection, empty
  expressions and identical numeric offsets landing in different
  sections;
* `context` — register/literal locations not reading memory, negative
  `DW_OP_fbreg`, register and literal frame bases, cross-segment
  reads and result ownership after the inputs are released;
* `errors` — truncated and unterminated lists, reversed ranges,
  wraparound, missing snapshot context, recursive frame bases,
  unsupported operations and recovery after a failed query.

`bash task_support/run.sh target` builds the library, the example and
the four test groups and then runs the existing ELF and expression
regressions. `bash task_support/run.sh baseline` runs only the original
regressions.
