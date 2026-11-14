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

// Task test cases for the offline RISC-V relocation planner.
// The minimal ET_REL objects are built entirely in C++; every instruction
// word below is hand-written and every comparison goes through TASK_CHECK.

#include "task_support/check.h"

#include <elfio/elfio.hpp>

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace ELFIO;

namespace {

//------------------------------------------------------------------------------
// Hand-written RISC-V instruction word builders (uncompressed, RV32/RV64).
std::uint32_t insn_auipc( std::uint32_t rd )
{
    return ( rd << 7 ) | 0x17U;
}
std::uint32_t insn_addi( std::uint32_t rd, std::uint32_t rs1 )
{
    return ( rd << 7 ) | ( rs1 << 15 ) | 0x13U;
}
std::uint32_t insn_lw( std::uint32_t rd, std::uint32_t rs1 )
{
    return ( rd << 7 ) | ( rs1 << 15 ) | ( 2U << 12 ) | 0x03U;
}
std::uint32_t insn_sw( std::uint32_t rs2, std::uint32_t rs1 )
{
    return ( rs2 << 20 ) | ( rs1 << 15 ) | ( 2U << 12 ) | 0x23U;
}
std::uint32_t insn_jalr( std::uint32_t rd, std::uint32_t rs1 )
{
    return ( rd << 7 ) | ( rs1 << 15 ) | 0x67U;
}

//------------------------------------------------------------------------------
std::int64_t sign_ext( std::uint64_t value, unsigned bits )
{
    const std::uint64_t sign = std::uint64_t( 1 ) << ( bits - 1 );
    return std::int64_t( ( value ^ sign ) - sign );
}

// Independent, test-side encoders used to derive the expected words.
std::uint32_t expect_auipc( std::uint32_t original, std::int64_t delta )
{
    const std::int64_t lo   = sign_ext( delta & 0xFFFU, 12 );
    const std::int64_t high = ( delta - lo ) / 4096;
    return ( original & 0xFFFU ) |
           ( static_cast<std::uint32_t>( high ) & 0xFFFFFU ) << 12;
}
std::uint32_t expect_i_type( std::uint32_t original, std::int64_t delta )
{
    const std::int64_t lo = sign_ext( delta & 0xFFFU, 12 );
    return ( original & 0x000FFFFFU ) |
           ( static_cast<std::uint32_t>( lo ) & 0xFFFU ) << 20;
}
std::uint32_t expect_s_type( std::uint32_t original, std::int64_t delta )
{
    const std::int64_t  lo  = sign_ext( delta & 0xFFFU, 12 );
    const std::uint32_t imm = static_cast<std::uint32_t>( lo ) & 0xFFFU;
    return ( original & 0x01FFF07FU ) | ( ( ( imm >> 5 ) & 0x7FU ) << 25 ) |
           ( ( imm & 0x1FU ) << 7 );
}

// Decode the immediates back out of patched words, the way a disassembler
// would, so reconstruction can be checked against D.
std::int64_t decode_auipc_imm( std::uint32_t word )
{
    return sign_ext( ( word >> 12 ) & 0xFFFFFU, 20 ) * 4096;
}
std::int64_t decode_i_imm( std::uint32_t word )
{
    return sign_ext( ( word >> 20 ) & 0xFFFU, 12 );
}
std::int64_t decode_s_imm( std::uint32_t word )
{
    const std::uint32_t imm =
        ( ( word >> 25 ) & 0x7FU ) << 5 | ( ( word >> 7 ) & 0x1FU );
    return sign_ext( imm, 12 );
}

//------------------------------------------------------------------------------
std::uint32_t read_word( const riscv_reloc_section& out, Elf64_Addr offset )
{
    return std::uint32_t( out.data[offset] ) |
           ( std::uint32_t( out.data[offset + 1] ) << 8 ) |
           ( std::uint32_t( out.data[offset + 2] ) << 16 ) |
           ( std::uint32_t( out.data[offset + 3] ) << 24 );
}

const riscv_reloc_change*
find_change( const riscv_reloc_plan& plan, Elf_Half section,
             Elf64_Addr offset, riscv_reloc_kind kind )
{
    for ( const auto& c : plan.changes ) {
        if ( c.code_section_index == section && c.code_offset == offset &&
             c.kind == kind ) {
            return &c;
        }
    }
    return nullptr;
}

bool mentions( const std::string& error, Elf_Half table, Elf_Xword entry )
{
    return error.find( "section " + std::to_string( table ) + " entry " +
                       std::to_string( entry ) ) != std::string::npos;
}

//------------------------------------------------------------------------------
// Minimal RISC-V ET_REL builder.
struct ObjBuilder
{
    elfio w;

    ObjBuilder()
    {
        w.create( ELFCLASS64, ELFDATA2LSB );
        w.set_type( ET_REL );
        w.set_machine( EM_RISCV );
    }

    section* code( const char* name,
                   const std::vector<std::uint32_t>& words )
    {
        section* s = w.sections.add( name );
        s->set_type( SHT_PROGBITS );
        s->set_flags( SHF_ALLOC | SHF_EXECINSTR );
        s->set_addr_align( 4 );
        std::vector<unsigned char> bytes;
        bytes.reserve( words.size() * 4 );
        for ( std::uint32_t word : words ) {
            for ( int k = 0; k < 4; ++k ) {
                bytes.push_back(
                    static_cast<unsigned char>( ( word >> ( 8 * k ) ) & 0xFFU ) );
            }
        }
        s->set_data( reinterpret_cast<const char*>( bytes.data() ),
                     bytes.size() );
        return s;
    }

    section* rodata( const char* name, std::size_t size )
    {
        section* s = w.sections.add( name );
        s->set_type( SHT_PROGBITS );
        s->set_flags( SHF_ALLOC );
        s->set_addr_align( 4 );
        const std::string zeros( size, '\0' );
        s->set_data( zeros );
        return s;
    }

    section* new_symtab( const char* sym_name, const char* str_name,
                         section*& strtab_out )
    {
        strtab_out = w.sections.add( str_name );
        strtab_out->set_type( SHT_STRTAB );

        section* sy = w.sections.add( sym_name );
        sy->set_type( SHT_SYMTAB );
        sy->set_link( strtab_out->get_index() );
        sy->set_info( 1 );
        sy->set_addr_align( 8 );
        sy->set_entry_size( w.get_default_entry_size( SHT_SYMTAB ) );
        return sy;
    }

    Elf_Word symbol( section* symtab, section* strtab, const char* name,
                     Elf64_Addr value, Elf_Half shndx,
                     unsigned char bind = STB_LOCAL,
                     unsigned char type = STT_NOTYPE )
    {
        string_section_accessor strings( strtab );
        symbol_section_accessor symbols( w, symtab );
        return symbols.add_symbol( strings, name, value, 0, bind, type, 0,
                                   shndx );
    }

    section* rela( const char* name, section* target_code,
                   section* symtab )
    {
        section* r = w.sections.add( name );
        r->set_type( SHT_RELA );
        r->set_info( target_code->get_index() );
        r->set_link( symtab->get_index() );
        r->set_addr_align( 8 );
        r->set_entry_size( w.get_default_entry_size( SHT_RELA ) );
        return r;
    }

    void add_reloc( section* rel, Elf64_Addr offset, Elf_Word sym,
                    unsigned type, Elf_Sxword addend = 0 )
    {
        relocation_section_accessor acc( w, rel );
        acc.add_entry( offset, sym, type, addend );
    }

    int reload( std::unique_ptr<elfio>& reader )
    {
        auto stream = std::make_shared<std::stringstream>(
            std::ios::in | std::ios::out | std::ios::binary );
        TASK_CHECK( w.save( *stream ) );
        reader = std::make_unique<elfio>();
        stream->seekg( 0 );
        TASK_CHECK( reader->load( *stream ) );
        (void)stream;
        return 0;
    }
};

std::vector<Elf64_Addr>
make_bases( const elfio& reader,
            std::initializer_list<std::pair<unsigned, Elf64_Addr>> values )
{
    std::vector<Elf64_Addr> bases( reader.sections.size(), 0 );
    for ( const auto& kv : values ) {
        bases[kv.first] = kv.second;
    }
    return bases;
}

constexpr Elf64_Addr TEXT_BASE = 0x10000;
constexpr Elf64_Addr RODATA_BASE = 0x80000;

} // namespace

//------------------------------------------------------------------------------
// pairs group: high/low pairing semantics
//------------------------------------------------------------------------------

