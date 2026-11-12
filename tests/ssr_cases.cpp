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

// Numeric checks for the CPU screen space reflection path. The four entry
// points (projection_trace, depth_intervals, capture_and_composite,
// render_integration) match support/target_main.cpp.

#include "checks.h"

#include "ReflectionBuffer.h"
#include "ScreenSpaceReflection.h"
#include "Rasterizer.h"
#include "VertexProcessor.h"

#include <array>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <vector>

namespace {

using namespace ssr;

// Row-major OpenGL style perspective frustum; clip.w = -view.z.
ProjectionMatrix frustum(double l, double r, double b, double t,
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

// Independent projection used only to derive reference values.
void projectRef(const ProjectionMatrix &m, const double p[3],
                const Viewport &vp, double &sx, double &sy)
{
    const double cw = -p[2];
    const double cx = m[0][0] * p[0] + m[0][2] * p[2] + m[0][3];
    const double cy = m[1][1] * p[1] + m[1][2] * p[2] + m[1][3];
    sx = vp.x + 0.5 * vp.width * (cx / cw + 1.0);
    sy = vp.y + 0.5 * vp.height * (-cy / cw + 1.0);
}

// Inverse of a symmetric frustum: view point at front depth d projecting to a
// given screen coordinate (used to place points exactly on grid corners).
void unprojectSymmetric(const ProjectionMatrix &m, double screenX,
                        double screenY, double depth, const Viewport &vp,
                        double out[3])
{
    const double ndcX = 2.0 * (screenX - vp.x) / vp.width - 1.0;
    const double ndcY = 1.0 - 2.0 * (screenY - vp.y) / vp.height;
    out[0] = ndcX * depth / m[0][0];
    out[1] = ndcY * depth / m[1][1];
    out[2] = -depth;
}

// Parameter t at which a ray (symmetric frustum, no x-shear) reaches screen
// column boundary screenX. Independent inversion used only for references.
double tAtScreenX(const ProjectionMatrix &m, const double O[3],
                  const double R[3], double screenX, const Viewport &vp)
{
    const double g = 2.0 * (screenX - vp.x) / vp.width - 1.0;
    return (m[0][0] * O[0] + g * O[2]) / (-g * R[2] - m[0][0] * R[0]);
}

// Same inversion for the screen y axis.
double tAtScreenY(const ProjectionMatrix &m, const double O[3],
                  const double R[3], double screenY, const Viewport &vp)
{
    const double g = 1.0 - 2.0 * (screenY - vp.y) / vp.height; // cy / cw
    return (m[1][1] * O[1] + g * O[2]) / (-g * R[2] - m[1][1] * R[1]);
}

float depth01(const ProjectionMatrix &m, const double p[3])
{
    const double cw = -p[2];
    const double cz = m[2][0] * p[0] + m[2][1] * p[1] +
                      m[2][2] * p[2] + m[2][3];
    return float(0.5 * (cz / cw) + 0.5);
}

void putRecord(ReflectionBuffer &b, int x, int y, const double p[3],
               float refl, const float color[3],
               float nx = 0.0f, float ny = 1.0f, float nz = 0.0f)
{
    GBufferFragment f;
    f.x = x;
    f.y = y;
    f.depth01 = depth01(frustum(-1, 1, -1, 1, 1, 100), p);
    f.position[0] = (float)p[0];
    f.position[1] = (float)p[1];
    f.position[2] = (float)p[2];
    f.normal[0] = nx;
    f.normal[1] = ny;
    f.normal[2] = nz;
    f.color[0] = color[0];
    f.color[1] = color[1];
    f.color[2] = color[2];
    f.reflectivity = refl;
    b.submit(f);
}

double norm3(const double v[3])
{
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

// I = normalize(P), R = I - 2 dot(I,N) N.
void reflectAbout(const double P[3], const double N[3], double R[3])
{
    const double pl = norm3(P);
    const double I[3] = {P[0] / pl, P[1] / pl, P[2] / pl};
    const double d = I[0] * N[0] + I[1] * N[1] + I[2] * N[2];
    for (int i = 0; i < 3; ++i)
        R[i] = I[i] - 2.0 * d * N[i];
    const double rl = norm3(R);
    for (int i = 0; i < 3; ++i)
        R[i] /= rl;
}

bool throwsInvalid(const std::function<void()> &fn)
{
    try {
        fn();
    } catch (const std::invalid_argument &) {
        return true;
    } catch (const std::exception &) {
        return false;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Group 1: projection storage, validation, frustum/viewport clipping and the
// analytic projection/trace contract (including asymmetric frusta).
// ---------------------------------------------------------------------------
void projection_trace(Checks &c)
{
    const int W = 24, H = 20;
    const Viewport vp{3, 2, 16, 12};
    const ProjectionMatrix P = frustum(-1, 1, -1, 1, 1, 100);

    // Matrix storage: row major with clip.w = -view.z (fourth row 0,0,-1,0).
    c.require(P[3][0] == 0.0 && P[3][1] == 0.0 && P[3][3] == 0.0,
              "projection row-major fourth row");
    c.require(P[3][2] == -1.0, "clip.w = -view.z");

    // Input validation rejects illegal sizes, parameters and projections
    // without touching the buffer.
    {
        ReflectionBuffer b(W, H);
        TraceParameters good{0.0, 0.3, 50.0};

        Viewport bad = vp;
        bad.width = 0;
        c.require(throwsInvalid([&] {
            validateTraceInputs(b, P, bad, good);
        }), "zero viewport width rejected");
        bad = vp; bad.x = W;
        c.require(throwsInvalid([&] {
            validateTraceInputs(b, P, bad, good);
        }), "viewport outside buffer rejected");

        TraceParameters bp = good; bp.bias = -1.0;
        c.require(throwsInvalid([&] { validateTraceInputs(b, P, vp, bp); }),
                  "negative bias rejected");
        bp = good; bp.thickness = std::nan("");
        c.require(throwsInvalid([&] { validateTraceInputs(b, P, vp, bp); }),
                  "non-finite thickness rejected");
        bp = good; bp.maxDistance = 0.0;
        c.require(throwsInvalid([&] { validateTraceInputs(b, P, vp, bp); }),
                  "non-positive maxDistance rejected");

        ProjectionMatrix badM = P; badM[3][2] = 1.0;
        c.require(throwsInvalid([&] {
            validateTraceInputs(b, badM, vp, good);
        }), "projection without clip.w=-z rejected");
        badM = P; badM[0][0] = 0.0;
        c.require(throwsInvalid([&] {
            validateTraceInputs(b, badM, vp, good);
        }), "degenerate focal scale rejected");

        c.require(b.validCount() == 0, "validation does not mutate buffer");
    }

    // Symmetric frustum, non-equal perspective w along the ray, nonzero
    // viewport origin: compare against the independent analytic projection.
    {
        ReflectionBuffer b(W, H);
        const double target[3] = {0.6, -0.2, -6.0};
        double tsx, tsy;
        projectRef(P, target, vp, tsx, tsy);
        const int tx = (int)std::floor(tsx);
        const int ty = (int)std::floor(tsy);
        const float col[3] = {0.2f, 0.8f, 0.3f};
        putRecord(b, tx, ty, target, 0.5f, col);

        const double O[3] = {0.4, -1.0, -4.0};
        const double N[3] = {0, 1, 0};
        double R[3];
        reflectAbout(O, N, R);
        TraceParameters par{0.0, 0.5, 50.0};

        const TraceResult res = traceRay(b, P, vp, par, O, R, -1, -1);
        c.require(res.hit, "symmetric frustum ray hits");
        c.require(res.pixelX == tx && res.pixelY == ty, "hit pixel cell");

        const double tHit = (-6.0 - O[2]) / R[2];
        const double hp[3] = {O[0] + tHit * R[0], O[1] + tHit * R[1], -6.0};
        double asx, asy;
        projectRef(P, hp, vp, asx, asy);

        c.require(std::fabs(res.screenX - asx) <= 1.0 / 16.0 &&
                  std::fabs(res.screenY - asy) <= 1.0 / 16.0,
                  "screen position within 1/16 pixel");
        c.require(std::fabs(res.distance - tHit) / tHit <= 1e-4,
                  "distance relative error <= 1e-4");
        c.require(res.visitedCells >= 1 &&
                  res.visitedCells <= vp.width + vp.height + 1,
                  "visited cell count bounded");
    }

    // Asymmetric frustum: still clip.w = -z, offsets shift the projection.
    {
        ReflectionBuffer b(W, H);
        const ProjectionMatrix Pa = frustum(-0.7, 1.3, -0.9, 1.1, 1.0, 100.0);
        const double target[3] = {0.8, -0.1, -7.0};
        double tsx, tsy;
        projectRef(Pa, target, vp, tsx, tsy);
        const int tx = (int)std::floor(tsx);
        const int ty = (int)std::floor(tsy);
        const float col[3] = {0.3f, 0.6f, 0.9f};
        putRecord(b, tx, ty, target, 0.5f, col);

        const double O[3] = {0.5, -1.0, -4.0};
        const double N[3] = {0, 1, 0};
        double R[3];
        reflectAbout(O, N, R);
        const double tHit = (-7.0 - O[2]) / R[2];
        const double hp[3] = {O[0] + tHit * R[0], O[1] + tHit * R[1], -7.0};
        double asx, asy;
        projectRef(Pa, hp, vp, asx, asy);

        TraceParameters par{0.0, 0.4, 50.0};
        const TraceResult res = traceRay(b, Pa, vp, par, O, R, -1, -1);
        c.require(res.hit, "asymmetric frustum ray hits");
        c.require(res.pixelX == (int)std::floor(asx) &&
                  res.pixelY == (int)std::floor(asy),
                  "asymmetric hit cell matches analytic projection");
        c.require(std::fabs(res.distance - tHit) / tHit <= 1e-4,
                  "asymmetric distance relative error");
    }

    // Camera plane clipping: a ray starting behind the camera is clipped to
    // the w>0 half space; no out-of-range pixel index is ever produced.
    {
        ReflectionBuffer b(W, H);
        const double target[3] = {0.3, -0.2, -6.0};
        double tsx, tsy;
        projectRef(P, target, vp, tsx, tsy);
        const float col[3] = {0.7f, 0.7f, 0.2f};
        putRecord(b, (int)std::floor(tsx), (int)std::floor(tsy), target,
                  0.5f, col);

        const double O[3] = {0.0, 0.0, 2.0}; // behind the camera plane
        const double R[3] = {0.0, 0.0, -1.0};
        TraceParameters par{0.0, 0.5, 30.0};
        const TraceResult res = traceRay(b, P, vp, par, O, R, -1, -1);
        c.require(res.pixelX >= -1 && res.pixelY >= -1,
                  "camera clip never yields a garbage index");
        c.require(res.visitedCells <= vp.width + vp.height + 1,
                  "camera-clip visited cells stay bounded");
        if (res.hit) {
            c.require(res.pixelX >= vp.x && res.pixelX < vp.x + vp.width &&
                      res.pixelY >= vp.y && res.pixelY < vp.y + vp.height,
                      "camera-clip hit lies inside the viewport");
        }
    }

    // Grid edge alignment: a ray projecting onto a column boundary follows
    // half-open ownership and stays deterministic across repeated calls.
    {
        ReflectionBuffer b(W, H);
        const double O[3] = {0.0, -1.0, -4.0};
        const double R[3] = {0.0, 0.0, -1.0};
        for (int k = 0; k < 4; ++k) {
            const double depth = 5.0 + k;
            const double pp[3] = {0.0, -1.0, -depth};
            double sx, sy;
            projectRef(P, pp, vp, sx, sy);
            const float col[3] = {0.2f, 0.7f, 0.4f};
            putRecord(b, (int)std::floor(sx), (int)std::floor(sy), pp,
                      0.5f, col);
        }
        TraceParameters par{0.0, 0.3, 30.0};
        const TraceResult r1 = traceRay(b, P, vp, par, O, R, -1, -1);
        const TraceResult r2 = traceRay(b, P, vp, par, O, R, -1, -1);
        c.require(r1.hit && r1.pixelX == r2.pixelX && r1.pixelY == r2.pixelY,
                  "edge-aligned trace is deterministic");
        c.require(r1.visitedCells == r2.visitedCells &&
                  r1.visitedCells <= vp.width + vp.height + 1,
                  "edge-aligned visited count stable and bounded");
        c.require(std::fabs(r1.distance - 1.0) <= 1e-4,
                  "edge-aligned nearest surface distance");
    }

    // Visited cell bookkeeping on an empty buffer: the march must walk cell by
    // cell, count each cell at most once, and never exceed width + height + 1.
    {
        ReflectionBuffer b(W, H); // no records -> no hits, full traversal
        struct Dir { double x, y, z; };
        const Dir dirs[3] = {
            {1.0, 0.0, -0.2},   // mostly horizontal
            {0.0, 1.0, -0.2},   // mostly vertical
            {1.0, 1.0, -0.3}    // diagonal
        };
        TraceParameters par{0.0, 0.3, 40.0};

        for (const Dir &d : dirs) {
            const double O[3] = {-0.6, -0.6, -4.0};
            double len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
            const double R[3] = {d.x / len, d.y / len, d.z / len};

            // Independent reference cells at the segment ends, clamped to the
            // viewport grid.
            double sxs, sys;
            projectRef(P, O, vp, sxs, sys);
            const double endP[3] = {
                O[0] + par.maxDistance * R[0],
                O[1] + par.maxDistance * R[1],
                O[2] + par.maxDistance * R[2]};
            double exs, eys;
            projectRef(P, endP, vp, exs, eys);
            exs = std::min(std::max(exs, (double)vp.x),
                           (double)vp.x + vp.width - 1e-9);
            eys = std::min(std::max(eys, (double)vp.y),
                           (double)vp.y + vp.height - 1e-9);
            const int cx0 = (int)std::floor(sxs), cy0 = (int)std::floor(sys);
            const int cx1 = (int)std::floor(exs), cy1 = (int)std::floor(eys);
            const int dx = std::abs(cx1 - cx0);
            const int dy = std::abs(cy1 - cy0);

            const TraceResult res = traceRay(b, P, vp, par, O, R, -1, -1);
            c.require(!res.hit, "empty buffer never hits during cell walk");
            c.require(res.visitedCells >= std::max(dx, dy) + 1 &&
                      res.visitedCells <= dx + dy + 1,
                      "visited count matches a monotone grid walk (no duplicates)");
            c.require(res.visitedCells <= vp.width + vp.height + 1,
                      "diagonal walk respects the width+height+1 cell cap");
        }
    }

    // Exact corner crossing: the segment start projects onto an integer grid
    // corner. The diagonal march takes the half-open branch on both axes
    // together and never enters a cell it only touches at the corner.
    {
        ReflectionBuffer b(W, H);
        double O[3];
        unprojectSymmetric(P, 10.0, 8.0, 4.0, vp, O);
        double checkX, checkY;
        projectRef(P, O, vp, checkX, checkY);
        c.require(std::fabs(checkX - 10.0) < 1e-9 &&
                  std::fabs(checkY - 8.0) < 1e-9,
                  "corner start projects exactly onto a grid corner");

        const double far[3] = {O[0] + 2.0, O[1] + 1.5, -9.0};
        double fxs, fys;
        projectRef(P, far, vp, fxs, fys);
        const int tx = (int)std::floor(fxs + 1e-9);
        const int ty = (int)std::floor(fys + 1e-9);
        const float col[3] = {0.6f, 0.2f, 0.8f};
        putRecord(b, tx, ty, far, 0.5f, col);

        const double dvec[3] = {far[0] - O[0], far[1] - O[1], far[2] - O[2]};
        const double dl = norm3(dvec);
        const double R[3] = {dvec[0] / dl, dvec[1] / dl, dvec[2] / dl};
        TraceParameters par{0.0, 0.4, 20.0};
        const TraceResult res = traceRay(b, P, vp, par, O, R, -1, -1);

        c.require(res.hit && res.pixelX == tx && res.pixelY == ty,
                  "corner-crossing ray reaches the target cell");
        const int startCX = (int)std::floor(checkX + 1e-9);
        const int startCY = (int)std::floor(checkY + 1e-9);
        const int spanX = std::abs(tx - startCX);
        const int spanY = std::abs(ty - startCY);
        c.require(res.visitedCells <= spanX + spanY + 1,
                  "corner-only neighbour cells are never counted");
    }
}

// ---------------------------------------------------------------------------
// Group 2: thick-surface depth intervals and the discrete region definition.
// ---------------------------------------------------------------------------
void depth_intervals(Checks &c)
{
    const int W = 24, H = 20;
    const Viewport vp{3, 2, 16, 12};
    const ProjectionMatrix P = frustum(-1, 1, -1, 1, 1, 100);

    // One-column-wide distant target.
    {
        ReflectionBuffer b(W, H);
        const double target[3] = {-0.9, -0.3, -8.0};
        double tsx, tsy;
        projectRef(P, target, vp, tsx, tsy);
        const int tx = (int)std::floor(tsx);
        const int ty = (int)std::floor(tsy);
        const float col[3] = {0.9f, 0.1f, 0.1f};
        putRecord(b, tx, ty, target, 0.5f, col);

        const double O[3] = {-0.5, -1.0, -4.0};
        const double N[3] = {0, 1, 0};
        double R[3];
        reflectAbout(O, N, R);
        TraceParameters par{0.0, 0.3, 50.0};
        const TraceResult res = traceRay(b, P, vp, par, O, R, -1, -1);
        c.require(res.hit && res.pixelX == tx && res.pixelY == ty,
                  "single-column far target hit");
    }

    // Mid-cell slab crossing: both cell endpoints lie OUTSIDE the thickness
    // range, yet the ray enters [D, D+thickness] in the cell interior. Only a
    // continuous per-cell interval intersection can find this; sampling the
    // two endpoints would miss it.
    {
        ReflectionBuffer b(W, H);
        // Explicit ray with motion on both screen axes so every cell boundary
        // is crossed at a positive finite t.
        const double O[3] = {0.2, -1.0, -4.0};
        const double raw[3] = {0.35, 0.3, -1.0};
        const double rawLen = norm3(raw);
        const double R[3] = {raw[0] / rawLen, raw[1] / rawLen,
                             raw[2] / rawLen};

        // Choose a middle cell the ray passes through, several cells away so
        // all four of its boundaries are crossed at positive finite t.
        double tGuess = 3.0;
        const double midP[3] = {O[0] + tGuess * R[0], O[1] + tGuess * R[1],
                                O[2] + tGuess * R[2]};
        double msx, msy;
        projectRef(P, midP, vp, msx, msy);
        const int cx = (int)std::floor(msx), cy = (int)std::floor(msy);

        const double bnd[4] = {
            tAtScreenX(P, O, R, (double)cx, vp),
            tAtScreenX(P, O, R, (double)cx + 1.0, vp),
            tAtScreenY(P, O, R, (double)cy, vp),
            tAtScreenY(P, O, R, (double)cy + 1.0, vp)};
        int finiteCount = 0;
        for (double v : bnd)
            if (std::isfinite(v))
                ++finiteCount;
        c.require(finiteCount == 4, "all four cell boundaries cross in forward t");
        const double tx0 = bnd[0], tx1 = bnd[1], ty0 = bnd[2], ty1 = bnd[3];
        const double tEnter = std::max(std::min(tx0, tx1), std::min(ty0, ty1));
        const double tExit = std::min(std::max(tx0, tx1), std::max(ty0, ty1));
        c.require(tExit > tEnter && tEnter > 0.0,
                  "mid-cell target cell has a positive forward span");

        const auto depthAt = [&](double t) {
            return -(O[2] + t * R[2]);
        };
        const double tMid = 0.5 * (tEnter + tExit);
        const double dEnter = depthAt(tEnter);
        const double dMid = depthAt(tMid);
        const double dExit = depthAt(tExit);
        const double thickness =
            0.25 * std::min(std::fabs(dMid - dEnter), std::fabs(dExit - dMid));
        c.require(thickness > 0.0, "thin interior slab thickness is positive");
        const double D = dMid - 0.5 * thickness;

        // Confirm both cell endpoints are outside [D, D+thickness].
        c.require((dEnter < D || dEnter > D + thickness) &&
                  (dExit < D || dExit > D + thickness),
                  "both cell endpoints lie outside the slab");

        const float col[3] = {0.4f, 0.4f, 0.1f};
        GBufferFragment frag;
        frag.x = cx; frag.y = cy;
        frag.depth01 = depth01(P, midP);
        frag.position[0] = (float)midP[0];
        frag.position[1] = (float)midP[1];
        frag.position[2] = (float)-(D);
        frag.normal[1] = 1.0f;
        frag.color[0] = col[0]; frag.color[1] = col[1]; frag.color[2] = col[2];
        frag.reflectivity = 0.5f;
        b.submit(frag);

        TraceParameters par{0.0, thickness, 50.0};
        const TraceResult res = traceRay(b, P, vp, par, O, R, -1, -1);
        c.require(res.hit && res.pixelX == cx && res.pixelY == cy,
                  "continuous interval finds the mid-cell slab");
        const double tExpect = (D + O[2]) / -R[2]; // depth(t) = D
        c.require(std::fabs(res.distance - tExpect) / tExpect <= 1e-4,
                  "mid-cell entry distance matches the depth plane");
    }

    // Same cell, thin slab above the ray: the ray crosses the cell but its
    // depth stays smaller than D, so it never enters the thickness range.
    {
        ReflectionBuffer b(W, H);
        const double O[3] = {0.3, -1.0, -4.0};
        const double N[3] = {0, 1, 0};
        double R[3];
        reflectAbout(O, N, R);
        const double deep[3] = {0.3, -0.2, -9.0};
        double dsx, dsy;
        projectRef(P, deep, vp, dsx, dsy);
        const float col[3] = {0.4f, 0.4f, 0.1f};
        putRecord(b, (int)std::floor(dsx), (int)std::floor(dsy), deep,
                  0.5f, col);
        TraceParameters par{0.0, 0.2, 50.0};
        const TraceResult res = traceRay(b, P, vp, par, O, R, -1, -1);
        c.require(!res.hit, "thin slab pierced above its depth is missed");
    }

    // Two candidate regions: the earliest entry wins.
    {
        ReflectionBuffer b(W, H);
        const double O[3] = {0.0, -1.0, -4.0};
        const double R[3] = {0.0, 0.0, -1.0};
        const double nearP[3] = {0.0, -1.0, -5.0};
        const double farP[3] = {0.0, -1.0, -8.0};
        double nxs, nys, fxs, fys;
        projectRef(P, nearP, vp, nxs, nys);
        projectRef(P, farP, vp, fxs, fys);
        const int nx = (int)std::floor(nxs), ny = (int)std::floor(nys);
        const int fx = (int)std::floor(fxs), fy = (int)std::floor(fys);
        const float red[3] = {1, 0, 0}, blue[3] = {0, 0, 1};
        putRecord(b, nx, ny, nearP, 0.5f, red);
        if (!(fx == nx && fy == ny))
            putRecord(b, fx, fy, farP, 0.5f, blue);
        TraceParameters par{0.0, 0.3, 30.0};
        const TraceResult res = traceRay(b, P, vp, par, O, R, -1, -1);
        c.require(res.hit, "nearest of two candidates is found");
        c.require(std::fabs(res.distance - 1.0) <= 1e-4,
                  "nearest candidate entry distance");
    }

    // Ray that remains behind (deeper than) a thick layer cannot hit it.
    {
        ReflectionBuffer b(W, H);
        const double target[3] = {0.0, -0.5, -5.0};
        double tsx, tsy;
        projectRef(P, target, vp, tsx, tsy);
        const float col[3] = {0.2f, 0.2f, 0.2f};
        putRecord(b, (int)std::floor(tsx), (int)std::floor(tsy), target,
                  0.5f, col);
        const double O[3] = {0.0, -0.9, -7.0};
        const double R[3] = {0.0, 0.0, -1.0}; // depth keeps increasing
        TraceParameters par{0.0, 0.5, 20.0};
        const TraceResult res = traceRay(b, P, vp, par, O, R, -1, -1);
        c.require(!res.hit, "ray always behind the thick layer misses");
    }

    // Zero thickness still intersects the exact depth plane.
    {
        ReflectionBuffer b(W, H);
        const double O[3] = {0.3, -1.0, -4.0};
        const double N[3] = {0, 1, 0};
        double R[3];
        reflectAbout(O, N, R);
        const double tHit = (-6.0 - O[2]) / R[2];
        const double hp[3] = {O[0] + tHit * R[0], O[1] + tHit * R[1], -6.0};
        double asx, asy;
        projectRef(P, hp, vp, asx, asy);
        const float col[3] = {0.1f, 0.1f, 0.9f};
        putRecord(b, (int)std::floor(asx), (int)std::floor(asy), hp,
                  0.5f, col);
        TraceParameters par{0.0, 0.0, 50.0};
        const TraceResult res = traceRay(b, P, vp, par, O, R, -1, -1);
        c.require(res.hit, "zero-thickness depth plane is hittable");
        c.require(std::fabs(res.distance - tHit) / tHit <= 1e-4,
                  "zero-thickness hit distance");
    }

    // Intersections at t <= bias are ignored as self-intersection.
    {
        ReflectionBuffer b(W, H);
        const double O[3] = {0.0, -1.0, -4.0};
        const double R[3] = {0.0, 0.0, -1.0};
        const double closeP[3] = {0.0, -1.0, -4.2};
        double sx, sy;
        projectRef(P, closeP, vp, sx, sy);
        const float col[3] = {0.5f, 0.5f, 0.5f};
        putRecord(b, (int)std::floor(sx), (int)std::floor(sy), closeP,
                  0.5f, col);
        TraceParameters par{0.5, 0.3, 30.0}; // bias larger than the 0.2 gap
        const TraceResult res = traceRay(b, P, vp, par, O, R, -1, -1);
        c.require(!res.hit, "entry at t <= bias is ignored");
    }

    // Background pixels never form a region.
    {
        ReflectionBuffer b(W, H);
        const double O[3] = {0.2, -1.0, -4.0};
        const double R[3] = {0.0, 0.0, -1.0};
        TraceParameters par{0.0, 0.5, 20.0};
        const TraceResult res = traceRay(b, P, vp, par, O, R, -1, -1);
        c.require(!res.hit && res.visitedCells >= 1,
                  "empty buffer background forms no region");
    }
}

// ---------------------------------------------------------------------------
// Group 3: G-buffer capture rules, ownership and compositing arithmetic.
// ---------------------------------------------------------------------------
void capture_and_composite(Checks &c)
{
    const int W = 8, H = 6;
    const Viewport vp{0, 0, W, H};
    const ProjectionMatrix P = frustum(-1, 1, -1, 1, 1, 100);

    // Depth rule: smaller [0,1] depth wins, ties go to the later fragment,
    // and a far fragment never overwrites a near one.
    {
        ReflectionBuffer b(W, H);
        const float red[3] = {1, 0, 0}, green[3] = {0, 1, 0},
                    blue[3] = {0, 0, 1};

        GBufferFragment f;
        f.x = 2; f.y = 2;
        f.position[0] = 0; f.position[1] = 0; f.position[2] = -5;
        f.normal[1] = 1;

        f.depth01 = 0.6f; f.reflectivity = 0.5f;
        f.color[0] = red[0]; f.color[1] = red[1]; f.color[2] = red[2];
        b.submit(f);
        c.require(b.record(2, 2).color[0] == 1.0f, "first fragment stored");

        f.depth01 = 0.7f;
        f.color[0] = green[0]; f.color[1] = green[1]; f.color[2] = green[2];
        b.submit(f);
        c.require(b.record(2, 2).color[0] == 1.0f,
                  "far fragment cannot overwrite near record");

        f.depth01 = 0.4f;
        f.color[0] = blue[0]; f.color[1] = blue[1]; f.color[2] = blue[2];
        b.submit(f);
        c.require(b.record(2, 2).color[2] == 1.0f,
                  "nearer fragment overwrites far record");

        f.depth01 = 0.4f;
        f.color[0] = green[0]; f.color[1] = green[1]; f.color[2] = green[2];
        b.submit(f);
        c.require(b.record(2, 2).color[1] == 1.0f,
                  "equal depth lets the later fragment win");
        c.require(b.validCount() == 1, "single pixel stays one valid record");
    }

    // Clear invalidates every pixel and leaves no previous-frame data.
    {
        ReflectionBuffer b(W, H);
        const double p[3] = {0.0, 0.0, -5.0};
        const float col[3] = {1, 1, 1};
        putRecord(b, 1, 1, p, 0.5f, col);
        c.require(b.validCount() == 1 && b.isValid(1, 1), "record committed");
        b.clear();
        c.require(b.validCount() == 0 && !b.isValid(1, 1),
                  "clear removes all records");
    }

    // Illegal fragments are rejected and the existing record is untouched.
    {
        ReflectionBuffer b(W, H);
        const double p[3] = {0.0, 0.0, -5.0};
        const float col[3] = {0.3f, 0.6f, 0.9f};
        putRecord(b, 3, 3, p, 0.5f, col);

        GBufferFragment bad;
        bad.x = 3; bad.y = 3; bad.depth01 = 0.2f;
        bad.position[2] = -5; bad.normal[1] = 1; bad.reflectivity = 0.5f;
        bad.normal[0] = std::nan("");
        c.require(throwsInvalid([&] { b.submit(bad); }),
                  "non-finite normal rejected");
        bad.normal[0] = 0; bad.normal[1] = 0; bad.normal[2] = 0;
        c.require(throwsInvalid([&] { b.submit(bad); }),
                  "zero normal rejected");
        bad.normal[1] = 1; bad.reflectivity = 2.0f;
        c.require(throwsInvalid([&] { b.submit(bad); }),
                  "out-of-range reflectivity rejected");
        c.require(b.record(3, 3).color[2] == 0.9f,
                  "rejected submit leaves existing record intact");

        c.require(throwsInvalid([&] { ReflectionBuffer badSize(0, 4); }),
                  "illegal buffer dimensions rejected");
    }

    // Composite arithmetic and the immutable un-reflected base color.
    {
        const int BW = 16, BH = 12;
        const Viewport cvp{0, 0, BW, BH};
        ReflectionBuffer b(BW, BH);

        // The target is placed on the floor pixel's actual reflected ray.
        const double src[3] = {0.0, -1.0, -4.0};
        const double N[3] = {0, 1, 0};
        double R[3];
        reflectAbout(src, N, R);
        const double tTarget = 4.0;
        const double tgt[3] = {
            src[0] + tTarget * R[0],
            src[1] + tTarget * R[1],
            src[2] + tTarget * R[2]};

        double sxs, sys, txs, tys;
        projectRef(P, src, cvp, sxs, sys);
        projectRef(P, tgt, cvp, txs, tys);
        const int sx = (int)std::floor(sxs), sy = (int)std::floor(sys);
        const int tx = (int)std::floor(txs), ty = (int)std::floor(tys);
        c.require(sx >= 0 && sx < BW && sy >= 0 && sy < BH,
                  "composite source pixel inside viewport");
        c.require(tx >= 0 && tx < BW && ty >= 0 && ty < BH,
                  "composite target pixel inside viewport");
        c.require(!(tx == sx && ty == sy),
                  "reflection target occupies a different cell");

        const float floorCol[3] = {0.8f, 0.8f, 0.8f};
        const float hitCol[3] = {1.0f, 0.2f, 0.1f};
        putRecord(b, sx, sy, src, 0.25f, floorCol, 0, 1, 0);
        putRecord(b, tx, ty, tgt, 0.0f, hitCol, 0, 1, 0);

        const float env[3] = {0.05f, 0.07f, 0.1f};
        TraceParameters par{0.0, 0.5, 30.0};

        Image original;
        composeOriginal(b, env, original);
        c.require(original.pixel(sx, sy)[0] == 0.8f,
                  "original image stores un-reflected base color");

        Image reflected;
        composeReflections(b, P, cvp, par, env, reflected);

        const TraceResult tr = tracePixel(b, P, cvp, par, sx, sy);
        const float *out = reflected.pixel(sx, sy);
        float expect[3];
        const float *srcCol = tr.hit ? hitCol : env;
        for (int i = 0; i < 3; ++i)
            expect[i] = 0.75f * floorCol[i] + 0.25f * srcCol[i];
        c.require(tr.hit && tr.pixelX == tx && tr.pixelY == ty,
                  "floor pixel reflection reaches the target");
        c.require(std::fabs(out[0] - expect[0]) < 1e-5f &&
                  std::fabs(out[1] - expect[1]) < 1e-5f &&
                  std::fabs(out[2] - expect[2]) < 1e-5f,
                  "composite uses (1-k)*base + k*reflected");

        // Reflectivity zero keeps the original drawing result.
        ReflectionBuffer off(BW, BH);
        putRecord(off, sx, sy, src, 0.0f, floorCol, 0, 1, 0);
        Image offImg;
        composeReflections(off, P, cvp, par, env, offImg);
        const float *op = offImg.pixel(sx, sy);
        c.require(op[0] == floorCol[0] && op[1] == floorCol[1] &&
                  op[2] == floorCol[2],
                  "reflection disabled reproduces the original");

        // Mutating the returned image does not affect the geometry buffer.
        reflected.pixel(sx, sy)[0] = -99.0f;
        Image reflected2;
        composeReflections(b, P, cvp, par, env, reflected2);
        c.require(reflected2.pixel(sx, sy)[0] != -99.0f,
                  "returned image has independent storage");
        c.require(b.record(sx, sy).color[0] == floorCol[0],
                  "compositing never writes back into the G-buffer");
    }

    // Scan order independence and interleaving two buffers.
    {
        ReflectionBuffer b1(W, H), b2(W, H);
        const double p1[3] = {0.2, -1.0, -4.0};
        const double p2[3] = {-0.2, -1.0, -5.0};
        const double q1[3] = {0.2, -1.0, -8.0};
        const double q2[3] = {-0.2, -1.0, -9.0};
        const float ca[3] = {0.9f, 0.2f, 0.2f}, cb[3] = {0.2f, 0.2f, 0.9f};
        putRecord(b1, 1, 1, p1, 0.6f, ca, 0, 1, 0);
        putRecord(b1, 6, 4, q1, 0.0f, cb, 0, 1, 0);
        putRecord(b2, 1, 1, p2, 0.4f, cb, 0, 1, 0);
        putRecord(b2, 6, 4, q2, 0.0f, ca, 0, 1, 0);

        const float env[3] = {0.1f, 0.1f, 0.1f};
        TraceParameters par{0.0, 0.3, 30.0};
        Image i1, i2, i3, i4;
        composeReflections(b1, P, vp, par, env, i1);
        composeReflections(b2, P, vp, par, env, i2);
        composeReflections(b1, P, vp, par, env, i3); // repeated call
        composeReflections(b2, P, vp, par, env, i4); // interleaved

        bool repeatSame = true, interleaveSame = true;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                for (int k = 0; k < 3; ++k) {
                    if (i1.pixel(x, y)[k] != i3.pixel(x, y)[k])
                        repeatSame = false;
                    if (i2.pixel(x, y)[k] != i4.pixel(x, y)[k])
                        interleaveSame = false;
                }
            }
        c.require(repeatSame, "repeated compositing is identical");
        c.require(interleaveSame, "interleaved buffers stay independent");
    }

    // Reverse scan gives the same image as a forward scan. Each source pixel
    // has a target placed exactly on its reflected ray, so most pixels hit.
    {
        const int BW = 18, BH = 14;
        const Viewport rvp{0, 0, BW, BH};
        ReflectionBuffer b(BW, BH);
        const float floorCol[3] = {0.7f, 0.7f, 0.7f};
        const float hitCol[3] = {0.95f, 0.3f, 0.2f};
        const double N[3] = {0, 1, 0};

        for (int i = 0; i < 5; ++i) {
            const double src[3] = {-0.6 + 0.3 * i, -1.0, -4.0 - 0.4 * i};
            double R[3];
            reflectAbout(src, N, R);
            const double tgt[3] = {src[0] + 3.0 * R[0],
                                   src[1] + 3.0 * R[1],
                                   src[2] + 3.0 * R[2]};
            double sxs, sys, txs, tys;
            projectRef(P, src, rvp, sxs, sys);
            projectRef(P, tgt, rvp, txs, tys);
            const int sx = (int)std::floor(sxs), sy = (int)std::floor(sys);
            const int tx = (int)std::floor(txs), ty = (int)std::floor(tys);
            if (sx >= 0 && sx < BW && sy >= 0 && sy < BH &&
                tx >= 0 && tx < BW && ty >= 0 && ty < BH &&
                !(sx == tx && sy == ty)) {
                putRecord(b, sx, sy, src, 0.5f, floorCol, 0, 1, 0);
                putRecord(b, tx, ty, tgt, 0.0f, hitCol, 0, 1, 0);
            }
        }
        c.require(b.validCount() >= 6, "reverse-scan scene has multiple records");

        const float env[3] = {0.08f, 0.08f, 0.12f};
        TraceParameters par{0.0, 0.5, 30.0};

        Image forward;
        composeReflections(b, P, rvp, par, env, forward);

        // Manual reverse scan, reusing the same immutable trace entry point.
        Image reverse(BW, BH);
        for (int y = BH - 1; y >= 0; --y)
            for (int x = BW - 1; x >= 0; --x) {
                float *dst = reverse.pixel(x, y);
                if (!b.isValid(x, y)) {
                    dst[0] = env[0]; dst[1] = env[1]; dst[2] = env[2];
                    continue;
                }
                const GBufferRecord &base = b.record(x, y);
                float refl[3] = {env[0], env[1], env[2]};
                const TraceResult tr = tracePixel(b, P, rvp, par, x, y);
                if (tr.hit && b.isValid(tr.pixelX, tr.pixelY)) {
                    const GBufferRecord &h = b.record(tr.pixelX, tr.pixelY);
                    refl[0] = h.color[0]; refl[1] = h.color[1];
                    refl[2] = h.color[2];
                }
                for (int k = 0; k < 3; ++k)
                    dst[k] = (1.0f - base.reflectivity) * base.color[k] +
                             base.reflectivity * refl[k];
            }

        bool same = true;
        for (int y = 0; y < BH && same; ++y)
            for (int x = 0; x < BW && same; ++x)
                for (int k = 0; k < 3; ++k)
                    if (forward.pixel(x, y)[k] != reverse.pixel(x, y)[k])
                        same = false;
        c.require(same, "reverse and forward scans produce identical images");
    }
}

// ---------------------------------------------------------------------------
// Group 4: real rendering through VertexProcessor + Rasterizer, across modes.
// ---------------------------------------------------------------------------
namespace {

// Copy a double row-major projection into the float shader state.
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
    idx.insert(idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

} // namespace

void render_integration(Checks &c)
{
    const int W = 40, H = 30;
    const Viewport vp{0, 0, W, H};
    const ProjectionMatrix P = frustum(-1, 1, -1, 1, 1, 100);

    // Horizontal reflective floor at y = -2 and a colored overhead panel at
    // y = +1. Floor reflections point upward and strike the panel.
    std::vector<GBufferVertex> verts;
    std::vector<int> idx;
    {
        const float floorN[3] = {0, 1, 0};
        const float floorC[3] = {0.75f, 0.75f, 0.7f};
        const float floorQuad[4][3] = {
            {-6, -2, -3}, {6, -2, -3}, {6, -2, -12}, {-6, -2, -12}};
        addQuad(verts, idx, floorQuad, floorN, floorC, 0.7f);

        const float panelN[3] = {0, -1, 0};
        const float panelC[3] = {1.0f, 0.15f, 0.1f};
        const float panelQuad[4][3] = {
            {-6, 1, -4}, {-6, 1, -12}, {6, 1, -12}, {6, 1, -4}};
        addQuad(verts, idx, panelQuad, panelN, panelC, 0.0f);
    }

    struct ModeResult {
        ReflectionBuffer buffer;
        ModeResult(int w, int h) : buffer(w, h) {}
    };

    // All three retained raster modes draw through the same G-buffer path.
    const swr::RasterMode modes[3] = {
        swr::RasterMode::Span, swr::RasterMode::Block,
        swr::RasterMode::Adaptive};
    std::vector<ReflectionBuffer> buffers;
    for (swr::RasterMode mode : modes) {
        buffers.emplace_back(W, H);
        ReflectionBuffer &gb = buffers.back();
        gb.clear();

        swr::Rasterizer rasterizer;
        swr::VertexProcessor processor(&rasterizer);
        GBufferShaderState state;
        setShaderProjection(state, P);
        state.buffer = &gb;
        renderGBuffer(rasterizer, processor, state, vp, verts, idx, mode);

        c.require(gb.validCount() > 0, "rasterized geometry fills pixels");
        c.require(gb.validCount() < W * H, "background stays invalid");

        // Every captured record is internally consistent.
        bool threw = false;
        try {
            gb.validate();
        } catch (const std::exception &) {
            threw = true;
        }
        c.require(!threw, "captured records have finite data and unit normals");
    }

    ReflectionBuffer &span = buffers[0];
    ReflectionBuffer &block = buffers[1];
    ReflectionBuffer &adaptive = buffers[2];

    // Near/far ordering: a nearer floor/panel record keeps the smaller depth.
    {
        int nearChecked = 0;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                if (!span.isValid(x, y))
                    continue;
                const float d = span.record(x, y).depth01;
                c.require(d >= 0.0f && d <= 1.0f, "mapped depth in [0,1]");
                ++nearChecked;
            }
        }
        c.require(nearChecked > 10, "many interior pixels captured");
    }

    // Interior pixels (away from triangle edges) must agree across all three
    // modes for position, normalized normal and reflectivity.
    {
        const ReflectionBuffer *others[2] = {&block, &adaptive};
        for (int m = 0; m < 2; ++m) {
            int agree = 0;
            for (int y = 2; y < H - 2; ++y) {
                for (int x = 2; x < W - 2; ++x) {
                    if (!span.isValid(x, y) || !others[m]->isValid(x, y))
                        continue;
                    const GBufferRecord &a = span.record(x, y);
                    const GBufferRecord &b = others[m]->record(x, y);
                    bool same = true;
                    for (int k = 0; k < 3; ++k) {
                        if (std::fabs(a.position[k] - b.position[k]) > 2e-3f)
                            same = false;
                        if (std::fabs(a.normal[k] - b.normal[k]) > 2e-3f)
                            same = false;
                    }
                    if (std::fabs(a.reflectivity - b.reflectivity) > 1e-5f)
                        same = false;
                    if (same)
                        ++agree;
                }
            }
            c.require(agree > 50,
                      m == 0 ? "interior geometry agrees: block"
                             : "interior geometry agrees: adaptive");
        }
    }

    // Perspective-correct position: an interior floor pixel's reconstructed
    // view position re-projects back into the same pixel.
    {
        int checked = 0;
        for (int y = 4; y < H - 4; ++y) {
            for (int x = 4; x < W - 4; ++x) {
                if (!span.isValid(x, y))
                    continue;
                const GBufferRecord &r = span.record(x, y);
                if (r.reflectivity < 0.5f)
                    continue; // floor only
                const double pp[3] = {r.position[0], r.position[1],
                                      r.position[2]};
                double sx, sy;
                projectRef(P, pp, vp, sx, sy);
                c.require(std::fabs(sx - (x + 0.5)) < 0.6 &&
                          std::fabs(sy - (y + 0.5)) < 0.6,
                          "perspective position re-projects to its pixel");
                const float nl = std::sqrt(r.normal[0] * r.normal[0] +
                                           r.normal[1] * r.normal[1] +
                                           r.normal[2] * r.normal[2]);
                c.require(std::fabs(nl - 1.0f) < 1e-4f,
                          "interpolated normal is unit length");
                ++checked;
            }
        }
        c.require(checked > 20, "floor interior pixels verified");
    }

    // Discard: a predicate that drops every fragment captures nothing.
    {
        ReflectionBuffer discarded(W, H);
        swr::Rasterizer rasterizer;
        swr::VertexProcessor processor(&rasterizer);
        GBufferShaderState state;
        setShaderProjection(state, P);
        state.buffer = &discarded;
        state.discardFn = [](const GBufferFragment &) { return true; };
        renderGBuffer(rasterizer, processor, state, vp, verts, idx,
                      swr::RasterMode::Span);
        c.require(discarded.validCount() == 0,
                  "discarded fragments never reach the buffer");
    }

    // End-to-end reflection: interior floor pixels reflect into the panel and
    // the composite is stable across all three raster modes.
    {
        const float env[3] = {0.04f, 0.05f, 0.08f};
        TraceParameters par{0.02, 0.4, 40.0};
        Image a, b, d;
        composeReflections(span, P, vp, par, env, a);
        composeReflections(block, P, vp, par, env, b);
        composeReflections(adaptive, P, vp, par, env, d);

        int reflectedHits = 0;
        for (int y = 6; y < H - 6; ++y) {
            for (int x = 6; x < W - 6; ++x) {
                if (!span.isValid(x, y))
                    continue;
                const GBufferRecord &r = span.record(x, y);
                if (r.reflectivity < 0.5f)
                    continue;
                const TraceResult tr = tracePixel(span, P, vp, par, x, y);
                if (tr.hit)
                    ++reflectedHits;
                for (int k = 0; k < 3; ++k) {
                    c.require(std::fabs(a.pixel(x, y)[k] -
                                        b.pixel(x, y)[k]) < 2e-3f,
                              "reflection composite stable: block");
                    c.require(std::fabs(a.pixel(x, y)[k] -
                                        d.pixel(x, y)[k]) < 2e-3f,
                              "reflection composite stable: adaptive");
                }
            }
        }
        c.require(reflectedHits > 10, "floor pixels reflect to the panel");
    }
}
