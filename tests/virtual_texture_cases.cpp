// Sparse virtual texture task cases.
//
// Three registered groups (paging / footprint / visibility), each built around
// a tiny dense reference texture owned by the test itself. The reference
// bilinear fetch, perspective UV quotient and its analytical derivatives are
// written here independently of the production rasterizer.
#include "virtual_texture.h"
#include "our_gl.h"

#include <cmath>
#include <limits>
#include <vector>

#include "task_support/check.h"

using namespace vt;

namespace {

constexpr int TEX = 7;          // texture id used by most cases
constexpr int SIDE = 8;         // 8x8 base texture, 4-texel pages
constexpr int PAGE = 4;
constexpr double EPS = 1e-12;

bool approx(double a, double b) { return std::fabs(a - b) <= EPS * (1.0 + std::fabs(b)); }

// --- Test-owned dense reference texture ------------------------------------
// color(x, y) is unique per texel and stays inside [0, 1].
vec3 ref_color(int x, int y) {
    return { (x + 1) / 10.0, (y + 1) / 10.0, ((x * SIDE + y) + 1) / 100.0 };
}

int pmod(int i, int n) { int r = i % n; if (r < 0) r += n; return r; }

// Independent repeat/bilinear reference matching the mandated coordinate rule.
vec3 ref_bilinear(double u, double v, int W = SIDE, int H = SIDE) {
    const double gx = u * W - 0.5;
    const double gy = v * H - 0.5;
    const int x0 = static_cast<int>(std::floor(gx));
    const int y0 = static_cast<int>(std::floor(gy));
    const double fx = gx - x0, fy = gy - y0;
    const double wx[2] = {1.0 - fx, fx};
    const double wy[2] = {1.0 - fy, fy};
    vec3 out{0., 0., 0.};
    for (int oy = 0; oy < 2; ++oy)
        for (int ox = 0; ox < 2; ++ox) {
            const double wgt = wx[ox] * wy[oy];
            if (!(wgt > 0.0)) continue;
            out = out + ref_color(pmod(x0 + ox, W), pmod(y0 + oy, H)) * wgt;
        }
    return out;
}

// Fill one page upload from the dense reference, with the page's real extents.
PageUpload make_page(VirtualTexturePool &pool, int level, int px, int py) {
    PageUpload up;
    up.key = {TEX, level, px, py};
    up.width = pool.page_width(TEX, level, px);
    up.height = pool.page_height(TEX, level, py);
    up.rgb.resize(static_cast<size_t>(up.width) * up.height * 3);
    for (int y = 0; y < up.height; ++y)
        for (int x = 0; x < up.width; ++x) {
            const vec3 c = ref_color(px * PAGE + x, py * PAGE + y);
            const size_t k = (static_cast<size_t>(y) * up.width + x) * 3;
            up.rgb[k] = c[0]; up.rgb[k + 1] = c[1]; up.rgb[k + 2] = c[2];
        }
    return up;
}

mat<4,4> full_viewport() { // maps NDC [-1,1]^2 onto an 8x8 pixel image
    return mat<4,4>{{{4.,0.,0.,4.}, {0.,4.,0.,4.}, {0.,0.,1.,0.}, {0.,0.,0.,1.}}};
}

// Two in-frustum triangles tile the whole NDC square, all with w = 1, so the
// perspective quotient degenerates to plain affine interpolation.
void submit_fullscreen_quads(VirtualTextureFrame &frame, int texture_id,
                             const vec2 uv_a[3], const vec2 uv_b[3],
                             double z = 0.2, bool discard = false) {
    const vec4 a[3] = {{-1., -1., z, 1.}, { 1., -1., z, 1.}, {-1.,  1., z, 1.}};
    const vec4 b[3] = {{ 1., -1., z, 1.}, { 1.,  1., z, 1.}, {-1.,  1., z, 1.}};
    frame.submit_triangle(texture_id, discard, a, uv_a);
    frame.submit_triangle(texture_id, discard, b, uv_b);
}

// --- Independent perspective quotient and quotient-rule derivatives --------
struct RefSample { vec2 uv, duvdx, duvdy; };

RefSample ref_evaluate(const vec4 clip[3], const vec2 uv[3],
                       const mat<4,4> &vp, int px, int py) {
    vec2 s[3];
    for (int i = 0; i < 3; ++i) {
        const vec4 ndc = clip[i] / clip[i].w;
        s[i] = (vp * ndc).xy();
    }
    const double den = (s[1].y - s[2].y) * (s[0].x - s[2].x) +
                       (s[2].x - s[1].x) * (s[0].y - s[2].y);
    auto bary = [&](double x, double y, double &l0, double &l1, double &l2) {
        l0 = ((s[1].y - s[2].y) * (x - s[2].x) + (s[2].x - s[1].x) * (y - s[2].y)) / den;
        l1 = ((s[2].y - s[0].y) * (x - s[2].x) + (s[0].x - s[2].x) * (y - s[2].y)) / den;
        l2 = 1.0 - l0 - l1;
    };
    double l[3];
    bary(px, py, l[0], l[1], l[2]);
    const double dl0_dx = (s[1].y - s[2].y) / den;
    const double dl1_dx = (s[2].y - s[0].y) / den;
    const double dl0_dy = (s[2].x - s[1].x) / den;
    const double dl1_dy = (s[0].x - s[2].x) / den;
    const double dl[2][3] = {{dl0_dx, dl1_dx, -dl0_dx - dl1_dx},
                             {dl0_dy, dl1_dy, -dl0_dy - dl1_dy}};

    double a[3], da[2][3], sum = 0.;
    for (int i = 0; i < 3; ++i) {
        a[i] = l[i] / clip[i].w;
        sum += a[i];
        for (int d = 0; d < 2; ++d) da[d][i] = dl[d][i] / clip[i].w;
    }
    vec2 num{0., 0.}, dnum[2] = {{0., 0.}, {0., 0.}};
    double dsum[2] = {0., 0.};
    for (int i = 0; i < 3; ++i) {
        num = num + uv[i] * a[i];
        for (int d = 0; d < 2; ++d) {
            dnum[d] = dnum[d] + uv[i] * da[d][i];
            dsum[d] += da[d][i];
        }
    }
    RefSample r;
    r.uv = num * (1.0 / sum);
    r.duvdx = (dnum[0] * sum - num * dsum[0]) * (1.0 / (sum * sum));
    r.duvdy = (dnum[1] * sum - num * dsum[1]) * (1.0 / (sum * sum));
    return r;
}

VirtualTexturePool make_pool() {
    return VirtualTexturePool(std::vector<TextureDesc>{{TEX, SIDE, SIDE, PAGE}});
}

bool vec3_near(const vec3 &a, const vec3 &b) {
    for (int i = 0; i < 3; ++i) if (!approx(a[i], b[i])) return false;
    return true;
}

} // namespace