// LO12 listed before HI20 in the same table: pairing must not rely on order.
static int pairs_out_of_order()
{
    ObjBuilder b;
    section* text = b.code( ".text", { insn_auipc( 1 ), insn_addi( 2, 1 ) } );
    section* strtab = nullptr;
    section* symtab = b.new_symtab( ".symtab", ".strtab", strtab );
    const Elf_Word anchor =
        b.symbol( symtab, strtab, ".Lanchor", 0, text->get_index() );
    const Elf_Word target =
        b.symbol( symtab, strtab, "target", 0x1800, SHN_ABS, STB_GLOBAL,
                  STT_OBJECT );
    section* rel = b.rela( ".rela.text", text, symtab );
    b.add_reloc( rel, 4, anchor, R_RISCV_PCREL_LO12_I ); // low first
    b.add_reloc( rel, 0, target, R_RISCV_PCREL_HI20 );   // high after

    std::unique_ptr<elfio> reader;
    TASK_CHECK( b.reload( reader ) == 0 );
    auto bases  = make_bases( *reader,
                             { { text->get_index(), TEXT_BASE } } );
    riscv_reloc_plan plan;
    const std::string error =
        make_riscv_reloc_plan( *reader, bases, plan );
    TASK_CHECK( error.empty() );
    TASK_CHECK( plan.sections.size() == 1 );
    TASK_CHECK( plan.changes.size() == 2 );

    const auto* hi = find_change( plan, text->get_index(), 0,
                                  riscv_reloc_kind::pcrel_hi20 );
    const auto* lo = find_change( plan, text->get_index(), 4,
                                  riscv_reloc_kind::pcrel_lo12_i );
    TASK_CHECK( hi != nullptr );
    TASK_CHECK( lo != nullptr );
    TASK_CHECK( hi->high_offset == 0 );
    TASK_CHECK( lo->high_offset == 0 );
    TASK_CHECK( hi->reloc_section_index == rel->get_index() );
    TASK_CHECK( hi->reloc_entry_index == 1 );
    TASK_CHECK( lo->reloc_entry_index == 0 );
    return 0;
}

// The low and high entries live in different RELA tables with different
// sh_link symbol tables; anchor identity is (section, st_value), not name.
static int pairs_cross_table()
{
    ObjBuilder b;
    section* text = b.code( ".text", { insn_auipc( 1 ), insn_addi( 2, 1 ) } );

    section* str_a = nullptr;
    section* sym_a = b.new_symtab( ".symtab.a", ".strtab.a", str_a );
    section* str_b = nullptr;
    section* sym_b = b.new_symtab( ".symtab.b", ".strtab.b", str_b );

    // Different anchor names on purpose: names must not drive pairing.
    const Elf_Word anchor_a =
        b.symbol( sym_a, str_a, "anchor_from_a", 0, text->get_index() );
    const Elf_Word target_b =
        b.symbol( sym_b, str_b, "target", 0x2000, SHN_ABS, STB_GLOBAL,
                  STT_OBJECT );

    section* rel_a = b.rela( ".rela.text.a", text, sym_a );
    section* rel_b = b.rela( ".rela.text.b", text, sym_b );
    b.add_reloc( rel_a, 4, anchor_a, R_RISCV_PCREL_LO12_I );
    b.add_reloc( rel_b, 0, target_b, R_RISCV_PCREL_HI20 );

    std::unique_ptr<elfio> reader;
    TASK_CHECK( b.reload( reader ) == 0 );
    auto bases  = make_bases( *reader,
                             { { text->get_index(), TEXT_BASE } } );
    riscv_reloc_plan plan;
    const std::string error =
        make_riscv_reloc_plan( *reader, bases, plan );
    TASK_CHECK( error.empty() );

    const auto* lo = find_change( plan, text->get_index(), 4,
                                  riscv_reloc_kind::pcrel_lo12_i );
    TASK_CHECK( lo != nullptr );
    TASK_CHECK( lo->high_offset == 0 );
    TASK_CHECK( lo->reloc_section_index == rel_a->get_index() );

    const std::int64_t delta =
        static_cast<std::int64_t>( 0x2000 ) -
        static_cast<std::int64_t>( TEXT_BASE );
    TASK_CHECK( decode_auipc_imm( plan.sections[0].data.size()
                                      ? read_word( plan.sections[0], 0 )
                                      : 0 ) +
                        decode_i_imm( read_word( plan.sections[0], 4 ) ) ==
                    delta );
    return 0;
}

// One high part serves several low parts, including an I-type and an S-type.
static int pairs_shared_high()
{
    ObjBuilder b;
    section* text = b.code( ".text",
                            { insn_auipc( 1 ), insn_addi( 2, 1 ),
                              insn_lw( 3, 1 ), insn_sw( 4, 1 ) } );
    section* strtab = nullptr;
    section* symtab = b.new_symtab( ".symtab", ".strtab", strtab );
    const Elf_Word anchor =
        b.symbol( symtab, strtab, ".La", 0, text->get_index() );
    const Elf_Word target =
        b.symbol( symtab, strtab, "target", 0x3800, SHN_ABS, STB_GLOBAL,
                  STT_OBJECT );
    section* rel = b.rela( ".rela.text", text, symtab );
    b.add_reloc( rel, 0, target, R_RISCV_PCREL_HI20 );
    b.add_reloc( rel, 4, anchor, R_RISCV_PCREL_LO12_I );
    b.add_reloc( rel, 8, anchor, R_RISCV_PCREL_LO12_I );
    b.add_reloc( rel, 12, anchor, R_RISCV_PCREL_LO12_S );

    std::unique_ptr<elfio> reader;
    TASK_CHECK( b.reload( reader ) == 0 );
    auto bases  = make_bases( *reader,
                             { { text->get_index(), TEXT_BASE } } );
    riscv_reloc_plan plan;
    TASK_CHECK( make_riscv_reloc_plan( *reader, bases, plan ).empty() );
    TASK_CHECK( plan.changes.size() == 4 );

    for ( const auto& c : plan.changes ) {
        if ( c.kind != riscv_reloc_kind::pcrel_hi20 ) {
            TASK_CHECK( c.high_offset == 0 );
        }
    }
    const auto* store = find_change( plan, text->get_index(), 12,
                                     riscv_reloc_kind::pcrel_lo12_s );
    TASK_CHECK( store != nullptr );
    return 0;
}

// Two anchors share a name but live at different offsets; each low part must
// pair with the high part identified by section identity and st_value.
static int pairs_same_name_distinct()
{
    ObjBuilder b;
    section* text = b.code( ".text",
                            { insn_auipc( 1 ), insn_addi( 2, 1 ),
                              insn_auipc( 3 ), insn_lw( 5, 3 ) } );
    section* strtab = nullptr;
    section* symtab = b.new_symtab( ".symtab", ".strtab", strtab );
    const Elf_Word anchor_0 =
        b.symbol( symtab, strtab, ".Lsame", 0, text->get_index() );
    const Elf_Word anchor_8 =
        b.symbol( symtab, strtab, ".Lsame", 8, text->get_index() );
    const Elf_Word target_0 =
        b.symbol( symtab, strtab, "t0", 0x4000, SHN_ABS, STB_GLOBAL,
                  STT_OBJECT );
    const Elf_Word target_8 =
        b.symbol( symtab, strtab, "t1", 0x5000, SHN_ABS, STB_GLOBAL,
                  STT_OBJECT );
    section* rel = b.rela( ".rela.text", text, symtab );
    b.add_reloc( rel, 0, target_0, R_RISCV_PCREL_HI20 );
    b.add_reloc( rel, 8, target_8, R_RISCV_PCREL_HI20 );
    b.add_reloc( rel, 4, anchor_0, R_RISCV_PCREL_LO12_I );
    b.add_reloc( rel, 12, anchor_8, R_RISCV_PCREL_LO12_I );

    std::unique_ptr<elfio> reader;
    TASK_CHECK( b.reload( reader ) == 0 );
    auto bases  = make_bases( *reader,
                             { { text->get_index(), TEXT_BASE } } );
    riscv_reloc_plan plan;
    TASK_CHECK( make_riscv_reloc_plan( *reader, bases, plan ).empty() );

    const auto* lo_0 = find_change( plan, text->get_index(), 4,
                                    riscv_reloc_kind::pcrel_lo12_i );
    const auto* lo_8 = find_change( plan, text->get_index(), 12,
                                    riscv_reloc_kind::pcrel_lo12_i );
    TASK_CHECK( lo_0 != nullptr && lo_8 != nullptr );
    TASK_CHECK( lo_0->high_offset == 0 );
    TASK_CHECK( lo_8->high_offset == 8 );
    return 0;
}

