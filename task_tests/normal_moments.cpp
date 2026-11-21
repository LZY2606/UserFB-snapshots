#include "cpurast.hpp"
#include "normal_moments.hpp"
#include "lean_shader.hpp"
#include "color_shader.hpp"
#include "task_support/check.h"

#include <array>
#include <cmath>
#include <memory>
#include <vector>

using cr::normal_moments_texture;
using cr::lean_vs;
using cr::lean_fs;
using std::array;
using std::vector;

namespace
{
    constexpr float EPS = 1e-4f;
    constexpr float EPS_LOOSE = 2e-3f;

    bool close(float a, float b, float eps = EPS)
    {
        return std::fabs(a - b) <= eps * (1.f + std::fabs(b));
    }

    bool close_moments(const normal_moments_texture::moments& a,
                       const normal_moments_texture::moments& b,
                       float eps = EPS)
    {
        for (unsigned k = 0; k < normal_moments_texture::MOMENT_COUNT; k++)
        {
            if (!close(a[k], b[k], eps)) return false;
        }
        return true;
    }

    // independent direct moments of a normal triple: p = nx/nz, q = ny/nz
    normal_moments_texture::moments direct_moments(float nx, float ny, float nz)
    {
        const float p = nx / nz;
        const float q = ny / nz;
        return {p, q, p * p, p * q, q * q};
    }

    // weighted average of moment sets (independent reference for mip blending)
    normal_moments_texture::moments weighted_average(
        const vector<normal_moments_texture::moments>& ms,
        const vector<unsigned>& weights)
    {
        normal_moments_texture::moments acc{0, 0, 0, 0, 0};
        unsigned total = 0;
        for (unsigned i = 0; i < ms.size(); i++)
        {
            for (unsigned k = 0; k < 5; k++) acc[k] += ms[i][k] * float(weights[i]);
            total += weights[i];
        }
        for (float& x : acc) x /= float(total);
        return acc;
    }

    // build a width x height normal array from independent (p, q) slopes
    vector<float> normals_from_pq(unsigned width, unsigned height,
                                  const vector<float>& ps,
                                  const vector<float>& qs)
    {
        vector<float> data(width * height * 3);
        for (unsigned i = 0; i < width * height; i++)
        {
            const float p = ps[i];
            const float q = qs[i];
            const float inv = 1.f / std::sqrt(p * p + q * q + 1.f);
            data[i * 3 + 0] = p * inv;
            data[i * 3 + 1] = q * inv;
            data[i * 3 + 2] = inv; // nz > 0
        }
        return data;
    }

    struct memory_canvas : cr::canvas
    {
        vector<cr::color> pixels;
        void draw(const vector<cr::color>& p, unsigned w, unsigned h) override
        {
            if (p.size() != w * h) std::abort();
            pixels = p;
        }
    };

    // fragment shader counting gradient invocations and remembering the last plane data
    struct probe_fs : cr::fragment_shader
    {
        bool wants_gradients() const override { return true; }
        cr::color run(const vector<float>& in) const override
        {
            legacy_calls++;
            return {in.size() ? in[0] : 0.f, 0.f, 0.f};
        }
        cr::color run(const vector<float>& in, const vector<float>& dx,
                      const vector<float>& dy, bool has_gradients) const override
        {
            grad_calls++;
            last_has_grad = has_gradients;
            last_in = in;
            last_dx = dx;
            last_dy = dy;
            return {in.size() ? in[0] : 0.f, 0.f, 0.f};
        }
        mutable unsigned grad_calls = 0;
        mutable unsigned legacy_calls = 0;
        mutable bool last_has_grad = false;
        mutable vector<float> last_in, last_dx, last_dy;
    };

    // counting lean fragment shader (same shading, extra counters)
    struct count_lean_fs : lean_fs
    {
        using lean_fs::lean_fs;
        cr::color run(const vector<float>& in, const vector<float>& dx,
                      const vector<float>& dy, bool has_gradients) const override
        {
            calls++;
            return lean_fs::run(in, dx, dy, has_gradients);
        }
        mutable unsigned calls = 0;
    };

    // independent affine plane through three integer points for one component
    struct ref_plane
    {
        float gx, gy, g0;
        ref_plane(int x1, int y1, float f1, int x2, int y2, float f2,
                  int x3, int y3, float f3)
        {
            const float dx12 = float(x2 - x1), dy12 = float(y2 - y1);
            const float dx13 = float(x3 - x1), dy13 = float(y3 - y1);
            const float det = dx12 * dy13 - dy12 * dx13;
            const float a = f2 - f1, b = f3 - f1;
            gx = (a * dy13 - dy12 * b) / det;
            gy = (dx12 * b - a * dx13) / det;
            g0 = f1;
            this->x1 = x1;
            this->y1 = y1;
        }
        int x1, y1;
        float at(int x, int y) const { return g0 + gx * float(x - x1) + gy * float(y - y1); }
    };
}

// ---------------------------------------------------------------- pyramid group

static int pyramid_base_5x3()
{
    const unsigned W = 5, H = 3;
    vector<float> ps(W * H), qs(W * H);
    for (unsigned i = 0; i < W * H; i++)
    {
        ps[i] = 0.3f * std::sin(float(i) * 1.7f);
        qs[i] = 0.4f * std::cos(float(i) * 1.1f);
    }
    const vector<float> data = normals_from_pq(W, H, ps, qs);
    const normal_moments_texture tex(W, H, data);

    // base level independently computed from the same p, q
    TASK_CHECK(tex.level_count() >= 1);
    for (unsigned y = 0; y < H; y++)
    {
        for (unsigned x = 0; x < W; x++)
        {
            const unsigned i = y * W + x;
            const auto ref = direct_moments(data[i * 3], data[i * 3 + 1], data[i * 3 + 2]);
            TASK_CHECK(close_moments(tex.texel(0, x, y), ref));
            // nz is guaranteed positive and finite by construction
            TASK_CHECK(data[i * 3 + 2] > 0.f);
        }
    }

    // mip dimensions use ceiling halves: 5 -> 3 -> 2 -> 1, 3 -> 2 -> 1
    TASK_CHECK(tex.level_width(0) == 5 && tex.level_height(0) == 3);
    TASK_CHECK(tex.level_width(1) == 3 && tex.level_height(1) == 2);
    TASK_CHECK(tex.level_width(2) == 2 && tex.level_height(2) == 1);
    TASK_CHECK(tex.level_width(3) == 1 && tex.level_height(3) == 1);
    TASK_CHECK(tex.level_count() == 4);
    return 0;
}

