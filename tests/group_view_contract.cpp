// Contract tests for the read-only section group / relation view.
// Hand-crafted minimal ELF objects are built with ELFIO itself and checked
// across all four class/endian combinations.

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <elfio/elfio.hpp>
#include <elfio/elfio_dump.hpp>

#include "task_support/check.hpp"

using namespace ELFIO;

namespace {

//------------------------------------------------------------------------------
elfio make_rel_file( unsigned char cls, unsigned char enc )
{
    elfio e;
    e.create( cls, enc );
    e.set_type( ET_REL );
    e.set_machine( EM_X86_64 );
    return e;
}

//------------------------------------------------------------------------------
elfio roundtrip( elfio& e )
{
    std::stringstream stream;
    if ( !e.save( stream ) ) {
        throw std::runtime_error( "save failed" );
    }
    elfio r;
    if ( !r.load( stream ) ) {
        throw std::runtime_error( "load failed" );
    }
    return r;
}

//------------------------------------------------------------------------------
section* add_strtab( elfio& e, const char* name )
{
    section* s = e.sections.add( name );
    s->set_type( SHT_STRTAB );
    s->set_addr_align( 1 );
    string_section_accessor strings( s );
    strings.add_string( "" );
    return s;
}

//------------------------------------------------------------------------------
section* add_symtab( elfio& e, const char* name, section* strtab )
{
    section* sym = e.sections.add( name );
    sym->set_type( SHT_SYMTAB );
    sym->set_info( 1 );
    sym->set_link( strtab->get_index() );
    sym->set_entry_size( e.get_class() == ELFCLASS32
                             ? sizeof( Elf32_Sym )
                             : sizeof( Elf64_Sym ) );
    sym->set_addr_align( e.get_class() == ELFCLASS32 ? 4 : 8 );
    return sym;
}

//------------------------------------------------------------------------------
void set_group_words( elfio& e, section* group,
                      const std::vector<Elf_Word>& words )
{
    std::vector<Elf_Word> raw;
    raw.reserve( words.size() );
    for ( Elf_Word word : words ) {
        raw.push_back( e.get_convertor()( word ) );
    }
    group->set_data( reinterpret_cast<const char*>( raw.data() ),
                     static_cast<Elf_Word>( raw.size() * sizeof( Elf_Word ) ) );
}

//------------------------------------------------------------------------------
section* add_progbits( elfio& e, const char* name, Elf_Xword flags )
{
    section* s = e.sections.add( name );
    s->set_type( SHT_PROGBITS );
    s->set_flags( flags );
    s->set_data( "abc", 3 );
    return s;
}

//------------------------------------------------------------------------------
unsigned count_occurrences( const std::string& haystack,
                            const std::string& needle )
{
    unsigned n = 0;
    size_t   pos = 0;
    while ( ( pos = haystack.find( needle, pos ) ) != std::string::npos ) {
        ++n;
        pos += needle.size();
    }
    return n;
}

//------------------------------------------------------------------------------
bool has_problem( const group_view& view, group_problem_kind kind,
                  Elf_Half group_section, Elf_Xword word_offset )
{
    for ( const auto& p : view.problems ) {
        if ( p.kind == kind && p.group_section == group_section &&
             p.word_offset == word_offset ) {
            return true;
        }
    }
    return false;
}

//------------------------------------------------------------------------------
std::set<Elf_Half> index_set( const std::vector<Elf_Half>& v )
{
    return std::set<Elf_Half>( v.begin(), v.end() );
}

//------------------------------------------------------------------------------
bool has_relation( const section_relation_view& r, relation_kind kind,
                   Elf_Half from, Elf_Half to )
{
    for ( const auto& edge : r.relations ) {
        if ( edge.kind == kind && edge.from == from && edge.to == to ) {
            return true;
        }
    }
    return false;
}

// Well-formed fixture: two same-named symbol tables backed by different
// string tables, three groups (plain, COMDAT with an unexplained flag, and a
// second COMDAT sharing signature text with group 2).
//------------------------------------------------------------------------------
elfio build_identity_fixture( unsigned char cls, unsigned char enc )
{
    elfio e = make_rel_file( cls, enc );

    section* str1 = add_strtab( e, ".strtab1" );
    section* sym1 = add_symtab( e, ".symtab", str1 );
    section* str2 = add_strtab( e, ".strtab2" );
    section* sym2 = add_symtab( e, ".symtab", str2 ); // same name on purpose

    section* text_a = add_progbits( e, ".text.a", SHF_GROUP );
    section* text_b = add_progbits( e, ".text.b", SHF_GROUP );
    section* rod_c  = add_progbits( e, ".rodata.c", SHF_GROUP );
    section* data_d = add_progbits( e, ".data.d", SHF_GROUP );

    symbol_section_accessor s1( e, sym1 );
    string_section_accessor st1( str1 );
    // Unnamed section symbol: signatures are not required to be functions.
    s1.add_symbol( st1, "", 0, 0, STB_LOCAL,
                   STT_SECTION, 0, text_a->get_index() );
    s1.add_symbol( st1, "foo", 0, 3, STB_GLOBAL,
                   STT_FUNC, 0, text_a->get_index() );

    symbol_section_accessor s2( e, sym2 );
    string_section_accessor st2( str2 );
    s2.add_symbol( st2, "foo", 0, 3, STB_GLOBAL, STT_FUNC, 0,
                   rod_c->get_index() );

    section* g1 = e.sections.add( ".group1" );
    g1->set_type( SHT_GROUP );
    g1->set_entry_size( 4 );
    g1->set_addr_align( 4 );
    g1->set_link( sym1->get_index() );
    g1->set_info( 1 );
    set_group_words(
        e, g1, { 0, text_a->get_index(), text_b->get_index() } );

    section* g2 = e.sections.add( ".group2" );
    g2->set_type( SHT_GROUP );
    g2->set_entry_size( 4 );
    g2->set_addr_align( 4 );
    g2->set_link( sym2->get_index() );
    g2->set_info( 1 );
    set_group_words( e, g2,
                     { GRP_COMDAT | 0x40000000u, rod_c->get_index() } );

    section* g3 = e.sections.add( ".group3" );
    g3->set_type( SHT_GROUP );
    g3->set_entry_size( 4 );
    g3->set_addr_align( 4 );
    g3->set_link( sym1->get_index() );
    g3->set_info( 2 );
    set_group_words( e, g3, { GRP_COMDAT, data_d->get_index() } );

    return e;
}

//------------------------------------------------------------------------------
elfio build_problem_fixture( unsigned char cls, unsigned char enc )
{
    elfio e = make_rel_file( cls, enc );

    section* str  = add_strtab( e, ".strtab" );
    section* sym  = add_symtab( e, ".symtab", str );
    section* plain = add_progbits( e, ".plain", 0 ); // no SHF_GROUP
    section* ok    = add_progbits( e, ".text.ok", SHF_GROUP );
    add_progbits( e, ".dangle", SHF_GROUP ); // never listed in any group
    section* same1 = add_progbits( e, ".text.same", SHF_GROUP );
    section* same2 = add_progbits( e, ".text.same", SHF_GROUP );

    symbol_section_accessor sa( e, sym );
    string_section_accessor  st( str );
    sa.add_symbol( st, "sig", 0, 0, STB_GLOBAL,
                   STT_FUNC, 0, ok->get_index() );

    // Empty group: flag word only.
    section* g_empty = e.sections.add( ".g.empty" );
    g_empty->set_type( SHT_GROUP );
    g_empty->set_link( sym->get_index() );
    g_empty->set_info( 1 );
    set_group_words( e, g_empty, { GRP_COMDAT } );

    // Malformed payload: size not divisible by Elf_Word.
    section* g_malformed = e.sections.add( ".g.malformed" );
    g_malformed->set_type( SHT_GROUP );
    g_malformed->set_link( sym->get_index() );
    g_malformed->set_info( 1 );
    char junk[6] = { 1, 0, 0, 0, 2, 0 };
    g_malformed->set_data( junk, 6 );

    // Zero, out-of-range, duplicate and unflagged members.
    section* g_bad = e.sections.add( ".g.bad" );
    g_bad->set_type( SHT_GROUP );
    g_bad->set_link( sym->get_index() );
    g_bad->set_info( 1 );
    set_group_words( e, g_bad,
                     { 0, SHN_UNDEF, 999, ok->get_index(), ok->get_index(),
                       plain->get_index() } );

    // Same member claimed by two groups.
    section* g_p = e.sections.add( ".g.p" );
    g_p->set_type( SHT_GROUP );
    g_p->set_link( sym->get_index() );
    g_p->set_info( 1 );
    set_group_words( e, g_p, { GRP_COMDAT, same1->get_index() } );

    section* g_q = e.sections.add( ".g.q" );
    g_q->set_type( SHT_GROUP );
    g_q->set_link( sym->get_index() );
    g_q->set_info( 1 );
    set_group_words(
        e, g_q, { GRP_COMDAT, same1->get_index(), same2->get_index() } );

    // sh_link does not name a symbol table.
    section* g_badlink = e.sections.add( ".g.badlink" );
    g_badlink->set_type( SHT_GROUP );
    g_badlink->set_link( str->get_index() );
    g_badlink->set_info( 1 );
    set_group_words( e, g_badlink, { 0 } );

    // sh_info outside the symbol table.
    section* g_badinfo = e.sections.add( ".g.badinfo" );
    g_badinfo->set_type( SHT_GROUP );
    g_badinfo->set_link( sym->get_index() );
    g_badinfo->set_info( 77 );
    set_group_words( e, g_badinfo, { 0 } );

    // Symbol table whose own sh_link is not a string table.
    section* weird_sym = e.sections.add( ".symtab.weird" );
    weird_sym->set_type( SHT_SYMTAB );
    weird_sym->set_info( 1 );
    weird_sym->set_link( plain->get_index() );
    weird_sym->set_entry_size( e.get_class() == ELFCLASS32
                                   ? sizeof( Elf32_Sym )
                                   : sizeof( Elf64_Sym ) );

    section* g_badstr = e.sections.add( ".g.badstr" );
    g_badstr->set_type( SHT_GROUP );
    g_badstr->set_link( weird_sym->get_index() );
    g_badstr->set_info( 0 );
    set_group_words( e, g_badstr, { 0 } );

    return e;
}

//------------------------------------------------------------------------------
elfio build_closure_fixture( unsigned char cls, unsigned char enc )
{
    elfio e = make_rel_file( cls, enc );

    section* str = add_strtab( e, ".strtab" );
    section* sym = add_symtab( e, ".symtab", str );

    section* text_f = add_progbits( e, ".text.f", SHF_GROUP );
    section* text_g = add_progbits( e, ".text.g", 0 );

    // Relocation section inside the group whose sh_info reaches another
    // target outside the group.
    section* rela = e.sections.add( ".rela.text.f" );
    rela->set_type( SHT_RELA );
    rela->set_flags( SHF_GROUP );
    rela->set_link( sym->get_index() ); // plain sh_link, must not be followed
    rela->set_info( text_g->get_index() );
    rela->set_entry_size( e.get_class() == ELFCLASS32
                              ? sizeof( Elf32_Rela )
                              : sizeof( Elf64_Rela ) );
    rela->set_addr_align( e.get_class() == ELFCLASS32 ? 4 : 8 );

    symbol_section_accessor sa( e, sym );
    string_section_accessor  st( str );
    sa.add_symbol( st, "g", 0, 0, STB_GLOBAL, STT_FUNC, 0,
                   text_f->get_index() );

    section* group = e.sections.add( ".group" );
    group->set_type( SHT_GROUP );
    group->set_entry_size( 4 );
    group->set_addr_align( 4 );
    group->set_link( sym->get_index() );
    group->set_info( 1 );
    set_group_words(
        e, group,
        { GRP_COMDAT, text_f->get_index(), rela->get_index() } );

    // SHF_LINK_ORDER pointing at the group's code section.
    section* meta = add_progbits( e, ".meta.f", SHF_LINK_ORDER );
    meta->set_link( text_f->get_index() );

    // Legal link-order cycle.
    section* lo_a = add_progbits( e, ".lo.a", SHF_LINK_ORDER );
    section* lo_b = add_progbits( e, ".lo.b", SHF_LINK_ORDER );
    lo_a->set_link( lo_b->get_index() );
    lo_b->set_link( lo_a->get_index() );

    // Plain SHT_REL (not RELA) exercising the other type.
    section* data_h = add_progbits( e, ".data.h", 0 );
    section* rel    = e.sections.add( ".rel.data" );
    rel->set_type( SHT_REL );
    rel->set_link( sym->get_index() );
    rel->set_info( data_h->get_index() );
    rel->set_entry_size( e.get_class() == ELFCLASS32 ? sizeof( Elf32_Rel )
                                                     : sizeof( Elf64_Rel ) );
    rel->set_addr_align( e.get_class() == ELFCLASS32 ? 4 : 8 );

    return e;
}

const unsigned char all_classes[2]  = { ELFCLASS32, ELFCLASS64 };
const unsigned char all_encodings[2] = { ELFDATA2LSB, ELFDATA2MSB };

} // namespace

