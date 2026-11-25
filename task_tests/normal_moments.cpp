#include "task_support/check.h"
#include "cpurast.hpp"
#include "lean_shader.hpp"
#include "color_shader.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

using std::vector;

namespace
{
    // ------------------------------------------------------------------ helpers
    struct memory_canvas : cr::canvas
    {
        vector<cr::color> pixels;
        void draw(const vector<cr::color>& p, unsigned w, unsigned h) override
        {
            if (p.size() != size_t(w) * h) std::abort();
            pixels = p;
        }
    };

    struct gradient_event
    {
        unsigned x = 0, y = 0;
        bool has_2d = false;
        vector<float> value, dx, dy;
        cr::lean_sample_diagnostics diag;
        cr::color out;
    };

    struct capture_lean_shader : cr::gradient_fragment_shader
    {
        std::shared_ptr<cr::lean_fragment_shader> inner;
        mutable unsigned legacy_calls = 0;
        mutable unsigned gradient_calls = 0;
        mutable vector<gradient_event> events;

        explicit capture_lean_shader(std::shared_ptr<cr::lean_fragment_shader> s) : inner(std::move(s)) {}

        cr::color run(const vector<float>& in) const override
        {
            ++legacy_calls;
            return inner->run(in);
        }

        cr::color run_gradient(const cr::fragment_gradient& in) const override
        {
            ++gradient_calls;
            gradient_event e;
            e.x = in.x;
            e.y = in.y;
            e.has_2d = in.has_2d_gradient;
            e.value = in.value;
            e.dx = in.dx;
            e.dy = in.dy;
            e.out = inner->run_gradient(in);
            e.diag = inner->diagnostics();
            events.push_back(e);
            return e.out;
        }

        const gradient_event* at(unsigned x, unsigned y) const
        {
            for (const auto& e : events) {
                if (e.x == x && e.y == y) return &e;
            }
            return nullptr;
        }
    };

    struct count_color_shader : color_fs
    {
        mutable unsigned calls = 0;
        cr::color run(const vector<float>& in) const override
        {
            ++calls;
            return color_fs::run(in);
        }
    };

    // float arithmetic mimicking renderer::get_framebuf_x / get_framebuf_y
    unsigned quantize_x(float x, unsigned vp_width)
    {
        unsigned rel = static_cast<unsigned>(((x + 1.f) * vp_width) / 2.f);
        if (rel == vp_width) rel--;
        return rel;
    }

    unsigned quantize_y(float y, unsigned vp_height)
    {
        unsigned rel = static_cast<unsigned>(((y + 1.f) * vp_height) / 2.f);
        if (rel == vp_height) rel--;
        return rel;
    }

    // affine plane through three integer canvas points, per attribute
    struct affine_set
    {
        struct plane
        {
            double c = 0, gx = 0, gy = 0;
            double at(int x, int y, int rx, int ry) const
            {
                return c + gx * double(x - rx) + gy * double(y - ry);
            }
        };
        vector<plane> planes;

        static affine_set build(int x1, int y1, const vector<float>& a1,
                                int x2, int y2, const vector<float>& a2,
                                int x3, int y3, const vector<float>& a3)
        {
            affine_set s;
            const double det = double(x2 - x1) * double(y3 - y1) - double(x3 - x1) * double(y2 - y1);
            s.planes.resize(a1.size());
            for (size_t i = 0; i < a1.size(); i++) {
                s.planes[i].c = a1[i];
                if (det != 0.0) {
                    const double d2 = double(a2[i]) - double(a1[i]);
                    const double d3 = double(a3[i]) - double(a1[i]);
                    s.planes[i].gx = (d2 * double(y3 - y1) - d3 * double(y2 - y1)) / det;
                    s.planes[i].gy = (d3 * double(x2 - x1) - d2 * double(x3 - x1)) / det;
                }
            }
            return s;
        }
    };

    // ------------------------------------------------------------------ textures
    // crossed slope pattern, non-trivial mean / covariance / non-zero pq
    vector<float> crossed_texture(unsigned w, unsigned h)
    {
        vector<float> n(size_t(w) * h * 3);
        for (unsigned y = 0; y < h; y++) {
            for (unsigned x = 0; x < w; x++) {
                const double cx = (double(x) + 0.5) / w - 0.5;
                const double cy = (double(y) + 0.5) / h - 0.5;
                // nx / nz slopes with crossed sign structure
                const double p = 0.9 * cx + 0.65 * cy;
                const double q = -0.55 * cx + 0.8 * cy;
                const double nz = 1.0;
                const double nx = p * nz;
                const double ny = q * nz;
                size_t i = (size_t(y) * w + x) * 3;
                n[i + 0] = float(nx);
                n[i + 1] = float(ny);
                n[i + 2] = float(nz);
            }
        }
        return n;
    }

    cr::normal_moment_moments direct_base_moments(const vector<float>& n, unsigned w, unsigned h,
                                                  unsigned x, unsigned y)
    {
        (void)w; (void)h;
        const size_t i = (size_t(y) * w + x) * 3;
        const double p = double(n[i]) / double(n[i + 2]);
        const double q = double(n[i + 1]) / double(n[i + 2]);
        cr::normal_moment_moments m;
        m.p = p; m.q = q; m.pp = p * p; m.pq = p * q; m.qq = q * q;
        return m;
    }

    // direct computation of a mip cell as the average of the base texels that
    // repeatedly halve down to it (ceil-halving is floor(child / 2))
    double direct_weight(unsigned base_w, unsigned base_h, unsigned level, unsigned x, unsigned y)
    {
        const unsigned sx = 1u << level;
        const unsigned sy = 1u << level;
        const unsigned x0 = x * sx, x1 = std::min(base_w, x0 + sx);
        const unsigned y0 = y * sy, y1 = std::min(base_h, y0 + sy);
        if (x1 <= x0 || y1 <= y0) return 0.0;
        return double(x1 - x0) * double(y1 - y0);
    }

    cr::normal_moment_moments direct_level_moments(const vector<float>& n, unsigned w, unsigned h,
                                                   unsigned level, unsigned x, unsigned y)
    {
        const unsigned step = 1u << level;
        cr::normal_moment_moments sum;
        double weight = 0;
        for (unsigned by = y * step; by < std::min(h, (y + 1) * step); by++) {
            for (unsigned bx = x * step; bx < std::min(w, (x + 1) * step); bx++) {
                const cr::normal_moment_moments m = direct_base_moments(n, w, h, bx, by);
                sum.p += m.p; sum.q += m.q; sum.pp += m.pp; sum.pq += m.pq; sum.qq += m.qq;
                weight += 1;
            }
        }
        cr::normal_moment_moments r;
        if (weight > 0) {
            r.p = sum.p / weight; r.q = sum.q / weight;
            r.pp = sum.pp / weight; r.pq = sum.pq / weight; r.qq = sum.qq / weight;
        }
        return r;
    }

    double repeat_mod(double i, unsigned size)
    {
        double m = std::fmod(i, double(size));
        if (m < 0) m += double(size);
        return m;
    }

    cr::normal_moment_moments direct_bilinear(const vector<float>& n, unsigned w, unsigned h,
                                              unsigned level, double u, double v)
    {
        const unsigned lw = (w + (1u << level) - 1u) >> level;
        const unsigned lh = (h + (1u << level) - 1u) >> level;
        const double px = u * double(lw) - 0.5;
        const double py = v * double(lh) - 0.5;
        const double fx0 = std::floor(px), fy0 = std::floor(py);
        const double fx = px - fx0, fy = py - fy0;
        const unsigned x0 = unsigned(repeat_mod(fx0, lw));
        const unsigned x1 = unsigned(repeat_mod(fx0 + 1, lw));
        const unsigned y0 = unsigned(repeat_mod(fy0, lh));
        const unsigned y1 = unsigned(repeat_mod(fy0 + 1, lh));
        const cr::normal_moment_moments m00 = direct_level_moments(n, w, h, level, x0, y0);
        const cr::normal_moment_moments m10 = direct_level_moments(n, w, h, level, x1, y0);
        const cr::normal_moment_moments m01 = direct_level_moments(n, w, h, level, x0, y1);
        const cr::normal_moment_moments m11 = direct_level_moments(n, w, h, level, x1, y1);
        const double w00 = (1 - fx) * (1 - fy), w10 = fx * (1 - fy);
        const double w01 = (1 - fx) * fy, w11 = fx * fy;
        cr::normal_moment_moments r;
        r.p = m00.p * w00 + m10.p * w10 + m01.p * w01 + m11.p * w11;
        r.q = m00.q * w00 + m10.q * w10 + m01.q * w01 + m11.q * w11;
        r.pp = m00.pp * w00 + m10.pp * w10 + m01.pp * w01 + m11.pp * w11;
        r.pq = m00.pq * w00 + m10.pq * w10 + m01.pq * w01 + m11.pq * w11;
        r.qq = m00.qq * w00 + m10.qq * w10 + m01.qq * w01 + m11.qq * w11;
        return r;
    }

