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

// Headless reflection preview. Generates a reflective floor and colored
// geometry entirely on the CPU through swr::VertexProcessor + swr::Rasterizer,
// then writes the un-reflected and reflected images as binary P6 PPM files.
//
// Usage: ReflectionPreview <output-directory>
// Writes <output-directory>/original.ppm and .../reflected.ppm.

#include "ReflectionBuffer.h"
#include "ScreenSpaceReflection.h"
#include "Rasterizer.h"
#include "VertexProcessor.h"

#include <cstdint>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ssr;

namespace {

ProjectionMatrix makeFrustum(double l, double r, double b, double t,
                             double n, double f)
{
    ProjectionMatrix m{};
    m[0][0] = 2.0 * n / (r - l);
    m[0][2] = (r + l) / (r - l);
    m[1][1] = 2.0 * n / (t - b);
    m[1][2] = (t + b) / (t - b);
    m[2][2] = -(f + n) / (f - n);
    m[2][3] = -2.0 * f * n / (f - n);
    m[3][2] = -1.0;
    return m;
}

void setShaderProjection(GBufferShaderState &state, const ProjectionMatrix &m)
{
    for (int r = 0; r < 4; ++r)
        for (int col = 0; col < 4; ++col)
            state.projection[r * 4 + col] = (float)m[r][col];
}

void addQuad(std::vector<GBufferVertex> &verts, std::vector<int> &idx,
             const float corners[4][3], const float normal[3],
             const float color[3], float reflectivity)
{
    const int base = (int)verts.size();
    for (int i = 0; i < 4; ++i) {
        GBufferVertex v;
        for (int k = 0; k < 3; ++k) {
            v.position[k] = corners[i][k];
            v.normal[k] = normal[k];
        }
        v.color[0] = color[0];
        v.color[1] = color[1];
        v.color[2] = color[2];
        v.reflectivity = reflectivity;
        verts.push_back(v);
    }
    idx.insert(idx.end(),
               {base, base + 1, base + 2, base, base + 2, base + 3});
}

// Axis aligned box as six outward-facing quads.
void addBox(std::vector<GBufferVertex> &verts, std::vector<int> &idx,
            const float center[3], const float half[3],
            const float color[3], float reflectivity)
{
    const float x0 = center[0] - half[0], x1 = center[0] + half[0];
    const float y0 = center[1] - half[1], y1 = center[1] + half[1];
    const float z0 = center[2] - half[2], z1 = center[2] + half[2];

    const float px[3] = {1, 0, 0}, nx[3] = {-1, 0, 0};
    const float py[3] = {0, 1, 0}, ny[3] = {0, -1, 0};
    const float pz[3] = {0, 0, 1}, nz[3] = {0, 0, -1};

    const float right[4][3] = {{x1, y0, z0}, {x1, y0, z1},
                               {x1, y1, z1}, {x1, y1, z0}};
    const float left[4][3] = {{x0, y0, z1}, {x0, y0, z0},
                              {x0, y1, z0}, {x0, y1, z1}};
    const float top[4][3] = {{x0, y1, z0}, {x1, y1, z0},
                             {x1, y1, z1}, {x0, y1, z1}};
    const float bottom[4][3] = {{x0, y0, z1}, {x1, y0, z1},
                                {x1, y0, z0}, {x0, y0, z0}};
    const float front[4][3] = {{x0, y0, z1}, {x1, y0, z1},
                               {x1, y1, z1}, {x0, y1, z1}};
    const float back[4][3] = {{x1, y0, z0}, {x0, y0, z0},
                              {x0, y1, z0}, {x1, y1, z0}};

    addQuad(verts, idx, right, px, color, reflectivity);
    addQuad(verts, idx, left, nx, color, reflectivity);
    addQuad(verts, idx, top, py, color, reflectivity);
    addQuad(verts, idx, bottom, ny, color, reflectivity);
    addQuad(verts, idx, front, pz, color, reflectivity);
    addQuad(verts, idx, back, nz, color, reflectivity);
}

void writePPM(const std::string &path, const Image &image)
{
    std::ofstream out(path, std::ios::binary);
    if (!out)
        throw std::runtime_error("cannot open output file: " + path);

    out << "P6\n" << image.width() << ' ' << image.height() << "\n255\n";
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const float *px = image.pixel(x, y);
            for (int c = 0; c < 3; ++c) {
                float v = px[c];
                if (v < 0.0f) v = 0.0f;
                if (v > 1.0f) v = 1.0f;
                const auto byte = (std::uint8_t)(v * 255.0f + 0.5f);
                out.put((char)byte);
            }
        }
    }
    if (!out)
        throw std::runtime_error("failed while writing: " + path);
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::cerr << "usage: " << (argc ? argv[0] : "ReflectionPreview")
                  << " <output-directory>\n";
        return 2;
    }

    try {
        const int width = 96;
        const int height = 72;
        const Viewport viewport{0, 0, width, height};
        const ProjectionMatrix projection =
            makeFrustum(-1.0, 1.0, -1.0, 1.0, 1.0, 100.0);

        std::vector<GBufferVertex> vertices;
        std::vector<int> indices;

        // Reflective floor.
        {
            const float normal[3] = {0, 1, 0};
            const float color[3] = {0.55f, 0.55f, 0.52f};
            const float floorQuad[4][3] = {
                {-8, -2, -3}, {8, -2, -3}, {8, -2, -16}, {-8, -2, -16}};
            addQuad(vertices, indices, floorQuad, normal, color, 0.85f);
        }

        // Colored boxes floating above the floor; their faces are reflected.
        {
            const float red[3] = {0.9f, 0.12f, 0.1f};
            const float green[3] = {0.1f, 0.8f, 0.2f};
            const float blue[3] = {0.15f, 0.3f, 0.95f};
            const float yellow[3] = {0.95f, 0.8f, 0.1f};

            const float half[3] = {0.9f, 0.9f, 0.9f};
            const float c1[3] = {-3.2f, 0.6f, -7.0f};
            const float c2[3] = {2.6f, 0.2f, -9.0f};
            const float c3[3] = {0.2f, 1.1f, -12.0f};
            const float c4[3] = {-1.4f, -0.4f, -5.5f};
            addBox(vertices, indices, c1, half, red, 0.0f);
            addBox(vertices, indices, c2, half, green, 0.0f);
            addBox(vertices, indices, c3, half, blue, 0.0f);
            addBox(vertices, indices, c4, half, yellow, 0.0f);
        }

        ReflectionBuffer buffer(width, height);
        buffer.clear();

        swr::Rasterizer rasterizer;
        swr::VertexProcessor processor(&rasterizer);
        GBufferShaderState state;
        setShaderProjection(state, projection);
        state.buffer = &buffer;
        renderGBuffer(rasterizer, processor, state, viewport,
                      vertices, indices, swr::RasterMode::Span);

        if (buffer.validCount() == 0)
            throw std::runtime_error("no geometry was rendered");

        const float environment[3] = {0.03f, 0.04f, 0.07f};
        const TraceParameters trace{0.02, 0.4, 40.0};

        Image original;
        composeOriginal(buffer, environment, original);

        Image reflected;
        composeReflections(buffer, projection, viewport, trace,
                           environment, reflected);

        // Count genuine reflections so the preview is demonstrably reflective.
        int reflectedPixels = 0;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                const float *a = original.pixel(x, y);
                const float *b = reflected.pixel(x, y);
                if (std::fabs(a[0] - b[0]) + std::fabs(a[1] - b[1]) +
                        std::fabs(a[2] - b[2]) >
                    1e-3f)
                    ++reflectedPixels;
            }
        if (reflectedPixels == 0)
            throw std::runtime_error("reflected image is identical to original");

        const std::string dir = argv[1];
        writePPM(dir + "/original.ppm", original);
        writePPM(dir + "/reflected.ppm", reflected);

        std::cout << "ReflectionPreview: " << buffer.validCount()
                  << " g-buffer pixels, " << reflectedPixels
                  << " reflected pixels, wrote original.ppm and reflected.ppm\n";
    } catch (const std::exception &e) {
        std::cerr << "ReflectionPreview failed: " << e.what() << '\n';
        return 1;
    }

    return 0;
}
