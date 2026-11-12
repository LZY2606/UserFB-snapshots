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
 * @file ReflectionBuffer.h
 *
 * Screen space geometry buffer ("G-buffer") for the CPU reflection path.
 *
 * The buffer is image sized and stores, per pixel:
 *   - view space position          (3 floats)
 *   - unit view space normal       (3 floats)
 *   - linear RGB base color        (3 floats, the un-reflected original)
 *   - reflectivity                 (1 float in [0, 1])
 *   - window depth mapped to [0,1] (1 float, used for the depth test)
 *   - a valid flag
 *
 * View space convention: right handed, camera at the origin looking down -Z.
 * The front facing depth of a record is D = -position.z (always positive).
 *
 * Screen coordinates match swr::VertexProcessor: x points right, y points
 * down, pixel (x, y) covers the half open cell [x, x+1) x [y, y+1).
 */

#include <cstdint>
#include <cmath>
#include <vector>

#include "Rasterizer.h"
#include "VertexProcessor.h"
#include "VertexShaderBase.h"
#include "PixelShaderBase.h"

namespace ssr {

/// Pixel rectangle with a possibly non-zero origin, matching the viewport.
struct Viewport {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

/// One interpolated, non-discarded fragment offered to the buffer.
struct GBufferFragment {
    int x = 0;              ///< Screen pixel x (absolute, may use viewport origin).
    int y = 0;              ///< Screen pixel y.
    float depth01 = 0.0f;   ///< Window depth mapped to [0,1]. Smaller is closer.
    float position[3] = {0.0f, 0.0f, 0.0f}; ///< View space position.
    float normal[3] = {0.0f, 0.0f, 1.0f};   ///< Unit normal (normalize after interpolation).
    float color[3] = {0.0f, 0.0f, 0.0f};    ///< Linear RGB base color.
    float reflectivity = 0.0f;              ///< [0,1].
};

/// Immutable view of one stored G-buffer record.
struct GBufferRecord {
    bool valid = false;
    float depth01 = 1.0f;
    float position[3] = {0.0f, 0.0f, 0.0f};
    float normal[3] = {0.0f, 0.0f, 1.0f};
    float color[3] = {0.0f, 0.0f, 0.0f};
    float reflectivity = 0.0f;
};

/**
 * Geometry buffer with an integrated [0,1] depth test.
 *
 * Depth rule: the fragment with the smaller mapped depth wins; on an exact
 * tie the fragment drawn later wins. A fragment is only committed when it is
 * not discarded (the caller decides that) and passes this depth test, so a
 * far surface can never overwrite a near record. Color and geometry are
 * always committed together.
 */
class ReflectionBuffer {
public:
    /// Allocate an empty (fully invalid) width x height buffer.
    ReflectionBuffer(int width, int height);

    int width() const { return m_width; }
    int height() const { return m_height; }

    /// Mark every pixel invalid. No data from a previous frame survives.
    void clear();

    /**
     * Commit one fragment after the caller's discard test.
     *
     * Performs the depth test and writes color and geometry atomically.
     * Throws std::invalid_argument on out-of-range coordinates or non-finite
     * data without modifying any existing record.
     */
    void submit(const GBufferFragment &fragment);

    bool isValid(int x, int y) const;
    const GBufferRecord &record(int x, int y) const;

    /// Front facing view depth D = -position.z. Throws on invalid pixels.
    float viewDepth(int x, int y) const;

    /// Number of currently valid pixels.
    int validCount() const { return m_validCount; }

    /**
     * Validate every valid record: finite data, positive view depth, unit
     * normal, reflectivity in [0,1], depth in [0,1]. Throws
     * std::runtime_error and never mutates the buffer.
     */
    void validate() const;

private:
    int index(int x, int y) const { return y * m_width + x; }

