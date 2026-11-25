#ifndef ISOSURFACE_H
#define ISOSURFACE_H

#include <vector>
#include "renderer.h"

// IsoGrid renders the isosurface F = isovalue of a trilinearly interpolated
// scalar field sampled on a regular vertex grid, without meshing and without
// fixed-step ray marching.
//
// Grid layout:
//   vertex (i,j,k), 0 <= i < nx, 0 <= j < ny, 0 <= k < nz, is stored at
//   index ((k*ny) + j)*nx + i, i.e. x is the fastest varying axis.
//   world position of vertex (i,j,k) is origin + (i*sx, j*sy, k*sz).
// The field exists only inside the grid box; the box is never capped.
//
// The object is read-only after construction and holds no mutable per-query
// cursor, so concurrent queries share only immutable data.
struct IsoGrid {
    // Throws std::invalid_argument when the sizes, spacing, data length or
    // any finite-ness requirement is violated. The scalar array is deep
    // copied.
    IsoGrid(const vec3& origin,
            const vec3& spacing,
            int nx, int ny, int nz,
            const std::vector<double>& scalar,
            double isovalue,
            const Material& material);

    // Nearest intersection of ray O + t*D with the isosurface over
    // t in [tmin, tmax). D must be non-zero and finite and is NOT normalized.
    // Returns true and fills hit when an intersection exists.
    bool intersect(const vec3& O, const vec3& D,
                   float tmin, float tmax,
                   IsoHit& hit, QueryStats* stats = nullptr) const;

    // True when any isosurface point lies strictly before tmax along the
    // ray, i.e. over [tmin, tmax). Used for finite shadow rays.
    bool occluded(const vec3& O, const vec3& D,
                  float tmin, float tmax,
                  QueryStats* stats = nullptr) const;

    // Read-only accessors (mainly for tests and the example).
    const vec3& origin()  const { return origin_; }
    const vec3& spacing() const { return spacing_; }
    int nx() const { return nx_; }
    int ny() const { return ny_; }
    int nz() const { return nz_; }
    double isovalue() const { return iso_; }
    const Material& material() const { return material_; }

  private:
    vec3 origin_;
    vec3 spacing_;
    int nx_, ny_, nz_;            // vertex counts, each >= 2
    double iso_;
    Material material_;
    std::vector<double> values_;  // size nx*ny*nz, x fastest

    // Brick acceleration: up to 4 cells per axis per brick.
    int nbx_, nby_, nbz_;
    struct Brick {
        double vmin = 0, vmax = 0; // min/max over the grid points of the brick
        bool   active = false;     // isovalue within [vmin, vmax]
    };
    std::vector<Brick> bricks_;
    int brick_index(int bx, int by, int bz) const {
        return (bz*nby_ + by)*nbx_ + bx;
    }
    const Brick& brick(int bx, int by, int bz) const {
        return bricks_[brick_index(bx, by, bz)];
    }

    double vertex_value(int i, int j, int k) const {
        return values_[(k*ny_ + j)*nx_ + i];
    }

    // Shared ray walk; the traversal is strictly increasing in t, so the
    // first root found is automatically the nearest one.
    bool trace(const vec3& O, const vec3& D,
               float tmin, float tmax,
               IsoHit* hit, QueryStats* stats) const;

    // Solve one trilinear cell; append roots (in cell-local ray time) to
    // out_roots, deduplicated. poly_solved reports whether the corner range
    // admitted a root and polynomial solving actually ran.
    bool solve_cell(int i, int j, int k,
                    const vec3& O, const vec3& D,
                    float t0, float t1,
                    std::vector<float>& out_roots,
                    bool& poly_solved) const;
};

#endif // ISOSURFACE_H
