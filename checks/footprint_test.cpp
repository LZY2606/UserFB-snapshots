// Windowless regression tests for swr::Texture footprint filtering.
//
// Covers: rotation/basis invariance of the texel-space ellipse, the
// anisotropy cap widening the short-axis filter, non-square textures,
// repeat wrapping and mip clamping, degenerate footprints, rejected
// non-finite input, the perspective derivative path used by the
// rasterizer, and a small side-by-side PPM showing that oblique stripes
// stay continuous when the screen basis rotates.

#include "src/examples/Texture.h"
#include "src/renderer/PixelData.h"
#include "src/renderer/TriangleEquations.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr float PI = 3.14159265358979323846f;

int g_failures = 0;
int g_checks = 0;

void reportFail(const std::string& what) {
    std::cerr << "FAIL: " << what << "\n";
    ++g_failures;
}

void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) reportFail(what);
}

void checkNear(float got, float expected, float tol, const std::string& what) {
    ++g_checks;
    if (!std::isfinite(got) || std::fabs(got - expected) > tol) {
        reportFail(what + " (got=" + std::to_string(got) +
                   " expected=" + std::to_string(expected) + ")");
    }
}

std::string fpDump(const swr::Texture::Footprint& f) {
    return "major=" + std::to_string(f.majorLength) +
           " minor=" + std::to_string(f.minorLength) +
           " effMinor=" + std::to_string(f.effectiveMinor) +
           " lod=" + std::to_string(f.lod) +
           " taps=" + std::to_string(f.tapCount) +
           " dir=(" + std::to_string(f.dirMajorU) + "," +
           std::to_string(f.dirMajorV) + ")";
}

Uint8 chan(Uint32 c, int shift) { return static_cast<Uint8>((c >> shift) & 0xFF); }

void checkColorClose(Uint32 a, Uint32 b, const std::string& what) {
    for (int shift = 0; shift <= 16; shift += 8) {
        ++g_checks;
        int d = std::abs(int(chan(a, shift)) - int(chan(b, shift)));
        if (d > 1) {
            reportFail(what + " channel shift=" + std::to_string(shift) +
                       " differ by " + std::to_string(d) +
                       " (a=0x" + std::to_string(a) + " b=0x" + std::to_string(b) + ")");
        }
    }
}

SDL_Surface* makeStripeSurface(int w, int h) {
    SDL_Surface* s = SDL_CreateRGBSurface(0, w, h, 32, 0xff0000, 0xff00, 0xff, 0);
    if (!s) return nullptr;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            static_cast<Uint32*>(s->pixels)[y * w + x] = (y % 2) ? 0xffffffu : 0u;
        }
    }
    return s;
}

SDL_Surface* makeSolidSurface(int w, int h, Uint8 gray) {
    SDL_Surface* s = SDL_CreateRGBSurface(0, w, h, 32, 0xff0000, 0xff00, 0xff, 0);
    if (!s) return nullptr;
    Uint32 px = (Uint32(gray) << 16) | (Uint32(gray) << 8) | gray;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            static_cast<Uint32*>(s->pixels)[y * w + x] = px;
    return s;
}

struct Derivs {
    float dudx, dvdx, dudy, dvdy;
};

swr::Texture::Footprint query(const swr::Texture& t, const Derivs& d) {
    return t.queryFootprint(0.37f, 0.16f, d.dudx, d.dvdx, d.dudy, d.dvdy);
}

Uint32 sampleAt(const swr::Texture& t, float u, float v, const Derivs& d) {
    Uint32 c = 0;
    t.sample(u, v, d.dudx, d.dvdx, d.dudy, d.dvdy, c);
    return c;
}

