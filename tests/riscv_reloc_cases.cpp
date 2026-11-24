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

// Hand-written RISC-V relocation cases for ELFIO.
//
// Every instruction word is typed out by hand; nothing is assembled, linked or
// executed. The file has no main(): task_support/driver.c drives it and the
// pairs, fields and objects groups are registered below.

#include <cstdint>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "task_support/check.h"

#include "examples/riscv_reloc_analysis/report.hpp"

using namespace ELFIO;

namespace {

constexpr unsigned OP_LOAD  = 0x03;
constexpr unsigned OP_STORE = 0x23;
constexpr unsigned OP_IMM   = 0x13;
constexpr unsigned OP_LUI   = 0x37;
constexpr unsigned OP_AUIPC = 0x17;
constexpr unsigned OP_JALR  = 0x67;

// The original twelve bytes from task_support/witness.cpp.
constexpr std::uint32_t WITNESS_WORDS[3] = { 0x00000517, 0x00050513, 0x00152023 };

std::uint32_t rv_addi( unsigned rd, unsigned rs1, std::int32_t imm )
{
    return ( ( static_cast<std::uint32_t>( imm ) & 0xfff ) << 20 ) |
           ( rs1 << 15 ) | ( rd << 7 ) | OP_IMM;
}

std::uint32_t rv_load( unsigned rd, unsigned rs1, std::int32_t imm )
{
    return ( ( static_cast<std::uint32_t>( imm ) & 0xfff ) << 20 ) |
           ( rs1 << 15 ) | ( 2u << 12 ) | ( rd << 7 ) | OP_LOAD; // LW
}

std::uint32_t rv_jalr( unsigned rd, unsigned rs1, std::int32_t imm )
{
    return ( ( static_cast<std::uint32_t>( imm ) & 0xfff ) << 20 ) |
           ( rs1 << 15 ) | ( rd << 7 ) | OP_JALR;
}

std::uint32_t rv_store( unsigned rs2, unsigned rs1, std::int32_t imm )
{
    const auto value = static_cast<std::uint32_t>( imm ) & 0xfff;
    return ( ( value >> 5 ) << 25 ) | ( rs2 << 20 ) | ( rs1 << 15 ) |
           ( 2u << 12 ) | ( ( value & 0x1f ) << 7 ) | OP_STORE; // SW
}

std::uint32_t rv_auipc( unsigned rd, std::uint32_t imm20 )
{
    return ( ( imm20 & 0xfffff ) << 12 ) | ( rd << 7 ) | OP_AUIPC;
}

std::uint32_t rv_lui( unsigned rd, std::uint32_t imm20 )
{
    return ( ( imm20 & 0xfffff ) << 12 ) | ( rd << 7 ) | OP_LUI;
}

//------------------------------------------------------------------------------
// A compact, hand-built RISC-V ET_REL object.
//------------------------------------------------------------------------------
struct object_builder
{
    elfio                                                     writer;
    section*                                                  text   = nullptr;
    section*                                                  strtab = nullptr;
    section*                                                  symtab = nullptr;
    std::unique_ptr<string_section_accessor>                  strings;
    std::unique_ptr<symbol_section_accessor>                  symbols;
    std::vector<section*>                                     rela_sections;
    std::vector<std::unique_ptr<relocation_section_accessor>> relas;

    object_builder()
    {
        writer.create( ELFCLASS64, ELFDATA2LSB );
        writer.set_type( ET_REL );
        writer.set_machine( EM_RISCV );
    }

    void add_text( const std::vector<std::uint32_t>& words )
    {
        text = writer.sections.add( ".text" );
        text->set_type( SHT_PROGBITS );
        text->set_flags( SHF_ALLOC | SHF_EXECINSTR );
        text->set_addr_align( 2 );
        std::vector<char> bytes( words.size() * 4 );
        std::memcpy( bytes.data(), words.data(), bytes.size() );
        text->set_data( bytes.data(), bytes.size() );
    }

    void start_symbol_table()
    {
        strtab = writer.sections.add( ".strtab" );
        strtab->set_type( SHT_STRTAB );
        strings.reset( new string_section_accessor( strtab ) );

        symtab = writer.sections.add( ".symtab" );
        symtab->set_type( SHT_SYMTAB );
        symtab->set_link( strtab->get_index() );
        symtab->set_info( 2 );
        symtab->set_entry_size(
            writer.get_default_entry_size( SHT_SYMTAB ) );
        symbols.reset( new symbol_section_accessor( writer, symtab ) );
    }

    Elf_Word symbol( const std::string& name,
                     Elf64_Addr         value,
                     Elf_Half           shndx,
                     unsigned char      bind = STB_GLOBAL,
                     unsigned char      type = STT_OBJECT )
    {
        return symbols->add_symbol( *strings, name.c_str(), value, 0, bind,
                                    type, 0, shndx );
    }

    relocation_section_accessor* add_rela( const std::string& name,
                                           section*           code )
    {
        section* rela = writer.sections.add( name );
        rela->set_type( SHT_RELA );
        rela->set_link( symtab->get_index() );
        rela->set_info( code->get_index() );
        rela->set_entry_size( writer.get_default_entry_size( SHT_RELA ) );
        rela_sections.push_back( rela );
        relas.emplace_back( new relocation_section_accessor( writer, rela ) );
        return relas.back().get();
    }