// An anchor whose offset has no HI20 must be rejected with table and entry.
static int pairs_anchor_missing()
{
    ObjBuilder b;
    section* text = b.code( ".text", { insn_auipc( 1 ), insn_addi( 2, 1 ) } );
    section* strtab = nullptr;
    section* symtab = b.new_symtab( ".symtab", ".strtab", strtab );
    const Elf_Word anchor =
        b.symbol( symtab, strtab, ".Lorphan", 8, text->get_index() );
    section* rel = b.rela( ".rela.text", text, symtab );
    b.add_reloc( rel, 4, anchor, R_RISCV_PCREL_LO12_I );

    std::unique_ptr<elfio> reader;
    TASK_CHECK( b.reload( reader ) == 0 );
    auto bases  = make_bases( *reader,
                             { { text->get_index(), TEXT_BASE } } );
    riscv_reloc_plan plan;
    const std::string error =
        make_riscv_reloc_plan( *reader, bases, plan );
    TASK_CHECK( !error.empty() );
    TASK_CHECK( mentions( error, rel->get_index(), 0 ) );
    TASK_CHECK( plan.sections.empty() );
    return 0;
}

// The anchor must be defined in the same code section as the low instruction.
static int pairs_anchor_wrong_section()
{
    ObjBuilder b;
    section* text   = b.code( ".text", { insn_auipc( 1 ),
                                         insn_addi( 2, 1 ) } );
    section* other  = b.code( ".text2", { insn_auipc( 6 ) } );
    section* strtab = nullptr;
    section* symtab = b.new_symtab( ".symtab", ".strtab", strtab );
    const Elf_Word anchor =
        b.symbol( symtab, strtab, ".Lforeign", 0, other->get_index() );
    section* rel = b.rela( ".rela.text", text, symtab );
    b.add_reloc( rel, 4, anchor, R_RISCV_PCREL_LO12_I );

    std::unique_ptr<elfio> reader;
    TASK_CHECK( b.reload( reader ) == 0 );
    auto bases  = make_bases( *reader,
                             { { text->get_index(), TEXT_BASE },
                               { other->get_index(), 0x20000 } } );
    riscv_reloc_plan plan;
    const std::string error =
        make_riscv_reloc_plan( *reader, bases, plan );
    TASK_CHECK( !error.empty() );
    TASK_CHECK( mentions( error, rel->get_index(), 0 ) );
    return 0;
}

// LO12 addends other than zero are not accepted.
static int pairs_lo_addend_nonzero()
{
    ObjBuilder b;
    section* text = b.code( ".text", { insn_auipc( 1 ), insn_addi( 2, 1 ) } );
    section* strtab = nullptr;
    section* symtab = b.new_symtab( ".symtab", ".strtab", strtab );
    const Elf_Word anchor =
        b.symbol( symtab, strtab, ".La", 0, text->get_index() );
    const Elf_Word target =
        b.symbol( symtab, strtab, "t", 0x1000, SHN_ABS, STB_GLOBAL,
                  STT_OBJECT );
    section* rel = b.rela( ".rela.text", text, symtab );
    b.add_reloc( rel, 0, target, R_RISCV_PCREL_HI20 );
    b.add_reloc( rel, 4, anchor, R_RISCV_PCREL_LO12_I, 4 );

    std::unique_ptr<elfio> reader;
    TASK_CHECK( b.reload( reader ) == 0 );
    auto bases  = make_bases( *reader,
                             { { text->get_index(), TEXT_BASE } } );
    riscv_reloc_plan plan;
    const std::string error =
        make_riscv_reloc_plan( *reader, bases, plan );
    TASK_CHECK( !error.empty() );
    TASK_CHECK( mentions( error, rel->get_index(), 1 ) );
    return 0;
}

//------------------------------------------------------------------------------
// fields group: bit-level encoding and instruction validation
//------------------------------------------------------------------------------

// Low twelve bits around the 0x7ff/0x800 boundary, including carry into HI20.
static int fields_boundary_values()
{
    struct Probe
    {
        Elf64_Addr target;
        Elf64_Addr pc;
    };
    const Probe probes[] = {
        { 0x337FF, 0x10000 }, // D = 0x237ff -> low 0x7ff, high 0x23
        { 0x33800, 0x10000 }, // D = 0x23800 -> low -0x800, high 0x24
        { 0x0FFFE, 0x10000 }  // D = -0x2    -> low -2, high 0
    };

    for ( const Probe& probe : probes ) {
        ObjBuilder b;
        section* text =
            b.code( ".text", { insn_auipc( 1 ), insn_addi( 2, 1 ) } );
        section* strtab = nullptr;
        section* symtab = b.new_symtab( ".symtab", ".strtab", strtab );
        const Elf_Word anchor =
            b.symbol( symtab, strtab, ".La", 0, text->get_index() );
        const Elf_Word target = b.symbol( symtab, strtab, "t",
                                          probe.target, SHN_ABS, STB_GLOBAL,
                                          STT_OBJECT );
        section* rel = b.rela( ".rela.text", text, symtab );
        b.add_reloc( rel, 0, target, R_RISCV_PCREL_HI20 );
        b.add_reloc( rel, 4, anchor, R_RISCV_PCREL_LO12_I );

        std::unique_ptr<elfio> reader;
        TASK_CHECK( b.reload( reader ) == 0 );
        auto bases  = make_bases( *reader,
                                 { { text->get_index(), probe.pc } } );
        riscv_reloc_plan plan;
        TASK_CHECK( make_riscv_reloc_plan( *reader, bases, plan ).empty() );

        const std::int64_t delta =
            static_cast<std::int64_t>( probe.target ) -
            static_cast<std::int64_t>( probe.pc );
        const std::uint32_t hi_word = read_word( plan.sections[0], 0 );
        const std::uint32_t lo_word = read_word( plan.sections[0], 4 );
        TASK_CHECK( hi_word ==
                    expect_auipc( insn_auipc( 1 ), delta ) );
        TASK_CHECK( lo_word ==
                    expect_i_type( insn_addi( 2, 1 ), delta ) );
        TASK_CHECK( decode_auipc_imm( hi_word ) +
                            decode_i_imm( lo_word ) ==
                        delta );
    }
    return 0;
}

// Negative displacements must sign-extend through both instruction words.
static int fields_negative_displacement()
{
    const Elf64_Addr targets[] = { 0xEFFE, 0xF7FF, 0x0F800 };
    // Deltas: -0x1002, -0x801, -0x800
    for ( Elf64_Addr target : targets ) {
        ObjBuilder b;
        section* text =
            b.code( ".text", { insn_auipc( 1 ), insn_addi( 2, 1 ) } );
        section* strtab = nullptr;
        section* symtab = b.new_symtab( ".symtab", ".strtab", strtab );
        const Elf_Word anchor =
            b.symbol( symtab, strtab, ".La", 0, text->get_index() );
        const Elf_Word tsym =
            b.symbol( symtab, strtab, "t", target, SHN_ABS, STB_GLOBAL,
                      STT_OBJECT );
        section* rel = b.rela( ".rela.text", text, symtab );
        b.add_reloc( rel, 0, tsym, R_RISCV_PCREL_HI20 );
        b.add_reloc( rel, 4, anchor, R_RISCV_PCREL_LO12_I );

        std::unique_ptr<elfio> reader;
        TASK_CHECK( b.reload( reader ) == 0 );
        auto bases  = make_bases( *reader,
                                 { { text->get_index(), TEXT_BASE } } );
        riscv_reloc_plan plan;
        TASK_CHECK( make_riscv_reloc_plan( *reader, bases, plan ).empty() );

        const std::int64_t delta =
            static_cast<std::int64_t>( target ) -
            static_cast<std::int64_t>( TEXT_BASE );
        const std::uint32_t hi_word = read_word( plan.sections[0], 0 );
        const std::uint32_t lo_word = read_word( plan.sections[0], 4 );
        TASK_CHECK( hi_word ==
                    expect_auipc( insn_auipc( 1 ), delta ) );
        TASK_CHECK( lo_word ==
                    expect_i_type( insn_addi( 2, 1 ), delta ) );
        TASK_CHECK( decode_auipc_imm( hi_word ) +
                            decode_i_imm( lo_word ) ==
                        delta );
    }
    return 0;
}

