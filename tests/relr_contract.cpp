// Contract tests for the read-only RELR analysis interface
// (elfio/elfio_relr.hpp) and dump::relr_tables.
//
// The reference expectations below are computed from hand-written byte
// patterns, independently of the library implementation.

#include <elfio/elfio.hpp>
#include <elfio/elfio_dump.hpp>

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace ELFIO;

static int failures = 0;

#define CHECK( cond )                                              \
    do {                                                           \
        if ( !( cond ) ) {                                         \
            std::cerr << "CHECK failed at line " << __LINE__       \
                      << ": " #cond << std::endl;                  \
            ++failures;                                            \
        }                                                          \
    } while ( 0 )

//------------------------------------------------------------------------------
// Independent reference decoder working on the raw entry values
struct RefLoc
{
    uint64_t addr;
    size_t   entry;
    uint64_t value;
    bool     bitmap;
    unsigned bit;
};

static std::vector<RefLoc> ref_decode( const std::vector<uint64_t>& words,
                                       unsigned                     word_bits )
{
    std::vector<RefLoc> out;
    const uint64_t      mask = word_bits == 64 ? ~0ull : 0xffffffffull;
    const uint64_t      w    = word_bits / 8;
    uint64_t            where = 0;
    for ( size_t i = 0; i < words.size(); ++i ) {
        const uint64_t v = words[i];
        if ( ( v & 1 ) == 0 ) {
            out.push_back( { v, i, v, false, 0 } );
            where = ( v + w ) & mask;
        }
        else {
            for ( unsigned b = 1; b < word_bits; ++b ) {
                if ( v & ( 1ull << b ) ) {
                    out.push_back(
                        { where + (uint64_t)( b - 1 ) * w, i, v, true, b } );
                }
            }
            where = ( where + (uint64_t)( word_bits - 1 ) * w ) & mask;
        }
    }
    return out;
}

//------------------------------------------------------------------------------
// Hand-written byte helpers (independent of the library endianness handling)
static void put_word( std::vector<char>& buf,
                      size_t             off,
                      uint64_t           value,
                      unsigned           word_size,
                      unsigned char      enc )
{
    for ( unsigned i = 0; i < word_size; ++i ) {
        const unsigned shift =
            ( enc == ELFDATA2LSB ) ? i * 8 : ( word_size - 1 - i ) * 8;
        buf[off + i] = static_cast<char>( ( value >> shift ) & 0xff );
    }
}

static uint64_t get_word( const std::vector<char>& buf,
                          size_t                   off,
                          unsigned                 word_size,
                          unsigned char            enc )
{
    uint64_t v = 0;
    for ( unsigned i = 0; i < word_size; ++i ) {
        const unsigned shift =
            ( enc == ELFDATA2LSB ) ? i * 8 : ( word_size - 1 - i ) * 8;
        v |= (uint64_t)(unsigned char)buf[off + i] << shift;
    }
    return v;
}

static std::string hex0x( uint64_t v, int width )
{
    std::ostringstream o;
    o << "0x" << std::setw( width ) << std::setfill( '0' ) << std::hex << v;
    return o.str();
}

//------------------------------------------------------------------------------
// Build an image with a .relr.dyn section holding the given raw words,
// save it, reload it and return the reloaded reader.
static elfio make_image( unsigned char                cls,
                         unsigned char                enc,
                         Elf_Half                     type,
                         const std::vector<uint64_t>& relr_words )
{
    elfio writer;
    writer.create( cls, enc );
    writer.set_type( type );
    writer.set_machine( EM_NONE );

    section* relr = writer.sections.add( ".relr.dyn" );
    relr->set_type( SHT_RELR );
    relr->set_addr_align( cls == ELFCLASS64 ? 8 : 4 );
    relr->set_entry_size( cls == ELFCLASS64 ? 8 : 4 );
    if ( cls == ELFCLASS64 ) {
        array_section_accessor<Elf_Xword> raw( writer, relr );
        for ( uint64_t v : relr_words )
            raw.add_entry( v );
    }
    else {
        array_section_accessor<Elf32_Word> raw( writer, relr );
        for ( uint64_t v : relr_words )
            raw.add_entry( v );
    }

    std::stringstream bytes( std::ios::in | std::ios::out | std::ios::binary );
    CHECK( writer.save( bytes ) );

    elfio reader;
    bytes.seekg( 0 );
    CHECK( reader.load( bytes ) );
    return reader;
}