    bool close(double a, double b, double eps = 1e-6)
    {
        return std::fabs(a - b) <= eps * std::max(1.0, std::max(std::fabs(a), std::fabs(b)));
    }

    double clamp01(double v) { return std::min(1.0, std::max(0.0, v)); }

    // standard 3D helpers used by the direct shading oracle
    double dot3(const double a[3], const double b[3]) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
    void cross3(const double a[3], const double b[3], double r[3])
    {
        r[0]=a[1]*b[2]-a[2]*b[1]; r[1]=a[2]*b[0]-a[0]*b[2]; r[2]=a[0]*b[1]-a[1]*b[0];
    }
    bool norm3(double a[3])
    {
        double l = std::sqrt(dot3(a,a));
        if (!(l > 0)) return false;
        a[0]/=l; a[1]/=l; a[2]/=l;
        return true;
    }

    // independent oracle for the lean specular/diffuse formula
    struct oracle_result
    {
        double r = 0, g = 0, b = 0;
        double mu_p = 0, mu_q = 0;
        double c_pp = 0, c_pq = 0, c_qq = 0;
        bool spec = false;
    };

    oracle_result oracle(const cr::normal_moment_moments& m,
                         double vx, double vy, double vz,
                         const double N[3], const double T[3], double handedness,
                         const cr::lean_material& mat, double u, double v)
    {
        oracle_result out;
        out.mu_p = m.p; out.mu_q = m.q;
        out.c_pp = m.pp - m.p * m.p;
        out.c_pq = m.pq - m.p * m.q;
        out.c_qq = m.qq - m.q * m.q;

        double n[3] = {N[0], N[1], N[2]};
        norm3(n);
        double t[3] = {T[0], T[1], T[2]};
        const double tn = dot3(t, n);
        for (int i = 0; i < 3; i++) t[i] -= tn * n[i];
        norm3(t);
        double b[3];
        cross3(n, t, b);
        for (int i = 0; i < 3; i++) b[i] *= handedness;

        // clamp covariance eigenvalues
        const double a = out.c_pp, bb = out.c_pq, d = out.c_qq;
        const double ht = 0.5 * (a + d);
        const double root = std::sqrt(std::max(0.0, 0.25 * (a - d) * (a - d) + bb * bb));
        double ev0 = std::max(0.0, ht - root), ev1 = std::max(0.0, ht + root);
        if (std::fabs(bb) > 1e-30) {
            double v0[2] = {bb, ev0 - a};
            double l = std::sqrt(v0[0]*v0[0] + v0[1]*v0[1]);
            v0[0] /= l; v0[1] /= l;
            out.c_pp = v0[0]*ev0*v0[0] + (-v0[1])*ev1*(-v0[1]);
            out.c_pq = v0[0]*ev0*v0[1] + (-v0[1])*ev1*v0[0];
            out.c_qq = v0[1]*ev0*v0[1] + v0[0]*ev1*v0[0];
        } else if (a > d) {
            out.c_pp = ev1; out.c_qq = ev0; out.c_pq = 0;
        } else {
            out.c_pp = ev0; out.c_qq = ev1; out.c_pq = 0;
        }

        const double shin = std::max(0.f, mat.shininess);
        const double base = 1.0 / (shin + 2.0);
        const double s_pp = out.c_pp + base, s_pq = out.c_pq, s_qq = out.c_qq + base;
        const double det = s_pp * s_qq - s_pq * s_pq;

        double L[3] = {mat.light[0], mat.light[1], mat.light[2]};
        double V[3] = {-vx, -vy, -vz};
        double H[3];
        bool lvh = norm3(L) && norm3(V);
        if (lvh) {
            H[0] = L[0]+V[0]; H[1] = L[1]+V[1]; H[2] = L[2]+V[2];
            lvh = norm3(H);
        }

        const double lz_n = dot3(L, n);
        double diff[3] = {
            mat.diffuse[0] * std::max(0.0, lz_n),
            mat.diffuse[1] * std::max(0.0, lz_n),
            mat.diffuse[2] * std::max(0.0, lz_n)
        };
        double spec[3] = {0,0,0};

        if (lvh) {
            const double axes[3][3] = {
                {t[0], b[0], n[0]},
                {t[1], b[1], n[1]},
                {t[2], b[2], n[2]}
            };
            double lt[3], vt[3], hht[3];
            for (int j = 0; j < 3; j++) {
                lt[j] = L[0]*axes[0][j] + L[1]*axes[1][j] + L[2]*axes[2][j];
                vt[j] = V[0]*axes[0][j] + V[1]*axes[1][j] + V[2]*axes[2][j];
                hht[j] = H[0]*axes[0][j] + H[1]*axes[1][j] + H[2]*axes[2][j];
            }
            const double hz = hht[2], lz = lt[2], vz = vt[2];
            if (hz > 0 && lz > 0 && vz > 0 && det > 0) {
                const double a0 = hht[0] / hz - out.mu_p;
                const double a1 = hht[1] / hz - out.mu_q;
                const double inv0 = s_qq / det, inv1 = -s_pq / det, inv2 = s_pp / det;
                double qf = a0 * (inv0*a0 + inv1*a1) + a1 * (inv1*a0 + inv2*a1);
                if (!(qf >= 0)) qf = 0;
                const double D = std::exp(-0.5*qf) / (2.0*M_PI*std::sqrt(det)*hz*hz*hz*hz);
                const double scale = D / (4.0*vz);
                spec[0] = mat.specular[0]*scale;
                spec[1] = mat.specular[1]*scale;
                spec[2] = mat.specular[2]*scale;
                out.spec = true;
            }
        }

        out.r = diff[0] + spec[0];
        out.g = diff[1] + spec[1];
        out.b = diff[2] + spec[2];
        (void)u; (void)v;
        return out;
    }

    // ------------------------------------------------------------------ fixtures
    struct lean_fixture
    {
        memory_canvas canvas;
        cr::context ctx;
        std::shared_ptr<cr::normal_moments> moments;
        std::shared_ptr<cr::lean_fragment_shader> lean;
        std::shared_ptr<capture_lean_shader> capture;

        lean_fixture(unsigned w, unsigned h, const vector<float>& normals, unsigned tw, unsigned th,
                     const cr::lean_material& mat) :
            canvas(), ctx(canvas, w, h),
            moments(std::make_shared<cr::normal_moments>(normals, tw, th)),
            lean(std::make_shared<cr::lean_fragment_shader>(moments, mat)),
            capture(std::make_shared<capture_lean_shader>(lean))
        {
            ctx.bind_vertex_shader(std::make_shared<cr::lean_vertex_shader>());
            ctx.bind_fragment_shader(capture);
            ctx.enable_fragment_gradients();
            ctx.enable_depth_test();          // before clearing: depth -> -1
            ctx.clear_framebuf(true, true);
        }
    };

    struct vertex_input
    {
        float clip[4]{-1.f, -1.f, 0.f, 1.f};
        float view[3]{0.f, 0.f, -1.f};
        float normal[3]{0.f, 0.f, 1.f};
        float tangent[3]{1.f, 0.f, 0.f};
        float handedness = 1.f;
        float u = 0.f, v = 0.f;
    };

