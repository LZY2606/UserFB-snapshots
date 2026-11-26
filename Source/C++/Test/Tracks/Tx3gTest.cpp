/*****************************************************************
|
|    AP4 - TX3G Decoder Regression Tests
|
|    These tests exercise tx3g sample description parsing and
|    AP4_Tx3gDecoder::Decode using hand-built binary descriptions
|    and text samples, plus a full MP4 file serialize/reparse pass.
|
 ****************************************************************/

#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "Ap4.h"
#include "test_support.h"

typedef std::vector<AP4_UI08> Bytes;

/*----------------------------------------------------------------------
|   byte builders (big endian)
+---------------------------------------------------------------------*/
static void
Put16(Bytes& b, AP4_UI16 v)
{
    b.push_back((AP4_UI08)(v >> 8));
    b.push_back((AP4_UI08)(v & 0xFF));
}

static void
Put32(Bytes& b, AP4_UI32 v)
{
    b.push_back((AP4_UI08)(v >> 24));
    b.push_back((AP4_UI08)(v >> 16));
    b.push_back((AP4_UI08)(v >> 8));
    b.push_back((AP4_UI08)(v & 0xFF));
}

static Bytes
MakeBox(AP4_UI32 type, const Bytes& payload)
{
    Bytes box;
    Put32(box, (AP4_UI32)(8 + payload.size()));
    Put32(box, type);
    box.insert(box.end(), payload.begin(), payload.end());
    return box;
}

static void
AppendBytes(Bytes& out, const Bytes& in)
{
    out.insert(out.end(), in.begin(), in.end());
}

/* Build one 12 byte StyleRecord. */
static Bytes
StyleRec(AP4_UI16 start_char, AP4_UI16 end_char, AP4_UI16 font_id,
         AP4_UI08 flags, AP4_UI08 font_size,
         AP4_UI08 r, AP4_UI08 g, AP4_UI08 b, AP4_UI08 a)
{
    Bytes r_;
    Put16(r_, start_char);
    Put16(r_, end_char);
    Put16(r_, font_id);
    r_.push_back(flags);
    r_.push_back(font_size);
    r_.push_back(r);
    r_.push_back(g);
    r_.push_back(b);
    r_.push_back(a);
    return r_;
}

/* Build a 'styl' box from raw 12 byte records. */
static Bytes
StylBox(const std::vector<Bytes>& records, AP4_UI16 count_override = 0xFFFF)
{
    Bytes payload;
    Put16(payload, count_override == 0xFFFF ? (AP4_UI16)records.size()
                                            : count_override);
    for (size_t i = 0; i < records.size(); ++i) {
        AppendBytes(payload, records[i]);
    }
    return MakeBox(AP4_ATOM_TYPE('s','t','y','l'), payload);
}

struct FontDef {
    AP4_UI16    id;
    const char* name;
};

/*
 * Build a complete 'tx3g' sample entry box, including the 8 byte base
 * SampleEntry fields, the 30 fixed tx3g bytes and an 'ftab' box.
 */
static Bytes
Tx3gEntry(AP4_UI16 default_font_id,
          AP4_UI08 default_flags,
          AP4_UI08 default_size,
          AP4_UI08 dr, AP4_UI08 dg, AP4_UI08 db, AP4_UI08 da,
          int horiz_just, int vert_just,
          AP4_SI16 top, AP4_SI16 left, AP4_SI16 bottom, AP4_SI16 right,
          AP4_UI08 br, AP4_UI08 bgc, AP4_UI08 bb, AP4_UI08 ba,
          const std::vector<FontDef>& fonts,
          const Bytes& extra_child = Bytes())
{
    Bytes body;

    /* base SampleEntry: 6 reserved bytes + data reference index */
    body.push_back(0); body.push_back(0); body.push_back(0);
    body.push_back(0); body.push_back(0); body.push_back(0);
    Put16(body, 1);

    /* displayFlags */
    Put32(body, 0);

    /* horizontal / vertical justification */
    body.push_back((AP4_UI08)(horiz_just & 0xFF));
    body.push_back((AP4_UI08)(vert_just & 0xFF));

    /* background RGBA */
    body.push_back(br); body.push_back(bgc);
    body.push_back(bb); body.push_back(ba);

    /* default BoxRecord */
    Put16(body, (AP4_UI16)top);
    Put16(body, (AP4_UI16)left);
    Put16(body, (AP4_UI16)bottom);
    Put16(body, (AP4_UI16)right);

    /* default StyleRecord (its range is irrelevant, use 0,0) */
    AppendBytes(body, StyleRec(0, 0, default_font_id, default_flags,
                               default_size, dr, dg, db, da));

    /* ftab font table */
    Bytes ftab;
    Put16(ftab, (AP4_UI16)fonts.size());
    for (size_t i = 0; i < fonts.size(); ++i) {
        Put16(ftab, fonts[i].id);
        AP4_UI08 name_len = (AP4_UI08)strlen(fonts[i].name);
        ftab.push_back(name_len);
        for (AP4_UI08 j = 0; j < name_len; ++j) {
            ftab.push_back((AP4_UI08)fonts[i].name[j]);
        }
    }
    AppendBytes(body, MakeBox(AP4_ATOM_TYPE('f','t','a','b'), ftab));

    /* any extra child boxes */
    AppendBytes(body, extra_child);

    return MakeBox(AP4_ATOM_TYPE_TX3G, body);
}

/* Parse a full sample entry box into an AP4_Tx3gSampleDescription. */
static AP4_Tx3gSampleDescription*
ParseDesc(const Bytes& entry)
{
    AP4_MemoryByteStream* stream =
        new AP4_MemoryByteStream(entry.data(), (AP4_Size)entry.size());
    AP4_AtomFactory factory;
    factory.PushContext(AP4_ATOM_TYPE_STSD);
    AP4_Atom* atom = NULL;
    AP4_Result result = factory.CreateAtomFromStream(*stream, atom);
    factory.PopContext();
    stream->Release();

    CHECK_CHECK(AP4_SUCCEEDED(result));
    CHECK_CHECK(atom != NULL);
    AP4_Tx3gSampleEntry* tx3g_entry =
        AP4_DYNAMIC_CAST(AP4_Tx3gSampleEntry, atom);
    CHECK_CHECK(tx3g_entry != NULL);

    AP4_SampleDescription* sd = tx3g_entry->ToSampleDescription();
    AP4_Tx3gSampleDescription* desc =
        AP4_DYNAMIC_CAST(AP4_Tx3gSampleDescription, sd);
    CHECK_CHECK(desc != NULL);

    delete atom;
    /* sd is the same object as desc; ownership transferred to caller */
    return desc;
}

