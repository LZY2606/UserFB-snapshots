/*****************************************************************
|
|    AP4 - TX3G Timed Text Support
|
|    Copyright 2002-2024 Axiomatic Systems, LLC
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
 * @brief TX3G (3GPP timed text) sample descriptions and sample decoder.
 *
 * This header exposes:
 *   - AP4_Tx3gSampleDescription: parsed 'tx3g' sample entry fields
 *     (default style, alignment, background color, default text box and
 *      the 'ftab' font table).
 *   - AP4_Tx3gDecoder::Decode: decodes one text sample into an
 *     AP4_Tx3gCue (UTF-8 text, merged style runs, layout and timing).
 *
 * All the data returned in an AP4_Tx3gCue is owned by the cue object, it
 * does not reference the sample data buffer or the sample description.
 */

#ifndef _AP4_TX3G_H_
#define _AP4_TX3G_H_

/*----------------------------------------------------------------------
|   includes
+---------------------------------------------------------------------*/
#include "Ap4Types.h"
#include "Ap4String.h"
#include "Ap4Array.h"
#include "Ap4DataBuffer.h"
#include "Ap4SampleDescription.h"
#include "Ap4SampleEntry.h"

#include <string>

/*----------------------------------------------------------------------
|   forward declarations
+---------------------------------------------------------------------*/
class AP4_Track;
class AP4_ByteStream;
class AP4_AtomFactory;

/*----------------------------------------------------------------------
|   constants
+---------------------------------------------------------------------*/
/* horizontal justification / alignment (3GPP values) */
const int AP4_TX3G_ALIGN_LEFT    = 0;
const int AP4_TX3G_ALIGN_CENTER  = 1;
const int AP4_TX3G_ALIGN_RIGHT   = -1;

/* vertical justification / alignment (3GPP values) */
const int AP4_TX3G_ALIGN_TOP     = 0;
const int AP4_TX3G_ALIGN_MIDDLE  = 1;
const int AP4_TX3G_ALIGN_BOTTOM  = -1;

/* faceStyleFlags bits */
const AP4_UI08 AP4_TX3G_FACE_BOLD      = 0x01;
const AP4_UI08 AP4_TX3G_FACE_ITALIC    = 0x02;
const AP4_UI08 AP4_TX3G_FACE_UNDERLINE = 0x04;

/*----------------------------------------------------------------------
|   AP4_Tx3gText
|
|   Owned UTF-8 text. This is an AP4_String (Bento4 API, GetChars() /
|   GetLength()) with std::string-style accessors (c_str() / size() /
|   empty() / begin() / end()) and an implicit conversion to std::string.
+---------------------------------------------------------------------*/
class AP4_Tx3gText : public AP4_String
{
 public:
    AP4_Tx3gText() : AP4_String() {}
    AP4_Tx3gText(const char* s) : AP4_String(s) {}
    AP4_Tx3gText(const char* s, AP4_Size n) : AP4_String(s, n) {}
    AP4_Tx3gText(const AP4_String& s) : AP4_String(s) {}
    AP4_Tx3gText(const AP4_Tx3gText& s) : AP4_String(s) {}

    AP4_Tx3gText& operator=(const char* s) {
        AP4_String::operator=(s);
        return *this;
    }
    AP4_Tx3gText& operator=(const AP4_String& s) {
        AP4_String::operator=(s);
        return *this;
    }
    AP4_Tx3gText& operator=(const AP4_Tx3gText& s) {
        AP4_String::operator=(static_cast<const AP4_String&>(s));
        return *this;
    }

    /* std::string-style interface */
    const char* c_str() const { return GetChars(); }
    const char* data() const  { return GetChars(); }
    AP4_Size    size() const   { return GetLength(); }
    AP4_Size    length() const { return GetLength(); }
    bool        empty() const  { return GetLength() == 0; }
    const char* begin() const  { return GetChars(); }
    const char* end() const    { return GetChars() + GetLength(); }

    operator std::string() const {
        return std::string(GetChars(), GetLength());
    }
};