    vector<float> pack(const vertex_input& v) {
        return cr::lean::pack_vertex(v.clip, v.view, v.normal, v.tangent, v.handedness, v.u, v.v);
    }
}

// ===================================================================== pyramid
static int case_pyramid_5x3(void)
{
    const unsigned w = 5, h = 3;
    const vector<float> n = crossed_texture(w, h);
    const cr::normal_moments moments(n, w, h);

    // level sizes, rounded up when halved
    TASK_CHECK(moments.level_count() == 4);
    TASK_CHECK(moments.level_size(0) == std::vector<unsigned>({5, 3}));
    TASK_CHECK(moments.level_size(1) == std::vector<unsigned>({3, 2}));
    TASK_CHECK(moments.level_size(2) == std::vector<unsigned>({2, 1}));
    TASK_CHECK(moments.level_size(3) == std::vector<unsigned>({1, 1}));

    // every cell equals the direct average of represented base texels
    for (unsigned l = 0; l < moments.level_count(); l++) {
        const auto size = moments.level_size(l);
        for (unsigned y = 0; y < size[1]; y++) {
            for (unsigned x = 0; x < size[0]; x++) {
                const cr::normal_moment_moments expect = direct_level_moments(n, w, h, l, x, y);
                const cr::normal_moment_moments got = moments.moments(l, x, y);
                TASK_CHECK(close(got.p, expect.p));
                TASK_CHECK(close(got.q, expect.q));
                TASK_CHECK(close(got.pp, expect.pp));
                TASK_CHECK(close(got.pq, expect.pq));
                TASK_CHECK(close(got.qq, expect.qq));
                TASK_CHECK(close(moments.cell_weight(l, x, y), direct_weight(w, h, l, x, y)));
            }
        }
    }

    // last cell holds the whole image statistics
    const cr::normal_moment_moments full = moments.moments(3, 0, 0);
    TASK_CHECK(moments.cell_weight(3, 0, 0) == double(w) * h);
    cr::normal_moment_moments direct_full = direct_level_moments(n, w, h, 3, 0, 0);
    TASK_CHECK(close(full.pp, direct_full.pp));
    TASK_CHECK(close(full.pq, direct_full.pq));

    // non-diagonal Sigma from the mixed moments (never a precomputed variance)
    const cr::normal_moment_moments m0 = moments.sample_level(0, 0.31, 0.72);
    const double mu_p = m0.p, mu_q = m0.q;
    const double c_pp = m0.pp - mu_p*mu_p;
    const double c_pq = m0.pq - mu_p*mu_q;
    const double c_qq = m0.qq - mu_q*mu_q;
    const cr::normal_moment_moments ref = direct_bilinear(n, w, h, 0, 0.31, 0.72);
    TASK_CHECK(close(c_pp, ref.pp - ref.p*ref.p));
    TASK_CHECK(close(c_pq, ref.pq - ref.p*ref.q));
    TASK_CHECK(close(c_qq, ref.qq - ref.q*ref.q));
    TASK_CHECK(std::fabs(c_pq) > 1e-4); // genuinely non-diagonal

    return 0;
}

static int case_pyramid_1xn(void)
{
    // a column texture: 1 x N base, last level must be the full-image average
    const unsigned w = 1, h = 6;
    vector<float> n(size_t(w) * h * 3);
    for (unsigned y = 0; y < h; y++) {
        const double p = -0.4 + 0.3 * y;
        const double q = 0.2 - 0.15 * y;
        n[(size_t(y) * w) * 3 + 0] = float(p);
        n[(size_t(y) * w) * 3 + 1] = float(q);
        n[(size_t(y) * w) * 3 + 2] = 1.f;
    }
    const cr::normal_moments moments(n, w, h);
    TASK_CHECK(moments.level_count() == 4);
    TASK_CHECK((moments.level_size(0) == std::vector<unsigned>({1, 6})));
    TASK_CHECK((moments.level_size(1) == std::vector<unsigned>({1, 3})));
    TASK_CHECK((moments.level_size(2) == std::vector<unsigned>({1, 2})));
    TASK_CHECK((moments.level_size(3) == std::vector<unsigned>({1, 1})));
    for (unsigned l = 0; l < moments.level_count(); l++) {
        const auto size = moments.level_size(l);
        for (unsigned y = 0; y < size[1]; y++) {
            for (unsigned x = 0; x < size[0]; x++) {
                const cr::normal_moment_moments e = direct_level_moments(n, w, h, l, x, y);
                const cr::normal_moment_moments g = moments.moments(l, x, y);
                TASK_CHECK(close(g.p, e.p) && close(g.q, e.q));
                TASK_CHECK(close(g.pp, e.pp) && close(g.pq, e.pq) && close(g.qq, e.qq));
                TASK_CHECK(close(moments.cell_weight(l, x, y), direct_weight(w, h, l, x, y)));
            }
        }
    }
    TASK_CHECK(close(moments.moments(3, 0, 0).p, -0.4 + 0.3 * 2.5));
    TASK_CHECK(close(moments.moments(3, 0, 0).q, 0.2 - 0.15 * 2.5));
    return 0;
}

static int case_pyramid_trilinear(void)
{
    // choose a footprint that selects an intermediate LOD and blend five moments
    const unsigned w = 8, h = 8;
    const vector<float> n = crossed_texture(w, h);
    const cr::normal_moments moments(n, w, h);

    // derivative giving rho = 2.5 -> lod = log2(2.5) ~ 1.3219
    const double rho = 2.5;
    const double du = rho / double(w);
    const double u = 0.23, v = 0.61;
    cr::normal_moment_footprint fp;
    const cr::normal_moment_moments s = moments.sample(u, v, du, 0.0, 0.0, 0.0, &fp);
    TASK_CHECK(close(fp.rho, rho));
    TASK_CHECK(close(fp.lod_raw, std::log2(rho)));
    TASK_CHECK(fp.level0 == 1 && fp.level1 == 2);
    TASK_CHECK(close(fp.weight1, std::log2(rho) - 1.0));

    const cr::normal_moment_moments m0 = direct_bilinear(n, w, h, 1, u, v);
    const cr::normal_moment_moments m1 = direct_bilinear(n, w, h, 2, u, v);
    const double t = fp.weight1;
    TASK_CHECK(close(s.p, (1-t)*m0.p + t*m1.p));
    TASK_CHECK(close(s.q, (1-t)*m0.q + t*m1.q));
    TASK_CHECK(close(s.pp, (1-t)*m0.pp + t*m1.pp));
    TASK_CHECK(close(s.pq, (1-t)*m0.pq + t*m1.pq));
    TASK_CHECK(close(s.qq, (1-t)*m0.qq + t*m1.qq));

    // mu and covariance are derived only after blending the raw moments
    const double mu_p = s.p, mu_q = s.q;
    const double c_pp = s.pp - mu_p*mu_p;
    const double c_pq = s.pq - mu_p*mu_q;
    const double c_qq = s.qq - mu_q*mu_q;
    const double ref_pp = (1-t)*m0.pp + t*m1.pp;
    TASK_CHECK(close(c_pp, ref_pp - mu_p*mu_p));
    TASK_CHECK(close(c_pq, (1-t)*m0.pq + t*m1.pq - mu_p*mu_q));
    TASK_CHECK(close(c_qq, (1-t)*m0.qq + t*m1.qq - mu_q*mu_q));

    // LOD is clamped into [0, last]
    const cr::normal_moment_footprint tiny = moments.footprint(0.0, 0.0, 0.0, 0.0);
    TASK_CHECK(tiny.lod == 0.0 && tiny.level0 == 0);
    const cr::normal_moment_footprint huge = moments.footprint(1e6, 0.0, 0.0, 0.0);
    TASK_CHECK(huge.lod == double(moments.last_level()));
    TASK_CHECK(huge.level0 == int(moments.last_level()));
    TASK_CHECK(huge.level1 == huge.level0);
    return 0;
}

