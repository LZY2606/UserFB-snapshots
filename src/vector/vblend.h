/*
 * Copyright (c) 2020 Samsung Electronics Co., Ltd. All rights reserved.

 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef VBLEND_H
#define VBLEND_H

#include <cstdint>
#include "vbitmap.h"

V_BEGIN_NAMESPACE

// Lottie layer blend modes ("bm"). The numeric values deliberately match the
// model parser values: Normal 0, Multiply 1, Screen 2, Overlay 3. Any unknown
// bm is normalized to Normal before reaching this point.
enum class LayerBlendMode : uint8_t {
    Normal = 0,
    Multiply = 1,
    Screen = 2,
    Overlay = 3,
};

/*
 * Composite one fully rendered layer surface onto a parent backdrop.
 *
 * Both surfaces hold ARGB32 premultiplied pixels of the same width/height.
 * The source already carries every per-layer property (raster coverage, mask
 * and matte results, the layer opacity when it is baked into the content);
 * constAlpha applies one additional opacity multiplication when the caller
 * historically applied it while blitting the layer surface.
 *
 * The non-Normal math runs in double precision on normalized, unpremultiplied
 * color values and rounds to the nearest byte:
 *
 *   Multiply: B = b * s
 *   Screen:   B = b + s - b * s
 *   Overlay:  B = 2*b*s                 when b <= 0.5
 *             B = 1 - 2*(1-b)*(1-s)     otherwise
 *
 *   out = (1-as)*D + (1-ab)*S + as*ab*B(b, s)
 *   outA = as + ab - as*ab
 *
 * Normal keeps the legacy integer SrcOver path so existing rounding is
 * preserved bit for bit.
 */
void blendLayer(VBitmap &dst, const VBitmap &src, LayerBlendMode mode,
                uint8_t constAlpha = 255);

V_END_NAMESPACE

#endif  // VBLEND_H
