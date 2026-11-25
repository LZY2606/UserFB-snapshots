// LEAN (LEAN Mapping / linearly filtered normal moments) shaders for cpurast
//
// vertex input packing (LEAN_VERTEX_SIZE floats per vertex):
//   [0..3]   clip-space homogeneous position (x, y, z, w); finite, w > 0
//   [4..6]   view-space position
//   [7..9]   view-space normal (nonzero)
//   [10..12] view-space tangent (nonzero, not parallel to the normal)
//   [13]     tangent handedness, +1 or -1
//   [14..15] texture coordinates (u, v)
//
// vertex shader output (13 floats, all divided by w except the last reciprocal):
//   [0..1]   u/w, v/w
//   [2..4]   view position / w
//   [5..7]   normal / w
//   [8..10]  tangent / w
//   [11]     1/w
//   [12]     handedness / w
// the fragment shader recovers every attribute by dividing by the 1/w plane.

#pragma once

#include "shader.hpp"
#include "normal_moments.hpp"
#include <array>
#include <memory>

namespace cr
{
    constexpr unsigned LEAN_VERTEX_SIZE = 16;
    constexpr unsigned LEAN_OUTPUT_SIZE = 13;

    // pack one vertex following the layout above
    inline std::vector<float> pack_lean_vertex(
        std::array<float, 4> clip,
        std::array<float, 3> view_pos,
        std::array<float, 3> normal,
        std::array<float, 3> tangent,
        float handedness,
        std::array<float, 2> uv)
    {
        return {
            clip[0], clip[1], clip[2], clip[3],
            view_pos[0], view_pos[1], view_pos[2],
            normal[0], normal[1], normal[2],
            tangent[0], tangent[1], tangent[2],
            handedness,
            uv[0], uv[1]
        };
    }

    struct lean_material
    {
        color diffuse{0.8f, 0.8f, 0.8f};
        color specular{1.f, 1.f, 1.f};
        float shininess = 32.f; // must be >= 0
        std::array<float, 3> light_dir{0.f, 0.f, 1.f}; // unit direction L
    };

    class lean_vs : public vertex_shader
    {
    public:
        position run(const vector<float>& in, vector<float>& out) const override;
    };

    class lean_fs : public fragment_shader
    {
    public:
        // the texture is shared by ownership; several shaders may hold it
        lean_fs(std::shared_ptr<const normal_moments_texture> texture, lean_material material);

        // fallback for primitives without 2D gradients (points, lines, disabled
        // interpolation): samples the base level with zero derivatives
        color run(const vector<float>& in) const override;
        color run(const vector<float>& in, const vector<gradient>& gradients,
                  bool gradients_valid) const override;
        bool uses_gradients() const override;

        struct diagnostic
        {
            bool valid = false; // set true after every fragment run
            color unclamped{0.f, 0.f, 0.f}; // color before the [0,1] clamp
            lean_sample sample{}; // blended moments and LOD of the last fragment
        };

        // returns the diagnostics of the most recently processed fragment
        diagnostic last_diagnostic() const;

    private:
        color shade(const vector<float>& in, const vector<gradient>& gradients,
                    bool gradients_valid, diagnostic& diag) const;

        std::shared_ptr<const normal_moments_texture> texture;
        lean_material material;
        mutable diagnostic last_diag;
    };
}