// ===========================================================================
// paging group
// ===========================================================================

static int paging_cross_page_and_period() {
    VirtualTexturePool pool = make_pool();
    // Every level-0 page resident; the pinned 1x1 page exists on its own.
    for (int py = 0; py < pool.pages_y(TEX, 0); ++py)
        for (int px = 0; px < pool.pages_x(TEX, 0); ++px)
            pool.upload(make_page(pool, 0, px, py));
    auto snap = pool.snapshot();
    TASK_CHECK(snap->page_count() == 5); // 4 level-0 pages + pinned 1x1

    VirtualTextureFrame frame(SIDE, SIDE, full_viewport(), snap);
    const vec2 uva[3] = {{0., 0.}, {1., 0.}, {0., 1.}};
    const vec2 uvb[3] = {{1., 0.}, {1., 1.}, {0., 1.}};
    submit_fullscreen_quads(frame, TEX, uva, uvb);
    frame.generate();
    TASK_CHECK(frame.missing_pages().empty());

    // Pixel (4, 0) lies on the bottom edge: u = 0.5, v = 0; gx = 0.5*8-0.5 =
    // 3.5, straddling the page boundary between texel 3 (page 0) and 4 (page 1).
    const FragmentInfo &f = frame.fragment(4, 0);
    TASK_CHECK(f.face >= 0 && approx(f.u, 0.5) && approx(f.v, 0.0));
    TASK_CHECK(vec3_near(vec3{f.r, f.g, f.b}, ref_bilinear(f.u, f.v)));

    // Repeat across the periodic boundary while keeping a unit UV span (so the
    // LOD stays 0): u in [0.75, 1.75] wraps texel 8 back onto texel 0 instead
    // of clamping inside the last page.
    VirtualTextureFrame wrap(SIDE, SIDE, full_viewport(), snap);
    const vec2 wa[3] = {{0.75, 0.75}, {1.75, 0.75}, {0.75, 1.75}};
    const vec2 wb[3] = {{1.75, 0.75}, {1.75, 1.75}, {0.75, 1.75}};
    submit_fullscreen_quads(wrap, TEX, wa, wb);
    wrap.generate();
    TASK_CHECK(wrap.missing_pages().empty());
    for (int y = 0; y < SIDE; ++y)
        for (int x = 0; x < SIDE; ++x) {
            const FragmentInfo &p = wrap.fragment(x, y);
            if (p.face < 0) continue;
            const vec3 expect = ref_bilinear(p.u, p.v);
            TASK_CHECK(vec3_near(vec3{p.r, p.g, p.b}, expect));
        }
    return 0;
}

