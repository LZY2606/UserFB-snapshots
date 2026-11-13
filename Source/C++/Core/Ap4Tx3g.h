/*****************************************************************
|
|    AP4 - 3GPP Timed Text (tx3g) Support
|
|    Copyright 2002-2008 Axiomatic Systems, LLC
|
|
|    This file is part of Bento4/AP4 (MP4 Atom Processing Library).
|
|    Unless you have obtained Bento4 under a difference license,
|    this version of Bento4 is Bento4|GPL.
|    Bento4|GPL is free software; you can redistribute it and/or modify
|    it under the terms of the GNU General Public License as published by
|    the Free Software Foundation; either version 2, or (at your option)
|    any later version.
|
|    Bento4|GPL is distributed in the hope that it will be useful,
|    but WITHOUT ANY WARRANTY; without even the implied warranty of
|    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
|    GNU General Public License for more details.
|
|    You should have received a copy of the GNU General Public License
|    along with Bento4|GPL; see the file COPYING.  If not, write to the
|    Free Software Foundation, 59 Temple Place - Suite 330, Boston, MA
|    02111-1307, USA.
|
 ****************************************************************/
/**
 * @file
 * @brief 3GPP Timed Text (tx3g) sample descriptions and sample decoder
 */

#ifndef _AP4_TX3G_H_
#define _AP4_TX3G_H_

/*----------------------------------------------------------------------
|   includes
+---------------------------------------------------------------------*/
#include "Ap4Types.h"
#include "Ap4Array.h"
#include "Ap4String.h"
#include "Ap4DataBuffer.h"
#include "Ap4SampleEntry.h"
#include "Ap4SampleDescription.h"

/*----------------------------------------------------------------------
|   class references
+---------------------------------------------------------------------*/
class AP4_Track;

/*----------------------------------------------------------------------
|   constants
+---------------------------------------------------------------------*/
const AP4_Atom::Type AP4_ATOM_TYPE_TX3G = AP4_ATOM_TYPE('t','x','3','g');
const AP4_Atom::Type AP4_ATOM_TYPE_STYL = AP4_ATOM_TYPE('s','t','y','l');
const AP4_Atom::Type AP4_ATOM_TYPE_FTAB = AP4_ATOM_TYPE('f','t','a','b');
const AP4_Atom::Type AP4_ATOM_TYPE_HLIT = AP4_ATOM_TYPE('h','l','i','t');
const AP4_Atom::Type AP4_ATOM_TYPE_HCLR = AP4_ATOM_TYPE('h','c','l','r');
const AP4_Atom::Type AP4_ATOM_TYPE_TBOX = AP4_ATOM_TYPE('t','b','o','x');
const AP4_Atom::Type AP4_ATOM_TYPE_KBOX = AP4_ATOM_TYPE('k','b','o','x');
const AP4_Atom::Type AP4_ATOM_TYPE_BLNK = AP4_ATOM_TYPE('b','l','n','k');
const AP4_Atom::Type AP4_ATOM_TYPE_DLAY = AP4_ATOM_TYPE('d','l','a','y');
const AP4_Atom::Type AP4_ATOM_TYPE_DROP = AP4_ATOM_TYPE('d','r','o','p');
const AP4_Atom::Type AP4_ATOM_TYPE_HREL = AP4_ATOM_TYPE('h','r','e','l');
const AP4_Atom::Type AP4_ATOM_TYPE_VREL = AP4_ATOM_TYPE('v','r','e','l');
const AP4_Atom::Type AP4_ATOM_TYPE_ROLL = AP4_ATOM_TYPE('r','o','l','l');
const AP4_Atom::Type AP4_ATOM_TYPE_SHRP = AP4_ATOM_TYPE('s','h','r','p');
const AP4_Atom::Type AP4_ATOM_TYPE_TWRP = AP4_ATOM_TYPE('t','w','r','p');

const AP4_UI08 AP4_TX3G_ALIGNMENT_LEFT    = 0;
const AP4_UI08 AP4_TX3G_ALIGNMENT_CENTER  = 1;
const AP4_UI08 AP4_TX3G_ALIGNMENT_RIGHT   = 2;
const AP4_UI08 AP4_TX3G_ALIGNMENT_JUSTIFY = 3;