// ==================================================================== footprint
static int case_footprint_quotient(void)
{
    // unequal w and negative UV: directly exercise the analytic quotient rule
    // a = A/w and i = 1/w are affine planes; expected derivatives are closed-form
    std::vector<float> n(3 * 4, 0.f);
    for (unsigned i = 0; i < 4; i++) n[i * 3 + 2] = 1.f;
    auto moments = std::make_shared<cr::normal_moments>(n, 2, 2);
    cr::lean_fragment_shader fs(moments, cr::lean_material{});

    // The production pipeline stores 1/w (an affine screen-space quantity) and
    // the divided attributes A * (1/w). Here both 1/w and the original UV are
    // affine planes with non-trivial slopes, an unequal w and negative UV.
    const int pix_x = 7, pix_y = 4;
    const int rx = 3, ry = 2;
    const double w0 = 2.0;
    const double i0 = 1.0 / w0;          // 1/w plane, affine
    const double ix = -0.021, iy = 0.017; // d(1/w)/dx, d(1/w)/dy
    const double A0 = -0.7, Ax = 0.02, Ay = 0.01; // original uv plane
    const double dxp = pix_x - rx, dyp = pix_y - ry;
    const double iw = i0 + ix * dxp + iy * dyp;
    const double wv = 1.0 / iw;
    const double Av = A0 + Ax * dxp + Ay * dyp;
    TASK_CHECK(std::fabs(wv - w0) > 0.05); // genuinely unequal w across the pixel

    vector<float> value(13, 0.f), dvx(13, 0.f), dvy(13, 0.f);
    value[5 + 2] = 0.5f * iw;  // normal z / w
    value[8 + 0] = 0.5f * iw;  // tangent x / w
    value[0] = float(Av * iw); // uv / w
    value[1] = 0.f;
    value[11] = float(iw);     // 1 / w
    value[12] = 1.f;

    // derivatives of the packed plane uv/w = A * (1/w)
    dvx[0] = float(Ax * iw + Av * ix);
    dvy[0] = float(Ay * iw + Av * iy);
    dvx[11] = float(ix);
    dvy[11] = float(iy);

    const cr::lean_restored_attributes r = fs.restore(value, dvx, dvy, true);
    TASK_CHECK(close(r.w, wv, 1e-5));
    TASK_CHECK(r.uv[0] < 0.0); // negative UV survives, no pre-modulo
    TASK_CHECK(close(r.uv[0], Av, 1e-5));

    // the analytic quotient rule recovers the derivatives of the original plane
    const double expect_dx = Ax;
    const double expect_dy = Ay;
    TASK_CHECK(close(r.uv_dx[0], expect_dx, 1e-4));
    TASK_CHECK(close(r.uv_dy[0], expect_dy, 1e-4));

    // zero-gradient path must yield zero derivatives and the same value
    cr::lean_restored_attributes z = fs.restore(value, dvx, dvy, false);
    TASK_CHECK(z.uv_dx[0] == 0.0 && z.uv_dy[0] == 0.0);
    TASK_CHECK(close(z.uv[0], Av, 1e-5));
    return 0;
}

static int case_footprint_repeat_and_lod(void)
{
    const unsigned w = 5, h = 3;
    const vector<float> n = crossed_texture(w, h);
    const cr::normal_moments moments(n, w, h);

    // repeat is addressing only: sampling at -0.3 equals sampling at 0.7
    const cr::normal_moment_moments a = moments.sample_level(0, -0.3, 0.4);
    const cr::normal_moment_moments b = moments.sample_level(0, 0.7, 0.4);
    TASK_CHECK(close(a.p, b.p) && close(a.q, b.q) && close(a.pp, b.pp)
                && close(a.pq, b.pq) && close(a.qq, b.qq));

    // and it equals direct repeat bilinear
    const cr::normal_moment_moments ref = direct_bilinear(n, w, h, 0, -0.3, 0.4);
    TASK_CHECK(close(a.p, ref.p) && close(a.pq, ref.pq));

    // derivatives are multiplied by the original image dimensions separately:
    // col_x = (du/dx * W, dv/dx * H), col_y = (du/dy * W, dv/dy * H);
    // rho is the largest singular value of J^T J built from those columns
    const double du = 0.3, dv = 0.5;
    const cr::normal_moment_footprint fp = moments.footprint(du, 0.0, 0.0, dv);
    const double cx0 = du * w, cx1 = 0.0;    // d(texel)/dx (u col, v col)
    const double cy0 = 0.0,  cy1 = dv * h;   // d(texel)/dy
    const double a00 = cx0*cx0 + cx1*cx1;
    const double a01 = cx0*cy0 + cx1*cy1;
    const double a11 = cy0*cy0 + cy1*cy1;
    const double half_tr = 0.5 * (a00 + a11);
    const double root = std::sqrt(std::max(0.0, 0.25*(a00-a11)*(a00-a11) + a01*a01));
    const double rho = std::sqrt(std::max(0.0, half_tr + root));
    TASK_CHECK(close(fp.rho, rho));
    TASK_CHECK(close(fp.lod_raw, std::log2(std::max(1.0, rho))));

    // a rotated (non-diagonal) Jacobian uses the largest singular value
    const cr::normal_moment_footprint rot = moments.footprint(0.5, 0.5, -0.5, 0.5);
    const double r00 = 0.5 * w, r10 = 0.5 * h;
    const double r01 = -0.5 * w, r11 = 0.5 * h;
    const double ra = r00*r00 + r10*r10;
    const double rb = r00*r01 + r10*r11;
    const double rd = r01*r01 + r11*r11;
    const double rh = 0.5 * (ra + rd);
    const double rr = std::sqrt(std::max(0.0, 0.25*(ra-rd)*(ra-rd) + rb*rb));
    TASK_CHECK(close(rot.rho, std::sqrt(std::max(0.0, rh + rr))));
    TASK_CHECK(std::fabs(rb) > 0.0); // genuinely non-diagonal Jacobian
    return 0;
}

