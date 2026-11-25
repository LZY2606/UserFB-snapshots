// https://github.com/dominiksalvet/cpurast

#pragma once

#include "shader.hpp"
#include "normal_moments.hpp"
#include <memory>

namespace cr
{
    // tight input packing for the lean vertex shader
    //
    //   [0..3]   homogeneous clip position x, y, z, w
    //   [4..6]   view-space position
    //   [7..9]   view-space normal
    //   [10..12] view-space tangent
    //   [13]     tangent handedness (+1 or -1)
    //   [14..15] texture coordinates u, v
    //
    // The vertex shader divides the texture coordinates, the view-space position,
    // the normal, and the tangent by w and appends 1/w; the fragment shader
    // restores them with the analytic quotient rule.
    namespace lean
    {
        constexpr unsigned input_attrib_count = 16;
        constexpr unsigned output_attrib_count = 13;

        // packed vertex shader output layout
        constexpr unsigned out_uv = 0;          // [0..1]   uv / w
        constexpr unsigned out_view_pos = 2;    // [2..4]   view-space position / w
        constexpr unsigned out_normal = 5;      // [5..7]   normal / w
        constexpr unsigned out_tangent = 8;     // [8..10]  tangent / w
        constexpr unsigned out_inv_w = 11;      //          1 / w
        constexpr unsigned out_handedness = 12; //          handedness, not divided

        // helper creating the packed input attribute vector
        std::vector<float> pack_vertex(const float* clip_position4,
                                       const float* view_position3,
                                       const float* normal3,
                                       const float* tangent3,
                                       float handedness, float u, float v);
    }

    // attributes restored from the packed vertex shader output with the analytic
    // quotient rule; derivatives are per canvas pixel
    struct lean_restored_attributes
    {
        double w = 1.0;
        double uv[2]{0.0, 0.0};
        double uv_dx[2]{0.0, 0.0};
        double uv_dy[2]{0.0, 0.0};
        double view_pos[3]{0.0, 0.0, 0.0};
        double view_pos_dx[3]{0.0, 0.0, 0.0};
        double view_pos_dy[3]{0.0, 0.0, 0.0};
        double normal[3]{0.0, 0.0, 0.0};
        double normal_dx[3]{0.0, 0.0, 0.0};
        double normal_dy[3]{0.0, 0.0, 0.0};
        double tangent[3]{0.0, 0.0, 0.0};
        double tangent_dx[3]{0.0, 0.0, 0.0};
        double tangent_dy[3]{0.0, 0.0, 0.0};
        double handedness = 1.0;
    };

    // material parameters of the lean slope-moment shader
    struct lean_material
    {
        float diffuse[3]{0.5f, 0.5f, 0.5f};
        float specular[3]{1.f, 1.f, 1.f};
        float shininess = 16.f;        // >= 0
        float light[3]{0.f, 0.f, 1.f}; // unit direction in view space
    };

    // diagnostics of the most recent lean fragment shading operation
    struct lean_sample_diagnostics
    {
        color clamped{0.f, 0.f, 0.f};
        float unclamped[3]{0.f, 0.f, 0.f};
        bool specular_active = false;
        cr::normal_moment_moments moments;
        cr::normal_moment_footprint footprint;
        float mu_p = 0.f;
        float mu_q = 0.f;
        float cov_pp = 0.f;
        float cov_pq = 0.f;
        float cov_qq = 0.f;
        float uv[2]{0.f, 0.f}; // restored (not divided) texture coordinates
        bool has_2d_gradient = false;
    };

    class lean_vertex_shader : public vertex_shader
    {
    public:
        position run(const vector<float>& in, vector<float>& out) const override;
    };

    // lean slope-moment fragment shader
    // It implements the optional gradient interface so the production rasterizer
    // can supply the common affine attribute plane per pixel. The plain run(in)
    // entry point remains available and uses zero gradients.
    class lean_fragment_shader : public gradient_fragment_shader
    {
    public:
        explicit lean_fragment_shader(std::shared_ptr<const normal_moments> moments,
                                      lean_material material = {});

        color run(const vector<float>& in) const override;
        color run_gradient(const fragment_gradient& in) const override;

        // restore the divided attributes with the analytic quotient rule
        lean_restored_attributes restore(const std::vector<float>& value,
                                         const std::vector<float>& dx,
                                         const std::vector<float>& dy,
                                         bool has_2d_gradient) const;

        // diagnostic access to the most recent shading result
        const lean_sample_diagnostics& diagnostics() const { return diag; }

        const lean_material& material() const { return mat; }
        void set_material(const lean_material& material) { mat = material; }

        const std::shared_ptr<const normal_moments>& moments_texture() const { return moments; }

    private:
        color shade(const vector<float>& value,
                    const std::vector<float>& dx,
                    const std::vector<float>& dy,
                    bool has_2d_gradient) const;

        std::shared_ptr<const normal_moments> moments;
        lean_material mat;
        mutable lean_sample_diagnostics diag;
    };
}
