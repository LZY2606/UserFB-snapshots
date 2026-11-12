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

#include "ReflectionBuffer.h"
#include "ScreenSpaceReflection.h"

#include <cmath>
#include <stdexcept>

namespace ssr {

GBufferShaderState *GBufferVertexShader::state = nullptr;
GBufferShaderState *GBufferPixelShader::state = nullptr;


namespace {

bool finite3(const float v[3])
{
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

} // namespace

ReflectionBuffer::ReflectionBuffer(int width, int height)
{
    if (width <= 0 || height <= 0)
        throw std::invalid_argument("ReflectionBuffer: dimensions must be positive");
    if (!std::isfinite((float)width) || !std::isfinite((float)height))
        throw std::invalid_argument("ReflectionBuffer: non-finite dimensions");

    m_width = width;
    m_height = height;
    m_records.assign((size_t)width * (size_t)height, GBufferRecord{});
}

void ReflectionBuffer::clear()
{
    for (GBufferRecord &r : m_records)
        r = GBufferRecord{};
    m_validCount = 0;
}

void ReflectionBuffer::submit(const GBufferFragment &fragment)
{
    if (fragment.x < 0 || fragment.x >= m_width || fragment.y < 0 || fragment.y >= m_height)
        throw std::invalid_argument("ReflectionBuffer::submit: pixel outside buffer");

    if (!std::isfinite(fragment.depth01) ||
        !finite3(fragment.position) ||
        !finite3(fragment.normal) ||
        !finite3(fragment.color) ||
        !std::isfinite(fragment.reflectivity))
        throw std::invalid_argument("ReflectionBuffer::submit: non-finite fragment data");

    if (fragment.depth01 < 0.0f || fragment.depth01 > 1.0f)
        throw std::invalid_argument("ReflectionBuffer::submit: depth outside [0,1]");
    if (fragment.reflectivity < 0.0f || fragment.reflectivity > 1.0f)
        throw std::invalid_argument("ReflectionBuffer::submit: reflectivity outside [0,1]");

    const float nlen = std::sqrt(
        fragment.normal[0] * fragment.normal[0] +
        fragment.normal[1] * fragment.normal[1] +
        fragment.normal[2] * fragment.normal[2]);
    if (!std::isfinite(nlen) || nlen == 0.0f)
        throw std::invalid_argument("ReflectionBuffer::submit: zero length normal");

    const int idx = index(fragment.x, fragment.y);
    GBufferRecord &target = m_records[idx];

    // Depth test: smaller mapped depth wins; an exact tie goes to the later
    // fragment. A far surface can never replace an existing near record.
    if (target.valid && fragment.depth01 > target.depth01)
        return;

    const bool wasValid = target.valid;

    target.valid = true;
    target.depth01 = fragment.depth01;
    for (int i = 0; i < 3; ++i) {
        target.position[i] = fragment.position[i];
        target.normal[i] = fragment.normal[i] / nlen;
        target.color[i] = fragment.color[i];
    }
    target.reflectivity = fragment.reflectivity;

    if (!wasValid)
        ++m_validCount;
}

bool ReflectionBuffer::isValid(int x, int y) const
{
    if (x < 0 || x >= m_width || y < 0 || y >= m_height)
        return false;
    return m_records[index(x, y)].valid;
}

const GBufferRecord &ReflectionBuffer::record(int x, int y) const
{
    if (x < 0 || x >= m_width || y < 0 || y >= m_height)
        throw std::out_of_range("ReflectionBuffer::record: pixel outside buffer");
    return m_records[index(x, y)];
}

float ReflectionBuffer::viewDepth(int x, int y) const
{
    const GBufferRecord &r = record(x, y);
    if (!r.valid)
        throw std::runtime_error("ReflectionBuffer::viewDepth: pixel is not valid");
    return -r.position[2];
}

void ReflectionBuffer::validate() const
{
    for (int y = 0; y < m_height; ++y) {
        for (int x = 0; x < m_width; ++x) {
            const GBufferRecord &r = m_records[index(x, y)];
            if (!r.valid)
                continue;

            if (!finite3(r.position) || !finite3(r.normal) || !finite3(r.color) ||
                !std::isfinite(r.reflectivity) || !std::isfinite(r.depth01))
                throw std::runtime_error("ReflectionBuffer::validate: non-finite record");

            const float nlen = std::sqrt(
                r.normal[0] * r.normal[0] +
                r.normal[1] * r.normal[1] +
                r.normal[2] * r.normal[2]);
            if (nlen == 0.0f || !std::isfinite(nlen))
                throw std::runtime_error("ReflectionBuffer::validate: zero normal");
            if (std::fabs(nlen - 1.0f) > 1e-4f)
                throw std::runtime_error("ReflectionBuffer::validate: normal not unit length");

            if (r.position[2] >= 0.0f)
                throw std::runtime_error("ReflectionBuffer::validate: view depth must be positive");
            if (r.reflectivity < 0.0f || r.reflectivity > 1.0f)
                throw std::runtime_error("ReflectionBuffer::validate: reflectivity outside [0,1]");
            if (r.depth01 < 0.0f || r.depth01 > 1.0f)
                throw std::runtime_error("ReflectionBuffer::validate: depth outside [0,1]");
        }
    }
}

void renderGBuffer(swr::Rasterizer &rasterizer,
                   swr::VertexProcessor &processor,
                   GBufferShaderState &state,
                   const Viewport &viewport,
                   const std::vector<GBufferVertex> &vertices,
                   const std::vector<int> &indices,
                   swr::RasterMode mode)
{
    if (state.buffer == nullptr)
        throw std::invalid_argument("renderGBuffer: shader state has no buffer");
    if (viewport.width <= 0 || viewport.height <= 0)
        throw std::invalid_argument("renderGBuffer: viewport dimensions must be positive");
    if (indices.empty() || indices.size() % 3 != 0)
        throw std::invalid_argument("renderGBuffer: indices must describe whole triangles");

    GBufferVertexShader::state = &state;
    GBufferPixelShader::state = &state;

    rasterizer.setRasterMode(mode);
    rasterizer.setScissorRect(viewport.x, viewport.y, viewport.width, viewport.height);
    rasterizer.setPixelShader<GBufferPixelShader>();

    processor.setRasterizer(&rasterizer);
    processor.setViewport(viewport.x, viewport.y, viewport.width, viewport.height);
    processor.setDepthRange(0.0f, 1.0f);
    processor.setCullMode(swr::CullMode::None);
    processor.setVertexShader<GBufferVertexShader>();
    processor.setVertexAttribPointer(0, sizeof(GBufferVertex), vertices.data());

    // drawElements takes a non-const int pointer; the existing API does not
    // mutate the contents, so a const_cast over the local copy is sufficient.
    std::vector<int> local = indices;
    processor.drawElements(swr::DrawMode::Triangle, local.size(), local.data());
}

} // namespace ssr
