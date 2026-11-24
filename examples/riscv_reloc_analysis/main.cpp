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

// Report RISC-V PC-relative relocations in an unlinked object.
//
// Usage:
//   riscv_reloc_analysis <elf-file> <section-index>=<base> [...]
//
// Every section that a relocation or symbol needs must be given a base. The
// report is written to stdout as tab-separated rows.

#include <cstdlib>
#include <iostream>
#include <string>

#include <elfio/elfio.hpp>

#include "report.hpp"

using namespace ELFIO;

namespace {

bool parse_base( elfio&                 reader,
                 const std::string&     argument,
                 riscv_section_base_table& bases )
{
    const auto separator = argument.find( '=' );
    if ( separator == std::string::npos ) {
        return false;
    }
    const std::string index_text = argument.substr( 0, separator );
    const std::string base_text  = argument.substr( separator + 1 );
    char*             end        = nullptr;
    const unsigned long index =
        std::strtoul( index_text.c_str(), &end, 0 );
    if ( end == index_text.c_str() || *end != '\0' ) {
        return false;
    }
    const unsigned long long base =
        std::strtoull( base_text.c_str(), &end, 0 );
    if ( end == base_text.c_str() || *end != '\0' ) {
        return false;
    }
    if ( reader.sections[static_cast<unsigned int>( index )] == nullptr ) {
        std::cerr << "section index " << index << " does not exist\n";
        return false;
    }
    bases[static_cast<Elf_Half>( index )] = base;
    return true;
}

} // namespace

int main( int argc, char** argv )
{
    if ( argc < 3 ) {
        std::cerr << "Usage: " << argv[0]
                  << " <elf-file> <section-index>=<base> [...]\n"
                  << "Example: " << argv[0] << " firmware.o 2=0x1000\n";
        return 1;
    }

    elfio reader;
    if ( !reader.load( argv[1] ) ) {
        std::cerr << "cannot load " << argv[1] << "\n";
        return 2;
    }

    riscv_section_base_table bases;
    for ( int i = 2; i < argc; ++i ) {
        if ( !parse_base( reader, argv[i], bases ) ) {
            std::cerr << "bad section base: " << argv[i] << "\n";
            return 3;
        }
    }

    const auto report =
        ELFIO::riscv_example::build_riscv_reference_report( reader, bases );
    if ( !report.ok ) {
        std::cerr << "analysis failed: " << report.error << "\n";
        return 4;
    }

    ELFIO::riscv_example::write_riscv_reference_report( report, std::cout );
    return 0;
}
