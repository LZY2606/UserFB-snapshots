/*****************************************************************
|
|    AP4 - 3GPP Timed Text (tx3g) Tests
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
#include "Ap4.h"
#include "test_support.h"

/*----------------------------------------------------------------------
|   helpers
+---------------------------------------------------------------------*/
// AP4_MemoryByteStream has a protected destructor (it is ref-counted);
// this small derived type makes stack-allocated scratch buffers possible
class LocalMemoryStream : public AP4_MemoryByteStream {
public:
    LocalMemoryStream() : AP4_MemoryByteStream() {}
    ~LocalMemoryStream() {}
};

static AP4_Tx3gSampleEntry*
ParseEntry(AP4_ByteStream& stream)
{
    stream.Seek(0);
    AP4_AtomFactory factory;
    factory.PushContext(AP4_ATOM_TYPE_STSD);
    AP4_Atom* atom = NULL;
    AP4_Result result = factory.CreateAtomFromStream(stream, atom);
    factory.PopContext();
    CHECK_CHECK(AP4_SUCCEEDED(result));
    AP4_Tx3gSampleEntry* entry = AP4_DYNAMIC_CAST(AP4_Tx3gSampleEntry, atom);
    CHECK_CHECK(entry != NULL);
    return entry;
}

/*
 * Build a hand-written tx3g sample entry:
 *   default style font 1, bold, size 24, color (11,22,33,44)
 *   h-justify center, v-justify 0, background (10,20,30,40)
 *   text box (1,2,3,4), ftab {1:Serif, 2:Sans}, one unknown 'zzzz' box
 */
static AP4_MemoryByteStream*
BuildEntryBytes()
{
    AP4_MemoryByteStream* stream = new AP4_MemoryByteStream();

    // ftab box: 2 entries, Serif (id 1) and Sans (id 2)
    LocalMemoryStream ftab;
    ftab.WriteUI16(2);
    ftab.WriteUI16(1); ftab.WriteUI08(5); ftab.Write("Serif", 5);
    ftab.WriteUI16(2); ftab.WriteUI08(4); ftab.Write("Sans",  4);

    // unknown extension box with a 4-byte payload
    LocalMemoryStream unk;
    unk.WriteUI32(0xAABBCCDD);

    AP4_UI32 fixed_size = 30;
    AP4_UI32 entry_size = 8 + 8 + fixed_size +
                          8 + ftab.GetDataSize() +
                          8 + unk.GetDataSize();

    stream->WriteUI32(entry_size);
    stream->WriteUI32(AP4_ATOM_TYPE_TX3G);

    // base sample entry fields
    AP4_UI08 reserved[6] = {0,0,0,0,0,0};
    stream->Write(reserved, 6);
    stream->WriteUI16(1); // data reference index

    // tx3g fixed fields (30 bytes, per ISO 14496-17 TextSampleEntry)
    stream->WriteUI32(0);             // display flags
    stream->WriteUI08(1);             // horizontal justification (center)
    stream->WriteUI08(0);             // vertical justification
    stream->WriteUI08(10); stream->WriteUI08(20);
    stream->WriteUI08(30); stream->WriteUI08(40); // background RGBA
    stream->WriteUI16(1); stream->WriteUI16(2);   // text box top, left
    stream->WriteUI16(3); stream->WriteUI16(4);   // text box bottom, right

    // default StyleRecord
    stream->WriteUI16(0);             // startChar
    stream->WriteUI16(0xFFFF);        // endChar
    stream->WriteUI16(1);             // font id
    stream->WriteUI08(0x01);          // bold
    stream->WriteUI08(24);            // font size
    stream->WriteUI08(11); stream->WriteUI08(22);
    stream->WriteUI08(33); stream->WriteUI08(44); // text color RGBA

    // child boxes
    stream->WriteUI32(8+ftab.GetDataSize());
    stream->WriteUI32(AP4_ATOM_TYPE_FTAB);
    stream->Write(ftab.GetData(), ftab.GetDataSize());
    stream->WriteUI32(8+unk.GetDataSize());
    stream->WriteUI32(AP4_ATOM_TYPE('z','z','z','z'));
    stream->Write(unk.GetData(), unk.GetDataSize());

    return stream;
}

static void
WriteStyleRecord(AP4_ByteStream&           stream,
                 AP4_UI16                  start,
                 AP4_UI16                  end,
                 AP4_UI16                  font_id,
                 AP4_UI08                  flags,
                 AP4_UI08                  size,
                 const AP4_UI08            rgba[4])
{
    stream.WriteUI16(start);
    stream.WriteUI16(end);
    stream.WriteUI16(font_id);
    stream.WriteUI08(flags);
    stream.WriteUI08(size);
    stream.Write(rgba, 4);
}

/*
 * Sample: 16-bit text length, UTF-8 "hello", a styl box with two overlapping
 * records ([1,3) then [2,4)) and an unknown extension box whose payload must
 * never be appended to the text.
 */