// ---------------------------------------------------------------------------
// 1. Rotating the screen basis must not change the texel-space ellipse.
// ---------------------------------------------------------------------------
void testRotationInvariance() {
    std::cout << "[1] rotation / basis invariance\n";
    SDL_Surface* surf = makeStripeSurface(16, 16);
    swr::Texture tex(surf, 8);
    SDL_FreeSurface(surf);

    const float q = std::sqrt(0.5f);
    // All bases span the same texel-space ellipse: long axis 8 texels along
    // u, short axis 1 texel along v. UV derivatives are texels / 16.
    std::vector<Derivs> bases = {
        {8.0f / 16, 0.0f,       0.0f,        1.0f / 16},  // axis aligned
        {8 * q / 16, q / 16,    -8 * q / 16, q / 16},      // rotated 45 degrees
        {8 * q / 16, -q / 16,   8 * q / 16,  q / 16},      // mirrored 45 degrees
        {0.0f,       1.0f / 16, 8.0f / 16,   0.0f},        // swapped screen x/y
        {-8.0f / 16, 0.0f,      0.0f,       -1.0f / 16},  // both axes reversed
    };

    swr::Texture::Footprint ref = query(tex, bases[0]);
    checkNear(ref.majorLength, 8.0f, 1e-4f, "axis-aligned major axis length");
    checkNear(ref.minorLength, 1.0f, 1e-4f, "axis-aligned minor axis length");
    checkNear(ref.effectiveMinor, 1.0f, 1e-5f, "axis-aligned effective minor width");
    checkNear(ref.lod, 0.0f, 1e-5f, "axis-aligned LOD");
    check(ref.tapCount == 8, "axis-aligned tap count is 8");
    check(std::fabs(std::fabs(ref.dirMajorU) - 1.0f) < 1e-5f &&
          std::fabs(ref.dirMajorV) < 1e-5f,
          "axis-aligned major direction follows the ellipse, not a screen axis");

    Uint32 refColor = sampleAt(tex, 0.37f, 0.16f, bases[0]);
    for (size_t i = 1; i < bases.size(); ++i) {
        swr::Texture::Footprint f = query(tex, bases[i]);
        const std::string ctx = "basis " + std::to_string(i);
        checkNear(f.majorLength, ref.majorLength, 1e-5f, ctx + " major length");
        checkNear(f.minorLength, ref.minorLength, 1e-5f, ctx + " minor length");
        checkNear(f.effectiveMinor, ref.effectiveMinor, 1e-5f, ctx + " effective minor");
        checkNear(f.lod, ref.lod, 1e-5f, ctx + " LOD");
        check(f.tapCount == ref.tapCount, ctx + " tap count");
        // Eigen axis stays along u in texel space for every screen basis.
        check(std::fabs(std::fabs(f.dirMajorU) - 1.0f) < 1e-4f &&
              std::fabs(f.dirMajorV) < 1e-4f,
              ctx + " eigen direction is texel-space invariant");

        Uint32 c = sampleAt(tex, 0.37f, 0.16f, bases[i]);
        if (std::abs(int(chan(c, 0)) - int(chan(refColor, 0))) > 1) {
            std::cerr << "  derivatives: " << bases[i].dudx << "," << bases[i].dvdx
                      << " / " << bases[i].dudy << "," << bases[i].dvdy
                      << "  lod=" << f.lod << "\n  ref " << fpDump(ref)
                      << "\n  got " << fpDump(f) << "\n";
        }
        checkColorClose(c, refColor, ctx + " stripe color");
    }

    // Independent color-path checks: zero-footprint point lookups of the
    // stripe texture must hit pure black / white rows.
    Derivs none = {0, 0, 0, 0};
    check(chan(sampleAt(tex, 0.5f, 2.0f / 15.0f, none), 16) == 0,
          "stripe row 2 is black");
    check(chan(sampleAt(tex, 0.5f, 3.0f / 15.0f, none), 16) == 255,
          "stripe row 3 is white");
}