/* Build a text sample: 16 bit byte length + text + modifier boxes. */
static Bytes
TextSample(const Bytes& text, const std::vector<Bytes>& boxes)
{
    Bytes sample;
    Put16(sample, (AP4_UI16)text.size());
    AppendBytes(sample, text);
    for (size_t i = 0; i < boxes.size(); ++i) {
        AppendBytes(sample, boxes[i]);
    }
    return sample;
}

static void
AddSample(AP4_SyntheticSampleTable* table,
          const Bytes& data,
          AP4_Ordinal desc_index,
          AP4_UI32 duration,
          AP4_UI32 cts_delta = 0)
{
    AP4_MemoryByteStream* stream =
        new AP4_MemoryByteStream(data.data(), (AP4_Size)data.size());
    AP4_Result result = table->AddSample(*stream, 0, (AP4_Size)data.size(),
                                         duration, desc_index,
                                         0 /* dts: auto */, cts_delta, true);
    CHECK_CHECK(AP4_SUCCEEDED(result));
    stream->Release();
}

/* The UTF-8 encoding of "A <grinning face> <zhong> B". */
static Bytes
Utf8_AEmojiZhongB()
{
    Bytes t;
    t.push_back('A');
    t.push_back(0xF0); t.push_back(0x9F); t.push_back(0x98); t.push_back(0x80);
    t.push_back(0xE4); t.push_back(0xB8); t.push_back(0xAD);
    t.push_back('B');
    return t;
}

/*----------------------------------------------------------------------
|   1. description parsing + overlapping styles + code point ranges
+---------------------------------------------------------------------*/
static void
test_overlapping_styles()
{
    std::vector<FontDef> fonts;
    fonts.push_back((FontDef){1, "Arial"});
    fonts.push_back((FontDef){2, "PingFang"});

    Bytes extra;
    {
        Bytes payload;
        payload.push_back(7); payload.push_back(8); payload.push_back(9);
        extra = MakeBox(AP4_ATOM_TYPE('b','l','n','k'), payload);
    }

    Bytes entry = Tx3gEntry(1, 0, 18, 255,255,255,255,
                            1, -1,
                            5, 6, 105, 206,
                            10, 20, 30, 40,
                            fonts, extra);
    AP4_Tx3gSampleDescription* desc = ParseDesc(entry);

    /* verify parsed sample description fields */
    CHECK_CHECK(desc->GetFormat() == AP4_SAMPLE_FORMAT_TX3G);
    CHECK_CHECK(desc->GetHorizontalJustification() == 1);
    CHECK_CHECK(desc->GetVerticalJustification() == -1);
    CHECK_CHECK(desc->GetHorizontalAlignment() == 1);
    CHECK_CHECK(desc->GetVerticalAlignment() == -1);
    const AP4_Tx3gColor& bg = desc->GetBackgroundColor();
    CHECK_CHECK(bg.r == 10 && bg.g == 20 && bg.b == 30 && bg.a == 40);
    CHECK_CHECK(bg[0] == 10 && bg[3] == 40);
    const AP4_Tx3gBox& box = desc->GetDefaultTextBox();
    CHECK_CHECK(box.top == 5 && box.left == 6);
    CHECK_CHECK(box.bottom == 105 && box.right == 206);
    CHECK_CHECK(box.width == 200 && box.height == 100);
    CHECK_CHECK(desc->GetFontCount() == 2);
    AP4_String font_name;
    CHECK_CHECK(AP4_SUCCEEDED(desc->FindFontName(2, font_name)));
    CHECK_CHECK(strcmp(font_name.GetChars(), "PingFang") == 0);
    CHECK_CHECK(desc->HasFont(1));
    CHECK_CHECK(!desc->HasFont(77));
    const AP4_Tx3gStyleRecord& ds = desc->GetDefaultStyleRecord();
    CHECK_CHECK(ds.fontID == 1 && ds.fontSize == 18);
    CHECK_CHECK(ds.textColorRGBA[3] == 255);

    /* build a track with one sample */
    AP4_SyntheticSampleTable* table = new AP4_SyntheticSampleTable();
    CHECK_CHECK(AP4_SUCCEEDED(table->AddSampleDescription(desc)));

    Bytes text = Utf8_AEmojiZhongB();
    std::vector<Bytes> recs;
    /* [1,4): PingFang bold 20 red */
    recs.push_back(StyleRec(1, 4, 2, 0x01, 20, 255, 0, 0, 255));
    /* [2,3): Arial italic+underline 22 blue, alpha 128 (wins overlap) */
    recs.push_back(StyleRec(2, 3, 1, 0x02 | 0x04, 22, 0, 0, 255, 128));
    std::vector<Bytes> boxes;
    boxes.push_back(StylBox(recs));
    Bytes sample = TextSample(text, boxes);
    AddSample(table, sample, 0, 2000, 250);

    AP4_Track track(AP4_Track::TYPE_SUBTITLES, table, 1,
                    1000, 100000, 1000, 100000, "eng", 0, 0);

    /* raw sample reading must still expose length header + boxes */
    AP4_Sample raw_sample;
    AP4_DataBuffer raw_data;
    CHECK_CHECK(AP4_SUCCEEDED(track.ReadSample(0, raw_sample, raw_data)));
    CHECK_CHECK(raw_data.GetDataSize() == sample.size());
    CHECK_CHECK(raw_data.GetData()[0] == 0);
    CHECK_CHECK(raw_data.GetData()[1] == 9);
    CHECK_CHECK(raw_sample.GetDuration() == 2000);

    AP4_Tx3gCue cue;
    CHECK_CHECK(AP4_SUCCEEDED(AP4_Tx3gDecoder::Decode(track, 0, cue)));

    /* timing in track ticks */
    CHECK_CHECK(cue.cts == 250);
    CHECK_CHECK(cue.pts == 250);
    CHECK_CHECK(cue.duration == 2000);

    /* text */
    CHECK_CHECK(cue.text.GetLength() == 9);
    CHECK_CHECK(memcmp(cue.text.GetChars(), text.data(), 9) == 0);
    CHECK_CHECK(cue.utf8.GetLength() == 9);
    CHECK_CHECK(cue.codepointCount == 4);

    /* layout carried to cue */
    CHECK_CHECK(cue.horizontalAlignment == 1);
    CHECK_CHECK(cue.verticalAlignment == -1);
    CHECK_CHECK(cue.hAlign == 1 && cue.vAlign == -1);
    CHECK_CHECK(cue.textBox.right == 206);
    CHECK_CHECK(cue.defaultTextBox.bottom == 105);
    CHECK_CHECK(cue.backgroundColor.g == 20);

    /* four runs: default, rec1, rec2, rec1 (split by the overlap) */
    CHECK_CHECK(cue.runs.ItemCount() == 4);
    CHECK_CHECK(cue.styles.ItemCount() == 4);

    const AP4_Tx3gStyleRun& r0 = cue.runs[0];
    CHECK_CHECK(r0.startChar == 0 && r0.endChar == 1);
    CHECK_CHECK(r0.start == 0 && r0.end == 1);
    CHECK_CHECK(r0.startByte == 0 && r0.endByte == 1);
    CHECK_CHECK(r0.fontID == 1);
    CHECK_CHECK(strcmp(r0.fontName.GetChars(), "Arial") == 0);
    CHECK_CHECK(!r0.bold && !r0.italic && !r0.underline);
    CHECK_CHECK(r0.fontSize == 18);
    CHECK_CHECK(r0.color.r == 255 && r0.color.a == 255);
    CHECK_CHECK(r0.text.GetLength() == 1);
    CHECK_CHECK(r0.text.GetChars()[0] == 'A');

    const AP4_Tx3gStyleRun& r1 = cue.runs[1];
    CHECK_CHECK(r1.startChar == 1 && r1.endChar == 2);
    CHECK_CHECK(r1.startCodepoint == 1 && r1.endCodepoint == 2);
    /* emoji occupies UTF-8 bytes [1,5) */
    CHECK_CHECK(r1.startByte == 1 && r1.endByte == 5);
    CHECK_CHECK(r1.utf8Start == 1 && r1.utf8End == 5);
    CHECK_CHECK(r1.fontID == 2 && r1.fontId == 2);
    CHECK_CHECK(strcmp(r1.fontName.GetChars(), "PingFang") == 0);
    CHECK_CHECK(r1.bold && !r1.italic);
    CHECK_CHECK(r1.style.bold && r1.style.fontSize == 20);
    CHECK_CHECK(r1.fontSize == 20);
    CHECK_CHECK(r1.color.r == 255 && r1.color.g == 0);
    CHECK_CHECK(r1.text.GetLength() == 4);
    CHECK_CHECK(memcmp(r1.text.GetChars(), "\xF0\x9F\x98\x80", 4) == 0);

    const AP4_Tx3gStyleRun& r2 = cue.runs[2];
    /* [2,3) is exactly the Chinese character, UTF-8 bytes [5,8) */
    CHECK_CHECK(r2.startChar == 2 && r2.endChar == 3);
    CHECK_CHECK(r2.startByte == 5 && r2.endByte == 8);
    CHECK_CHECK(r2.italic && r2.underline && !r2.bold);
    CHECK_CHECK(r2.fontSize == 22);
    CHECK_CHECK(r2.color.b == 255 && r2.color.a == 128);
    CHECK_CHECK(r2.textColorRGBA[3] == 128);
    CHECK_CHECK(r2.rgba[2] == 255);
    CHECK_CHECK(r2.text.GetLength() == 3);
    CHECK_CHECK(memcmp(r2.text.GetChars(), "\xE4\xB8\xAD", 3) == 0);

    const AP4_Tx3gStyleRun& r3 = cue.runs[3];
    CHECK_CHECK(r3.startChar == 3 && r3.endChar == 4);
    CHECK_CHECK(r3.startByte == 8 && r3.endByte == 9);
    CHECK_CHECK(r3.fontID == 2 && r3.bold);
    CHECK_CHECK(r3.text.GetChars()[0] == 'B');
}