static int pyramid_weighted_5x3()
{
    const unsigned W = 5, H = 3;
    vector<float> ps(W * H), qs(W * H);
    for (unsigned i = 0; i < W * H; i++)
    {
        ps[i] = 0.5f * std::sin(float(i) * 0.9f) + 0.1f;
        qs[i] = 0.6f * std::cos(float(i) * 1.3f) - 0.2f;
    }
    const vector<float> data = normals_from_pq(W, H, ps, qs);
    const normal_moments_texture tex(W, H, data);

    // independent reference for level 1 texel (0, 0): children (0,0),(1,0),(0,1),(1,1)
    auto base_at = [&](unsigned x, unsigned y) {
        const unsigned i = y * W + x;
        return direct_moments(data[i * 3], data[i * 3 + 1], data[i * 3 + 2]);
    };

    const vector<normal_moments_texture::moments> children = {
        base_at(0, 0), base_at(1, 0), base_at(0, 1), base_at(1, 1)};
    const auto ref00 = weighted_average(children, {1, 1, 1, 1});
    TASK_CHECK(close_moments(tex.texel(1, 0, 0), ref00));

    // edge texel (2, 0) of level 1 (width 3): children x=4 only column, y=0,1
    const vector<normal_moments_texture::moments> edge = {base_at(4, 0), base_at(4, 1)};
    const auto ref_edge = weighted_average(edge, {1, 1});
    TASK_CHECK(close_moments(tex.texel(1, 2, 0), ref_edge));

    // bottom row texel (1, 1) of level 1 (height 2): children y=2,3; only y=2 exists
    const vector<normal_moments_texture::moments> bot = {
        base_at(2, 2), base_at(3, 2)};
    const auto ref_bot = weighted_average(bot, {1, 1});
    TASK_CHECK(close_moments(tex.texel(1, 1, 1), ref_bot));

    // level 2 (2x1) texel 0 gathers the four level-1 children (0,0),(1,0),(0,1),(1,1),
    // whose base texel counts are 4, 4, 2, 2 (the bottom row has no y=3)
    const vector<normal_moments_texture::moments> l2src = {
        tex.texel(1, 0, 0), tex.texel(1, 1, 0),
        tex.texel(1, 0, 1), tex.texel(1, 1, 1)};
    const auto ref_l2 = weighted_average(l2src, {4, 4, 2, 2});
    TASK_CHECK(close_moments(tex.texel(2, 0, 0), ref_l2));

    // the last level is the whole-image statistic, equal to a flat average of the base
    vector<normal_moments_texture::moments> all_base;
    vector<unsigned> ones;
    for (unsigned i = 0; i < W * H; i++)
    {
        all_base.push_back(base_at(i % W, i / W));
        ones.push_back(1);
    }
    const auto whole = weighted_average(all_base, ones);
    const auto& last = tex.texel(tex.level_count() - 1, 0, 0);
    TASK_CHECK(close_moments(last, whole, EPS_LOOSE));
    return 0;
}

static int pyramid_1xN()
{
    const unsigned N = 7;
    vector<float> ps(1 * N), qs(1 * N);
    for (unsigned i = 0; i < N; i++)
    {
        ps[i] = 0.2f * float(i) - 0.6f;
        qs[i] = -0.15f * float(i) + 0.3f;
    }
    const vector<float> data = normals_from_pq(1, N, ps, qs);
    const normal_moments_texture tex(1, N, data);

    // heights: 7 -> 4 -> 2 -> 1, width stays 1
    TASK_CHECK(tex.level_count() == 4);
    TASK_CHECK(tex.level_width(0) == 1 && tex.level_height(0) == 7);
    TASK_CHECK(tex.level_width(1) == 1 && tex.level_height(1) == 4);
    TASK_CHECK(tex.level_width(2) == 1 && tex.level_height(2) == 2);
    TASK_CHECK(tex.level_width(3) == 1 && tex.level_height(3) == 1);

    auto base_at = [&](unsigned y) {
        return direct_moments(data[y * 3], data[y * 3 + 1], data[y * 3 + 2]);
    };

    // level 1, texel y=3 covers base y=6,7; only y=6 exists (weight 1)
    TASK_CHECK(close_moments(tex.texel(1, 0, 3), base_at(6)));
    // level 1, texel y=0 covers base y=0,1 (weight 1 each)
    const auto ref0 = weighted_average({base_at(0), base_at(1)}, {1, 1});
    TASK_CHECK(close_moments(tex.texel(1, 0, 0), ref0));

    // level 2, texel y=1 covers level1 y=2 (base 4,5) and y=3 (base 6): weights 2 and 1
    const auto ref_l2_1 = weighted_average(
        {tex.texel(1, 0, 2), tex.texel(1, 0, 3)}, {2, 1});
    TASK_CHECK(close_moments(tex.texel(2, 0, 1), ref_l2_1));

    // final single texel equals the flat base average
    vector<normal_moments_texture::moments> all;
    vector<unsigned> ones;
    for (unsigned y = 0; y < N; y++) { all.push_back(base_at(y)); ones.push_back(1); }
    TASK_CHECK(close_moments(tex.texel(3, 0, 0), weighted_average(all, ones), EPS_LOOSE));
    return 0;
}

