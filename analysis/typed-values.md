# Typed DWARF expression recovery — `task_support/fixtures/typed.elf`

The table in `typed-values.tsv` was produced by
`examples/typed_expression`, which walks the real `.debug_info` of the
bundled ELF with `debug/elf` + `debug/dwarf`, builds one
`godwarf.NewBaseTypeResolver` per compilation unit, and evaluates each
variable's `DW_AT_location` bytes with `op.ExecuteStackProgramTyped`
(ptrSize 8, little endian, no memory snapshot). Nothing was filled in by
file name, variable name, or CU order; every row is derived from the DIEs
and expression bytes themselves.

## The two `narrow` variables

Both CUs contain a variable named `narrow` whose location is the exact same
byte sequence (`a40d01fea8159f`):

```
DW_OP_const_type 0xd 1 [fe]  DW_OP_convert 0x15  DW_OP_stack_value
```

Yet the recovered values differ:

* CU at header offset `0x0`, variable DIE `0x2e`: **-2**, piece low bytes `feff`
* CU at header offset `0x3f`, variable DIE `0x6d`: **254**, piece low bytes `fe00`

The difference comes entirely from the type DIEs the expression refers to,
not from the expression bytes:

1. `DW_OP_const_type 0xd 1 [fe]` pushes the raw byte `fe` using the base
   type at CU-relative offset `0xd` (the DIE named `byte`).
   * In CU `0x0` that DIE (global offset `0xd`) is `DW_ATE_signed`, size 1,
     so `fe` is the value **-2**.
   * In CU `0x3f` the same relative offset `0xd` (global offset `0x4c`) is
     `DW_ATE_unsigned`, size 1, so `fe` is the value **254**.
2. `DW_OP_convert 0x15` converts by value to the type at relative offset
   `0x15` (`word`, `DW_ATE_signed`, size 2 — same signedness in both CUs).
   Conversion sign-extends or truncates the *numeric value*: -2 becomes
   `fffe`, 254 becomes `00fe`.
3. `DW_OP_stack_value` turns the typed value into an immediate piece holding
   the raw low bytes of the result, little endian: `feff` for the first CU,
   `fe00` for the second. The type stack is reset at the piece boundary, so
   a following piece would not inherit this type.

The signed decimal in the `value` column is the interpretation of those
piece bytes under the *final* base type (`word`, signed 16-bit, listed in
`type_rel_off`/`type_die_off`/`type_size`/`type_signed`): `feff` reads as
-2, `fe00` reads as 254. The piece bytes themselves stay raw; they are not
an address and are never used to read memory.

## Why the same local offset cannot share a global cache

Both compilation units lay out their base types at identical CU-relative
offsets: `byte` at `0xd`, `word` at `0x15`, `wide` at `0x1d`. But relative
offset `0xd` means a *signed* 1-byte type in CU `0x0` and an *unsigned*
1-byte type in CU `0x3f` — the type identity is the DIE, and the DIEs live
at different global offsets (`0xd` vs `0x4c` from the start of
`.debug_info`). A resolver cache keyed only by the CU-relative offset would
return whichever unit was resolved first and silently flip the signedness
for the other unit, changing `fe` from -2 to 254 (or vice versa). That is
why `NewBaseTypeResolver` takes the CU header offset and builds a private
per-unit table: interleaving the two resolvers on the same relative offset
`0xd` yields `{1, signed}` and `{1, unsigned}` independently, and offsets
outside the unit, non-DIE starts, and non-base-type DIEs are rejected.

## The `plain` variables

Both `plain` variables (DIEs `0x25` and `0x64`) have location `37`, i.e.
`DW_OP_lit7`. The program pushes 7 and ends without `DW_OP_stack_value`, so
the result is an *address location*: 7 is the address of the variable, not
its value. The rows are classified `address` with no piece bytes, and since
no memory snapshot was supplied the tool does not dereference 7.
