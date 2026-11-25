// https://github.com/dominiksalvet/cpurast

#include "lean_shader.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>

namespace cr
{
    namespace
    {
        constexpr double kPi = 3.14159265358979323846;

        void cross3(const double a[3], const double b[3], double r[3])
        {
            r[0] = a[1] * b[2] - a[2] * b[1];
            r[1] = a[2] * b[0] - a[0] * b[2];
            r[2] = a[0] * b[1] - a[1] * b[0];
        }

        double dot3(const double a[3], const double b[3]) {
            return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        }

        bool normalize3(double a[3])
        {
            const double len = std::sqrt(dot3(a, a));
            if (!(len > 0.0) || !std::isfinite(len)) {
                return false;
            }
            a[0] /= len;
            a[1] /= len;
            a[2] /= len;
            return true;
        }

        bool inverse_sym2(double a, double b, double c, double out[3])
        {
            const double det = a * c - b * b;
            if (!(std::abs(det) > 1e-30) || !std::isfinite(det)) {
                return false;
            }
            const double inv = 1.0 / det;
            out[0] = c * inv;   // (0, 0)
            out[1] = -b * inv;  // (0, 1) = (1, 0)
            out[2] = a * inv;   // (1, 1)
            return true;
        }

        // eigen decomposition of a symmetric 2x2 matrix [[a, b], [b, d]];
        // eigenvalues ascending, eigenvectors stored as columns
        void eigen_sym2(double a, double b, double d,
                        double evals[2], double evecs[2][2])
        {
            const double half_tr = 0.5 * (a + d);
            const double half_diff = 0.5 * (a - d);
            const double root = std::sqrt(std::max(0.0, half_diff * half_diff + b * b));

            evals[0] = half_tr - root;
            evals[1] = half_tr + root;

            if (std::abs(b) > 1e-30) {
                double v0[2] = {b, evals[0] - a};
                const double len = std::sqrt(v0[0] * v0[0] + v0[1] * v0[1]);
                if (len > 0) {
                    v0[0] /= len;
                    v0[1] /= len;
                }
                evecs[0][0] = v0[0];
                evecs[0][1] = v0[1];
                evecs[1][0] = -v0[1];
                evecs[1][1] = v0[0];
            } else if (a <= d) {
                evecs[0][0] = 1; evecs[0][1] = 0;
                evecs[1][0] = 0; evecs[1][1] = 1;
            } else {
                evecs[0][0] = 0; evecs[0][1] = 1;
                evecs[1][0] = 1; evecs[1][1] = 0;
            }
        }
    }

    namespace lean
    {
        std::vector<float> pack_vertex(const float* clip_position4,
                                       const float* view_position3,
                                       const float* normal3,
                                       const float* tangent3,
                                       float handedness, float u, float v)
        {
            std::vector<float> out;
            out.reserve(input_attrib_count);
            for (unsigned i = 0; i < 4; i++) out.push_back(clip_position4[i]);
            for (unsigned i = 0; i < 3; i++) out.push_back(view_position3[i]);
            for (unsigned i = 0; i < 3; i++) out.push_back(normal3[i]);
            for (unsigned i = 0; i < 3; i++) out.push_back(tangent3[i]);
            out.push_back(handedness);
            out.push_back(u);
            out.push_back(v);
            return out;
        }
    }

    position lean_vertex_shader::run(const vector<float>& in, vector<float>& out) const
    {
        assert(in.size() >= lean::input_attrib_count);

        const float w = in[3];
        assert(w > 0.f);

        const float inv_w = 1.f / w;

        // homogeneous clip position divided by w
        const position pos{in[0] * inv_w, in[1] * inv_w, in[2] * inv_w};

        out.resize(lean::output_attrib_count);

        // uv / w
        out[lean::out_uv + 0] = in[14] * inv_w;
        out[lean::out_uv + 1] = in[15] * inv_w;

        // view-space position / w
        for (unsigned i = 0; i < 3; i++) {
            out[lean::out_view_pos + i] = in[4 + i] * inv_w;
        }

        // normal / w
        for (unsigned i = 0; i < 3; i++) {
            out[lean::out_normal + i] = in[7 + i] * inv_w;
        }

        // tangent / w
        for (unsigned i = 0; i < 3; i++) {
            out[lean::out_tangent + i] = in[10 + i] * inv_w;
        }

        // 1 / w
        out[lean::out_inv_w] = inv_w;

        // handedness is a sign, not a homogeneous quantity; do not divide it
        out[lean::out_handedness] = in[13];

        return pos;
    }