static int paging_partial_residency_fallback() {
    VirtualTexturePool pool = make_pool();
    // Only the level-2 (2x2) page is uploaded besides the pinned 1x1 page.
    pool.upload(make_page(pool, 2, 0, 0));
    auto snap = pool.snapshot();

    VirtualTextureFrame frame(SIDE, SIDE, full_viewport(), snap);
    const vec2 uva[3] = {{0., 0.}, {1., 0.}, {0., 1.}};
    const vec2 uvb[3] = {{1., 0.}, {1., 1.}, {0., 1.}};
    submit_fullscreen_quads(frame, TEX, uva, uvb);
    frame.generate();

    const FragmentInfo &f = frame.fragment(4, 4);
    TASK_CHECK(f.face >= 0);
    TASK_CHECK(f.level0 == 0);                 // ideal level stays 0
    TASK_CHECK(f.used_level0 == 2);            // walked 0 -> 1 -> 2
    TASK_CHECK(f.used_level1 == -1);           // LOD 0: the zero-weight dword is never sampled
    // The color is the level-2 bilinear fetch, still evaluated on the same UV.
    TASK_CHECK(vec3_near(vec3{f.r, f.g, f.b}, ref_bilinear(f.u, f.v, 2, 2)));

    // Feedback names only the ideal (level-0) missing pages, never the level-1
    // page probed during the fallback walk, and never a resident page.
    const auto &missing = frame.missing_pages();
    TASK_CHECK(missing.size() == 4);
    for (const PageKey &k : missing) {
        TASK_CHECK(k.texture_id == TEX && k.level == 0);
        TASK_CHECK(!snap->contains(k));
    }
    TASK_CHECK(page_key_less(missing[0], missing[1]) || missing.size() < 2);
    return 0;
}

static int paging_missing_keys_exact_and_sorted() {
    VirtualTexturePool pool = make_pool(); // only the pinned page is resident
    auto snap = pool.snapshot();
    VirtualTextureFrame frame(SIDE, SIDE, full_viewport(), snap);
    const vec2 uva[3] = {{0., 0.}, {1., 0.}, {0., 1.}};
    const vec2 uvb[3] = {{1., 0.}, {1., 1.}, {0., 1.}};
    submit_fullscreen_quads(frame, TEX, uva, uvb);
    frame.generate();

    const auto &missing = frame.missing_pages();
    TASK_CHECK(missing.size() == 4); // exactly the four level-0 pages, de-duplicated
    // Ordered by texture, level, page y, page x; PageKey fields are (x, y).
    const PageKey expect[4] = {{TEX, 0, 0, 0}, {TEX, 0, 1, 0},
                               {TEX, 0, 0, 1}, {TEX, 0, 1, 1}};
    for (int i = 0; i < 4; ++i) {
        TASK_CHECK(missing[i] == expect[i]); // ordered tex, level, page y, page x
        if (i > 0) TASK_CHECK(page_key_less(missing[i - 1], missing[i]));
    }
    // A second generate() must publish the identical list.
    frame.generate();
    TASK_CHECK(frame.missing_pages().size() == 4);
    TASK_CHECK(frame.missing_pages()[2] == expect[2]);
    return 0;
}

