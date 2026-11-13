/*
 * Copyright (C) 2024 magicxqq <xqq@xqq.im>. All rights reserved.
 *
 * This file is part of libaribcaption.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

// Regression tests for Decoder::DecodeSequence(): TIME-driven incremental caption fragments.
//
// All PES packets below are hand-built from raw ARIB STD-B24 bytes, including packets that
// carry several data units (DRCS definition + statement body) inside one PES, so that the
// single-time-axis and per-fragment DRCS ownership rules can be verified.

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <string>
#include <vector>

#include "test_support.h"

#include <aribcaption/aribcaption.hpp>
#include <aribcaption/decoder.h>

using namespace aribcaption;

namespace {

using Bytes = std::vector<uint8_t>;

void AppendU16(Bytes& v, uint16_t n) {
    v.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
    v.push_back(static_cast<uint8_t>(n & 0xFF));
}

void AppendU24(Bytes& v, uint32_t n) {
    v.push_back(static_cast<uint8_t>((n >> 16) & 0xFF));
    v.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
    v.push_back(static_cast<uint8_t>(n & 0xFF));
}

Bytes MakeDataUnit(uint8_t data_unit_parameter, const Bytes& body) {
    Bytes unit;
    unit.push_back(0x1F);  // unit_separator
    unit.push_back(data_unit_parameter);
    AppendU24(unit, static_cast<uint32_t>(body.size()));
    unit.insert(unit.end(), body.begin(), body.end());
    return unit;
}

// Wrap already concatenated data units into a caption statement PES.
// dgi_id selects the language id (data_group_id low nibble), group_bit is the DGI group bit.
Bytes MakeStatementPES(const Bytes& data_units, uint8_t dgi_id = 1, uint8_t group_bit = 0,
                       uint8_t data_identifier = 0x80) {
    Bytes statement;
    statement.push_back(0x00);  // TMD == 0b00, no OTM
    AppendU24(statement, static_cast<uint32_t>(data_units.size()));
    statement.insert(statement.end(), data_units.begin(), data_units.end());

    Bytes data_group;
    data_group.push_back(static_cast<uint8_t>(((group_bit & 1) << 5) | ((dgi_id & 0x0F) << 2)));
    data_group.push_back(0x00);
    data_group.push_back(0x00);
    AppendU16(data_group, static_cast<uint16_t>(statement.size()));
    data_group.insert(data_group.end(), statement.begin(), statement.end());

    Bytes pes;
    pes.push_back(data_identifier);  // data_identifier (0x80 caption / 0x81 superimpose)
    pes.push_back(0xFF);             // private_stream_id
    pes.push_back(0xF0);             // PES_data_packet_header_length == 0; data group starts at index 3
    pes.insert(pes.end(), data_group.begin(), data_group.end());
    return pes;
}

Bytes MakeStatementUnit(const Bytes& body) {
    return MakeDataUnit(0x20, body);
}

Bytes MakeStatementUnit(std::initializer_list<Bytes> parts) {
    Bytes body;
    for (const Bytes& part : parts) {
        body.insert(body.end(), part.begin(), part.end());
    }
    return MakeStatementUnit(body);
}

// A 1-byte DRCS definition unit (data_unit_parameter 0x30) targeting DRCS set 1 / char 0x21.
Bytes MakeDRCSUnit(const Bytes& bitmap, uint8_t width, uint8_t height) {
    Bytes body;
    body.push_back(1);  // number_of_code
    AppendU16(body, 0x4121);  // character_code: f = 0x41 (DRCS1), char = 0x21
    body.push_back(1);        // number_of_font
    body.push_back(0x01 << 4);  // font_id = 1, mode = 0b0000 (bitmap)
    body.push_back(0);         // depth - 2 == 0 -> depth 2 (1 bit/pixel)
    body.push_back(width);
    body.push_back(height);
    body.insert(body.end(), bitmap.begin(), bitmap.end());
    return MakeDataUnit(0x30, body);
}

// STD-B24 control sequences used by the tests.
Bytes TimeWait(uint8_t n) { return {0x9D, 0x20, static_cast<uint8_t>(n & 0x3F)}; }
Bytes ClearScreen() { return {0x0C}; }
Bytes LockingShift1() { return {0x0E}; }  // GL -> G1 (alphanumeric)
Bytes RedForeground() { return {0x81}; }  // C1 RDF: red text color (CLUT palette 0 index 1)
Bytes ActivePositionSet(uint8_t y, uint8_t x) { return {0x1C, y, x}; }
Bytes SelectDRCS1IntoGL() { return {0x1B, 0x28, 0x20, 0x41}; }  // ESC ( SP A: 1-byte DRCS1 -> G0/GL

// Fullwidth alphanumeric characters are reached through G1 after LS1.
Bytes Fullwidth(char c) { return {static_cast<uint8_t>(c)}; }

constexpr uint32_t kDRCSCode = (1u << 16) | 0x21u;  // map_index 1 (DRCS_1), key 0x21
constexpr uint32_t kRedRGBA = 0xFF0000FFu;           // ColorRGBA(255, 0, 0, 255)

// A, wait 500ms, B, wait 300ms, clear screen. The statements live in two separate data units
// inside one PES, exercising the single-time-axis rule across data unit boundaries. The graphic
// set (LS1) and text color are selected only in the first unit so the second unit also proves
// that codeset / color state survives a fragment cut and a data-unit boundary.
Bytes BuildMultiUnitPES() {
    Bytes unit1 = MakeStatementUnit({
        ActivePositionSet(0, 0),
        RedForeground(),
        LockingShift1(),
        Fullwidth('A'),
        TimeWait(5),  // 5 * 100ms = 500ms
    });
    Bytes unit2 = MakeStatementUnit({
        Fullwidth('B'),  // GL still G1 and color still red after the cut
        TimeWait(3),     // 3 * 100ms = 300ms
        ClearScreen(),
    });
    Bytes units;
    units.insert(units.end(), unit1.begin(), unit1.end());
    units.insert(units.end(), unit2.begin(), unit2.end());
    return MakeStatementPES(units);
}

}  // namespace

void run_time_sequence_tests() {
    // -----------------------------------------------------------------------------------
    // 1. Multiple data units, incremental drawing, timing, positions and clear screen.
    // -----------------------------------------------------------------------------------
    {
        Context context;
        Decoder decoder(context);
        CHECK_CHECK(decoder.Initialize());

        Bytes pes = BuildMultiUnitPES();
        DecodeSequenceResult seq;
        DecodeStatus status = decoder.DecodeSequence(pes.data(), pes.size(), 1000, seq);
        CHECK_CHECK(status == DecodeStatus::kGotCaption);
        CHECK_CHECK(seq.captions.size() == 3);

        // Fragment 0: A starts at pts 1000 and stays for 500ms.
        Caption& f0 = *seq.captions[0];
        CHECK_CHECK(f0.pts == 1000);
        CHECK_CHECK(f0.wait_duration == 500);
        CHECK_CHECK((f0.flags & CaptionFlags::kCaptionFlagsWaitDuration) != 0);
        CHECK_CHECK((f0.flags & CaptionFlags::kCaptionFlagsClearScreen) == 0);
        CHECK_CHECK(f0.regions.size() == 1);
        CHECK_CHECK(f0.regions[0].chars.size() == 1);
        CHECK_CHECK(f0.regions[0].x == 0);
        CHECK_CHECK(f0.regions[0].y == 0);
        CHECK_CHECK(f0.regions[0].chars[0].codepoint == 0xFF21u);  // FULLWIDTH LATIN CAPITAL LETTER A
        CHECK_CHECK(f0.regions[0].chars[0].x == 0);
        CHECK_CHECK(f0.regions[0].chars[0].text_color.u32 == kRedRGBA);  // RDF selected before the cut
        CHECK_CHECK(f0.text == std::string(f0.regions[0].chars[0].u8str));
        CHECK_CHECK(f0.drcs_map.empty());

        // Fragment 1: B is incremental, starts at 1500 and stays 300ms; A must not be repeated.
        Caption& f1 = *seq.captions[1];
        CHECK_CHECK(f1.pts == 1500);
        CHECK_CHECK(f1.wait_duration == 300);
        CHECK_CHECK((f1.flags & CaptionFlags::kCaptionFlagsWaitDuration) != 0);
        CHECK_CHECK((f1.flags & CaptionFlags::kCaptionFlagsClearScreen) == 0);
        CHECK_CHECK(f1.regions.size() == 1);
        CHECK_CHECK(f1.regions[0].chars.size() == 1);
        CHECK_CHECK(f1.regions[0].x == 40);  // active position carried over after A (36 + 4 spacing)
        CHECK_CHECK(f1.regions[0].chars[0].codepoint == 0xFF22u);  // FULLWIDTH B
        CHECK_CHECK(f1.regions[0].chars[0].text_color.u32 == kRedRGBA);  // color state survived the cut
        CHECK_CHECK(f1.text.size() == 3);  // single 3-byte UTF-8 fullwidth character

        // Fragment 2: pure clear screen at 1800, indefinite duration, no characters.
        Caption& f2 = *seq.captions[2];
        CHECK_CHECK(f2.pts == 1800);
        CHECK_CHECK(f2.wait_duration == DURATION_INDEFINITE);
        CHECK_CHECK((f2.flags & CaptionFlags::kCaptionFlagsClearScreen) != 0);
        CHECK_CHECK((f2.flags & CaptionFlags::kCaptionFlagsWaitDuration) == 0);
        CHECK_CHECK(f2.regions.empty());
        CHECK_CHECK(f2.text.empty());

        // Fragments own independent storage.
        CHECK_CHECK(seq.captions[0].get() != seq.captions[1].get());
        CHECK_CHECK(seq.captions[1].get() != seq.captions[2].get());
    }

    // -----------------------------------------------------------------------------------
    // 2. Per-fragment DRCS ownership: a redefined DRCS code must not mutate older fragments.
    // -----------------------------------------------------------------------------------
    {
        Context context;
        Decoder decoder(context);
        CHECK_CHECK(decoder.Initialize());

        const Bytes bitmap_v1 = {0xF0, 0x0F};
        const Bytes bitmap_v2 = {0x0F, 0xF0};

        Bytes drcs_define_v1 = MakeDRCSUnit(bitmap_v1, 4, 4);
        Bytes statement_v1 = MakeStatementUnit({
            ActivePositionSet(0, 0),
            SelectDRCS1IntoGL(),
            Fullwidth(0x21),  // DRCS character key 0x21
            TimeWait(3),      // 300ms
        });
        Bytes drcs_define_v2 = MakeDRCSUnit(bitmap_v2, 4, 4);  // same code, new glyph
        Bytes statement_v2 = MakeStatementUnit({
            SelectDRCS1IntoGL(),
            Fullwidth(0x21),
        });

        Bytes units;
        units.insert(units.end(), drcs_define_v1.begin(), drcs_define_v1.end());
        units.insert(units.end(), statement_v1.begin(), statement_v1.end());
        units.insert(units.end(), drcs_define_v2.begin(), drcs_define_v2.end());
        units.insert(units.end(), statement_v2.begin(), statement_v2.end());
        Bytes pes = MakeStatementPES(units);

        DecodeSequenceResult seq;
        CHECK_CHECK(decoder.DecodeSequence(pes.data(), pes.size(), 2000, seq) == DecodeStatus::kGotCaption);
        CHECK_CHECK(seq.captions.size() == 2);

        Caption& g0 = *seq.captions[0];
        CHECK_CHECK(g0.pts == 2000);
        CHECK_CHECK(g0.wait_duration == 300);
        CHECK_CHECK(g0.regions.size() == 1);
        CHECK_CHECK(g0.regions[0].chars.size() == 1);
        CHECK_CHECK(g0.regions[0].chars[0].type == CaptionCharType::kDRCS);
        CHECK_CHECK(g0.regions[0].chars[0].drcs_code == kDRCSCode);
        auto d0 = g0.drcs_map.find(kDRCSCode);
        CHECK_CHECK(d0 != g0.drcs_map.end());
        CHECK_CHECK(d0->second.pixels == bitmap_v1);

        Caption& g1 = *seq.captions[1];
        CHECK_CHECK(g1.pts == 2300);
        CHECK_CHECK(g1.wait_duration == DURATION_INDEFINITE);
        CHECK_CHECK(g1.regions[0].chars[0].type == CaptionCharType::kDRCS);
        CHECK_CHECK(g1.regions[0].chars[0].drcs_code == kDRCSCode);
        auto d1 = g1.drcs_map.find(kDRCSCode);
        CHECK_CHECK(d1 != g1.drcs_map.end());
        CHECK_CHECK(d1->second.pixels == bitmap_v2);

        // The already published first fragment still carries the old glyph after redefinition.
        CHECK_CHECK(g0.drcs_map.find(kDRCSCode)->second.pixels == bitmap_v1);
        CHECK_CHECK(g0.drcs_map.find(kDRCSCode)->second.pixels !=
                    g1.drcs_map.find(kDRCSCode)->second.pixels);
    }

    // -----------------------------------------------------------------------------------
    // 3. TIME 0 does not cut; waits without display changes produce no empty fragment.
    // -----------------------------------------------------------------------------------
    {
        Context context;
        Decoder decoder(context);
        CHECK_CHECK(decoder.Initialize());

        Bytes unit = MakeStatementUnit({
            ActivePositionSet(0, 0),
            TimeWait(2),  // 200ms with nothing drawn yet: only advances time, no fragment
            LockingShift1(),
            Fullwidth('A'),
            TimeWait(0),  // TIME 0: no cut and no time advance
            Fullwidth('B'),
            TimeWait(3),  // 300ms: cuts "AB"
            Fullwidth('C'),
        });
        Bytes pes = MakeStatementPES(unit);

        DecodeSequenceResult seq;
        CHECK_CHECK(decoder.DecodeSequence(pes.data(), pes.size(), 5000, seq) == DecodeStatus::kGotCaption);
        CHECK_CHECK(seq.captions.size() == 2);

        Caption& h0 = *seq.captions[0];
        CHECK_CHECK(h0.pts == 5200);  // base 5000 + the leading 200ms wait
        CHECK_CHECK(h0.wait_duration == 300);
        CHECK_CHECK(h0.regions[0].chars.size() == 2);  // A and B stayed in the same pending fragment
        CHECK_CHECK(h0.regions[0].chars[0].codepoint == 0xFF21u);
        CHECK_CHECK(h0.regions[0].chars[1].codepoint == 0xFF22u);

        Caption& h1 = *seq.captions[1];
        CHECK_CHECK(h1.pts == 5500);
        CHECK_CHECK(h1.wait_duration == DURATION_INDEFINITE);
        CHECK_CHECK(h1.regions[0].chars.size() == 1);
        CHECK_CHECK(h1.regions[0].chars[0].codepoint == 0xFF23u);
    }

    // -----------------------------------------------------------------------------------
    // 4. Unknown PTS is preserved as PTS_NOPTS; arithmetic on the sentinel is forbidden.
    // -----------------------------------------------------------------------------------
    {
        Context context;
        Decoder decoder(context);
        CHECK_CHECK(decoder.Initialize());

        Bytes unit = MakeStatementUnit({
            ActivePositionSet(0, 0),
            LockingShift1(),
            Fullwidth('A'),
            TimeWait(4),
            Fullwidth('B'),
        });
        Bytes pes = MakeStatementPES(unit);

        DecodeSequenceResult seq;
        CHECK_CHECK(decoder.DecodeSequence(pes.data(), pes.size(), PTS_NOPTS, seq) ==
                    DecodeStatus::kGotCaption);
        CHECK_CHECK(seq.captions.size() == 2);
        CHECK_CHECK(seq.captions[0]->pts == PTS_NOPTS);
        CHECK_CHECK(seq.captions[0]->wait_duration == 400);
        CHECK_CHECK(seq.captions[1]->pts == PTS_NOPTS);
        CHECK_CHECK(seq.captions[1]->wait_duration == DURATION_INDEFINITE);
    }

    // -----------------------------------------------------------------------------------
    // 5. Truncated instruction and malformed data unit: error, no partial sequence.
    // -----------------------------------------------------------------------------------
    {
        Context context;
        Decoder decoder(context);
        CHECK_CHECK(decoder.Initialize());

        // Reuse a good result first to prove an error empties the output instead of leaking it.
        Bytes good = BuildMultiUnitPES();
        DecodeSequenceResult seq;
        CHECK_CHECK(decoder.DecodeSequence(good.data(), good.size(), 1000, seq) ==
                    DecodeStatus::kGotCaption);
        CHECK_CHECK(!seq.captions.empty());

        // Truncated TIME instruction (missing its third byte).
        Bytes truncated_unit = MakeStatementUnit(Bytes{0x9D, 0x20});
        Bytes truncated_pes = MakeStatementPES(truncated_unit);
        CHECK_CHECK(decoder.DecodeSequence(truncated_pes.data(), truncated_pes.size(), 1000, seq) ==
                    DecodeStatus::kError);
        CHECK_CHECK(seq.captions.empty());

        // Bad unit separator inside a data unit.
        Bytes bad_unit = MakeDataUnit(0x20, Bytes{0x0E, 'A'});
        bad_unit[0] = 0x1E;  // corrupt unit_separator (valid is 0x1F)
        Bytes bad_pes = MakeStatementPES(bad_unit);
        CHECK_CHECK(decoder.DecodeSequence(bad_pes.data(), bad_pes.size(), 1000, seq) ==
                    DecodeStatus::kError);
        CHECK_CHECK(seq.captions.empty());

        // A truncated PES header is rejected as well.
        CHECK_CHECK(decoder.DecodeSequence(good.data(), 2, 1000, seq) == DecodeStatus::kError);
        CHECK_CHECK(seq.captions.empty());
    }

    // -----------------------------------------------------------------------------------
    // 6. PTS + accumulated wait overflow: error and no published fragment.
    // -----------------------------------------------------------------------------------
    {
        Context context;
        Decoder decoder(context);
        CHECK_CHECK(decoder.Initialize());

        Bytes unit = MakeStatementUnit({
            ActivePositionSet(0, 0),
            LockingShift1(),
            Fullwidth('A'),
            TimeWait(3),  // 300ms
        });
        Bytes pes = MakeStatementPES(unit);

        DecodeSequenceResult seq;
        int64_t near_max = std::numeric_limits<int64_t>::max() - 10;
        CHECK_CHECK(decoder.DecodeSequence(pes.data(), pes.size(), near_max, seq) ==
                    DecodeStatus::kError);
        CHECK_CHECK(seq.captions.empty());

        // The same wait against PTS_NOPTS must not trip the overflow check.
        DecodeSequenceResult nopts_seq;
        CHECK_CHECK(decoder.DecodeSequence(pes.data(), pes.size(), PTS_NOPTS, nopts_seq) ==
                    DecodeStatus::kGotCaption);
        CHECK_CHECK(nopts_seq.captions.size() == 1);
        CHECK_CHECK(nopts_seq.captions[0]->pts == PTS_NOPTS);
    }

    // -----------------------------------------------------------------------------------
    // 7. Language filtering and Flush() conventions are shared with the legacy entry.
    // -----------------------------------------------------------------------------------
    {
        Context context;
        Decoder decoder(context);
        CHECK_CHECK(decoder.Initialize());

        Bytes unit = MakeStatementUnit({LockingShift1(), Fullwidth('A')});
        Bytes second_language_pes = MakeStatementPES(unit, /*dgi_id=*/2);
        DecodeSequenceResult seq;
        CHECK_CHECK(decoder.DecodeSequence(second_language_pes.data(), second_language_pes.size(),
                                           1000, seq) == DecodeStatus::kNoCaption);
        CHECK_CHECK(seq.captions.empty());

        // Flush resets state and the decoder keeps working afterwards.
        decoder.Flush();
        Bytes good = BuildMultiUnitPES();
        CHECK_CHECK(decoder.DecodeSequence(good.data(), good.size(), 1000, seq) ==
                    DecodeStatus::kGotCaption);
        CHECK_CHECK(seq.captions.size() == 3);
    }

    // -----------------------------------------------------------------------------------
    // 8. Legacy Decode() still returns the single aggregated caption on the same packet.
    // -----------------------------------------------------------------------------------
    {
        Context context;
        Decoder decoder(context);
        CHECK_CHECK(decoder.Initialize());

        Bytes pes = BuildMultiUnitPES();

        DecodeResult agg;
        CHECK_CHECK(decoder.Decode(pes.data(), pes.size(), 1000, agg) == DecodeStatus::kGotCaption);
        CHECK_CHECK(agg.caption != nullptr);
        CHECK_CHECK(agg.caption->pts == 1000);
        CHECK_CHECK(agg.caption->wait_duration == 800);  // 500 + 300 accumulated
        CHECK_CHECK((agg.caption->flags & CaptionFlags::kCaptionFlagsClearScreen) != 0);
        CHECK_CHECK((agg.caption->flags & CaptionFlags::kCaptionFlagsWaitDuration) != 0);
        // Aggregation keeps every character, whereas the sequence fragments are incremental.
        std::size_t total_chars = 0;
        for (const CaptionRegion& region : agg.caption->regions) {
            total_chars += region.chars.size();
        }
        CHECK_CHECK(total_chars == 2);

        DecodeSequenceResult seq;
        CHECK_CHECK(decoder.DecodeSequence(pes.data(), pes.size(), 1000, seq) ==
                    DecodeStatus::kGotCaption);
        CHECK_CHECK(seq.captions.size() == 3);
        std::size_t incremental_chars = 0;
        for (const auto& fragment : seq.captions) {
            for (const CaptionRegion& region : fragment->regions) {
                incremental_chars += region.chars.size();
            }
        }
        CHECK_CHECK(incremental_chars == total_chars);
    }

    // -----------------------------------------------------------------------------------
    // 9. C API: identical content, independent allocations, idempotent cleanup, DRCS free.
    // -----------------------------------------------------------------------------------
    {
        aribcc_context_t* ctx = aribcc_context_alloc();
        aribcc_decoder_t* dec = aribcc_decoder_alloc(ctx);
        CHECK_CHECK(aribcc_decoder_initialize(dec,
                                              ARIBCC_ENCODING_SCHEME_ARIB_STD_B24_JIS,
                                              ARIBCC_CAPTIONTYPE_CAPTION,
                                              ARIBCC_PROFILE_A,
                                              ARIBCC_LANGUAGEID_FIRST));

        Bytes pes = BuildMultiUnitPES();
        aribcc_decode_sequence_result_t result;
        aribcc_decode_status_t cstatus =
            aribcc_decoder_decode_sequence(dec, pes.data(), pes.size(), 1000, &result);
        CHECK_CHECK(cstatus == ARIBCC_DECODE_STATUS_GOT_CAPTION);
        CHECK_CHECK(result.caption_count == 3);

        const aribcc_caption_t& c0 = result.captions[0];
        CHECK_CHECK(c0.pts == 1000);
        CHECK_CHECK(c0.wait_duration == 500);
        CHECK_CHECK((c0.flags & ARIBCC_CAPTIONFLAGS_WAITDURATION) != 0);
        CHECK_CHECK(c0.region_count == 1);
        CHECK_CHECK(c0.regions[0].char_count == 1);
        CHECK_CHECK(c0.regions[0].x == 0);
        CHECK_CHECK(c0.regions[0].chars[0].codepoint == 0xFF21u);
        CHECK_CHECK(c0.regions[0].chars[0].text_color == kRedRGBA);

        const aribcc_caption_t& c1 = result.captions[1];
        CHECK_CHECK(c1.pts == 1500);
        CHECK_CHECK(c1.wait_duration == 300);
        CHECK_CHECK(c1.regions[0].x == 40);
        CHECK_CHECK(c1.regions[0].chars[0].codepoint == 0xFF22u);
        CHECK_CHECK(c1.regions[0].chars[0].text_color == kRedRGBA);  // color survived the cut

        const aribcc_caption_t& c2 = result.captions[2];
        CHECK_CHECK(c2.pts == 1800);
        CHECK_CHECK(c2.wait_duration == ARIBCC_DURATION_INDEFINITE);
        CHECK_CHECK((c2.flags & ARIBCC_CAPTIONFLAGS_CLEARSCREEN) != 0);
        CHECK_CHECK(c2.region_count == 0);
        CHECK_CHECK(c2.text == nullptr);

        // Cleanup must be repeatable.
        aribcc_decoder_decode_sequence_cleanup(&result);
        CHECK_CHECK(result.captions == nullptr);
        CHECK_CHECK(result.caption_count == 0);
        aribcc_decoder_decode_sequence_cleanup(&result);  // second call must be a no-op
        aribcc_decode_sequence_result_t zeroed{};
        aribcc_decoder_decode_sequence_cleanup(&zeroed);  // cleanup on an empty result is safe

        // A sequence carrying DRCS: every fragment's map and char arrays are released by cleanup.
        const Bytes bitmap_v1 = {0xF0, 0x0F};
        const Bytes bitmap_v2 = {0x0F, 0xF0};
        Bytes drcs_units;
        Bytes d1 = MakeDRCSUnit(bitmap_v1, 4, 4);
        Bytes s1 = MakeStatementUnit({ActivePositionSet(0, 0), SelectDRCS1IntoGL(),
                                      Fullwidth(0x21), TimeWait(3)});
        Bytes d2 = MakeDRCSUnit(bitmap_v2, 4, 4);
        Bytes s2 = MakeStatementUnit({SelectDRCS1IntoGL(), Fullwidth(0x21)});
        drcs_units.insert(drcs_units.end(), d1.begin(), d1.end());
        drcs_units.insert(drcs_units.end(), s1.begin(), s1.end());
        drcs_units.insert(drcs_units.end(), d2.begin(), d2.end());
        drcs_units.insert(drcs_units.end(), s2.begin(), s2.end());
        Bytes drcs_pes = MakeStatementPES(drcs_units);

        aribcc_decode_sequence_result_t drcs_result;
        CHECK_CHECK(aribcc_decoder_decode_sequence(dec, drcs_pes.data(), drcs_pes.size(),
                                                   2000, &drcs_result) ==
                    ARIBCC_DECODE_STATUS_GOT_CAPTION);
        CHECK_CHECK(drcs_result.caption_count == 2);
        CHECK_CHECK(drcs_result.captions[0].drcs_map != nullptr);
        aribcc_drcs_t* drcs = aribcc_drcsmap_get(drcs_result.captions[0].drcs_map, kDRCSCode);
        CHECK_CHECK(drcs != nullptr);
        uint8_t* pixels = nullptr;
        size_t pixel_size = 0;
        aribcc_drcs_get_pixels(drcs, &pixels, &pixel_size);
        CHECK_CHECK(pixel_size == bitmap_v1.size());
        CHECK_CHECK(std::memcmp(pixels, bitmap_v1.data(), pixel_size) == 0);
        CHECK_CHECK(drcs_result.captions[0].regions[0].chars[0].type == ARIBCC_CHARTYPE_DRCS);
        aribcc_decoder_decode_sequence_cleanup(&drcs_result);
        CHECK_CHECK(drcs_result.captions == nullptr);

        // A NO_CAPTION / ERROR outcome yields an empty, cleanup-safe result.
        Bytes no_caption_pes = MakeStatementPES(MakeStatementUnit({LockingShift1(), Fullwidth('A')}),
                                                /*dgi_id=*/2);
        aribcc_decode_sequence_result_t empty_result;
        CHECK_CHECK(aribcc_decoder_decode_sequence(dec, no_caption_pes.data(),
                                                   no_caption_pes.size(), 1000, &empty_result) ==
                    ARIBCC_DECODE_STATUS_NO_CAPTION);
        CHECK_CHECK(empty_result.captions == nullptr);
        CHECK_CHECK(empty_result.caption_count == 0);
        aribcc_decoder_decode_sequence_cleanup(&empty_result);

        aribcc_decoder_free(dec);
        aribcc_context_free(ctx);
    }
}
