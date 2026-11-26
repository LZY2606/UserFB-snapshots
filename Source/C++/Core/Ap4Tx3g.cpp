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

/*----------------------------------------------------------------------
|   includes
+---------------------------------------------------------------------*/
#include "Ap4Tx3g.h"
#include "Ap4Track.h"
#include "Ap4Sample.h"
#include "Ap4Utils.h"

/*----------------------------------------------------------------------
|   dynamic cast support
+---------------------------------------------------------------------*/
AP4_DEFINE_DYNAMIC_CAST_ANCHOR(AP4_Tx3gSampleDescription)
AP4_DEFINE_DYNAMIC_CAST_ANCHOR(AP4_Tx3gSampleEntry)

/*----------------------------------------------------------------------
|   local byte helpers (big endian)
+---------------------------------------------------------------------*/
static AP4_UI16
Tx3g_ReadUI16(const AP4_UI08* p)
{
    return (AP4_UI16)(((AP4_UI16)p[0] << 8) | p[1]);
}

static AP4_UI32
Tx3g_ReadUI32(const AP4_UI08* p)
{
    return (((AP4_UI32)p[0] << 24) |
            ((AP4_UI32)p[1] << 16) |
            ((AP4_UI32)p[2] << 8)  |
            ((AP4_UI32)p[3]));
}

static void
Tx3g_WriteUI16(AP4_DataBuffer& buf, AP4_Size& pos, AP4_UI16 value)
{
    AP4_UI08* d = buf.UseData();
    d[pos++] = (AP4_UI08)(value >> 8);
    d[pos++] = (AP4_UI08)(value & 0xFF);
}

static void
Tx3g_WriteUI32(AP4_DataBuffer& buf, AP4_Size& pos, AP4_UI32 value)
{
    AP4_UI08* d = buf.UseData();
    d[pos++] = (AP4_UI08)(value >> 24);
    d[pos++] = (AP4_UI08)(value >> 16);
    d[pos++] = (AP4_UI08)(value >> 8);
    d[pos++] = (AP4_UI08)(value & 0xFF);
}

static void
Tx3g_WriteUI08(AP4_DataBuffer& buf, AP4_Size& pos, AP4_UI08 value)
{
    buf.UseData()[pos++] = value;
}

static void
Tx3g_AppendBytes(AP4_DataBuffer& target, const AP4_UI08* bytes, AP4_Size size)
{
    if (size == 0 || bytes == NULL) return;
    AP4_Size old_size = target.GetDataSize();
    target.SetDataSize(old_size + size);
    AP4_CopyMemory(target.UseData() + old_size, bytes, size);
}

/*----------------------------------------------------------------------
|   ftab parsing / serialization
+---------------------------------------------------------------------*/
static AP4_Result
Tx3g_ParseFtab(const AP4_UI08* payload, AP4_Size payload_size,
               AP4_Array<AP4_Tx3gFontRecord>& fonts)
{
    if (payload_size < 2) return AP4_ERROR_NOT_ENOUGH_DATA;
    AP4_UI16 entry_count = Tx3g_ReadUI16(payload);
    AP4_Size pos = 2;
    for (AP4_UI16 i = 0; i < entry_count; ++i) {
        if (pos + 3 > payload_size) return AP4_ERROR_NOT_ENOUGH_DATA;
        AP4_UI16 font_id = Tx3g_ReadUI16(payload + pos);
        pos += 2;
        AP4_UI08 name_len = payload[pos++];
        if (pos + name_len > payload_size) return AP4_ERROR_NOT_ENOUGH_DATA;
        AP4_String name((const char*)(payload + pos), name_len);
        pos += name_len;
        AP4_Result r = fonts.Append(AP4_Tx3gFontRecord(font_id, name.GetChars()));
        if (AP4_FAILED(r)) return r;
    }
    return AP4_SUCCESS;
}

/* Build an 'ftab' box (including its 8 byte box header). */
static AP4_Result
Tx3g_BuildFtab(const AP4_Array<AP4_Tx3gFontRecord>& fonts,
               AP4_DataBuffer& box)
{
    AP4_Size total = 8 + 2;
    for (AP4_Cardinal i = 0; i < fonts.ItemCount(); ++i) {
        AP4_Size name_len = fonts[i].fontName.GetLength();
        if (name_len > 255) name_len = 255;
        total += 2 + 1 + name_len;
    }
    box.SetDataSize(total);
    AP4_Size pos = 0;
    Tx3g_WriteUI32(box, pos, total);
    Tx3g_WriteUI32(box, pos, AP4_ATOM_TYPE('f','t','a','b'));
    Tx3g_WriteUI16(box, pos, (AP4_UI16)fonts.ItemCount());
    for (AP4_Cardinal i = 0; i < fonts.ItemCount(); ++i) {
        Tx3g_WriteUI16(box, pos, fonts[i].fontID);
        AP4_Size name_len = fonts[i].fontName.GetLength();
        if (name_len > 255) name_len = 255;
        Tx3g_WriteUI08(box, pos, (AP4_UI08)name_len);
        const char* name = fonts[i].fontName.GetChars();
        for (AP4_Size j = 0; j < name_len; ++j) {
            Tx3g_WriteUI08(box, pos, (AP4_UI08)name[j]);
        }
    }
    return AP4_SUCCESS;
}

