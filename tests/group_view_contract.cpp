// Contract tests for the read-only section group / relation view.
//
// The ELF files used here are assembled by hand with ELFIO's own writer and
// reloaded through a string stream, so the tests exercise real on-disk
// encoding for every ELFCLASS32/64 and little/big endian combination.
#include <elfio/elfio.hpp>
#include <elfio/elfio_dump.hpp>

#include <algorithm>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "../task_support/check.hpp"

using namespace ELFIO;

namespace {

constexpr unsigned char ALL_CLASSES[]   = { ELFCLASS32, ELFCLASS64 };
constexpr unsigned char ALL_ENCODINGS[] = { ELFDATA2LSB, ELFDATA2MSB };

struct section_snapshot
{
    Elf_Half    index;
    std::string name;
    Elf_Word    type;
    Elf_Xword   flags;
    Elf_Word    link;
    Elf_Word    info;
    Elf_Xword   size;
    std::string data;
};

std::vector<section_snapshot> snapshot( const elfio& reader )
{
    std::vector<section_snapshot> out;
    for ( Elf_Half i = 0; i < reader.sections.size(); ++i ) {
        const section* sec = reader.sections[i];
        section_snapshot snap;
        snap.index = sec->get_index();
        snap.name  = sec->get_name();
        snap.type  = sec->get_type();
        snap.flags = sec->get_flags();
        snap.link  = sec->get_link();
        snap.info  = sec->get_info();
        snap.size  = sec->get_size();
        if ( sec->get_type() != SHT_NOBITS && sec->get_data() != nullptr ) {
            snap.data.assign( sec->get_data(),
                              static_cast<size_t>( sec->get_size() ) );
        }
        out.push_back( snap );
    }
    return out;
}

void same_snapshot( const std::vector<section_snapshot>& before,
                    const std::vector<section_snapshot>& after )
{
    TASK_CHECK( before.size() == after.size() );
    for ( size_t i = 0; i < before.size(); ++i ) {
        TASK_CHECK( before[i].index == after[i].index );
        TASK_CHECK( before[i].name == after[i].name );
        TASK_CHECK( before[i].type == after[i].type );
        TASK_CHECK( before[i].flags == after[i].flags );
        TASK_CHECK( before[i].link == after[i].link );
        TASK_CHECK( before[i].info == after[i].info );
        TASK_CHECK( before[i].size == after[i].size );
        TASK_CHECK( before[i].data == after[i].data );
    }
}

const group_error* find_error( const group_relation_view& view,
                               group_error_kind           kind,
                               Elf_Half                   group_index )
{
    for ( const auto& error : view.errors ) {
        if ( error.kind == kind &&
             error.group_section_index == group_index ) {
            return &error;
        }
    }
    return nullptr;
}

bool has_section( const std::vector<Elf_Half>& sections, Elf_Half index )
{
    return std::find( sections.begin(), sections.end(), index ) !=
           sections.end();
}

bool has_relation( const std::vector<section_relation>& relations,
                   section_relation_kind                 kind,
                   Elf_Half                              from,
                   Elf_Half                              to )
{
    return std::any_of( relations.begin(), relations.end(),
                        [&]( const section_relation& relation ) {
                            return relation.kind == kind &&
                                   relation.from == from && relation.to == to;
                        } );
}

struct good_ids
{
    Elf_Half str_a, sym_a, str_b, sym_b;
    Elf_Half text_f, rel_f, rel_h, text_h;
    Elf_Half group_a, data_g, group_b, text_s, group_c;
    Elf_Half exidx, cyc_text, cyc_rel, ord_cyc;
    Elf_Word sig_a, sig_c, sig_b;
};

section* make_progbits( elfio&       reader,
                        const char*  name,
                        Elf_Xword    flags,
                        const char*  bytes )
{
    section* sec = reader.sections.add( name );
    sec->set_type( SHT_PROGBITS );
    sec->set_flags( flags );
    sec->set_data( bytes, static_cast<Elf_Word>( std::strlen( bytes ) ) );
    sec->set_addr_align( 1 );
    return sec;
}

void write_group( elfio& reader, section* group,
                  const std::vector<Elf_Word>& words )
{
    std::vector<Elf_Word> encoded;
    encoded.reserve( words.size() );
    for ( Elf_Word word : words ) {
        encoded.push_back( reader.get_convertor()( word ) );
    }
    group->set_data( reinterpret_cast<const char*>( encoded.data() ),
                     static_cast<Elf_Word>( encoded.size() *
                                            sizeof( Elf_Word ) ) );
}

// A valid, deliberately rich relocatable object containing three groups,
// two same-named symbol tables, cross-group relocations and a relation cycle.
void build_good_file( elfio& reader, good_ids& ids )
{
    const bool is64 = reader.get_class() == ELFCLASS64;
    const Elf_Xword sym_entry = is64 ? sizeof( Elf64_Sym ) : sizeof( Elf32_Sym );
    const Elf_Xword rela_entry =
        is64 ? sizeof( Elf64_Rela ) : sizeof( Elf32_Rela );

    section* str_a = reader.sections.add( ".strtab" );
    str_a->set_type( SHT_STRTAB );
    str_a->set_addr_align( 1 );
    string_section_accessor str_a_acc( str_a );
    ids.str_a = str_a->get_index();

    section* sym_a = reader.sections.add( ".symtab" );
    sym_a->set_type( SHT_SYMTAB );
    sym_a->set_link( str_a->get_index() );
    sym_a->set_info( 1 );
    sym_a->set_entry_size( sym_entry );
    sym_a->set_addr_align( is64 ? 8 : 4 );
    ids.sym_a = sym_a->get_index();

    section* str_b = reader.sections.add( ".strtab2" );
    str_b->set_type( SHT_STRTAB );
    str_b->set_addr_align( 1 );
    string_section_accessor str_b_acc( str_b );
    ids.str_b = str_b->get_index();

    // Deliberately the same section name as sym_a: identity is the index.
    section* sym_b = reader.sections.add( ".symtab" );
    sym_b->set_type( SHT_SYMTAB );
    sym_b->set_link( str_b->get_index() );
    sym_b->set_info( 1 );
    sym_b->set_entry_size( sym_entry );
    sym_b->set_addr_align( is64 ? 8 : 4 );
    ids.sym_b = sym_b->get_index();

    section* text_f =
        make_progbits( reader, ".text.f", SHF_GROUP, "textf" );
    ids.text_f = text_f->get_index();

    section* rel_f = reader.sections.add( ".rela.text.f" );
    rel_f->set_type( SHT_RELA );
    rel_f->set_flags( SHF_GROUP );
    rel_f->set_link( sym_a->get_index() );
    rel_f->set_info( text_f->get_index() );
    rel_f->set_entry_size( rela_entry );
    rel_f->set_addr_align( is64 ? 8 : 4 );
    ids.rel_f = rel_f->get_index();
    relocation_section_accessor( reader, rel_f )
        .add_entry( 0, 0, R_X86_64_NONE, 0 );

    section* text_h = make_progbits( reader, ".text.h", 0, "texth" );
    ids.text_h = text_h->get_index();

    // Relocation living inside group A whose sh_info target sits outside the
    // group: group expansion must keep following the relationship.
    section* rel_h = reader.sections.add( ".rela.text.h" );
    rel_h->set_type( SHT_RELA );
    rel_h->set_flags( SHF_GROUP );
    rel_h->set_link( sym_a->get_index() );
    rel_h->set_info( text_h->get_index() );
    rel_h->set_entry_size( rela_entry );
    rel_h->set_addr_align( is64 ? 8 : 4 );
    ids.rel_h = rel_h->get_index();
    relocation_section_accessor( reader, rel_h )
        .add_entry( 0, 0, R_X86_64_NONE, 0 );

    symbol_section_accessor sym_a_acc( reader, sym_a );
    ids.sig_a = sym_a_acc.add_symbol( str_a_acc, "comsig", 0, 4, STB_GLOBAL,
                                      STT_FUNC, 0, text_f->get_index() );
    // Unnamed section symbol: signatures are not required to be functions.
    ids.sig_c = sym_a_acc.add_symbol( str_a_acc, "", 0, 0, STB_LOCAL,
                                      STT_SECTION, 0, text_f->get_index() );

    section* group_a = reader.sections.add( ".group" );
    group_a->set_type( SHT_GROUP );
    group_a->set_entry_size( 4 );
    group_a->set_addr_align( 4 );
    group_a->set_link( sym_a->get_index() );
    group_a->set_info( ids.sig_a );
    ids.group_a = group_a->get_index();
    write_group( reader, group_a,
                 { GRP_COMDAT | 0x10000, text_f->get_index(),
                   rel_f->get_index(), rel_h->get_index() } );

    // Same section name as text_f: name-based merging would confuse these.
    section* data_g =
        make_progbits( reader, ".text.f", SHF_GROUP, "datag" );
    ids.data_g = data_g->get_index();

    symbol_section_accessor sym_b_acc( reader, sym_b );
    ids.sig_b = sym_b_acc.add_symbol( str_b_acc, "comsig", 0, 8, STB_GLOBAL,
                                      STT_OBJECT, 0, data_g->get_index() );

    // Plain (non-COMDAT) group, same signature text but a distinct identity.
    section* group_b = reader.sections.add( ".group" );
    group_b->set_type( SHT_GROUP );
    group_b->set_entry_size( 4 );
    group_b->set_addr_align( 4 );
    group_b->set_link( sym_b->get_index() );
    group_b->set_info( ids.sig_b );
    ids.group_b = group_b->get_index();
    write_group( reader, group_b, { 0, data_g->get_index() } );

    section* text_s =
        make_progbits( reader, ".text.s", SHF_GROUP, "texts" );
    ids.text_s = text_s->get_index();

    section* group_c = reader.sections.add( ".group" );
    group_c->set_type( SHT_GROUP );
    group_c->set_entry_size( 4 );
    group_c->set_addr_align( 4 );
    group_c->set_link( sym_a->get_index() );
    group_c->set_info( ids.sig_c );
    ids.group_c = group_c->get_index();
    write_group( reader, group_c, { GRP_COMDAT, text_s->get_index() } );

    section* exidx =
        make_progbits( reader, ".exidx", SHF_LINK_ORDER, "exidx" );
    exidx->set_link( text_f->get_index() );
    ids.exidx = exidx->get_index();

    // Triangle of relationships: link-order, link-order, relocation. The
    // undirected graph therefore contains a legal cycle.
    section* cyc_text = make_progbits( reader, ".text.cyc",
                                       SHF_LINK_ORDER, "cyc" );
    ids.cyc_text = cyc_text->get_index();

    section* ord_cyc =
        make_progbits( reader, ".ord.cyc", SHF_LINK_ORDER, "ord" );
    ids.ord_cyc = ord_cyc->get_index();

    section* cyc_rel = reader.sections.add( ".rela.text.cyc" );
    cyc_rel->set_type( SHT_RELA );
    cyc_rel->set_link( sym_a->get_index() );
    cyc_rel->set_info( cyc_text->get_index() );
    cyc_rel->set_entry_size( rela_entry );
    cyc_rel->set_addr_align( is64 ? 8 : 4 );
    ids.cyc_rel = cyc_rel->get_index();
    relocation_section_accessor( reader, cyc_rel )
        .add_entry( 0, 0, R_X86_64_NONE, 0 );

    cyc_text->set_link( ord_cyc->get_index() );
    ord_cyc->set_link( cyc_rel->get_index() );
}

struct bad_ids
{
    Elf_Half strtab, symtab, plain;
    Elf_Half text_a, text_b, text_c, text_d, text_e;
    Elf_Half group_empty, group_bad, group_two, group_malformed;
    Elf_Half symtab_other, group_bad_strtab;
};

// An object exercising every structural error the view has to localize.
void build_bad_file( elfio& reader, bad_ids& ids )
{
    const bool is64 = reader.get_class() == ELFCLASS64;

    section* strtab = reader.sections.add( ".strtab" );
    strtab->set_type( SHT_STRTAB );
    strtab->set_addr_align( 1 );
    string_section_accessor strings( strtab );
    ids.strtab = strtab->get_index();

    section* symtab = reader.sections.add( ".symtab" );
    symtab->set_type( SHT_SYMTAB );
    symtab->set_link( strtab->get_index() );
    symtab->set_info( 1 );
    symtab->set_entry_size( is64 ? sizeof( Elf64_Sym ) : sizeof( Elf32_Sym ) );
    symtab->set_addr_align( is64 ? 8 : 4 );
    ids.symtab = symtab->get_index();
    symbol_section_accessor( reader, symtab )
        .add_symbol( strings, "x", 0, 1, STB_GLOBAL, STT_NOTYPE, 0, 0 );

    section* plain = make_progbits( reader, ".plain", 0, "p" );
    ids.plain = plain->get_index();

    section* text_a =
        make_progbits( reader, ".text.a", SHF_GROUP, "a" );
    ids.text_a = text_a->get_index();
    section* text_b =
        make_progbits( reader, ".text.b", SHF_GROUP, "b" );
    ids.text_b = text_b->get_index();
    section* text_c = make_progbits( reader, ".text.c", 0, "c" );
    ids.text_c = text_c->get_index();
    section* text_d =
        make_progbits( reader, ".text.d", SHF_GROUP, "d" );
    ids.text_d = text_d->get_index();
    section* text_e =
        make_progbits( reader, ".text.e", SHF_GROUP, "e" );
    ids.text_e = text_e->get_index();

    // Empty group: flag word only.
    section* group_empty = reader.sections.add( ".group" );
    group_empty->set_type( SHT_GROUP );
    group_empty->set_entry_size( 4 );
    group_empty->set_addr_align( 4 );
    group_empty->set_link( symtab->get_index() );
    group_empty->set_info( 1 );
    ids.group_empty = group_empty->get_index();
    write_group( reader, group_empty, { GRP_COMDAT } );

    // Out-of-range member, duplicate member, member missing SHF_GROUP, and a
    // sh_link that does not point at a symbol table.
    section* group_bad = reader.sections.add( ".group" );
    group_bad->set_type( SHT_GROUP );
    group_bad->set_entry_size( 4 );
    group_bad->set_addr_align( 4 );
    group_bad->set_link( plain->get_index() );
    group_bad->set_info( 1 );
    ids.group_bad = group_bad->get_index();
    write_group( reader, group_bad,
                 { GRP_COMDAT, 999, text_b->get_index(),
                   text_b->get_index(), text_c->get_index() } );

    // text_b is now claimed by two groups; sh_info names no real symbol.
    section* group_two = reader.sections.add( ".group" );
    group_two->set_type( SHT_GROUP );
    group_two->set_entry_size( 4 );
    group_two->set_addr_align( 4 );
    group_two->set_link( symtab->get_index() );
    group_two->set_info( 99 );
    ids.group_two = group_two->get_index();
    write_group( reader, group_two,
                 { GRP_COMDAT, text_b->get_index(), text_a->get_index() } );

    // 11 bytes: two whole words plus a trailing fragment, so malformed data
    // without also tripping the empty-group diagnostic.
    section* group_malformed = reader.sections.add( ".group" );
    group_malformed->set_type( SHT_GROUP );
    group_malformed->set_entry_size( 4 );
    group_malformed->set_addr_align( 4 );
    group_malformed->set_link( symtab->get_index() );
    group_malformed->set_info( 1 );
    ids.group_malformed = group_malformed->get_index();
    {
        Elf_Word words[2] = { reader.get_convertor()( Elf_Word( GRP_COMDAT ) ),
                              reader.get_convertor()(
                                  Elf_Word( text_e->get_index() ) ) };
        char buffer[11];
        std::memcpy( buffer, words, sizeof( words ) );
        buffer[8]  = 0;
        buffer[9]  = 0;
        buffer[10] = 0;
        group_malformed->set_data( buffer, 11 );
    }

    // Symbol table whose sh_link is not a string table, reached through a
    // group: signature string-table resolution must fail cleanly.
    section* symtab_other = reader.sections.add( ".symtab.other" );
    symtab_other->set_type( SHT_SYMTAB );
    symtab_other->set_link( plain->get_index() );
    symtab_other->set_info( 1 );
    symtab_other->set_entry_size(
        is64 ? sizeof( Elf64_Sym ) : sizeof( Elf32_Sym ) );
    symtab_other->set_addr_align( is64 ? 8 : 4 );
    ids.symtab_other = symtab_other->get_index();

    section* group_bad_strtab = reader.sections.add( ".group" );
    group_bad_strtab->set_type( SHT_GROUP );
    group_bad_strtab->set_entry_size( 4 );
    group_bad_strtab->set_addr_align( 4 );
    group_bad_strtab->set_link( symtab_other->get_index() );
    group_bad_strtab->set_info( 1 );
    ids.group_bad_strtab = group_bad_strtab->get_index();
    write_group( reader, group_bad_strtab,
                 { GRP_COMDAT, text_a->get_index() } );
    // text_d deliberately stays unreferenced: dangling SHF_GROUP.
}

void check_good_view( const elfio& reader, const good_ids& ids )
{
    group_relation_view view = build_group_relation_view( reader );

    TASK_CHECK( view.errors.empty() );
    TASK_CHECK( view.dangling_sections.empty() );
    TASK_CHECK( view.groups.size() == 3 );
    TASK_CHECK( view.groups[0].group_section_index == ids.group_a );
    TASK_CHECK( view.groups[1].group_section_index == ids.group_b );
    TASK_CHECK( view.groups[2].group_section_index == ids.group_c );

    const group_info& a = view.groups[0];
    TASK_CHECK( a.flags.raw == ( GRP_COMDAT | 0x10000 ) );
    TASK_CHECK( a.flags.comdat );
    TASK_CHECK( a.flags.uninterpreted_bits.size() == 1 );
    TASK_CHECK( a.flags.uninterpreted_bits[0] == 0x10000 );
    TASK_CHECK( a.members.size() == 3 );
    TASK_CHECK( a.members[0].section_index == ids.text_f );
    TASK_CHECK( a.members[1].section_index == ids.rel_f );
    TASK_CHECK( a.members[2].section_index == ids.rel_h );
    TASK_CHECK( a.members[0].word_offset == 4 );
    TASK_CHECK( a.members[1].word_offset == 8 );
    TASK_CHECK( a.members[2].word_offset == 12 );
    TASK_CHECK( a.members[0].in_range && a.members[0].has_group_flag );
    TASK_CHECK( !a.members[0].duplicate );

    // Signature triple: group, symbol table and symbol index all retained.
    TASK_CHECK( a.signature.resolved );
    TASK_CHECK( a.signature.symbol_table_index == ids.sym_a );
    TASK_CHECK( a.signature.symbol_index == ids.sig_a );
    TASK_CHECK( a.signature.string_table_index == ids.str_a );
    TASK_CHECK( a.signature.name == "comsig" );
    TASK_CHECK( a.signature.type == STT_FUNC );

    const group_info& b = view.groups[1];
    TASK_CHECK( b.flags.raw == 0 );
    TASK_CHECK( !b.flags.comdat );
    TASK_CHECK( b.flags.uninterpreted_bits.empty() );
    TASK_CHECK( b.members.size() == 1 );
    TASK_CHECK( b.members[0].section_index == ids.data_g );
    // Same-named symbol tables must not be mixed, and identical signature
    // text must not cause the groups to merge.
    TASK_CHECK( b.signature.symbol_table_index == ids.sym_b );
    TASK_CHECK( b.signature.symbol_index == ids.sig_b );
    TASK_CHECK( b.signature.string_table_index == ids.str_b );
    TASK_CHECK( b.signature.name == "comsig" );
    TASK_CHECK( b.signature.type == STT_OBJECT );
    TASK_CHECK( a.signature.symbol_table_index !=
                b.signature.symbol_table_index );
    TASK_CHECK( a.signature.string_table_index !=
                b.signature.string_table_index );

    const group_info& c = view.groups[2];
    TASK_CHECK( c.flags.comdat );
    TASK_CHECK( c.signature.resolved );
    TASK_CHECK( c.signature.name.empty() );
    TASK_CHECK( c.signature.type == STT_SECTION );
    TASK_CHECK( c.signature.symbol_table_index == ids.sym_a );
    TASK_CHECK( c.signature.symbol_index == ids.sig_c );

    // Same-named sections keep distinct physical identities.
    TASK_CHECK( ids.text_f != ids.data_g );
    TASK_CHECK( reader.sections[ids.text_f]->get_name() ==
                reader.sections[ids.data_g]->get_name() );
    TASK_CHECK( view.group_owner[ids.text_f] == ids.group_a );
    TASK_CHECK( view.group_owner[ids.data_g] == ids.group_b );
    TASK_CHECK( view.group_owner[ids.text_s] == ids.group_c );
    TASK_CHECK( view.group_owner[ids.text_h] == SHN_UNDEF );
}

void check_bad_view( const elfio& reader, const bad_ids& ids )
{
    group_relation_view view = build_group_relation_view( reader );

    const group_error* empty =
        find_error( view, group_error_kind::empty_group, ids.group_empty );
    TASK_CHECK( empty != nullptr );

    const group_error* range = find_error(
        view, group_error_kind::member_out_of_range, ids.group_bad );
    TASK_CHECK( range != nullptr );
    TASK_CHECK( range->word_offset == 4 );
    TASK_CHECK( range->value == 999 );

    const group_error* duplicate =
        find_error( view, group_error_kind::duplicate_member, ids.group_bad );
    TASK_CHECK( duplicate != nullptr );
    TASK_CHECK( duplicate->word_offset == 12 );
    TASK_CHECK( duplicate->value == ids.text_b );

    const group_error* no_flag = find_error(
        view, group_error_kind::member_missing_group_flag, ids.group_bad );
    TASK_CHECK( no_flag != nullptr );
    TASK_CHECK( no_flag->word_offset == 16 );
    TASK_CHECK( no_flag->value == ids.text_c );

    const group_error* two_groups = find_error(
        view, group_error_kind::member_in_multiple_groups, ids.group_two );
    TASK_CHECK( two_groups != nullptr );
    TASK_CHECK( two_groups->word_offset == 4 );
    TASK_CHECK( two_groups->value == ids.text_b );

    const group_error* bad_link =
        find_error( view, group_error_kind::invalid_group_link,
                    ids.group_bad );
    TASK_CHECK( bad_link != nullptr );
    TASK_CHECK( bad_link->value == ids.plain );

    const group_error* bad_info =
        find_error( view, group_error_kind::invalid_group_info,
                    ids.group_two );
    TASK_CHECK( bad_info != nullptr );
    TASK_CHECK( bad_info->value == 99 );

    const group_error* bad_strtab =
        find_error( view, group_error_kind::invalid_signature_string_table,
                    ids.group_bad_strtab );
    TASK_CHECK( bad_strtab != nullptr );

    const group_error* malformed = find_error(
        view, group_error_kind::malformed_group_data, ids.group_malformed );
    TASK_CHECK( malformed != nullptr );
    TASK_CHECK( malformed->value == 11 );

    const group_error* dangling = nullptr;
    for ( const auto& error : view.errors ) {
        if ( error.kind == group_error_kind::dangling_group_flag ) {
            dangling = &error;
            break;
        }
    }
    TASK_CHECK( dangling != nullptr );
    TASK_CHECK( dangling->group_section_index == SHN_UNDEF );
    TASK_CHECK( dangling->value == ids.text_d );
    TASK_CHECK( view.dangling_sections.size() == 1 );
    TASK_CHECK( view.dangling_sections[0] == ids.text_d );
}

void check_closure( const elfio& reader, const good_ids& ids )
{
    // Selecting the group itself pulls in every member, and the in-group
    // relocation keeps expanding to its external target.
    section_relation_result from_group =
        compute_section_relations( reader, { ids.group_a } );
    TASK_CHECK( from_group.ok );
    TASK_CHECK( has_section( from_group.sections, ids.group_a ) );
    TASK_CHECK( has_section( from_group.sections, ids.text_f ) );
    TASK_CHECK( has_section( from_group.sections, ids.rel_f ) );
    TASK_CHECK( has_section( from_group.sections, ids.rel_h ) );
    TASK_CHECK( has_section( from_group.sections, ids.text_h ) );
    TASK_CHECK( has_section( from_group.sections, ids.exidx ) );
    // Plain sh_link edges are not followed: symbol/string tables stay out.
    TASK_CHECK( !has_section( from_group.sections, ids.sym_a ) );
    TASK_CHECK( !has_section( from_group.sections, ids.str_a ) );
    TASK_CHECK( !has_section( from_group.sections, ids.sym_b ) );
    TASK_CHECK( !has_section( from_group.sections, ids.group_b ) );
    TASK_CHECK( std::is_sorted( from_group.sections.begin(),
                                from_group.sections.end() ) );
    TASK_CHECK( has_relation( from_group.relations,
                              section_relation_kind::group_member,
                              ids.group_a, ids.text_f ) );
    TASK_CHECK( has_relation( from_group.relations,
                              section_relation_kind::relocation_target,
                              ids.rel_h, ids.text_h ) );
    TASK_CHECK( has_relation( from_group.relations,
                              section_relation_kind::relocation_target,
                              ids.rel_f, ids.text_f ) );
    TASK_CHECK( has_relation( from_group.relations,
                              section_relation_kind::link_order,
                              ids.exidx, ids.text_f ) );

    // Selecting a member reaches the identical set.
    section_relation_result from_member =
        compute_section_relations( reader, { ids.text_f } );
    TASK_CHECK( from_member.ok );
    TASK_CHECK( from_member.sections == from_group.sections );

    // Selecting the in-group reloc reaches the group as well.
    section_relation_result from_rel =
        compute_section_relations( reader, { ids.rel_h } );
    TASK_CHECK( from_rel.ok );
    TASK_CHECK( has_section( from_rel.sections, ids.group_a ) );
    TASK_CHECK( has_section( from_rel.sections, ids.text_h ) );

    // Group B is isolated.
    section_relation_result group_b =
        compute_section_relations( reader, { ids.group_b } );
    TASK_CHECK( group_b.ok );
    TASK_CHECK( group_b.sections.size() == 2 );
    TASK_CHECK( has_section( group_b.sections, ids.group_b ) );
    TASK_CHECK( has_section( group_b.sections, ids.data_g ) );
    section_relation_result from_data =
        compute_section_relations( reader, { ids.data_g } );
    TASK_CHECK( from_data.sections == group_b.sections );

    // The relation cycle terminates and is traversable from either side.
    section_relation_result cycle_one =
        compute_section_relations( reader, { ids.cyc_text } );
    TASK_CHECK( cycle_one.ok );
    TASK_CHECK( cycle_one.sections.size() == 3 );
    TASK_CHECK( has_section( cycle_one.sections, ids.cyc_text ) );
    TASK_CHECK( has_section( cycle_one.sections, ids.cyc_rel ) );
    TASK_CHECK( has_section( cycle_one.sections, ids.ord_cyc ) );
    section_relation_result cycle_two =
        compute_section_relations( reader, { ids.ord_cyc } );
    TASK_CHECK( cycle_two.ok );
    TASK_CHECK( cycle_two.sections == cycle_one.sections );
    TASK_CHECK( has_relation( cycle_two.relations,
                              section_relation_kind::relocation_target,
                              ids.cyc_rel, ids.cyc_text ) );
    TASK_CHECK( has_relation( cycle_two.relations,
                              section_relation_kind::link_order,
                              ids.ord_cyc, ids.cyc_rel ) );
    TASK_CHECK( has_relation( cycle_two.relations,
                              section_relation_kind::link_order,
                              ids.cyc_text, ids.ord_cyc ) );

    // Two paths to .text.f: group membership and link order. Both edges are
    // retained as evidence.
    section_relation_result multi_path = compute_section_relations(
        reader, { ids.group_a, ids.exidx } );
    TASK_CHECK( multi_path.ok );
    TASK_CHECK( has_relation( multi_path.relations,
                              section_relation_kind::group_member,
                              ids.group_a, ids.text_f ) );
    TASK_CHECK( has_relation( multi_path.relations,
                              section_relation_kind::link_order,
                              ids.exidx, ids.text_f ) );

    // The request set is echoed back sorted and de-duplicated.
    section_relation_result requested = compute_section_relations(
        reader, { ids.text_h, ids.text_f, ids.text_h } );
    TASK_CHECK( requested.ok );
    TASK_CHECK( requested.requested.size() == 2 );
    TASK_CHECK( requested.requested[0] == ids.text_f );
    TASK_CHECK( requested.requested[1] == ids.text_h );
    TASK_CHECK( has_section( requested.sections, ids.group_a ) );

    // Invalid input: no plausible partial answer.
    section_relation_result invalid =
        compute_section_relations( reader, { 999 } );
    TASK_CHECK( !invalid.ok );
    TASK_CHECK( !invalid.error.empty() );
    TASK_CHECK( invalid.sections.empty() );
    TASK_CHECK( invalid.relations.empty() );
    TASK_CHECK( invalid.requested.empty() );
}

void check_dump_and_immutability( elfio& reader, const good_ids& ids )
{
    const auto before = snapshot( reader );

    section_relation_result result =
        compute_section_relations( reader, { ids.group_a, ids.cyc_text } );
    TASK_CHECK( result.ok );

    std::stringstream relation_output;
    dump::section_relations( relation_output, result, reader );
    const std::string relation_text = relation_output.str();
    TASK_CHECK( relation_text.find( "Section Relations:" ) !=
                std::string::npos );
    TASK_CHECK( relation_text.find( "comsig" ) != std::string::npos );
    for ( const auto& group : result.group_view.groups ) {
        TASK_CHECK( relation_text.find(
                        "Group [" ) != std::string::npos );
        TASK_CHECK( relation_text.find(
                        std::to_string( group.group_section_index ) ) !=
                    std::string::npos );
        TASK_CHECK( relation_text.find( "signature: symtab=" ) !=
                    std::string::npos );
    }
    for ( const auto& relation : result.relations ) {
        TASK_CHECK( relation_text.find(
                        detail::relation_kind_name( relation.kind ) ) !=
                    std::string::npos );
        TASK_CHECK( relation_text.find(
                        std::to_string( relation.from ) ) !=
                    std::string::npos );
        TASK_CHECK( relation_text.find(
                        std::to_string( relation.to ) ) != std::string::npos );
    }

    // Convenience overload taking the request directly.
    std::stringstream direct_output;
    dump::section_relations( direct_output, reader, { ids.group_b } );
    TASK_CHECK( direct_output.str().find( "Section Relations:" ) !=
                std::string::npos );

    // Existing dump output keeps working and still mentions the groups.
    std::stringstream header_output;
    dump::section_headers( header_output, reader );
    TASK_CHECK( header_output.str().find( "GROUP" ) != std::string::npos );

    same_snapshot( before, snapshot( reader ) );

    // The serialized object is byte-identical before and after the views ran.
    std::stringstream first_save;
    TASK_CHECK( reader.save( first_save ) );
    const std::string saved_before = first_save.str();
    (void)build_group_relation_view( reader );
    std::stringstream second_save;
    TASK_CHECK( reader.save( second_save ) );
    TASK_CHECK( saved_before == second_save.str() );
}

} // namespace