static int case_footprint_viewport_and_winding(void)
{
    const unsigned tw = 6, th = 4;
    const vector<float> n = crossed_texture(tw, th);
    const cr::lean_material mat;
    lean_fixture f(48, 48, n, tw, th, mat);

    // local viewport offset and a non-full-canvas viewport size
    f.ctx.set_viewport(7, 5, 31, 29);

    vertex_input a, b, c;
    a.clip[0] = -0.8f; a.clip[1] = -0.6f; a.clip[2] = 0.2f; a.clip[3] = 2.f;
    b.clip[0] =  0.7f; b.clip[1] = -0.5f; b.clip[2] = 0.2f; b.clip[3] = 2.f;
    c.clip[0] =  0.1f; c.clip[1] =  0.9f; c.clip[2] = 0.2f; c.clip[3] = 2.f;
    a.u = -0.9f; a.v = -0.2f;
    b.u =  1.4f; b.v = -0.1f;
    c.u =  0.1f; c.v =  1.9f;
    a.view[0] = -1.f; a.view[1] = -1.f; a.view[2] = -3.f;
    b.view[0] =  1.f; b.view[1] = -1.f; b.view[2] = -3.f;
    c.view[0] =  0.f; c.view[1] =  1.f; c.view[2] = -3.f;

    // independently compute the quantized canvas vertices and attribute planes
    const unsigned qw = 31, qh = 29;
    const unsigned x1 = 7 + quantize_x(a.clip[0] / a.clip[3], qw + 1);
    const unsigned y1 = 5 + quantize_y(a.clip[1] / a.clip[3], qh + 1);
    const unsigned x2 = 7 + quantize_x(b.clip[0] / b.clip[3], qw + 1);
    const unsigned y2 = 5 + quantize_y(b.clip[1] / b.clip[3], qh + 1);
    const unsigned x3 = 7 + quantize_x(c.clip[0] / c.clip[3], qw + 1);
    const unsigned y3 = 5 + quantize_y(c.clip[1] / c.clip[3], qh + 1);

    cr::lean_vertex_shader vs;
    vector<float> p1, p2, p3;
    vs.run(pack(a), p1);
    vs.run(pack(b), p2);
    vs.run(pack(c), p3);

    const affine_set planes = affine_set::build(int(x1), int(y1), p1,
                                                int(x2), int(y2), p2,
                                                int(x3), int(y3), p3);

    f.capture->events.clear();
    f.ctx.draw_triangle(pack(a), pack(b), pack(c));
    f.ctx.update_canvas();
    TASK_CHECK(f.capture->gradient_calls > 0);

    bool found = false;
    for (const auto& e : f.capture->events) {
        if (!e.has_2d) return 1;
        // the values and derivatives must come from the same affine plane
        for (size_t i = 0; i < e.value.size(); i++) {
            const double expect_val = planes.planes[i].at(int(e.x), int(e.y), int(x1), int(y1));
            TASK_CHECK(close(e.value[i], expect_val, 1e-5));
            TASK_CHECK(close(e.dx[i], planes.planes[i].gx, 1e-5));
            TASK_CHECK(close(e.dy[i], planes.planes[i].gy, 1e-5));
        }
        // restored uv follows the plane after the quotient rule
        const double pw = planes.planes[cr::lean::out_inv_w].at(int(e.x), int(e.y), int(x1), int(y1));
        const double pu = planes.planes[cr::lean::out_uv].at(int(e.x), int(e.y), int(x1), int(y1));
        TASK_CHECK(close(e.diag.uv[0], pu / pw, 1e-5));
        found = true;
        break;
    }
    TASK_CHECK(found);

    // reversed winding must produce the very same affine plane at interior pixels
    f.ctx.clear_framebuf(true, true); // identical depth would fail the strict test
    f.capture->events.clear();
    f.ctx.draw_triangle(pack(c), pack(b), pack(a));
    f.ctx.update_canvas();
    TASK_CHECK(f.capture->gradient_calls > 0);
    bool winding_ok = false;
    const unsigned cx = (x1 + x2 + x3) / 3;
    const unsigned cy = (y1 + y2 + y3) / 3;
    for (const auto& e : f.capture->events) {
        if (!e.has_2d) return 1;
        if (e.x != cx || e.y != cy) continue;
        for (size_t i = 0; i < e.value.size(); i++) {
            TASK_CHECK(close(e.value[i], planes.planes[i].at(int(e.x), int(e.y), int(x1), int(y1)), 1e-5));
            TASK_CHECK(close(e.dx[i], planes.planes[i].gx, 1e-5));
            TASK_CHECK(close(e.dy[i], planes.planes[i].gy, 1e-5));
        }
        winding_ok = true;
        break;
    }
    TASK_CHECK(winding_ok);

    // Real draw with genuinely unequal w that varies across the triangle, plus
    // negative UV and a viewport offset. The restored UV and its derivative must
    // match the analytic quotient-rule value at every covered pixel.
    vertex_input a2, b2, c2;
    a2.clip[0] = -0.85f; a2.clip[1] = -0.75f; a2.clip[2] = 0.1f; a2.clip[3] = 3.f;
    b2.clip[0] =  0.80f; b2.clip[1] = -0.65f; b2.clip[2] = 0.1f; b2.clip[3] = 1.f;
    c2.clip[0] =  0.10f; c2.clip[1] =  0.85f; c2.clip[2] = 0.1f; c2.clip[3] = 2.f;
    a2.u = -1.6f; a2.v = -0.7f;
    b2.u =  2.1f; b2.v =  0.3f;
    c2.u = -0.2f; c2.v =  1.7f;
    a2.view[2] = b2.view[2] = c2.view[2] = -3.f;
    a2.view[0] = -1.f; a2.view[1] = -1.f;
    b2.view[0] =  1.f; b2.view[1] = -0.8f;
    c2.view[0] =  0.1f; c2.view[1] =  1.2f;

    // x/y/w envelopes stay inside [-1, 1]: |x|/w <= 0.85 < 1
    cr::lean_vertex_shader vs2;
    vector<float> q1, q2, q3;
    vs2.run(pack(a2), q1); vs2.run(pack(b2), q2); vs2.run(pack(c2), q3);

    const unsigned vx = 7, vy = 5, vw = 31, vh = 29;
    const unsigned ax = vx + quantize_x(a2.clip[0] / a2.clip[3], vw + 1);
    const unsigned ay = vy + quantize_y(a2.clip[1] / a2.clip[3], vh + 1);
    const unsigned bx = vx + quantize_x(b2.clip[0] / b2.clip[3], vw + 1);
    const unsigned by = vy + quantize_y(b2.clip[1] / b2.clip[3], vh + 1);
    const unsigned cx2 = vx + quantize_x(c2.clip[0] / c2.clip[3], vw + 1);
    const unsigned cy2 = vy + quantize_y(c2.clip[1] / c2.clip[3], vh + 1);

    const affine_set planes2 = affine_set::build(int(ax), int(ay), q1,
                                                 int(bx), int(by), q2,
                                                 int(cx2), int(cy2), q3);

    f.ctx.set_viewport(vx, vy, vw, vh);
    f.ctx.clear_framebuf(true, true);
    f.capture->events.clear();
    f.capture->gradient_calls = 0;
    f.ctx.draw_triangle(pack(a2), pack(b2), pack(c2));
    TASK_CHECK(f.capture->gradient_calls > 0);

    unsigned checked_pixels = 0;
    for (const auto& e : f.capture->events) {
        TASK_CHECK(e.has_2d);
        const auto& wplane = planes2.planes[cr::lean::out_inv_w];
        const auto& uplane = planes2.planes[cr::lean::out_uv];
        const auto& vplane = planes2.planes[cr::lean::out_uv + 1];
        const double iw = wplane.at(int(e.x), int(e.y), int(ax), int(ay));
        const double iw_x = wplane.gx, iw_y = wplane.gy;
        TASK_CHECK(std::fabs(iw - 1.0 / 3.0) > 1e-3); // w genuinely unequal
        const double pu = uplane.at(int(e.x), int(e.y), int(ax), int(ay));
        const double pv = vplane.at(int(e.x), int(e.y), int(ax), int(ay));
        TASK_CHECK(close(e.diag.uv[0], pu / iw, 1e-4));
        TASK_CHECK(close(e.diag.uv[1], pv / iw, 1e-4));
        // quotient rule for uv/x with u = pu/iw
        const double du_dx = (uplane.gx * iw - pu * iw_x) / (iw * iw);
        const double dv_dy = (vplane.gy * iw - pv * iw_y) / (iw * iw);
        TASK_CHECK(close(e.value[cr::lean::out_uv], pu, 1e-5)); // raw divided value
        TASK_CHECK(close(e.dx[cr::lean::out_uv], uplane.gx, 1e-5));
        TASK_CHECK(std::isfinite(du_dx) && std::isfinite(dv_dy));
        if (++checked_pixels >= 8) break;
    }
    TASK_CHECK(checked_pixels >= 8);
    return 0;
}