/*----------------------------------------------------------------------
|   2. run merging and zero length records
+---------------------------------------------------------------------*/
static void
test_merging_and_zero_records()
{
    std::vector<FontDef> fonts;
    fonts.push_back((FontDef){1, "Mono"});
    Bytes entry = Tx3gEntry(1, 0, 14, 0,0,0,255, 0, 0,
                            0, 0, 0, 0, 0, 0, 0, 0, fonts);
    AP4_Tx3gSampleDescription* desc = ParseDesc(entry);
    AP4_SyntheticSampleTable* table = new AP4_SyntheticSampleTable();
    table->AddSampleDescription(desc);

    /* two records with identical complete styles merge into one run */
    {
        Bytes text;
        text.push_back('a'); text.push_back('b'); text.push_back('c');
        text.push_back('d'); text.push_back('e'); text.push_back('f');
        std::vector<Bytes> recs;
        recs.push_back(StyleRec(0, 3, 1, 0x01, 12, 255, 0, 0, 255));
        recs.push_back(StyleRec(3, 6, 1, 0x01, 12, 255, 0, 0, 255));
        std::vector<Bytes> boxes; boxes.push_back(StylBox(recs));
        AddSample(table, TextSample(text, boxes), 0, 1000);
    }

    /* differing color does not merge */
    {
        Bytes text; text.push_back('a'); text.push_back('b');
        std::vector<Bytes> recs;
        recs.push_back(StyleRec(0, 1, 1, 0, 12, 255, 0, 0, 255));
        recs.push_back(StyleRec(1, 2, 1, 0, 12, 0, 255, 0, 255));
        std::vector<Bytes> boxes; boxes.push_back(StylBox(recs));
        AddSample(table, TextSample(text, boxes), 0, 1000);
    }

    /* zero length record is ignored even with an out-of-range/bad font */
    {
        Bytes text; text.push_back('a'); text.push_back('b');
        std::vector<Bytes> recs;
        recs.push_back(StyleRec(0xFFFF, 0xFFFF, 999, 0xFF, 99, 1, 2, 3, 4));
        recs.push_back(StyleRec(0, 1, 1, 0x01, 12, 0, 0, 0, 255));
        std::vector<Bytes> boxes; boxes.push_back(StylBox(recs));
        AddSample(table, TextSample(text, boxes), 0, 1000);
    }

    /* isolated styled character splits identical defaults on both sides */
    {
        Bytes text; text.push_back('x'); text.push_back('y'); text.push_back('z');
        std::vector<Bytes> recs;
        recs.push_back(StyleRec(1, 2, 1, 0x01, 12, 0, 0, 0, 255));
        std::vector<Bytes> boxes; boxes.push_back(StylBox(recs));
        AddSample(table, TextSample(text, boxes), 0, 1000);
    }

    AP4_Track track(AP4_Track::TYPE_SUBTITLES, table, 1,
                    1000, 100000, 1000, 100000, "eng", 0, 0);
    AP4_Tx3gCue cue;

    CHECK_CHECK(AP4_SUCCEEDED(AP4_Tx3gDecoder::Decode(track, 0, cue)));
    CHECK_CHECK(cue.runs.ItemCount() == 1);
    CHECK_CHECK(cue.runs.size() == 1);
    CHECK_CHECK(cue.GetRunCount() == 1);
    CHECK_CHECK(cue.GetCts() == cue.cts);
    CHECK_CHECK(cue.GetDuration() == cue.duration);
    CHECK_CHECK(cue.GetText().GetLength() == cue.text.GetLength());
    unsigned int visited = 0;
    for (AP4_Tx3gRunArray::const_iterator it = cue.runs.begin();
         it != cue.runs.end(); ++it) {
        visited++;
        CHECK_CHECK(it->endChar == 6);
    }
    CHECK_CHECK(visited == 1);
    CHECK_CHECK(!cue.runs.empty());
    CHECK_CHECK(cue.runs[0].startChar == 0 && cue.runs[0].endChar == 6);
    CHECK_CHECK(cue.runs[0].bold);

    CHECK_CHECK(AP4_SUCCEEDED(AP4_Tx3gDecoder::Decode(track, 1, cue)));
    CHECK_CHECK(cue.runs.ItemCount() == 2);
    CHECK_CHECK(cue.runs[0].color.r == 255);
    CHECK_CHECK(cue.runs[1].color.g == 255);

    CHECK_CHECK(AP4_SUCCEEDED(AP4_Tx3gDecoder::Decode(track, 2, cue)));
    CHECK_CHECK(cue.runs.ItemCount() == 2);
    CHECK_CHECK(cue.runs[0].bold);
    CHECK_CHECK(!cue.runs[1].bold);

    CHECK_CHECK(AP4_SUCCEEDED(AP4_Tx3gDecoder::Decode(track, 3, cue)));
    CHECK_CHECK(cue.runs.ItemCount() == 3);
    CHECK_CHECK(cue.runs[0].startChar == 0 && cue.runs[0].endChar == 1);
    CHECK_CHECK(cue.runs[1].startChar == 1 && cue.runs[1].endChar == 2);
    CHECK_CHECK(cue.runs[2].startChar == 2 && cue.runs[2].endChar == 3);
}

