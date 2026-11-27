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

#ifndef ELFIO_GROUP_VIEW_HPP
#define ELFIO_GROUP_VIEW_HPP

#include <cstring>
#include <string>
#include <vector>

#include <elfio/elfio.hpp>

namespace ELFIO {

// Problems that can be reported while interpreting an SHT_GROUP section.
enum class group_problem_kind
{
    malformed_data,             // size is not a whole number of Elf_Word words
    empty_group,                // the group carries the flag word only
    zero_member,                // a member word is SHN_UNDEF (0)
    member_out_of_range,        // a member word does not name a real section
    duplicate_member,           // the same member word appears more than once
    member_without_group_flag,  // member section does not carry SHF_GROUP
    member_in_multiple_groups,  // member section is listed by another group too
    invalid_symbol_table,       // sh_link does not name a symbol table
    invalid_symbol_index,       // sh_info is outside the symbol table
    invalid_string_table,       // symbol table's sh_link does not name a strtab
};

struct group_problem
{
    group_problem_kind kind;
    Elf_Half  group_section;                  // physical index of the group
    Elf_Xword word_offset;                    // byte offset in group data,
                                              // or (Elf_Xword)-1 for sh_link/sh_info
    Elf_Word  word_value;                     // raw offending word / field value
    Elf_Half  other_section;                  // second group, for multi-membership
};

// The signature is a triple of identities: the symbol table named by the
// group's sh_link, the symbol named by sh_info inside that table, and the
// string table reached through the symbol table's own sh_link. Symbol tables
// sharing a name are never merged: the indices stay part of the identity.
struct group_signature_info
{
    bool        resolved = false;
    Elf_Half    symbol_table = 0;
    Elf_Word    symbol_index = 0;
    Elf_Half    string_table = 0;
    std::string name;                        // may legitimately be empty
};

struct group_description
{
    Elf_Half  section_index = 0;
    std::string section_name;

    Elf_Word flags = 0;                      // raw flag word, preserved as is
    bool     comdat = false;                 // GRP_COMDAT bit
    bool     flags_fully_interpreted = true; // false if any other bit is set
    std::vector<Elf_Word> unexplained_flags; // each unknown set bit

    std::vector<Elf_Half> members; // physical member indices, original order
    group_signature_info  signature;
    std::vector<group_problem> problems;
};

struct group_view
{
    std::vector<group_description> groups; // in physical section order
    std::vector<Elf_Half> dangling_group_sections; // SHF_GROUP, never a member
    std::vector<group_problem> problems;           // every problem above

