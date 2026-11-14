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

/*
 * This example builds a minimal RISC-V ET_REL object in memory, then runs the
 * offline relocation planner make_riscv_reloc_plan() on it. No linker is
 * invoked and no instruction is executed: the planner only reads the object
 * and returns independently owned, relocated section bytes plus a description
 * of every instruction word it changed.
 *
 * Build and run:
 *   c++ -std=c++17 -I.. riscv_reloc.cpp -o riscv_reloc
 *   ./riscv_reloc
 */

#include <cstdint>
#include <iostream>
#include <sstream>
#include <vector>

#include <elfio/elfio.hpp>

using namespace ELFIO;

namespace {

// Hand-written, uncompressed RISC-V instruction words.
std::uint32_t auipc( std::uint32_t rd ) { return ( rd << 7 ) | 0x17U; }
std::uint32_t addi( std::uint32_t rd, std::uint32_t rs1 )
{
    return ( rd << 7 ) | ( rs1 << 15 ) | 0x13U;
}
std::uint32_t sw( std::uint32_t rs2, std::uint32_t rs1 )
{
    return ( rs2 << 20 ) | ( rs1 << 15 ) | ( 2U << 12 ) | 0x23U;
}
std::uint32_t jalr( std::uint32_t rd, std::uint32_t rs1 )
{
    return ( rd << 7 ) | ( rs1 << 15 ) | 0x67U;
}

void append_words( section* s, const std::vector<std::uint32_t>& words )
{
    std::vector<unsigned char> bytes;
    for ( std::uint32_t word : words ) {
        for ( int k = 0; k < 4; ++k ) {
            bytes.push_back(
                static_cast<unsigned char>( ( word >> ( 8 * k ) ) & 0xFFU ) );
        }
    }
    s->set_data( reinterpret_cast<const char*>( bytes.data() ),
                 bytes.size() );
}

const char* kind_name( riscv_reloc_kind kind )
{
    switch ( kind ) {
    case riscv_reloc_kind::pcrel_hi20:
        return "PCREL_HI20";
    case riscv_reloc_kind::pcrel_lo12_i:
        return "PCREL_LO12_I";
    case riscv_reloc_kind::pcrel_lo12_s:
        return "PCREL_LO12_S";
    case riscv_reloc_kind::call:
        return "CALL";
    }
    return "?";
}

} // namespace