// ---------------------------------------------------------------------------
// 2. An 8:1 footprint is limited to maxAnisotropy:1 by widening the filter.
// ---------------------------------------------------------------------------
void testAnisotropyCap() {
    std::cout << "[2] anisotropy cap widens the short-axis filter\n";
    SDL_Surface* surf = makeStripeSurface(16, 16);
    Derivs d = {8.0f / 16, 0.0f, 0.0f, 1.0f / 16}; // 8 x 1 texels

    {
        swr::Texture tex(surf, 2);
        swr::Texture::Footprint f = query(tex, d);
        checkNear(f.majorLength, 8.0f, 1e-4f, "cap2 major length");
        checkNear(f.minorLength, 1.0f, 1e-4f, "cap2 raw minor length");
        checkNear(f.effectiveMinor, 4.0f, 1e-5f,
                  "cap2 effective minor widens to major/2");
        checkNear(f.lod, 2.0f, 1e-5f, "cap2 LOD follows widened minor");
        check(f.tapCount == 2, "cap2 tap count");
    }
    {
        swr::Texture tex(surf, 8);
        swr::Texture::Footprint f = query(tex, d);
        checkNear(f.effectiveMinor, 1.0f, 1e-5f, "cap8 effective minor");
        checkNear(f.lod, 0.0f, 1e-5f, "cap8 LOD");
        check(f.tapCount == 8, "cap8 tap count");
    }
    {
        swr::Texture tex(surf, 1);
        swr::Texture::Footprint f = query(tex, d);
        checkNear(f.effectiveMinor, 8.0f, 1e-5f, "cap1 effective minor equals major");
        checkNear(f.lod, 3.0f, 1e-5f, "cap1 LOD");
        check(f.tapCount == 1, "cap1 tap count");
    }
    {
        swr::Texture tex(surf, 16);
        swr::Texture::Footprint f = query(tex, d);
        check(f.tapCount == 8, "cap16 still only needs 8 taps here");
        checkNear(f.effectiveMinor, 1.0f, 1e-5f, "cap16 keeps fine mip");
    }

    swr::Texture clampedLow(surf, 0);
    swr::Texture clampedHigh(surf, 99);
    check(clampedLow.maxAnisotropy() == 1, "anisotropy 0 clamps to 1");
    check(clampedHigh.maxAnisotropy() == swr::Texture::MAX_ANISOTROPY,
          "anisotropy 99 clamps to 16");
    SDL_FreeSurface(surf);
}

// ---------------------------------------------------------------------------
// 3. Non-square textures: width and height scale the derivatives separately.
// ---------------------------------------------------------------------------
void testNonSquare() {
    std::cout << "[3] non-square texture\n";
    SDL_Surface* surf = makeStripeSurface(16, 8);
    swr::Texture tex(surf, 8);
    SDL_FreeSurface(surf);

    // 4 texels in u and 4 texels in v: square footprint despite unequal UV.
    Derivs square = {4.0f / 16, 0.0f, 0.0f, 4.0f / 8};
    swr::Texture::Footprint f = query(tex, square);
    checkNear(f.majorLength, 4.0f, 1e-4f, "non-square major texel length");
    checkNear(f.minorLength, 4.0f, 1e-4f, "non-square minor texel length");
    check(f.tapCount == 1, "square footprint uses one tap");

    // Equal UV deltas are not equal texel deltas on a non-square texture.
    Derivs skew = {0.25f, 0.25f, 0.0f, 0.0f}; // 4 u-texels, 2 v-texels
    f = query(tex, skew);
    checkNear(f.majorLength, std::sqrt(20.0f), 1e-4f, "skew major length");
    checkNear(f.minorLength, 0.0f, 1e-4f, "skew is rank one");
    checkNear(f.effectiveMinor, 1.0f, 1e-5f, "rank-one effective minor is 1");
    check(f.tapCount == 5, "skew tap count");
    float dot = f.dirMajorU * (4.0f / std::sqrt(20.0f)) +
                f.dirMajorV * (2.0f / std::sqrt(20.0f));
    check(std::fabs(std::fabs(dot) - 1.0f) < 1e-4f,
          "skew major direction follows texel-space ellipse");
}

