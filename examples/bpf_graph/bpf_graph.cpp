// Read static BPF call/map provenance from an offline ET_REL object.
// This example never loads or executes BPF and never applies relocations.
#include <elfio/elfio.hpp>
#include <elfio/elfio_bpf_graph.hpp>

#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace ELFIO;

static std::string tsv_text( std::string value )
{
    for ( char& ch : value ) {
        if ( ch == '\t' || ch == '\r' || ch == '\n' ) {
            ch = ' ';
        }
    }
    return value;
}

static std::string dot_text( const std::string& value )
{
    std::string out;
    for ( char ch : value ) {
        switch ( ch ) {
        case '\\': out += "\\\\"; break;
        case '"':  out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': break;
        default:   out.push_back( ch ); break;
        }
    }
    return out;
}

static std::string id_text( const bpf_symbol_id& id )
{
    return id.to_string();
}

static void write_location_tsv( std::ostream& out, const bpf_site& site )
{
    const auto& loc = site.instruction;
    out << '\t' << loc.section_index
        << '\t' << loc.byte_offset
        << '\t' << loc.slot_index
        << '\t' << loc.file_offset;
    if ( site.has_relocation ) {
        const auto& rel = site.relocation;
        out << '\t' << rel.section_index
            << '\t' << rel.entry_index
            << '\t' << rel.file_offset
            << '\t' << id_text( rel.symbol );
    }
    else {
        out << "\t-\t-\t-\t-";
    }
}

static std::string path_text( const std::vector<bpf_call>& path )
{
    if ( path.empty() ) {
        return "-";
    }
    std::string out;
    for ( std::size_t i = 0; i < path.size(); ++i ) {
        if ( i != 0 ) {
            out += ';';
        }
        const bpf_call& step = path[i];
        const auto& loc      = step.site.instruction;
        out += id_text( step.caller ) + "->" + id_text( step.callee ) + "@" +
               std::to_string( loc.section_index ) + ":" +
               std::to_string( loc.byte_offset ) + "/slot=" +
               std::to_string( loc.slot_index ) + "/foff=" +
               std::to_string( loc.file_offset );
        if ( step.site.has_relocation ) {
            const auto& rel = step.site.relocation;
            out += "/rel=" + std::to_string( rel.section_index ) + ":" +
                   std::to_string( rel.entry_index ) + "/rfoff=" +
                   std::to_string( rel.file_offset ) + "/rsym=" +
                   id_text( rel.symbol );
        }
    }
    return out;
}

static unsigned find_maps_section( const elfio& elf )
{
    for ( unsigned i = 0; i < elf.sections.size(); ++i ) {
        const section* sec = elf.sections[i];
        if ( sec != nullptr && sec->get_name() == "maps" ) {
            return i;
        }
    }
    return static_cast<unsigned>( -1 );
}

static std::vector<bpf_symbol_id> find_global_functions( const elfio& elf )
{
    std::vector<bpf_symbol_id> entries;
    for ( unsigned symtab_index = 0; symtab_index < elf.sections.size();
          ++symtab_index ) {
        const section* symtab = elf.sections[symtab_index];
        if ( symtab == nullptr || symtab->get_type() != SHT_SYMTAB ) {
            continue;
        }

        const_symbol_section_accessor symbols( elf, symtab );
        for ( Elf_Xword i = 0; i < symbols.get_symbols_num(); ++i ) {
            std::string name;
            Elf64_Addr value = 0;
            Elf_Xword size   = 0;
            unsigned char bind = 0, type = 0, other = 0;
            Elf_Half section_index = 0;
            if ( !symbols.get_symbol( i, name, value, size, bind, type,
                                      section_index, other ) ) {
                continue;
            }
            const section* sec =
                section_index < elf.sections.size() ? elf.sections[section_index]
                                                    : nullptr;
            if ( bind == STB_GLOBAL && type == STT_FUNC && size > 0 &&
                 sec != nullptr && sec->get_type() == SHT_PROGBITS &&
                 ( sec->get_flags() & SHF_EXECINSTR ) != 0 ) {
                entries.push_back( { symtab_index,
                                     static_cast<unsigned>( i ) } );
            }
        }
    }
    return entries;
}