static int case_footprint_shared_and_resized(void)
{
    const unsigned tw = 4, th = 4;
    const vector<float> n = crossed_texture(tw, th);

    // owning texture: the source array is released before any shader uses it
    float* heap = new float[n.size()];
    std::copy(n.begin(), n.end(), heap);
    auto moments = std::make_shared<cr::normal_moments>(heap, tw, th);
    delete[] heap;

    cr::lean_material mat_a, mat_b;
    mat_a.diffuse[0] = 0.2f; mat_a.specular[0] = 1.f; mat_a.shininess = 0.f;
    mat_b.diffuse[1] = 0.4f; mat_b.specular[1] = 0.f; mat_b.shininess = 8.f;

    auto fs_a = std::make_shared<cr::lean_fragment_shader>(moments, mat_a);
    auto fs_b = std::make_shared<cr::lean_fragment_shader>(moments, mat_b);
    TASK_CHECK(fs_a->moments_texture() == moments);
    TASK_CHECK(fs_b->moments_texture() == moments);

    // both shaders read valid data after the source array is gone
    const cr::normal_moment_moments expected = direct_bilinear(n, tw, th, 0, 0.37, 0.81);
    const cr::normal_moment_moments sampled = moments->sample_level(0, 0.37, 0.81);
    TASK_CHECK(close(sampled.p, expected.p) && close(sampled.pq, expected.pq));

    // interleave the two shaders through a real context on a resized framebuffer
    memory_canvas canvas;
    cr::context ctx(canvas, 32, 32);
    auto cap_a = std::make_shared<capture_lean_shader>(fs_a);
    auto cap_b = std::make_shared<capture_lean_shader>(fs_b);
    ctx.bind_vertex_shader(std::make_shared<cr::lean_vertex_shader>());
    ctx.enable_depth_test();
    ctx.enable_fragment_gradients();

    vertex_input a, b, c;
    a.clip[0] = -0.9f; a.clip[1] = -0.8f; a.clip[2] = 0.1f;
    b.clip[0] =  0.9f; b.clip[1] = -0.7f; b.clip[2] = 0.1f;
    c.clip[0] =  0.2f; c.clip[1] =  0.9f; c.clip[2] = 0.1f;
    a.u = 0.1f; a.v = 0.1f; b.u = 1.2f; b.v = 0.2f; c.u = 0.5f; c.v = 1.5f;

    ctx.bind_fragment_shader(cap_a);
    ctx.disable_depth_test();
    ctx.clear_framebuf(true, false);
    ctx.draw_triangle(pack(a), pack(b), pack(c));
    const auto diag_a = fs_a->diagnostics();
    const unsigned count_a = cap_a->gradient_calls;

    ctx.bind_fragment_shader(cap_b);
    ctx.clear_framebuf(true, false);
    ctx.draw_triangle(pack(a), pack(b), pack(c));
    const auto diag_b = fs_b->diagnostics();
    TASK_CHECK(cap_a->gradient_calls == count_a); // no cross-talk
    TASK_CHECK(cap_b->gradient_calls > 0);
    TASK_CHECK(diag_a.cov_pq == diag_b.cov_pq); // same texture, same moments
    TASK_CHECK(diag_a.moments.p == diag_b.moments.p);

    // resize the framebuffer and change the viewport: values still follow the
    // current canvas through the quantized vertices
    ctx.bind_fragment_shader(cap_a);
    ctx.resize_framebuf(64, 60);
    ctx.set_viewport(3, 4, 58, 52);
    ctx.clear_framebuf(true, true);
    ctx.enable_depth_test();
    cap_a->events.clear();
    ctx.draw_triangle(pack(a), pack(b), pack(c));
    TASK_CHECK(cap_a->gradient_calls > 0);

    const unsigned qw = 58, qh = 52;
    const unsigned x1 = 3 + quantize_x(a.clip[0] / a.clip[3], qw + 1);
    const unsigned y1 = 4 + quantize_y(a.clip[1] / a.clip[3], qh + 1);
    const unsigned x2 = 3 + quantize_x(b.clip[0] / b.clip[3], qw + 1);
    const unsigned y2 = 4 + quantize_y(b.clip[1] / b.clip[3], qh + 1);
    const unsigned x3 = 3 + quantize_x(c.clip[0] / c.clip[3], qw + 1);
    const unsigned y3 = 4 + quantize_y(c.clip[1] / c.clip[3], qh + 1);
    cr::lean_vertex_shader vs;
    vector<float> p1, p2, p3;
    vs.run(pack(a), p1); vs.run(pack(b), p2); vs.run(pack(c), p3);
    const affine_set planes = affine_set::build(int(x1), int(y1), p1,
                                                int(x2), int(y2), p2,
                                                int(x3), int(y3), p3);

    bool checked = false;
    for (const auto& e : cap_a->events) {
        if (e.x <= x1 + 1) continue; // interior pixel
        TASK_CHECK(close(e.value[cr::lean::out_uv],
                         planes.planes[cr::lean::out_uv].at(int(e.x), int(e.y), int(x1), int(y1)), 1e-5));
        TASK_CHECK(close(e.dx[cr::lean::out_uv], planes.planes[cr::lean::out_uv].gx, 1e-5));
        checked = true;
        break;
    }
    TASK_CHECK(checked);
    return 0;
}

// ====================================================================== shading
static int case_shading_oracle(void)
{
    // direct call: w = 1, known view position, normal, tangent and handedness
    const unsigned tw = 4, th = 4;
    const vector<float> n = crossed_texture(tw, th);
    auto moments = std::make_shared<cr::normal_moments>(n, tw, th);

    cr::lean_material mat;
    mat.diffuse[0] = 0.25f; mat.diffuse[1] = 0.35f; mat.diffuse[2] = 0.2f;
    mat.specular[0] = 0.9f; mat.specular[1] = 0.8f; mat.specular[2] = 0.7f;
    mat.shininess = 6.f;
    mat.light[0] = 0.1f; mat.light[1] = 0.5f; mat.light[2] = 0.7f;
    cr::lean_fragment_shader fs(moments, mat);

    const double N[3] = {0.2, -0.1, 0.97};
    const double T[3] = {0.8, 0.15, 0.0}; // not orthogonal, not parallel: orthonormalized
    const double view[3] = {-0.3, 0.2, -2.2};
    const double u = 0.42, v = 0.66;

    vector<float> value(13, 0.f), dvx(13, 0.f), dvy(13, 0.f);
    value[0] = float(u); value[1] = float(v);
    for (int i = 0; i < 3; i++) {
        value[2 + i] = float(view[i]);
        value[5 + i] = float(N[i]);
        value[8 + i] = float(T[i]);
    }
    value[11] = 1.f;
    value[12] = 1.f;

    cr::fragment_gradient g{value, dvx, dvy, true, 1, 1};
    cr::color c = fs.run_gradient(g);
    const auto& diag = fs.diagnostics();

    const cr::normal_moment_moments m = moments->sample_level(0, u, v);
    const oracle_result expect = oracle(m, view[0], view[1], view[2], N, T, 1.0, mat, u, v);

    TASK_CHECK(close(diag.unclamped[0], expect.r, 1e-4));
    TASK_CHECK(close(diag.unclamped[1], expect.g, 1e-4));
    TASK_CHECK(close(diag.unclamped[2], expect.b, 1e-4));
    TASK_CHECK(diag.specular_active == expect.spec);
    TASK_CHECK(close(diag.mu_p, expect.mu_p));
    TASK_CHECK(close(diag.cov_pq, expect.c_pq));
    TASK_CHECK(close(diag.cov_pp, expect.c_pp) && close(diag.cov_qq, expect.c_qq));

    // clamped color equals the linear color clamped per channel
    TASK_CHECK(close(c.r, clamp01(expect.r), 1e-6));
    TASK_CHECK(close(c.g, clamp01(expect.g), 1e-6));
    TASK_CHECK(close(c.b, clamp01(expect.b), 1e-6));

    // mirrored handedness flips B, so the tangent-space H.y changes
    value[12] = -1.f;
    fs.run_gradient(g);
    const auto& dm = fs.diagnostics();
    const oracle_result em = oracle(m, view[0], view[1], view[2], N, T, -1.0, mat, u, v);
    const bool differs = std::fabs(em.g - expect.g) > 1e-6 || std::fabs(em.r - expect.r) > 1e-6;
    TASK_CHECK(differs); // the oracle differs, proving handedness matters
    TASK_CHECK(close(dm.unclamped[0], em.r, 1e-4));
    TASK_CHECK(close(dm.unclamped[1], em.g, 1e-4));

    // specular must vanish when H.z, L.z or V.z is non-positive
    cr::lean_material back = mat;
    back.light[0] = 0.f; back.light[1] = 0.f; back.light[2] = -1.f; // L.z < 0
    cr::lean_fragment_shader fs2(moments, back);
    fs2.run_gradient(g);
    TASK_CHECK(!fs2.diagnostics().specular_active);
    TASK_CHECK(close(fs2.diagnostics().unclamped[0], 0.0));

    // shininess >= 0, including the base-width limiter at zero
    cr::lean_material shiny = mat;
    shiny.shininess = 0.f;
    cr::lean_fragment_shader fs3(moments, shiny);
    fs3.run_gradient(g);
    TASK_CHECK(close(fs3.diagnostics().unclamped[0],
                     oracle(m, view[0], view[1], view[2], N, T, -1.0, shiny, u, v).r, 1e-4));
    return 0;
}