static int pyramid_owns_data()
{
    const unsigned W = 4, H = 4;
    vector<float> data(W * H * 3);
    for (unsigned i = 0; i < W * H; i++)
    {
        const float p = 0.2f * float(i);
        const float q = -0.1f * float(i);
        const float inv = 1.f / std::sqrt(p * p + q * q + 1.f);
        data[i * 3] = p * inv;
        data[i * 3 + 1] = q * inv;
        data[i * 3 + 2] = inv;
    }
    normal_moments_texture tex(W, H, data);
    const auto snapshot = tex.texel(0, 1, 1);
    data.assign(data.size(), 0.f); // mutate and release the caller array

    // the texture must keep its own copy
    TASK_CHECK(close_moments(tex.texel(0, 1, 1), snapshot));

    // two shaders may share the same texture
    auto shared = std::make_shared<normal_moments_texture>(W, H, normals_from_pq(
        W, H, vector<float>(W * H, 0.1f), vector<float>(W * H, -0.1f)));
    lean_fs fs1(shared);
    lean_fs fs2(shared);
    TASK_CHECK(fs1.diagnostics().fragment_count == 0);
    TASK_CHECK(fs2.diagnostics().fragment_count == 0);
    return 0;
}

// -------------------------------------------------------------- footprint group

static int footprint_quotient_derivatives()
{
    // unequal perspective w and negative uv: plane values are divided attributes,
    // the fragment shader restores uv with the analytic quotient rule
    memory_canvas canvas;
    cr::context ctx(canvas, 64, 64);
    auto tex = std::make_shared<normal_moments_texture>(
        4, 4, normals_from_pq(4, 4, vector<float>(16, 0.f), vector<float>(16, 0.f)));
    auto fs = std::make_shared<probe_fs>();
    auto vs = std::make_shared<lean_vs>();
    ctx.bind_vertex_shader(vs);
    ctx.bind_fragment_shader(fs);
    ctx.clear_framebuf(true, false);

    // three vertices with distinct w and negative uv values
    auto vtx = [](float cx, float cy, float w, float u, float v) {
        return cr::make_lean_vertex(cx * w, cy * w, 0.f, w,
                                    cx, cy, -3.f,   // view position
                                    0.f, 0.f, 1.f,  // normal
                                    1.f, 0.f, 0.f,  // tangent
                                    1.f, u, v);
    };
    const vector<float> a = vtx(-0.8f, -0.8f, 1.0f, -2.0f, -1.0f);
    const vector<float> b = vtx(0.8f, -0.8f, 2.5f, -1.0f, 0.5f);
    const vector<float> c = vtx(0.0f, 0.8f, 1.6f, 0.5f, 2.0f);
    ctx.draw_triangle(a, b, c);
    TASK_CHECK(fs->grad_calls > 0);
    TASK_CHECK(fs->last_has_grad);

    // independently evaluate the divided plane at the last covered fragment
    const unsigned n = cr::lean_attr::OUTPUT_SIZE;
    TASK_CHECK(fs->last_in.size() == n);
    TASK_CHECK(fs->last_dx.size() == n && fs->last_dy.size() == n);

    // reconstruct the same divided planes from the three vertex outputs
    vector<float> oa(n), ob(n), oc(n);
    cr::position pa = vs->run(a, oa);
    cr::position pb = vs->run(b, ob);
    cr::position pc = vs->run(c, oc);
    TASK_CHECK(pa.is_normalized() && pb.is_normalized() && pc.is_normalized());

    // integer canvas vertices as produced by the renderer quantization
    auto qx = [&](float ndc) {
        unsigned rx = unsigned(((ndc + 1.f) * 65.f) / 2.f);
        if (rx == 64u) rx--;
        return int(rx);
    };
    auto qy = [&](float ndc) {
        unsigned ry = unsigned(((ndc + 1.f) * 65.f) / 2.f);
        if (ry == 64u) ry--;
        return int(ry);
    };
    const int x1 = qx(pa.x), y1 = qy(pa.y);
    const int x2 = qx(pb.x), y2 = qy(pb.y);
    const int x3 = qx(pc.x), y3 = qy(pc.y);

    // check every plane component value and derivative against an independent plane
    for (unsigned k = 0; k < n; k++)
    {
        ref_plane plane(x1, y1, oa[k], x2, y2, ob[k], x3, y3, oc[k]);
        TASK_CHECK(close(fs->last_dx[k], plane.gx, EPS_LOOSE));
        TASK_CHECK(close(fs->last_dy[k], plane.gy, EPS_LOOSE));
    }

    // verify the quotient rule for restored u on the last fragment position
    const unsigned iu = cr::lean_attr::OUT_UV;
    const unsigned iw = cr::lean_attr::OUT_INV_W;
    const float u_div = fs->last_in[iu], v_div = fs->last_in[iu + 1];
    const float wplane = fs->last_in[iw];
    const float restored_u = u_div / wplane;
    const float restored_v = v_div / wplane;
    auto quotient = [](float av, float da, float g, float dg) {
        return (da * g - av * dg) / (g * g);
    };
    const float du_dx = quotient(u_div, fs->last_dx[iu], wplane, fs->last_dx[iw]);
    const float du_dy = quotient(u_div, fs->last_dy[iu], wplane, fs->last_dy[iw]);
    const float dv_dx = quotient(v_div, fs->last_dx[iu + 1], wplane, fs->last_dx[iw]);
    const float dv_dy = quotient(v_div, fs->last_dy[iu + 1], wplane, fs->last_dy[iw]);

    // finite differences of the restored plane must match the analytic quotient derivative
    const float u_x = (u_div + fs->last_dx[iu]) / (wplane + fs->last_dx[iw]);
    const float u_y = (u_div + fs->last_dy[iu]) / (wplane + fs->last_dy[iw]);
    TASK_CHECK(close(du_dx, u_x - restored_u, EPS_LOOSE));
    TASK_CHECK(close(du_dy, u_y - restored_u, EPS_LOOSE));
    const float v_x = (v_div + fs->last_dx[iu + 1]) / (wplane + fs->last_dx[iw]);
    const float v_y = (v_div + fs->last_dy[iu + 1]) / (wplane + fs->last_dy[iw]);
    TASK_CHECK(close(dv_dx, v_x - restored_v, EPS_LOOSE));
    TASK_CHECK(close(dv_dy, v_y - restored_v, EPS_LOOSE));
    return 0;
}