/*----------------------------------------------------------------------
|   3. UTF-16 BOM decoding and surrogate pairs
+---------------------------------------------------------------------*/
static void
test_utf16()
{
    std::vector<FontDef> fonts;
    fonts.push_back((FontDef){1, "F"});
    Bytes entry = Tx3gEntry(1, 0, 16, 255,255,255,255, 0, 0,
                            0,0,0,0, 0,0,0,0, fonts);
    AP4_Tx3gSampleDescription* desc = ParseDesc(entry);
    AP4_SyntheticSampleTable* table = new AP4_SyntheticSampleTable();
    table->AddSampleDescription(desc);

    /* big endian: FE FF, A=0041, emoji=D83D DE00, zhong=4E2D, B=0042 */
    Bytes be;
    be.push_back(0xFE); be.push_back(0xFF);
    be.push_back(0x00); be.push_back('A');
    be.push_back(0xD8); be.push_back(0x3D);
    be.push_back(0xDE); be.push_back(0x00);
    be.push_back(0x4E); be.push_back(0x2D);
    be.push_back(0x00); be.push_back('B');

    /* little endian encoding of the same code units */
    Bytes le;
    le.push_back(0xFF); le.push_back(0xFE);
    le.push_back('A');  le.push_back(0x00);
    le.push_back(0x3D); le.push_back(0xD8);
    le.push_back(0x00); le.push_back(0xDE);
    le.push_back(0x2D); le.push_back(0x4E);
    le.push_back('B');  le.push_back(0x00);

    std::vector<Bytes> recs;
    recs.push_back(StyleRec(1, 3, 1, 0, 16, 0, 0, 0, 255));
    std::vector<Bytes> boxes; boxes.push_back(StylBox(recs));

    AddSample(table, TextSample(be, boxes), 0, 1000);
    AddSample(table, TextSample(le, boxes), 0, 1000);

    AP4_Track track(AP4_Track::TYPE_SUBTITLES, table, 1,
                    1000, 100000, 1000, 100000, "eng", 0, 0);
    Bytes expected = Utf8_AEmojiZhongB();

    for (AP4_Ordinal i = 0; i < 2; ++i) {
        AP4_Tx3gCue cue;
        CHECK_CHECK(AP4_SUCCEEDED(AP4_Tx3gDecoder::Decode(track, i, cue)));
        CHECK_CHECK(cue.text.GetLength() == expected.size());
        CHECK_CHECK(memcmp(cue.text.GetChars(), expected.data(),
                           expected.size()) == 0);
        CHECK_CHECK(cue.codepointCount == 4);
        CHECK_CHECK(cue.runs.ItemCount() == 3);
        /* [1,3) covers the surrogate pair and the CJK char: bytes [1,8) */
        CHECK_CHECK(cue.runs[1].startChar == 1 && cue.runs[1].endChar == 3);
        CHECK_CHECK(cue.runs[1].startByte == 1 && cue.runs[1].endByte == 8);
        CHECK_CHECK(cue.runs[1].text.GetLength() == 7);
    }

    /* bad: lone high surrogate */
    Bytes lone_high;
    lone_high.push_back(0xFE); lone_high.push_back(0xFF);
    lone_high.push_back(0xD8); lone_high.push_back(0x3D);
    AddSample(table, TextSample(lone_high, std::vector<Bytes>()), 0, 1000);

    /* bad: high surrogate followed by non-low */
    Bytes bad_pair;
    bad_pair.push_back(0xFE); bad_pair.push_back(0xFF);
    bad_pair.push_back(0xD8); bad_pair.push_back(0x3D);
    bad_pair.push_back(0x00); bad_pair.push_back(0x41);
    AddSample(table, TextSample(bad_pair, std::vector<Bytes>()), 0, 1000);

    /* bad: odd trailing byte */
    Bytes odd;
    odd.push_back(0xFE); odd.push_back(0xFF); odd.push_back(0x00);
    AddSample(table, TextSample(odd, std::vector<Bytes>()), 0, 1000);

    /* bad UTF-8: overlong NUL (C0 80) */
    Bytes overlong; overlong.push_back(0xC0); overlong.push_back(0x80);
    AddSample(table, TextSample(overlong, std::vector<Bytes>()), 0, 1000);

    /* bad UTF-8: surrogate code point ED A0 80 */
    Bytes utf8_surr;
    utf8_surr.push_back(0xED); utf8_surr.push_back(0xA0); utf8_surr.push_back(0x80);
    AddSample(table, TextSample(utf8_surr, std::vector<Bytes>()), 0, 1000);

    /* bad UTF-8: truncated 4 byte sequence */
    Bytes trunc4;
    trunc4.push_back(0xF0); trunc4.push_back(0x9F);
    AddSample(table, TextSample(trunc4, std::vector<Bytes>()), 0, 1000);

    for (AP4_Ordinal i = 2; i <= 6; ++i) {
        AP4_Tx3gCue cue;
        CHECK_CHECK(AP4_FAILED(AP4_Tx3gDecoder::Decode(track, i, cue)));
        /* a fresh cue stays default on failure */
        CHECK_CHECK(cue.text.GetLength() == 0);
    }
}