static int paging_validation_rejects_before_mutation() {
    VirtualTexturePool pool = make_pool();
    auto before = pool.snapshot();
    const size_t n_before = before->page_count();

    PageUpload bad;
    bad.key = {TEX, 0, 0, 0};
    bad.width = PAGE; bad.height = PAGE;
    bad.rgb.resize(PAGE * PAGE * 3);
    bad.rgb[7] = std::numeric_limits<double>::infinity();
    bool threw = false;
    try { pool.upload(bad); } catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK(threw && pool.snapshot()->page_count() == n_before);

    bad.rgb[7] = 0.0;
    bad.rgb.pop_back(); // wrong pixel count
    threw = false;
    try { pool.upload(bad); } catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK(threw && pool.snapshot()->page_count() == n_before);

    bad.rgb.push_back(0.0);
    bad.width = 3; // extents disagree with the page's actual 4x4 range
    threw = false;
    try { pool.upload(bad); } catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK(threw && pool.snapshot()->page_count() == n_before);

    PageUpload oob;
    oob.key = {TEX, 0, 5, 0}; // illegal page coordinate
    oob.width = 1; oob.height = 1; oob.rgb = {0., 0., 0.};
    threw = false;
    try { pool.upload(oob); } catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK(threw);

    // Tail levels smaller than a page: the 2x2 level must reject a 4x4 upload
    // but accept its real 2x2 extent.
    TASK_CHECK(pool.level_width(TEX, 2) == 2 && pool.level_height(TEX, 2) == 2);
    PageUpload tail = make_page(pool, 2, 0, 0);
    TASK_CHECK(tail.width == 2 && tail.height == 2 && tail.rgb.size() == 12);
    pool.upload(tail);
    TASK_CHECK(pool.resident({TEX, 2, 0, 0}));

    // The pinned 1x1 last level cannot be evicted; unknown pages cannot either.
    threw = false;
    try { pool.evict({TEX, pool.last_level(TEX), 0, 0}); }
    catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK(threw && pool.resident({TEX, pool.last_level(TEX), 0, 0}));
    threw = false;
    try { pool.evict({TEX, 0, 1, 1}); } catch (const std::invalid_argument&) { threw = true; }
    TASK_CHECK(threw);
    return 0;
}

static int paging_snapshot_replace_evict_independence() {
    VirtualTexturePool pool = make_pool();
    pool.upload(make_page(pool, 0, 0, 0));
    auto snap_a = pool.snapshot();
    TASK_CHECK(snap_a->contains({TEX, 0, 0, 0}));

    // Re-upload replaces the content; old snapshots keep their own pixels.
    PageUpload replaced = make_page(pool, 0, 0, 0);
    replaced.rgb[0] += 0.5;
    pool.upload(replaced);
    auto snap_b = pool.snapshot();
    TASK_CHECK(snap_a->page_count() == snap_b->page_count());
    TASK_CHECK(snap_a->contains({TEX, 0, 0, 0}) && snap_b->contains({TEX, 0, 0, 0}));

    pool.evict({TEX, 0, 0, 0});
    auto snap_c = pool.snapshot();
    TASK_CHECK(snap_a->contains({TEX, 0, 0, 0}));
    TASK_CHECK(!snap_c->contains({TEX, 0, 0, 0}));
    TASK_CHECK(snap_c->page_count() + 1 == snap_a->page_count());

    // A frame bound to the old snapshot still sees the evicted page as resident.
    VirtualTextureFrame frame(SIDE, SIDE, full_viewport(), snap_a);
    const vec2 uva[3] = {{0., 0.}, {1., 0.}, {0., 1.}};
    const vec2 uvb[3] = {{1., 0.}, {1., 1.}, {0., 1.}};
    submit_fullscreen_quads(frame, TEX, uva, uvb);
    frame.generate();
    bool has_level0 = false, has_other = false;
    for (const PageKey &k : frame.missing_pages()) {
        if (k.level == 0 && k.page_x == 0 && k.page_y == 0) has_level0 = true;
        if (k.level != 0) has_other = true;
    }
    TASK_CHECK(!has_level0 && !has_other);
    return 0;
}

// ===========================================================================
// footprint group
// ===========================================================================

