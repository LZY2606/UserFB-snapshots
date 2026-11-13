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

#include <limits>
#include <string>
#include <vector>

#include <elfio/elfio.hpp>

namespace ELFIO {

// Interpretation of a single word found in an SHT_GROUP section.
struct group_member_info
{
    Elf_Word  section_index = 0; // Raw word value: referenced section index
    Elf_Xword word_offset   = 0; // Byte offset of the word in the group section
    bool      in_range      = false;
    bool      has_group_flag = false;
    bool      duplicate     = false; // Same section index seen earlier in group
};

// The signature is identified by the triple (group's sh_link symbol table,
// group's sh_info symbol index, symbol table's sh_link string table). Two
// symbol tables sharing a name are still distinct identities.
struct group_signature_info
{
    Elf_Half    symbol_table_index = SHN_UNDEF;
    Elf_Word    symbol_index       = 0;
    Elf_Half    string_table_index = SHN_UNDEF;
    bool        resolved           = false;
    std::string name; // Signature text; empty is legal (e.g. section symbol)
    unsigned char bind    = 0;
    unsigned char type    = 0;
    Elf_Half      shndx   = SHN_UNDEF;
};

// Flags are kept as the raw word, not collapsed into a boolean: GRP_COMDAT is
// interpreted, every other set bit is preserved as an uninterpreted value.
struct group_flag_info
{
    Elf_Word              raw = 0;
    bool                  comdat = false;
    std::vector<Elf_Word> uninterpreted_bits;
};

enum class group_error_kind
{
    malformed_group_data,     // Size is not a whole number of Elf_Word words
    empty_group,              // No member words after the flag word
    member_out_of_range,
    duplicate_member,
    member_missing_group_flag,
    member_in_multiple_groups,
    invalid_group_link,       // sh_link does not name a symbol table
    invalid_group_info,       // sh_info is not a valid symbol index
    invalid_signature_string_table,
    dangling_group_flag       // SHF_GROUP section referenced by no SHT_GROUP
};

struct group_error
{
    group_error_kind kind = group_error_kind::empty_group;
    Elf_Half         group_section_index = SHN_UNDEF; // SHN_UNDEF: not group specific
    Elf_Xword        word_offset = std::numeric_limits<Elf_Xword>::max();
    Elf_Word         value       = 0; // Relevant raw word, when applicable
    std::string      message;
};

struct group_info
{
    Elf_Half              group_section_index = SHN_UNDEF;
    group_flag_info       flags;
    group_signature_info  signature;
    std::vector<group_member_info> members; // Original on-disk order
};

struct group_relation_view
{
    std::vector<group_info>  groups;       // Sorted by group section index
    std::vector<group_error> errors;       // Every structural problem found
    std::vector<Elf_Half>    dangling_sections; // SHF_GROUP sections with no group