/*----------------------------------------------------------------------
|   4. empty text is a clear-screen cue and does not inherit old text
+---------------------------------------------------------------------*/
static void
test_clear_screen()
{
    std::vector<FontDef> fonts;
    fonts.push_back((FontDef){1, "F"});
    Bytes entry = Tx3gEntry(1, 0, 16, 255,255,255,255, 0, 0,
                            0,0,0,0, 0,0,0,0, fonts);
    AP4_Tx3gSampleDescription* desc = ParseDesc(entry);
    AP4_SyntheticSampleTable* table = new AP4_SyntheticSampleTable();
    table->AddSampleDescription(desc);

    Bytes hello; hello.push_back('h'); hello.push_back('i');
    AddSample(table, TextSample(hello, std::vector<Bytes>()), 0, 5000);

    Bytes empty; /* TextSample of an empty text -> 00 00 */
    AddSample(table, TextSample(empty, std::vector<Bytes>()), 0, 3000);

    AP4_Track track(AP4_Track::TYPE_SUBTITLES, table, 1,
                    1000, 100000, 1000, 100000, "eng", 0, 0);

    AP4_Tx3gCue cue;
    CHECK_CHECK(AP4_SUCCEEDED(AP4_Tx3gDecoder::Decode(track, 0, cue)));
    CHECK_CHECK(cue.text == "hi");
    /* std::string-style access works too */
    std::string std_text = cue.text;
    CHECK_CHECK(std_text == "hi");
    CHECK_CHECK(cue.text.size() == 2);
    CHECK_CHECK(cue.text.length() == 2);
    CHECK_CHECK(!cue.text.empty());
    CHECK_CHECK(strcmp(cue.text.c_str(), "hi") == 0);
    CHECK_CHECK(cue.runs[0].text.size() == 2);
    CHECK_CHECK(cue.runs.ItemCount() == 1);
    CHECK_CHECK(cue.duration == 5000);

    /* reuse the same cue object: old line must not survive */
    CHECK_CHECK(AP4_SUCCEEDED(AP4_Tx3gDecoder::Decode(track, 1, cue)));
    CHECK_CHECK(cue.text.GetLength() == 0);
    CHECK_CHECK(cue.text == "");
    CHECK_CHECK(cue.runs.ItemCount() == 0);
    CHECK_CHECK(cue.styles.ItemCount() == 0);
    CHECK_CHECK(cue.codepointCount == 0);
    CHECK_CHECK(cue.duration == 3000);
}

/*----------------------------------------------------------------------
|   5. per-sample description / font table switching
+---------------------------------------------------------------------*/
static void
test_description_switching()
{
    std::vector<FontDef> fonts0;
    fonts0.push_back((FontDef){1, "FontZero"});
    Bytes entry0 = Tx3gEntry(1, 0, 16, 255,255,255,255, 0, 0,
                             0,0,0,0, 0,0,0,0, fonts0);

    std::vector<FontDef> fonts1;
    fonts1.push_back((FontDef){1, "FontOneDifferent"});
    fonts1.push_back((FontDef){2, "FontTwo"});
    Bytes entry1 = Tx3gEntry(1, 0, 16, 255,255,255,255, 0, 0,
                             0,0,0,0, 0,0,0,0, fonts1);

    AP4_Tx3gSampleDescription* desc0 = ParseDesc(entry0);
    AP4_Tx3gSampleDescription* desc1 = ParseDesc(entry1);

    AP4_SyntheticSampleTable* table = new AP4_SyntheticSampleTable();
    table->AddSampleDescription(desc0);
    table->AddSampleDescription(desc1);

    Bytes aa; aa.push_back('a'); aa.push_back('a');
    Bytes bb; bb.push_back('b'); bb.push_back('b');
    Bytes cc; cc.push_back('c'); cc.push_back('c');
    Bytes dd; dd.push_back('d'); dd.push_back('d');

    std::vector<Bytes> r1; r1.push_back(StyleRec(0, 2, 1, 0, 16, 0,0,0,255));
    std::vector<Bytes> r2; r2.push_back(StyleRec(0, 2, 2, 0, 16, 0,0,0,255));
    std::vector<Bytes> rbad; rbad.push_back(StyleRec(0, 2, 2, 0, 16, 0,0,0,255));
    std::vector<Bytes> boxes1; boxes1.push_back(StylBox(r1));
    std::vector<Bytes> boxes2; boxes2.push_back(StylBox(r2));
    std::vector<Bytes> boxesbad; boxesbad.push_back(StylBox(rbad));

    AddSample(table, TextSample(aa, boxes1), 0, 1000); /* FontZero */
    AddSample(table, TextSample(bb, boxes1), 1, 1000); /* FontOneDifferent */
    AddSample(table, TextSample(cc, boxes2), 1, 1000); /* FontTwo */
    AddSample(table, TextSample(dd, boxesbad), 0, 1000); /* id2 absent in desc0 */
    AddSample(table, TextSample(aa, boxes1), 0, 1000); /* switch back */

    AP4_Track track(AP4_Track::TYPE_SUBTITLES, table, 1,
                    1000, 100000, 1000, 100000, "eng", 0, 0);

    AP4_Tx3gCue cue;
    CHECK_CHECK(AP4_SUCCEEDED(AP4_Tx3gDecoder::Decode(track, 0, cue)));
    CHECK_CHECK(strcmp(cue.runs[0].fontName.GetChars(), "FontZero") == 0);

    CHECK_CHECK(AP4_SUCCEEDED(AP4_Tx3gDecoder::Decode(track, 1, cue)));
    CHECK_CHECK(strcmp(cue.runs[0].fontName.GetChars(),
                       "FontOneDifferent") == 0);

    CHECK_CHECK(AP4_SUCCEEDED(AP4_Tx3gDecoder::Decode(track, 2, cue)));
    CHECK_CHECK(strcmp(cue.runs[0].fontName.GetChars(), "FontTwo") == 0);

    CHECK_CHECK(AP4_FAILED(AP4_Tx3gDecoder::Decode(track, 3, cue)));

    /* switching back to description 0 still resolves its own font table */
    CHECK_CHECK(AP4_SUCCEEDED(AP4_Tx3gDecoder::Decode(track, 4, cue)));
    CHECK_CHECK(strcmp(cue.runs[0].fontName.GetChars(), "FontZero") == 0);
}