static AP4_MemoryByteStream*
BuildOverlapSample()
{
    static const AP4_UI08 color1[4] = {200, 100, 50, 255};
    static const AP4_UI08 color2[4] = {1, 2, 3, 128};

    LocalMemoryStream styl;
    styl.WriteUI16(2); // two records
    WriteStyleRecord(styl, 1, 3, 2, 0x02, 20, color1); // italic, Sans
    WriteStyleRecord(styl, 2, 4, 1, 0x04, 22, color2); // underline, Serif

    AP4_MemoryByteStream* sample = new AP4_MemoryByteStream();
    sample->WriteUI16(5);
    sample->Write("hello", 5);
    sample->WriteUI32(8+styl.GetDataSize());
    sample->WriteUI32(AP4_ATOM_TYPE_STYL);
    sample->Write(styl.GetData(), styl.GetDataSize());
    sample->WriteUI32(12); // unknown box, 4 payload bytes
    sample->WriteUI32(AP4_ATOM_TYPE('z','z','z','z'));
    sample->WriteUI32(0xAABBCCDD);
    return sample;
}

/*
 * Decode raw sample bytes against a description, through a real
 * AP4_Track / AP4_SyntheticSampleTable pair.
 */
static AP4_Result
DecodeBytes(AP4_Tx3gSampleDescription& description,
            const AP4_UI08*            bytes,
            AP4_Size                   size,
            AP4_Tx3gCue&               cue)
{
    AP4_MemoryByteStream* stream = new AP4_MemoryByteStream(bytes, size);
    AP4_SyntheticSampleTable* table = new AP4_SyntheticSampleTable();
    // the description outlives the table, so no ownership transfer
    table->AddSampleDescription(&description, false);
    table->AddSample(*stream, 0, size, 2000, 0, 1000, 500, true);
    AP4_Track track(AP4_Track::TYPE_SUBTITLES, table, 1,
                    1000, 2000, 1000, 2000, "eng", 0, 0);
    stream->Release();
    return AP4_Tx3gDecoder::Decode(track, 0, cue);
}

static AP4_Result
DecodeStream(AP4_Tx3gSampleDescription& description,
             AP4_MemoryByteStream&      sample_stream,
             AP4_Tx3gCue&               cue)
{
    return DecodeBytes(description,
                       sample_stream.GetData(),
                       sample_stream.GetDataSize(),
                       cue);
}

/*----------------------------------------------------------------------
|   TestSampleEntryParsing
+---------------------------------------------------------------------*/
static void
TestSampleEntryParsing(AP4_Tx3gSampleDescription*& description_out)
{
    AP4_MemoryByteStream* bytes = BuildEntryBytes();
    AP4_Tx3gSampleEntry* entry = ParseEntry(*bytes);

    // fixed fields are parsed as fields, not as child boxes
    CHECK_CHECK(entry->GetHorizontalJustification() == AP4_TX3G_ALIGNMENT_CENTER);
    CHECK_CHECK(entry->GetVerticalJustification() == 0);
    const AP4_Tx3gColor& bg = entry->GetBackgroundColor();
    CHECK_CHECK(bg.r == 10 && bg.g == 20 && bg.b == 30 && bg.a == 40);
    AP4_UI32 top, left, bottom, right;
    entry->GetDefaultTextBox(top, left, bottom, right);
    CHECK_CHECK(top == 1 && left == 2 && bottom == 3 && right == 4);

    const AP4_Tx3gStyleRecord& style = entry->GetDefaultStyle();
    CHECK_CHECK(style.m_FontId == 1);
    CHECK_CHECK(style.m_FontStyleFlags == 0x01);
    CHECK_CHECK(style.m_FontSize == 24);
    CHECK_CHECK(style.m_Color.r == 11 && style.m_Color.g == 22 &&
                style.m_Color.b == 33 && style.m_Color.a == 44);

    // ftab font table
    const AP4_Array<AP4_Tx3gFontRecord>& fonts = entry->GetFontTable();
    CHECK_CHECK(fonts.ItemCount() == 2);
    CHECK_CHECK(fonts[0].m_FontId == 1 && fonts[0].m_Name == "Serif");
    CHECK_CHECK(fonts[1].m_FontId == 2 && fonts[1].m_Name == "Sans");
    CHECK_CHECK(entry->FindFontName(2) != NULL);
    CHECK_CHECK(*entry->FindFontName(2) == "Sans");
    CHECK_CHECK(entry->FindFontName(99) == NULL);

    // the ftab is a real typed child box, the unknown box is preserved
    AP4_Tx3gFtabAtom* ftab = AP4_DYNAMIC_CAST(AP4_Tx3gFtabAtom,
                                               entry->GetChild(AP4_ATOM_TYPE_FTAB));
    CHECK_CHECK(ftab != NULL);
    CHECK_CHECK(entry->GetChild(AP4_ATOM_TYPE('z','z','z','z')) != NULL);

    AP4_SampleDescription* base = entry->ToSampleDescription();
    AP4_Tx3gSampleDescription* tx3g =
        AP4_DYNAMIC_CAST(AP4_Tx3gSampleDescription, base);
    CHECK_CHECK(tx3g != NULL);
    CHECK_CHECK(tx3g->GetFormat() == AP4_SAMPLE_FORMAT_TX3G);
    CHECK_CHECK(tx3g->GetType() == AP4_SampleDescription::TYPE_UNKNOWN);
    CHECK_CHECK(tx3g->GetFontTable().ItemCount() == 2);

    description_out = tx3g;
    delete entry;
    bytes->Release();
}