    bool valid() const
    {
        return problems.empty() && dangling_group_sections.empty();
    }
};

namespace detail {

inline const char* group_problem_text( group_problem_kind kind )
{
    switch ( kind ) {
    case group_problem_kind::malformed_data:
        return "malformed group data";
    case group_problem_kind::empty_group:
        return "empty group";
    case group_problem_kind::zero_member:
        return "zero member";
    case group_problem_kind::member_out_of_range:
        return "member out of range";
    case group_problem_kind::duplicate_member:
        return "duplicate member";
    case group_problem_kind::member_without_group_flag:
        return "member lacks SHF_GROUP";
    case group_problem_kind::member_in_multiple_groups:
        return "member belongs to multiple groups";
    case group_problem_kind::invalid_symbol_table:
        return "sh_link does not name a symbol table";
    case group_problem_kind::invalid_symbol_index:
        return "sh_info is outside the symbol table";
    case group_problem_kind::invalid_string_table:
        return "symbol table sh_link does not name a string table";
    }
    return "unknown group problem";
}

} // namespace detail

//------------------------------------------------------------------------------
//! Builds a read-only interpretation of every SHT_GROUP section. The view
//! never selects a symbol provider, simulates linking or modifies the file.
inline group_view make_group_view( const elfio& elf_file )
{
    group_view view;

    const Elf_Half section_count = elf_file.sections.size();

    // First physical group that lists every section, for multi-membership
    // and dangling SHF_GROUP detection.
    std::vector<Elf_Half> listed_by( section_count, Elf_Half( 0xFFFF ) );

    for ( Elf_Half gi = 0; gi < section_count; ++gi ) {
        const section* gsec = elf_file.sections[gi];
        if ( gsec == nullptr || gsec->get_type() != SHT_GROUP ) {
            continue;
        }

        group_description desc;
        desc.section_index = gi;
        desc.section_name  = gsec->get_name();

        const char*      data = gsec->get_data();
        const Elf_Xword  size = gsec->get_size();
        const Elf_Xword  word_count = size / sizeof( Elf_Word );

        if ( data == nullptr || size == 0 ||
             size % sizeof( Elf_Word ) != 0 ) {
            desc.problems.push_back(
                { group_problem_kind::malformed_data, gi, Elf_Xword( -1 ),
                  Elf_Word( size ), 0 } );
        }

        if ( word_count >= 1 ) {
            Elf_Word raw_flags = 0;
            std::memcpy( &raw_flags, data, sizeof( Elf_Word ) );
            raw_flags = elf_file.get_convertor()( raw_flags );

            desc.flags  = raw_flags;
            desc.comdat = ( raw_flags & GRP_COMDAT ) != 0;

            Elf_Word explained = GRP_COMDAT;
            for ( unsigned bit = 0; bit < 32; ++bit ) {
                Elf_Word mask = Elf_Word( 1 ) << bit;
                if ( ( raw_flags & mask ) != 0 &&
                     ( explained & mask ) == 0 ) {
                    desc.unexplained_flags.push_back( mask );
                }
            }
            desc.flags_fully_interpreted =
                desc.unexplained_flags.empty();
        }

        if ( word_count <= 1 ) {
            if ( size != 0 && size % sizeof( Elf_Word ) == 0 ) {
                desc.problems.push_back(
                    { group_problem_kind::empty_group, gi,
                      sizeof( Elf_Word ), 0, 0 } );
            }
        }

        std::vector<bool> seen( section_count, false );
        for ( Elf_Xword wi = 1; wi < word_count; ++wi ) {
            const Elf_Xword offset = wi * sizeof( Elf_Word );

            Elf_Word raw_member = 0;
            std::memcpy( &raw_member, data + offset, sizeof( Elf_Word ) );
            raw_member = elf_file.get_convertor()( raw_member );

            if ( raw_member == SHN_UNDEF ) {
                desc.problems.push_back(
                    { group_problem_kind::zero_member, gi, offset,
                      raw_member, 0 } );
                continue;
            }
            if ( raw_member >= section_count ) {
                desc.problems.push_back(
                    { group_problem_kind::member_out_of_range, gi, offset,
                      raw_member, 0 } );
                continue;
            }

            const Elf_Half member = static_cast<Elf_Half>( raw_member );
            const section* msec   = elf_file.sections[member];

            if ( seen[member] ) {
                desc.problems.push_back(
                    { group_problem_kind::duplicate_member, gi, offset,
                      raw_member, 0 } );
                continue;
            }
            seen[member] = true;

            if ( msec == nullptr ||
                 ( msec->get_flags() & SHF_GROUP ) == 0 ) {
                desc.problems.push_back(
                    { group_problem_kind::member_without_group_flag, gi,
                      offset, raw_member, 0 } );
            }
            if ( listed_by[member] != Elf_Half( 0xFFFF ) ) {
                desc.problems.push_back(
                    { group_problem_kind::member_in_multiple_groups, gi,
                      offset, raw_member, listed_by[member] } );
            }
            else {
                listed_by[member] = gi;
            }

            desc.members.push_back( member );
        }

        // Signature chain: group sh_link -> symbol table, sh_info -> symbol,
        // symbol table sh_link -> string table.
        const Elf_Word link = gsec->get_link();
        const Elf_Word info = gsec->get_info();

        desc.signature.symbol_table = static_cast<Elf_Half>( link );
        desc.signature.symbol_index = info;

        const section* symsec =
            link < section_count ? elf_file.sections[link] : nullptr;
        bool symtab_ok =
            symsec != nullptr &&
            ( symsec->get_type() == SHT_SYMTAB ||
              symsec->get_type() == SHT_DYNSYM );

        if ( !symtab_ok ) {
            desc.problems.push_back(
                { group_problem_kind::invalid_symbol_table, gi,
                  Elf_Xword( -1 ), link, 0 } );
        }
        else {
            const Elf_Word str_link = symsec->get_link();
            const section* strsec =
                str_link < section_count ? elf_file.sections[str_link]
                                         : nullptr;
            if ( strsec == nullptr || strsec->get_type() != SHT_STRTAB ) {
                desc.problems.push_back(
                    { group_problem_kind::invalid_string_table, gi,
                      Elf_Xword( -1 ), str_link, 0 } );
            }
            else {
                desc.signature.string_table = static_cast<Elf_Half>( str_link );
            }

            const_symbol_section_accessor symbols( elf_file, symsec );
            if ( info >= symbols.get_symbols_num() ) {
                desc.problems.push_back(
                    { group_problem_kind::invalid_symbol_index, gi,
                      Elf_Xword( -1 ), info, 0 } );
            }
            else {
                std::string   name;
                Elf64_Addr    value = 0;
                Elf_Xword     sym_size = 0;
                unsigned char bind = 0, type = 0, other = 0;
                Elf_Half      shndx = 0;
                if ( symbols.get_symbol( info, name, value, sym_size, bind,
                                         type, shndx, other ) ) {
                    desc.signature.name = name;
                }
                desc.signature.resolved =
                    strsec != nullptr && strsec->get_type() == SHT_STRTAB;
            }
        }

        for ( const auto& p : desc.problems ) {
            view.problems.push_back( p );
        }
        view.groups.push_back( std::move( desc ) );
    }

    // Sections carrying SHF_GROUP that no SHT_GROUP section claims.
    for ( Elf_Half i = 0; i < section_count; ++i ) {
        const section* sec = elf_file.sections[i];
        if ( sec != nullptr && ( sec->get_flags() & SHF_GROUP ) != 0 &&
             listed_by[i] == Elf_Half( 0xFFFF ) ) {
            view.dangling_group_sections.push_back( i );
        }
    }

    return view;
}

} // namespace ELFIO

#endif // ELFIO_GROUP_VIEW_HPP