static int footprint_viewport_offsets()
{
    // gradient plane must follow the current canvas, including a viewport offset
    memory_canvas canvas;
    cr::context ctx(canvas, 80, 60);
    auto fs = std::make_shared<probe_fs>();
    ctx.bind_vertex_shader(std::make_shared<lean_vs>());
    ctx.bind_fragment_shader(fs);
    ctx.set_viewport(10, 7, 60, 40);
    ctx.clear_framebuf(true, false);

    auto vtx = [](float cx, float cy) {
        const float w = 2.f;
        return cr::make_lean_vertex(cx * w, cy * w, 0.f, w,
                                    cx, cy, -4.f, 0.f, 0.f, 1.f,
                                    1.f, 0.f, 0.f, 1.f, cx, cy);
    };
    ctx.draw_triangle(vtx(-0.6f, -0.6f), vtx(0.6f, -0.6f), vtx(0.f, 0.6f));
    TASK_CHECK(fs->grad_calls > 0);

    // the divided u plane is u/w = cx/2; its per-pixel derivative is constant and
    // must be expressed in current canvas pixels (viewport width 60), not framebuffer 80
    const unsigned iu = cr::lean_attr::OUT_UV;
    const float expected_dx = (0.6f / 2.f - (-0.6f) / 2.f) / float(60);
    TASK_CHECK(std::fabs(fs->last_dx[iu]) > 0.f);
    TASK_CHECK(close(std::fabs(fs->last_dx[iu]), std::fabs(expected_dx), 0.05f));
    return 0;
}

static int footprint_no_residual_point_line()
{
    // after a gradient triangle, points and lines must report no 2D gradients
    memory_canvas canvas;
    cr::context ctx(canvas, 48, 48);
    auto fs = std::make_shared<probe_fs>();
    ctx.bind_vertex_shader(std::make_shared<lean_vs>());
    ctx.bind_fragment_shader(fs);
    ctx.clear_framebuf(true, false);

    auto vtx = [](float cx, float cy, float u) {
        const float w = 1.5f;
        return cr::make_lean_vertex(cx * w, cy * w, 0.f, w,
                                    cx, cy, -3.f, 0.f, 0.f, 1.f,
                                    1.f, 0.f, 0.f, 1.f, u, 0.f);
    };
    ctx.draw_triangle(vtx(-0.7f, -0.7f, 0.f), vtx(0.7f, -0.7f, 1.f),
                      vtx(0.f, 0.7f, 0.5f));
    TASK_CHECK(fs->grad_calls > 0 && fs->last_has_grad);

    fs->grad_calls = 0;
    ctx.draw_point(vtx(0.f, 0.f, 0.2f));
    TASK_CHECK(fs->grad_calls == 1);
    TASK_CHECK(!fs->last_has_grad);
    for (float g : fs->last_dx) TASK_CHECK(g == 0.f);
    for (float g : fs->last_dy) TASK_CHECK(g == 0.f);

    fs->grad_calls = 0;
    ctx.draw_line(vtx(-0.5f, -0.5f, 0.f), vtx(0.5f, 0.5f, 1.f));
    TASK_CHECK(fs->grad_calls > 0);
    TASK_CHECK(!fs->last_has_grad);
    for (float g : fs->last_dx) TASK_CHECK(g == 0.f);
    for (float g : fs->last_dy) TASK_CHECK(g == 0.f);
    return 0;
}

static int footprint_disabled_interpolation()
{
    // disabled interpolation uses the first submitted vertex and zero gradients
    memory_canvas canvas;
    cr::context ctx(canvas, 48, 48);
    auto fs = std::make_shared<probe_fs>();
    ctx.bind_vertex_shader(std::make_shared<lean_vs>());
    ctx.bind_fragment_shader(fs);
    ctx.disable_interpolation();
    ctx.clear_framebuf(true, false);

    vector<float> first;
    auto vtx = [](float cx, float cy, float u) {
        const float w = 1.2f;
        return cr::make_lean_vertex(cx * w, cy * w, 0.f, w,
                                    cx, cy, -3.f, 0.f, 0.f, 1.f,
                                    1.f, 0.f, 0.f, 1.f, u, 0.f);
    };
    first = vtx(-0.7f, -0.7f, 0.37f);
    ctx.draw_triangle(first, vtx(0.7f, -0.7f, 1.f), vtx(0.f, 0.7f, 0.5f));
    TASK_CHECK(fs->grad_calls > 0);
    TASK_CHECK(!fs->last_has_grad);
    for (float g : fs->last_dx) TASK_CHECK(g == 0.f);

    // the attribute value must be the first vertex's divided output
    vector<float> out_first;
    cr::lean_vs().run(first, out_first);
    TASK_CHECK(fs->last_in.size() == out_first.size());
    for (unsigned k = 0; k < out_first.size(); k++)
    {
        TASK_CHECK(close(fs->last_in[k], out_first[k]));
    }
    return 0;
}

