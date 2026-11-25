// https://github.com/dominiksalvet/cpurast

#pragma once

#include <vector>
#include <cstddef>

namespace cr
{
    // five raw moments of the normal slopes p = nx/nz, q = ny/nz
    // E[p], E[q], E[p^2], E[pq], E[q^2]
    struct normal_moment_moments
    {
        double p = 0;   // E[p]
        double q = 0;   // E[q]
        double pp = 0;  // E[p^2]
        double pq = 0;  // E[pq]
        double qq = 0;  // E[q^2]
    };

    // resolved texture footprint for one sampling operation
    struct normal_moment_footprint
    {
        double rho = 0;       // largest singular value of the texel-space Jacobian
        double lod_raw = 0;   // log2(max(1, rho))
        double lod = 0;       // lod_raw clamped to [0, last_level]
        int level0 = 0;
        int level1 = 0;
        double weight1 = 0;   // blend weight towards level1
    };

    // owning mip pyramid of the five slope moments
    //
    // Input normals are tightly packed as nx, ny, nz per texel in row-major order,
    // starting at the bottom row. All components must be finite and nz > 0.
    // The constructor copies the data; no reference to the source array is kept,
    // so a single instance can be shared by several shader objects.
    //
    // Level sizes are rounded up when halved. Every cell of a level is the
    // weighted average of its valid 2x2 sub-cells of the previous level; a
    // sub-cell's weight is the number of base texels it represents. Therefore
    // the last (1x1) cell always holds the full-image statistics.
    class normal_moments
    {
    public:
        normal_moments(const float* normals, unsigned width, unsigned height);
        normal_moments(const std::vector<float>& normals, unsigned width, unsigned height);
        normal_moments(const normal_moments&) = default;
        normal_moments& operator=(const normal_moments&) = default;

        unsigned width() const { return base_width; }
        unsigned height() const { return base_height; }
        unsigned level_count() const { return static_cast<unsigned>(levels.size()); }
        unsigned last_level() const { return static_cast<unsigned>(levels.size()) - 1; }

        // (width, height) of a mip level
        std::vector<unsigned> level_size(unsigned level) const;

        // direct access to a stored cell of a mip level
        normal_moment_moments moments(unsigned level, unsigned x, unsigned y) const;
        // number of base texels represented by a stored cell
        double cell_weight(unsigned level, unsigned x, unsigned y) const;

        // bilinear, repeat-addressed sampling of a single mip level
        // u, v are the original (not pre-moduloed) texture coordinates
        normal_moment_moments sample_level(unsigned level, double u, double v) const;

        // LOD/footprint resolution from the per-pixel derivatives du = (du/dx, dv/dx)
        // and dv = (du/dy, dv/dy). Derivatives are in texture coordinate units per
        // canvas pixel; they are multiplied by the original image dimensions here.
        normal_moment_footprint footprint(double du_u, double du_v,
                                          double dv_u, double dv_v) const;

        // bilinear + trilinear sampling. The five moments are blended first; no
        // derived variance is mixed across levels. footprint_out is optional.
        normal_moment_moments sample(double u, double v,
                                     double du_u, double du_v,
                                     double dv_u, double dv_v,
                                     normal_moment_footprint* footprint_out = nullptr) const;

    private:
        struct cell
        {
            double weight = 0;
            normal_moment_moments m;
        };

        struct level
        {
            unsigned width;
            unsigned height;
            std::vector<cell> cells;
        };

        void build(const float* normals);
        void build_next_level(const level& prev, level& next) const;

        unsigned base_width;
        unsigned base_height;
        std::vector<level> levels;
    };
}