static section* add_progbits( elfio&               writer,
                              const char*          name,
                              Elf64_Addr           address,
                              const std::vector<char>& content )
{
    section* sec = writer.sections.add( name );
    sec->set_type( SHT_PROGBITS );
    sec->set_flags( SHF_ALLOC | SHF_WRITE );
    sec->set_address( address );
    sec->set_addr_align( 1 );
    sec->set_data( content.data(), content.size() );
    return sec;
}

//------------------------------------------------------------------------------
// Compare the library decode against the hand-written reference
static void check_decode( const elfio&                 reader,
                          const std::vector<uint64_t>& words,
                          unsigned                     word_bits )
{
    const section* sec = reader.sections[".relr.dyn"];
    CHECK( sec != nullptr );
    if ( sec == nullptr )
        return;

    const_relr_section_accessor relr( reader, sec );
    std::vector<relr_entry>     entries;
    relr_error                  error;
    CHECK( relr.decode( entries, error ) );
    CHECK( error.status == relr_status::ok );

    const std::vector<RefLoc> ref = ref_decode( words, word_bits );
    CHECK( entries.size() == ref.size() );
    const size_t n = entries.size() < ref.size() ? entries.size() : ref.size();
    for ( size_t i = 0; i < n; ++i ) {
        CHECK( entries[i].address == ref[i].addr );
        CHECK( entries[i].entry_index == ref[i].entry );
        CHECK( entries[i].entry_value == ref[i].value );
        CHECK( entries[i].from_bitmap == ref[i].bitmap );
        CHECK( entries[i].bitmap_bit == ref[i].bit );
    }
}

//------------------------------------------------------------------------------
static void group_decode_4combo()
{
    const unsigned char classes[] = { ELFCLASS32, ELFCLASS32, ELFCLASS64,
                                      ELFCLASS64 };
    const unsigned char encs[] = { ELFDATA2LSB, ELFDATA2MSB, ELFDATA2LSB,
                                   ELFDATA2MSB };
    for ( int k = 0; k < 4; ++k ) {
        const std::vector<uint64_t> words = { 0x1000, 0xb };
        elfio reader = make_image( classes[k], encs[k], ET_DYN, words );

        const section* sec = reader.sections[".relr.dyn"];
        CHECK( sec != nullptr );
        const_relr_section_accessor relr( reader, sec );

        // Raw entries are still readable as words
        Elf_Xword raw = 0;
        CHECK( relr.get_entries_num() == 2 );
        CHECK( relr.get_entry( 0, raw ) && raw == 0x1000 );
        CHECK( relr.get_entry( 1, raw ) && raw == 0xb );

        check_decode( reader, words, classes[k] == ELFCLASS64 ? 64 : 32 );

        // The legacy REL/RELA accessor keeps its behavior on SHT_RELR
        const_relocation_section_accessor legacy( reader, sec );
        Elf64_Addr                        offset = 0;
        Elf_Word                          symbol = 0;
        unsigned                          type   = 0;
        Elf_Sxword                        addend = 0;
        CHECK( !legacy.get_entry( 0, offset, symbol, type, addend ) );
    }
}

//------------------------------------------------------------------------------
static void group_empty_bitmap_run()
{
    const unsigned char classes[] = { ELFCLASS32, ELFCLASS32, ELFCLASS64,
                                      ELFCLASS64 };
    const unsigned char encs[] = { ELFDATA2LSB, ELFDATA2MSB, ELFDATA2LSB,
                                   ELFDATA2MSB };
    for ( int k = 0; k < 4; ++k ) {
        // Two consecutive bitmaps carrying only the flag bit still advance
        // the full coverage range each time
        const std::vector<uint64_t> words = { 0x2000, 0x1, 0x1, 0x3 };
        elfio reader = make_image( classes[k], encs[k], ET_DYN, words );
        check_decode( reader, words, classes[k] == ELFCLASS64 ? 64 : 32 );

        const section* sec = reader.sections[".relr.dyn"];
        const_relr_section_accessor relr( reader, sec );
        std::vector<relr_entry>     entries;
        relr_error                  error;
        CHECK( relr.decode( entries, error ) );
        CHECK( entries.size() == 2 );
        if ( entries.size() == 2 ) {
            const uint64_t w = classes[k] == ELFCLASS64 ? 8 : 4;
            const uint64_t bits = w * 8;
            CHECK( entries[0].address == 0x2000 && !entries[0].from_bitmap );
            CHECK( entries[1].address ==
                   0x2000 + w + 2 * ( bits - 1 ) * w );
            CHECK( entries[1].from_bitmap && entries[1].bitmap_bit == 1 );
            CHECK( entries[1].entry_index == 3 );
        }
    }
}