// S-type: the immediate is split across 31:25 and 11:7; all other fields
// (rs2, rs1, funct3, opcode) survive bit-for-bit.
static int fields_s_type_bitfields()
{
    ObjBuilder b;
    const std::uint32_t store = insn_sw( 4, 1 );
    section* text =
        b.code( ".text", { insn_auipc( 1 ), store } );
    section* strtab = nullptr;
    section* symtab = b.new_symtab( ".symtab", ".strtab", strtab );
    const Elf_Word anchor =
        b.symbol( symtab, strtab, ".La", 0, text->get_index() );
    const Elf_Word target =
        b.symbol( symtab, strtab, "t", 0x12ABC, SHN_ABS, STB_GLOBAL,
                  STT_OBJECT );
    section* rel = b.rela( ".rela.text", text, symtab );
    b.add_reloc( rel, 0, target, R_RISCV_PCREL_HI20 );
    b.add_reloc( rel, 4, anchor, R_RISCV_PCREL_LO12_S );

    std::unique_ptr<elfio> reader;
    TASK_CHECK( b.reload( reader ) == 0 );
    auto bases  = make_bases( *reader,
                             { { text->get_index(), TEXT_BASE } } );
    riscv_reloc_plan plan;
    TASK_CHECK( make_riscv_reloc_plan( *reader, bases, plan ).empty() );

    const std::int64_t delta =
        static_cast<std::int64_t>( 0x12ABC ) -
        static_cast<std::int64_t>( TEXT_BASE );
    const std::uint32_t patched = read_word( plan.sections[0], 4 );
    TASK_CHECK( patched == expect_s_type( store, delta ) );

    // Non-immediate fields preserved: rs2 (31:20 region outside imm),
    // rs1 (19:15), funct3 (14:12) and opcode (6:0).
    TASK_CHECK( ( patched & 0x000F8000U ) == ( store & 0x000F8000U ) ); // rs1
    TASK_CHECK( ( patched & 0x00007000U ) == ( store & 0x00007000U ) ); // funct3
    TASK_CHECK( ( patched & 0x0000007FU ) == ( store & 0x0000007FU ) ); // opcode
    TASK_CHECK( ( ( patched >> 20 ) & 0x1FU ) == 4 );                   // rs2
    TASK_CHECK( decode_auipc_imm( read_word( plan.sections[0], 0 ) ) +
                        decode_s_imm( patched ) ==
                    delta );
    return 0;
}

// I-type relocations on ADDI, a load and JALR keep rd, rs1 and funct3 intact.
static int fields_i_type_preservation()
{
    ObjBuilder b;
    section* text = b.code(
        ".text",
        { insn_auipc( 1 ), insn_addi( 2, 1 ), insn_lw( 3, 1 ),
          insn_jalr( 7, 1 ) } );
    section* strtab = nullptr;
    section* symtab = b.new_symtab( ".symtab", ".strtab", strtab );
    const Elf_Word anchor =
        b.symbol( symtab, strtab, ".La", 0, text->get_index() );
    const Elf_Word target =
        b.symbol( symtab, strtab, "t", 0x777, SHN_ABS, STB_GLOBAL,
                  STT_OBJECT );
    section* rel = b.rela( ".rela.text", text, symtab );
    b.add_reloc( rel, 0, target, R_RISCV_PCREL_HI20 );
    b.add_reloc( rel, 4, anchor, R_RISCV_PCREL_LO12_I );
    b.add_reloc( rel, 8, anchor, R_RISCV_PCREL_LO12_I );
    b.add_reloc( rel, 12, anchor, R_RISCV_PCREL_LO12_I );

    std::unique_ptr<elfio> reader;
    TASK_CHECK( b.reload( reader ) == 0 );
    auto bases  = make_bases( *reader,
                             { { text->get_index(), TEXT_BASE } } );
    riscv_reloc_plan plan;
    TASK_CHECK( make_riscv_reloc_plan( *reader, bases, plan ).empty() );

    const std::int64_t delta =
        static_cast<std::int64_t>( 0x777 ) -
        static_cast<std::int64_t>( TEXT_BASE );
    const std::uint32_t originals[] = { insn_addi( 2, 1 ), insn_lw( 3, 1 ),
                                        insn_jalr( 7, 1 ) };
    const Elf64_Addr offsets[] = { 4, 8, 12 };
    for ( int i = 0; i < 3; ++i ) {
        const std::uint32_t patched =
            read_word( plan.sections[0], offsets[i] );
        TASK_CHECK( patched ==
                    expect_i_type( originals[i], delta ) );
        // Bits 19:0 (rd, funct3, rs1, opcode) are untouched.
        TASK_CHECK( ( patched & 0x000FFFFFU ) ==
                    ( originals[i] & 0x000FFFFFU ) );
    }
    return 0;
}

// AUIPC only rewrites 31:12; rd and opcode (low 12 bits) are preserved.
static int fields_auipc_rd_preserved()
{
    ObjBuilder b;
    const std::uint32_t auipc = insn_auipc( 31 );
    section* text = b.code( ".text", { auipc, insn_addi( 2, 31 ) } );
    section* strtab = nullptr;
    section* symtab = b.new_symtab( ".symtab", ".strtab", strtab );
    const Elf_Word anchor =
        b.symbol( symtab, strtab, ".La", 0, text->get_index() );
    const Elf_Word target =
        b.symbol( symtab, strtab, "t", 0xABCDE, SHN_ABS, STB_GLOBAL,
                  STT_OBJECT );
    section* rel = b.rela( ".rela.text", text, symtab );
    b.add_reloc( rel, 0, target, R_RISCV_PCREL_HI20 );
    b.add_reloc( rel, 4, anchor, R_RISCV_PCREL_LO12_I );

    std::unique_ptr<elfio> reader;
    TASK_CHECK( b.reload( reader ) == 0 );
    auto bases  = make_bases( *reader,
                             { { text->get_index(), TEXT_BASE } } );
    riscv_reloc_plan plan;
    TASK_CHECK( make_riscv_reloc_plan( *reader, bases, plan ).empty() );

    const std::int64_t delta =
        static_cast<std::int64_t>( 0xABCDE ) -
        static_cast<std::int64_t>( TEXT_BASE );
    const std::uint32_t patched = read_word( plan.sections[0], 0 );
    TASK_CHECK( patched == expect_auipc( auipc, delta ) );
    TASK_CHECK( ( patched & 0xFFFU ) == ( auipc & 0xFFFU ) );
    return 0;
}

// Displacements that cannot fit the HI20/LO12 pair are rejected, and no
// partial plan is published.
static int fields_out_of_range_rejected()
{
    const Elf64_Addr targets[] = {
        TEXT_BASE + 0x80000000ULL,       // delta  +2^31
        TEXT_BASE - 0x80000001ULL        // delta  -2^31 - 1
    };
    for ( Elf64_Addr target : targets ) {
        ObjBuilder b;
        section* text =
            b.code( ".text", { insn_auipc( 1 ), insn_addi( 2, 1 ) } );
        section* strtab = nullptr;
        section* symtab = b.new_symtab( ".symtab", ".strtab", strtab );
        const Elf_Word anchor =
            b.symbol( symtab, strtab, ".La", 0, text->get_index() );
        const Elf_Word tsym =
            b.symbol( symtab, strtab, "t", target, SHN_ABS, STB_GLOBAL,
                      STT_OBJECT );
        section* rel = b.rela( ".rela.text", text, symtab );
        b.add_reloc( rel, 0, tsym, R_RISCV_PCREL_HI20 );
        b.add_reloc( rel, 4, anchor, R_RISCV_PCREL_LO12_I );

        std::unique_ptr<elfio> reader;
        TASK_CHECK( b.reload( reader ) == 0 );
        auto bases  = make_bases( *reader,
                                 { { text->get_index(), TEXT_BASE } } );
        riscv_reloc_plan plan;
        const std::string error =
            make_riscv_reloc_plan( *reader, bases, plan );
        TASK_CHECK( !error.empty() );
        TASK_CHECK( plan.sections.empty() );
        TASK_CHECK( plan.changes.empty() );
    }
    return 0;
}

