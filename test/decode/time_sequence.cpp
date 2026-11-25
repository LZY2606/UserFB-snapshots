#include "test_support.h"

#include "aribcaption/caption.h"
#include "aribcaption/context.h"
#include "aribcaption/context.hpp"
#include "aribcaption/decoder.h"
#include "aribcaption/decoder.hpp"

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace aribcaption;

namespace {

using Bytes = std::vector<uint8_t>;

Bytes MakeDataUnit(uint8_t parameter, const Bytes& body) {
    CHECK_CHECK(body.size() <= 0xffffff);
    Bytes unit = {
        0x1f,
        parameter,
        0x00,
        0x00,
        static_cast<uint8_t>(body.size()),
    };
    unit.insert(unit.end(), body.begin(), body.end());
    return unit;
}

Bytes StatementUnit(const Bytes& body) {
    return MakeDataUnit(0x20, body);
}

Bytes DrcsUnit(uint16_t code, uint8_t first_pixel) {
    // 8x8, 2-depth DRCS. depth field 0 means depth = 2 and depth_bits = 1.
    Bytes body = {
        0x01,                    // number of codes
        static_cast<uint8_t>(code >> 8),
        static_cast<uint8_t>(code & 0xff),
        0x01,                    // number of fonts
        0x00,                    // font id 0, mode 0
        0x00,                    // depth - 2
        0x08,                    // width
        0x08,                    // height
        first_pixel, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42,
        static_cast<uint8_t>(first_pixel ^ 0xff),
    };
    return MakeDataUnit(0x31, body);
}

Bytes BuildPES(const std::vector<Bytes>& units, uint8_t data_group_id_byte = 0x04) {
    Bytes data_unit_loop;
    for (const Bytes& unit : units) {
        data_unit_loop.insert(data_unit_loop.end(), unit.begin(), unit.end());
    }

    CHECK_CHECK(data_unit_loop.size() <= 0xffffff);
    Bytes group_data = {
        0x00,  // TMD = 0
        0x00,
        0x00,
        static_cast<uint8_t>(data_unit_loop.size()),
    };
    // The 24-bit data-unit-loop length starts at group_data[1].
    group_data[1] = static_cast<uint8_t>((data_unit_loop.size() >> 16) & 0xff);
    group_data[2] = static_cast<uint8_t>((data_unit_loop.size() >> 8) & 0xff);
    group_data.insert(group_data.end(), data_unit_loop.begin(), data_unit_loop.end());

    CHECK_CHECK(group_data.size() <= 0xffff);
    Bytes pes = {
        0x80,  // data_identifier: caption
        0xff,  // private_stream_1
        0xf0,  // PES header length: 0
        data_group_id_byte,
        0x00,
        0x00,
        static_cast<uint8_t>((group_data.size() >> 8) & 0xff),
        static_cast<uint8_t>(group_data.size() & 0xff),
    };
    pes.insert(pes.end(), group_data.begin(), group_data.end());
    return pes;
}

// A at APS(2,2), wait 500ms, B in the next data unit, wait 300ms, then CS.
Bytes BuildPositionSequencePES() {
    Bytes unit1_body = {
        0x0e,              // LS1: alphanumeric G1
        0x1c, 0x42, 0x42,  // APS y=2, x=2
        0x41,              // A (mapped to full-width A)
        0x9d, 0x20, 0x05,   // TIME, wait 5 * 100ms
    };
    Bytes unit2_body = {
        0x42,              // B; active position and G1 survive the TIME split
        0x9d, 0x20, 0x03,   // TIME, wait 3 * 100ms
        0x0c,              // CS
    };
    return BuildPES({StatementUnit(unit1_body), StatementUnit(unit2_body)});
}

Bytes BuildDRCSSequencePES() {
    Bytes first_statement = {
        0x1c, 0x42, 0x42,              // APS(2,2)
        0xee, 0xb0, 0x80,              // U+EC00 in UTF-8
        0xc2, 0x9d, 0x20, 0x05,        // UTF-8 encoded C1 TIME, 500ms
    };
    Bytes second_statement = {
        0xee, 0xb0, 0x80,              // U+EC00, now using redefined DRCS
        0xc2, 0x9d, 0x20, 0x03,        // TIME, 300ms
        0x0c,                          // CS
    };

    return BuildPES({
        DrcsUnit(0xec00, 0x81),
        StatementUnit(first_statement),
        DrcsUnit(0xec00, 0x3c),
        StatementUnit(second_statement),
    });
}

void CheckPositionSequence(const DecodeSequenceResult& sequence) {
    CHECK_CHECK(sequence.captions.size() == 3);

    const Caption& a = sequence.captions[0];
    const Caption& b = sequence.captions[1];
    const Caption& clear = sequence.captions[2];

    CHECK_CHECK(a.pts == 1000);
    CHECK_CHECK(a.wait_duration == 500);
    CHECK_CHECK(a.flags == kCaptionFlagsWaitDuration);
    CHECK_CHECK(a.text == "\xef\xbc\xa1");  // U+FF21 full-width A
    CHECK_CHECK(a.regions.size() == 1);
    CHECK_CHECK(a.regions[0].chars.size() == 1);
    CHECK_CHECK(a.regions[0].x == 80);
    CHECK_CHECK(a.regions[0].y == 120);
    CHECK_CHECK(a.regions[0].width == 40);
    CHECK_CHECK(a.regions[0].height == 60);
    const CaptionChar& char_a = a.regions[0].chars[0];
    CHECK_CHECK(char_a.codepoint == 0xff21);
    CHECK_CHECK(char_a.x == 80);
    CHECK_CHECK(char_a.y == 120);
    CHECK_CHECK(char_a.char_width == 36);
    CHECK_CHECK(char_a.char_height == 36);
    CHECK_CHECK(std::strcmp(char_a.u8str, "\xef\xbc\xa1") == 0);

    CHECK_CHECK(b.pts == 1500);
    CHECK_CHECK(b.wait_duration == 300);
    CHECK_CHECK(b.flags == kCaptionFlagsWaitDuration);
    CHECK_CHECK(b.text == "\xef\xbc\xa2");  // U+FF22 full-width B
    CHECK_CHECK(b.regions.size() == 1);
    CHECK_CHECK(b.regions[0].chars.size() == 1);
    CHECK_CHECK(b.regions[0].x == 120);
    CHECK_CHECK(b.regions[0].y == 120);
    const CaptionChar& char_b = b.regions[0].chars[0];
    CHECK_CHECK(char_b.codepoint == 0xff22);
    CHECK_CHECK(char_b.x == 120);
    CHECK_CHECK(char_b.y == 120);

    CHECK_CHECK(clear.pts == 1800);
    CHECK_CHECK(clear.wait_duration == DURATION_INDEFINITE);
    CHECK_CHECK(clear.flags == kCaptionFlagsClearScreen);
    CHECK_CHECK(clear.text.empty());
    CHECK_CHECK(clear.regions.empty());

    for (size_t i = 1; i < sequence.captions.size(); ++i) {
        CHECK_CHECK(sequence.captions[i - 1].pts <= sequence.captions[i].pts);
    }
}

void TestTimingPositionsAndOldDecode() {
    Context context;
    Decoder decoder(context);
    CHECK_CHECK(decoder.Initialize());

    Bytes pes = BuildPositionSequencePES();
    DecodeSequenceResult sequence;
    DecodeStatus status = decoder.DecodeSequence(pes.data(), pes.size(), 1000, sequence);
    CHECK_CHECK(status == DecodeStatus::kGotCaption);
    CheckPositionSequence(sequence);

    // The legacy API remains one aggregated caption with accumulated waits.
    Decoder legacy_decoder(context);
    CHECK_CHECK(legacy_decoder.Initialize());
    DecodeResult legacy;
    status = legacy_decoder.Decode(pes.data(), pes.size(), 1000, legacy);
    CHECK_CHECK(status == DecodeStatus::kGotCaption);
    CHECK_CHECK(legacy.caption != nullptr);
    CHECK_CHECK(legacy.caption->pts == 1000);
    CHECK_CHECK(legacy.caption->wait_duration == 800);
    CHECK_CHECK(legacy.caption->flags ==
                static_cast<CaptionFlags>(kCaptionFlagsWaitDuration | kCaptionFlagsClearScreen));
    CHECK_CHECK(legacy.caption->text == "\xef\xbc\xa1\xef\xbc\xa2");
    CHECK_CHECK(legacy.caption->regions.size() == 1);
    CHECK_CHECK(legacy.caption->regions[0].chars.size() == 2);

    // Flush leaves the decoder usable and does not alter PES timing rules.
    decoder.Flush();
    DecodeSequenceResult after_flush;
    status = decoder.DecodeSequence(pes.data(), pes.size(), 1000, after_flush);
    CHECK_CHECK(status == DecodeStatus::kGotCaption);
    CheckPositionSequence(after_flush);

    // A boundary between two statement data units is not a new timeline or a
    // split when there is no positive TIME control.
    Bytes cross_unit_pes = BuildPES({
        StatementUnit(Bytes{0x0e, 0x41}),
        StatementUnit(Bytes{0x42}),
    });
    DecodeSequenceResult cross_unit;
    status = decoder.DecodeSequence(cross_unit_pes.data(),
                                    cross_unit_pes.size(),
                                    1000,
                                    cross_unit);
    CHECK_CHECK(status == DecodeStatus::kGotCaption);
    CHECK_CHECK(cross_unit.captions.size() == 1);
    CHECK_CHECK(cross_unit.captions[0].text == "\xef\xbc\xa1\xef\xbc\xa2");
    CHECK_CHECK(cross_unit.captions[0].regions.size() == 1);
    CHECK_CHECK(cross_unit.captions[0].regions[0].chars.size() == 2);
}

void TestDecodeStateSurvivesSplit() {
    Context context;
    Decoder decoder(context);
    CHECK_CHECK(decoder.Initialize());

    Bytes pes = BuildPES({
        StatementUnit(Bytes{
            0x0e,              // LS1 remains selected after splitting
            0x90, 0x49,        // COL: palette 0, text color entry 9
            0x89,              // MSZ: horizontal scale 0.5
            0x1c, 0x44, 0x44,  // APS(4,4)
            0x41,
            0x9d, 0x20, 0x05,
        }),
        StatementUnit(Bytes{0x42}),
    });

    DecodeSequenceResult sequence;
    CHECK_CHECK(decoder.DecodeSequence(pes.data(), pes.size(), 1000, sequence) ==
                DecodeStatus::kGotCaption);
    CHECK_CHECK(sequence.captions.size() == 2);
    CHECK_CHECK(sequence.captions[0].regions[0].chars.size() == 1);
    CHECK_CHECK(sequence.captions[1].regions[0].chars.size() == 1);

    const CaptionChar& a = sequence.captions[0].regions[0].chars[0];
    const CaptionChar& b = sequence.captions[1].regions[0].chars[0];
    CHECK_CHECK(a.codepoint == 0x0041);  // MSZ replacement yields half-width A
    CHECK_CHECK(b.codepoint == 0x0042);  // G1 and MSZ survived across the split
    CHECK_CHECK(a.char_horizontal_scale == 0.5f);
    CHECK_CHECK(a.char_vertical_scale == 1.0f);
    CHECK_CHECK(b.char_horizontal_scale == 0.5f);
    CHECK_CHECK(b.char_vertical_scale == 1.0f);
    CHECK_CHECK(a.text_color.u32 != 0);
    CHECK_CHECK(a.text_color.u32 == b.text_color.u32);
    CHECK_CHECK(a.x == 80);
    CHECK_CHECK(b.x == 100);  // A's half-width section advanced the active position by 20
    CHECK_CHECK(a.y == b.y);
}

void TestDRCSOwnershipAndRedefinition() {
    Context context;
    Decoder decoder(context);
    CHECK_CHECK(decoder.Initialize(EncodingScheme::kARIB_STD_B24_UTF8));

    Bytes pes = BuildDRCSSequencePES();
    DecodeSequenceResult sequence;
    DecodeStatus status = decoder.DecodeSequence(pes.data(), pes.size(), 1000, sequence);
    CHECK_CHECK(status == DecodeStatus::kGotCaption);
    CHECK_CHECK(sequence.captions.size() == 3);

    CHECK_CHECK(sequence.captions[0].regions.size() == 1);
    CHECK_CHECK(sequence.captions[0].regions[0].chars.size() == 1);
    const CaptionChar& first_glyph = sequence.captions[0].regions[0].chars[0];
    CHECK_CHECK(first_glyph.type == CaptionCharType::kDRCS);
    CHECK_CHECK(first_glyph.drcs_code == 0xec00);
    CHECK_CHECK(sequence.captions[0].drcs_map.count(0xec00) == 1);
    CHECK_CHECK(sequence.captions[0].drcs_map.at(0xec00).pixels.size() == 8);
    CHECK_CHECK(sequence.captions[0].drcs_map.at(0xec00).pixels[0] == 0x81);

    const CaptionChar& second_glyph = sequence.captions[1].regions[0].chars[0];
    CHECK_CHECK(second_glyph.type == CaptionCharType::kDRCS);
    CHECK_CHECK(second_glyph.drcs_code == 0xec00);
    CHECK_CHECK(sequence.captions[1].drcs_map.at(0xec00).pixels[0] == 0x3c);
    CHECK_CHECK(sequence.captions[2].flags == kCaptionFlagsClearScreen);

    // Redefine EC00 in a later decode. Already-returned fragments keep their
    // own copied DRCS bitmaps.
    Bytes redefine_pes = BuildPES({
        DrcsUnit(0xec00, 0x7e),
        StatementUnit(Bytes{0xee, 0xb0, 0x80}),
    });
    DecodeSequenceResult redefined;
    status = decoder.DecodeSequence(redefine_pes.data(), redefine_pes.size(), 0, redefined);
    CHECK_CHECK(status == DecodeStatus::kGotCaption);
    CHECK_CHECK(redefined.captions.at(0).drcs_map.at(0xec00).pixels[0] == 0x7e);
    CHECK_CHECK(sequence.captions[0].drcs_map.at(0xec00).pixels[0] == 0x81);
    CHECK_CHECK(sequence.captions[1].drcs_map.at(0xec00).pixels[0] == 0x3c);
}

void CheckCAndCppCaptionSame(const Caption& cpp, const aribcc_caption_t* c) {
    CHECK_CHECK(c != nullptr);
    CHECK_CHECK(static_cast<int>(c->type) == static_cast<int>(cpp.type));
    CHECK_CHECK(static_cast<int>(c->flags) == static_cast<int>(cpp.flags));
    CHECK_CHECK(c->iso6392_language_code == cpp.iso6392_language_code);
    CHECK_CHECK(c->pts == cpp.pts);
    CHECK_CHECK(c->wait_duration == cpp.wait_duration);
    CHECK_CHECK(c->plane_width == cpp.plane_width);
    CHECK_CHECK(c->plane_height == cpp.plane_height);
    CHECK_CHECK(c->has_builtin_sound == cpp.has_builtin_sound);
    CHECK_CHECK(c->builtin_sound_id == cpp.builtin_sound_id);
    CHECK_CHECK(std::string(c->text ? c->text : "") == cpp.text);
    CHECK_CHECK(c->region_count == cpp.regions.size());

    for (size_t i = 0; i < cpp.regions.size(); ++i) {
        const CaptionRegion& src_region = cpp.regions[i];
        const aribcc_caption_region_t* dst_region = &c->regions[i];
        CHECK_CHECK(dst_region->x == src_region.x);
        CHECK_CHECK(dst_region->y == src_region.y);
        CHECK_CHECK(dst_region->width == src_region.width);
        CHECK_CHECK(dst_region->height == src_region.height);
        CHECK_CHECK(dst_region->is_ruby == src_region.is_ruby);
        CHECK_CHECK(dst_region->char_count == src_region.chars.size());
        for (size_t j = 0; j < src_region.chars.size(); ++j) {
            const CaptionChar& src_char = src_region.chars[j];
            const aribcc_caption_char_t* dst_char = &dst_region->chars[j];
            CHECK_CHECK(static_cast<int>(dst_char->type) == static_cast<int>(src_char.type));
            CHECK_CHECK(dst_char->codepoint == src_char.codepoint);
            CHECK_CHECK(dst_char->pua_codepoint == src_char.pua_codepoint);
            CHECK_CHECK(dst_char->drcs_code == src_char.drcs_code);
            CHECK_CHECK(dst_char->x == src_char.x);
            CHECK_CHECK(dst_char->y == src_char.y);
            CHECK_CHECK(dst_char->char_width == src_char.char_width);
            CHECK_CHECK(dst_char->char_height == src_char.char_height);
            CHECK_CHECK(dst_char->text_color == src_char.text_color.u32);
            CHECK_CHECK(dst_char->back_color == src_char.back_color.u32);
            CHECK_CHECK(dst_char->stroke_color == src_char.stroke_color.u32);
            CHECK_CHECK(std::strcmp(dst_char->u8str, src_char.u8str) == 0);
        }
    }

    CHECK_CHECK(c->drcs_map != nullptr || cpp.drcs_map.empty());
    if (c->drcs_map != nullptr) {
        for (const auto& [code, drcs] : cpp.drcs_map) {
            aribcc_drcs_t* c_drcs = aribcc_drcsmap_get(c->drcs_map, code);
            CHECK_CHECK(c_drcs != nullptr);
            int width = 0, height = 0;
            aribcc_drcs_get_size(c_drcs, &width, &height);
            CHECK_CHECK(width == drcs.width);
            CHECK_CHECK(height == drcs.height);
            uint8_t* pixels = nullptr;
            size_t pixel_size = 0;
            aribcc_drcs_get_pixels(c_drcs, &pixels, &pixel_size);
            CHECK_CHECK(pixel_size == drcs.pixels.size());
            CHECK_CHECK(std::memcmp(pixels, drcs.pixels.data(), pixel_size) == 0);
        }
    }
}

void TestCAPIAndCleanup() {
    Bytes pes = BuildDRCSSequencePES();

    Context cpp_context;
    Decoder cpp_decoder(cpp_context);
    CHECK_CHECK(cpp_decoder.Initialize(EncodingScheme::kARIB_STD_B24_UTF8));
    DecodeSequenceResult cpp_sequence;
    CHECK_CHECK(cpp_decoder.DecodeSequence(pes.data(), pes.size(), 1000, cpp_sequence) ==
                DecodeStatus::kGotCaption);

    aribcc_context_t* context = aribcc_context_alloc();
    CHECK_CHECK(context != nullptr);
    aribcc_decoder_t* decoder = aribcc_decoder_alloc(context);
    CHECK_CHECK(decoder != nullptr);
    CHECK_CHECK(aribcc_decoder_initialize(
        decoder,
        ARIBCC_ENCODING_SCHEME_ARIB_STD_B24_UTF8,
        ARIBCC_CAPTIONTYPE_CAPTION,
        ARIBCC_PROFILE_A,
        ARIBCC_LANGUAGEID_FIRST));

    aribcc_decode_sequence_result_t sequence;
    std::memset(&sequence, 0x5a, sizeof(sequence));  // API must initialize it.
    aribcc_decode_status_t status =
        aribcc_decoder_decode_sequence(decoder, pes.data(), pes.size(), 1000, &sequence);
    CHECK_CHECK(status == ARIBCC_DECODE_STATUS_GOT_CAPTION);
    CHECK_CHECK(sequence.caption_count == 3);
    CHECK_CHECK(sequence.captions != nullptr);

    for (size_t i = 0; i < cpp_sequence.captions.size(); ++i) {
        CheckCAndCppCaptionSame(cpp_sequence.captions[i], &sequence.captions[i]);
    }

    CHECK_CHECK(sequence.captions[0].region_count == 1);
    CHECK_CHECK(sequence.captions[0].regions[0].char_count == 1);
    CHECK_CHECK(sequence.captions[0].drcs_map != nullptr);
    aribcc_drcs_t* first_drcs = aribcc_drcsmap_get(sequence.captions[0].drcs_map, 0xec00);
    CHECK_CHECK(first_drcs != nullptr);
    uint8_t* pixels = nullptr;
    size_t pixel_size = 0;
    aribcc_drcs_get_pixels(first_drcs, &pixels, &pixel_size);
    CHECK_CHECK(pixel_size == 8);
    CHECK_CHECK(pixels[0] == 0x81);

    // Result storage outlives the decoder/context; cleanup must release it all.
    aribcc_decoder_free(decoder);
    aribcc_context_free(context);
    aribcc_decode_sequence_result_cleanup(&sequence);
    CHECK_CHECK(sequence.captions == nullptr);
    CHECK_CHECK(sequence.caption_count == 0);
    aribcc_decode_sequence_result_cleanup(&sequence);  // repeated cleanup is safe
    aribcc_decode_sequence_result_cleanup(nullptr);
}

void TestUnknownPTSLanguageFilterAndNoEmptyFragments() {
    Context context;
    Decoder decoder(context);
    CHECK_CHECK(decoder.Initialize());

    Bytes pes = BuildPositionSequencePES();
    DecodeSequenceResult sequence;
    CHECK_CHECK(decoder.DecodeSequence(pes.data(), pes.size(), PTS_NOPTS, sequence) ==
                DecodeStatus::kGotCaption);
    CHECK_CHECK(sequence.captions.size() == 3);
    for (const Caption& caption : sequence.captions) {
        CHECK_CHECK(caption.pts == PTS_NOPTS);
    }

    // dgi_id = 2 selects the second language.
    Bytes second_language_pes = BuildPES({StatementUnit(Bytes{0x0e, 0x41})}, 0x08);
    DecodeSequenceResult filtered;
    CHECK_CHECK(decoder.DecodeSequence(second_language_pes.data(),
                                       second_language_pes.size(),
                                       1000,
                                       filtered) == DecodeStatus::kNoCaption);
    CHECK_CHECK(filtered.captions.empty());

    decoder.SwitchLanguage(LanguageId::kSecond);
    CHECK_CHECK(decoder.DecodeSequence(second_language_pes.data(),
                                       second_language_pes.size(),
                                       1000,
                                       filtered) == DecodeStatus::kGotCaption);
    CHECK_CHECK(filtered.captions.size() == 1);

    decoder.SwitchLanguage(LanguageId::kFirst);

    // Minimal caption-management packet; repeated group is the deduplicated
    // retransmission case and must remain kNoCaption.
    Bytes management_pes = {
        0x80, 0xff, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x0a,
        0x00, 0x01, 0x00, 'j', 'p', 'n', 0x80, 0x00, 0x00, 0x00,
    };
    DecodeSequenceResult management;
    CHECK_CHECK(decoder.DecodeSequence(management_pes.data(),
                                       management_pes.size(),
                                       1000,
                                       management) == DecodeStatus::kNoCaption);
    CHECK_CHECK(management.captions.empty());
    CHECK_CHECK(decoder.DecodeSequence(management_pes.data(),
                                       management_pes.size(),
                                       1000,
                                       management) == DecodeStatus::kNoCaption);

    // A wait with no pending display change advances time but creates no caption.
    Bytes wait_then_a = BuildPES({StatementUnit(Bytes{
        0x0e,
        0x9d, 0x20, 0x05,
        0x41,
    })});
    DecodeSequenceResult delayed_a;
    CHECK_CHECK(decoder.DecodeSequence(wait_then_a.data(), wait_then_a.size(), 1000, delayed_a) ==
                DecodeStatus::kGotCaption);
    CHECK_CHECK(delayed_a.captions.size() == 1);
    CHECK_CHECK(delayed_a.captions[0].pts == 1500);
    CHECK_CHECK(delayed_a.captions[0].wait_duration == DURATION_INDEFINITE);

    Bytes only_wait = BuildPES({StatementUnit(Bytes{0x9d, 0x20, 0x05})});
    DecodeSequenceResult empty;
    CHECK_CHECK(decoder.DecodeSequence(only_wait.data(), only_wait.size(), 1000, empty) ==
                DecodeStatus::kNoCaption);
    CHECK_CHECK(empty.captions.empty());

    // TIME 0 does not split.
    Bytes time_zero = BuildPES({StatementUnit(Bytes{0x0e, 0x41, 0x9d, 0x20, 0x00, 0x42})});
    DecodeSequenceResult zero_sequence;
    CHECK_CHECK(decoder.DecodeSequence(time_zero.data(), time_zero.size(), 1000, zero_sequence) ==
                DecodeStatus::kGotCaption);
    CHECK_CHECK(zero_sequence.captions.size() == 1);
    CHECK_CHECK(zero_sequence.captions[0].text == "\xef\xbc\xa1\xef\xbc\xa2");
    CHECK_CHECK(zero_sequence.captions[0].wait_duration == DURATION_INDEFINITE);
}

void TestOverflowAndMalformedUnits() {
    Context context;
    Decoder decoder(context);
    CHECK_CHECK(decoder.Initialize());

    Bytes pes = BuildPositionSequencePES();
    DecodeSequenceResult overflow;
    int64_t almost_max = std::numeric_limits<int64_t>::max() - 200;
    CHECK_CHECK(decoder.DecodeSequence(pes.data(), pes.size(), almost_max, overflow) ==
                DecodeStatus::kError);
    CHECK_CHECK(overflow.captions.empty());

    Bytes truncated_time = BuildPES({StatementUnit(Bytes{0x9d, 0x20})});
    DecodeSequenceResult truncated;
    CHECK_CHECK(decoder.DecodeSequence(truncated_time.data(),
                                       truncated_time.size(),
                                       1000,
                                       truncated) == DecodeStatus::kError);
    CHECK_CHECK(truncated.captions.empty());
    DecodeResult legacy_error;
    CHECK_CHECK(decoder.Decode(truncated_time.data(),
                               truncated_time.size(),
                               1000,
                               legacy_error) == DecodeStatus::kError);

    Bytes bad_unit_id = BuildPositionSequencePES();
    CHECK_CHECK(bad_unit_id.size() > 12);
    bad_unit_id[12] = 0x00;  // Required unit_separator is 0x1f.
    DecodeSequenceResult bad_unit;
    CHECK_CHECK(decoder.DecodeSequence(bad_unit_id.data(),
                                       bad_unit_id.size(),
                                       1000,
                                       bad_unit) == DecodeStatus::kError);
    CHECK_CHECK(bad_unit.captions.empty());

    DecodeSequenceResult truncated_pes;
    CHECK_CHECK(decoder.DecodeSequence(pes.data(), pes.size() - 1, 1000, truncated_pes) ==
                DecodeStatus::kError);
    CHECK_CHECK(truncated_pes.captions.empty());
}

}  // namespace

void run_time_sequence_tests() {
    TestTimingPositionsAndOldDecode();
    TestDecodeStateSurvivesSplit();
    TestDRCSOwnershipAndRedefinition();
    TestCAPIAndCleanup();
    TestUnknownPTSLanguageFilterAndNoEmptyFragments();
    TestOverflowAndMalformedUnits();
}