//------------------------------------------------------------------------------
int contract_groups()
{
    unsigned checks = 0;

    for ( unsigned char cls : all_classes ) {
        for ( unsigned char enc : all_encodings ) {
            // --- Identity fixture ------------------------------------------------
            {
                elfio built = build_identity_fixture( cls, enc );
                elfio e     = roundtrip( built );

                const group_view view = make_group_view( e );
                TASK_CHECK( view.valid() );
                TASK_CHECK( view.groups.size() == 3 );
                TASK_CHECK( view.dangling_group_sections.empty() );

                const group_description& g1 = view.groups[0];
                const group_description& g2 = view.groups[1];
                const group_description& g3 = view.groups[2];

                TASK_CHECK( g1.section_index ==
                            e.sections[ ".group1" ]->get_index() );
                TASK_CHECK( g2.section_index ==
                            e.sections[ ".group2" ]->get_index() );
                TASK_CHECK( g3.section_index ==
                            e.sections[ ".group3" ]->get_index() );

                // Raw flags are preserved; unknown bits stay unexplained
                // instead of collapsing the word to a boolean.
                TASK_CHECK( g1.flags == 0 && !g1.comdat &&
                            g1.flags_fully_interpreted );
                TASK_CHECK( g2.flags == ( GRP_COMDAT | 0x40000000u ) );
                TASK_CHECK( g2.comdat && !g2.flags_fully_interpreted );
                TASK_CHECK( g2.unexplained_flags.size() == 1 &&
                            g2.unexplained_flags[0] == 0x40000000u );
                TASK_CHECK( g3.flags == GRP_COMDAT && g3.comdat );

                // Members keep their original order and physical identity.
                TASK_CHECK( g1.members.size() == 2 );
                TASK_CHECK( g1.members[0] ==
                            e.sections[ ".text.a" ]->get_index() );
                TASK_CHECK( g1.members[1] ==
                            e.sections[ ".text.b" ]->get_index() );
                TASK_CHECK( g2.members.size() == 1 &&
                            g2.members[0] ==
                                e.sections[ ".rodata.c" ]->get_index() );

                // Signature triple: same symbol table name must not mix
                // the two tables, and the unnamed section symbol is fine.
                TASK_CHECK( g1.signature.resolved );
                TASK_CHECK( g1.signature.symbol_table ==
                            e.sections[ ".symtab" ]->get_index() );
                TASK_CHECK( g1.signature.symbol_index == 1 );
                TASK_CHECK( g1.signature.string_table ==
                            e.sections[ ".strtab1" ]->get_index() );
                TASK_CHECK( g1.signature.name.empty() );

                // sections[".symtab"] returns the first same-named table;
                // g2 must reference the second one by physical index.
                Elf_Half second_symtab = 0;
                for ( Elf_Half i = 0; i < e.sections.size(); ++i ) {
                    if ( e.sections[i]->get_type() == SHT_SYMTAB &&
                         e.sections[i]->get_link() ==
                             e.sections[ ".strtab2" ]->get_index() ) {
                        second_symtab = i;
                    }
                }
                TASK_CHECK( g2.signature.symbol_table == second_symtab );
                TASK_CHECK( g2.signature.symbol_index == 1 );
                TASK_CHECK( g2.signature.string_table ==
                            e.sections[ ".strtab2" ]->get_index() );
                TASK_CHECK( g2.signature.name == "foo" );

                // Equal signature text does not merge g2 and g3.
                TASK_CHECK( g3.signature.name == "foo" );
                TASK_CHECK( g3.signature.symbol_table !=
                            g2.signature.symbol_table );
                TASK_CHECK( g3.signature.symbol_index == 2 );
                TASK_CHECK( g3.section_index != g2.section_index );
                checks += 26;
            }

            // --- Problem fixture -------------------------------------------------
            {
                elfio built = build_problem_fixture( cls, enc );
                elfio e     = roundtrip( built );
                const group_view view = make_group_view( e );

                TASK_CHECK( !view.valid() );
                TASK_CHECK( view.dangling_group_sections.size() == 1 &&
                            view.dangling_group_sections[0] ==
                                e.sections[ ".dangle" ]->get_index() );

                const Elf_Half idx_empty =
                    e.sections[ ".g.empty" ]->get_index();
                const Elf_Half idx_malformed =
                    e.sections[ ".g.malformed" ]->get_index();
                const Elf_Half idx_bad =
                    e.sections[ ".g.bad" ]->get_index();
                const Elf_Half idx_p = e.sections[ ".g.p" ]->get_index();
                const Elf_Half idx_q = e.sections[ ".g.q" ]->get_index();
                const Elf_Half idx_badlink =
                    e.sections[ ".g.badlink" ]->get_index();
                const Elf_Half idx_badinfo =
                    e.sections[ ".g.badinfo" ]->get_index();
                const Elf_Half idx_badstr =
                    e.sections[ ".g.badstr" ]->get_index();

                TASK_CHECK( has_problem( view, group_problem_kind::empty_group,
                                         idx_empty, 4 ) );
                TASK_CHECK( has_problem(
                    view, group_problem_kind::malformed_data, idx_malformed,
                    Elf_Xword( -1 ) ) );
                TASK_CHECK( has_problem( view,
                                         group_problem_kind::zero_member,
                                         idx_bad, 4 ) );
                TASK_CHECK( has_problem(
                    view, group_problem_kind::member_out_of_range, idx_bad,
                    8 ) );
                TASK_CHECK( has_problem(
                    view, group_problem_kind::duplicate_member, idx_bad,
                    16 ) );
                TASK_CHECK( has_problem(
                    view, group_problem_kind::member_without_group_flag,
                    idx_bad, 20 ) );

                bool multi = false;
                for ( const auto& p : view.problems ) {
                    if ( p.kind ==
                             group_problem_kind::member_in_multiple_groups &&
                         p.group_section == idx_q &&
                         p.other_section == idx_p ) {
                        multi = true;
                    }
                }
                TASK_CHECK( multi );

                TASK_CHECK( has_problem(
                    view, group_problem_kind::invalid_symbol_table,
                    idx_badlink, Elf_Xword( -1 ) ) );
                TASK_CHECK( has_problem(
                    view, group_problem_kind::invalid_symbol_index,
                    idx_badinfo, Elf_Xword( -1 ) ) );
                TASK_CHECK( has_problem(
                    view, group_problem_kind::invalid_string_table,
                    idx_badstr, Elf_Xword( -1 ) ) );

                // Same-named sections keep distinct physical identities.
                TASK_CHECK( e.sections[ ".text.same" ]->get_index() !=
                            idx_p ); // first match is still a real section
                unsigned same_name_count = 0;
                for ( Elf_Half i = 0; i < e.sections.size(); ++i ) {
                    if ( e.sections[i]->get_name() == ".text.same" ) {
                        ++same_name_count;
                    }
                }
                TASK_CHECK( same_name_count == 2 );
                checks += 16;
            }
        }
    }

    return static_cast<int>( checks );
}