/*----------------------------------------------------------------------
|   AP4_Tx3gColor
|
|   RGBA color, one byte per component. The r/g/b/a names are the
|   canonical fields; red/green/blue/alpha and operator[] are provided
|   for convenience.
+---------------------------------------------------------------------*/
struct AP4_Tx3gColor
{
    AP4_UI08 r;
    AP4_UI08 g;
    AP4_UI08 b;
    AP4_UI08 a;

    /* long-name aliases */
    AP4_UI08 red;
    AP4_UI08 green;
    AP4_UI08 blue;
    AP4_UI08 alpha;

    AP4_Tx3gColor(): r(0), g(0), b(0), a(0),
                     red(0), green(0), blue(0), alpha(0) {}
    AP4_Tx3gColor(AP4_UI08 rr, AP4_UI08 gg, AP4_UI08 bb, AP4_UI08 aa):
        r(rr), g(gg), b(bb), a(aa),
        red(rr), green(gg), blue(bb), alpha(aa) {}

    void Set(AP4_UI08 rr, AP4_UI08 gg, AP4_UI08 bb, AP4_UI08 aa) {
        r = red = rr;
        g = green = gg;
        b = blue = bb;
        a = alpha = aa;
    }

    AP4_UI08& operator[](unsigned int i) {
        static AP4_UI08 dummy = 0;
        switch (i) {
            case 0: return r;
            case 1: return g;
            case 2: return b;
            case 3: return a;
            default: return dummy;
        }
    }
    const AP4_UI08& operator[](unsigned int i) const {
        static const AP4_UI08 dummy = 0;
        switch (i) {
            case 0: return r;
            case 1: return g;
            case 2: return b;
            case 3: return a;
            default: return dummy;
        }
    }

    bool operator==(const AP4_Tx3gColor& o) const {
        return r == o.r && g == o.g && b == o.b && a == o.a;
    }
    bool operator!=(const AP4_Tx3gColor& o) const { return !(*this == o); }
};

/*----------------------------------------------------------------------
|   AP4_Tx3gBox
|
|   Default text box (BoxRecord): top/left/bottom/right coordinates.
|   x/y/width/height aliases are computed on demand by SyncAliases().
+---------------------------------------------------------------------*/
struct AP4_Tx3gBox
{
    AP4_SI16 top;
    AP4_SI16 left;
    AP4_SI16 bottom;
    AP4_SI16 right;

    /* geometry aliases (x == left, y == top) */
    AP4_SI16 x;
    AP4_SI16 y;
    AP4_SI16 width;
    AP4_SI16 height;

    AP4_Tx3gBox(): top(0), left(0), bottom(0), right(0),
                   x(0), y(0), width(0), height(0) {}
    AP4_Tx3gBox(AP4_SI16 t, AP4_SI16 l, AP4_SI16 btm, AP4_SI16 rgt):
        top(t), left(l), bottom(btm), right(rgt),
        x(l), y(t), width((AP4_SI16)(rgt-l)), height((AP4_SI16)(btm-t)) {}

    void Set(AP4_SI16 t, AP4_SI16 l, AP4_SI16 btm, AP4_SI16 rgt) {
        top = t; left = l; bottom = btm; right = rgt;
        SyncAliases();
    }

    /* recompute the x/y/width/height aliases after direct field writes */
    void SyncAliases() {
        x = left;
        y = top;
        width  = (AP4_SI16)(right - left);
        height = (AP4_SI16)(bottom - top);
    }

    bool operator==(const AP4_Tx3gBox& o) const {
        return top == o.top && left == o.left &&
               bottom == o.bottom && right == o.right;
    }
    bool operator!=(const AP4_Tx3gBox& o) const { return !(*this == o); }
};

/*----------------------------------------------------------------------
|   AP4_Tx3gStyleRecord
|
|   On-disk 12-byte StyleRecord fields. startChar/endChar are decoded
|   Unicode code point offsets with an half-open [startChar,endChar)
|   range. For the default style of a sample entry the range is not
|   meaningful (the default covers the whole text).
+---------------------------------------------------------------------*/
struct AP4_Tx3gStyleRecord
{
    AP4_UI16 startChar;
    AP4_UI16 endChar;
    AP4_UI16 fontID;
    AP4_UI08 faceStyleFlags;
    AP4_UI08 fontSize;
    AP4_UI08 textColorRGBA[4];