static int footprint_framebuf_resize()
{
    // after a framebuffer resize the plane derivatives must follow the new canvas,
    // i.e. one derivative unit is still one (new) canvas pixel
    memory_canvas canvas;
    cr::context ctx(canvas, 80, 80);
    auto fs = std::make_shared<probe_fs>();
    ctx.bind_vertex_shader(std::make_shared<lean_vs>());
    ctx.bind_fragment_shader(fs);
    ctx.clear_framebuf(true, false);

    auto vtx = [](float cx, float cy) {
        const float w = 2.f;
        return cr::make_lean_vertex(cx * w, cy * w, 0.f, w,
                                    cx, cy, -4.f, 0.f, 0.f, 1.f,
                                    1.f, 0.f, 0.f, 1.f, cx, cy);
    };
    const auto tri = [&] {
        ctx.draw_triangle(vtx(-0.6f, -0.6f), vtx(0.6f, -0.6f), vtx(0.f, 0.6f));
    };
    tri();
    const unsigned iu = cr::lean_attr::OUT_UV;
    const float dx_before = fs->last_dx[iu];
    TASK_CHECK(dx_before != 0.f);

    // shrink the canvas (and its viewport, as the SDL examples do); the same NDC
    // triangle now spans fewer pixels, so the per-pixel plane derivative doubles
    ctx.resize_framebuf(40, 40);
    ctx.set_viewport(0, 0, 40, 40);
    fs->grad_calls = 0;
    ctx.clear_framebuf(true, false);
    tri();
    TASK_CHECK(fs->grad_calls > 0 && fs->last_has_grad);
    const float dx_after = fs->last_dx[iu];
    TASK_CHECK(close(dx_after, dx_before * 2.f, 0.05f));

    // grow it back and confirm the derivative returns to the original canvas scale
    ctx.resize_framebuf(80, 80);
    ctx.set_viewport(0, 0, 80, 80);
    fs->grad_calls = 0;
    ctx.clear_framebuf(true, false);
    tri();
    TASK_CHECK(close(fs->last_dx[iu], dx_before, 0.05f));
    return 0;
}

static int footprint_lod_and_repeat()
{
    // build an 8x8 texture and check the LOD formula with controlled derivatives
    const unsigned W = 8, H = 8;
    vector<float> ps(W * H), qs(W * H);
    for (unsigned i = 0; i < W * H; i++)
    {
        ps[i] = 0.3f * std::sin(float(i));
        qs[i] = 0.3f * std::cos(float(i));
    }
    const normal_moments_texture tex(W, H, normals_from_pq(W, H, ps, qs));
    TASK_CHECK(tex.level_count() == 4); // 8->4->2->1, last LOD index 3

    // zero footprint -> rho 0 -> LOD clamped to 0
    auto r0 = tex.sample(0.5f, 0.5f, 0.f, 0.f, 0.f, 0.f);
    TASK_CHECK(close(r0.lod, 0.f));

    // a footprint mapping one canvas pixel to four texture pixels: rho = 4 -> LOD 2
    auto r1 = tex.sample(0.3f, 0.7f, 4.f / float(W), 0.f, 0.f, 4.f / float(H));
    TASK_CHECK(close(r1.lod, 2.f, EPS_LOOSE));

    // LOD is clamped to the last level
    auto r2 = tex.sample(0.3f, 0.7f, 100.f / float(W), 0.f, 0.f, 100.f / float(H));
    TASK_CHECK(close(r2.lod, float(tex.level_count() - 1), EPS_LOOSE));

    // repeat addressing: sampling at u + 1 must match u at level 0 (integer texel shift)
    auto a0 = tex.fetch_bilinear(0, 0.25f, 0.25f);
    auto a1 = tex.fetch_bilinear(0, 1.25f, 0.25f);
    auto a2 = tex.fetch_bilinear(0, -0.75f, 0.25f); // negative repeat
    TASK_CHECK(close_moments(a0, a1));
    TASK_CHECK(close_moments(a0, a2));

    // trilinear blending at a fractional LOD is a convex mix of the two level fetches
    const float lod = 1.25f;
    const float rho = std::pow(2.f, lod);
    auto rf = tex.sample(0.4f, 0.6f, rho / float(W), 0.f, 0.f, 0.f);
    TASK_CHECK(close(rf.lod, lod, EPS_LOOSE));
    const auto mlo = tex.fetch_bilinear(1, 0.4f, 0.6f);
    const auto mhi = tex.fetch_bilinear(2, 0.4f, 0.6f);
    normal_moments_texture::moments expected{};
    for (unsigned k = 0; k < 5; k++) expected[k] = mlo[k] * 0.75f + mhi[k] * 0.25f;
    TASK_CHECK(close_moments(rf.m, expected, EPS_LOOSE));
    return 0;
}

// ---------------------------------------------------------------- shading group

static vector<float> shade_vertex(float cx, float cy, float w, float u, float v,
                                  float handedness = 1.f)
{
    return cr::make_lean_vertex(cx * w, cy * w, 0.f, w,
                                cx, cy, -3.f,
                                0.f, 0.f, 1.f,
                                1.f, 0.f, 0.f,
                                handedness, u, v);
}

