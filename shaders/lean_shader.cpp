#include "lean_shader.hpp"
#include <cmath>
#include <cassert>

namespace cr
{
    namespace
    {
        using v3 = array<float, 3>;

        constexpr float PI = 3.14159265358979323846f;

        float dot3(const v3& a, const v3& b)
        {
            return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        }

        v3 cross3(const v3& a, const v3& b)
        {
            return {a[1] * b[2] - a[2] * b[1],
                    a[2] * b[0] - a[0] * b[2],
                    a[0] * b[1] - a[1] * b[0]};
        }

        v3 normalize3(const v3& a)
        {
            const float len = std::sqrt(dot3(a, a));
            assert(len > 0.f);
            const float inv = 1.f / len;
            return {a[0] * inv, a[1] * inv, a[2] * inv};
        }

        // safe variant for vectors that may legitimately collapse to zero
        // (e.g. the half vector when light and view point in opposite directions)
        v3 safe_normalize3(const v3& a)
        {
            const float len = std::sqrt(dot3(a, a));
            if (!(len > 0.f)) return {0.f, 0.f, 0.f};
            const float inv = 1.f / len;
            return {a[0] * inv, a[1] * inv, a[2] * inv};
        }

        // derivative of the analytic quotient a/g, where g is the plane value of 1/w
        float quotient_deriv(float a, float da, float g, float dg)
        {
            const float g2 = g * g;
            return (da * g - a * dg) / g2;
        }

        float clamp01(float x)
        {
            return x < 0.f ? 0.f : (x > 1.f ? 1.f : x);
        }
    }

    vector<float> make_lean_vertex(float cx, float cy, float cz, float cw,
                                   float px, float py, float pz,
                                   float nx, float ny, float nz,
                                   float tx, float ty, float tz,
                                   float handedness, float u, float v)
    {
        return {cx, cy, cz, cw,
                px, py, pz,
                nx, ny, nz,
                tx, ty, tz,
                handedness, u, v};
    }

    position lean_vs::run(const vector<float>& in, vector<float>& out) const
    {
        assert(in.size() >= lean_attr::INPUT_SIZE);
        assert(std::isfinite(in[lean_attr::CLIP_W]) && in[lean_attr::CLIP_W] > 0.f);

        const float w = in[lean_attr::CLIP_W];
        const float inv_w = 1.f / w;

        out.resize(lean_attr::OUTPUT_SIZE);
        for (unsigned i = 0; i < 3; i++)
        {
            out[lean_attr::OUT_VIEW_POS + i] = in[lean_attr::VIEW_POS + i] * inv_w;
            out[lean_attr::OUT_NORMAL + i] = in[lean_attr::NORMAL + i] * inv_w;
            out[lean_attr::OUT_TANGENT + i] = in[lean_attr::TANGENT + i] * inv_w;
        }
        out[lean_attr::OUT_HANDEDNESS] = in[lean_attr::HANDEDNESS] * inv_w;
        out[lean_attr::OUT_UV + 0] = in[lean_attr::UV + 0] * inv_w;
        out[lean_attr::OUT_UV + 1] = in[lean_attr::UV + 1] * inv_w;
        out[lean_attr::OUT_INV_W] = inv_w;

        return {in[lean_attr::CLIP_X] * inv_w,
                in[lean_attr::CLIP_Y] * inv_w,
                in[lean_attr::CLIP_Z] * inv_w};
    }

    lean_fs::lean_fs(shared_ptr<const normal_moments_texture> texture) :
        tex(std::move(texture))
    {
        assert(tex != nullptr);
    }

    void lean_fs::set_material(const array<float, 3>& diffuse,
                               const array<float, 3>& specular,
                               float shininess,
                               const array<float, 3>& light_dir)
    {
        assert(shininess >= 0.f);
        assert(std::isfinite(light_dir[0]) && std::isfinite(light_dir[1]) && std::isfinite(light_dir[2]));
        this->diffuse = diffuse;
        this->specular = specular;
        this->shininess = shininess;
        this->light_dir = normalize3(light_dir);
    }

    color lean_fs::run(const vector<float>& in) const
    {
        const vector<float> zero(in.size(), 0.f);
        return shade(in, zero, zero, false);
    }

    color lean_fs::run(const vector<float>& in,
                       const vector<float>& dx,
                       const vector<float>& dy,
                       bool has_gradients) const
    {
        assert(in.size() == dx.size() && in.size() == dy.size());
        return shade(in, dx, dy, has_gradients);
    }