/*----------------------------------------------------------------------
|   6. error cases leave the previous cue untouched
+---------------------------------------------------------------------*/
static void
test_errors_keep_cue()
{
    std::vector<FontDef> fonts;
    fonts.push_back((FontDef){1, "GoodFont"});
    Bytes entry = Tx3gEntry(1, 0, 16, 255,255,255,255, 0, 0,
                            7, 8, 9, 10, 1, 2, 3, 4, fonts);
    AP4_Tx3gSampleDescription* desc = ParseDesc(entry);
    AP4_SyntheticSampleTable* table = new AP4_SyntheticSampleTable();
    table->AddSampleDescription(desc);

    Bytes good; good.push_back('o'); good.push_back('k');
    {
        std::vector<Bytes> recs;
        recs.push_back(StyleRec(0, 2, 1, 0x01, 16, 1, 2, 3, 255));
        std::vector<Bytes> boxes; boxes.push_back(StylBox(recs));
        AddSample(table, TextSample(good, boxes), 0, 1234);
    }

    /* 1: buffer shorter than the 16 bit length field */
    {
        Bytes b; b.push_back(0x00);
        AddSample(table, b, 0, 1000);
    }
    /* 2: declared text length exceeds buffer */
    {
        Bytes b; b.push_back(0x00); b.push_back(0x0A); b.push_back('x');
        AddSample(table, b, 0, 1000);
    }
    /* 3: bad UTF-8 */
    {
        Bytes t; t.push_back(0xFF);
        AddSample(table, TextSample(t, std::vector<Bytes>()), 0, 1000);
    }
    /* 4: style range beyond text */
    {
        Bytes t; t.push_back('a'); t.push_back('b');
        std::vector<Bytes> recs;
        recs.push_back(StyleRec(0, 3, 1, 0, 16, 0,0,0,255));
        std::vector<Bytes> boxes; boxes.push_back(StylBox(recs));
        AddSample(table, TextSample(t, boxes), 0, 1000);
    }
    /* 5: reversed range */
    {
        Bytes t; t.push_back('a'); t.push_back('b'); t.push_back('c');
        std::vector<Bytes> recs;
        recs.push_back(StyleRec(2, 1, 1, 0, 16, 0,0,0,255));
        std::vector<Bytes> boxes; boxes.push_back(StylBox(recs));
        AddSample(table, TextSample(t, boxes), 0, 1000);
    }
    /* 6: unknown fontID */
    {
        Bytes t; t.push_back('a'); t.push_back('b');
        std::vector<Bytes> recs;
        recs.push_back(StyleRec(0, 2, 99, 0, 16, 0,0,0,255));
        std::vector<Bytes> boxes; boxes.push_back(StylBox(recs));
        AddSample(table, TextSample(t, boxes), 0, 1000);
    }
    /* 7: incomplete styl (count says 2 records, only 1 present) */
    {
        Bytes t; t.push_back('a'); t.push_back('b');
        std::vector<Bytes> recs;
        recs.push_back(StyleRec(0, 2, 1, 0, 16, 0,0,0,255));
        Bytes bad_styl = StylBox(recs, 2);
        std::vector<Bytes> boxes; boxes.push_back(bad_styl);
        AddSample(table, TextSample(t, boxes), 0, 1000);
    }
    /* 8: box length runs past the end of the sample */
    {
        Bytes t; t.push_back('a');
        Bytes bad;
        Put32(bad, 100);
        Put32(bad, AP4_ATOM_TYPE('s','t','y','l'));
        AddSample(table, TextSample(t, std::vector<Bytes>(1, bad)), 0, 1000);
    }
    /* 9: huge declared box size must not overflow bounds arithmetic */
    {
        Bytes t; t.push_back('a');
        Bytes bad;
        Put32(bad, 0xFFFFFFFFu);
        Put32(bad, AP4_ATOM_TYPE('z','z','z','z'));
        AddSample(table, TextSample(t, std::vector<Bytes>(1, bad)), 0, 1000);
    }

    AP4_Track track(AP4_Track::TYPE_SUBTITLES, table, 1,
                    1000, 100000, 1000, 100000, "eng", 0, 0);

    /* establish a non-empty sentinel cue */
    AP4_Tx3gCue cue;
    CHECK_CHECK(AP4_SUCCEEDED(AP4_Tx3gDecoder::Decode(track, 0, cue)));
    CHECK_CHECK(cue.text == "ok");
    CHECK_CHECK(cue.runs.ItemCount() == 1);
    AP4_String saved_text = cue.text;
    AP4_Cardinal saved_runs = cue.runs.ItemCount();
    AP4_UI64 saved_cts = cue.cts;
    AP4_UI32 saved_dur = cue.duration;

    for (AP4_Ordinal i = 1; i <= 9; ++i) {
        AP4_Result rr = AP4_Tx3gDecoder::Decode(track, i, cue);
        CHECK_CHECK(AP4_FAILED(rr));
        /* original cue unchanged */
        CHECK_CHECK(cue.text == saved_text);
        CHECK_CHECK(cue.runs.ItemCount() == saved_runs);
        CHECK_CHECK(cue.cts == saved_cts);
        CHECK_CHECK(cue.duration == saved_dur);
        CHECK_CHECK(cue.runs[0].fontName == "GoodFont");
    }
}

/*----------------------------------------------------------------------
|   7. unknown extension boxes are skipped, not appended to text
+---------------------------------------------------------------------*/
static void
test_unknown_extension_boxes()
{
    std::vector<FontDef> fonts;
    fonts.push_back((FontDef){1, "F"});
    Bytes entry = Tx3gEntry(1, 0, 16, 255,255,255,255, 0, 0,
                            0,0,0,0, 0,0,0,0, fonts);
    AP4_Tx3gSampleDescription* desc = ParseDesc(entry);
    AP4_SyntheticSampleTable* table = new AP4_SyntheticSampleTable();
    table->AddSampleDescription(desc);

    Bytes text; text.push_back('A'); text.push_back('B');

    /* unknown box with payload that looks like text bytes */
    Bytes junk_payload;
    junk_payload.push_back('X'); junk_payload.push_back('Y');
    junk_payload.push_back('Z');
    Bytes junk = MakeBox(AP4_ATOM_TYPE('z','z','z','z'), junk_payload);

    std::vector<Bytes> recs;
    recs.push_back(StyleRec(0, 1, 1, 0x01, 16, 0,0,0,255));
    Bytes styl = StylBox(recs);

    /* put junk before and after styl */
    std::vector<Bytes> boxes;
    boxes.push_back(junk);
    boxes.push_back(styl);
    boxes.push_back(junk);
    AddSample(table, TextSample(text, boxes), 0, 1000);

    AP4_Track track(AP4_Track::TYPE_SUBTITLES, table, 1,
                    1000, 100000, 1000, 100000, "eng", 0, 0);
    AP4_Tx3gCue cue;
    CHECK_CHECK(AP4_SUCCEEDED(AP4_Tx3gDecoder::Decode(track, 0, cue)));
    CHECK_CHECK(cue.text.GetLength() == 2);
    CHECK_CHECK(cue.text == "AB");
    CHECK_CHECK(cue.runs.ItemCount() == 2);
    CHECK_CHECK(cue.runs[0].bold);
    CHECK_CHECK(!cue.runs[1].bold);
}