// ---------------------------------------------------------------------------
// 4. Wrapping, mip clamping and one-texel-wide textures stay finite.
// ---------------------------------------------------------------------------
void testBoundaries() {
    std::cout << "[4] wrapping, mip clamp, narrow textures\n";
    SDL_Surface* surf = makeStripeSurface(16, 16);
    swr::Texture tex(surf, 8);

    Derivs none = {0, 0, 0, 0};
    Uint32 base = sampleAt(tex, 0.1f, 0.2f, none);
    checkColorClose(sampleAt(tex, 1.1f, 0.2f, none), base, "repeat wrap u=1.1");
    checkColorClose(sampleAt(tex, -0.9f, 0.2f, none), base, "repeat wrap u=-0.9");
    checkColorClose(sampleAt(tex, 0.1f, 2.2f, none),
                    sampleAt(tex, 0.1f, 0.2f, none), "repeat wrap v");

    // Huge footprint: LOD clamps to the last mip level (16 -> 1, level 4).
    Derivs huge = {100.0f, 0.0f, 0.0f, 100.0f};
    swr::Texture::Footprint f = query(tex, huge);
    check(std::isfinite(f.lod) && f.lod == 4.0f, "huge footprint LOD clamps to 4");
    check(std::isfinite(f.effectiveMinor) && f.tapCount >= 1,
          "huge footprint stays finite");
    Uint32 c = sampleAt(tex, 0.37f, 0.16f, huge);
    check(c <= 0xffffff, "huge footprint produces a packed color");
    SDL_FreeSurface(surf);

    // Largest finite derivatives must still produce finite footprint values:
    // the Gram matrix is formed in double so intermediate squares cannot
    // overflow into NaN / inf and corrupt the integer conversions.
    const float fmax = std::numeric_limits<float>::max();
    Derivs extreme = {fmax, 0.0f, 0.0f, fmax};
    swr::Texture::Footprint ef = query(tex, extreme);
    check(std::isfinite(ef.majorLength) && std::isfinite(ef.minorLength) &&
          std::isfinite(ef.effectiveMinor) && std::isfinite(ef.lod),
          "largest finite derivatives keep footprint finite");
    check(ef.tapCount >= 1 && ef.tapCount <= swr::Texture::MAX_ANISOTROPY,
          "largest finite derivatives keep tap count bounded");
    check(sampleAt(tex, 0.37f, 0.16f, extreme) <= 0xffffff,
          "largest finite derivatives produce a packed color");

    // One pixel wide and one pixel tall textures.
    SDL_Surface* narrow = makeStripeSurface(1, 16);
    swr::Texture texNarrow(narrow, 8);
    SDL_FreeSurface(narrow);
    Derivs wide = {5.0f, 0.0f, 0.0f, 2.0f / 16};
    f = query(texNarrow, wide);
    check(std::isfinite(f.majorLength) && std::isfinite(f.minorLength) &&
          std::isfinite(f.lod) && f.tapCount >= 1,
          "1-wide texture footprint is finite");
    check(std::isfinite(float(sampleAt(texNarrow, 0.0f, 0.5f, wide))),
          "1-wide texture sampling is finite");

    SDL_Surface* one = makeSolidSurface(1, 1, 200);
    swr::Texture texOne(one, 4);
    SDL_FreeSurface(one);
    Derivs d = {3.0f, 0.25f, -1.0f, 0.5f};
    f = query(texOne, d);
    check(f.lod == 0.0f && f.tapCount >= 1, "1x1 texture clamps LOD to 0");
    check(chan(sampleAt(texOne, 0.0f, 0.0f, d), 0) == 200,
          "1x1 texture returns its only texel");
}