//------------------------------------------------------------------------------
static void group_msb_bitmap()
{
    const unsigned char classes[] = { ELFCLASS32, ELFCLASS32, ELFCLASS64,
                                      ELFCLASS64 };
    const unsigned char encs[] = { ELFDATA2LSB, ELFDATA2MSB, ELFDATA2LSB,
                                   ELFDATA2MSB };
    for ( int k = 0; k < 4; ++k ) {
        const uint64_t top = classes[k] == ELFCLASS64
                                 ? 0x8000000000000001ull
                                 : 0x80000001ull;
        const std::vector<uint64_t> words = { 0x4000, top };
        elfio reader = make_image( classes[k], encs[k], ET_DYN, words );
        check_decode( reader, words, classes[k] == ELFCLASS64 ? 64 : 32 );

        const section* sec = reader.sections[".relr.dyn"];
        const_relr_section_accessor relr( reader, sec );
        std::vector<relr_entry>     entries;
        relr_error                  error;
        CHECK( relr.decode( entries, error ) );
        CHECK( entries.size() == 2 );
        if ( entries.size() == 2 ) {
            const uint64_t w    = classes[k] == ELFCLASS64 ? 8 : 4;
            const uint64_t bits = w * 8;
            CHECK( entries[1].address == 0x4000 + w + ( bits - 2 ) * w );
            CHECK( entries[1].bitmap_bit == bits - 1 );
        }
    }
}

//------------------------------------------------------------------------------
static void group_unaligned_location()
{
    const unsigned char classes[] = { ELFCLASS32, ELFCLASS32, ELFCLASS64,
                                      ELFCLASS64 };
    const unsigned char encs[] = { ELFDATA2LSB, ELFDATA2MSB, ELFDATA2LSB,
                                   ELFDATA2MSB };
    for ( int k = 0; k < 4; ++k ) {
        // 0x1002 is even but not word aligned: the format only excludes
        // odd addresses
        const std::vector<uint64_t> words = { 0x1002, 0x3 };
        elfio reader = make_image( classes[k], encs[k], ET_DYN, words );
        check_decode( reader, words, classes[k] == ELFCLASS64 ? 64 : 32 );

        const section* sec = reader.sections[".relr.dyn"];
        const_relr_section_accessor relr( reader, sec );
        std::vector<relr_entry>     entries;
        relr_error                  error;
        CHECK( relr.decode( entries, error ) );
        CHECK( entries.size() == 2 );
        if ( entries.size() == 2 ) {
            const uint64_t w = classes[k] == ELFCLASS64 ? 8 : 4;
            CHECK( entries[0].address == 0x1002 );
            CHECK( entries[1].address == 0x1002 + w );
        }
    }
}

//------------------------------------------------------------------------------
static void group_nobits_preview()
{
    const unsigned char classes[] = { ELFCLASS32, ELFCLASS64 };
    const unsigned char encs[]    = { ELFDATA2LSB, ELFDATA2MSB };
    for ( int k = 0; k < 2; ++k ) {
        const unsigned w = classes[k] == ELFCLASS64 ? 8 : 4;

        elfio writer;
        writer.create( classes[k], encs[k] );
        writer.set_type( ET_DYN );
        writer.set_machine( EM_NONE );

        section* relr_sec = writer.sections.add( ".relr.dyn" );
        relr_sec->set_type( SHT_RELR );
        relr_sec->set_addr_align( w );
        relr_sec->set_entry_size( w );
        if ( w == 8 ) {
            array_section_accessor<Elf_Xword> raw( writer, relr_sec );
            raw.add_entry( 0x5000 );
            raw.add_entry( 0x7 ); // bits 1 and 2
        }
        else {
            array_section_accessor<Elf32_Word> raw( writer, relr_sec );
            raw.add_entry( 0x5000 );
            raw.add_entry( 0x7 );
        }

        section* bss = writer.sections.add( ".bss" );
        bss->set_type( SHT_NOBITS );
        bss->set_flags( SHF_ALLOC | SHF_WRITE );
        bss->set_address( 0x5000 );
        bss->set_addr_align( w );
        bss->set_size( 4 * w );

        std::stringstream bytes( std::ios::in | std::ios::out |
                                 std::ios::binary );
        CHECK( writer.save( bytes ) );
        elfio reader;
        bytes.seekg( 0 );
        CHECK( reader.load( bytes ) );

        const section* sec = reader.sections[".relr.dyn"];
        const_relr_section_accessor relr( reader, sec );
        std::vector<relr_entry>     entries;
        relr_error                  error;
        CHECK( relr.decode( entries, error ) );
        CHECK( entries.size() == 3 );

        const Elf_Half bss_index = reader.sections[".bss"]->get_index();
        const uint64_t mask = w == 8 ? ~0ull : 0xffffffffull;
        for ( size_t i = 0; i < entries.size(); ++i ) {
            relr_preview pv;
            CHECK( relr.preview( entries[i].address, 0x10000, pv ) );
            CHECK( pv.status == relr_preview_status::ok );
            CHECK( pv.section_index == bss_index );
            CHECK( pv.section_offset == entries[i].address - 0x5000 );
            CHECK( pv.initial_value == 0 );
            CHECK( pv.relocated_value == ( 0x10000 & mask ) );
        }
    }
}