static int shading_pixel_and_lod()
{
    memory_canvas canvas;
    cr::context ctx(canvas, 64, 64);
    // nonzero slopes give a non-diagonal covariance with a pq cross term
    vector<float> ps(16), qs(16);
    for (unsigned i = 0; i < 16; i++)
    {
        ps[i] = 0.25f * std::sin(float(i) * 1.2f);
        qs[i] = 0.25f * std::cos(float(i) * 0.9f);
    }
    auto tex = std::make_shared<normal_moments_texture>(4, 4, normals_from_pq(4, 4, ps, qs));
    auto fs = std::make_shared<count_lean_fs>(tex);
    ctx.bind_vertex_shader(std::make_shared<lean_vs>());
    ctx.bind_fragment_shader(fs);
    fs->set_material({0.4f, 0.4f, 0.4f}, {1.f, 1.f, 1.f}, 16.f, {0.f, 0.f, 1.f});
    ctx.enable_depth_test();
    ctx.clear_framebuf(true, true);

    ctx.draw_triangle(shade_vertex(-0.8f, -0.8f, 1.f, 0.f, 0.f),
                      shade_vertex(0.8f, -0.8f, 1.f, 1.f, 0.f),
                      shade_vertex(0.f, 0.8f, 1.f, 0.5f, 1.f));
    TASK_CHECK(fs->calls > 0);

    // an enlarged triangle must raise the average LOD above the close-up one
    const float close_lod = fs->diagnostics().lod;
    fs->reset_diagnostics();
    ctx.clear_framebuf(true, true);
    ctx.draw_triangle(shade_vertex(-0.8f, -0.8f, 3.f, 0.f, 0.f),
                      shade_vertex(0.8f, -0.8f, 3.f, 1.f, 0.f),
                      shade_vertex(0.f, 0.8f, 3.f, 0.5f, 1.f));
    TASK_CHECK(fs->calls > 0);
    const float far_lod = fs->diagnostics().lod;
    TASK_CHECK(far_lod >= close_lod);

    // every written color is finite and clamped to [0, 1]
    ctx.update_canvas();
    for (cr::color c : canvas.pixels)
    {
        TASK_CHECK(std::isfinite(c.r) && std::isfinite(c.g) && std::isfinite(c.b));
        TASK_CHECK(c.r >= 0.f && c.r <= 1.f);
        TASK_CHECK(c.g >= 0.f && c.g <= 1.f);
        TASK_CHECK(c.b >= 0.f && c.b <= 1.f);
    }

    // diagnostics retain an unclamped value and the sampled moments
    const auto& d = fs->diagnostics();
    TASK_CHECK(d.moments[2] >= 0.f);
    TASK_CHECK(std::isfinite(d.unclamped.r));
    return 0;
}

static int shading_occluded_no_fs()
{
    // a near triangle fully covers a far one; with early depth the far fragments
    // must never execute the fragment shader
    memory_canvas canvas;
    cr::context ctx(canvas, 64, 64);
    auto tex = std::make_shared<normal_moments_texture>(
        2, 2, normals_from_pq(2, 2, vector<float>(4, 0.f), vector<float>(4, 0.f)));
    auto fs = std::make_shared<count_lean_fs>(tex);
    ctx.bind_vertex_shader(std::make_shared<lean_vs>());
    ctx.bind_fragment_shader(fs);
    ctx.enable_depth_test();
    ctx.clear_framebuf(true, true);

    // far triangle first (lower depth), then the covering near triangle
    ctx.draw_triangle(shade_vertex(-0.9f, -0.9f, 1.f, 0.f, 0.f),
                      shade_vertex(0.9f, -0.9f, 1.f, 1.f, 0.f),
                      shade_vertex(0.f, 0.9f, 1.f, 0.5f, 1.f));
    const unsigned far_calls = fs->calls;
    TASK_CHECK(far_calls > 0);

    // the near triangle covers the same area; reuse the same vertex x/y but higher depth
    auto near_v = [](float cx, float cy, float u, float v) {
        return cr::make_lean_vertex(cx, cy, 0.8f, 1.f,
                                    cx, cy, -3.f, 0.f, 0.f, 1.f,
                                    1.f, 0.f, 0.f, 1.f, u, v);
    };
    ctx.draw_triangle(near_v(-0.9f, -0.9f, 0.f, 0.f),
                      near_v(0.9f, -0.9f, 1.f, 0.f),
                      near_v(0.f, 0.9f, 0.5f, 1.f));
    const unsigned near_calls = fs->calls - far_calls;
    TASK_CHECK(near_calls > 0);

    // redraw the far triangle: every fragment is now occluded, so the FS is not called
    const unsigned before = fs->calls;
    ctx.draw_triangle(shade_vertex(-0.9f, -0.9f, 1.f, 0.f, 0.f),
                      shade_vertex(0.9f, -0.9f, 1.f, 1.f, 0.f),
                      shade_vertex(0.f, 0.9f, 1.f, 0.5f, 1.f));
    TASK_CHECK(fs->calls == before);
    return 0;
}

static int shading_winding_and_mirror()
{
    // both windings render, and a mirrored handedness flips the bitangent frame
    memory_canvas canvas;
    cr::context ctx(canvas, 48, 48);
    auto tex = std::make_shared<normal_moments_texture>(
        2, 2, normals_from_pq(2, 2, vector<float>(4, 0.1f), vector<float>(4, 0.1f)));
    auto fs = std::make_shared<lean_fs>(tex);
    ctx.bind_vertex_shader(std::make_shared<lean_vs>());
    ctx.bind_fragment_shader(fs);
    fs->set_material({0.3f, 0.3f, 0.3f}, {1.f, 1.f, 1.f}, 8.f, {0.3f, 0.3f, 0.9f});
    ctx.clear_framebuf(true, false);

    const vector<float> a = shade_vertex(-0.7f, -0.7f, 1.f, 0.f, 0.f, 1.f);
    const vector<float> b = shade_vertex(0.7f, -0.7f, 1.f, 1.f, 0.f, 1.f);
    const vector<float> c = shade_vertex(0.f, 0.7f, 1.f, 0.5f, 1.f, 1.f);
    ctx.draw_triangle(a, b, c); // counter-clockwise
    TASK_CHECK(fs->diagnostics().fragment_count > 0);

    const unsigned ccw_count = fs->diagnostics().fragment_count;
    ctx.clear_framebuf(true, false);
    fs->reset_diagnostics();
    ctx.draw_triangle(a, c, b); // reversed winding still rasterizes
    TASK_CHECK(fs->diagnostics().fragment_count == ccw_count);

    // mirrored handedness flips the bitangent and therefore the cross-slope response
    const vector<float> am = shade_vertex(-0.7f, -0.7f, 1.f, 0.f, 0.f, -1.f);
    const vector<float> bm = shade_vertex(0.7f, -0.7f, 1.f, 1.f, 0.f, -1.f);
    const vector<float> cm = shade_vertex(0.f, 0.7f, 1.f, 0.5f, 1.f, -1.f);
    fs->reset_diagnostics();
    ctx.clear_framebuf(true, false);
    ctx.draw_triangle(am, bm, cm);
    TASK_CHECK(fs->diagnostics().fragment_count > 0);
    TASK_CHECK(fs->diagnostics().has_gradients);
    return 0;
}