// CALL patches a consecutive AUIPC/JALR pair using the first PC and keeps
// the destination and base registers of both words.
static int fields_call_pair()
{
    ObjBuilder b;
    const std::uint32_t auipc = insn_auipc( 1 );
    const std::uint32_t jalr  = insn_jalr( 7, 1 );
    section* text =
        b.code( ".text", { auipc, jalr, insn_auipc( 1 ), insn_addi( 2, 1 ) } );
    section* rod  = b.rodata( ".rodata", 16 );
    section* strtab = nullptr;
    section* symtab = b.new_symtab( ".symtab", ".strtab", strtab );
    const Elf_Word call_target =
        b.symbol( symtab, strtab, "fn", 0x40, rod->get_index(), STB_GLOBAL,
                  STT_FUNC );
    const Elf_Word anchor =
        b.symbol( symtab, strtab, ".La", 8, text->get_index() );
    const Elf_Word abs_target =
        b.symbol( symtab, strtab, "data", 0x9000, SHN_ABS, STB_GLOBAL,
                  STT_OBJECT );
    section* rel = b.rela( ".rela.text", text, symtab );
    b.add_reloc( rel, 0, call_target, R_RISCV_CALL );
    b.add_reloc( rel, 8, abs_target, R_RISCV_PCREL_HI20 );
    b.add_reloc( rel, 12, anchor, R_RISCV_PCREL_LO12_I );

    std::unique_ptr<elfio> reader;
    TASK_CHECK( b.reload( reader ) == 0 );
    auto bases  = make_bases( *reader,
                             { { text->get_index(), TEXT_BASE },
                               { rod->get_index(), RODATA_BASE } } );
    riscv_reloc_plan plan;
    TASK_CHECK( make_riscv_reloc_plan( *reader, bases, plan ).empty() );

    const std::int64_t call_delta =
        static_cast<std::int64_t>( RODATA_BASE + 0x40 ) -
        static_cast<std::int64_t>( TEXT_BASE );
    const std::uint32_t hi_word = read_word( plan.sections[0], 0 );
    const std::uint32_t lo_word = read_word( plan.sections[0], 4 );
    TASK_CHECK( hi_word == expect_auipc( auipc, call_delta ) );
    TASK_CHECK( lo_word == expect_i_type( jalr, call_delta ) );

    const auto* hi_change = find_change( plan, text->get_index(), 0,
                                         riscv_reloc_kind::call );
    const auto* lo_change = find_change( plan, text->get_index(), 4,
                                         riscv_reloc_kind::call );
    TASK_CHECK( hi_change != nullptr && lo_change != nullptr );
    TASK_CHECK( hi_change->original_word == auipc );
    TASK_CHECK( lo_change->original_word == jalr );
    TASK_CHECK( lo_change->high_offset == 0 );
    // JALR rd (x7), rs1 (x1), funct3 and opcode survive.
    TASK_CHECK( ( lo_word & 0x000FFFFFU ) == ( jalr & 0x000FFFFFU ) );
    TASK_CHECK( decode_auipc_imm( hi_word ) +
                        decode_i_imm( lo_word ) ==
                    call_delta );
    return 0;
}

// AUIPC rd must feed JALR rs1.
static int fields_call_register_mismatch()
{
    ObjBuilder b;
    section* text = b.code( ".text",
                            { insn_auipc( 1 ), insn_jalr( 7, 2 ) } );
    section* rod  = b.rodata( ".rodata", 8 );
    section* strtab = nullptr;
    section* symtab = b.new_symtab( ".symtab", ".strtab", strtab );
    const Elf_Word fn =
        b.symbol( symtab, strtab, "fn", 0, rod->get_index(), STB_GLOBAL,
                  STT_FUNC );
    section* rel = b.rela( ".rela.text", text, symtab );
    b.add_reloc( rel, 0, fn, R_RISCV_CALL );

    std::unique_ptr<elfio> reader;
    TASK_CHECK( b.reload( reader ) == 0 );
    auto bases  = make_bases( *reader,
                             { { text->get_index(), TEXT_BASE },
                               { rod->get_index(), RODATA_BASE } } );
    riscv_reloc_plan plan;
    const std::string error =
        make_riscv_reloc_plan( *reader, bases, plan );
    TASK_CHECK( !error.empty() );
    TASK_CHECK( mentions( error, rel->get_index(), 0 ) );
    return 0;
}

// The second CALL instruction must be JALR.
static int fields_call_second_not_jalr()
{
    ObjBuilder b;
    section* text = b.code( ".text",
                            { insn_auipc( 1 ), insn_addi( 7, 1 ) } );
    section* rod  = b.rodata( ".rodata", 8 );
    section* strtab = nullptr;
    section* symtab = b.new_symtab( ".symtab", ".strtab", strtab );
    const Elf_Word fn =
        b.symbol( symtab, strtab, "fn", 0, rod->get_index(), STB_GLOBAL,
                  STT_FUNC );
    section* rel = b.rela( ".rela.text", text, symtab );
    b.add_reloc( rel, 0, fn, R_RISCV_CALL );

    std::unique_ptr<elfio> reader;
    TASK_CHECK( b.reload( reader ) == 0 );
    auto bases  = make_bases( *reader,
                             { { text->get_index(), TEXT_BASE },
                               { rod->get_index(), RODATA_BASE } } );
    riscv_reloc_plan plan;
    const std::string error =
        make_riscv_reloc_plan( *reader, bases, plan );
    TASK_CHECK( !error.empty() );
    TASK_CHECK( mentions( error, rel->get_index(), 0 ) );
    return 0;
}

// Instruction opcode suitability is enforced.
static int fields_wrong_opcodes_rejected()
{
    {
        ObjBuilder b;
        section* text =
            b.code( ".text", { insn_addi( 1, 0 ), insn_addi( 2, 1 ) } );
        section* strtab = nullptr;
        section* symtab = b.new_symtab( ".symtab", ".strtab", strtab );
        const Elf_Word target =
            b.symbol( symtab, strtab, "t", 0x1000, SHN_ABS, STB_GLOBAL,
                      STT_OBJECT );
        section* rel = b.rela( ".rela.text", text, symtab );
        b.add_reloc( rel, 0, target, R_RISCV_PCREL_HI20 );

        std::unique_ptr<elfio> reader;
        TASK_CHECK( b.reload( reader ) == 0 );
        auto bases  = make_bases( *reader,
                                 { { text->get_index(), TEXT_BASE } } );
        riscv_reloc_plan plan;
        TASK_CHECK( !make_riscv_reloc_plan( *reader, bases, plan ).empty() );
    }
    {
        ObjBuilder b;
        section* text =
            b.code( ".text", { insn_auipc( 1 ), insn_addi( 2, 1 ) } );
        section* strtab = nullptr;
        section* symtab = b.new_symtab( ".symtab", ".strtab", strtab );
        const Elf_Word anchor =
            b.symbol( symtab, strtab, ".La", 0, text->get_index() );
        const Elf_Word target =
            b.symbol( symtab, strtab, "t", 0x1000, SHN_ABS, STB_GLOBAL,
                      STT_OBJECT );
        section* rel = b.rela( ".rela.text", text, symtab );
        b.add_reloc( rel, 0, target, R_RISCV_PCREL_HI20 );
        b.add_reloc( rel, 4, anchor, R_RISCV_PCREL_LO12_S ); // addi, not store

        std::unique_ptr<elfio> reader;
        TASK_CHECK( b.reload( reader ) == 0 );
        auto bases  = make_bases( *reader,
                                 { { text->get_index(), TEXT_BASE } } );
        riscv_reloc_plan plan;
        const std::string error =
            make_riscv_reloc_plan( *reader, bases, plan );
        TASK_CHECK( !error.empty() );
        TASK_CHECK( mentions( error, rel->get_index(), 1 ) );
    }
    return 0;
}

//------------------------------------------------------------------------------
// objects group: whole-object behavior
//------------------------------------------------------------------------------