//------------------------------------------------------------------------------
static void group_overlap_and_truncated()
{
    // Overlapping SHF_ALLOC PROGBITS sections both fully contain the word
    {
        elfio writer;
        writer.create( ELFCLASS64, ELFDATA2LSB );
        writer.set_type( ET_DYN );
        writer.set_machine( EM_NONE );

        section* relr_sec = writer.sections.add( ".relr.dyn" );
        relr_sec->set_type( SHT_RELR );
        relr_sec->set_addr_align( 8 );
        relr_sec->set_entry_size( 8 );
        array_section_accessor<Elf_Xword> raw( writer, relr_sec );
        raw.add_entry( 0x6000 );

        std::vector<char> content( 8, 0 );
        put_word( content, 0, 0x1111, 8, ELFDATA2LSB );
        add_progbits( writer, ".data1", 0x6000, content );
        add_progbits( writer, ".data2", 0x6000, content );

        std::stringstream bytes( std::ios::in | std::ios::out |
                                 std::ios::binary );
        CHECK( writer.save( bytes ) );
        elfio reader;
        bytes.seekg( 0 );
        CHECK( reader.load( bytes ) );

        const_relr_section_accessor relr( reader,
                                          reader.sections[".relr.dyn"] );
        relr_preview pv;
        CHECK( !relr.preview( 0x6000, 0, pv ) );
        CHECK( pv.status == relr_preview_status::overlapping_owners );
    }

    // A PROGBITS section whose data is actually truncated in the file
    for ( int k = 0; k < 2; ++k ) {
        const unsigned char cls  = k == 0 ? ELFCLASS32 : ELFCLASS64;
        const unsigned char enc  = k == 0 ? ELFDATA2LSB : ELFDATA2MSB;
        const unsigned      w    = cls == ELFCLASS64 ? 8 : 4;

        elfio writer;
        writer.create( cls, enc );
        writer.set_type( ET_DYN );
        writer.set_machine( EM_NONE );

        section* relr_sec = writer.sections.add( ".relr.dyn" );
        relr_sec->set_type( SHT_RELR );
        relr_sec->set_addr_align( w );
        relr_sec->set_entry_size( w );
        if ( w == 8 ) {
            array_section_accessor<Elf_Xword> raw( writer, relr_sec );
            raw.add_entry( 0x7000 );
        }
        else {
            array_section_accessor<Elf32_Word> raw( writer, relr_sec );
            raw.add_entry( 0x7000 );
        }

        std::vector<char> content( w, 0 );
        put_word( content, 0, 0x2222, w, enc );
        add_progbits( writer, ".data", 0x7000, content );

        std::stringstream bytes( std::ios::in | std::ios::out |
                                 std::ios::binary );
        CHECK( writer.save( bytes ) );
        std::string image = bytes.str();

        // Corrupt sh_size of .data so its data extends beyond the file
        elfio probe;
        std::stringstream probe_stream( image );
        CHECK( probe.load( probe_stream ) );
        const Elf_Half  idx     = probe.sections[".data"]->get_index();
        const Elf64_Off shoff   = probe.get_sections_offset();
        const Elf_Half  shentsz = probe.get_section_entry_size();
        const size_t    size_field =
            (size_t)shoff + (size_t)idx * shentsz + ( cls == ELFCLASS64 ? 32 : 20 );
        const uint64_t huge = 0x100000;
        for ( unsigned i = 0; i < w; ++i ) {
            const unsigned shift =
                ( enc == ELFDATA2LSB ) ? i * 8 : ( w - 1 - i ) * 8;
            image[size_field + i] =
                static_cast<char>( ( huge >> shift ) & 0xff );
        }

        elfio reader;
        std::stringstream corrupted( image );
        CHECK( reader.load( corrupted ) );

        const_relr_section_accessor relr( reader,
                                          reader.sections[".relr.dyn"] );
        relr_preview pv;
        CHECK( !relr.preview( 0x7000, 0, pv ) );
        CHECK( pv.status == relr_preview_status::truncated_data );
    }

    // A word straddling two adjacent sections must not be spliced
    {
        elfio writer;
        writer.create( ELFCLASS32, ELFDATA2LSB );
        writer.set_type( ET_DYN );
        writer.set_machine( EM_NONE );

        section* relr_sec = writer.sections.add( ".relr.dyn" );
        relr_sec->set_type( SHT_RELR );
        relr_sec->set_addr_align( 4 );
        relr_sec->set_entry_size( 4 );
        array_section_accessor<Elf32_Word> raw( writer, relr_sec );
        raw.add_entry( 0x8002 );

        std::vector<char> content( 4, 0x5A );
        add_progbits( writer, ".a", 0x8000, content );
        add_progbits( writer, ".b", 0x8004, content );

        std::stringstream bytes( std::ios::in | std::ios::out |
                                 std::ios::binary );
        CHECK( writer.save( bytes ) );
        elfio reader;
        bytes.seekg( 0 );
        CHECK( reader.load( bytes ) );

        const_relr_section_accessor relr( reader,
                                          reader.sections[".relr.dyn"] );
        relr_preview pv;
        CHECK( !relr.preview( 0x8002, 0, pv ) );
        CHECK( pv.status == relr_preview_status::no_owner );
        CHECK( !relr.preview( 0x9000, 0, pv ) );
        CHECK( pv.status == relr_preview_status::no_owner );
    }
}

