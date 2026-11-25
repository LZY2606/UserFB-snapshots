#pragma once
#include <stdint.h>
#include <stddef.h>

/*
 * Header-only. Variable-length code tables (ISO/IEC 11172-2 Annex B):
 * dct_dc_size_luminance/chrominance (Table B.5/B.6), the combined DCT
 * coefficient run/level table (Table B.14, including its escape and
 * end-of-block codes), macroblock_address_increment (Table B.1),
 * macroblock_type for P- and B-pictures (Table B.2's P- and B-picture
 * rows - I-pictures still don't need a table, see
 * decoder/mpg_macroblock.h::decodeMbTypeIntra()), coded_block_pattern
 * (Table B.9), and motion_code (Table B.10, shared by forward and
 * backward motion vectors alike - motion_residual isn't a VLC, it's
 * `fcode - 1` raw bits, decoded directly in decoder/mpg_motion.h).
 *
 * Every code/bits/run/level value below was cross-checked against
 * FFmpeg's libavcodec/mpeg12data.c (ff_mpeg12_vlc_dc_lum_code/bits,
 * ff_mpeg12_vlc_dc_chroma_code/bits, ff_mpeg1_vlc_table, ff_mpeg12_run,
 * ff_mpeg12_level, ff_mpeg12_mbAddrIncrTable, ff_mpeg12_mbPatTable,
 * ff_mpeg12_mbMotionVectorTable) and libavcodec/mpeg12.c
 * (table_mb_ptype/ptype2mb_type, table_mb_btype/btype2mb_type) rather
 * than transcribed from memory alone - a single wrong bit in one of these
 * breaks decoding in a way that's very hard to spot by inspection, so
 * this project's own correctness oracle (docs/testing.md) is what
 * actually validates them, not just cross-referencing.
 *
 * Decoded via BitReader::readVlc() (decoder/mpg_bitreader.h): a linear
 * scan comparing the next `maxBits` peeked bits (left-aligned) against
 * each entry's own code/bits - correct for any true prefix-free VLC table
 * regardless of scan order, though not the fastest possible approach (a
 * binary trie would be O(bits) instead of O(table size) per symbol) -
 * left as a future optimization once correctness is settled (see
 * docs/memory-budget.md's Performance section - now a real measured
 * number, not just a TBD).
 */