    AP4_Tx3gStyleRecord():
        startChar(0), endChar(0), fontID(0),
        faceStyleFlags(0), fontSize(0) {
        textColorRGBA[0] = textColorRGBA[1] = 0;
        textColorRGBA[2] = textColorRGBA[3] = 0;
    }
};

/*----------------------------------------------------------------------
|   AP4_Tx3gFontRecord
+---------------------------------------------------------------------*/
struct AP4_Tx3gFontRecord
{
    AP4_UI16   fontID;
    AP4_String fontName;

    AP4_Tx3gFontRecord(): fontID(0) {}
    AP4_Tx3gFontRecord(AP4_UI16 id, const char* name):
        fontID(id), fontName(name) {}
};

/*----------------------------------------------------------------------
|   AP4_Tx3gStyle
|
|   Fully resolved style attributes. The font name has been resolved
|   against the 'ftab' table of the sample description that applies to
|   the sample, and is owned by this object.
+---------------------------------------------------------------------*/
struct AP4_Tx3gStyle
{
    AP4_UI16      fontID;
    AP4_UI16      fontId; /* spelling alias */
    AP4_String    fontName;
    AP4_UI08      faceStyleFlags;
    bool          bold;
    bool          italic;
    bool          underline;
    AP4_UI08      fontSize;
    AP4_Tx3gColor color;

    /* alternative views of the color */
    AP4_Tx3gColor textColor;
    AP4_UI08      textColorRGBA[4];
    AP4_UI08      rgba[4];

    AP4_Tx3gStyle():
        fontID(0), fontId(0), faceStyleFlags(0),
        bold(false), italic(false), underline(false), fontSize(0) {
        textColorRGBA[0] = textColorRGBA[1] = 0;
        textColorRGBA[2] = textColorRGBA[3] = 0;
        rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0;
    }

    /* set color from RGBA components, keeping all color views in sync */
    void SetColor(AP4_UI08 r, AP4_UI08 g, AP4_UI08 b, AP4_UI08 a) {
        color.Set(r, g, b, a);
        textColor.Set(r, g, b, a);
        textColorRGBA[0] = rgba[0] = r;
        textColorRGBA[1] = rgba[1] = g;
        textColorRGBA[2] = rgba[2] = b;
        textColorRGBA[3] = rgba[3] = a;
    }

    void SetFlags(AP4_UI08 flags) {
        faceStyleFlags = flags;
        bold      = (flags & AP4_TX3G_FACE_BOLD) != 0;
        italic    = (flags & AP4_TX3G_FACE_ITALIC) != 0;
        underline = (flags & AP4_TX3G_FACE_UNDERLINE) != 0;
    }

    /* compare every effective style attribute */
    bool StyleEquals(const AP4_Tx3gStyle& o) const {
        return fontID == o.fontID &&
               fontName == o.fontName &&
               faceStyleFlags == o.faceStyleFlags &&
               fontSize == o.fontSize &&
               color == o.color;
    }
};

/*----------------------------------------------------------------------
|   AP4_Tx3gStyleRun
|
|   A maximal run of code points sharing the exact same effective
|   style. Ranges are half-open [startChar,endChar) in decoded Unicode
|   code points; startByte/endByte are the matching UTF-8 byte offsets
|   into AP4_Tx3gCue::text.
+---------------------------------------------------------------------*/
struct AP4_Tx3gStyleRun : public AP4_Tx3gStyle
{
    /* canonical code point range */
    AP4_UI32 startChar;
    AP4_UI32 endChar;

    /* short-name aliases (code points) */
    AP4_UI32 start;
    AP4_UI32 end;

    /* explicit code-point spelling aliases */
    AP4_UI32 startCodepoint;
    AP4_UI32 endCodepoint;

    /* UTF-8 byte range into the cue text */
    AP4_UI32 startByte;
    AP4_UI32 endByte;
    AP4_UI32 utf8Start;
    AP4_UI32 utf8End;

    /* the UTF-8 substring covered by this run */
    AP4_Tx3gText text;