//------------------------------------------------------------------------------
static void group_encoding_overflow()
{
    // The first entry is a bitmap
    {
        elfio reader = make_image( ELFCLASS64, ELFDATA2LSB, ET_DYN, { 0x3 } );
        const_relr_section_accessor relr( reader,
                                          reader.sections[".relr.dyn"] );
        std::vector<relr_entry> entries;
        relr_error              error;
        CHECK( !relr.decode( entries, error ) );
        CHECK( error.status == relr_status::bitmap_without_base );
        CHECK( error.entry_index == 0 );
    }

    // sh_entsize differs from the file word size
    {
        elfio writer;
        writer.create( ELFCLASS64, ELFDATA2LSB );
        writer.set_type( ET_DYN );
        writer.set_machine( EM_NONE );
        section* relr_sec = writer.sections.add( ".relr.dyn" );
        relr_sec->set_type( SHT_RELR );
        relr_sec->set_addr_align( 8 );
        relr_sec->set_entry_size( 16 );
        array_section_accessor<Elf_Xword> raw( writer, relr_sec );
        raw.add_entry( 0x1000 );
        std::stringstream bytes( std::ios::in | std::ios::out |
                                 std::ios::binary );
        CHECK( writer.save( bytes ) );
        elfio reader;
        bytes.seekg( 0 );
        CHECK( reader.load( bytes ) );

        const_relr_section_accessor relr( reader,
                                          reader.sections[".relr.dyn"] );
        std::vector<relr_entry> entries;
        relr_error              error;
        CHECK( !relr.decode( entries, error ) );
        CHECK( error.status == relr_status::bad_entry_size );
    }

    // Trailing bytes smaller than one entry
    {
        elfio writer;
        writer.create( ELFCLASS32, ELFDATA2LSB );
        writer.set_type( ET_DYN );
        writer.set_machine( EM_NONE );
        section* relr_sec = writer.sections.add( ".relr.dyn" );
        relr_sec->set_type( SHT_RELR );
        relr_sec->set_addr_align( 4 );
        relr_sec->set_entry_size( 4 );
        array_section_accessor<Elf32_Word> raw( writer, relr_sec );
        raw.add_entry( 0x1000 );
        relr_sec->append_data( "xy", 2 );
        std::stringstream bytes( std::ios::in | std::ios::out |
                                 std::ios::binary );
        CHECK( writer.save( bytes ) );
        elfio reader;
        bytes.seekg( 0 );
        CHECK( reader.load( bytes ) );

        const_relr_section_accessor relr( reader,
                                          reader.sections[".relr.dyn"] );
        std::vector<relr_entry> entries;
        relr_error              error;
        CHECK( !relr.decode( entries, error ) );
        CHECK( error.status == relr_status::trailing_entry );
        CHECK( error.entry_index == 1 );
        CHECK( entries.size() == 1 && entries[0].address == 0x1000 );
    }

    // Position after an address entry overflows the 32-bit word width
    {
        elfio reader =
            make_image( ELFCLASS32, ELFDATA2LSB, ET_DYN, { 0xFFFFFFFC } );
        const_relr_section_accessor relr( reader,
                                          reader.sections[".relr.dyn"] );
        std::vector<relr_entry> entries;
        relr_error              error;
        CHECK( !relr.decode( entries, error ) );
        CHECK( error.status == relr_status::address_overflow );
        CHECK( error.entry_index == 0 );
        CHECK( entries.size() == 1 && entries[0].address == 0xFFFFFFFC );
    }

    // Position after an address entry overflows the 64-bit word width
    {
        elfio reader = make_image( ELFCLASS64, ELFDATA2MSB, ET_DYN,
                                   { 0xFFFFFFFFFFFFFFF8ull } );
        const_relr_section_accessor relr( reader,
                                          reader.sections[".relr.dyn"] );
        std::vector<relr_entry> entries;
        relr_error              error;
        CHECK( !relr.decode( entries, error ) );
        CHECK( error.status == relr_status::address_overflow );
        CHECK( error.entry_index == 0 );
    }

    // Bitmap coverage range overflows the 32-bit word width
    {
        elfio reader = make_image( ELFCLASS32, ELFDATA2MSB, ET_DYN,
                                   { 0xFFFFFF80, 0x1 } );
        const_relr_section_accessor relr( reader,
                                          reader.sections[".relr.dyn"] );
        std::vector<relr_entry> entries;
        relr_error              error;
        CHECK( !relr.decode( entries, error ) );
        CHECK( error.status == relr_status::range_overflow );
        CHECK( error.entry_index == 1 );
        CHECK( entries.size() == 1 && entries[0].address == 0xFFFFFF80 );
    }

    // Bitmap coverage range overflows the 64-bit word width
    {
        elfio reader = make_image( ELFCLASS64, ELFDATA2LSB, ET_DYN,
                                   { 0xFFFFFFFFFFFFFF00ull, 0x1 } );
        const_relr_section_accessor relr( reader,
                                          reader.sections[".relr.dyn"] );
        std::vector<relr_entry> entries;
        relr_error              error;
        CHECK( !relr.decode( entries, error ) );
        CHECK( error.status == relr_status::range_overflow );
        CHECK( error.entry_index == 1 );
    }
}

