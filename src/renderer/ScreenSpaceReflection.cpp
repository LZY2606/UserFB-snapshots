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

#include "ScreenSpaceReflection.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace ssr {

namespace {

constexpr double kEps = 1e-12;
constexpr double kCameraEps = 1e-9;

struct ClipVec {
    double x, y, z, w;
};

ClipVec applyProjection(const ProjectionMatrix &m, const double p[3])
{
    ClipVec c;
    c.x = m[0][0] * p[0] + m[0][1] * p[1] + m[0][2] * p[2] + m[0][3];
    c.y = m[1][0] * p[0] + m[1][1] * p[1] + m[1][2] * p[2] + m[1][3];
    c.z = m[2][0] * p[0] + m[2][1] * p[1] + m[2][2] * p[2] + m[2][3];
    c.w = m[3][0] * p[0] + m[3][1] * p[1] + m[3][2] * p[2] + m[3][3];
    return c;
}

// Clip space coefficients of the ray O + t R: component(t) = v0 + v1 * t.
struct RayClip {
    double cx0, cx1, cy0, cy1, cz0, cz1, cw0, cw1;
};

RayClip rayClipCoefficients(const ProjectionMatrix &m,
                            const double o[3], const double r[3])
{
    ClipVec a = applyProjection(m, o);
    double p[3] = {o[0] + r[0], o[1] + r[1], o[2] + r[2]};
    ClipVec b = applyProjection(m, p);
    RayClip rc;
    rc.cx0 = a.x; rc.cx1 = b.x - a.x;
    rc.cy0 = a.y; rc.cy1 = b.y - a.y;
    rc.cz0 = a.z; rc.cz1 = b.z - a.z;
    rc.cw0 = a.w; rc.cw1 = b.w - a.w;
    return rc;
}

// Slab clipping of a linear clip-space ray against the six frustum planes and
// the w > 0 camera half space. Returns false when the ray never enters.
bool clipToFrustum(const RayClip &rc, double maxDistance, double &tLo, double &tHi)
{
    // Each entry is a + b t <= 0.
    struct Slab { double a, b; };
    const Slab slabs[] = {
        {rc.cx0 - rc.cw0, rc.cx1 - rc.cw1}, // x <= w
        {-rc.cx0 - rc.cw0, -rc.cx1 - rc.cw1}, // -w <= x
        {rc.cy0 - rc.cw0, rc.cy1 - rc.cw1}, // y <= w
        {-rc.cy0 - rc.cw0, -rc.cy1 - rc.cw1}, // -w <= y
        {rc.cz0 - rc.cw0, rc.cz1 - rc.cw1}, // z <= w
        {-rc.cz0 - rc.cw0, -rc.cz1 - rc.cw1}, // -w <= z
        {-rc.cw0, -rc.cw1} // w >= 0
    };

    tLo = 0.0;
    tHi = maxDistance;

    for (const Slab &s : slabs) {
        if (std::fabs(s.b) < kEps) {
            if (s.a > 0.0)
                return false;
            continue;
        }
        const double t = -s.a / s.b;
        if (s.b > 0.0)
            tHi = std::min(tHi, t);
        else
            tLo = std::max(tLo, t);
        if (tLo > tHi + kEps)
            return false;
    }

    if (tLo > tHi)
        return false;

    // Keep a strictly positive clip.w (= -view.z) so projection is defined.
    if (rc.cw1 > kEps) {
        tLo = std::max(tLo, (kCameraEps - rc.cw0) / rc.cw1);
    } else if (rc.cw0 <= kCameraEps) {
        return false;
    }

    return tLo <= tHi + kEps && tHi >= 0.0;
}

struct ScreenPoint {
    double x, y, w;
};

bool projectAt(const RayClip &rc, const Viewport &vp, double t, ScreenPoint &out)
{
    const double cw = rc.cw0 + rc.cw1 * t;
    if (!(cw > kCameraEps))
        return false;
    const double cx = rc.cx0 + rc.cx1 * t;
    const double cy = rc.cy0 + rc.cy1 * t;
    out.w = cw;
    out.x = vp.x + vp.width * 0.5 * (cx / cw + 1.0);
    out.y = vp.y + vp.height * 0.5 * (-cy / cw + 1.0);
    return true;
}

// Parameter t at which screen x reaches integer column k.
double columnCrossing(const RayClip &rc, const Viewport &vp, int k)
{
    const double ndc = 2.0 * (k - vp.x) / vp.width - 1.0; // cx / cw at boundary
    const double denom = rc.cx1 - ndc * rc.cw1;
    const double num = -(rc.cx0 - ndc * rc.cw0);
    if (std::fabs(denom) < kEps)
        return std::numeric_limits<double>::infinity();
    return num / denom;
}

// Parameter t at which screen y reaches integer row k.
double rowCrossing(const RayClip &rc, const Viewport &vp, int k)
{
    const double ndc = 1.0 - 2.0 * (k - vp.y) / vp.height; // cy / cw at boundary
    const double denom = rc.cy1 - ndc * rc.cw1;
    const double num = -(rc.cy0 - ndc * rc.cw0);
    if (std::fabs(denom) < kEps)
        return std::numeric_limits<double>::infinity();
    return num / denom;
}

// Intersect the linear view-depth d(t) = -(Oz + t Rz) with
// [D, D + thickness] over the cell interval [tA, tB]. Returns the entry
// parameter on a real overlap.
bool slabEntry(const double origin[3], const double reflection[3],
               double D, double thickness,
               double tA, double tB, double &entry)
{
    const double d0 = -origin[2];
    const double d1 = -reflection[2]; // d(t) = d0 + d1 t

    double lo, hi;
    if (std::fabs(d1) < kEps) {
        if (d0 < D - kEps || d0 > D + thickness + kEps)
            return false;
        lo = tA;
        hi = tB;
    } else {
        const double tD = (D - d0) / d1;
        const double tT = (D + thickness - d0) / d1;
        lo = std::min(tD, tT);
        hi = std::max(tD, tT);
        lo = std::max(lo, tA);
        hi = std::min(hi, tB);
    }

    if (lo > hi + kEps)
        return false;
    entry = std::max(lo, tA);
    return true;
}

TraceResult march(const ReflectionBuffer &buffer,
                  const ProjectionMatrix &projection,
                  const Viewport &vp,
                  const TraceParameters &params,
                  const double origin[3],
                  const double reflection[3],
                  int selfX,
                  int selfY)
{
    TraceResult result{};

    RayClip rc = rayClipCoefficients(projection, origin, reflection);

    double tLo, tHi;
    if (!clipToFrustum(rc, params.maxDistance, tLo, tHi))
        return result;
    tLo = std::max(tLo, 0.0);
    if (tLo > tHi)
        return result;

    ScreenPoint start;
    if (!projectAt(rc, vp, tLo, start))
        return result;

    // Screen movement signs are constant along a projective (Möbius) path.
    // Screen x follows cx/cw directly; screen y is flipped (y grows down),
    // hence the negated y sign.
    const double sxNum = rc.cx1 * rc.cw0 - rc.cx0 * rc.cw1;
    const double syNum = rc.cy1 * rc.cw0 - rc.cy0 * rc.cw1;
    const int stepX = (sxNum > kEps) - (sxNum < -kEps);
    const int stepY = -((syNum > kEps) - (syNum < -kEps));

    int cellX = (int)std::floor(start.x);
    int cellY = (int)std::floor(start.y);

    // Integer boundary index ahead of the current cell on each moving axis.
    int boundX = cellX + (stepX > 0 ? 1 : 0);
    int boundY = cellY + (stepY > 0 ? 1 : 0);

    // A screen segment crosses at most width + height internal boundaries, so
    // it can occupy at most width + height + 1 distinct cells.
    const int maxSteps = vp.width + vp.height + 1;
    double tCur = tLo;

    for (int step = 0; step < maxSteps && tCur <= tHi + kEps; ++step) {
        const bool inside = cellX >= vp.x && cellX < vp.x + vp.width &&
                            cellY >= vp.y && cellY < vp.y + vp.height;

        double tx = std::numeric_limits<double>::infinity();
        double ty = std::numeric_limits<double>::infinity();
        if (stepX != 0)
            tx = columnCrossing(rc, vp, boundX);
        if (stepY != 0)
            ty = rowCrossing(rc, vp, boundY);

        double tExit = std::min(tHi, std::min(tx, ty));
        if (!(tExit >= tCur))
            tExit = tCur;

        // Only count cells occupied for a positive parameter span.
        const bool occupied = inside && tExit > tCur + kEps;
        if (occupied)
            ++result.visitedCells;

        if (occupied && !(cellX == selfX && cellY == selfY) &&
            buffer.isValid(cellX, cellY)) {
            const double D = buffer.viewDepth(cellX, cellY);
            double entry;
            if (slabEntry(origin, reflection, D, params.thickness, tCur, tExit, entry) &&
                entry > params.bias) {
                ScreenPoint hit;
                if (projectAt(rc, vp, entry, hit)) {
                    result.hit = true;
                    result.pixelX = cellX;
                    result.pixelY = cellY;
                    result.distance = entry;
                    result.screenX = hit.x;
                    result.screenY = hit.y;
                    // Move an open-boundary entry at most 1e-6 px into the cell.
                    if (hit.x <= cellX + 1e-7)
                        result.screenX = cellX + 1e-6;
                    else if (hit.x >= cellX + 1.0 - 1e-7)
                        result.screenX = cellX + 1.0 - 1e-6;
                    if (hit.y <= cellY + 1e-7)
                        result.screenY = cellY + 1e-6;
                    else if (hit.y >= cellY + 1.0 - 1e-7)
                        result.screenY = cellY + 1.0 - 1e-6;
                    return result;
                }
            }
        }

        if (tExit >= tHi - kEps)
            break;

        // Half-open convention: on a shared crossing instant both axes move
        // together, so a corner-only neighbour cell is never entered or tested.
        const bool crossX = stepX != 0 && std::fabs(tx - tExit) <= kEps;
        const bool crossY = stepY != 0 && std::fabs(ty - tExit) <= kEps;
        if (crossX) {
            cellX += stepX;
            boundX += stepX;
        }
        if (crossY) {
            cellY += stepY;
            boundY += stepY;
        }
        tCur = tExit;
    }

    return result;
}

} // namespace