/*
 * Walk the child-box region of a tx3g sample entry: parse the first
 * 'ftab' box into fonts, and copy every other box verbatim into
 * other_boxes.
 */
static AP4_Result
Tx3g_SplitChildBoxes(const AP4_DataBuffer& child_data,
                     AP4_Array<AP4_Tx3gFontRecord>& fonts,
                     AP4_DataBuffer& other_boxes)
{
    const AP4_UI08* p = child_data.GetData();
    AP4_Size total = child_data.GetDataSize();
    AP4_Size pos = 0;
    while (pos < total) {
        if (total - pos < 8) return AP4_ERROR_NOT_ENOUGH_DATA;
        AP4_UI32 size32 = Tx3g_ReadUI32(p + pos);
        AP4_UI32 type   = Tx3g_ReadUI32(p + pos + 4);
        AP4_Size header_size = 8;
        AP4_UI64 box_size = size32;
        if (size32 == 1) {
            if (total - pos < 16) return AP4_ERROR_NOT_ENOUGH_DATA;
            box_size = (((AP4_UI64)Tx3g_ReadUI32(p + pos + 8)) << 32) |
                       Tx3g_ReadUI32(p + pos + 12);
            header_size = 16;
        } else if (size32 == 0) {
            box_size = total - pos;
        }
        if (box_size < header_size || pos + box_size > total) {
            return AP4_ERROR_INVALID_FORMAT;
        }
        AP4_Size box_end = pos + (AP4_Size)box_size;

        if (type == AP4_ATOM_TYPE('f','t','a','b')) {
            AP4_Result r = Tx3g_ParseFtab(p + pos + header_size,
                                          (AP4_Size)box_size - header_size,
                                          fonts);
            if (AP4_FAILED(r)) return r;
            /* ftab is regenerated on write, do not keep the raw bytes */
        } else {
            Tx3g_AppendBytes(other_boxes, p + pos, (AP4_Size)box_size);
        }
        pos = box_end;
    }
    return AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   UTF decoding
+---------------------------------------------------------------------*/
namespace {
struct Tx3gDecodedText {
    AP4_String                utf8;
    AP4_Array<AP4_UI32>        cp_start; /* byte offset of each code point,
                                            plus a final sentinel */
    AP4_UI32                  cp_count;
};
}

/* Validate strict UTF-8 and record code point byte offsets. */
static AP4_Result
Tx3g_DecodeUtf8(const AP4_UI08* p, AP4_Size size, Tx3gDecodedText& out)
{
    AP4_Array<AP4_UI32>& starts = out.cp_start;
    AP4_Size i = 0;
    while (i < size) {
        AP4_UI08 b = p[i];
        AP4_UI32 cp = 0;
        AP4_Size len = 0;
        AP4_UI32 min_cp = 0;
        if (b < 0x80) {
            len = 1; cp = b;
        } else if ((b & 0xE0) == 0xC0) {
            len = 2; cp = b & 0x1F; min_cp = 0x80;
        } else if ((b & 0xF0) == 0xE0) {
            len = 3; cp = b & 0x0F; min_cp = 0x800;
        } else if ((b & 0xF8) == 0xF0) {
            len = 4; cp = b & 0x07; min_cp = 0x10000;
        } else {
            return AP4_ERROR_INVALID_FORMAT;
        }
        if (i + len > size) return AP4_ERROR_INVALID_FORMAT;
        for (AP4_Size k = 1; k < len; ++k) {
            AP4_UI08 c = p[i + k];
            if ((c & 0xC0) != 0x80) return AP4_ERROR_INVALID_FORMAT;
            cp = (cp << 6) | (c & 0x3F);
        }
        if (cp < min_cp) return AP4_ERROR_INVALID_FORMAT; /* overlong */
        if (cp >= 0xD800 && cp <= 0xDFFF) return AP4_ERROR_INVALID_FORMAT; /* surrogates */
        if (cp > 0x10FFFF) return AP4_ERROR_INVALID_FORMAT;

        AP4_Result r = starts.Append((AP4_UI32)i);
        if (AP4_FAILED(r)) return r;
        i += len;
    }
    starts.Append((AP4_UI32)size);
    out.cp_count = starts.ItemCount() - 1;
    out.utf8.Assign((const char*)p, size);
    return AP4_SUCCESS;
}

/* Decode UTF-16 (BE or LE according to the BOM) into UTF-8. */
static AP4_Result
Tx3g_DecodeUtf16(const AP4_UI08* p, AP4_Size size, bool little_endian,
                 Tx3gDecodedText& out)
{
    if (size & 1) return AP4_ERROR_INVALID_FORMAT; /* odd trailing byte */
    AP4_Array<AP4_UI16> units;
    AP4_Cardinal unit_count = size / 2;
    units.SetItemCount(unit_count);
    for (AP4_Cardinal i = 0; i < unit_count; ++i) {
        AP4_UI16 u = little_endian
            ? (AP4_UI16)(p[2*i] | ((AP4_UI16)p[2*i+1] << 8))
            : Tx3g_ReadUI16(p + 2*i);
        units[i] = u;
    }

    AP4_DataBuffer utf8;
    AP4_Size utf8_pos = 0;
    AP4_Array<AP4_UI32>& starts = out.cp_start;

    AP4_Cardinal i = 0;
    while (i < unit_count) {
        AP4_UI32 cp = units[i];
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            /* high surrogate, must be followed by a low surrogate */
            if (i + 1 >= unit_count) return AP4_ERROR_INVALID_FORMAT;
            AP4_UI16 lo = units[i + 1];
            if (lo < 0xDC00 || lo > 0xDFFF) return AP4_ERROR_INVALID_FORMAT;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            i += 2;
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            /* lone low surrogate */
            return AP4_ERROR_INVALID_FORMAT;
        } else {
            ++i;
        }

        /* record the UTF-8 byte offset before encoding */
        AP4_Result r = starts.Append(utf8_pos);
        if (AP4_FAILED(r)) return r;

        AP4_UI08 encoded[4];
        AP4_Size enc_len = 0;
        if (cp < 0x80) {
            encoded[0] = (AP4_UI08)cp; enc_len = 1;
        } else if (cp < 0x800) {
            encoded[0] = (AP4_UI08)(0xC0 | (cp >> 6));
            encoded[1] = (AP4_UI08)(0x80 | (cp & 0x3F));
            enc_len = 2;
        } else if (cp < 0x10000) {
            encoded[0] = (AP4_UI08)(0xE0 | (cp >> 12));
            encoded[1] = (AP4_UI08)(0x80 | ((cp >> 6) & 0x3F));
            encoded[2] = (AP4_UI08)(0x80 | (cp & 0x3F));
            enc_len = 3;
        } else {
            encoded[0] = (AP4_UI08)(0xF0 | (cp >> 18));
            encoded[1] = (AP4_UI08)(0x80 | ((cp >> 12) & 0x3F));
            encoded[2] = (AP4_UI08)(0x80 | ((cp >> 6) & 0x3F));
            encoded[3] = (AP4_UI08)(0x80 | (cp & 0x3F));
            enc_len = 4;
        }
        AP4_Size old = utf8.GetDataSize();
        utf8.SetDataSize(old + enc_len);
        AP4_CopyMemory(utf8.UseData() + old, encoded, enc_len);
        utf8_pos += enc_len;
    }
    starts.Append(utf8_pos);
    out.cp_count = starts.ItemCount() - 1;
    out.utf8.Assign((const char*)utf8.GetData(), utf8_pos);
    return AP4_SUCCESS;
}