    /* nested style view, kept equal to the inherited style attributes */
    AP4_Tx3gStyle style;

    AP4_Tx3gStyleRun():
        startChar(0), endChar(0),
        start(0), end(0),
        startCodepoint(0), endCodepoint(0),
        startByte(0), endByte(0),
        utf8Start(0), utf8End(0) {}

    void SetRange(AP4_UI32 sc, AP4_UI32 ec, AP4_UI32 sb, AP4_UI32 eb) {
        startChar = start = startCodepoint = sc;
        endChar   = end   = endCodepoint   = ec;
        startByte = utf8Start = sb;
        endByte   = utf8End   = eb;
    }
};

/*----------------------------------------------------------------------
|   AP4_Tx3gRunArray
|
|   AP4_Array of style runs with std-style size()/empty()/begin()/end()
|   aliases, so the runs can be used with both Bento4 indexing and
|   range-based for loops.
+---------------------------------------------------------------------*/
class AP4_Tx3gRunArray : public AP4_Array<AP4_Tx3gStyleRun>
{
 public:
    typedef AP4_Tx3gStyleRun        value_type;
    typedef AP4_Tx3gStyleRun*       iterator;
    typedef const AP4_Tx3gStyleRun* const_iterator;

    AP4_Cardinal size() const  { return ItemCount(); }
    bool         empty() const { return ItemCount() == 0; }

    AP4_Tx3gStyleRun* begin() {
        return ItemCount() ? &(*this)[0] : NULL;
    }
    AP4_Tx3gStyleRun* end() {
        return ItemCount() ? (&(*this)[0]) + ItemCount() : NULL;
    }
    const AP4_Tx3gStyleRun* begin() const {
        return ItemCount() ? &(*this)[0] : NULL;
    }
    const AP4_Tx3gStyleRun* end() const {
        return ItemCount() ? (&(*this)[0]) + ItemCount() : NULL;
    }
};

/*----------------------------------------------------------------------
|   AP4_Tx3gCue
|
|   One decoded text sample. All members are value/owned types, so a
|   cue stays valid independently of the track/sample buffers.
+---------------------------------------------------------------------*/
struct AP4_Tx3gCue
{
    /* UTF-8 encoded text (BOM removed). Empty for clear-screen cues. */
    AP4_Tx3gText text;

    /* spelling aliases for the UTF-8 text */
    AP4_Tx3gText utf8;
    AP4_Tx3gText utf8Text;

    /* the effective default style (resolved font name, if available) */
    AP4_Tx3gStyle defaultStyle;

    /* merged, non-overlapping style runs in text order */
    AP4_Tx3gRunArray runs;

    /* alternative names for the same merged run list (kept in sync) */
    AP4_Tx3gRunArray styles;
    AP4_Tx3gRunArray styleRuns;

    /* layout */
    AP4_Tx3gBox   textBox;
    AP4_Tx3gBox   box;             /* alias */
    AP4_Tx3gBox   defaultTextBox;  /* alias */
    int           horizontalAlignment;
    int           verticalAlignment;
    int           horizontalJustification; /* alias */
    int           verticalJustification;   /* alias */
    int           hAlign;                  /* alias */
    int           vAlign;                  /* alias */
    AP4_Tx3gColor backgroundColor;

    /* timing, in the track (media) timescale ticks */
    AP4_UI64 cts;
    AP4_UI64 pts; /* alias */
    AP4_UI32 duration;

    /* number of decoded Unicode code points in text */
    AP4_UI32 codepointCount;

    AP4_Tx3gCue():
        horizontalAlignment(AP4_TX3G_ALIGN_LEFT),
        verticalAlignment(AP4_TX3G_ALIGN_TOP),
        horizontalJustification(AP4_TX3G_ALIGN_LEFT),
        verticalJustification(AP4_TX3G_ALIGN_TOP),
        hAlign(AP4_TX3G_ALIGN_LEFT),
        vAlign(AP4_TX3G_ALIGN_TOP),
        cts(0), pts(0), duration(0), codepointCount(0) {}

