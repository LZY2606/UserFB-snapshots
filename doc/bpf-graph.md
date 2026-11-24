# BPF graph analysis

`elfio/elfio_bpf_graph.hpp` adds a small, read-only static analysis layer for
offline BPF relocatable objects. It is exported by `<elfio/elfio.hpp>`.

The analyzer only parses ELFIO data structures. It does not load or execute
BPF, make kernel calls, invoke libbpf or an external disassembler, apply
relocations, or modify the input `elfio`.

## Supported object profile

The current profile is:

- `ELFCLASS64`, little-endian `ELFDATA2LSB`;
- `ET_REL` and `EM_BPF`;
- no program segments;
- no compressed sections;
- ordinary `SHT_REL` relocation records.

Functions are positive-size `STT_FUNC` symbols defined in an `SHT_PROGBITS`
section carrying `SHF_EXECINSTR`. Their intervals must be in bounds, eight-byte
aligned, complete, and non-overlapping. Maps are positive-size `STT_OBJECT`
symbols in caller-selected sections; their intervals must not overlap.

The object normally has one `SHT_SYMTAB`. Every identity is physical:

```text
symbol-table-section-index:symbol-ordinal
```

Names are copied for display only. In particular, identically named local
functions in different code sections remain distinct because their symbol
table index, ordinal, code section, and start offset differ.

## C++ API

```cpp
#include <elfio/elfio.hpp>
#include <elfio/elfio_bpf_graph.hpp>

ELFIO::elfio elf;
elf.load("program.o");

ELFIO::bpf_symbol_id entry{/*symtab section*/6, /*ordinal*/10};
std::vector<unsigned> map_sections = {4};

ELFIO::bpf_analysis_result result =
    ELFIO::analyze_bpf_graph(elf, {entry}, map_sections);

if (!result.ok) {
    // result.error contains a stable code and explanation.
    // result.issues[0].location contains section/file positions when possible.
    return 2;
}

const ELFIO::bpf_graph& graph = result.graph;
```

All strings and positions in `bpf_graph` are values owned by the result. The
result can be moved or copied and used after the source `elfio` is destroyed.

Important result types are:

- `bpf_symbol_id`: physical symbol-table section and ordinal;
- `bpf_instruction_location`: code section, section offset, original slot
  index (`offset / 8`), and physical file offset;
- `bpf_relocation_location`: `SHT_REL` section, entry ordinal, physical entry
  file offset, and the relocation's original symbol identity;
- `bpf_site`: instruction location plus optional relocation location;
- `bpf_function`, `bpf_map`, `bpf_call`, `bpf_map_reference`,
  `bpf_helper_call`, and `bpf_provenance`.

Physical file offsets are computed from the loaded section's `get_offset()`:

```text
instruction file offset = code section sh_offset + section byte offset
REL entry file offset   = REL section sh_offset + entry_index * sh_entsize
```

Virtual addresses and slot numbers are never used as file positions.

A fatal out-of-scope construct returns `ok == false`, a stable error code, a
human-readable message, and, where possible, a physical location. The graph in
that result is empty rather than a seemingly complete partial graph. Examples
include zero-length functions, multiple function identities at one start,
overlapping function/map intervals, unresolved call targets, invalid jump
targets, `src=2` BTF calls, and JMP32 long `JA` (`0x06`).

## Static analysis rules

Each BPF instruction slot is eight bytes. Opcode `0x18` (`lddw`) occupies two
consecutive slots; the second slot is not an instruction node. Linear slotting
is performed over the whole function before reachability, so instructions
bypassed by an unconditional jump are still split correctly.

The control-flow graph uses these rules:

- ordinary instructions continue to the next instruction start;
- an `lddw` continues after both eight-byte slots;
- `EXIT` (`0x95`) ends a path;
- `CALL` (`0x85`) returns and continues to the next slot;
- 16-bit `JA` (`0x05`) has only its jump edge;
- ordinary `JMP` and `JMP32` conditional branches retain both fall-through and
  taken edges; condition truth is not inferred;
- jump targets are `next_slot + signed_offset`;
- back edges are allowed and traversal is iterative.

Only call, helper, and map sites reachable from their containing function's
start are placed in the graph. A site bypassed by a legal unconditional jump is
still validated/slotted but produces no dependency. This is a static possible
reference graph; it does not prove a map is read or written and does not
perform register or stack abstract interpretation.

For `CALL` instructions:

- `src=0` without relocation is a helper. Its signed immediate is retained as
  the helper ID and it is not a function-graph edge.
- `src=1` without relocation targets
  `call_byte_offset + 8 + 8 * signed_imm` in the same code section.
- `src=1` with `R_BPF_64_32` (`10`) uses that REL record's `sh_link` symbol
  table and targets
  `st_value + 8 * (signed_imm + 1)` in the symbol's `st_shndx`. The `+1` is
  the implicit addend in an unlinked object; the call-site PC is not added.
  `STT_FUNC` and `STT_SECTION` symbols are accepted, and the final target must
  be a defined function start.
- `src=2` BTF calls are unsupported in this profile.

Each REL section's own `sh_info` identifies its code section; section names
and relocation entry order are not used to associate records with code.