int main()
{
    constexpr Elf64_Addr TEXT_BASE   = 0x10000;
    constexpr Elf64_Addr RODATA_BASE = 0x80000;

    //--------------------------------------------------------------------------
    // Build the object: one executable section with three relocation sites.
    //   0:  AUIPC x1            <- PCREL_HI20 (absolute symbol "data")
    //   4:  ADDI  x2, x1        <- PCREL_LO12_I (anchor ".La")
    //   8:  AUIPC x3            <- CALL high part (symbol "fn" in .rodata)
    //   12: JALR  x7, x3        <- CALL low part
    //   16: AUIPC x1            <- PCREL_HI20 for the store below
    //   20: SW    x4, 0(x1)     <- PCREL_LO12_S (anchor ".Ls")
    //--------------------------------------------------------------------------
    elfio writer;
    writer.create( ELFCLASS64, ELFDATA2LSB );
    writer.set_type( ET_REL );
    writer.set_machine( EM_RISCV );

    section* text = writer.sections.add( ".text" );
    text->set_type( SHT_PROGBITS );
    text->set_flags( SHF_ALLOC | SHF_EXECINSTR );
    text->set_addr_align( 4 );
    append_words( text, { auipc( 1 ), addi( 2, 1 ), auipc( 3 ),
                          jalr( 7, 3 ), auipc( 1 ), sw( 4, 1 ) } );

    section* rodata = writer.sections.add( ".rodata" );
    rodata->set_type( SHT_PROGBITS );
    rodata->set_flags( SHF_ALLOC );
    rodata->set_addr_align( 4 );
    rodata->set_data( std::string( 32, '\0' ) );

    section* strtab = writer.sections.add( ".strtab" );
    strtab->set_type( SHT_STRTAB );

    section* symtab = writer.sections.add( ".symtab" );
    symtab->set_type( SHT_SYMTAB );
    symtab->set_link( strtab->get_index() );
    symtab->set_info( 1 );
    symtab->set_addr_align( 8 );
    symtab->set_entry_size( writer.get_default_entry_size( SHT_SYMTAB ) );

    string_section_accessor strings( strtab );
    symbol_section_accessor symbols( writer, symtab );
    const Elf_Word anchor_i = symbols.add_symbol(
        strings, ".La", 0, 0, STB_LOCAL, STT_NOTYPE, 0, text->get_index() );
    const Elf_Word anchor_s = symbols.add_symbol(
        strings, ".Ls", 16, 0, STB_LOCAL, STT_NOTYPE, 0, text->get_index() );
    const Elf_Word data_sym = symbols.add_symbol(
        strings, "data", 0x21004, 0, STB_GLOBAL, STT_OBJECT, 0, SHN_ABS );
    const Elf_Word fn_sym = symbols.add_symbol(
        strings, "fn", 0x40, 0, STB_GLOBAL, STT_FUNC, 0,
        rodata->get_index() );

    section* rel = writer.sections.add( ".rela.text" );
    rel->set_type( SHT_RELA );
    rel->set_info( text->get_index() );
    rel->set_link( symtab->get_index() );
    rel->set_addr_align( 8 );
    rel->set_entry_size( writer.get_default_entry_size( SHT_RELA ) );

    relocation_section_accessor rela( writer, rel );
    rela.add_entry( 0, data_sym, R_RISCV_PCREL_HI20, 0 );
    rela.add_entry( 4, anchor_i, R_RISCV_PCREL_LO12_I, 0 );
    rela.add_entry( 8, fn_sym, R_RISCV_CALL, 0 );
    rela.add_entry( 16, data_sym, R_RISCV_PCREL_HI20, 0 );
    rela.add_entry( 20, anchor_s, R_RISCV_PCREL_LO12_S, 0 );

    // Round-trip through a stream so the planner sees a plain loaded object.
    std::stringstream stream( std::ios::in | std::ios::out |
                              std::ios::binary );
    if ( !writer.save( stream ) ) {
        std::cerr << "failed to save object\n";
        return 1;
    }
    elfio reader;
    stream.seekg( 0 );
    if ( !reader.load( stream ) ) {
        std::cerr << "failed to load object\n";
        return 2;
    }

    // One physical base address per section index. Sections not referenced by
    // an active relocation may simply stay zero; missing entries that are
    // needed are reported as errors.
    std::vector<Elf64_Addr> bases( reader.sections.size(), 0 );
    bases[text->get_index()]   = TEXT_BASE;
    bases[rodata->get_index()] = RODATA_BASE;

    riscv_reloc_plan plan;
    const std::string error = make_riscv_reloc_plan( reader, bases, plan );
    if ( !error.empty() ) {
        std::cerr << "relocation planning failed: " << error << "\n";
        return 3;
    }

    std::cout << "relocated " << plan.sections.size()
              << " code section(s), " << plan.changes.size()
              << " instruction word change(s)\n";
    for ( const riscv_reloc_change& c : plan.changes ) {
        std::cout << "  section " << c.code_section_index << " +0x" << std::hex
                  << c.code_offset << std::dec << " "
                  << kind_name( c.kind ) << " [rela "
                  << c.reloc_section_index << "#" << c.reloc_entry_index
                  << ", high +0x" << std::hex << c.high_offset << std::dec
                  << "] 0x" << std::hex << c.original_word << " -> 0x"
                  << c.patched_word << std::dec << "\n";
    }

    // The original object is untouched: its .text still holds the words that
    // were written before planning.
    const section* loaded_text = reader.sections[text->get_index()];
    if ( loaded_text->get_size() != text->get_size() ) {
        std::cerr << "original object size changed\n";
        return 4;
    }

    std::cout << "riscv_reloc example OK\n";
    return 0;
}