    /* restore a default-constructed state */
    void Reset() {
        text = "";
        utf8 = "";
        utf8Text = "";
        defaultStyle = AP4_Tx3gStyle();
        runs.Clear();
        styles.Clear();
        styleRuns.Clear();
        textBox = box = defaultTextBox = AP4_Tx3gBox();
        horizontalAlignment = horizontalJustification = hAlign = AP4_TX3G_ALIGN_LEFT;
        verticalAlignment   = verticalJustification   = vAlign = AP4_TX3G_ALIGN_TOP;
        backgroundColor = AP4_Tx3gColor();
        cts = pts = 0;
        duration = 0;
        codepointCount = 0;
    }

    /* copy decoded results into every alias member */
    void SyncAliases() {
        utf8 = text;
        utf8Text = text;

        box = textBox;
        defaultTextBox = textBox;

        horizontalJustification = hAlign = horizontalAlignment;
        verticalJustification   = vAlign = verticalAlignment;

        pts = cts;

        /* rebuild the alternative run arrays as deep copies */
        styles.Clear();
        styleRuns.Clear();
        for (AP4_Cardinal i = 0; i < runs.ItemCount(); ++i) {
            styles.Append(runs[i]);
            styleRuns.Append(runs[i]);
        }
    }

    /* Bento4-style accessors */
    const AP4_String&       GetText() const { return text; }
    AP4_Size                GetTextLength() const { return text.GetLength(); }
    AP4_Cardinal            GetRunCount() const { return runs.ItemCount(); }
    AP4_Cardinal            GetStyleCount() const { return runs.ItemCount(); }
    const AP4_Tx3gStyleRun& GetRun(AP4_Ordinal i) const { return runs[i]; }
    const AP4_Tx3gStyleRun& GetStyleRun(AP4_Ordinal i) const { return runs[i]; }
    const AP4_Tx3gRunArray& GetRuns() const { return runs; }
    const AP4_Tx3gRunArray& GetStyles() const { return styles; }
    AP4_UI64                GetCts() const { return cts; }
    AP4_UI64                GetPts() const { return cts; }
    AP4_UI32                GetDuration() const { return duration; }
    AP4_UI32                GetCodepointCount() const { return codepointCount; }
    const AP4_Tx3gBox&      GetTextBox() const { return textBox; }
    int                     GetHorizontalAlignment() const { return horizontalAlignment; }
    int                     GetVerticalAlignment() const { return verticalAlignment; }
    const AP4_Tx3gColor&    GetBackgroundColor() const { return backgroundColor; }
};

/*----------------------------------------------------------------------
|   AP4_Tx3gSampleDescription
+---------------------------------------------------------------------*/
class AP4_Tx3gSampleDescription : public AP4_SampleDescription
{
 public:
    AP4_IMPLEMENT_DYNAMIC_CAST_D(AP4_Tx3gSampleDescription, AP4_SampleDescription)

    /**
     * Programmatic constructor.
     *
     * @param format sample format (usually AP4_SAMPLE_FORMAT_TX3G)
     * @param default_style default StyleRecord (fontID/flags/size/color)
     * @param horizontal_justification 0=left, 1=center, -1=right
     * @param vertical_justification   0=top, 1=center, -1=bottom
     * @param background_rgba background color
     * @param default_text_box default BoxRecord
     * @param fonts 'ftab' font records
     * @param other_boxes optional raw bytes of additional sample-entry
     *        child boxes (each a full box including its header); they are
     *        preserved verbatim when the description is written.
     */
    AP4_Tx3gSampleDescription(
        AP4_UI32                            format,
        const AP4_Tx3gStyleRecord&         default_style,
        int                                 horizontal_justification,
        int                                 vertical_justification,
        const AP4_Tx3gColor&                background_rgba,
        const AP4_Tx3gBox&                  default_text_box,
        const AP4_Array<AP4_Tx3gFontRecord>* fonts = NULL,
        const AP4_DataBuffer*               other_boxes = NULL);