/* Decode the length-prefixed text, detecting a UTF-16 BOM. */
static AP4_Result
Tx3g_DecodeTextBytes(const AP4_UI08* text, AP4_Size text_size,
                     Tx3gDecodedText& out)
{
    if (text_size >= 2 && text[0] == 0xFE && text[1] == 0xFF) {
        /* UTF-16 big endian BOM, BOM excluded from the result */
        return Tx3g_DecodeUtf16(text + 2, text_size - 2, false, out);
    }
    if (text_size >= 2 && text[0] == 0xFF && text[1] == 0xFE) {
        /* UTF-16 little endian BOM */
        return Tx3g_DecodeUtf16(text + 2, text_size - 2, true, out);
    }
    /* strict UTF-8 (also covers plain ASCII and empty text) */
    return Tx3g_DecodeUtf8(text, text_size, out);
}

/*----------------------------------------------------------------------
|   AP4_Tx3gSampleDescription
+---------------------------------------------------------------------*/
AP4_Tx3gSampleDescription::AP4_Tx3gSampleDescription(
    AP4_UI32                             format,
    const AP4_Tx3gStyleRecord&          default_style,
    int                                  horizontal_justification,
    int                                  vertical_justification,
    const AP4_Tx3gColor&                 background_rgba,
    const AP4_Tx3gBox&                   default_text_box,
    const AP4_Array<AP4_Tx3gFontRecord>* fonts,
    const AP4_DataBuffer*                other_boxes) :
    AP4_SampleDescription(TYPE_SUBTITLES, format, NULL),
    m_DisplayFlags(0),
    m_HorizJustification(horizontal_justification),
    m_VertJustification(vertical_justification),
    m_BackgroundColor(background_rgba),
    m_DefaultTextBox(default_text_box),
    m_DefaultStyle(default_style)
{
    m_BackgroundColorArray[0] = background_rgba.r;
    m_BackgroundColorArray[1] = background_rgba.g;
    m_BackgroundColorArray[2] = background_rgba.b;
    m_BackgroundColorArray[3] = background_rgba.a;
    if (fonts) {
        for (AP4_Cardinal i = 0; i < fonts->ItemCount(); ++i) {
            m_Fonts.Append((*fonts)[i]);
        }
    }
    if (other_boxes && other_boxes->GetDataSize()) {
        m_OtherBoxes.SetData(other_boxes->GetData(), other_boxes->GetDataSize());
    }
}

