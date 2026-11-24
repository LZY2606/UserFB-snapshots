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

#ifndef ELFIO_RISCV_RELOC_HPP
#define ELFIO_RISCV_RELOC_HPP

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ELFIO {

//------------------------------------------------------------------------------
//! \brief Physical section bases, keyed by physical section index.
//!
//! The key is the section index used by sh_link and sh_info. Only sections
//! that take part in an analysed relocation have to be present.
//------------------------------------------------------------------------------
using riscv_section_base_table = std::map<Elf_Half, Elf64_Addr>;

//------------------------------------------------------------------------------
//! \brief The supported RISC-V relocation kinds.
//------------------------------------------------------------------------------
enum class riscv_reloc_kind {
    pcrel_hi20,
    pcrel_lo12_i,
    pcrel_lo12_s,
    call
};

//------------------------------------------------------------------------------
//! \brief One instruction word touched by a relocation.
//!
//! The offset is relative to the code section; the file offset points at the
//! same instruction in the input ELF image.
//------------------------------------------------------------------------------
struct riscv_reloc_instruction
{
    Elf64_Addr    offset        = 0;
    Elf64_Off     file_offset   = 0;
    std::uint32_t original_word = 0;
    std::uint32_t patched_word  = 0;
};

//------------------------------------------------------------------------------
//! \brief A single applied relocation record.
//!
//! For pcrel_lo12_i/pcrel_lo12_s the symbol_* fields describe the anchor
//! symbol referenced by the low relocation, while the anchor_* fields identify
//! the matching PCREL_HI20 record and its final target. For pcrel_hi20 and call
//! the symbol_* fields describe the final target symbol.
//------------------------------------------------------------------------------
struct riscv_reloc_change
{
    riscv_reloc_kind kind = riscv_reloc_kind::pcrel_hi20;
    unsigned         type = 0;

    Elf_Half   rela_section     = 0;
    Elf_Xword  rela_entry       = 0;
    Elf64_Off  rela_file_offset = 0;

    Elf_Half   symbol_table        = 0;
    Elf_Word   symbol_index        = 0;
    Elf64_Off  symbol_file_offset  = 0;
    Elf64_Addr symbol_address      = 0;
    Elf_Sxword addend              = 0;
    Elf64_Addr target_address      = 0;

    Elf_Half   code_section = 0;
    Elf64_Addr code_offset  = 0;
    Elf64_Addr pc           = 0;

    bool       has_anchor                = false;
    Elf_Half   anchor_rela_section       = 0;
    Elf_Xword  anchor_rela_entry         = 0;
    Elf_Half   anchor_symbol_table       = 0;
    Elf_Word   anchor_symbol_index       = 0;
    Elf64_Off  anchor_symbol_file_offset = 0;
    Elf64_Addr anchor_symbol_address     = 0;
    Elf_Sxword anchor_addend             = 0;
    Elf64_Addr anchor_code_offset        = 0;
    Elf64_Addr anchor_pc                 = 0;

    std::vector<riscv_reloc_instruction> instructions;
};

//------------------------------------------------------------------------------
//! \brief A RELAX record accepted as a hint for an existing main record.
//------------------------------------------------------------------------------
struct riscv_reloc_hint
{
    Elf_Half   rela_section       = 0;
    Elf_Xword  rela_entry         = 0;
    Elf64_Off  rela_file_offset   = 0;
    Elf_Half   code_section       = 0;
    Elf64_Addr code_offset        = 0;
    Elf64_Off  code_file_offset   = 0;
    Elf64_Addr pc                 = 0;
    Elf_Half   main_rela_section  = 0;
    Elf_Xword  main_rela_entry    = 0;
};

//------------------------------------------------------------------------------
//! \brief Failure information for an analysis.
//!
//! rela_section/rela_entry identify the offending relocation table and entry.
//! has_entry is false when the failure concerns the table or file as a whole.
//------------------------------------------------------------------------------
struct riscv_reloc_error
{
    bool        is_error     = false;
    Elf_Half    rela_section = 0;
    Elf_Xword   rela_entry   = 0;
    bool        has_entry    = false;
    std::string message;
};

//------------------------------------------------------------------------------
//! \brief An independently owned copy of one patched target section.
//------------------------------------------------------------------------------
struct riscv_reloc_output_section
{
    Elf_Half                  section = 0;
    std::vector<std::uint8_t> data;
};

//------------------------------------------------------------------------------
//! \brief The complete result of an analysis.
//!
//! On failure ok is false, error is populated, and changes/hints/outputs stay
//! empty; no partial plan is ever published. changes and hints use a stable
//! order that does not depend on relocation table traversal order.
//------------------------------------------------------------------------------
struct riscv_reloc_plan
{
    bool                                    ok = false;
    riscv_reloc_error                       error;
    std::vector<riscv_reloc_change>         changes;
    std::vector<riscv_reloc_hint>           hints;
    std::vector<riscv_reloc_output_section> outputs;

    //------------------------------------------------------------------------------
    //! \brief Find the owned output bytes of a section, or nullptr.
    //------------------------------------------------------------------------------
    const riscv_reloc_output_section* find_output( Elf_Half section ) const
    {
        for ( const auto& out : outputs ) {
            if ( out.section == section ) {
                return &out;
            }
        }
        return nullptr;
    }

    //------------------------------------------------------------------------------
    //! \brief Find a change by its source relocation table and entry.
    //------------------------------------------------------------------------------
    const riscv_reloc_change* find_change( Elf_Half  rela_section,
                                           Elf_Xword rela_entry ) const
    {
        for ( const auto& change : changes ) {
            if ( change.rela_section == rela_section &&
                 change.rela_entry == rela_entry ) {
                return &change;
            }
        }
        return nullptr;
    }
};

namespace riscv_detail {

//------------------------------------------------------------------------------
//! \brief A symbol entry read from a symbol table.
//------------------------------------------------------------------------------
struct symbol_record
{
    Elf_Word      name_index = 0;
    Elf64_Addr    value      = 0;
    Elf_Xword     size       = 0;
    unsigned char bind       = 0;
    unsigned char type       = 0;
    unsigned char other      = 0;
    Elf_Half      shndx      = SHN_UNDEF;
    Elf64_Off     file_offset = 0;
};

//------------------------------------------------------------------------------
//! \brief A relocation entry read from a SHT_RELA table.
//------------------------------------------------------------------------------
struct entry_record
{
    Elf_Half    rela_section     = 0;
    Elf_Xword   rela_entry       = 0;
    Elf64_Off   rela_file_offset = 0;
    Elf64_Addr  offset           = 0;
    Elf_Word    symbol_index     = 0;
    unsigned    type             = 0;
    Elf_Sxword  addend           = 0;
    Elf_Half    code_section     = 0;
    bool        has_symbol       = false;
    symbol_record symbol;
    bool        has_anchor       = false;
    symbol_record anchor;
};

//------------------------------------------------------------------------------
//! \brief Checked addition of two unsigned 64-bit values.
//------------------------------------------------------------------------------
inline bool add_checked( std::uint64_t a,
                         std::uint64_t b,
                         std::uint64_t& result )
{
    if ( a > std::numeric_limits<std::uint64_t>::max() - b ) {
        return false;
    }
    result = a + b;
    return true;
}

//------------------------------------------------------------------------------
//! \brief Combine two signed magnitudes without overflowing 64 bits.
//!
//! Each operand is represented as a sign and a magnitude. The sum is stored in
//! the same form. Returns false when the true sum exceeds 64-bit range.
//------------------------------------------------------------------------------
inline bool combine_signed( bool          a_negative,
                            std::uint64_t a_magnitude,
                            bool          b_negative,
                            std::uint64_t b_magnitude,
                            bool&         negative,
                            std::uint64_t& magnitude )
{
    if ( a_magnitude == 0 ) {
        negative  = b_negative;
        magnitude = b_magnitude;
        return true;
    }
    if ( b_magnitude == 0 ) {
        negative  = a_negative;
        magnitude = a_magnitude;
        return true;
    }
    if ( a_negative == b_negative ) {
        if ( a_magnitude >
             std::numeric_limits<std::uint64_t>::max() - b_magnitude ) {
            return false;
        }
        negative  = a_negative;
        magnitude = a_magnitude + b_magnitude;
        return true;
    }
    if ( a_magnitude >= b_magnitude ) {
        magnitude = a_magnitude - b_magnitude;
        negative  = a_magnitude == b_magnitude ? false : a_negative;
    }
    else {
        magnitude = b_magnitude - a_magnitude;
        negative  = b_negative;
    }
    return true;
}

//------------------------------------------------------------------------------
//! \brief Sign-extend a 12-bit immediate.
//------------------------------------------------------------------------------
inline std::int32_t sign_extend_12( std::uint32_t value )
{
    return static_cast<std::int32_t>( value & 0xfff ) -
           ( ( value & 0x800 ) ? 0x1000 : 0 );
}

//------------------------------------------------------------------------------
//! \brief Sign-extend a 20-bit immediate.
//------------------------------------------------------------------------------
inline std::int64_t sign_extend_20( std::uint32_t value )
{
    return static_cast<std::int64_t>( value & 0xfffff ) -
           ( ( value & 0x80000 ) ? 0x100000 : 0 );
}

//------------------------------------------------------------------------------
//! \brief Split a PC-relative delta into AUIPC 31:12 and I/S low bits.
//!
//! The combined value is verified against the full 64-bit delta; truncating it
//! to 32 bits is never accepted.
//------------------------------------------------------------------------------
inline bool encode_pcrel_pair( bool           negative,
                               std::uint64_t  magnitude,
                               std::uint32_t& hi20,
                               std::uint32_t& lo12,
                               std::string&   message )
{
    const std::uint64_t positive_limit = ( std::uint64_t( 1 ) << 31 ) - 2049;
    const std::uint64_t negative_limit = ( std::uint64_t( 1 ) << 31 ) + 2048;
    if ( ( negative && magnitude > negative_limit ) ||
         ( !negative && magnitude > positive_limit ) ) {
        message = "PC-relative value cannot be represented by an AUIPC/low "
                  "instruction pair";
        return false;
    }

    const std::int64_t delta =
        negative ? -static_cast<std::int64_t>( magnitude )
                 : static_cast<std::int64_t>( magnitude );
    // hi = floor( delta / 4096 ), rounded so that lo fits [-2048, 2047].
    const std::int64_t adjusted = delta + 2048;
    std::int64_t       hi       = adjusted / 4096;
    if ( adjusted % 4096 != 0 && adjusted < 0 ) {
        --hi;
    }
    const std::int64_t lo = delta - hi * 4096;
    if ( hi < -( std::int64_t( 1 ) << 19 ) ||
         hi > ( std::int64_t( 1 ) << 19 ) - 1 || lo < -2048 || lo > 2047 ) {
        message = "PC-relative value cannot be represented by an AUIPC/low "
                  "instruction pair";
        return false;
    }

    const std::uint32_t hi_bits = static_cast<std::uint32_t>( hi ) & 0xfffff;
    const std::int64_t  recon =
        sign_extend_20( hi_bits ) * 4096 +
        static_cast<std::int64_t>( sign_extend_12(
            static_cast<std::uint32_t>( lo ) & 0xfff ) );
    if ( recon != delta ) {
        message = "PC-relative value does not round-trip through the "
                  "sign-extended AUIPC/low pair";
        return false;
    }

    hi20 = hi_bits;
    lo12 = static_cast<std::uint32_t>( lo ) & 0xfff;
    return true;
}

//------------------------------------------------------------------------------
//! \brief Instruction field helpers.
//------------------------------------------------------------------------------
inline std::uint32_t instruction_opcode( std::uint32_t word )
{
    return word & 0x7f;
}
inline std::uint32_t instruction_funct3( std::uint32_t word )
{
    return ( word >> 12 ) & 0x7;
}
inline std::uint32_t instruction_rd( std::uint32_t word )
{
    return ( word >> 7 ) & 0x1f;
}
inline std::uint32_t instruction_rs1( std::uint32_t word )
{
    return ( word >> 15 ) & 0x1f;
}

} // namespace riscv_detail

//------------------------------------------------------------------------------
//! \brief Fill the source identity fields of a change.
//------------------------------------------------------------------------------
inline void riscv_fill_change_source( riscv_reloc_change&         change,
                                      const riscv_detail::entry_record& entry,
                                      const riscv_detail::symbol_record& symbol,
                                      Elf_Half  symbol_table,
                                      Elf64_Addr pc )
{
    change.rela_section      = entry.rela_section;
    change.rela_entry        = entry.rela_entry;
    change.rela_file_offset  = entry.rela_file_offset;
    change.symbol_table      = symbol_table;
    change.symbol_index      = entry.symbol_index;
    change.symbol_file_offset = symbol.file_offset;
    change.symbol_address    = 0;
    change.addend            = entry.addend;
    change.code_section      = entry.code_section;
    change.code_offset       = entry.offset;
    change.pc                = pc;
}

namespace riscv_detail {

//------------------------------------------------------------------------------
//! \brief Resolve the address of a symbol under the supplied section layout.
//!
//! SHN_ABS symbols take their st_value directly. Other symbols add the base of
//! their section. Undefined, COMMON, reserved and extended indices are errors.
//------------------------------------------------------------------------------
inline bool resolve_symbol_address( const symbol_record&           symbol,
                                    const riscv_section_base_table& bases,
                                    Elf64_Addr&                     address,
                                    std::string&                    message )
{
    if ( symbol.shndx == SHN_UNDEF ) {
        message = "relocation references an undefined symbol";
        return false;
    }
    if ( symbol.shndx == SHN_COMMON ) {
        message = "relocation references a COMMON symbol";
        return false;
    }
    if ( symbol.shndx == SHN_ABS ) {
        address = symbol.value;
        return true;
    }
    if ( symbol.shndx >= SHN_LORESERVE ) {
        message =
            "relocation references an extended or reserved section index";
        return false;
    }
    auto base_it = bases.find( symbol.shndx );
    if ( base_it == bases.end() ) {
        message = "missing base address for the symbol's section";
        return false;
    }
    if ( !add_checked( base_it->second, symbol.value, address ) ) {
        message = "symbol address calculation overflows";
        return false;
    }
    return true;
}

//------------------------------------------------------------------------------
//! \brief Compute D = S + A - P as a signed magnitude with overflow checks.
//------------------------------------------------------------------------------
inline bool compute_pcrel_delta( Elf64_Addr    s,
                                 Elf_Sxword    addend,
                                 Elf64_Addr    p,
                                 bool&         negative,
                                 std::uint64_t& magnitude,
                                 std::string&  message )
{
    const bool addend_negative = addend < 0;
    const std::uint64_t addend_magnitude =
        addend_negative ? ( std::uint64_t( 0 ) - static_cast<std::uint64_t>(
                                                      addend ) )
                        : static_cast<std::uint64_t>( addend );

    bool          sum_negative = false;
    std::uint64_t sum_magnitude = 0;
    if ( !combine_signed( false, s, addend_negative, addend_magnitude,
                          sum_negative, sum_magnitude ) ) {
        message = "address calculation overflows 64 bits";
        return false;
    }
    if ( !combine_signed( sum_negative, sum_magnitude, true, p, negative,
                          magnitude ) ) {
        message = "address calculation overflows 64 bits";
        return false;
    }
    return true;
}

} // namespace riscv_detail

//------------------------------------------------------------------------------
//! \brief Analyse RISC-V PC-relative relocations against a section layout.
//!
//! The input object must be ELF64, little endian, EM_RISCV, ET_REL. Relocation
//! targets must be uncompressed executable PROGBITS sections. Neither the ELF
//! object nor the supplied bases are modified.
//!
//! \param elf_file      The input object file.
//! \param section_bases Physical section index to runtime base address.
//! \return The patched section bytes and a per-record change description.
//------------------------------------------------------------------------------
inline riscv_reloc_plan
analyze_riscv_relocations( const elfio&                    elf_file,
                           const riscv_section_base_table& section_bases )
{
    using namespace riscv_detail;

    riscv_reloc_plan plan;

    auto fail = [&plan]( Elf_Half    table,
                         Elf_Xword   entry,
                         bool        has_entry,
                         std::string message ) -> riscv_reloc_plan& {
        plan                        = riscv_reloc_plan{};
        plan.ok                     = false;
        plan.error.is_error         = true;
        plan.error.rela_section     = table;
        plan.error.rela_entry       = entry;
        plan.error.has_entry        = has_entry;
        plan.error.message          = std::move( message );
        return plan;
    };

    if ( elf_file.get_class() != ELFCLASS64 ) {
        return fail( 0, 0, false, "only ELF64 input is supported" );
    }
    if ( elf_file.get_encoding() != ELFDATA2LSB ) {
        return fail( 0, 0, false, "only little-endian input is supported" );
    }
    if ( elf_file.get_machine() != EM_RISCV ) {
        return fail( 0, 0, false, "input is not an EM_RISCV object" );
    }
    if ( elf_file.get_type() != ET_REL ) {
        return fail( 0, 0, false, "input is not an ET_REL object" );
    }

    const auto& convertor    = elf_file.get_convertor();
    const auto  section_count = elf_file.sections.size();

    auto read_word = [&convertor]( const char* data ) {
        std::uint32_t raw = 0;
        std::memcpy( &raw, data, sizeof( raw ) );
        return ( *convertor )( raw );
    };

    //------------------------------------------------------------------
    // Locate and validate all RELA tables. The table name is irrelevant;
    // sh_link supplies symbols and sh_info supplies the code section.
    //------------------------------------------------------------------
    struct table_record
    {
        Elf_Half       index        = 0;
        Elf_Xword      entry_size   = 0;
        Elf_Xword      entry_count  = 0;
        const char*    data         = nullptr;
        Elf_Half       symbol_table = 0;
        Elf_Half       code_section = 0;
        const section* code         = nullptr;
        Elf64_Addr     code_base    = 0;
    };
    std::vector<table_record> tables;

    for ( Elf_Half index = 0; index < section_count; ++index ) {
        const section* rela = elf_file.sections[index];
        if ( rela == nullptr || rela->get_type() != SHT_RELA ) {
            continue;
        }

        table_record table;
        table.index = index;

        const Elf_Half link = static_cast<Elf_Half>( rela->get_link() );
        if ( link == 0 || link >= section_count ) {
            return fail( index, 0, false,
                         "relocation table sh_link is not a valid section "
                         "index" );
        }
        const section* symtab = elf_file.sections[link];
        if ( symtab == nullptr || symtab->get_type() != SHT_SYMTAB ) {
            return fail( index, 0, false,
                         "relocation table sh_link does not reference a "
                         "symbol table" );
        }
        table.symbol_table = link;

        const Elf_Half info = static_cast<Elf_Half>( rela->get_info() );
        if ( info == 0 || info >= section_count ) {
            return fail( index, 0, false,
                         "relocation table sh_info is not a valid section "
                         "index" );
        }
        const section* code = elf_file.sections[info];
        if ( code == nullptr || code->get_type() != SHT_PROGBITS ) {
            return fail( index, 0, false,
                         "relocation target is not a PROGBITS section" );
        }
        if ( ( code->get_flags() & SHF_EXECINSTR ) == 0 ) {
            return fail( index, 0, false,
                         "relocation target is not an executable section" );
        }
        if ( ( code->get_flags() & ( SHF_COMPRESSED | SHF_RPX_DEFLATE ) ) !=
             0 ) {
            return fail( index, 0, false,
                         "relocation target is a compressed section" );
        }
        table.code_section = info;
        table.code         = code;
        auto base_it       = section_bases.find( info );
        if ( base_it == section_bases.end() ) {
            return fail( index, 0, false,
                         "missing base address for relocation target "
                         "section" );
        }
        table.code_base = base_it->second;

        table.entry_size = rela->get_entry_size();
        if ( table.entry_size < sizeof( Elf64_Rela ) ) {
            return fail( index, 0, false,
                         "relocation table entry size is too small" );
        }
        if ( rela->get_size() % table.entry_size != 0 ) {
            return fail( index, rela->get_size() / table.entry_size, true,
                         "relocation table ends with a truncated entry" );
        }
        table.entry_count = rela->get_size() / table.entry_size;
        table.data        = rela->get_data();
        if ( table.entry_count != 0 && table.data == nullptr ) {
            return fail( index, 0, true,
                         "relocation table data cannot be read" );
        }
        tables.push_back( table );
    }

    //------------------------------------------------------------------
    // Parse entries.
    //------------------------------------------------------------------
    std::vector<entry_record> entries;
    for ( const auto& table : tables ) {
        const section* rela_sec = elf_file.sections[table.index];
        for ( Elf_Xword i = 0; i < table.entry_count; ++i ) {
            const char* ptr = table.data + i * table.entry_size;
            Elf64_Rela  raw;
            std::memcpy( &raw, ptr, sizeof( raw ) );

            entry_record entry;
            entry.rela_section     = table.index;
            entry.rela_entry       = i;
            entry.rela_file_offset = rela_sec->get_offset() +
                                     i * table.entry_size;
            entry.offset   = ( *convertor )( raw.r_offset );
            const Elf_Xword info = ( *convertor )( raw.r_info );
            entry.symbol_index =
                static_cast<Elf_Word>( ELF64_R_SYM( info ) );
            entry.type   = static_cast<unsigned>( ELF64_R_TYPE( info ) );
            entry.addend = ( *convertor )( raw.r_addend );
            entry.code_section = table.code_section;
            entries.push_back( entry );
        }
    }

    //------------------------------------------------------------------
    // Read a symbol from a symbol table.
    //------------------------------------------------------------------
    auto read_symbol = [&elf_file, &convertor, section_count](
                           Elf_Half       symtab_index,
                           Elf_Word       symbol_index,
                           symbol_record& symbol ) -> bool {
        if ( symtab_index == 0 || symtab_index >= section_count ) {
            return false;
        }
        const section* symtab = elf_file.sections[symtab_index];
        if ( symtab == nullptr || symtab->get_type() != SHT_SYMTAB ) {
            return false;
        }
        const Elf_Xword entry_size = symtab->get_entry_size();
        if ( entry_size < sizeof( Elf64_Sym ) ||
             symtab->get_size() % entry_size != 0 ) {
            return false;
        }
        const Elf_Xword count = symtab->get_size() / entry_size;
        if ( symbol_index >= count ) {
            return false;
        }
        const char* data = symtab->get_data();
        if ( data == nullptr ) {
            return false;
        }
        Elf64_Sym raw;
        std::memcpy( &raw, data + symbol_index * entry_size, sizeof( raw ) );
        symbol.name_index  = ( *convertor )( raw.st_name );
        symbol.value       = ( *convertor )( raw.st_value );
        symbol.size        = ( *convertor )( raw.st_size );
        symbol.bind        = ELF_ST_BIND( raw.st_info );
        symbol.type        = ELF_ST_TYPE( raw.st_info );
        symbol.other       = raw.st_other;
        symbol.shndx       = ( *convertor )( raw.st_shndx );
        symbol.file_offset = symtab->get_offset() +
                             symbol_index * entry_size;
        return true;
    };

    auto find_table = [&tables]( Elf_Half index ) -> const table_record* {
        for ( const auto& table : tables ) {
            if ( table.index == index ) {
                return &table;
            }
        }
        return nullptr;
    };

    //------------------------------------------------------------------
    // Validate entry types, symbol indices and anchor references.
    //------------------------------------------------------------------
    for ( auto& entry : entries ) {
        const bool needs_symbol =
            entry.type == R_RISCV_PCREL_HI20 ||
            entry.type == R_RISCV_PCREL_LO12_I ||
            entry.type == R_RISCV_PCREL_LO12_S ||
            entry.type == R_RISCV_CALL;
        if ( !needs_symbol ) {
            if ( entry.type != R_RISCV_NONE &&
                 entry.type != R_RISCV_RELAX ) {
                return fail( entry.rela_section, entry.rela_entry, true,
                             "unsupported relocation type " +
                                 std::to_string( entry.type ) );
            }
            continue;
        }

        const table_record* table = find_table( entry.rela_section );
        if ( table == nullptr ) {
            return fail( entry.rela_section, entry.rela_entry, true,
                         "relocation table disappeared" );
        }
        if ( !read_symbol( table->symbol_table, entry.symbol_index,
                           entry.symbol ) ) {
            return fail( entry.rela_section, entry.rela_entry, true,
                         "relocation symbol index is out of range or the "
                         "symbol table is malformed" );
        }
        entry.has_symbol = true;

        if ( entry.type == R_RISCV_PCREL_LO12_I ||
             entry.type == R_RISCV_PCREL_LO12_S ) {
            if ( entry.addend != 0 ) {
                return fail( entry.rela_section, entry.rela_entry, true,
                             "PCREL_LO12 addend must be zero" );
            }
            if ( entry.symbol.shndx == SHN_UNDEF ) {
                return fail( entry.rela_section, entry.rela_entry, true,
                             "relocation references an undefined symbol" );
            }
            if ( entry.symbol.shndx == SHN_COMMON ) {
                return fail( entry.rela_section, entry.rela_entry, true,
                             "relocation references a COMMON symbol" );
            }
            if ( entry.symbol.shndx >= SHN_LORESERVE ) {
                return fail( entry.rela_section, entry.rela_entry, true,
                             "relocation references an extended or reserved "
                             "section index" );
            }
            if ( entry.symbol.shndx != entry.code_section ) {
                return fail( entry.rela_section, entry.rela_entry, true,
                             "PCREL_LO12 anchor is not in the instruction's "
                             "code section" );
            }
            entry.has_anchor = true;
            entry.anchor     = entry.symbol;
        }
    }

    //------------------------------------------------------------------
    // Index PCREL_HI20 records for anchor lookup.
    //------------------------------------------------------------------
    struct hi_record
    {
        Elf_Half   rela_section = 0;
        Elf_Xword  rela_entry   = 0;
        Elf_Half   code_section = 0;
        Elf64_Addr offset       = 0;
    };
    std::vector<hi_record> hi_records;
    for ( const auto& entry : entries ) {
        if ( entry.type == R_RISCV_PCREL_HI20 ) {
            hi_records.push_back( hi_record{ entry.rela_section,
                                             entry.rela_entry,
                                             entry.code_section,
                                             entry.offset } );
        }
    }

    //------------------------------------------------------------------
    // Resolve symbols and build changes.
    //------------------------------------------------------------------
    std::vector<riscv_reloc_change> changes;

    auto instruction_bounds = []( const section* code,
                                  Elf64_Addr   offset,
                                  Elf_Xword    length ) -> bool {
        return offset <= code->get_size() &&
               length <= code->get_size() - offset;
    };

    for ( const auto& entry : entries ) {
        const table_record* table = find_table( entry.rela_section );
        if ( table == nullptr ) {
            continue;
        }
        const section* code = table->code;

        if ( entry.type == R_RISCV_NONE || entry.type == R_RISCV_RELAX ) {
            continue; // NONE writes nothing; RELAX is handled later.
        }

        if ( !instruction_bounds( code, entry.offset, 4 ) ) {
            return fail( entry.rela_section, entry.rela_entry, true,
                         "relocation writes outside its code section" );
        }
        const char* code_data = code->get_data();
        if ( code_data == nullptr ) {
            return fail( entry.rela_section, entry.rela_entry, true,
                         "code section data cannot be read" );
        }
        const std::uint32_t original = read_word( code_data + entry.offset );
        Elf64_Addr          pc       = 0;
        if ( !add_checked( table->code_base, entry.offset, pc ) ) {
            return fail( entry.rela_section, entry.rela_entry, true,
                         "instruction address overflows 64 bits" );
        }

        if ( entry.type == R_RISCV_PCREL_HI20 ||
             entry.type == R_RISCV_CALL ) {
            Elf64_Addr  target = 0;
            std::string message;
            if ( !resolve_symbol_address( entry.symbol, section_bases, target,
                                          message ) ) {
                return fail( entry.rela_section, entry.rela_entry, true,
                             message );
            }

            bool          negative  = false;
            std::uint64_t magnitude = 0;
            if ( !compute_pcrel_delta( target, entry.addend, pc, negative,
                                       magnitude, message ) ) {
                return fail( entry.rela_section, entry.rela_entry, true,
                             message );
            }
            std::uint32_t hi20 = 0;
            std::uint32_t lo12 = 0;
            if ( !encode_pcrel_pair( negative, magnitude, hi20, lo12,
                                     message ) ) {
                return fail( entry.rela_section, entry.rela_entry, true,
                             message );
            }

            if ( entry.type == R_RISCV_PCREL_HI20 ) {
                if ( instruction_opcode( original ) != 0x17 ) {
                    return fail( entry.rela_section, entry.rela_entry, true,
                                 "PCREL_HI20 does not point at an AUIPC "
                                 "instruction" );
                }
                riscv_reloc_change change;
                change.kind = riscv_reloc_kind::pcrel_hi20;
                change.type = entry.type;
                riscv_fill_change_source( change, entry, entry.symbol,
                                          table->symbol_table, pc );
                change.symbol_address = target;
                change.target_address = target;
                riscv_reloc_instruction insn;
                insn.offset        = entry.offset;
                insn.file_offset   = code->get_offset() + entry.offset;
                insn.original_word = original;
                insn.patched_word  = ( original & 0xfff ) | ( hi20 << 12 );
                change.instructions.push_back( insn );
                changes.push_back( change );
            }
            else {
                if ( !instruction_bounds( code, entry.offset, 8 ) ) {
                    return fail( entry.rela_section, entry.rela_entry, true,
                                 "CALL relocation writes outside its code "
                                 "section" );
                }
                const std::uint32_t second =
                    read_word( code_data + entry.offset + 4 );
                if ( instruction_opcode( original ) != 0x17 ||
                     instruction_opcode( second ) != 0x67 ||
                     instruction_funct3( second ) != 0 ) {
                    return fail(
                        entry.rela_section, entry.rela_entry, true,
                        "CALL requires an AUIPC followed by a JALR" );
                }
                if ( instruction_rd( original ) !=
                     instruction_rs1( second ) ) {
                    return fail( entry.rela_section, entry.rela_entry, true,
                                 "CALL AUIPC destination register does not "
                                 "match the JALR base register" );
                }
                riscv_reloc_change change;
                change.kind = riscv_reloc_kind::call;
                change.type = entry.type;
                riscv_fill_change_source( change, entry, entry.symbol,
                                          table->symbol_table, pc );
                change.symbol_address = target;
                change.target_address = target;
                riscv_reloc_instruction first;
                first.offset        = entry.offset;
                first.file_offset   = code->get_offset() + entry.offset;
                first.original_word = original;
                first.patched_word  = ( original & 0xfff ) | ( hi20 << 12 );
                riscv_reloc_instruction next;
                next.offset        = entry.offset + 4;
                next.file_offset   = code->get_offset() + entry.offset + 4;
                next.original_word = second;
                next.patched_word =
                    ( second & ~( 0xfffu << 20 ) ) | ( lo12 << 20 );
                change.instructions.push_back( first );
                change.instructions.push_back( next );
                changes.push_back( change );
            }
        }
        else { // PCREL_LO12_I / PCREL_LO12_S
            // The low record points at the HI anchor symbol, never at the
            // final target and never at its own PC.
            const hi_record* anchor = nullptr;
            for ( const auto& candidate : hi_records ) {
                if ( candidate.code_section == entry.anchor.shndx &&
                     candidate.offset == entry.anchor.value ) {
                    if ( anchor != nullptr ) {
                        return fail( entry.rela_section, entry.rela_entry,
                                     true,
                                     "PCREL_LO12 anchor matches more than one "
                                     "PCREL_HI20 record" );
                    }
                    anchor = &candidate;
                }
            }
            if ( anchor == nullptr ) {
                return fail( entry.rela_section, entry.rela_entry, true,
                             "PCREL_LO12 anchor does not match any "
                             "PCREL_HI20 record" );
            }

            const entry_record* hi_entry = nullptr;
            for ( const auto& candidate : entries ) {
                if ( candidate.rela_section == anchor->rela_section &&
                     candidate.rela_entry == anchor->rela_entry ) {
                    hi_entry = &candidate;
                    break;
                }
            }
            const table_record* hi_table = find_table( anchor->rela_section );
            if ( hi_entry == nullptr || hi_table == nullptr ||
                 !hi_entry->has_symbol ) {
                return fail( entry.rela_section, entry.rela_entry, true,
                             "matched PCREL_HI20 record is incomplete" );
            }

            Elf64_Addr  hi_target = 0;
            std::string message;
            if ( !resolve_symbol_address( hi_entry->symbol, section_bases,
                                          hi_target, message ) ) {
                return fail( anchor->rela_section, anchor->rela_entry, true,
                             message );
            }
            Elf64_Addr hi_pc = 0;
            if ( !add_checked( hi_table->code_base, hi_entry->offset, hi_pc ) ) {
                return fail( anchor->rela_section, anchor->rela_entry, true,
                             "instruction address overflows 64 bits" );
            }

            bool          negative  = false;
            std::uint64_t magnitude = 0;
            if ( !compute_pcrel_delta( hi_target, hi_entry->addend, hi_pc,
                                       negative, magnitude, message ) ) {
                return fail( anchor->rela_section, anchor->rela_entry, true,
                             message );
            }
            std::uint32_t hi20 = 0;
            std::uint32_t lo12 = 0;
            if ( !encode_pcrel_pair( negative, magnitude, hi20, lo12,
                                     message ) ) {
                return fail( anchor->rela_section, anchor->rela_entry, true,
                             message );
            }

            std::uint32_t patched = 0;
            if ( entry.type == R_RISCV_PCREL_LO12_I ) {
                const std::uint32_t opcode = instruction_opcode( original );
                const std::uint32_t funct3 = instruction_funct3( original );
                const bool is_addi = opcode == 0x13 && funct3 == 0;
                const bool is_load = opcode == 0x03 && funct3 <= 5;
                const bool is_jalr = opcode == 0x67 && funct3 == 0;
                if ( !is_addi && !is_load && !is_jalr ) {
                    return fail( entry.rela_section, entry.rela_entry, true,
                                 "PCREL_LO12_I does not point at ADDI, an "
                                 "integer load, or JALR" );
                }
                patched =
                    ( original & ~( 0xfffu << 20 ) ) | ( lo12 << 20 );
            }
            else {
                if ( instruction_opcode( original ) != 0x23 ||
                     instruction_funct3( original ) > 3 ) {
                    return fail( entry.rela_section, entry.rela_entry, true,
                                 "PCREL_LO12_S does not point at an integer "
                                 "store instruction" );
                }
                const std::uint32_t imm_11_5 = ( lo12 >> 5 ) & 0x7f;
                const std::uint32_t imm_4_0  = lo12 & 0x1f;
                patched = ( original & ~( 0x7fu << 25 ) ) |
                          ( imm_11_5 << 25 );
                patched = ( patched & ~( 0x1fu << 7 ) ) | ( imm_4_0 << 7 );
            }

            riscv_reloc_change change;
            change.kind = entry.type == R_RISCV_PCREL_LO12_I
                              ? riscv_reloc_kind::pcrel_lo12_i
                              : riscv_reloc_kind::pcrel_lo12_s;
            change.type = entry.type;
            riscv_fill_change_source( change, entry, entry.symbol,
                                      table->symbol_table, pc );
            Elf64_Addr anchor_address = 0;
            if ( !resolve_symbol_address( entry.anchor, section_bases,
                                          anchor_address, message ) ) {
                return fail( entry.rela_section, entry.rela_entry, true,
                             message );
            }
            change.symbol_address = anchor_address;

            change.has_anchor                = true;
            change.anchor_rela_section       = anchor->rela_section;
            change.anchor_rela_entry         = anchor->rela_entry;
            change.anchor_symbol_table       = hi_table->symbol_table;
            change.anchor_symbol_index       = hi_entry->symbol_index;
            change.anchor_symbol_file_offset = hi_entry->symbol.file_offset;
            change.anchor_symbol_address     = hi_target;
            change.anchor_addend             = hi_entry->addend;
            change.anchor_code_offset        = hi_entry->offset;
            change.anchor_pc                 = hi_pc;
            change.target_address            = hi_target;

            riscv_reloc_instruction insn;
            insn.offset        = entry.offset;
            insn.file_offset   = code->get_offset() + entry.offset;
            insn.original_word = original;
            insn.patched_word  = patched;
            change.instructions.push_back( insn );
            changes.push_back( change );
        }
    }

    //------------------------------------------------------------------
    // Reject two supported records that write the same instruction.
    //------------------------------------------------------------------
    std::map<std::pair<Elf_Half, Elf64_Addr>, std::pair<Elf_Half, Elf_Xword>>
        written;
    for ( const auto& change : changes ) {
        for ( const auto& insn : change.instructions ) {
            const auto key = std::make_pair( change.code_section, insn.offset );
            auto       it  = written.find( key );
            if ( it != written.end() ) {
                return fail( change.rela_section, change.rela_entry, true,
                             "relocation writes the same instruction as "
                             "table " +
                                 std::to_string( it->second.first ) +
                                 " entry " +
                                 std::to_string( it->second.second ) );
            }
            written.emplace( key, std::make_pair( change.rela_section,
                                                  change.rela_entry ) );
        }
    }

    //------------------------------------------------------------------
    // Accept RELAX entries only as hints for an existing main record.
    //------------------------------------------------------------------
    std::vector<riscv_reloc_hint> hints;
    for ( const auto& entry : entries ) {
        if ( entry.type != R_RISCV_RELAX ) {
            continue;
        }
        const riscv_reloc_change* main = nullptr;
        for ( const auto& change : changes ) {
            if ( change.code_section == entry.code_section &&
                 change.code_offset == entry.offset ) {
                main = &change;
                break;
            }
        }
        if ( main == nullptr ) {
            return fail( entry.rela_section, entry.rela_entry, true,
                         "RELAX has no supported main relocation at the same "
                         "position" );
        }
        const table_record* table = find_table( entry.rela_section );
        if ( table == nullptr ) {
            return fail( entry.rela_section, entry.rela_entry, true,
                         "relocation table disappeared" );
        }
        riscv_reloc_hint hint;
        hint.rela_section     = entry.rela_section;
        hint.rela_entry       = entry.rela_entry;
        hint.rela_file_offset = entry.rela_file_offset;
        hint.code_section     = entry.code_section;
        hint.code_offset      = entry.offset;
        hint.code_file_offset = table->code->get_offset() + entry.offset;
        if ( !add_checked( table->code_base, entry.offset, hint.pc ) ) {
            return fail( entry.rela_section, entry.rela_entry, true,
                         "instruction address overflows 64 bits" );
        }
        hint.main_rela_section = main->rela_section;
        hint.main_rela_entry   = main->rela_entry;
        hints.push_back( hint );
    }

    //------------------------------------------------------------------
    // Stable ordering independent of relocation table traversal order.
    //------------------------------------------------------------------
    auto before = []( Elf_Half   code_section_a,
                      Elf64_Addr code_offset_a,
                      Elf_Half   rela_section_a,
                      Elf_Xword  rela_entry_a,
                      Elf_Half   code_section_b,
                      Elf64_Addr code_offset_b,
                      Elf_Half   rela_section_b,
                      Elf_Xword  rela_entry_b ) {
        if ( code_section_a != code_section_b ) {
            return code_section_a < code_section_b;
        }
        if ( code_offset_a != code_offset_b ) {
            return code_offset_a < code_offset_b;
        }
        if ( rela_section_a != rela_section_b ) {
            return rela_section_a < rela_section_b;
        }
        return rela_entry_a < rela_entry_b;
    };
    std::stable_sort( changes.begin(), changes.end(),
                      [&before]( const riscv_reloc_change& a,
                                 const riscv_reloc_change& b ) {
                          return before( a.code_section, a.code_offset,
                                         a.rela_section, a.rela_entry,
                                         b.code_section, b.code_offset,
                                         b.rela_section, b.rela_entry );
                      } );
    std::stable_sort( hints.begin(), hints.end(),
                      [&before]( const riscv_reloc_hint& a,
                                 const riscv_reloc_hint& b ) {
                          return before( a.code_section, a.code_offset,
                                         a.rela_section, a.rela_entry,
                                         b.code_section, b.code_offset,
                                         b.rela_section, b.rela_entry );
                      } );

    //------------------------------------------------------------------
    // Produce independently owned copies of touched sections only.
    //------------------------------------------------------------------
    for ( const auto& change : changes ) {
        const section* code = elf_file.sections[change.code_section];
        if ( code == nullptr ) {
            return fail( change.rela_section, change.rela_entry, true,
                         "code section disappeared" );
        }
        riscv_reloc_output_section* output = nullptr;
        for ( auto& candidate : plan.outputs ) {
            if ( candidate.section == change.code_section ) {
                output = &candidate;
                break;
            }
        }
        if ( output == nullptr ) {
            riscv_reloc_output_section fresh;
            fresh.section   = change.code_section;
            const char* data = code->get_data();
            const auto  size = static_cast<std::size_t>( code->get_size() );
            if ( data == nullptr && size != 0 ) {
                return fail( change.rela_section, change.rela_entry, true,
                             "code section data cannot be read" );
            }
            fresh.data.resize( size );
            if ( size != 0 ) {
                std::memcpy( fresh.data.data(), data, size );
            }
            plan.outputs.push_back( std::move( fresh ) );
            output = &plan.outputs.back();
        }
        for ( const auto& insn : change.instructions ) {
            const std::uint32_t converted =
                ( *convertor )( insn.patched_word );
            std::memcpy( output->data.data() + insn.offset, &converted,
                         sizeof( converted ) );
        }
    }

    plan.ok      = true;
    plan.changes = std::move( changes );
    plan.hints   = std::move( hints );
    return plan;
}

} // namespace ELFIO

#endif // ELFIO_RISCV_RELOC_HPP