// ---------------------------------------------------------------------------
// 5. Non-finite UVs or derivatives are rejected before indexing memory.
// ---------------------------------------------------------------------------
void testRejectedInputs() {
    std::cout << "[5] non-finite input is rejected\n";
    SDL_Surface* surf = makeStripeSurface(16, 16);
    swr::Texture tex(surf, 8);
    SDL_FreeSurface(surf);

    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float bad[] = {inf, -inf, nan};

    for (float b : bad) {
        auto expectThrow = [&](const char* name, float u, float v,
                               float dudx, float dvdx, float dudy, float dvdy) {
            bool threw = false;
            try {
                Uint32 c;
                tex.sample(u, v, dudx, dvdx, dudy, dvdy, c);
            } catch (const std::invalid_argument&) {
                threw = true;
            } catch (...) {
                reportFail(std::string("wrong exception type for ") + name);
                threw = true;
            }
            check(threw, std::string("sample rejects ") + name);

            threw = false;
            try {
                tex.queryFootprint(u, v, dudx, dvdx, dudy, dvdy);
            } catch (const std::invalid_argument&) {
                threw = true;
            } catch (...) {
                reportFail(std::string("wrong exception type for query ") + name);
                threw = true;
            }
            check(threw, std::string("queryFootprint rejects ") + name);
        };

        expectThrow("non-finite u", b, 0.1f, 0, 0, 0, 0);
        expectThrow("non-finite v", 0.1f, b, 0, 0, 0, 0);
        expectThrow("non-finite dudx", 0.1f, 0.1f, b, 0, 0, 0);
        expectThrow("non-finite dvdx", 0.1f, 0.1f, 0, b, 0, 0);
        expectThrow("non-finite dudy", 0.1f, 0.1f, 0, 0, b, 0);
        expectThrow("non-finite dvdy", 0.1f, 0.1f, 0, 0, 0, b);
    }
}

// ---------------------------------------------------------------------------
// 6. Degenerate footprints and tap symmetry.
// ---------------------------------------------------------------------------
void testDegenerateFootprints() {
    std::cout << "[6] zero, rank-one and square footprints\n";
    SDL_Surface* surf = makeStripeSurface(16, 16);
    swr::Texture tex(surf, 8);
    SDL_FreeSurface(surf);

    swr::Texture::Footprint f = query(tex, {0, 0, 0, 0});
    check(f.majorLength == 0.0f && f.minorLength == 0.0f,
          "zero derivatives give zero axes");
    check(f.effectiveMinor == 1.0f && f.lod == 0.0f && f.tapCount == 1,
          "zero footprint filters one texel at LOD 0");
    check(std::isfinite(f.dirMajorU) && std::isfinite(f.dirMajorV),
          "zero footprint direction is finite");

    f = query(tex, {0.5f, 0.0f, 0.0f, 0.0f}); // 8 texels, rank one
    checkNear(f.majorLength, 8.0f, 1e-4f, "rank-one major length");
    checkNear(f.minorLength, 0.0f, 1e-5f, "rank-one minor length");
    check(f.tapCount == 8, "rank-one tap count");

    f = query(tex, {0.25f, 0.0f, 0.0f, 0.25f}); // 4 x 4 texels
    checkNear(f.majorLength, 4.0f, 1e-5f, "square major");
    checkNear(f.minorLength, 4.0f, 1e-5f, "square minor");
    check(f.tapCount == 1, "square footprint is isotropic");

    // Reversing direction must not change the result: taps are symmetric.
    Derivs d = {8.0f / 16, 1.0f / 16, -2.0f / 16, 1.0f / 16};
    Derivs neg = {-d.dudx, -d.dvdx, -d.dudy, -d.dvdy};
    checkColorClose(sampleAt(tex, 0.37f, 0.16f, d),
                    sampleAt(tex, 0.37f, 0.16f, neg),
                    "negated derivatives");
    checkColorClose(sampleAt(tex, 0.37f, 0.16f, d),
                    sampleAt(tex, 0.37f, 0.16f, {d.dudy, d.dvdy, d.dudx, d.dvdx}),
                    "swapped screen axes");
}

