/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * OpenEXR PIZ compression helpers used by the EXR encoder.
 *
 * The pipeline is the exact inverse of the PIZ decoding path in exr.c:
 *   1. a bitmap of the 16-bit words actually used plus a sorted reverse
 *      lookup table remaps the words onto dense indices,
 *   2. a reversible two dimensional Haar style wavelet is applied per
 *      channel and per 16-bit word plane (float channels own two planes),
 *   3. the wavelet coefficients are entropy coded with the canonical
 *      Huffman table and bit stream layout defined by the EXR format.
 *
 * Nothing here depends on another EXR library or an external program.
 */

#ifndef AVCODEC_EXRPIZ_H
#define AVCODEC_EXRPIZ_H

#include <stdint.h>

/* A PIZ block holds at most 32 scan lines; the usage bitmap stores one byte
 * per eight 16-bit words and thus covers the whole 65536 value range. */
#define FF_EXR_PIZ_MAX_LINES    32
#define FF_EXR_PIZ_BITMAP_SIZE  (1 << 13)

typedef struct EXRPizEnc {
    /* value usage bitmap, sorted reverse LUT and word -> dense index map */
    uint8_t  *bitmap;
    uint16_t *lut;
    uint16_t *map;

    /* wavelet coefficients remapped for the Huffman stage */
    uint16_t *mapped;
    int       mapped_cap;

    /* Huffman frequency / code book, indexed by coefficient value */
    uint32_t *freq;
    uint32_t *code;
    uint8_t  *length;
} EXRPizEnc;

int  ff_exr_piz_init(EXRPizEnc *s);
void ff_exr_piz_uninit(EXRPizEnc *s);

/**
 * Encode one PIZ block.
 *
 * @param words           nb raw 16-bit words, channel major layout with the
 *                        pixel_half_size planes of one channel interleaved;
 *                        transformed in place
 * @param nb              number of words
 * @param nb_channels     number of EXR channels
 * @param pixel_half_size 1 for HALF, 2 for FLOAT
 * @param xsize, ysize    block width and actual number of scan lines
 * @param dst             destination buffer
 * @param dst_size        size of dst in bytes; needs room for up to
 *                        4 * nb + 8192 bytes
 *
 * @return number of compressed bytes written, which is always smaller than
 *         the raw 2 * nb bytes on success; AVERROR(ENOSPC) when the block
 *         cannot be represented smaller than the raw data (the caller must
 *         store it uncompressed), or another negative AVERROR code.
 */
int ff_exr_piz_encode_block(EXRPizEnc *s, uint16_t *words, int nb,
                            int nb_channels, int pixel_half_size,
                            int xsize, int ysize,
                            uint8_t *dst, int dst_size);

#endif /* AVCODEC_EXRPIZ_H */
