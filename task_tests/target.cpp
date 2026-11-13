/*
 * Task test suite for layer blend modes (bm 0..3), isolation, mattes and
 * surface/cache reuse. This translation unit defines no main(); the runner
 * in task_support/main.cpp drives the registry declared in
 * task_support/registry.hpp.
 *
 * Every expected value is computed here in double precision from the color
 * formulas and simple rectangular coverage. The two public renderer entries
 * (rlottie::Animation::renderSync and the C lottie_animation_render entry)
 * are only checked against each other, never used as the reference.
 */

#include "task_support/registry.hpp"

#include "rlottie.h"
#include "rlottie_capi.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Independent double precision reference
// ---------------------------------------------------------------------------

enum RefMode { REF_NORMAL = 0, REF_MULTIPLY = 1, REF_SCREEN = 2,
               REF_OVERLAY = 3 };

// Straight (unpremultiplied) normalized color plus coverage alpha.
struct RefColor {
    double a{0.0}, r{0.0}, g{0.0}, b{0.0};
    RefColor() = default;
    RefColor(double alpha, double red, double green, double blue)
        : a(alpha), r(red), g(green), b(blue) {}
};

inline double refBlendChannel(double base, double src, RefMode mode)
{
    switch (mode) {
    case REF_MULTIPLY:
        return base * src;
    case REF_SCREEN:
        return base + src - base * src;
    case REF_OVERLAY:
        return base <= 0.5 ? (2.0 * base * src)
                           : (1.0 - 2.0 * (1.0 - base) * (1.0 - src));
    default:
        return src;
    }
}

inline int roundByte(double v)
{
    long n = std::lround(v);
    if (n < 0) n = 0;
    if (n > 255) n = 255;
    return int(n);
}

// Composite premultiplied source over premultiplied destination with a layer
// blend mode and return the premultiplied ARGB32 pixel the engine should
// store.
inline uint32_t refComposite(uint32_t dst, uint32_t src, RefMode mode)
{
    int sa = (src >> 24) & 0xff;
    if (sa == 0) return dst;  // transparent source never participates
    int da = (dst >> 24) & 0xff;

    double as = sa / 255.0, ab = da / 255.0;
    // Premultiplied normalized channels.
    double sR = ((src >> 16) & 0xff) / 255.0;
    double sG = ((src >> 8) & 0xff) / 255.0;
    double sB = (src & 0xff) / 255.0;
    double dR = ((dst >> 16) & 0xff) / 255.0;
    double dG = ((dst >> 8) & 0xff) / 255.0;
    double dB = (dst & 0xff) / 255.0;
    // Unpremultiplied normalized colors feeding the separable blend B.
    double sr = as > 0.0 ? sR / as : 0.0;
    double sg = as > 0.0 ? sG / as : 0.0;
    double sb = as > 0.0 ? sB / as : 0.0;
    double br = ab > 0.0 ? dR / ab : 0.0;
    double bg = ab > 0.0 ? dG / ab : 0.0;
    double bb = ab > 0.0 ? dB / ab : 0.0;

    double nr = refBlendChannel(br, sr, mode);
    double ng = refBlendChannel(bg, sg, mode);
    double nb = refBlendChannel(bb, sb, mode);

    double cross = as * ab;
    double outR = (1.0 - as) * dR + (1.0 - ab) * sR + cross * nr;
    double outG = (1.0 - as) * dG + (1.0 - ab) * sG + cross * ng;
    double outB = (1.0 - as) * dB + (1.0 - ab) * sB + cross * nb;
    int outA = roundByte((as + ab - cross) * 255.0);

    return (uint32_t(outA) << 24) |
           (uint32_t(roundByte(outR * 255.0)) << 16) |
           (uint32_t(roundByte(outG * 255.0)) << 8) |
           uint32_t(roundByte(outB * 255.0));
}

// Normal SrcOver of two straight colors, used to model overlapping shapes
// inside one layer before the layer blend mode is applied.
inline RefColor refSrcOver(const RefColor &dst, const RefColor &src)
{
    double outA = src.a + dst.a * (1.0 - src.a);
    if (outA == 0.0) return {};
    auto mix = [&](double s, double d) {
        return (src.a * s + dst.a * d * (1.0 - src.a)) / outA;
    };
    return {outA, mix(src.r, dst.r), mix(src.g, dst.g), mix(src.b, dst.b)};
}

