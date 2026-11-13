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

#include "vblend.h"
#include <cmath>
#include "vdrawhelper.h"

V_BEGIN_NAMESPACE

static inline uint8_t roundByte(double v)
{
    long n = std::lround(v);
    if (n < 0) n = 0;
    if (n > 255) n = 255;
    return uint8_t(n);
}

// One separable blend function for normalized, unpremultiplied base b and
// source s. The Overlay branch is decided by the base color.
static inline double blendChannel(double b, double s, LayerBlendMode mode)
{
    switch (mode) {
    case LayerBlendMode::Multiply:
        return b * s;
    case LayerBlendMode::Screen:
        return b + s - b * s;
    case LayerBlendMode::Overlay:
        return b <= 0.5 ? (2.0 * b * s)
                        : (1.0 - 2.0 * (1.0 - b) * (1.0 - s));
    default:
        return s;
    }
}

static inline uint32_t packPremul(uint8_t a, double r, double g, double b)
{
    double af = a / 255.0;
    uint8_t pr = roundByte(r * af * 255.0);
    uint8_t pg = roundByte(g * af * 255.0);
    uint8_t pb = roundByte(b * af * 255.0);
    return (uint32_t(a) << 24) | (uint32_t(pr) << 16) |
           (uint32_t(pg) << 8) | uint32_t(pb);
}

// General Porter/Duff style layer composite evaluated in double precision.
// dst/src hold premultiplied ARGB32; the formula consumes normalized
// unpremultiplied colors and writes premultiplied channels back.
static uint32_t blendPixel(uint32_t dst, uint32_t src, LayerBlendMode mode)
{
    int sa = vAlpha(src);
    if (sa == 0) return dst;  // fully transparent source changes nothing, and
                              // its RGB must never participate in the result.

    int da = vAlpha(dst);

    double as = sa / 255.0;
    double ab = da / 255.0;
    double invAs = 1.0 - as;
    double invAb = 1.0 - ab;

    // Unpremultiply. When an alpha is zero its color is irrelevant: every
    // term that could read it is multiplied by that same zero alpha.
    double sr = sa ? vRed(src) * 255.0 / sa : 0.0;
    double sg = sa ? vGreen(src) * 255.0 / sa : 0.0;
    double sb = sa ? vBlue(src) * 255.0 / sa : 0.0;
    double br = da ? vRed(dst) * 255.0 / da : 0.0;
    double bg = da ? vGreen(dst) * 255.0 / da : 0.0;
    double bb = da ? vBlue(dst) * 255.0 / da : 0.0;

    double nr = blendChannel(br / 255.0, sr / 255.0, mode);
    double ng = blendChannel(bg / 255.0, sg / 255.0, mode);
    double nb = blendChannel(bb / 255.0, sb / 255.0, mode);

    double cross = as * ab;
    double outR = invAs * (vRed(dst) / 255.0) +
                  invAb * (vRed(src) / 255.0) + cross * nr;
    double outG = invAs * (vGreen(dst) / 255.0) +
                  invAb * (vGreen(src) / 255.0) + cross * ng;
    double outB = invAs * (vBlue(dst) / 255.0) +
                  invAb * (vBlue(src) / 255.0) + cross * nb;

    uint8_t outA = roundByte((as + ab - cross) * 255.0);
    return packPremul(outA, outR, outG, outB);
}

// Legacy Normal SrcOver, kept bit for bit identical to src_SourceOver so the
// Normal-only output and rounding do not change.
static inline uint32_t sourceOverPixel(uint32_t dst, uint32_t src,
                                       uint8_t constAlpha)
{
    uint32_t s = (constAlpha == 255) ? src : BYTE_MUL(src, constAlpha);
    if (s == 0) return dst;
    uint32_t ialpha = vAlpha(~s);
    return s + BYTE_MUL(dst, ialpha);
}

void blendLayer(VBitmap &dst, const VBitmap &src, LayerBlendMode mode,
                uint8_t constAlpha)
{
    if (!dst.valid() || !src.valid()) return;
    if (src.width() != dst.width() || src.height() != dst.height()) return;

    const size_t w = dst.width();
    const size_t h = dst.height();
    const size_t dstStride = dst.stride();
    const size_t srcStride = src.stride();
    uint8_t *dstData = dst.data();
    const uint8_t *srcData = src.data();

    if (mode == LayerBlendMode::Normal) {
        for (size_t y = 0; y < h; ++y) {
            uint32_t *d = reinterpret_cast<uint32_t *>(dstData + y * dstStride);
            const uint32_t *s =
                reinterpret_cast<const uint32_t *>(srcData + y * srcStride);
            for (size_t x = 0; x < w; ++x)
                d[x] = sourceOverPixel(d[x], s[x], constAlpha);
        }
        return;
    }

    for (size_t y = 0; y < h; ++y) {
        uint32_t *d = reinterpret_cast<uint32_t *>(dstData + y * dstStride);
        const uint32_t *s =
            reinterpret_cast<const uint32_t *>(srcData + y * srcStride);
        for (size_t x = 0; x < w; ++x) {
            // Apply the single optional layer opacity to the source only.
            uint32_t source =
                (constAlpha == 255) ? s[x] : BYTE_MUL(s[x], constAlpha);
            d[x] = blendPixel(d[x], source, mode);
        }
    }
}

V_END_NAMESPACE