/*----------------------------------------------------------------------
|   8. description write -> parse round trip preserves fields and fonts
+---------------------------------------------------------------------*/
static void
test_description_roundtrip()
{
    std::vector<FontDef> fonts;
    fonts.push_back((FontDef){3, "Round Font"});
    fonts.push_back((FontDef){9, "Nine"});

    Bytes extra;
    {
        Bytes p; p.push_back(1); p.push_back(2); p.push_back(3); p.push_back(4);
        extra = MakeBox(AP4_ATOM_TYPE('h','l','i','t'), p);
    }

    Bytes entry = Tx3gEntry(3, 0x01 | 0x04, 24, 11, 22, 33, 44,
                            -1, 1,
                            11, 22, 33, 44,
                            55, 66, 77, 88,
                            fonts, extra);
    AP4_Tx3gSampleDescription* desc = ParseDesc(entry);

    /* ToAtom + write + factory parse */
    AP4_Atom* atom = desc->ToAtom();
    CHECK_CHECK(atom != NULL);
    AP4_MemoryByteStream* mbs = new AP4_MemoryByteStream();
    CHECK_CHECK(AP4_SUCCEEDED(atom->Write(*mbs)));
    delete atom;

    mbs->Seek(0);
    AP4_AtomFactory factory;
    factory.PushContext(AP4_ATOM_TYPE_STSD);
    AP4_Atom* atom2 = NULL;
    CHECK_CHECK(AP4_SUCCEEDED(factory.CreateAtomFromStream(*mbs, atom2)));
    factory.PopContext();
    mbs->Release();

    AP4_Tx3gSampleEntry* e2 = AP4_DYNAMIC_CAST(AP4_Tx3gSampleEntry, atom2);
    CHECK_CHECK(e2 != NULL);
    AP4_Tx3gSampleDescription* d2 =
        AP4_DYNAMIC_CAST(AP4_Tx3gSampleDescription, e2->ToSampleDescription());
    CHECK_CHECK(d2 != NULL);

    CHECK_CHECK(d2->GetHorizontalJustification() == -1);
    CHECK_CHECK(d2->GetVerticalJustification() == 1);
    const AP4_Tx3gColor& bg = d2->GetBackgroundColor();
    CHECK_CHECK(bg.r == 55 && bg.g == 66 && bg.b == 77 && bg.a == 88);
    const AP4_Tx3gBox& bx = d2->GetDefaultTextBox();
    CHECK_CHECK(bx.top == 11 && bx.left == 22);
    CHECK_CHECK(bx.bottom == 33 && bx.right == 44);
    const AP4_Tx3gStyleRecord& sr = d2->GetDefaultStyleRecord();
    CHECK_CHECK(sr.fontID == 3);
    CHECK_CHECK(sr.faceStyleFlags == (0x01 | 0x04));
    CHECK_CHECK(sr.fontSize == 24);
    CHECK_CHECK(sr.textColorRGBA[0] == 11 && sr.textColorRGBA[3] == 44);
    CHECK_CHECK(d2->GetFontCount() == 2);
    AP4_String name;
    CHECK_CHECK(AP4_SUCCEEDED(d2->FindFontName(3, name)));
    CHECK_CHECK(name == "Round Font");
    CHECK_CHECK(AP4_SUCCEEDED(d2->FindFontName(9, name)));
    CHECK_CHECK(name == "Nine");

    /* the unknown extension box is preserved */
    const AP4_DataBuffer& other = d2->GetOtherBoxes();
    CHECK_CHECK(other.GetDataSize() == extra.size());
    CHECK_CHECK(memcmp(other.GetData(), extra.data(), extra.size()) == 0);

    /* Clone() also yields a typed description */
    AP4_Result clone_result = AP4_FAILURE;
    AP4_SampleDescription* clone = desc->Clone(&clone_result);
    CHECK_CHECK(AP4_SUCCEEDED(clone_result));
    CHECK_CHECK(clone != NULL);
    CHECK_CHECK(AP4_DYNAMIC_CAST(AP4_Tx3gSampleDescription, clone) != NULL);
    delete clone;
    delete atom2;
    delete d2;
    delete desc;
}