    /**
     * Internal constructor used by AP4_Tx3gSampleEntry while parsing.
     * Takes ownership-compatible copies of all fields.
     */
    AP4_Tx3gSampleDescription(
        AP4_UI32                     format,
        AP4_UI32                     display_flags,
        int                          horizontal_justification,
        int                          vertical_justification,
        const AP4_Tx3gColor&         background_rgba,
        const AP4_Tx3gBox&           default_text_box,
        const AP4_Tx3gStyleRecord&   default_style,
        AP4_Array<AP4_Tx3gFontRecord>& fonts,
        AP4_DataBuffer&              other_boxes);

    virtual ~AP4_Tx3gSampleDescription() {}

    /* AP4_SampleDescription */
    virtual AP4_Atom* ToAtom() const;
    virtual AP4_SampleDescription* Clone(AP4_Result* result = NULL);

    /* accessors */
    AP4_UI32                     GetDisplayFlags() const { return m_DisplayFlags; }
    const AP4_Tx3gStyleRecord&   GetDefaultStyleRecord() const { return m_DefaultStyle; }
    const AP4_Tx3gStyleRecord&   GetStyleRecord() const { return m_DefaultStyle; }
    AP4_Tx3gStyleRecord&         UseDefaultStyleRecord() { return m_DefaultStyle; }
    AP4_Tx3gStyle                GetDefaultStyle() const;

    int                          GetHorizontalJustification() const { return m_HorizJustification; }
    int                          GetVerticalJustification() const   { return m_VertJustification; }
    int                          GetHorizontalAlignment() const    { return m_HorizJustification; }
    int                          GetVerticalAlignment() const      { return m_VertJustification; }
    int                          GetHAlign() const { return m_HorizJustification; }
    int                          GetVAlign() const { return m_VertJustification; }

    const AP4_Tx3gColor&         GetBackgroundColor() const { return m_BackgroundColor; }
    const AP4_Tx3gColor&         GetBackgroundRGBA() const  { return m_BackgroundColor; }
    const AP4_Tx3gColor&         GetBackground() const      { return m_BackgroundColor; }
    const AP4_UI08*              GetBackgroundColorRGBA() const { return m_BackgroundColorArray; }

    const AP4_Tx3gBox&           GetDefaultTextBox() const { return m_DefaultTextBox; }
    const AP4_Tx3gBox&           GetTextBox() const        { return m_DefaultTextBox; }
    const AP4_Tx3gBox&           GetBox() const            { return m_DefaultTextBox; }

    AP4_Cardinal                 GetFontCount() const { return m_Fonts.ItemCount(); }
    const AP4_Tx3gFontRecord&    GetFont(AP4_Ordinal index) const { return m_Fonts[index]; }
    const AP4_Array<AP4_Tx3gFontRecord>& GetFonts() const { return m_Fonts; }
    const AP4_Array<AP4_Tx3gFontRecord>& GetFontTable() const { return m_Fonts; }

    /** resolve a fontID against this description's 'ftab' table */
    bool        HasFont(AP4_UI16 font_id) const;
    AP4_Result  FindFontName(AP4_UI16 font_id, AP4_String& name) const;

    /** raw preserved extension boxes (excluding 'ftab') */
    const AP4_DataBuffer& GetOtherBoxes() const { return m_OtherBoxes; }

 protected:
    AP4_UI32                          m_DisplayFlags;
    int                               m_HorizJustification;
    int                               m_VertJustification;
    AP4_Tx3gColor                     m_BackgroundColor;
    AP4_UI08                          m_BackgroundColorArray[4];
    AP4_Tx3gBox                       m_DefaultTextBox;
    AP4_Tx3gStyleRecord               m_DefaultStyle;
    AP4_Array<AP4_Tx3gFontRecord>     m_Fonts;
    AP4_DataBuffer                    m_OtherBoxes;
};

/*----------------------------------------------------------------------
|   AP4_Tx3gSampleEntry
|
|   Atom-level representation of a 'tx3g' sample entry. The fixed
|   TextSampleEntry fields are read/written by ReadFields/WriteFields;
|   the remaining child boxes ('ftab' and extensions) are kept as a raw
|   byte region so they round-trip without needing individual parsers.
+---------------------------------------------------------------------*/
class AP4_Tx3gSampleEntry : public AP4_SampleEntry
{
 public:
    AP4_IMPLEMENT_DYNAMIC_CAST_D(AP4_Tx3gSampleEntry, AP4_SampleEntry)

