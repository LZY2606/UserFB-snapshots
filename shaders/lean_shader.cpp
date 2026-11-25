#include "lean_shader.hpp"
#include <cmath>
#include <cassert>

namespace cr
{
    namespace
    {
        struct vec3
        {
            float x, y, z;
        };

        vec3 make_vec3(const float* p)
        {
            return {p[0], p[1], p[2]};
        }

        float dot3(vec3 a, vec3 b)
        {
            return a.x * b.x + a.y * b.y + a.z * b.z;
        }

        vec3 cross3(vec3 a, vec3 b)
        {
            return {a.y * b.z - a.z * b.y,
                    a.z * b.x - a.x * b.z,
                    a.x * b.y - a.y * b.x};
        }

        vec3 scale3(vec3 a, float s)
        {
            return {a.x * s, a.y * s, a.z * s};
        }

        vec3 sub3(vec3 a, vec3 b)
        {
            return {a.x - b.x, a.y - b.y, a.z - b.z};
        }

        vec3 add3(vec3 a, vec3 b)
        {
            return {a.x + b.x, a.y + b.y, a.z + b.z};
        }

        vec3 normalize3(vec3 a)
        {
            const float len = std::sqrt(dot3(a, a));
            assert(len > 0.f);
            return scale3(a, 1.f / len);
        }

        // half-vector normalization: an exactly opposing L, V yields a zero
        // sum, which cannot be normalized; return zero so the H.z gate rejects it
        vec3 normalize_half(vec3 a)
        {
            const float len = std::sqrt(dot3(a, a));
            if (!(len > 0.f)) {
                return {0.f, 0.f, 0.f};
            }
            return scale3(a, 1.f / len);
        }

        // analytic derivative of the quotient g/r, where both are affine
        // attribute planes with screen-space derivatives dg, dr
        float quotient_deriv(float g, float dg, float r, float dr)
        {
            return (dg * r - g * dr) / (r * r);
        }

        constexpr float PI = 3.14159265358979323846f;
    }

    position lean_vs::run(const vector<float>& in, vector<float>& out) const
    {
        assert(in.size() >= LEAN_VERTEX_SIZE);

        const float w = in[3];
        assert(std::isfinite(w) && w > 0.f);
        const float inv_w = 1.f / w;

        out.resize(LEAN_OUTPUT_SIZE);
        // perspective-divided attributes plus the reciprocal itself
        out[0] = in[14] * inv_w; // u / w
        out[1] = in[15] * inv_w; // v / w
        for (unsigned i = 0; i < 3u; i++)
        {
            out[2 + i] = in[4 + i] * inv_w;  // view position / w
            out[5 + i] = in[7 + i] * inv_w;  // normal / w
            out[8 + i] = in[10 + i] * inv_w; // tangent / w
        }
        out[11] = inv_w;            // 1 / w
        out[12] = in[13] * inv_w;   // handedness / w

        return {in[0] * inv_w, in[1] * inv_w, in[2] * inv_w};
    }

    lean_fs::lean_fs(std::shared_ptr<const normal_moments_texture> texture, lean_material material) :
        texture(std::move(texture)),
        material(std::move(material))
    {
        assert(this->texture != nullptr);
        assert(material.shininess >= 0.f);
        const float ll = std::sqrt(dot3(make_vec3(material.light_dir.data()),
                                        make_vec3(material.light_dir.data())));
        assert(ll > 0.f);
    }

    color lean_fs::run(const vector<float>& in) const
    {
        static const vector<gradient> no_gradients;
        return shade(in, no_gradients, false, last_diag);
    }

    color lean_fs::run(const vector<float>& in, const vector<gradient>& gradients,
                       bool gradients_valid) const
    {
        return shade(in, gradients, gradients_valid, last_diag);
    }

    bool lean_fs::uses_gradients() const
    {
        return true;
    }

    lean_fs::diagnostic lean_fs::last_diagnostic() const
    {
        return last_diag;
    }

