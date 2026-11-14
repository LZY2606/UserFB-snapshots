# DWARF5 indexed forms and implicit constants

libdwarf++ reads DWARF5 indexed attribute forms and
`DW_FORM_implicit_const` through the regular DIE interface:
`dwarf::dwarf::compilation_units()`, `unit::root()`, child DIE
iteration, `die::attributes()`, `die::operator[]` and
`die::resolve()`.  No separate parser is involved; the forms are
decoded by `value`'s usual accessors.

## Supported forms

* String-indexed forms `DW_FORM_strx`, `DW_FORM_strx1`,
  `DW_FORM_strx2`, `DW_FORM_strx3` and `DW_FORM_strx4`.  The index
  selects an entry in the compilation unit's contribution to
  `.debug_str_offsets`; the entry holds an offset into `.debug_str`.
  Read them with `value::as_string()` / `value::as_cstr()`.  Their
  value type is `value::type::string`.
* Address-indexed forms `DW_FORM_addrx`, `DW_FORM_addrx1`,
  `DW_FORM_addrx2`, `DW_FORM_addrx3` and `DW_FORM_addrx4`.  The index
  selects an entry in the unit's contribution to `.debug_addr`, whose
  entries are target addresses.  Read them with
  `value::as_address()`.  Their value type is `value::type::address`.
* `DW_FORM_implicit_const`.  The signed LEB128 constant is recorded
  in the abbrev declaration and occupies no bytes in the DIE.  Read
  it with `value::as_sconstant()`.  Its value type is
  `value::type::sconstant`.
* `DW_FORM_indirect` wrapping any of the indexed forms above (the
  actual form is resolved from the DIE data, as for other forms).

The `.debug_str_offsets` and `.debug_addr` sections are loaded
through the existing `dwarf::loader` interface (and recognized by
`dwarf::elf::section_name_to_type`), so any loader that serves the
other DWARF sections serves these as well.

## Table location and per-unit context

Table entries are located relative to the owning compilation unit:

* `DW_AT_str_offsets_base` (default 0) gives the start of the unit's
  `.debug_str_offsets` contribution; entry size is 4 bytes for
  DWARF32 units and 8 bytes for DWARF64 units.
* `DW_AT_addr_base` (default 0) gives the start of the unit's
  `.debug_addr` contribution; entry size is the unit's address size
  (1, 2, 4 or 8 bytes).

Both base attributes are resolved lazily from the unit's root DIE
when an indexed value is first read, so they may be declared in the
root DIE *after* the attributes that use them (for example,
`DW_AT_name` before `DW_AT_str_offsets_base`, or `DW_AT_low_pc`
before `DW_AT_addr_base`); callers never need to read the base
attributes first.  Section byte order (little- or big-endian) and
DWARF32/DWARF64 format are honored for indices, table entries and
offsets.

Each `value` remembers the unit that actually provides the
attribute.  When `die::resolve()` follows `DW_AT_specification` or
`DW_AT_abstract_origin` into a different compilation unit, the
returned value is interpreted with *that* unit's
`str_offsets_base`/`addr_base` and tables.  Two units may therefore
use the same table indices with disjoint table contributions, and
interleaved queries stay correct.

## Error behavior

* A missing `.debug_str_offsets`, `.debug_addr` or `.debug_str`
  section raises `dwarf::format_error` when a value that needs it is
  read.
* An index whose entry lies outside the section (including outside
  the unit's contribution) raises `dwarf::format_error`; neighboring
  tables are never read and no zero is substituted.
* A string offset that escapes `.debug_str` raises
  `dwarf::format_error`.
* A truncated index in DIE data and a truncated SLEB128 constant in
  an abbrev declaration raise `std::underflow_error` from the cursor
  layer.

## Out of scope

Split DWARF (`.dwo`/supplementary files and their forms), indexed
forms in line number programs, location lists and range lists
(`DW_FORM_loclistx`, `DW_FORM_rnglistx`), and indexed operations
inside DWARF expressions (`DW_OP_addrx`, `DW_OP_constx`) are not
decoded.  Pre-DWARF5 behavior (direct `DW_FORM_string`,
`DW_FORM_strp`, `DW_FORM_addr`, and all DWARF4 unit formats) is
unchanged.