    /* parse constructor (used by AP4_AtomFactory) */
    AP4_Tx3gSampleEntry(AP4_Size         size,
                        AP4_ByteStream&  stream,
                        AP4_AtomFactory& atom_factory);

    /* write/programmatic constructor */
    AP4_Tx3gSampleEntry(AP4_UI32                          format,
                        AP4_UI32                          display_flags,
                        int                               horiz_justification,
                        int                               vert_justification,
                        const AP4_Tx3gColor&              background_color,
                        const AP4_Tx3gBox&                text_box,
                        const AP4_Tx3gStyleRecord&        default_style,
                        const AP4_Array<AP4_Tx3gFontRecord>& fonts,
                        const AP4_DataBuffer*             other_boxes);

    virtual AP4_SampleDescription* ToSampleDescription();

    /* field access (used by ToSampleDescription) */
    AP4_UI32                   GetDisplayFlags() const { return m_DisplayFlags; }
    int                        GetHorizontalJustification() const { return m_HorizJustification; }
    int                        GetVerticalJustification() const { return m_VertJustification; }
    const AP4_Tx3gColor&       GetBackgroundColor() const { return m_BackgroundColor; }
    const AP4_Tx3gBox&         GetTextBox() const { return m_TextBox; }
    const AP4_Tx3gStyleRecord& GetDefaultStyle() const { return m_DefaultStyle; }
    const AP4_DataBuffer&      GetChildData() const { return m_ChildData; }

 protected:
    virtual AP4_Size   GetFieldsSize();
    virtual AP4_Result ReadFields(AP4_ByteStream& stream);
    virtual AP4_Result WriteFields(AP4_ByteStream& stream);

 private:
    AP4_UI32             m_DisplayFlags;
    int                  m_HorizJustification;
    int                  m_VertJustification;
    AP4_Tx3gColor        m_BackgroundColor;
    AP4_Tx3gBox          m_TextBox;
    AP4_Tx3gStyleRecord  m_DefaultStyle;
    AP4_DataBuffer       m_ChildData; /* ftab + other child boxes */
};

/*----------------------------------------------------------------------
|   AP4_Tx3gDecoder
+---------------------------------------------------------------------*/
/**
 * Stateless decoder for tx3g timed-text samples.
 *
 * Decode() is static so it can be invoked either as
 * `AP4_Tx3gDecoder::Decode(track, i, cue)` or through an instance.
 * On failure @p cue is left untouched (the decode result is built in a
 * temporary and only copied into @p cue on success).
 */
class AP4_Tx3gDecoder
{
 public:
    AP4_Tx3gDecoder() {}

    /**
     * Decode sample @p index of @p track.
     *
     * @return AP4_SUCCESS on success, or a negative AP4_Result error:
     *   AP4_ERROR_NOT_ENOUGH_DATA  - truncated sample/box
     *   AP4_ERROR_INVALID_FORMAT   - bad UTF encoding / reversed range /
     *                                malformed styl box
     *   AP4_ERROR_OUT_OF_RANGE     - a style range exceeds the text
     *   AP4_ERROR_NO_SUCH_ITEM     - a fontID is absent from the current
     *                                sample description's ftab table
     *   AP4_ERROR_INVALID_TRACK_TYPE / AP4_ERROR_INVALID_STATE - the
     *                                sample description is not tx3g
     */
    static AP4_Result Decode(AP4_Track&       track,
                              AP4_Ordinal      index,
                              AP4_Tx3gCue&     cue);

    /**
     * Decode from raw sample bytes and an explicit sample description.
     * (Same semantics as Decode(); exposed for direct/unit use.)
     */
    static AP4_Result DecodeBytes(const AP4_UI08*                    data,
                                  AP4_Size                           data_size,
                                  const AP4_Tx3gSampleDescription*   description,
                                  AP4_UI64                           cts,
                                  AP4_UI32                           duration,
                                  AP4_Tx3gCue&                       cue);
};

#endif /* _AP4_TX3G_H_ */