Image::Image(int width, int height)
{
    resize(width, height);
}

void Image::resize(int width, int height)
{
    if (width <= 0 || height <= 0)
        throw std::invalid_argument("Image: dimensions must be positive");
    m_width = width;
    m_height = height;
    m_pixels.assign((size_t)width * (size_t)height * 3, 0.0f);
}

void Image::fill(const float rgb[3])
{
    for (int y = 0; y < m_height; ++y)
        for (int x = 0; x < m_width; ++x) {
            float *p = pixel(x, y);
            p[0] = rgb[0];
            p[1] = rgb[1];
            p[2] = rgb[2];
        }
}

void validateTraceInputs(const ReflectionBuffer &buffer,
                         const ProjectionMatrix &projection,
                         const Viewport &viewport,
                         const TraceParameters &parameters)
{
    if (viewport.width <= 0 || viewport.height <= 0)
        throw std::invalid_argument("SSR: viewport dimensions must be positive");
    if (viewport.x < 0 || viewport.y < 0 ||
        viewport.x + viewport.width > buffer.width() ||
        viewport.y + viewport.height > buffer.height())
        throw std::invalid_argument("SSR: viewport lies outside the buffer");

    if (!std::isfinite(parameters.bias) || parameters.bias < 0.0)
        throw std::invalid_argument("SSR: bias must be a non-negative finite value");
    if (!std::isfinite(parameters.thickness) || parameters.thickness < 0.0)
        throw std::invalid_argument("SSR: thickness must be a non-negative finite value");
    if (!std::isfinite(parameters.maxDistance) || parameters.maxDistance <= 0.0)
        throw std::invalid_argument("SSR: maxDistance must be a positive finite value");

    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            if (!std::isfinite(projection[r][c]))
                throw std::invalid_argument("SSR: projection contains non-finite entries");

    // Conventional perspective: fourth row (0, 0, -1, 0) giving clip.w = -z.
    if (std::fabs(projection[3][0]) > 1e-9 ||
        std::fabs(projection[3][1]) > 1e-9 ||
        std::fabs(projection[3][2] + 1.0) > 1e-9 ||
        std::fabs(projection[3][3]) > 1e-9)
        throw std::invalid_argument("SSR: projection must use clip.w = -view.z");
    if (projection[0][0] == 0.0 || projection[1][1] == 0.0)
        throw std::invalid_argument("SSR: projection has a degenerate focal scale");

    buffer.validate();
}