/*----------------------------------------------------------------------
|   9. full MP4 file write -> AP4_File parse -> Decode
+---------------------------------------------------------------------*/
static void
test_real_file()
{
    /* programmatic description (exercises the public constructor) */
    AP4_Tx3gStyleRecord def;
    def.fontID = 1;
    def.faceStyleFlags = 0;
    def.fontSize = 28;
    def.textColorRGBA[0] = 0xFF;
    def.textColorRGBA[1] = 0xFF;
    def.textColorRGBA[2] = 0xFF;
    def.textColorRGBA[3] = 0xFF;

    AP4_Array<AP4_Tx3gFontRecord> fonts;
    CHECK_CHECK(AP4_SUCCEEDED(fonts.Append(
        AP4_Tx3gFontRecord(1, "FileFont"))));

    AP4_Tx3gBox box(0, 0, 180, 320);
    AP4_Tx3gSampleDescription* desc = new AP4_Tx3gSampleDescription(
        AP4_SAMPLE_FORMAT_TX3G, def, 0, -1,
        AP4_Tx3gColor(0, 0, 0, 128), box, &fonts, NULL);

    AP4_SyntheticSampleTable* table = new AP4_SyntheticSampleTable();
    CHECK_CHECK(AP4_SUCCEEDED(table->AddSampleDescription(desc)));

    Bytes text = Utf8_AEmojiZhongB();
    std::vector<Bytes> recs;
    recs.push_back(StyleRec(1, 3, 1, 0x01, 28, 255, 0, 0, 255));
    std::vector<Bytes> boxes; boxes.push_back(StylBox(recs));
    Bytes s0 = TextSample(text, boxes);
    AddSample(table, s0, 0, 4000);

    Bytes empty;
    Bytes s1 = TextSample(empty, std::vector<Bytes>());
    AddSample(table, s1, 0, 2000);

    AP4_Track* track = new AP4_Track(AP4_Track::TYPE_SUBTITLES, table, 1,
                                     1000, 6000, 1000, 6000,
                                     "eng", 0, 0);
    AP4_Movie* movie = new AP4_Movie();
    CHECK_CHECK(AP4_SUCCEEDED(movie->AddTrack(track)));
    AP4_File file(movie);

    AP4_MemoryByteStream* file_bytes = new AP4_MemoryByteStream();
    AP4_Result wr = AP4_FileWriter::Write(file, *file_bytes);
    CHECK_CHECK(AP4_SUCCEEDED(wr));
    AP4_LargeSize written_size = 0;
    file_bytes->GetSize(written_size);
    CHECK_CHECK(written_size > 64);

    /* parse the serialized file through the real AP4_File parser */
    file_bytes->Seek(0);
    {
        AP4_File reparsed(*file_bytes);
        AP4_Movie* parsed_movie = reparsed.GetMovie();
        CHECK_CHECK(parsed_movie != NULL);
        AP4_List<AP4_Track>& tracks = parsed_movie->GetTracks();
        CHECK_CHECK(tracks.ItemCount() == 1);

        AP4_Track* parsed_track = NULL;
        CHECK_CHECK(AP4_SUCCEEDED(tracks.Get(0, parsed_track)));
        CHECK_CHECK(parsed_track != NULL);

        AP4_SampleDescription* psd = parsed_track->GetSampleDescription(0);
        CHECK_CHECK(psd != NULL);
        AP4_Tx3gSampleDescription* ptd =
            AP4_DYNAMIC_CAST(AP4_Tx3gSampleDescription, psd);
        CHECK_CHECK(ptd != NULL);
        CHECK_CHECK(ptd->HasFont(1));
        AP4_String fname;
        CHECK_CHECK(AP4_SUCCEEDED(ptd->FindFontName(1, fname)));
        CHECK_CHECK(fname == "FileFont");
        CHECK_CHECK(ptd->GetDefaultStyleRecord().fontSize == 28);

        AP4_Tx3gCue cue;
        CHECK_CHECK(AP4_SUCCEEDED(AP4_Tx3gDecoder::Decode(*parsed_track, 0, cue)));
        CHECK_CHECK(cue.text.GetLength() == text.size());
        CHECK_CHECK(memcmp(cue.text.GetChars(), text.data(), text.size()) == 0);
        CHECK_CHECK(cue.duration == 4000);
        CHECK_CHECK(cue.runs.ItemCount() == 3);
        bool found_styled = false;
        for (AP4_Cardinal i = 0; i < cue.runs.ItemCount(); ++i) {
            if (cue.runs[i].startChar == 1 && cue.runs[i].endChar == 3) {
                CHECK_CHECK(cue.runs[i].bold);
                CHECK_CHECK(strcmp(cue.runs[i].fontName.GetChars(),
                                   "FileFont") == 0);
                found_styled = true;
            }
        }
        CHECK_CHECK(found_styled);

        /* second sample is the clear-screen cue */
        AP4_Tx3gCue cue2;
        CHECK_CHECK(AP4_SUCCEEDED(AP4_Tx3gDecoder::Decode(*parsed_track, 1, cue2)));
        CHECK_CHECK(cue2.text.GetLength() == 0);
        CHECK_CHECK(cue2.runs.ItemCount() == 0);
        CHECK_CHECK(cue2.duration == 2000);
    }
    file_bytes->Release();
}

/*----------------------------------------------------------------------
|   10. baseline-style generic description fallback / other formats
+---------------------------------------------------------------------*/
static void
test_generic_fallback_and_other_formats()
{
    /* exactly the baseline setup: a generic TX3G-format description */
    {
        AP4_SyntheticSampleTable* table = new AP4_SyntheticSampleTable();
        CHECK_CHECK(AP4_SUCCEEDED(table->AddSampleDescription(
            new AP4_SampleDescription(AP4_SampleDescription::TYPE_UNKNOWN,
                                      AP4_SAMPLE_FORMAT_TX3G, 0))));
        Bytes s;
        s.push_back(0); s.push_back(5);
        s.push_back('h'); s.push_back('e'); s.push_back('l');
        s.push_back('l'); s.push_back('o');
        AddSample(table, s, 0, 2000);

        AP4_Track track(AP4_Track::TYPE_SUBTITLES, table, 1,
                        1000, 2000, 1000, 2000, "eng", 0, 0);
        AP4_Tx3gCue cue;
        CHECK_CHECK(AP4_SUCCEEDED(AP4_Tx3gDecoder::Decode(track, 0, cue)));
        CHECK_CHECK(cue.text == "hello");
        CHECK_CHECK(cue.runs.ItemCount() == 1);
        CHECK_CHECK(cue.duration == 2000);
    }

    /* a non-tx3g sample description is rejected, cue untouched */
    {
        AP4_SyntheticSampleTable* table = new AP4_SyntheticSampleTable();
        CHECK_CHECK(AP4_SUCCEEDED(table->AddSampleDescription(
            new AP4_SampleDescription(AP4_SampleDescription::TYPE_UNKNOWN,
                                      AP4_ATOM_TYPE('z','z','z','z'), 0))));
        Bytes s; s.push_back(0); s.push_back(0);
        AddSample(table, s, 0, 1000);

        AP4_Track track(AP4_Track::TYPE_SUBTITLES, table, 1,
                        1000, 1000, 1000, 1000, "eng", 0, 0);
        AP4_Tx3gCue cue;
        cue.cts = 777;
        CHECK_CHECK(AP4_FAILED(AP4_Tx3gDecoder::Decode(track, 0, cue)));
        CHECK_CHECK(cue.cts == 777);
    }

    /* out of range sample index */
    {
        AP4_SyntheticSampleTable* table = new AP4_SyntheticSampleTable();
        table->AddSampleDescription(
            new AP4_SampleDescription(AP4_SampleDescription::TYPE_UNKNOWN,
                                     AP4_SAMPLE_FORMAT_TX3G, 0));
        AP4_Track track(AP4_Track::TYPE_SUBTITLES, table, 1,
                        1000, 1000, 1000, 1000, "eng", 0, 0);
        AP4_Tx3gCue cue;
        CHECK_CHECK(AP4_FAILED(AP4_Tx3gDecoder::Decode(track, 3, cue)));
    }
}

/*----------------------------------------------------------------------
|   entry point
+---------------------------------------------------------------------*/
void
run_tx3g_tests()
{
    test_overlapping_styles();
    test_merging_and_zero_records();
    test_utf16();
    test_clear_screen();
    test_description_switching();
    test_errors_keep_cue();
    test_unknown_extension_boxes();
    test_description_roundtrip();
    test_real_file();
    test_generic_fallback_and_other_formats();
}
