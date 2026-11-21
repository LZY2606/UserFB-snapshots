// normal moments texture for lean (LEAN) specular shading
//
// owns all of its storage: the constructor copies the caller's normal array, so the
// array may be released immediately afterwards. several shaders may share one texture
// instance through a shared_ptr.

#pragma once

#include <array>
#include <vector>
#include <memory>

using std::array;
using std::vector;

namespace cr
{
    class normal_moments_texture
    {
    public:
        // five raw moments of the (p, q) = (nx/nz, ny/nz) distribution:
        // E[p], E[q], E[p^2], E[p*q], E[q^2]
        static constexpr unsigned MOMENT_COUNT = 5;
        using moments = array<float, MOMENT_COUNT>;

        struct sample_result
        {
            moments m; // blended raw moments (variances/means are never precomputed)
            float lod; // fractional trilinear LOD actually used
        };

        normal_moments_texture() = default;

        // normals points to width*height triples (nx, ny, nz); every component must be
        // finite and nz > 0. the pointer is not retained after construction.
        normal_moments_texture(unsigned width, unsigned height, const float* normals);
        normal_moments_texture(unsigned width, unsigned height, const vector<float>& normals);

        unsigned base_width() const { return base_w; }
        unsigned base_height() const { return base_h; }
        unsigned level_count() const { return unsigned(levels.size()); }
        unsigned level_width(unsigned level) const;
        unsigned level_height(unsigned level) const;

        // raw moments of one texel at one mip level (diagnostics and direct testing)
        const moments& texel(unsigned level, unsigned x, unsigned y) const;

        // repeat-wrapped bilinear fetch at one mip level; u/v are used unmodified
        // except for repeat addressing
        moments fetch_bilinear(unsigned level, float u, float v) const;

        // full filtered lookup.
        // u, v: texture coordinates without any modulo (repeat is applied for addressing only)
        // dudx, dvdx, dudy, dvdy: uv derivatives per one canvas pixel
        sample_result sample(float u, float v,
                             float dudx, float dvdx,
                             float dudy, float dvdy) const;

    private:
        void build(unsigned width, unsigned height, const float* normals);
        const moments& level_texel(unsigned level, int x, int y) const;

        unsigned base_w = 0;
        unsigned base_h = 0;
        // one contiguous texel array per mip level, level 0 is the base
        vector<vector<moments>> levels;
    };
}
