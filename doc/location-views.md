# Location views and `(PC, view)` recovery

GNU location views keep distinct logical assignments that share a machine
instruction address.  The interfaces below preserve both coordinates instead
of collapsing a same-PC range to one record.

All data is read from the offline ELF/DWARF bytes.  The implementation does
not execute the object, inspect another process, or read hardware registers.
`DW_OP_reg0` and `DW_OP_reg1` are interpreted only as “the location is DWARF
register 0 or 1”; no register value is obtained.

## Coordinate model

A range is the half-open lexicographic interval

```text
[(begin_pc, begin_view), (end_pc, end_view))
```

over ordered pairs `(PC, view)`.

- Different PCs are ordered by PC first.
- At the same PC, increasing view numbers form a valid, potentially
  non-empty range.
- The range is empty only when both coordinates are exactly equal.
- A lexicographically reversed range is reported as `DW_DLV_ERROR`.

A missing view pair is different from an explicit `(0, 0)` pair.  Missing
views leave `Dwarf_Locview.has_views` false; an explicit zero pair leaves it
true with `begin_view == end_view == 0`.

## Public API

```c
typedef struct Dwarf_Locview_s {
    Dwarf_Bool     has_views;
    Dwarf_Unsigned begin_view;
    Dwarf_Unsigned end_view;
    Dwarf_Small    source_kind;
    Dwarf_Unsigned section_offset;
    Dwarf_Unsigned byte_length;
} Dwarf_Locview;

int dwarf_get_loclist_views_c(Dwarf_Die dw_die,
    Dwarf_Attribute dw_attr,
    Dwarf_Loc_Head_c *head,
    Dwarf_Unsigned *count,
    Dwarf_Error *error);

int dwarf_get_locdesc_views(Dwarf_Locdesc_c locdesc,
    Dwarf_Locview *view,
    Dwarf_Error *error);
```

`dwarf_get_loclist_views_c()` inserts the owning DIE before the
`dwarf_get_loclist_c()` attribute argument.  It finds a
`DW_AT_GNU_locviews` companion on that exact DIE.  Without the companion it
is equivalent to `dwarf_get_loclist_c()`, including inline view-pair
prefixes.

`source_kind` is:

| Value | Constant                       | Meaning |
|------:|--------------------------------|---------|
| 0     | `DW_LOCVIEW_SOURCE_NONE`       | No view pair |
| 1     | `DW_LOCVIEW_SOURCE_INLINE`     | Inline `DW_LLE_GNU_view_pair` |
| 2     | `DW_LOCVIEW_SOURCE_EXTERNAL`   | `DW_AT_GNU_locviews` ULEB table |

`section_offset` is relative to the owning section: `.debug_loclists` for
DWARF 5 or `.debug_loc` for DWARF 4.  For inline views, `byte_length`
includes the `DW_LLE_GNU_view_pair` opcode.  For external views it includes
only the two ULEB128 values.

View storage is attached to the `Dwarf_Loc_Head_c` / `Dwarf_Locdesc_c`
records and follows the normal head lifetime.  Each DIE uses only its own
companion attribute; companion offsets are not shared implicitly between
DIEs.

## Inline DWARF 5 encoding

`DW_LLE_GNU_view_pair` is defined as `0x09`.  It is followed by two unsigned
LEB128 view values:

```text
09 begin_view end_view
```

The prefix modifies only the immediately following address-range entry:

- `DW_LLE_offset_pair`
- `DW_LLE_start_end`
- `DW_LLE_start_length`
- `DW_LLE_startx_endx`
- `DW_LLE_startx_length`

It does not change the address base and has no location expression.  The
public description count excludes the view-pair record itself, while base,
default, address-range, and end records remain.  Two consecutive prefixes, or
a prefix immediately before a base/default/end record, are errors.

The raw section API `dwarf_get_loclist_lle()` still reports the prefix itself:
`entry_kind == DW_LLE_GNU_view_pair`, operand 1 and 2 are the two views, and
`entrylen` is the exact byte length including opcode and ULEB bytes.

## External `DW_AT_GNU_locviews` tables

A `DW_AT_GNU_locviews` attribute is a section offset to a sequence of ULEB128
pairs.  The table is in the same location section as its companion list:

- DWARF 4: `.debug_loc`
- DWARF 5: `.debug_loclists`

Pairs are consumed in list order only for address-range records.  Base,
default-location, and end records consume no pair.  After all address-range
records have consumed a pair, the external decode cursor must be exactly at
the resolved start of the companion location list.  Extra bytes, truncated
ULEB128 data, or an overrrun into the list are errors; the reader never
borrows bytes from an adjacent table.

Both `DW_FORM_sec_offset` and `DW_FORM_loclistx` location attributes use the
resolved real list start.  For `DW_FORM_loclistx`, the offset table entry is
resolved through the `.debug_loclists` header before checking the external
table boundary.  Split DWARF and non-zero segment selectors are outside this
change.

## Existing offset behavior

For each retained description:

- `dwarf_get_locdesc_entry_e()`'s `dw_locdesc_offset_out` points at the
  address-entry opcode.  With an inline prefix this is after the view-pair
  bytes, not at the list start or view opcode.
- `dw_expression_offset_out` points at the first expression byte.
- `Dwarf_Locview.section_offset` points at the inline view opcode or the
  first ULEB byte of an external pair.

## `dwarfdump -i` output

View-bearing ranges receive an additional line of the form:

```text
views=[(0x1000,1),(0x1000,2)) view_source=inline view_section=.debug_loclists view_offset=0x15 view_length=3
```

The original same-PC expression remains printed underneath.  Ranges without
views retain the prior output.  `dwarfdump --print-raw-loclists` also names
and prints `DW_LLE_GNU_view_pair`.

## Analysis consumer and tests

`tests/location_views_contract.c` contains the `inline_views`,
`external_views`, and `consumer_views` task cases.  Its consumer initializes
the generated ELF objects with libdwarf, traverses CU/DIE trees, calls
`dwarf_get_loclist_views_c()` and `dwarf_get_locdesc_views()`, and writes:

- `analysis/location-views.tsv`
- `analysis/location-views.txt` from an actual built `dwarfdump -i` run

Relevant implementation units are:

- `src/lib/libdwarf/dwarf_location_views.c`: public view API and external
  table association.
- `src/lib/libdwarf/dwarf_loclists.c`: opcode 0x09 raw parsing, inline
  prefix validation/attachment, and loclist offset correction.
- `src/lib/libdwarf/dwarf_loc.c`: DWARF 4 `.debug_loc` start-offset
  propagation.
- `src/bin/dwarfdump/print_die.c`: `-i` view lines.
- `src/bin/dwarfdump/print_loclists.c`: raw opcode display.
- `tests/location_views_contract.c`: contract tests and TSV consumer.