/*----------------------------------------------------------------------
|   TestOverlappingStyles
+---------------------------------------------------------------------*/
static void
TestOverlappingStyles(AP4_Tx3gSampleDescription& description)
{
    AP4_MemoryByteStream* sample = BuildOverlapSample();
    AP4_Tx3gCue cue;
    CHECK_CHECK(AP4_SUCCEEDED(DecodeStream(description, *sample, cue)));
    sample->Release();

    // text is UTF-8 "hello", the unknown box's payload is not appended
    CHECK_CHECK(cue.GetText().GetDataSize() == 5);
    CHECK_CHECK(AP4_CompareMemory(cue.GetText().GetData(), "hello", 5) == 0);
    CHECK_CHECK(cue.GetCharacterCount() == 5);

    // timing comes from the sample, in track ticks
    CHECK_CHECK(cue.GetCts() == 1500);
    CHECK_CHECK(cue.GetDuration() == 2000);

    // presentation defaults come from the active description
    CHECK_CHECK(cue.GetHorizontalJustification() == AP4_TX3G_ALIGNMENT_CENTER);
    CHECK_CHECK(cue.GetBackgroundColor().a == 40);

    const AP4_Array<AP4_Tx3gCue::Run>& runs = cue.GetRuns();
    // [0,1) default, [1,2) record 1, [2,4) record 2 merged, [4,5) default
    CHECK_CHECK(runs.ItemCount() == 4);
    CHECK_CHECK(runs[0].m_StartChar == 0 && runs[0].m_EndChar == 1);
    CHECK_CHECK(runs[0].m_Style.m_FontId == 1);
    CHECK_CHECK((runs[0].m_Style.m_FontStyleFlags & 0x01) != 0);
    CHECK_CHECK(runs[0].m_Style.m_FontSize == 24);
    CHECK_CHECK(runs[0].m_Style.m_Color.a == 44);
    CHECK_CHECK(runs[0].m_FontName == "Serif");

    CHECK_CHECK(runs[1].m_StartChar == 1 && runs[1].m_EndChar == 2);
    CHECK_CHECK(runs[1].m_Style.m_FontId == 2);
    CHECK_CHECK((runs[1].m_Style.m_FontStyleFlags & 0x02) != 0);
    CHECK_CHECK(runs[1].m_FontName == "Sans");

    // overlapping part [2,3) is won by the later record, [3,4) merges into it
    CHECK_CHECK(runs[2].m_StartChar == 2 && runs[2].m_EndChar == 4);
    CHECK_CHECK(runs[2].m_Style.m_FontId == 1);
    CHECK_CHECK((runs[2].m_Style.m_FontStyleFlags & 0x04) != 0);
    CHECK_CHECK(runs[2].m_Style.m_FontSize == 22);
    CHECK_CHECK(runs[2].m_Style.m_Color.a == 128); // alpha retained
    CHECK_CHECK(runs[2].m_FontName == "Serif");

    CHECK_CHECK(runs[3].m_StartChar == 4 && runs[3].m_EndChar == 5);
    CHECK_CHECK(runs[3].m_Style.m_FontSize == 24);
}

