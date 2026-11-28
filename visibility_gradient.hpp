// visibility_gradient.hpp
//
// Direct-lighting radiometry and shadow-boundary derivatives for smallpt
// scenes.  The entry point below estimates linear RGB direct lighting on
// diffuse receivers and the derivative of that RGB with respect to the
// center of each specified occluding sphere, using a motion-contour boundary
// integral on the unit direction sphere.  No finite differences, no scene
// re-renders and no smoothed visibility are used anywhere in this file.
//
// The implementation reuses the smallpt Vec / Ray / Sphere types and the
// ray-sphere intersection conventions from task_support/legacy.hpp.
#pragma once

#include "task_support/legacy.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace vg {

// Rectangle area light.  The rectangle spans
//   center + axis1 * u + axis2 * v,  |u| <= half1, |v| <= half2.
// axis1 / axis2 are orthogonal unit vectors, half1 / half2 are strictly
// positive, normal is a unit vector orthogonal to both axes and points
// toward the receiver side of the light.  Emission is a constant,
// component-wise non-negative linear RGB radiance.
struct RectLight {
  Vec center;
  Vec axis1;
  Vec axis2;
  Vec normal;
  double half1 = 0.0;
  double half2 = 0.0;
  Vec emission;
};

// Simple pinhole camera.  Pixel (i, j) casts
//   normalize(forward + right * ((i + .5) / width  - .5)
//                    + up    * ((j + .5) / height - .5)).
// forward / right / up only need to be finite; forward must be non-zero.
struct Camera {
  Vec origin;
  Vec forward;
  Vec right;
  Vec up;
};

// Counters published so callers can audit the integration.  In particular
// boundarySamples equals boundarySamplesPerSphere * (#specified spheres) for
// every fixed-point query: the three Cartesian derivatives are all derived
// from the same set of contour candidates, there is no per-axis re-render.
struct PointStats {
  unsigned long long boundarySamples = 0; // contour candidates generated
  unsigned long long retainedArcs = 0;    // candidates surviving all gates
  unsigned long long segmentQueries = 0;  // finite ray-segment visibility tests
};

struct PointResult {
  Vec color = Vec();                          // linear RGB estimate
  std::vector<std::array<Vec, 3>> gradients;  // [specifiedSphere][x/y/z] -> RGB
  PointStats stats;
};

struct Image {
  int width = 0;
  int height = 0;
  std::vector<Vec> color;                    // width * height linear RGB
  // width * height * K entries (K = number of specified spheres), indexed as
  // [pixel * K + specifiedSphere][axis], axis 0/1/2 -> x/y/z.
  std::vector<std::array<Vec, 3>> gradient;
  PointStats stats;                          // totals over every pixel
};

// Holds its own copy of the scene (spheres + light) and its own random
// state.  Two instances constructed with the same seed walk the same random
// sequence; instances never share mutable state.
//
// Only diffuse (DIFF) receiver surfaces and opaque sphere occluders are
// supported; there is no indirect-light recursion.  Occluding spheres may
// overlap each other, but must not intersect the receiving surfaces, and
// every specified sphere's center must be farther from the light plane than
// its radius.  Illegal geometry, illegal sample counts and non-finite
// parameters make the constructor (or query / render) throw
// std::invalid_argument.
class VisibilityGradient {
 public:
  VisibilityGradient(std::vector<Sphere> spheres,
                     const RectLight& light,
                     std::vector<int> specifiedSphereIndices,
                     std::uint64_t seed,
                     int areaSamples,
                     int boundarySamplesPerSphere);

  // Fixed receiver point query.  point must be strictly outside every
  // specified sphere, normal must be unit length, albedo must be
  // component-wise non-negative.  Returns the direct-light RGB and the RGB
  // gradient for each specified sphere along the three Cartesian axes.
  PointResult query(const Vec& point,
                    const Vec& normal,
                    const Vec& albedo) const;

  // Casts width * height primary rays.  Primary hits on a specified sphere
  // produce black with zero gradients; primary light hits return the
  // emission (back face / miss return zero); other sphere hits are shaded
  // with their facing normal and surface color.  First-hit position,
  // normal, material and primary visibility stay fixed for the derivative
  // pass: only the specified occluders move in the boundary integral.
  Image render(const Camera& camera, int width, int height) const;

  // Read-only access to the instance scene.
  const std::vector<Sphere>& spheres() const { return spheres_; }
  const RectLight& light() const { return light_; }
  const std::vector<int>& specifiedIndices() const { return specified_; }
  int areaSamples() const { return areaSamples_; }
  int boundarySamples() const { return boundarySamples_; }
  std::uint64_t seed() const { return seed_; }

 private:
  std::vector<Sphere> spheres_;
  RectLight light_;
  std::vector<int> specified_;
  std::uint64_t seed_;
  int areaSamples_;
  int boundarySamples_;
  mutable std::uint64_t rngState_ = 0;  // instance-owned SplitMix64 stream
};

}  // namespace vg
