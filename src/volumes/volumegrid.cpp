
/*
    pbrt source code Copyright(c) 1998-2010 Matt Pharr and Greg Humphreys.

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


// volumes/volumegrid.cpp*
#include "stdafx.h"
#include "volumes/volumegrid.h"
#include "paramset.h"

// VolumeGridDensity Method Definitions
float VolumeGridDensity::Density(const Point &Pobj) const {
    if (!extent.Inside(Pobj)) return 0;
    // Compute voxel coordinates and offsets for _Pobj_
    Vector vox = extent.Offset(Pobj);
    vox.x = vox.x * nx - .5f;
    vox.y = vox.y * ny - .5f;
    vox.z = vox.z * nz - .5f;
    int vx = Floor2Int(vox.x), vy = Floor2Int(vox.y), vz = Floor2Int(vox.z);
    float dx = vox.x - vx, dy = vox.y - vy, dz = vox.z - vz;

    // Trilinearly interpolate density values to compute local density
    float d00 = Lerp(dx, D(vx, vy, vz),     D(vx+1, vy, vz));
    float d10 = Lerp(dx, D(vx, vy+1, vz),   D(vx+1, vy+1, vz));
    float d01 = Lerp(dx, D(vx, vy, vz+1),   D(vx+1, vy, vz+1));
    float d11 = Lerp(dx, D(vx, vy+1, vz+1), D(vx+1, vy+1, vz+1));
    float d0 = Lerp(dy, d00, d10);
    float d1 = Lerp(dy, d01, d11);
    return Lerp(dz, d0, d1);
}


// Grid Tracking: Validation --------------------------------------------------
const Transform &VolumeGridDensity::CheckVolumeTransform(const Transform &v2w) {
    const Matrix4x4 &m = v2w.GetMatrix();
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            if (!isfinite(m.m[i][j]))
                throw std::invalid_argument(
                    "VolumeGridDensity: non-finite VolumeToWorld transform");
    float det =
        m.m[0][0] * (m.m[1][1]*m.m[2][2] - m.m[1][2]*m.m[2][1]) -
        m.m[0][1] * (m.m[1][0]*m.m[2][2] - m.m[1][2]*m.m[2][0]) +
        m.m[0][2] * (m.m[1][0]*m.m[2][1] - m.m[1][1]*m.m[2][0]);
    if (!isfinite(det) || det == 0.f)
        throw std::invalid_argument(
            "VolumeGridDensity: VolumeToWorld transform is not invertible");
    return v2w;
}


void VolumeGridDensity::CheckDensityValues(const float *d, int count) {
    if (d == NULL)
        throw std::invalid_argument("VolumeGridDensity: NULL density array");
    for (int i = 0; i < count; ++i)
        if (!isfinite(d[i]) || d[i] < 0.f)
            throw std::invalid_argument(
                "VolumeGridDensity: density values must be non-negative and finite");
}


void VolumeGridDensity::CheckExtinctionCoefficients(const Spectrum &sa,
        const Spectrum &ss) {
    float a[3], s[3];
    sa.ToRGB(a);
    ss.ToRGB(s);
    for (int c = 0; c < 3; ++c)
        if (!isfinite(a[c]) || a[c] < 0.f || !isfinite(s[c]) || s[c] < 0.f)
            throw std::invalid_argument(
                "VolumeGridDensity: extinction coefficients must be non-negative and finite");
}


// Grid Tracking: Brick Majorants ---------------------------------------------
// The interpolation domain of one axis with n density centers is the voxel
// range [-0.5, n-0.5], split into n+1 slabs: the two outer half-cells
// [-0.5,0] and [n-1,n-0.5] plus the n-1 gaps between adjacent centers.  Each
// brick covers at most 4 consecutive slabs per axis; slab s interpolates
// between centers s-1 and s, so a brick covering slabs [s0,s1] must bound
// centers [s0-1, s1] (clamped to the grid), including centers that lie
// outside the brick but take part in an interpolation inside it.  Because
// trilinear interpolation is a convex combination of the 8 corner samples,
// the maximum of those samples bounds the interpolant everywhere in the
// brick.
void VolumeGridDensity::BuildBrickMajorants() {
    brickMajorant = new float[nbx*nby*nbz];
    const int n[3] = { nx, ny, nz };
    const int nb[3] = { nbx, nby, nbz };
    for (int bz = 0; bz < nbz; ++bz) {
        for (int by = 0; by < nby; ++by) {
            for (int bx = 0; bx < nbx; ++bx) {
                const int b[3] = { bx, by, bz };
                int lo[3], hi[3];
                for (int a = 0; a < 3; ++a) {
                    int slabLo = 4*b[a];
                    int slabHi = min(4*b[a]+3, n[a]);
                    lo[a] = max(0, slabLo-1);
                    hi[a] = min(n[a]-1, slabHi);
                }
                float m = 0.f;
                for (int z = lo[2]; z <= hi[2]; ++z)
                    for (int y = lo[1]; y <= hi[1]; ++y)
                        for (int x = lo[0]; x <= hi[0]; ++x)
                            m = max(m, density[z*nx*ny + y*nx + x]);
                brickMajorant[(bz*nby + by)*nbx + bx] = m;
            }
        }
    }
}


// Grid Tracking: Brick Traversal ---------------------------------------------
// DDA cursor that walks a volume-space ray segment [t0, t1] brick by brick.
// The affine VolumeToWorld transform preserves the ray's parameterization,
// so all t values here are also valid parameters of the caller's world-space
// ray; world-space lengths are recovered by multiplying with the world
// direction's length.
struct GridBrickCursor {
    const VolumeGridDensity *grid;
    Ray ro;             // volume-space ray
    float t, t1;        // current position and end of the clipped segment
    int brick[3];
    float nextT[3];     // t where the ray leaves the current brick, per axis
    int step[3];        // +1/-1 per axis (0: ray parallel to axis)

    void Init(const VolumeGridDensity *g, const Ray &volRay,
              float t0, float tEnd) {
        grid = g;
        ro = volRay;
        t = t0;
        t1 = tEnd;
        const int n[3] = { g->nx, g->ny, g->nz };
        const int nb[3] = { g->nbx, g->nby, g->nbz };
        Point p = ro(t0);
        for (int a = 0; a < 3; ++a) {
            float pMin = g->extent.pMin[a], diag = g->extent.pMax[a] - pMin;
            float vox = (p[a]-pMin)/diag * n[a] - .5f;
            vox = Clamp(vox, -.5f, n[a]-.5f);
            brick[a] = Clamp(Floor2Int((vox+1.f)*.25f), 0, nb[a]-1);
            step[a] = ro.d[a] > 0.f ? 1 : (ro.d[a] < 0.f ? -1 : 0);
            nextT[a] = BoundaryT(a, brick[a]);
        }
    }

    // Parametric t at which the ray crosses the boundary of brick _b_ along
    // axis _a_ in the current direction of travel.
    float BoundaryT(int a, int b) const {
        if (step[a] == 0) return INFINITY;
        const int n = (&grid->nx)[a];
        if (b < 0 || b >= (&grid->nbx)[a]) return INFINITY;
        float vb = step[a] > 0 ? min(4.f*b+3.f, n-.5f) : max(4.f*b-1.f, -.5f);
        float pMin = grid->extent.pMin[a];
        float diag = grid->extent.pMax[a] - pMin;
        float obj = pMin + (vb+.5f)/n * diag;
        return (obj - ro.o[a]) / ro.d[a];
    }

    // Advances to the next brick on the segment.  Returns false when the
    // segment is exhausted; otherwise returns the brick coordinates and the
    // parametric sub-segment [s0, s1] the ray spends inside it.  Bricks are
    // reported exactly once and consecutive sub-segments tile [t0, t1]
    // without overlap, so boundaries are never processed twice.
    bool Next(int *bx, int *by, int *bz, float *s0, float *s1) {
        if (t >= t1) return false;
        *bx = brick[0]; *by = brick[1]; *bz = brick[2];
        *s0 = t;
        float tNext = min(t1, min(nextT[0], min(nextT[1], nextT[2])));
        if (tNext < t) tNext = t;   // float roundoff at brick boundaries
        *s1 = tNext;
        t = tNext;
        if (t < t1) {
            // Step every axis whose boundary sits at t (handles edge/corner
            // crossings without emitting zero-length segments).
            for (int a = 0; a < 3; ++a) {
                if (step[a] != 0 && nextT[a] <= t) {
                    brick[a] += step[a];
                    nextT[a] = BoundaryT(a, brick[a]);
                }
            }
        }
        return true;
    }
};


// Grid Tracking: Queries -----------------------------------------------------
static void CheckTrackingRay(const Ray &ray, const char *caller) {
    if (!isfinite(ray.o.x) || !isfinite(ray.o.y) || !isfinite(ray.o.z) ||
        !isfinite(ray.d.x) || !isfinite(ray.d.y) || !isfinite(ray.d.z))
        throw std::invalid_argument(caller);
    if (ray.d.LengthSquared() == 0.f)
        throw std::invalid_argument(caller);
    if (isnan(ray.mint) || isnan(ray.maxt) ||
        ray.mint < 0.f || ray.mint > ray.maxt)
        throw std::invalid_argument(caller);
}


// Advances a free-path proposal inside one brick; guards against the
// proposal collapsing onto the current point in floating point.
static inline float AdvanceProposal(float t, double dt, float segEnd) {
    double tNew = (double)t + dt;
    if (tNew >= segEnd) return segEnd;
    float tf = (float)tNew;
    if (tf <= t) tf = nextafterf(t, INFINITY);
    return tf;
}


GridDistanceSample VolumeGridDensity::SampleDistance(const Ray &ray,
        float extinction, RNG &rng, GridTrackingStats *stats) const {
    if (stats) *stats = GridTrackingStats();
    CheckTrackingRay(ray, "VolumeGridDensity::SampleDistance: malformed ray");
    if (!isfinite(extinction) || extinction < 0.f)
        throw std::invalid_argument(
            "VolumeGridDensity::SampleDistance: extinction must be non-negative and finite");
    GridDistanceSample result;
    result.hit = false;
    result.t = ray.maxt;
    if (extinction == 0.f) return result;   // vacuum: escape, no RNG
    Ray ro = WorldToVolume(ray);
    float t0, t1;
    if (!extent.IntersectP(ro, &t0, &t1) || !(t1 > t0)) return result;
    const float worldPerT = ray.d.Length();   // world length per unit t
    GridBrickCursor cursor;
    cursor.Init(this, ro, t0, t1);
    double tau = 0.;      // remaining majorant optical depth to the proposal
    bool haveTau = false; // drawn lazily: all-empty paths consume no RNG
    int bx, by, bz;
    float s0, s1;
    while (cursor.Next(&bx, &by, &bz, &s0, &s1)) {
        if (stats) ++stats->brick_visits;
        float M = BrickMajorant(bx, by, bz);
        if (M == 0.f || !(s1 > s0)) continue;   // skip empty bricks silently
        const double lambda = (double)extinction * M * worldPerT;
        if (!haveTau) {
            tau = -log((double)(1.f - rng.RandomFloat()));
            haveTau = true;
        }
        float t = s0;
        for (;;) {
            double dt = tau / lambda;
            float tp = AdvanceProposal(t, dt, s1);
            if (tp >= s1) {   // proposal falls past the brick: consume depth
                tau -= lambda * (double)(s1 - t);
                break;
            }
            t = tp;
            float d = Density(ro(t));
            if (stats) ++stats->density_evaluations;
            float accept = d / M;
            if (accept > 1.f) accept = 1.f;   // float safety; M bounds d
            if (rng.RandomFloat() < accept) {
                result.hit = true;
                result.t = t;
                result.position = ray(t);
                return result;
            }
            if (stats) ++stats->null_events;
            tau = -log((double)(1.f - rng.RandomFloat()));
        }
    }
    return result;
}


Spectrum VolumeGridDensity::RatioTransmittance(const Ray &ray, RNG &rng,
        GridTrackingStats *stats) const {
    if (stats) *stats = GridTrackingStats();
    CheckTrackingRay(ray, "VolumeGridDensity::RatioTransmittance: malformed ray");
    float sigT[3];
    Spectrum(sig_a + sig_s).ToRGB(sigT);
    float maxSig = max(sigT[0], max(sigT[1], sigT[2]));
    float w[3] = { 1.f, 1.f, 1.f };
    if (maxSig == 0.f) return Spectrum::FromRGB(w);   // vacuum: no RNG
    Ray ro = WorldToVolume(ray);
    float t0, t1;
    if (!extent.IntersectP(ro, &t0, &t1) || !(t1 > t0))
        return Spectrum::FromRGB(w);
    const float worldPerT = ray.d.Length();
    GridBrickCursor cursor;
    cursor.Init(this, ro, t0, t1);
    double tau = 0.;
    bool haveTau = false;
    int bx, by, bz;
    float s0, s1;
    while (cursor.Next(&bx, &by, &bz, &s0, &s1)) {
        if (stats) ++stats->brick_visits;
        float M = BrickMajorant(bx, by, bz);
        if (M == 0.f || !(s1 > s0)) continue;
        const double lambda = (double)maxSig * M * worldPerT;
        if (!haveTau) {
            tau = -log((double)(1.f - rng.RandomFloat()));
            haveTau = true;
        }
        float t = s0;
        for (;;) {
            double dt = tau / lambda;
            float tp = AdvanceProposal(t, dt, s1);
            if (tp >= s1) {
                tau -= lambda * (double)(s1 - t);
                break;
            }
            t = tp;
            float d = Density(ro(t));
            if (stats) ++stats->density_evaluations;
            float ratio = d / M;
            if (ratio > 1.f) ratio = 1.f;
            // Ratio tracking: multiply each channel by 1 - mu_c/lambda.
            // Every factor stays in [0,1], so the estimate is non-negative.
            for (int c = 0; c < 3; ++c) {
                float f = 1.f - (sigT[c]/maxSig) * ratio;
                if (f < 0.f) f = 0.f;
                w[c] *= f;
            }
            tau = -log((double)(1.f - rng.RandomFloat()));
        }
    }
    return Spectrum::FromRGB(w);
}


VolumeGridDensity *CreateGridVolumeRegion(const Transform &volume2world,
        const ParamSet &params) {
    // Initialize common volume region parameters
    Spectrum sigma_a = params.FindOneSpectrum("sigma_a", 0.);
    Spectrum sigma_s = params.FindOneSpectrum("sigma_s", 0.);
    float g = params.FindOneFloat("g", 0.);
    Spectrum Le = params.FindOneSpectrum("Le", 0.);
    Point p0 = params.FindOnePoint("p0", Point(0,0,0));
    Point p1 = params.FindOnePoint("p1", Point(1,1,1));
    int nitems;
    const float *data = params.FindFloat("density", &nitems);
    if (!data) {
        Error("No \"density\" values provided for volume grid?");
        return NULL;
    }
    int nx = params.FindOneInt("nx", 1);
    int ny = params.FindOneInt("ny", 1);
    int nz = params.FindOneInt("nz", 1);
    if (nitems != nx*ny*nz) {
        Error("VolumeGridDensity has %d density values but nx*ny*nz = %d",
            nitems, nx*ny*nz);
        return NULL;
    }
    return new VolumeGridDensity(sigma_a, sigma_s, g, Le, BBox(p0, p1),
        volume2world, nx, ny, nz, data);
}