// Full layout: only relocated executable sections are emitted, every other
// section is absent from the plan, and untouched words keep their bytes.
static int objects_full_layout_and_preservation()
{
    ObjBuilder b;
    const std::uint32_t gap = 0xDEADBEEFU;
    section* text = b.code(
        ".text",
        { insn_auipc( 1 ), insn_addi( 2, 1 ), gap, insn_sw( 4, 1 ) } );
    section* rod  = b.rodata( ".rodata", 32 );
    section* strtab = nullptr;
    section* symtab = b.new_symtab( ".symtab", ".strtab", strtab );
    const Elf_Word anchor =
        b.symbol( symtab, strtab, ".La", 0, text->get_index() );
    const Elf_Word target =
        b.symbol( symtab, strtab, "obj", 0x1800, rod->get_index(),
                  STB_GLOBAL, STT_OBJECT );
    section* rel = b.rela( ".rela.text", text, symtab );
    b.add_reloc( rel, 0, target, R_RISCV_PCREL_HI20 );
    b.add_reloc( rel, 4, anchor, R_RISCV_PCREL_LO12_I );
    b.add_reloc( rel, 12, anchor, R_RISCV_PCREL_LO12_S );

    std::unique_ptr<elfio> reader;
    TASK_CHECK( b.reload( reader ) == 0 );
    auto bases  = make_bases( *reader,
                             { { text->get_index(), TEXT_BASE },
                               { rod->get_index(), RODATA_BASE } } );
    riscv_reloc_plan plan;
    TASK_CHECK( make_riscv_reloc_plan( *reader, bases, plan ).empty() );

    TASK_CHECK( plan.sections.size() == 1 );
    TASK_CHECK( plan.sections[0].section_index == text->get_index() );
    TASK_CHECK( plan.sections[0].data.size() == 16 );

    const std::int64_t delta =
        static_cast<std::int64_t>( RODATA_BASE + 0x1800 ) -
        static_cast<std::int64_t>( TEXT_BASE );

    // Build the expected whole buffer independently and compare all bytes.
    std::vector<unsigned char> expected( 16 );
    const std::uint32_t expected_words[4] = {
        expect_auipc( insn_auipc( 1 ), delta ),
        expect_i_type( insn_addi( 2, 1 ), delta ), gap,
        expect_s_type( insn_sw( 4, 1 ), delta )
    };
    for ( int i = 0; i < 4; ++i ) {
        for ( int k = 0; k < 4; ++k ) {
            expected[i * 4 + k] = static_cast<unsigned char>(
                ( expected_words[i] >> ( 8 * k ) ) & 0xFFU );
        }
    }
    TASK_CHECK( std::memcmp( plan.sections[0].data.data(),
                             expected.data(), 16 ) == 0 );

    // No output copies of rodata, symbol tables or relocation tables.
    for ( const auto& out : plan.sections ) {
        TASK_CHECK( out.section_index == text->get_index() );
    }
    TASK_CHECK( plan.sections[0].changes.size() == 3 );
    return 0;
}

// Saving and reloading must leave the original object's code bytes intact;
// the planner only reads its input and owns its output buffers.
static int objects_save_reload_unchanged()
{
    ObjBuilder b;
    section* text = b.code(
        ".text",
        { insn_auipc( 1 ), insn_addi( 2, 1 ), insn_sw( 4, 1 ) } );
    section* strtab = nullptr;
    section* symtab = b.new_symtab( ".symtab", ".strtab", strtab );
    const Elf_Word anchor =
        b.symbol( symtab, strtab, ".La", 0, text->get_index() );
    const Elf_Word target =
        b.symbol( symtab, strtab, "t", 0x5544, SHN_ABS, STB_GLOBAL,
                  STT_OBJECT );
    section* rel = b.rela( ".rela.text", text, symtab );
    b.add_reloc( rel, 0, target, R_RISCV_PCREL_HI20 );
    b.add_reloc( rel, 4, anchor, R_RISCV_PCREL_LO12_I );
    b.add_reloc( rel, 8, anchor, R_RISCV_PCREL_LO12_S );

    const std::string original_bytes( text->get_data(), text->get_size() );

    std::unique_ptr<elfio> reader;
    TASK_CHECK( b.reload( reader ) == 0 );
    section* loaded_text = reader->sections[text->get_index()];
    const std::string loaded_before( loaded_text->get_data(),
                                     loaded_text->get_size() );
    TASK_CHECK( loaded_before == original_bytes );

    auto bases = make_bases( *reader,
                             { { text->get_index(), TEXT_BASE } } );
    riscv_reloc_plan plan;
    TASK_CHECK( make_riscv_reloc_plan( *reader, bases, plan ).empty() );
    TASK_CHECK( !plan.sections.empty() );

    const std::string loaded_after( loaded_text->get_data(),
                                    loaded_text->get_size() );
    TASK_CHECK( loaded_after == original_bytes );
    TASK_CHECK( std::string( text->get_data(), text->get_size() ) ==
                original_bytes );

    // Re-saving the original writer and reloading still yields the same
    // untouched instruction bytes.
    std::unique_ptr<elfio> second;
    TASK_CHECK( b.reload( second ) == 0 );
    section* again = second->sections[text->get_index()];
    TASK_CHECK( std::string( again->get_data(), again->get_size() ) ==
                original_bytes );
    return 0;
}

// Undefined, COMMON and reserved/extended symbol section indices must fail,
// as must a missing base address; SHN_ABS must work.
static int objects_symbol_kinds_and_bases()
{
    // Undefined symbol (index 0).
    {
        ObjBuilder b;
        section* text =
            b.code( ".text", { insn_auipc( 1 ), insn_addi( 2, 1 ) } );
        section* strtab = nullptr;
        section* symtab = b.new_symtab( ".symtab", ".strtab", strtab );
        section* rel = b.rela( ".rela.text", text, symtab );
        b.add_reloc( rel, 0, 0, R_RISCV_PCREL_HI20 );

        std::unique_ptr<elfio> reader;
        TASK_CHECK( b.reload( reader ) == 0 );
        auto bases  = make_bases( *reader,
                                 { { text->get_index(), TEXT_BASE } } );
        riscv_reloc_plan plan;
        TASK_CHECK(
            !make_riscv_reloc_plan( *reader, bases, plan ).empty() );
    }
    // COMMON symbol.
    {
        ObjBuilder b;
        section* text =
            b.code( ".text", { insn_auipc( 1 ), insn_addi( 2, 1 ) } );
        section* strtab = nullptr;
        section* symtab = b.new_symtab( ".symtab", ".strtab", strtab );
        const Elf_Word common = b.symbol( symtab, strtab, "com", 0x10,
                                          SHN_COMMON, STB_GLOBAL,
                                          STT_COMMON );
        section* rel = b.rela( ".rela.text", text, symtab );
        b.add_reloc( rel, 0, common, R_RISCV_PCREL_HI20 );

        std::unique_ptr<elfio> reader;
        TASK_CHECK( b.reload( reader ) == 0 );
        auto bases  = make_bases( *reader,
                                 { { text->get_index(), TEXT_BASE } } );
        riscv_reloc_plan plan;
        TASK_CHECK(
            !make_riscv_reloc_plan( *reader, bases, plan ).empty() );
    }
    // Reserved/extended section index (SHN_XINDEX).
    {
        ObjBuilder b;
        section* text =
            b.code( ".text", { insn_auipc( 1 ), insn_addi( 2, 1 ) } );
        section* strtab = nullptr;
        section* symtab = b.new_symtab( ".symtab", ".strtab", strtab );
        const Elf_Word ext = b.symbol( symtab, strtab, "ext", 0,
                                       SHN_XINDEX, STB_GLOBAL,
                                       STT_OBJECT );
        section* rel = b.rela( ".rela.text", text, symtab );
        b.add_reloc( rel, 0, ext, R_RISCV_PCREL_HI20 );

        std::unique_ptr<elfio> reader;
        TASK_CHECK( b.reload( reader ) == 0 );
        auto bases  = make_bases( *reader,
                                 { { text->get_index(), TEXT_BASE } } );
        riscv_reloc_plan plan;
        TASK_CHECK(
            !make_riscv_reloc_plan( *reader, bases, plan ).empty() );
    }
    // Missing base address for the code section.
    {
        ObjBuilder b;
        section* text =
            b.code( ".text", { insn_auipc( 1 ), insn_addi( 2, 1 ) } );
        section* strtab = nullptr;
        section* symtab = b.new_symtab( ".symtab", ".strtab", strtab );
        const Elf_Word target =
            b.symbol( symtab, strtab, "t", 0x1000, SHN_ABS, STB_GLOBAL,
                      STT_OBJECT );
        section* rel = b.rela( ".rela.text", text, symtab );
        b.add_reloc( rel, 0, target, R_RISCV_PCREL_HI20 );

        std::unique_ptr<elfio> reader;
        TASK_CHECK( b.reload( reader ) == 0 );
        const std::vector<Elf64_Addr> bases; // deliberately empty
        riscv_reloc_plan plan;
        TASK_CHECK(
            !make_riscv_reloc_plan( *reader, bases, plan ).empty() );
    }
    // SHN_ABS is accepted and takes its value directly.
    {
        ObjBuilder b;
        section* text =
            b.code( ".text", { insn_auipc( 1 ), insn_addi( 2, 1 ) } );
        section* strtab = nullptr;
        section* symtab = b.new_symtab( ".symtab", ".strtab", strtab );
        const Elf_Word anchor =
            b.symbol( symtab, strtab, ".La", 0, text->get_index() );
        const Elf_Word target =
            b.symbol( symtab, strtab, "abs", 0x21004, SHN_ABS,
                      STB_GLOBAL, STT_OBJECT );
        section* rel = b.rela( ".rela.text", text, symtab );
        b.add_reloc( rel, 0, target, R_RISCV_PCREL_HI20 );
        b.add_reloc( rel, 4, anchor, R_RISCV_PCREL_LO12_I );

        std::unique_ptr<elfio> reader;
        TASK_CHECK( b.reload( reader ) == 0 );
        auto bases  = make_bases( *reader,
                                 { { text->get_index(), TEXT_BASE } } );
        riscv_reloc_plan plan;
        TASK_CHECK( make_riscv_reloc_plan( *reader, bases, plan ).empty() );
        const std::int64_t delta =
            static_cast<std::int64_t>( 0x21004 ) -
            static_cast<std::int64_t>( TEXT_BASE );
        TASK_CHECK( decode_auipc_imm( read_word( plan.sections[0], 0 ) ) +
                            decode_i_imm( read_word( plan.sections[0], 4 ) ) ==
                        delta );
    }
    return 0;
}