//------------------------------------------------------------------------------
int contract_closure()
{
    unsigned checks = 0;

    for ( unsigned char cls : all_classes ) {
        for ( unsigned char enc : all_encodings ) {
            elfio built = build_closure_fixture( cls, enc );
            elfio e     = roundtrip( built );

            const Elf_Half text_f = e.sections[ ".text.f" ]->get_index();
            const Elf_Half text_g = e.sections[ ".text.g" ]->get_index();
            const Elf_Half rela   = e.sections[ ".rela.text.f" ]->get_index();
            const Elf_Half group  = e.sections[ ".group" ]->get_index();
            const Elf_Half meta   = e.sections[ ".meta.f" ]->get_index();
            const Elf_Half lo_a   = e.sections[ ".lo.a" ]->get_index();
            const Elf_Half lo_b   = e.sections[ ".lo.b" ]->get_index();
            const Elf_Half data_h = e.sections[ ".data.h" ]->get_index();
            const Elf_Half rel    = e.sections[ ".rel.data" ]->get_index();
            const Elf_Half strtab = e.sections[ ".strtab" ]->get_index();
            const Elf_Half symtab = e.sections[ ".symtab" ]->get_index();

            // Selecting a member expands the whole group, and the group's
            // relocation section drags its external target in. The link
            // order section follows as well; plain sh_link does not.
            section_relation_view r =
                make_section_relations( e, { text_f } );
            TASK_CHECK( r.valid );
            std::set<Elf_Half> got = index_set( r.sections );
            TASK_CHECK( got.count( text_f ) && got.count( group ) &&
                        got.count( rela ) && got.count( text_g ) &&
                        got.count( meta ) );
            TASK_CHECK( !got.count( strtab ) && !got.count( symtab ) &&
                        !got.count( lo_a ) && !got.count( lo_b ) &&
                        !got.count( data_h ) && !got.count( rel ) );
            TASK_CHECK( std::is_sorted( r.sections.begin(),
                                        r.sections.end() ) );

            // Selecting the group itself gives the identical closure.
            section_relation_view from_group =
                make_section_relations( e, { group } );
            TASK_CHECK( from_group.valid &&
                        index_set( from_group.sections ) == got );

            // Selecting the relocation section reaches back into the group
            // and forward to its sh_info target.
            section_relation_view from_rela =
                make_section_relations( e, { rela } );
            TASK_CHECK( from_rela.valid );
            std::set<Elf_Half> got_rela = index_set( from_rela.sections );
            TASK_CHECK( got_rela.count( text_g ) &&
                        got_rela.count( group ) &&
                        got_rela.count( text_f ) );

            // Reverse direction: target pulls the relocation section, which
            // then unlocks the group.
            section_relation_view from_target =
                make_section_relations( e, { text_g } );
            TASK_CHECK( from_target.valid &&
                        index_set( from_target.sections ) == got );

            // Link-order cycle converges and stays isolated.
            section_relation_view cycle =
                make_section_relations( e, { lo_a } );
            TASK_CHECK( cycle.valid );
            TASK_CHECK( index_set( cycle.sections ) ==
                        std::set<Elf_Half>( { lo_a, lo_b } ) );

            // SHT_REL works the same as SHT_RELA, both directions.
            section_relation_view rel_forward =
                make_section_relations( e, { rel } );
            TASK_CHECK( rel_forward.valid &&
                        index_set( rel_forward.sections ) ==
                            std::set<Elf_Half>( { rel, data_h } ) );
            section_relation_view rel_reverse =
                make_section_relations( e, { data_h } );
            TASK_CHECK( rel_reverse.valid &&
                        index_set( rel_reverse.sections ) ==
                            std::set<Elf_Half>( { rel, data_h } ) );

            // Request normalisation: sorted, unique, preserved.
            section_relation_view normalised =
                make_section_relations( e, { text_g, text_f, text_f } );
            TASK_CHECK( normalised.valid );
            TASK_CHECK( normalised.requested.size() == 2 &&
                        normalised.requested[0] == text_f &&
                        normalised.requested[1] == text_g );

            // Invalid input: no seemingly-complete partial answer.
            section_relation_view broken =
                make_section_relations( e, { text_f, 1000 } );
            TASK_CHECK( !broken.valid && !broken.errors.empty() );
            TASK_CHECK( broken.sections.empty() &&
                        broken.relations.empty() &&
                        broken.requested.empty() );

            // Every direct edge survives, even where several paths meet.
            TASK_CHECK( r.relations.size() == 4 );
            TASK_CHECK( has_relation( r, relation_kind::group_contains,
                                      group, text_f ) );
            TASK_CHECK( has_relation( r, relation_kind::group_contains,
                                      group, rela ) );
            TASK_CHECK( has_relation( r, relation_kind::relocation_target,
                                      rela, text_g ) );
            TASK_CHECK( has_relation( r, relation_kind::link_order, meta,
                                      text_f ) );
            TASK_CHECK( std::is_sorted( r.sections.begin(),
                                        r.sections.end() ) );
            checks += 19;
        }
    }

    return static_cast<int>( checks );
}

