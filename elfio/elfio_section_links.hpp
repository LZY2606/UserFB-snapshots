/*
Copyright (C) 2001-present by Serge Lamikhov-Center

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#ifndef ELFIO_SECTION_LINKS_HPP
#define ELFIO_SECTION_LINKS_HPP

#include <algorithm>
#include <queue>
#include <string>
#include <vector>

#include <elfio/elfio.hpp>
#include <elfio/elfio_group_view.hpp>

namespace ELFIO {

// Kinds of direct section-to-section relationships understood by the
// read-only relation view. Plain sh_link references (for example a symbol
// table's string table) are deliberately not represented here.
enum class section_relation_kind
{
    group_member,      // SHT_GROUP section 'from' lists member section 'to'
    relocation_target, // REL/RELA section 'from'; sh_info names target 'to'
    link_order         // SHF_LINK_ORDER section 'from'; sh_link names target 'to'
};

struct section_relation
{
    section_relation_kind kind = section_relation_kind::group_member;
    Elf_Half              from = SHN_UNDEF;
    Elf_Half              to   = SHN_UNDEF;
};

struct section_relation_result
{
    bool                  ok = false;
    std::string           error;          // Populated when ok == false
    std::vector<Elf_Half> requested;      // Original request set, sorted
    std::vector<Elf_Half> sections;       // Reached sections, physical order
    std::vector<section_relation> relations; // Direct edges inside the result
    group_relation_view           group_view;  // Full group interpretation
};

namespace detail {

inline const char* relation_kind_name( section_relation_kind kind )
{
    switch ( kind ) {
    case section_relation_kind::group_member:
        return "group";
    case section_relation_kind::relocation_target:
        return "relocation";
    case section_relation_kind::link_order:
        return "link_order";
    }
    return "unknown";
}

} // namespace detail

// Expand a set of physical section indices until no further section can be
// reached. The expansion rules are:
//   * selecting an SHT_GROUP section or any of its members pulls in the group
//     section together with every member;
//   * a reached target section pulls in the REL/RELA sections whose sh_info
//     points at it, and selecting such a relocation section pulls in the
//     target;
//   * a reached section pulls in SHF_LINK_ORDER sections whose sh_link points
//     at it, and selecting such a section pulls in its target.
// The rules are applied in both directions and iterated to a fixpoint, so
// legitimate relationship cycles terminate. Plain sh_link edges are never
// followed. Every direct relation between two reached sections is retained,
// including when the same section is reached by several different paths.
inline section_relation_result
compute_section_relations( const elfio&                 reader,
                           const std::vector<Elf_Half>& requested_indices )
{
    section_relation_result result;
    result.group_view = build_group_relation_view( reader );

    const Elf_Half section_count = reader.sections.size();

    // Validate the request before producing any partial answer.
    for ( Elf_Half index : requested_indices ) {
        if ( index >= section_count ) {
            result.ok    = false;
            result.error = "requested section index " +
                           std::to_string( index ) + " is out of range (0.." +
                           std::to_string( section_count - 1 ) + ")";
            result.requested.clear();
            result.sections.clear();
            result.relations.clear();
            return result;
        }
    }

    result.requested = requested_indices;
    std::sort( result.requested.begin(), result.requested.end() );
    result.requested.erase(
        std::unique( result.requested.begin(), result.requested.end() ),
        result.requested.end() );

    // Build the undirected graph of the relationships we are allowed to
    // follow. Edges are stored once, from the referencing side to its target;
    // traversal treats them as undirected.
    std::vector<section_relation> edges;

    for ( const auto& group : result.group_view.groups ) {
        for ( const auto& member : group.members ) {
            if ( member.in_range ) {
                edges.push_back(
                    { section_relation_kind::group_member,
                      group.group_section_index,
                      static_cast<Elf_Half>( member.section_index ) } );
            }
        }
    }

    for ( Elf_Half i = 0; i < section_count; ++i ) {
        const section* sec = reader.sections[i];
        if ( sec == nullptr ) {
            continue;
        }

        const Elf_Word type = sec->get_type();
        if ( type == SHT_REL || type == SHT_RELA ) {
            const Elf_Word target = sec->get_info();
            if ( target < section_count ) {
                edges.push_back(
                    { section_relation_kind::relocation_target, i,
                      static_cast<Elf_Half>( target ) } );
            }
        }

        if ( ( sec->get_flags() & SHF_LINK_ORDER ) != 0 ) {
            const Elf_Word target = sec->get_link();
            if ( target < section_count ) {
                edges.push_back(
                    { section_relation_kind::link_order, i,
                      static_cast<Elf_Half>( target ) } );
            }
        }
    }

    std::vector<std::vector<Elf_Half>> adjacency( section_count );
    for ( const auto& edge : edges ) {
        adjacency[edge.from].push_back( edge.to );
        adjacency[edge.to].push_back( edge.from );
    }

    std::vector<char> reached( section_count, false );
    std::queue<Elf_Half> worklist;
    for ( Elf_Half index : result.requested ) {
        if ( !reached[index] ) {
            reached[index] = true;
            worklist.push( index );
        }
    }

    while ( !worklist.empty() ) {
        const Elf_Half current = worklist.front();
        worklist.pop();
        for ( Elf_Half neighbour : adjacency[current] ) {
            if ( !reached[neighbour] ) {
                reached[neighbour] = true;
                worklist.push( neighbour );
            }
        }
    }

    for ( Elf_Half i = 0; i < section_count; ++i ) {
        if ( reached[i] ) {
            result.sections.push_back( i );
        }
    }

    // Keep the evidence of every direct relationship whose two ends are part
    // of the answer; distinct paths therefore survive as distinct edges.
    for ( const auto& edge : edges ) {
        if ( reached[edge.from] && reached[edge.to] ) {
            result.relations.push_back( edge );
        }
    }

    std::sort( result.relations.begin(), result.relations.end(),
               []( const section_relation& a, const section_relation& b ) {
                   if ( a.from != b.from ) return a.from < b.from;
                   if ( a.to != b.to ) return a.to < b.to;
                   return static_cast<int>( a.kind ) <
                          static_cast<int>( b.kind );
               } );

    result.ok = true;
    return result;
}

} // namespace ELFIO

#endif // ELFIO_SECTION_LINKS_HPP
