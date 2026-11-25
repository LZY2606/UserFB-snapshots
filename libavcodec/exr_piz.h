/*
 * OpenEXR PIZ compression helpers.
 *
 * PIZ packs 16-bit words through a used-value bitmap, a compact remapping
 * LUT, a reversible Haar wavelet and a canonical Huffman stream.  The
 * implementation in this file is independent from libavcodec's EXR decoder
 * but writes the on-disk structures parsed by exr.c.
 */

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

#ifndef AVCODEC_EXR_PIZ_H
#define AVCODEC_EXR_PIZ_H

#include <stddef.h>
#include <stdint.h>

#define EXR_PIZ_BLOCK_SCANLINES 32
#define EXR_PIZ_BITMAP_SIZE     8192
#define EXR_PIZ_USHORT_RANGE    65536
#define EXR_PIZ_HUF_ENCSIZE     (EXR_PIZ_USHORT_RANGE + 1)

typedef struct EXRPizWork EXRPizWork;

EXRPizWork *ff_exr_piz_work_alloc(void);
void ff_exr_piz_work_free(EXRPizWork **work);

/*
 * Return a conservative temporary-buffer size for a PIZ block containing
 * raw_size bytes of packed EXR pixel data.  A block which does not compress
 * below raw_size is stored uncompressed by the caller.
 */
size_t ff_exr_piz_max_block_size(size_t raw_size);

/*
 * Encode one PIZ scanline block.
 *
 * packed/packed_size use the ordinary EXR scanline layout: row, then
 * channels in header order, then width * word_pairs 16-bit little-endian
 * words per channel sample.  word_pairs is 1 for HALF and 2 for FLOAT.
 * width/height are the block dimensions (height may be the final, short
 * block).  On success returns the positive PIZ chunk size in bytes, zero if
 * the caller must store the uncompressed block instead, or a negative
 * AVERROR code.
 */
int ff_exr_piz_encode(EXRPizWork *work,
                      uint8_t *dst, size_t dst_capacity,
                      const uint8_t *packed, size_t packed_size,
                      int width, int height,
                      int channels, int word_pairs);

#endif /* AVCODEC_EXR_PIZ_H */
