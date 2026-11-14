# `dwarf::at_location`: where is a variable at a given PC?

`dwarf::at_location(variable, function, pc, ctx)` answers an offline
question using only the debugging sections and a caller-supplied
register/memory snapshot:

```cpp
dwarf::expr_result
dwarf::at_location(const dwarf::die &variable,
                   const dwarf::die &function,
                   dwarf::taddr pc,
                   dwarf::expr_context *ctx);
```

It returns an `expr_result` describing where `variable` lives at
program counter `pc`.  The target program is never executed; register
and memory values come exclusively from `ctx`.

`function` is the subprogram DIE that contains `variable`.  The caller
provides it; the library does not search the DIE tree to find the
enclosing function.

## Scope

This entry supports:

* Direct `DW_AT_location` expressions, including pre-DWARF 4 block
  forms (`DW_FORM_block`, `block1`, `block2`, `block4`), which are
  interpreted as expression locations.
* Legacy DWARF 2–4 location lists in `.debug_loc`.

It deliberately does **not** support:

* DWARF 5 `.debug_loclists` (the new loclist format).
* Composite locations (`DW_OP_piece` / `DW_OP_bit_piece`).
* Reading the live target process.  Register and implicit values are
  described, never fetched automatically.
* Searching the DIE tree for the owning function.

The meaning of `expr_result` is unchanged.  A register location still
only names a register, an implicit location still only points at the
encoded bytes, and the caller decides whether and how to read them.

## How the location attribute is resolved

Both `DW_AT_location` (on `variable`) and `DW_AT_frame_base` (on
`function`) are read with the existing `die::resolve` rules, so the
normal precedence of concrete DIE, `DW_AT_abstract_origin`, and
`DW_AT_specification` is preserved.  A location may therefore be owned
by a different compilation unit than the DIE it was reached through.

If `variable` has no resolvable `DW_AT_location`, the result is an
`expr_result` of type `empty`.  A zero address is never fabricated to
stand in for a missing location.

## Legacy `.debug_loc` semantics

A location list is decoded entirely with the address size, byte order,
and initial base address of the **compilation unit that owns the
location attribute**, not the unit of the variable DIE.  The initial
base address is the owning CU root DIE's `DW_AT_low_pc`, or zero when
absent.

The scanner implements the DWARF 2–4 rules:

* An entry whose first address is the maximum address for the unit's
  address size is a base-address selection entry; its second address
  becomes the new base.
* Other entries are half-open ranges `[low, high)` relative to the
  current base.  Zero-length ranges (`low == high`) never match.
* A matching entry with a zero-length expression is an empty location.
* The `(0, 0)` pair terminates the list.
* When several entries contain `pc`, the first one in table order
  wins.  If the selected expression is empty, the result is `empty`;
  scanning does not continue to a later entry.
* A PC that falls in no range (a gap) yields `empty`.

The whole list, through its terminator, is structurally validated even
when an earlier entry matches, so a malformed tail is reported rather
than hidden behind a successful match.  Validation stops at the
terminator; bytes belonging to the following list are never touched.
Unselected expressions are parsed for length but never evaluated.

Nothing about a selection is cached: each call re-scans the list, so
repeated and reverse-order queries are independent.

Expression bytes embedded in `.debug_loc` are read from that section;
their offsets are not interpreted as `.debug_info` offsets.  The
owning CU still supplies the target context used to interpret
`DW_OP_addr`, register opcodes, constants, and `DW_OP_stack_value`.

## `DW_OP_fbreg`

`DW_OP_fbreg` adds a signed offset to the frame base selected for the
same PC.  The frame base is taken from `function`'s
`DW_AT_frame_base`, which may itself be a direct expression or a
`.debug_loc` list.

The frame base is resolved **lazily**: it is only evaluated when the
selected location expression actually executes `DW_OP_fbreg`.  A
variable located purely in a register or at a `DW_OP_breg*` address
therefore never requires a usable frame base.

When the frame-base expression yields:

* a **register** result, the register number is passed to
  `expr_context::reg` and the register's contents become the base;
* an **address** or **literal** result, its numeric value is used
  directly.

The register number is never treated as an address, and no extra
memory dereference is performed on the base.

A frame base that itself (transitively) uses `DW_OP_fbreg` is reported
as a circular frame-base dependency instead of recursing.  Calling
`expr::evaluate` directly without supplying a frame base keeps the
previous behavior of raising an error for `DW_OP_fbreg`.

## Empty results versus exceptions

An **empty** `expr_result` is the normal answer when:

* the variable has no location attribute;
* `pc` lies in a gap between location ranges; or
* the matching location entry has an empty expression.

A `dwarf::format_error` (or, for expression problems,
`dwarf::expr_error`) is thrown for malformed input.  Location-list
errors name the section and the absolute entry offset, and include:

* a missing `.debug_loc` section;
* a truncated address pair, expression length, or expression body;
* a list with no terminator;
* a reversed range;
* base-address arithmetic that wraps the target address space;
* a frame base that is absent or has an unsuitable result type.

These errors are never silently converted into an empty result.