/*----------------------------------------------------------------------
|   TestAdjacentRecordsMerge
+---------------------------------------------------------------------*/
static void
TestAdjacentRecordsMerge(AP4_Tx3gSampleDescription& description)
{
    static const AP4_UI08 color[4] = {7, 8, 9, 10};

    // two adjacent records carrying identical, complete styles
    LocalMemoryStream styl;
    styl.WriteUI16(2);
    WriteStyleRecord(styl, 0, 2, 2, 0x02, 20, color);
    WriteStyleRecord(styl, 2, 4, 2, 0x02, 20, color);

    LocalMemoryStream sample;
    sample.WriteUI16(4);
    sample.Write("abcd", 4);
    sample.WriteUI32(8+styl.GetDataSize());
    sample.WriteUI32(AP4_ATOM_TYPE_STYL);
    sample.Write(styl.GetData(), styl.GetDataSize());

    AP4_Tx3gCue cue;
    CHECK_CHECK(AP4_SUCCEEDED(DecodeStream(description, sample, cue)));
    CHECK_CHECK(cue.GetCharacterCount() == 4);
    CHECK_CHECK(cue.GetRuns().ItemCount() == 1);
    CHECK_CHECK(cue.GetRuns()[0].m_StartChar == 0);
    CHECK_CHECK(cue.GetRuns()[0].m_EndChar == 4);
    CHECK_CHECK(cue.GetRuns()[0].m_FontName == "Sans");

    // a zero-length styl record is ignored, not rejected
    LocalMemoryStream styl_zero;
    styl_zero.WriteUI16(1);
    WriteStyleRecord(styl_zero, 2, 2, 2, 0x02, 20, color);
    LocalMemoryStream sample_zero;
    sample_zero.WriteUI16(4);
    sample_zero.Write("abcd", 4);
    sample_zero.WriteUI32(8+styl_zero.GetDataSize());
    sample_zero.WriteUI32(AP4_ATOM_TYPE_STYL);
    sample_zero.Write(styl_zero.GetData(), styl_zero.GetDataSize());

    AP4_Tx3gCue cue_zero;
    CHECK_CHECK(AP4_SUCCEEDED(DecodeStream(description, sample_zero, cue_zero)));
    CHECK_CHECK(cue_zero.GetRuns().ItemCount() == 1);
    CHECK_CHECK(cue_zero.GetRuns()[0].m_Style.m_FontId == 1); // default
}

/*----------------------------------------------------------------------
|   TestUtf16AndSurrogates
+---------------------------------------------------------------------*/
static void
TestUtf16AndSurrogates(AP4_Tx3gSampleDescription& description)
{
    // "A😀中B" encoded UTF-16 big-endian with a FEFF BOM
    static const AP4_UI08 be[] = {
        0x00, 0x0C,                                     // length 12
        0xFE, 0xFF,                                     // BOM (stripped)
        0x00, 0x41,                                     // A
        0xD8, 0x3D, 0xDE, 0x00,                         // 😀 (surrogate pair)
        0x4E, 0x2D,                                     // 中
        0x00, 0x42                                      // B
    };

    static const AP4_UI08 styl_color[4] = {5, 6, 7, 8};
    LocalMemoryStream styl;
    styl.WriteUI16(1);
    WriteStyleRecord(styl, 1, 3, 2, 0x02, 20, styl_color); // [1,3): 😀 and 中

    LocalMemoryStream sample;
    sample.Write(be, sizeof(be));
    sample.WriteUI32(8+styl.GetDataSize());
    sample.WriteUI32(AP4_ATOM_TYPE_STYL);
    sample.Write(styl.GetData(), styl.GetDataSize());

    AP4_Tx3gCue cue;
    CHECK_CHECK(AP4_SUCCEEDED(DecodeStream(description, sample, cue)));

    // four code points, UTF-8 sizes: A=1, 😀=4, 中=3, B=1 -> 9 bytes, no BOM
    CHECK_CHECK(cue.GetCharacterCount() == 4);
    CHECK_CHECK(cue.GetText().GetDataSize() == 9);
    static const AP4_UI08 expected_utf8[] = {
        'A',
        0xF0, 0x9F, 0x98, 0x80,                          // 😀
        0xE4, 0xB8, 0xAD,                               // 中
        'B'
    };
    CHECK_CHECK(AP4_CompareMemory(cue.GetText().GetData(),
                                  expected_utf8, 9) == 0);

    // runs are cut by code point, not by UTF-8 byte, and the pair stays whole
    const AP4_Array<AP4_Tx3gCue::Run>& runs = cue.GetRuns();
    CHECK_CHECK(runs.ItemCount() == 3);
    CHECK_CHECK(runs[0].m_StartChar == 0 && runs[0].m_EndChar == 1);
    CHECK_CHECK(runs[1].m_StartChar == 1 && runs[1].m_EndChar == 3);
    CHECK_CHECK(runs[1].m_FontName == "Sans");
    CHECK_CHECK(runs[2].m_StartChar == 3 && runs[2].m_EndChar == 4);

    // the same text little-endian with a FFFE BOM
    static const AP4_UI08 le[] = {
        0x00, 0x0C,
        0xFF, 0xFE,
        0x41, 0x00,
        0x3D, 0xD8, 0x00, 0xDE,
        0x2D, 0x4E,
        0x42, 0x00
    };
    AP4_Tx3gCue cue_le;
    CHECK_CHECK(AP4_SUCCEEDED(DecodeBytes(description, le, sizeof(le), cue_le)));
    CHECK_CHECK(cue_le.GetCharacterCount() == 4);
    CHECK_CHECK(cue_le.GetText().GetDataSize() == 9);
    CHECK_CHECK(AP4_CompareMemory(cue_le.GetText().GetData(),
                                  expected_utf8, 9) == 0);

    // a UTF-8 BOM is stripped as well
    static const AP4_UI08 bom8[] = {
        0x00, 0x06,
        0xEF, 0xBB, 0xBF, 'h', 'i', '!'
    };
    AP4_Tx3gCue cue8;
    CHECK_CHECK(AP4_SUCCEEDED(DecodeBytes(description, bom8, sizeof(bom8), cue8)));
    CHECK_CHECK(cue8.GetCharacterCount() == 3);
    CHECK_CHECK(cue8.GetText().GetDataSize() == 3);
    CHECK_CHECK(AP4_CompareMemory(cue8.GetText().GetData(), "hi!", 3) == 0);

    // a lone high surrogate is a bad encoding
    static const AP4_UI08 bad[] = {
        0x00, 0x04,
        0xFE, 0xFF, 0xD8, 0x3D
    };
    AP4_Tx3gCue cue_bad;
    CHECK_CHECK(AP4_FAILED(DecodeBytes(description, bad, sizeof(bad), cue_bad)));
}