Map references are `lddw` first slots carrying `R_BPF_64_64` (`1`). The target
byte offset is `st_value + unsigned_32(first_slot_imm)`. Both direct
`STT_OBJECT` symbols and `STT_SECTION` symbols with an implicit offset are
recognized, and the resolved position must be a map definition start in one of
the requested map sections. An unrelocated wide immediate is only a constant,
even if its numeric value resembles an address. Legal data references outside
the requested map sections are omitted from the map graph.

For every requested entry, the analyzer emits one provenance record for every
reachable map-reference site. References to the same map at different sites
remain separate. For one reference site, the selected call chain has the
minimum number of call edges. Equal-length chains are compared by the ordered
sequence of `(code_section_index, call_byte_offset)` pairs. The final map
instruction is attached after the call path; a direct entry reference has an
empty call path. Graph traversal uses explicit queues and terminates on cycles.

Functions, calls, references, helpers, and provenance records are sorted by
their physical symbol identities first and then by physical section/offset
sites; names never participate in identity or ordering.

## Example program

Build/run:

```sh
c++ -std=c++17 -O1 -I. \
  examples/bpf_graph/bpf_graph.cpp -o bpf_graph
./bpf_graph path/to/object.o output-prefix
```

The example treats every defined global `STT_FUNC` as an entry and passes the
section named `maps` to the analyzer. It writes `output-prefix.tsv` and
`output-prefix.dot`; both are generated solely from `bpf_analysis_result`.

## TSV format

The first column is the record tag: `F` function, `C` call, `M` map reference,
`H` helper, and `W` entry provenance/witness path.

A missing optional relocation is rendered as four `-` fields. Names containing
tabs or line breaks have those characters replaced by spaces.

### `F` — function

| Column | Meaning |
|---:|---|
| 1 | `F` |
| 2 | function symbol identity `table:ordinal` |
| 3 | copied symbol name (display only) |
| 4 | code section index |
| 5 | `st_value` / section byte offset |
| 6 | `st_size` |
| 7 | original starting slot index |
| 8 | physical file offset of function start |

### `C` — subprogram call

| Column | Meaning |
|---:|---|
| 1 | `C` |
| 2 | caller symbol identity |
| 3 | callee function symbol identity |
| 4 | callee name |
| 5 | resolved target section |
| 6 | resolved target byte offset |
| 7 | call instruction code section |
| 8 | call byte offset in that section |
| 9 | call slot index |
| 10 | physical file offset of call instruction |
| 11 | REL section index, or `-` |
| 12 | REL entry ordinal, or `-` |
| 13 | physical REL entry file offset, or `-` |
| 14 | original REL symbol identity, or `-` |

### `M` — map reference

| Column | Meaning |
|---:|---|
| 1 | `M` |
| 2 | owning function symbol identity |
| 3 | resolved map symbol identity |
| 4 | map name |
| 5 | map definition section |
| 6 | map definition offset |
| 7 | map size |
| 8 | physical file offset of map definition |
| 9 | `lddw` first-slot code section |
| 10 | `lddw` byte offset |
| 11 | first-slot index |
| 12 | physical file offset of `lddw` |
| 13 | REL section index |
| 14 | REL entry ordinal |
| 15 | physical REL entry file offset |
| 16 | original REL symbol identity (may be a `STT_SECTION` symbol) |

### `H` — helper call

| Column | Meaning |
|---:|---|
| 1 | `H` |
| 2 | owning function symbol identity |
| 3 | helper ID from `imm` |
| 4–7 | call instruction section, byte offset, slot, and file offset |
| 8–11 | REL fields; normally all `-` because helpers have no REL |

### `W` — entry-to-map-source provenance

| Column | Meaning |
|---:|---|
| 1 | `W` |
| 2 | requested entry symbol identity |
| 3 | resolved map symbol identity |
| 4 | map name |
| 5 | number of call edges before the map reference |
| 6 | encoded call path, or `-` for a direct reference |
| 7–10 | final map-reference `lddw` section, byte offset, slot, and file offset |
| 11–14 | final map REL section/entry/file offset/original symbol identity |

Column 6 encodes zero or more semicolon-separated steps:

```text
caller-id->callee-id@code-section:byte-offset/slot=N/foff=N[/rel=rel-section:entry/rfoff=N/rsym=table:ordinal]
```

For example:

```text
6:11->6:5@3:152/slot=19/foff=416/rel=7:0/rfoff=824/rsym=6:5;6:5->6:6@2:24/slot=3/foff=160
```

The lexicographic tie-break uses only the `code-section:byte-offset` sequence
in this path.

## DOT format

The DOT output contains:

- function nodes with IDs `"F:table:ordinal"`;
- map nodes with IDs `"M:table:ordinal"`;
- function-to-function edges for `C` records;
- function-to-map edges for `M` records.

Node labels are only the copied symbol name. Because the DOT node identifier
contains the physical symbol identity, two different `dup` local functions in
different sections produce two separate nodes even though both labels are
`dup`. Edge labels show the call/reference site as `C section:offset` or
`M section:offset`. Helper records are currently represented in the TSV only;
they are not drawn as subprogram nodes.
