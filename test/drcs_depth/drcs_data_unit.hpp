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

#ifndef ARIBCAPTION_TEST_DRCS_DATA_UNIT_HPP
#define ARIBCAPTION_TEST_DRCS_DATA_UNIT_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

namespace drcs_test {

// ceil(log2(depth)) for depth >= 1.
int DepthBitsOf(int depth);

// Pack sample values MSB-first, without row padding.
std::vector<uint8_t> PackSamples(const std::vector<uint32_t>& samples, int depth_bits);

// One font record inside a DRCS definition. The pixel buffer is kept raw so
// that tests can construct truncated or out-of-range definitions on purpose.
struct FontRecord {
    uint8_t font_id = 0;
    uint8_t mode = 1;           // 0b0000 or 0b0001, both carry a bitmap definition
    uint8_t depth_field = 0;    // encoded depth: actual depth - 2
    uint8_t width = 0;
    uint8_t height = 0;
    std::vector<uint8_t> pixels;
};

// One character code and the font records defined for it.
struct CodeRecord {
    uint16_t character_code = 0;
    std::vector<FontRecord> fonts;
};

// Build a valid bitmap font record, packing samples with the depth's bit width.
FontRecord MakeValidFont(int width, int height, int depth, const std::vector<uint32_t>& samples);

// Build the body of a DRCS data unit (number_of_code followed by code/font records).
std::vector<uint8_t> MakeDRCSBody(const std::vector<CodeRecord>& codes);

// Wrap a body into a data unit with a 24-bit size field.
std::vector<uint8_t> MakeDataUnit(uint8_t data_unit_parameter, const std::vector<uint8_t>& body);

// Assemble a complete caption PES containing the given data units.
// The statement part is TMD=0 with a data unit loop.
std::vector<uint8_t> MakePES(const std::vector<std::vector<uint8_t>>& data_units);

// Minimal statement body: clear screen, writing format 7, designate DRCS-1 to G0,
// move to the given character cell, then emit the DRCS character code.
std::vector<uint8_t> MakeDRCSStatement(uint8_t char_code, uint8_t cell_x, uint8_t cell_y);

}  // namespace drcs_test

#endif  // ARIBCAPTION_TEST_DRCS_DATA_UNIT_HPP