/*----------------------------------------------------------------------
|   TestClearScreen
+---------------------------------------------------------------------*/
static void
TestClearScreen(AP4_Tx3gSampleDescription& description)
{
    AP4_MemoryByteStream* populated = BuildOverlapSample();
    AP4_Tx3gCue cue;
    CHECK_CHECK(AP4_SUCCEEDED(DecodeStream(description, *populated, cue)));
    populated->Release();
    CHECK_CHECK(cue.GetCharacterCount() == 5);

    // an empty sample is a legal clear-screen cue: the same cue is reset,
    // it does not keep the previous line
    static const AP4_UI08 empty[] = {0x00, 0x00};
    CHECK_CHECK(AP4_SUCCEEDED(DecodeBytes(description, empty, sizeof(empty), cue)));
    CHECK_CHECK(cue.GetText().GetDataSize() == 0);
    CHECK_CHECK(cue.GetCharacterCount() == 0);
    CHECK_CHECK(cue.GetRuns().ItemCount() == 0);

    // the presentation defaults are still carried by the clear cue
    CHECK_CHECK(cue.GetHorizontalJustification() == AP4_TX3G_ALIGNMENT_CENTER);
}

/*----------------------------------------------------------------------
|   TestDescriptionSwitching
+---------------------------------------------------------------------*/
static void
TestDescriptionSwitching()
{
    // two descriptions, each with its own ftab: font id 1 means different fonts
    AP4_MemoryByteStream* bytes0 = BuildEntryBytes();
    AP4_Tx3gSampleEntry* entry0 = ParseEntry(*bytes0);
    AP4_Tx3gSampleDescription* desc0 =
        AP4_DYNAMIC_CAST(AP4_Tx3gSampleDescription, entry0->ToSampleDescription());
    CHECK_CHECK(desc0 != NULL);

    AP4_Tx3gSampleEntry entry1;
    entry1.AddFont(1, "Mono");
    AP4_Tx3gStyleRecord style;
    style.m_FontId = 1;
    style.m_FontSize = 30;
    entry1.SetDefaultStyle(style);
    AP4_Tx3gSampleDescription* desc1 =
        AP4_DYNAMIC_CAST(AP4_Tx3gSampleDescription, entry1.ToSampleDescription());
    CHECK_CHECK(desc1 != NULL);

    static const AP4_UI08 sample_bytes[] = {0x00, 0x01, 'x'};

    AP4_MemoryByteStream* stream = new AP4_MemoryByteStream(sample_bytes,
                                                             sizeof(sample_bytes));
    AP4_SyntheticSampleTable* table = new AP4_SyntheticSampleTable();
    table->AddSampleDescription(desc0); // ownership transferred
    table->AddSampleDescription(desc1); // ownership transferred
    table->AddSample(*stream, 0, sizeof(sample_bytes), 100, 0, 0,   0, true);
    table->AddSample(*stream, 0, sizeof(sample_bytes), 100, 1, 100, 0, true);
    AP4_Track track(AP4_Track::TYPE_SUBTITLES, table, 1,
                    1000, 200, 1000, 200, "eng", 0, 0);
    stream->Release();

    AP4_Tx3gCue cue0;
    CHECK_CHECK(AP4_SUCCEEDED(AP4_Tx3gDecoder::Decode(track, 0, cue0)));
    CHECK_CHECK(cue0.GetRuns().ItemCount() == 1);
    CHECK_CHECK(cue0.GetRuns()[0].m_FontName == "Serif");
    CHECK_CHECK(cue0.GetRuns()[0].m_Style.m_FontSize == 24);

    AP4_Tx3gCue cue1;
    CHECK_CHECK(AP4_SUCCEEDED(AP4_Tx3gDecoder::Decode(track, 1, cue1)));
    CHECK_CHECK(cue1.GetRuns().ItemCount() == 1);
    CHECK_CHECK(cue1.GetRuns()[0].m_FontName == "Mono");
    CHECK_CHECK(cue1.GetRuns()[0].m_Style.m_FontSize == 30);

    delete entry0;
    bytes0->Release();
}

