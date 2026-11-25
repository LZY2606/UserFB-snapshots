// owning normal-distribution moment texture with a weighted mip pyramid
//
// the base level stores, per texel, the five raw moments of the slope
// coordinates p = nx/nz, q = ny/nz:
//   E[p], E[q], E[p^2], E[pq], E[q^2]
// higher levels pool 2x2 blocks of the level below, weighted by the number of
// base-level texels each block represents, so the single texel of the last
// level is the statistics of the whole image.

#pragma once

#include <memory>
#include <vector>

namespace cr
{
    // one input normal texel; every component must be finite and nz > 0
    struct normal_pixel
    {
        float nx;
        float ny;
        float nz;
    };

    // the five raw moments, in the fixed order E[p], E[q], E[p2], E[pq], E[q2]
    struct moments5
    {
        float m[5];
    };

    // result of a trilinear sample; keeps the blended raw moments and the LOD
    struct lean_sample
    {
        moments5 moments;
        float lod; // fractional LOD after clamping to [0, last_level]
    };

    class normal_moments_texture
    {
    public:
        // copies the input array; the texture never borrows it afterwards
        normal_moments_texture(const normal_pixel* data, unsigned width, unsigned height);
        normal_moments_texture(const std::vector<normal_pixel>& data, unsigned width, unsigned height);

        unsigned get_width() const;
        unsigned get_height() const;
        unsigned level_count() const; // number of mip levels (>= 1)
        unsigned level_width(unsigned level) const;
        unsigned level_height(unsigned level) const;

        // repeat-wrapped bilinear fetch of one level
        // u and v are used unmodified (no modulo before u*w - 0.5); only the
        // final texel addresses are wrapped
        moments5 sample_bilinear(float u, float v, unsigned level) const;

        // full sample: u/v derivatives are given per canvas pixel
        // (dudx, dudy) and (dvdx, dvdy); the largest singular value of the
        // scaled derivative matrix selects the clamped trilinear LOD
        lean_sample sample(float u, float v,
                           float dudx, float dudy,
                           float dvdx, float dvdy) const;

        // direct access to one stored texel of a level (diagnostics/tests)
        const moments5& texel(unsigned level, unsigned x, unsigned y) const;

    private:
        void build(const normal_pixel* data);

        unsigned base_w;
        unsigned base_h;
        unsigned levels;
        std::vector<unsigned> level_w; // width of each level
        std::vector<unsigned> level_h; // height of each level
        // one owning block of moments5 per level, row-major, origin at bottom
        std::vector<std::vector<moments5>> pyramid;
    };
}