// ---------------------------------------------------------------------------
// 7. A solid texture is invariant under every footprint, and the input
//    surface is never modified.
// ---------------------------------------------------------------------------
void testSolidAndSurfaceUntouched() {
    std::cout << "[7] solid texture and surface immutability\n";
    SDL_Surface* surf = makeSolidSurface(8, 8, 100);
    std::vector<Uint32> before(static_cast<Uint32*>(surf->pixels),
                               static_cast<Uint32*>(surf->pixels) + 64);
    swr::Texture tex(surf, 8);

    const Derivs cases[] = {
        {0, 0, 0, 0},
        {0.5f, 0.1f, -0.2f, 0.3f},
        {2.0f, 0.0f, 0.0f, 2.0f},
        {0.1f, 0.4f, 0.4f, -0.1f},
    };
    for (const auto& d : cases) {
        Uint32 c = sampleAt(tex, 0.73f, 0.41f, d);
        // The existing bilinear rule truncates a float sum that can land a
        // hair below the integer even for a constant neighborhood, so a solid
        // texture is allowed the same one-unit tolerance as every other path.
        check(std::abs(int(chan(c, 16)) - 100) <= 1 &&
              std::abs(int(chan(c, 8)) - 100) <= 1 &&
              std::abs(int(chan(c, 0)) - 100) <= 1,
              "solid gray survives every footprint / LOD");
    }
    const Uint32* after = static_cast<Uint32*>(surf->pixels);
    bool unchanged = true;
    for (size_t i = 0; i < before.size(); ++i)
        unchanged &= (after[i] == before[i]);
    check(unchanged, "input surface pixels are not modified");
    SDL_FreeSurface(surf);
}

