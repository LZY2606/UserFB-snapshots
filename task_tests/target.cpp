#include "checks.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "our_gl.h"
#include "transparency.h"

namespace {

constexpr int kSize = 16;

double weight_for(double alpha, double depth) {
    return std::clamp(0.01 + 8.*alpha, 0.01, 8.) * std::clamp(1. + depth, 0.1, 2.);
}

// Independent reference of the documented per-pixel accumulation.
struct RefPixel {
    double r = 0, g = 0, b = 0, a = 0, T = 1;
    void add(vec3 rgb, double alpha, double depth) {
        const double w = weight_for(alpha, depth);
        r += rgb.x * alpha * w;
        g += rgb.y * alpha * w;
        b += rgb.z * alpha * w;
        a += alpha * w;
        T *= 1. - alpha;
    }
};

std::uint8_t to_byte(double linear) {
    const long v = std::lround(std::clamp(linear, 0., 1.) * 255.);
    return static_cast<std::uint8_t>(v);
}

TGAColor make_color(int b, int g, int r) {
    TGAColor c;
    c.bytespp = 3;
    c[0] = static_cast<std::uint8_t>(b);
    c[1] = static_cast<std::uint8_t>(g);
    c[2] = static_cast<std::uint8_t>(r);
    return c;
}

struct SolidShader : IShader {
    TGAColor color;
    explicit SolidShader(TGAColor c) : color(c) {}
    std::pair<bool,TGAColor> fragment(vec3) const override { return {false, color}; }
};

struct FloatShader : ITransparentShader {
    vec3 rgb;
    double alpha;
    bool discard;
    FloatShader(vec3 c, double a, bool d = false) : rgb(c), alpha(a), discard(d) {}
    TransparentFragment fragment(vec3) const override {
        TransparentFragment f;
        f.rgb = rgb;
        f.alpha = alpha;
        f.discard = discard;
        return f;
    }
};

// Triangle covering the whole viewport at the given ndc depth.
void make_full_triangle(Triangle &tri, double depth) {
    tri[0] = {-1,-1,depth,1};
    tri[1] = {1,-1,depth,1};
    tri[2] = {0,1,depth,1};
}

// Triangle covering the whole viewport with per-vertex ndc depth.
void make_graded_triangle(Triangle &tri, double z0, double z1, double z2) {
    tri[0] = {-1,-1,z0,1};
    tri[1] = {1,-1,z1,1};
    tri[2] = {0,1,z2,1};
}

// Submit a full-viewport test triangle to a transparency buffer.
void draw_full(TransparencyBuffer &tb, const ITransparentShader &shader, double depth) {
    Triangle tri;
    make_full_triangle(tri, depth);
    tb.triangle(tri, shader);
}

void draw_graded(TransparencyBuffer &tb, const ITransparentShader &shader,
                 double z0, double z1, double z2) {
    Triangle tri;
    make_graded_triangle(tri, z0, z1, z2);
    tb.triangle(tri, shader);
}

void setup_frame(int width, int height) {
    init_viewport(0, 0, width, height);
    init_zbuffer(width, height);
}

} // namespace