static int case_shading_real_draw(void)
{
    // real draw_triangle: pick an interior pixel, compare with an independent
    // analytic evaluation of the quantized attribute plane and moment formula
    const unsigned tw = 4, th = 4;
    const vector<float> n = crossed_texture(tw, th);

    cr::lean_material mat;
    mat.diffuse[0] = 0.3f; mat.diffuse[1] = 0.3f; mat.diffuse[2] = 0.3f;
    mat.specular[0] = 0.6f; mat.specular[1] = 0.9f; mat.specular[2] = 1.f;
    mat.shininess = 2.f;
    mat.light[0] = 0.2f; mat.light[1] = 0.35f; mat.light[2] = 0.9f;

    const unsigned cw = 64, ch = 64;
    lean_fixture f(cw, ch, n, tw, th, mat);

    vertex_input a, b, c;
    // clip w = 1 keeps the restored attributes exactly the supplied ones
    a.clip[0] = -0.95f; a.clip[1] = -0.9f; a.clip[2] = -0.4f; a.clip[3] = 1.f;
    b.clip[0] =  0.95f; b.clip[1] = -0.85f; b.clip[2] = -0.4f; b.clip[3] = 1.f;
    c.clip[0] =  0.1f;  c.clip[1] =  0.95f; c.clip[2] = -0.4f; c.clip[3] = 1.f;
    a.u = -0.2f; a.v = -0.1f;
    b.u =  1.3f; b.v =  0.0f;
    c.u =  0.4f; c.v =  1.2f;
    // a slanted surface and non-axis-aligned tangent
    a.view[0] = -1.4f; a.view[1] = -1.2f; a.view[2] = -2.6f;
    b.view[0] =  1.4f; b.view[1] = -1.0f; b.view[2] = -2.6f;
    c.view[0] =  0.2f; c.view[1] =  1.6f; c.view[2] = -2.6f;
    a.normal[0] = 0.1f; a.normal[1] = 0.0f; a.normal[2] = 1.f;
    b.normal[0] = -0.1f; b.normal[1] = 0.05f; b.normal[2] = 1.f;
    c.normal[0] = 0.0f; c.normal[1] = -0.1f; c.normal[2] = 1.f;
    a.tangent[0] = 1.f; a.tangent[1] = 0.1f; a.tangent[2] = 0.f;
    b.tangent[0] = 0.9f; b.tangent[1] = 0.2f; b.tangent[2] = 0.1f;
    c.tangent[0] = 0.7f; c.tangent[1] = 0.4f; c.tangent[2] = 0.f;

    f.capture->events.clear();
    f.ctx.draw_triangle(pack(a), pack(b), pack(c));
    f.ctx.update_canvas();
    TASK_CHECK(f.capture->gradient_calls > 0);

    // choose a well-interior pixel away from the excluded max edges
    const unsigned qw = cw, qh = ch;
    const unsigned x1 = quantize_x(a.clip[0], qw + 1), y1 = quantize_y(a.clip[1], qh + 1);
    const unsigned x2 = quantize_x(b.clip[0], qw + 1), y2 = quantize_y(b.clip[1], qh + 1);
    const unsigned x3 = quantize_x(c.clip[0], qw + 1), y3 = quantize_y(c.clip[1], qh + 1);

    cr::lean_vertex_shader vs;
    vector<float> p1, p2, p3;
    vs.run(pack(a), p1); vs.run(pack(b), p2); vs.run(pack(c), p3);
    const affine_set planes = affine_set::build(int(x1), int(y1), p1,
                                                int(x2), int(y2), p2,
                                                int(x3), int(y3), p3);

    // the pixel barycentrically deep inside the triangle
    const unsigned px = (x1 + x2 + x3) / 3;
    const unsigned py = (y1 + y2 + y3) / 3;
    const gradient_event* e = f.capture->at(px, py);
    TASK_CHECK(e != nullptr);
    if (!e) return 1;

    TASK_CHECK(close(e->diag.uv[0], planes.planes[cr::lean::out_uv].at(int(px), int(py), int(x1), int(y1)), 1e-4));
    TASK_CHECK(close(e->diag.uv[1], planes.planes[cr::lean::out_uv + 1].at(int(px), int(py), int(x1), int(y1)), 1e-4));

    // view position and normal from the same plane
    double vx = 0, vy = 0, vz = 0, N[3], T[3];
    for (int i = 0; i < 3; i++) {
        const int idx = int(px), idy = int(py);
        if (i == 0) vx = planes.planes[cr::lean::out_view_pos].at(idx, idy, int(x1), int(y1));
        if (i == 1) vy = planes.planes[cr::lean::out_view_pos + 1].at(idx, idy, int(x1), int(y1));
        if (i == 2) vz = planes.planes[cr::lean::out_view_pos + 2].at(idx, idy, int(x1), int(y1));
        N[i] = planes.planes[cr::lean::out_normal + i].at(idx, idy, int(x1), int(y1));
        T[i] = planes.planes[cr::lean::out_tangent + i].at(idx, idy, int(x1), int(y1));
    }

    // direct moment sampling at the texture coordinates of this pixel
    const double u = e->diag.uv[0], v = e->diag.uv[1];
    TASK_CHECK(e->diag.footprint.lod < 1e-6); // tiny footprint -> base level
    const cr::normal_moment_moments m = direct_bilinear(n, tw, th, 0, u, v);
    TASK_CHECK(close(e->diag.moments.p, m.p, 1e-4));
    TASK_CHECK(close(e->diag.moments.pq, m.pq, 1e-4));

    const oracle_result expect = oracle(m, vx, vy, vz, N, T, 1.0, mat, u, v);
    TASK_CHECK(close(e->diag.unclamped[0], expect.r, 1e-3));
    TASK_CHECK(close(e->diag.unclamped[1], expect.g, 1e-3));
    TASK_CHECK(close(e->diag.unclamped[2], expect.b, 1e-3));
    TASK_CHECK(close(e->out.r, std::min(1.0, std::max(0.0, expect.r)), 1e-6));

    // the pixel actually written into the normalized color buffer matches
    const unsigned fb_index = (ch - py - 1) * cw + px;
    TASK_CHECK(close(f.canvas.pixels[fb_index].r, e->out.r));
    TASK_CHECK(close(f.canvas.pixels[fb_index].g, e->out.g));
    TASK_CHECK(close(f.canvas.pixels[fb_index].b, e->out.b));

    // specular pixel visible, and the LOD of a large footprint follows rho
    TASK_CHECK(e->diag.specular_active);
    TASK_CHECK(std::isfinite(e->diag.footprint.lod));

    // enlarge derivative footprint: draw a small triangle with a big UV slope;
    // the captured LOD must match the analytic formula for the plane derivative
    f.capture->events.clear();
    vertex_input d = a, e2 = b, g2 = c;
    d.clip[0] = -0.3f; d.clip[1] = -0.2f;
    e2.clip[0] = 0.3f; e2.clip[1] = -0.2f;
    g2.clip[0] = 0.0f; g2.clip[1] = 0.4f;
    d.u = 0.0f; d.v = 0.0f; e2.u = 8.0f; e2.v = 0.0f; g2.u = 0.0f; g2.v = 8.0f;
    f.ctx.clear_framebuf(true, true);
    f.ctx.draw_triangle(pack(d), pack(e2), pack(g2));
    TASK_CHECK(f.capture->gradient_calls > 0);
    TASK_CHECK(!f.capture->events.empty());
    if (f.capture->events.empty()) return 1;

    const gradient_event& big = f.capture->events.front();
    TASK_CHECK(big.diag.footprint.rho > 1.0);
    const double expect_lod = std::min(double(f.moments->last_level()),
                                       std::log2(std::max(1.0, double(big.diag.footprint.rho))));
    TASK_CHECK(close(big.diag.footprint.lod, expect_lod, 1e-6));
    return 0;
}