//------------------------------------------------------------------------------
static void group_preview_progbits()
{
    const unsigned char classes[] = { ELFCLASS32, ELFCLASS32, ELFCLASS64,
                                      ELFCLASS64 };
    const unsigned char encs[] = { ELFDATA2LSB, ELFDATA2MSB, ELFDATA2LSB,
                                   ELFDATA2MSB };
    const uint64_t stored[] = { 0x11223344, 0x55667788, 0x99AABBCC,
                                0xDDEEFF00, 0x13579BDF, 0x2468ACE0 };
    const uint64_t bias = 0x100000;

    for ( int k = 0; k < 4; ++k ) {
        const unsigned w      = classes[k] == ELFCLASS64 ? 8 : 4;
        const uint64_t mask   = w == 8 ? ~0ull : 0xffffffffull;
        const uint64_t base   = 0x1000;
        const unsigned nwords = 4;

        elfio writer;
        writer.create( classes[k], encs[k] );
        writer.set_type( ET_DYN );
        writer.set_machine( EM_NONE );

        section* relr_sec = writer.sections.add( ".relr.dyn" );
        relr_sec->set_type( SHT_RELR );
        relr_sec->set_addr_align( w );
        relr_sec->set_entry_size( w );
        if ( w == 8 ) {
            array_section_accessor<Elf_Xword> raw( writer, relr_sec );
            raw.add_entry( base );
            raw.add_entry( 0x1b ); // bits 1, 3 and 4
        }
        else {
            array_section_accessor<Elf32_Word> raw( writer, relr_sec );
            raw.add_entry( base );
            raw.add_entry( 0x1b );
        }

        std::vector<char> content( nwords * w, 0 );
        for ( unsigned i = 0; i < nwords; ++i )
            put_word( content, i * w, stored[i], w, encs[k] );
        add_progbits( writer, ".data", base, content );

        std::stringstream bytes( std::ios::in | std::ios::out |
                                 std::ios::binary );
        CHECK( writer.save( bytes ) );
        elfio reader;
        bytes.seekg( 0 );
        CHECK( reader.load( bytes ) );

        const section* sec = reader.sections[".relr.dyn"];
        const_relr_section_accessor relr( reader, sec );
        std::vector<relr_entry>     entries;
        relr_error                  error;
        CHECK( relr.decode( entries, error ) );
        CHECK( entries.size() == 4 );

        const Elf_Half data_index = reader.sections[".data"]->get_index();
        for ( size_t i = 0; i < entries.size(); ++i ) {
            relr_preview pv;
            const uint64_t addr = entries[i].address;
            if ( addr + w <= base + nwords * w ) {
                const uint64_t want =
                    get_word( content, addr - base, w, encs[k] );
                CHECK( relr.preview( addr, bias, pv ) );
                CHECK( pv.status == relr_preview_status::ok );
                CHECK( pv.section_index == data_index );
                CHECK( pv.section_offset == addr - base );
                CHECK( pv.initial_value == want );
                CHECK( pv.relocated_value == ( ( want + bias ) & mask ) );
            }
            else {
                // The last bitmap bit lands past the end of .data; this
                // single unreadable location must not mask the others
                CHECK( !relr.preview( addr, bias, pv ) );
                CHECK( pv.status == relr_preview_status::no_owner );
            }
        }
    }

    // ET_EXEC is previewable as well, ET_REL is not
    {
        elfio exec = make_image( ELFCLASS64, ELFDATA2LSB, ET_EXEC,
                                 { 0x1000, 0xb } );
        const_relr_section_accessor relr( exec,
                                          exec.sections[".relr.dyn"] );
        relr_preview pv;
        CHECK( !relr.preview( 0x1000, 0, pv ) );
        CHECK( pv.status == relr_preview_status::no_owner );

        elfio rel = make_image( ELFCLASS64, ELFDATA2LSB, ET_REL,
                                { 0x1000, 0xb } );
        const_relr_section_accessor relr2( rel,
                                           rel.sections[".relr.dyn"] );
        CHECK( !relr2.preview( 0x1000, 0, pv ) );
        CHECK( pv.status == relr_preview_status::unsupported_image );
    }
}