namespace tinympg {

/// dct_dc_size_luminance (Table B.5), values 0..11 indexed by table row.
static const uint16_t kDcLumCode[12] = {
    0x4, 0x0, 0x1, 0x5, 0x6, 0xe, 0x1e, 0x3e, 0x7e, 0xfe, 0x1fe, 0x1ff,
};
static const uint8_t kDcLumBits[12] = {
    3, 2, 2, 3, 3, 4, 5, 6, 7, 8, 9, 9,
};
static const int kDcLumMaxBits = 9;

/// dct_dc_size_chrominance (Table B.6), values 0..11 indexed by table row.
static const uint16_t kDcChromaCode[12] = {
    0x0, 0x1, 0x2, 0x6, 0xe, 0x1e, 0x3e, 0x7e, 0xfe, 0x1fe, 0x3fe, 0x3ff,
};
static const uint8_t kDcChromaBits[12] = {
    2, 2, 2, 3, 4, 5, 6, 7, 8, 9, 10, 10,
};
static const int kDcChromaMaxBits = 10;

/**
 * Combined DCT coefficient run/level table (Table B.14): 111 (run,level)
 * codes, followed by the escape code (index kAcEscapeIndex) and the
 * end-of-block code (index kAcEobIndex). kAcRun[i]/kAcLevel[i] are only
 * valid for i < kAcEscapeIndex; decodeIntraBlock() (mpg_macroblock.h)
 * handles the escape/EOB indices specially. kAcLevel[] values are always
 * the *positive* magnitude - the bitstream carries the sign as a separate
 * bit immediately after a matched non-escape code.
 */
static const int kAcTableSize = 113;
static const int kAcEscapeIndex = 111;
static const int kAcEobIndex = 112;
static const int kAcMaxBits = 16;

static const uint16_t kAcCode[113] = {
    0x3,  0x4,  0x5,  0x6,  0x26, 0x21, 0xa,  0x1d, 0x18, 0x13, 0x10, 0x1a,
    0x19, 0x18, 0x17, 0x1f, 0x1e, 0x1d, 0x1c, 0x1b, 0x1a, 0x19, 0x18, 0x17,
    0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x18, 0x17, 0x16, 0x15, 0x14,
    0x13, 0x12, 0x11, 0x10, 0x3,  0x6,  0x25, 0xc,  0x1b, 0x16, 0x15, 0x1f,
    0x1e, 0x1d, 0x1c, 0x1b, 0x1a, 0x19, 0x13, 0x12, 0x11, 0x10, 0x5,  0x4,
    0xb,  0x14, 0x14, 0x7,  0x24, 0x1c, 0x13, 0x6,  0xf,  0x12, 0x7,  0x9,
    0x12, 0x5,  0x1e, 0x14, 0x4,  0x15, 0x7,  0x11, 0x5,  0x11, 0x27, 0x10,
    0x23, 0x1a, 0x22, 0x19, 0x20, 0x18, 0xe,  0x17, 0xd,  0x16, 0x8,  0x15,
    0x1f, 0x1a, 0x19, 0x17, 0x16, 0x1f, 0x1e, 0x1d, 0x1c, 0x1b, 0x1f, 0x1e,
    0x1d, 0x1c, 0x1b,
    0x1,  // escape (index 111)
    0x2,  // EOB (index 112)
};
static const uint8_t kAcBits[113] = {
    2,  4,  5,  7,  8,  8,  10, 12, 12, 12, 12, 13,
    13, 13, 13, 14, 14, 14, 14, 14, 14, 14, 14, 14,
    14, 14, 14, 14, 14, 14, 14, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 3,  6,  8,  10, 12, 13, 13, 15,
    15, 15, 15, 15, 15, 15, 16, 16, 16, 16, 4,  7,
    10, 12, 13, 5,  8,  12, 13, 5,  10, 12, 6,  10,
    13, 6,  12, 16, 6,  12, 7,  12, 7,  13, 8,  13,
    8,  16, 8,  16, 8,  16, 10, 16, 10, 16, 10, 16,
    12, 12, 12, 12, 12, 13, 13, 13, 13, 13, 16, 16,
    16, 16, 16,
    6,  // escape (index 111)
    2,  // EOB (index 112)
};
static const int8_t kAcLevel[111] = {
     1,  2,  3,  4,  5,  6,  7,  8,
     9, 10, 11, 12, 13, 14, 15, 16,
    17, 18, 19, 20, 21, 22, 23, 24,
    25, 26, 27, 28, 29, 30, 31, 32,
    33, 34, 35, 36, 37, 38, 39, 40,
     1,  2,  3,  4,  5,  6,  7,  8,
     9, 10, 11, 12, 13, 14, 15, 16,
    17, 18,  1,  2,  3,  4,  5,  1,
     2,  3,  4,  1,  2,  3,  1,  2,
     3,  1,  2,  3,  1,  2,  1,  2,
     1,  2,  1,  2,  1,  2,  1,  2,
     1,  2,  1,  2,  1,  2,  1,  2,
     1,  1,  1,  1,  1,  1,  1,  1,
     1,  1,  1,  1,  1,  1,  1,
};
static const int8_t kAcRun[111] = {
     0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,
     1,  1,  1,  1,  1,  1,  1,  1,
     1,  1,  1,  1,  1,  1,  1,  1,
     1,  1,  2,  2,  2,  2,  2,  3,
     3,  3,  3,  4,  4,  4,  5,  5,
     5,  6,  6,  6,  7,  7,  8,  8,
     9,  9, 10, 10, 11, 11, 12, 12,
    13, 13, 14, 14, 15, 15, 16, 16,
    17, 18, 19, 20, 21, 22, 23, 24,
    25, 26, 27, 28, 29, 30, 31,
};

/**
 * macroblock_address_increment (Table B.1), 36 rows. Rows 0..32 (0-based)
 * are the real values 1..33 - the decoded row index IS (value - 1), used
 * directly as the numeric contribution to a running column/skip-run total
 * (see decodeMbAddrIncrement() in mpg_macroblock.h): row 32 is value 33.
 * Row 33 is macroblock_escape (adds 33 to the total and keeps reading),
 * row 34 is macroblock_stuffing (adds nothing, keeps reading), and row 35
 * is a deliberately all-zero-bits guard code that naturally matches the
 * zero-padding between the last macroblock of a slice and the next
 * (byte-aligned) start code - decodeMbAddrIncrement() treats it as
 * end-of-slice, not an error.
 */
static const int kMbAddrIncTableSize = 36;
static const int kMbAddrIncMaxBits = 11;
static const int kMbAddrIncEscapeIndex = 33;
static const int kMbAddrIncStuffingIndex = 34;
static const int kMbAddrIncEndIndex = 35;

static const uint16_t kMbAddrIncCode[36] = {
    0x1,  0x3,  0x2,  0x3,  0x2,  0x3,  0x2,  0x7,  0x6,  0xb,
    0xa,  0x9,  0x8,  0x7,  0x6,  0x17, 0x16, 0x15, 0x14, 0x13,
    0x12, 0x23, 0x22, 0x21, 0x20, 0x1f, 0x1e, 0x1d, 0x1c, 0x1b,
    0x1a, 0x19, 0x18, 0x8,  0xf,  0x0,
};
static const uint8_t kMbAddrIncBits[36] = {
    1,  3,  3,  4,  4,  5,  5,  7,  7,  8,
    8,  8,  8,  8,  8,  10, 10, 10, 10, 10,
    10, 11, 11, 11, 11, 11, 11, 11, 11, 11,
    11, 11, 11, 11, 11, 8,
};

/**
 * macroblock_type, P-picture rows only (Table B.2 - the I-picture rows
 * are handled separately by decodeMbTypeIntra(), and the B-picture rows
 * are the separate kMbTypeBCode/kMbTypeBBits table below). 7 entries; the
 * semantic meaning of each row (which of intra/quant/explicit-forward-mv/
 * zero-mv/coded_block_pattern it signals) is encoded in decodeMbTypeP()
 * (mpg_macroblock.h), mirroring FFmpeg's ptype2mb_type[] rather than
 * duplicating its MB_TYPE_* bitmask scheme here.
 */
static const int kMbTypePTableSize = 7;
static const int kMbTypePMaxBits = 6;
static const uint16_t kMbTypePCode[7] = {
    0x3, 0x1, 0x1, 0x1, 0x1, 0x1, 0x2,
};
static const uint8_t kMbTypePBits[7] = {
    5, 2, 3, 1, 6, 5, 5,
};

/**
 * macroblock_type, B-picture rows (Table B.2's other half). 11 entries;
 * the semantic meaning of each row (intra/quant/forward-only/backward-
 * only/bidirectional/coded_block_pattern) is encoded in decodeMbTypeB()
 * (mpg_macroblock.h), mirroring FFmpeg's btype2mb_type[].
 */
static const int kMbTypeBTableSize = 11;
static const int kMbTypeBMaxBits = 6;
static const uint16_t kMbTypeBCode[11] = {
    0x3, 0x2, 0x3, 0x2, 0x3, 0x2, 0x3, 0x1, 0x2, 0x3, 0x2,
};
static const uint8_t kMbTypeBBits[11] = {
    5, 3, 3, 4, 4, 2, 2, 6, 6, 6, 5,
};

/**
 * coded_block_pattern (Table B.9), 64 rows. Unlike macroblock_address_
 * increment, the decoded row index *is* the 6-bit coded_block_pattern
 * value directly (bit 5 = block 0 (Y0) ... bit 0 = block 5 (Cr), tested
 * MSB-first in decodeCodedBlockPattern()) - no further remapping. Row 0
 * (cbp == 0, "no block coded") is reserved/never actually signaled (a
 * macroblock with nothing coded doesn't set the coded_block_pattern-
 * present bit in macroblock_type in the first place) - decodeCodedBlockPattern()
 * rejects it as a bitstream error, matching FFmpeg's own `cbp <= 0` check.
 */
static const int kCbpTableSize = 64;
static const int kCbpMaxBits = 9;
static const uint16_t kCbpCode[64] = {
    0x1,  0xb,  0x9,  0xd,  0xd,  0x17, 0x13, 0x1f,
    0xc,  0x16, 0x12, 0x1e, 0x13, 0x1b, 0x17, 0x13,
    0xb,  0x15, 0x11, 0x1d, 0x11, 0x19, 0x15, 0x11,
    0xf,  0xf,  0xd,  0x3,  0xf,  0xb,  0x7,  0x7,
    0xa,  0x14, 0x10, 0x1c, 0xe,  0xe,  0xc,  0x2,
    0x10, 0x18, 0x14, 0x10, 0xe,  0xa,  0x6,  0x6,
    0x12, 0x1a, 0x16, 0x12, 0xd,  0x9,  0x5,  0x5,
    0xc,  0x8,  0x4,  0x4,  0x7,  0xa,  0x8,  0xc,
};
static const uint8_t kCbpBits[64] = {
    9, 5, 5, 6, 4, 7, 7, 8,
    4, 7, 7, 8, 5, 8, 8, 8,
    4, 7, 7, 8, 5, 8, 8, 8,
    6, 8, 8, 9, 5, 8, 8, 9,
    4, 7, 7, 8, 6, 8, 8, 9,
    5, 8, 8, 8, 5, 8, 8, 9,
    5, 8, 8, 8, 5, 8, 8, 9,
    5, 8, 8, 9, 3, 5, 5, 6,
};

/**
 * motion_code (Table B.10), 17 rows: row 0 ("no motion_code", a single
 * '1' bit) means the motion vector component equals its predictor
 * unchanged - see decodeMotionComponent() (mpg_motion.h), which handles
 * that case before consuming a sign bit or any motion_residual bits.
 * Rows 1..16 are the magnitude 1..16 (sign coded separately, then scaled
 * up by f_code via motion_residual raw bits for f_code > 1).
 */
static const int kMotionCodeTableSize = 17;
static const int kMotionCodeMaxBits = 10;
static const uint16_t kMotionCode[17] = {
    0x1, 0x1, 0x1, 0x1, 0x3, 0x5, 0x4, 0x3, 0xb,
    0xa, 0x9, 0x11, 0x10, 0xf, 0xe, 0xd, 0xc,
};
static const uint8_t kMotionCodeBits[17] = {
    1, 2, 3, 4, 6, 7, 7, 7, 9,
    9, 9, 10, 10, 10, 10, 10, 10,
};

}  // namespace tinympg