TraceResult traceRay(const ReflectionBuffer &buffer,
                     const ProjectionMatrix &projection,
                     const Viewport &viewport,
                     const TraceParameters &parameters,
                     const double origin[3],
                     const double reflection[3],
                     int selfPixelX,
                     int selfPixelY)
{
    validateTraceInputs(buffer, projection, viewport, parameters);

    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(origin[i]) || !std::isfinite(reflection[i]))
            throw std::invalid_argument("SSR: ray contains non-finite data");
    }
    const double rlen = std::sqrt(
        reflection[0] * reflection[0] +
        reflection[1] * reflection[1] +
        reflection[2] * reflection[2]);
    if (rlen == 0.0 || !std::isfinite(rlen))
        throw std::invalid_argument("SSR: reflection direction must be unit length and non-zero");

    double r[3] = {reflection[0] / rlen, reflection[1] / rlen, reflection[2] / rlen};
    return march(buffer, projection, viewport, parameters, origin, r,
                 selfPixelX, selfPixelY);
}

// Build the reflection ray for a valid source pixel and march it. Assumes the
// caller has already validated the inputs and the source pixel.
static TraceResult tracePixelValidated(const ReflectionBuffer &buffer,
                                       const ProjectionMatrix &projection,
                                       const Viewport &viewport,
                                       const TraceParameters &parameters,
                                       int sourcePixelX,
                                       int sourcePixelY)
{
    const GBufferRecord &rec = buffer.record(sourcePixelX, sourcePixelY);

    double P[3] = {rec.position[0], rec.position[1], rec.position[2]};
    double N[3] = {rec.normal[0], rec.normal[1], rec.normal[2]};

    const double plen = std::sqrt(P[0] * P[0] + P[1] * P[1] + P[2] * P[2]);
    double I[3] = {P[0] / plen, P[1] / plen, P[2] / plen};

    const double dot = I[0] * N[0] + I[1] * N[1] + I[2] * N[2];
    double R[3] = {
        I[0] - 2.0 * dot * N[0],
        I[1] - 2.0 * dot * N[1],
        I[2] - 2.0 * dot * N[2]};
    const double rlen = std::sqrt(R[0] * R[0] + R[1] * R[1] + R[2] * R[2]);
    R[0] /= rlen; R[1] /= rlen; R[2] /= rlen;

    double O[3] = {
        P[0] + parameters.bias * N[0],
        P[1] + parameters.bias * N[1],
        P[2] + parameters.bias * N[2]};

    return march(buffer, projection, viewport, parameters, O, R,
                 sourcePixelX, sourcePixelY);
}