//------------------------------------------------------------------------------
static void group_dump_correspondence()
{
    const unsigned w    = 8;
    const uint64_t base = 0x1000;
    const uint64_t bias = 0x100000;
    const uint64_t stored[] = { 0x1122334455667788ull, 0x99AABBCCDDEEFF00ull,
                                0x13579BDF2468ACE0ull, 0x0F1E2D3C4B5A6978ull };

    elfio writer;
    writer.create( ELFCLASS64, ELFDATA2LSB );
    writer.set_type( ET_DYN );
    writer.set_machine( EM_NONE );

    section* relr_sec = writer.sections.add( ".relr.dyn" );
    relr_sec->set_type( SHT_RELR );
    relr_sec->set_addr_align( w );
    relr_sec->set_entry_size( w );
    array_section_accessor<Elf_Xword> raw( writer, relr_sec );
    raw.add_entry( base );
    raw.add_entry( 0x1b ); // bits 1, 3 and 4

    std::vector<char> content( 4 * w, 0 );
    for ( unsigned i = 0; i < 4; ++i )
        put_word( content, i * w, stored[i], w, ELFDATA2LSB );
    add_progbits( writer, ".data", base, content );

    std::stringstream bytes( std::ios::in | std::ios::out | std::ios::binary );
    CHECK( writer.save( bytes ) );
    elfio reader;
    bytes.seekg( 0 );
    CHECK( reader.load( bytes ) );

    const_relr_section_accessor relr( reader, reader.sections[".relr.dyn"] );
    std::vector<relr_entry>     entries;
    relr_error                  error;
    CHECK( relr.decode( entries, error ) );
    std::vector<relr_preview> previews;
    relr.preview_all( entries, bias, previews );

    std::ostringstream dump_out;
    dump::relr_tables( dump_out, reader, bias );
    const std::string text = dump_out.str();

    CHECK( text.find( "RELR table (.relr.dyn)" ) != std::string::npos );
    for ( size_t i = 0; i < entries.size(); ++i ) {
        CHECK( text.find( hex0x( entries[i].address, 16 ) ) !=
               std::string::npos );
        if ( previews[i].status == relr_preview_status::ok ) {
            CHECK( text.find( "initial=" +
                              hex0x( previews[i].initial_value, 16 ) ) !=
                   std::string::npos );
            CHECK( text.find( "preview=" +
                              hex0x( previews[i].relocated_value, 16 ) ) !=
                   std::string::npos );
        }
        else {
            CHECK( text.find( "preview=" +
                              const_relr_section_accessor::
                                  preview_status_string( previews[i].status ) ) !=
                   std::string::npos );
        }
    }
    CHECK( text.find( "direct" ) != std::string::npos );
    CHECK( text.find( "bitmap bit 1" ) != std::string::npos );
    CHECK( text.find( "preview=no_owner" ) != std::string::npos );

    // A broken encoding is reported with its raw entry position
    {
        elfio broken = make_image( ELFCLASS64, ELFDATA2LSB, ET_DYN, { 0x7 } );
        std::ostringstream broken_out;
        dump::relr_tables( broken_out, broken );
        CHECK( broken_out.str().find( "Error:" ) != std::string::npos );
        CHECK( broken_out.str().find( "at entry 0" ) != std::string::npos );
    }

    // Existing dump functions are unaffected
    {
        std::ostringstream other;
        dump::header( other, reader );
        dump::section_headers( other, reader );
        dump::symbol_tables( other, reader );
        dump::dynamic_tags( other, reader );
        CHECK( other.str().find( "ELF Header" ) != std::string::npos );
        CHECK( other.str().find( "Section Headers:" ) != std::string::npos );
    }
}

