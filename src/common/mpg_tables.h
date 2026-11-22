#pragma once
#include <stdint.h>

/*
 * Header-only. Static ISO/IEC 11172-2 tables that aren't Huffman/VLC code
 * tables (those live in mpg_vlc_tables.h): the zig-zag coefficient scan
 * order (Figure 2-D), the default intra/non-intra quantizer matrices
 * (used whenever a sequence_header doesn't load a custom one), and the
 * picture_rate/pel_aspect_ratio lookup tables (Table 6-4/6-3). Values
 * cross-checked against FFmpeg's libavcodec/mpeg12data.c (the reference
 * decoder this project validates against, see docs/testing.md) rather
 * than transcribed from memory alone.
 */

namespace tinympg {

/**
 * Zig-zag scan order (ISO/IEC 11172-2 Figure 2-D): kZigzag[i] is the
 * raster-order (row*8+col) position of the i-th coefficient in scan
 * order. Used both to read sequence_header's optional custom quantizer
 * matrices (stored in the bitstream in this order) and to place decoded
 * DCT coefficients into their raster position during block decode.
 */
static const uint8_t kZigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63,
};

/**
 * Default intra_quantizer_matrix (ISO/IEC 11172-2 clause 2.4.4.4), raster
 * (row-major) order - used whenever sequence_header's
 * load_intra_quantizer_matrix flag is 0. Matches FFmpeg's
 * ff_mpeg1_default_intra_matrix exactly.
 */
static const uint16_t kDefaultIntraMatrix[64] = {
     8, 16, 19, 22, 26, 27, 29, 34,
    16, 16, 22, 24, 27, 29, 34, 37,
    19, 22, 26, 27, 29, 34, 34, 38,
    22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48,
    26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69,
    27, 29, 35, 38, 46, 56, 69, 83,
};

/**
 * Default non_intra_quantizer_matrix (flat/uniform) - used whenever
 * sequence_header's load_non_intra_quantizer_matrix flag is 0. Not
 * consumed by Phase A (I-frame only decode never dequantizes a
 * non-intra block), kept here for the Phase B (P-frame) decode that will
 * need it.
 */
static const uint16_t kDefaultNonIntraMatrix[64] = {
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
};

/// A small rational number, used for exact (non-fractional-drift) frame rates.
struct Rational {
  int32_t num;
  int32_t den;
};

/**
 * picture_rate (Table 6-4) -> frames/second, indexed by the 4-bit
 * frame_rate_code straight from sequence_header (1..8 valid, 0 and 9..15
 * reserved/forbidden).
 */
static const Rational kFrameRateTable[16] = {
    {0, 1},        // 0: forbidden
    {24000, 1001}, // 1: 23.976...
    {24, 1},       // 2
    {25, 1},       // 3
    {30000, 1001}, // 4: 29.97...
    {30, 1},       // 5
    {50, 1},       // 6
    {60000, 1001}, // 7: 59.94...
    {60, 1},       // 8
    {0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 1},  // 9..15: reserved
};

/**
 * pel_aspect_ratio (Table 6-3), indexed by the 4-bit aspect_ratio_code
 * straight from sequence_header (1..14 valid, 0 and 15 reserved/
 * forbidden). Matches FFmpeg's ff_mpeg1_aspect exactly.
 */
static const float kAspectRatioTable[16] = {
    0.0000f, 1.0000f, 0.6735f, 0.7031f,
    0.7615f, 0.8055f, 0.8437f, 0.8935f,
    0.9157f, 0.9815f, 1.0255f, 1.0695f,
    1.0950f, 1.1575f, 1.2015f, 0.0000f,
};

}  // namespace tinympg