AP4_Tx3gSampleDescription::AP4_Tx3gSampleDescription(
    AP4_UI32                          format,
    AP4_UI32                          display_flags,
    int                               horizontal_justification,
    int                               vertical_justification,
    const AP4_Tx3gColor&              background_rgba,
    const AP4_Tx3gBox&                default_text_box,
    const AP4_Tx3gStyleRecord&        default_style,
    AP4_Array<AP4_Tx3gFontRecord>&    fonts,
    AP4_DataBuffer&                   other_boxes) :
    AP4_SampleDescription(TYPE_SUBTITLES, format, NULL),
    m_DisplayFlags(display_flags),
    m_HorizJustification(horizontal_justification),
    m_VertJustification(vertical_justification),
    m_BackgroundColor(background_rgba),
    m_DefaultTextBox(default_text_box),
    m_DefaultStyle(default_style)
{
    m_BackgroundColorArray[0] = background_rgba.r;
    m_BackgroundColorArray[1] = background_rgba.g;
    m_BackgroundColorArray[2] = background_rgba.b;
    m_BackgroundColorArray[3] = background_rgba.a;
    for (AP4_Cardinal i = 0; i < fonts.ItemCount(); ++i) {
        m_Fonts.Append(fonts[i]);
    }
    if (other_boxes.GetDataSize()) {
        m_OtherBoxes.SetData(other_boxes.GetData(), other_boxes.GetDataSize());
    }
}

AP4_SampleDescription*
AP4_Tx3gSampleDescription::Clone(AP4_Result* result)
{
    return AP4_SampleDescription::Clone(result);
}

AP4_Atom*
AP4_Tx3gSampleDescription::ToAtom() const
{
    return new AP4_Tx3gSampleEntry(m_Format,
                                   m_DisplayFlags,
                                   m_HorizJustification,
                                   m_VertJustification,
                                   m_BackgroundColor,
                                   m_DefaultTextBox,
                                   m_DefaultStyle,
                                   m_Fonts,
                                   &m_OtherBoxes);
}

AP4_Tx3gStyle
AP4_Tx3gSampleDescription::GetDefaultStyle() const
{
    AP4_Tx3gStyle style;
    style.fontID = style.fontId = m_DefaultStyle.fontID;
    style.SetFlags(m_DefaultStyle.faceStyleFlags);
    style.fontSize = m_DefaultStyle.fontSize;
    style.SetColor(m_DefaultStyle.textColorRGBA[0],
                   m_DefaultStyle.textColorRGBA[1],
                   m_DefaultStyle.textColorRGBA[2],
                   m_DefaultStyle.textColorRGBA[3]);
    AP4_String name;
    if (AP4_SUCCEEDED(FindFontName(m_DefaultStyle.fontID, name))) {
        style.fontName = name;
    }
    return style;
}

bool
AP4_Tx3gSampleDescription::HasFont(AP4_UI16 font_id) const
{
    for (AP4_Cardinal i = 0; i < m_Fonts.ItemCount(); ++i) {
        if (m_Fonts[i].fontID == font_id) return true;
    }
    return false;
}

AP4_Result
AP4_Tx3gSampleDescription::FindFontName(AP4_UI16 font_id, AP4_String& name) const
{
    for (AP4_Cardinal i = 0; i < m_Fonts.ItemCount(); ++i) {
        if (m_Fonts[i].fontID == font_id) {
            name = m_Fonts[i].fontName;
            return AP4_SUCCESS;
        }
    }
    return AP4_ERROR_NO_SUCH_ITEM;
}