// Straight 0..1 color built from 8-bit channels and an alpha byte.
inline RefColor straight(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
{
    return {a / 255.0, r / 255.0, g / 255.0, b / 255.0};
}

inline uint32_t premul(const RefColor &c)
{
    int a = roundByte(c.a * 255.0);
    int r = roundByte(c.r * a);
    int g = roundByte(c.g * a);
    int b = roundByte(c.b * a);
    return (uint32_t(a) << 24) | (uint32_t(r) << 16) |
           (uint32_t(g) << 8) | uint32_t(b);
}

// ---------------------------------------------------------------------------
// Pixel assertions
// ---------------------------------------------------------------------------

inline bool channelClose(uint32_t got, uint32_t want, int tol)
{
    for (int shift = 0; shift <= 24; shift += 8) {
        int delta = int((got >> shift) & 0xff) - int((want >> shift) & 0xff);
        if (delta < -tol || delta > tol) return false;
    }
    return true;
}

inline void expectPixel(Case &c, const std::vector<uint32_t> &buf, int w,
                        int x, int y, uint32_t want, const char *what,
                        int tol = 2)
{
    uint32_t got = buf[size_t(y) * w + x];
    char msg[160];
    std::snprintf(msg, sizeof(msg),
                  "%s at (%d,%d): got %08x want %08x (tol %d)", what, x, y,
                  got, want, tol);
    c.check(channelClose(got, want, tol), msg);
}

// ---------------------------------------------------------------------------
// JSON builders
// ---------------------------------------------------------------------------

std::string transform(int px = 0, int py = 0, int opacity = 100)
{
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "\"ks\":{\"o\":{\"a\":0,\"k\":%d},\"r\":{\"a\":0,\"k\":0},"
                  "\"p\":{\"a\":0,\"k\":[%d,%d]},\"a\":{\"a\":0,\"k\":[0,0]},"
                  "\"s\":{\"a\":0,\"k\":[100,100]}}",
                  opacity, px, py);
    return buf;
}

std::string solidLayer(int ind, int bm, const char *color, int w, int h,
                       int opacity = 100, int px = 0, int py = 0,
                       int ip = 0, int op = 3, int tt = 0)
{
    char buf[1024];
    std::snprintf(buf, sizeof(buf),
                  "{\"ty\":1,\"ind\":%d,\"nm\":\"solid%d\",\"sw\":%d,\"sh\":%d,"
                  "\"sc\":\"%s\",\"bm\":%d,\"tt\":%d,\"ip\":%d,\"op\":%d,"
                  "\"st\":0,%s}",
                  ind, ind, w, h, color, bm, tt, ip, op,
                  transform(px, py, opacity).c_str());
    return buf;
}

std::string colorArray(uint8_t r, uint8_t g, uint8_t b, double a = 1.0)
{
    char buf[96];
    std::snprintf(buf, sizeof(buf), "[%.6f,%.6f,%.6f,%.6f]",
                  r / 255.0, g / 255.0, b / 255.0, a);
    return buf;
}

// One filled axis-aligned rectangle group. The rect center/size are given in
// layer coordinates; fill alpha is a separate 0..100 opacity.
std::string rectGroup(const char *name, int cx, int cy, int w, int h,
                      uint8_t r, uint8_t g, uint8_t b, int fillOpacity = 100)
{
    char buf[768];
    std::snprintf(buf, sizeof(buf),
                  "{\"ty\":\"gr\",\"nm\":\"%s\",\"it\":["
                  "{\"ty\":\"rc\",\"nm\":\"r\",\"d\":1,"
                  "\"p\":{\"a\":0,\"k\":[%d,%d]},"
                  "\"s\":{\"a\":0,\"k\":[%d,%d]},"
                  "\"r\":{\"a\":0,\"k\":0}},"
                  "{\"ty\":\"fl\",\"nm\":\"f\",\"r\":1,\"c\":{\"a\":0,\"k\":%s},"
                  "\"o\":{\"a\":0,\"k\":%d}},"
                  "{\"ty\":\"tr\",\"p\":{\"a\":0,\"k\":[0,0]},"
                  "\"a\":{\"a\":0,\"k\":[0,0]},\"s\":{\"a\":0,\"k\":[100,100]},"
                  "\"r\":{\"a\":0,\"k\":0},\"o\":{\"a\":0,\"k\":100}}]}",
                  name, cx, cy, w, h,
                  colorArray(r, g, b).c_str(), fillOpacity);
    return buf;
}

