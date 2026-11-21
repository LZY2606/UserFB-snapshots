// LEAN (lean mapping) specular shaders for cpurast
//
// vertex packing (16 floats per input vertex):
//   [0..3]  homogeneous clip position (x, y, z, w), finite with w > 0
//   [4..6]  view-space position P
//   [7..9]  view-space normal N (nonzero)
//   [10..12] view-space tangent T (nonzero, not parallel to N)
//   [13]    tangent handedness, +1 or -1
//   [14..15] texture coordinates (u, v), never reduced modulo repeat
//
// the vertex shader divides position, normal, tangent, handedness and uv by w and
// appends 1/w; the fragment shader restores every attribute analytically and derives
// uv gradients with the quotient rule on the perspective-divided plane values.

#pragma once

#include "shader.hpp"
#include "normal_moments.hpp"
#include <array>
#include <memory>

using std::array;
using std::shared_ptr;

namespace cr
{
    namespace lean_attr
    {
        // input vertex layout
        constexpr unsigned CLIP_X = 0;
        constexpr unsigned CLIP_Y = 1;
        constexpr unsigned CLIP_Z = 2;
        constexpr unsigned CLIP_W = 3;
        constexpr unsigned VIEW_POS = 4;
        constexpr unsigned NORMAL = 7;
        constexpr unsigned TANGENT = 10;
        constexpr unsigned HANDEDNESS = 13;
        constexpr unsigned UV = 14;
        constexpr unsigned INPUT_SIZE = 16;

        // vertex shader output layout (all 3D vectors are divided by w)
        constexpr unsigned OUT_VIEW_POS = 0;   // P/w, 3
        constexpr unsigned OUT_NORMAL = 3;     // N/w, 3
        constexpr unsigned OUT_TANGENT = 6;    // T/w, 3
        constexpr unsigned OUT_HANDEDNESS = 9; // handedness / w
        constexpr unsigned OUT_UV = 10;        // u/w, v/w
        constexpr unsigned OUT_INV_W = 12;     // 1/w
        constexpr unsigned OUTPUT_SIZE = 13;
    }

    // convenience builder for one lean input vertex
    vector<float> make_lean_vertex(float cx, float cy, float cz, float cw,
                                   float px, float py, float pz,
                                   float nx, float ny, float nz,
                                   float tx, float ty, float tz,
                                   float handedness, float u, float v);

    class lean_vs : public vertex_shader
    {
    public:
        position run(const vector<float>& in, vector<float>& out) const override;
    };

    struct lean_diagnostics
    {
        color unclamped; // color before the [0, 1] framebuffer clamp
        normal_moments_texture::moments moments; // blended raw moments of the last fragment
        float lod = 0.f; // trilinear LOD used for the last fragment
        bool has_gradients = false; // false for points, lines and disabled interpolation
        unsigned fragment_count = 0; // fragments processed since the last reset
    };

    class lean_fs : public fragment_shader
    {
    public:
        explicit lean_fs(shared_ptr<const normal_moments_texture> texture);

        // diffuse/specular are linear colors in [0, 1], shininess >= 0,
        // light_dir is a unit vector pointing towards the light
        void set_material(const array<float, 3>& diffuse,
                          const array<float, 3>& specular,
                          float shininess,
                          const array<float, 3>& light_dir);

        bool wants_gradients() const override { return true; }

        // full gradient-aware path used by triangle fragments
        color run(const vector<float>& in,
                  const vector<float>& dx,
                  const vector<float>& dy,
                  bool has_gradients) const override;

        // legacy entry: identical shading with zero gradients (LOD 0),
        // also used for points, lines and disabled interpolation
        color run(const vector<float>& in) const override;

        const lean_diagnostics& diagnostics() const { return diag; }
        void reset_diagnostics() const { diag = lean_diagnostics{}; }

    private:
        color shade(const vector<float>& in,
                    const vector<float>& dx,
                    const vector<float>& dy,
                    bool has_gradients) const;

        shared_ptr<const normal_moments_texture> tex;

        array<float, 3> diffuse{0.8f, 0.8f, 0.8f};
        array<float, 3> specular{1.f, 1.f, 1.f};
        float shininess = 32.f;
        array<float, 3> light_dir{0.f, 0.f, 1.f};

        mutable lean_diagnostics diag;
    };
}