static int shading_sigma_offdiagonal()
{
    // four explicit (p, q) slope samples with a deliberate pq cross term; the expected
    // mean and covariance are computed independently straight from these samples
    const vector<float> ps{0.10f, 0.30f, -0.20f, 0.05f};
    const vector<float> qs{0.05f, 0.25f, 0.15f, -0.30f};

    float mp = 0.f, mq = 0.f, ep2 = 0.f, epq = 0.f, eq2 = 0.f;
    for (unsigned i = 0; i < 4; i++)
    {
        mp += ps[i]; mq += qs[i];
        ep2 += ps[i] * ps[i]; epq += ps[i] * qs[i]; eq2 += qs[i] * qs[i];
    }
    mp /= 4.f; mq /= 4.f; ep2 /= 4.f; epq /= 4.f; eq2 /= 4.f;
    const float ref_c00 = ep2 - mp * mp;
    const float ref_c01 = epq - mp * mq;
    const float ref_c11 = eq2 - mq * mq;
    TASK_CHECK(std::fabs(ref_c01) > 1e-4f); // the pq cross term really is nonzero

    auto tex = std::make_shared<normal_moments_texture>(2, 2, normals_from_pq(2, 2, ps, qs));
    lean_fs fs(tex);
    fs.set_material({0.f, 0.f, 0.f}, {1.f, 1.f, 1.f}, 0.f, {0.f, 0.f, 1.f});

    // run the legacy path on a straight-on fragment (zero gradients)
    vector<float> in(cr::lean_attr::OUTPUT_SIZE, 0.f);
    in[cr::lean_attr::OUT_VIEW_POS + 2] = -3.f;
    in[cr::lean_attr::OUT_NORMAL + 2] = 1.f;
    in[cr::lean_attr::OUT_TANGENT + 0] = 1.f;
    in[cr::lean_attr::OUT_HANDEDNESS] = 1.f;
    in[cr::lean_attr::OUT_INV_W] = 1.f;
    const cr::color col = fs.run(in);
    TASK_CHECK(std::isfinite(col.r) && col.r >= 0.f && col.r <= 1.f);

    // u = v = 0 with the -0.5 convention bilinearly averages all four base texels,
    // i.e. the flat sample statistics above
    const auto& m = fs.diagnostics().moments;
    const float mu_p = m[0], mu_q = m[1];
    TASK_CHECK(close(mu_p, mp, EPS_LOOSE));
    TASK_CHECK(close(mu_q, mq, EPS_LOOSE));
    TASK_CHECK(close(m[2] - mu_p * mu_p, ref_c00, EPS_LOOSE));
    TASK_CHECK(close(m[3] - mu_p * mu_q, ref_c01, EPS_LOOSE));
    TASK_CHECK(close(m[4] - mu_q * mu_q, ref_c11, EPS_LOOSE));

    // with shininess 0 the base width is 1/2; Sigma stays positive definite
    TASK_CHECK(close(1.f / (0.f + 2.f), 0.5f));
    return 0;
}

static int shading_specular_formula()
{
    // controlled straight-on configuration with a flat (zero-slope) texture, so the
    // LEAN Gaussian term can be computed by hand and compared term by term
    auto flat = std::make_shared<normal_moments_texture>(
        2, 2, normals_from_pq(2, 2, vector<float>(4, 0.f), vector<float>(4, 0.f)));
    lean_fs fs(flat);

    vector<float> in(cr::lean_attr::OUTPUT_SIZE, 0.f);
    in[cr::lean_attr::OUT_VIEW_POS + 2] = -3.f; // P = (0,0,-3)
    in[cr::lean_attr::OUT_NORMAL + 2] = 1.f;    // N = (0,0,1)
    in[cr::lean_attr::OUT_TANGENT + 0] = 1.f;   // T = (1,0,0)
    in[cr::lean_attr::OUT_HANDEDNESS] = 1.f;
    in[cr::lean_attr::OUT_INV_W] = 1.f;

    // shininess 0 -> base width b = 1/2; L = V = H = (0,0,1), a = 0,
    // D = 1/(2*pi*b) = 1/pi, spec = D/4 = 1/(4*pi)
    fs.set_material({0.f, 0.f, 0.f}, {1.f, 1.f, 1.f}, 0.f, {0.f, 0.f, 1.f});
    cr::color col = fs.run(in);
    const float expected_spec = 1.f / (4.f * 3.14159265358979f);
    TASK_CHECK(close(fs.diagnostics().unclamped.r, expected_spec, EPS_LOOSE));
    TASK_CHECK(close(col.r, expected_spec, EPS_LOOSE)); // below 1, so unchanged by clamp
    TASK_CHECK(close(fs.diagnostics().lod, 0.f));

    // light pointing below the surface: L.z <= 0 zeroes both diffuse and specular
    fs.set_material({0.5f, 0.5f, 0.5f}, {1.f, 1.f, 1.f}, 4.f, {0.f, 0.f, -1.f});
    col = fs.run(in);
    TASK_CHECK(col.r == 0.f && col.g == 0.f && col.b == 0.f);
    TASK_CHECK(fs.diagnostics().unclamped.r == 0.f);
    return 0;
}