    // Owning group section index for every section claimed by at least one
    // group. Indexed by physical section index; SHN_UNDEF means unclaimed.
    std::vector<Elf_Half> group_owner;
};

namespace detail {

inline std::string group_error_text( group_error_kind kind )
{
    switch ( kind ) {
    case group_error_kind::malformed_group_data:
        return "group data size is not a multiple of Elf_Word";
    case group_error_kind::empty_group:
        return "group has no member sections";
    case group_error_kind::member_out_of_range:
        return "group member section index is out of range";
    case group_error_kind::duplicate_member:
        return "group member section index appears more than once";
    case group_error_kind::member_missing_group_flag:
        return "group member section does not carry SHF_GROUP";
    case group_error_kind::member_in_multiple_groups:
        return "section is a member of more than one group";
    case group_error_kind::invalid_group_link:
        return "group sh_link does not point to a symbol table";
    case group_error_kind::invalid_group_info:
        return "group sh_info is not a valid symbol table index";
    case group_error_kind::invalid_signature_string_table:
        return "signature symbol table sh_link does not point to a string table";
    case group_error_kind::dangling_group_flag:
        return "section carries SHF_GROUP but is not referenced by any group";
    }
    return "unknown group error";
}

} // namespace detail

// Read-only interpretation of every SHT_GROUP section in the file. The
// reader is never modified: no symbol provider is chosen, no linking is
// simulated and no section is rewritten.
inline group_relation_view build_group_relation_view( const elfio& reader )
{
    group_relation_view view;
    const Elf_Half      section_count = reader.sections.size();
    view.group_owner.assign( section_count, SHN_UNDEF );

    const endianess_convertor& convertor = reader.get_convertor();

    for ( Elf_Half gi = 0; gi < section_count; ++gi ) {
        const section* gsec = reader.sections[gi];
        if ( gsec == nullptr || gsec->get_type() != SHT_GROUP ) {
            continue;
        }

        group_info group;
        group.group_section_index = gi;

        const Elf_Xword data_size = gsec->get_size();
        const char*     data      = gsec->get_data();

        if ( data_size % sizeof( Elf_Word ) != 0 ) {
            view.errors.push_back(
                { group_error_kind::malformed_group_data, gi,
                  std::numeric_limits<Elf_Xword>::max(),
                  static_cast<Elf_Word>( data_size ),
                  detail::group_error_text(
                      group_error_kind::malformed_group_data ) } );
        }

        const Elf_Xword word_count = data_size / sizeof( Elf_Word );

        // First word: group flags.
        if ( word_count >= 1 && data != nullptr ) {
            const Elf_Word raw_flags =
                convertor( *reinterpret_cast<const Elf_Word*>( data ) );
            group.flags.raw = raw_flags;
            group.flags.comdat = ( raw_flags & GRP_COMDAT ) != 0;
            for ( unsigned bit = 0; bit < 32; ++bit ) {
                const Elf_Word bit_value = Elf_Word( 1 ) << bit;
                if ( ( raw_flags & bit_value ) && bit_value != GRP_COMDAT ) {
                    group.flags.uninterpreted_bits.push_back( bit_value );
                }
            }
        }

        if ( word_count <= 1 ) {
            view.errors.push_back(
                { group_error_kind::empty_group, gi,
                  std::numeric_limits<Elf_Xword>::max(), 0,
                  detail::group_error_text( group_error_kind::empty_group ) } );
        }

        // Remaining words: member section indices, kept in original order.
        for ( Elf_Xword w = 1; w < word_count; ++w ) {
            const Elf_Xword offset = w * sizeof( Elf_Word );
            if ( data == nullptr ) {
                view.errors.push_back(
                    { group_error_kind::malformed_group_data, gi, offset, 0,
                      "group section has no readable data" } );
                break;
            }
            const Elf_Word  member =
                convertor( *reinterpret_cast<const Elf_Word*>(
                    data + offset ) );

            group_member_info member_info;
            member_info.section_index = member;
            member_info.word_offset   = offset;
            member_info.in_range      = member < section_count;

            for ( const auto& earlier : group.members ) {
                if ( earlier.section_index == member ) {
                    member_info.duplicate = true;
                    break;
                }
            }

            if ( !member_info.in_range ) {
                view.errors.push_back(
                    { group_error_kind::member_out_of_range, gi, offset,
                      member,
                      detail::group_error_text(
                          group_error_kind::member_out_of_range ) } );
            }
            else {
                const section* msec = reader.sections[member];
                member_info.has_group_flag =
                    ( msec->get_flags() & SHF_GROUP ) != 0;

                if ( !member_info.has_group_flag ) {
                    view.errors.push_back(
                        { group_error_kind::member_missing_group_flag, gi,
                          offset, member,
                          detail::group_error_text(
                              group_error_kind::member_missing_group_flag ) } );
                }
                if ( member_info.duplicate ) {
                    view.errors.push_back(
                        { group_error_kind::duplicate_member, gi, offset,
                          member,
                          detail::group_error_text(
                              group_error_kind::duplicate_member ) } );
                }
                if ( view.group_owner[member] != SHN_UNDEF &&
                     view.group_owner[member] != gi ) {
                    view.errors.push_back(
                        { group_error_kind::member_in_multiple_groups, gi,
                          offset, member,
                          detail::group_error_text(
                              group_error_kind::member_in_multiple_groups ) } );
                }
                else {
                    view.group_owner[member] = gi;
                }
            }

            group.members.push_back( member_info );
        }

        // Signature: sh_link names the group's own symbol table, sh_info is
        // the symbol index, the symbol table's sh_link names the string
        // table. All three identities are retained.
        group.signature.symbol_table_index =
            static_cast<Elf_Half>( gsec->get_link() );
        group.signature.symbol_index = gsec->get_info();

        const section* symsec =
            reader.sections[group.signature.symbol_table_index];
        if ( symsec == nullptr ||
             ( symsec->get_type() != SHT_SYMTAB &&
               symsec->get_type() != SHT_DYNSYM ) ) {
            view.errors.push_back(
                { group_error_kind::invalid_group_link, gi,
                  std::numeric_limits<Elf_Xword>::max(),
                  gsec->get_link(),
                  detail::group_error_text(
                      group_error_kind::invalid_group_link ) } );
        }
        else {
            const Elf_Half strtab_index =
                static_cast<Elf_Half>( symsec->get_link() );
            group.signature.string_table_index = strtab_index;
            const section* strsec = reader.sections[strtab_index];
            if ( strsec == nullptr || strsec->get_type() != SHT_STRTAB ) {
                view.errors.push_back(
                    { group_error_kind::invalid_signature_string_table, gi,
                      std::numeric_limits<Elf_Xword>::max(), strtab_index,
                      detail::group_error_text(
                          group_error_kind::invalid_signature_string_table ) } );
            }
            else {
                const_symbol_section_accessor symbols( reader, symsec );
                if ( group.signature.symbol_index >=
                     symbols.get_symbols_num() ) {
                    view.errors.push_back(
                        { group_error_kind::invalid_group_info, gi,
                          std::numeric_limits<Elf_Xword>::max(),
                          group.signature.symbol_index,
                          detail::group_error_text(
                              group_error_kind::invalid_group_info ) } );
                }
                else {
                    std::string   name;
                    Elf64_Addr    value = 0;
                    Elf_Xword     size  = 0;
                    unsigned char bind = 0, type = 0, other = 0;
                    Elf_Half      shndx = SHN_UNDEF;
                    if ( symbols.get_symbol( group.signature.symbol_index, name,
                                             value, size, bind, type, shndx,
                                             other ) ) {
                        group.signature.resolved = true;
                        group.signature.name     = name;
                        group.signature.bind     = bind;
                        group.signature.type     = type;
                        group.signature.shndx    = shndx;
                    }
                }
            }
        }

        view.groups.push_back( std::move( group ) );
    }

    // SHF_GROUP sections that no SHT_GROUP section claims.
    for ( Elf_Half si = 0; si < section_count; ++si ) {
        const section* sec = reader.sections[si];
        if ( sec != nullptr && ( sec->get_flags() & SHF_GROUP ) &&
             view.group_owner[si] == SHN_UNDEF ) {
            view.dangling_sections.push_back( si );
            view.errors.push_back(
                { group_error_kind::dangling_group_flag, SHN_UNDEF,
                  std::numeric_limits<Elf_Xword>::max(), si,
                  detail::group_error_text(
                      group_error_kind::dangling_group_flag ) } );
        }
    }

    return view;
}

} // namespace ELFIO

#endif // ELFIO_GROUP_VIEW_HPP