// ---------------------------------------------------------------------------
// 8. Perspective-correct derivative path (PixelData / TriangleEquations):
//    per-pixel evaluation and span stepping must produce identical
//    footprints, and the quotient-rule derivatives must match a
//    double-precision reference computed from the plane coefficients.
// ---------------------------------------------------------------------------
void testPerspectiveTriangle() {
    std::cout << "[8] perspective triangle derivatives\n";
    swr::RasterizerVertex v0{}, v1{}, v2{};
    v0.x = 10;  v0.y = 10; v0.w = 1.0f; v0.pvar[0] = 0.0f; v0.pvar[1] = 0.0f;
    v1.x = 110; v1.y = 15; v1.w = 3.0f; v1.pvar[0] = 1.0f; v1.pvar[1] = 0.0f;
    v2.x = 25;  v2.y = 95; v2.w = 0.4f; v2.pvar[0] = 0.0f; v2.pvar[1] = 1.0f;

    // Guarantee a front-facing (CCW) triangle before building the equations.
    auto edgeCross = [](const swr::RasterizerVertex& a,
                        const swr::RasterizerVertex& b,
                        const swr::RasterizerVertex& c) {
        return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    };
    if (edgeCross(v0, v1, v2) <= 0.0f)
        std::swap(v1, v2);

    swr::TriangleEquations eqn(v0, v1, v2, 0, 2);
    check(eqn.area2 > 0.0f, "test triangle has positive area");

    SDL_Surface* surf = makeStripeSurface(16, 16);
    swr::Texture tex(surf, 8);
    SDL_FreeSurface(surf);

    int minX = std::min({v0.x, v1.x, v2.x});
    int maxX = std::max({v0.x, v1.x, v2.x});
    int minY = std::min({v0.y, v1.y, v2.y});
    int maxY = std::max({v0.y, v1.y, v2.y});

    int tested = 0;
    for (int y = minY; y < maxY && tested < 6; ++y) {
        for (int x = minX; x < maxX && tested < 6; ++x) {
            float fx = x + 0.5f, fy = y + 0.5f;
            if (!(eqn.e0.test(eqn.e0.evaluate(fx, fy)) &&
                  eqn.e1.test(eqn.e1.evaluate(fx, fy)) &&
                  eqn.e2.test(eqn.e2.evaluate(fx, fy))))
                continue;
            ++tested;

            // Per-pixel path, as used by block rasterization.
            swr::PixelData pa;
            pa.init(eqn, fx, fy, 0, 2, false, true);
            pa.x = x; pa.y = y;
            float dudx, dudy, dvdx, dvdy;
            pa.computePerspectiveDerivatives(eqn, 0, dudx, dudy);
            pa.computePerspectiveDerivatives(eqn, 1, dvdx, dvdy);
            check(std::isfinite(pa.pvar[0]) && std::isfinite(pa.pvar[1]) &&
                  std::isfinite(dudx) && std::isfinite(dvdy),
                  "perspective UVs and derivatives are finite");

            // Double-precision reference for the quotient rule, evaluated
            // straight from the plane equation coefficients. This validates
            // the PixelData formula without cancellation in finite diffs.
            auto analyticRef = [&](int varIndex, float& refDdx, float& refDdy) {
                double xd = fx, yd = fy;
                double invw = double(eqn.invw.a) * xd + double(eqn.invw.b) * yd +
                              double(eqn.invw.c);
                double vvar = double(eqn.pvar[varIndex].a) * xd +
                              double(eqn.pvar[varIndex].b) * yd +
                              double(eqn.pvar[varIndex].c);
                double dinvwX = eqn.invw.a, dinvwY = eqn.invw.b;
                double dvX = eqn.pvar[varIndex].a, dvY = eqn.pvar[varIndex].b;
                refDdx = float((invw * dvX - vvar * dinvwX) / (invw * invw));
                refDdy = float((invw * dvY - vvar * dinvwY) / (invw * invw));
            };
            auto relClose = [](float got, float expected) {
                float scale = std::max(1e-6f, std::fabs(expected));
                return std::fabs(got - expected) <= 5e-3f * scale;
            };
            float refDudx, refDudy, refDvdx, refDvdy;
            analyticRef(0, refDudx, refDudy);
            analyticRef(1, refDvdx, refDvdy);
            check(relClose(dudx, refDudx), "dudx matches double-precision quotient rule");
            check(relClose(dvdx, refDvdx), "dvdx matches double-precision quotient rule");
            check(relClose(dudy, refDudy), "dudy matches double-precision quotient rule");
            check(relClose(dvdy, refDvdy), "dvdy matches double-precision quotient rule");

            // Span path: init at the row start and stepX to this pixel.
            swr::PixelData pb;
            pb.init(eqn, minX + 0.5f, fy, 0, 2, false, true);
            for (int sx = minX; sx < x; ++sx)
                pb.stepX(eqn, 0, 2, false, true);
            pb.x = x; pb.y = y;
            checkNear(pb.pvar[0], pa.pvar[0], 1e-5f, "span UV u matches per-pixel");
            checkNear(pb.pvar[1], pa.pvar[1], 1e-5f, "span UV v matches per-pixel");

            float sdudx, sdudy, sdvdx, sdvdy;
            pb.computePerspectiveDerivatives(eqn, 0, sdudx, sdudy);
            pb.computePerspectiveDerivatives(eqn, 1, sdvdx, sdvdy);
            checkNear(sdudx, dudx, 1e-6f, "span dudx matches per-pixel");
            checkNear(sdvdy, dvdy, 1e-6f, "span dvdy matches per-pixel");

            Uint32 ca = 0, cb = 0;
            tex.sample(pa.pvar[0], pa.pvar[1], dudx, dvdx, dudy, dvdy, ca);
            tex.sample(pb.pvar[0], pb.pvar[1], sdudx, sdvdx, sdudy, sdvdy, cb);
            checkColorClose(ca, cb, "span vs per-pixel sampled color");

            swr::Texture::Footprint fa =
                tex.queryFootprint(pa.pvar[0], pa.pvar[1], dudx, dvdx, dudy, dvdy);
            swr::Texture::Footprint fb =
                tex.queryFootprint(pb.pvar[0], pb.pvar[1], sdudx, sdvdx, sdudy, sdvdy);
            checkNear(fb.majorLength, fa.majorLength, 1e-5f,
                      "span vs per-pixel footprint major axis");
            checkNear(fb.lod, fa.lod, 1e-5f, "span vs per-pixel LOD");
            check(fb.tapCount == fa.tapCount, "span vs per-pixel tap count");
        }
    }
    check(tested >= 4, "perspective triangle covered enough pixels");
}