// NONE writes nothing; RELAX only annotates an existing primary entry; an
// orphan RELAX is an error.
static int objects_none_and_relax_hints()
{
    {
        ObjBuilder b;
        section* text =
            b.code( ".text", { insn_auipc( 1 ), insn_addi( 2, 1 ) } );
        section* strtab = nullptr;
        section* symtab = b.new_symtab( ".symtab", ".strtab", strtab );
        section* rel = b.rela( ".rela.text", text, symtab );
        b.add_reloc( rel, 0, 0, R_RISCV_NONE );

        std::unique_ptr<elfio> reader;
        TASK_CHECK( b.reload( reader ) == 0 );
        auto bases  = make_bases( *reader,
                                 { { text->get_index(), TEXT_BASE } } );
        riscv_reloc_plan plan;
        TASK_CHECK( make_riscv_reloc_plan( *reader, bases, plan ).empty() );
        TASK_CHECK( plan.changes.empty() );
        TASK_CHECK( plan.sections.empty() );
    }
    {
        ObjBuilder b;
        section* text =
            b.code( ".text", { insn_auipc( 1 ), insn_addi( 2, 1 ) } );
        section* strtab = nullptr;
        section* symtab = b.new_symtab( ".symtab", ".strtab", strtab );
        const Elf_Word anchor =
            b.symbol( symtab, strtab, ".La", 0, text->get_index() );
        const Elf_Word target =
            b.symbol( symtab, strtab, "t", 0x33008, SHN_ABS, STB_GLOBAL,
                      STT_OBJECT );
        section* rel = b.rela( ".rela.text", text, symtab );
        b.add_reloc( rel, 0, target, R_RISCV_PCREL_HI20 );
        b.add_reloc( rel, 0, 0, R_RISCV_RELAX ); // same offset as the HI20
        b.add_reloc( rel, 4, anchor, R_RISCV_PCREL_LO12_I );

        std::unique_ptr<elfio> reader;
        TASK_CHECK( b.reload( reader ) == 0 );
        auto bases  = make_bases( *reader,
                                 { { text->get_index(), TEXT_BASE } } );
        riscv_reloc_plan plan;
        TASK_CHECK( make_riscv_reloc_plan( *reader, bases, plan ).empty() );
        TASK_CHECK( plan.changes.size() == 2 );
        for ( const auto& c : plan.changes ) {
            TASK_CHECK( c.kind != riscv_reloc_kind::call );
        }
    }
    {
        ObjBuilder b;
        section* text = b.code( ".text", { insn_auipc( 1 ) } );
        section* strtab = nullptr;
        section* symtab = b.new_symtab( ".symtab", ".strtab", strtab );
        section* rel = b.rela( ".rela.text", text, symtab );
        b.add_reloc( rel, 0, 0, R_RISCV_RELAX );

        std::unique_ptr<elfio> reader;
        TASK_CHECK( b.reload( reader ) == 0 );
        auto bases  = make_bases( *reader,
                                 { { text->get_index(), TEXT_BASE } } );
        riscv_reloc_plan plan;
        const std::string error =
            make_riscv_reloc_plan( *reader, bases, plan );
        TASK_CHECK( !error.empty() );
        TASK_CHECK( mentions( error, rel->get_index(), 0 ) );
    }
    return 0;
}

// Truncated records, broken sh_link/sh_info and out-of-range symbol indices
// are all reported with the responsible table index and entry position.
static int objects_truncated_and_bad_links()
{
    // Truncated trailing RELA entry.
    {
        ObjBuilder b;
        section* text =
            b.code( ".text", { insn_auipc( 1 ), insn_addi( 2, 1 ) } );
        section* strtab = nullptr;
        section* symtab = b.new_symtab( ".symtab", ".strtab", strtab );
        const Elf_Word anchor =
            b.symbol( symtab, strtab, ".La", 0, text->get_index() );
        const Elf_Word target =
            b.symbol( symtab, strtab, "t", 0x1000, SHN_ABS, STB_GLOBAL,
                      STT_OBJECT );
        section* rel = b.rela( ".rela.text", text, symtab );
        b.add_reloc( rel, 0, target, R_RISCV_PCREL_HI20 );
        b.add_reloc( rel, 4, anchor, R_RISCV_PCREL_LO12_I );
        // Copy first: set_data() frees the section's old buffer before
        // copying, so handing it its own get_data() pointer would read freed
        // memory.
        const Elf_Xword truncated_size = rel->get_size() - 1;
        std::vector<unsigned char> truncated(
            reinterpret_cast<const unsigned char*>( rel->get_data() ),
            reinterpret_cast<const unsigned char*>( rel->get_data() ) +
                truncated_size );
        rel->set_data( reinterpret_cast<const char*>( truncated.data() ),
                       truncated_size );

        std::unique_ptr<elfio> reader;
        TASK_CHECK( b.reload( reader ) == 0 );
        auto bases  = make_bases( *reader,
                                 { { text->get_index(), TEXT_BASE } } );
        riscv_reloc_plan plan;
        const std::string error =
            make_riscv_reloc_plan( *reader, bases, plan );
        TASK_CHECK( !error.empty() );
        TASK_CHECK( error.find( "section " +
                                std::to_string( rel->get_index() ) ) !=
                    std::string::npos );
    }
    // Bad sh_link.
    {
        ObjBuilder b;
        section* text = b.code( ".text", { insn_auipc( 1 ) } );
        section* strtab = nullptr;
        section* symtab = b.new_symtab( ".symtab", ".strtab", strtab );
        section* rel = b.rela( ".rela.text", text, symtab );
        rel->set_link( 0x3FFF );
        b.add_reloc( rel, 0, 1, R_RISCV_PCREL_HI20 );

        std::unique_ptr<elfio> reader;
        TASK_CHECK( b.reload( reader ) == 0 );
        auto bases  = make_bases( *reader,
                                 { { text->get_index(), TEXT_BASE } } );
        riscv_reloc_plan plan;
        const std::string error =
            make_riscv_reloc_plan( *reader, bases, plan );
        TASK_CHECK( !error.empty() );
        TASK_CHECK( error.find( "section " +
                                std::to_string( rel->get_index() ) ) !=
                    std::string::npos );
    }
    // sh_info points at a non-executable section.
    {
        ObjBuilder b;
        section* text = b.code( ".text", { insn_auipc( 1 ) } );
        section* data = b.rodata( ".rodata", 8 );
        section* strtab = nullptr;
        section* symtab = b.new_symtab( ".symtab", ".strtab", strtab );
        section* rel = b.rela( ".rela.text", text, symtab );
        rel->set_info( data->get_index() );
        b.add_reloc( rel, 0, 1, R_RISCV_PCREL_HI20 );

        std::unique_ptr<elfio> reader;
        TASK_CHECK( b.reload( reader ) == 0 );
        auto bases  = make_bases( *reader,
                                 { { text->get_index(), TEXT_BASE } } );
        riscv_reloc_plan plan;
        TASK_CHECK( !make_riscv_reloc_plan( *reader, bases, plan ).empty() );
    }
    // Symbol index outside the table's own symbol table.
    {
        ObjBuilder b;
        section* text =
            b.code( ".text", { insn_auipc( 1 ), insn_addi( 2, 1 ) } );
        section* strtab = nullptr;
        section* symtab = b.new_symtab( ".symtab", ".strtab", strtab );
        const Elf_Word target =
            b.symbol( symtab, strtab, "t", 0x1000, SHN_ABS, STB_GLOBAL,
                      STT_OBJECT );
        section* rel = b.rela( ".rela.text", text, symtab );
        b.add_reloc( rel, 0, target, R_RISCV_PCREL_HI20 );
        b.add_reloc( rel, 4, 99, R_RISCV_PCREL_LO12_I );

        std::unique_ptr<elfio> reader;
        TASK_CHECK( b.reload( reader ) == 0 );
        auto bases  = make_bases( *reader,
                                 { { text->get_index(), TEXT_BASE } } );
        riscv_reloc_plan plan;
        const std::string error =
            make_riscv_reloc_plan( *reader, bases, plan );
        TASK_CHECK( !error.empty() );
        TASK_CHECK( mentions( error, rel->get_index(), 1 ) );
    }
    return 0;
}