/*----------------------------------------------------------------------
|   AP4_Tx3gSampleEntry
+---------------------------------------------------------------------*/
AP4_Tx3gSampleEntry::AP4_Tx3gSampleEntry(AP4_Size         size,
                                         AP4_ByteStream&  stream,
                                         AP4_AtomFactory& atom_factory) :
    AP4_SampleEntry(AP4_SAMPLE_FORMAT_TX3G, size),
    m_DisplayFlags(0),
    m_HorizJustification(0),
    m_VertJustification(0)
{
    m_BackgroundColor.Set(0, 0, 0, 0);
    Read(stream, atom_factory);
}

AP4_Tx3gSampleEntry::AP4_Tx3gSampleEntry(
    AP4_UI32                            format,
    AP4_UI32                            display_flags,
    int                                 horiz_justification,
    int                                 vert_justification,
    const AP4_Tx3gColor&                background_color,
    const AP4_Tx3gBox&                  text_box,
    const AP4_Tx3gStyleRecord&          default_style,
    const AP4_Array<AP4_Tx3gFontRecord>& fonts,
    const AP4_DataBuffer*               other_boxes) :
    AP4_SampleEntry(format),
    m_DisplayFlags(display_flags),
    m_HorizJustification(horiz_justification),
    m_VertJustification(vert_justification),
    m_BackgroundColor(background_color),
    m_TextBox(text_box),
    m_DefaultStyle(default_style)
{
    /* serialize the ftab box */
    AP4_DataBuffer ftab;
    Tx3g_BuildFtab(fonts, ftab);
    Tx3g_AppendBytes(m_ChildData, ftab.GetData(), ftab.GetDataSize());

    /* append preserved extension boxes */
    if (other_boxes && other_boxes->GetDataSize()) {
        Tx3g_AppendBytes(m_ChildData, other_boxes->GetData(),
                         other_boxes->GetDataSize());
    }

    /* account for the 30 fixed tx3g bytes and child region */
    m_Size32 += 30 + m_ChildData.GetDataSize();
}

AP4_Size
AP4_Tx3gSampleEntry::GetFieldsSize()
{
    return AP4_SampleEntry::GetFieldsSize() + 30 + m_ChildData.GetDataSize();
}

AP4_Result
AP4_Tx3gSampleEntry::ReadFields(AP4_ByteStream& stream)
{
    AP4_Result result = AP4_SampleEntry::ReadFields(stream);
    if (AP4_FAILED(result)) return result;

    /* displayFlags */
    result = stream.ReadUI32(m_DisplayFlags);
    if (AP4_FAILED(result)) return result;

    /* horizontal / vertical justification (signed bytes) */
    AP4_UI08 hj = 0, vj = 0;
    result = stream.ReadUI08(hj);
    if (AP4_FAILED(result)) return result;
    result = stream.ReadUI08(vj);
    if (AP4_FAILED(result)) return result;
    m_HorizJustification = (hj < 0x80) ? (int)hj : (int)hj - 0x100;
    m_VertJustification  = (vj < 0x80) ? (int)vj : (int)vj - 0x100;

    /* background color RGBA */
    AP4_UI08 bg[4];
    result = stream.Read(bg, 4);
    if (AP4_FAILED(result)) return result;
    m_BackgroundColor.Set(bg[0], bg[1], bg[2], bg[3]);

    /* default BoxRecord: top, left, bottom, right (signed 16 bit) */
    AP4_UI16 top = 0, left = 0, bottom = 0, right = 0;
    result = stream.ReadUI16(top);    if (AP4_FAILED(result)) return result;
    result = stream.ReadUI16(left);   if (AP4_FAILED(result)) return result;
    result = stream.ReadUI16(bottom); if (AP4_FAILED(result)) return result;
    result = stream.ReadUI16(right);  if (AP4_FAILED(result)) return result;
    m_TextBox.Set((AP4_SI16)top, (AP4_SI16)left,
                  (AP4_SI16)bottom, (AP4_SI16)right);

    /* default StyleRecord (12 bytes) */
    result = stream.ReadUI16(m_DefaultStyle.startChar); if (AP4_FAILED(result)) return result;
    result = stream.ReadUI16(m_DefaultStyle.endChar);   if (AP4_FAILED(result)) return result;
    result = stream.ReadUI16(m_DefaultStyle.fontID);    if (AP4_FAILED(result)) return result;
    result = stream.ReadUI08(m_DefaultStyle.faceStyleFlags); if (AP4_FAILED(result)) return result;
    result = stream.ReadUI08(m_DefaultStyle.fontSize);       if (AP4_FAILED(result)) return result;
    result = stream.Read(m_DefaultStyle.textColorRGBA, 4);
    if (AP4_FAILED(result)) return result;

    /* everything left in this atom is the ftab + extension box region */
    AP4_UI64 atom_size = GetSize();
    AP4_UI32 header_size = GetHeaderSize();
    AP4_SI64 child_size = (AP4_SI64)atom_size - (AP4_SI64)header_size
                          - (AP4_SI64)AP4_SampleEntry::GetFieldsSize() - 30;
    if (child_size > 0) {
        m_ChildData.SetDataSize((AP4_Size)child_size);
        result = stream.Read(m_ChildData.UseData(), (AP4_Size)child_size);
        if (AP4_FAILED(result)) return result;
    }

    return AP4_SUCCESS;
}

