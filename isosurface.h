#ifndef ISOSURFACE_H
#define ISOSURFACE_H

// IsoGrid: a regular scalar grid whose F = isovalue level set is intersected
// directly (no triangle extraction, no fixed-step marching).  Inside the
// bounding box each cell is trilinearly interpolated, so along a ray the
// residual is a cubic polynomial whose roots are found analytically; outside
// the box there is no surface and the box is never capped.

#include <array>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <vector>

#include "renderer.h"

struct IsoHit {
    double t = 0;                       // ray parameter of the hit
    vec3   point{0,0,0};                // world-space hit position
    vec3   normal{0,0,0};               // unit gradient, pointing toward increasing scalar
    std::array<int,3> cell{0,0,0};      // owning cell (x-fastest linear id smallest on shared faces)
    Material material;
};

struct QueryStats {
    long bricks_visited = 0;            // bricks actually entered by the traversal
    long cells_evaluated = 0;           // cells for which polynomial root solving ran
};

class IsoGrid {
public:
    // origin           : world coordinate of grid vertex (0,0,0)
    // spacing          : positive per-axis vertex spacing
    // counts           : per-axis vertex counts, each >= 2
    // values           : nx*ny*nz doubles, x fastest, then y, then z
    // isovalue         : level set value
    // material         : shading material
    // The scalar data are deep-copied.  Invalid dimensions, data length,
    // spacing or non-finite scalar/origin/isovalue throw std::invalid_argument
    // at construction time.
    IsoGrid(const vec3 &origin,
            const std::array<double,3> &spacing,
            const std::array<int,3> &counts,
            const std::vector<double> &values,
            double isovalue,
            const Material &material);

    // Nearest hit with O + t*D, t in [tmin, tmax).  D must be non-zero and
    // finite and is never normalized by the query.  Returns false when no
    // level-set point exists in the interval.  Tangencies (multiple roots)
    // count as hits.
    bool nearest(const vec3 &O, const vec3 &D,
                 double tmin, double tmax,
                 IsoHit &hit, QueryStats *stats = nullptr) const;

    // Any hit in [tmin, tmax)?  Same traversal, but stops at the first root.
    bool occluded(const vec3 &O, const vec3 &D,
                  double tmin, double tmax,
                  QueryStats *stats = nullptr) const;

    const vec3& origin() const { return origin_; }
    const std::array<double,3>& spacing() const { return spacing_; }
    const std::array<int,3>& counts() const { return counts_; }
    double isovalue() const { return isovalue_; }
    const Material& material() const { return material_; }

    // Number of cells / bricks per axis (bricks hold up to 4 cells per axis).
    static constexpr int BRICK_CELLS = 4;
    std::array<int,3> cell_dims() const { return {counts_[0]-1, counts_[1]-1, counts_[2]-1}; }
    std::array<int,3> brick_dims() const {
        return {(counts_[0]-2)/BRICK_CELLS + 1,
                (counts_[1]-2)/BRICK_CELLS + 1,
                (counts_[2]-2)/BRICK_CELLS + 1};
    }

private:
    vec3 origin_;
    std::array<double,3> spacing_;
    std::array<int,3> counts_;          // vertex counts
    std::vector<double> values_;        // deep copy, x fastest
    double isovalue_;
    Material material_;

    struct Brick {
        std::array<int,3> bmin{0,0,0};  // min vertex indices covered (inclusive)
        std::array<int,3> bmax{0,0,0};  // max vertex indices covered (inclusive)
        double vmin = 0, vmax = 0;      // scalar range over those grid points
        bool active = false;            // isovalue within [vmin, vmax]
    };
    std::vector<Brick> bricks_;         // brick x fastest, then y, then z

    const Brick& brick_at(int bx, int by, int bz) const {
        const std::array<int,3> bd = brick_dims();
        return bricks_[bx + bd[0]*(by + bd[1]*bz)];
    }

    // Shared traversal: brick event walk with cell DDA inside active bricks.
    // Stops at the first root (visitation is strictly t-ordered).  When hit
    // is non-null it is filled (nearest query), otherwise only existence is
    // reported (occlusion query).
    bool traverse(const vec3 &O, const vec3 &D,
                  double tmin, double tmax,
                  QueryStats &stats, IsoHit *hit) const;

    // Analytic cubic root solving for one cell; roots are appended in t.
    void cell_roots(const int cell[3],
                    double u0, double v0, double w0,
                    double du, double dv, double dw,
                    double tlo, double thi,
                    std::vector<double> &roots,
                    QueryStats &stats) const;

    // Gradient of the trilinear field in world coordinates at local cell
    // coordinates (u,v,w) in [0,1].
    vec3 cell_gradient(const int cell[3], double u, double v, double w) const;

    int vertex_index(int i, int j, int k) const {
        return i + counts_[0]*(j + counts_[1]*k);
    }
};

#endif // ISOSURFACE_H