/*----------------------------------------------------------------------
|   TestErrorCases
+---------------------------------------------------------------------*/
static void
TestErrorCases(AP4_Tx3gSampleDescription& description)
{
    AP4_MemoryByteStream* good = BuildOverlapSample();
    AP4_Tx3gCue cue;
    CHECK_CHECK(AP4_SUCCEEDED(DecodeStream(description, *good, cue)));
    good->Release();
    CHECK_CHECK(cue.GetCharacterCount() == 5);

    // truncated length header
    static const AP4_UI08 truncated[] = {0x00};
    AP4_Tx3gCue probe = cue;
    CHECK_CHECK(AP4_FAILED(DecodeBytes(description, truncated,
                                       sizeof(truncated), probe)));
    CHECK_CHECK(probe.GetCharacterCount() == 5); // cue untouched on failure

    // declared text length runs past the sample
    static const AP4_UI08 too_long[] = {0x00, 0x09, 'a', 'b'};
    probe = cue;
    CHECK_CHECK(AP4_FAILED(DecodeBytes(description, too_long,
                                       sizeof(too_long), probe)));
    CHECK_CHECK(probe.GetCharacterCount() == 5);

    // bad UTF-8: overlong encoding of NUL
    static const AP4_UI08 bad_utf8[] = {0x00, 0x02, 0xC0, 0x80};
    probe = cue;
    CHECK_CHECK(AP4_FAILED(DecodeBytes(description, bad_utf8,
                                       sizeof(bad_utf8), probe)));
    CHECK_CHECK(probe.GetCharacterCount() == 5);

    // styl range past the text
    LocalMemoryStream styl_over;
    styl_over.WriteUI16(1);
    static const AP4_UI08 any_color[4] = {0, 0, 0, 0};
    WriteStyleRecord(styl_over, 0, 9, 1, 0, 20, any_color);
    LocalMemoryStream sample_over;
    sample_over.WriteUI16(1);
    sample_over.Write("a", 1);
    sample_over.WriteUI32(8+styl_over.GetDataSize());
    sample_over.WriteUI32(AP4_ATOM_TYPE_STYL);
    sample_over.Write(styl_over.GetData(), styl_over.GetDataSize());
    probe = cue;
    CHECK_CHECK(AP4_FAILED(DecodeStream(description, sample_over, probe)));
    CHECK_CHECK(probe.GetCharacterCount() == 5);

    // reverse styl range
    LocalMemoryStream styl_rev;
    styl_rev.WriteUI16(1);
    WriteStyleRecord(styl_rev, 3, 1, 1, 0, 20, any_color);
    LocalMemoryStream sample_rev;
    sample_rev.WriteUI16(4);
    sample_rev.Write("abcd", 4);
    sample_rev.WriteUI32(8+styl_rev.GetDataSize());
    sample_rev.WriteUI32(AP4_ATOM_TYPE_STYL);
    sample_rev.Write(styl_rev.GetData(), styl_rev.GetDataSize());
    probe = cue;
    CHECK_CHECK(AP4_FAILED(DecodeStream(description, sample_rev, probe)));
    CHECK_CHECK(probe.GetCharacterCount() == 5);

    // unknown font id
    LocalMemoryStream styl_font;
    styl_font.WriteUI16(1);
    WriteStyleRecord(styl_font, 0, 1, 99, 0, 20, any_color);
    LocalMemoryStream sample_font;
    sample_font.WriteUI16(1);
    sample_font.Write("a", 1);
    sample_font.WriteUI32(8+styl_font.GetDataSize());
    sample_font.WriteUI32(AP4_ATOM_TYPE_STYL);
    sample_font.Write(styl_font.GetData(), styl_font.GetDataSize());
    probe = cue;
    CHECK_CHECK(AP4_FAILED(DecodeStream(description, sample_font, probe)));
    CHECK_CHECK(probe.GetCharacterCount() == 5);

    // incomplete styl: one record announced but only 5 of 12 bytes present
    LocalMemoryStream sample_bad_styl;
    sample_bad_styl.WriteUI16(1);
    sample_bad_styl.Write("a", 1);
    sample_bad_styl.WriteUI32(8+2+5);
    sample_bad_styl.WriteUI32(AP4_ATOM_TYPE_STYL);
    sample_bad_styl.WriteUI16(1);
    sample_bad_styl.WriteUI16(0);
    sample_bad_styl.WriteUI16(1);
    sample_bad_styl.WriteUI08(1);
    probe = cue;
    CHECK_CHECK(AP4_FAILED(DecodeStream(description, sample_bad_styl, probe)));
    CHECK_CHECK(probe.GetCharacterCount() == 5);

    // a trailing extension box header without a full body
    LocalMemoryStream sample_bad_box;
    sample_bad_box.WriteUI16(1);
    sample_bad_box.Write("a", 1);
    sample_bad_box.WriteUI32(16);
    sample_bad_box.WriteUI32(AP4_ATOM_TYPE('z','z','z','z'));
    sample_bad_box.WriteUI32(0); // only 4 of the 8 payload bytes
    probe = cue;
    CHECK_CHECK(AP4_FAILED(DecodeStream(description, sample_bad_box, probe)));
    CHECK_CHECK(probe.GetCharacterCount() == 5);

    // truncated UTF-8 multibyte sequence
    static const AP4_UI08 truncated_utf8[] = {0x00, 0x02, 0xE4, 0xB8};
    probe = cue;
    CHECK_CHECK(AP4_FAILED(DecodeBytes(description, truncated_utf8,
                                       sizeof(truncated_utf8), probe)));
    CHECK_CHECK(probe.GetCharacterCount() == 5);
    CHECK_CHECK(probe.GetRuns().ItemCount() == 4); // runs untouched too

    // a description whose default font id is absent from its own ftab:
    // even an empty (clear-screen) sample must report the unknown font id,
    // and the previous cue must be left completely intact
    AP4_Tx3gSampleEntry broken_entry;
    AP4_Tx3gSampleDescription* broken_description =
        AP4_DYNAMIC_CAST(AP4_Tx3gSampleDescription, broken_entry.ToSampleDescription());
    CHECK_CHECK(broken_description != NULL);
    static const AP4_UI08 empty_sample[] = {0x00, 0x00};
    probe = cue;
    CHECK_CHECK(AP4_FAILED(DecodeBytes(*broken_description, empty_sample,
                                       sizeof(empty_sample), probe)));
    CHECK_CHECK(probe.GetCharacterCount() == 5);
    CHECK_CHECK(probe.GetRuns().ItemCount() == 4);
    delete broken_description;
}