static int footprint_quotient_derivatives_and_lod() {
    VirtualTexturePool pool = make_pool();
    for (int py = 0; py < pool.pages_y(TEX, 0); ++py)
        for (int px = 0; px < pool.pages_x(TEX, 0); ++px)
            pool.upload(make_page(pool, 0, px, py));
    auto snap = pool.snapshot();
    VirtualTextureFrame frame(SIDE, SIDE, full_viewport(), snap);

    // One vertex carries w = 3, so perspective correction and its quotient
    // derivatives vary from pixel to pixel.
    const vec4 clip[3] = {{-1., -1., 0.2, 1.}, {1., -1., 0.2, 1.}, {-1., 1., 0.2, 3.}};
    const vec2 uv[3] = {{0., 0.}, {1., 0.}, {0., 1.}};
    frame.submit_triangle(TEX, false, clip, uv);
    frame.generate();

    const int probes[][2] = {{1, 1}, {3, 2}, {2, 4}, {1, 5}};
    for (const auto &q : probes) {
        const int x = q[0], y = q[1];
        const FragmentInfo &f = frame.fragment(x, y);
        if (f.face < 0) continue;
        const RefSample ref = ref_evaluate(clip, uv, full_viewport(), x, y);
        TASK_CHECK(approx(f.u, ref.uv[0]) && approx(f.v, ref.uv[1]));
        TASK_CHECK(approx(f.dudx, ref.duvdx[0]) && approx(f.dvdx, ref.duvdx[1]));
        TASK_CHECK(approx(f.dudy, ref.duvdy[0]) && approx(f.dvdy, ref.duvdy[1]));

        const double rho_x = std::hypot(SIDE * ref.duvdx[0], SIDE * ref.duvdx[1]);
        const double rho_y = std::hypot(SIDE * ref.duvdy[0], SIDE * ref.duvdy[1]);
        const double lod = std::clamp(std::log2(std::max(1.0, std::max(rho_x, rho_y))),
                                      0.0, static_cast<double>(pool.last_level(TEX)));
        TASK_CHECK(approx(f.lod, lod));
    }
    return 0;
}

static int footprint_fractional_lod_mixing() {
    VirtualTexturePool pool = make_pool();
    for (int lev = 0; lev <= pool.last_level(TEX); ++lev)
        for (int py = 0; py < pool.pages_y(TEX, lev); ++py)
            for (int px = 0; px < pool.pages_x(TEX, lev); ++px)
                pool.upload(make_page(pool, lev, px, py));
    auto snap = pool.snapshot();

    // Genuinely fractional LOD: span 3 over an 8-pixel image gives rho = 3 and
    // LOD = log2(3), so both dwords carry non-zero weight.
    VirtualTextureFrame frac(SIDE, SIDE, full_viewport(), snap);
    const vec2 ga[3] = {{0., 0.}, {3., 0.}, {0., 3.}};
    const vec2 gb[3] = {{3., 0.}, {3., 3.}, {0., 3.}};
    submit_fullscreen_quads(frac, TEX, ga, gb);
    frac.generate();
    const FragmentInfo &h = frac.fragment(2, 2);
    TASK_CHECK(h.level0 == 1 && h.level1 == 2);
    TASK_CHECK(h.used_level0 == 1 && h.used_level1 == 2);
    TASK_CHECK(h.lod_frac > 0.0 && h.lod_frac < 1.0);
    TASK_CHECK(approx(h.lod_frac, h.lod - 1.0));
    const vec3 c0 = ref_bilinear(h.u, h.v, 4, 4);
    const vec3 c1 = ref_bilinear(h.u, h.v, 2, 2);
    const vec3 expect = c0 * (1.0 - h.lod_frac) + c1 * h.lod_frac;
    TASK_CHECK(vec3_near(vec3{h.r, h.g, h.b}, expect));

    // Even when both dwords fall back to the same level, the fractional mix is
    // still applied (here: nothing but the pinned 1x1 page is resident).
    VirtualTexturePool sparse = make_pool();
    auto sparse_snap = sparse.snapshot();
    VirtualTextureFrame fb(SIDE, SIDE, full_viewport(), sparse_snap);
    submit_fullscreen_quads(fb, TEX, ga, gb);
    fb.generate();
    const FragmentInfo &g = fb.fragment(2, 2);
    TASK_CHECK(g.level0 == 1 && g.level1 == 2);
    TASK_CHECK(g.used_level0 == sparse.last_level(TEX));
    TASK_CHECK(g.used_level1 == sparse.last_level(TEX));
    TASK_CHECK(approx(g.r, 0.0) && approx(g.g, 0.0) && approx(g.b, 0.0));
    return 0;
}

