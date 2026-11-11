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

#include <cstdint>
#include "common/drcs_bits.hpp"
#include "renderer/alphablend.hpp"
#include "renderer/bitmap.hpp"
#include "renderer/canvas.hpp"
#include "renderer/drcs_renderer.hpp"

namespace aribcaption {

bool DRCSRenderer::DrawDRCS(const DRCS& drcs, CharStyle style, ColorRGBA color, ColorRGBA stroke_color,
                            int stroke_width, int target_width, int target_height,
                            Bitmap& target_bmp, int target_x, int target_y) {
    if (target_width <= 0 || target_height <= 0) {
        return false;
    }

    // Validate the whole definition before touching the target bitmap,
    // so a failed draw never leaves a partial stroke or glyph behind.
    if (!DRCSValidateDefinition(drcs)) {
        return false;
    }

    Canvas canvas(target_bmp);

    // Draw stroke (border) if needed
    if (style & CharStyle::kCharStyleStroke) {
        Bitmap stroke_bitmap = DRCSToColoredBitmap(drcs, target_width, target_height, stroke_color);

        canvas.DrawBitmap(stroke_bitmap, target_x - stroke_width, target_y);
        canvas.DrawBitmap(stroke_bitmap, target_x + stroke_width, target_y);
        canvas.DrawBitmap(stroke_bitmap, target_x, target_y - stroke_width);
        canvas.DrawBitmap(stroke_bitmap, target_x, target_y + stroke_width);
    }

    // Draw DRCS with text color
    Bitmap text_bitmap = DRCSToColoredBitmap(drcs, target_width, target_height, color);
    canvas.DrawBitmap(text_bitmap, target_x, target_y);

    return true;
}

Bitmap DRCSRenderer::DRCSToColoredBitmap(const DRCS& drcs, int target_width, int target_height, ColorRGBA color) {
    Bitmap bitmap(target_width, target_height, PixelFormat::kRGBA8888);

    float x_fraction = static_cast<float>(drcs.width) / static_cast<float>(target_width);
    float y_fraction = static_cast<float>(drcs.height) / static_cast<float>(target_height);

    for (int y = 0; y < target_height; y++) {
        ColorRGBA* dest = bitmap.GetPixelAt(0, y);
        int drcs_y = static_cast<int>(y_fraction * static_cast<float>(y));
        for (int x = 0; x < target_width; x++) {
            int drcs_x = static_cast<int>(x_fraction * static_cast<float>(x));

            // Samples are packed MSB-first and may span bytes.
            size_t sample_index = static_cast<size_t>(drcs_y) * static_cast<size_t>(drcs.width) +
                                  static_cast<size_t>(drcs_x);
            uint32_t value = DRCSReadSample(drcs, sample_index);
            uint8_t grey = alphablend::Clamp255(static_cast<uint32_t>(255) * value /
                                                static_cast<uint32_t>(drcs.depth - 1));

            if (grey) {
                uint8_t alpha = (alphablend::WeightOf(grey) * color.a) >> 8;
                dest[x] = ColorRGBA(color, alpha);
            } else {
                dest[x] = ColorRGBA(0);
            }
        }
    }

    return bitmap;
}

}  // namespace aribcaption