// Change descriptions come out in a stable order even when entries are
// scattered across tables and inserted out of order.
static int objects_stable_change_order()
{
    ObjBuilder b;
    section* text_a = b.code( ".text.a",
                              { insn_auipc( 1 ), insn_addi( 2, 1 ) } );
    section* text_b = b.code( ".text.b",
                              { insn_auipc( 3 ), insn_addi( 4, 3 ) } );
    section* str_a = nullptr;
    section* sym_a = b.new_symtab( ".symtab.a", ".strtab.a", str_a );
    section* str_b = nullptr;
    section* sym_b = b.new_symtab( ".symtab.b", ".strtab.b", str_b );

    const Elf_Word anchor_a =
        b.symbol( sym_a, str_a, ".La", 0, text_a->get_index() );
    const Elf_Word target_a =
        b.symbol( sym_a, str_a, "ta", 0x11004, SHN_ABS, STB_GLOBAL,
                  STT_OBJECT );
    const Elf_Word anchor_b =
        b.symbol( sym_b, str_b, ".Lb", 0, text_b->get_index() );
    const Elf_Word target_b =
        b.symbol( sym_b, str_b, "tb", 0x22008, SHN_ABS, STB_GLOBAL,
                  STT_OBJECT );

    // Second table is created first in traversal terms but targets .text.b;
    // entries are inserted low-before-high inside each table.
    section* rel_b = b.rela( ".rela.text.b", text_b, sym_b );
    section* rel_a = b.rela( ".rela.text.a", text_a, sym_a );
    b.add_reloc( rel_b, 4, anchor_b, R_RISCV_PCREL_LO12_I );
    b.add_reloc( rel_b, 0, target_b, R_RISCV_PCREL_HI20 );
    b.add_reloc( rel_a, 4, anchor_a, R_RISCV_PCREL_LO12_I );
    b.add_reloc( rel_a, 0, target_a, R_RISCV_PCREL_HI20 );

    std::unique_ptr<elfio> reader;
    TASK_CHECK( b.reload( reader ) == 0 );
    auto bases  = make_bases( *reader,
                             { { text_a->get_index(), TEXT_BASE },
                               { text_b->get_index(), 0x40000 } } );
    riscv_reloc_plan plan;
    TASK_CHECK( make_riscv_reloc_plan( *reader, bases, plan ).empty() );
    TASK_CHECK( plan.changes.size() == 4 );

    for ( std::size_t i = 1; i < plan.changes.size(); ++i ) {
        const auto& x = plan.changes[i - 1];
        const auto& y = plan.changes[i];
        const bool ordered =
            ( x.code_section_index < y.code_section_index ) ||
            ( x.code_section_index == y.code_section_index &&
              ( x.code_offset < y.code_offset ||
                ( x.code_offset == y.code_offset &&
                  ( x.reloc_section_index < y.reloc_section_index ||
                    ( x.reloc_section_index == y.reloc_section_index &&
                      x.reloc_entry_index <= y.reloc_entry_index ) ) ) ) );
        TASK_CHECK( ordered );
    }
    TASK_CHECK( plan.changes.front().code_section_index ==
                text_a->get_index() );
    TASK_CHECK( plan.sections[0].section_index == text_a->get_index() );
    TASK_CHECK( plan.sections[1].section_index == text_b->get_index() );
    return 0;
}

// An explicit HI20 addend participates in D = S + A - P.
static int objects_explicit_addend()
{
    ObjBuilder b;
    section* rod  = b.rodata( ".rodata", 16 );
    section* text =
        b.code( ".text", { insn_auipc( 1 ), insn_addi( 2, 1 ) } );
    section* strtab = nullptr;
    section* symtab = b.new_symtab( ".symtab", ".strtab", strtab );
    const Elf_Word anchor =
        b.symbol( symtab, strtab, ".La", 0, text->get_index() );
    const Elf_Word target =
        b.symbol( symtab, strtab, "obj", 0x100, rod->get_index(),
                  STB_GLOBAL, STT_OBJECT );
    section* rel = b.rela( ".rela.text", text, symtab );
    b.add_reloc( rel, 0, target, R_RISCV_PCREL_HI20, 0x10 );
    b.add_reloc( rel, 4, anchor, R_RISCV_PCREL_LO12_I );

    std::unique_ptr<elfio> reader;
    TASK_CHECK( b.reload( reader ) == 0 );
    auto bases  = make_bases( *reader,
                             { { text->get_index(), TEXT_BASE },
                               { rod->get_index(), RODATA_BASE } } );
    riscv_reloc_plan plan;
    TASK_CHECK( make_riscv_reloc_plan( *reader, bases, plan ).empty() );

    const std::int64_t delta =
        static_cast<std::int64_t>( RODATA_BASE + 0x100 + 0x10 ) -
        static_cast<std::int64_t>( TEXT_BASE );
    TASK_CHECK( decode_auipc_imm( read_word( plan.sections[0], 0 ) ) +
                        decode_i_imm( read_word( plan.sections[0], 4 ) ) ==
                    delta );
    TASK_CHECK( read_word( plan.sections[0], 0 ) ==
                expect_auipc( insn_auipc( 1 ), delta ) );
    return 0;
}

//------------------------------------------------------------------------------
// Registration
//------------------------------------------------------------------------------

const task_case task_cases[] = {
    { "pairs", "out_of_order", pairs_out_of_order },
    { "pairs", "cross_table", pairs_cross_table },
    { "pairs", "shared_high", pairs_shared_high },
    { "pairs", "same_name_distinct", pairs_same_name_distinct },
    { "pairs", "anchor_missing", pairs_anchor_missing },
    { "pairs", "anchor_wrong_section", pairs_anchor_wrong_section },
    { "pairs", "lo_addend_nonzero", pairs_lo_addend_nonzero },

    { "fields", "boundary_values", fields_boundary_values },
    { "fields", "negative_displacement", fields_negative_displacement },
    { "fields", "s_type_bitfields", fields_s_type_bitfields },
    { "fields", "i_type_preservation", fields_i_type_preservation },
    { "fields", "auipc_rd_preserved", fields_auipc_rd_preserved },
    { "fields", "out_of_range_rejected", fields_out_of_range_rejected },
    { "fields", "call_pair", fields_call_pair },
    { "fields", "call_register_mismatch", fields_call_register_mismatch },
    { "fields", "call_second_not_jalr", fields_call_second_not_jalr },
    { "fields", "wrong_opcodes_rejected", fields_wrong_opcodes_rejected },

    { "objects", "full_layout_and_preservation",
      objects_full_layout_and_preservation },
    { "objects", "save_reload_unchanged", objects_save_reload_unchanged },
    { "objects", "symbol_kinds_and_bases",
      objects_symbol_kinds_and_bases },
    { "objects", "none_and_relax_hints", objects_none_and_relax_hints },
    { "objects", "truncated_and_bad_links",
      objects_truncated_and_bad_links },
    { "objects", "stable_change_order", objects_stable_change_order },
    { "objects", "explicit_addend", objects_explicit_addend },
};

const std::size_t task_case_count =
    sizeof( task_cases ) / sizeof( task_cases[0] );