static int footprint_integer_lod_skips_zero_weight_dword() {
    VirtualTexturePool pool = make_pool();
    for (int py = 0; py < pool.pages_y(TEX, 0); ++py)
        for (int px = 0; px < pool.pages_x(TEX, 0); ++px)
            pool.upload(make_page(pool, 0, px, py));
    auto snap = pool.snapshot();
    VirtualTextureFrame frame(SIDE, SIDE, full_viewport(), snap);

    // Unit UV span: rho = 1 -> LOD = 0 exactly, so the second dword has zero
    // weight and must neither be sampled nor reported as missing.
    const vec2 uva[3] = {{0., 0.}, {1., 0.}, {0., 1.}};
    const vec2 uvb[3] = {{1., 0.}, {1., 1.}, {0., 1.}};
    submit_fullscreen_quads(frame, TEX, uva, uvb);
    frame.generate();
    const FragmentInfo &f = frame.fragment(5, 5);
    TASK_CHECK(f.face >= 0 && approx(f.lod, 0.0));
    TASK_CHECK(f.level0 == 0 && f.level1 == -1);
    TASK_CHECK(f.used_level0 == 0 && f.used_level1 == -1);
    TASK_CHECK(approx(f.lod_frac, 0.0));
    TASK_CHECK(frame.missing_pages().empty());

    // A UV grid shifted by half a texel (1/16) puts pixel centers exactly on
    // texel centers: gx = (x/8 + 1/16)*8 - 0.5 = x, so each fetch collapses to
    // a single non-zero tap returning the exact texel color.
    VirtualTextureFrame centered(SIDE, SIDE, full_viewport(), snap);
    const vec2 ca[3] = {{1. / 16., 1. / 16.}, {1. + 1. / 16., 1. / 16.},
                        {1. / 16., 1. + 1. / 16.}};
    const vec2 cb[3] = {{1. + 1. / 16., 1. / 16.}, {1. + 1. / 16., 1. + 1. / 16.},
                        {1. / 16., 1. + 1. / 16.}};
    submit_fullscreen_quads(centered, TEX, ca, cb);
    centered.generate();
    const FragmentInfo &c = centered.fragment(2, 0);
    TASK_CHECK(approx(c.u, 0.3125));
    const vec3 exact = ref_color(2, 0);
    TASK_CHECK(approx(c.r, exact[0]) && approx(c.g, exact[1]) && approx(c.b, exact[2]));
    return 0;
}

// ===========================================================================
// visibility group
// ===========================================================================