    color lean_fs::shade(const vector<float>& in,
                         const vector<float>& dx,
                         const vector<float>& dy,
                         bool has_gradients) const
    {
        assert(in.size() == lean_attr::OUTPUT_SIZE);

        // restore perspective-correct attributes from plane values
        const float iw = in[lean_attr::OUT_INV_W];
        assert(iw > 0.f);

        v3 P, N, T;
        for (unsigned i = 0; i < 3; i++)
        {
            P[i] = in[lean_attr::OUT_VIEW_POS + i] / iw;
            N[i] = in[lean_attr::OUT_NORMAL + i] / iw;
            T[i] = in[lean_attr::OUT_TANGENT + i] / iw;
        }
        const float handedness = in[lean_attr::OUT_HANDEDNESS] / iw;
        const float u = in[lean_attr::OUT_UV + 0] / iw;
        const float v = in[lean_attr::OUT_UV + 1] / iw;

        // analytic quotient derivatives of the restored uv over canvas pixels
        float dudx = 0.f, dvdx = 0.f, dudy = 0.f, dvdy = 0.f;
        if (has_gradients)
        {
            const float diw_dx = dx[lean_attr::OUT_INV_W];
            const float diw_dy = dy[lean_attr::OUT_INV_W];
            dudx = quotient_deriv(in[lean_attr::OUT_UV + 0], dx[lean_attr::OUT_UV + 0], iw, diw_dx);
            dudy = quotient_deriv(in[lean_attr::OUT_UV + 0], dy[lean_attr::OUT_UV + 0], iw, diw_dy);
            dvdx = quotient_deriv(in[lean_attr::OUT_UV + 1], dx[lean_attr::OUT_UV + 1], iw, diw_dx);
            dvdy = quotient_deriv(in[lean_attr::OUT_UV + 1], dy[lean_attr::OUT_UV + 1], iw, diw_dy);
        }

        // orthonormalize the current view-space TBN frame
        N = normalize3(N);
        v3 T_orth = T;
        const float nt = dot3(T_orth, N);
        T_orth = {T_orth[0] - N[0] * nt, T_orth[1] - N[1] * nt, T_orth[2] - N[2] * nt};
        T_orth = normalize3(T_orth);
        const v3 B = [&] {
            v3 b = cross3(N, T_orth);
            const float h = handedness >= 0.f ? 1.f : -1.f;
            return v3{b[0] * h, b[1] * h, b[2] * h};
        }();

        // view direction points from the surface towards the view-space origin
        const v3 L = light_dir;
        const v3 V = safe_normalize3(v3{-P[0], -P[1], -P[2]});
        const v3 H = safe_normalize3(v3{L[0] + V[0], L[1] + V[1], L[2] + V[2]});

        // move L, V, H into the current tangent frame (z axis is the normal)
        const v3 Lt = {dot3(L, T_orth), dot3(L, B), dot3(L, N)};
        const v3 Vt = {dot3(V, T_orth), dot3(V, B), dot3(V, N)};
        const v3 Ht = {dot3(H, T_orth), dot3(H, B), dot3(H, N)};

        // filtered five raw moments; repeat addressing only, uv is never reduced modulo here
        const normal_moments_texture::sample_result smp =
            tex->sample(u, v, dudx, dvdx, dudy, dvdy);

        // mean and covariance are derived only after blending the raw moments
        const float mu_p = smp.m[0];
        const float mu_q = smp.m[1];
        float c00 = smp.m[2] - mu_p * mu_p;
        const float c01 = smp.m[3] - mu_p * mu_q;
        float c11 = smp.m[4] - mu_q * mu_q;

        // zero out small negative eigenvalues produced by rounding, keeping the pq cross term
        const float tr = c00 + c11;
        const float det = c00 * c11 - c01 * c01;
        const float disc = std::sqrt(std::max(0.f, tr * tr - 4.f * det));
        const float lam0 = std::max(0.f, (tr - disc) * 0.5f);
        const float lam1 = std::max(0.f, (tr + disc) * 0.5f);

        // theta orients the eigenvector of the largest eigenvalue lam1
        const float theta = 0.5f * std::atan2(2.f * c01, c00 - c11);
        const float ct = std::cos(theta);
        const float st = std::sin(theta);
        const float eig_gap = lam1 - lam0;
        c00 = lam0 + eig_gap * ct * ct;
        c11 = lam0 + eig_gap * st * st;
        float c01_clamped = eig_gap * st * ct;

        // base roughness width added only after clamping
        const float base = 1.f / (shininess + 2.f);
        const float s00 = c00 + base;
        const float s11 = c11 + base;
        const float s01 = c01_clamped;

        color unclamped{0.f, 0.f, 0.f};

        // diffuse term
        const float n_dot_l = std::max(0.f, Lt[2]);
        unclamped.r = diffuse[0] * n_dot_l;
        unclamped.g = diffuse[1] * n_dot_l;
        unclamped.b = diffuse[2] * n_dot_l;

        // LEAN Gaussian specular term, no Fresnel or occlusion
        if (Ht[2] > 0.f && Lt[2] > 0.f && Vt[2] > 0.f)
        {
            const float det_s = s00 * s11 - s01 * s01;
            assert(det_s > 0.f);
            const float inv_det = 1.f / det_s;

            const float a0 = Ht[0] / Ht[2] - mu_p;
            const float a1 = Ht[1] / Ht[2] - mu_q;
            const float inv00 = s11 * inv_det;
            const float inv11 = s00 * inv_det;
            const float inv01 = -s01 * inv_det;
            const float quad = a0 * inv00 * a0 + 2.f * a0 * inv01 * a1 + a1 * inv11 * a1;

            const float D = std::exp(-0.5f * quad) /
                            (2.f * PI * std::sqrt(det_s) * std::pow(Ht[2], 4.f));
            const float spec = D / (4.f * Vt[2]);

            unclamped.r += specular[0] * spec;
            unclamped.g += specular[1] * spec;
            unclamped.b += specular[2] * spec;
        }

        diag.unclamped = unclamped;
        diag.moments = smp.m;
        diag.lod = smp.lod;
        diag.has_gradients = has_gradients;
        diag.fragment_count++;

        return {clamp01(unclamped.r), clamp01(unclamped.g), clamp01(unclamped.b)};
    }
}