/*----------------------------------------------------------------------
|   AP4_Tx3gColor
+---------------------------------------------------------------------*/
struct AP4_Tx3gColor {
    AP4_UI08 r, g, b, a;
};

/*----------------------------------------------------------------------
|   AP4_Tx3gStyleRecord
+---------------------------------------------------------------------*/
/**
 * A complete text style, as defined by the StyleRecord structure of the
 * 3GPP timed text specification (3GPP TS 26.245).
 */
class AP4_Tx3gStyleRecord {
public:
    AP4_Tx3gStyleRecord() :
        m_StartChar(0),
        m_EndChar(0),
        m_FontId(1),
        m_FontStyleFlags(0),
        m_FontSize(18)
    {
        m_Color.r = 0xFF;
        m_Color.g = 0xFF;
        m_Color.b = 0xFF;
        m_Color.a = 0xFF;
    }

    // style flag bits (from the spec's font-style field)
    enum {
        FONT_STYLE_BOLD      = 0x01,
        FONT_STYLE_ITALIC    = 0x02,
        FONT_STYLE_UNDERLINE = 0x04
    };

    // character range: half-open, measured in decoded Unicode code points
    AP4_UI16 m_StartChar;
    AP4_UI16 m_EndChar;

    // complete style attributes
    AP4_UI16     m_FontId;
    AP4_UI08     m_FontStyleFlags; // bold / italic / underline
    AP4_UI08     m_FontSize;
    AP4_Tx3gColor m_Color;          // ARGB, alpha retained
};

/*----------------------------------------------------------------------
|   AP4_Tx3gFontRecord
+---------------------------------------------------------------------*/
class AP4_Tx3gFontRecord {
public:
    AP4_Tx3gFontRecord() : m_FontId(0) {}
    AP4_Tx3gFontRecord(AP4_UI16 font_id, const char* name) :
        m_FontId(font_id), m_Name(name) {}

    AP4_UI16   m_FontId;
    AP4_String m_Name;
};

/*----------------------------------------------------------------------
|   AP4_Tx3gFtabAtom
+---------------------------------------------------------------------*/
/**
 * Font Table ('ftab') child box of a tx3g sample entry.
 */
class AP4_Tx3gFtabAtom : public AP4_Atom {
public:
    AP4_IMPLEMENT_DYNAMIC_CAST_D(AP4_Tx3gFtabAtom, AP4_Atom)

    // construct from a stream (the box header has already been read)
    AP4_Tx3gFtabAtom(AP4_UI32 size, AP4_ByteStream& stream);

    // construct from a font table
    AP4_Tx3gFtabAtom(const AP4_Array<AP4_Tx3gFontRecord>& fonts);

    // accessor
    const AP4_Array<AP4_Tx3gFontRecord>& GetFontTable() const { return m_FontTable; }

    // AP4_Atom methods
    virtual AP4_Result WriteFields(AP4_ByteStream& stream);

private:
    // members
    AP4_Array<AP4_Tx3gFontRecord> m_FontTable;
};

/*----------------------------------------------------------------------
|   AP4_Tx3gSampleEntry
+---------------------------------------------------------------------*/
/**
 * Sample entry for a 3GPP timed text ('tx3g') track. The fixed fields of
 * the sample entry (default style, alignment, background color, default
 * text box and font table) are parsed as fields rather than treated as
 * child atoms. Other extension boxes (hlit, hclr, tbox, kbox, ...) are
 * kept as regular child atoms, unknown extension boxes are preserved
 * verbatim.
 */
class AP4_Tx3gSampleEntry : public AP4_SampleEntry {
public:
    AP4_IMPLEMENT_DYNAMIC_CAST_D(AP4_Tx3gSampleEntry, AP4_SampleEntry)

    // construct an empty entry with specification default values
    AP4_Tx3gSampleEntry();

    // parsing constructor
    AP4_Tx3gSampleEntry(AP4_Size         size,
                        AP4_ByteStream&  stream,
                        AP4_AtomFactory& atom_factory);

