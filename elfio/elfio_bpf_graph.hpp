/*
MIT License

Copyright (c) Task contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#ifndef ELFIO_BPF_GRAPH_HPP
#define ELFIO_BPF_GRAPH_HPP

#include <elfio/elfio.hpp>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ELFIO {

//! Identity of a symbol as its symbol-table section and symbol ordinal.
struct bpf_symbol_id
{
    unsigned symbol_table = 0; //!< Physical section index of SHT_SYMTAB
    unsigned index        = 0; //!< Symbol ordinal in that table

    std::string to_string() const
    {
        return std::to_string( symbol_table ) + ":" + std::to_string( index );
    }

    friend bool operator==( const bpf_symbol_id& a, const bpf_symbol_id& b )
    {
        return a.symbol_table == b.symbol_table && a.index == b.index;
    }

    friend bool operator!=( const bpf_symbol_id& a, const bpf_symbol_id& b )
    {
        return !( a == b );
    }

    friend bool operator<( const bpf_symbol_id& a, const bpf_symbol_id& b )
    {
        return std::tie( a.symbol_table, a.index ) <
               std::tie( b.symbol_table, b.index );
    }
};

//! Physical location of one eight-byte instruction slot.
struct bpf_instruction_location
{
    unsigned      section_index = 0; //!< Code section index
    std::uint64_t byte_offset   = 0; //!< Offset within the code section
    std::uint64_t slot_index    = 0; //!< Original eight-byte slot/8
    std::uint64_t file_offset   = 0; //!< sh_offset(code section)+byte_offset

    friend bool operator==( const bpf_instruction_location& a,
                            const bpf_instruction_location& b )
    {
        return std::tie( a.section_index, a.byte_offset, a.slot_index,
                         a.file_offset ) ==
               std::tie( b.section_index, b.byte_offset, b.slot_index,
                         b.file_offset );
    }

    friend bool operator!=( const bpf_instruction_location& a,
                            const bpf_instruction_location& b )
    {
        return !( a == b );
    }
};

//! Physical identity of a relocation record and its linked symbol.
struct bpf_relocation_location
{
    unsigned       section_index = 0; //!< SHT_REL section index
    std::uint64_t  entry_index   = 0; //!< Entry ordinal in that section
    std::uint64_t  file_offset   = 0; //!< Physical offset of Elf64_Rel
    bpf_symbol_id  symbol; //!< Symbol identity from this REL's sh_link/r_info

    friend bool operator==( const bpf_relocation_location& a,
                            const bpf_relocation_location& b )
    {
        return std::tie( a.section_index, a.entry_index, a.file_offset,
                         a.symbol ) ==
               std::tie( b.section_index, b.entry_index, b.file_offset,
                         b.symbol );
    }

    friend bool operator!=( const bpf_relocation_location& a,
                            const bpf_relocation_location& b )
    {
        return !( a == b );
    }
};

//! An instruction site and, when present, its unique REL record.
struct bpf_site
{
    bpf_instruction_location instruction;
    bool                     has_relocation = false;
    bpf_relocation_location  relocation{};

    friend bool operator==( const bpf_site& a, const bpf_site& b )
    {
        return a.instruction == b.instruction &&
               a.has_relocation == b.has_relocation &&
               ( !a.has_relocation || a.relocation == b.relocation );
    }

    friend bool operator!=( const bpf_site& a, const bpf_site& b )
    {
        return !( a == b );
    }
};

//! A defined STT_FUNC selected from an executable PROGBITS section.
struct bpf_function
{
    bpf_symbol_id id;
    std::string   name;
    unsigned      section_index = 0;
    std::uint64_t value         = 0;
    std::uint64_t size          = 0;
    std::uint64_t file_offset   = 0;
    std::uint64_t slot_index    = 0;

    friend bool operator==( const bpf_function& a, const bpf_function& b )
    {
        return std::tie( a.id, a.name, a.section_index, a.value, a.size,
                         a.file_offset, a.slot_index ) ==
               std::tie( b.id, b.name, b.section_index, b.value, b.size,
                         b.file_offset, b.slot_index );
    }
};

//! A resolved map definition (a positive-size STT_OBJECT in a requested map section).
struct bpf_map
{
    bpf_symbol_id id;
    std::string   name;
    unsigned      section_index = 0;
    std::uint64_t value         = 0;
    std::uint64_t size          = 0;
    std::uint64_t file_offset   = 0;

    friend bool operator==( const bpf_map& a, const bpf_map& b )
    {
        return std::tie( a.id, a.name, a.section_index, a.value, a.size,
                         a.file_offset ) ==
               std::tie( b.id, b.name, b.section_index, b.value, b.size,
                         b.file_offset );
    }
};

//! A reachable src=1 BPF call. One record exists for each call instruction site.
struct bpf_call
{
    bpf_symbol_id caller;
    bpf_symbol_id callee;
    unsigned      target_section = 0;
    std::uint64_t target_offset  = 0;
    bpf_site      site;

    friend bool operator==( const bpf_call& a, const bpf_call& b )
    {
        return std::tie( a.caller, a.callee, a.target_section,
                         a.target_offset, a.site ) ==
               std::tie( b.caller, b.callee, b.target_section,
                         b.target_offset, b.site );
    }
};

//! A reachable lddw/R_BPF_64_64 site that resolves to a map definition start.
struct bpf_map_reference
{
    bpf_symbol_id     owner;
    bpf_symbol_id     map;
    bpf_site          site;

    friend bool operator==( const bpf_map_reference& a,
                            const bpf_map_reference& b )
    {
        return std::tie( a.owner, a.map, a.site ) ==
               std::tie( b.owner, b.map, b.site );
    }
};

//! A reachable src=0 call without relocation. The immediate is the helper ID.
struct bpf_helper_call
{
    bpf_symbol_id owner;
    std::int32_t  helper_id = 0;
    bpf_site      site;

    friend bool operator==( const bpf_helper_call& a, const bpf_helper_call& b )
    {
        return std::tie( a.owner, a.helper_id, a.site ) ==
               std::tie( b.owner, b.helper_id, b.site );
    }
};

//! One entry-to-map-site provenance. call_path is ordered entry caller to owner.
struct bpf_provenance
{
    bpf_symbol_id          entry;
    bpf_symbol_id          map;
    bpf_site               map_site;
    std::vector<bpf_call>  call_path;

    friend bool operator==( const bpf_provenance& a, const bpf_provenance& b )
    {
        return std::tie( a.entry, a.map, a.map_site, a.call_path ) ==
               std::tie( b.entry, b.map, b.map_site, b.call_path );
    }
};

//! A human-readable unsupported/error diagnostic with optional physical location.
struct bpf_issue
{
    std::string                 code;
    std::string                 message;
    bool                        has_location = false;
    bpf_instruction_location    location{};
};

//! Independently owned analysis result. It is safe after the source elfio dies.
struct bpf_graph
{
    std::vector<bpf_function>       functions;
    std::vector<bpf_map>            maps;
    std::vector<bpf_call>           calls;
    std::vector<bpf_map_reference>  map_references;
    std::vector<bpf_helper_call>    helpers;
    std::vector<bpf_provenance>     provenance;
};

//! Return type for analyze_bpf_graph(). Check ok before using graph.
struct bpf_analysis_result
{
    bool             ok = false;
    std::string      error;
    std::vector<bpf_issue> issues;
    bpf_graph        graph;

    explicit operator bool() const { return ok; }
};

namespace bpf_graph_detail {

constexpr unsigned char OP_LDDW       = 0x18;
constexpr unsigned char OP_CALL       = 0x85;
constexpr unsigned char OP_EXIT       = 0x95;
constexpr unsigned char OP_JA16       = 0x05;
constexpr unsigned char OP_JA32_LONG  = 0x06;
constexpr unsigned      R_BPF_64_64   = 1;
constexpr unsigned      R_BPF_64_32   = 10;

struct symbol_info
{
    bpf_symbol_id id;
    std::string   name;
    Elf64_Addr    value   = 0;
    Elf_Xword     size    = 0;
    unsigned char bind    = 0;
    unsigned char type    = 0;
    unsigned char other   = 0;
    Elf_Half      section = 0;
};

struct relocation_info
{
    bpf_relocation_location identity;
    unsigned                target_section = 0;
    std::uint64_t           offset         = 0;
    unsigned                type           = 0;
};

struct instruction_info
{
    std::uint64_t offset = 0;
    unsigned char opcode = 0;
    unsigned      dst    = 0;
    unsigned      src    = 0;
    std::int16_t  jump_offset = 0;
    std::int32_t  immediate   = 0;
    std::uint32_t second_immediate = 0;
    bool          wide             = false;
};

struct function_info
{
    bpf_function                 info;
    unsigned                     section_index = 0;
    std::vector<instruction_info> instructions;
    std::map<std::uint64_t, std::size_t> offset_to_index;
    std::vector<std::vector<std::size_t>> successors;
    std::vector<bool>            reachable;
};

struct failure
{
    bpf_issue issue;
};

[[noreturn]] inline void fail( const std::string& code,
                               const std::string& message,
                               const bpf_instruction_location* location = nullptr )
{
    failure f;
    f.issue.code    = code;
    f.issue.message = message;
    if ( location != nullptr ) {
        f.issue.has_location = true;
        f.issue.location     = *location;
    }
    throw f;
}

inline std::uint16_t read_le16( const char* p )
{
    const auto* u = reinterpret_cast<const unsigned char*>( p );
    return static_cast<std::uint16_t>( unsigned( u[0] ) |
                                       ( unsigned( u[1] ) << 8 ) );
}

inline std::uint32_t read_le32( const char* p )
{
    const auto* u = reinterpret_cast<const unsigned char*>( p );
    return std::uint32_t( u[0] ) | ( std::uint32_t( u[1] ) << 8 ) |
           ( std::uint32_t( u[2] ) << 16 ) |
           ( std::uint32_t( u[3] ) << 24 );
}

inline bpf_instruction_location
make_location( const section* sec, std::uint64_t offset )
{
    bpf_instruction_location loc;
    loc.section_index = sec->get_index();
    loc.byte_offset   = offset;
    loc.slot_index    = offset / 8;
    loc.file_offset   = static_cast<std::uint64_t>( sec->get_offset() ) + offset;
    return loc;
}

inline bool location_less( const bpf_instruction_location& a,
                           const bpf_instruction_location& b )
{
    return std::tie( a.section_index, a.byte_offset, a.slot_index,
                     a.file_offset ) <
           std::tie( b.section_index, b.byte_offset, b.slot_index,
                     b.file_offset );
}

inline std::vector<std::pair<unsigned, std::uint64_t>>
path_key( const std::vector<bpf_call>& path )
{
    std::vector<std::pair<unsigned, std::uint64_t>> key;
    key.reserve( path.size() );
    for ( const auto& step : path ) {
        key.emplace_back( step.site.instruction.section_index,
                          step.site.instruction.byte_offset );
    }
    return key;
}

class analyzer
{
  public:
    analyzer( const elfio&                 elf_file,
              std::vector<bpf_symbol_id>   entries,
              std::vector<unsigned>        map_sections )
        : elf( elf_file ), entry_ids( std::move( entries ) ),
          map_section_input( std::move( map_sections ) )
    {
    }

    bpf_analysis_result run()
    {
        validate_profile();
        collect_symbol_tables();
        normalize_map_sections();
        collect_maps();
        collect_functions();
        collect_relocations();
        decode_functions();
        validate_code_relocations();
        find_reachable_sites();
        sort_records();
        build_provenance();
        validate_entries();

        bpf_analysis_result result;
        result.ok    = true;
        result.graph = std::move( graph );
        return result;
    }

  private:
    const elfio&                          elf;
    std::vector<bpf_symbol_id>           entry_ids;
    std::vector<unsigned>                map_section_input;
    std::set<unsigned>                   map_sections;
    std::map<unsigned, std::vector<symbol_info>> symbols;
    std::map<bpf_symbol_id, std::size_t> function_by_id;
    std::map<std::pair<unsigned, std::uint64_t>, std::size_t> function_by_start;
    std::vector<function_info>           functions;
    std::map<bpf_symbol_id, std::size_t> map_by_id;
    std::map<std::pair<unsigned, std::uint64_t>, std::size_t> map_by_start;
    std::vector<relocation_info>         relocations;
    std::map<std::pair<unsigned, std::uint64_t>, std::vector<std::size_t>>
        relocations_by_site;
    bpf_graph                            graph;

    const section* get_section( unsigned index ) const
    {
        if ( index >= elf.sections.size() ) {
            return nullptr;
        }
        return elf.sections[index];
    }

    const symbol_info* find_symbol( const bpf_symbol_id& id ) const
    {
        auto table_it = symbols.find( id.symbol_table );
        if ( table_it == symbols.end() ||
             id.index >= table_it->second.size() ) {
            return nullptr;
        }
        return &table_it->second[id.index];
    }

    void validate_profile()
    {
        if ( elf.get_class() != ELFCLASS64 ) {
            fail( "unsupported_elf_class", "only ELF64 objects are supported" );
        }
        if ( elf.get_encoding() != ELFDATA2LSB ) {
            fail( "unsupported_byte_order", "only little-endian objects are supported" );
        }
        if ( elf.get_type() != ET_REL ) {
            fail( "unsupported_elf_type", "only ET_REL objects are supported" );
        }
        if ( elf.get_machine() != EM_BPF ) {
            fail( "unsupported_machine", "only EM_BPF objects are supported" );
        }
        if ( elf.segments.size() != 0 ) {
            fail( "unsupported_program_segment",
                  "BPF graph analysis requires ET_REL objects without program segments" );
        }

        for ( unsigned i = 0; i < elf.sections.size(); ++i ) {
            const section* sec = elf.sections[i];
            if ( sec == nullptr ) {
                bpf_instruction_location loc{ i, 0, 0, 0 };
                fail( "invalid_section", "section table contains a null entry", &loc );
            }
            if ( ( sec->get_flags() & SHF_COMPRESSED ) != 0 ) {
                auto loc = make_location( sec, 0 );
                fail( "unsupported_compressed_section",
                       "compressed ELF sections are outside this BPF profile", &loc );
            }
            const std::string name = sec->get_name();
            if ( name.rfind( ".zdebug", 0 ) == 0 ||
                 name.rfind( ".z", 0 ) == 0 ) {
                auto loc = make_location( sec, 0 );
                fail( "unsupported_compressed_section",
                       "GNU-style compressed section names are outside this BPF profile",
                       &loc );
            }
            if ( sec->get_type() == SHT_RELA ) {
                auto loc = make_location( sec, 0 );
                fail( "unsupported_rela",
                       "this read-only BPF profile supports SHT_REL records, not SHT_RELA",
                       &loc );
            }
        }
    }

    void collect_symbol_tables()
    {
        for ( unsigned section_index = 0;
              section_index < elf.sections.size(); ++section_index ) {
            const section* sec = elf.sections[section_index];
            if ( sec == nullptr || sec->get_type() != SHT_SYMTAB ) {
                continue;
            }

            const_symbol_section_accessor accessor( elf, sec );
            const Elf_Xword count = accessor.get_symbols_num();
            auto& table           = symbols[section_index];
            table.reserve( static_cast<std::size_t>( count ) );

            for ( Elf_Xword ordinal = 0; ordinal < count; ++ordinal ) {
                symbol_info sym;
                sym.id.symbol_table = section_index;
                sym.id.index        = static_cast<unsigned>( ordinal );
                if ( !accessor.get_symbol( ordinal, sym.name, sym.value,
                                           sym.size, sym.bind, sym.type,
                                           sym.section, sym.other ) ) {
                    bpf_instruction_location loc = make_location( sec, 0 );
                    fail( "invalid_symbol", "could not read symbol table entry", &loc );
                }
                table.push_back( std::move( sym ) );
            }
        }

        if ( symbols.empty() ) {
            fail( "missing_symtab", "object does not contain an SHT_SYMTAB section" );
        }
    }

    void normalize_map_sections()
    {
        for ( unsigned index : map_section_input ) {
            const section* sec = get_section( index );
            if ( index == 0 || sec == nullptr ) {
                fail( "invalid_map_section",
                       "requested map section index does not identify a section" );
            }
            map_sections.insert( index );
        }
    }

    void collect_maps()
    {
        for ( const auto& [table_index, table] : symbols ) {
            for ( const symbol_info& sym : table ) {
                if ( sym.type != STT_OBJECT || sym.size == 0 ||
                     !map_sections.count( sym.section ) ) {
                    continue;
                }

                const section* sec = get_section( sym.section );
                if ( sec == nullptr ) {
                    fail( "invalid_map_section_symbol",
                           "STT_OBJECT symbol refers to a missing map section" );
                }
                const std::uint64_t section_size = sec->get_size();
                if ( sym.value > section_size ||
                     sym.size > section_size - sym.value ) {
                    auto loc = make_location( sec, std::min( sym.value, section_size ) );
                    fail( "unsupported_map_bounds",
                           "map symbol interval leaves its requested section", &loc );
                }

                bpf_map map;
                map.id          = sym.id;
                map.name        = sym.name;
                map.section_index = sym.section;
                map.value       = sym.value;
                map.size        = sym.size;
                map.file_offset = static_cast<std::uint64_t>( sec->get_offset() ) +
                                  sym.value;
                graph.maps.push_back( std::move( map ) );
            }
        }

        std::sort( graph.maps.begin(), graph.maps.end(),
                   []( const bpf_map& a, const bpf_map& b ) {
                       return std::tie( a.section_index, a.value, a.id ) <
                              std::tie( b.section_index, b.value, b.id );
                   } );

        for ( std::size_t i = 0; i < graph.maps.size(); ++i ) {
            const bpf_map& map = graph.maps[i];
            if ( !map_by_id.emplace( map.id, i ).second ) {
                fail( "duplicate_map_identity", "duplicate map symbol identity" );
            }
            auto start_key = std::make_pair( map.section_index, map.value );
            if ( !map_by_start.emplace( start_key, i ).second ) {
                auto loc = make_location( get_section( map.section_index ), map.value );
                fail( "unsupported_overlapping_maps",
                       "multiple map definitions start at the same physical location",
                       &loc );
            }
            if ( i > 0 ) {
                const bpf_map& prev = graph.maps[i - 1];
                if ( prev.section_index == map.section_index &&
                     prev.value + prev.size > map.value ) {
                    auto loc = make_location( get_section( map.section_index ), map.value );
                    fail( "unsupported_overlapping_maps",
                           "map definition intervals overlap", &loc );
                }
            }
        }
    }

    bool is_executable_code_section( const section* sec ) const
    {
        return sec != nullptr && sec->get_type() == SHT_PROGBITS &&
               ( sec->get_flags() & SHF_EXECINSTR ) != 0;
    }

    void collect_functions()
    {
        std::vector<bpf_function> selected;

        for ( const auto& [table_index, table] : symbols ) {
            for ( const symbol_info& sym : table ) {
                if ( sym.type != STT_FUNC ) {
                    continue;
                }
                const section* sec = get_section( sym.section );
                if ( !is_executable_code_section( sec ) ) {
                    continue;
                }

                auto loc = make_location( sec, std::min<std::uint64_t>(
                                                  sym.value, sec->get_size() ) );
                if ( sym.size == 0 ) {
                    fail( "unsupported_zero_length_function",
                           "zero-length STT_FUNC identities are not guessed as boundaries",
                           &loc );
                }

                const std::uint64_t section_size = sec->get_size();
                if ( sym.value > section_size ||
                     sym.size > section_size - sym.value ) {
                    fail( "unsupported_function_bounds",
                           "STT_FUNC interval leaves its executable section", &loc );
                }
                if ( sym.value % 8 != 0 || sym.size % 8 != 0 ) {
                    fail( "unsupported_instruction_alignment",
                           "function start or size is not aligned to eight-byte slots",
                           &loc );
                }
                if ( sec->get_data() == nullptr ) {
                    fail( "missing_section_data",
                           "executable section data is not available for reading", &loc );
                }

                bpf_function fn;
                fn.id           = sym.id;
                fn.name         = sym.name;
                fn.section_index = sym.section;
                fn.value        = sym.value;
                fn.size         = sym.size;
                fn.slot_index   = sym.value / 8;
                fn.file_offset  = static_cast<std::uint64_t>( sec->get_offset() ) +
                                 sym.value;
                selected.push_back( std::move( fn ) );
            }
        }

        std::sort( selected.begin(), selected.end(),
                   []( const bpf_function& a, const bpf_function& b ) {
                       return std::tie( a.section_index, a.value, a.id ) <
                              std::tie( b.section_index, b.value, b.id );
                   } );

        for ( std::size_t i = 0; i < selected.size(); ++i ) {
            const bpf_function& fn = selected[i];
            const section* sec     = get_section( fn.section_index );
            auto start_loc         = make_location( sec, fn.value );

            if ( i > 0 ) {
                const bpf_function& prev = selected[i - 1];
                if ( prev.section_index == fn.section_index ) {
                    if ( prev.value == fn.value ) {
                        fail( "unsupported_duplicate_function_start",
                               "multiple function identities share one physical start; "
                               "boundaries are not guessed",
                               &start_loc );
                    }
                    if ( prev.value + prev.size > fn.value ) {
                        fail( "unsupported_overlapping_functions",
                               "STT_FUNC intervals overlap", &start_loc );
                    }
                }
            }

            function_info info;
            info.info          = fn;
            info.section_index = fn.section_index;
            functions.push_back( std::move( info ) );
        }

        // Keep identity/start maps after functions vector is complete.
        for ( std::size_t i = 0; i < functions.size(); ++i ) {
            const bpf_function& fn = functions[i].info;
            function_by_id.emplace( fn.id, i );
            function_by_start.emplace(
                std::make_pair( fn.section_index, fn.value ), i );
            graph.functions.push_back( fn );
        }
    }

    void collect_relocations()
    {
        for ( unsigned section_index = 0;
              section_index < elf.sections.size(); ++section_index ) {
            const section* sec = elf.sections[section_index];
            if ( sec == nullptr || sec->get_type() != SHT_REL ) {
                continue;
            }

            const unsigned target_section = sec->get_info();
            const unsigned symbol_table   = sec->get_link();
            const section* target         = get_section( target_section );
            if ( target == nullptr ) {
                auto loc = make_location( sec, 0 );
                fail( "invalid_relocation_target",
                       "SHT_REL sh_info does not identify a section", &loc );
            }
            if ( symbols.find( symbol_table ) == symbols.end() ) {
                auto loc = make_location( sec, 0 );
                fail( "invalid_relocation_symbol_table",
                       "SHT_REL sh_link does not identify a supported SHT_SYMTAB", &loc );
            }

            const_relocation_section_accessor accessor( elf, sec );
            const Elf_Xword count = accessor.get_entries_num();
            const auto entry_size = sec->get_entry_size();
            if ( count != 0 && entry_size < sizeof( Elf64_Rel ) ) {
                auto loc = make_location( sec, 0 );
                fail( "invalid_relocation_entry_size",
                       "ELF64 SHT_REL entry size is too small", &loc );
            }

            for ( Elf_Xword entry_index = 0; entry_index < count; ++entry_index ) {
                Elf64_Addr offset = 0;
                Elf_Word   symbol = 0;
                unsigned   type   = 0;
                Elf_Sxword addend = 0;
                if ( !accessor.get_entry( entry_index, offset, symbol, type, addend ) ) {
                    auto loc = make_location( sec, 0 );
                    fail( "invalid_relocation_entry", "could not read REL entry", &loc );
                }
                if ( offset > target->get_size() ) {
                    auto loc = make_location( target,
                                              std::min<std::uint64_t>(
                                                  offset, target->get_size() ) );
                    fail( "invalid_relocation_offset",
                           "REL r_offset is outside its sh_info target section", &loc );
                }

                relocation_info rel;
                rel.identity.section_index = section_index;
                rel.identity.entry_index   = entry_index;
                rel.identity.file_offset =
                    static_cast<std::uint64_t>( sec->get_offset() ) +
                    entry_index * static_cast<std::uint64_t>( entry_size );
                rel.identity.symbol.symbol_table = symbol_table;
                rel.identity.symbol.index        = symbol;
                rel.target_section               = target_section;
                rel.offset                       = offset;
                rel.type                         = type;

                if ( find_symbol( rel.identity.symbol ) == nullptr ) {
                    auto loc = make_location( target, offset );
                    fail( "invalid_relocation_symbol",
                           "REL r_info refers to a missing symbol identity", &loc );
                }

                const std::size_t rel_index = relocations.size();
                relocations.push_back( rel );
                relocations_by_site[std::make_pair( target_section, offset )]
                    .push_back( rel_index );
            }
        }
    }

    void decode_functions()
    {
        for ( function_info& fn : functions ) {
            const section* sec = get_section( fn.section_index );
            const char* data   = sec->get_data();
            const std::uint64_t start = fn.info.value;
            const std::uint64_t end   = start + fn.info.size;

            for ( std::uint64_t offset = start; offset < end; ) {
                if ( offset + 8 > end ) {
                    auto loc = make_location( sec, offset );
                    fail( "unsupported_instruction_bounds",
                           "instruction slot extends beyond STT_FUNC size", &loc );
                }

                const char* p = data + offset;
                instruction_info insn;
                insn.offset       = offset;
                insn.opcode       = static_cast<unsigned char>( p[0] );
                const unsigned regs = static_cast<unsigned char>( p[1] );
                insn.dst          = regs & 0x0fU;
                insn.src          = ( regs >> 4 ) & 0x0fU;
                insn.jump_offset  = static_cast<std::int16_t>( read_le16( p + 2 ) );
                insn.immediate    = static_cast<std::int32_t>( read_le32( p + 4 ) );

                if ( insn.opcode == OP_LDDW ) {
                    insn.wide = true;
                    if ( offset + 16 > end ) {
                        auto loc = make_location( sec, offset );
                        fail( "unsupported_instruction_bounds",
                               "0x18 lddw lacks its second eight-byte slot", &loc );
                    }
                    insn.second_immediate = read_le32( p + 12 );
                    fn.instructions.push_back( insn );
                    fn.offset_to_index[offset] = fn.instructions.size() - 1;
                    offset += 16;
                }
                else {
                    fn.instructions.push_back( insn );
                    fn.offset_to_index[offset] = fn.instructions.size() - 1;
                    offset += 8;
                }
            }

            const std::size_t n = fn.instructions.size();
            fn.successors.assign( n, {} );
            fn.reachable.assign( n, false );

            auto target_index = [&]( std::int64_t target ) -> std::size_t {
                if ( target < 0 ||
                     static_cast<std::uint64_t>( target ) < start ||
                     static_cast<std::uint64_t>( target ) >= end ) {
                    auto loc = make_location( sec, 0 );
                    fail( "unsupported_control_flow",
                           "jump target leaves its STT_FUNC interval", &loc );
                }
                auto it = fn.offset_to_index.find(
                    static_cast<std::uint64_t>( target ) );
                if ( it == fn.offset_to_index.end() ) {
                    auto loc = make_location( sec,
                                              static_cast<std::uint64_t>( target ) );
                    fail( "unsupported_control_flow",
                           "jump target is not an instruction start "
                           "(it may be an lddw second slot)", &loc );
                }
                return it->second;
            };

            for ( std::size_t i = 0; i < n; ++i ) {
                const instruction_info& insn = fn.instructions[i];
                const std::int64_t here     = insn.offset;
                const std::int64_t next     = here + ( insn.wide ? 16 : 8 );
                const unsigned char op      = insn.opcode;

                if ( op == OP_EXIT ) {
                    continue;
                }

                if ( op == OP_JA32_LONG ) {
                    auto loc = make_location( sec, insn.offset );
                    fail( "unsupported_jmp32_long_ja",
                           "JMP32 long JA is outside the current BPF graph profile",
                           &loc );
                }

                if ( op == OP_JA16 ) {
                    const std::int64_t target =
                        here + 8 + 8 * static_cast<std::int64_t>( insn.jump_offset );
                    fn.successors[i].push_back( target_index( target ) );
                    continue;
                }

                if ( op == OP_CALL ) {
                    // Calls return to the next slot.
                    fn.successors[i].push_back( target_index( next ) );
                    continue;
                }

                const unsigned jump_operation = op & 0xf0U;
                const bool known_conditional_operation =
                    ( jump_operation >= 0x10U && jump_operation <= 0x70U ) ||
                    ( jump_operation >= 0xa0U && jump_operation <= 0xd0U );
                const bool is_jmp_cond =
                    ( op & 0x07U ) == 0x05U && known_conditional_operation;
                const bool is_jmp32_cond =
                    ( op & 0x07U ) == 0x06U && known_conditional_operation;
                if ( is_jmp_cond || is_jmp32_cond ) {
                    // Both edges are statically possible; condition truth is not inferred.
                    fn.successors[i].push_back( target_index( next ) );
                    const std::int64_t target =
                        here + 8 + 8 * static_cast<std::int64_t>( insn.jump_offset );
                    fn.successors[i].push_back( target_index( target ) );
                    continue;
                }

                if ( ( op & 0x07U ) == 0x05U || ( op & 0x07U ) == 0x06U ) {
                    auto loc = make_location( sec, insn.offset );
                    fail( "unsupported_jump_instruction",
                           "unrecognized JMP/JMP32 instruction is outside this profile",
                           &loc );
                }

                // Ordinary instructions, including lddw, advance sequentially.
                fn.successors[i].push_back( target_index( next ) );
            }

            // Iterative CFG reachability. No host recursion is involved.
            if ( n != 0 ) {
                std::deque<std::size_t> work;
                fn.reachable[0] = true;
                work.push_back( 0 );
                while ( !work.empty() ) {
                    const std::size_t current = work.front();
                    work.pop_front();
                    for ( std::size_t succ : fn.successors[current] ) {
                        if ( !fn.reachable[succ] ) {
                            fn.reachable[succ] = true;
                            work.push_back( succ );
                        }
                    }
                }
            }
        }
    }

    const relocation_info& unique_relocation(
        unsigned section, std::uint64_t offset )
    {
        auto it = relocations_by_site.find( std::make_pair( section, offset ) );
        if ( it == relocations_by_site.end() || it->second.empty() ) {
            auto loc = make_location( get_section( section ), offset );
            fail( "missing_relocation", "expected REL record at instruction site", &loc );
        }
        if ( it->second.size() != 1 ) {
            auto loc = make_location( get_section( section ), offset );
            fail( "unsupported_multiple_relocations",
                   "multiple REL records describe one instruction site", &loc );
        }
        return relocations[it->second.front()];
    }

    void validate_code_relocations()
    {
        for ( const relocation_info& rel : relocations ) {
            const section* target = get_section( rel.target_section );
            if ( !is_executable_code_section( target ) ) {
                continue;
            }

            const function_info* containing = nullptr;
            for ( const function_info& fn : functions ) {
                if ( fn.section_index == rel.target_section &&
                     fn.info.value <= rel.offset &&
                     rel.offset < fn.info.value + fn.info.size ) {
                    containing = &fn;
                    break;
                }
            }
            if ( containing == nullptr ) {
                auto loc = make_location( target, rel.offset );
                fail( "unsupported_relocation_outside_function",
                       "code relocation is outside every defined function interval",
                       &loc );
            }

            auto insn_it = containing->offset_to_index.find( rel.offset );
            if ( insn_it == containing->offset_to_index.end() ) {
                auto loc = make_location( target, rel.offset );
                fail( "unsupported_relocation_site",
                       "code relocation points at a non-instruction slot", &loc );
            }
            const instruction_info& insn =
                containing->instructions[insn_it->second];

            const auto& site_rels =
                relocations_by_site.at( std::make_pair( rel.target_section,
                                                        rel.offset ) );
            if ( site_rels.size() != 1 ) {
                auto loc = make_location( target, rel.offset );
                fail( "unsupported_multiple_relocations",
                       "multiple REL records describe one instruction site", &loc );
            }

            if ( rel.type == R_BPF_64_32 ) {
                if ( insn.opcode != OP_CALL || insn.src != 1 ) {
                    auto loc = make_location( target, rel.offset );
                    fail( "unsupported_relocation_site",
                           "R_BPF_64_32 must annotate a src=1 CALL instruction", &loc );
                }
            }
            else if ( rel.type == R_BPF_64_64 ) {
                if ( insn.opcode != OP_LDDW ) {
                    auto loc = make_location( target, rel.offset );
                    fail( "unsupported_relocation_site",
                           "R_BPF_64_64 must annotate the first slot of an lddw", &loc );
                }
                if ( insn.second_immediate != 0 ) {
                    auto loc = make_location( target, rel.offset );
                    fail( "unsupported_nonzero_lddw_high_word",
                           "relocated lddw has a nonzero second-slot immediate", &loc );
                }
            }
            else {
                auto loc = make_location( target, rel.offset );
                fail( "unsupported_bpf_relocation_type",
                       "only R_BPF_64_64(1) and R_BPF_64_32(10) are supported", &loc );
            }
        }
    }

    bpf_site make_site( unsigned section, std::uint64_t offset )
    {
        bpf_site site;
        site.instruction = make_location( get_section( section ), offset );
        auto it = relocations_by_site.find( std::make_pair( section, offset ) );
        if ( it != relocations_by_site.end() && !it->second.empty() ) {
            if ( it->second.size() != 1 ) {
                fail( "unsupported_multiple_relocations",
                       "multiple REL records describe one instruction site",
                       &site.instruction );
            }
            site.has_relocation = true;
            site.relocation     = relocations[it->second.front()].identity;
        }
        return site;
    }

    void find_reachable_sites()
    {
        for ( const function_info& fn_const : functions ) {
            // The CFG/reachability has already been computed; this loop only reads it.
            const function_info& fn = fn_const;
            for ( std::size_t ii = 0; ii < fn.instructions.size(); ++ii ) {
                if ( !fn.reachable[ii] ) {
                    continue;
                }
                const instruction_info& insn = fn.instructions[ii];
                const unsigned section       = fn.section_index;
                const std::uint64_t offset   = insn.offset;

                if ( insn.opcode == OP_CALL ) {
                    bpf_site site = make_site( section, offset );

                    if ( insn.src == 0 ) {
                        if ( site.has_relocation ) {
                            fail( "unsupported_helper_relocation",
                                   "src=0 CALL is a helper only when it has no REL record",
                                   &site.instruction );
                        }
                        bpf_helper_call helper;
                        helper.owner     = fn.info.id;
                        helper.helper_id = insn.immediate;
                        helper.site      = site;
                        graph.helpers.push_back( std::move( helper ) );
                        continue;
                    }

                    if ( insn.src == 2 ) {
                        fail( "unsupported_btf_call",
                               "src=2 BTF calls are outside this BPF graph profile",
                               &site.instruction );
                    }
                    if ( insn.src != 1 ) {
                        fail( "unsupported_call_source",
                               "only src=0 helper calls and src=1 subprogram calls are supported",
                               &site.instruction );
                    }

                    unsigned target_section = section;
                    std::int64_t target_offset =
                        static_cast<std::int64_t>( offset ) + 8 +
                        8 * static_cast<std::int64_t>( insn.immediate );

                    if ( site.has_relocation ) {
                        const relocation_info& rel = unique_relocation( section, offset );
                        if ( rel.type != R_BPF_64_32 ) {
                            fail( "unsupported_relocation_type",
                                   "subprogram CALL REL must be R_BPF_64_32(10)",
                                   &site.instruction );
                        }
                        const symbol_info* sym = find_symbol( rel.identity.symbol );
                        if ( sym == nullptr ) {
                            fail( "invalid_relocation_symbol",
                                   "CALL REL symbol identity is unavailable",
                                   &site.instruction );
                        }
                        if ( sym->type != STT_FUNC && sym->type != STT_SECTION ) {
                            fail( "unsupported_call_symbol",
                                   "CALL REL symbol must be STT_FUNC or STT_SECTION",
                                   &site.instruction );
                        }
                        target_section = sym->section;
                        target_offset =
                            static_cast<std::int64_t>( sym->value ) +
                            8 * ( static_cast<std::int64_t>( insn.immediate ) + 1 );
                    }

                    if ( target_offset < 0 ) {
                        fail( "unsupported_call_target",
                               "resolved CALL target has a negative offset",
                               &site.instruction );
                    }
                    auto target_key = std::make_pair(
                        target_section,
                        static_cast<std::uint64_t>( target_offset ) );
                    auto target_it = function_by_start.find( target_key );
                    if ( target_it == function_by_start.end() ) {
                        fail( "unsupported_call_target",
                               "CALL does not land at a defined function start",
                               &site.instruction );
                    }

                    bpf_call call;
                    call.caller         = fn.info.id;
                    call.callee         = functions[target_it->second].info.id;
                    call.target_section = target_section;
                    call.target_offset  = static_cast<std::uint64_t>( target_offset );
                    call.site           = site;
                    graph.calls.push_back( std::move( call ) );
                }
                else if ( insn.opcode == OP_LDDW ) {
                    auto rel_it = relocations_by_site.find(
                        std::make_pair( section, offset ) );
                    if ( rel_it == relocations_by_site.end() ||
                         rel_it->second.empty() ) {
                        // An unrelocated wide immediate is a constant, never a map ref.
                        continue;
                    }

                    bpf_site site = make_site( section, offset );
                    const relocation_info& rel =
                        unique_relocation( section, offset );
                    if ( rel.type != R_BPF_64_64 ) {
                        fail( "unsupported_relocation_type",
                               "lddw REL must be R_BPF_64_64(1)", &site.instruction );
                    }
                    const symbol_info* sym = find_symbol( rel.identity.symbol );
                    if ( sym == nullptr ) {
                        fail( "invalid_relocation_symbol",
                               "map REL symbol identity is unavailable",
                               &site.instruction );
                    }

                    const std::uint64_t target_offset =
                        sym->value +
                        static_cast<std::uint64_t>(
                            static_cast<std::uint32_t>( insn.immediate ) );

                    // Data references outside caller-provided map sections are legal
                    // but intentionally absent from the map graph.
                    if ( !map_sections.count( sym->section ) ) {
                        continue;
                    }
                    if ( sym->type != STT_OBJECT && sym->type != STT_SECTION ) {
                        fail( "unsupported_map_relocation_symbol",
                               "map REL symbol must be STT_OBJECT or STT_SECTION",
                               &site.instruction );
                    }

                    auto map_it = map_by_start.find(
                        std::make_pair( sym->section, target_offset ) );
                    if ( map_it == map_by_start.end() ) {
                        fail( "unsupported_map_reference",
                               "map relocation does not land at a map definition start",
                               &site.instruction );
                    }

                    bpf_map_reference ref;
                    ref.owner = fn.info.id;
                    ref.map   = graph.maps[map_it->second].id;
                    ref.site  = site;
                    graph.map_references.push_back( std::move( ref ) );
                }
            }
        }
    }

    void sort_records()
    {
        std::sort( graph.functions.begin(), graph.functions.end(),
                   []( const bpf_function& a, const bpf_function& b ) {
                       return std::tie( a.id, a.section_index, a.value ) <
                              std::tie( b.id, b.section_index, b.value );
                   } );
        std::sort( graph.calls.begin(), graph.calls.end(),
                   []( const bpf_call& a, const bpf_call& b ) {
                       if ( a.caller != b.caller ) {
                           return a.caller < b.caller;
                       }
                       if ( location_less( a.site.instruction,
                                           b.site.instruction ) ) {
                           return true;
                       }
                       if ( location_less( b.site.instruction,
                                           a.site.instruction ) ) {
                           return false;
                       }
                       return std::tie( a.callee, a.target_section,
                                        a.target_offset ) <
                              std::tie( b.callee, b.target_section,
                                        b.target_offset );
                   } );
        std::sort( graph.map_references.begin(), graph.map_references.end(),
                   []( const bpf_map_reference& a, const bpf_map_reference& b ) {
                       if ( a.owner != b.owner ) {
                           return a.owner < b.owner;
                       }
                       if ( location_less( a.site.instruction,
                                           b.site.instruction ) ) {
                           return true;
                       }
                       if ( location_less( b.site.instruction,
                                           a.site.instruction ) ) {
                           return false;
                       }
                       return a.map < b.map;
                   } );
        std::sort( graph.helpers.begin(), graph.helpers.end(),
                   []( const bpf_helper_call& a, const bpf_helper_call& b ) {
                       if ( a.owner != b.owner ) {
                           return a.owner < b.owner;
                       }
                       if ( location_less( a.site.instruction,
                                           b.site.instruction ) ) {
                           return true;
                       }
                       if ( location_less( b.site.instruction,
                                           a.site.instruction ) ) {
                           return false;
                       }
                       return a.helper_id < b.helper_id;
                   } );
    }

    void validate_entries()
    {
        std::set<bpf_symbol_id> unique;
        for ( const bpf_symbol_id& entry : entry_ids ) {
            if ( function_by_id.find( entry ) == function_by_id.end() ) {
                fail( "invalid_entry",
                       "entry identity is not a positive-size defined STT_FUNC" );
            }
            unique.insert( entry );
        }
        entry_ids.assign( unique.begin(), unique.end() );
    }

    void build_provenance()
    {
        validate_entries();

        std::map<bpf_symbol_id, std::vector<bpf_call>> adjacency;
        for ( const bpf_call& call : graph.calls ) {
            adjacency[call.caller].push_back( call );
        }
        for ( auto& [caller, calls] : adjacency ) {
            std::sort( calls.begin(), calls.end(),
                       []( const bpf_call& a, const bpf_call& b ) {
                           return location_less( a.site.instruction,
                                                  b.site.instruction );
                       } );
        }

        for ( const bpf_symbol_id& entry : entry_ids ) {
            struct best_path
            {
                int                        depth = -1;
                std::vector<bpf_call>      calls;
            };

            std::map<bpf_symbol_id, best_path> best;
            best[entry] = best_path{ 0, {} };
            std::vector<bpf_symbol_id> layer{ entry };
            int depth = 0;

            // Layered BFS gives minimum edge count; candidate comparison gives the
            // required lexicographic tie-break. Back edges terminate through 'best'.
            while ( !layer.empty() ) {
                std::map<bpf_symbol_id, std::vector<bpf_call>> candidates;

                for ( const bpf_symbol_id& caller : layer ) {
                    auto adj_it = adjacency.find( caller );
                    if ( adj_it == adjacency.end() ) {
                        continue;
                    }
                    const std::vector<bpf_call>& prefix = best.at( caller ).calls;
                    for ( const bpf_call& edge : adj_it->second ) {
                        if ( best.find( edge.callee ) != best.end() ) {
                            continue;
                        }
                        std::vector<bpf_call> candidate = prefix;
                        candidate.push_back( edge );

                        auto cand_it = candidates.find( edge.callee );
                        if ( cand_it == candidates.end() ||
                             path_key( candidate ) < path_key( cand_it->second ) ) {
                            candidates[edge.callee] = std::move( candidate );
                        }
                    }
                }

                std::vector<bpf_symbol_id> next_layer;
                for ( auto& [callee, path] : candidates ) {
                    if ( best.find( callee ) != best.end() ) {
                        continue;
                    }
                    best[callee] = best_path{ depth + 1, std::move( path ) };
                    next_layer.push_back( callee );
                }
                layer = std::move( next_layer );
                ++depth;
            }

            for ( const bpf_map_reference& ref : graph.map_references ) {
                auto path_it = best.find( ref.owner );
                if ( path_it == best.end() ) {
                    continue;
                }
                bpf_provenance prov;
                prov.entry     = entry;
                prov.map       = ref.map;
                prov.map_site  = ref.site;
                prov.call_path = path_it->second.calls;
                graph.provenance.push_back( std::move( prov ) );
            }
        }

        std::sort( graph.provenance.begin(), graph.provenance.end(),
                   []( const bpf_provenance& a, const bpf_provenance& b ) {
                       if ( a.entry != b.entry ) {
                           return a.entry < b.entry;
                       }
                       return location_less( a.map_site.instruction,
                                             b.map_site.instruction );
                   } );
    }
};

} // namespace bpf_graph_detail

//! Read static BPF call/map provenance without loading, executing, or relocating.
inline bpf_analysis_result
analyze_bpf_graph( const elfio&                                elf_file,
                   const std::vector<bpf_symbol_id>&          entry_symbols,
                   const std::vector<unsigned>&               map_section_indices )
{
    try {
        bpf_graph_detail::analyzer analyzer(
            elf_file, entry_symbols, map_section_indices );
        return analyzer.run();
    }
    catch ( const bpf_graph_detail::failure& f ) {
        bpf_analysis_result result;
        result.ok    = false;
        result.error = f.issue.code + ": " + f.issue.message;
        result.issues.push_back( f.issue );
        return result;
    }
    catch ( const std::exception& e ) {
        bpf_analysis_result result;
        result.ok    = false;
        result.error = std::string( "exception: " ) + e.what();
        return result;
    }
    catch ( ... ) {
        bpf_analysis_result result;
        result.ok    = false;
        result.error = "unknown analysis failure";
        return result;
    }
}

} // namespace ELFIO

#endif // ELFIO_BPF_GRAPH_HPP