static int case_shading_occlusion_and_legacy(void)
{
    const unsigned tw = 4, th = 4;
    const vector<float> n = crossed_texture(tw, th);
    cr::lean_material mat;
    mat.shininess = 0.f;

    memory_canvas canvas;
    cr::context ctx(canvas, 48, 40);
    auto moments = std::make_shared<cr::normal_moments>(n, tw, th);
    auto lean = std::make_shared<cr::lean_fragment_shader>(moments, mat);
    auto cap = std::make_shared<capture_lean_shader>(lean);
    ctx.bind_vertex_shader(std::make_shared<cr::lean_vertex_shader>());
    ctx.bind_fragment_shader(cap);
    ctx.enable_fragment_gradients();
    ctx.enable_depth_test();
    ctx.clear_framebuf(true, true);

    // the occluding front triangle uses a plain color shader
    auto color_counter = std::make_shared<count_color_shader>();
    vertex_input a, b, c;
    a.clip[2] = 0.8f; b.clip[2] = 0.8f; c.clip[2] = 0.8f;
    a.clip[0] = -0.8f; a.clip[1] = -0.7f;
    b.clip[0] = 0.8f; b.clip[1] = -0.7f;
    c.clip[0] = 0.f; c.clip[1] = 0.8f;
    vector<float> ca = {a.clip[0], a.clip[1], 0.8f, 1, 1, 1};
    vector<float> cb = {b.clip[0], b.clip[1], 0.8f, 1, 1, 1};
    vector<float> cc = {c.clip[0], c.clip[1], 0.8f, 1, 1, 1};
    ctx.bind_vertex_shader(std::make_shared<color_vs>());
    ctx.bind_fragment_shader(color_counter);
    ctx.draw_triangle(ca, cb, cc);
    TASK_CHECK(color_counter->calls > 0);

    // same covered pixels, behind: no lean fragment may run
    vertex_input far_a = a, far_b = b, far_c = c;
    far_a.clip[2] = -0.6f; far_b.clip[2] = -0.6f; far_c.clip[2] = -0.6f;
    far_a.u = 0.1f; far_b.u = 1.1f; far_c.u = 0.3f;
    far_a.v = 0.2f; far_b.v = 0.1f; far_c.v = 1.2f;
    far_a.view[2] = far_b.view[2] = far_c.view[2] = -2.f;
    far_a.view[0] = 0.f; far_b.view[0] = 0.f; far_c.view[0] = 0.f;
    far_a.view[1] = 0.f; far_b.view[1] = 0.f; far_c.view[1] = 0.f;

    ctx.bind_vertex_shader(std::make_shared<cr::lean_vertex_shader>());
    ctx.bind_fragment_shader(cap);
    cap->events.clear();
    ctx.draw_triangle(pack(far_a), pack(far_b), pack(far_c));
    TASK_CHECK(cap->gradient_calls == 0); // early depth test before fragment shader

    // re-enable interpolation, clear depth, and run the same lean triangle again
    ctx.clear_framebuf(false, true);
    cap->events.clear();
    ctx.draw_triangle(pack(far_a), pack(far_b), pack(far_c));
    TASK_CHECK(cap->gradient_calls > 0);
    const unsigned ok_calls = cap->gradient_calls;
    const unsigned ok_events = unsigned(cap->events.size());
    TASK_CHECK(ok_events == ok_calls);

    // legacy run(in) path of the same lean shader (no gradient input)
    ctx.bind_fragment_shader(lean);
    TASK_CHECK(lean->material().shininess == 0.f);

    // old color shader still works and uses run(in), not the gradient callback
    color_counter->calls = 0;
    ctx.bind_vertex_shader(std::make_shared<color_vs>());
    ctx.bind_fragment_shader(color_counter);
    ctx.disable_fragment_gradients();
    ctx.clear_framebuf(true, true);
    ctx.draw_triangle(ca, cb, cc);
    TASK_CHECK(color_counter->calls > 0);

    // gradient state must not linger on the old shader path
    ctx.enable_fragment_gradients();
    color_counter->calls = 0;
    ctx.clear_framebuf(true, true);
    ctx.draw_triangle(ca, cb, cc);
    TASK_CHECK(color_counter->calls > 0); // run(in) still used for old shaders

    // point and line with a gradient shader are explicitly marked without 2D gradients
    ctx.bind_fragment_shader(cap);
    ctx.bind_vertex_shader(std::make_shared<cr::lean_vertex_shader>());
    ctx.clear_framebuf(false, true); // place the point at a free depth
    cap->events.clear();
    cap->gradient_calls = 0;
    ctx.draw_point(pack(far_a));
    TASK_CHECK(cap->gradient_calls == 1);
    TASK_CHECK(!cap->events[0].has_2d);
    TASK_CHECK(cap->events[0].dx.size() == cap->events[0].value.size());
    for (float v : cap->events[0].dx) TASK_CHECK(v == 0.f);
    for (float v : cap->events[0].dy) TASK_CHECK(v == 0.f);

    cap->events.clear();
    ctx.draw_line(pack(far_a), pack(far_b));
    TASK_CHECK(cap->gradient_calls > 0);
    for (const auto& ev : cap->events) {
        TASK_CHECK(!ev.has_2d);
        for (float v : ev.dx) TASK_CHECK(v == 0.f);
        for (float v : ev.dy) TASK_CHECK(v == 0.f);
    }

    // interpolation disabled: first submitted vertex and zero gradient
    ctx.disable_interpolation();
    cap->events.clear();
    ctx.draw_triangle(pack(far_a), pack(far_b), pack(far_c));
    TASK_CHECK(cap->gradient_calls > 0);
    vector<float> first;
    cr::lean_vertex_shader vs;
    vs.run(pack(far_a), first);
    for (const auto& ev : cap->events) {
        TASK_CHECK(!ev.has_2d);
        TASK_CHECK(ev.value == first);
        for (float v : ev.dx) TASK_CHECK(v == 0.f);
    }

    // re-enabling interpolation restores full 2D gradients without residual state
    ctx.enable_fragment_gradients();
    ctx.clear_framebuf(false, true); // the same depth must pass the strict depth test
    cap->events.clear();
    cap->gradient_calls = 0;
    ctx.draw_triangle(pack(far_a), pack(far_b), pack(far_c));
    TASK_CHECK(cap->gradient_calls > 0);
    TASK_CHECK(!cap->events.empty());
    if (cap->events.empty()) return 1;
    TASK_CHECK(cap->events[0].has_2d);
    bool nonzero = false;
    for (float v : cap->events[0].dx) nonzero |= (v != 0.f);
    TASK_CHECK(nonzero);

    // restoring the legacy interpolation flag keeps old outputs unchanged
    ctx.bind_vertex_shader(std::make_shared<color_vs>());
    ctx.bind_fragment_shader(color_counter);
    ctx.disable_fragment_gradients();
    ctx.clear_framebuf(true, true);
    color_counter->calls = 0;
    ctx.draw_triangle(ca, cb, cc);
    TASK_CHECK(color_counter->calls > 0);
    ctx.update_canvas();
    TASK_CHECK(canvas.pixels.size() == size_t(48) * 40);
    return 0;
}

// keep the helper in scope for the real-draw case

// ------------------------------------------------------------------ registry
const struct task_case task_cases[] = {
    {"pyramid", "5x3_levels_and_full_statistics", case_pyramid_5x3},
    {"pyramid", "1xN_levels", case_pyramid_1xn},
    {"pyramid", "trilinear_moment_blend", case_pyramid_trilinear},
    {"footprint", "quotient_derivative_negative_uv", case_footprint_quotient},
    {"footprint", "repeat_and_lod", case_footprint_repeat_and_lod},
    {"footprint", "viewport_and_winding", case_footprint_viewport_and_winding},
    {"footprint", "shared_source_resized_canvas", case_footprint_shared_and_resized},
    {"shading", "oracle_direct", case_shading_oracle},
    {"shading", "real_draw_pixel_and_lod", case_shading_real_draw},
    {"shading", "occlusion_legacy_and_state", case_shading_occlusion_and_legacy},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