int contract_groups()
{
    int checks = 0;
    for ( unsigned char cls : ALL_CLASSES ) {
        for ( unsigned char enc : ALL_ENCODINGS ) {
            elfio reader;
            reader.create( cls, enc );
            reader.set_type( ET_REL );
            reader.set_machine( EM_X86_64 );

            good_ids ids;
            build_good_file( reader, ids );
            check_good_view( reader, ids );

            elfio bad;
            bad.create( cls, enc );
            bad.set_type( ET_REL );
            bad.set_machine( EM_X86_64 );
            bad_ids bad_id_set;
            build_bad_file( bad, bad_id_set );
            check_bad_view( bad, bad_id_set );

            ++checks;
        }
    }
    return checks;
}

int contract_closure()
{
    int checks = 0;
    for ( unsigned char cls : ALL_CLASSES ) {
        for ( unsigned char enc : ALL_ENCODINGS ) {
            elfio reader;
            reader.create( cls, enc );
            reader.set_type( ET_REL );
            reader.set_machine( EM_X86_64 );

            good_ids ids;
            build_good_file( reader, ids );
            check_closure( reader, ids );
            ++checks;
        }
    }
    return checks;
}

int contract_views()
{
    int checks = 0;
    for ( unsigned char cls : ALL_CLASSES ) {
        for ( unsigned char enc : ALL_ENCODINGS ) {
            elfio reader;
            reader.create( cls, enc );
            reader.set_type( ET_REL );
            reader.set_machine( EM_X86_64 );

            good_ids ids;
            build_good_file( reader, ids );
            check_dump_and_immutability( reader, ids );

            // Round trip through save/load: identities survive on disk for
            // every class/endian combination.
            std::stringstream store;
            TASK_CHECK( reader.save( store ) );
            elfio reloaded;
            TASK_CHECK( reloaded.load( store ) );
            group_relation_view view = build_group_relation_view( reloaded );
            TASK_CHECK( view.errors.empty() );
            TASK_CHECK( view.groups.size() == 3 );
            TASK_CHECK( view.groups[0].signature.symbol_table_index ==
                        ids.sym_a );
            TASK_CHECK( view.groups[1].signature.symbol_table_index ==
                        ids.sym_b );
            TASK_CHECK( view.groups[0].members.size() == 3 );
            TASK_CHECK( view.groups[0].members[2].section_index ==
                        ids.rel_h );

            section_relation_result closure =
                compute_section_relations( reloaded, { ids.group_a } );
            TASK_CHECK( closure.ok );
            TASK_CHECK( has_section( closure.sections, ids.text_h ) );
            TASK_CHECK( !has_section( closure.sections, ids.str_a ) );

            ++checks;
        }
    }
    return checks;
}
