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

#ifndef ELFIO_RELR_HPP
#define ELFIO_RELR_HPP

#include <cstring>
#include <string>
#include <vector>

namespace ELFIO {

//------------------------------------------------------------------------------
//! \enum relr_status
//! \brief Status codes reported while decoding a SHT_RELR section
enum class relr_status {
    ok,                 //!< No error
    not_relr_section,   //!< The section is not of type SHT_RELR
    no_section_data,    //!< The section data could not be read
    bad_entry_size,     //!< sh_entsize differs from the file word size
    trailing_entry,     //!< Trailing bytes smaller than one entry remain
    bitmap_without_base, //!< The very first entry is a bitmap
    address_overflow,   //!< The position following an address entry wraps
    range_overflow      //!< The coverage range of a bitmap wraps
};

//------------------------------------------------------------------------------
//! \struct relr_error
//! \brief Locatable failure reason produced by RELR decoding
struct relr_error
{
    relr_status status       = relr_status::ok; //!< Error code
    Elf_Xword   entry_index  = 0;               //!< Raw entry ordinal
};

//------------------------------------------------------------------------------
//! \struct relr_entry
//! \brief A single location expanded from a SHT_RELR section
struct relr_entry
{
    Elf64_Addr  address     = 0;     //!< Expanded location
    Elf_Xword   entry_index = 0;     //!< Ordinal of the raw entry it came from
    Elf_Xword entry_value = 0;     //!< Raw value of that entry (host order)
    bool        from_bitmap = false; //!< True if expanded from a bitmap entry
    unsigned    bitmap_bit  = 0;     //!< Bit number within the bitmap entry
};

//------------------------------------------------------------------------------
//! \enum relr_preview_status
//! \brief Per-location result of the read-only relocation preview
enum class relr_preview_status {
    ok,                 //!< The target word was read and relocated
    no_owner,           //!< No SHF_ALLOC section fully contains the word
    overlapping_owners, //!< More than one SHF_ALLOC section fully contains it
    truncated_data,     //!< The owning section data is actually truncated
    unsupported_image,  //!< The image is neither ET_DYN nor ET_EXEC
    unsupported_owner   //!< The unique owner is neither PROGBITS nor NOBITS
};

//------------------------------------------------------------------------------
//! \struct relr_preview
//! \brief Read-only preview of a single RELR location after applying a bias
struct relr_preview
{
    relr_preview_status status = relr_preview_status::no_owner; //!< Result
    Elf_Half   section_index   = 0; //!< Index of the owning section
    Elf64_Off  section_offset  = 0; //!< Offset of the word within the section
    Elf64_Addr initial_value   = 0; //!< Stored word (0 for NOBITS)
    Elf64_Addr relocated_value = 0; //!< initial_value + load bias
};

//------------------------------------------------------------------------------
//! \class relr_section_accessor_template
//! \brief Read-only accessor interpreting SHT_RELR sections (ELF gABI RELR
//! encoding). It never modifies the image.
template <class S> class relr_section_accessor_template
{
  public:
    //------------------------------------------------------------------------------
    //! \brief Constructor
    //! \param elf_file Reference to the ELF file
    //! \param section Pointer to the SHT_RELR section
    explicit relr_section_accessor_template( const elfio& elf_file,
                                             S*           section )
        : elf_file( elf_file ), relr_section( section )
    {
    }

    //------------------------------------------------------------------------------
    //! \brief Get the number of raw entries (words) in the section
    //! \return Number of raw entries
    Elf_Xword get_entries_num() const
    {
        const Elf_Xword word_size = get_word_size();
        if ( word_size == 0 ) {
            return 0;
        }
        return relr_section->get_size() / word_size;
    }

    //------------------------------------------------------------------------------
    //! \brief Get a raw entry value, converted to host endianness
    //! \param index Index of the raw entry
    //! \param value Raw value of the entry
    //! \return True if successful, false otherwise
    bool get_entry( Elf_Xword index, Elf_Xword& value ) const
    {
        if ( index >= get_entries_num() ) {
            return false;
        }

        const char* data = relr_section->get_data();
        if ( data == nullptr ) {
            return false;
        }

        const auto& convertor = elf_file.get_convertor();
        if ( elf_file.get_class() == ELFCLASS64 ) {
            Elf_Xword temp;
            std::memcpy( &temp, data + index * sizeof( Elf_Xword ),
                         sizeof( temp ) );
            value = ( *convertor )( temp );
        }
        else {
            Elf32_Word temp;
            std::memcpy( &temp, data + index * sizeof( Elf32_Word ),
                         sizeof( temp ) );
            value = ( *convertor )( temp );
        }

        return true;
    }

    //------------------------------------------------------------------------------
    //! \brief Decode the RELR encoding into the list of covered locations.
    //! The output strictly follows the encoding order: it is neither sorted
    //! nor deduplicated. On failure, the locations decoded before the error
    //! are still returned and the error parameter pinpoints the raw entry.
    //! \param entries Output vector of expanded locations
    //! \param error Output locatable failure reason
    //! \return True if the whole section decoded cleanly, false otherwise
    bool decode( std::vector<relr_entry>& entries, relr_error& error ) const
    {
        entries.clear();
        error = relr_error{};

        if ( relr_section == nullptr ||
             relr_section->get_type() != SHT_RELR ) {
            error.status = relr_status::not_relr_section;
            return false;
        }

        const Elf_Xword word_size = get_word_size();
        if ( word_size == 0 ||
             relr_section->get_entry_size() != word_size ) {
            error.status = relr_status::bad_entry_size;
            return false;
        }

        const char* data = relr_section->get_data();
        if ( data == nullptr ) {
            error.status = relr_status::no_section_data;
            return false;
        }

        const Elf_Xword   size = relr_section->get_size();
        const Elf_Xword   num  = size / word_size;
        const Elf_Xword mask = word_mask();
        const unsigned    bits = static_cast<unsigned>( word_size ) * 8;

        Elf_Xword where     = 0;
        bool        have_base = false;

        for ( Elf_Xword i = 0; i < num; ++i ) {
            Elf_Xword value = 0;
            if ( !get_entry( i, value ) ) {
                error.status       = relr_status::no_section_data;
                error.entry_index  = i;
                return false;
            }

            if ( ( value & 1 ) == 0 ) {
                // Even entry: a direct location. It may be unaligned;
                // the format only excludes odd addresses.
                relr_entry entry;
                entry.address     = value;
                entry.entry_index = i;
                entry.entry_value = value;
                entries.push_back( entry );

                // The next position follows this address
                if ( value > mask - word_size ) {
                    error.status      = relr_status::address_overflow;
                    error.entry_index = i;
                    return false;
                }
                where     = ( value + word_size ) & mask;
                have_base = true;
            }
            else {
                // Odd entry: a bitmap describing the next bits-1 positions
                if ( !have_base ) {
                    error.status      = relr_status::bitmap_without_base;
                    error.entry_index = i;
                    return false;
                }

                // Every bitmap advances the full coverage range, even
                // when only the flag bit is set
                const Elf_Xword advance =
                    static_cast<Elf_Xword>( bits - 1 ) * word_size;
                if ( where > mask - advance ) {
                    error.status      = relr_status::range_overflow;
                    error.entry_index = i;
                    return false;
                }

                for ( unsigned bit = 1; bit < bits; ++bit ) {
                    if ( ( value & ( Elf_Xword( 1 ) << bit ) ) != 0 ) {
                        relr_entry entry;
                        entry.address =
                            where +
                            static_cast<Elf_Xword>( bit - 1 ) * word_size;
                        entry.entry_index = i;
                        entry.entry_value = value;
                        entry.from_bitmap = true;
                        entry.bitmap_bit  = bit;
                        entries.push_back( entry );
                    }
                }

                where = ( where + advance ) & mask;
            }
        }

        if ( size % word_size != 0 ) {
            error.status      = relr_status::trailing_entry;
            error.entry_index = num;
            return false;
        }

        return true;
    }

    //------------------------------------------------------------------------------
    //! \brief Read-only preview of the relocation of one location.
    //! The word at the given address is looked up among the SHF_ALLOC
    //! sections of an ET_DYN or ET_EXEC image by virtual address range.
    //! Exactly one fully containing PROGBITS section provides the stored
    //! bytes (read with the file endianness); a fully containing NOBITS
    //! section yields a zero initial value. Bytes of adjacent sections are
    //! never spliced together.
    //! \param address Location to preview
    //! \param load_bias Load bias added to the stored word
    //! \param result Output preview (status is always set)
    //! \return True if the preview succeeded, false otherwise
    bool preview( Elf64_Addr    address,
                  Elf64_Addr    load_bias,
                  relr_preview& result ) const
    {
        result = relr_preview{};

        const Elf_Half type = elf_file.get_type();
        if ( type != ET_DYN && type != ET_EXEC ) {
            result.status = relr_preview_status::unsupported_image;
            return false;
        }

        const Elf_Xword word_size = get_word_size();
        if ( word_size == 0 ) {
            result.status = relr_preview_status::unsupported_image;
            return false;
        }

        // Find SHF_ALLOC sections fully containing the target word
        const section* owner       = nullptr;
        Elf_Half       owner_index = 0;
        unsigned       owners      = 0;
        for ( Elf_Half i = 0; i < elf_file.sections.size(); ++i ) {
            const section* sec = elf_file.sections[i];
            if ( sec == nullptr || ( sec->get_flags() & SHF_ALLOC ) == 0 ||
                 sec->get_type() == SHT_NULL ) {
                continue;
            }
            const Elf64_Addr begin = sec->get_address();
            const Elf_Xword  size  = sec->get_size();
            if ( address >= begin && address - begin < size &&
                 word_size <= size - ( address - begin ) ) {
                ++owners;
                owner       = sec;
                owner_index = i;
            }
        }

        if ( owners == 0 ) {
            result.status = relr_preview_status::no_owner;
            return false;
        }
        if ( owners > 1 ) {
            result.status = relr_preview_status::overlapping_owners;
            return false;
        }

        result.section_index  = owner_index;
        result.section_offset = address - owner->get_address();

        Elf_Xword initial = 0;
        if ( owner->get_type() == SHT_NOBITS ) {
            initial = 0;
        }
        else if ( owner->get_type() == SHT_PROGBITS ) {
            const char* data = owner->get_data();
            if ( data == nullptr ) {
                result.status = relr_preview_status::truncated_data;
                return false;
            }
            const auto& convertor = elf_file.get_convertor();
            if ( word_size == sizeof( Elf_Xword ) ) {
                Elf_Xword temp;
                std::memcpy( &temp, data + result.section_offset,
                             sizeof( temp ) );
                initial = ( *convertor )( temp );
            }
            else {
                Elf32_Word temp;
                std::memcpy( &temp, data + result.section_offset,
                             sizeof( temp ) );
                initial = ( *convertor )( temp );
            }
        }
        else {
            result.status = relr_preview_status::unsupported_owner;
            return false;
        }

        result.initial_value   = initial;
        result.relocated_value = ( initial + load_bias ) & word_mask();
        result.status          = relr_preview_status::ok;
        return true;
    }

    //------------------------------------------------------------------------------
    //! \brief Preview every decoded location; one unreadable location does
    //! not mask the others
    //! \param entries Locations previously returned by decode()
    //! \param load_bias Load bias added to each stored word
    //! \param previews Output previews, one per entry, in the same order
    void preview_all( const std::vector<relr_entry>& entries,
                      Elf64_Addr                     load_bias,
                      std::vector<relr_preview>&     previews ) const
    {
        previews.clear();
        previews.reserve( entries.size() );
        for ( const auto& entry : entries ) {
            relr_preview result;
            preview( entry.address, load_bias, result );
            previews.push_back( result );
        }
    }

    //------------------------------------------------------------------------------
    //! \brief Convert a decode status to a readable string
    //! \param status The decode status
    //! \return Human readable string
    static std::string status_string( relr_status status )
    {
        switch ( status ) {
        case relr_status::ok:
            return "ok";
        case relr_status::not_relr_section:
            return "not a SHT_RELR section";
        case relr_status::no_section_data:
            return "section data is not readable";
        case relr_status::bad_entry_size:
            return "entry size differs from the file word size";
        case relr_status::trailing_entry:
            return "trailing bytes smaller than one entry";
        case relr_status::bitmap_without_base:
            return "first entry is a bitmap";
        case relr_status::address_overflow:
            return "position after an address entry overflows the word width";
        case relr_status::range_overflow:
            return "bitmap coverage range overflows the word width";
        }
        return "unknown";
    }

    //------------------------------------------------------------------------------
    //! \brief Convert a preview status to a readable string
    //! \param status The preview status
    //! \return Human readable string
    static std::string preview_status_string( relr_preview_status status )
    {
        switch ( status ) {
        case relr_preview_status::ok:
            return "ok";
        case relr_preview_status::no_owner:
            return "no_owner";
        case relr_preview_status::overlapping_owners:
            return "overlapping_owners";
        case relr_preview_status::truncated_data:
            return "truncated_data";
        case relr_preview_status::unsupported_image:
            return "unsupported_image";
        case relr_preview_status::unsupported_owner:
            return "unsupported_owner";
        }
        return "unknown";
    }

    //------------------------------------------------------------------------------
  private:
    //------------------------------------------------------------------------------
    //! \brief Get the file word size in bytes
    //! \return 4 for ELFCLASS32, 8 for ELFCLASS64, 0 otherwise
    Elf_Xword get_word_size() const
    {
        if ( elf_file.get_class() == ELFCLASS64 ) {
            return sizeof( Elf_Xword );
        }
        if ( elf_file.get_class() == ELFCLASS32 ) {
            return sizeof( Elf32_Word );
        }
        return 0;
    }

    //------------------------------------------------------------------------------
    //! \brief Get the bitmask of the file word width
    //! \return 32- or 64-bit all-ones mask
    Elf_Xword word_mask() const
    {
        if ( elf_file.get_class() == ELFCLASS64 ) {
            return ~Elf_Xword( 0 );
        }
        return Elf_Xword( 0xFFFFFFFF );
    }

    //------------------------------------------------------------------------------
  private:
    const elfio& elf_file;                          //!< Reference to ELF file
    S*           relr_section = nullptr;            //!< RELR section
};

using relr_section_accessor = relr_section_accessor_template<section>;
using const_relr_section_accessor =
    relr_section_accessor_template<const section>;

} // namespace ELFIO

#endif // ELFIO_RELR_HPP