    // accessors
    const AP4_Tx3gStyleRecord& GetDefaultStyle() const { return m_DefaultStyle; }
    AP4_UI08  GetHorizontalJustification() const { return m_HorizJustification; }
    AP4_UI08  GetVerticalJustification() const { return m_VertJustification; }
    const AP4_Tx3gColor& GetBackgroundColor() const { return m_BackgroundColor; }
    void      GetDefaultTextBox(AP4_UI32& top,
                                AP4_UI32& left,
                                AP4_UI32& bottom,
                                AP4_UI32& right) const {
        top    = m_DefaultTextBox[0];
        left   = m_DefaultTextBox[1];
        bottom = m_DefaultTextBox[2];
        right  = m_DefaultTextBox[3];
    }
    const AP4_Array<AP4_Tx3gFontRecord>& GetFontTable() const { return m_FontTable; }

    // mutators (for building entries programmatically)
    AP4_Result SetDefaultStyle(const AP4_Tx3gStyleRecord& style);
    AP4_Result SetJustification(AP4_UI08 horizontal, AP4_UI08 vertical);
    AP4_Result SetBackgroundColor(const AP4_Tx3gColor& color);
    AP4_Result SetDefaultTextBox(AP4_UI32 top,
                                 AP4_UI32 left,
                                 AP4_UI32 bottom,
                                 AP4_UI32 right);
    AP4_Result SetFontTable(const AP4_Array<AP4_Tx3gFontRecord>& fonts);
    AP4_Result AddFont(AP4_UI16 font_id, const char* name);

    // resolve a font identifier from the ftab table
    const AP4_String* FindFontName(AP4_UI16 font_id) const;

    // AP4_SampleEntry methods
    virtual AP4_SampleDescription* ToSampleDescription();

protected:
    // methods
    virtual AP4_Size   GetFieldsSize();
    virtual AP4_Result ReadFields(AP4_ByteStream& stream);
    virtual AP4_Result WriteFields(AP4_ByteStream& stream);
    virtual AP4_Result InspectFields(AP4_AtomInspector& inspector);

private:
    // members
    AP4_Tx3gStyleRecord          m_DefaultStyle;
    AP4_UI08                     m_HorizJustification;
    AP4_UI08                     m_VertJustification;
    AP4_Tx3gColor                m_BackgroundColor;
    AP4_UI32                     m_DefaultTextBox[4]; // top, left, bottom, right
    AP4_Array<AP4_Tx3gFontRecord> m_FontTable;
};

/*----------------------------------------------------------------------
|   AP4_Tx3gSampleDescription
+---------------------------------------------------------------------*/
/**
 * Parsed sample description for a 3GPP timed text ('tx3g') track.
 * The object owns all of its data and does not reference the stream or
 * atom it was built from, so it stays valid independently of the source
 * file. A track may use several of these descriptions, each with its own
 * default style and font table.
 */
class AP4_Tx3gSampleDescription : public AP4_SampleDescription {
public:
    AP4_IMPLEMENT_DYNAMIC_CAST_D(AP4_Tx3gSampleDescription, AP4_SampleDescription)

    // construct from a parsed sample entry (copies all fields)
    AP4_Tx3gSampleDescription(const AP4_Tx3gSampleEntry& entry);

    // accessors
    const AP4_Tx3gStyleRecord& GetDefaultStyle() const { return m_DefaultStyle; }
    AP4_UI08  GetHorizontalJustification() const { return m_HorizJustification; }
    AP4_UI08  GetVerticalJustification() const { return m_VertJustification; }
    const AP4_Tx3gColor& GetBackgroundColor() const { return m_BackgroundColor; }
    void      GetDefaultTextBox(AP4_UI32& top,
                                AP4_UI32& left,
                                AP4_UI32& bottom,
                                AP4_UI32& right) const {
        top    = m_DefaultTextBox[0];
        left   = m_DefaultTextBox[1];
        bottom = m_DefaultTextBox[2];
        right  = m_DefaultTextBox[3];
    }
    const AP4_Array<AP4_Tx3gFontRecord>& GetFontTable() const { return m_FontTable; }
    const AP4_String* FindFontName(AP4_UI16 font_id) const;