//------------------------------------------------------------------------------
static void group_save_roundtrip()
{
    elfio writer;
    writer.create( ELFCLASS64, ELFDATA2MSB );
    writer.set_type( ET_DYN );
    writer.set_machine( EM_NONE );

    section* relr_sec = writer.sections.add( ".relr.dyn" );
    relr_sec->set_type( SHT_RELR );
    relr_sec->set_addr_align( 8 );
    relr_sec->set_entry_size( 8 );
    array_section_accessor<Elf_Xword> raw( writer, relr_sec );
    raw.add_entry( 0x1000 );
    raw.add_entry( 0xb );

    std::vector<char> content( 4 * 8, 0 );
    put_word( content, 0, 0xA5A5A5A5A5A5A5A5ull, 8, ELFDATA2MSB );
    add_progbits( writer, ".data", 0x1000, content );

    std::stringstream before( std::ios::in | std::ios::out |
                              std::ios::binary );
    CHECK( writer.save( before ) );
    const std::string bytes_before = before.str();

    elfio reader;
    before.seekg( 0 );
    CHECK( reader.load( before ) );

    // Decode, preview and dump: none of them may modify the image
    const_relr_section_accessor relr( reader, reader.sections[".relr.dyn"] );
    std::vector<relr_entry>     entries;
    relr_error                  error;
    CHECK( relr.decode( entries, error ) );
    std::vector<relr_preview> previews;
    relr.preview_all( entries, 0x4000, previews );
    std::ostringstream dump_out;
    dump::relr_tables( dump_out, reader, 0x4000 );

    std::stringstream after( std::ios::in | std::ios::out | std::ios::binary );
    CHECK( reader.save( after ) );
    CHECK( after.str() == bytes_before );
}

//------------------------------------------------------------------------------
int main()
{
    struct Group
    {
        const char* name;
        void ( *fn )();
    };
    const Group groups[] = {
        { "decode_4combo", group_decode_4combo },
        { "empty_bitmap_run", group_empty_bitmap_run },
        { "msb_bitmap", group_msb_bitmap },
        { "unaligned_location", group_unaligned_location },
        { "nobits_preview", group_nobits_preview },
        { "overlap_and_truncated", group_overlap_and_truncated },
        { "encoding_overflow", group_encoding_overflow },
        { "preview_progbits", group_preview_progbits },
        { "dump_correspondence", group_dump_correspondence },
        { "save_roundtrip", group_save_roundtrip },
    };

    int failed = 0;
    for ( const Group& group : groups ) {
        const int before = failures;
        group.fn();
        if ( failures == before ) {
            std::cout << "PASS " << group.name << std::endl;
        }
        else {
            std::cout << "FAIL " << group.name << std::endl;
            ++failed;
        }
    }

    return failed == 0 ? 0 : 1;
}