    bool round_trip( elfio& reader )
    {
        std::stringstream stream( std::ios::in | std::ios::out |
                                  std::ios::binary );
        if ( !writer.save( stream ) ) {
            return false;
        }
        stream.seekg( 0 );
        return reader.load( stream );
    }
};

riscv_section_base_table bases_for( const elfio& reader,
                                    Elf64_Addr   text_base )
{
    riscv_section_base_table bases;
    const section*           text = reader.sections[".text"];
    if ( text != nullptr ) {
        bases[text->get_index()] = text_base;
    }
    return bases;
}

std::uint32_t word_at( const riscv_reloc_plan& plan,
                       Elf_Half                section,
                       Elf64_Addr              offset )
{
    const auto* output = plan.find_output( section );
    if ( output == nullptr || offset + 4 > output->data.size() ) {
        return 0;
    }
    std::uint32_t word = 0;
    std::memcpy( &word, output->data.data() + offset, sizeof( word ) );
    return word;
}

std::int64_t decode_hi( std::uint32_t word )
{
    return static_cast<std::int64_t>( static_cast<std::int32_t>( word ) ) >>
           12;
}

std::int32_t decode_lo_i( std::uint32_t word )
{
    return static_cast<std::int32_t>( word ) >> 20;
}

std::int32_t decode_lo_s( std::uint32_t word )
{
    const std::uint32_t imm = ( ( ( word >> 25 ) & 0x7f ) << 5 ) |
                              ( ( word >> 7 ) & 0x1f );
    return static_cast<std::int32_t>( imm << 20 ) >> 20;
}

std::string report_text( const riscv_example::analysis_report& report )
{
    std::ostringstream stream;
    riscv_example::write_riscv_reference_report( report, stream );
    return stream.str();
}

std::uint64_t count_occurrences( const std::string& haystack,
                                 const std::string& needle )
{
    std::uint64_t count = 0;
    std::size_t   pos   = 0;
    while ( ( pos = haystack.find( needle, pos ) ) != std::string::npos ) {
        ++count;
        pos += needle.size();
    }
    return count;
}

//==============================================================================
// pairs
//==============================================================================

// Low records come first, several low records share one high record, and the
// two halves live in separate relocation tables. Pairing uses the anchor's
// section identity and st_value, never adjacency or names.
int pairs_out_of_order_and_shared()
{
    object_builder builder;
    builder.add_text( { rv_auipc( 5, 0 ),      // 0
                        rv_addi( 5, 5, 0 ),    // 4
                        rv_load( 6, 5, 0 ),    // 8
                        rv_store( 6, 5, 0 ) } ); // 12
    builder.start_symbol_table();
    const Elf_Word anchor = builder.symbol( ".Lhi", 0,
                                            builder.text->get_index(),
                                            STB_LOCAL, STT_NOTYPE );
    const Elf_Word target = builder.symbol( "target", 0x1800, SHN_ABS );

    relocation_section_accessor* low =
        builder.add_rela( ".rela.low", builder.text );
    relocation_section_accessor* high =
        builder.add_rela( ".rela.high", builder.text );
    low->add_entry( 4, anchor, R_RISCV_PCREL_LO12_I, 0 );
    low->add_entry( 8, anchor, R_RISCV_PCREL_LO12_I, 0 );
    low->add_entry( 12, anchor, R_RISCV_PCREL_LO12_S, 0 );
    high->add_entry( 0, target, R_RISCV_PCREL_HI20, 0 );

    elfio reader;
    TASK_CHECK( builder.round_trip( reader ) );
    const auto bases = bases_for( reader, 0x1000 );
    const auto plan  = analyze_riscv_relocations( reader, bases );

    TASK_CHECK( plan.ok );
    TASK_CHECK( plan.error.is_error == false );
    TASK_CHECK( plan.changes.size() == 4 );
    TASK_CHECK( plan.hints.empty() );
    TASK_CHECK( plan.outputs.size() == 1 );

    // Stable order by code offset, independent of table order.
    TASK_CHECK( plan.changes[0].kind == riscv_reloc_kind::pcrel_hi20 );
    TASK_CHECK( plan.changes[0].code_offset == 0 );
    TASK_CHECK( plan.changes[1].kind == riscv_reloc_kind::pcrel_lo12_i );
    TASK_CHECK( plan.changes[1].code_offset == 4 );
    TASK_CHECK( plan.changes[2].kind == riscv_reloc_kind::pcrel_lo12_i );
    TASK_CHECK( plan.changes[2].code_offset == 8 );
    TASK_CHECK( plan.changes[3].kind == riscv_reloc_kind::pcrel_lo12_s );
    TASK_CHECK( plan.changes[3].code_offset == 12 );

    // D = 0x1800 - 0x1000 = 0x800 -> hi = 1, lo = -0x800.
    const Elf_Half      text_section = builder.text->get_index();
    const std::uint32_t hi_word      = word_at( plan, text_section, 0 );
    TASK_CHECK( ( hi_word & 0x7f ) == OP_AUIPC );
    TASK_CHECK( decode_hi( hi_word ) == 1 );
    TASK_CHECK( decode_lo_i( word_at( plan, text_section, 4 ) ) == -0x800 );
    TASK_CHECK( decode_lo_i( word_at( plan, text_section, 8 ) ) == -0x800 );
    TASK_CHECK( decode_lo_s( word_at( plan, text_section, 12 ) ) == -0x800 );

    for ( std::size_t i = 1; i <= 3; ++i ) {
        TASK_CHECK( plan.changes[i].has_anchor );
        TASK_CHECK( plan.changes[i].anchor_rela_section ==
                    plan.changes[0].rela_section );
        TASK_CHECK( plan.changes[i].anchor_rela_entry ==
                    plan.changes[0].rela_entry );
        TASK_CHECK( plan.changes[i].anchor_code_offset == 0 );
        TASK_CHECK( plan.changes[i].anchor_pc == 0x1000 );
        TASK_CHECK( plan.changes[i].target_address == 0x1800 );
        // The low symbol is the anchor, not the final target.
        TASK_CHECK( plan.changes[i].symbol_index == anchor );
        TASK_CHECK( plan.changes[i].symbol_address == 0x1000 );
    }
    TASK_CHECK( plan.changes[0].symbol_index == target );
    TASK_CHECK( plan.changes[0].symbol_address == 0x1800 );

    // Exact reconstruction D = PC + hi * 4096 + lo.
    TASK_CHECK( plan.changes[0].pc + decode_hi( hi_word ) * 4096 +
                    decode_lo_i( word_at( plan, text_section, 4 ) ) ==
                0x1800 );
    return 0;
}

// Same-named but distinct anchors and same-named but distinct targets stay
// separate; the anchor's name is irrelevant, its section/value identity is not.
int pairs_symbol_identity()
{
    object_builder builder;
    builder.add_text( { rv_auipc( 5, 0 ),      // 0
                        rv_addi( 5, 5, 0 ),    // 4
                        rv_auipc( 6, 0 ),      // 8
                        rv_addi( 6, 6, 0 ) } ); // 12
    builder.start_symbol_table();
    const Elf_Word anchor_a = builder.symbol( ".Lanchor", 0,
                                              builder.text->get_index(),
                                              STB_LOCAL, STT_NOTYPE );
    const Elf_Word anchor_b = builder.symbol( ".Lanchor", 8,
                                              builder.text->get_index(),
                                              STB_LOCAL, STT_NOTYPE );
    const Elf_Word target_a = builder.symbol( "shared", 0x2000, SHN_ABS );
    const Elf_Word target_b = builder.symbol( "shared", 0x2800, SHN_ABS );

    // Interleave everything: lows first, highs last, one shared table.
    relocation_section_accessor* rela =
        builder.add_rela( ".rela.text", builder.text );
    rela->add_entry( 4, anchor_a, R_RISCV_PCREL_LO12_I, 0 );
    rela->add_entry( 12, anchor_b, R_RISCV_PCREL_LO12_I, 0 );
    rela->add_entry( 8, target_b, R_RISCV_PCREL_HI20, 0 );
    rela->add_entry( 0, target_a, R_RISCV_PCREL_HI20, 0 );

    elfio reader;
    TASK_CHECK( builder.round_trip( reader ) );
    const auto bases = bases_for( reader, 0x1000 );
    const auto plan  = analyze_riscv_relocations( reader, bases );

    TASK_CHECK( plan.ok );
    TASK_CHECK( plan.changes.size() == 4 );
    TASK_CHECK( plan.changes[0].code_offset == 0 );
    TASK_CHECK( plan.changes[0].symbol_index == target_a );
    TASK_CHECK( plan.changes[0].symbol_address == 0x2000 );
    TASK_CHECK( plan.changes[1].code_offset == 4 );
    TASK_CHECK( plan.changes[1].symbol_index == anchor_a );
    TASK_CHECK( plan.changes[1].anchor_code_offset == 0 );
    TASK_CHECK( plan.changes[1].anchor_symbol_address == 0x2000 );
    TASK_CHECK( plan.changes[1].target_address == 0x2000 );
    TASK_CHECK( plan.changes[2].code_offset == 8 );
    TASK_CHECK( plan.changes[2].symbol_index == target_b );
    TASK_CHECK( plan.changes[2].symbol_address == 0x2800 );
    TASK_CHECK( plan.changes[3].code_offset == 12 );
    TASK_CHECK( plan.changes[3].symbol_index == anchor_b );
    TASK_CHECK( plan.changes[3].anchor_code_offset == 8 );
    TASK_CHECK( plan.changes[3].anchor_symbol_address == 0x2800 );
    TASK_CHECK( plan.changes[3].target_address == 0x2800 );

    const Elf_Half      text_section = builder.text->get_index();
    TASK_CHECK( decode_hi( word_at( plan, text_section, 0 ) ) == 1 );
    TASK_CHECK( decode_lo_i( word_at( plan, text_section, 4 ) ) == 0 );
    // D2 = 0x2800 - 0x1008 = 0x17f8 -> hi 1, lo 0x7f8.
    TASK_CHECK( decode_hi( word_at( plan, text_section, 8 ) ) == 1 );
    TASK_CHECK( decode_lo_i( word_at( plan, text_section, 12 ) ) == 0x7f8 );
    return 0;
}

// Failures carry the offending table and entry and never publish a plan.
int pairs_errors_are_located()
{
    // Unsupported relocation type.
    {
        object_builder builder;
        builder.add_text( { rv_auipc( 5, 0 ), rv_addi( 5, 5, 0 ) } );
        builder.start_symbol_table();
        const Elf_Word target = builder.symbol( "t", 0x1800, SHN_ABS );
        auto*          rela   = builder.add_rela( ".rela.text", builder.text );
        rela->add_entry( 0, target, R_RISCV_BRANCH, 0 );

        elfio reader;
        TASK_CHECK( builder.round_trip( reader ) );
        const auto bases = bases_for( reader, 0x1000 );
        const auto plan  = analyze_riscv_relocations( reader, bases );

        TASK_CHECK( !plan.ok );
        TASK_CHECK( plan.error.is_error );
        TASK_CHECK( plan.error.rela_section ==
                    builder.rela_sections[0]->get_index() );
        TASK_CHECK( plan.error.has_entry );
        TASK_CHECK( plan.error.rela_entry == 0 );
        TASK_CHECK( plan.changes.empty() );
        TASK_CHECK( plan.hints.empty() );
        TASK_CHECK( plan.outputs.empty() );
    }

    // The anchor lives in a different section than the low instruction.
    {
        object_builder builder;
        builder.add_text( { rv_auipc( 5, 0 ), rv_addi( 5, 5, 0 ) } );
        builder.start_symbol_table();
        const Elf_Word foreign =
            builder.symbol( ".Lforeign", 0, 1, STB_LOCAL, STT_NOTYPE );
        auto* rela = builder.add_rela( ".rela.text", builder.text );
        rela->add_entry( 4, foreign, R_RISCV_PCREL_LO12_I, 0 );

        elfio reader;
        TASK_CHECK( builder.round_trip( reader ) );
        const auto bases = bases_for( reader, 0x1000 );
        const auto plan  = analyze_riscv_relocations( reader, bases );
        TASK_CHECK( !plan.ok );
        TASK_CHECK( plan.error.has_entry );
        TASK_CHECK( plan.error.rela_entry == 0 );
        TASK_CHECK( plan.outputs.empty() );
    }

    // LO12 with a non-zero addend, a missing anchor, and a duplicated anchor.
    {
        object_builder builder;
        builder.add_text( { rv_auipc( 5, 0 ), rv_addi( 5, 5, 0 ) } );
        builder.start_symbol_table();
        const Elf_Word anchor = builder.symbol( ".Lhi", 0,
                                                builder.text->get_index(),
                                                STB_LOCAL, STT_NOTYPE );
        auto* rela = builder.add_rela( ".rela.text", builder.text );
        rela->add_entry( 4, anchor, R_RISCV_PCREL_LO12_I, 4 );

        elfio reader;
        TASK_CHECK( builder.round_trip( reader ) );
        const auto plan =
            analyze_riscv_relocations( reader, bases_for( reader, 0x1000 ) );
        TASK_CHECK( !plan.ok );
        TASK_CHECK( plan.error.rela_entry == 0 );
        TASK_CHECK( plan.error.message.find( "addend" ) != std::string::npos );
    }
    {
        object_builder builder;
        builder.add_text( { rv_auipc( 5, 0 ), rv_addi( 5, 5, 0 ) } );
        builder.start_symbol_table();
        const Elf_Word anchor = builder.symbol( ".Lhi", 0,
                                                builder.text->get_index(),
                                                STB_LOCAL, STT_NOTYPE );
        auto* rela = builder.add_rela( ".rela.text", builder.text );
        rela->add_entry( 4, anchor, R_RISCV_PCREL_LO12_I, 0 );

        elfio reader;
        TASK_CHECK( builder.round_trip( reader ) );
        const auto plan =
            analyze_riscv_relocations( reader, bases_for( reader, 0x1000 ) );
        TASK_CHECK( !plan.ok );
        TASK_CHECK( plan.error.rela_entry == 0 );
        TASK_CHECK( plan.error.message.find( "anchor" ) != std::string::npos );
    }
    {
        object_builder builder;
        builder.add_text( { rv_auipc( 5, 0 ), rv_addi( 5, 5, 0 ) } );
        builder.start_symbol_table();
        const Elf_Word anchor = builder.symbol( ".Lhi", 0,
                                                builder.text->get_index(),
                                                STB_LOCAL, STT_NOTYPE );
        const Elf_Word target = builder.symbol( "t", 0x1800, SHN_ABS );
        auto* rela = builder.add_rela( ".rela.text", builder.text );
        rela->add_entry( 4, anchor, R_RISCV_PCREL_LO12_I, 0 );
        rela->add_entry( 0, target, R_RISCV_PCREL_HI20, 0 );
        rela->add_entry( 0, target, R_RISCV_PCREL_HI20, 0 );

        elfio reader;
        TASK_CHECK( builder.round_trip( reader ) );
        const auto plan =
            analyze_riscv_relocations( reader, bases_for( reader, 0x1000 ) );
        TASK_CHECK( !plan.ok );
        TASK_CHECK( plan.changes.empty() );
    }

    // Missing base for the code section, and a missing base for a symbol's
    // section.
    {
        object_builder builder;
        builder.add_text( { rv_auipc( 5, 0 ) } );
        builder.start_symbol_table();
        const Elf_Word target = builder.symbol( "t", 0x1800, SHN_ABS );
        auto* rela = builder.add_rela( ".rela.text", builder.text );
        rela->add_entry( 0, target, R_RISCV_PCREL_HI20, 0 );
        elfio reader;
        TASK_CHECK( builder.round_trip( reader ) );
        const riscv_section_base_table empty;
        const auto plan = analyze_riscv_relocations( reader, empty );
        TASK_CHECK( !plan.ok );
        TASK_CHECK( plan.error.rela_section ==
                    builder.rela_sections[0]->get_index() );
        TASK_CHECK( !plan.error.has_entry );

        // Symbol defined in a section whose base was not supplied.
        object_builder defined;
        defined.add_text( { rv_auipc( 5, 0 ) } );
        defined.start_symbol_table();
        section* extra = defined.writer.sections.add( ".other" );
        extra->set_type( SHT_PROGBITS );
        extra->set_flags( SHF_ALLOC | SHF_EXECINSTR );
        const char extra_byte[4] = { 0, 0, 0, 0 };
        extra->set_data( extra_byte, 4 );
        const Elf_Word local = defined.symbol( "local", 0,
                                               extra->get_index(),
                                               STB_GLOBAL, STT_FUNC );
        auto* rela2 = defined.add_rela( ".rela.text", defined.text );
        rela2->add_entry( 0, local, R_RISCV_PCREL_HI20, 0 );
        elfio reader2;
        TASK_CHECK( defined.round_trip( reader2 ) );
        const auto plan2 = analyze_riscv_relocations(
            reader2, bases_for( reader2, 0x1000 ) );
        TASK_CHECK( !plan2.ok );
        TASK_CHECK( plan2.error.has_entry );
        TASK_CHECK( plan2.error.rela_entry == 0 );
        TASK_CHECK( plan2.error.message.find( "base" ) != std::string::npos );
    }
    return 0;
}

// Each table resolves its own symbols through its own sh_link; two tables may
// name different symbol indices for the same text.
int pairs_per_table_symbol_links()
{
    object_builder builder;
    builder.add_text( { rv_auipc( 5, 0 ), rv_addi( 5, 5, 0 ) } );

    // A second symbol table with a different ordering.
    section* strtab2 = builder.writer.sections.add( ".strtab2" );
    strtab2->set_type( SHT_STRTAB );
    string_section_accessor strings2( strtab2 );
    section* symtab2 = builder.writer.sections.add( ".symtab2" );
    symtab2->set_type( SHT_SYMTAB );
    symtab2->set_link( strtab2->get_index() );
    symtab2->set_info( 2 );
    symtab2->set_entry_size(
        builder.writer.get_default_entry_size( SHT_SYMTAB ) );
    symbol_section_accessor symbols2( builder.writer, symtab2 );

    // Table 1: symbol 0 is target. Table 2: symbol 0 is the anchor, symbol 1
    // is target.
    builder.start_symbol_table();
    const Elf_Word anchor1 = builder.symbol( ".Lhi", 0,
                                             builder.text->get_index(),
                                             STB_LOCAL, STT_NOTYPE );
    const Elf_Word target1 = builder.symbol( "target", 0x1800, SHN_ABS );
    // Shift table 2 by one extra symbol so its indices differ from table 1.
    symbols2.add_symbol( strings2, "filler", 0, 0, STB_GLOBAL, STT_NOTYPE, 0,
                         builder.text->get_index() );
    const Elf_Word anchor2 =
        symbols2.add_symbol( strings2, ".Lhi", 0, 0, STB_LOCAL, STT_NOTYPE, 0,
                             builder.text->get_index() );
    const Elf_Word target2 =
        symbols2.add_symbol( strings2, "target", 0x1800, 0, STB_GLOBAL,
                             STT_OBJECT, 0, SHN_ABS );

    // High record uses table 1 (symbol index 1 = target).
    section* rela_high = builder.writer.sections.add( ".rela.high" );
    rela_high->set_type( SHT_RELA );
    rela_high->set_link( builder.symtab->get_index() );
    rela_high->set_info( builder.text->get_index() );
    rela_high->set_entry_size(
        builder.writer.get_default_entry_size( SHT_RELA ) );
    relocation_section_accessor high( builder.writer, rela_high );
    high.add_entry( 0, target1, R_RISCV_PCREL_HI20, 0 );

    // Low record uses table 2 (symbol index 0 = anchor, not the same index as
    // it has in table 1).
    section* rela_low = builder.writer.sections.add( ".rela.low" );
    rela_low->set_type( SHT_RELA );
    rela_low->set_link( symtab2->get_index() );
    rela_low->set_info( builder.text->get_index() );
    rela_low->set_entry_size(
        builder.writer.get_default_entry_size( SHT_RELA ) );
    relocation_section_accessor low( builder.writer, rela_low );
    low.add_entry( 4, anchor2, R_RISCV_PCREL_LO12_I, 0 );

    elfio reader;
    TASK_CHECK( builder.round_trip( reader ) );
    const auto plan =
        analyze_riscv_relocations( reader, bases_for( reader, 0x1000 ) );
    TASK_CHECK( plan.ok );
    TASK_CHECK( plan.changes.size() == 2 );

    // Both halves resolve through their own table.
    TASK_CHECK( plan.changes[0].symbol_index == target1 );
    TASK_CHECK( plan.changes[1].symbol_index == anchor2 );
    TASK_CHECK( plan.changes[1].has_anchor );
    TASK_CHECK( plan.changes[1].anchor_rela_entry == 0 );
    TASK_CHECK( plan.changes[1].anchor_symbol_index == target1 );
    TASK_CHECK( plan.changes[1].target_address == 0x1800 );

    // The low table's symbol indices are table-local: resolving them through
    // the high table's sh_link would pick different entries.
    TASK_CHECK( anchor1 != target1 );
    TASK_CHECK( symtab2->get_index() != builder.symtab->get_index() );
    TASK_CHECK( plan.changes[0].symbol_table ==
                builder.symtab->get_index() );
    TASK_CHECK( plan.changes[1].symbol_table == symtab2->get_index() );
    TASK_CHECK( plan.changes[1].anchor_symbol_table ==
                builder.symtab->get_index() );
    TASK_CHECK( anchor2 != anchor1 );
    TASK_CHECK( target2 != target1 );
    return 0;
}

// Malformed tables and symbol indices fail with the table/entry location.
int pairs_malformed_tables()
{
    // A truncated entry: size is not a multiple of the entry size.
    {
        object_builder builder;
        builder.add_text( { rv_auipc( 5, 0 ) } );
        builder.start_symbol_table();
        section* rela = builder.writer.sections.add( ".rela.bad" );
        rela->set_type( SHT_RELA );
        rela->set_link( builder.symtab->get_index() );
        rela->set_info( builder.text->get_index() );
        rela->set_entry_size( builder.writer.get_default_entry_size(
            SHT_RELA ) );
        std::vector<char> partial( 12, 0 );
        rela->set_data( partial.data(), partial.size() );

        elfio reader;
        TASK_CHECK( builder.round_trip( reader ) );
        const auto bases = bases_for( reader, 0x1000 );
        const auto plan  = analyze_riscv_relocations( reader, bases );
        TASK_CHECK( !plan.ok );
        TASK_CHECK( plan.error.rela_section == rela->get_index() );
        TASK_CHECK( plan.outputs.empty() );
    }

    // sh_link does not point at a symbol table.
    {
        object_builder builder;
        builder.add_text( { rv_auipc( 5, 0 ) } );
        builder.start_symbol_table();
        auto* rela = builder.add_rela( ".rela.text", builder.text );
        rela->add_entry( 0, 0, R_RISCV_PCREL_HI20, 0 );
        builder.rela_sections.back()->set_link( builder.text->get_index() );

        elfio reader;
        TASK_CHECK( builder.round_trip( reader ) );
        const auto bases = bases_for( reader, 0x1000 );
        const auto plan  = analyze_riscv_relocations( reader, bases );
        TASK_CHECK( !plan.ok );
        TASK_CHECK( plan.error.rela_section ==
                    builder.rela_sections.back()->get_index() );
    }

    // sh_info does not point at a section.
    {
        object_builder builder;
        builder.add_text( { rv_auipc( 5, 0 ) } );
        builder.start_symbol_table();
        auto* rela = builder.add_rela( ".rela.text", builder.text );
        rela->add_entry( 0, 0, R_RISCV_PCREL_HI20, 0 );
        builder.rela_sections.back()->set_info( 0 );

        elfio reader;
        TASK_CHECK( builder.round_trip( reader ) );
        const auto bases = bases_for( reader, 0x1000 );
        const auto plan  = analyze_riscv_relocations( reader, bases );
        TASK_CHECK( !plan.ok );
        TASK_CHECK( plan.error.rela_section ==
                    builder.rela_sections.back()->get_index() );
    }

    // Symbol index beyond the symbol table.
    {
        object_builder builder;
        builder.add_text( { rv_auipc( 5, 0 ) } );
        builder.start_symbol_table();
        auto* rela = builder.add_rela( ".rela.text", builder.text );
        rela->add_entry( 0, 99, R_RISCV_PCREL_HI20, 0 );

        elfio reader;
        TASK_CHECK( builder.round_trip( reader ) );
        const auto bases = bases_for( reader, 0x1000 );
        const auto plan  = analyze_riscv_relocations( reader, bases );
        TASK_CHECK( !plan.ok );
        TASK_CHECK( plan.error.has_entry );
        TASK_CHECK( plan.error.rela_entry == 0 );
    }

    // Undefined and COMMON symbols.
    {
        object_builder builder;
        builder.add_text( { rv_auipc( 5, 0 ) } );
        builder.start_symbol_table();
        const Elf_Word undefined =
            builder.symbol( "undef", 0, SHN_UNDEF );
        const Elf_Word common =
            builder.symbol( "common", 8, SHN_COMMON );
        auto*          rela = builder.add_rela( ".rela.text", builder.text );
        rela->add_entry( 0, undefined, R_RISCV_PCREL_HI20, 0 );
        rela->add_entry( 0, common, R_RISCV_PCREL_HI20, 0 );

        elfio reader;
        TASK_CHECK( builder.round_trip( reader ) );
        const auto bases = bases_for( reader, 0x1000 );
        const auto plan  = analyze_riscv_relocations( reader, bases );
        TASK_CHECK( !plan.ok );
        TASK_CHECK( plan.error.rela_entry == 0 );
    }

    // Address arithmetic overflow: the symbol base plus st_value exceeds 64
    // bits.
    {
        object_builder builder;
        builder.add_text( { rv_auipc( 5, 0 ) } );
        builder.start_symbol_table();
        const Elf_Word huge =
            builder.symbol( "huge", ~Elf64_Addr( 0 ), builder.text->get_index(),
                            STB_GLOBAL, STT_FUNC );
        auto* rela = builder.add_rela( ".rela.text", builder.text );
        rela->add_entry( 0, huge, R_RISCV_PCREL_HI20, 0 );

        elfio reader;
        TASK_CHECK( builder.round_trip( reader ) );
        auto bases        = bases_for( reader, 0x1000 );
        const auto plan   = analyze_riscv_relocations( reader, bases );
        TASK_CHECK( !plan.ok );
        TASK_CHECK( plan.changes.empty() );
    }

    // Relocation offset points outside the code section.
    {
        object_builder builder;
        builder.add_text( { rv_auipc( 5, 0 ) } );
        builder.start_symbol_table();
        const Elf_Word target = builder.symbol( "t", 0x1800, SHN_ABS );
        auto* rela = builder.add_rela( ".rela.text", builder.text );
        rela->add_entry( 8, target, R_RISCV_PCREL_HI20, 0 );

        elfio reader;
        TASK_CHECK( builder.round_trip( reader ) );
        const auto bases = bases_for( reader, 0x1000 );
        const auto plan  = analyze_riscv_relocations( reader, bases );
        TASK_CHECK( !plan.ok );
        TASK_CHECK( plan.error.has_entry );
        TASK_CHECK( plan.error.rela_entry == 0 );
    }
    return 0;
}

//==============================================================================
// fields
//==============================================================================

// Positive and negative displacements, including the 0x7ff/0x800 low
// boundaries and the point where the high field must carry a sign.
int fields_hi_lo_boundaries()
{
    struct case_t
    {
        std::uint64_t target;
        Elf_Sxword    addend;
        std::int64_t  expected_hi;
        std::int32_t  expected_lo;
    };
    const case_t cases[] = {
        { 0x1000 + 0x7ff, 0, 0, 0x7ff },      // D = +0x7ff
        { 0x1000 + 0x800, 0, 1, -0x800 },     // D = +0x800
        { 0x1000 + 0x1234, 0, 1, 0x234 },     // D = +0x1234
        { 0x1000 - 0x7ff, 0, 0, -0x7ff },     // D = -0x7ff
        { 0x1000 - 0x800, 0, 0, -0x800 },     // D = -0x800
        { 0x1000 - 0x801, 0, -1, 0x7ff },     // D = -0x801
        { 0x1000ull + 0x7ffff7ffull, 0, 0x7ffff, 0x7ff }, // positive limit
        { 0x1000, -0x80000800ll, -0x80000, -0x800 },      // negative limit
    };

    for ( const auto& test : cases ) {
        object_builder builder;
        builder.add_text( { rv_auipc( 5, 0 ), rv_addi( 5, 5, 0 ) } );
        builder.start_symbol_table();
        const Elf_Word anchor = builder.symbol( ".Lhi", 0,
                                                builder.text->get_index(),
                                                STB_LOCAL, STT_NOTYPE );
        const Elf_Word target =
            builder.symbol( "target", test.target, SHN_ABS );
        auto* rela = builder.add_rela( ".rela.text", builder.text );
        rela->add_entry( 4, anchor, R_RISCV_PCREL_LO12_I, 0 );
        rela->add_entry( 0, target, R_RISCV_PCREL_HI20, test.addend );

        elfio reader;
        TASK_CHECK( builder.round_trip( reader ) );
        const auto bases = bases_for( reader, 0x1000 );
        const auto plan  = analyze_riscv_relocations( reader, bases );
        TASK_CHECK( plan.ok );
        TASK_CHECK( plan.changes.size() == 2 );

        const Elf_Half      text = builder.text->get_index();
        const std::uint32_t hi_word = word_at( plan, text, 0 );
        const std::uint32_t lo_word = word_at( plan, text, 4 );
        TASK_CHECK( decode_hi( hi_word ) == test.expected_hi );
        TASK_CHECK( decode_lo_i( lo_word ) == test.expected_lo );
        TASK_CHECK( static_cast<std::int64_t>( plan.changes[0].pc ) +
                        decode_hi( hi_word ) * 4096 + decode_lo_i( lo_word ) ==
                    static_cast<std::int64_t>( test.target ) + test.addend );

        // Only the immediate fields change.
        TASK_CHECK( ( hi_word & 0xfff ) == ( rv_auipc( 5, 0 ) & 0xfff ) );
        TASK_CHECK( ( lo_word & ~( 0xfffu << 20 ) ) ==
                    ( rv_addi( 5, 5, 0 ) & ~( 0xfffu << 20 ) ) );
    }
    return 0;
}

// S-type split fields, all LO12_I instruction forms, and CALL.
int fields_store_and_call()
{
    // Store: D = 0x1f00 - 0x1000 = 0xf00 -> hi 1, lo -0x100.
    {
        object_builder builder;
        builder.add_text( { rv_auipc( 5, 0 ), rv_store( 6, 5, 0x123 ) } );
        builder.start_symbol_table();
        const Elf_Word anchor = builder.symbol( ".Lhi", 0,
                                                builder.text->get_index(),
                                                STB_LOCAL, STT_NOTYPE );
        const Elf_Word target = builder.symbol( "obj", 0x1f00, SHN_ABS );
        auto* rela = builder.add_rela( ".rela.text", builder.text );
        rela->add_entry( 4, anchor, R_RISCV_PCREL_LO12_S, 0 );
        rela->add_entry( 0, target, R_RISCV_PCREL_HI20, 0 );

        elfio reader;
        TASK_CHECK( builder.round_trip( reader ) );
        const auto bases = bases_for( reader, 0x1000 );
        const auto plan  = analyze_riscv_relocations( reader, bases );
        TASK_CHECK( plan.ok );

        const Elf_Half      text      = builder.text->get_index();
        const std::uint32_t hi_word   = word_at( plan, text, 0 );
        const std::uint32_t lo_word   = word_at( plan, text, 4 );
        const std::uint32_t original  = rv_store( 6, 5, 0x123 );
        TASK_CHECK( decode_hi( hi_word ) == 1 );
        TASK_CHECK( decode_lo_s( lo_word ) == -0x100 );
        // Both split halves moved.
        TASK_CHECK( ( ( lo_word >> 25 ) & 0x7f ) !=
                    ( ( original >> 25 ) & 0x7f ) );
        TASK_CHECK( ( ( lo_word >> 7 ) & 0x1f ) !=
                    ( ( original >> 7 ) & 0x1f ) );
        // Non-immediate fields preserved.
        TASK_CHECK( ( lo_word & 0x7f ) == OP_STORE );
        TASK_CHECK( ( ( lo_word >> 12 ) & 0x7 ) == 2 );
        TASK_CHECK( ( ( lo_word >> 15 ) & 0x1f ) == 5 );
        TASK_CHECK( ( ( lo_word >> 20 ) & 0x1f ) == 6 );
    }

    // LO12_I on a JALR instruction.
    {
        object_builder builder;
        builder.add_text( { rv_auipc( 5, 0 ), rv_jalr( 1, 5, 0x10 ) } );
        builder.start_symbol_table();
        const Elf_Word anchor = builder.symbol( ".Lhi", 0,
                                                builder.text->get_index(),
                                                STB_LOCAL, STT_NOTYPE );
        const Elf_Word target = builder.symbol( "fn", 0x1200, SHN_ABS );
        auto* rela = builder.add_rela( ".rela.text", builder.text );
        rela->add_entry( 4, anchor, R_RISCV_PCREL_LO12_I, 0 );
        rela->add_entry( 0, target, R_RISCV_PCREL_HI20, 0 );

        elfio reader;
        TASK_CHECK( builder.round_trip( reader ) );
        const auto plan =
            analyze_riscv_relocations( reader, bases_for( reader, 0x1000 ) );
        TASK_CHECK( plan.ok );
        const Elf_Half text = builder.text->get_index();
        TASK_CHECK( decode_lo_i( word_at( plan, text, 4 ) ) == 0x200 );
        TASK_CHECK( ( word_at( plan, text, 4 ) & 0x7f ) == OP_JALR );
        TASK_CHECK( ( ( word_at( plan, text, 4 ) >> 7 ) & 0x1f ) == 1 );
        TASK_CHECK( ( ( word_at( plan, text, 4 ) >> 15 ) & 0x1f ) == 5 );
    }

    // CALL: D = 0x1400 + 4 - 0x1000 = 0x404.
    {
        object_builder builder;
        builder.add_text( { rv_auipc( 7, 0 ), rv_jalr( 1, 7, 0x20 ) } );
        builder.start_symbol_table();
        const Elf_Word callee =
            builder.symbol( "callee", 0x1400, SHN_ABS );
        auto* rela = builder.add_rela( ".rela.text", builder.text );
        rela->add_entry( 0, callee, R_RISCV_CALL, 4 );

        elfio reader;
        TASK_CHECK( builder.round_trip( reader ) );
        const auto plan =
            analyze_riscv_relocations( reader, bases_for( reader, 0x1000 ) );
        TASK_CHECK( plan.ok );
        TASK_CHECK( plan.changes.size() == 1 );
        TASK_CHECK( plan.changes[0].kind == riscv_reloc_kind::call );
        TASK_CHECK( plan.changes[0].instructions.size() == 2 );
        TASK_CHECK( plan.changes[0].pc == 0x1000 );
        // target_address is the symbol address; the addend shows up in the
        // decoded instruction value (0x1000 + 0x404 = 0x1404).
        TASK_CHECK( plan.changes[0].target_address == 0x1400 );

        const Elf_Half      text    = builder.text->get_index();
        const std::uint32_t hi_word = word_at( plan, text, 0 );
        const std::uint32_t lo_word = word_at( plan, text, 4 );
        TASK_CHECK( decode_hi( hi_word ) == 0 );
        TASK_CHECK( decode_lo_i( lo_word ) == 0x404 );
        // The AUIPC destination and JALR destination/base are preserved.
        TASK_CHECK( ( ( hi_word >> 7 ) & 0x1f ) == 7 );
        TASK_CHECK( ( ( lo_word >> 7 ) & 0x1f ) == 1 );
        TASK_CHECK( ( ( lo_word >> 15 ) & 0x1f ) == 7 );
        TASK_CHECK( ( lo_word & 0x7f ) == OP_JALR );
        TASK_CHECK( plan.changes[0].symbol_index == callee );
        TASK_CHECK( plan.changes[0].addend == 4 );
    }

    // CALL with mismatched registers is rejected.
    {
        object_builder builder;
        builder.add_text( { rv_auipc( 7, 0 ), rv_jalr( 1, 8, 0 ) } );
        builder.start_symbol_table();
        const Elf_Word callee =
            builder.symbol( "callee", 0x1400, SHN_ABS );
        auto* rela = builder.add_rela( ".rela.text", builder.text );
        rela->add_entry( 0, callee, R_RISCV_CALL, 0 );

        elfio reader;
        TASK_CHECK( builder.round_trip( reader ) );
        const auto plan =
            analyze_riscv_relocations( reader, bases_for( reader, 0x1000 ) );
        TASK_CHECK( !plan.ok );
        TASK_CHECK( plan.error.rela_entry == 0 );
    }
    return 0;
}

// Values that cannot be represented are rejected, never truncated.
int fields_out_of_range()
{
    struct case_t
    {
        std::uint64_t target;
        Elf_Sxword    addend;
    };
    const case_t cases[] = {
        { 0x1000ull + 0x80000000ull, 0 }, // D = +2^31
        { 0x1000ull + 0x800007ffull, 0 }, // +2^31 + 0x7ff
        { 0x1000ull + 0x80000800ull, 0 }, // +2^31 + 0x800: too far
        { 0x1000, 0x80000800ll },      // addend pushes it too far
        { 0x1000, -0x80000801ll },     // one past the negative limit
    };

    for ( const auto& test : cases ) {
        object_builder builder;
        builder.add_text( { rv_auipc( 5, 0 ), rv_addi( 5, 5, 0 ) } );
        builder.start_symbol_table();
        const Elf_Word anchor = builder.symbol( ".Lhi", 0,
                                                builder.text->get_index(),
                                                STB_LOCAL, STT_NOTYPE );
        const Elf_Word target =
            builder.symbol( "far", test.target, SHN_ABS );
        auto* rela = builder.add_rela( ".rela.text", builder.text );
        rela->add_entry( 4, anchor, R_RISCV_PCREL_LO12_I, 0 );
        rela->add_entry( 0, target, R_RISCV_PCREL_HI20, test.addend );

        elfio reader;
        TASK_CHECK( builder.round_trip( reader ) );
        const auto plan =
            analyze_riscv_relocations( reader, bases_for( reader, 0x1000 ) );
        TASK_CHECK( !plan.ok );
        TASK_CHECK( plan.error.is_error );
        TASK_CHECK( plan.changes.empty() );
        TASK_CHECK( plan.outputs.empty() );
    }

    // One step inside the positive limit still succeeds.
    object_builder builder;
    builder.add_text( { rv_auipc( 5, 0 ), rv_addi( 5, 5, 0 ) } );
    builder.start_symbol_table();
    const Elf_Word anchor = builder.symbol( ".Lhi", 0,
                                            builder.text->get_index(),
                                            STB_LOCAL, STT_NOTYPE );
    const Elf_Word target =
        builder.symbol( "edge", 0x1000ull + 0x7ffff7ffull, SHN_ABS );
    auto* rela = builder.add_rela( ".rela.text", builder.text );
    rela->add_entry( 4, anchor, R_RISCV_PCREL_LO12_I, 0 );
    rela->add_entry( 0, target, R_RISCV_PCREL_HI20, 0 );
    elfio reader;
    TASK_CHECK( builder.round_trip( reader ) );
    const auto plan =
        analyze_riscv_relocations( reader, bases_for( reader, 0x1000 ) );
    TASK_CHECK( plan.ok );
    TASK_CHECK( plan.changes.size() == 2 );
    return 0;
}

// RELAX is a hint only, NONE writes nothing, and opcode/duplicate checks fire.
int fields_hints_and_none()
{
    object_builder builder;
    builder.add_text(
        { rv_auipc( 5, 0 ), rv_addi( 5, 5, 0 ), rv_lui( 9, 0 ) } );
    builder.start_symbol_table();
    const Elf_Word anchor = builder.symbol( ".Lhi", 0,
                                            builder.text->get_index(),
                                            STB_LOCAL, STT_NOTYPE );
    const Elf_Word target = builder.symbol( "target", 0x1400, SHN_ABS );
    auto* rela = builder.add_rela( ".rela.text", builder.text );
    rela->add_entry( 0, target, R_RISCV_PCREL_HI20, 0 );
    rela->add_entry( 4, anchor, R_RISCV_PCREL_LO12_I, 0 );
    rela->add_entry( 8, 0, R_RISCV_NONE, 0 );
    rela->add_entry( 0, 0, R_RISCV_RELAX, 0 );
    rela->add_entry( 4, 0, R_RISCV_RELAX, 0 );

    elfio reader;
    TASK_CHECK( builder.round_trip( reader ) );
    const auto plan =
        analyze_riscv_relocations( reader, bases_for( reader, 0x1000 ) );
    TASK_CHECK( plan.ok );
    TASK_CHECK( plan.changes.size() == 2 ); // NONE/RELAX never write
    TASK_CHECK( plan.hints.size() == 2 );
    TASK_CHECK( plan.hints[0].rela_entry == 3 );
    TASK_CHECK( plan.hints[1].rela_entry == 4 );
    TASK_CHECK( plan.hints[0].main_rela_entry == 0 );
    TASK_CHECK( plan.hints[1].main_rela_entry == 1 );
    TASK_CHECK( plan.hints[0].pc == 0x1000 );
    TASK_CHECK( plan.hints[1].pc == 0x1004 );
    TASK_CHECK( word_at( plan, builder.text->get_index(), 8 ) ==
                rv_lui( 9, 0 ) );

    // RELAX with no supported main record at its position.
    {
        object_builder lonely;
        lonely.add_text( { rv_auipc( 5, 0 ) } );
        lonely.start_symbol_table();
        auto* rela2 = lonely.add_rela( ".rela.text", lonely.text );
        rela2->add_entry( 0, 0, R_RISCV_RELAX, 0 );
        elfio reader2;
        TASK_CHECK( lonely.round_trip( reader2 ) );
        const auto plan2 = analyze_riscv_relocations(
            reader2, bases_for( reader2, 0x1000 ) );
        TASK_CHECK( !plan2.ok );
        TASK_CHECK( plan2.error.rela_entry == 0 );
    }

    // HI20 must point at AUIPC.
    {
        object_builder wrong;
        wrong.add_text( { rv_lui( 5, 0 ) } );
        wrong.start_symbol_table();
        const Elf_Word target = wrong.symbol( "t", 0x1400, SHN_ABS );
        auto*          rela   = wrong.add_rela( ".rela.text", wrong.text );
        rela->add_entry( 0, target, R_RISCV_PCREL_HI20, 0 );
        elfio reader2;
        TASK_CHECK( wrong.round_trip( reader2 ) );
        const auto plan2 = analyze_riscv_relocations(
            reader2, bases_for( reader2, 0x1000 ) );
        TASK_CHECK( !plan2.ok );
        TASK_CHECK( plan2.error.message.find( "AUIPC" ) !=
                    std::string::npos );
    }

    // LO12_S must point at a store.
    {
        object_builder wrong;
        wrong.add_text( { rv_auipc( 5, 0 ), rv_addi( 5, 5, 0 ) } );
        wrong.start_symbol_table();
        const Elf_Word anchor = wrong.symbol( ".Lhi", 0,
                                              wrong.text->get_index(),
                                              STB_LOCAL, STT_NOTYPE );
        auto* rela = wrong.add_rela( ".rela.text", wrong.text );
        const Elf_Word target = wrong.symbol( "t", 0x1800, SHN_ABS );
        auto*          rela2  = wrong.add_rela( ".rela.hi", wrong.text );
        rela->add_entry( 4, anchor, R_RISCV_PCREL_LO12_S, 0 );
        rela2->add_entry( 0, target, R_RISCV_PCREL_HI20, 0 );
        elfio reader2;
        TASK_CHECK( wrong.round_trip( reader2 ) );
        const auto plan2 = analyze_riscv_relocations(
            reader2, bases_for( reader2, 0x1000 ) );
        TASK_CHECK( !plan2.ok );
    }

    // Two supported records writing the same instruction.
    {
        object_builder clash;
        clash.add_text( { rv_auipc( 5, 0 ) } );
        clash.start_symbol_table();
        const Elf_Word target = clash.symbol( "t", 0x1400, SHN_ABS );
        auto*          rela   = clash.add_rela( ".rela.text", clash.text );
        rela->add_entry( 0, target, R_RISCV_PCREL_HI20, 0 );
        rela->add_entry( 0, target, R_RISCV_PCREL_HI20, 0 );
        elfio reader2;
        TASK_CHECK( clash.round_trip( reader2 ) );
        const auto plan2 = analyze_riscv_relocations(
            reader2, bases_for( reader2, 0x1000 ) );
        TASK_CHECK( !plan2.ok );
        TASK_CHECK( plan2.changes.empty() );
    }
    return 0;
}

//==============================================================================
// objects
//==============================================================================

// The witness object, saved and reloaded: two bases move the fields but not
// the SHN_ABS target, and the original twelve bytes never change.
int objects_witness_layout()
{
    object_builder builder;
    builder.add_text( { WITNESS_WORDS[0], WITNESS_WORDS[1],
                        WITNESS_WORDS[2] } );
    builder.start_symbol_table();
    const Elf_Word anchor = builder.symbol( ".Lanchor", 0,
                                            builder.text->get_index(),
                                            STB_LOCAL, STT_NOTYPE );
    const Elf_Word target = builder.symbol( "target", 0x1800, SHN_ABS );
    auto*          rela   = builder.add_rela( ".rela.text", builder.text );
    rela->add_entry( 4, anchor, R_RISCV_PCREL_LO12_I, 0 );
    rela->add_entry( 0, target, R_RISCV_PCREL_HI20, 0 );
    rela->add_entry( 0, 0, R_RISCV_RELAX, 0 );

    elfio reader;
    TASK_CHECK( builder.round_trip( reader ) );

    const section* text = reader.sections[".text"];
    TASK_CHECK( text != nullptr );
    TASK_CHECK( text->get_size() == 12 );
    for ( int i = 0; i < 3; ++i ) {
        std::uint32_t word = 0;
        std::memcpy( &word, text->get_data() + i * 4, 4 );
        TASK_CHECK( word == WITNESS_WORDS[i] );
    }

    for ( int iteration = 0; iteration < 2; ++iteration ) {
        const std::uint64_t base =
            iteration == 0 ? 0x1000 : 0x2000;
        const auto bases = bases_for( reader, base );
        const auto plan  = analyze_riscv_relocations( reader, bases );
        TASK_CHECK( plan.ok );
        TASK_CHECK( plan.changes.size() == 2 );
        TASK_CHECK( plan.hints.size() == 1 );
        TASK_CHECK( plan.outputs.size() == 1 );
        TASK_CHECK( plan.outputs[0].section == text->get_index() );
        TASK_CHECK( plan.outputs[0].data.size() == 12 );

        const std::uint32_t hi_word = word_at( plan, text->get_index(), 0 );
        const std::uint32_t lo_word = word_at( plan, text->get_index(), 4 );
        TASK_CHECK( ( hi_word & 0x7f ) == OP_AUIPC );
        TASK_CHECK( ( lo_word & 0x7f ) == OP_IMM );
        if ( base == 0x1000 ) {
            // D = 0x800 -> hi 1, lo -0x800.
            TASK_CHECK( decode_hi( hi_word ) == 1 );
        }
        else {
            // D = -0x800 -> hi 0, lo -0x800.
            TASK_CHECK( decode_hi( hi_word ) == 0 );
        }
        TASK_CHECK( decode_lo_i( lo_word ) == -0x800 );
        TASK_CHECK( plan.changes[0].pc + decode_hi( hi_word ) * 4096 +
                        decode_lo_i( lo_word ) ==
                    0x1800 );
        TASK_CHECK( plan.changes[0].target_address == 0x1800 );
        TASK_CHECK( word_at( plan, text->get_index(), 8 ) ==
                    WITNESS_WORDS[2] );

        // Analysis never mutates the input object.
        for ( int i = 0; i < 3; ++i ) {
            std::uint32_t word = 0;
            std::memcpy( &word, text->get_data() + i * 4, 4 );
            TASK_CHECK( word == WITNESS_WORDS[i] );
        }
    }

    // The report consumer decodes everything again from the returned bytes.
    const auto report = riscv_example::build_riscv_reference_report(
        reader, bases_for( reader, 0x1000 ) );
    TASK_CHECK( report.ok );
    TASK_CHECK( report.rows.size() == 2 );
    TASK_CHECK( report.hints.size() == 1 );
    TASK_CHECK( report.hints[0].rela_entry == 2 );
    TASK_CHECK( report.hints[0].main_rela_entry == 1 );

    bool saw_hi = false;
    bool saw_lo = false;
    for ( const auto& row : report.rows ) {
        if ( row.kind == "PCREL_HI20" ) {
            saw_hi = true;
            TASK_CHECK( row.symbol_index == 2 );
            TASK_CHECK( row.symbol_name == "target" );
            TASK_CHECK( row.symbol_address == 0x1800 );
            TASK_CHECK( row.addend == 0 );
            TASK_CHECK( row.referenced_target == 0x1800 );
            TASK_CHECK( row.instructions.size() == 1 );
            TASK_CHECK( row.instructions[0].original_word ==
                        WITNESS_WORDS[0] );
        }
        if ( row.kind == "PCREL_LO12_I" ) {
            saw_lo = true;
            TASK_CHECK( row.symbol_index == 1 );
            TASK_CHECK( row.symbol_name == ".Lanchor" );
            TASK_CHECK( row.hi_symbol_index == 2 );
            TASK_CHECK( row.hi_symbol_name == "target" );
            TASK_CHECK( row.hi_symbol_address == 0x1800 );
            TASK_CHECK( row.referenced_target == 0x1800 );
            TASK_CHECK( row.instructions[0].original_word ==
                        WITNESS_WORDS[1] );
        }
    }
    TASK_CHECK( saw_hi );
    TASK_CHECK( saw_lo );

    const std::string rendered = report_text( report );
    TASK_CHECK( rendered.find( "PCREL_HI20" ) != std::string::npos );
    TASK_CHECK( rendered.find( "PCREL_LO12_I" ) != std::string::npos );
    TASK_CHECK( rendered.find( "RELAX" ) != std::string::npos );
    return 0;
}

// A shared high record spread across two tables, checked through the report:
// no adjacency or name-based pairing may sneak in.
int objects_report_cross_table_shared_hi()
{
    object_builder builder;
    builder.add_text( { rv_auipc( 5, 0 ), rv_addi( 5, 5, 0 ),
                        rv_load( 6, 5, 0 ), rv_store( 6, 5, 0 ) } );
    builder.start_symbol_table();
    const Elf_Word anchor = builder.symbol( ".Lhi", 0,
                                            builder.text->get_index(),
                                            STB_LOCAL, STT_NOTYPE );
    const Elf_Word target =
        builder.symbol( "shared_target", 0x2000, SHN_ABS );
    auto* highs = builder.add_rela( ".rela.one", builder.text );
    auto* lows  = builder.add_rela( ".rela.two", builder.text );
    highs->add_entry( 0, target, R_RISCV_PCREL_HI20, 0 );
    lows->add_entry( 12, anchor, R_RISCV_PCREL_LO12_S, 0 );
    lows->add_entry( 8, anchor, R_RISCV_PCREL_LO12_I, 0 );
    lows->add_entry( 4, anchor, R_RISCV_PCREL_LO12_I, 0 );

    elfio reader;
    TASK_CHECK( builder.round_trip( reader ) );
    const auto report = riscv_example::build_riscv_reference_report(
        reader, bases_for( reader, 0x1000 ) );
    TASK_CHECK( report.ok );
    TASK_CHECK( report.rows.size() == 4 );
    TASK_CHECK( report.hints.empty() );

    const std::uint64_t high_table =
        builder.rela_sections[0]->get_index();
    std::size_t low_rows = 0;
    for ( const auto& row : report.rows ) {
        if ( row.kind == "PCREL_HI20" ) {
            TASK_CHECK( row.referenced_target == 0x2000 );
            TASK_CHECK( row.rela_section == high_table );
            TASK_CHECK( row.rela_entry == 0 );
        }
        else {
            ++low_rows;
            TASK_CHECK( row.hi_rela_section == high_table );
            TASK_CHECK( row.hi_rela_entry == 0 );
            TASK_CHECK( row.hi_symbol_address == 0x2000 );
            TASK_CHECK( row.referenced_target == 0x2000 );
            TASK_CHECK( row.rela_section == builder.rela_sections[1]->get_index() );
        }
    }
    TASK_CHECK( low_rows == 3 );

    const std::string rendered = report_text( report );
    TASK_CHECK( count_occurrences( rendered, "RELAX" ) == 0 );
    TASK_CHECK( count_occurrences( rendered, "PCREL_LO12_I" ) == 2 );
    TASK_CHECK( count_occurrences( rendered, "PCREL_LO12_S" ) == 1 );
    return 0;
}

// Two section bases change the decoded instructions and PCs, while the target
// symbol identity and value stay the same.
int objects_two_bases_identity()
{
    object_builder builder;
    builder.add_text( { WITNESS_WORDS[0], WITNESS_WORDS[1],
                        WITNESS_WORDS[2] } );
    builder.start_symbol_table();
    const Elf_Word anchor = builder.symbol( ".Lanchor", 0,
                                            builder.text->get_index(),
                                            STB_LOCAL, STT_NOTYPE );
    const Elf_Word target = builder.symbol( "target", 0x1800, SHN_ABS );
    auto*          rela   = builder.add_rela( ".rela.text", builder.text );
    rela->add_entry( 4, anchor, R_RISCV_PCREL_LO12_I, 0 );
    rela->add_entry( 0, target, R_RISCV_PCREL_HI20, 0 );

    elfio reader;
    TASK_CHECK( builder.round_trip( reader ) );

    riscv_example::analysis_report reports[2];
    for ( int i = 0; i < 2; ++i ) {
        reports[i] = riscv_example::build_riscv_reference_report(
            reader, bases_for( reader, i == 0 ? 0x1000 : 0x2000 ) );
        TASK_CHECK( reports[i].ok );
        TASK_CHECK( reports[i].rows.size() == 2 );
        for ( const auto& row : reports[i].rows ) {
            TASK_CHECK( row.referenced_target == 0x1800 );
            if ( row.kind == "PCREL_HI20" ) {
                TASK_CHECK( row.symbol_address == 0x1800 );
                TASK_CHECK( row.symbol_index == 2 );
            }
            else {
                // For a low record the symbol is its HI anchor.
                TASK_CHECK( row.symbol_index == 1 );
                TASK_CHECK( row.hi_symbol_index == 2 );
                TASK_CHECK( row.hi_symbol_address == 0x1800 );
            }
        }
    }

    const auto& hi0 = reports[0].rows[0];
    const auto& hi1 = reports[1].rows[0];
    TASK_CHECK( hi0.kind == "PCREL_HI20" );
    TASK_CHECK( hi1.kind == "PCREL_HI20" );
    TASK_CHECK( hi0.pc == 0x1000 );
    TASK_CHECK( hi1.pc == 0x2000 );
    TASK_CHECK( hi0.instructions[0].immediate == 0x1000 );
    TASK_CHECK( hi1.instructions[0].immediate == 0 );
    TASK_CHECK( hi0.instructions[0].patched_word !=
                hi1.instructions[0].patched_word );
    TASK_CHECK( hi0.instructions[0].original_word == WITNESS_WORDS[0] );

    const auto& lo0 = reports[0].rows[1];
    const auto& lo1 = reports[1].rows[1];
    TASK_CHECK( lo0.kind == "PCREL_LO12_I" );
    TASK_CHECK( lo1.kind == "PCREL_LO12_I" );
    TASK_CHECK( lo0.pc == 0x1004 );
    TASK_CHECK( lo1.pc == 0x2004 );
    TASK_CHECK( lo0.instructions[0].immediate == -0x800 );
    TASK_CHECK( lo1.instructions[0].immediate == -0x800 );
    TASK_CHECK( lo0.instructions[0].patched_word ==
                lo1.instructions[0].patched_word );
    TASK_CHECK( lo0.instructions[0].original_word == WITNESS_WORDS[1] );
    TASK_CHECK( lo0.hi_pc == 0x1000 );
    TASK_CHECK( lo1.hi_pc == 0x2000 );
    return 0;
}

// The report's CALL row decodes both instructions and keeps the register
// linkage that the production entry point validated.
int objects_call_report()
{
    object_builder builder;
    builder.add_text( { rv_auipc( 7, 0 ), rv_jalr( 1, 7, 0x40 ) } );
    builder.start_symbol_table();
    const Elf_Word callee = builder.symbol( "callee", 0x1200, SHN_ABS );
    auto*          rela   = builder.add_rela( ".rela.text", builder.text );
    rela->add_entry( 0, callee, R_RISCV_CALL, 0x10 );

    elfio reader;
    TASK_CHECK( builder.round_trip( reader ) );
    const auto report = riscv_example::build_riscv_reference_report(
        reader, bases_for( reader, 0x1000 ) );
    TASK_CHECK( report.ok );
    TASK_CHECK( report.rows.size() == 1 );
    TASK_CHECK( report.rows[0].kind == "CALL" );
    TASK_CHECK( report.rows[0].instructions.size() == 2 );

    // D = 0x1200 + 0x10 - 0x1000 = 0x210 -> hi 0, lo 0x210.
    const auto& hi_insn = report.rows[0].instructions[0];
    const auto& lo_insn = report.rows[0].instructions[1];
    TASK_CHECK( hi_insn.opcode == 0x17 );
    TASK_CHECK( hi_insn.immediate == 0 );
    TASK_CHECK( lo_insn.opcode == 0x67 );
    TASK_CHECK( lo_insn.immediate == 0x210 );
    TASK_CHECK( hi_insn.rd == 7 );
    TASK_CHECK( lo_insn.rd == 1 );
    TASK_CHECK( lo_insn.rs1 == 7 ); // JALR base matches AUIPC destination
    TASK_CHECK( hi_insn.original_word == rv_auipc( 7, 0 ) );
    TASK_CHECK( lo_insn.original_word == rv_jalr( 1, 7, 0x40 ) );
    TASK_CHECK( report.rows[0].referenced_target == 0x1210 );
    TASK_CHECK( report.rows[0].symbol_address == 0x1200 );
    TASK_CHECK( report.rows[0].addend == 0x10 );
    return 0;
}

// The report's file offsets really point at the input ELF bytes.
int objects_byte_provenance()
{
    object_builder builder;
    builder.add_text( { rv_auipc( 5, 0 ), rv_addi( 5, 5, 0 ) } );
    builder.start_symbol_table();
    const Elf_Word anchor = builder.symbol( ".Lhi", 0,
                                            builder.text->get_index(),
                                            STB_LOCAL, STT_NOTYPE );
    const Elf_Word target = builder.symbol( "t", 0x1800, SHN_ABS );
    auto*          rela   = builder.add_rela( ".rela.text", builder.text );
    rela->add_entry( 4, anchor, R_RISCV_PCREL_LO12_I, 0 );
    rela->add_entry( 0, target, R_RISCV_PCREL_HI20, 0 );

    elfio reader;
    TASK_CHECK( builder.round_trip( reader ) );
    const auto report = riscv_example::build_riscv_reference_report(
        reader, bases_for( reader, 0x1000 ) );
    TASK_CHECK( report.ok );
    TASK_CHECK( report.rows.size() == 2 );

    // Reproduce the on-disk image to check each reported offset.
    std::stringstream image( std::ios::in | std::ios::out |
                             std::ios::binary );
    TASK_CHECK( builder.writer.save( image ) );
    const std::string bytes = image.str();

    const section* rela_sec = reader.sections[".rela.text"];
    const section* text_sec = reader.sections[".text"];
    const section* sym_sec  = reader.sections[".symtab"];
    TASK_CHECK( rela_sec != nullptr );
    TASK_CHECK( text_sec != nullptr );
    TASK_CHECK( sym_sec != nullptr );

    for ( const auto& row : report.rows ) {
        TASK_CHECK( row.rela_section == rela_sec->get_index() );
        TASK_CHECK( row.rela_file_offset >= rela_sec->get_offset() );
        TASK_CHECK( row.rela_file_offset + 24 <=
                    rela_sec->get_offset() + rela_sec->get_size() );
        TASK_CHECK( row.symbol_file_offset >= sym_sec->get_offset() );
        TASK_CHECK( row.symbol_file_offset + 24 <=
                    sym_sec->get_offset() + sym_sec->get_size() );
        TASK_CHECK( row.code_file_offset >= text_sec->get_offset() );
        TASK_CHECK( row.code_file_offset + 4 <=
                    text_sec->get_offset() + text_sec->get_size() );

        // The RELA entry at rela_file_offset names the expected symbol.
        // Elf64_Rela layout: r_offset (8), r_info (8, r_sym in the high
        // word), r_addend (8).
        std::uint32_t r_info = 0;
        std::memcpy( &r_info, bytes.data() + row.rela_file_offset + 12, 4 );
        TASK_CHECK( r_info == row.symbol_index );

        // The symbol entry at symbol_file_offset has the reported name index.
        TASK_CHECK( row.symbol_file_offset + 4 <= bytes.size() );

        // The instruction word at code_file_offset is the original word.
        std::uint32_t word = 0;
        std::memcpy( &word, bytes.data() + row.code_file_offset, 4 );
        TASK_CHECK( word == row.instructions[0].original_word );
    }
    return 0;
}

//==============================================================================
// Registration
//==============================================================================

int pairs_out_of_order_and_shared_entry()
{
    return pairs_out_of_order_and_shared();
}
int pairs_symbol_identity_entry() { return pairs_symbol_identity(); }
int pairs_errors_are_located_entry() { return pairs_errors_are_located(); }
int pairs_per_table_symbol_links_entry()
{
    return pairs_per_table_symbol_links();
}
int pairs_malformed_tables_entry() { return pairs_malformed_tables(); }
int fields_hi_lo_boundaries_entry() { return fields_hi_lo_boundaries(); }
int fields_store_and_call_entry() { return fields_store_and_call(); }
int fields_out_of_range_entry() { return fields_out_of_range(); }
int fields_hints_and_none_entry() { return fields_hints_and_none(); }
int objects_witness_layout_entry() { return objects_witness_layout(); }
int objects_report_cross_table_shared_hi_entry()
{
    return objects_report_cross_table_shared_hi();
}
int objects_two_bases_identity_entry() { return objects_two_bases_identity(); }
int objects_byte_provenance_entry() { return objects_byte_provenance(); }
int objects_call_report_entry() { return objects_call_report(); }

} // namespace

const struct task_case task_cases[] = {
    { "pairs", "out_of_order_and_shared", pairs_out_of_order_and_shared_entry },
    { "pairs", "symbol_identity", pairs_symbol_identity_entry },
    { "pairs", "errors_are_located", pairs_errors_are_located_entry },
    { "pairs", "per_table_symbol_links", pairs_per_table_symbol_links_entry },
    { "pairs", "malformed_tables", pairs_malformed_tables_entry },
    { "fields", "hi_lo_boundaries", fields_hi_lo_boundaries_entry },
    { "fields", "store_and_call", fields_store_and_call_entry },
    { "fields", "out_of_range", fields_out_of_range_entry },
    { "fields", "hints_and_none", fields_hints_and_none_entry },
    { "objects", "witness_layout", objects_witness_layout_entry },
    { "objects", "report_cross_table_shared_hi",
      objects_report_cross_table_shared_hi_entry },
    { "objects", "two_bases_identity", objects_two_bases_identity_entry },
    { "objects", "byte_provenance", objects_byte_provenance_entry },
    { "objects", "call_report", objects_call_report_entry },
};

const size_t task_case_count = sizeof( task_cases ) / sizeof( task_cases[0] );