/*----------------------------------------------------------------------
|   TestRoundTrip
+---------------------------------------------------------------------*/
static void
TestRoundTrip()
{
    AP4_MemoryByteStream* bytes = BuildEntryBytes();
    AP4_Tx3gSampleEntry* entry = ParseEntry(*bytes);
    AP4_Tx3gSampleDescription* description =
        AP4_DYNAMIC_CAST(AP4_Tx3gSampleDescription, entry->ToSampleDescription());
    CHECK_CHECK(description != NULL);

    // serialize the description back out and parse it again through the
    // sample-entry factory path
    AP4_Atom* atom = description->ToAtom();
    CHECK_CHECK(atom != NULL);
    LocalMemoryStream serialized;
    CHECK_CHECK(AP4_SUCCEEDED(atom->Write(serialized)));
    delete atom;

    AP4_Tx3gSampleEntry* re_entry = ParseEntry(serialized);
    AP4_Tx3gSampleDescription* re_description =
        AP4_DYNAMIC_CAST(AP4_Tx3gSampleDescription, re_entry->ToSampleDescription());
    CHECK_CHECK(re_description != NULL);

    CHECK_CHECK(re_description->GetHorizontalJustification() ==
                AP4_TX3G_ALIGNMENT_CENTER);
    CHECK_CHECK(re_description->GetVerticalJustification() == 0);
    CHECK_CHECK(re_description->GetBackgroundColor().a == 40);
    AP4_UI32 top, left, bottom, right;
    re_description->GetDefaultTextBox(top, left, bottom, right);
    CHECK_CHECK(top == 1 && left == 2 && bottom == 3 && right == 4);
    CHECK_CHECK(re_description->GetDefaultStyle().m_FontSize == 24);
    CHECK_CHECK(re_description->GetDefaultStyle().m_FontStyleFlags == 0x01);
    CHECK_CHECK(re_description->GetDefaultStyle().m_Color.a == 44);
    CHECK_CHECK(re_description->GetFontTable().ItemCount() == 2);
    CHECK_CHECK(*re_description->FindFontName(1) == "Serif");
    CHECK_CHECK(*re_description->FindFontName(2) == "Sans");

    // extension boxes other than ftab are preserved too
    CHECK_CHECK(re_entry->GetChild(AP4_ATOM_TYPE('z','z','z','z')) != NULL);

    // Clone() must produce an equivalent description
    AP4_Result clone_result = AP4_FAILURE;
    AP4_SampleDescription* clone = description->Clone(&clone_result);
    CHECK_CHECK(AP4_SUCCEEDED(clone_result));
    AP4_Tx3gSampleDescription* tx3g_clone =
        AP4_DYNAMIC_CAST(AP4_Tx3gSampleDescription, clone);
    CHECK_CHECK(tx3g_clone != NULL);
    CHECK_CHECK(tx3g_clone->GetFontTable().ItemCount() == 2);
    CHECK_CHECK(tx3g_clone->GetDefaultStyle().m_FontSize == 24);
    delete clone;

    delete re_entry;
    delete description;
    delete entry;
    bytes->Release();
}

