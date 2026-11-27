# Section group and relation view

The group and relation view is a read-only analysis layer on top of an
already loaded `ELFIO::elfio` object. It never selects a symbol provider,
simulates linking or modifies the target file. All public entry points take
a `const elfio&`, and the two headers are pulled in automatically by
`elfio/elfio.hpp`:

```cpp
#include <elfio/elfio.hpp>
#include <elfio/elfio_dump.hpp>

const ELFIO::group_view view = ELFIO::make_group_view( reader );

const ELFIO::section_relation_view relations =
    ELFIO::make_section_relations( reader, { text_index } );

ELFIO::dump::section_relations( std::cout, reader, { text_index } );
```

The implementation works for ELF32/ELF64 and both byte orders.

## Group interpretation (`make_group_view`)

Every `SHT_GROUP` section is interpreted independently and reported in
physical section order. Each `group_description` keeps:

- the raw flag word, the decoded `GRP_COMDAT` bit and every bit that has no
  known meaning. Unknown flags are preserved as values, never folded into a
  boolean;
- the members in their original on-disk order, stored as physical section
  indices;
- the signature identity as a triple:
  - `sh_link` of the group names the group's own symbol table,
  - `sh_info` is the symbol index inside that table,
  - the symbol table's own `sh_link` names the string table used to read the
    symbol name.

Two symbol tables that share a section name are never mixed: the symbol
table index stays part of the signature, so equal signature text on
different groups does not merge them. The signature does not have to be a
function, and a section symbol with an empty name is valid. Empty groups,
duplicate section names and repeated signature text are all kept distinct.

Structural faults are collected in `group_view::problems` together with the
group section index and the relevant byte offset (or a marker when the fault
is in `sh_link`/`sh_info` rather than in the group payload):

- malformed payload (size not divisible by `Elf_Word`), empty group,
  zero/undefined member, member out of range, duplicate member, member
  missing `SHF_GROUP`, member claimed by two groups;
- `sh_link` not naming a symbol table, `sh_info` outside the symbol table,
  symbol table `sh_link` not naming a string table.

Sections carrying `SHF_GROUP` that no group section lists are reported in
`dangling_group_sections`.

## Relation closure (`make_section_relations`)

The input is a set of physical section indices. The request is normalised
(sorted, de-duplicated) and echoed back in the result. The following rules
are applied repeatedly until the result stops growing:

1. **Groups.** Selecting an `SHT_GROUP` section itself, or any of its
   members, pulls the whole group in: the group section plus every member.
   Group membership and generic section relations are represented
   separately.
2. **Relocations.** When a target section is in the result, every
   `SHT_REL`/`SHT_RELA` section whose `sh_info` points at it joins the
   result. Selecting the relocation section directly pulls its `sh_info`
   target in, so the relation works in both directions. A relocation section
   pulled in through a group can therefore bring in another target.
3. **Link ordering.** A section carrying `SHF_LINK_ORDER` is related to its
   `sh_link` target, again in both directions.
4. **Plain `sh_link` is not followed.** In particular, a symbol table's
   string table is never treated as a code dependency.

Legitimate relation cycles (for example two `SHF_LINK_ORDER` sections
pointing at each other) converge naturally. The closure is emitted in
stable physical index order. Every direct relation whose endpoints are both
in the closure is listed with its kind and both endpoints, so multiple
paths reaching the same section keep all their evidence instead of being
collapsed.

If any requested index is out of range, `valid` is false, the offending
indices are reported in `errors`, and no seemingly-complete partial
closure is returned: the section and relation lists are empty.

## Dump output

`ELFIO::dump::section_relations` prints the request, the closure and every
relation, followed by the identities of the groups that participate in the
closure. The no-argument overload prints every group and relation in the
file. The existing `dump::section_headers`, symbol accessors and their
wording are unchanged.
