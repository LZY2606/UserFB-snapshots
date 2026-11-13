# Section group and relation view

Unlinked relocatable objects are more than a list of section names: a piece of
code can belong to an `SHT_GROUP`, carry relocations, and be referenced by
link-order sections. ELFIO provides two read-only views that make these
relationships explicit. Neither view chooses a symbol provider, simulates
linking, or modifies the object file.

* `elfio/elfio_group_view.hpp` — interpretation of every `SHT_GROUP` section.
* `elfio/elfio_section_links.hpp` — closure query over the sections related to
  a requested set of physical section indices.

Both are pulled in by `#include <elfio/elfio.hpp>` and take a `const elfio&`,
so the underlying reader is never altered.

## Group interpretation

`build_group_relation_view(reader)` returns a `group_relation_view`:

* one `group_info` per `SHT_GROUP`, ordered by physical section index;
* the flag word kept as its raw value. `GRP_COMDAT` is interpreted
  (`flags.comdat`); every other set bit is preserved in
  `flags.uninterpreted_bits` — the word is never treated as a boolean;
* members in their original on-disk order, each with its section index and the
  byte offset of the word inside the group section;
* the signature triple. The group's `sh_link` names its own symbol table,
  `sh_info` is the symbol index in that table, and the symbol table's
  `sh_link` names the string table. All three indices
  (`symbol_table_index`, `symbol_index`, `string_table_index`) are retained.
  Two symbol tables with the same name are still distinct identities, and
  groups with identical signature text are never merged;
* the signature does not have to name a function, and a section symbol with an
  empty name is accepted.

The view works identically for ELF32/ELF64 and little/big endian objects.

### Structural diagnostics

Group membership and relational edges are expressed separately. Nothing is
merged by name: empty groups, multiple sections sharing a name, and several
groups with the same signature text all keep their physical identities. Every
problem is recorded in `view.errors` with the group section index and, where
relevant, the byte offset and raw value of the offending word:

* group data whose size is not a whole number of `Elf_Word` words;
* empty groups (a flag word with no members);
* member indices out of range, duplicated, or pointing at a section without
  `SHF_GROUP`;
* one section claimed by two different groups;
* a group `sh_link` that does not name a symbol table, a `sh_info` that is not
  a valid symbol index, or a symbol table whose `sh_link` is not a string
  table;
* dangling `SHF_GROUP` sections that no group references (also listed in
  `view.dangling_sections`).

## Relation closure rules

`compute_section_relations(reader, requested)` takes a set of physical section
indices and expands it until no further section can be reached.

1. Selecting a group section itself, or any of its members, pulls the group
   and **all** of its members into the result.
2. When a target section is reached, every `SHT_REL`/`SHT_RELA` section whose
   `sh_info` points at it is pulled in; selecting such a relocation section
   pulls in its target.
3. Sections carrying `SHF_LINK_ORDER` follow the same rule through `sh_link`,
   in both directions.
4. The rules are applied repeatedly until the result stops growing, so a
   relocation section reached through a group can pull in yet another target,
   and legal relationship cycles terminate.
5. Plain `sh_link` references are **not** followed. In particular, a symbol
   table's string table is never treated as a code dependency.

The result contains:

* `requested` — the original request set, sorted and de-duplicated;
* `sections` — every reached section, stably ordered by physical index;
* `relations` — every direct edge (kind plus both endpoints) whose two ends
  are in the result. Reaching one section through several paths does not
  discard any edge, so all relationship evidence survives.

If any requested index is out of range, the call fails (`ok == false`) with an
error message and returns no partial result.

## Example

```cpp
#include <elfio/elfio.hpp>
#include <elfio/elfio_dump.hpp>
#include <fstream>
#include <vector>

using namespace ELFIO;

int main()
{
    elfio reader;
    if ( !reader.load( "firmware.o" ) ) {
        return 1;
    }

    // Interpret every SHT_GROUP, keeping symbol-table/string-table identities.
    group_relation_view groups = build_group_relation_view( reader );
    for ( const auto& error : groups.errors ) {
        // Localize with group section index and word offset.
    }
    for ( const auto& group : groups.groups ) {
        bool comdat = group.flags.comdat;
        const auto& signature = group.signature; // symtab/symbol/strtab triple
        for ( const auto& member : group.members ) {
            // member.section_index, member.word_offset
        }
    }

    // Expand from one physical section to its full related set.
    section_relation_result relations =
        compute_section_relations( reader, { groups.groups[0].group_section_index } );
    if ( !relations.ok ) {
        return 1;
    }
    for ( const auto& edge : relations.relations ) {
        // edge.kind: group_member / relocation_target / link_order
        // edge.from -> edge.to
    }

    // Human-readable rendering of the same identities and relationships.
    dump::section_relations( std::cout, relations, reader );
    return 0;
}
```