std::string shapeLayer(int ind, int bm, const std::vector<std::string> &groups,
                       int opacity = 100, int tt = 0, int ip = 0, int op = 3)
{
    std::string shapes;
    for (size_t i = 0; i < groups.size(); ++i) {
        if (i) shapes += ",";
        shapes += groups[i];
    }
    char buf[2048];
    std::snprintf(buf, sizeof(buf),
                  "{\"ty\":4,\"ind\":%d,\"nm\":\"shape%d\",\"bm\":%d,\"tt\":%d,"
                  "\"ip\":%d,\"op\":%d,\"st\":0,%s,\"shapes\":[%s]}",
                  ind, ind, bm, tt, ip, op,
                  transform(0, 0, opacity).c_str(), shapes.c_str());
    return buf;
}

std::string precompLayer(int ind, int bm, const char *refId, int w, int h,
                         int opacity = 100)
{
    char buf[1024];
    std::snprintf(buf, sizeof(buf),
                  "{\"ty\":0,\"ind\":%d,\"nm\":\"precomp%d\",\"refId\":\"%s\","
                  "\"w\":%d,\"h\":%d,\"bm\":%d,\"ip\":0,\"op\":3,\"st\":0,%s}",
                  ind, ind, refId, w, h, bm,
                  transform(0, 0, opacity).c_str());
    return buf;
}

std::string precompAsset(const char *id, const std::vector<std::string> &layers)
{
    std::string body;
    for (size_t i = 0; i < layers.size(); ++i) {
        if (i) body += ",";
        body += layers[i];
    }
    return std::string("{\"id\":\"") + id + "\",\"layers\":[" + body + "]}";
}

std::string composition(int w, int h,
                        const std::vector<std::string> &layers,
                        const std::vector<std::string> &assets = {})
{
    std::string layerBody, assetBody;
    for (size_t i = 0; i < layers.size(); ++i) {
        if (i) layerBody += ",";
        layerBody += layers[i];
    }
    if (!assets.empty()) {
        assetBody = ",\"assets\":[";
        for (size_t i = 0; i < assets.size(); ++i) {
            if (i) assetBody += ",";
            assetBody += assets[i];
        }
        assetBody += "]";
    }
    char head[4096];
    std::snprintf(head, sizeof(head),
                  "{\"v\":\"5.5.0\",\"fr\":30,\"ip\":0,\"op\":3,\"w\":%d,"
                  "\"h\":%d%s,\"layers\":[",
                  w, h, assetBody.c_str());
    return std::string(head) + layerBody + "]}";
}

// ---------------------------------------------------------------------------
// Rendering through both public entries
// ---------------------------------------------------------------------------

unsigned renderKey()
{
    static unsigned key = 1;
    return key++;
}

std::vector<uint32_t> renderCpp(const std::string &json, int w, int h,
                                int frame = 0)
{
    std::string key = "cpp-" + std::to_string(renderKey());
    auto anim = rlottie::Animation::loadFromData(json, key, "", false);
    std::vector<uint32_t> buf(size_t(w) * h, 0);
    if (anim) {
        rlottie::Surface surface(buf.data(), w, h, w * 4);
        anim->renderSync(frame, surface);
    }
    return buf;
}

std::vector<uint32_t> renderC(const std::string &json, int w, int h,
                              int frame = 0)
{
    std::string key = "c-" + std::to_string(renderKey());
    Lottie_Animation *anim =
        lottie_animation_from_data(json.c_str(), key.c_str(), "");
    std::vector<uint32_t> buf(size_t(w) * h, 0);
    if (anim) {
        lottie_animation_render(anim, frame, buf.data(), w, h, w * 4);
        lottie_animation_destroy(anim);
    }
    return buf;
}

void expectEntriesAgree(Case &c, const std::string &json, int w, int h,
                        const char *what)
{
    auto cpp = renderCpp(json, w, h);
    auto capi = renderC(json, w, h);
    c.check(cpp == capi, what);
}

// ---------------------------------------------------------------------------
// modes group
// ---------------------------------------------------------------------------