    int m_width;
    int m_height;
    int m_validCount = 0;
    std::vector<GBufferRecord> m_records;
};

/**
 * Vertex fed to the G-buffer shader. Position is in view space; the vertex
 * shader projects it with the row major projection supplied through
 * GBufferShaderState.
 */
struct GBufferVertex {
    float position[3];     ///< View space position.
    float normal[3];       ///< Unit normal.
    float color[3];        ///< Linear RGB base color.
    float reflectivity;    ///< [0,1].
};

/// Shared context for the G-buffer vertex and pixel shaders.
struct GBufferShaderState {
    /// Row major projection: clip = projection * [position, 1]. clip.w = -z.
    float projection[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, -1, 0};
    ReflectionBuffer *buffer = nullptr;
    /// Optional per-fragment discard predicate; null keeps every fragment.
    bool (*discardFn)(const GBufferFragment &) = nullptr;
};

/**
 * Reusable vertex shader. Position and normal are perspective correct
 * (PVarCount 6); color and reflectivity are affine (AVarCount 4). The window
 * depth z written here is the [0,1] value used by the buffer depth test.
 */
class GBufferVertexShader
    : public swr::VertexShaderBase<GBufferVertexShader> {
public:
    static const int AttribCount = 1;
    static const int AVarCount = 4;
    static const int PVarCount = 6;

    static GBufferShaderState *state;

    static void processVertex(swr::VertexShaderInput in,
                              swr::VertexShaderOutput *out)
    {
        const GBufferVertex &v = *static_cast<const GBufferVertex *>(in[0]);
        const float *m = state->projection;

        const float cx = m[0] * v.position[0] + m[1] * v.position[1] +
                         m[2] * v.position[2] + m[3];
        const float cy = m[4] * v.position[0] + m[5] * v.position[1] +
                         m[6] * v.position[2] + m[7];
        const float cz = m[8] * v.position[0] + m[9] * v.position[1] +
                         m[10] * v.position[2] + m[11];
        const float cw = m[12] * v.position[0] + m[13] * v.position[1] +
                         m[14] * v.position[2] + m[15];

        out->x = cx;
        out->y = cy;
        out->z = cz;
        out->w = cw;

        for (int i = 0; i < 3; ++i) {
            out->pvar[i] = v.position[i];     // perspective corrected position
            out->pvar[3 + i] = v.normal[i];   // perspective corrected normal
            out->avar[i] = v.color[i];
        }
        out->avar[3] = v.reflectivity;
    }
};

/**
 * Reusable pixel shader. The interpolated normal is normalized AFTER
 * interpolation; only fragments that survive the discard predicate and the
 * buffer depth test are committed together with their geometry.
 */
class GBufferPixelShader
    : public swr::PixelShaderBase<GBufferPixelShader> {
public:
    static const int InterpolateZ = true;
    static const int InterpolateW = true;
    static const int AVarCount = 4;
    static const int PVarCount = 6;

    static GBufferShaderState *state;

    static void drawPixel(const swr::PixelData &p)
    {
        GBufferFragment f;
        f.x = p.x;
        f.y = p.y;
        f.depth01 = p.z;
        for (int i = 0; i < 3; ++i) {
            f.position[i] = p.pvar[i];
            f.normal[i] = p.pvar[3 + i];
            f.color[i] = p.avar[i];
        }
        f.reflectivity = p.avar[3];

        // Normalize after interpolation.
        const float len = std::sqrt(
            f.normal[0] * f.normal[0] +
            f.normal[1] * f.normal[1] +
            f.normal[2] * f.normal[2]);
        if (len > 0.0f) {
            f.normal[0] /= len;
            f.normal[1] /= len;
            f.normal[2] /= len;
        }

        if (state->discardFn && state->discardFn(f))
            return; // discarded fragments never reach the buffer

        state->buffer->submit(f);
    }
};

/**
 * Convenience entry point wiring the reusable shaders to a VertexProcessor
 * and Rasterizer. The viewport and scissor are set to the buffer rectangle.
 * Triangles are drawn through the existing clipper and rasterizer.
 */
void renderGBuffer(swr::Rasterizer &rasterizer,
                   swr::VertexProcessor &processor,
                   GBufferShaderState &state,
                   const Viewport &viewport,
                   const std::vector<GBufferVertex> &vertices,
                   const std::vector<int> &indices,
                   swr::RasterMode mode = swr::RasterMode::Span);

} // namespace ssr