// ---------------------------------------------------------------------------
// 9. Comparison PPM: top panel sweeps the screen basis through a 90 degree
//    rotation, bottom panel keeps the canonical axis-aligned basis. Both
//    describe one fixed texel-space ellipse and must stay identical,
//    showing continuity of the oblique stripes while the camera turns.
// ---------------------------------------------------------------------------
void writeComparisonPpm(const std::string& path) {
    std::cout << "[9] comparison PPM -> " << path << "\n";
    SDL_Surface* surf = makeStripeSurface(16, 16);
    swr::Texture tex(surf, 8);
    SDL_FreeSurface(surf);

    const int W = 128, H = 48;
    std::vector<Uint8> top(W * H * 3), bottom(W * H * 3);
    for (int i = 0; i < W; ++i) {
        float theta = (float(i) / float(W - 1)) * (PI * 0.5f);
        float c = std::cos(theta), s = std::sin(theta);
        // J' = J0 R(theta): same ellipse (8 x 1 texels), rotated screen basis.
        Derivs rotated = {8 * c / 16, s / 16, -8 * s / 16, c / 16};
        Derivs canonical = {8.0f / 16, 0.0f, 0.0f, 1.0f / 16};
        for (int j = 0; j < H; ++j) {
            float v = 0.05f + 0.9f * float(j) / float(H - 1);
            Uint32 a = sampleAt(tex, 0.37f, v, rotated);
            Uint32 b = sampleAt(tex, 0.37f, v, canonical);
            checkColorClose(a, b, "PPM rotation continuity at column " +
                               std::to_string(i));
            for (int k = 0; k < 3; ++k) {
                top[(j * W + i) * 3 + k] = chan(a, 16);
                bottom[(j * W + i) * 3 + k] = chan(b, 16);
            }
        }
    }

    std::ofstream out(path, std::ios::binary);
    check(out.good(), "open PPM output file");
    out << "P6\n" << W << " " << (2 * H + 2) << "\n255\n";
    out.write(reinterpret_cast<const char*>(top.data()), top.size());
    const Uint8 separator[3] = {255, 0, 0};
    for (int i = 0; i < W; ++i)
        out.write(reinterpret_cast<const char*>(separator), 3);
    for (int i = 0; i < W; ++i)
        out.write(reinterpret_cast<const char*>(separator), 3);
    out.write(reinterpret_cast<const char*>(bottom.data()), bottom.size());
    check(out.good(), "write PPM output file");
}

} // namespace

int main(int argc, char** argv) {
    std::string ppmPath = argc > 1 ? argv[1] : "footprint_compare.ppm";

    testRotationInvariance();
    testAnisotropyCap();
    testNonSquare();
    testBoundaries();
    testRejectedInputs();
    testDegenerateFootprints();
    testSolidAndSurfaceUntouched();
    testPerspectiveTriangle();
    writeComparisonPpm(ppmPath);

    std::cout << (g_failures == 0 ? "ALL PASS" : "FAILURES")
              << ": " << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
