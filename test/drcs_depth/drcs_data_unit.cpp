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

#include "drcs_data_unit.hpp"

namespace drcs_test {

int DepthBitsOf(int depth) {
    int bits = 0;
    for (int n = depth - 1; n > 0; n >>= 1) {
        bits++;
    }
    return bits;
}

std::vector<uint8_t> PackSamples(const std::vector<uint32_t>& samples, int depth_bits) {
    std::vector<uint8_t> packed((samples.size() * static_cast<size_t>(depth_bits) + 7) / 8, 0);

    for (size_t i = 0; i < samples.size(); i++) {
        size_t bit_pos = i * static_cast<size_t>(depth_bits);
        uint32_t value = samples[i] & ((1u << depth_bits) - 1);
        for (int bit = 0; bit < depth_bits; bit++) {
            uint32_t mask = 1u << (depth_bits - 1 - bit);
            if (value & mask) {
                size_t target = bit_pos + static_cast<size_t>(bit);
                packed[target / 8] |= static_cast<uint8_t>(0x80 >> (target % 8));
            }
        }
    }

    return packed;
}

FontRecord MakeValidFont(int width, int height, int depth, const std::vector<uint32_t>& samples) {
    FontRecord record;
    record.mode = 1;
    record.depth_field = static_cast<uint8_t>(depth - 2);
    record.width = static_cast<uint8_t>(width);
    record.height = static_cast<uint8_t>(height);
    record.pixels = PackSamples(samples, DepthBitsOf(depth));
    return record;
}

std::vector<uint8_t> MakeDRCSBody(const std::vector<CodeRecord>& codes) {
    std::vector<uint8_t> body;
    body.push_back(static_cast<uint8_t>(codes.size()));

    for (const CodeRecord& code : codes) {
        body.push_back(static_cast<uint8_t>(code.character_code >> 8));
        body.push_back(static_cast<uint8_t>(code.character_code & 0xFF));
        body.push_back(static_cast<uint8_t>(code.fonts.size()));

        for (const FontRecord& font : code.fonts) {
            body.push_back(static_cast<uint8_t>((font.font_id << 4) | (font.mode & 0x0F)));
            body.push_back(font.depth_field);
            body.push_back(font.width);
            body.push_back(font.height);
            body.insert(body.end(), font.pixels.begin(), font.pixels.end());
        }
    }

    return body;
}

std::vector<uint8_t> MakeDataUnit(uint8_t data_unit_parameter, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> unit;
    unit.push_back(0x1F);
    unit.push_back(data_unit_parameter);
    unit.push_back(static_cast<uint8_t>((body.size() >> 16) & 0xFF));
    unit.push_back(static_cast<uint8_t>((body.size() >> 8) & 0xFF));
    unit.push_back(static_cast<uint8_t>(body.size() & 0xFF));
    unit.insert(unit.end(), body.begin(), body.end());
    return unit;
}

std::vector<uint8_t> MakePES(const std::vector<std::vector<uint8_t>>& data_units) {
    std::vector<uint8_t> statement;
    size_t loop_length = 0;
    for (const auto& unit : data_units) {
        loop_length += unit.size();
    }

    statement.push_back(0x3F);  // TMD = 0
    statement.push_back(static_cast<uint8_t>((loop_length >> 16) & 0xFF));
    statement.push_back(static_cast<uint8_t>((loop_length >> 8) & 0xFF));
    statement.push_back(static_cast<uint8_t>(loop_length & 0xFF));
    for (const auto& unit : data_units) {
        statement.insert(statement.end(), unit.begin(), unit.end());
    }

    std::vector<uint8_t> pes;
    pes.push_back(0x80);  // data_identifier
    pes.push_back(0xFF);  // private_stream_id
    pes.push_back(0xF0);  // PES_data_packet_header_length = 0
    pes.push_back(0x04);  // data_group_id: statement data, language 1, group 0
    pes.push_back(0x00);
    pes.push_back(0x00);
    pes.push_back(static_cast<uint8_t>((statement.size() >> 8) & 0xFF));
    pes.push_back(static_cast<uint8_t>(statement.size() & 0xFF));
    pes.insert(pes.end(), statement.begin(), statement.end());
    return pes;
}

std::vector<uint8_t> MakeDRCSStatement(uint8_t char_code, uint8_t cell_x, uint8_t cell_y) {
    std::vector<uint8_t> body;
    body.push_back(0x0C);                          // CS: clear screen
    body.push_back(0x9B);                          // CSI
    body.push_back(0x37);                          // SWF 7 -> 960x540, 36x36 cells
    body.push_back(0x20);
    body.push_back(0x53);
    body.push_back(0x1B);                          // ESC
    body.push_back(0x28);                          // designate to G0
    body.push_back(0x20);                          // 1-byte DRCS
    body.push_back(0x41);                          // DRCS-1
    body.push_back(0x1C);                          // APS
    body.push_back(cell_y & 0x3F);
    body.push_back(cell_x & 0x3F);
    body.push_back(char_code);
    return body;
}

}  // namespace drcs_test
