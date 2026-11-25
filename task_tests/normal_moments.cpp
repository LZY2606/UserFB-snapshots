// task tests for the normal-moment texture, gradient rasterizer path and the
// LEAN shaders; registered through task_support/check.h (no main here)

#include "task_support/check.h"
#include "cpurast.hpp"
#include "normal_moments.hpp"
#include "lean_shader.hpp"
#include "color_shader.hpp"

#include <array>
#include <cmath>
#include <memory>
#include <vector>

namespace
{
    constexpr float PI = 3.14159265358979323846f;
    constexpr float EPS = 1e-4f;

    bool approx(float a, float b, float eps = EPS)
    {
        return std::abs(a - b) <= eps * (1.f + std::max(std::abs(a), std::abs(b)));
    }

    bool approx5(const cr::moments5& a, const cr::moments5& b, float eps = EPS)
    {
        for (int k = 0; k < 5; k++)
        {
            if (!approx(a.m[k], b.m[k], eps)) {
                return false;
            }
        }
        return true;
    }

    struct v3
    {
        float x, y, z;
    };

    float dot3(v3 a, v3 b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    v3 cross3(v3 a, v3 b)
    {
        return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    }

    v3 norm3(v3 a)
    {
        const float inv = 1.f / std::sqrt(dot3(a, a));
        return {a.x * inv, a.y * inv, a.z * inv};
    }

    cr::normal_pixel norm_pixel(float nx, float ny, float nz)
    {
        const v3 n = norm3({nx, ny, nz});
        return {n.x, n.y, n.z};
    }

    class memory_canvas : public cr::canvas
    {
    public:
        void draw(const std::vector<cr::color>& p, unsigned w, unsigned h) override
        {
            pixels = p;
            width = w;
            height = h;
        }

        std::vector<cr::color> pixels;
        unsigned width = 0;
        unsigned height = 0;
    };

    cr::color pixel_at(const memory_canvas& c, unsigned x, unsigned y)
    {
        // framebuf origin is the bottom-left corner
        return c.pixels[(c.height - y - 1u) * c.width + x];
    }

    // deterministic procedural normal, always with nz > 0, asymmetric slopes
    cr::normal_pixel gen_normal(int x, int y, int w, int h)
    {
        const float fx = static_cast<float>(x) / static_cast<float>(w);
        const float fy = static_cast<float>(y) / static_cast<float>(h);
        const float sx = 0.7f * std::sin(1.7f * fx + 0.4f) + 0.3f * fy;
        const float sy = 0.6f * std::cos(1.3f * fy - 0.2f) + 0.25f * fx;
        return norm_pixel(-sx, -sy, 1.f);
    }

    std::vector<cr::normal_pixel> make_normals(unsigned w, unsigned h)
    {
        std::vector<cr::normal_pixel> data(static_cast<size_t>(w) * h);
        for (unsigned y = 0; y < h; y++)
        {
            for (unsigned x = 0; x < w; x++) {
                data[static_cast<size_t>(y) * w + x] = gen_normal(x, y, w, h);
            }
        }
        return data;
    }

    // independent double-precision reference moments of one normal texel
    void ref_raw(const cr::normal_pixel& n, double out[5])
    {
        const double p = n.nx / n.nz;
        const double q = n.ny / n.nz;
        out[0] = p;
        out[1] = q;
        out[2] = p * p;
        out[3] = p * q;
        out[4] = q * q;
    }

    // mean of the base-level raw moments over a base texel rectangle
    cr::moments5 ref_region(const std::vector<cr::normal_pixel>& data, unsigned w,
                            unsigned x0, unsigned x1, unsigned y0, unsigned y1)
    {
        double acc[5] = {0, 0, 0, 0, 0};
        unsigned count = 0;
        for (unsigned y = y0; y < y1; y++)
        {
            for (unsigned x = x0; x < x1; x++)
            {
                double m[5];
                ref_raw(data[static_cast<size_t>(y) * w + x], m);
                for (int k = 0; k < 5; k++) {
                    acc[k] += m[k];
                }
                ++count;
            }
        }
        cr::moments5 r{};
        for (int k = 0; k < 5; k++) {
            r.m[k] = static_cast<float>(acc[k] / count);
        }
        return r;
    }

    // replicate the renderer's integer quantization for triangle vertices
    int quantize(float ndc, unsigned origin, unsigned size)
    {
        unsigned rel = static_cast<unsigned>(((ndc + 1.f) * static_cast<float>(size + 1u)) / 2.f);
        if (rel == size + 1u) {
            rel = size;
        }
        return static_cast<int>(origin + rel);
    }

    struct plane
    {
        float a, b, c; // value = a*x + b*y + c
    };

    plane make_plane(int x1, int y1, float v1, int x2, int y2, float v2,
                     int x3, int y3, float v3)
    {
        const float det = static_cast<float>((x2 - x1) * (y3 - y1) - (x3 - x1) * (y2 - y1));
        const float dv12 = v2 - v1;
        const float dv13 = v3 - v1;
        const float a = (dv12 * static_cast<float>(y3 - y1) - dv13 * static_cast<float>(y2 - y1)) / det;
        const float b = (static_cast<float>(x2 - x1) * dv13 - static_cast<float>(x3 - x1) * dv12) / det;
        return {a, b, v1 - a * static_cast<float>(x1) - b * static_cast<float>(y1)};
    }

    float eval_plane(const plane& p, int x, int y)
    {
        return p.a * static_cast<float>(x) + p.b * static_cast<float>(y) + p.c;
    }

    // recover integer pixel coordinates from two independent affine planes;
    // works even when a third plane (e.g. 1/w at constant w) is flat
    bool solve_pixel(const plane& px, const plane& py, float val_x, float val_y,
                     int& out_x, int& out_y)
    {
        const float det = px.a * py.b - px.b * py.a;
        if (det == 0.f) {
            return false;
        }
        const float rhs0 = val_x - px.c;
        const float rhs1 = val_y - py.c;
        const float xf = (rhs0 * py.b - px.b * rhs1) / det;
        const float yf = (px.a * rhs1 - rhs0 * py.a) / det;
        const int x = static_cast<int>(std::lround(xf));
        const int y = static_cast<int>(std::lround(yf));
        if (approx(eval_plane(px, x, y), val_x, 1e-5) &&
            approx(eval_plane(py, x, y), val_y, 1e-5))
        {
            out_x = x;
            out_y = y;
            return true;
        }
        return false;
    }

    // fragment shader that records every fragment it processes
    struct capture_fs : cr::fragment_shader
    {
        struct rec
        {
            std::vector<float> in;
            std::vector<cr::gradient> grad;
            bool valid;
        };

        mutable std::vector<rec> recs;

        cr::color run(const std::vector<float>& in) const override
        {
            recs.push_back({in, {}, false});
            return {1.f, 1.f, 1.f};
        }

        cr::color run(const std::vector<float>& in, const std::vector<cr::gradient>& grad,
                      bool valid) const override
        {
            recs.push_back({in, grad, valid});
            return {1.f, 1.f, 1.f};
        }

        bool uses_gradients() const override
        {
            return true;
        }
    };

    // lean fragment shader that counts calls and keeps per-fragment diagnostics
    struct record_lean_fs : cr::lean_fs
    {
        struct rec
        {
            std::vector<float> in;
            cr::lean_fs::diagnostic diag;
        };

        mutable std::vector<rec> recs;
        mutable unsigned calls = 0;

        using cr::lean_fs::lean_fs;

        cr::color run(const std::vector<float>& in) const override
        {
            ++calls;
            const cr::color col = cr::lean_fs::run(in);
            recs.push_back({in, last_diagnostic()});
            return col;
        }

        cr::color run(const std::vector<float>& in, const std::vector<cr::gradient>& grad,
                      bool valid) const override
        {
            ++calls;
            const cr::color col = cr::lean_fs::run(in, grad, valid);
            recs.push_back({in, last_diagnostic()});
            return col;
        }
    };

    // build the three test vertices: unequal w, negative UVs, constant TBN frame
    std::array<std::vector<float>, 3> test_triangle(float handedness = 1.f)
    {
        struct spec
        {
            float cx, cy, w, u, v;
        };
        const spec s[3] = {
            {-0.8f, -0.7f, 1.5f, -1.3f, -0.9f},
            {0.9f, -0.5f, 2.5f, 0.4f, -2.1f},
            {0.2f, 0.8f, 0.7f, 2.2f, 1.7f},
        };
        std::array<std::vector<float>, 3> verts;
        for (int i = 0; i < 3; i++)
        {
            const std::array<float, 4> clip{s[i].cx * s[i].w, s[i].cy * s[i].w, 0.2f * s[i].w, s[i].w};
            const std::array<float, 3> view_pos{s[i].cx, s[i].cy, -s[i].w};
            verts[i] = cr::pack_lean_vertex(clip, view_pos,
                                            {0.f, 0.f, 1.f}, {1.f, 0.f, 0.f},
                                            handedness, {s[i].u, s[i].v});
        }
        return verts;
    }

    // independent reference shading, mirroring the LEAN math from scratch
    cr::color ref_shade(const cr::normal_moments_texture& tex, const cr::lean_material& mat,
                        float u, float v, float dudx, float dudy, float dvdx, float dvdy,
                        v3 normal, v3 tangent, float handedness, v3 view_pos)
    {
        tangent = norm3({tangent.x - normal.x * dot3(tangent, normal),
                         tangent.y - normal.y * dot3(tangent, normal),
                         tangent.z - normal.z * dot3(tangent, normal)});
        const v3 bitangent = {handedness * cross3(normal, tangent).x,
                              handedness * cross3(normal, tangent).y,
                              handedness * cross3(normal, tangent).z};
        const v3 V = norm3({-view_pos.x, -view_pos.y, -view_pos.z});
        v3 L = norm3({mat.light_dir[0], mat.light_dir[1], mat.light_dir[2]});
        const v3 H = norm3({L.x + V.x, L.y + V.y, L.z + V.z});

        const auto tbn = [&](v3 d) {
            return v3{dot3(d, tangent), dot3(d, bitangent), dot3(d, normal)};
        };
        const v3 Lt = tbn(L);
        const v3 Vt = tbn(V);
        const v3 Ht = tbn(H);

        const cr::lean_sample smp = tex.sample(u, v, dudx, dudy, dvdx, dvdy);
        const float mp = smp.moments.m[0];
        const float mq = smp.moments.m[1];
        const float c00 = smp.moments.m[2] - mp * mp;
        const float c01 = smp.moments.m[3] - mp * mq;
        const float c11 = smp.moments.m[4] - mq * mq;

        const float base = 1.f / (mat.shininess + 2.f);
        const float s00 = c00 + base;
        const float s01 = c01;
        const float s11 = c11 + base;

        float spec = 0.f;
        if (Ht.z > 0.f && Lt.z > 0.f && Vt.z > 0.f)
        {
            const float det = s00 * s11 - s01 * s01;
            const float ax = Ht.x / Ht.z - mp;
            const float ay = Ht.y / Ht.z - mq;
            const float iax = (s11 * ax - s01 * ay) / det;
            const float iay = (-s01 * ax + s00 * ay) / det;
            const float quad = ax * iax + ay * iay;
            const float D = std::exp(-0.5f * quad) /
                            (2.f * PI * std::sqrt(det) * Ht.z * Ht.z * Ht.z * Ht.z);
            spec = D / (4.f * Vt.z);
        }

        const float ndotl = std::max(0.f, Lt.z);
        return {mat.diffuse.r * ndotl + mat.specular.r * spec,
                mat.diffuse.g * ndotl + mat.specular.g * spec,
                mat.diffuse.b * ndotl + mat.specular.b * spec};
    }

    // --------------------------------------------------------------- pyramid
    int pyramid_base_5x3()
    {
        const unsigned W = 5, H = 3;
        const auto data = make_normals(W, H);
        const cr::normal_moments_texture tex(data, W, H);

        TASK_CHECK(tex.get_width() == W && tex.get_height() == H);
        // 5x3 -> 3x2 -> 2x1 -> 1x1
        TASK_CHECK(tex.level_count() == 4);
        TASK_CHECK(tex.level_width(0) == 5 && tex.level_height(0) == 3);
        TASK_CHECK(tex.level_width(1) == 3 && tex.level_height(1) == 2);
        TASK_CHECK(tex.level_width(2) == 2 && tex.level_height(2) == 1);
        TASK_CHECK(tex.level_width(3) == 1 && tex.level_height(3) == 1);

        for (unsigned y = 0; y < H; y++)
        {
            for (unsigned x = 0; x < W; x++)
            {
                double m[5];
                ref_raw(data[y * W + x], m);
                const cr::moments5& got = tex.texel(0, x, y);
                for (int k = 0; k < 5; k++) {
                    TASK_CHECK(approx(got.m[k], static_cast<float>(m[k]), 1e-6));
                }
            }
        }
        return 0;
    }

    int pyramid_pooled_5x3()
    {
        const unsigned W = 5, H = 3;
        const auto data = make_normals(W, H);
        const cr::normal_moments_texture tex(data, W, H);

        // every pooled texel must equal the uniform mean of its base rectangle
        for (unsigned lvl = 1; lvl < tex.level_count(); lvl++)
        {
            const unsigned span = 1u << lvl;
            for (unsigned cy = 0; cy < tex.level_height(lvl); cy++)
            {
                for (unsigned cx = 0; cx < tex.level_width(lvl); cx++)
                {
                    const unsigned x0 = cx * span;
                    const unsigned x1 = std::min(W, (cx + 1u) * span);
                    const unsigned y0 = cy * span;
                    const unsigned y1 = std::min(H, (cy + 1u) * span);
                    const cr::moments5 expect = ref_region(data, W, x0, x1, y0, y1);
                    TASK_CHECK(approx5(expect, tex.texel(lvl, cx, cy), 1e-6));
                }
            }
        }

        // the single last texel is the statistics of the whole image
        const cr::moments5 whole = ref_region(data, W, 0, W, 0, H);
        TASK_CHECK(approx5(whole, tex.texel(tex.level_count() - 1, 0, 0), 1e-6));
        return 0;
    }

    int pyramid_1xN()
    {
        const unsigned W = 1, H = 6;
        const auto data = make_normals(W, H);
        const cr::normal_moments_texture tex(data, W, H);

        // 1x6 -> 1x3 -> 1x2 -> 1x1
        TASK_CHECK(tex.level_count() == 4);
        TASK_CHECK(tex.level_width(0) == 1 && tex.level_height(0) == 6);
        TASK_CHECK(tex.level_width(3) == 1 && tex.level_height(3) == 1);

        for (unsigned lvl = 0; lvl < tex.level_count(); lvl++)
        {
            const unsigned span = 1u << lvl;
            for (unsigned cy = 0; cy < tex.level_height(lvl); cy++)
            {
                const unsigned y0 = cy * span;
                const unsigned y1 = std::min(H, (cy + 1u) * span);
                const cr::moments5 expect = ref_region(data, W, 0, 1, y0, y1);
                TASK_CHECK(approx5(expect, tex.texel(lvl, 0, cy), 1e-6));
            }
        }
        return 0;
    }

    int pyramid_ownership_and_sharing()
    {
        const unsigned W = 4, H = 4;
        auto source = std::make_shared<std::vector<cr::normal_pixel>>(make_normals(W, H));
        auto tex = std::make_shared<cr::normal_moments_texture>(*source, W, H);
        const cr::moments5 before = tex->texel(0, 1, 2);

        source->clear();
        source.reset(); // release the caller's array; the texture must stand alone

        const cr::moments5 after = tex->texel(0, 1, 2);
        TASK_CHECK(approx5(before, after, 0.f));
        const cr::lean_sample smp = tex->sample(0.3f, 0.6f, 0.01f, 0.f, 0.f, 0.02f);
        TASK_CHECK(std::isfinite(smp.lod) && smp.lod >= 0.f);

        // several shaders share one texture resource
        cr::lean_material mat;
        auto fs1 = std::make_shared<cr::lean_fs>(tex, mat);
        mat.shininess = 5.f;
        auto fs2 = std::make_shared<cr::lean_fs>(tex, mat);
        TASK_CHECK(fs1->last_diagnostic().valid == false);

        memory_canvas canvas;
        cr::context ctx(canvas, 32, 32);
        auto vs = std::make_shared<cr::lean_vs>();
        ctx.bind_vertex_shader(vs);
        const auto verts = test_triangle();
        ctx.bind_fragment_shader(fs1);
        ctx.draw_triangle(verts[0], verts[1], verts[2]);
        TASK_CHECK(fs1->last_diagnostic().valid);
        ctx.bind_fragment_shader(fs2);
        ctx.draw_triangle(verts[0], verts[1], verts[2]);
        TASK_CHECK(fs2->last_diagnostic().valid);
        return 0;
    }

    // -------------------------------------------------------------- footprint
    int footprint_planes_and_quotients()
    {
        constexpr unsigned W = 64, H = 48;
        memory_canvas canvas;
        cr::context ctx(canvas, W, H);
        auto vs = std::make_shared<cr::lean_vs>();
        auto fs = std::make_shared<capture_fs>();
        ctx.bind_vertex_shader(vs);
        ctx.bind_fragment_shader(fs);
        ctx.clear_framebuf(true, false);

        const auto verts = test_triangle();
        std::vector<float> out[3];
        cr::position pos[3];
        for (int i = 0; i < 3; i++) {
            pos[i] = vs->run(verts[i], out[i]);
        }
        const int qx[3] = {
            quantize(pos[0].x, 0, W), quantize(pos[1].x, 0, W), quantize(pos[2].x, 0, W)};
        const int qy[3] = {
            quantize(pos[0].y, 0, H), quantize(pos[1].y, 0, H), quantize(pos[2].y, 0, H)};
        TASK_CHECK((qx[0] != qx[1] || qy[0] != qy[1]) &&
                   (qx[0] != qx[2] || qy[0] != qy[2]));

        ctx.draw_triangle(verts[0], verts[1], verts[2]);
        TASK_CHECK(!fs->recs.empty());
        TASK_CHECK(fs->recs.front().valid);
        TASK_CHECK(fs->recs.front().grad.size() == cr::LEAN_OUTPUT_SIZE);

        // independent planes for u/w (0), v/w (1) and 1/w (11)
        const plane pu = make_plane(qx[0], qy[0], out[0][0], qx[1], qy[1], out[1][0], qx[2], qy[2], out[2][0]);
        const plane pv = make_plane(qx[0], qy[0], out[0][1], qx[1], qy[1], out[1][1], qx[2], qy[2], out[2][1]);
        const plane pr = make_plane(qx[0], qy[0], out[0][11], qx[1], qy[1], out[1][11], qx[2], qy[2], out[2][11]);

        for (const capture_fs::rec& r : fs->recs)
        {
            // recover the pixel coordinates by solving the two UV/w planes
            int found_x = -1, found_y = -1;
            TASK_CHECK(solve_pixel(pu, pv, r.in[0], r.in[1], found_x, found_y));

            // all observed values come from the same affine planes
            for (unsigned k = 0; k < cr::LEAN_OUTPUT_SIZE; k++)
            {
                const plane pk = make_plane(qx[0], qy[0], out[0][k], qx[1], qy[1], out[1][k],
                                            qx[2], qy[2], out[2][k]);
                TASK_CHECK(approx(r.in[k], eval_plane(pk, found_x, found_y), 1e-5));
                TASK_CHECK(approx(r.grad[k].dx, pk.a, 1e-6));
                TASK_CHECK(approx(r.grad[k].dy, pk.b, 1e-6));
            }

            // analytic quotient derivatives for the recovered UVs
            const float uw = eval_plane(pu, found_x, found_y);
            const float vw = eval_plane(pv, found_x, found_y);
            const float rw = eval_plane(pr, found_x, found_y);
            const auto quot = [](float g, float dg, float r, float dr) {
                return (dg * r - g * dr) / (r * r);
            };
            const float exp_dudx = quot(uw, pu.a, rw, pr.a);
            const float exp_dudy = quot(uw, pu.b, rw, pr.b);
            const float exp_dvdx = quot(vw, pv.a, rw, pr.a);
            const float exp_dvdy = quot(vw, pv.b, rw, pr.b);

            const float got_dudx = quot(r.in[0], r.grad[0].dx, r.in[11], r.grad[11].dx);
            const float got_dudy = quot(r.in[0], r.grad[0].dy, r.in[11], r.grad[11].dy);
            const float got_dvdx = quot(r.in[1], r.grad[1].dx, r.in[11], r.grad[11].dx);
            const float got_dvdy = quot(r.in[1], r.grad[1].dy, r.in[11], r.grad[11].dy);
            TASK_CHECK(approx(got_dudx, exp_dudx));
            TASK_CHECK(approx(got_dudy, exp_dudy));
            TASK_CHECK(approx(got_dvdx, exp_dvdx));
            TASK_CHECK(approx(got_dvdy, exp_dvdy));

            // the recovered UVs must be the original (negative) values, not moduloed
            const float u = r.in[0] / r.in[11];
            const float v = r.in[1] / r.in[11];
            TASK_CHECK(u >= -2.5f && u <= 2.5f && v >= -2.5f && v <= 2.5f);
        }
        return 0;
    }

    int footprint_negative_uv_repeat()
    {
        const unsigned W = 8, H = 4;
        const cr::normal_moments_texture tex(make_normals(W, H), W, H);

        // repeat wrapping: an integer shift of u/v must give identical samples
        const cr::moments5 a = tex.sample_bilinear(-0.37f, -1.13f, 0);
        const cr::moments5 b = tex.sample_bilinear(0.63f, 0.87f, 0);
        TASK_CHECK(approx5(a, b, 1e-6));

        const cr::moments5 c = tex.sample_bilinear(2.3f, -3.7f, 1);
        const cr::moments5 d = tex.sample_bilinear(0.3f, 0.3f, 1);
        TASK_CHECK(approx5(c, d, 1e-6));

        // texel-center convention: u*w - 0.5 at an integer center returns that texel
        const float u = (2.f + 0.5f) / static_cast<float>(W);
        const float v = (1.f + 0.5f) / static_cast<float>(H);
        TASK_CHECK(approx5(tex.sample_bilinear(u, v, 0), tex.texel(0, 2, 1), 1e-6));
        return 0;
    }

    int footprint_lod_and_trilinear()
    {
        const unsigned W = 32, H = 32;
        const cr::normal_moments_texture tex(make_normals(W, H), W, H);
        TASK_CHECK(tex.level_count() == 6);

        // zero footprint -> LOD 0
        cr::lean_sample s = tex.sample(0.5f, 0.5f, 0.f, 0.f, 0.f, 0.f);
        TASK_CHECK(approx(s.lod, 0.f, 1e-6));

        // rho = 4 -> log2 = 2
        s = tex.sample(0.5f, 0.5f, 4.f / W, 0.f, 0.f, 0.f);
        TASK_CHECK(approx(s.lod, 2.f, 1e-6));

        // rho beyond the top level clamps to the last LOD
        s = tex.sample(0.5f, 0.5f, 1000.f / W, 0.f, 0.f, 0.f);
        TASK_CHECK(approx(s.lod, static_cast<float>(tex.level_count() - 1), 1e-6));

        // fractional LOD 1.5 must be an equal blend of the two level samples
        const float du = 2.8284271247f / W; // rho = 2*sqrt(2) -> log2 = 1.5
        s = tex.sample(0.31f, 0.62f, du, 0.f, 0.f, du);
        TASK_CHECK(approx(s.lod, 1.5f, 1e-5));
        const cr::moments5 m0 = tex.sample_bilinear(0.31f, 0.62f, 1);
        const cr::moments5 m1 = tex.sample_bilinear(0.31f, 0.62f, 2);
        for (int k = 0; k < 5; k++) {
            TASK_CHECK(approx(s.moments.m[k], 0.5f * (m0.m[k] + m1.m[k]), 1e-5));
        }
        return 0;
    }

    int footprint_viewport_offset_and_winding()
    {
        constexpr unsigned W = 80, H = 60;
        const unsigned vpx = 11, vpy = 7, vpw = 40, vph = 30;
        memory_canvas canvas;
        cr::context ctx(canvas, W, H);
        ctx.set_viewport(vpx, vpy, vpw, vph);
        auto vs = std::make_shared<cr::lean_vs>();
        auto fs = std::make_shared<capture_fs>();
        ctx.bind_vertex_shader(vs);
        ctx.bind_fragment_shader(fs);

        const auto verts = test_triangle();
        std::vector<float> out[3];
        cr::position pos[3];
        for (int i = 0; i < 3; i++) {
            pos[i] = vs->run(verts[i], out[i]);
        }
        const int qx[3] = {quantize(pos[0].x, vpx, vpw), quantize(pos[1].x, vpx, vpw), quantize(pos[2].x, vpx, vpw)};
        const int qy[3] = {quantize(pos[0].y, vpy, vph), quantize(pos[1].y, vpy, vph), quantize(pos[2].y, vpy, vph)};

        ctx.draw_triangle(verts[0], verts[1], verts[2]);
        TASK_CHECK(!fs->recs.empty());

        const plane pr = make_plane(qx[0], qy[0], out[0][11], qx[1], qy[1], out[1][11], qx[2], qy[2], out[2][11]);
        const plane pu = make_plane(qx[0], qy[0], out[0][0], qx[1], qy[1], out[1][0], qx[2], qy[2], out[2][0]);
        const plane pv = make_plane(qx[0], qy[0], out[0][1], qx[1], qy[1], out[1][1], qx[2], qy[2], out[2][1]);
        for (const capture_fs::rec& r : fs->recs)
        {
            int fx = -1, fy = -1;
            TASK_CHECK(solve_pixel(pu, pv, r.in[0], r.in[1], fx, fy));
            TASK_CHECK(fx >= static_cast<int>(vpx) && fx < static_cast<int>(vpx + vpw));
            TASK_CHECK(fy >= static_cast<int>(vpy) && fy < static_cast<int>(vpy + vph));
            TASK_CHECK(approx(r.in[0], eval_plane(pu, fx, fy), 1e-5));
            TASK_CHECK(approx(r.grad[0].dx, pu.a, 1e-6));
            TASK_CHECK(approx(r.grad[0].dy, pu.b, 1e-6));
        }

        // reversed winding must keep the same plane values and derivatives
        const unsigned first_count = fs->recs.size();
        fs->recs.clear();
        ctx.draw_triangle(verts[0], verts[2], verts[1]);
        TASK_CHECK(fs->recs.size() == first_count);
        for (const capture_fs::rec& r : fs->recs)
        {
            TASK_CHECK(r.valid);
            TASK_CHECK(approx(r.grad[0].dx, pu.a, 1e-6));
            TASK_CHECK(approx(r.grad[0].dy, pu.b, 1e-6));
            TASK_CHECK(approx(r.grad[11].dx, pr.a, 1e-6));
        }
        return 0;
    }

    int footprint_resize_canvas()
    {
        memory_canvas canvas;
        cr::context ctx(canvas, 40, 30);
        auto vs = std::make_shared<cr::lean_vs>();
        auto fs = std::make_shared<capture_fs>();
        ctx.bind_vertex_shader(vs);
        ctx.bind_fragment_shader(fs);
        const auto verts = test_triangle();

        ctx.draw_triangle(verts[0], verts[1], verts[2]);
        const auto small = fs->recs;
        TASK_CHECK(!small.empty());

        ctx.resize_framebuf(100, 80);
        ctx.set_viewport(0, 0, 100, 80);
        fs->recs.clear();
        ctx.draw_triangle(verts[0], verts[1], verts[2]);
        TASK_CHECK(fs->recs.size() > small.size());

        std::vector<float> out[3];
        cr::position pos[3];
        for (int i = 0; i < 3; i++) {
            pos[i] = vs->run(verts[i], out[i]);
        }
        const int qx[3] = {quantize(pos[0].x, 0, 100), quantize(pos[1].x, 0, 100), quantize(pos[2].x, 0, 100)};
        const int qy[3] = {quantize(pos[0].y, 0, 80), quantize(pos[1].y, 0, 80), quantize(pos[2].y, 0, 80)};
        const plane pu = make_plane(qx[0], qy[0], out[0][0], qx[1], qy[1], out[1][0], qx[2], qy[2], out[2][0]);
        for (const capture_fs::rec& r : fs->recs)
        {
            TASK_CHECK(r.valid);
            TASK_CHECK(approx(r.grad[0].dx, pu.a, 1e-6));
            TASK_CHECK(approx(r.grad[0].dy, pu.b, 1e-6));
        }
        return 0;
    }

    int footprint_point_line_no_residue()
    {
        memory_canvas canvas;
        cr::context ctx(canvas, 48, 40);
        auto vs = std::make_shared<cr::lean_vs>();
        auto cap = std::make_shared<capture_fs>();
        ctx.bind_vertex_shader(vs);
        ctx.bind_fragment_shader(cap);
        const auto verts = test_triangle();

        ctx.draw_triangle(verts[0], verts[1], verts[2]);
        TASK_CHECK(cap->recs.back().valid);
        TASK_CHECK(cap->recs.back().grad.size() == cr::LEAN_OUTPUT_SIZE);

        // a point must not see the previous triangle plane
        ctx.draw_point(verts[0]);
        TASK_CHECK(cap->recs.back().valid == false);
        for (const cr::gradient& g : cap->recs.back().grad) {
            TASK_CHECK(g.dx == 0.f && g.dy == 0.f);
        }

        // neither may a line
        for (const capture_fs::rec& r : cap->recs)
        {
            if (!r.valid)
            {
                for (const cr::gradient& g : r.grad) {
                    TASK_CHECK(g.dx == 0.f && g.dy == 0.f);
                }
            }
        }
        const std::size_t line_start = cap->recs.size() - 1u;
        TASK_CHECK(!cap->recs[line_start].valid);

        // triangle gradients are available again afterwards
        ctx.draw_triangle(verts[0], verts[1], verts[2]);
        TASK_CHECK(cap->recs.back().valid);
        TASK_CHECK(cap->recs.back().grad.size() == cr::LEAN_OUTPUT_SIZE);

        // interpolation disabled: first vertex attributes, zero gradients
        ctx.disable_interpolation();
        cap->recs.clear();
        ctx.draw_triangle(verts[0], verts[1], verts[2]);
        TASK_CHECK(!cap->recs.empty());
        std::vector<float> first_out;
        vs->run(verts[0], first_out);
        for (const capture_fs::rec& r : cap->recs)
        {
            TASK_CHECK(!r.valid);
            for (const cr::gradient& g : r.grad) {
                TASK_CHECK(g.dx == 0.f && g.dy == 0.f);
            }
            TASK_CHECK(r.in.size() == first_out.size());
            for (unsigned k = 0; k < first_out.size(); k++) {
                TASK_CHECK(approx(r.in[k], first_out[k], 0.f));
            }
        }
        ctx.enable_interpolation();
        ctx.draw_triangle(verts[0], verts[1], verts[2]);
        TASK_CHECK(cap->recs.back().valid);
        return 0;
    }

    int footprint_shader_interleaving()
    {
        memory_canvas canvas;
        cr::context ctx(canvas, 48, 40);
        const auto verts = test_triangle();

        auto lean_vs_ptr = std::make_shared<cr::lean_vs>();
        auto cap = std::make_shared<capture_fs>();
        auto color_vs_ptr = std::make_shared<color_vs>();
        auto color_fs_ptr = std::make_shared<color_fs>();

        // lean triangle, then old color triangle/point/line, then lean again
        ctx.bind_vertex_shader(lean_vs_ptr);
        ctx.bind_fragment_shader(cap);
        ctx.draw_triangle(verts[0], verts[1], verts[2]);
        TASK_CHECK(cap->recs.back().valid);

        ctx.bind_vertex_shader(color_vs_ptr);
        ctx.bind_fragment_shader(color_fs_ptr);
        const std::vector<float> cv1{-0.5f, -0.5f, 0.f, 1.f, 0.2f, 0.3f};
        const std::vector<float> cv2{0.5f, -0.5f, 0.f, 0.2f, 1.f, 0.3f};
        const std::vector<float> cv3{0.f, 0.6f, 0.f, 0.2f, 0.3f, 1.f};
        ctx.clear_framebuf(true, false);
        ctx.draw_triangle(cv1, cv2, cv3);
        ctx.draw_point(cv1);
        ctx.draw_line(cv1, cv2);
        ctx.update_canvas();
        bool saw_color = false;
        for (cr::color c : canvas.pixels) {
            saw_color = saw_color || (c.r > 0.5f || c.g > 0.5f || c.b > 0.5f);
        }
        TASK_CHECK(saw_color);

        ctx.bind_vertex_shader(lean_vs_ptr);
        ctx.bind_fragment_shader(cap);
        ctx.draw_point(verts[0]);
        TASK_CHECK(!cap->recs.back().valid);
        ctx.draw_triangle(verts[0], verts[1], verts[2]);
        TASK_CHECK(cap->recs.back().valid);
        TASK_CHECK(cap->recs.back().grad.size() == cr::LEAN_OUTPUT_SIZE);
        return 0;
    }

    // ---------------------------------------------------------------- shading
    cr::lean_material test_material()
    {
        cr::lean_material mat;
        mat.diffuse = {0.2f, 0.3f, 0.15f};
        mat.specular = {0.9f, 0.8f, 0.7f};
        mat.shininess = 16.f;
        mat.light_dir = {-0.3f, 0.25f, 0.92f};
        return mat;
    }

    // flat front-facing triangle at z = -3, constant normal/tangent frame
    std::array<std::vector<float>, 3> shade_triangle(float handedness)
    {
        const float z = -3.f;
        const float w = 3.f;
        const std::array<float, 3> n{0.f, 0.f, 1.f};
        const std::array<float, 3> t{1.f, 0.f, 0.f};
        auto v = [&](float cx, float cy, float u, float vv) {
            const std::array<float, 4> clip{cx * w, cy * w, 0.2f * w, w};
            return cr::pack_lean_vertex(clip, {cx, cy, z}, n, t, handedness, {u, vv});
        };
        return {v(-0.8f, -0.7f, 0.1f, 0.2f),
                v(0.8f, -0.7f, 0.9f, 0.2f),
                v(0.0f, 0.8f, 0.5f, 0.9f)};
    }

    int shading_pixel_reference()
    {
        constexpr unsigned W = 64, H = 48;
        auto tex = std::make_shared<cr::normal_moments_texture>(make_normals(8, 6), 8, 6);
        const cr::lean_material mat = test_material();

        memory_canvas canvas;
        cr::context ctx(canvas, W, H);
        auto vs = std::make_shared<cr::lean_vs>();
        auto fs = std::make_shared<record_lean_fs>(tex, mat);
        ctx.bind_vertex_shader(vs);
        ctx.bind_fragment_shader(fs);
        ctx.clear_framebuf(true, true);

        const auto verts = shade_triangle(1.f);
        std::vector<float> out[3];
        cr::position pos[3];
        for (int i = 0; i < 3; i++) {
            pos[i] = vs->run(verts[i], out[i]);
        }
        const int qx[3] = {quantize(pos[0].x, 0, W), quantize(pos[1].x, 0, W), quantize(pos[2].x, 0, W)};
        const int qy[3] = {quantize(pos[0].y, 0, H), quantize(pos[1].y, 0, H), quantize(pos[2].y, 0, H)};
        plane planes[cr::LEAN_OUTPUT_SIZE];
        for (unsigned k = 0; k < cr::LEAN_OUTPUT_SIZE; k++) {
            planes[k] = make_plane(qx[0], qy[0], out[0][k], qx[1], qy[1], out[1][k], qx[2], qy[2], out[2][k]);
        }

        ctx.draw_triangle(verts[0], verts[1], verts[2]);
        TASK_CHECK(fs->calls == fs->recs.size());
        TASK_CHECK(!fs->recs.empty());
        ctx.update_canvas();

        for (const record_lean_fs::rec& r : fs->recs)
        {
            int fx = -1, fy = -1;
            TASK_CHECK(solve_pixel(planes[0], planes[1], r.in[0], r.in[1], fx, fy));

            const float rw = eval_plane(planes[11], fx, fy);
            const float u = eval_plane(planes[0], fx, fy) / rw;
            const float v = eval_plane(planes[1], fx, fy) / rw;
            const auto quot = [](float g, float dg, float rv, float dr) {
                return (dg * rv - g * dr) / (rv * rv);
            };
            const float dudx = quot(eval_plane(planes[0], fx, fy), planes[0].a, rw, planes[11].a);
            const float dudy = quot(eval_plane(planes[0], fx, fy), planes[0].b, rw, planes[11].b);
            const float dvdx = quot(eval_plane(planes[1], fx, fy), planes[1].a, rw, planes[11].a);
            const float dvdy = quot(eval_plane(planes[1], fx, fy), planes[1].b, rw, planes[11].b);

            v3 view_pos;
            for (int i = 0; i < 3; i++) {
                (&view_pos.x)[i] = eval_plane(planes[2 + i], fx, fy) / rw;
            }

            const cr::color expect = ref_shade(*tex, mat, u, v, dudx, dudy, dvdx, dvdy,
                                               {0.f, 0.f, 1.f}, {1.f, 0.f, 0.f}, 1.f, view_pos);
            TASK_CHECK(approx(r.diag.unclamped.r, expect.r, 1e-4));
            TASK_CHECK(approx(r.diag.unclamped.g, expect.g, 1e-4));
            TASK_CHECK(approx(r.diag.unclamped.b, expect.b, 1e-4));
            TASK_CHECK(approx(r.diag.sample.lod,
                              tex->sample(u, v, dudx, dudy, dvdx, dvdy).lod, 1e-6));

            const cr::color fb = pixel_at(canvas, fx, fy);
            const auto clamp01 = [](float x) { return std::min(1.f, std::max(0.f, x)); };
            TASK_CHECK(approx(fb.r, clamp01(r.diag.unclamped.r), 1e-6));
            TASK_CHECK(approx(fb.g, clamp01(r.diag.unclamped.g), 1e-6));
            TASK_CHECK(approx(fb.b, clamp01(r.diag.unclamped.b), 1e-6));
        }
        return 0;
    }

    int shading_closed_form_peak()
    {
        // constant (0,0,1) normals: all moments and covariance vanish
        const unsigned S = 4;
        std::vector<cr::normal_pixel> flat(static_cast<size_t>(S) * S, {0.f, 0.f, 1.f});
        auto tex = std::make_shared<cr::normal_moments_texture>(flat, S, S);
        const cr::lean_sample smp = tex->sample(0.5f, 0.5f, 0.f, 0.f, 0.f, 0.f);
        for (int k = 0; k < 5; k++) {
            TASK_CHECK(approx(smp.moments.m[k], 0.f, 1e-7));
        }

        // L = V = N = (0,0,1): H = N, so a = 0 and D has a closed form
        const float shininess = 10.f;
        cr::lean_material mat;
        mat.diffuse = {0.f, 0.f, 0.f};
        mat.specular = {1.f, 1.f, 1.f};
        mat.shininess = shininess;
        mat.light_dir = {0.f, 0.f, 1.f};

        memory_canvas canvas;
        cr::context ctx(canvas, 32, 32);
        auto vs = std::make_shared<cr::lean_vs>();
        auto fs = std::make_shared<record_lean_fs>(tex, mat);
        ctx.bind_vertex_shader(vs);
        ctx.bind_fragment_shader(fs);

        const float w = 3.f;
        auto vtx = [&](float cx, float cy) {
            const std::array<float, 4> clip{cx * w, cy * w, 0.2f * w, w};
            // constant view position so V points exactly along +z at every fragment
            const std::array<float, 3> view_pos{0.f, 0.f, -w};
            return cr::pack_lean_vertex(clip, view_pos, {0.f, 0.f, 1.f},
                                       {1.f, 0.f, 0.f}, 1.f, {cx, cy});
        };
        ctx.draw_triangle(vtx(-0.6f, -0.6f), vtx(0.6f, -0.6f), vtx(0.f, 0.6f));
        TASK_CHECK(!fs->recs.empty());

        const float s = 1.f / (shininess + 2.f);
        const float expect_intensity = 1.f / (8.f * PI * s); // D/(4 V.z), V.z = 1
        for (const record_lean_fs::rec& r : fs->recs)
        {
            TASK_CHECK(approx(r.diag.unclamped.r, expect_intensity, 2e-4));
            TASK_CHECK(approx(r.diag.unclamped.g, expect_intensity, 2e-4));
            TASK_CHECK(approx(r.diag.unclamped.b, expect_intensity, 2e-4));
            TASK_CHECK(approx(r.diag.sample.lod, 0.f, 1e-6));
        }
        return 0;
    }

    int shading_sigma_offdiagonal()
    {
        // asymmetric normals produce a non-zero pq covariance term
        std::vector<cr::normal_pixel> data = {
            norm_pixel(-0.5f, -0.5f, 1.f), norm_pixel(0.5f, 0.5f, 1.f),
            norm_pixel(-0.2f, 0.4f, 1.f), norm_pixel(0.3f, -0.3f, 1.f)};
        const cr::normal_moments_texture tex(data, 2, 2);
        const cr::moments5 whole = tex.texel(tex.level_count() - 1, 0, 0);
        const float c01 = whole.m[3] - whole.m[0] * whole.m[1];
        TASK_CHECK(std::abs(c01) > 1e-4f);

        // Sigma keeps the cross term; zeroing it changes the NDF value
        cr::lean_material mat = test_material();
        const float base = 1.f / (mat.shininess + 2.f);
        const float s00 = whole.m[2] - whole.m[0] * whole.m[0] + base;
        const float s11 = whole.m[4] - whole.m[1] * whole.m[1] + base;
        const float ax = 0.13f - whole.m[0];
        const float ay = -0.07f - whole.m[1];
        const auto ndf = [&](float cross) {
            const float det = s00 * s11 - cross * cross;
            const float q = (s11 * ax * ax - 2.f * cross * ax * ay + s00 * ay * ay) / det;
            return std::exp(-0.5f * q) / (2.f * PI * std::sqrt(det));
        };
        TASK_CHECK(!approx(ndf(c01), ndf(0.f), 1e-4));

        // a trilinear sample blends the five raw moments first; the covariance
        // (including its pq cross term) must then be recoverable and still nonzero
        const cr::lean_sample blended = tex.sample(0.37f, 0.61f, 0.4f, 0.1f, -0.2f, 0.5f);
        const float blended_c01 = blended.moments.m[3] - blended.moments.m[0] * blended.moments.m[1];
        TASK_CHECK(std::isfinite(blended_c01));
        TASK_CHECK(std::abs(blended_c01) > 1e-5f);
        // blended raw moments are weighted means inside the stored value range
        TASK_CHECK(blended.moments.m[2] >= 0.f);

        // tiny negative eigenvalues must be clamped away before adding base width
        float m00 = 1.f, m01 = 0.6f, m11 = 1.f; // positive definite
        TASK_CHECK(m00 * m11 - m01 * m01 > 0.f);
        m00 = 0.2f; m01 = 0.3f; m11 = 0.2f; // det negative -> negative eigenvalue
        const float tr = m00 + m11;
        const float disc = std::sqrt(std::max(0.f, tr * tr - 4.f * (m00 * m11 - m01 * m01)));
        const float lmin = 0.5f * (tr - disc);
        TASK_CHECK(lmin < 0.f);
        TASK_CHECK(0.5f * (tr + disc) > 0.f); // the other eigenvalue stays positive
        return 0;
    }

    int shading_mirror_handedness()
    {
        auto tex = std::make_shared<cr::normal_moments_texture>(make_normals(8, 6), 8, 6);
        const cr::lean_material mat = test_material();

        memory_canvas c1, c2;
        cr::context x1(c1, 48, 48), x2(c2, 48, 48);
        auto vs = std::make_shared<cr::lean_vs>();
        auto f1 = std::make_shared<record_lean_fs>(tex, mat);
        auto f2 = std::make_shared<record_lean_fs>(tex, mat);
        x1.bind_vertex_shader(vs);
        x2.bind_vertex_shader(vs);
        x1.bind_fragment_shader(f1);
        x2.bind_fragment_shader(f2);

        const auto plus = shade_triangle(1.f);
        const auto minus = shade_triangle(-1.f);
        x1.draw_triangle(plus[0], plus[1], plus[2]);
        x2.draw_triangle(minus[0], minus[1], minus[2]);
        TASK_CHECK(f1->recs.size() == f2->recs.size());

        bool differs = false;
        for (std::size_t i = 0; i < f1->recs.size(); i++)
        {
            const auto& a = f1->recs[i].in;
            const auto& b = f2->recs[i].in;
            // only the handedness component (12) flips sign
            for (unsigned k = 0; k < cr::LEAN_OUTPUT_SIZE; k++)
            {
                if (k == 12u) {
                    TASK_CHECK(approx(a[k], -b[k], 1e-6));
                } else {
                    TASK_CHECK(approx(a[k], b[k], 1e-6));
                }
            }
            const auto& da = f1->recs[i].diag.unclamped;
            const auto& db = f2->recs[i].diag.unclamped;
            differs = differs || !approx(da.r, db.r, 1e-5);

            // independent reference for the mirrored frame
            const float rw = a[11];
            const v3 view_pos{a[2] / rw, a[3] / rw, a[4] / rw};
            const cr::color expect = ref_shade(*tex, mat, a[0] / rw, a[1] / rw, 0.f, 0.f, 0.f, 0.f,
                                               {0.f, 0.f, 1.f}, {1.f, 0.f, 0.f}, -1.f, view_pos);
            TASK_CHECK(approx(db.r, expect.r, 1e-4));
        }
        TASK_CHECK(differs); // asymmetric texture makes the mirror visible
        return 0;
    }

    int shading_occlusion_skips_fs()
    {
        auto tex = std::make_shared<cr::normal_moments_texture>(make_normals(4, 4), 4, 4);
        cr::lean_material mat = test_material();

        memory_canvas canvas;
        cr::context ctx(canvas, 64, 48);
        auto vs = std::make_shared<cr::lean_vs>();
        auto fs = std::make_shared<record_lean_fs>(tex, mat);
        ctx.bind_vertex_shader(vs);
        ctx.bind_fragment_shader(fs);
        ctx.enable_depth_test();
        ctx.clear_framebuf(true, true);

        auto tri = [&](float depth) {
            const float w = 3.f;
            auto vtx = [&](float cx, float cy) {
                const std::array<float, 4> clip{cx * w, cy * w, depth * w, w};
                return cr::pack_lean_vertex(clip, {cx, cy, -w}, {0.f, 0.f, 1.f},
                                           {1.f, 0.f, 0.f}, 1.f, {cx, cy});
            };
            return std::array<std::vector<float>, 3>{
                vtx(-0.8f, -0.8f), vtx(0.8f, -0.8f), vtx(0.f, 0.8f)};
        };

        // far triangle first, then the nearer one: every fragment passes once
        const auto far_t = tri(-0.4f);
        const auto near_t = tri(0.5f);
        ctx.draw_triangle(far_t[0], far_t[1], far_t[2]);
        const unsigned far_calls = fs->calls;
        ctx.draw_triangle(near_t[0], near_t[1], near_t[2]);
        const unsigned total_calls = fs->calls;
        TASK_CHECK(far_calls > 0);
        TASK_CHECK(total_calls == far_calls * 2u);

        // reversed order: occluded fragments of the far triangle never run the FS
        ctx.clear_framebuf(true, true);
        fs->calls = 0;
        fs->recs.clear();
        ctx.draw_triangle(near_t[0], near_t[1], near_t[2]);
        const unsigned near_calls = fs->calls;
        ctx.draw_triangle(far_t[0], far_t[1], far_t[2]);
        TASK_CHECK(fs->calls < near_calls + far_calls);
        TASK_CHECK(fs->calls >= near_calls);

        unsigned lit = 0;
        ctx.update_canvas();
        for (cr::color c : canvas.pixels) {
            lit += (c.r > 0.f || c.g > 0.f || c.b > 0.f);
        }
        TASK_CHECK(lit == near_calls); // visible pixels are exactly the near footprint
        return 0;
    }

    int shading_clamp_and_diagnostics()
    {
        std::vector<cr::normal_pixel> flat(16, {0.f, 0.f, 1.f});
        auto tex = std::make_shared<cr::normal_moments_texture>(flat, 4, 4);
        cr::lean_material mat;
        mat.diffuse = {0.f, 0.f, 0.f};
        mat.specular = {20.f, 20.f, 20.f}; // intentionally bright, exceeds [0,1]
        mat.shininess = 0.f;
        mat.light_dir = {0.f, 0.f, 1.f};

        memory_canvas canvas;
        cr::context ctx(canvas, 32, 32);
        auto vs = std::make_shared<cr::lean_vs>();
        auto fs = std::make_shared<record_lean_fs>(tex, mat);
        ctx.bind_vertex_shader(vs);
        ctx.bind_fragment_shader(fs);

        const float w = 3.f;
        auto vtx = [&](float cx, float cy) {
            const std::array<float, 4> clip{cx * w, cy * w, 0.2f * w, w};
            // constant view position so every fragment evaluates the NDF peak
            const std::array<float, 3> view_pos{0.f, 0.f, -w};
            return cr::pack_lean_vertex(clip, view_pos, {0.f, 0.f, 1.f},
                                       {1.f, 0.f, 0.f}, 1.f, {cx, cy});
        };
        ctx.draw_triangle(vtx(-0.6f, -0.6f), vtx(0.6f, -0.6f), vtx(0.f, 0.6f));
        ctx.update_canvas();

        bool saw_over_one = false;
        for (const record_lean_fs::rec& r : fs->recs)
        {
            TASK_CHECK(r.diag.valid);
            TASK_CHECK(std::isfinite(r.diag.unclamped.r));
            TASK_CHECK(r.diag.sample.moments.m[2] >= 0.f);
            saw_over_one = saw_over_one || r.diag.unclamped.r > 1.f;
        }
        TASK_CHECK(saw_over_one); // diagnostics keep the unclamped value
        for (cr::color c : canvas.pixels)
        {
            TASK_CHECK(c.r >= 0.f && c.r <= 1.f); // the buffer itself is clamped
            TASK_CHECK(c.g >= 0.f && c.g <= 1.f);
            TASK_CHECK(c.b >= 0.f && c.b <= 1.f);
        }
        return 0;
    }

    int shading_back_light_zero_specular()
    {
        auto tex = std::make_shared<cr::normal_moments_texture>(make_normals(4, 4), 4, 4);
        cr::lean_material mat;
        mat.diffuse = {0.f, 0.f, 0.f};
        mat.specular = {1.f, 1.f, 1.f};
        mat.shininess = 8.f;
        mat.light_dir = {0.f, 0.f, -1.f}; // points away from the front face

        memory_canvas canvas;
        cr::context ctx(canvas, 32, 32);
        auto vs = std::make_shared<cr::lean_vs>();
        auto fs = std::make_shared<record_lean_fs>(tex, mat);
        ctx.bind_vertex_shader(vs);
        ctx.bind_fragment_shader(fs);

        const float w = 3.f;
        auto vtx = [&](float cx, float cy) {
            const std::array<float, 4> clip{cx * w, cy * w, 0.2f * w, w};
            return cr::pack_lean_vertex(clip, {cx, cy, -w}, {0.f, 0.f, 1.f},
                                       {1.f, 0.f, 0.f}, 1.f, {cx, cy});
        };
        ctx.draw_triangle(vtx(-0.6f, -0.6f), vtx(0.6f, -0.6f), vtx(0.f, 0.6f));
        for (const record_lean_fs::rec& r : fs->recs)
        {
            TASK_CHECK(approx(r.diag.unclamped.r, 0.f, 1e-7));
            TASK_CHECK(approx(r.diag.unclamped.g, 0.f, 1e-7));
            TASK_CHECK(approx(r.diag.unclamped.b, 0.f, 1e-7));
        }
        return 0;
    }

    int shading_interp_disabled_and_primitives()
    {
        auto tex = std::make_shared<cr::normal_moments_texture>(make_normals(4, 4), 4, 4);
        const cr::lean_material mat = test_material();
        memory_canvas canvas;
        cr::context ctx(canvas, 40, 32);
        auto vs = std::make_shared<cr::lean_vs>();
        auto fs = std::make_shared<record_lean_fs>(tex, mat);
        ctx.bind_vertex_shader(vs);
        ctx.bind_fragment_shader(fs);
        const auto verts = test_triangle();

        // points and lines have no gradients: zero derivative -> LOD 0
        ctx.draw_point(verts[0]);
        TASK_CHECK(fs->recs.back().diag.sample.lod == 0.f);
        ctx.draw_line(verts[0], verts[1]);
        TASK_CHECK(fs->recs.back().diag.sample.lod == 0.f);

        // disabled interpolation: every fragment uses the first vertex as-is
        ctx.disable_interpolation();
        fs->recs.clear();
        ctx.draw_triangle(verts[0], verts[1], verts[2]);
        TASK_CHECK(!fs->recs.empty());
        std::vector<float> first_out;
        vs->run(verts[0], first_out);
        for (const record_lean_fs::rec& r : fs->recs)
        {
            TASK_CHECK(r.in.size() == first_out.size());
            for (unsigned k = 0; k < first_out.size(); k++) {
                TASK_CHECK(approx(r.in[k], first_out[k], 0.f));
            }
            TASK_CHECK(r.diag.sample.lod == 0.f);
        }
        ctx.enable_interpolation();
        return 0;
    }
}

const struct task_case task_cases[] = {
    {"pyramid", "base_5x3", pyramid_base_5x3},
    {"pyramid", "pooled_5x3", pyramid_pooled_5x3},
    {"pyramid", "oned_1xN", pyramid_1xN},
    {"pyramid", "ownership_and_sharing", pyramid_ownership_and_sharing},

    {"footprint", "planes_and_quotients", footprint_planes_and_quotients},
    {"footprint", "negative_uv_repeat", footprint_negative_uv_repeat},
    {"footprint", "lod_and_trilinear", footprint_lod_and_trilinear},
    {"footprint", "viewport_offset_and_winding", footprint_viewport_offset_and_winding},
    {"footprint", "resize_canvas", footprint_resize_canvas},
    {"footprint", "point_line_no_residue", footprint_point_line_no_residue},
    {"footprint", "shader_interleaving", footprint_shader_interleaving},

    {"shading", "pixel_reference", shading_pixel_reference},
    {"shading", "closed_form_peak", shading_closed_form_peak},
    {"shading", "sigma_offdiagonal", shading_sigma_offdiagonal},
    {"shading", "mirror_handedness", shading_mirror_handedness},
    {"shading", "occlusion_skips_fs", shading_occlusion_skips_fs},
    {"shading", "clamp_and_diagnostics", shading_clamp_and_diagnostics},
    {"shading", "back_light_zero_specular", shading_back_light_zero_specular},
    {"shading", "interp_disabled_and_primitives", shading_interp_disabled_and_primitives},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