/*----------------------------------------------------------------------
|   TestRealFileRoundTrip
|   Go through a full MP4 file: synthetic track -> AP4_FileWriter ->
|   AP4_File parse -> AP4_Tx3gDecoder::Decode on the parsed track.
+---------------------------------------------------------------------*/
static void
TestRealFileRoundTrip(AP4_Tx3gSampleDescription& description)
{
    AP4_MemoryByteStream* sample_stream = BuildOverlapSample();

    AP4_SyntheticSampleTable* table = new AP4_SyntheticSampleTable();
    AP4_Result clone_result = AP4_FAILURE;
    AP4_SampleDescription* owned_description = description.Clone(&clone_result);
    CHECK_CHECK(AP4_SUCCEEDED(clone_result));
    table->AddSampleDescription(owned_description);
    table->AddSample(*sample_stream, 0, sample_stream->GetDataSize(),
                     2000, 0, 0, 500, true);

    // a second, clear-screen sample with a different CTS delta forces a
    // ctts box to be serialized (a constant delta across all samples is
    // represented by the absence of a ctts box)
    static const AP4_UI08 clear_bytes[] = {0x00, 0x00};
    AP4_MemoryByteStream* clear_stream =
        new AP4_MemoryByteStream(clear_bytes, sizeof(clear_bytes));
    table->AddSample(*clear_stream, 0, sizeof(clear_bytes),
                     1000, 0, 2000, 0, true);

    AP4_Track* track = new AP4_Track(AP4_Track::TYPE_SUBTITLES, table, 1,
                                     1000, 4000, 1000, 4000, "eng", 0, 0);
    AP4_Movie* movie = new AP4_Movie(1000);
    movie->AddTrack(track);
    AP4_File* output_file = new AP4_File(movie);

    AP4_MemoryByteStream* file_image = new AP4_MemoryByteStream();
    CHECK_CHECK(AP4_SUCCEEDED(AP4_FileWriter::Write(*output_file, *file_image)));
    AP4_Size expected_sample_size = sample_stream->GetDataSize();
    delete output_file;
    sample_stream->Release();
    clear_stream->Release();

    // parse the serialized file like any real input
    file_image->Seek(0);
    AP4_File* input_file = new AP4_File(*file_image);
    AP4_Movie* input_movie = input_file->GetMovie();
    CHECK_CHECK(input_movie != NULL);
    AP4_Track* input_track = input_movie->GetTrack(AP4_Track::TYPE_SUBTITLES);
    CHECK_CHECK(input_track != NULL);
    CHECK_CHECK(input_track->GetSampleCount() == 2);

    AP4_SampleDescription* parsed_description =
        input_track->GetSampleDescription(0);
    AP4_Tx3gSampleDescription* parsed_tx3g =
        AP4_DYNAMIC_CAST(AP4_Tx3gSampleDescription, parsed_description);
    CHECK_CHECK(parsed_tx3g != NULL);
    CHECK_CHECK(parsed_tx3g->GetFontTable().ItemCount() == 2);

    // the raw sample reading interface still works unchanged
    AP4_Sample raw_sample;
    AP4_DataBuffer raw_data;
    CHECK_CHECK(AP4_SUCCEEDED(input_track->ReadSample(0, raw_sample, raw_data)));
    CHECK_CHECK(raw_data.GetDataSize() == expected_sample_size);

    // decode through the objects parsed from the real file
    AP4_Tx3gCue cue;
    CHECK_CHECK(AP4_SUCCEEDED(AP4_Tx3gDecoder::Decode(*input_track, 0, cue)));
    CHECK_CHECK(cue.GetCharacterCount() == 5);
    CHECK_CHECK(AP4_CompareMemory(cue.GetText().GetData(), "hello", 5) == 0);
    CHECK_CHECK(cue.GetRuns().ItemCount() == 4);
    CHECK_CHECK(cue.GetRuns()[2].m_FontName == "Serif");
    CHECK_CHECK(cue.GetCts() == 500);
    CHECK_CHECK(cue.GetDuration() == 2000);

    // the clear-screen sample decodes to empty text with its own timing
    AP4_Tx3gCue clear_cue;
    CHECK_CHECK(AP4_SUCCEEDED(AP4_Tx3gDecoder::Decode(*input_track, 1, clear_cue)));
    CHECK_CHECK(clear_cue.GetCharacterCount() == 0);
    CHECK_CHECK(clear_cue.GetText().GetDataSize() == 0);
    CHECK_CHECK(clear_cue.GetCts() == 2000);
    CHECK_CHECK(clear_cue.GetDuration() == 1000);

    delete input_file;
    file_image->Release();
}

/*----------------------------------------------------------------------
|   run_tx3g_tests
+---------------------------------------------------------------------*/
void
run_tx3g_tests()
{
    AP4_Tx3gSampleDescription* description = NULL;
    TestSampleEntryParsing(description);
    CHECK_CHECK(description != NULL);

    TestOverlappingStyles(*description);
    TestAdjacentRecordsMerge(*description);
    TestUtf16AndSurrogates(*description);
    TestClearScreen(*description);
    TestDescriptionSwitching();
    TestErrorCases(*description);
    TestRoundTrip();
    TestRealFileRoundTrip(*description);

    delete description;
}