static int visibility_occlusion_order_swap() {
    VirtualTexturePool pool = make_pool(); // texture TEX: only pinned page
    const int TEX2 = 8;
    VirtualTexturePool two(std::vector<TextureDesc>{
        {TEX, SIDE, SIDE, PAGE}, {TEX2, SIDE, SIDE, PAGE}});

    auto build = [&](bool swap_order) {
        auto snap = two.snapshot();
        auto *frame = new VirtualTextureFrame(SIDE, SIDE, full_viewport(), snap);
        const vec2 uva[3] = {{0., 0.}, {1., 0.}, {0., 1.}};
        const vec2 uvb[3] = {{1., 0.}, {1., 1.}, {0., 1.}};
        const vec4 fa[3] = {{-1., -1., 0.4, 1.}, {1., -1., 0.4, 1.}, {-1., 1., 0.4, 1.}};
        const vec4 fb[3] = {{ 1., -1., 0.4, 1.}, {1.,  1., 0.4, 1.}, {-1., 1., 0.4, 1.}};
        // Two layers covering the same pixels: near layer TEX, far layer TEX2.
        const vec4 na[3] = {{-1., -1., 0.8, 1.}, {1., -1., 0.8, 1.}, {-1., 1., 0.8, 1.}};
        const vec4 nb[3] = {{ 1., -1., 0.8, 1.}, {1.,  1., 0.8, 1.}, {-1., 1., 0.8, 1.}};
        if (!swap_order) {
            frame->submit_triangle(TEX2, false, fa, uva);
            frame->submit_triangle(TEX2, false, fb, uvb);
            frame->submit_triangle(TEX,  false, na, uva);
            frame->submit_triangle(TEX,  false, nb, uvb);
        } else {
            frame->submit_triangle(TEX,  false, na, uva);
            frame->submit_triangle(TEX,  false, nb, uvb);
            frame->submit_triangle(TEX2, false, fa, uva);
            frame->submit_triangle(TEX2, false, fb, uvb);
        }
        frame->generate();
        return frame;
    };

    VirtualTextureFrame *f1 = build(false);
    VirtualTextureFrame *f2 = build(true);
    for (int y = 0; y < SIDE; ++y)
        for (int x = 0; x < SIDE; ++x) {
            const FragmentInfo &a = f1->fragment(x, y);
            const FragmentInfo &b = f2->fragment(x, y);
            TASK_CHECK(a.face >= 0 && b.face >= 0);
            TASK_CHECK(approx(a.depth, b.depth));
            TASK_CHECK(approx(a.r, b.r) && approx(a.g, b.g) && approx(a.b, b.b));
            TASK_CHECK(approx(a.u, b.u) && approx(a.v, b.v));
        }
    // The occluded TEX2 layer must not leave any page request behind, in
    // either submission order.
    for (VirtualTextureFrame *f : {f1, f2}) {
        for (const PageKey &k : f->missing_pages())
            TASK_CHECK(k.texture_id == TEX);
        delete f;
    }
    (void)pool;
    return 0;
}

static int visibility_discard_and_depth_ties() {
    VirtualTexturePool pool = make_pool();
    auto snap = pool.snapshot();

    // A discarded foreground triangle leaves the background visible and never
    // writes depth, so the later background triangle still wins every pixel.
    VirtualTextureFrame frame(SIDE, SIDE, full_viewport(), snap);
    const vec2 uva[3] = {{0., 0.}, {1., 0.}, {0., 1.}};
    const vec2 uvb[3] = {{1., 0.}, {1., 1.}, {0., 1.}};
    submit_fullscreen_quads(frame, TEX, uva, uvb, 0.9, /*discard=*/true);
    submit_fullscreen_quads(frame, TEX, uva, uvb, 0.3, /*discard=*/false);
    frame.generate();
    for (int y = 0; y < SIDE; ++y)
        for (int x = 0; x < SIDE; ++x) {
            const FragmentInfo &f = frame.fragment(x, y);
            TASK_CHECK(f.face >= 2); // one of the two background submissions
            TASK_CHECK(approx(f.depth, 0.3));
        }

    // Equal depth: the first submitted face keeps the pixel.
    VirtualTextureFrame tied(SIDE, SIDE, full_viewport(), snap);
    submit_fullscreen_quads(tied, TEX, uva, uvb, 0.5, false);
    submit_fullscreen_quads(tied, TEX, uva, uvb, 0.5, false);
    tied.generate();
    for (int y = 0; y < SIDE; ++y)
        for (int x = 0; x < SIDE; ++x)
            TASK_CHECK(tied.fragment(x, y).face <= 1);
    return 0;
}

static int visibility_empty_background_and_ownership() {
    VirtualTexturePool pool = make_pool();
    auto snap = pool.snapshot();
    VirtualTextureFrame frame(SIDE, SIDE, full_viewport(), snap);
    frame.generate();
    const FragmentInfo &bg = frame.fragment(0, 0);
    TASK_CHECK(bg.face == -1 && bg.used_level1 == -1);
    TASK_CHECK(bg.r == 0.0 && bg.g == 0.0 && bg.b == 0.0);
    TASK_CHECK(frame.missing_pages().empty());

    // The frame owns its inputs: mutating the caller's arrays after submitting
    // must not change the generated result.
    vec4 clip[3] = {{-1., -1., 0.2, 1.}, {1., -1., 0.2, 1.}, {-1., 1., 0.2, 1.}};
    vec2 uv[3] = {{0., 0.}, {1., 0.}, {0., 1.}};
    VirtualTextureFrame owned(SIDE, SIDE, full_viewport(), snap);
    owned.submit_triangle(TEX, false, clip, uv);
    for (vec4 &v : clip) v.z = 0.95;
    for (vec2 &t : uv) { t[0] += 5.; t[1] += 5.; }
    owned.generate();
    const FragmentInfo &f = owned.fragment(1, 1);
    TASK_CHECK(f.face == 0 && approx(f.depth, 0.2));
    TASK_CHECK(f.u >= 0.0 && f.u < 1.0 && f.v >= 0.0 && f.v < 1.0);
    return 0;
}

