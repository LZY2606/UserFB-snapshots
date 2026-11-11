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

#ifndef ARIBCAPTION_DRCS_BITS_HPP
#define ARIBCAPTION_DRCS_BITS_HPP

#include <cstddef>
#include <cstdint>
#include <limits>
#include "aribcaption/caption.hpp"

namespace aribcaption {

/**
 * Number of bits needed to represent a sample value in [0, depth - 1].
 * This is ceil(log2(depth)) for depth >= 2, and 0 for depth <= 1.
 */
inline int DRCSDepthBits(int depth) {
    int bits = 0;
    for (int n = depth - 1; n > 0; n >>= 1) {
        bits++;
    }
    return bits;
}

/**
 * Read one sample from the packed pixel buffer.
 *
 * Samples are packed MSB-first, without aligning each row to a byte boundary.
 * A sample may therefore span two bytes, and a row may share its last byte
 * with the next row. Bits past the last sample in the buffer are padding and
 * are never read as part of a sample.
 */
inline uint32_t DRCSReadSample(const DRCS& drcs, size_t sample_index) {
    size_t bit_pos = sample_index * static_cast<size_t>(drcs.depth_bits);
    size_t byte_index = bit_pos / 8;
    int bit_offset = static_cast<int>(bit_pos % 8);

    uint32_t word = static_cast<uint32_t>(drcs.pixels[byte_index]) << 8;
    if (byte_index + 1 < drcs.pixels.size()) {
        word |= drcs.pixels[byte_index + 1];
    }

    int shift = 16 - bit_offset - drcs.depth_bits;
    return (word >> shift) & ((1u << drcs.depth_bits) - 1);
}

/**
 * Check whether a DRCS definition is drawable.
 *
 * Requirements:
 *  - width and height must be positive;
 *  - depth must be in [2, 256] and depth_bits must be ceil(log2(depth));
 *  - the pixel buffer must hold ceil(width * height * depth_bits / 8) bytes;
 *  - every effective sample must be in [0, depth - 1].
 *
 * Trailing bits that do not belong to any sample are ignored.
 */
inline bool DRCSValidateDefinition(const DRCS& drcs) {
    if (drcs.width <= 0 || drcs.height <= 0) {
        return false;
    }
    if (drcs.depth < 2 || drcs.depth > 256) {
        return false;
    }
    if (drcs.depth_bits != DRCSDepthBits(drcs.depth)) {
        return false;
    }

    uint64_t sample_count = static_cast<uint64_t>(drcs.width) * static_cast<uint64_t>(drcs.height);
    uint64_t max_sample_count = std::numeric_limits<uint64_t>::max() / static_cast<uint64_t>(drcs.depth_bits);
    if (sample_count > max_sample_count) {
        return false;
    }

    uint64_t total_bits = sample_count * static_cast<uint64_t>(drcs.depth_bits);
    uint64_t required_bytes = (total_bits + 7) / 8;
    if (required_bytes > drcs.pixels.size()) {
        return false;
    }

    for (uint64_t i = 0; i < sample_count; i++) {
        if (DRCSReadSample(drcs, static_cast<size_t>(i)) >= static_cast<uint32_t>(drcs.depth)) {
            return false;
        }
    }

    return true;
}

}  // namespace aribcaption

#endif  // ARIBCAPTION_DRCS_BITS_HPP
