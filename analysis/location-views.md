# `location-views` analysis notes

Generated data:

- `analysis/location-views.tsv` — one row per variable location range from
  `.task-build/plain.elf` and `.task-build/views.elf`.
- `analysis/location-views.txt` — actual built `dwarfdump -i` output for both
  objects, concatenated.

The rows are produced by the `consumer_views` C task case through
`dwarf_init_path()`, CU/DIE traversal, `dwarf_get_loclist_views_c()`,
`dwarf_get_locdesc_entry_e()`, and `dwarf_get_locdesc_views()`.  Base and
end-of-list records remain available from libdwarf but are not written as
variable ranges.

## Recovered records

| Object | PC interval | View interval | Expression | Conclusion |
|---|---|---|---|---|
| `plain.elf` | `[0x1000,0x1000)` | absent | `50` / `DW_OP_reg0` | empty same-PC range |
| `plain.elf` | `[0x1000,0x1010)` | absent | `51` / `DW_OP_reg1` | ordinary non-empty range |
| `views.elf` | `[(0x1000,1),(0x1000,2))` | `[1,2)` inline | `50` / `DW_OP_reg0` | non-empty same-PC view range |
| `views.elf` | `[(0x1000,0),(0x1010,0))` | explicit `(0,0)` | `51` / `DW_OP_reg1` | ordinary PC width; explicit zero views |

`DW_OP_reg0` and `DW_OP_reg1` mean that the variable is located in DWARF
register number 0 or 1.  This analysis does not read those registers or
report runtime values.

## Why the first record differs between the two objects

The plain object's first address-range entry at `.debug_loclists` offset
`0x15` is:

```text
04 00 00 01 50
```

Decoding:

- `04` = `DW_LLE_offset_pair`
- begin offset ULEB `00`
- end offset ULEB `00`
- expression length ULEB `01`
- expression byte `50` = `DW_OP_reg0`

The preceding `DW_LLE_base_address` at section offset `0x0c` supplies base
PC `0x1000`.  Therefore the PC interval is

```text
[0x1000 + 0, 0x1000 + 0) = [0x1000, 0x1000)
```

and, because there is no view pair, the lexicographic endpoints are equal.
The range is empty.  The consumer records this as
`empty-same-pc-no-view`; it does not invent a view number.

The view object inserts three bytes immediately before the same address
entry:

```text
09 01 02
```

Here `09` is `DW_LLE_GNU_view_pair`, followed by ULEB128 values `1` and `2`.
The address entry after it is unchanged:

```text
09 01 02 | 04 00 00 01 50
```

Thus the logical interval is

```text
[(0x1000,1), (0x1000,2))
```

Although both PCs are `0x1000`, the lexicographic start `(0x1000,1)` is less
than the end `(0x1000,2)`.  This is a valid non-empty range, recorded as
`nonempty-same-pc-view`.  Both this relation and the later register-1
relation are retained; neither is selected as the “only” variable location.

## Raw-byte and offset cross-reference

For the first view-bearing register-0 relation in `views.elf`:

| Item | Section offset | ELF file offset in the fixture | Bytes / value |
|---|---:|---:|---|
| `DW_LLE_base_address` opcode | `0x0c` | `0x85` | `06` |
| Base address | `0x0d` | `0x86` | `00 10 00 00 00 00 00 00` = `0x1000` |
| Inline view-pair opcode | `0x15` | `0x8e` | `09` |
| Begin view ULEB | `0x16` | `0x8f` | `01` |
| End view ULEB | `0x17` | `0x90` | `02` |
| Address-entry opcode | `0x18` | `0x91` | `04` |
| Address operands/length | `0x19..0x1b` | `0x92..0x94` | `00 00 01` |
| Expression first byte | `0x1c` | `0x95` | `50` (`DW_OP_reg0`) |

The `.debug_loclists` section's `sh_offset` in the generated fixture is
`0x79`, so an ELF file offset is `0x79 + section_offset`.

The API returns exactly these distinctions:

```text
Dwarf_Locview.section_offset = 0x15
Dwarf_Locview.byte_length    = 3          /* includes 0x09 */
address entry offset         = 0x18
expression offset            = 0x1c
```

For the plain object the corresponding address entry starts at `0x15` and
its expression starts at `0x19`.  These offsets are not left at the list
start (`0x0c`).

## Mutation check

The `mutate_first_uleb_pair` C case copies `views.elf`, changes only the
first external ULEB bytes of the first inline pair from `01 02` to `00 00`,
and reopens the copy through the same production API.  The first relation
then remains explicitly viewed but becomes

```text
[(0x1000,0),(0x1000,0))
```

and its analysis label changes from `nonempty-same-pc-view` to
`empty-explicit-view`.  Section offsets, byte lengths, PC, expression, and
the second record remain unchanged.  Changing the pair to `02 01` makes the
coordinates lexicographically reversed and
`dwarf_get_loclist_views_c()` returns `DW_DLV_ERROR`.

## External tables

The `external_views` cases construct companion tables immediately before
DWARF 4 `.debug_loc` and DWARF 5 `.debug_loclists` lists.  Each address-range
record consumes one ULEB128 pair; base, default-location, and end records do
not.  The tests verify:

- same-PC external relation `[(0x1000,1),(0x1000,2))`;
- explicit external `(0,0)`;
- source kind `2`;
- external `byte_length` covering only the two ULEB values;
- base-address switching;
- two DIEs in one section with different companion tables;
- `DW_FORM_sec_offset` and `DW_FORM_loclistx` real list starts;
- truncated ULEB128 and extra-table-byte errors.

Reproduce all checks and regenerate the analysis files with:

```sh
bash task_support/run.sh target
```
