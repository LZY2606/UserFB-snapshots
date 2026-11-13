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

/*----------------------------------------------------------------------
|   includes
+---------------------------------------------------------------------*/
#include "Ap4Tx3g.h"
#include "Ap4Utils.h"
#include "Ap4AtomFactory.h"
#include "Ap4Track.h"
#include "Ap4Sample.h"

/*----------------------------------------------------------------------
|   dynamic cast support
+---------------------------------------------------------------------*/
AP4_DEFINE_DYNAMIC_CAST_ANCHOR(AP4_Tx3gFtabAtom)
AP4_DEFINE_DYNAMIC_CAST_ANCHOR(AP4_Tx3gSampleEntry)
AP4_DEFINE_DYNAMIC_CAST_ANCHOR(AP4_Tx3gSampleDescription)

/*----------------------------------------------------------------------
|   fixed sizes
+---------------------------------------------------------------------*/
const AP4_Size AP4_TX3G_FIXED_FIELDS_SIZE = 30; // fields after the 8-byte sample entry header
const AP4_Size AP4_TX3G_STYLE_RECORD_SIZE = 12;

/*----------------------------------------------------------------------
|   AP4_Tx3gFtabAtom
+---------------------------------------------------------------------*/
AP4_Tx3gFtabAtom::AP4_Tx3gFtabAtom(AP4_UI32 size, AP4_ByteStream& stream) :
    AP4_Atom(AP4_ATOM_TYPE_FTAB, size)
{
    AP4_Size payload = (size >= AP4_ATOM_HEADER_SIZE) ? (size-AP4_ATOM_HEADER_SIZE) : 0;
    if (payload < 2) return; // truncated, nothing usable to read

    AP4_UI16 entry_count = 0;
    if (AP4_FAILED(stream.ReadUI16(entry_count))) return;
    AP4_Size consumed = 2;
    for (AP4_Ordinal i = 0; i < entry_count; i++) {
        if (consumed+3 > payload) return; // truncated table
        AP4_UI16 font_id = 0;
        AP4_UI08 name_len = 0;
        if (AP4_FAILED(stream.ReadUI16(font_id))) return;
        if (AP4_FAILED(stream.ReadUI08(name_len))) return;
        consumed += 3;
        if (consumed+name_len > payload) return; // truncated name
        char* name = new char[name_len+1];
        AP4_Result result = stream.Read(name, name_len);
        name[name_len] = '\0';
        if (AP4_FAILED(result)) {
            delete[] name;
            return;
        }
        m_FontTable.Append(AP4_Tx3gFontRecord(font_id, name));
        delete[] name;
        consumed += name_len;
    }
}

/*----------------------------------------------------------------------
|   AP4_Tx3gFtabAtom::AP4_Tx3gFtabAtom
+---------------------------------------------------------------------*/
AP4_Tx3gFtabAtom::AP4_Tx3gFtabAtom(const AP4_Array<AP4_Tx3gFontRecord>& fonts) :
    AP4_Atom(AP4_ATOM_TYPE_FTAB, AP4_ATOM_HEADER_SIZE+2),
    m_FontTable(fonts)
{
    AP4_UI32 size = AP4_ATOM_HEADER_SIZE+2;
    for (AP4_Ordinal i = 0; i < m_FontTable.ItemCount(); i++) {
        size += 3+m_FontTable[i].m_Name.GetLength();
    }
    SetSize(size);
}