static void write_tsv( const bpf_graph& graph, std::ostream& out )
{
    std::map<bpf_symbol_id, std::string> function_names;
    std::map<bpf_symbol_id, std::string> map_names;
    for ( const auto& fn : graph.functions ) {
        function_names[fn.id] = fn.name;
    }
    for ( const auto& map : graph.maps ) {
        map_names[map.id] = map.name;
    }

    for ( const bpf_function& fn : graph.functions ) {
        out << "F\t" << id_text( fn.id )
            << '\t' << tsv_text( fn.name )
            << '\t' << fn.section_index
            << '\t' << fn.value
            << '\t' << fn.size
            << '\t' << fn.slot_index
            << '\t' << fn.file_offset << '\n';
    }

    for ( const bpf_call& call : graph.calls ) {
        std::string callee_name;
        auto it = function_names.find( call.callee );
        if ( it != function_names.end() ) {
            callee_name = it->second;
        }
        out << "C\t" << id_text( call.caller )
            << '\t' << id_text( call.callee )
            << '\t' << tsv_text( callee_name )
            << '\t' << call.target_section
            << '\t' << call.target_offset;
        write_location_tsv( out, call.site );
        out << '\n';
    }

    for ( const bpf_map_reference& ref : graph.map_references ) {
        const bpf_map* map = nullptr;
        for ( const bpf_map& candidate : graph.maps ) {
            if ( candidate.id == ref.map ) {
                map = &candidate;
                break;
            }
        }
        out << "M\t" << id_text( ref.owner )
            << '\t' << id_text( ref.map )
            << '\t' << tsv_text( map != nullptr ? map->name : std::string() );
        if ( map != nullptr ) {
            out << '\t' << map->section_index
                << '\t' << map->value
                << '\t' << map->size
                << '\t' << map->file_offset;
        }
        else {
            out << "\t-\t-\t-\t-";
        }
        write_location_tsv( out, ref.site );
        out << '\n';
    }

    for ( const bpf_helper_call& helper : graph.helpers ) {
        out << "H\t" << id_text( helper.owner )
            << '\t' << helper.helper_id;
        write_location_tsv( out, helper.site );
        out << '\n';
    }

    for ( const bpf_provenance& prov : graph.provenance ) {
        std::string map_name;
        auto it = map_names.find( prov.map );
        if ( it != map_names.end() ) {
            map_name = it->second;
        }
        out << "W\t" << id_text( prov.entry )
            << '\t' << id_text( prov.map )
            << '\t' << tsv_text( map_name )
            << '\t' << prov.call_path.size()
            << '\t' << tsv_text( path_text( prov.call_path ) );
        write_location_tsv( out, prov.map_site );
        out << '\n';
    }
}

static std::string function_node( const bpf_symbol_id& id )
{
    return "\"F:" + id.to_string() + "\"";
}

static std::string map_node( const bpf_symbol_id& id )
{
    return "\"M:" + id.to_string() + "\"";
}

static void write_dot( const bpf_graph& graph, std::ostream& out )
{
    out << "digraph bpf_graph {\n";
    out << "  rankdir=LR;\n";
    out << "  node [fontname=\"monospace\"];\n";

    for ( const bpf_function& fn : graph.functions ) {
        out << "  " << function_node( fn.id )
            << " [shape=box,label=\"" << dot_text( fn.name ) << "\"];\n";
    }
    for ( const bpf_map& map : graph.maps ) {
        out << "  " << map_node( map.id )
            << " [shape=cylinder,label=\"" << dot_text( map.name ) << "\"];\n";
    }
    for ( const bpf_call& call : graph.calls ) {
        const auto& loc = call.site.instruction;
        out << "  " << function_node( call.caller ) << " -> "
            << function_node( call.callee )
            << " [label=\"C " << loc.section_index << ':' << loc.byte_offset
            << "\"];\n";
    }
    for ( const bpf_map_reference& ref : graph.map_references ) {
        const auto& loc = ref.site.instruction;
        out << "  " << function_node( ref.owner ) << " -> "
            << map_node( ref.map )
            << " [label=\"M " << loc.section_index << ':' << loc.byte_offset
            << "\"];\n";
    }
    out << "}\n";
}

int main( int argc, char** argv )
{
    if ( argc != 3 ) {
        std::cerr << "usage: " << ( argc > 0 ? argv[0] : "bpf_graph" )
                  << " <object.o> <output-prefix>\n";
        return 2;
    }

    const std::string object_path = argv[1];
    const std::string prefix      = argv[2];

    elfio elf;
    if ( !elf.load( object_path ) ) {
        std::cerr << "BPF_GRAPH_ERROR cannot load ELF object: "
                  << object_path << '\n';
        return 1;
    }

    const unsigned maps_section = find_maps_section( elf );
    if ( maps_section == static_cast<unsigned>( -1 ) ) {
        std::cerr << "BPF_GRAPH_ERROR missing section named 'maps'\n";
        return 2;
    }

    const std::vector<bpf_symbol_id> entries = find_global_functions( elf );
    bpf_analysis_result result =
        analyze_bpf_graph( elf, entries, { maps_section } );
    if ( !result.ok ) {
        std::cerr << "BPF_GRAPH_UNSUPPORTED " << result.error << '\n';
        for ( const bpf_issue& issue : result.issues ) {
            if ( issue.has_location ) {
                const auto& loc = issue.location;
                std::cerr << "  at section=" << loc.section_index
                          << " byte_offset=" << loc.byte_offset
                          << " slot=" << loc.slot_index
                          << " file_offset=" << loc.file_offset << '\n';
            }
        }
        return 2;
    }

    std::ofstream tsv( prefix + ".tsv", std::ios::trunc );
    std::ofstream dot( prefix + ".dot", std::ios::trunc );
    if ( !tsv || !dot ) {
        std::cerr << "BPF_GRAPH_ERROR cannot open output prefix: "
                  << prefix << '\n';
        return 1;
    }

    write_tsv( result.graph, tsv );
    write_dot( result.graph, dot );
    if ( !tsv || !dot ) {
        std::cerr << "BPF_GRAPH_ERROR failed while writing outputs\n";
        return 1;
    }
    return 0;
}
