/*
MIT License

Copyright (c) 2026 SSR preview contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#pragma once

/**
 * @file ScreenSpaceReflection.h
 *
 * CPU screen space reflection tracing and compositing on top of a
 * ssr::ReflectionBuffer.
 *
 * Coordinate systems
 * ------------------
 * View space is right handed: the camera sits at the origin and looks down
 * -Z. The front facing depth of a view point P is D = -P.z (positive).
 *
 * The projection matrix is supplied as 16 doubles in ROW MAJOR order, i.e.
 * projection[row][col]. It maps a view space point P (w = 1) to clip space
 * with C = projection * [P, 1]. Only finite conventional perspective
 * projections are accepted, including asymmetric frusta: the fourth row must
 * be (0, 0, -1, 0) so that clip.w = -view.z, and the NDC z range is [-1, 1].
 *
 * Screen coordinates match swr::VertexProcessor: x grows right, y grows down.
 * The viewport origin (x, y) may be non-zero and pixel (x, y) covers the half
 * open cell [x, x+1) x [y, y+1).
 *
 * Thick surface interpretation
 * -----------------------------
 * A valid pixel with front depth D represents the region reached by rays
 * whose projection falls inside its half open pixel cell while their front
 * depth lies in [D, D + thickness]. A hit is the region the ray enters first
 * together with its entry parameter t.
 */

#include "ReflectionBuffer.h"

#include <array>
#include <vector>

namespace ssr {

/// Row major 4x4 projection matrix: element [row][column].
using ProjectionMatrix = std::array<std::array<double, 4>, 4>;

/// Ray generation and marching parameters.
struct TraceParameters {
    double bias = 0.0;        ///< Non-negative finite ray offset along N.
    double thickness = 0.0;   ///< Non-negative finite slab thickness.
    double maxDistance = 0.0; ///< Positive finite trace distance limit.
};

/// Linear RGB image with its own independent storage.
class Image {
public:
    Image() = default;
    Image(int width, int height);

    int width() const { return m_width; }
    int height() const { return m_height; }

    void resize(int width, int height);
    void fill(const float rgb[3]);

    float *data() { return m_pixels.data(); }
    const float *data() const { return m_pixels.data(); }

    float *pixel(int x, int y) { return &m_pixels[((size_t)y * m_width + x) * 3]; }
    const float *pixel(int x, int y) const { return &m_pixels[((size_t)y * m_width + x) * 3]; }

private:
    int m_width = 0;
    int m_height = 0;
    std::vector<float> m_pixels;
};

/// Result of a single reflection ray.
struct TraceResult {
    bool hit = false;            ///< Whether a thick surface region was hit.
    int pixelX = -1;             ///< Hit pixel column (absolute screen coords).
    int pixelY = -1;             ///< Hit pixel row.
    double screenX = 0.0;        ///< Continuous hit screen coordinate.
    double screenY = 0.0;
    double distance = 0.0;       ///< View space distance from O to the hit.
    int visitedCells = 0;        ///< Unique pixel cells visited by the march.
};

/**
 * Validate trace inputs. Throws std::invalid_argument on illegal dimensions,
 * parameters or projection and std::runtime_error on a corrupt buffer. The
 * buffer is never modified.
 */
void validateTraceInputs(const ReflectionBuffer &buffer,
                         const ProjectionMatrix &projection,
                         const Viewport &viewport,
                         const TraceParameters &parameters);

/**
 * Trace a reflection ray generated from one valid buffer pixel.
 *
 * For position P and unit normal N:
 *   I = normalize(P), R = I - 2 * dot(I, N) * N, O = P + bias * N
 * and the march follows O + t R for 0 <= t <= maxDistance. The pixel that
 * owns the source surface is excluded from hit testing.
 *
 * Throws on invalid input without modifying the buffer.
 */
TraceResult tracePixel(const ReflectionBuffer &buffer,
                       const ProjectionMatrix &projection,
                       const Viewport &viewport,
                       const TraceParameters &parameters,
                       int sourcePixelX,
                       int sourcePixelY);

/**
 * Trace an explicit ray. Used by callers that already have O and R. R must be
 * unit length. selfPixelX/selfPixelY name a pixel cell to exclude (-1, -1
 * disables self exclusion).
 */
TraceResult traceRay(const ReflectionBuffer &buffer,
                     const ProjectionMatrix &projection,
                     const Viewport &viewport,
                     const TraceParameters &parameters,
                     const double origin[3],
                     const double reflection[3],
                     int selfPixelX,
                     int selfPixelY);

/**
 * Write the immutable, un-reflected base colors. Invalid pixels receive the
 * environment color.
 */
void composeOriginal(const ReflectionBuffer &buffer,
                     const float environment[3],
                     Image &out);

/**
 * Composite reflections over the whole buffer.
 *
 * For every valid pixel, out = (1 - reflectivity) * baseColor
 * + reflectivity * (hit base color or environment). The G-buffer is treated
 * as immutable while scanning, so composited colors are never read back. The
 * result is independent of scan direction, repeated calls and interleaving
 * two buffers. Invalid pixels receive the environment color.
 */
void composeReflections(const ReflectionBuffer &buffer,
                        const ProjectionMatrix &projection,
                        const Viewport &viewport,
                        const TraceParameters &parameters,
                        const float environment[3],
                        Image &out);

} // namespace ssr
