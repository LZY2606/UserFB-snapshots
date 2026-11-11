/*
 * Copyright (C) 2021 magicxqq <xqq@xqq.im>. All rights reserved.
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

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include "aribcaption/caption.hpp"
#include "aribcaption/color.hpp"
#include "aribcaption/context.hpp"
#include "aribcaption/decoder.hpp"
#include "aribcaption/decoder.h"
#include "aribcaption/image.hpp"
#include "aribcaption/renderer.hpp"
#include "renderer/bitmap.hpp"
#include "renderer/drcs_renderer.hpp"
#include "drcs_data_unit.hpp"
#include "sample_data.h"

using namespace aribcaption;
using namespace drcs_test;

static int g_failures = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            g_failures++;                                                             \
        }                                                                             \
    } while (0)

#define CHECK_EQ_U32(actual, expected)                                                \
    do {                                                                              \
        uint32_t actual__ = static_cast<uint32_t>(actual);                            \
        uint32_t expected__ = static_cast<uint32_t>(expected);                        \
        if (actual__ != expected__) {                                                 \
            fprintf(stderr, "FAIL %s:%d: %s == 0x%x, expected 0x%x\n",                \
                    __FILE__, __LINE__, #actual, actual__, expected__);               \
            g_failures++;                                                             \
        }                                                                             \
    } while (0)

// ----------------------------------------------------------------------------
// Reference math for the expected pixels
// ----------------------------------------------------------------------------

static uint8_t GreyOf(uint32_t value, int depth) {
    uint32_t grey = static_cast<uint32_t>(255) * value / static_cast<uint32_t>(depth - 1);
    return static_cast<uint8_t>(grey > 255 ? 255 : grey);
}

static uint8_t TextAlphaOf(uint8_t grey, uint8_t color_alpha) {
    uint32_t weight = grey + (grey >> 7);
    return static_cast<uint8_t>((weight * color_alpha) >> 8);
}

// The intermediate pixel DRCSToColoredBitmap puts into the text bitmap.
static ColorRGBA TextPixel(uint32_t value, int depth, ColorRGBA color) {
    uint8_t grey = GreyOf(value, depth);
    if (grey == 0) {
        return ColorRGBA(0);
    }
    return ColorRGBA(color, TextAlphaOf(grey, color.a));
}

// Canvas blends a straight foreground over a premultiplied background buffer.
static ColorRGBA BlendOverPremultiplied(ColorRGBA bg, ColorRGBA fg) {
    uint32_t fg_weight = fg.a + (fg.a >> 7);
    uint32_t bg_weight = 256 - fg_weight;

    ColorRGBA out;
    out.r = static_cast<uint8_t>((static_cast<uint32_t>(fg.r) * fg_weight +
                                  static_cast<uint32_t>(bg.r) * bg_weight) >> 8);
    out.g = static_cast<uint8_t>((static_cast<uint32_t>(fg.g) * fg_weight +
                                  static_cast<uint32_t>(bg.g) * bg_weight) >> 8);
    out.b = static_cast<uint8_t>((static_cast<uint32_t>(fg.b) * fg_weight +
                                  static_cast<uint32_t>(bg.b) * bg_weight) >> 8);
    out.a = static_cast<uint8_t>((static_cast<uint32_t>(255) * fg_weight +
                                  static_cast<uint32_t>(bg.a) * bg_weight) >> 8);
    return out;
}

static ColorRGBA ExpectedPixel(uint32_t value, int depth, ColorRGBA color, ColorRGBA bg) {
    ColorRGBA text_pixel = TextPixel(value, depth, color);
    return BlendOverPremultiplied(bg, text_pixel);
}

// A rendered (straight RGBA) image pixel after one transparent-region draw.
static ColorRGBA RenderedPixel(uint32_t value, int depth, ColorRGBA color) {
    ColorRGBA blended = ExpectedPixel(value, depth, color, ColorRGBA(0));
    if (blended.a == 0) {
        return ColorRGBA(0);
    }
    return ColorRGBA(color.r, color.g, color.b, blended.a);
}

static bool PixelsEqual(ColorRGBA a, ColorRGBA b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

static void CheckPixel(const Bitmap& bitmap, int x, int y, ColorRGBA expected,
                       const char* what, int line) {
    ColorRGBA actual = *bitmap.GetPixelAt(x, y);
    if (!PixelsEqual(actual, expected)) {
        fprintf(stderr,
                "FAIL %s:%d: %s at (%d,%d) = (%u,%u,%u,%u), expected (%u,%u,%u,%u)\n",
                __FILE__, line, what, x, y, actual.r, actual.g, actual.b, actual.a,
                expected.r, expected.g, expected.b, expected.a);
        g_failures++;
    }
}

#define CHECK_PIXEL(bitmap, x, y, expected) CheckPixel(bitmap, x, y, expected, #bitmap, __LINE__)

static ColorRGBA ImagePixel(const Image& image, int x, int y) {
    const uint8_t* p = image.bitmap.data() + static_cast<size_t>(y) * image.stride + x * 4;
    return ColorRGBA(p[0], p[1], p[2], p[3]);
}

static void CheckImagePixel(const Image& image, int x, int y, ColorRGBA expected, int line) {
    ColorRGBA actual = ImagePixel(image, x, y);
    if (!PixelsEqual(actual, expected)) {
        fprintf(stderr,
                "FAIL %s:%d: image pixel at (%d,%d) = (%u,%u,%u,%u), expected (%u,%u,%u,%u)\n",
                __FILE__, line, x, y, actual.r, actual.g, actual.b, actual.a,
                expected.r, expected.g, expected.b, expected.a);
        g_failures++;
    }
}

#define CHECK_IMAGE_PIXEL(image, x, y, expected) CheckImagePixel(image, x, y, expected, __LINE__)

static void CheckBytes(const std::vector<uint8_t>& actual, const std::vector<uint8_t>& expected,
                       const char* what, int line) {
    if (actual != expected) {
        fprintf(stderr, "FAIL %s:%d: %s bytes differ (size %zu vs %zu)\n",
                __FILE__, line, what, actual.size(), expected.size());
        g_failures++;
    }
}

#define CHECK_BYTES(actual, expected) CheckBytes(actual, expected, #actual, __LINE__)

// ----------------------------------------------------------------------------
// Data unit helpers
// ----------------------------------------------------------------------------

static constexpr uint16_t kDRCS1CharacterCode = 0x0121;  // DRCS-1, character 0x21
static constexpr uint32_t kDRCS1CaptionCode = 0x10021;   // (map_index << 16) | 0x21

static std::vector<uint8_t> MakeDefinitionPES(uint16_t character_code,
                                              int width, int height, int depth,
                                              const std::vector<uint32_t>& samples,
                                              uint8_t statement_char = 0x21) {
    CodeRecord code;
    code.character_code = character_code;
    code.fonts.push_back(MakeValidFont(width, height, depth, samples));

    return MakePES({MakeDataUnit(0x30, MakeDRCSBody({code})),
                    MakeDataUnit(0x20, MakeDRCSStatement(statement_char, 0, 0))});
}

static std::vector<uint8_t> MakeReferencePES(uint8_t statement_char = 0x21) {
    return MakePES({MakeDataUnit(0x20, MakeDRCSStatement(statement_char, 0, 0))});
}

// The DRCS width field is one byte, so a 256-depth sample ramp needs two rows.
static void MakeDepthGeometry(int depth, int* width, int* height,
                              std::vector<uint32_t>* samples) {
    *height = depth <= 255 ? 1 : 2;
    *width = (depth + *height - 1) / *height;
    samples->clear();
    for (int v = 0; v < depth; v++) {
        samples->push_back(static_cast<uint32_t>(v));
    }
}

static std::vector<uint8_t> MakeRawFontPES(const FontRecord& font, uint16_t character_code) {
    CodeRecord code;
    code.character_code = character_code;
    code.fonts.push_back(font);
    return MakePES({MakeDataUnit(0x30, MakeDRCSBody({code})),
                    MakeDataUnit(0x20, MakeDRCSStatement(static_cast<uint8_t>(character_code & 0x7F), 0, 0))});
}

// ----------------------------------------------------------------------------
// Direct DrawDRCS tests
// ----------------------------------------------------------------------------

static void TestBaselineInput() {
    // The input from verification/baseline.cc: 3x1, depth 3, samples 0,1,2
    // packed MSB-first as 0b00011000.
    DRCS drcs;
    drcs.width = 3;
    drcs.height = 1;
    drcs.depth = 3;
    drcs.depth_bits = 2;
    drcs.pixels = {0x18};

    Bitmap bitmap(3, 1, PixelFormat::kRGBA8888);
    DRCSRenderer renderer;
    CHECK(renderer.DrawDRCS(drcs, kCharStyleDefault, ColorRGBA(255, 255, 255, 255),
                            ColorRGBA(0), 0, 3, 1, bitmap, 0, 0));

    CHECK_PIXEL(bitmap, 0, 0, ExpectedPixel(0, 3, ColorRGBA(255, 255, 255, 255), ColorRGBA(0)));
    CHECK_PIXEL(bitmap, 1, 0, ExpectedPixel(1, 3, ColorRGBA(255, 255, 255, 255), ColorRGBA(0)));
    CHECK_PIXEL(bitmap, 2, 0, ExpectedPixel(2, 3, ColorRGBA(255, 255, 255, 255), ColorRGBA(0)));

    // Concrete alphas for the witness: 0, 125, 255. The middle sample must not disappear.
    CHECK_EQ_U32(bitmap.GetPixelAt(0, 0)->a, 0);
    CHECK_EQ_U32(bitmap.GetPixelAt(1, 0)->a, 125);
    CHECK_EQ_U32(bitmap.GetPixelAt(2, 0)->a, 255);
}

static void TestAllDepthsDirect() {
    const int depths[] = {2, 3, 4, 5, 17, 256};

    for (int depth : depths) {
        int bits = DepthBitsOf(depth);
        std::vector<uint32_t> samples;
        for (int v = 0; v < depth; v++) {
            samples.push_back(static_cast<uint32_t>(v));
        }

        DRCS drcs;
        drcs.width = depth;
        drcs.height = 1;
        drcs.depth = depth;
        drcs.depth_bits = bits;
        drcs.pixels = PackSamples(samples, bits);
        CHECK_EQ_U32(drcs.pixels.size(), (static_cast<size_t>(depth) * bits + 7) / 8);

        Bitmap bitmap(depth, 1, PixelFormat::kRGBA8888);
        DRCSRenderer renderer;
        CHECK(renderer.DrawDRCS(drcs, kCharStyleDefault, ColorRGBA(255, 255, 255, 255),
                                ColorRGBA(0), 0, depth, 1, bitmap, 0, 0));

        for (int v = 0; v < depth; v++) {
            CHECK_PIXEL(bitmap, v, 0,
                        ExpectedPixel(static_cast<uint32_t>(v), depth,
                                      ColorRGBA(255, 255, 255, 255), ColorRGBA(0)));
        }
    }
}

static void TestCrossByteAndMultipleRows() {
    // depth 5 -> 3 bits per sample, 3x3 = 9 samples = 27 bits.
    // Rows are contiguous, so row boundaries fall inside bytes and the last
    // byte is shared between row 2 and padding bits.
    const int depth = 5;
    const int bits = DepthBitsOf(depth);
    std::vector<uint32_t> samples = {
        0, 1, 2,
        3, 4, 0,
        1, 2, 3,
    };
    CHECK_EQ_U32(PackSamples(samples, bits).size(), (9u * bits + 7) / 8);

    DRCS drcs;
    drcs.width = 3;
    drcs.height = 3;
    drcs.depth = depth;
    drcs.depth_bits = bits;
    drcs.pixels = PackSamples(samples, bits);

    Bitmap bitmap(3, 3, PixelFormat::kRGBA8888);
    DRCSRenderer renderer;
    CHECK(renderer.DrawDRCS(drcs, kCharStyleDefault, ColorRGBA(200, 180, 160, 255),
                            ColorRGBA(0), 0, 3, 3, bitmap, 0, 0));

    for (int y = 0; y < 3; y++) {
        for (int x = 0; x < 3; x++) {
            CHECK_PIXEL(bitmap, x, y,
                        ExpectedPixel(samples[y * 3 + x], depth,
                                      ColorRGBA(200, 180, 160, 255), ColorRGBA(0)));
        }
    }
}

static void TestNearestNeighborScaling() {
    const int depth = 3;
    const int bits = DepthBitsOf(depth);
    std::vector<uint32_t> samples = {0, 1, 2, 0, 1, 2};

    DRCS drcs;
    drcs.width = 3;
    drcs.height = 2;
    drcs.depth = depth;
    drcs.depth_bits = bits;
    drcs.pixels = PackSamples(samples, bits);

    // Different horizontal and vertical fractions, nearest neighbor by target coordinate.
    const int targets[][2] = {{7, 5}, {5, 7}, {11, 3}};
    DRCSRenderer renderer;

    for (const auto& target : targets) {
        const int target_width = target[0];
        const int target_height = target[1];
        Bitmap bitmap(target_width, target_height, PixelFormat::kRGBA8888);
        CHECK(renderer.DrawDRCS(drcs, kCharStyleDefault, ColorRGBA(255, 255, 255, 255),
                                ColorRGBA(0), 0, target_width, target_height, bitmap, 0, 0));

        float x_fraction = static_cast<float>(drcs.width) / static_cast<float>(target_width);
        float y_fraction = static_cast<float>(drcs.height) / static_cast<float>(target_height);

        for (int y = 0; y < target_height; y++) {
            int source_y = static_cast<int>(y_fraction * static_cast<float>(y));
            for (int x = 0; x < target_width; x++) {
                int source_x = static_cast<int>(x_fraction * static_cast<float>(x));
                CHECK_PIXEL(bitmap, x, y,
                            ExpectedPixel(samples[source_y * drcs.width + source_x], depth,
                                          ColorRGBA(255, 255, 255, 255), ColorRGBA(0)));
            }
        }
    }
}

static void TestNonZeroTargetPosition() {
    const int depth = 3;
    std::vector<uint32_t> samples = {0, 1, 2};

    DRCS drcs;
    drcs.width = 3;
    drcs.height = 1;
    drcs.depth = depth;
    drcs.depth_bits = DepthBitsOf(depth);
    drcs.pixels = PackSamples(samples, DepthBitsOf(depth));

    Bitmap bitmap(9, 7, PixelFormat::kRGBA8888);
    DRCSRenderer renderer;
    CHECK(renderer.DrawDRCS(drcs, kCharStyleDefault, ColorRGBA(255, 255, 255, 255),
                            ColorRGBA(0), 0, 3, 1, bitmap, 1, 2));

    for (int y = 0; y < 7; y++) {
        for (int x = 0; x < 9; x++) {
            bool inside = (y == 2 && x >= 1 && x <= 3);
            if (inside) {
                CHECK_PIXEL(bitmap, x, y,
                            ExpectedPixel(samples[x - 1], depth,
                                          ColorRGBA(255, 255, 255, 255), ColorRGBA(0)));
            } else {
                CHECK_PIXEL(bitmap, x, y, ColorRGBA(0));
            }
        }
    }
}

static void TestStroke() {
    // A full-depth 3x1 glyph drawn in the middle of a 5x3 target with a
    // one-pixel stroke. Every stroke / glyph pixel is opaque, so the result
    // is exactly computable from the drawing order and clipping.
    const int depth = 3;
    std::vector<uint32_t> samples = {2, 2, 2};

    DRCS drcs;
    drcs.width = 3;
    drcs.height = 1;
    drcs.depth = depth;
    drcs.depth_bits = DepthBitsOf(depth);
    drcs.pixels = PackSamples(samples, DepthBitsOf(depth));

    Bitmap bitmap(5, 3, PixelFormat::kRGBA8888);
    DRCSRenderer renderer;
    ColorRGBA text(255, 255, 255, 255);
    ColorRGBA stroke(0, 0, 255, 255);
    CHECK(renderer.DrawDRCS(drcs, static_cast<CharStyle>(kCharStyleStroke), text, stroke,
                            1, 3, 1, bitmap, 1, 1));

    // Row 0: stroke up covers x = 1..3.
    CHECK_PIXEL(bitmap, 0, 0, ColorRGBA(0));
    CHECK_PIXEL(bitmap, 1, 0, stroke);
    CHECK_PIXEL(bitmap, 2, 0, stroke);
    CHECK_PIXEL(bitmap, 3, 0, stroke);
    CHECK_PIXEL(bitmap, 4, 0, ColorRGBA(0));

    // Row 1: left/right strokes are then covered by the glyph in the middle.
    CHECK_PIXEL(bitmap, 0, 1, stroke);
    CHECK_PIXEL(bitmap, 1, 1, text);
    CHECK_PIXEL(bitmap, 2, 1, text);
    CHECK_PIXEL(bitmap, 3, 1, text);
    CHECK_PIXEL(bitmap, 4, 1, stroke);

    // Row 2: stroke down covers x = 1..3.
    CHECK_PIXEL(bitmap, 0, 2, ColorRGBA(0));
    CHECK_PIXEL(bitmap, 1, 2, stroke);
    CHECK_PIXEL(bitmap, 2, 2, stroke);
    CHECK_PIXEL(bitmap, 3, 2, stroke);
    CHECK_PIXEL(bitmap, 4, 2, ColorRGBA(0));
}

static void TestSemiTransparentBackground() {
    const int depth = 3;
    std::vector<uint32_t> samples = {0, 1, 2};
    ColorRGBA color(200, 100, 50, 180);
    ColorRGBA background(40, 20, 10, 128);  // a premultiplied intermediate pixel

    DRCS drcs;
    drcs.width = 3;
    drcs.height = 1;
    drcs.depth = depth;
    drcs.depth_bits = DepthBitsOf(depth);
    drcs.pixels = PackSamples(samples, DepthBitsOf(depth));

    Bitmap bitmap(3, 1, PixelFormat::kRGBA8888);
    for (int x = 0; x < 3; x++) {
        *bitmap.GetPixelAt(x, 0) = background;
    }

    DRCSRenderer renderer;
    CHECK(renderer.DrawDRCS(drcs, kCharStyleDefault, color, ColorRGBA(0), 0, 3, 1, bitmap, 0, 0));

    for (int v = 0; v < 3; v++) {
        CHECK_PIXEL(bitmap, v, 0, ExpectedPixel(static_cast<uint32_t>(v), depth, color, background));
    }
}

static void TestInvalidDefinitionsLeaveTargetUntouched() {
    ColorRGBA sentinel(1, 2, 3, 4);
    DRCSRenderer renderer;

    auto expect_rejected = [&](const DRCS& drcs, const char* what, int line) {
        Bitmap bitmap(4, 2, PixelFormat::kRGBA8888);
        for (int y = 0; y < bitmap.height(); y++) {
            for (int x = 0; x < bitmap.width(); x++) {
                *bitmap.GetPixelAt(x, y) = sentinel;
            }
        }

        // Request a stroke as well: a rejected definition must not leave any
        // stroke pixels behind either.
        bool drawn = renderer.DrawDRCS(drcs, static_cast<CharStyle>(kCharStyleStroke),
                                       ColorRGBA(255, 255, 255, 255), ColorRGBA(0, 0, 255, 255),
                                       2, 3, 1, bitmap, 0, 0);
        if (drawn) {
            fprintf(stderr, "FAIL %s:%d: %s was drawn but should be rejected\n",
                    __FILE__, line, what);
            g_failures++;
        }

        for (int y = 0; y < bitmap.height(); y++) {
            for (int x = 0; x < bitmap.width(); x++) {
                CheckPixel(bitmap, x, y, sentinel, "untouched target", line);
            }
        }
    };

#define EXPECT_REJECTED(drcs, what) expect_rejected(drcs, what, __LINE__)

    {
        DRCS drcs;
        drcs.width = 0;
        drcs.height = 1;
        drcs.depth = 3;
        drcs.depth_bits = 2;
        drcs.pixels = {0x18};
        EXPECT_REJECTED(drcs, "zero width");
    }
    {
        DRCS drcs;
        drcs.width = 3;
        drcs.height = 0;
        drcs.depth = 3;
        drcs.depth_bits = 2;
        drcs.pixels = {0x18};
        EXPECT_REJECTED(drcs, "zero height");
    }
    {
        DRCS drcs;
        drcs.width = 3;
        drcs.height = 1;
        drcs.depth = 3;
        drcs.depth_bits = 1;  // should be 2
        drcs.pixels = {0x18};
        EXPECT_REJECTED(drcs, "depth_bits mismatch");
    }
    {
        DRCS drcs;
        drcs.width = 8;
        drcs.height = 1;
        drcs.depth = 3;
        drcs.depth_bits = 2;
        drcs.pixels = {0x00};  // needs 2 bytes
        EXPECT_REJECTED(drcs, "short pixel buffer");
    }
    {
        DRCS drcs;
        drcs.width = 3;
        drcs.height = 1;
        drcs.depth = 3;
        drcs.depth_bits = 2;
        drcs.pixels = {0x1C};  // samples 0,1,3; 3 is out of range
        EXPECT_REJECTED(drcs, "out-of-range sample");
    }
    {
        DRCS drcs;
        drcs.width = 2;
        drcs.height = 1;
        drcs.depth = 257;  // must be rejected, never truncated to 0
        drcs.depth_bits = DepthBitsOf(257);
        drcs.pixels = {0x00};
        EXPECT_REJECTED(drcs, "depth 257");
    }
    {
        DRCS drcs;
        drcs.width = 3;
        drcs.height = 1;
        drcs.depth = 1;
        drcs.depth_bits = 0;
        drcs.pixels = {};
        EXPECT_REJECTED(drcs, "depth 1");
    }
    {
        DRCS drcs;
        drcs.width = 3;
        drcs.height = 1;
        drcs.depth = 3;
        drcs.depth_bits = 2;
        drcs.pixels = {};
        EXPECT_REJECTED(drcs, "empty pixel buffer");
    }
#undef EXPECT_REJECTED
}

static void TestLegacyTwoAndFourBitOutputs() {
    // Two-bit glyph: samples 0,1,1,0,1,0,1,1 -> 0b01101011.
    {
        DRCS drcs;
        drcs.width = 8;
        drcs.height = 1;
        drcs.depth = 2;
        drcs.depth_bits = 1;
        drcs.pixels = {0x6B};

        Bitmap bitmap(8, 1, PixelFormat::kRGBA8888);
        DRCSRenderer renderer;
        CHECK(renderer.DrawDRCS(drcs, kCharStyleDefault, ColorRGBA(255, 255, 255, 255),
                                ColorRGBA(0), 0, 8, 1, bitmap, 0, 0));

        uint32_t values[] = {0, 1, 1, 0, 1, 0, 1, 1};
        for (int x = 0; x < 8; x++) {
            CHECK_PIXEL(bitmap, x, 0,
                        ExpectedPixel(values[x], 2, ColorRGBA(255, 255, 255, 255), ColorRGBA(0)));
        }
    }

    // Four-bit glyph: samples 0,1,2,3 -> bytes 0x01, 0x23.
    {
        DRCS drcs;
        drcs.width = 4;
        drcs.height = 1;
        drcs.depth = 4;
        drcs.depth_bits = 2;
        drcs.pixels = {0x1B};

        Bitmap bitmap(4, 1, PixelFormat::kRGBA8888);
        DRCSRenderer renderer;
        CHECK(renderer.DrawDRCS(drcs, kCharStyleDefault, ColorRGBA(255, 255, 255, 255),
                                ColorRGBA(0), 0, 4, 1, bitmap, 0, 0));

        for (int x = 0; x < 4; x++) {
            CHECK_PIXEL(bitmap, x, 0,
                        ExpectedPixel(static_cast<uint32_t>(x), 4,
                                      ColorRGBA(255, 255, 255, 255), ColorRGBA(0)));
        }
        // Legacy exact alphas (no rounding change): 0, 83, 170, 255.
        CHECK_EQ_U32(bitmap.GetPixelAt(0, 0)->a, 0);
        CHECK_EQ_U32(bitmap.GetPixelAt(1, 0)->a, 83);
        CHECK_EQ_U32(bitmap.GetPixelAt(2, 0)->a, 170);
        CHECK_EQ_U32(bitmap.GetPixelAt(3, 0)->a, 255);
    }
}

// ----------------------------------------------------------------------------
// Decoder tests (C++ and C entry points)
// ----------------------------------------------------------------------------

static void TestSampleDataCompatibility() {
    Context context;
    context.SetLogcatCallback([](LogLevel, const char*) {});

    Decoder decoder(context);
    CHECK(decoder.Initialize());

    DecodeResult result;
    DecodeStatus status = decoder.Decode(sample_data_drcs_1, sizeof(sample_data_drcs_1), 0, result);
    CHECK(status == DecodeStatus::kGotCaption);
    if (status != DecodeStatus::kGotCaption) {
        return;
    }

    CHECK_EQ_U32(result.caption->regions.size(), 3);
    CHECK_EQ_U32(result.caption->plane_width, 960);
    CHECK_EQ_U32(result.caption->plane_height, 540);
    CHECK(result.caption->text == u8"｟バサバサッ！｠(白鬼院凜々蝶)｟あっ…｠");
    CHECK_EQ_U32(result.caption->drcs_map.size(), 3);

    struct ExpectedEntry {
        uint32_t code;
        int width;
        int height;
        int depth;
        int depth_bits;
        size_t pixels;
        const char* md5;
        const char* alternative;
    };
    ExpectedEntry expected[] = {
        {0x10021, 18, 36, 4, 2, 162, "5063561406195ca45f5992e3f7ad77d2", u8"｟"},
        {0x10022, 18, 36, 4, 2, 162, "d84fc83615b75802ed422eda4ba39465", u8"｠"},
        {0x10023, 36, 36, 4, 2, 324, "f022cfe594d6f6930d7a5b994e1a0b71", u8"凜"},
    };

    for (const ExpectedEntry& entry : expected) {
        auto iter = result.caption->drcs_map.find(entry.code);
        CHECK(iter != result.caption->drcs_map.end());
        if (iter == result.caption->drcs_map.end()) {
            continue;
        }
        const DRCS& drcs = iter->second;
        CHECK_EQ_U32(drcs.width, entry.width);
        CHECK_EQ_U32(drcs.height, entry.height);
        CHECK_EQ_U32(drcs.depth, entry.depth);
        CHECK_EQ_U32(drcs.depth_bits, entry.depth_bits);
        CHECK_EQ_U32(drcs.pixels.size(), entry.pixels);
        CHECK(drcs.md5 == entry.md5);
        CHECK(drcs.alternative_text == entry.alternative);
    }
}

static void TestDecoderAllDepths() {
    const int depths[] = {2, 3, 4, 5, 17, 256};

    // C++ Decoder entry point.
    {
        Context context;
        context.SetLogcatCallback([](LogLevel, const char*) {});
        Decoder decoder(context);
        CHECK(decoder.Initialize());

        for (int depth : depths) {
            int width = 0, height = 0;
            std::vector<uint32_t> samples;
            MakeDepthGeometry(depth, &width, &height, &samples);

            std::vector<uint8_t> pes =
                MakeDefinitionPES(kDRCS1CharacterCode, width, height, depth, samples);
            DecodeResult result;
            DecodeStatus status = decoder.Decode(pes.data(), pes.size(), 0, result);
            CHECK(status == DecodeStatus::kGotCaption);
            if (status != DecodeStatus::kGotCaption) {
                continue;
            }

            auto iter = result.caption->drcs_map.find(kDRCS1CaptionCode);
            CHECK(iter != result.caption->drcs_map.end());
            if (iter == result.caption->drcs_map.end()) {
                continue;
            }
            const DRCS& drcs = iter->second;
            CHECK_EQ_U32(drcs.width, width);
            CHECK_EQ_U32(drcs.height, height);
            CHECK_EQ_U32(drcs.depth, depth);
            CHECK_EQ_U32(drcs.depth_bits, DepthBitsOf(depth));
            CHECK_BYTES(drcs.pixels, PackSamples(samples, DepthBitsOf(depth)));
        }
    }

    // C decoder entry point.
    {
        aribcc_context_t* context = aribcc_context_alloc();
        aribcc_decoder_t* decoder = aribcc_decoder_alloc(context);
        CHECK(aribcc_decoder_initialize(decoder,
                                        ARIBCC_ENCODING_SCHEME_ARIB_STD_B24_JIS,
                                        ARIBCC_CAPTIONTYPE_CAPTION,
                                        ARIBCC_PROFILE_A,
                                        ARIBCC_LANGUAGEID_FIRST));

        for (int depth : depths) {
            int width = 0, height = 0;
            std::vector<uint32_t> samples;
            MakeDepthGeometry(depth, &width, &height, &samples);

            std::vector<uint8_t> pes =
                MakeDefinitionPES(kDRCS1CharacterCode, width, height, depth, samples);
            aribcc_caption_t caption = {};
            aribcc_decode_status_t status =
                aribcc_decoder_decode(decoder, pes.data(), pes.size(), 0, &caption);
            CHECK(status == ARIBCC_DECODE_STATUS_GOT_CAPTION);
            if (status != ARIBCC_DECODE_STATUS_GOT_CAPTION) {
                continue;
            }

            aribcc_drcs_t* drcs = aribcc_drcsmap_get(caption.drcs_map, kDRCS1CaptionCode);
            CHECK(drcs != nullptr);
            if (drcs) {
                int actual_width = 0, actual_height = 0, actual_depth = 0, depth_bits = 0;
                aribcc_drcs_get_size(drcs, &actual_width, &actual_height);
                aribcc_drcs_get_depth(drcs, &actual_depth, &depth_bits);
                CHECK_EQ_U32(actual_width, width);
                CHECK_EQ_U32(actual_height, height);
                CHECK_EQ_U32(actual_depth, depth);
                CHECK_EQ_U32(depth_bits, DepthBitsOf(depth));

                uint8_t* pixels = nullptr;
                size_t size = 0;
                aribcc_drcs_get_pixels(drcs, &pixels, &size);
                std::vector<uint8_t> actual(pixels, pixels + size);
                CHECK_BYTES(actual, PackSamples(samples, DepthBitsOf(depth)));
            }

            aribcc_caption_cleanup(&caption);
        }

        aribcc_decoder_free(decoder);
        aribcc_context_free(context);
    }
}

static void TestDecoderRedefinition() {
    Context context;
    context.SetLogcatCallback([](LogLevel, const char*) {});
    Decoder decoder(context);
    CHECK(decoder.Initialize());

    // Seed a valid definition.
    std::vector<uint32_t> original = {0, 1, 2};
    std::vector<uint8_t> valid_pes =
        MakeDefinitionPES(kDRCS1CharacterCode, 3, 1, 3, original);
    DecodeResult result;
    CHECK(decoder.Decode(valid_pes.data(), valid_pes.size(), 0, result) == DecodeStatus::kGotCaption);

    // Redefine the same code with a different but valid definition.
    std::vector<uint32_t> replacement = {4, 3, 2, 1, 0};
    std::vector<uint8_t> replacement_pes =
        MakeDefinitionPES(kDRCS1CharacterCode, 5, 1, 5, replacement);
    CHECK(decoder.Decode(replacement_pes.data(), replacement_pes.size(), 1000, result) ==
          DecodeStatus::kGotCaption);
    auto iter = result.caption->drcs_map.find(kDRCS1CaptionCode);
    CHECK(iter != result.caption->drcs_map.end());
    if (iter != result.caption->drcs_map.end()) {
        CHECK_EQ_U32(iter->second.depth, 5);
        CHECK_BYTES(iter->second.pixels, PackSamples(replacement, DepthBitsOf(5)));
    }

    // Restore a known original for the failure tests below.
    CHECK(decoder.Decode(valid_pes.data(), valid_pes.size(), 2000, result) == DecodeStatus::kGotCaption);
}

static void TestDecoderInvalidDefinitions() {
    Context context;
    context.SetLogcatCallback([](LogLevel, const char*) {});
    Decoder decoder(context);
    CHECK(decoder.Initialize());

    // Seed the decoder's DRCS map with a valid definition under 0x0121.
    std::vector<uint32_t> logical = {0, 1, 2};
    std::vector<uint8_t> valid_pes =
        MakeDefinitionPES(kDRCS1CharacterCode, 3, 1, 3, logical);
    DecodeResult result;
    CHECK(decoder.Decode(valid_pes.data(), valid_pes.size(), 0, result) == DecodeStatus::kGotCaption);

    std::vector<std::vector<uint8_t>> bad_pes;

    // Truncated pixel data: 8 samples at 2 bits need 2 bytes, only 1 present.
    {
        FontRecord font;
        font.mode = 1;
        font.depth_field = 1;  // depth 3
        font.width = 8;
        font.height = 1;
        font.pixels = {0x00};
        bad_pes.push_back(MakeRawFontPES(font, kDRCS1CharacterCode));
    }

    // Illegal depth code: 255 + 2 = 257, which must be rejected rather than
    // wrapping around in a narrow integer.
    {
        FontRecord font;
        font.mode = 1;
        font.depth_field = 255;
        font.width = 3;
        font.height = 1;
        font.pixels = {0x00};
        bad_pes.push_back(MakeRawFontPES(font, kDRCS1CharacterCode));
    }

    // Wrong metadata: zero width.
    {
        FontRecord font;
        font.mode = 1;
        font.depth_field = 1;  // depth 3
        font.width = 0;
        font.height = 1;
        font.pixels = {};
        bad_pes.push_back(MakeRawFontPES(font, kDRCS1CharacterCode));
    }

    // Effective sample value exceeds depth - 1: depth 3, sample 3.
    {
        FontRecord font = MakeValidFont(3, 1, 3, {0, 1, 3});
        bad_pes.push_back(MakeRawFontPES(font, kDRCS1CharacterCode));
    }

    for (const auto& pes : bad_pes) {
        DecodeResult bad_result;
        DecodeStatus bad_status = decoder.Decode(pes.data(), pes.size(), 3000, bad_result);
        CHECK(bad_status == DecodeStatus::kError);

        // The previous definition must still be usable.
        std::vector<uint8_t> reference = MakeReferencePES();
        DecodeResult reference_result;
        DecodeStatus reference_status =
            decoder.Decode(reference.data(), reference.size(), 4000, reference_result);
        CHECK(reference_status == DecodeStatus::kGotCaption);
        if (reference_status != DecodeStatus::kGotCaption) {
            continue;
        }

        auto iter = reference_result.caption->drcs_map.find(kDRCS1CaptionCode);
        CHECK(iter != reference_result.caption->drcs_map.end());
        if (iter == reference_result.caption->drcs_map.end()) {
            continue;
        }
        CHECK_EQ_U32(iter->second.width, 3);
        CHECK_EQ_U32(iter->second.depth, 3);
        CHECK_BYTES(iter->second.pixels, PackSamples(logical, DepthBitsOf(3)));

        // Restore a valid definition for the next iteration.
        DecodeResult restore_result;
        CHECK(decoder.Decode(valid_pes.data(), valid_pes.size(), 5000, restore_result) ==
              DecodeStatus::kGotCaption);
    }
}

// ----------------------------------------------------------------------------
// Renderer integration
// ----------------------------------------------------------------------------

static std::vector<uint8_t> MakeTwoRegionStatement(uint8_t drcs_char_code) {
    std::vector<uint8_t> body;
    // Clear screen, set writing format 7 (960x540, 36x36 chars, 4/24 spacing).
    const uint8_t prologue[] = {0x0C, 0x9B, 0x37, 0x20, 0x53};
    body.insert(body.end(), prologue, prologue + sizeof(prologue));
    // Designate DRCS-1 into G0.
    const uint8_t designate[] = {0x1B, 0x28, 0x20, 0x41};
    body.insert(body.end(), designate, designate + sizeof(designate));
    // APS(10, 5) then the DRCS character.
    const uint8_t aps1[] = {0x1C, 0x05, 0x0A};
    body.insert(body.end(), aps1, aps1 + sizeof(aps1));
    body.push_back(drcs_char_code);
    // APS(20, 5), switch GL to G1 (alphanumeric), draw a normal text character.
    const uint8_t aps2[] = {0x1C, 0x05, 0x14, 0x0E};
    body.insert(body.end(), aps2, aps2 + sizeof(aps2));
    body.push_back(0x41);  // 'A'
    return body;
}

static void TestRendererIntegration() {
    Context context;
    context.SetLogcatCallback([](LogLevel, const char*) {});

    Decoder decoder(context);
    CHECK(decoder.Initialize());

    std::vector<uint32_t> samples = {0, 1, 2};
    CodeRecord code;
    code.character_code = kDRCS1CharacterCode;
    code.fonts.push_back(MakeValidFont(3, 1, 3, samples));
    std::vector<uint8_t> pes =
        MakePES({MakeDataUnit(0x30, MakeDRCSBody({code})),
                 MakeDataUnit(0x20, MakeTwoRegionStatement(0x21))});

    DecodeResult decoded;
    DecodeStatus decode_status = decoder.Decode(pes.data(), pes.size(), 0, decoded);
    CHECK(decode_status == DecodeStatus::kGotCaption);
    if (decode_status != DecodeStatus::kGotCaption) {
        return;
    }

    CHECK_EQ_U32(decoded.caption->regions.size(), 2);
    if (decoded.caption->regions.size() != 2) {
        return;
    }
    CHECK(decoded.caption->regions[0].chars[0].type == CaptionCharType::kDRCS);
    CHECK(decoded.caption->regions[1].chars[0].type == CaptionCharType::kText);
    CHECK_EQ_U32(decoded.caption->regions[1].chars[0].codepoint, 0xFF21);  // fullwidth 'A'
    CHECK_EQ_U32(decoded.caption->drcs_map.size(), 1);

    const CaptionChar& drcs_char = decoded.caption->regions[0].chars[0];

    Renderer renderer(context);
    CHECK(renderer.Initialize());
    renderer.SetReplaceDRCS(false);  // Never substitute Unicode for this glyph
    renderer.SetForceNoBackground(true);
    CHECK(renderer.SetFrameSize(960, 540));
    CHECK(renderer.SetMargins(0, 0, 0, 0));

    CHECK(renderer.AppendCaption(*decoded.caption));

    RenderResult result;
    RenderStatus status = renderer.Render(0, result);
    CHECK(status == RenderStatus::kGotImage);
    CHECK_EQ_U32(result.images.size(), 2);

    // Region order is preserved, with non-zero region origins.
    CHECK_EQ_U32(result.images[0].dst_x, 400);
    CHECK_EQ_U32(result.images[0].dst_y, 300);
    CHECK_EQ_U32(result.images[1].dst_x, 800);
    CHECK_EQ_U32(result.images[1].dst_y, 300);

    // Compute the glyph's placement inside the first region image exactly as
    // RegionRenderer does (magnification is 1 here).
    int char_x = static_cast<int>(std::floor(
        static_cast<float>(drcs_char.x - decoded.caption->regions[0].x) +
        static_cast<float>(drcs_char.char_horizontal_spacing) * drcs_char.char_horizontal_scale / 2));
    int char_y = static_cast<int>(std::floor(
        static_cast<float>(drcs_char.y - decoded.caption->regions[0].y) +
        static_cast<float>(drcs_char.char_vertical_spacing) * drcs_char.char_vertical_scale / 2));
    int char_width = static_cast<int>(drcs_char.char_width * drcs_char.char_horizontal_scale);
    int char_height = static_cast<int>(drcs_char.char_height * drcs_char.char_vertical_scale);
    CHECK_EQ_U32(char_width, 36);
    CHECK_EQ_U32(char_height, 36);

    int expected_x[] = {6, 18, 30};  // target columns mapping to samples 0,1,2
    int expected_value[] = {0, 1, 2};
    for (size_t i = 0; i < 3; i++) {
        int x = char_x + expected_x[i];
        int y = char_y;
        ColorRGBA expected = RenderedPixel(static_cast<uint32_t>(expected_value[i]), 3,
                                           ColorRGBA(255, 255, 255, 255));
        CHECK_IMAGE_PIXEL(result.images[0], x, y, expected);
    }

    // Pixels outside the glyph stay transparent.
    CHECK_IMAGE_PIXEL(result.images[0], char_x - 1, char_y, ColorRGBA(0));
    CHECK_IMAGE_PIXEL(result.images[0], char_x + 36, char_y, ColorRGBA(0));

    // Caption cache: the same PTS returns the unchanged images.
    RenderResult cached;
    CHECK(renderer.Render(0, cached) == RenderStatus::kGotImageUnchanged);
    CHECK_EQ_U32(cached.images.size(), 2);
    CHECK(cached.images[0].bitmap == result.images[0].bitmap);

    // Merge switch: region images are merged into one, and the glyph pixels
    // are unchanged.
    renderer.SetMergeRegionImages(true);
    RenderResult merged;
    CHECK(renderer.Render(0, merged) == RenderStatus::kGotImage);
    CHECK_EQ_U32(merged.images.size(), 1);
    CHECK_EQ_U32(merged.images[0].dst_x, 400);
    CHECK_EQ_U32(merged.images[0].dst_y, 300);
    for (size_t i = 0; i < 3; i++) {
        int x = char_x + expected_x[i];
        int y = char_y;
        ColorRGBA expected = RenderedPixel(static_cast<uint32_t>(expected_value[i]), 3,
                                           ColorRGBA(255, 255, 255, 255));
        CHECK_IMAGE_PIXEL(merged.images[0], x, y, expected);
    }

    // Turning merge off invalidates the cached images and restores two regions.
    renderer.SetMergeRegionImages(false);
    RenderResult unmerged;
    CHECK(renderer.Render(0, unmerged) == RenderStatus::kGotImage);
    CHECK_EQ_U32(unmerged.images.size(), 2);

    // A later caption is rendered fresh, not reused from the previous PTS.
    Caption later = *decoded.caption;
    later.pts = 1000;
    CHECK(renderer.AppendCaption(later));
    RenderResult later_result;
    CHECK(renderer.Render(1000, later_result) == RenderStatus::kGotImage);
    CHECK_EQ_U32(later_result.images.size(), 2);
}

// Test that decoded depth glyphs go through a real Renderer for several depths.
static void TestRendererDecodedDepths() {
    const int depths[] = {3, 5, 17, 256};

    Context context;
    context.SetLogcatCallback([](LogLevel, const char*) {});
    Decoder decoder(context);
    CHECK(decoder.Initialize());

    Renderer renderer(context);
    CHECK(renderer.Initialize());
    renderer.SetReplaceDRCS(false);
    renderer.SetForceNoBackground(true);
    CHECK(renderer.SetFrameSize(960, 540));
    CHECK(renderer.SetMargins(0, 0, 0, 0));

    for (size_t i = 0; i < sizeof(depths) / sizeof(depths[0]); i++) {
        int depth = depths[i];
        int width = 0, height = 0;
        std::vector<uint32_t> samples;
        MakeDepthGeometry(depth, &width, &height, &samples);

        std::vector<uint8_t> pes =
            MakeDefinitionPES(kDRCS1CharacterCode, width, height, depth, samples);
        DecodeResult decoded;
        DecodeStatus status = decoder.Decode(pes.data(), pes.size(), static_cast<int64_t>(i) * 1000,
                                             decoded);
        CHECK(status == DecodeStatus::kGotCaption);
        if (status != DecodeStatus::kGotCaption) {
            continue;
        }

        int64_t pts = static_cast<int64_t>(i) * 1000;
        CHECK(renderer.AppendCaption(std::move(*decoded.caption)));
        RenderResult result;
        CHECK(renderer.Render(pts, result) == RenderStatus::kGotImage);
        CHECK_EQ_U32(result.images.size(), 1);
        if (result.images.size() != 1) {
            continue;
        }

        const Image& image = result.images[0];
        CHECK_EQ_U32(image.dst_x, 0);
        CHECK_EQ_U32(image.dst_y, 0);

        int char_x = 2;   // (char_horizontal_spacing == 4) / 2
        int char_y = 12;  // char_vertical_spacing == 24 / 2
        float x_fraction = static_cast<float>(width) / 36.0f;
        float y_fraction = static_cast<float>(height) / 36.0f;

        // Probe several target columns in source rows 0 and 1 (when present).
        int target_ys[] = {3, 20};
        for (int target_y : target_ys) {
            int source_y = static_cast<int>(y_fraction * static_cast<float>(target_y));
            if (source_y >= height) {
                continue;
            }
            for (int target_x = 0; target_x < 36; target_x += 5) {
                int source_x = static_cast<int>(x_fraction * static_cast<float>(target_x));
                uint32_t value = samples[static_cast<size_t>(source_y) * width + source_x];
                ColorRGBA expected = RenderedPixel(value, depth, ColorRGBA(255, 255, 255, 255));
                CHECK_IMAGE_PIXEL(image, char_x + target_x, char_y + target_y, expected);
            }
        }
    }
}

int main() {
    TestBaselineInput();
    TestAllDepthsDirect();
    TestCrossByteAndMultipleRows();
    TestNearestNeighborScaling();
    TestNonZeroTargetPosition();
    TestStroke();
    TestSemiTransparentBackground();
    TestInvalidDefinitionsLeaveTargetUntouched();
    TestLegacyTwoAndFourBitOutputs();
    TestSampleDataCompatibility();
    TestDecoderAllDepths();
    TestDecoderRedefinition();
    TestDecoderInvalidDefinitions();
    TestRendererIntegration();
    TestRendererDecodedDepths();

    if (g_failures != 0) {
        fprintf(stderr, "FAILED: %d check(s) failed\n", g_failures);
        return 1;
    }

    printf("PASS: DRCS grayscale depth tests\n");
    return 0;
}