    // inherited from AP4_SampleDescription
    virtual AP4_SampleDescription* Clone(AP4_Result* result = NULL);
    virtual AP4_Result GetCodecString(AP4_String& codec);
    virtual AP4_Atom*  ToAtom() const;

private:
    // members
    AP4_Tx3gStyleRecord           m_DefaultStyle;
    AP4_UI08                      m_HorizJustification;
    AP4_UI08                      m_VertJustification;
    AP4_Tx3gColor                 m_BackgroundColor;
    AP4_UI32                      m_DefaultTextBox[4]; // top, left, bottom, right
    AP4_Array<AP4_Tx3gFontRecord> m_FontTable;
};

/*----------------------------------------------------------------------
|   AP4_Tx3gCue
+---------------------------------------------------------------------*/
/**
 * One decoded timed text sample. The text is encoded as UTF-8 and is
 * owned by the cue. Style runs are non-overlapping, sorted in text order
 * and expressed in decoded Unicode code point units (half-open ranges).
 * A cue with empty text is a valid clear-screen cue.
 */
class AP4_Tx3gCue {
public:
    AP4_Tx3gCue();
    AP4_Tx3gCue(const AP4_Tx3gCue& other);
    AP4_Tx3gCue& operator=(const AP4_Tx3gCue& other);

    // one resolved style run: the style is complete on every run
    struct Run {
        AP4_Ordinal        m_StartChar; // first code point covered
        AP4_Ordinal        m_EndChar;   // one past the last code point
        AP4_Tx3gStyleRecord m_Style;
        AP4_String         m_FontName;  // resolved through the description's ftab
    };

    // text (UTF-8, owned)
    const AP4_DataBuffer& GetText() const { return m_Text; }
    AP4_Ordinal GetCharacterCount() const { return m_CharacterCount; }

    // style runs (non-overlapping, text order)
    const AP4_Array<Run>& GetRuns() const { return m_Runs; }

    // presentation defaults copied from the active sample description
    AP4_UI08  GetHorizontalJustification() const { return m_HorizJustification; }
    AP4_UI08  GetVerticalJustification() const { return m_VertJustification; }
    const AP4_Tx3gColor& GetBackgroundColor() const { return m_BackgroundColor; }
    void      GetDefaultTextBox(AP4_UI32& top,
                                AP4_UI32& left,
                                AP4_UI32& bottom,
                                AP4_UI32& right) const {
        top    = m_DefaultTextBox[0];
        left   = m_DefaultTextBox[1];
        bottom = m_DefaultTextBox[2];
        right  = m_DefaultTextBox[3];
    }

    // timing, expressed in the media (track) timescale ticks
    AP4_UI64 GetCts() const { return m_Cts; }
    AP4_UI32 GetDuration() const { return m_Duration; }

private:
    // AP4_Tx3gDecoder fills the fields directly
    friend class AP4_Tx3gDecoder;

    // members
    AP4_DataBuffer         m_Text;
    AP4_Ordinal            m_CharacterCount;
    AP4_Array<Run>         m_Runs;
    AP4_UI08               m_HorizJustification;
    AP4_UI08               m_VertJustification;
    AP4_Tx3gColor          m_BackgroundColor;
    AP4_UI32               m_DefaultTextBox[4];
    AP4_UI64               m_Cts;
    AP4_UI32               m_Duration;
};

/*----------------------------------------------------------------------
|   AP4_Tx3gDecoder
+---------------------------------------------------------------------*/
/**
 * Decoder for 3GPP timed text samples. The decoder is stateless: every
 * sample is decoded on its own against the sample description selected
 * for that sample, so empty (clear-screen) samples never inherit text
 * from a previous sample.
 */
class AP4_Tx3gDecoder {
public:
    /**
     * Decode one sample of a timed text track.
     *
     * @param track  The track that contains the sample.
     * @param index  Ordinal of the sample within the track.
     * @param cue    Output cue. Left untouched when the method fails.
     *
     * @return AP4_SUCCESS, or a locatable error such as
     * AP4_ERROR_NOT_ENOUGH_DATA, AP4_ERROR_INVALID_FORMAT,
     * AP4_ERROR_OUT_OF_RANGE or AP4_ERROR_NO_SUCH_ITEM.
     */
    static AP4_Result Decode(AP4_Track&   track,
                             AP4_Ordinal  index,
                             AP4_Tx3gCue& cue);
};

#endif // _AP4_TX3G_H_