void testModesSolid(Case &c)
{
    const int W = 8, H = 8;
    // Asymmetric opaque colors so each channel of every mode is exercised.
    const uint8_t baseR = 0x20, baseG = 0x60, baseB = 0xA0;
    const uint8_t srcR = 0xC0, srcG = 0x40, srcB = 0x80;
    char baseHex[16], srcHex[16];
    std::snprintf(baseHex, sizeof(baseHex), "#%02X%02X%02X", baseR, baseG,
                  baseB);
    std::snprintf(srcHex, sizeof(srcHex), "#%02X%02X%02X", srcR, srcG, srcB);

    const RefMode modes[4] = {REF_NORMAL, REF_MULTIPLY, REF_SCREEN,
                              REF_OVERLAY};
    for (int bm = 0; bm < 4; ++bm) {
        // Front-to-back order: source layer first, base layer behind it.
        std::string json = composition(
            W, H, {solidLayer(1, bm, srcHex, W, H),
                   solidLayer(2, 0, baseHex, W, H)});
        auto buf = renderCpp(json, W, H);
        uint32_t want = refComposite(
            premul(straight(baseR, baseG, baseB)),
            premul(straight(srcR, srcG, srcB)), modes[bm]);
        expectPixel(c, buf, W, 3, 3, want, "solid mode blend");
        expectPixel(c, buf, W, 7, 7, want, "solid mode blend");
        if (bm == 0)
            expectEntriesAgree(c, json, W, H, "C/C++ entries disagree (solid)");
    }

    // Unknown bm values fall back to Normal.
    std::string unknown = composition(
        W, H, {solidLayer(1, 99, srcHex, W, H),
               solidLayer(2, 0, baseHex, W, H)});
    std::string normal = composition(
        W, H, {solidLayer(1, 0, srcHex, W, H),
               solidLayer(2, 0, baseHex, W, H)});
    c.check(renderCpp(unknown, W, H) == renderCpp(normal, W, H),
            "unknown bm must fall back to Normal");
}

void testModesTransparentBase(Case &c)
{
    const int W = 8, H = 8;
    // Base only covers the left half; the right half stays fully transparent.
    std::string json = composition(
        W, H,
        {solidLayer(1, 2, "#C04080", W, H),
         solidLayer(2, 0, "#2060A0", W / 2, H)});
    auto buf = renderCpp(json, W, H);

    // Right half: base alpha 0, opaque source -> source passes through for
    // every blend mode. Screen with an empty backdrop must not lighten it.
    uint32_t sourceOnly = premul(straight(0xC0, 0x40, 0x80));
    expectPixel(c, buf, W, 6, 3, sourceOnly, "screen over transparent base");
    expectPixel(c, buf, W, 7, 7, sourceOnly, "screen over transparent base");

    // Left half: normal screen against the opaque base.
    uint32_t screened = refComposite(premul(straight(0x20, 0x60, 0xA0)),
                                     premul(straight(0xC0, 0x40, 0x80)),
                                     REF_SCREEN);
    expectPixel(c, buf, W, 1, 1, screened, "screen over opaque base");
}

void testModesSemiTransparent(Case &c)
{
    const int W = 8, H = 8;
    const uint8_t alpha = 127;  // layer opacity 50% -> stored alpha byte 127
    for (int bm = 0; bm < 4; ++bm) {
        RefMode mode = RefMode(bm);
        std::string json = composition(
            W, H,
            {solidLayer(1, bm, "#C04080", W, H, 50),
             solidLayer(2, 0, "#2060A0", W, H)});
        auto buf = renderCpp(json, W, H);
        uint32_t src = premul(straight(0xC0, 0x40, 0x80, alpha));
        uint32_t want =
            refComposite(premul(straight(0x20, 0x60, 0xA0)), src, mode);
        expectPixel(c, buf, W, 4, 4, want, "semi-transparent source blend");
    }
}

void testModesShapeAndPrecomp(Case &c)
{
    const int W = 8, H = 8;

    // Shape layer carrying a Screen mode: one filled rect covering the canvas.
    std::string shapeJson = composition(
        W, H,
        {shapeLayer(1, 2, {rectGroup("g", 4, 4, 8, 8, 0xC0, 0x40, 0x80)}),
         solidLayer(2, 0, "#2060A0", W, H)});
    auto shapeBuf = renderCpp(shapeJson, W, H);
    uint32_t screened = refComposite(premul(straight(0x20, 0x60, 0xA0)),
                                     premul(straight(0xC0, 0x40, 0x80)),
                                     REF_SCREEN);
    expectPixel(c, shapeBuf, W, 4, 4, screened, "shape layer screen");
    expectEntriesAgree(c, shapeJson, W, H, "C/C++ entries disagree (shape)");

    // Precomp layer carrying a Screen mode; its child is a plain solid.
    std::string precompJson = composition(
        W, H,
        {precompLayer(1, 2, "comp1", W, H),
         solidLayer(2, 0, "#2060A0", W, H)},
        {precompAsset("comp1", {solidLayer(3, 0, "#C04080", W, H)})});
    auto precompBuf = renderCpp(precompJson, W, H);
    expectPixel(c, precompBuf, W, 4, 4, screened, "precomp layer screen");
    expectEntriesAgree(c, precompJson, W, H,
                       "C/C++ entries disagree (precomp)");
}

// ---------------------------------------------------------------------------
// isolation group
// ---------------------------------------------------------------------------