void test_accumulation(Check &check) {
    setup_frame(kSize, kSize);
    TGAImage fb(kSize, kSize, TGAImage::RGB, make_color(0, 0, 0));
    Triangle tri;
    make_full_triangle(tri, 0.2);

    TransparencyBuffer tb(kSize, kSize);
    tb.clear();
    tb.set_opaque_depth(zbuffer);
    tb.submit();

    const vec3 red{0.8, 0.1, 0.2};
    const vec3 blue{0.1, 0.3, 0.9};
    const double a_red = 0.25, a_blue = 0.6;
    const double depth = 0.2;

    // --- single fragment: formula and clamped weight
    FloatShader single(red, a_red);
    tb.triangle(tri, single);
    const double w_single = weight_for(a_red, depth);
    check(std::abs(tb.accumulated_alpha(8, 8) - a_red * w_single) < 1e-12, "single alpha accumulation");
    check(std::abs(tb.accumulated_rgb(8, 8).x - red.x * a_red * w_single) < 1e-12, "single rgb accumulation");
    check(std::abs(tb.transmittance(8, 8) - (1. - a_red)) < 1e-12, "single transmittance");
    check(std::abs(tb.accumulated_alpha(0, 0) - a_red * w_single) < 1e-12, "coverage reaches corner");

    // --- two fragments, independent reference
    FloatShader second(blue, a_blue);
    tb.triangle(tri, second);
    RefPixel ref;
    ref.add(red, a_red, depth);
    ref.add(blue, a_blue, depth);
    check(std::abs(tb.accumulated_alpha(8, 8) - ref.a) < 1e-12, "two-fragment alpha sum");
    check(std::abs(tb.accumulated_rgb(8, 8).x - ref.r) < 1e-12, "two-fragment red sum");
    check(std::abs(tb.accumulated_rgb(8, 8).y - ref.g) < 1e-12, "two-fragment green sum");
    check(std::abs(tb.accumulated_rgb(8, 8).z - ref.b) < 1e-12, "two-fragment blue sum");
    check(std::abs(tb.transmittance(8, 8) - ref.T) < 1e-12, "two-fragment transmittance");

    const vec3 average{ref.r/ref.a, ref.g/ref.a, ref.b/ref.a};
    check(average.x >= 0. && average.x <= 1., "average red in range");

    // --- swapped order must produce identical accumulation
    TransparencyBuffer swapped(kSize, kSize);
    swapped.clear();
    swapped.set_opaque_depth(zbuffer);
    swapped.submit();
    swapped.triangle(tri, second);
    swapped.triangle(tri, single);
    for (int i=0; i<3; i++)
        check(std::abs(swapped.accumulated_rgb(8, 8)[i] - tb.accumulated_rgb(8, 8)[i]) < 1e-12, "order independent rgb");
    check(std::abs(swapped.accumulated_alpha(8, 8) - tb.accumulated_alpha(8, 8)) < 1e-12, "order independent alpha");
    check(std::abs(swapped.transmittance(8, 8) - tb.transmittance(8, 8)) < 1e-12, "order independent transmittance");

    // --- discard and zero alpha leave no contribution
    tb.clear();
    tb.submit();
    draw_full(tb, FloatShader(red, 0.0), 0.2);
    check(tb.accumulated_alpha(8, 8) == 0., "zero alpha contributes nothing");
    check(tb.transmittance(8, 8) == 1., "zero alpha keeps transmittance");

    tb.clear();
    tb.submit();
    draw_full(tb, FloatShader(red, 0.5, true), 0.2);
    check(tb.accumulated_alpha(8, 8) == 0., "discard contributes nothing");
    check(tb.accumulated_rgb(8, 8).x == 0., "discard adds no rgb");
    check(tb.transmittance(8, 8) == 1., "discard keeps transmittance");

    // --- weight clamps: alpha=1 and depth at both ends
    tb.clear();
    tb.submit();
    draw_graded(tb, FloatShader(red, 1.0), -1.0, -1.0, -1.0);
    const double w_low = weight_for(1.0, -1.0);
    check(std::abs(tb.accumulated_alpha(8, 8) - 1.0 * w_low) < 1e-12, "alpha=1 weight upper clamp");
    check(w_low > 0., "weight stays positive");
    tb.clear();
    tb.submit();
    draw_graded(tb, FloatShader(red, 0.5), 1.0, 1.0, 1.0);
    const double w_high = weight_for(0.5, 1.0);
    check(std::abs(tb.accumulated_alpha(8, 8) - 0.5 * w_high) < 1e-12, "depth weight upper clamp");

    // --- final composite is independent of submission order
    TGAImage fb_a(kSize, kSize, TGAImage::RGB, make_color(10, 20, 30));
    TGAImage fb_b(kSize, kSize, TGAImage::RGB, make_color(10, 20, 30));
    TransparencyBuffer first(kSize, kSize), reverse(kSize, kSize);
    first.clear(); first.set_opaque_depth(zbuffer); first.submit();
    reverse.clear(); reverse.set_opaque_depth(zbuffer); reverse.submit();
    first.triangle(tri, single); first.triangle(tri, second);
    reverse.triangle(tri, second); reverse.triangle(tri, single);
    first.composite(fb_a);
    reverse.composite(fb_b);
    for (int y=0; y<kSize; y++)
        for (int x=0; x<kSize; x++) {
            const TGAColor ca = fb_a.get(x, y), cb = fb_b.get(x, y);
            check(ca[0]==cb[0] && ca[1]==cb[1] && ca[2]==cb[2], "composite order independent");
        }

    // --- final pixel agrees with an independent composition
    const double coverage = 1. - ref.T;
    const double bg_b = 10/255.;
    const double out_b = std::clamp(bg_b*(1.-coverage) + average.z*coverage, 0., 1.);
    check(fb_a.get(8, 8)[0] == to_byte(out_b), "composite matches reference blue");
    check(fb_a.get(8, 8).bytespp == 3, "RGB output stays 3 bytes per pixel");
}