    color lean_fs::shade(const vector<float>& in, const vector<gradient>& gradients,
                         bool gradients_valid, diagnostic& diag) const
    {
        assert(in.size() == LEAN_OUTPUT_SIZE);

        // recover the perspective-divided attributes through the 1/w plane
        const float r = in[11]; // 1/w at this fragment
        assert(r > 0.f);

        const float u = in[0] / r;
        const float v = in[1] / r;
        const vec3 view_pos = scale3(make_vec3(&in[2]), 1.f / r);
        vec3 normal = normalize3(scale3(make_vec3(&in[5]), 1.f / r));
        vec3 tangent = scale3(make_vec3(&in[8]), 1.f / r);
        const float handedness = in[12] / r;

        // texture-coordinate derivatives from analytic quotients of affine planes
        float dudx = 0.f, dudy = 0.f, dvdx = 0.f, dvdy = 0.f;
        if (gradients_valid)
        {
            assert(gradients.size() == LEAN_OUTPUT_SIZE);
            const gradient& gu = gradients[0];
            const gradient& gv = gradients[1];
            const gradient& gr = gradients[11];

            dudx = quotient_deriv(in[0], gu.dx, r, gr.dx);
            dudy = quotient_deriv(in[0], gu.dy, r, gr.dy);
            dvdx = quotient_deriv(in[1], gv.dx, r, gr.dx);
            dvdy = quotient_deriv(in[1], gv.dy, r, gr.dy);
        }

        // orthogonalize the tangent against the normal in current view space
        const float nn = dot3(normal, normal);
        tangent = sub3(tangent, scale3(normal, dot3(normal, tangent) / nn));
        tangent = normalize3(tangent);
        const vec3 bitangent = scale3(cross3(normal, tangent), handedness); // B = h * (N x T)

        // directions in view space
        const vec3 V = normalize3(scale3(view_pos, -1.f)); // toward the view origin
        const vec3 L = normalize3(make_vec3(material.light_dir.data()));
        const vec3 H = normalize_half(add3(L, V));

        // move L, V, H into the current tangent frame
        const auto to_tbn = [&](vec3 d) {
            return vec3{dot3(d, tangent), dot3(d, bitangent), dot3(d, normal)};
        };
        const vec3 Lt = to_tbn(L);
        const vec3 Vt = to_tbn(V);
        const vec3 Ht = to_tbn(H);

        // filtered normal moments; raw moments are blended inside the texture
        const lean_sample smp = texture->sample(u, v, dudx, dudy, dvdx, dvdy);
        const float mp = smp.moments.m[0];
        const float mq = smp.moments.m[1];
        const float mp2 = smp.moments.m[2];
        const float mpq = smp.moments.m[3];
        const float mq2 = smp.moments.m[4];

        // covariance from the blended raw moments (never blended directly)
        float c00 = mp2 - mp * mp;
        float c01 = mpq - mp * mq;
        float c11 = mq2 - mq * mq;

        // spectral cleanup: push tiny negative eigenvalues caused by rounding to zero
        {
            const float tr = c00 + c11;
            const float det = c00 * c11 - c01 * c01;
            const float disc = std::sqrt(std::max(0.f, tr * tr - 4.f * det));
            float l0 = std::max(0.f, 0.5f * (tr - disc));
            float l1 = std::max(0.f, 0.5f * (tr + disc));
            if (c00 + c11 > 0.f)
            {
                // eigenvector of the smaller eigenvalue
                vec3 e0;
                if (std::abs(c01) > 1e-12f) {
                    e0 = normalize3({l0 - c11, c01, 0.f});
                } else {
                    e0 = (c00 < c11) ? vec3{1.f, 0.f, 0.f} : vec3{0.f, 1.f, 0.f};
                }
                const vec3 e1 = {-e0.y, e0.x, 0.f};
                c00 = l0 * e0.x * e0.x + l1 * e1.x * e1.x;
                c01 = l0 * e0.x * e0.y + l1 * e1.x * e1.y;
                c11 = l0 * e0.y * e0.y + l1 * e1.y * e1.y;
            }
            else
            {
                c00 = c01 = c11 = 0.f;
            }
        }

        // base roughness width from the Phong-style shininess
        const float base_width = 1.f / (material.shininess + 2.f);
        const float s00 = c00 + base_width;
        const float s01 = c01;
        const float s11 = c11 + base_width;

        // LEAN specular NDF evaluation
        vec3 specular{0.f, 0.f, 0.f};
        const float hz = Ht.z;
        if (hz > 0.f && Lt.z > 0.f && Vt.z > 0.f)
        {
            const float det_s = s00 * s11 - s01 * s01;
            assert(det_s > 0.f);
            const float inv_det = 1.f / det_s;

            const vec3 a_vec{Ht.x / hz - mp, Ht.y / hz - mq, 0.f};
            const float iax = inv_det * (s11 * a_vec.x - s01 * a_vec.y);
            const float iay = inv_det * (-s01 * a_vec.x + s00 * a_vec.y);
            const float quad = a_vec.x * iax + a_vec.y * iay;

            const float D = std::exp(-0.5f * quad) /
                            (2.f * PI * std::sqrt(det_s) * hz * hz * hz * hz);
            const float intensity = D / (4.f * Vt.z);
            specular = {material.specular.r * intensity,
                        material.specular.g * intensity,
                        material.specular.b * intensity};
        }

        const float ndotl = std::max(0.f, Lt.z);
        const color unclamped{
            material.diffuse.r * ndotl + specular.x,
            material.diffuse.g * ndotl + specular.y,
            material.diffuse.b * ndotl + specular.z};

        diag.valid = true;
        diag.unclamped = unclamped;
        diag.sample = smp;

        // clamp per channel before entering the normalized color buffer
        const auto clamp01 = [](float x) {
            return std::min(1.f, std::max(0.f, x));
        };
        return {clamp01(unclamped.r), clamp01(unclamped.g), clamp01(unclamped.b)};
    }
}