void testIsolationOverlappingShapes(Case &c)
{
    const int W = 8, H = 8;
    // One shape layer with two overlapping rects. Lottie shape items are
    // listed front-to-back (like layers), so the orange group comes first and
    // is painted on top after the engine reverses the list. The rects combine
    // with ordinary SrcOver first; the finished layer image then screens once
    // with the backdrop.
    //   orange group: center (5,4) size 6 -> covers x in [2,8)
    //   dark group:   center (3,4) size 6 -> covers x in [0,6)
    std::string json = composition(
        W, H,
        {shapeLayer(1, 2,
                    {rectGroup("top", 5, 4, 6, 6, 0xE0, 0x80, 0x40),
                     rectGroup("bottom", 3, 4, 6, 6, 0x10, 0x10, 0x10)}),
         solidLayer(2, 0, "#303030", W, H)});
    auto buf = renderCpp(json, W, H);

    const RefColor backdrop = straight(0x30, 0x30, 0x30);
    const RefColor orange = straight(0xE0, 0x80, 0x40);
    const RefColor dark = straight(0x10, 0x10, 0x10);

    // Independent rectangular coverage model: bottom dark is painted first,
    // top orange second; each covers [center-size/2, center+size/2).
    auto expectedAt = [&](int x) -> uint32_t {
        RefColor layer{};
        if (x >= 0 && x < 6) layer = refSrcOver(layer, dark);
        if (x >= 2 && x < 8) layer = refSrcOver(layer, orange);
        return refComposite(premul(backdrop), premul(layer), REF_SCREEN);
    };

    expectPixel(c, buf, W, 1, 4, expectedAt(1),
                "bottom-only region of the layer");
    expectPixel(c, buf, W, 5, 4, expectedAt(5),
                "overlap blends shapes first, then the layer");
    expectPixel(c, buf, W, 7, 4, expectedAt(7), "top-only region of the layer");
    expectEntriesAgree(c, json, W, H,
                       "C/C++ entries disagree (overlapping shapes)");
}

void testIsolationPrecompModes(Case &c)
{
    const int W = 8, H = 8;
    // Inside the precomp a Multiply child sits over an opaque child solid; the
    // precomp itself then Screens against the outer backdrop.
    std::string json = composition(
        W, H,
        {precompLayer(1, 2, "comp1", W, H),
         solidLayer(2, 0, "#204080", W, H)},
        {precompAsset("comp1",
                      {solidLayer(3, 1, "#FF8040", W, H),
                       solidLayer(4, 0, "#404040", W, H)})});
    auto buf = renderCpp(json, W, H);

    // Inner step: Multiply is decided by the inner base (#404040).
    auto mul = [](double b, double s) { return b * s; };
    RefColor inner(1.0, mul(0x40 / 255.0, 0xFF / 255.0),
                   mul(0x40 / 255.0, 0x80 / 255.0),
                   mul(0x40 / 255.0, 0x40 / 255.0));
    // Outer step: the precomp screens the outer backdrop exactly once.
    uint32_t want = refComposite(premul(straight(0x20, 0x40, 0x80)),
                                 premul(inner), REF_SCREEN);
    expectPixel(c, buf, W, 4, 4, want, "inner multiply then outer screen");
    expectEntriesAgree(c, json, W, H, "C/C++ entries disagree (nested bm)");
}

void testIsolationPrecompTransparency(Case &c)
{
    const int W = 8, H = 8;
    // The precomp child only covers the left half, so the right half of the
    // precomp surface stays transparent. A child blend mode must not read the
    // outer backdrop, and the precomp Screen must leave outer-only pixels
    // untouched on the right half.
    std::string json = composition(
        W, H,
        {precompLayer(1, 2, "comp1", W, H),
         solidLayer(2, 0, "#505050", W, H)},
        {precompAsset("comp1",
                      {solidLayer(3, 1, "#A0A0A0", W / 2, H)})});
    auto buf = renderCpp(json, W, H);

    // Right half: no precomp content -> outer backdrop unchanged.
    expectPixel(c, buf, W, 6, 4, premul(straight(0x50, 0x50, 0x50)),
                "precomp transparency isolates outer backdrop");
    // Left half: opaque multiply over transparent stays the source color,
    // then that screens the backdrop.
    uint32_t want = refComposite(premul(straight(0x50, 0x50, 0x50)),
                                 premul(straight(0xA0, 0xA0, 0xA0)),
                                 REF_SCREEN);
    expectPixel(c, buf, W, 2, 4, want, "precomp left half screen");
}

// ---------------------------------------------------------------------------
// matte group
// ---------------------------------------------------------------------------