void test_depth(Check &check) {
    setup_frame(kSize, kSize);
    const double opaque_depth = 0.1;
    zbuffer[8 + 8*kSize] = opaque_depth; // opaque geometry at the center

    TransparencyBuffer tb(kSize, kSize);
    tb.clear();
    tb.set_opaque_depth(zbuffer);
    tb.submit();

    const vec3 magenta{0.8, 0.1, 0.8};
    // Behind the opaque surface: nothing accumulates.
    draw_full(tb, FloatShader(magenta, 0.5), 0.0);
    check(tb.accumulated_alpha(8, 8) == 0., "fragment behind opaque is rejected");
    check(tb.transmittance(8, 8) == 1., "rejected fragment keeps transmittance");

    // Equal depth still counts as occluded.
    draw_full(tb, FloatShader(magenta, 0.5), opaque_depth);
    check(tb.accumulated_alpha(8, 8) == 0., "equal depth is occluded");

    // In front of the opaque surface: accumulates.
    draw_full(tb, FloatShader(magenta, 0.5), 0.2);
    check(tb.accumulated_alpha(8, 8) > 0., "fragment in front of opaque accumulates");

    // Transparent fragments never modify the opaque depth buffer.
    check(std::abs(zbuffer[8 + 8*kSize] - opaque_depth) < 1e-15, "opaque depth unchanged by transparent path");

    // Transparent depth is not used to occlude other transparent fragments.
    tb.clear();
    tb.submit();
    draw_full(tb, FloatShader(magenta, 0.3), 0.8);
    check(tb.accumulated_alpha(8, 8) > 0., "near fragment accumulates");
    draw_full(tb, FloatShader(magenta, 0.6), 0.2);
    RefPixel ref;
    ref.add(magenta, 0.3, 0.8);
    ref.add(magenta, 0.6, 0.2);
    check(std::abs(tb.accumulated_alpha(8, 8) - ref.a) < 1e-12, "far fragment not occluded by transparent depth");
    check(std::abs(tb.transmittance(8, 8) - ref.T) < 1e-12, "transmittance composes across transparent depths");

    // Only the opaque snapshot is used: updating zbuffer after submit() must
    // not change what the transparent buffer sees.
    tb.clear();
    tb.submit();
    zbuffer[8 + 8*kSize] = 0.5; // opaque path draws more geometry behind
    draw_full(tb, FloatShader(magenta, 0.5), 0.2);
    check(tb.accumulated_alpha(8, 8) > 0., "frozen opaque snapshot used for depth test");
    check(std::abs(zbuffer[8 + 8*kSize] - 0.5) < 1e-15, "frozen snapshot does not write back");

    // A fresh submit() picks up the updated opaque depth.
    tb.clear();
    tb.submit();
    draw_full(tb, FloatShader(magenta, 0.5), 0.2);
    check(tb.accumulated_alpha(8, 8) == 0., "resubmitted opaque depth blocks");

    // Partial coverage: the opaque depth only covers the center pixel.
    setup_frame(kSize, kSize);
    zbuffer[8 + 8*kSize] = 0.5;
    TransparencyBuffer partial(kSize, kSize);
    partial.clear();
    partial.set_opaque_depth(zbuffer);
    partial.submit();
    draw_full(partial, FloatShader(magenta, 0.5), 0.2);
    check(partial.accumulated_alpha(8, 8) == 0., "blocked only where opaque depth is present");
    check(partial.accumulated_alpha(0, 0) > 0., "unblocked elsewhere");
}

