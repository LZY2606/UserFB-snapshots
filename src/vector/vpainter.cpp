/*
 * Copyright (c) 2020 Samsung Electronics Co., Ltd. All rights reserved.

 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "vpainter.h"
#include <algorithm>
#include <cstring>


V_BEGIN_NAMESPACE


void VPainter::drawRle(const VPoint &, const VRle &rle)
{
    if (rle.empty()) return;
    // mSpanData.updateSpanFunc();

    if (!mSpanData.mUnclippedBlendFunc) return;

    // do draw after applying clip.
    rle.intersect(mSpanData.clipRect(), mSpanData.mUnclippedBlendFunc,
                  &mSpanData);
}

void VPainter::drawRle(const VRle &rle, const VRle &clip)
{
    if (rle.empty() || clip.empty()) return;

    if (!mSpanData.mUnclippedBlendFunc) return;

    rle.intersect(clip, mSpanData.mUnclippedBlendFunc, &mSpanData);
}

static void fillRect(const VRect &r, VSpanData *data)
{
    auto x1 = std::max(r.x(), 0);
    auto x2 = std::min(r.x() + r.width(), data->mDrawableSize.width());
    auto y1 = std::max(r.y(), 0);
    auto y2 = std::min(r.y() + r.height(), data->mDrawableSize.height());

    if (x2 <= x1 || y2 <= y1) return;

    const int  nspans = 256;
    VRle::Span spans[nspans];

    int y = y1;
    while (y < y2) {
        int n = std::min(nspans, y2 - y);
        int i = 0;
        while (i < n) {
            spans[i].x = short(x1);
            spans[i].len = uint16_t(x2 - x1);
            spans[i].y = short(y + i);
            spans[i].coverage = 255;
            ++i;
        }

        data->mUnclippedBlendFunc(n, spans, data);
        y += n;
    }
}

void VPainter::drawBitmapUntransform(const VRect &  target,
                                         const VBitmap &bitmap,
                                         const VRect &  source,
                                         uint8_t        const_alpha)
{
    mSpanData.initTexture(&bitmap, const_alpha, source);
    if (!mSpanData.mUnclippedBlendFunc) return;

    // update translation matrix for source texture.
    mSpanData.dx = float(target.x() - source.x());
    mSpanData.dy = float(target.y() - source.y());

    fillRect(target, &mSpanData);
}

VPainter::VPainter(VBitmap *buffer)
{
    begin(buffer);
}
bool VPainter::begin(VBitmap *buffer, bool clearOnBegin)
{
    mClearOnBegin = clearOnBegin;
    mBuffer.prepare(buffer);
    mSpanData.init(&mBuffer);
    // Offscreen layer surfaces start from clean pixels. A painter that wraps
    // a caller owned surface keeps the existing pixels and clears only its
    // draw region explicitly (see clearDrawRegion).
    if (mClearOnBegin) mBuffer.clear();
    return true;
}
void VPainter::end() {}

void VPainter::clearDrawRegion()
{
    const VPoint &offset = mSpanData.mOffset;
    const VSize  &size = mSpanData.mDrawableSize;
    if (size.width() <= 0 || size.height() <= 0) return;

    const size_t bytesPerPixel = mBuffer.bytesPerPixel();
    const size_t rowBytes = size_t(size.width()) * bytesPerPixel;
    for (int y = 0; y < size.height(); ++y) {
        // scanLine addresses the full surface, so add the region offset
        // explicitly; pixels outside the region (including per-row padding)
        // are never touched.
        uint8_t *row = mBuffer.scanLine(y + offset.y()) +
                       size_t(offset.x()) * bytesPerPixel;
        memset(row, 0, rowBytes);
    }
}

void VPainter::setDrawRegion(const VRect &region)
{
    mSpanData.setDrawRegion(region);
}

void VPainter::setBrush(const VBrush &brush)
{
    mSpanData.setup(brush);
}

void VPainter::setBlendMode(BlendMode mode)
{
    mSpanData.mBlendMode = mode;
}

void VPainter::blendLayerSurface(const VBitmap &bitmap, LayerBlendMode mode,
                                 uint8_t constAlpha)
{
    if (!bitmap.valid()) return;

    const VSize  &size = mSpanData.mDrawableSize;
    if (size.width() <= 0 || size.height() <= 0) return;

    // Offscreen layer surfaces are rendered at the draw region size. Build a
    // destination view rooted at the region origin: the destination keeps the
    // full surface stride so per-row padding is preserved byte for byte.
    if (bitmap.width() != size_t(size.width()) ||
        bitmap.height() != size_t(size.height()))
        return;

    uint8_t *origin = reinterpret_cast<uint8_t *>(mSpanData.buffer(0, 0));
    VBitmap  dstView(origin, size_t(size.width()), size_t(size.height()),
                     mBuffer.bytesPerLine(), mBuffer.format());
    blendLayer(dstView, bitmap, mode, constAlpha);
}

VRect VPainter::clipBoundingRect() const
{
    return mSpanData.clipRect();
}

void VPainter::drawBitmap(const VPoint &point, const VBitmap &bitmap,
                          const VRect &source, uint8_t const_alpha)
{
    if (!bitmap.valid()) return;

    drawBitmap(VRect(point, bitmap.size()),
               bitmap, source, const_alpha);
}

void VPainter::drawBitmap(const VRect &target, const VBitmap &bitmap,
                          const VRect &source, uint8_t const_alpha)
{
    if (!bitmap.valid()) return;

    // clear any existing brush data.
    setBrush(VBrush());

    if (target.size() == source.size()) {
        drawBitmapUntransform(target, bitmap, source, const_alpha);
    } else {
        // @TODO scaling
    }
}

void VPainter::drawBitmap(const VPoint &point, const VBitmap &bitmap,
                          uint8_t const_alpha)
{
    if (!bitmap.valid()) return;

    drawBitmap(VRect(point, bitmap.size()),
               bitmap, bitmap.rect(),
               const_alpha);
}

void VPainter::drawBitmap(const VRect &rect, const VBitmap &bitmap,
                          uint8_t const_alpha)
{
    if (!bitmap.valid()) return;

    drawBitmap(rect, bitmap, bitmap.rect(),
               const_alpha);
}

V_END_NAMESPACE
