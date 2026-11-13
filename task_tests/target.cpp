#include <cmath>
#include <stdexcept>
#include "checks.hpp"
#include "our_gl.h"
#include "transparency.h"

namespace {
constexpr int W = 8;
constexpr int H = 8;
constexpr double EPS = 1e-12;

struct ConstShader : ITransparencyShader {
    double r, g, b, a;
    bool   discard_fragment;
    ConstShader(double r_, double g_, double b_, double a_, bool disc = false)
        : r(r_), g(g_), b(b_), a(a_), discard_fragment(disc) {}
    TFragment fragment(vec3) const override {
        return {discard_fragment, {r, g, b}, a};
    }
};

struct OpaqueShader : IShader {
    TGAColor color;
    explicit OpaqueShader(TGAColor c) : color(c) {}
    std::pair<bool,TGAColor> fragment(vec3) const override { return {false, color}; }
};

// Independent re-implementation of the per-pixel bookkeeping, kept separate
// from the production code so the test does not validate the code with the
// code.
struct ReferenceAccumulator {
    double ar = 0., ag = 0., ab = 0., aa = 0., reveal = 1.;
    static double clampd(double v, double lo, double hi) { return std::max(lo, std::min(hi, v)); }
    void add(double r, double g, double b, double alpha, double z) {
        const double w  = clampd(0.01 + 8.*alpha, 0.01, 8.) * clampd(1. + z, 0.1, 2.);
        const double wa = alpha * w;
        ar += r*wa; ag += g*wa; ab += b*wa; aa += wa;
        reveal *= 1. - alpha;
    }
    unsigned char channel_byte(double tint, unsigned char bg) const {
        const double coverage = 1. - reveal;
        const double blended  = tint*coverage + (bg/255.)*(1.-coverage);
        return static_cast<unsigned char>(std::lround(clampd(blended, 0., 1.)*255.));
    }
};

// Constant-depth triangle covering the lower-left half of the frame
// (screen vertices (0,0),(8,0),(0,8)); pixel (2,2) is well inside it and
// pixel (7,7) is outside it.
void full_tri(Triangle &t, double z) {
    t[0] = vec4{-1., -1., z, 1.};
    t[1] = vec4{ 1., -1., z, 1.};
    t[2] = vec4{-1.,  1., z, 1.};
}

constexpr int INSIDE_X = 2, INSIDE_Y = 2;
constexpr int OUTSIDE_X = 7, OUTSIDE_Y = 7;

// Smaller triangle in the lower-left corner, screen vertices (0,0),(4,0),(0,4).
// OCC (1,1) sits inside it; FREE (5,1) is outside it but inside full_tri().
void occluder_tri(Triangle &t, double z) {
    t[0] = vec4{-1., -1., z, 1.};
    t[1] = vec4{ 0., -1., z, 1.};
    t[2] = vec4{-1.,  0., z, 1.};
}
constexpr int OCC_X = 1, OCC_Y = 1;
constexpr int OCC_IDX  = OCC_X + OCC_Y*W;
constexpr int FREE_X = 5, FREE_Y = 1;
constexpr int FREE_IDX = FREE_X + FREE_Y*W;

void fresh_pipeline(TGAImage &fb) {
    init_viewport(0, 0, W, H);
    init_zbuffer(W, H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            fb.set(x, y, TGAColor{30, 60, 90, 255}); // BGRA: B=30 G=60 R=90
}

void near(Check &check, double got, double expected, double tol, const char *msg) {
    check(std::abs(got - expected) <= tol, msg);
}
}

void test_accumulation(Check &check) {
    TGAImage fb(W, H, TGAImage::RGB);
    fresh_pipeline(fb);

    const double z1 = 0.3, a1 = 0.4;
    const double z2 = -0.5, a2 = 0.7;
    ConstShader s1(1.0, 0.2, 0.1, a1);
    ConstShader s2(0.0, 0.5, 0.9, a2);
    Triangle t1, t2;
    full_tri(t1, z1);
    full_tri(t2, z2);

    TransparencyBuffer tb(W, H, zbuffer);
    tb.submit(t1, s1);
    tb.submit(t2, s2);

    ReferenceAccumulator ref;
    ref.add(1.0, 0.2, 0.1, a1, z1);
    ref.add(0.0, 0.5, 0.9, a2, z2);

    auto rgb = tb.accumulated_rgb(INSIDE_X, INSIDE_Y);
    near(check, rgb[0], ref.ar, EPS, "accumulated red mismatch");
    near(check, rgb[1], ref.ag, EPS, "accumulated green mismatch");
    near(check, rgb[2], ref.ab, EPS, "accumulated blue mismatch");
    near(check, tb.accumulated_alpha(INSIDE_X, INSIDE_Y), ref.aa, EPS, "accumulated alpha mismatch");
    near(check, tb.revealage(INSIDE_X, INSIDE_Y), ref.reveal, EPS, "revealage mismatch");
    check(tb.accumulated_alpha(OUTSIDE_X, OUTSIDE_Y) == 0., "uncovered pixel must stay empty");
    near(check, tb.revealage(OUTSIDE_X, OUTSIDE_Y), 1., EPS, "uncovered revealage must stay 1");

    // Explicit weight constants, pinning both clamps of the weight formula.
    const double w_full_alpha_back = ReferenceAccumulator::clampd(0.01+8.*1., .01, 8.)
                                   * ReferenceAccumulator::clampd(1.+(-1.), .1, 2.);
    near(check, w_full_alpha_back, 0.8, EPS, "weight for alpha=1, z=-1 must be 0.8");
    const double w_tiny_front = ReferenceAccumulator::clampd(0.01+8.*0.001, .01, 8.)
                              * ReferenceAccumulator::clampd(1.+2., .1, 2.);
    near(check, w_tiny_front, 0.036, EPS, "weight for alpha=0.001, z=2 must be 0.036");

    // Swapping the submission order must leave every accumulator unchanged.
    TransparencyBuffer swapped(W, H, zbuffer);
    swapped.submit(t2, s2);
    swapped.submit(t1, s1);
    auto rgb_s = swapped.accumulated_rgb(INSIDE_X, INSIDE_Y);
    near(check, rgb_s[0], rgb[0], EPS, "order swap changes red accumulation");
    near(check, rgb_s[1], rgb[1], EPS, "order swap changes green accumulation");
    near(check, rgb_s[2], rgb[2], EPS, "order swap changes blue accumulation");
    near(check, swapped.accumulated_alpha(INSIDE_X, INSIDE_Y),
                 tb.accumulated_alpha(INSIDE_X, INSIDE_Y), EPS, "order swap changes alpha accumulation");
    near(check, swapped.revealage(INSIDE_X, INSIDE_Y),
                 tb.revealage(INSIDE_X, INSIDE_Y), EPS, "order swap changes revealage");

    // Discard and zero alpha must not leave any contribution behind.
    const double aa_before = tb.accumulated_alpha(INSIDE_X, INSIDE_Y);
    const double r_before  = tb.revealage(INSIDE_X, INSIDE_Y);
    ConstShader discarding(1., 1., 1., 0.9, true);
    ConstShader invisible (1., 1., 1., 0.0);
    Triangle td;
    full_tri(td, 0.6);
    tb.submit(td, discarding);
    tb.submit(td, invisible);
    near(check, tb.accumulated_alpha(INSIDE_X, INSIDE_Y), aa_before, EPS, "discard must not accumulate");
    near(check, tb.revealage(INSIDE_X, INSIDE_Y), r_before, EPS, "discard must not touch revealage");

    // The composited bytes must match the independently rounded reference,
    // channel order included (TGA stores BGRA), and untouched pixels keep
    // their exact background bytes.
    const TGAColor bg_before = fb.get(OUTSIDE_X, OUTSIDE_Y);
    tb.composite(fb);
    TGAColor out = fb.get(INSIDE_X, INSIDE_Y);
    const double tint_r = ref.ar/ref.aa, tint_g = ref.ag/ref.aa, tint_b = ref.ab/ref.aa;
    check(out[2] == ref.channel_byte(tint_r, 90), "red channel byte mismatch");
    check(out[1] == ref.channel_byte(tint_g, 60), "green channel byte mismatch");
    check(out[0] == ref.channel_byte(tint_b, 30), "blue channel byte mismatch");
    const TGAColor bg_after = fb.get(OUTSIDE_X, OUTSIDE_Y);
    check(bg_after[0]==bg_before[0] && bg_after[1]==bg_before[1] && bg_after[2]==bg_before[2],
          "uncovered background bytes must be preserved");

    // Compositing twice must produce the very same image.
    tb.composite(fb);
    TGAColor out2 = fb.get(INSIDE_X, INSIDE_Y);
    check(out2[0]==out[0] && out2[1]==out[1] && out2[2]==out[2], "double compositing changed the image");
}

void test_depth(Check &check) {
    TGAImage fb(W, H, TGAImage::RGB);
    fresh_pipeline(fb);

    // Freeze an opaque surface at depth 0 over a small corner region.
    OpaqueShader opaque(TGAColor{10, 20, 30, 255});
    Triangle block;
    occluder_tri(block, 0.0);
    rasterize(block, opaque, fb);
    near(check, zbuffer[OCC_IDX], 0., EPS, "opaque depth must be frozen at 0");
    near(check, zbuffer[FREE_IDX], -1000., EPS, "uncovered depth must stay at the clear value");

    auto covered_alpha = [&](double z) {
        TransparencyBuffer t(W, H, zbuffer);
        ConstShader sh(1., 1., 1., 0.5);
        Triangle tri;
        full_tri(tri, z);
        t.submit(tri, sh);
        return t.accumulated_alpha(OCC_X, OCC_Y);
    };

    // Equal depth counts as occluded; anything deeper is occluded as well.
    check(covered_alpha(0.0)  == 0., "equal-depth transparent fragment must be occluded");
    check(covered_alpha(-0.1) == 0., "deeper transparent fragment must be occluded");
    check(covered_alpha(0.1)  >  0., "closer transparent fragment must be accepted");

    // Transparent submissions never modify the frozen opaque depth.
    {
        TransparencyBuffer t(W, H, zbuffer);
        ConstShader sh(1., 1., 1., 0.5);
        Triangle tri;
        full_tri(tri, 0.9);
        t.submit(tri, sh);
        near(check, zbuffer[OCC_IDX], 0., EPS, "transparent pass must not write depth");
        near(check, zbuffer[FREE_IDX], -1000., EPS, "transparent pass must not clear depth");
    }

    // Two transparent surfaces at different depths both contribute: the
    // nearer one must not depth-block the farther one, in either order.
    ConstShader near_sh(1., 0., 0., 0.6);
    ConstShader far_sh (0., 0., 1., 0.3);
    Triangle t_near, t_far;
    full_tri(t_far, 0.5);
    full_tri(t_near, 0.2);
    TransparencyBuffer ab(W, H, zbuffer);
    ab.submit(t_far, far_sh);
    ab.submit(t_near, near_sh);
    TransparencyBuffer ba(W, H, zbuffer);
    ba.submit(t_near, near_sh);
    ba.submit(t_far, far_sh);

    ReferenceAccumulator ref;
    ref.add(0., 0., 1., 0.3, 0.5);
    ref.add(1., 0., 0., 0.6, 0.2);
    near(check, ab.accumulated_alpha(INSIDE_X, INSIDE_Y), ref.aa, EPS, "both transparent surfaces must accumulate");
    near(check, ba.accumulated_alpha(INSIDE_X, INSIDE_Y), ref.aa, EPS, "reorder must keep both surfaces");
    near(check, ab.accumulated_alpha(INSIDE_X, INSIDE_Y),
                 ba.accumulated_alpha(INSIDE_X, INSIDE_Y), EPS, "depth order must not change the totals");

    // Where no opaque surface exists, transparent fragments pass against
    // the frozen clear depth and still contribute.
    check(ab.accumulated_alpha(FREE_X, FREE_Y) > 0., "transparent fragment must pass the cleared depth");
    check(ab.accumulated_alpha(FREE_X, FREE_Y) ==
          ba.accumulated_alpha(FREE_X, FREE_Y), "cleared-depth region must be order independent too");
}

void test_frames(Check &check) {
    TGAImage fb(W, H, TGAImage::RGB);
    fresh_pipeline(fb);

    ConstShader sh(0.8, 0.4, 0.2, 0.55);
    Triangle tri;
    full_tri(tri, 0.4);
    TransparencyBuffer tb(W, H, zbuffer);
    tb.submit(tri, sh);
    tb.composite(fb);
    const TGAColor first = fb.get(INSIDE_X, INSIDE_Y);

    // New frame: explicit clear, repainted background, same scene.
    tb.clear();
    fresh_pipeline(fb);
    check(tb.accumulated_alpha(INSIDE_X, INSIDE_Y) == 0., "clear must reset alpha accumulator");
    near(check, tb.revealage(INSIDE_X, INSIDE_Y), 1., EPS, "clear must reset revealage to 1");
    auto zero = tb.accumulated_rgb(INSIDE_X, INSIDE_Y);
    check(zero[0]==0. && zero[1]==0. && zero[2]==0., "clear must reset color accumulators");
    tb.submit(tri, sh);
    tb.composite(fb);
    const TGAColor second = fb.get(INSIDE_X, INSIDE_Y);
    check(first[0]==second[0] && first[1]==second[1] && first[2]==second[2],
          "a cleared frame must reproduce the previous frame");

    // A freshly constructed buffer must behave exactly like the cleared one.
    TransparencyBuffer fresh(W, H, zbuffer);
    TGAImage fb2(W, H, TGAImage::RGB);
    fresh_pipeline(fb2);
    fresh.submit(tri, sh);
    fresh.composite(fb2);
    const TGAColor third = fb2.get(INSIDE_X, INSIDE_Y);
    check(third[0]==first[0] && third[1]==first[1] && third[2]==first[2],
          "fresh buffer and cleared buffer must agree");

    // Buffer geometry must match the color and depth buffers.
    bool threw = false;
    try { TransparencyBuffer wrong(W, H + 1, zbuffer); }
    catch (const std::runtime_error&) { threw = true; }
    check(threw, "depth size mismatch must be reported");
    threw = false;
    try {
        TGAImage small(W - 1, H, TGAImage::RGB);
        TransparencyBuffer t(W, H, zbuffer);
        t.composite(small);
    } catch (const std::runtime_error&) { threw = true; }
    check(threw, "color buffer size mismatch must be reported");

    // RGBA backgrounds are accepted only when fully opaque, and the output
    // alpha is forced to 255.
    TGAImage rgba(W, H, TGAImage::RGBA);
    fresh_pipeline(rgba);
    TransparencyBuffer t4(W, H, zbuffer);
    t4.submit(tri, sh);
    t4.composite(rgba);
    check(rgba.get(INSIDE_X, INSIDE_Y)[3] == 255, "composited RGBA alpha must be 255");
    bool alpha_threw = false;
    try {
        TGAImage translucent(W, H, TGAImage::RGBA);
        fresh_pipeline(translucent);
        TGAColor c = translucent.get(INSIDE_X, INSIDE_Y);
        c[3] = 200;
        translucent.set(INSIDE_X, INSIDE_Y, c);
        TransparencyBuffer t(W, H, zbuffer);
        t.submit(tri, sh); // the tampered pixel is inside this triangle
        t.composite(translucent);
    } catch (const std::runtime_error&) { alpha_threw = true; }
    check(alpha_threw, "non-opaque RGBA background must be rejected");
}
