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
#include <string>
#include <vector>

#include <elfio/elfio.hpp>
#include <elfio/elfio_group_view.hpp>

namespace ELFIO {

// Kinds of direct section relationships followed by the relation query.
enum class relation_kind
{
    group_contains,     // an SHT_GROUP section lists a member
    relocation_target,  // an SHT_REL/SHT_RELA section points at sh_info
    link_order,         // an SHF_LINK_ORDER section points at sh_link
};

struct section_relation
{
    relation_kind kind;
    Elf_Half      from;
    Elf_Half      to;
};

struct relation_input_error
{
    Elf_Half      requested_index; // physical index taken from the request
    std::string   message;
};

struct section_relation_view
{
    bool                      valid = true;
    std::vector<relation_input_error> errors;

    std::vector<Elf_Half>           requested; // original request, sorted, unique
    std::vector<Elf_Half>           sections; // closure, physical index order
    std::vector<section_relation>   relations; // every direct edge in the closure
};

namespace detail {

inline const char* relation_kind_text( relation_kind kind )
{
    switch ( kind ) {
    case relation_kind::group_contains:
        return "group";
    case relation_kind::relocation_target:
        return "relocation";
    case relation_kind::link_order:
        return "link_order";
    }
    return "unknown";
}

} // namespace detail

//------------------------------------------------------------------------------
//! Computes the read-only relationship closure for a set of physical section
//! indices. Rules:
//!  - selecting an SHT_GROUP section or any of its members pulls in the whole
//!    group (the group section plus every member);
//!  - when a target section is in the result, every SHT_REL/SHT_RELA section
//!    whose sh_info points at it is pulled in, and selecting such a relocation
//!    section pulls its sh_info target in;
//!  - SHF_LINK_ORDER sections are linked to their sh_link target in both
//!    directions;
//!  - plain sh_link references (for example a symbol table's string table)
//!    are never followed;
//!  - the rules are applied repeatedly until the result stops growing, so
//!    legitimate relationship cycles converge.
//! Invalid request indices make the result invalid; no partial closure is
//! returned in that case.
inline section_relation_view
make_section_relations( const elfio&              elf_file,
                        std::vector<Elf_Half>     requested_indices )
{
    section_relation_view result;

    const Elf_Half section_count = elf_file.sections.size();

    // Normalise the request and validate every index first.
    std::sort( requested_indices.begin(), requested_indices.end() );
    requested_indices.erase(
        std::unique( requested_indices.begin(), requested_indices.end() ),
        requested_indices.end() );
    result.requested = requested_indices;

    for ( Elf_Half idx : requested_indices ) {
        if ( idx >= section_count ) {
            result.valid = false;
            result.errors.push_back(
                { idx, "requested section index is out of range" } );
        }
    }
    if ( !result.valid ) {
        result.requested.clear();
        result.sections.clear();
        result.relations.clear();
        return result;
    }

    const group_view groups = make_group_view( elf_file );

    // group_of[i] is the SHT_GROUP section listing section i, if any.
    std::vector<Elf_Half> group_of( section_count, Elf_Half( 0xFFFF ) );
    for ( const auto& group : groups.groups ) {
        for ( Elf_Half member : group.members ) {
            if ( member < section_count ) {
                group_of[member] = group.section_index;
            }
        }
    }

    std::vector<bool> in_result( section_count, false );
    std::vector<Elf_Half> worklist;

    auto add = [&]( Elf_Half index ) {
        if ( index < section_count && !in_result[index] ) {
            in_result[index] = true;
            worklist.push_back( index );
        }
    };

    for ( Elf_Half idx : requested_indices ) {
        add( idx );
    }

    while ( !worklist.empty() ) {
        const Elf_Half current = worklist.back();
        worklist.pop_back();
        const section* sec = elf_file.sections[current];
        if ( sec == nullptr ) {
            continue;
        }

        // Group membership, in both directions: selecting the group itself
        // or any member expands the whole group.
        if ( sec->get_type() == SHT_GROUP ) {
            add( current );
            for ( const auto& group : groups.groups ) {
                if ( group.section_index == current ) {
                    for ( Elf_Half member : group.members ) {
                        add( member );
                    }
                    break;
                }
            }
        }
        if ( group_of[current] != Elf_Half( 0xFFFF ) ) {
            const Elf_Half group_section = group_of[current];
            add( group_section );
            for ( const auto& group : groups.groups ) {
                if ( group.section_index == group_section ) {
                    for ( Elf_Half member : group.members ) {
                        add( member );
                    }
                    break;
                }
            }
        }

        // Relocation sections: sh_info names the relocated target.
        if ( sec->get_type() == SHT_REL || sec->get_type() == SHT_RELA ) {
            const Elf_Word target = sec->get_info();
            if ( target < section_count ) {
                add( static_cast<Elf_Half>( target ) );
            }
        }
        for ( Elf_Half i = 0; i < section_count; ++i ) {
            const section* other = elf_file.sections[i];
            if ( other != nullptr &&
                 ( other->get_type() == SHT_REL ||
                   other->get_type() == SHT_RELA ) &&
                 other->get_info() == current ) {
                add( i );
            }
        }

        // SHF_LINK_ORDER sections: sh_link names the ordering target.
        if ( ( sec->get_flags() & SHF_LINK_ORDER ) != 0 ) {
            const Elf_Word target = sec->get_link();
            if ( target != 0 && target < section_count ) {
                add( static_cast<Elf_Half>( target ) );
            }
        }
        for ( Elf_Half i = 0; i < section_count; ++i ) {
            const section* other = elf_file.sections[i];
            if ( other != nullptr &&
                 ( other->get_flags() & SHF_LINK_ORDER ) != 0 &&
                 other->get_link() != 0 && other->get_link() == current ) {
                add( i );
            }
        }
    }

    for ( Elf_Half i = 0; i < section_count; ++i ) {
        if ( in_result[i] ) {
            result.sections.push_back( i );
        }
    }

    // Materialise every direct edge whose two endpoints are in the closure,
    // scanning in physical index order so the evidence is stable. Distinct
    // paths reaching the same section keep their own edge records.
    for ( const auto& group : groups.groups ) {
        if ( !in_result[group.section_index] ) {
            continue;
        }
        for ( Elf_Half member : group.members ) {
            if ( in_result[member] ) {
                result.relations.push_back(
                    { relation_kind::group_contains, group.section_index,
                      member } );
            }
        }
    }
    for ( Elf_Half i = 0; i < section_count; ++i ) {
        const section* sec = elf_file.sections[i];
        if ( sec == nullptr || !in_result[i] ) {
            continue;
        }
        if ( sec->get_type() == SHT_REL || sec->get_type() == SHT_RELA ) {
            const Elf_Word target = sec->get_info();
            if ( target < section_count &&
                 in_result[static_cast<Elf_Half>( target )] ) {
                result.relations.push_back(
                    { relation_kind::relocation_target, i,
                      static_cast<Elf_Half>( target ) } );
            }
        }
        if ( ( sec->get_flags() & SHF_LINK_ORDER ) != 0 ) {
            const Elf_Word target = sec->get_link();
            if ( target != 0 && target < section_count &&
                 in_result[static_cast<Elf_Half>( target )] ) {
                result.relations.push_back(
                    { relation_kind::link_order, i,
                      static_cast<Elf_Half>( target ) } );
            }
        }
    }

    return result;
}

} // namespace ELFIO

#endif // ELFIO_SECTION_LINKS_HPP