AP4_Result
AP4_Tx3gSampleEntry::WriteFields(AP4_ByteStream& stream)
{
    AP4_Result result = AP4_SampleEntry::WriteFields(stream);
    if (AP4_FAILED(result)) return result;

    result = stream.WriteUI32(m_DisplayFlags);
    if (AP4_FAILED(result)) return result;

    result = stream.WriteUI08((AP4_UI08)(m_HorizJustification & 0xFF));
    if (AP4_FAILED(result)) return result;
    result = stream.WriteUI08((AP4_UI08)(m_VertJustification & 0xFF));
    if (AP4_FAILED(result)) return result;

    AP4_UI08 bg[4] = {
        m_BackgroundColor.r, m_BackgroundColor.g,
        m_BackgroundColor.b, m_BackgroundColor.a
    };
    result = stream.Write(bg, 4);
    if (AP4_FAILED(result)) return result;

    result = stream.WriteUI16((AP4_UI16)m_TextBox.top);
    if (AP4_FAILED(result)) return result;
    result = stream.WriteUI16((AP4_UI16)m_TextBox.left);
    if (AP4_FAILED(result)) return result;
    result = stream.WriteUI16((AP4_UI16)m_TextBox.bottom);
    if (AP4_FAILED(result)) return result;
    result = stream.WriteUI16((AP4_UI16)m_TextBox.right);
    if (AP4_FAILED(result)) return result;

    result = stream.WriteUI16(m_DefaultStyle.startChar);
    if (AP4_FAILED(result)) return result;
    result = stream.WriteUI16(m_DefaultStyle.endChar);
    if (AP4_FAILED(result)) return result;
    result = stream.WriteUI16(m_DefaultStyle.fontID);
    if (AP4_FAILED(result)) return result;
    result = stream.WriteUI08(m_DefaultStyle.faceStyleFlags);
    if (AP4_FAILED(result)) return result;
    result = stream.WriteUI08(m_DefaultStyle.fontSize);
    if (AP4_FAILED(result)) return result;
    result = stream.Write(m_DefaultStyle.textColorRGBA, 4);
    if (AP4_FAILED(result)) return result;

    if (m_ChildData.GetDataSize()) {
        result = stream.Write(m_ChildData.GetData(), m_ChildData.GetDataSize());
        if (AP4_FAILED(result)) return result;
    }

    return AP4_SUCCESS;
}

AP4_SampleDescription*
AP4_Tx3gSampleEntry::ToSampleDescription()
{
    AP4_Array<AP4_Tx3gFontRecord> fonts;
    AP4_DataBuffer other_boxes;
    /* splitting is best effort: a malformed ftab simply yields no fonts */
    Tx3g_SplitChildBoxes(m_ChildData, fonts, other_boxes);

    return new AP4_Tx3gSampleDescription(m_Type,
                                         m_DisplayFlags,
                                         m_HorizJustification,
                                         m_VertJustification,
                                         m_BackgroundColor,
                                         m_TextBox,
                                         m_DefaultStyle,
                                         fonts,
                                         other_boxes);
}

/*----------------------------------------------------------------------
|   styl parsing / style layering
+---------------------------------------------------------------------*/
/*
 * Parse a 'styl' box and apply its records (in order) on top of the
 * per-code-point styles. Zero length records are ignored.
 */