    lean_fragment_shader::lean_fragment_shader(std::shared_ptr<const normal_moments> moments,
                                               lean_material material) :
        moments(std::move(moments)),
        mat(material)
    {
        assert(this->moments != nullptr);
    }

    lean_restored_attributes lean_fragment_shader::restore(const std::vector<float>& value,
                                                           const std::vector<float>& dx,
                                                           const std::vector<float>& dy,
                                                           bool has_2d_gradient) const
    {
        assert(value.size() >= lean::output_attrib_count);
        assert(dx.size() == value.size());
        assert(dy.size() == value.size());

        lean_restored_attributes r;

        // packed a = A / w plus 1/w; then
        //   A   = a * w
        //   A'  = a' * w + a * w',  w' = -(1/w)' / (1/w)^2
        // This is the analytic quotient rule, not a difference of interpolated
        // neighbours; the derivatives stay per canvas pixel.
        const double iw = value[lean::out_inv_w];
        assert(std::isfinite(iw) && iw != 0.0);
        const double iw_dx = has_2d_gradient ? dx[lean::out_inv_w] : 0.0;
        const double iw_dy = has_2d_gradient ? dy[lean::out_inv_w] : 0.0;

        r.w = 1.0 / iw;
        const double w_dx = -iw_dx / (iw * iw);
        const double w_dy = -iw_dy / (iw * iw);

        for (unsigned i = 0; i < 2; i++) {
            const double a = value[lean::out_uv + i];
            r.uv[i] = a * r.w;
            r.uv_dx[i] = (has_2d_gradient ? dx[lean::out_uv + i] : 0.0) * r.w + a * w_dx;
            r.uv_dy[i] = (has_2d_gradient ? dy[lean::out_uv + i] : 0.0) * r.w + a * w_dy;
        }

        for (unsigned i = 0; i < 3; i++) {
            const double a_p = value[lean::out_view_pos + i];
            const double a_n = value[lean::out_normal + i];
            const double a_t = value[lean::out_tangent + i];

            r.view_pos[i] = a_p * r.w;
            r.normal[i] = a_n * r.w;
            r.tangent[i] = a_t * r.w;

            r.view_pos_dx[i] = (has_2d_gradient ? dx[lean::out_view_pos + i] : 0.0) * r.w + a_p * w_dx;
            r.view_pos_dy[i] = (has_2d_gradient ? dy[lean::out_view_pos + i] : 0.0) * r.w + a_p * w_dy;
            r.normal_dx[i] = (has_2d_gradient ? dx[lean::out_normal + i] : 0.0) * r.w + a_n * w_dx;
            r.normal_dy[i] = (has_2d_gradient ? dy[lean::out_normal + i] : 0.0) * r.w + a_n * w_dy;
            r.tangent_dx[i] = (has_2d_gradient ? dx[lean::out_tangent + i] : 0.0) * r.w + a_t * w_dx;
            r.tangent_dy[i] = (has_2d_gradient ? dy[lean::out_tangent + i] : 0.0) * r.w + a_t * w_dy;
        }

        r.handedness = value[lean::out_handedness] >= 0.f ? 1.0 : -1.0;
        return r;
    }

    color lean_fragment_shader::run(const vector<float>& in) const
    {
        // legacy entry point: no gradient information (zero derivatives)
        return shade(in, in, in, false);
    }