// Lottie order is front-to-back: the matte provider is the layer directly
// above the layer that carries the "tt" target flag. The provider is white so
// it is also a full-luminosity luma matte.
std::string matteComposition(int contentBm, int tt)
{
    const int W = 8, H = 8;
    auto provider = shapeLayer(
        2, 0, {rectGroup("matte", 3, 4, 4, 8, 0xFF, 0xFF, 0xFF)});
    auto content = solidLayer(1, contentBm, "#008000", W, H, 100, 0, 0, 0, 3,
                              tt);
    auto backdrop = solidLayer(3, 0, "#808080", W, H);
    return composition(W, H, {provider, content, backdrop});
}

void checkMatteGeometry(Case &c, const std::string &json, RefMode mode,
                        const char *tag)
{
    const int W = 8;
    auto buf = renderCpp(json, W, 8);
    uint32_t backdrop = premul(straight(0x80, 0x80, 0x80));
    uint32_t blended = refComposite(backdrop,
                                    premul(straight(0x00, 0x80, 0x00)), mode);

    bool inverse = (tag[0] == 'i');  // inverse mattes keep content outside
    // A normal matte keeps the content inside the provider rect; an inverted
    // matte keeps it outside.
    expectPixel(c, buf, W, 2, 4, inverse ? backdrop : blended,
                "matte inside region");
    expectPixel(c, buf, W, 6, 4, inverse ? blended : backdrop,
                "matte outside region");
    // The provider must never reach the parent as colored content: where the
    // backdrop survives it must be exactly the neutral gray.
    uint32_t survivor = buf[4 * W + (inverse ? 2 : 6)];
    c.check(survivor == backdrop, "matte provider leaked as content");
}

void testMatteAlpha(Case &c)
{
    checkMatteGeometry(c, matteComposition(2, 1), REF_SCREEN, "alpha");
    expectEntriesAgree(c, matteComposition(2, 1), 8, 8,
                       "C/C++ entries disagree (alpha matte)");
}

void testMatteAlphaInverse(Case &c)
{
    checkMatteGeometry(c, matteComposition(2, 2), REF_SCREEN,
                       "inverse-alpha");
}

void testMatteLuma(Case &c)
{
    // tt=3 luma, white provider -> full luminosity inside the rect.
    checkMatteGeometry(c, matteComposition(3, 3), REF_OVERLAY, "luma");
    // tt=4 inverted luma.
    checkMatteGeometry(c, matteComposition(3, 4), REF_OVERLAY,
                       "inverse-luma");
}

void testMatteOverflow(Case &c)
{
    const int W = 8, H = 8;
    // Content is 8x8 but the alpha matte only spans x in [1,5); the source
    // visibly extends past the mask. Outside the mask the backdrop must be
    // untouched even though content exists underneath.
    std::string json = matteComposition(2, 1);
    auto buf = renderCpp(json, W, H);
    uint32_t backdrop = premul(straight(0x80, 0x80, 0x80));
    expectPixel(c, buf, W, 0, 0, backdrop, "overflow left of mask");
    expectPixel(c, buf, W, 7, 7, backdrop, "overflow right of mask");
    // Region covered by the provider but where the content is transparent:
    // shrink the content to the left half and confirm the provider alone
    // still paints nothing on the right half.
    auto provider2 = shapeLayer(
        2, 0, {rectGroup("matte", 3, 4, 4, 8, 0xFF, 0xFF, 0xFF)});
    auto content2 = solidLayer(1, 2, "#008000", W / 2, H, 100, 0, 0, 0, 3, 1);
    auto back = solidLayer(3, 0, "#808080", W, H);
    auto buf2 = renderCpp(composition(W, H, {provider2, content2, back}), W, H);
    expectPixel(c, buf2, W, 6, 4, backdrop,
                "provider without content must not paint");
}

// ---------------------------------------------------------------------------
// reuse group
// ---------------------------------------------------------------------------

std::string frameVisibilityComposition(int bm)
{
    const int W = 8, H = 8;
    // Frame 0: blue layer visible. Frame 1: red layer visible (ip/op windows).
    return composition(
        W, H,
        {solidLayer(1, bm, "#2060E0", W, H, 100, 0, 0, 0, 1),
         solidLayer(4, bm, "#E04020", W, H, 100, 0, 0, 1, 2),
         solidLayer(2, 0, "#101010", W, H)});
}

