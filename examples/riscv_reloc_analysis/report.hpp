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

#ifndef ELFIO_RISCV_RELOC_ANALYSIS_REPORT_HPP
#define ELFIO_RISCV_RELOC_ANALYSIS_REPORT_HPP

#include <cstdint>
#include <cstring>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

#include <elfio/elfio.hpp>

namespace ELFIO {
namespace riscv_example {

//------------------------------------------------------------------------------
//! \brief One decoded instruction of a report row.
//!
//! The referenced target is decoded back from the patched machine words, not
//! copied from the relocation plan.
//------------------------------------------------------------------------------
struct decoded_reference
{
    std::uint32_t word_offset    = 0;
    std::uint32_t original_word  = 0;
    std::uint32_t patched_word   = 0;
    std::uint32_t opcode         = 0;
    std::uint32_t funct3         = 0;
    std::uint32_t rd             = 0;
    std::uint32_t rs1            = 0;
    std::uint32_t rs2            = 0;
    std::int64_t  immediate      = 0;
    std::uint64_t instruction_pc = 0;
};

//------------------------------------------------------------------------------
//! \brief One TSV row, ready for stream output.
//------------------------------------------------------------------------------
struct reference_row
{
    std::string   kind;
    std::uint64_t rela_section        = 0;
    std::uint64_t rela_entry          = 0;
    std::uint64_t rela_file_offset    = 0;
    std::uint64_t symbol_table        = 0;
    std::uint64_t symbol_index        = 0;
    std::string   symbol_name;
    std::uint64_t symbol_file_offset  = 0;
    std::uint64_t symbol_address      = 0;
    std::int64_t  addend              = 0;
    std::uint64_t hi_rela_section     = 0;
    std::uint64_t hi_rela_entry       = 0;
    std::uint64_t hi_symbol_table     = 0;
    std::uint64_t hi_symbol_index     = 0;
    std::string   hi_symbol_name;
    std::uint64_t hi_symbol_address   = 0;
    std::int64_t  hi_addend           = 0;
    std::uint64_t hi_pc               = 0;
    std::uint64_t code_section        = 0;
    std::uint64_t code_offset         = 0;
    std::uint64_t code_file_offset    = 0;
    std::uint64_t pc                  = 0;
    std::vector<decoded_reference> instructions;
    std::uint64_t referenced_target   = 0;
};

//------------------------------------------------------------------------------
//! \brief A decoded hint row (RELAX).
//------------------------------------------------------------------------------
struct hint_row
{
    std::uint64_t rela_section     = 0;
    std::uint64_t rela_entry       = 0;
    std::uint64_t rela_file_offset = 0;
    std::uint64_t code_section     = 0;
    std::uint64_t code_offset      = 0;
    std::uint64_t code_file_offset = 0;
    std::uint64_t pc               = 0;
    std::uint64_t main_rela_section = 0;
    std::uint64_t main_rela_entry   = 0;
};

//------------------------------------------------------------------------------
//! \brief The complete analysis of one object under one section layout.
//------------------------------------------------------------------------------
struct analysis_report
{
    bool                          ok = false;
    std::string                   error;
    std::vector<reference_row>    rows;
    std::vector<hint_row>         hints;
};

namespace detail {

inline std::uint32_t read_word( const std::vector<std::uint8_t>& data,
                                std::uint64_t                    offset,
                                unsigned char encoding )
{
    std::uint32_t value = 0;
    if ( encoding == ELFDATA2LSB ) {
        for ( unsigned i = 0; i < 4; ++i ) {
            value |= static_cast<std::uint32_t>( data[offset + i] )
                     << ( 8 * i );
        }
    }
    else {
        for ( unsigned i = 0; i < 4; ++i ) {
            value = ( value << 8 ) |
                    static_cast<std::uint32_t>( data[offset + i] );
        }
    }
    return value;
}

inline std::int32_t sign_extend( std::uint32_t value, unsigned bits )
{
    const std::uint32_t sign = std::uint32_t( 1 ) << ( bits - 1 );
    return static_cast<std::int32_t>( value & ( ( std::uint32_t( 1 ) << bits ) - 1 ) ) -
           ( ( value & sign ) ? ( std::int32_t( 1 ) << bits ) : 0 );
}

//------------------------------------------------------------------------------
//! \brief Decode the immediate of an I-type instruction from its low bits.
//------------------------------------------------------------------------------
inline std::int32_t decode_i_immediate( std::uint32_t word )
{
    return sign_extend( ( word >> 20 ) & 0xfff, 12 );
}

//------------------------------------------------------------------------------
//! \brief Decode the immediate of an S-type instruction from its split fields.
//------------------------------------------------------------------------------
inline std::int32_t decode_s_immediate( std::uint32_t word )
{
    const std::uint32_t imm_11_5 = ( word >> 25 ) & 0x7f;
    const std::uint32_t imm_4_0  = ( word >> 7 ) & 0x1f;
    return sign_extend( ( imm_11_5 << 5 ) | imm_4_0, 12 );
}

//------------------------------------------------------------------------------
//! \brief Decode the 20-bit AUIPC immediate from bits 31:12.
//------------------------------------------------------------------------------
inline std::int64_t decode_u_immediate( std::uint32_t word )
{
    return sign_extend( word >> 12, 20 );
}

//------------------------------------------------------------------------------
//! \brief Decode the immediate of a JALR instruction.
//------------------------------------------------------------------------------
inline std::int32_t decode_jalr_immediate( std::uint32_t word )
{
    return decode_i_immediate( word );
}

inline std::string register_name( std::uint32_t reg )
{
    static const char* names[] = { "zero", "ra", "sp", "gp", "tp", "t0", "t1",
                                   "t2",   "s0", "s1", "a0", "a1", "a2", "a3",
                                   "a4",   "a5", "a6", "a7", "s2", "s3", "s4",
                                   "s5",   "s6", "s7", "s8", "s9", "s10",
                                   "s11",  "t3", "t4", "t5", "t6" };
    if ( reg < sizeof( names ) / sizeof( names[0] ) ) {
        return names[reg];
    }
    return std::to_string( reg );
}

//------------------------------------------------------------------------------
//! \brief Read a symbol name purely for display.
//------------------------------------------------------------------------------
inline std::string symbol_display_name( const elfio&  elf_file,
                                        std::uint64_t symbol_table,
                                        std::uint64_t symbol_index )
{
    if ( symbol_table >= elf_file.sections.size() ) {
        return std::string();
    }
    const section* symtab = elf_file.sections[static_cast<unsigned>(
        symbol_table )];
    if ( symtab == nullptr || symtab->get_type() != SHT_SYMTAB ) {
        return std::string();
    }
    const Elf_Half string_table =
        static_cast<Elf_Half>( symtab->get_link() );
    if ( string_table == 0 || string_table >= elf_file.sections.size() ) {
        return std::string();
    }
    const section* strtab = elf_file.sections[string_table];
    if ( strtab == nullptr ) {
        return std::string();
    }
    const Elf_Xword entry_size = symtab->get_entry_size();
    if ( entry_size < sizeof( Elf64_Sym ) ) {
        return std::string();
    }
    const char* data = symtab->get_data();
    if ( data == nullptr ||
         symbol_index >= symtab->get_size() / entry_size ) {
        return std::string();
    }
    Elf64_Sym raw;
    std::memcpy( &raw, data + symbol_index * entry_size, sizeof( raw ) );
    const auto& convertor = elf_file.get_convertor();
    const Elf_Word name_index = ( *convertor )( raw.st_name );
    const_string_section_accessor strings( strtab );
    const char* name = strings.get_string( name_index );
    return name == nullptr ? std::string() : std::string( name );
}

//------------------------------------------------------------------------------
//! \brief Decode one patched word into a reference description.
//------------------------------------------------------------------------------
inline decoded_reference decode_insn( std::uint32_t word,
                                      std::uint32_t offset,
                                      std::uint64_t pc )
{
    decoded_reference insn;
    insn.word_offset    = offset;
    insn.patched_word   = word;
    insn.opcode         = word & 0x7f;
    insn.funct3         = ( word >> 12 ) & 0x7;
    insn.rd             = ( word >> 7 ) & 0x1f;
    insn.rs1            = ( word >> 15 ) & 0x1f;
    insn.rs2            = ( word >> 20 ) & 0x1f;
    insn.instruction_pc = pc + offset;
    if ( insn.opcode == 0x17 ) { // AUIPC
        insn.immediate = decode_u_immediate( word ) << 12;
    }
    else if ( insn.opcode == 0x03 || insn.opcode == 0x13 ||
              insn.opcode == 0x67 ) { // load, ADDI, JALR
        insn.immediate = decode_i_immediate( word );
    }
    else if ( insn.opcode == 0x23 ) { // store
        insn.immediate = decode_s_immediate( word );
    }
    return insn;
}

} // namespace detail

//------------------------------------------------------------------------------
//! \brief Build a report from an ELF object and a section base table.
//!
//! The production entry point analyze_riscv_relocations() supplies the patched
//! bytes. The referenced target of every paired record is decoded back from
//! those bytes (AUIPC sign-extended upper part plus the sign-extended low
//! immediate plus the AUIPC's own PC), never copied from the plan.
//------------------------------------------------------------------------------
inline analysis_report
build_riscv_reference_report( const elfio&                     elf_file,
                              const riscv_section_base_table&  section_bases )
{
    analysis_report report;

    const riscv_reloc_plan plan =
        analyze_riscv_relocations( elf_file, section_bases );
    if ( !plan.ok ) {
        report.ok    = false;
        report.error = plan.error.message;
        return report;
    }

    auto output_word = [&plan, &elf_file]( std::uint64_t section,
                                std::uint64_t offset,
                                std::uint32_t& word ) -> bool {
        const auto* out = plan.find_output( static_cast<Elf_Half>( section ) );
        if ( out == nullptr || offset + 4 > out->data.size() ) {
            return false;
        }
        word = detail::read_word( out->data, offset,
                                  elf_file.get_encoding() );
        return true;
    };

    for ( const auto& change : plan.changes ) {
        reference_row row;
        row.rela_section       = change.rela_section;
        row.rela_entry         = change.rela_entry;
        row.rela_file_offset   = change.rela_file_offset;
        row.symbol_table       = change.symbol_table;
        row.symbol_index       = change.symbol_index;
        row.symbol_name        =
            detail::symbol_display_name( elf_file, change.symbol_table,
                                         change.symbol_index );
        row.symbol_file_offset = change.symbol_file_offset;
        row.symbol_address     = change.symbol_address;
        row.addend             = change.addend;
        row.code_section       = change.code_section;
        row.code_offset        = change.code_offset;
        row.code_file_offset   = 0;
        row.pc                 = change.pc;

        switch ( change.kind ) {
        case riscv_reloc_kind::pcrel_hi20:
            row.kind = "PCREL_HI20";
            break;
        case riscv_reloc_kind::pcrel_lo12_i:
            row.kind = "PCREL_LO12_I";
            break;
        case riscv_reloc_kind::pcrel_lo12_s:
            row.kind = "PCREL_LO12_S";
            break;
        case riscv_reloc_kind::call:
            row.kind = "CALL";
            break;
        }

        if ( change.has_anchor ) {
            row.hi_rela_section   = change.anchor_rela_section;
            row.hi_rela_entry     = change.anchor_rela_entry;
            row.hi_symbol_table   = change.anchor_symbol_table;
            row.hi_symbol_index   = change.anchor_symbol_index;
            row.hi_symbol_name =
                detail::symbol_display_name( elf_file,
                                             change.anchor_symbol_table,
                                             change.anchor_symbol_index );
            row.hi_symbol_address = change.anchor_symbol_address;
            row.hi_addend         = change.anchor_addend;
            row.hi_pc             = change.anchor_pc;
        }

        // Decode the patched words. The low half is always interpreted at the
        // PC of the AUIPC anchor it belongs to, never at the low instruction's
        // own PC.
        const std::uint64_t reference_pc =
            change.has_anchor ? change.anchor_pc : change.pc;
        std::int64_t decoded_hi   = 0;
        bool         have_hi      = false;
        std::int64_t decoded_lo12 = 0;
        bool         have_lo12    = false;

        for ( std::size_t i = 0; i < change.instructions.size(); ++i ) {
            const auto& source = change.instructions[i];
            std::uint32_t patched = 0;
            if ( !output_word( change.code_section, source.offset, patched ) ) {
                report.ok    = false;
                report.error = "patched output bytes are unavailable";
                return report;
            }
            const std::uint64_t own_pc =
                change.pc +
                ( source.offset - static_cast<std::uint64_t>(
                                      change.code_offset ) );
            decoded_reference insn = detail::decode_insn(
                patched, static_cast<std::uint32_t>( source.offset ), own_pc );
            insn.original_word = source.original_word;
            row.instructions.push_back( insn );

            if ( insn.opcode == 0x17 ) {
                decoded_hi = insn.immediate;
                have_hi    = true;
            }
            else {
                decoded_lo12 = insn.immediate;
                have_lo12    = true;
            }
        }

        // A HI record alone does not carry the low 12 bits; recover them from
        // the paired low record that names this HI record as its anchor.
        if ( change.kind == riscv_reloc_kind::pcrel_hi20 ) {
            for ( const auto& candidate : plan.changes ) {
                if ( have_lo12 || !candidate.has_anchor ||
                     candidate.anchor_rela_section != change.rela_section ||
                     candidate.anchor_rela_entry != change.rela_entry ||
                     candidate.code_section != change.code_section ) {
                    continue;
                }
                std::uint32_t low_word = 0;
                if ( output_word( candidate.code_section, candidate.code_offset,
                                  low_word ) ) {
                    decoded_lo12 = detail::decode_insn( low_word, 0, 0 )
                                       .immediate;
                    have_lo12 = true;
                }
            }
        }

        // A low record decodes its own low 12 bits; the upper part comes from
        // the anchor's patched AUIPC word in the returned bytes.
        if ( change.has_anchor && !have_hi ) {
            std::uint32_t anchor_word = 0;
            if ( output_word( change.code_section, change.anchor_code_offset,
                              anchor_word ) ) {
                const decoded_reference upper =
                    detail::decode_insn( anchor_word, 0, 0 );
                if ( upper.opcode == 0x17 ) {
                    decoded_hi = upper.immediate;
                    have_hi    = true;
                }
            }
        }

        const section* code = elf_file.sections[change.code_section];
        if ( code != nullptr ) {
            row.code_file_offset = code->get_offset() + change.code_offset;
        }

        if ( have_hi && have_lo12 ) {
            row.referenced_target =
                static_cast<std::uint64_t>( reference_pc + decoded_hi +
                                            decoded_lo12 );
        }
        else if ( have_hi ) {
            // A lone HI record cannot resolve the low part on its own.
            row.referenced_target =
                static_cast<std::uint64_t>( reference_pc + decoded_hi );
        }
        else if ( have_lo12 ) {
            row.referenced_target =
                static_cast<std::uint64_t>( reference_pc + decoded_lo12 );
        }

        report.rows.push_back( std::move( row ) );
    }

    for ( const auto& hint : plan.hints ) {
        hint_row row;
        row.rela_section      = hint.rela_section;
        row.rela_entry        = hint.rela_entry;
        row.rela_file_offset  = hint.rela_file_offset;
        row.code_section      = hint.code_section;
        row.code_offset       = hint.code_offset;
        row.code_file_offset  = hint.code_file_offset;
        row.pc                = hint.pc;
        row.main_rela_section = hint.main_rela_section;
        row.main_rela_entry   = hint.main_rela_entry;
        report.hints.push_back( row );
    }

    report.ok = true;
    return report;
}

namespace detail {

inline std::string hex_value( std::uint64_t value, unsigned width )
{
    std::ostringstream stream;
    stream << "0x" << std::hex << std::setw( static_cast<int>( width ) )
           << std::setfill( '0' ) << value;
    return stream.str();
}

inline std::string signed_hex( std::int64_t value, unsigned width )
{
    const std::uint64_t magnitude =
        value < 0 ? ( std::uint64_t( 0 ) - static_cast<std::uint64_t>( value ) )
                  : static_cast<std::uint64_t>( value );
    std::ostringstream stream;
    stream << ( value < 0 ? "-" : "+" ) << "0x" << std::hex
           << std::setw( static_cast<int>( width ) ) << std::setfill( '0' )
           << magnitude;
    return stream.str();
}

} // namespace detail

//------------------------------------------------------------------------------
//! \brief Write the report as tab-separated rows.
//!
//! One row per supported relocation, one row per decoded instruction, and one
//! row per RELAX hint. Every row carries the physical file offset of the
//! relocation entry and, for references, the symbol entry and code word.
//------------------------------------------------------------------------------
inline void write_riscv_reference_report( const analysis_report& report,
                                          std::ostream&          stream )
{
    if ( !report.ok ) {
        stream << "ERROR\t" << report.error << "\n";
        return;
    }

    stream << "row\trela_section\trela_entry\trela_file_off"
              "\tsymtab\tsymidx\tsymname\tsym_file_off\tsym_addr\taddend"
              "\thi_rela_section\thi_rela_entry\thi_symtab\thi_symidx"
              "\thi_symname\thi_sym_addr\thi_addend\thi_pc"
              "\tcode_section\tcode_offset\tcode_file_off\tpc"
              "\tword_index\tword_file_off\topcode\tfunct3\trd\trs1\trs2"
              "\timm\torig_word\tpatched_word\treferenced_target\n";

    auto row_prefix = [&stream]( const reference_row& row ) {
        stream << row.kind << '\t' << row.rela_section << '\t' << row.rela_entry
               << '\t' << row.rela_file_offset << '\t' << row.symbol_table
               << '\t' << row.symbol_index << '\t' << row.symbol_name << '\t'
               << row.symbol_file_offset << '\t' << row.symbol_address << '\t'
               << row.addend << '\t';
        if ( row.hi_rela_section != 0 || row.hi_rela_entry != 0 ) {
            stream << row.hi_rela_section << '\t' << row.hi_rela_entry << '\t'
                   << row.hi_symbol_table << '\t' << row.hi_symbol_index << '\t'
                   << row.hi_symbol_name << '\t' << row.hi_symbol_address
                   << '\t' << row.hi_addend << '\t' << row.hi_pc << '\t';
        }
        else {
            stream << "-\t-\t-\t-\t-\t-\t-\t-\t";
        }
    };

    for ( const auto& row : report.rows ) {
        if ( row.instructions.empty() ) {
            row_prefix( row );
            stream << row.code_section << '\t' << row.code_offset << '\t'
                   << detail::hex_value( row.code_file_offset, 8 ) << '\t'
                   << row.pc << "\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-"
                   << '\t' << row.referenced_target << '\n';
            continue;
        }
        for ( std::size_t i = 0; i < row.instructions.size(); ++i ) {
            const auto& insn = row.instructions[i];
            row_prefix( row );
            stream << row.code_section << '\t' << row.code_offset << '\t'
                   << detail::hex_value( row.code_file_offset, 8 ) << '\t'
                   << row.pc << '\t' << insn.word_offset << '\t'
                   << detail::hex_value( row.code_file_offset +
                                             ( insn.word_offset -
                                               static_cast<std::uint32_t>(
                                                   row.code_offset ) ),
                                         8 )
                   << '\t' << detail::hex_value( insn.opcode, 2 ) << '\t'
                   << insn.funct3 << '\t' << detail::register_name( insn.rd )
                   << '\t' << detail::register_name( insn.rs1 ) << '\t'
                   << detail::register_name( insn.rs2 ) << '\t'
                   << detail::signed_hex( insn.immediate, 4 ) << '\t'
                   << detail::hex_value( insn.original_word, 8 ) << '\t'
                   << detail::hex_value( insn.patched_word, 8 ) << '\t'
                   << ( i == 0 ? detail::hex_value( row.referenced_target, 8 )
                               : std::string( "-" ) )
                   << '\n';
        }
    }

    for ( const auto& hint : report.hints ) {
        // A RELAX hint points at the main record it relaxes; it never gets a
        // second instruction row or a fabricated reference target.
        stream << "RELAX\t" << hint.rela_section << '\t' << hint.rela_entry
               << '\t' << hint.rela_file_offset
               << "\t-\t-\t-\t-\t-\t-" // symtab .. addend
               << '\t' << hint.main_rela_section << '\t'
               << hint.main_rela_entry
               << "\t-\t-\t-\t-\t-\t-" // remaining hi columns
               << '\t' << hint.code_section << '\t' << hint.code_offset
               << '\t' << detail::hex_value( hint.code_file_offset, 8 )
               << '\t' << hint.pc
               << "\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\n";
    }
}

} // namespace riscv_example
} // namespace ELFIO

#endif // ELFIO_RISCV_RELOC_ANALYSIS_REPORT_HPP
