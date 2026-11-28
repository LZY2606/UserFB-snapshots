# DWARF 5 indexed forms

This document describes the support libelfin adds for reading the
DWARF 5 indexed string and address forms, along with
`DW_FORM_implicit_const`.  It documents the supported scope, which
compilation unit an attribute's value belongs to, and the error
behaviour.

The existing DWARF 5 compilation-unit header (version 5, unit type,
address size, abbrev offset) was already readable.  These changes
extend the existing `compilation_units()`, `root()`, child-DIE
iteration, `attributes()`, `operator[]` and `resolve()` interface so
callers do not need a separate parser.

## Supported forms

| Form | Meaning | Backing section |
| --- | --- | --- |
| `DW_FORM_strx` | ULEB128 string index | `.debug_str_offsets` → `.debug_str` |
| `DW_FORM_strx1` | 1-byte string index | `.debug_str_offsets` → `.debug_str` |
| `DW_FORM_strx2` | 2-byte string index | `.debug_str_offsets` → `.debug_str` |
| `DW_FORM_strx3` | 3-byte string index | `.debug_str_offsets` → `.debug_str` |
| `DW_FORM_strx4` | 4-byte string index | `.debug_str_offsets` → `.debug_str` |
| `DW_FORM_addrx` | ULEB128 address index | `.debug_addr` |
| `DW_FORM_addrx1` | 1-byte address index | `.debug_addr` |
| `DW_FORM_addrx2` | 2-byte address index | `.debug_addr` |
| `DW_FORM_addrx3` | 3-byte address index | `.debug_addr` |
| `DW_FORM_addrx4` | 4-byte address index | `.debug_addr` |
| `DW_FORM_indirect` | wraps any of the above (and the existing forms) | as wrapped |
| `DW_FORM_implicit_const` | signed LEB128 value stored in the abbrev | `.debug_abbrev` |

String forms are returned by the existing `value::as_string()` /
`value::as_cstr()`; `as_cstr()` returns a pointer into the loaded
`.debug_str` section, exactly as for `DW_FORM_strp`.  Address forms are
returned by `value::as_address()`.

`DW_FORM_implicit_const` values are read through the existing constant
converters: `as_sconstant()` returns the signed value and
`as_uconstant()` returns its two's-complement unsigned form.  The
value is stored in the abbreviation table and occupies **no** bytes in
the DIE, so DIE decoding and the `next`-DIE offset remain correct even
when implicit constants are interleaved with indexed and ordinary
attributes.

### Table bases

Indexed attributes are resolved relative to the owning compilation
unit's `DW_AT_str_offsets_base` / `DW_AT_addr_base`.  The base
attributes do **not** need to appear, or be read, before the indexed
attributes they locate.  libelfin scans the root DIE's abbreviated
attribute stream directly (`unit::root_attribute`) instead of
constructing the root `die`, so a `DW_AT_name` encoded as `strx` may be
read before `DW_AT_str_offsets_base`, and a `DW_AT_low_pc` encoded as
`addrx` may be read before `DW_AT_addr_base`.

If a unit omits the base attribute, the base defaults to the first
entry of the first contribution in the corresponding section.

The meaning of the DWARF32/64 initial length, the section byte order
and the address width is preserved:

* The `.debug_str_offsets` and `.debug_addr` contributions are parsed
  from their own headers.  A DWARF64 contribution has 8-byte table
  entries; a DWARF32 contribution has 4-byte entries for
  `.debug_str_offsets`.
* Table entries are read in the file's byte order.
* `.debug_addr` entries are read at the contribution's address size
  (falling back to the compilation unit's address size).
* `DW_FORM_strx3` / `addrx3` and other 3-byte quantities honour the
  file's byte order.

### Attribute ownership

Every `value` keeps a pointer to the unit that actually supplied it.
For `die::resolve()`, when an attribute is inherited from an
`DW_AT_abstract_origin` or `DW_AT_specification` in another
compilation unit, the returned value belongs to the providing unit and
its indexed forms are resolved against **that** unit's tables, not the
referencing unit's.  `value::get_unit_offset()` is relative to the
providing unit, and `value::as_reference()`-style accessors identify
the providing unit.  Two compilation units may use identical indices
with different tables; interleaved lookups each resolve against the
correct unit.

## Deliberately out of scope

* Split DWARF (`DW_FORM_ref_sup*`, `DW_FORM_strp_sup`, and the
  supplementary object files).
* Line-number programs (`DW_FORM_line_strp` and line-table indices).
* Location and range lists (`DW_FORM_loclistx`, `DW_FORM_rnglistx`,
  and the `.debug_loclists` / `.debug_rnglists` sections).
* Indexed operands inside location expressions.
* `DW_FORM_data16` is not decoded; an abbreviation using it is
  rejected.

## Error behaviour

libelfin fails loudly rather than reading a neighbouring contribution
or fabricating zeros.  The following conditions throw
`dwarf::format_error` (or, where the underlying cursor runs off the
end of a section, `std::underflow_error`):

* An indexed form is used but the required `.debug_str_offsets` or
  `.debug_addr` section is missing.
* The base attribute points outside every contribution in its section,
  or into a contribution header rather than at a table entry.
* An index selects an entry beyond the end of the **owning
  contribution** (a matching index in an adjacent contribution is
  never used).
* A string offset read from `.debug_str_offsets` points outside
  `.debug_str`.
* The DIE is truncated in the middle of an index (fixed-width or
  ULEB128).
* The abbreviation table is truncated in the middle of an
  `DW_FORM_implicit_const` SLEB128 value.

Forms that are recognised but unsupported (split DWARF, line strings,
list indices) are rejected with `format_error` when an abbreviation
using them is parsed.

## Regression tests

`test/indexed_forms.cc` registers four groups through
`task_support/check.h`'s `task_cases` / `TASK_CHECK`:

* **indexed** — `strx`/`strx1..4`, `addrx`/`addrx1..4`,
  `DW_FORM_indirect` wrapping, three-byte indices, and a big-endian
  DWARF64 unit with a 4-byte address model.
* **constants** — `DW_FORM_implicit_const` (including negative values
  adjacent to ordinary fields and a following DIE at the exact expected
  offset), plus out-of-contribution, missing-section and truncation
  failures.
* **context** — two units sharing indices but using different tables
  with interleaved reads, late base attributes, and cross-unit
  `resolve()`.
* **legacy** — direct strings, `DW_FORM_strp`, `DW_FORM_addr` and the
  baseline DWARF 4 fixture.

All fixtures are constructed in C++ using `task_support/fixture.hh`;
no image is executed.  Run them with:

```sh
bash task_support/run.sh target
```