void testReuseFrameReorder(Case &c)
{
    auto anim = rlottie::Animation::loadFromData(
        frameVisibilityComposition(2), "reorder", "", false);
    c.check(anim != nullptr, "reorder animation load");

    auto renderAt = [&](int frame, int w, int h) {
        std::vector<uint32_t> buf(size_t(w) * h, 0);
        rlottie::Surface s(buf.data(), w, h, w * 4);
        anim->renderSync(frame, s);
        return buf;
    };

    auto f0a = renderAt(0, 8, 8);
    auto f1 = renderAt(1, 8, 8);
    auto f0b = renderAt(0, 8, 8);
    c.check(f0a == f0b, "frame 0 must be stable across 0->1->0");
    c.check(f0a != f1, "frame 1 must show the other layer");

    uint32_t want0 = refComposite(premul(straight(0x10, 0x10, 0x10)),
                                  premul(straight(0x20, 0x60, 0xE0)),
                                  REF_SCREEN);
    expectPixel(c, f0b, 8, 4, 4, want0, "revisited frame 0 value");
}

void testReuseSizeSwitch(Case &c)
{
    auto anim = rlottie::Animation::loadFromData(
        frameVisibilityComposition(1), "sizes", "", false);
    c.check(anim != nullptr, "size switch animation load");

    auto renderAt = [&](int w, int h) {
        std::vector<uint32_t> buf(size_t(w) * h, 0);
        rlottie::Surface s(buf.data(), w, h, w * 4);
        anim->renderSync(0, s);
        return buf;
    };

    auto small = renderAt(8, 8);
    auto big = renderAt(16, 16);
    auto smallAgain = renderAt(8, 8);
    c.check(small == smallAgain, "8x8 render stable after a 16x16 render");

    uint32_t want = refComposite(premul(straight(0x10, 0x10, 0x10)),
                                 premul(straight(0x20, 0x60, 0xE0)),
                                 REF_MULTIPLY);
    expectPixel(c, small, 8, 4, 4, want, "small surface value");
    expectPixel(c, big, 16, 8, 8, want, "large surface value");
}

void testReuseDrawRegion(Case &c)
{
    const uint32_t sentinel = 0xCAFEBABEu;
    for (int bm = 0; bm <= 2; bm += 2) {  // Normal then Screen
        auto anim = rlottie::Animation::loadFromData(
            frameVisibilityComposition(bm),
            std::string("region-") + char('0' + bm), "", false);
        c.check(anim != nullptr, "draw region animation load");

        const int W = 12, H = 10;
        std::vector<uint32_t> buf(size_t(W) * H, sentinel);
        rlottie::Surface s(buf.data(), W, H, W * 4);
        s.setDrawRegion(2, 1, 8, 8);  // region generated at 8x8
        anim->renderSync(0, s);

        bool outsideKept = true, paddingKept = true, insideTouched = false;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                uint32_t px = buf[size_t(y) * W + x];
                bool inside = (x >= 2 && x < 10 && y >= 1 && y < 9);
                if (inside) {
                    if (px != sentinel) insideTouched = true;
                } else if (px != sentinel) {
                    outsideKept = false;
                }
            }
        }
        for (int y = 1; y < 9; ++y)
            for (int x = 10; x < 12; ++x)
                if (buf[size_t(y) * W + x] != sentinel) paddingKept = false;

        c.check(outsideKept, "draw region must preserve outside pixels");
        c.check(paddingKept, "draw region must preserve row padding");
        c.check(insideTouched, "draw region interior must be rendered");

        // The 8x8 frame is placed at the region offset; sample its center at
        // surface coordinate (6,5).
        RefMode mode = bm == 2 ? REF_SCREEN : REF_NORMAL;
        uint32_t want = refComposite(premul(straight(0x10, 0x10, 0x10)),
                                     premul(straight(0x20, 0x60, 0xE0)), mode);
        expectPixel(c, buf, W, 6, 5, want, "offset region content");
    }
}

void testReusePaddedStride(Case &c)
{
    // Same animation rendered into surfaces whose stride is wider than the
    // width: padding columns must survive every render, at several sizes.
    auto anim = rlottie::Animation::loadFromData(
        frameVisibilityComposition(2), "padded", "", false);
    c.check(anim != nullptr, "padded stride animation load");

    const uint32_t sentinel = 0x0BADF00Du;
    const int dims[][2] = {{8, 8}, {10, 6}, {8, 8}};
    for (auto &d : dims) {
        int w = d[0], h = d[1], stride = w + 3;
        std::vector<uint32_t> buf(size_t(stride) * h, sentinel);
        rlottie::Surface s(buf.data(), w, h, stride * 4);
        anim->renderSync(0, s);
        bool paddingKept = true;
        for (int y = 0; y < h; ++y)
            for (int x = w; x < stride; ++x)
                if (buf[size_t(y) * stride + x] != sentinel) paddingKept = false;
        c.check(paddingKept, "per-row padding preserved across size changes");
        uint32_t want = refComposite(premul(straight(0x10, 0x10, 0x10)),
                                     premul(straight(0x20, 0x60, 0xE0)),
                                     REF_SCREEN);
        expectPixel(c, buf, stride, w / 2, h / 2, want,
                    "padded surface content");
    }
}

