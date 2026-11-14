// Virtual texture task cases. Registered with the C driver in
// task_support/driver.c through task_support/check.h; no main() here.
//
// Every check works against a tiny dense test texture: each page holds known
// linear colors, so cross-page bilinear results, fallback levels and missing
// page keys can be compared against hand-computed values. Derivatives are
// verified against a direct differentiation of the perspective-interpolated
// quotient (the same quotient the implementation differentiates), never
// against a screen-space finite difference.
#include <cmath>
#include <limits>
#include <memory>
#include <vector>
#include <stdexcept>
#include "../task_support/check.h"
#include "our_gl.h"
#include "virtual_texture.h"

using namespace vtex;

namespace {

constexpr double EPS = 1e-10;

bool close(double a, double b, double eps = EPS) { return std::fabs(a - b) <= eps; }
bool close_rgb(const RGBd &a, const RGBd &b, double eps = EPS) {
    return close(a.r, b.r, eps) && close(a.g, b.g, eps) && close(a.b, b.b, eps);
}
bool close_vec(const vec2 &a, const vec2 &b, double eps = EPS) {
    return close(a.x, b.x, eps) && close(a.y, b.y, eps);
}

// Solid color page.
PageData solid_page(int width, int height, RGBd c) {
    PageData p;
    p.width = width;
    p.height = height;
    p.pixels.assign(static_cast<size_t>(width) * height, c);
    return p;
}

// Same mapping as init_viewport(0,0,w,h), captured per frame.
mat<4,4> test_viewport(int w, int h) {
    return mat<4,4>{{{w/2., 0, 0, w/2.}, {0, h/2., 0, h/2.}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
}

// A full-screen quad made of two triangles, with a uv ramp of the given span
// (u and v both run from 0 to span across the viewport). The frame owns its
// own copies, so raw arrays are fine.
void submit_fullscreen_quad(VirtualTextureFrame &frame, int texture, double z,
                            double span = 1., double wv = 1.) {
    vec4 a[3] = { vec4{-wv, -wv, z, wv}, vec4{wv, -wv, z, wv}, vec4{-wv, wv, z, wv} };
    vec4 b[3] = { vec4{ wv, -wv, z, wv}, vec4{wv,  wv, z, wv}, vec4{-wv, wv, z, wv} };
    vec2 ua[3] = { vec2{0, 0}, vec2{span, 0}, vec2{0, span} };
    vec2 ub[3] = { vec2{span, 0}, vec2{span, span}, vec2{0, span} };
    frame.submit(a, ua, texture);
    frame.submit(b, ub, texture);
}

bool has_key(const std::vector<PageKey> &keys, int tex, int level, int px, int py) {
    for (const PageKey &k : keys)
        if (k.texture == tex && k.level == level && k.page_x == px && k.page_y == py) return true;
    return false;
}

// Upload every page of every mip level with the same color.
void upload_all_levels(SparseTexturePool &pool, int tex, RGBd c) {
    int levels = pool.level_count(tex);
    for (int level = 0; level < levels; ++level) {
        int npx = pool.page_count_x(tex, level), npy = pool.page_count_y(tex, level);
        for (int py = 0; py < npy; ++py)
            for (int px = 0; px < npx; ++px) {
                int ew, eh;
                pool.page_extent(tex, level, px, py, ew, eh);
                pool.upload_page(tex, level, px, py, solid_page(ew, eh, c));
            }
    }
}

// Reference derivatives for the full-screen quad. Screen barycentrics are
// affine in the pixel coordinates; with equal vertex w, S = 1/w is constant,
// so the perspective quotient lambda equals the screen barycentrics and
// d(uv)/dx = (span/W, 0), d(uv)/dy = (0, span/H) independently of w. This is
// the quotient derivative written out directly, not a finite difference.
void reference_derivatives(int Wp, int Hp, double span, vec2 &dudx, vec2 &dudy) {
    dudx = vec2{span / Wp, 0.};
    dudy = vec2{0., span / Hp};
}

// ------------------------------------------------------------- paging group

int paging_upload_and_validation() {
    SparseTexturePool pool;
    pool.declare_texture(0, TextureDesc{8, 8, 4}); // levels 8,4,2,1; page edge 4
    TASK_CHECK(pool.level_count(0) == 4);
    int lw = 0, lh = 0;
    pool.level_size(0, 2, lw, lh);
    TASK_CHECK(lw == 2 && lh == 2);
    TASK_CHECK(pool.page_count_x(0, 0) == 2 && pool.page_count_y(0, 0) == 2);
    TASK_CHECK(pool.page_count_x(0, 3) == 1 && pool.page_count_y(0, 3) == 1);

    // Tail page at level 3 is 1x1, smaller than the page edge: a 2x2 upload
    // must be rejected.
    bool threw = false;
    try { pool.upload_page(0, 3, 0, 0, solid_page(2, 2, RGBd{1, 1, 1})); }
    catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK(threw);
    pool.upload_page(0, 3, 0, 0, solid_page(1, 1, RGBd{0.2, 0.4, 0.6}));
    TASK_CHECK(pool.is_resident(0, 3, 0, 0));

    // Re-uploading must be rejected; replacement requires residency.
    threw = false;
    try { pool.upload_page(0, 3, 0, 0, solid_page(1, 1, RGBd{})); }
    catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK(threw);
    threw = false;
    try { pool.replace_page(0, 0, 0, 0, solid_page(4, 4, RGBd{})); }
    catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK(threw);

    // Non-finite values and wrong pixel counts are rejected before mutation.
    PageData bad = solid_page(4, 4, RGBd{});
    bad.pixels[7].g = std::numeric_limits<double>::infinity();
    threw = false;
    try { pool.upload_page(0, 0, 0, 0, bad); }
    catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK(threw);
    TASK_CHECK(!pool.is_resident(0, 0, 0, 0));
    threw = false;
    try { pool.upload_page(0, 0, 1, 1, solid_page(3, 4, RGBd{})); }
    catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK(threw);
    threw = false;
    try { pool.upload_page(0, 0, 2, 0, solid_page(4, 4, RGBd{})); } // outside the 2x2 page grid
    catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK(threw);

    // Illegal descriptions never enter the pool.
    threw = false;
    try { pool.declare_texture(1, TextureDesc{6, 8, 4}); } // base width not a power of two
    catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK(threw);
    TASK_CHECK(!pool.has_texture(1));
    threw = false;
    try { pool.declare_texture(2, TextureDesc{8, 8, 3}); } // page edge not a power of two
    catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK(threw);

    // Pinned 1x1 level cannot be evicted; a regular page can.
    pool.upload_page(0, 2, 0, 0, solid_page(2, 2, RGBd{1, 0, 0}));
    threw = false;
    try { pool.evict_page(0, 3, 0, 0); }
    catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK(threw);
    TASK_CHECK(pool.is_resident(0, 3, 0, 0));
    pool.evict_page(0, 2, 0, 0);
    TASK_CHECK(!pool.is_resident(0, 2, 0, 0));
    threw = false;
    try { pool.evict_page(0, 2, 0, 0); } // evicting twice is rejected
    catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK(threw);
    return 0;
}

int paging_snapshot_immutability() {
    SparseTexturePool pool;
    pool.declare_texture(0, TextureDesc{4, 4, 2}); // levels 4,2,1
    pool.upload_page(0, 2, 0, 0, solid_page(1, 1, RGBd{0.1, 0.2, 0.3}));
    auto snap1 = pool.snapshot();
    TASK_CHECK(snap1->page_keys().size() == 1);

    pool.upload_page(0, 0, 0, 0, solid_page(2, 2, RGBd{0.9, 0.8, 0.7}));
    auto snap2 = pool.snapshot();
    TASK_CHECK(snap1->page_keys().size() == 1); // old snapshot untouched
    TASK_CHECK(snap2->page_keys().size() == 2);
    TASK_CHECK(snap1->find(0, 0, 0, 0) == nullptr);
    TASK_CHECK(snap2->find(0, 0, 0, 0) != nullptr);

    pool.replace_page(0, 0, 0, 0, solid_page(2, 2, RGBd{0.2, 0.3, 0.4}));
    const PageData *old_page = snap2->find(0, 0, 0, 0);
    TASK_CHECK(close_rgb(old_page->pixels[0], RGBd{0.9, 0.8, 0.7}));
    pool.evict_page(0, 0, 0, 0);
    TASK_CHECK(snap2->find(0, 0, 0, 0) != nullptr); // eviction does not reach snapshots
    TASK_CHECK(!pool.is_resident(0, 0, 0, 0));
    return 0;
}

int paging_feedback_and_fallback() {
    SparseTexturePool pool;
    pool.declare_texture(0, TextureDesc{8, 8, 4}); // 4 levels; level 0 has a 2x2 page grid
    pool.upload_page(0, 3, 0, 0, solid_page(1, 1, RGBd{0.5, 0.5, 0.5}));
    // Level 1 is 4x4: exactly one 4x4 page.
    TASK_CHECK(pool.page_count_x(0, 1) == 1 && pool.page_count_y(0, 1) == 1);
    pool.upload_page(0, 1, 0, 0, solid_page(4, 4, RGBd{0.1, 0.2, 0.3}));

    // 16x16 frame against an 8x8 base texture: minification still clamps LOD 0,
    // but fractional texel positions exercise all four level-0 pages.
    auto snap = pool.snapshot();
    VirtualTextureFrame frame(16, 16, snap, test_viewport(16, 16));
    submit_fullscreen_quad(frame, 0, 0.2);
    frame.resolve();

    // Every level-0 page is missing, so the sampler walks to resident level 1.
    const FragmentResult &f = frame.fragment(8, 8);
    TASK_CHECK(close_rgb(f.color, RGBd{0.1, 0.2, 0.3}));
    TASK_CHECK(f.lod_lo == 1 && f.lod_hi == -1);
    const std::vector<PageKey> &miss = frame.missing_pages();
    TASK_CHECK(miss.size() == 4);
    for (const PageKey &k : miss) TASK_CHECK(k.texture == 0 && k.level == 0);
    for (int py = 0; py < 2; ++py)
        for (int px = 0; px < 2; ++px)
            TASK_CHECK(has_key(miss, 0, 0, px, py));
    // Sorted by texture, level, page y, page x and deduplicated.
    for (size_t i = 1; i < miss.size(); ++i) TASK_CHECK(miss[i - 1] < miss[i]);

    // After uploading exactly the missing pages, a new frame needs no fallback.
    for (int py = 0; py < 2; ++py)
        for (int px = 0; px < 2; ++px)
            pool.upload_page(0, 0, px, py, solid_page(4, 4, RGBd{0.7, 0.6, 0.5}));
    VirtualTextureFrame frame2(16, 16, pool.snapshot(), test_viewport(16, 16));
    submit_fullscreen_quad(frame2, 0, 0.2);
    frame2.resolve();
    TASK_CHECK(frame2.missing_pages().empty());
    TASK_CHECK(frame2.fragment(8, 8).lod_lo == 0);
    TASK_CHECK(close_rgb(frame2.fragment(8, 8).color, RGBd{0.7, 0.6, 0.5}));
    // The old frame still reports the old feedback and fallback color.
    TASK_CHECK(frame.missing_pages().size() == 4);
    TASK_CHECK(close_rgb(frame.fragment(8, 8).color, RGBd{0.1, 0.2, 0.3}));
    return 0;
}

int paging_two_bands_same_fallback() {
    SparseTexturePool pool;
    pool.declare_texture(0, TextureDesc{8, 8, 4}); // levels 0..3; only the pinned level resident
    pool.upload_page(0, 3, 0, 0, solid_page(1, 1, RGBd{0.4, 0.5, 0.6}));

    // span 2*sqrt(2) over 16 pixels: rho = sqrt(2), LOD = 0.5, so the two
    // bands start at ideal levels 0 and 1. Both must walk all the way to the
    // pinned level 3, independently, and still be blended by t = 0.5.
    auto snap = pool.snapshot();
    VirtualTextureFrame frame(16, 16, snap, test_viewport(16, 16));
    submit_fullscreen_quad(frame, 0, 0.2, 2. * std::sqrt(2.));
    frame.resolve();

    const FragmentResult &f = frame.fragment(8, 8);
    TASK_CHECK(close(f.lod, 0.5, 1e-9));
    TASK_CHECK(f.lod_lo == 3 && f.lod_hi == 3); // both bands ended on the same level
    TASK_CHECK(close_rgb(f.color, RGBd{0.4, 0.5, 0.6}));

    // Feedback names the ideal level of each band (all four level-0 pages and
    // the single level-1 page), never the intermediate levels 2 or the pinned 3.
    const std::vector<PageKey> &miss = frame.missing_pages();
    TASK_CHECK(miss.size() == 5);
    for (int py = 0; py < 2; ++py)
        for (int px = 0; px < 2; ++px)
            TASK_CHECK(has_key(miss, 0, 0, px, py));
    TASK_CHECK(has_key(miss, 0, 1, 0, 0));
    for (const PageKey &k : miss) TASK_CHECK(k.level <= 1);
    return 0;
}

// ---------------------------------------------------------- footprint group

int footprint_cross_page_bilinear() {
    SparseTexturePool pool;
    pool.declare_texture(0, TextureDesc{4, 4, 2}); // level 0: four 2x2 pages
    pool.upload_page(0, 2, 0, 0, solid_page(1, 1, RGBd{0., 0., 0.}));
    pool.upload_page(0, 0, 0, 0, solid_page(2, 2, RGBd{1., 0., 0.})); // red
    pool.upload_page(0, 0, 1, 0, solid_page(2, 2, RGBd{0., 1., 0.})); // green
    pool.upload_page(0, 0, 0, 1, solid_page(2, 2, RGBd{1., 0., 1.})); // magenta
    pool.upload_page(0, 0, 1, 1, solid_page(2, 2, RGBd{0., 1., 1.})); // cyan

    auto snap = pool.snapshot();
    VirtualTextureFrame frame(8, 8, snap, test_viewport(8, 8));
    submit_fullscreen_quad(frame, 0, 0.0);
    frame.resolve();
    TASK_CHECK(frame.missing_pages().empty());

    // The shared rasterizer evaluates barycentrics at integer pixel positions:
    // u = x/8, v = y/8, hence fx = x/2 - 0.5 and fy = y/2 - 0.5.
    // Pixel (4,2): fx = 1.5 (x taps texels 1 and 2, equal weights, across the
    // vertical page boundary), fy = 0.5 (y taps stay in page row 0).
    // Color = .5*(red + green) = (0.5, 0.5, 0); a clamping sampler could never
    // reach the green page.
    TASK_CHECK(close_rgb(frame.fragment(4, 2).color, RGBd{0.5, 0.5, 0.}));

    // Pixel (4,4): fx = fy = 1.5: one tap lands in each of the four pages,
    // each with weight .25 -> the average of red, green, magenta and cyan.
    TASK_CHECK(close_rgb(frame.fragment(4, 4).color, RGBd{0.5, 0.5, 0.5}));

    // Pixel (2,4): x taps stay in page column 0 while the y taps cross the
    // horizontal boundary: .5*(red + magenta) = (1, 0, 0.5).
    TASK_CHECK(close_rgb(frame.fragment(2, 4).color, RGBd{1., 0., 0.5}));

    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            TASK_CHECK(frame.fragment(x, y).lod_lo == 0);
    return 0;
}

int footprint_periodic_wrap() {
    SparseTexturePool pool;
    pool.declare_texture(0, TextureDesc{4, 4, 2});
    pool.upload_page(0, 2, 0, 0, solid_page(1, 1, RGBd{}));
    PageData page00 = solid_page(2, 2, RGBd{0.2, 0.2, 0.2});
    page00.pixels[0] = RGBd{0.9, 0.9, 0.9}; // only texel (0,0) is bright
    pool.upload_page(0, 0, 0, 0, page00);
    pool.upload_page(0, 0, 1, 0, solid_page(2, 2, RGBd{0.2, 0.2, 0.2}));
    pool.upload_page(0, 0, 0, 1, solid_page(2, 2, RGBd{0.2, 0.2, 0.2}));
    pool.upload_page(0, 0, 1, 1, solid_page(2, 2, RGBd{0.2, 0.2, 0.2}));

    auto snap = pool.snapshot();
    VirtualTextureFrame frame(16, 16, snap, test_viewport(16, 16));
    // uv ramp starting at 1: pixel (0,0) has u = v = 1, so fx = fy = 3.5 and
    // the taps are texel 3 (weight .5) and texel 4 mod 4 = 0 (weight .5). The
    // second tap exists only through periodic wrapping; clamping would read
    // texel 3 twice and miss the bright texel.
    vec4 a[3] = { vec4{-1, -1, 0, 1}, vec4{1, -1, 0, 1}, vec4{-1, 1, 0, 1} };
    vec4 b[3] = { vec4{ 1, -1, 0, 1}, vec4{1,  1, 0, 1}, vec4{-1, 1, 0, 1} };
    vec2 ua[3] = { vec2{1, 1}, vec2{2, 1}, vec2{1, 2} };
    vec2 ub[3] = { vec2{2, 1}, vec2{2, 2}, vec2{1, 2} };
    frame.submit(a, ua, 0);
    frame.submit(b, ub, 0);
    frame.resolve();
    TASK_CHECK(frame.missing_pages().empty());

    // Bright texel weight 0.5*0.5 = 0.25: 0.2 + 0.7*0.25 = 0.375.
    TASK_CHECK(close_rgb(frame.fragment(0, 0).color, RGBd{0.375, 0.375, 0.375}, 1e-12));
    // Every covered pixel blends only the two known colors.
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            const FragmentResult &q = frame.fragment(x, y);
            if (q.face < 0) continue;
            TASK_CHECK(q.color.r >= 0.2 - EPS && q.color.r <= 0.9 + EPS);
        }
    return 0;
}

int footprint_derivatives_and_lod() {
    SparseTexturePool pool;
    pool.declare_texture(0, TextureDesc{16, 16, 4}); // 5 levels
    upload_all_levels(pool, 0, RGBd{1, 1, 1});       // every page resident

    const int Wp = 16, Hp = 16;
    for (double wv : {1., 2., 0.5}) {
        auto snap = pool.snapshot();
        VirtualTextureFrame frame(Wp, Hp, snap, test_viewport(Wp, Hp));
        submit_fullscreen_quad(frame, 0, 0.1, 1., wv);
        frame.resolve();
        TASK_CHECK(frame.missing_pages().empty());

        vec2 ref_dx, ref_dy;
        reference_derivatives(Wp, Hp, 1., ref_dx, ref_dy);
        for (int y = 1; y < Hp; y += 3)
            for (int x = 1; x < Wp; x += 3) {
                const FragmentResult &f = frame.fragment(x, y);
                TASK_CHECK(close_vec(f.duv_dx, ref_dx, 1e-9));
                TASK_CHECK(close_vec(f.duv_dy, ref_dy, 1e-9));
                double rho = std::max(std::hypot(16 * f.duv_dx.x, 16 * f.duv_dx.y),
                                      std::hypot(16 * f.duv_dy.x, 16 * f.duv_dy.y));
                TASK_CHECK(close(rho, 1., 1e-9));
                TASK_CHECK(close(f.lod, 0., 1e-9));
                TASK_CHECK(f.lod_lo == 0 && f.lod_hi == -1);
            }
    }

    // uv spans 4 repeats over 16 pixels: rho = 4, LOD exactly 2, t = 0 so the
    // fine band is never sampled.
    {
        auto snap = pool.snapshot();
        VirtualTextureFrame frame(Wp, Hp, snap, test_viewport(Wp, Hp));
        submit_fullscreen_quad(frame, 0, 0.1, 4.);
        frame.resolve();
        const FragmentResult &f = frame.fragment(8, 4);
        TASK_CHECK(close(f.lod, 2., 1e-9));
        TASK_CHECK(f.lod_lo == 2 && f.lod_hi == -1);
    }

    // A non-axis-aligned uv ramp gives a fractional LOD and non-zero quotient
    // derivatives in both screen directions: rho = sqrt(5), LOD = log2(sqrt(5)).
    {
        auto snap = pool.snapshot();
        VirtualTextureFrame frame(Wp, Hp, snap, test_viewport(Wp, Hp));
        vec4 a[3] = { vec4{-1, -1, 0, 1}, vec4{1, -1, 0, 1}, vec4{-1, 1, 0, 1} };
        vec4 b[3] = { vec4{ 1, -1, 0, 1}, vec4{1,  1, 0, 1}, vec4{-1, 1, 0, 1} };
        vec2 ua[3] = { vec2{0, 0}, vec2{2, 1}, vec2{1, 2} };
        vec2 ub[3] = { vec2{2, 1}, vec2{2, 2}, vec2{1, 2} };
        frame.submit(a, ua, 0);
        frame.submit(b, ub, 0);
        frame.resolve();
        const FragmentResult &f = frame.fragment(8, 8);
        TASK_CHECK(f.duv_dx.y != 0. && f.duv_dy.x != 0.);
        TASK_CHECK(close(f.lod, std::log2(std::sqrt(5.)), 1e-9));
        TASK_CHECK(f.lod_lo == 1 && f.lod_hi == 2);
    }
    return 0;
}

int footprint_two_band_blend() {
    SparseTexturePool pool;
    pool.declare_texture(0, TextureDesc{16, 16, 4});
    // Level 0 fully white; level 1 (8x8 -> a 2x2 grid of 4x4 pages) fully red.
    for (int py = 0; py < pool.page_count_y(0, 0); ++py)
        for (int px = 0; px < pool.page_count_x(0, 0); ++px)
            pool.upload_page(0, 0, px, py, solid_page(4, 4, RGBd{1, 1, 1}));
    for (int py = 0; py < pool.page_count_y(0, 1); ++py)
        for (int px = 0; px < pool.page_count_x(0, 1); ++px)
            pool.upload_page(0, 1, px, py, solid_page(4, 4, RGBd{1, 0, 0}));
    pool.upload_page(0, 4, 0, 0, solid_page(1, 1, RGBd{}));

    auto snap = pool.snapshot();
    const int Wp = 16;
    VirtualTextureFrame frame(Wp, Wp, snap, test_viewport(Wp, Wp));
    // uv span of sqrt(2): rho = sqrt(2), LOD = 0.5.
    submit_fullscreen_quad(frame, 0, 0., std::sqrt(2.));
    frame.resolve();
    TASK_CHECK(frame.missing_pages().empty());
    const FragmentResult &f = frame.fragment(8, 4);
    TASK_CHECK(close(f.lod, 0.5, 1e-9));
    TASK_CHECK(f.lod_lo == 0 && f.lod_hi == 1);
    TASK_CHECK(close_rgb(f.color, RGBd{1., 0.5, 0.5}, 1e-9));
    return 0;
}

// --------------------------------------------------------- visibility group

int visibility_depth_and_ties() {
    SparseTexturePool pool;
    pool.declare_texture(0, TextureDesc{4, 4, 2});
    upload_all_levels(pool, 0, RGBd{1, 1, 1});
    auto snap = pool.snapshot();

    // Far layer first, near layer second: larger z is nearer.
    VirtualTextureFrame frame(8, 8, snap, test_viewport(8, 8));
    submit_fullscreen_quad(frame, 0, 0.3);
    submit_fullscreen_quad(frame, 0, 0.8);
    frame.resolve();
    TASK_CHECK(frame.fragment(2, 2).face == 2); // second layer, first triangle of it
    TASK_CHECK(close(frame.fragment(2, 2).depth, 0.8));

    // Swap the order; the nearer layer still wins.
    VirtualTextureFrame frame2(8, 8, snap, test_viewport(8, 8));
    submit_fullscreen_quad(frame2, 0, 0.8);
    submit_fullscreen_quad(frame2, 0, 0.3);
    frame2.resolve();
    TASK_CHECK(frame2.fragment(2, 2).face == 0);
    TASK_CHECK(close(frame2.fragment(2, 2).depth, 0.8));

    // Equal depth keeps the first submitted face at the pixel.
    VirtualTextureFrame frame3(8, 8, snap, test_viewport(8, 8));
    submit_fullscreen_quad(frame3, 0, 0.5);
    submit_fullscreen_quad(frame3, 0, 0.5);
    frame3.resolve();
    TASK_CHECK(frame3.fragment(2, 2).face == 0);

    // Background pixels stay at face -1 with zero color: query a point outside
    // both triangles of a single-layer quad (the corner opposite the diagonal).
    VirtualTextureFrame frame4(8, 8, snap, test_viewport(8, 8));
    vec4 a[3] = { vec4{-1, -1, 0, 1}, vec4{1, -1, 0, 1}, vec4{-1, 1, 0, 1} };
    vec2 u[3] = { vec2{0, 0}, vec2{1, 0}, vec2{0, 1} };
    frame4.submit(a, u, 0);
    frame4.resolve();
    TASK_CHECK(frame4.fragment(7, 7).face == -1);
    TASK_CHECK(close_rgb(frame4.fragment(7, 7).color, RGBd{}));
    return 0;
}

int visibility_discard_and_occlusion_feedback() {
    SparseTexturePool pool;
    pool.declare_texture(0, TextureDesc{4, 4, 2});
    pool.upload_page(0, 2, 0, 0, solid_page(1, 1, RGBd{0.5, 0.5, 0.5})); // pinned level only
    auto snap = pool.snapshot();

    // Near layer discarded: it writes no depth, the far layer wins everywhere.
    VirtualTextureFrame frame(8, 8, snap, test_viewport(8, 8));
    submit_fullscreen_quad(frame, 0, 0.2);
    {
        vec4 a[3] = { vec4{-1, -1, 0.9, 1}, vec4{1, -1, 0.9, 1}, vec4{-1, 1, 0.9, 1} };
        vec4 b[3] = { vec4{ 1, -1, 0.9, 1}, vec4{1,  1, 0.9, 1}, vec4{-1, 1, 0.9, 1} };
        vec2 ua[3] = { vec2{0, 0}, vec2{1, 0}, vec2{0, 1} };
        vec2 ub[3] = { vec2{1, 0}, vec2{1, 1}, vec2{0, 1} };
        frame.submit(a, ua, 0, true);
        frame.submit(b, ub, 0, true);
    }
    frame.resolve();
    const FragmentResult &f = frame.fragment(2, 2);
    TASK_CHECK(f.face < 2); // one of the far layer's triangles
    TASK_CHECK(close(f.depth, 0.2));
    TASK_CHECK(close_rgb(f.color, RGBd{0.5, 0.5, 0.5})); // fell back to the pinned level
    TASK_CHECK(f.lod_lo == 2);
    TASK_CHECK(!frame.missing_pages().empty());
    for (const PageKey &k : frame.missing_pages()) TASK_CHECK(k.level == 0);

    // Opaque near layer now occludes the far one: feedback is identical because
    // only winner requests are published and both layers sample the same uv.
    VirtualTextureFrame frame2(8, 8, snap, test_viewport(8, 8));
    submit_fullscreen_quad(frame2, 0, 0.2);
    submit_fullscreen_quad(frame2, 0, 0.9);
    frame2.resolve();
    TASK_CHECK(frame2.fragment(2, 2).face == 2);
    TASK_CHECK(close(frame2.fragment(2, 2).depth, 0.9));
    TASK_CHECK(frame2.missing_pages().size() == frame.missing_pages().size());
    for (size_t i = 0; i < frame2.missing_pages().size(); ++i)
        TASK_CHECK(frame2.missing_pages()[i] == frame.missing_pages()[i]);

    // Empty frame: zero color, face -1, empty feedback.
    VirtualTextureFrame empty(8, 8, snap, test_viewport(8, 8));
    empty.resolve();
    const FragmentResult &e = empty.fragment(0, 0);
    TASK_CHECK(e.face == -1 && close_rgb(e.color, RGBd{}));
    TASK_CHECK(empty.missing_pages().empty());
    return 0;
}

int visibility_snapshot_and_state_isolation() {
    SparseTexturePool pool;
    pool.declare_texture(0, TextureDesc{4, 4, 2});
    pool.upload_page(0, 2, 0, 0, solid_page(1, 1, RGBd{0.3, 0.3, 0.3}));
    auto snap = pool.snapshot();

    VirtualTextureFrame frame(8, 8, snap, test_viewport(8, 8));
    submit_fullscreen_quad(frame, 0, 0.4);
    frame.resolve();

    // Upload the rest afterwards: the bound snapshot keeps the old answers.
    for (int py = 0; py < 2; ++py)
        for (int px = 0; px < 2; ++px)
            pool.upload_page(0, 0, px, py, solid_page(2, 2, RGBd{0.9, 0.9, 0.9}));
    frame.resolve(); // idempotent re-resolve
    const FragmentResult &f = frame.fragment(2, 2);
    TASK_CHECK(close_rgb(f.color, RGBd{0.3, 0.3, 0.3}));
    TASK_CHECK(f.lod_lo == 2);
    TASK_CHECK(!frame.missing_pages().empty());

    // A fresh frame on the new snapshot samples level 0 directly.
    auto snap2 = pool.snapshot();
    VirtualTextureFrame frame2(8, 8, snap2, test_viewport(8, 8));
    submit_fullscreen_quad(frame2, 0, 0.4);
    frame2.resolve();
    TASK_CHECK(close_rgb(frame2.fragment(2, 2).color, RGBd{0.9, 0.9, 0.9}));
    TASK_CHECK(frame2.fragment(2, 2).lod_lo == 0);
    TASK_CHECK(frame2.missing_pages().empty());

    // Interleave two frames while the global viewport changes: frames captured
    // their own viewport matrix, so the results are unaffected.
    init_viewport(0, 0, 32, 32);
    VirtualTextureFrame frame3(8, 8, snap2, test_viewport(8, 8));
    submit_fullscreen_quad(frame3, 0, 0.4);
    frame3.resolve();
    TASK_CHECK(close_vec(frame3.fragment(2, 2).duv_dx, vec2{1. / 8, 0.}, 1e-9));
    TASK_CHECK(close_vec(frame3.fragment(2, 2).duv_dy, vec2{0., 1. / 8}, 1e-9));
    TASK_CHECK(close_rgb(frame.fragment(2, 2).color, RGBd{0.3, 0.3, 0.3}));
    TASK_CHECK(frame2.missing_pages().empty());

    // Invalid submissions are rejected before the frame state changes.
    VirtualTextureFrame frame4(8, 8, snap2, test_viewport(8, 8));
    bool threw = false;
    vec4 bad_clip[3] = { vec4{-1, -1, 0, 1}, vec4{1, -1, 0, 1}, vec4{-1, 1, 0, -1} };
    vec2 good_uv[3]  = { vec2{0, 0}, vec2{1, 0}, vec2{0, 1} };
    try { frame4.submit(bad_clip, good_uv, 0); } // negative w
    catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK(threw);
    vec4 far_clip[3] = { vec4{-1, -1, 0, 1}, vec4{1, -1, 0, 1}, vec4{-1, 1, 1.3, 1.2} };
    threw = false;
    try { frame4.submit(far_clip, good_uv, 0); } // vertex outside the clip frustum (z > w)
    catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK(threw);
    submit_fullscreen_quad(frame4, 0, 0.4); // the frame still accepts valid triangles
    frame4.resolve();
    TASK_CHECK(frame4.fragment(2, 2).face == 0);
    return 0;
}

} // namespace

const task_case task_cases[] = {
    {"paging",     "upload_validation",          paging_upload_and_validation},
    {"paging",     "snapshot_immutability",      paging_snapshot_immutability},
    {"paging",     "feedback_and_fallback",      paging_feedback_and_fallback},
    {"paging",     "two_bands_same_fallback",    paging_two_bands_same_fallback},
    {"footprint",  "cross_page_bilinear",        footprint_cross_page_bilinear},
    {"footprint",  "periodic_wrap",              footprint_periodic_wrap},
    {"footprint",  "derivatives_and_lod",        footprint_derivatives_and_lod},
    {"footprint",  "two_band_blend",             footprint_two_band_blend},
    {"visibility", "depth_and_ties",             visibility_depth_and_ties},
    {"visibility", "discard_occlusion_feedback", visibility_discard_and_occlusion_feedback},
    {"visibility", "snapshot_state_isolation",   visibility_snapshot_and_state_isolation},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
