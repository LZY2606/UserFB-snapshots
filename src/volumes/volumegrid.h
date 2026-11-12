
/*
    pbrt source code Copyright(c) 1998-2012 Matt Pharr and Greg Humphreys.

    This file is part of pbrt.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are
    met:

    - Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.

    - Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
    IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
    TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
    PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
    HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 */

#if defined(_MSC_VER)
#pragma once
#endif

#ifndef PBRT_VOLUMES_VOLUMEGRID_H
#define PBRT_VOLUMES_VOLUMEGRID_H

// volumes/volumegrid.h*
#include "volume.h"
#include <stdint.h>
#include <stdexcept>

// Grid Tracking Interface ----------------------------------------------------
// Diagnostic counters for the stochastic grid queries below.  A fresh
// GridTrackingStats is all zeros; if one is passed to SampleDistance() or
// RatioTransmittance() it is reset to zero at the start of the call and then
// filled in as the ray is tracked through the grid:
//   brick_visits:        number of majorant bricks the traversal entered
//                        (including zero-majorant bricks that were skipped
//                        without any density evaluation or RNG draw).
//   density_evaluations: number of times the trilinear density interpolant
//                        was actually evaluated.
//   null_events:         virtual collisions that were rejected while sampling
//                        a free path with SampleDistance().  RatioTransmittance()
//                        performs no accept/reject test, so its candidate
//                        events are *not* counted here (it always leaves
//                        null_events == 0).
struct GridTrackingStats {
    uint64_t brick_visits;
    uint64_t density_evaluations;
    uint64_t null_events;
    GridTrackingStats()
        : brick_visits(0), density_evaluations(0), null_events(0) { }
};

// Result of VolumeGridDensity::SampleDistance().  If a real scattering event
// was sampled, hit == true, t is the parametric distance along the *caller's*
// ray (the event is at ray(t) = O + t D, not a world-space distance) and
// position is the world-space event point.  On escape, hit == false and
// t == ray.maxt; position is then unspecified.
struct GridDistanceSample {
    bool hit;
    float t;
    Point position;
    GridDistanceSample() : hit(false), t(0.f), position() { }
};

// VolumeGridDensity Declarations
class VolumeGridDensity : public DensityRegion {
public:
    // VolumeGridDensity Public Methods
    VolumeGridDensity(const Spectrum &sa, const Spectrum &ss, float gg,
            const Spectrum &emit, const BBox &e, const Transform &v2w,
            int x, int y, int z, const float *d)
        : DensityRegion(sa, ss, gg, emit, CheckVolumeTransform(v2w)),
          nx(x), ny(y), nz(z), extent(e),
          nbx((x+4)/4), nby((y+4)/4), nbz((z+4)/4) {
        if (nx <= 0 || ny <= 0 || nz <= 0)
            throw std::invalid_argument(
                "VolumeGridDensity: grid dimensions must be positive");
        Vector diag = e.pMax - e.pMin;
        if (!(diag.x > 0.f) || !(diag.y > 0.f) || !(diag.z > 0.f))
            throw std::invalid_argument(
                "VolumeGridDensity: extent side lengths must be positive");
        CheckDensityValues(d, nx*ny*nz);
        CheckExtinctionCoefficients(sa, ss);
        density = new float[nx*ny*nz];
        memcpy(density, d, nx*ny*nz*sizeof(float));
        BuildBrickMajorants();
    }
    ~VolumeGridDensity() { delete[] density; delete[] brickMajorant; }
    BBox WorldBound() const { return Inverse(WorldToVolume)(extent); }
    bool IntersectP(const Ray &r, float *t0, float *t1) const {
        Ray ray = WorldToVolume(r);
        return extent.IntersectP(ray, t0, t1);
    }
    float Density(const Point &Pobj) const;
    float D(int x, int y, int z) const {
        x = Clamp(x, 0, nx-1);
        y = Clamp(y, 0, ny-1);
        z = Clamp(z, 0, nz-1);
        return density[z*nx*ny + y*nx + x];
    }

    // Stochastic grid queries -----------------------------------------------
    // Both queries track a ray stochastically through the density grid using
    // per-brick majorants of the trilinear interpolant (delta tracking for
    // distances, ratio tracking for transmittance).  They share a read-only
    // brick-majorant index that is built once at construction; neither query
    // allocates memory or mutates the grid, so concurrent calls with
    // caller-provided RNGs are safe.  The ray is first clipped to the
    // intersection of its own [mint, maxt] interval with the grid bound, and
    // all reported t values are parameters of the caller's ray O + t D.
    //
    // SampleDistance(): samples the first real collision of the
    // non-homogeneous Poisson process with rate extinction * density along
    // the ray, where the exponential free path is measured in *world-space*
    // length (any invertible affine VolumeToWorld transform, including
    // translation, non-uniform scale and shear, is accounted for).  Returns
    // hit == false with t == ray.maxt if the ray escapes the medium.
    // extinction must be finite and non-negative; extinction == 0 (vacuum)
    // yields an immediate escape without consuming any RNG values.
    //
    // RatioTransmittance(): returns a single-sample unbiased estimate of the
    // transmittance exp(-tau) along the clipped ray segment, computed with
    // ratio tracking using this region's RGB extinction sig_a + sig_s.  The
    // estimate is non-negative in every channel.  A zero extinction
    // coefficient, a non-intersecting ray and a zero-length segment all
    // return an all-ones Spectrum without consuming RNG values.
    //
    // Both methods throw std::invalid_argument for malformed input rays
    // (non-finite origin/direction, zero direction, mint/maxt out of order;
    // maxt may be +INFINITY) or non-finite/negative extinction.
    GridDistanceSample SampleDistance(const Ray &ray, float extinction,
        RNG &rng, GridTrackingStats *stats = NULL) const;
    Spectrum RatioTransmittance(const Ray &ray, RNG &rng,
        GridTrackingStats *stats = NULL) const;
private:
    // VolumeGridDensity Private Methods
    static const Transform &CheckVolumeTransform(const Transform &v2w);
    static void CheckDensityValues(const float *d, int count);
    static void CheckExtinctionCoefficients(const Spectrum &sa,
        const Spectrum &ss);
    void BuildBrickMajorants();
    float BrickMajorant(int bx, int by, int bz) const {
        return brickMajorant[(bz*nby + by)*nbx + bx];
    }

    // VolumeGridDensity Private Data
    float *density;
    const int nx, ny, nz;
    const BBox extent;
    // Per-brick upper bounds of the trilinear density interpolant.  Bricks
    // cover at most 4 interpolation intervals (gaps between adjacent density
    // centers) per axis, plus the outermost half-cells; each brick's bound
    // also includes neighbor samples outside the brick that take part in an
    // interpolation inside it.
    const int nbx, nby, nbz;
    float *brickMajorant;
    friend struct GridBrickCursor;
};


VolumeGridDensity *CreateGridVolumeRegion(const Transform &volume2world,
        const ParamSet &params);

#endif // PBRT_VOLUMES_VOLUMEGRID_H