// ---------------------------------------------------------------------------
// PPM illustration of blend ordering
// ---------------------------------------------------------------------------

void writeBlendPpm()
{
    const char *tmp = std::getenv("TMPDIR");
    std::string path = tmp ? (std::string(tmp) + "/rlottie_blend_order.ppm")
                           : "rlottie_blend_order.ppm";
    const int W = 40, stripH = 12, modes = 4;
    const int H = stripH * modes;
    std::vector<uint32_t> pixels(size_t(W) * H, 0);

    for (int m = 0; m < modes; ++m) {
        char json[1024];
        // Back layer: full strip in blue. Front layer: right 70% in red.
        std::snprintf(json, sizeof(json),
                      "{\"v\":\"5.5.0\",\"fr\":30,\"ip\":0,\"op\":3,"
                      "\"w\":%d,\"h\":%d,\"layers\":["
                      "{\"ty\":1,\"ind\":1,\"nm\":\"top\",\"sw\":28,\"sh\":%d,"
                      "\"sc\":\"#E04040\",\"bm\":%d,\"ip\":0,\"op\":3,\"st\":0,"
                      "\"ks\":{\"o\":{\"a\":0,\"k\":100},\"r\":{\"a\":0,\"k\":0},"
                      "\"p\":{\"a\":0,\"k\":[12,0]},\"a\":{\"a\":0,\"k\":[0,0]},"
                      "\"s\":{\"a\":0,\"k\":[100,100]}}},"
                      "{\"ty\":1,\"ind\":2,\"nm\":\"bottom\",\"sw\":%d,\"sh\":%d,"
                      "\"sc\":\"#3060C0\",\"bm\":0,\"ip\":0,\"op\":3,\"st\":0,"
                      "\"ks\":{\"o\":{\"a\":0,\"k\":100},\"r\":{\"a\":0,\"k\":0},"
                      "\"p\":{\"a\":0,\"k\":[0,0]},\"a\":{\"a\":0,\"k\":[0,0]},"
                      "\"s\":{\"a\":0,\"k\":[100,100]}}}]}",
                      W, stripH, stripH, m, W, stripH);
        auto strip = renderCpp(json, W, stripH);
        std::memcpy(pixels.data() + size_t(m) * stripH * W, strip.data(),
                    strip.size() * sizeof(uint32_t));
    }

    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << W << " " << H << "\n255\n";
    for (uint32_t px : pixels) {
        unsigned char rgb[3] = {uint8_t((px >> 16) & 0xff),
                                uint8_t((px >> 8) & 0xff),
                                uint8_t(px & 0xff)};
        out.write(reinterpret_cast<char *>(rgb), 3);
    }
    std::printf("PPM blend-order illustration (Normal,Multiply,Screen,Overlay "
                "strips): %s\n",
                path.c_str());
}

}  // namespace

void register_task_tests(Registry &r)
{
    r.add("modes", "solid_modes_and_unknown_fallback", testModesSolid);
    r.add("modes", "transparent_base", testModesTransparentBase);
    r.add("modes", "semi_transparent_source", testModesSemiTransparent);
    r.add("modes", "shape_and_precomp_layers", testModesShapeAndPrecomp);

    r.add("isolation", "overlapping_shapes_one_blend",
          testIsolationOverlappingShapes);
    r.add("isolation", "precomp_inner_and_outer_bm", testIsolationPrecompModes);
    r.add("isolation", "precomp_transparency_isolation",
          testIsolationPrecompTransparency);

    r.add("matte", "alpha_matte", testMatteAlpha);
    r.add("matte", "alpha_inverse_matte", testMatteAlphaInverse);
    r.add("matte", "luma_matte_and_inverse", testMatteLuma);
    r.add("matte", "source_overflow_and_provider", testMatteOverflow);

    r.add("reuse", "out_of_order_frames", testReuseFrameReorder);
    r.add("reuse", "size_switch", testReuseSizeSwitch);
    r.add("reuse", "nonzero_draw_region", testReuseDrawRegion);
    r.add("reuse", "padded_stride", testReusePaddedStride);

    writeBlendPpm();
}