//------------------------------------------------------------------------------
int contract_views()
{
    unsigned checks = 0;

    for ( unsigned char cls : all_classes ) {
        for ( unsigned char enc : all_encodings ) {
            elfio built = build_identity_fixture( cls, enc );

            // Snapshot before any analysis; the view is read-only.
            std::stringstream before;
            TASK_CHECK( built.save( before ) );
            const std::string before_bytes = before.str();

            elfio e = roundtrip( built );

            const group_view view = make_group_view( e );
            const Elf_Half g1 = view.groups[0].section_index;
            const Elf_Half g2 = view.groups[1].section_index;

            const section_relation_view relations =
                make_section_relations( e, { g1 } );
            TASK_CHECK( relations.valid );

            std::stringstream out;
            dump::section_relations( out, e, { g1 } );
            const std::string text = out.str();

            TASK_CHECK( text.find( "Section Relations:" ) !=
                        std::string::npos );
            TASK_CHECK( text.find( "Section Groups:" ) !=
                        std::string::npos );
            TASK_CHECK( text.find( "signature: symbol_table=" ) !=
                        std::string::npos );
            TASK_CHECK( count_occurrences( text, "Relation:" ) ==
                        relations.relations.size() );
            TASK_CHECK( count_occurrences( text, "member[" ) == 2 );

            // The scoped dump carries g1's own identity; unrelated groups
            // stay out of the closure view.
            auto group_marker = []( Elf_Half index ) {
                std::ostringstream m;
                m << "Group [" << std::setw( 5 ) << index << "]";
                return m.str();
            };
            const std::string own_marker   = group_marker( g1 );
            const std::string other_marker = group_marker( g2 );
            TASK_CHECK( text.find( own_marker ) != std::string::npos );
            TASK_CHECK( text.find( other_marker ) == std::string::npos );

            // Full-file dump and the existing section header dump keep
            // working with their established wording, and expose every
            // group identity including g2's unexplained flag bits.
            std::stringstream full;
            dump::section_relations( full, e );
            const std::string full_text = full.str();
            TASK_CHECK( count_occurrences( full_text, "Relation:" ) >=
                        relations.relations.size() );
            TASK_CHECK( full_text.find( other_marker ) !=
                        std::string::npos );
            TASK_CHECK( full_text.find( "flags=0x40000001" ) !=
                        std::string::npos );
            TASK_CHECK( full_text.find( "flags_interpreted=0" ) !=
                        std::string::npos );

            std::stringstream headers;
            dump::section_headers( headers, e );
            const std::string header_text = headers.str();
            TASK_CHECK( header_text.find( "GROUP" ) != std::string::npos );
            TASK_CHECK( header_text.find( ".text.a" ) !=
                        std::string::npos );
            TASK_CHECK( header_text.find( "Section Headers:" ) !=
                        std::string::npos );

            // Full-file problem output locates each fault and the dangling
            // SHF_GROUP section.
            elfio problems_built = build_problem_fixture( cls, enc );
            elfio problems       = roundtrip( problems_built );
            std::stringstream problem_out;
            dump::section_relations( problem_out, problems );
            const std::string problem_text = problem_out.str();
            TASK_CHECK( problem_text.find( "problem:" ) !=
                        std::string::npos );
            TASK_CHECK( problem_text.find( "Dangling SHF_GROUP" ) !=
                        std::string::npos );

            // Re-saving the analysed objects must reproduce the snapshot.
            std::stringstream after;
            TASK_CHECK( built.save( after ) );
            TASK_CHECK( after.str() == before_bytes );

            std::stringstream after_loaded;
            TASK_CHECK( e.save( after_loaded ) );
            checks += 17;
        }
    }

    return static_cast<int>( checks );
}