/*----------------------------------------------------------------------
|   AP4_Tx3gFtabAtom::WriteFields
+---------------------------------------------------------------------*/
AP4_Result
AP4_Tx3gFtabAtom::WriteFields(AP4_ByteStream& stream)
{
    AP4_Result result = stream.WriteUI16((AP4_UI16)m_FontTable.ItemCount());
    if (AP4_FAILED(result)) return result;
    for (AP4_Ordinal i = 0; i < m_FontTable.ItemCount(); i++) {
        const AP4_Tx3gFontRecord& font = m_FontTable[i];
        result = stream.WriteUI16(font.m_FontId);
        if (AP4_FAILED(result)) return result;
        AP4_Size name_len = font.m_Name.GetLength();
        if (name_len > 255) name_len = 255;
        result = stream.WriteUI08((AP4_UI08)name_len);
        if (AP4_FAILED(result)) return result;
        if (name_len) {
            result = stream.Write(font.m_Name.GetChars(), name_len);
            if (AP4_FAILED(result)) return result;
        }
    }
    return AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   AP4_Tx3gSampleEntry::AP4_Tx3gSampleEntry
+---------------------------------------------------------------------*/
AP4_Tx3gSampleEntry::AP4_Tx3gSampleEntry() :
    AP4_SampleEntry(AP4_ATOM_TYPE_TX3G),
    m_HorizJustification(AP4_TX3G_ALIGNMENT_LEFT),
    m_VertJustification(0)
{
    m_BackgroundColor.r = 0;
    m_BackgroundColor.g = 0;
    m_BackgroundColor.b = 0;
    m_BackgroundColor.a = 0xFF;
    for (unsigned int i = 0; i < 4; i++) {
        m_DefaultTextBox[i] = 0;
    }
    m_Size32 += AP4_TX3G_FIXED_FIELDS_SIZE;
}

/*----------------------------------------------------------------------
|   AP4_Tx3gSampleEntry::AP4_Tx3gSampleEntry
+---------------------------------------------------------------------*/
AP4_Tx3gSampleEntry::AP4_Tx3gSampleEntry(AP4_Size         size,
                                         AP4_ByteStream&  stream,
                                         AP4_AtomFactory& atom_factory) :
    AP4_SampleEntry(AP4_ATOM_TYPE_TX3G, size)
{
    // default values, overridden by ReadFields when the entry is complete
    m_HorizJustification = AP4_TX3G_ALIGNMENT_LEFT;
    m_VertJustification   = 0;
    m_BackgroundColor.r = 0;
    m_BackgroundColor.g = 0;
    m_BackgroundColor.b = 0;
    m_BackgroundColor.a = 0xFF;
    for (unsigned int i = 0; i < 4; i++) {
        m_DefaultTextBox[i] = 0;
    }

    Read(stream, atom_factory);

    // harvest the font table out of the (parsed) ftab child box
    AP4_Tx3gFtabAtom* ftab = AP4_DYNAMIC_CAST(AP4_Tx3gFtabAtom,
                                               GetChild(AP4_ATOM_TYPE_FTAB));
    if (ftab) {
        m_FontTable = ftab->GetFontTable();
    }
}

/*----------------------------------------------------------------------
|   AP4_Tx3gSampleEntry::GetFieldsSize
+---------------------------------------------------------------------*/
AP4_Size
AP4_Tx3gSampleEntry::GetFieldsSize()
{
    return AP4_SampleEntry::GetFieldsSize()+AP4_TX3G_FIXED_FIELDS_SIZE;
}

/*----------------------------------------------------------------------
|   AP4_Tx3gSampleEntry::ReadFields
+---------------------------------------------------------------------*/
AP4_Result
AP4_Tx3gSampleEntry::ReadFields(AP4_ByteStream& stream)
{
    // base sample entry fields (reserved + data reference index)
    AP4_Result result = AP4_SampleEntry::ReadFields(stream);
    if (AP4_FAILED(result)) return result;

    AP4_Size payload = (AP4_Size)(GetSize()-GetHeaderSize());
    if (payload < AP4_SampleEntry::GetFieldsSize()+AP4_TX3G_FIXED_FIELDS_SIZE) {
        return AP4_ERROR_NOT_ENOUGH_DATA;
    }

    // display flags (4 bytes), then horizontal/vertical justification
    AP4_UI32 display_flags = 0;
    result = stream.ReadUI32(display_flags);
    if (AP4_FAILED(result)) return result;
    result = stream.ReadUI08(m_HorizJustification);
    if (AP4_FAILED(result)) return result;
    result = stream.ReadUI08(m_VertJustification);
    if (AP4_FAILED(result)) return result;

    // background color (RGBA)
    result = stream.Read(&m_BackgroundColor, 4);
    if (AP4_FAILED(result)) return result;

    // default text box: top, left, bottom, right (16-bit each)
    for (unsigned int i = 0; i < 4; i++) {
        AP4_UI16 value = 0;
        result = stream.ReadUI16(value);
        if (AP4_FAILED(result)) return result;
        m_DefaultTextBox[i] = value;
    }

    // default StyleRecord
    result = stream.ReadUI16(m_DefaultStyle.m_StartChar);
    if (AP4_FAILED(result)) return result;
    result = stream.ReadUI16(m_DefaultStyle.m_EndChar);
    if (AP4_FAILED(result)) return result;
    result = stream.ReadUI16(m_DefaultStyle.m_FontId);
    if (AP4_FAILED(result)) return result;
    result = stream.ReadUI08(m_DefaultStyle.m_FontStyleFlags);
    if (AP4_FAILED(result)) return result;
    result = stream.ReadUI08(m_DefaultStyle.m_FontSize);
    if (AP4_FAILED(result)) return result;
    result = stream.Read(&m_DefaultStyle.m_Color, 4);
    if (AP4_FAILED(result)) return result;

    // all remaining bytes are child boxes (ftab, hlit, hclr, ...),
    // parsed by Read()
    return AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   AP4_Tx3gSampleEntry::WriteFields
+---------------------------------------------------------------------*/
AP4_Result
AP4_Tx3gSampleEntry::WriteFields(AP4_ByteStream& stream)
{
    AP4_Result result = AP4_SampleEntry::WriteFields(stream);
    if (AP4_FAILED(result)) return result;

    // display flags (0) + horizontal/vertical justification
    result = stream.WriteUI32(0);
    if (AP4_FAILED(result)) return result;
    result = stream.WriteUI08(m_HorizJustification);
    if (AP4_FAILED(result)) return result;
    result = stream.WriteUI08(m_VertJustification);
    if (AP4_FAILED(result)) return result;

    // background color
    result = stream.Write(&m_BackgroundColor, 4);
    if (AP4_FAILED(result)) return result;

    // default text box
    for (unsigned int i = 0; i < 4; i++) {
        result = stream.WriteUI16((AP4_UI16)m_DefaultTextBox[i]);
        if (AP4_FAILED(result)) return result;
    }

    // default StyleRecord
    result = stream.WriteUI16(m_DefaultStyle.m_StartChar);
    if (AP4_FAILED(result)) return result;
    result = stream.WriteUI16(m_DefaultStyle.m_EndChar);
    if (AP4_FAILED(result)) return result;
    result = stream.WriteUI16(m_DefaultStyle.m_FontId);
    if (AP4_FAILED(result)) return result;
    result = stream.WriteUI08(m_DefaultStyle.m_FontStyleFlags);
    if (AP4_FAILED(result)) return result;
    result = stream.WriteUI08(m_DefaultStyle.m_FontSize);
    if (AP4_FAILED(result)) return result;
    result = stream.Write(&m_DefaultStyle.m_Color, 4);
    if (AP4_FAILED(result)) return result;

    // child boxes (ftab, ...) are written by AP4_SampleEntry::Write
    return AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   AP4_Tx3gSampleEntry::InspectFields
+---------------------------------------------------------------------*/
AP4_Result
AP4_Tx3gSampleEntry::InspectFields(AP4_AtomInspector& inspector)
{
    AP4_SampleEntry::InspectFields(inspector);
    inspector.AddField("horizontal_justification", m_HorizJustification);
    inspector.AddField("vertical_justification",   m_VertJustification);
    inspector.AddField("font_count",                m_FontTable.ItemCount());
    return AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   AP4_Tx3gSampleEntry::SetDefaultStyle
+---------------------------------------------------------------------*/
AP4_Result
AP4_Tx3gSampleEntry::SetDefaultStyle(const AP4_Tx3gStyleRecord& style)
{
    m_DefaultStyle = style;
    return AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   AP4_Tx3gSampleEntry::SetJustification
+---------------------------------------------------------------------*/
AP4_Result
AP4_Tx3gSampleEntry::SetJustification(AP4_UI08 horizontal, AP4_UI08 vertical)
{
    m_HorizJustification = horizontal;
    m_VertJustification   = vertical;
    return AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   AP4_Tx3gSampleEntry::SetBackgroundColor
+---------------------------------------------------------------------*/
AP4_Result
AP4_Tx3gSampleEntry::SetBackgroundColor(const AP4_Tx3gColor& color)
{
    m_BackgroundColor = color;
    return AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   AP4_Tx3gSampleEntry::SetDefaultTextBox
+---------------------------------------------------------------------*/
AP4_Result
AP4_Tx3gSampleEntry::SetDefaultTextBox(AP4_UI32 top,
                                       AP4_UI32 left,
                                       AP4_UI32 bottom,
                                       AP4_UI32 right)
{
    m_DefaultTextBox[0] = top;
    m_DefaultTextBox[1] = left;
    m_DefaultTextBox[2] = bottom;
    m_DefaultTextBox[3] = right;
    return AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   AP4_Tx3gSampleEntry::SetFontTable
+---------------------------------------------------------------------*/
AP4_Result
AP4_Tx3gSampleEntry::SetFontTable(const AP4_Array<AP4_Tx3gFontRecord>& fonts)
{
    m_FontTable = fonts;

    // keep the serialized ftab child box in sync
    DeleteChild(AP4_ATOM_TYPE_FTAB);
    if (m_FontTable.ItemCount()) {
        AddChild(new AP4_Tx3gFtabAtom(m_FontTable));
    }
    return AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   AP4_Tx3gSampleEntry::AddFont
+---------------------------------------------------------------------*/
AP4_Result
AP4_Tx3gSampleEntry::AddFont(AP4_UI16 font_id, const char* name)
{
    m_FontTable.Append(AP4_Tx3gFontRecord(font_id, name));
    DeleteChild(AP4_ATOM_TYPE_FTAB);
    AddChild(new AP4_Tx3gFtabAtom(m_FontTable));
    return AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   AP4_Tx3gSampleEntry::FindFontName
+---------------------------------------------------------------------*/
const AP4_String*
AP4_Tx3gSampleEntry::FindFontName(AP4_UI16 font_id) const
{
    for (AP4_Ordinal i = 0; i < m_FontTable.ItemCount(); i++) {
        if (m_FontTable[i].m_FontId == font_id) {
            return &m_FontTable[i].m_Name;
        }
    }
    return NULL;
}

/*----------------------------------------------------------------------
|   AP4_Tx3gSampleEntry::ToSampleDescription
+---------------------------------------------------------------------*/
AP4_SampleDescription*
AP4_Tx3gSampleEntry::ToSampleDescription()
{
    return new AP4_Tx3gSampleDescription(*this);
}

/*----------------------------------------------------------------------
|   AP4_Tx3gSampleDescription::AP4_Tx3gSampleDescription
+---------------------------------------------------------------------*/
AP4_Tx3gSampleDescription::AP4_Tx3gSampleDescription(const AP4_Tx3gSampleEntry& entry) :
    AP4_SampleDescription(AP4_SampleDescription::TYPE_UNKNOWN,
                          AP4_SAMPLE_FORMAT_TX3G,
                          const_cast<AP4_Tx3gSampleEntry*>(&entry)),
    m_DefaultStyle(entry.GetDefaultStyle()),
    m_HorizJustification(entry.GetHorizontalJustification()),
    m_VertJustification(entry.GetVerticalJustification()),
    m_BackgroundColor(entry.GetBackgroundColor()),
    m_FontTable(entry.GetFontTable())
{
    entry.GetDefaultTextBox(m_DefaultTextBox[0],
                            m_DefaultTextBox[1],
                            m_DefaultTextBox[2],
                            m_DefaultTextBox[3]);
}

/*----------------------------------------------------------------------
|   AP4_Tx3gSampleDescription::FindFontName
+---------------------------------------------------------------------*/
const AP4_String*
AP4_Tx3gSampleDescription::FindFontName(AP4_UI16 font_id) const
{
    for (AP4_Ordinal i = 0; i < m_FontTable.ItemCount(); i++) {
        if (m_FontTable[i].m_FontId == font_id) {
            return &m_FontTable[i].m_Name;
        }
    }
    return NULL;
}

/*----------------------------------------------------------------------
|   AP4_Tx3gSampleDescription::Clone
+---------------------------------------------------------------------*/
AP4_SampleDescription*
AP4_Tx3gSampleDescription::Clone(AP4_Result* result)
{
    // round-trip through the serialized entry: fixed fields, ftab and any
    // other extension boxes are all preserved by the sample entry
    return AP4_SampleDescription::Clone(result);
}

/*----------------------------------------------------------------------
|   AP4_Tx3gSampleDescription::GetCodecString
+---------------------------------------------------------------------*/
AP4_Result
AP4_Tx3gSampleDescription::GetCodecString(AP4_String& codec)
{
    return AP4_SampleDescription::GetCodecString(codec);
}

/*----------------------------------------------------------------------
|   AP4_Tx3gSampleDescription::ToAtom
+---------------------------------------------------------------------*/
AP4_Atom*
AP4_Tx3gSampleDescription::ToAtom() const
{
    AP4_Tx3gSampleEntry* entry = new AP4_Tx3gSampleEntry();
    entry->SetDefaultStyle(m_DefaultStyle);
    entry->SetJustification(m_HorizJustification, m_VertJustification);
    entry->SetBackgroundColor(m_BackgroundColor);
    entry->SetDefaultTextBox(m_DefaultTextBox[0],
                             m_DefaultTextBox[1],
                             m_DefaultTextBox[2],
                             m_DefaultTextBox[3]);
    entry->SetFontTable(m_FontTable);

    // preserve extension boxes other than ftab (ftab is rebuilt above)
    AP4_AtomParent& details = const_cast<AP4_AtomParent&>(m_Details);
    for (AP4_List<AP4_Atom>::Item* item = details.GetChildren().FirstItem();
         item;
         item = item->GetNext()) {
        AP4_Atom* child = item->GetData();
        if (child && child->GetType() != AP4_ATOM_TYPE_FTAB) {
            AP4_Atom* clone = child->Clone();
            if (clone) entry->AddChild(clone);
        }
    }

    return entry;
}

/*----------------------------------------------------------------------
|   AP4_Tx3gCue
+---------------------------------------------------------------------*/
AP4_Tx3gCue::AP4_Tx3gCue() :
    m_CharacterCount(0),
    m_HorizJustification(0),
    m_VertJustification(0),
    m_Cts(0),
    m_Duration(0)
{
    m_BackgroundColor.r = 0;
    m_BackgroundColor.g = 0;
    m_BackgroundColor.b = 0;
    m_BackgroundColor.a = 0xFF;
    for (unsigned int i = 0; i < 4; i++) {
        m_DefaultTextBox[i] = 0;
    }
}

/*----------------------------------------------------------------------
|   AP4_Tx3gCue::AP4_Tx3gCue
+---------------------------------------------------------------------*/
AP4_Tx3gCue::AP4_Tx3gCue(const AP4_Tx3gCue& other) :
    m_Text(other.m_Text),
    m_CharacterCount(other.m_CharacterCount),
    m_Runs(other.m_Runs),
    m_HorizJustification(other.m_HorizJustification),
    m_VertJustification(other.m_VertJustification),
    m_BackgroundColor(other.m_BackgroundColor),
    m_Cts(other.m_Cts),
    m_Duration(other.m_Duration)
{
    for (unsigned int i = 0; i < 4; i++) {
        m_DefaultTextBox[i] = other.m_DefaultTextBox[i];
    }
}

/*----------------------------------------------------------------------
|   AP4_Tx3gCue::operator=
+---------------------------------------------------------------------*/
AP4_Tx3gCue&
AP4_Tx3gCue::operator=(const AP4_Tx3gCue& other)
{
    if (this == &other) return *this;

    if (other.m_Text.GetDataSize() > 0) {
        m_Text.SetData(other.m_Text.GetData(), other.m_Text.GetDataSize());
    } else {
        m_Text.SetDataSize(0);
    }
    m_CharacterCount    = other.m_CharacterCount;
    m_Runs              = other.m_Runs;
    m_HorizJustification = other.m_HorizJustification;
    m_VertJustification  = other.m_VertJustification;
    m_BackgroundColor    = other.m_BackgroundColor;
    for (unsigned int i = 0; i < 4; i++) {
        m_DefaultTextBox[i] = other.m_DefaultTextBox[i];
    }
    m_Cts      = other.m_Cts;
    m_Duration = other.m_Duration;
    return *this;
}

/*----------------------------------------------------------------------
|   EncodeUtf8
+---------------------------------------------------------------------*/
static AP4_Result
EncodeUtf8(AP4_UI32 cp, AP4_DataBuffer& out)
{
    AP4_Byte bytes[4];
    unsigned int count = 0;
    if (cp < 0x80) {
        bytes[0] = (AP4_Byte)cp;
        count = 1;
    } else if (cp < 0x800) {
        bytes[0] = (AP4_Byte)(0xC0 | (cp >> 6));
        bytes[1] = (AP4_Byte)(0x80 | (cp & 0x3F));
        count = 2;
    } else if (cp < 0x10000) {
        bytes[0] = (AP4_Byte)(0xE0 | (cp >> 12));
        bytes[1] = (AP4_Byte)(0x80 | ((cp >> 6) & 0x3F));
        bytes[2] = (AP4_Byte)(0x80 | (cp & 0x3F));
        count = 3;
    } else if (cp <= 0x10FFFF) {
        bytes[0] = (AP4_Byte)(0xF0 | (cp >> 18));
        bytes[1] = (AP4_Byte)(0x80 | ((cp >> 12) & 0x3F));
        bytes[2] = (AP4_Byte)(0x80 | ((cp >> 6) & 0x3F));
        bytes[3] = (AP4_Byte)(0x80 | (cp & 0x3F));
        count = 4;
    } else {
        return AP4_ERROR_INVALID_FORMAT;
    }
    return out.AppendData(bytes, count);
}

/*----------------------------------------------------------------------
|   DecodeUtf8
|   Validates strict UTF-8, strips a leading BOM and counts code points.
+---------------------------------------------------------------------*/
static AP4_Result
DecodeUtf8(const AP4_Byte*         bytes,
           AP4_Size                size,
           bool                    has_bom,
           AP4_DataBuffer&         out,
           AP4_Ordinal&            char_count)
{
    AP4_Size start = has_bom ? 3 : 0;
    if (has_bom && (size < 3 ||
                    bytes[0] != 0xEF || bytes[1] != 0xBB || bytes[2] != 0xBF)) {
        return AP4_ERROR_INVALID_FORMAT;
    }

    out.SetDataSize(0);
    char_count = 0;

    AP4_Size i = start;
    while (i < size) {
        AP4_Byte first = bytes[i];
        AP4_UI32 cp = 0;
        unsigned int extra = 0;
        if ((first & 0x80) == 0) {
            cp = first;
            extra = 0;
        } else if ((first & 0xE0) == 0xC0) {
            cp = first & 0x1F;
            extra = 1;
        } else if ((first & 0xF0) == 0xE0) {
            cp = first & 0x0F;
            extra = 2;
        } else if ((first & 0xF8) == 0xF0) {
            cp = first & 0x07;
            extra = 3;
        } else {
            return AP4_ERROR_INVALID_FORMAT; // stray continuation or invalid lead
        }

        if (i+1+extra > size) {
            return AP4_ERROR_NOT_ENOUGH_DATA; // truncated sequence
        }
        for (unsigned int j = 1; j <= extra; j++) {
            AP4_Byte next = bytes[i+j];
            if ((next & 0xC0) != 0x80) {
                return AP4_ERROR_INVALID_FORMAT;
            }
            cp = (cp << 6) | (next & 0x3F);
        }

        // reject overlong encodings, surrogates and out-of-range values
        if ((extra == 1 && cp < 0x80)   ||
            (extra == 2 && cp < 0x800)  ||
            (extra == 3 && cp < 0x10000)) {
            return AP4_ERROR_INVALID_FORMAT;
        }
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            return AP4_ERROR_INVALID_FORMAT;
        }
        if (cp > 0x10FFFF) {
            return AP4_ERROR_INVALID_FORMAT;
        }

        AP4_Result result = EncodeUtf8(cp, out);
        if (AP4_FAILED(result)) return result;
        ++char_count;
        i += 1+extra;
    }

    return AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   DecodeUtf16
|   Decodes UTF-16 (BE or LE, selected by the BOM), strips the BOM and
|   emits UTF-8. Surrogate pairs are kept intact.
+---------------------------------------------------------------------*/
static AP4_Result
DecodeUtf16(const AP4_Byte*         bytes,
            AP4_Size                size,
            bool                    big_endian,
            AP4_DataBuffer&         out,
            AP4_Ordinal&            char_count)
{
    if (size < 2 || (size % 2) != 0) {
        return AP4_ERROR_INVALID_FORMAT;
    }

    out.SetDataSize(0);
    char_count = 0;

    AP4_Size units = size/2;
    AP4_Size first = 0;
    AP4_UI16 bom = big_endian
        ? (AP4_UI16)((bytes[0] << 8) | bytes[1])
        : (AP4_UI16)((bytes[1] << 8) | bytes[0]);
    if (bom == 0xFEFF) {
        first = 1; // BOM does not enter the result
    }

    for (AP4_Size u = first; u < units; u++) {
        AP4_UI16 unit = big_endian
            ? (AP4_UI16)((bytes[2*u] << 8) | bytes[2*u+1])
            : (AP4_UI16)((bytes[2*u+1] << 8) | bytes[2*u]);

        AP4_UI32 cp = unit;
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            // high surrogate: a low surrogate must follow
            if (u+1 >= units) {
                return AP4_ERROR_INVALID_FORMAT;
            }
            AP4_UI16 next = big_endian
                ? (AP4_UI16)((bytes[2*(u+1)] << 8) | bytes[2*(u+1)+1])
                : (AP4_UI16)((bytes[2*(u+1)+1] << 8) | bytes[2*(u+1)]);
            if (next < 0xDC00 || next > 0xDFFF) {
                return AP4_ERROR_INVALID_FORMAT;
            }
            cp = 0x10000 + (((AP4_UI32)(unit - 0xD800)) << 10)
                          + (AP4_UI32)(next - 0xDC00);
            u++;
        } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
            // lone low surrogate
            return AP4_ERROR_INVALID_FORMAT;
        }

        AP4_Result result = EncodeUtf8(cp, out);
        if (AP4_FAILED(result)) return result;
        ++char_count;
    }

    return AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   StylesEqual
+---------------------------------------------------------------------*/
static bool
StylesEqual(const AP4_Tx3gStyleRecord& a, const AP4_Tx3gStyleRecord& b)
{
    return a.m_FontId         == b.m_FontId &&
           a.m_FontStyleFlags == b.m_FontStyleFlags &&
           a.m_FontSize       == b.m_FontSize &&
           a.m_Color.r        == b.m_Color.r &&
           a.m_Color.g        == b.m_Color.g &&
           a.m_Color.b        == b.m_Color.b &&
           a.m_Color.a        == b.m_Color.a;
}

/*----------------------------------------------------------------------
|   AP4_Tx3gDecoder::Decode
+---------------------------------------------------------------------*/
AP4_Result
AP4_Tx3gDecoder::Decode(AP4_Track&   track,
                        AP4_Ordinal  index,
                        AP4_Tx3gCue& cue)
{
    // resolve the sample and its active description
    AP4_Sample sample;
    AP4_Result result = track.GetSample(index, sample);
    if (AP4_FAILED(result)) return result;

    AP4_SampleDescription* description =
        track.GetSampleDescription(sample.GetDescriptionIndex());
    AP4_Tx3gSampleDescription* tx3g =
        AP4_DYNAMIC_CAST(AP4_Tx3gSampleDescription, description);
    if (tx3g == NULL) {
        return AP4_ERROR_NOT_SUPPORTED;
    }

    AP4_DataBuffer data;
    result = sample.ReadData(data);
    if (AP4_FAILED(result)) return result;

    const AP4_Byte* bytes = data.GetData();
    AP4_Size        size  = data.GetDataSize();

    // decode into a local cue so the caller's cue is untouched on failure
    AP4_Tx3gCue local;

    // the text payload is introduced by a 16-bit length
    if (size < 2) {
        return AP4_ERROR_NOT_ENOUGH_DATA;
    }
    AP4_Size text_len = (AP4_Size)(((AP4_Size)bytes[0] << 8) | bytes[1]);
    AP4_Size pos = 2;
    if (text_len > size-pos) {
        return AP4_ERROR_NOT_ENOUGH_DATA;
    }
    const AP4_Byte* text_bytes = bytes+pos;

    // detect the encoding (BOM) or assume strict UTF-8
    if (text_len >= 2 && text_bytes[0] == 0xFE && text_bytes[1] == 0xFF) {
        result = DecodeUtf16(text_bytes, text_len, true,
                             local.m_Text, local.m_CharacterCount);
        if (AP4_FAILED(result)) return result;
    } else if (text_len >= 2 && text_bytes[0] == 0xFF && text_bytes[1] == 0xFE) {
        result = DecodeUtf16(text_bytes, text_len, false,
                             local.m_Text, local.m_CharacterCount);
        if (AP4_FAILED(result)) return result;
    } else {
        bool has_bom = (text_len >= 3 &&
                        text_bytes[0] == 0xEF &&
                        text_bytes[1] == 0xBB &&
                        text_bytes[2] == 0xBF);
        result = DecodeUtf8(text_bytes, text_len, has_bom,
                            local.m_Text, local.m_CharacterCount);
        if (AP4_FAILED(result)) return result;
    }
    AP4_Ordinal char_count = local.m_CharacterCount;
    pos += text_len;

    // the default style's font must resolve through this description's ftab
    if (tx3g->FindFontName(tx3g->GetDefaultStyle().m_FontId) == NULL) {
        return AP4_ERROR_NO_SUCH_ITEM;
    }

    // every codepoint starts with the complete default style
    AP4_Array<const AP4_Tx3gStyleRecord*> active;
    active.SetItemCount(char_count);
    for (AP4_Ordinal i = 0; i < char_count; i++) {
        active[i] = &tx3g->GetDefaultStyle();
    }
    AP4_Array<AP4_Tx3gStyleRecord> overrides;

    // parse the remaining boxes (styl and any other extension boxes)
    while (pos < size) {
        if (size-pos < AP4_ATOM_HEADER_SIZE) {
            return AP4_ERROR_NOT_ENOUGH_DATA;
        }
        AP4_UI32 box_size = ((AP4_UI32)bytes[pos]   << 24) |
                            ((AP4_UI32)bytes[pos+1] << 16) |
                            ((AP4_UI32)bytes[pos+2] <<  8) |
                            ((AP4_UI32)bytes[pos+3]);
        AP4_UI32 box_type = ((AP4_UI32)bytes[pos+4] << 24) |
                            ((AP4_UI32)bytes[pos+5] << 16) |
                            ((AP4_UI32)bytes[pos+6] <<  8) |
                            ((AP4_UI32)bytes[pos+7]);
        if (box_size < AP4_ATOM_HEADER_SIZE || box_size > size-pos) {
            return AP4_ERROR_NOT_ENOUGH_DATA;
        }

        if (box_type == AP4_ATOM_TYPE_STYL) {
            AP4_Size payload = box_size-AP4_ATOM_HEADER_SIZE;
            const AP4_Byte* p = bytes+pos+AP4_ATOM_HEADER_SIZE;
            if (payload < 2) {
                return AP4_ERROR_NOT_ENOUGH_DATA; // incomplete styl
            }
            AP4_UI16 record_count = (AP4_UI16)(((AP4_UI16)p[0] << 8) | p[1]);
            AP4_Size needed = 2+(AP4_Size)record_count*AP4_TX3G_STYLE_RECORD_SIZE;
            if (needed > payload) {
                return AP4_ERROR_NOT_ENOUGH_DATA; // incomplete styl
            }
            for (AP4_Ordinal r = 0; r < record_count; r++) {
                const AP4_Byte* s = p+2+r*AP4_TX3G_STYLE_RECORD_SIZE;
                AP4_Tx3gStyleRecord rec;
                rec.m_StartChar      = (AP4_UI16)((s[0] << 8) | s[1]);
                rec.m_EndChar        = (AP4_UI16)((s[2] << 8) | s[3]);
                rec.m_FontId         = (AP4_UI16)((s[4] << 8) | s[5]);
                rec.m_FontStyleFlags = s[6];
                rec.m_FontSize       = s[7];
                rec.m_Color.r        = s[8];
                rec.m_Color.g        = s[9];
                rec.m_Color.b        = s[10];
                rec.m_Color.a        = s[11];

                // zero-length records are ignored entirely
                if (rec.m_StartChar == rec.m_EndChar) continue;

                // reverse ranges and ranges past the text are errors
                if (rec.m_StartChar > rec.m_EndChar ||
                    rec.m_EndChar   > char_count) {
                    return AP4_ERROR_OUT_OF_RANGE;
                }

                // the font must exist in this description's ftab
                if (tx3g->FindFontName(rec.m_FontId) == NULL) {
                    return AP4_ERROR_NO_SUCH_ITEM;
                }

                overrides.Append(rec);
            }
        }
        // unknown extension boxes are skipped by size, their bytes never
        // become part of the decoded text

        pos += box_size;
    }

    // apply the styl records in their on-file order: later overlapping
    // records win, everything not covered keeps the default style
    for (AP4_Ordinal r = 0; r < overrides.ItemCount(); r++) {
        const AP4_Tx3gStyleRecord& rec = overrides[r];
        for (AP4_Ordinal i = rec.m_StartChar; i < rec.m_EndChar; i++) {
            active[i] = &overrides[r];
        }
    }

    // build the merged, text-ordered, non-overlapping runs
    for (AP4_Ordinal i = 0; i < char_count; ) {
        const AP4_Tx3gStyleRecord* style = active[i];
        AP4_Ordinal start = i;
        while (i < char_count && active[i] == style) {
            ++i;
        }
        // equal-pointer stretches already share every attribute; also merge
        // across different records that happen to carry identical styles
        if (local.m_Runs.ItemCount() > 0) {
            AP4_Tx3gCue::Run& previous =
                local.m_Runs[local.m_Runs.ItemCount()-1];
            const AP4_String* font = tx3g->FindFontName(style->m_FontId);
            if (previous.m_EndChar == start &&
                StylesEqual(previous.m_Style, *style) &&
                font != NULL &&
                previous.m_FontName == font->GetChars()) {
                previous.m_EndChar = i;
                previous.m_Style.m_EndChar = (AP4_UI16)i;
                continue;
            }
        }
        AP4_Tx3gCue::Run run;
        run.m_StartChar = start;
        run.m_EndChar   = i;
        run.m_Style     = *style;
        run.m_Style.m_StartChar = (AP4_UI16)start;
        run.m_Style.m_EndChar   = (AP4_UI16)i;
        const AP4_String* font = tx3g->FindFontName(style->m_FontId);
        if (font) {
            run.m_FontName = font->GetChars();
        }
        local.m_Runs.Append(run);
    }

    // presentation defaults from the active sample description
    local.m_HorizJustification = tx3g->GetHorizontalJustification();
    local.m_VertJustification  = tx3g->GetVerticalJustification();
    local.m_BackgroundColor    = tx3g->GetBackgroundColor();
    tx3g->GetDefaultTextBox(local.m_DefaultTextBox[0],
                            local.m_DefaultTextBox[1],
                            local.m_DefaultTextBox[2],
                            local.m_DefaultTextBox[3]);

    // timing, expressed in the media (track) timescale
    local.m_Cts      = sample.GetCts();
    local.m_Duration = sample.GetDuration();

    // commit only after every step succeeded
    cue = local;
    return AP4_SUCCESS;
}
