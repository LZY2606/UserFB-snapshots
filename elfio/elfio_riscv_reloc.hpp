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
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ELFIO {

namespace riscv_reloc_detail {

// RISC-V base instruction opcodes used while validating the instruction
// words a relocation is allowed to patch.
constexpr std::uint32_t OPCODE_MASK = 0x7FU;
constexpr std::uint32_t OP_AUIPC     = 0x17U;
constexpr std::uint32_t OP_JALR      = 0x67U;
constexpr std::uint32_t OP_STORE     = 0x23U;
constexpr std::uint32_t OP_OP_IMM    = 0x13U;
constexpr std::uint32_t OP_LOAD      = 0x03U;

//------------------------------------------------------------------------------
//! Sign-extend a value held in the lowest \p bits bits.
inline std::int64_t sign_extend( std::uint64_t value, unsigned bits )
{
    const std::uint64_t sign = std::uint64_t( 1 ) << ( bits - 1 );
    return std::int64_t( ( value ^ sign ) - sign );
}

//------------------------------------------------------------------------------
//! Read a 32-bit instruction word from a little-endian code buffer without
//! relying on strict-alignment accesses.
inline std::uint32_t read_le32( const std::vector<unsigned char>& data,
                                Elf_Xword                          offset )
{
    return std::uint32_t( data[offset] ) |
           ( std::uint32_t( data[offset + 1] ) << 8 ) |
           ( std::uint32_t( data[offset + 2] ) << 16 ) |
           ( std::uint32_t( data[offset + 3] ) << 24 );
}

//------------------------------------------------------------------------------
//! Write a 32-bit instruction word back into a little-endian code buffer.
inline void write_le32( std::vector<unsigned char>& data,
                        Elf_Xword                   offset,
                        std::uint32_t               word )
{
    data[offset]     = static_cast<unsigned char>( word & 0xFFU );
    data[offset + 1] = static_cast<unsigned char>( ( word >> 8 ) & 0xFFU );
    data[offset + 2] = static_cast<unsigned char>( ( word >> 16 ) & 0xFFU );
    data[offset + 3] = static_cast<unsigned char>( ( word >> 24 ) & 0xFFU );
}

//------------------------------------------------------------------------------
inline std::uint32_t opcode_of( std::uint32_t word )
{
    return word & OPCODE_MASK;
}

//------------------------------------------------------------------------------
inline bool is_auipc( std::uint32_t word )
{
    return opcode_of( word ) == OP_AUIPC;
}

//------------------------------------------------------------------------------
inline bool is_jalr( std::uint32_t word )
{
    return opcode_of( word ) == OP_JALR;
}

//------------------------------------------------------------------------------
//! ADDI, any integer load or JALR accept a PCREL_LO12_I relocation.
inline bool is_pcrel_lo_i_candidate( std::uint32_t word )
{
    const std::uint32_t op = opcode_of( word );
    return op == OP_OP_IMM || op == OP_LOAD || op == OP_JALR;
}

//------------------------------------------------------------------------------
//! Every integer store instruction accepts a PCREL_LO12_S relocation.
inline bool is_pcrel_lo_s_candidate( std::uint32_t word )
{
    return opcode_of( word ) == OP_STORE;
}

} // namespace riscv_reloc_detail

//------------------------------------------------------------------------------
//! Kind of a single, already validated relocation change.
enum class riscv_reloc_kind
{
    pcrel_hi20,   //!< R_RISCV_PCREL_HI20 on an AUIPC instruction
    pcrel_lo12_i, //!< R_RISCV_PCREL_LO12_I on an I-type instruction
    pcrel_lo12_s, //!< R_RISCV_PCREL_LO12_S on an S-type instruction
    call          //!< R_RISCV_CALL on an AUIPC/JALR pair
};

//------------------------------------------------------------------------------
//! \class riscv_reloc_change
//! \brief One concrete instruction-word rewrite produced by the planner.
//!
//! Every object describes a single 4-byte instruction word inside one output
//! code section. CALL relocations produce two changes, one for the AUIPC and
//! one for the JALR. R_RISCV_NONE and R_RISCV_RELAX never produce a change.
class riscv_reloc_change
{
  public:
    riscv_reloc_kind kind = riscv_reloc_kind::pcrel_hi20;

    Elf_Half   reloc_section_index = 0; //!< Index of the source RELA table
    Elf_Xword  reloc_entry_index   = 0; //!< Entry position inside that table
    Elf_Half   code_section_index  = 0; //!< Index of the patched code section
    Elf64_Addr code_offset         = 0; //!< Offset of the patched word

    //! For low-part and CALL/JALR changes: offset of the AUIPC holding the
    //! matching high part. Equals code_offset for high-part changes.
    Elf64_Addr high_offset = 0;

    std::uint32_t original_word = 0; //!< Instruction word before patching
    std::uint32_t patched_word  = 0; //!< Instruction word after patching
};

//------------------------------------------------------------------------------
//! \class riscv_reloc_section
//! \brief Independently owned output bytes for one relocated code section.
class riscv_reloc_section
{
  public:
    Elf_Half                        section_index = 0; //!< Original index
    std::vector<unsigned char>      data;              //!< Relocated bytes
    std::vector<riscv_reloc_change> changes;           //!< Changes in data
};

//------------------------------------------------------------------------------
//! \class riscv_reloc_plan
//! \brief The full result of a successful relocation planning run.
class riscv_reloc_plan
{
  public:
    //! Output sections, ordered by their original section index. Only code
    //! sections carrying at least one active relocation are present; no
    //! other section gets an output copy.
    std::vector<riscv_reloc_section> sections;

    //! Every change across all output sections, in a stable order that does
    //! not depend on section-table traversal (see make_riscv_reloc_plan()).
    std::vector<riscv_reloc_change> changes;
};

namespace riscv_reloc_detail {

//------------------------------------------------------------------------------
//! Everything remembered about one raw RELA entry while the plan is built.
struct raw_entry
{
    Elf_Half    table_index;
    Elf_Xword   entry_index;
    Elf64_Addr  offset;
    Elf_Word    symbol;
    unsigned    type;
    Elf_Sxword  addend;
    Elf_Half    code_index;
};

//------------------------------------------------------------------------------
//! The high-part information shared by an AUIPC and all of its low parts.
struct high_part
{
    raw_entry    entry;
    std::int64_t delta = 0;   //!< D = S + A - P
    bool         is_call = false; //!< CALL pairs, not valid LO12 anchors
};

//------------------------------------------------------------------------------
//! Owned output buffer for one relocated code section.
struct code_buffer
{
    Elf_Half                   index = 0;
    std::vector<unsigned char> data;
};

//------------------------------------------------------------------------------
//! \class planner
//! \brief All state and validation for a single make_riscv_reloc_plan() call.
class planner
{
  public:
    planner( const elfio& elf_file,
             const std::vector<Elf64_Addr>& bases )
        : elf( elf_file ), base_addresses( bases )
    {
    }

    //------------------------------------------------------------------------------
    //! Build the plan. On failure nothing is published and \p error carries a
    //! message naming the relocation table index and entry position.
    bool run( riscv_reloc_plan& plan_out, std::string& error )
    {
        if ( !validate_environment( error ) ) {
            return false;
        }
        if ( !collect_entries( error ) ) {
            return false;
        }
        if ( entries.empty() ) {
            return true; // Nothing to relocate: an empty plan is valid.
        }
        if ( !resolve_high_parts( error ) ) {
            return false;
        }
        if ( !validate_low_parts_and_hints( error ) ) {
            return false;
        }
        if ( !apply_and_verify( error ) ) {
            return false;
        }
        emit( plan_out );
        return true;
    }

  private:
    const elfio&                    elf;
    const std::vector<Elf64_Addr>&  base_addresses;

    std::vector<raw_entry> entries;
    // Key: (code section index, AUIPC offset inside that section).
    std::map<std::pair<Elf_Half, Elf64_Addr>, high_part> high_parts;
    std::vector<Elf_Half>                                active_code_indices;
    std::map<Elf_Half, code_buffer>                      published_buffers;
    std::vector<riscv_reloc_change>                      published_changes;

    //------------------------------------------------------------------------------
    static std::string location( Elf_Half table, Elf_Xword entry )
    {
        return "relocation section " + std::to_string( table ) + " entry " +
               std::to_string( entry );
    }

    //------------------------------------------------------------------------------
    static std::string at( const raw_entry& e, const std::string& what )
    {
        return "riscv reloc: " + location( e.table_index, e.entry_index ) +
               ": " + what;
    }

    //------------------------------------------------------------------------------
    bool fail( const raw_entry& e, const std::string& what,
               std::string& error )
    {
        error = at( e, what );
        return false;
    }

    //------------------------------------------------------------------------------
    bool validate_environment( std::string& error )
    {
        if ( elf.get_class() != ELFCLASS64 ) {
            error = "riscv reloc: only ELFCLASS64 objects are supported";
            return false;
        }
        if ( elf.get_encoding() != ELFDATA2LSB ) {
            error = "riscv reloc: only little-endian objects are supported";
            return false;
        }
        if ( elf.get_machine() != EM_RISCV ) {
            error = "riscv reloc: object machine is not EM_RISCV";
            return false;
        }
        if ( elf.get_type() != ET_REL ) {
            error = "riscv reloc: only ET_REL objects are supported";
            return false;
        }
        return true;
    }

    //------------------------------------------------------------------------------
    //! Walk every relocation table in section-index order and remember each
    //! entry, resolving the table's symbol table (sh_link) and code section
    //! (sh_info) on the way. Section names never participate.
    bool collect_entries( std::string& error )
    {
        const Elf_Half section_count = elf.sections.size();

        for ( Elf_Half s = 0; s < section_count; ++s ) {
            const section* rel = elf.sections[s];
            if ( rel->get_type() != SHT_RELA &&
                 rel->get_type() != SHT_REL ) {
                continue;
            }
            if ( rel->get_type() == SHT_REL ) {
                error = "riscv reloc: relocation section " +
                        std::to_string( s ) +
                        " uses SHT_REL, which RISC-V does not use and is "
                        "not supported";
                return false;
            }

            const Elf_Word link = rel->get_link();
            const Elf_Word info = rel->get_info();

            if ( link == 0 || link >= section_count ) {
                error = "riscv reloc: relocation section " +
                        std::to_string( s ) +
                        " has an out-of-range sh_link symbol table index " +
                        std::to_string( link );
                return false;
            }
            if ( info == 0 || info >= section_count ) {
                error = "riscv reloc: relocation section " +
                        std::to_string( s ) +
                        " has an out-of-range sh_info section index " +
                        std::to_string( info );
                return false;
            }

            const section* symtab = elf.sections[link];
            if ( symtab->get_type() != SHT_SYMTAB ) {
                error = "riscv reloc: relocation section " +
                        std::to_string( s ) + " sh_link section " +
                        std::to_string( link ) + " is not a symbol table";
                return false;
            }

            const section* code = elf.sections[info];
            if ( code->get_type() != SHT_PROGBITS ||
                 ( code->get_flags() & ( SHF_ALLOC | SHF_EXECINSTR ) ) !=
                     ( SHF_ALLOC | SHF_EXECINSTR ) ) {
                error = "riscv reloc: relocation section " +
                        std::to_string( s ) +
                        " targets a section that is not an allocated "
                        "executable PROGBITS section";
                return false;
            }

            const Elf_Xword entry_size = rel->get_entry_size();
            if ( entry_size < sizeof( Elf64_Rela ) ) {
                error = "riscv reloc: " + location( s, 0 ) +
                        ": relocation entry size is too small (truncated "
                        "table)";
                return false;
            }
            // A trailing partial entry must be reported instead of silently
            // dropped: get_entries_num() would floor the division.
            if ( rel->get_size() % entry_size != 0 ) {
                const Elf_Xword bad = rel->get_size() / entry_size;
                error = "riscv reloc: " + location( s, bad ) +
                        ": truncated relocation entry";
                return false;
            }

            const_relocation_section_accessor relacc( elf, rel );
            const Elf_Xword count = relacc.get_entries_num();
            for ( Elf_Xword i = 0; i < count; ++i ) {
                Elf64_Addr  offset = 0;
                Elf_Word    symbol = 0;
                unsigned    type   = 0;
                Elf_Sxword  addend = 0;
                if ( !relacc.get_entry( i, offset, symbol, type, addend ) ) {
                    error = "riscv reloc: " + location( s, i ) +
                            ": entry could not be decoded (truncated entry)";
                    return false;
                }

                raw_entry e{ s, i, offset, symbol, type, addend,
                             static_cast<Elf_Half>( info ) };
                if ( !validate_entry_bounds( e, code, error ) ) {
                    return false;
                }
                entries.push_back( e );
            }
        }
        return true;
    }

    //------------------------------------------------------------------------------
    //! Bounds and alignment checks that do not need symbol resolution.
    bool validate_entry_bounds( const raw_entry& e, const section* code,
                                std::string& error )
    {
        if ( e.type == R_RISCV_NONE || e.type == R_RISCV_RELAX ) {
            return true; // Hint entries never write bytes.
        }

        constexpr Elf_Xword INSN_SIZE = 4;
        const Elf_Xword span =
            e.type == R_RISCV_CALL ? 2 * INSN_SIZE : INSN_SIZE;

        if ( e.offset % INSN_SIZE != 0 ) {
            return fail(
                e,
                "misaligned relocation offset; only uncompressed "
                "instructions are supported",
                error );
        }
        if ( e.offset > code->get_size() ||
             e.offset + span > code->get_size() ) {
            return fail( e, "relocation writes outside of its code section",
                         error );
        }
        return true;
    }

    //------------------------------------------------------------------------------
    //! Validate a symbol index against the table's own sh_link symbol table
    //! and return the decoded st_value / st_shndx.
    bool get_symbol( const raw_entry& e, const section* symtab,
                     Elf64_Addr& value, Elf_Half& shndx,
                     std::string& error )
    {
        const_symbol_section_accessor symacc( elf, symtab );
        const Elf_Xword sym_count = symacc.get_symbols_num();
        if ( e.symbol >= sym_count ) {
            return fail( e,
                         "invalid symbol index " +
                             std::to_string( e.symbol ) +
                             " for the table's sh_link symbol table",
                         error );
        }

        std::string   name;
        Elf_Xword     size  = 0;
        unsigned char bind  = 0;
        unsigned char stype = 0;
        unsigned char other = 0;
        if ( !symacc.get_symbol( e.symbol, name, value, size, bind, stype,
                                 shndx, other ) ) {
            return fail( e, "symbol entry could not be decoded", error );
        }
        return true;
    }

    //------------------------------------------------------------------------------
    //! Address of an ordinary defined symbol: section base + st_value.
    //! SHN_ABS symbols take their value directly.
    bool symbol_address( const raw_entry& e, const section* symtab,
                         Elf64_Addr& address, std::string& error )
    {
        Elf64_Addr value = 0;
        Elf_Half   shndx = 0;
        if ( !get_symbol( e, symtab, value, shndx, error ) ) {
            return false;
        }

        if ( shndx == SHN_UNDEF ) {
            return fail( e, "symbol is undefined (SHN_UNDEF)", error );
        }
        if ( shndx == SHN_COMMON ) {
            return fail( e, "common symbols cannot be relocated offline",
                         error );
        }
        if ( shndx == SHN_XINDEX ||
             ( shndx >= SHN_LORESERVE && shndx != SHN_ABS ) ) {
            return fail( e,
                         "symbol uses a reserved/extended section index "
                         "which is not supported",
                         error );
        }
        if ( shndx == SHN_ABS ) {
            address = value;
            return true;
        }

        if ( shndx >= elf.sections.size() ) {
            return fail( e,
                         "symbol section index " + std::to_string( shndx ) +
                             " is out of range",
                         error );
        }
        if ( shndx >= base_addresses.size() ) {
            return fail( e,
                         "missing base address for symbol section " +
                             std::to_string( shndx ),
                         error );
        }

        const Elf64_Addr base = base_addresses[shndx];
        if ( value > std::numeric_limits<Elf64_Addr>::max() - base ) {
            return fail( e, "symbol address calculation overflows", error );
        }
        address = base + value;
        return true;
    }

    //------------------------------------------------------------------------------
    //! Resolve and validate every PCREL_HI20 / CALL high part first.
    bool resolve_high_parts( std::string& error )
    {
        for ( const raw_entry& e : entries ) {
            switch ( e.type ) {
            case R_RISCV_NONE:
            case R_RISCV_RELAX:
            case R_RISCV_PCREL_LO12_I:
            case R_RISCV_PCREL_LO12_S:
                break; // Handled in later passes.

            case R_RISCV_PCREL_HI20:
                if ( !register_high_part( e, false, error ) ) {
                    return false;
                }
                break;

            case R_RISCV_CALL:
                if ( !register_high_part( e, true, error ) ) {
                    return false;
                }
                break;

            default:
                return fail(
                    e,
                    "unsupported RISC-V relocation type " +
                        std::to_string( e.type ) +
                        " (supported: NONE, RELAX, PCREL_HI20, "
                        "PCREL_LO12_I, PCREL_LO12_S, CALL)",
                    error );
            }
        }
        return true;
    }

    //------------------------------------------------------------------------------
    //! Shared code for PCREL_HI20 and CALL; CALL additionally validates the
    //! following JALR and the register linkage between the two words.
    bool register_high_part( const raw_entry& e, bool is_call,
                             std::string& error )
    {
        const section* symtab = elf.sections[
            elf.sections[e.table_index]->get_link()];
        const section* code = elf.sections[e.code_index];

        if ( e.code_index >= base_addresses.size() ) {
            return fail( e,
                         "missing base address for code section " +
                             std::to_string( e.code_index ),
                         error );
        }

        Elf64_Addr s_address = 0;
        if ( !symbol_address( e, symtab, s_address, error ) ) {
            return false;
        }

        const Elf64_Addr base = base_addresses[e.code_index];
        if ( e.offset > std::numeric_limits<Elf64_Addr>::max() - base ) {
            return fail( e, "AUIPC address calculation overflows", error );
        }
        const Elf64_Addr p_address = base + e.offset;

        std::int64_t delta = 0;
        if ( !compute_delta( e, s_address, e.addend, p_address, delta,
                             error ) ) {
            return false;
        }

        const std::uint32_t auipc_word = read_code_word( code, e.offset );
        if ( !is_auipc( auipc_word ) ) {
            return fail( e,
                         is_call
                             ? "CALL relocation does not start with AUIPC"
                             : "PCREL_HI20 relocation target is not AUIPC",
                         error );
        }

        if ( is_call ) {
            const std::uint32_t jalr_word =
                read_code_word( code, e.offset + 4 );
            if ( !is_jalr( jalr_word ) ) {
                return fail( e,
                             "CALL relocation second instruction is not JALR",
                             error );
            }
            // AUIPC rd must feed JALR rs1; every other field is preserved.
            const std::uint32_t auipc_rd = ( auipc_word >> 7 ) & 0x1FU;
            const std::uint32_t jalr_rs1 = ( jalr_word >> 15 ) & 0x1FU;
            if ( auipc_rd != jalr_rs1 ) {
                return fail(
                    e,
                    "CALL AUIPC destination register does not match JALR "
                    "base register",
                    error );
            }
        }

        const auto key = std::make_pair( e.code_index, e.offset );
        if ( high_parts.find( key ) != high_parts.end() ) {
            return fail( e,
                         "two relocations modify the same AUIPC "
                         "instruction",
                         error );
        }

        high_part hp;
        hp.entry   = e;
        hp.delta   = delta;
        hp.is_call = is_call;
        high_parts.emplace( key, hp );
        return true;
    }

    //------------------------------------------------------------------------------
    //! Compute D = S + A - P using fully-defined unsigned 64-bit arithmetic,
    //! rejecting anything that does not fit a signed 64-bit displacement.
    bool compute_delta( const raw_entry& e, Elf64_Addr s,
                        Elf_Sxword a, Elf64_Addr p,
                        std::int64_t& delta, std::string& error )
    {
        constexpr Elf64_Addr UMAX =
            std::numeric_limits<Elf64_Addr>::max();
        constexpr Elf64_Addr SMAX =
            static_cast<Elf64_Addr>(
                std::numeric_limits<std::int64_t>::max() );

        Elf64_Addr sum = 0;
        if ( a >= 0 ) {
            const Elf64_Addr aa = static_cast<Elf64_Addr>( a );
            if ( s > UMAX - aa ) {
                return fail( e, "S + A address calculation overflows",
                             error );
            }
            sum = s + aa;
        }
        else {
            // Negate in the unsigned domain so INT64_MIN stays defined.
            const Elf64_Addr neg =
                Elf64_Addr( 0 ) - static_cast<Elf64_Addr>( a );
            if ( s < neg ) {
                return fail( e, "S + A address calculation overflows",
                             error );
            }
            sum = s - neg;
        }

        if ( sum >= p ) {
            const Elf64_Addr d = sum - p;
            if ( d > SMAX ) {
                return fail( e,
                             "S + A - P displacement overflows", error );
            }
            delta = static_cast<std::int64_t>( d );
        }
        else {
            const Elf64_Addr d = p - sum;
            // The most negative signed value is 2^63, i.e. SMAX + 1.
            if ( d > SMAX + 1 ) {
                return fail( e,
                             "S + A - P displacement overflows", error );
            }
            // Defined negation even for INT64_MIN (d == 2^63).
            delta = -static_cast<std::int64_t>( d - 1 ) - 1;
        }
        return true;
    }

    //------------------------------------------------------------------------------
    static std::uint32_t read_code_word( const section* code,
                                         Elf64_Addr     offset )
    {
        const auto* raw =
            reinterpret_cast<const unsigned char*>( code->get_data() );
        return std::uint32_t( raw[offset] ) |
               ( std::uint32_t( raw[offset + 1] ) << 8 ) |
               ( std::uint32_t( raw[offset + 2] ) << 16 ) |
               ( std::uint32_t( raw[offset + 3] ) << 24 );
    }

    //------------------------------------------------------------------------------
    //! Validate every low-part relocation and RELAX hint against the
    //! already-resolved high parts.
    bool validate_low_parts_and_hints( std::string& error )
    {
        for ( const raw_entry& e : entries ) {
            if ( e.type == R_RISCV_RELAX ) {
                if ( !validate_relax( e, error ) ) {
                    return false;
                }
            }
            else if ( e.type == R_RISCV_PCREL_LO12_I ||
                      e.type == R_RISCV_PCREL_LO12_S ) {
                if ( !validate_low_part( e, error ) ) {
                    return false;
                }
            }
        }

        for ( const raw_entry& e : entries ) {
            if ( e.type != R_RISCV_NONE && e.type != R_RISCV_RELAX &&
                 std::find( active_code_indices.begin(),
                            active_code_indices.end(),
                            e.code_index ) ==
                     active_code_indices.end() ) {
                active_code_indices.push_back( e.code_index );
            }
        }
        std::sort( active_code_indices.begin(), active_code_indices.end() );
        return true;
    }

    //------------------------------------------------------------------------------
    //! A RELAX hint is only valid where a supported active relocation
    //! targets the same code section and offset. It never writes a byte and
    //! never changes any instruction length.
    bool validate_relax( const raw_entry& e, std::string& error )
    {
        for ( const raw_entry& other : entries ) {
            if ( other.type == R_RISCV_NONE ||
                 other.type == R_RISCV_RELAX ) {
                continue;
            }
            if ( other.code_index == e.code_index &&
                 other.offset == e.offset ) {
                return true;
            }
        }
        return fail(
            e,
            "R_RISCV_RELAX has no supported primary relocation at the "
            "same offset",
            error );
    }

    //------------------------------------------------------------------------------
    //! Resolve the anchor symbol of a low-part relocation to exactly one
    //! high-part AUIPC, matching by physical section identity and st_value
    //! (the anchor labels the AUIPC). Neither symbol names nor entry
    //! adjacency are used.
    bool validate_low_part( const raw_entry& e, std::string& error )
    {
        if ( e.addend != 0 ) {
            return fail( e,
                         "PCREL_LO12 relocations must carry a zero addend",
                         error );
        }

        const section* symtab = elf.sections[
            elf.sections[e.table_index]->get_link()];
        const section* code = elf.sections[e.code_index];

        Elf64_Addr anchor_value = 0;
        Elf_Half   anchor_shndx = 0;
        if ( !get_symbol( e, symtab, anchor_value, anchor_shndx, error ) ) {
            return false;
        }
        if ( anchor_shndx == SHN_UNDEF || anchor_shndx == SHN_COMMON ||
             anchor_shndx == SHN_XINDEX ||
             ( anchor_shndx >= SHN_LORESERVE && anchor_shndx != SHN_ABS ) ) {
            return fail( e,
                         "low-part anchor symbol has an unsupported "
                         "section index",
                         error );
        }
        if ( anchor_shndx == SHN_ABS ) {
            return fail(
                e,
                "low-part anchor must label an AUIPC in a code section, "
                "not SHN_ABS",
                error );
        }
        if ( anchor_shndx != e.code_index ) {
            return fail(
                e,
                "low-part anchor must live in the same code section as "
                "the low-part instruction",
                error );
        }

        const std::uint32_t low_word = read_code_word( code, e.offset );
        if ( e.type == R_RISCV_PCREL_LO12_I ) {
            if ( !is_pcrel_lo_i_candidate( low_word ) ) {
                return fail(
                    e,
                    "PCREL_LO12_I target is not ADDI, an integer load "
                    "or JALR",
                    error );
            }
        }
        else if ( !is_pcrel_lo_s_candidate( low_word ) ) {
            return fail( e,
                         "PCREL_LO12_S target is not an integer store",
                         error );
        }

        // The anchor's (section, value) is the AUIPC location; high_parts is
        // keyed by exactly that physical identity. CALL pairs are not legal
        // LO12 anchors and are excluded.
        const auto  key   = std::make_pair( anchor_shndx, anchor_value );
        const auto  found = high_parts.find( key );
        if ( found == high_parts.end() || found->second.is_call ) {
            return fail(
                e,
                "no PCREL_HI20 AUIPC matching the anchor symbol was found",
                error );
        }
        return true;
    }

    //------------------------------------------------------------------------------
    //! Apply every active relocation to private buffers, recording each word
    //! rewrite. Nothing is published until all rewrites have succeeded, so a
    //! failure never leaves a half-built plan behind.
    bool apply_and_verify( std::string& error )
    {
        std::map<Elf_Half, code_buffer> buffers;
        for ( Elf_Half idx : active_code_indices ) {
            const section* code = elf.sections[idx];
            code_buffer    b;
            b.index = idx;
            b.data.assign(
                reinterpret_cast<const unsigned char*>( code->get_data() ),
                reinterpret_cast<const unsigned char*>( code->get_data() ) +
                    code->get_size() );
            buffers.emplace( idx, std::move( b ) );
        }

        // Word positions already rewritten, to reject two active
        // relocations touching the same instruction.
        std::map<std::pair<Elf_Half, Elf64_Addr>, raw_entry> written;
        std::vector<riscv_reloc_change> changes;

        for ( const raw_entry& e : entries ) {
            if ( e.type == R_RISCV_NONE || e.type == R_RISCV_RELAX ) {
                continue;
            }

            if ( e.type == R_RISCV_PCREL_HI20 ||
                 e.type == R_RISCV_CALL ) {
                const high_part& hp =
                    high_parts.at( std::make_pair( e.code_index, e.offset ) );

                if ( !claim_word( written, e, e.offset, error ) ) {
                    return false;
                }
                std::uint32_t original =
                    read_le32( buffers.at( e.code_index ).data, e.offset );
                std::uint32_t patched = 0;
                if ( !encode_auipc( e, hp.delta, original, patched,
                                    error ) ) {
                    return false;
                }
                write_le32( buffers.at( e.code_index ).data, e.offset,
                            patched );
                changes.push_back( make_change(
                    e.type == R_RISCV_CALL ? riscv_reloc_kind::call
                                           : riscv_reloc_kind::pcrel_hi20,
                    e, e.offset, e.offset, original, patched ) );

                if ( e.type == R_RISCV_CALL ) {
                    if ( !claim_word( written, e, e.offset + 4, error ) ) {
                        return false;
                    }
                    std::uint32_t jalr_orig = read_le32(
                        buffers.at( e.code_index ).data, e.offset + 4 );
                    std::uint32_t jalr_patched = 0;
                    if ( !encode_i_type( e, hp.delta, jalr_orig,
                                         jalr_patched, error ) ) {
                        return false;
                    }
                    write_le32( buffers.at( e.code_index ).data,
                                e.offset + 4, jalr_patched );
                    changes.push_back( make_change(
                        riscv_reloc_kind::call, e, e.offset + 4, e.offset,
                        jalr_orig, jalr_patched ) );
                }
            }
            else {
                const section* symtab = elf.sections[
                    elf.sections[e.table_index]->get_link()];
                Elf64_Addr anchor_value = 0;
                Elf_Half   anchor_shndx = 0;
                if ( !get_symbol( e, symtab, anchor_value, anchor_shndx,
                                  error ) ) {
                    return false;
                }
                const high_part& hp = high_parts.at(
                    std::make_pair( anchor_shndx, anchor_value ) );

                if ( !claim_word( written, e, e.offset, error ) ) {
                    return false;
                }
                std::uint32_t original =
                    read_le32( buffers.at( e.code_index ).data, e.offset );
                std::uint32_t patched = 0;
                if ( e.type == R_RISCV_PCREL_LO12_I ) {
                    if ( !encode_i_type( e, hp.delta, original, patched,
                                         error ) ) {
                        return false;
                    }
                }
                else if ( !encode_s_type( e, hp.delta, original, patched,
                                          error ) ) {
                    return false;
                }
                write_le32( buffers.at( e.code_index ).data, e.offset,
                            patched );
                changes.push_back( make_change(
                    e.type == R_RISCV_PCREL_LO12_I
                        ? riscv_reloc_kind::pcrel_lo12_i
                        : riscv_reloc_kind::pcrel_lo12_s,
                    e, e.offset, hp.entry.offset, original, patched ) );
            }
        }

        published_buffers = std::move( buffers );
        published_changes = std::move( changes );
        return true;
    }

    //------------------------------------------------------------------------------
    bool claim_word(
        std::map<std::pair<Elf_Half, Elf64_Addr>, raw_entry>& written,
        const raw_entry& e, Elf64_Addr offset, std::string& error )
    {
        const auto key   = std::make_pair( e.code_index, offset );
        const auto found = written.find( key );
        if ( found != written.end() ) {
            return fail(
                e,
                "conflicts with " +
                    location( found->second.table_index,
                              found->second.entry_index ) +
                    " modifying the same instruction",
                error );
        }
        written.emplace( key, e );
        return true;
    }

    //------------------------------------------------------------------------------
    //! AUIPC carries bits 31:12 of D, folding in the carry contributed by
    //! the signed low 12 bits. The sign-extended pair must reconstruct D
    //! exactly; truncating D to 32 bits is never accepted.
    bool encode_auipc( const raw_entry& e, std::int64_t delta,
                       std::uint32_t original, std::uint32_t& patched,
                       std::string& error )
    {
        if ( delta <
                 std::int64_t(
                     std::numeric_limits<std::int32_t>::min() ) ||
             delta >
                 std::int64_t(
                     std::numeric_limits<std::int32_t>::max() ) ) {
            return fail(
                e,
                "pc-relative displacement does not fit in AUIPC plus a "
                "signed 12-bit low part",
                error );
        }

        const std::int64_t low = sign_extend(
            static_cast<std::uint64_t>( delta ) & 0xFFFU, 12 );
        // delta - low is a multiple of 4096, so plain division is exact and
        // avoids any implementation-defined right-shift behavior.
        const std::int64_t high = ( delta - low ) / 4096;

        // Reconstruction check: sign-extended high part plus sign-extended
        // low part must equal D bit-for-bit. Multiplication (not a signed
        // left shift) keeps the negative high case fully defined.
        if ( high * 4096 + low != delta || high < -0x80000 ||
             high > 0x7FFFF ) {
            return fail(
                e,
                "pc-relative displacement cannot be represented by the "
                "HI20/LO12 pair",
                error );
        }

        const std::uint32_t imm20 =
            static_cast<std::uint32_t>( high ) & 0xFFFFFU;
        // AUIPC: only bits 31:12 change; rd and opcode (low 12) stay.
        patched = ( original & 0xFFFU ) | ( imm20 << 12 );
        return true;
    }

    //------------------------------------------------------------------------------
    //! I-type (ADDI/load/JALR): replace bits 31:20 only.
    bool encode_i_type( const raw_entry& e, std::int64_t delta,
                        std::uint32_t original, std::uint32_t& patched,
                        std::string& error )
    {
        const std::int64_t low = sign_extend(
            static_cast<std::uint64_t>( delta ) & 0xFFFU, 12 );
        if ( low < -2048 || low > 2047 ) {
            return fail( e,
                         "low-part immediate is outside the signed "
                         "12-bit range",
                         error );
        }
        const std::uint32_t imm12 =
            static_cast<std::uint32_t>( low ) & 0xFFFU;
        // I-type: imm[31:20] changes; rs1, funct3, rd and opcode (19:0)
        // are preserved.
        patched = ( original & 0x000FFFFFU ) | ( imm12 << 20 );
        return true;
    }

    //------------------------------------------------------------------------------
    //! S-type (integer store): split the signed 12-bit immediate across
    //! bits 31:25 and 11:7; every other field is preserved.
    bool encode_s_type( const raw_entry& e, std::int64_t delta,
                        std::uint32_t original, std::uint32_t& patched,
                        std::string& error )
    {
        const std::int64_t low = sign_extend(
            static_cast<std::uint64_t>( delta ) & 0xFFFU, 12 );
        if ( low < -2048 || low > 2047 ) {
            return fail( e,
                         "low-part immediate is outside the signed "
                         "12-bit range",
                         error );
        }
        const std::uint32_t imm12    =
            static_cast<std::uint32_t>( low ) & 0xFFFU;
        const std::uint32_t imm11_5 = ( imm12 >> 5 ) & 0x7FU;
        const std::uint32_t imm4_0  = imm12 & 0x1FU;

        patched = original & 0x01FFF07FU;
        patched |= imm11_5 << 25;
        patched |= imm4_0 << 7;
        return true;
    }

    //------------------------------------------------------------------------------
    static riscv_reloc_change
    make_change( riscv_reloc_kind kind, const raw_entry& e,
                 Elf64_Addr code_offset, Elf64_Addr high_offset,
                 std::uint32_t original, std::uint32_t patched )
    {
        riscv_reloc_change c;
        c.kind                = kind;
        c.reloc_section_index = e.table_index;
        c.reloc_entry_index   = e.entry_index;
        c.code_section_index  = e.code_index;
        c.code_offset         = code_offset;
        c.high_offset         = high_offset;
        c.original_word       = original;
        c.patched_word        = patched;
        return c;
    }

    //------------------------------------------------------------------------------
    //! Assemble the public plan in a stable order.
    void emit( riscv_reloc_plan& plan_out )
    {
        std::stable_sort(
            published_changes.begin(), published_changes.end(),
            []( const riscv_reloc_change& a,
                const riscv_reloc_change& b ) {
                if ( a.code_section_index != b.code_section_index ) {
                    return a.code_section_index < b.code_section_index;
                }
                if ( a.code_offset != b.code_offset ) {
                    return a.code_offset < b.code_offset;
                }
                if ( a.reloc_section_index != b.reloc_section_index ) {
                    return a.reloc_section_index < b.reloc_section_index;
                }
                return a.reloc_entry_index < b.reloc_entry_index;
            } );

        for ( Elf_Half idx : active_code_indices ) {
            riscv_reloc_section out;
            out.section_index = idx;
            out.data          = published_buffers.at( idx ).data;
            for ( const riscv_reloc_change& c : published_changes ) {
                if ( c.code_section_index == idx ) {
                    out.changes.push_back( c );
                }
            }
            plan_out.sections.push_back( std::move( out ) );
        }
        plan_out.changes = std::move( published_changes );
    }
};

} // namespace riscv_reloc_detail

//------------------------------------------------------------------------------
//! Build an offline RISC-V relocation plan for an unlinked object file.
//!
//! \param elf_file       A loaded ELFCLASS64, little-endian, EM_RISCV, ET_REL
//!                       object. The object is never modified.
//! \param base_addresses One physical base address per section index, i.e.
//!                       base_addresses[section_index] is the address where
//!                       that section is placed. Every section referenced by
//!                       an active relocation, directly or through a symbol,
//!                       must have an entry.
//! \param plan           Receives the independently owned output bytes and
//!                       per-change descriptions. Left untouched on failure.
//! \return Error message, empty on success.
//!
//! Supported relocations: R_RISCV_PCREL_HI20, R_RISCV_PCREL_LO12_I,
//! R_RISCV_PCREL_LO12_S and R_RISCV_CALL, plus R_RISCV_NONE and
//! R_RISCV_RELAX as recognized, non-writing entries. No linker is invoked
//! and no instruction is executed.
inline std::string
make_riscv_reloc_plan( const elfio&                   elf_file,
                       const std::vector<Elf64_Addr>& base_addresses,
                       riscv_reloc_plan&              plan )
{
    riscv_reloc_detail::planner p( elf_file, base_addresses );
    std::string                 error;
    if ( !p.run( plan, error ) ) {
        return error;
    }
    return {};
}

} // namespace ELFIO

#endif // ELFIO_RISCV_RELOC_HPP