static int shading_interleaved_shaders()
{
    // interleave the legacy color shader and the lean shader across draws; gradient
    // state must not leak between them, and the old path keeps its legacy output
    memory_canvas canvas;
    cr::context ctx(canvas, 48, 48);
    auto lean_tex = std::make_shared<normal_moments_texture>(
        2, 2, normals_from_pq(2, 2, vector<float>(4, 0.f), vector<float>(4, 0.f)));
    auto c_vs = std::make_shared<color_vs>();
    auto c_fs = std::make_shared<color_fs>();
    auto lvs = std::make_shared<lean_vs>();
    auto lfs = std::make_shared<count_lean_fs>(lean_tex);
    lfs->set_material({0.2f, 0.2f, 0.2f}, {1.f, 1.f, 1.f}, 4.f, {0.f, 0.f, 1.f});

    auto color_v = [](float x, float y, float r, float g) {
        return vector<float>{x, y, 0.f, r, g, 0.f};
    };
    auto lean_v = [](float x, float y) {
        return shade_vertex(x, y, 1.f, (x + 1.f) * 0.5f, (y + 1.f) * 0.5f);
    };

    ctx.clear_framebuf(true, false);

    ctx.bind_vertex_shader(c_vs);
    ctx.bind_fragment_shader(c_fs);
    ctx.draw_triangle(color_v(-0.8f, -0.8f, 1.f, 0.f),
                      color_v(0.2f, -0.8f, 0.f, 1.f),
                      color_v(-0.3f, 0.2f, 0.f, 0.f));

    ctx.bind_vertex_shader(lvs);
    ctx.bind_fragment_shader(lfs);
    const unsigned before = lfs->calls;
    ctx.draw_triangle(lean_v(-0.2f, -0.2f), lean_v(0.8f, -0.2f), lean_v(0.3f, 0.8f));
    TASK_CHECK(lfs->calls > before);
    TASK_CHECK(lfs->diagnostics().has_gradients);

    // switch back: the color shader must still use run(in) and produce plain colors
    ctx.bind_vertex_shader(c_vs);
    ctx.bind_fragment_shader(c_fs);
    ctx.update_canvas();
    bool found_red = false, found_green = false;
    for (cr::color c : canvas.pixels)
    {
        if (c.r > 0.5f && c.g < 0.2f) found_red = true;
        if (c.g > 0.5f && c.r < 0.2f) found_green = true;
    }
    TASK_CHECK(found_red && found_green);

    // a point through the lean shader after the color triangle has no gradients
    ctx.bind_vertex_shader(lvs);
    ctx.bind_fragment_shader(lfs);
    lfs->reset_diagnostics();
    ctx.draw_point(lean_v(0.f, 0.f));
    TASK_CHECK(lfs->diagnostics().fragment_count == 1);
    TASK_CHECK(!lfs->diagnostics().has_gradients);
    return 0;
}

static int shading_shared_source_released()
{
    // release the source normal array and drop the last external shared_ptr reference;
    // a shader-bound texture must keep rendering correctly
    memory_canvas canvas;
    cr::context ctx(canvas, 32, 32);
    vector<float> data = normals_from_pq(2, 2,
                                         vector<float>{0.1f, -0.1f, 0.1f, -0.1f},
                                         vector<float>{0.05f, 0.05f, -0.05f, -0.05f});
    auto tex = std::make_shared<normal_moments_texture>(2, 2, data);
    data.clear();
    data.shrink_to_fit();

    auto fs = std::make_shared<lean_fs>(tex);
    fs->set_material({0.3f, 0.3f, 0.3f}, {1.f, 1.f, 1.f}, 4.f, {0.f, 0.f, 1.f});
    tex.reset(); // only the shader now owns the texture

    ctx.bind_vertex_shader(std::make_shared<lean_vs>());
    ctx.bind_fragment_shader(fs);
    ctx.clear_framebuf(true, false);
    ctx.draw_triangle(shade_vertex(-0.7f, -0.7f, 1.f, 0.f, 0.f),
                      shade_vertex(0.7f, -0.7f, 1.f, 1.f, 0.f),
                      shade_vertex(0.f, 0.7f, 1.f, 0.5f, 1.f));
    TASK_CHECK(fs->diagnostics().fragment_count > 0);
    ctx.update_canvas();
    for (cr::color c : canvas.pixels)
    {
        TASK_CHECK(std::isfinite(c.r) && std::isfinite(c.g) && std::isfinite(c.b));
    }
    return 0;
}

extern const struct task_case task_cases[] = {
    // pyramid
    {"pyramid", "base_moments_5x3", pyramid_base_5x3},
    {"pyramid", "weighted_mip_5x3", pyramid_weighted_5x3},
    {"pyramid", "mip_1xN", pyramid_1xN},
    {"pyramid", "owns_and_shares_data", pyramid_owns_data},
    // footprint
    {"footprint", "quotient_derivatives", footprint_quotient_derivatives},
    {"footprint", "viewport_offsets", footprint_viewport_offsets},
    {"footprint", "point_line_no_residual", footprint_no_residual_point_line},
    {"footprint", "disabled_interpolation", footprint_disabled_interpolation},
    {"footprint", "framebuf_resize", footprint_framebuf_resize},
    {"footprint", "lod_repeat_trilinear", footprint_lod_and_repeat},
    // shading
    {"shading", "pixel_specular_and_lod", shading_pixel_and_lod},
    {"shading", "occluded_fragments_skip_fs", shading_occluded_no_fs},
    {"shading", "winding_and_mirror_handedness", shading_winding_and_mirror},
    {"shading", "offdiagonal_sigma", shading_sigma_offdiagonal},
    {"shading", "specular_formula_and_gating", shading_specular_formula},
    {"shading", "interleaved_shaders", shading_interleaved_shaders},
    {"shading", "shared_source_released", shading_shared_source_released},
};

extern const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