static int visibility_interleaved_frames_ignore_global_viewport() {
    VirtualTexturePool pool = make_pool();
    pool.upload(make_page(pool, 0, 0, 0));
    pool.upload(make_page(pool, 0, 1, 0));
    pool.upload(make_page(pool, 0, 0, 1));
    pool.upload(make_page(pool, 0, 1, 1));
    auto snap_full = pool.snapshot();
    pool.evict({TEX, 0, 1, 1});
    auto snap_part = pool.snapshot();

    const mat<4,4> vp_full = full_viewport();
    const mat<4,4> vp_shift = mat<4,4>{{{3.,0.,0.,4.}, {0.,3.,0.,4.},
                                        {0.,0.,1.,0.}, {0.,0.,0.,1.}}};
    const vec2 uva[3] = {{0., 0.}, {1., 0.}, {0., 1.}};
    const vec2 uvb[3] = {{1., 0.}, {1., 1.}, {0., 1.}};

    VirtualTextureFrame f1(SIDE, SIDE, vp_full, snap_full);
    submit_fullscreen_quads(f1, TEX, uva, uvb);
    VirtualTextureFrame f2(SIDE, SIDE, vp_shift, snap_part);
    submit_fullscreen_quads(f2, TEX, uva, uvb);

    f2.generate();
    TASK_CHECK(!f2.missing_pages().empty()); // sparse snapshot reports requests

    // Touch the legacy global viewport in between; bound frames must ignore it.
    init_viewport(0, 0, 2, 2);
    f1.generate();
    TASK_CHECK(f1.missing_pages().empty()); // full snapshot still complete
    const FragmentInfo &a = f1.fragment(4, 4);
    const vec4 ref_clip[3] = {{-1., -1., 0.2, 1.}, {1., -1., 0.2, 1.}, {-1., 1., 0.2, 1.}};
    const RefSample ref = ref_evaluate(ref_clip, uva, vp_full, 4, 4);
    TASK_CHECK(approx(a.u, ref.uv[0]) && approx(a.v, ref.uv[1]));
    TASK_CHECK(vec3_near(vec3{a.r, a.g, a.b}, ref_bilinear(a.u, a.v)));

    bool part_reports_evicted = false;
    for (const PageKey &k : f2.missing_pages())
        if (k == PageKey{TEX, 0, 1, 1}) part_reports_evicted = true;
    TASK_CHECK(part_reports_evicted);
    return 0;
}

// ===========================================================================
// Registration
// ===========================================================================

const struct task_case task_cases[] = {
    {"paging",     "cross_page_and_period",            paging_cross_page_and_period},
    {"paging",     "partial_residency_fallback",       paging_partial_residency_fallback},
    {"paging",     "missing_keys_exact_and_sorted",    paging_missing_keys_exact_and_sorted},
    {"paging",     "validation_rejects_before_mutation", paging_validation_rejects_before_mutation},
    {"paging",     "snapshot_replace_evict_independence", paging_snapshot_replace_evict_independence},
    {"footprint",  "quotient_derivatives_and_lod",     footprint_quotient_derivatives_and_lod},
    {"footprint",  "fractional_lod_mixing",            footprint_fractional_lod_mixing},
    {"footprint",  "integer_lod_skips_zero_weight",    footprint_integer_lod_skips_zero_weight_dword},
    {"visibility", "occlusion_order_swap",             visibility_occlusion_order_swap},
    {"visibility", "discard_and_depth_ties",           visibility_discard_and_depth_ties},
    {"visibility", "empty_background_and_ownership",   visibility_empty_background_and_ownership},
    {"visibility", "interleaved_frames_viewport",      visibility_interleaved_frames_ignore_global_viewport},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