    color lean_fragment_shader::run_gradient(const fragment_gradient& in) const
    {
        return shade(in.value, in.dx, in.dy, in.has_2d_gradient);
    }

    color lean_fragment_shader::shade(const vector<float>& value,
                                      const vector<float>& dx,
                                      const vector<float>& dy,
                                      bool has_2d_gradient) const
    {
        const lean_restored_attributes a = restore(value, dx, dy, has_2d_gradient);

        // reset diagnostics for every fragment
        diag = lean_sample_diagnostics{};
        diag.has_2d_gradient = has_2d_gradient;
        diag.uv[0] = static_cast<float>(a.uv[0]);
        diag.uv[1] = static_cast<float>(a.uv[1]);

        // ---- current view-space orthonormal frame ----
        double n[3] = {a.normal[0], a.normal[1], a.normal[2]};
        if (!normalize3(n)) {
            return diag.clamped;
        }

        // Gram-Schmidt: remove the component of T along N
        double t[3] = {a.tangent[0], a.tangent[1], a.tangent[2]};
        const double tn = dot3(t, n);
        for (unsigned i = 0; i < 3; i++) {
            t[i] -= tn * n[i];
        }
        if (!normalize3(t)) {
            return diag.clamped;
        }

        // B = handedness * cross(N, T), handedness is +1 or -1
        double b[3];
        cross3(n, t, b);
        for (unsigned i = 0; i < 3; i++) {
            b[i] *= a.handedness;
        }

        // tangent-space basis vectors; axes[0] = T, axes[1] = B, axes[2] = N,
        // so dot(L, axes[j]) is the j-th component of L in the TBN frame
        const double axes[3][3] = {
            {t[0], t[1], t[2]},
            {b[0], b[1], b[2]},
            {n[0], n[1], n[2]}
        };

        // ---- texture coordinates and derivatives per canvas pixel ----
        // repeat is only the addressing mode inside normal_moments; the original
        // UVs (including values outside [0, 1]) are handed over unmodified
        const double tu = a.uv[0];
        const double tv = a.uv[1];
        const double du_dx = a.uv_dx[0];
        const double dv_dx = a.uv_dy[0];
        const double du_dy = a.uv_dx[1];
        const double dv_dy = a.uv_dy[1];

        const bool finite = std::isfinite(tu) && std::isfinite(tv) &&
                            std::isfinite(du_dx) && std::isfinite(dv_dx) &&
                            std::isfinite(du_dy) && std::isfinite(dv_dy);

        normal_moment_moments m;
        normal_moment_footprint fp;
        if (finite) {
            m = moments->sample(tu, tv, du_dx, dv_dx, du_dy, dv_dy, &fp);
        }

        // ---- covariance of the slopes from the blended raw moments ----
        const double mu_p = m.p;
        const double mu_q = m.q;
        double c_pp = m.pp - mu_p * mu_p;
        double c_pq = m.pq - mu_p * mu_q;
        double c_qq = m.qq - mu_q * mu_q;

        // clamp tiny negative eigenvalues caused by rounding BEFORE adding width
        double evals[2];
        double evecs[2][2];
        eigen_sym2(c_pp, c_pq, c_qq, evals, evecs);
        evals[0] = std::max(0.0, evals[0]);
        evals[1] = std::max(0.0, evals[1]);
        c_pp = evecs[0][0] * evals[0] * evecs[0][0] + evecs[1][0] * evals[1] * evecs[1][0];
        c_pq = evecs[0][0] * evals[0] * evecs[0][1] + evecs[1][0] * evals[1] * evecs[1][1];
        c_qq = evecs[0][1] * evals[0] * evecs[0][1] + evecs[1][1] * evals[1] * evecs[1][1];

        const double shininess = mat.shininess > 0.f ? double(mat.shininess) : 0.0;
        const double base = 1.0 / (shininess + 2.0);
        const double s_pp = c_pp + base; // Sigma = C + I / (shininess + 2)
        const double s_pq = c_pq;        // the pq cross term is kept
        const double s_qq = c_qq + base;
        const double det = s_pp * s_qq - s_pq * s_pq;
        double inv[3] = {0, 0, 0};
        inverse_sym2(s_pp, s_pq, s_qq, inv);

        // ---- view-space shading vectors ----
        double L[3] = {mat.light[0], mat.light[1], mat.light[2]};
        double V[3] = {-a.view_pos[0], -a.view_pos[1], -a.view_pos[2]}; // surface -> origin
        double H[3];

        bool have_lvh = normalize3(L) && normalize3(V);
        if (have_lvh) {
            H[0] = L[0] + V[0];
            H[1] = L[1] + V[1];
            H[2] = L[2] + V[2];
            have_lvh = normalize3(H);
        }

        // L.z in tangent space is dot(L, N); diffuse = diffuse * max(L.z, 0)
        const double lz_axis = dot3(L, n);
        const double diff[3] = {
            mat.diffuse[0] * std::max(0.0, lz_axis),
            mat.diffuse[1] * std::max(0.0, lz_axis),
            mat.diffuse[2] * std::max(0.0, lz_axis)
        };

        double spec[3] = {0, 0, 0};
        bool spec_active = false;

        if (have_lvh) {
            // transform L, V, H into the current TBN
            double l_t[3], v_t[3], h_t[3];
            for (unsigned j = 0; j < 3; j++) {
                l_t[j] = dot3(L, axes[j]);
                v_t[j] = dot3(V, axes[j]);
                h_t[j] = dot3(H, axes[j]);
            }

            const double hz = h_t[2];
            const double lz = l_t[2];
            const double vz = v_t[2];

            // specular vanishes when any tangent-space z component is non-positive
            if (hz > 0 && lz > 0 && vz > 0 && det > 0.0 && std::isfinite(det)) {
                // a = H.xy / H.z - mu
                const double a0 = h_t[0] / hz - mu_p;
                const double a1 = h_t[1] / hz - mu_q;

                double qf = a0 * (inv[0] * a0 + inv[1] * a1)
                          + a1 * (inv[1] * a0 + inv[2] * a1);
                if (!(qf >= 0.0)) {
                    qf = 0.0;
                }

                const double hz4 = hz * hz * hz * hz;
                double D = std::exp(-0.5 * qf) / (2.0 * kPi * std::sqrt(det) * hz4);
                if (!std::isfinite(D)) {
                    D = 0.0;
                }

                // specular = specular color * D / (4 * V.z)
                const double scale = D / (4.0 * vz);
                spec[0] = double(mat.specular[0]) * scale;
                spec[1] = double(mat.specular[1]) * scale;
                spec[2] = double(mat.specular[2]) * scale;
                spec_active = true;
            }
        }

        // ---- linear color: diffuse * max(L.z, 0) + specular ----
        const double lin[3] = {diff[0] + spec[0], diff[1] + spec[1], diff[2] + spec[2]};

        // diagnostics retain the unclamped values, sampled moments and LOD
        diag.unclamped[0] = static_cast<float>(lin[0]);
        diag.unclamped[1] = static_cast<float>(lin[1]);
        diag.unclamped[2] = static_cast<float>(lin[2]);
        diag.specular_active = spec_active;
        diag.moments = m;
        diag.footprint = fp;
        diag.mu_p = static_cast<float>(mu_p);
        diag.mu_q = static_cast<float>(mu_q);
        diag.cov_pp = static_cast<float>(c_pp);
        diag.cov_pq = static_cast<float>(c_pq);
        diag.cov_qq = static_cast<float>(c_qq);

        // clamp channel-wise before entering the normalized color buffer
        const color c{
            static_cast<float>(std::min(1.0, std::max(0.0, lin[0]))),
            static_cast<float>(std::min(1.0, std::max(0.0, lin[1]))),
            static_cast<float>(std::min(1.0, std::max(0.0, lin[2]))
        )};
        diag.clamped = c;
        return c;
    }
}