static AP4_Result
Tx3g_ApplyStyl(const AP4_UI08* styl_box, AP4_Size styl_box_size,
               AP4_UI32 cp_count,
               const AP4_Tx3gSampleDescription* description,
               AP4_Array<AP4_Tx3gStyle>& cp_styles)
{
    if (styl_box_size < 8 + 2) return AP4_ERROR_INVALID_FORMAT;
    /* styl_box points at the box header */
    AP4_UI16 entry_count = Tx3g_ReadUI16(styl_box + 8);
    AP4_Size records_end = 8 + 2 + (AP4_Size)entry_count * 12;
    if (records_end > styl_box_size) {
        /* incomplete styl: declared records do not fit in the box */
        return AP4_ERROR_INVALID_FORMAT;
    }

    AP4_Size pos = 10;
    for (AP4_UI16 i = 0; i < entry_count; ++i) {
        AP4_Tx3gStyleRecord rec;
        rec.startChar       = Tx3g_ReadUI16(styl_box + pos);
        rec.endChar         = Tx3g_ReadUI16(styl_box + pos + 2);
        rec.fontID          = Tx3g_ReadUI16(styl_box + pos + 4);
        rec.faceStyleFlags  = styl_box[pos + 6];
        rec.fontSize        = styl_box[pos + 7];
        rec.textColorRGBA[0] = styl_box[pos + 8];
        rec.textColorRGBA[1] = styl_box[pos + 9];
        rec.textColorRGBA[2] = styl_box[pos + 10];
        rec.textColorRGBA[3] = styl_box[pos + 11];
        pos += 12;

        /* reversed range */
        if (rec.startChar > rec.endChar) {
            return AP4_ERROR_INVALID_FORMAT;
        }
        /* zero length record: ignore completely */
        if (rec.startChar == rec.endChar) {
            continue;
        }
        /* range must fit within the decoded text */
        if (rec.endChar > cp_count) {
            return AP4_ERROR_OUT_OF_RANGE;
        }

        /* resolve the font against the current sample description */
        AP4_String font_name;
        AP4_Result fr = description
                      ? description->FindFontName(rec.fontID, font_name)
                      : AP4_ERROR_INVALID_STATE;
        if (AP4_FAILED(fr)) return AP4_ERROR_NO_SUCH_ITEM;

        AP4_Tx3gStyle style;
        style.fontID = style.fontId = rec.fontID;
        style.fontName = font_name;
        style.SetFlags(rec.faceStyleFlags);
        style.fontSize = rec.fontSize;
        style.SetColor(rec.textColorRGBA[0], rec.textColorRGBA[1],
                       rec.textColorRGBA[2], rec.textColorRGBA[3]);

        for (AP4_UI32 c = rec.startChar; c < rec.endChar; ++c) {
            cp_styles[c] = style;
        }
    }
    return AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   AP4_Tx3gDecoder::DecodeBytes
+---------------------------------------------------------------------*/
AP4_Result
AP4_Tx3gDecoder::DecodeBytes(const AP4_UI08*                  data,
                             AP4_Size                         data_size,
                             const AP4_Tx3gSampleDescription* description,
                             AP4_UI64                         cts,
                             AP4_UI32                         duration,
                             AP4_Tx3gCue&                     cue)
{
    if (data == NULL || data_size < 2) {
        return AP4_ERROR_NOT_ENOUGH_DATA;
    }

    /* 16 bit length of the text byte range */
    AP4_UI16 text_byte_length = Tx3g_ReadUI16(data);
    if ((AP4_Size)2 + text_byte_length > data_size) {
        return AP4_ERROR_NOT_ENOUGH_DATA;
    }
    const AP4_UI08* text_bytes = data + 2;

    /* decode the text (strict UTF-8 or BOM-marked UTF-16) */
    Tx3gDecodedText decoded;
    AP4_Result result = Tx3g_DecodeTextBytes(text_bytes,
                                             text_byte_length,
                                             decoded);
    if (AP4_FAILED(result)) return result;
    AP4_UI32 cp_count = decoded.cp_count;

    /* start building the output cue in a temporary object */
    AP4_Tx3gCue out;
    out.text = decoded.utf8;
    out.codepointCount = cp_count;
    out.cts = cts;
    out.duration = duration;

    if (description) {
        out.textBox = description->GetDefaultTextBox();
        out.horizontalAlignment = description->GetHorizontalJustification();
        out.verticalAlignment   = description->GetVerticalJustification();
        out.backgroundColor    = description->GetBackgroundColor();
    }
    out.textBox.SyncAliases();

    /* per-code-point effective styles, initialized to the default style */
    AP4_Array<AP4_Tx3gStyle> cp_styles;
    AP4_Tx3gStyle default_style;
    if (description) {
        default_style = description->GetDefaultStyle();
        /* the default font must exist if it is going to be used */
        if (cp_count > 0 &&
            default_style.fontName.GetLength() == 0 &&
            description->HasFont(description->GetDefaultStyleRecord().fontID) == false) {
            return AP4_ERROR_NO_SUCH_ITEM;
        }
    }
    out.defaultStyle = default_style;

    if (cp_count > 0) {
        result = cp_styles.SetItemCount(cp_count);
        if (AP4_FAILED(result)) return result;

        for (AP4_UI32 c = 0; c < cp_count; ++c) {
            cp_styles[c] = default_style;
        }
    }

    /* walk the sample modifier boxes; only 'styl' changes the output */
    AP4_Size pos = (AP4_Size)2 + text_byte_length;
    while (pos < data_size) {
        if (data_size - pos < 8) {
            return AP4_ERROR_NOT_ENOUGH_DATA;
        }
        AP4_UI32 size32 = Tx3g_ReadUI32(data + pos);
        AP4_UI32 type   = Tx3g_ReadUI32(data + pos + 4);
        AP4_Size header_size = 8;
        AP4_UI64 box_size = size32;
        if (size32 == 1) {
            if (data_size - pos < 16) return AP4_ERROR_NOT_ENOUGH_DATA;
            box_size = (((AP4_UI64)Tx3g_ReadUI32(data + pos + 8)) << 32) |
                       Tx3g_ReadUI32(data + pos + 12);
            header_size = 16;
        } else if (size32 == 0) {
            box_size = data_size - pos;
        }
        if (box_size < (AP4_UI64)header_size) {
            return AP4_ERROR_INVALID_FORMAT;
        }
        /* overflow-safe fit check against the end of the sample */
        if (box_size > (AP4_UI64)(data_size - pos)) {
            return AP4_ERROR_NOT_ENOUGH_DATA;
        }
        AP4_Size box_size32 = (AP4_Size)box_size;

        if (type == AP4_ATOM_TYPE('s','t','y','l')) {
            result = Tx3g_ApplyStyl(data + pos, box_size32,
                                    cp_count, description, cp_styles);
            if (AP4_FAILED(result)) return result;
        }
        /* every other box is skipped by its declared length */
        pos += box_size32;
    }

    /* merge adjacent code points with identical effective styles */
    if (cp_count > 0) {
        AP4_UI32 run_start = 0;
        for (AP4_UI32 c = 1; c <= cp_count; ++c) {
            bool same = (c < cp_count) &&
                        cp_styles[c].StyleEquals(cp_styles[run_start]);
            if (same) continue;

            AP4_Tx3gStyleRun run;
            static_cast<AP4_Tx3gStyle&>(run) = cp_styles[run_start];
            AP4_UI32 start_byte = decoded.cp_start[run_start];
            AP4_UI32 end_byte   = decoded.cp_start[c];
            run.SetRange(run_start, c, start_byte, end_byte);
            run.text.Assign(decoded.utf8.GetChars() + start_byte,
                            end_byte - start_byte);
            /* make sure spelling aliases agree */
            run.fontId = run.fontID;
            run.SetColor(run.color.r, run.color.g, run.color.b, run.color.a);
            /* keep the nested style view in sync */
            run.style = static_cast<const AP4_Tx3gStyle&>(run);
            out.runs.Append(run);

            run_start = c;
        }
    }

    /* propagate values to compatibility aliases */
    out.SyncAliases();

    /* commit only on full success: the caller's cue is untouched otherwise */
    cue = out;
    return AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   AP4_Tx3gDecoder::Decode
+---------------------------------------------------------------------*/
AP4_Result
AP4_Tx3gDecoder::Decode(AP4_Track&   track,
                        AP4_Ordinal  index,
                        AP4_Tx3gCue& cue)
{
    AP4_Sample sample;
    AP4_DataBuffer sample_data;
    AP4_Result result = track.ReadSample(index, sample, sample_data);
    if (AP4_FAILED(result)) return result;

    AP4_SampleDescription* sd =
        track.GetSampleDescription(sample.GetDescriptionIndex());
    AP4_Tx3gSampleDescription* tx3g_desc =
        sd ? AP4_DYNAMIC_CAST(AP4_Tx3gSampleDescription, sd) : NULL;

    /*
     * Parsed files and properly constructed descriptions provide the
     * typed AP4_Tx3gSampleDescription. As a lenient fallback, synthetic
     * generic descriptions carrying the tx3g format are decoded with
     * built-in defaults and no font table.
     */
    const AP4_Tx3gSampleDescription* desc = tx3g_desc;
    if (desc == NULL) {
        if (sd == NULL || sd->GetFormat() != AP4_SAMPLE_FORMAT_TX3G) {
            return AP4_ERROR_INVALID_TRACK_TYPE;
        }
        desc = NULL; /* DecodeBytes then uses built-in defaults */
    }

    return DecodeBytes(sample_data.GetData(),
                       sample_data.GetDataSize(),
                       desc,
                       sample.GetCts(),
                       sample.GetDuration(),
                       cue);
}