void test_frames(Check &check) {
    setup_frame(kSize, kSize);
    TGAImage fb(kSize, kSize, TGAImage::RGB, make_color(40, 80, 120));
    const TGAColor original = fb.get(3, 3);
    const double depth = 0.2;

    TransparencyBuffer tb(kSize, kSize);
    tb.clear();
    tb.set_opaque_depth(zbuffer);
    tb.submit();

    const vec3 green{0.2, 0.9, 0.2};
    const double alpha = 0.4;
    draw_full(tb, FloatShader(green, alpha), depth);

    // --- pixels with no contribution keep their background bytes
    setup_frame(kSize, kSize);
    TGAImage fb2(kSize, kSize, TGAImage::RGB, make_color(40, 80, 120));
    TransparencyBuffer empty(kSize, kSize);
    empty.clear();
    empty.set_opaque_depth(zbuffer);
    empty.submit();
    empty.composite(fb2);
    for (int y=0; y<kSize; y++)
        for (int x=0; x<kSize; x++) {
            const TGAColor c = fb2.get(x, y);
            check(c[0]==40 && c[1]==80 && c[2]==120, "empty contribution preserves background bytes");
        }

    // --- composite reference
    TGAImage out(kSize, kSize, TGAImage::RGB, make_color(40, 80, 120));
    tb.composite(out);
    RefPixel ref;
    ref.add(green, alpha, depth);
    const double coverage = 1. - ref.T;
    const vec3 avg{ref.r/ref.a, ref.g/ref.a, ref.b/ref.a};
    for (int i=0; i<3; i++) {
        const double bg = make_color(40, 80, 120)[i] / 255.;
        const double expected = std::clamp(bg*(1.-coverage) + avg[i]*coverage, 0., 1.);
        check(out.get(8, 8)[i] == to_byte(expected), "frame composite matches reference");
    }
    check(out.get(8, 8).bytespp == 3, "RGB frame stays 3 bytes per pixel");

    // --- compositing twice yields the same image (background frozen)
    TGAImage twice = out;
    tb.composite(twice);
    for (int y=0; y<kSize; y++)
        for (int x=0; x<kSize; x++) {
            const TGAColor a = out.get(x, y), b = twice.get(x, y);
            check(a[0]==b[0] && a[1]==b[1] && a[2]==b[2], "double composite is idempotent");
        }

    // --- clearing the frame resets all accumulators
    tb.clear();
    check(tb.accumulated_alpha(8, 8) == 0., "clear resets alpha accumulation");
    check(tb.accumulated_rgb(8, 8).x == 0. && tb.accumulated_rgb(8, 8).y == 0. && tb.accumulated_rgb(8, 8).z == 0.,
          "clear resets rgb accumulation");
    check(tb.transmittance(8, 8) == 1., "clear resets transmittance");

    // After clear() the background can be captured again: a new composite must
    // reflect the new frame only, not the previous one.
    tb.set_opaque_depth(zbuffer);
    tb.submit();
    draw_full(tb, FloatShader(vec3{0.9, 0.1, 0.1}, 0.5), depth);
    TGAImage fresh(kSize, kSize, TGAImage::RGB, make_color(40, 80, 120));
    tb.composite(fresh);
    TGAImage again(kSize, kSize, TGAImage::RGB, make_color(40, 80, 120));
    tb.composite(again);
    for (int y=0; y<kSize; y++)
        for (int x=0; x<kSize; x++) {
            const TGAColor a = fresh.get(x, y), b = again.get(x, y);
            check(a[0]==b[0] && a[1]==b[1] && a[2]==b[2], "new frame double composite idempotent");
        }
    check(fresh.get(8, 8)[0] != out.get(8, 8)[0] || fresh.get(8, 8)[2] != out.get(8, 8)[2], "new frame differs from old frame");

    // --- RGBA background: alpha 255 accepted, final alpha stays 255
    setup_frame(kSize, kSize);
    TGAImage rgba(kSize, kSize, TGAImage::RGBA, TGAColor{40, 80, 120, 255, 4});
    TransparencyBuffer tb_rgba(kSize, kSize);
    tb_rgba.clear();
    tb_rgba.set_opaque_depth(zbuffer);
    tb_rgba.submit();
    draw_full(tb_rgba, FloatShader(green, alpha), depth);
    tb_rgba.composite(rgba);
    check(rgba.get(8, 8)[3] == 255, "RGBA composite final alpha 255");
    for (int i=0; i<3; i++) {
        // byte i is BGRA-ordered, so it pairs with RGB component 2-i
        const double bg = make_color(40, 80, 120)[i] / 255.;
        check(rgba.get(8, 8)[i] == to_byte(bg*(1.-coverage) + avg[2-i]*coverage), "RGBA channel matches reference");
    }

    // --- RGB vs RGBA backgrounds with equal bytes produce equal RGB output
    TGAImage rgb_bg(kSize, kSize, TGAImage::RGB, make_color(40, 80, 120));
    TransparencyBuffer tb_rgb(kSize, kSize);
    tb_rgb.clear();
    tb_rgb.set_opaque_depth(zbuffer);
    tb_rgb.submit();
    draw_full(tb_rgb, FloatShader(green, alpha), depth);
    tb_rgb.composite(rgb_bg);
    for (int i=0; i<3; i++)
        check(rgb_bg.get(8, 8)[i] == rgba.get(8, 8)[i], "RGB and RGBA backgrounds agree");

    // --- size mismatch rejected
    bool threw = false;
    try {
        std::vector<double> wrong(kSize*kSize+1, 0.);
        TransparencyBuffer buf(kSize, kSize);
        buf.set_opaque_depth(wrong);
    } catch (const std::invalid_argument &) { threw = true; }
    check(threw, "depth size mismatch rejected");

    // --- a framebuffer whose size does not match is rejected
    threw = false;
    try {
        TGAImage wrong_fb(kSize+1, kSize, TGAImage::RGB);
        tb_rgb.composite(wrong_fb);
    } catch (const std::invalid_argument &) { threw = true; }
    check(threw, "framebuffer size mismatch rejected");

    // --- a non-255 RGBA background is rejected
    threw = false;
    try {
        TGAImage translucent(kSize, kSize, TGAImage::RGBA, TGAColor{40, 80, 120, 128, 4});
        TransparencyBuffer buf(kSize, kSize);
        buf.clear();
        buf.set_opaque_depth(zbuffer);
        buf.submit();
        draw_full(buf, FloatShader(green, alpha), depth);
        buf.composite(translucent);
    } catch (const std::invalid_argument &) { threw = true; }
    check(threw, "RGBA background with alpha != 255 rejected");

    (void)original;
}