TraceResult tracePixel(const ReflectionBuffer &buffer,
                       const ProjectionMatrix &projection,
                       const Viewport &viewport,
                       const TraceParameters &parameters,
                       int sourcePixelX,
                       int sourcePixelY)
{
    validateTraceInputs(buffer, projection, viewport, parameters);

    if (!buffer.isValid(sourcePixelX, sourcePixelY))
        throw std::invalid_argument("SSR::tracePixel: source pixel is not valid");

    const GBufferRecord &probe = buffer.record(sourcePixelX, sourcePixelY);
    const double plen = std::sqrt(
        probe.position[0] * probe.position[0] +
        probe.position[1] * probe.position[1] +
        probe.position[2] * probe.position[2]);
    if (plen == 0.0 || !std::isfinite(plen))
        throw std::runtime_error("SSR::tracePixel: degenerate view position");

    return tracePixelValidated(buffer, projection, viewport, parameters,
                               sourcePixelX, sourcePixelY);
}

void composeOriginal(const ReflectionBuffer &buffer,
                     const float environment[3],
                     Image &out)
{
    if (out.width() != buffer.width() || out.height() != buffer.height())
        out.resize(buffer.width(), buffer.height());

    for (int y = 0; y < buffer.height(); ++y) {
        for (int x = 0; x < buffer.width(); ++x) {
            float *dst = out.pixel(x, y);
            if (buffer.isValid(x, y)) {
                const GBufferRecord &r = buffer.record(x, y);
                dst[0] = r.color[0];
                dst[1] = r.color[1];
                dst[2] = r.color[2];
            } else {
                dst[0] = environment[0];
                dst[1] = environment[1];
                dst[2] = environment[2];
            }
        }
    }
}

void composeReflections(const ReflectionBuffer &buffer,
                        const ProjectionMatrix &projection,
                        const Viewport &viewport,
                        const TraceParameters &parameters,
                        const float environment[3],
                        Image &out)
{
    validateTraceInputs(buffer, projection, viewport, parameters);

    if (out.width() != buffer.width() || out.height() != buffer.height())
        out.resize(buffer.width(), buffer.height());

    for (int y = 0; y < buffer.height(); ++y) {
        for (int x = 0; x < buffer.width(); ++x) {
            float *dst = out.pixel(x, y);

            if (!buffer.isValid(x, y)) {
                dst[0] = environment[0];
                dst[1] = environment[1];
                dst[2] = environment[2];
                continue;
            }

            const GBufferRecord &base = buffer.record(x, y);
            const float k = base.reflectivity;

            float reflected[3] = {environment[0], environment[1], environment[2]};
            // Inputs were validated once above; reuse the no-revalidation march.
            const TraceResult tr = tracePixelValidated(buffer, projection,
                                                       viewport, parameters, x, y);
            if (tr.hit && buffer.isValid(tr.pixelX, tr.pixelY)) {
                const GBufferRecord &hit = buffer.record(tr.pixelX, tr.pixelY);
                reflected[0] = hit.color[0];
                reflected[1] = hit.color[1];
                reflected[2] = hit.color[2];
            }

            for (int c = 0; c < 3; ++c)
                dst[c] = (1.0f - k) * base.color[c] + k * reflected[c];
        }
    }
}

} // namespace ssr
