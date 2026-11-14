#pragma once
#include "task_support/legacy.hpp" // reuse smallpt Vec / Ray / Sphere / ray-sphere intersect
#include <array>
#include <cstdint>
#include <random>
#include <vector>

// CPU direct-lighting entry with analytic visibility gradients.
//
// Color at a fixed receiver point x with unit normal n and Lambertian albedo
// rho, lit by a rectangular area light with constant emission Le:
//
//   L = (rho/pi) * integral_A Le * cos_x * cos_y / r^2 * V(x, y) dA(y)
//
// estimated by uniform area sampling.  V is the finite-segment visibility
// against every sphere in the scene (the sphere carrying x is included; its
// near root is handled by the positive-root / distance tolerance of the
// smallpt Sphere::intersect routine instead of being skipped wholesale).
//
// Gradient w.r.t. the center c_k of a designated occluder sphere.  With
// d = c_k - x, D = |d|, tangent length L = sqrt(D^2 - R^2), the silhouette
// as seen from x is a small circle on the unit direction sphere
// (sin(alpha) = R/D).  Moving the sphere with velocity q gives the signed
// normal velocity of that circle on the unit sphere (A = dhat.q,
// B = circle.q, the circle vector being the unit vector perpendicular to dhat):
//
//   v_n(q) = (L B - R A) / (D L) = -(n_s . q) / L
//
// where n_s is the outward sphere normal at the tangent point, and the
// spherical arc measure is ds = (R/D) dphi.  The transport theorem on the
// direction sphere gives
//
//   dL/dc_k = -(rho/pi) Le * integral_active (n . w) v_n(e_i) ds
//
// integrated by uniform azimuth sampling of the true tangent cone.  Only arcs
// that (1) hit the rectangle interior through its front face, (2) have the
// tangent point before the light, (3) face the receiver (n . w > 0) and
// (4) are not covered by another sphere contribute.  The gradient never mixes
// color samples in: rho, the light and x are held fixed, only shadow
// boundaries move.  Coincident / co-arc silhouettes are not supported;
// generic transverse overlap is.

namespace vg {

struct RectLight {
  Vec center;      // rectangle center
  Vec axis1;       // unit vector
  Vec axis2;       // unit vector, orthogonal to axis1
  double half1;    // positive half length along axis1
  double half2;    // positive half length along axis2
  Vec normal;      // unit normal, orthogonal to both axes, facing the receiver
  Vec emission;    // constant, finite, non-negative linear RGB radiance
};

struct Camera {
  Vec origin;
  Vec forward;     // unit viewing direction
  Vec viewUp;      // any non-parallel up hint; an orthonormal frame is built
  double fovY;     // vertical field of view in degrees, in (0, 180)
};

// RGB gradient of one query: [axis] -> d(color)/d(center component), each a
// linear RGB Vec.
using RGBGradient = std::array<Vec, 3>;

struct QueryResult {
  Vec color = Vec();                       // linear RGB direct lighting
  std::vector<RGBGradient> gradient;       // one entry per designated sphere
  std::uint64_t boundarySamples = 0;       // contour candidates generated
  std::uint64_t arcsKept = 0;              // contour candidates surviving all gates
  std::uint64_t segmentQueries = 0;        // finite sphere shadow-segment tests issued
};

struct Image {
  int width = 0;
  int height = 0;
  std::vector<Vec> color;                  // width*height linear RGB pixels
  std::vector<std::vector<RGBGradient>> gradient; // [occluder][pixel]
  std::uint64_t boundarySamples = 0;       // sums over every pixel query
  std::uint64_t arcsKept = 0;
  std::uint64_t segmentQueries = 0;
};

class VisibilityEngine {
public:
  // Takes the scene by value.  Throws std::invalid_argument on illegal
  // geometry (non-unit/non-orthogonal light frame, non-positive half lengths,
  // non-positive sphere radii, a sphere crossing the light plane, bad
  // occluder indices, non-finite data, ...).
  VisibilityEngine(std::vector<Sphere> spheres,
                   RectLight light,
                   std::vector<int> occluderSphereIndices,
                   std::uint64_t seed);

  // Fixed receiver point query.  The point must be strictly outside every
  // designated occluder.  Both sample counts must be positive; throws
  // std::invalid_argument otherwise.  Exactly boundarySamples contour
  // candidates are generated per designated sphere.
  QueryResult query(const Vec &point, const Vec &normal, const Vec &albedo,
                    int areaSamples, int boundarySamples);

  // Casts real camera rays and produces a small linear RGB / gradient image.
  // Primary hits on the light front face return its emission; light back
  // faces and misses return zero.  A primary hit on a designated occluder is
  // black with zero gradient.  Every other sphere is shaded as a diffuse
  // receiver with its face-forward normal and own color.  First-hit
  // position, normal, material and primary visibility are fixed for the
  // gradient: only the designated occluders move.
  Image render(const Camera &camera, int width, int height,
               int areaSamples, int boundarySamples);

  // Cumulative counters for the life of this instance, so callers can confirm
  // the three axes are produced together without per-axis re-rendering.
  std::uint64_t cumulativeBoundarySamples() const { return totalBoundary_; }
  std::uint64_t cumulativeArcsKept() const { return totalArcs_; }
  std::uint64_t cumulativeSegmentQueries() const { return totalSegments_; }

  const std::vector<Sphere> &sceneSpheres() const { return spheres_; }
  const RectLight &sceneLight() const { return light_; }
  const std::vector<int> &occluderIndices() const { return occluders_; }

private:
  struct DirectSample {
    Vec color;
    std::vector<RGBGradient> gradient;
    std::uint64_t boundarySamples = 0;
    std::uint64_t arcsKept = 0;
    std::uint64_t segmentQueries = 0;
  };

  void validate() const;
  DirectSample computeDirect(const Vec &point, const Vec &normal,
                             const Vec &albedo,
                             int areaSamples, int boundarySamples);
  bool frontLightHit(const Vec &origin, const Vec &dir,
                     double &distance) const;
  bool segmentBlocked(const Vec &origin, const Vec &dir, double tMax,
                      int excludedSphere);
  double uniform01();

  std::vector<Sphere> spheres_;
  RectLight light_;
  std::vector<int> occluders_;
  std::mt19937_64 rng_;
  std::uniform_real_distribution<double> unit_;
  std::uint64_t totalBoundary_ = 0;
  std::uint64_t totalArcs_ = 0;
  std::uint64_t totalSegments_ = 0;
};

} // namespace vg
