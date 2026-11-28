// visibility_gradient.cpp
//
// Implementation of the direct-lighting / shadow-boundary entry point.  See
// visibility_gradient.hpp for the contract.
//
// The color at a fixed receiver point x with unit normal n and RGB albedo
// rho is the area integral
//
//   C = (rho/pi) * Int_A Le * (n.w)(-nL.w) / |y-x|^2 * V(x,y) dA(y)
//
// estimated by uniform area sampling of the rectangle.
//
// The derivative w.r.t. the center c of an occluding sphere is a motion
// contour boundary integral on the unit direction sphere.  The tangent cone
// from x to the sphere is the small circle
//
//   g(w) = (c-x).w - sqrt(|c-x|^2 - R^2) = 0,
//
// sampled uniformly in its azimuth phi with spherical arc-length measure
// ds = sin(alpha) dphi.  The contour's inward unit normal (toward the cone
// axis) is m = (b - q w) / R, b = c-x, q = sqrt(|b|^2 - R^2); translating
// the sphere by velocity v gives signed inward normal velocity
//
//   v_n = v . (b/q - w) / R.
//
// Because the color loses the integrand exactly over the blocked cap, the
// shape derivative yields
//
//   dC/dv = (rho/pi) Le * Int_contour (n.w) v_n ds,
//
// keeping only arcs whose forward ray lands inside the rectangle, reaches
// the tangent point before the light, has a positive receiver cosine and is
// not blocked by another sphere.  There is no derivative of any surface
// area density and no derivative of a binary shadow-ray result: only the
// moving silhouette is integrated.
#include "visibility_gradient.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace vg {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDistEps = 1e-4;   // ray distance tolerance (smallpt eps)
constexpr double kCosEps = 1e-7;    // positive-cosine gate
constexpr double kRectEps = 1e-7;   // rectangle boundary tolerance
constexpr double kUnitEps = 1e-9;   // unit-length / orthogonality tolerance

bool finiteVec(const Vec& v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

double dot(const Vec& a, const Vec& b) { return a.dot(b); }

Vec cross(const Vec& a, const Vec& b) {
  return Vec(a.y * b.z - a.z * b.y,
             a.z * b.x - a.x * b.z,
             a.x * b.y - a.y * b.x);
}

double length(const Vec& v) { return std::sqrt(dot(v, v)); }

Vec normalized(const Vec& v) {
  const double l = length(v);
  return Vec(v.x / l, v.y / l, v.z / l);
}

// Both roots of |o + t d - c|^2 = R^2.  Returns false for a miss.  Roots are
// ordered; unlike Sphere::intersect this carries no epsilon logic so the
// caller can apply the positive-root / distance-tolerance rule itself (in
// particular for a ray starting on the receiver sphere).
bool sphereRoots(const Vec& o, const Vec& d, const Vec& c, double r,
                 double& t1, double& t2) {
  const Vec oc = o - c;
  const double b = dot(oc, d);
  const double det = b * b - dot(oc, oc) + r * r;
  if (det < 0.0) return false;
  const double s = std::sqrt(det);
  t1 = -b - s;
  t2 = -b + s;
  return true;
}

// Intersection of a ray with the light plane.  Returns false when the ray is
// parallel to the plane.  The hit may be on either face.
bool rectPlaneHit(const RectLight& L, const Vec& o, const Vec& d, double& t) {
  const double denom = dot(d, L.normal);
  if (std::fabs(denom) < 1e-15) return false;
  t = dot(L.center - o, L.normal) / denom;
  return true;
}

bool rectContains(const RectLight& L, const Vec& p) {
  const Vec r = p - L.center;
  return std::fabs(dot(r, L.axis1)) <= L.half1 + kRectEps &&
         std::fabs(dot(r, L.axis2)) <= L.half2 + kRectEps;
}

// Finite-segment visibility: true if the ray reaches tMax without entering
// any sphere.  Every sphere participates, including the one owning the ray
// origin: roots at (or below) the positive distance tolerance are ignored,
// so a receiver sphere is handled by its roots rather than skipped whole.
// skipIndex (when >= 0) excludes one sphere: a contour ray is tangent to the
// sphere whose silhouette it samples, so that expected grazing contact is
// not treated as occlusion.
bool pointVisible(const std::vector<Sphere>& spheres, const Vec& o,
                  const Vec& d, double tMax, int skipIndex = -1) {
  for (std::size_t i = 0; i < spheres.size(); ++i) {
    if (static_cast<int>(i) == skipIndex) continue;
    const Sphere& s = spheres[i];
    double t1, t2;
    if (!sphereRoots(o, d, s.p, s.rad, t1, t2)) continue;
    if (t1 > kDistEps && t1 < tMax - kDistEps) return false;
    if (t2 > kDistEps && t2 < tMax - kDistEps) return false;
  }
  return true;
}

// SplitMix64: a self-contained, deterministic per-instance random stream
// (C++17 standard library only).  Equal seeds produce identical sequences.
std::uint64_t splitMix64(std::uint64_t& state) {
  std::uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

double nextDouble(std::uint64_t& state) {
  // 53-bit mantissa -> a uniform value in [0, 1).
  return static_cast<double>(splitMix64(state) >> 11) *
         (1.0 / 9007199254740992.0);  // 2^-53
}

}  // namespace

VisibilityGradient::VisibilityGradient(std::vector<Sphere> spheres,
                                       const RectLight& light,
                                       std::vector<int> specifiedSphereIndices,
                                       std::uint64_t seed, int areaSamples,
                                       int boundarySamplesPerSphere)
    : spheres_(std::move(spheres)),
      light_(light),
      specified_(std::move(specifiedSphereIndices)),
      seed_(seed),
      areaSamples_(areaSamples),
      boundarySamples_(boundarySamplesPerSphere) {
  if (areaSamples <= 0 || boundarySamplesPerSphere <= 0)
    throw std::invalid_argument(
        "visibility_gradient: sample counts must be positive");
  if (!finiteVec(light_.center) || !finiteVec(light_.axis1) ||
      !finiteVec(light_.axis2) || !finiteVec(light_.normal) ||
      !finiteVec(light_.emission))
    throw std::invalid_argument("visibility_gradient: non-finite light");
  if (!(std::isfinite(light_.half1) && std::isfinite(light_.half2)) ||
      light_.half1 <= 0.0 || light_.half2 <= 0.0)
    throw std::invalid_argument(
        "visibility_gradient: light half-lengths must be positive");
  if (light_.emission.x < 0.0 || light_.emission.y < 0.0 ||
      light_.emission.z < 0.0)
    throw std::invalid_argument(
        "visibility_gradient: emission must be non-negative");
  if (std::fabs(length(light_.axis1) - 1.0) > kUnitEps ||
      std::fabs(length(light_.axis2) - 1.0) > kUnitEps ||
      std::fabs(length(light_.normal) - 1.0) > kUnitEps)
    throw std::invalid_argument(
        "visibility_gradient: light axes and normal must be unit length");
  if (std::fabs(dot(light_.axis1, light_.axis2)) > kUnitEps ||
      std::fabs(dot(light_.axis1, light_.normal)) > kUnitEps ||
      std::fabs(dot(light_.axis2, light_.normal)) > kUnitEps)
    throw std::invalid_argument(
        "visibility_gradient: light axes and normal must be mutually "
        "orthogonal");

  for (const Sphere& s : spheres_) {
    if (!std::isfinite(s.rad) || s.rad <= 0.0 || !finiteVec(s.p) ||
        !finiteVec(s.e) || !finiteVec(s.c))
      throw std::invalid_argument(
          "visibility_gradient: non-finite or illegal sphere");
  }
  for (int idx : specified_) {
    if (idx < 0 || idx >= static_cast<int>(spheres_.size()))
      throw std::invalid_argument(
          "visibility_gradient: specified sphere index out of range");
    const Sphere& s = spheres_[idx];
    // Strictly greater than the radius: the plane may not cut the sphere.
    const double planeDist = std::fabs(dot(s.p - light_.center, light_.normal));
    if (!(planeDist > s.rad))
      throw std::invalid_argument(
          "visibility_gradient: specified sphere center must be farther from "
          "the light plane than its radius");
  }
  for (std::size_t i = 0; i < specified_.size(); ++i)
    for (std::size_t j = i + 1; j < specified_.size(); ++j)
      if (specified_[i] == specified_[j])
        throw std::invalid_argument(
            "visibility_gradient: specified sphere indices must be unique");

  // Per-instance random stream; equal seeds replay identically.
  rngState_ = seed_;
}

PointResult VisibilityGradient::query(const Vec& point, const Vec& normal,
                                      const Vec& albedo) const {
  if (!finiteVec(point) || !finiteVec(normal) || !finiteVec(albedo))
    throw std::invalid_argument("visibility_gradient: non-finite query input");
  if (std::fabs(length(normal) - 1.0) > kUnitEps)
    throw std::invalid_argument("visibility_gradient: normal must be unit");
  if (albedo.x < 0.0 || albedo.y < 0.0 || albedo.z < 0.0)
    throw std::invalid_argument(
        "visibility_gradient: albedo must be non-negative");
  for (int idx : specified_) {
    const Sphere& s = spheres_[idx];
    if (!(length(point - s.p) > s.rad))
      throw std::invalid_argument(
          "visibility_gradient: query point must be strictly outside every "
          "specified sphere");
  }

  PointResult out;
  out.gradients.assign(specified_.size(),
                       std::array<Vec, 3>{Vec(), Vec(), Vec()});

  // ---- Color: uniform area sampling of the rectangle --------------------
  const double rectArea = 4.0 * light_.half1 * light_.half2;
  Vec colorAccum;
  for (int sample = 0; sample < areaSamples_; ++sample) {
    const double u1 = nextDouble(rngState_);
    const double u2 = nextDouble(rngState_);
    const Vec y = light_.center +
                  light_.axis1 * ((2.0 * u1 - 1.0) * light_.half1) +
                  light_.axis2 * ((2.0 * u2 - 1.0) * light_.half2);
    const Vec toLight = y - point;
    const double dist2 = dot(toLight, toLight);
    if (dist2 <= 0.0) continue;
    const double dist = std::sqrt(dist2);
    const Vec w = toLight * (1.0 / dist);
    const double cosReceiver = dot(normal, w);
    const double cosLight = -dot(light_.normal, w);
    if (cosReceiver <= kCosEps || cosLight <= kCosEps) continue;
    ++out.stats.segmentQueries;
    if (!pointVisible(spheres_, point, w, dist)) continue;
    const double geometry =
        cosReceiver * cosLight / dist2 * (rectArea / areaSamples_);
    colorAccum = colorAccum +
                 light_.emission.mult(albedo) * (geometry / kPi);
  }
  out.color = colorAccum;

  // ---- Gradients: one contour per specified sphere ----------------------
  const Vec rhoOverPi = albedo * (1.0 / kPi);
  for (std::size_t k = 0; k < specified_.size(); ++k) {
    const Sphere& s = spheres_[specified_[k]];
    const Vec b = s.p - point;                      // x -> sphere center
    const double d = length(b);
    const double r = s.rad;
    const double d2 = d * d;
    const double q = std::sqrt(d2 - r * r);         // tangent distance
    const double sinAlpha = r / d;
    const Vec axis = b * (1.0 / d);                 // cone axis
    // Orthonormal frame around the cone axis.
    const Vec tmp = std::fabs(axis.x) > 0.9 ? Vec(0.0, 1.0, 0.0)
                                            : Vec(1.0, 0.0, 0.0);
    const Vec e1 = normalized(cross(axis, tmp));
    const Vec e2 = cross(axis, e1);
    const double cosAlpha = q / d;
    // Integrand weight excluding the per-sample (n.w) v_n terms.
    const Vec base = rhoOverPi.mult(light_.emission) *
                     (sinAlpha * (2.0 * kPi / boundarySamples_));

    for (int sample = 0; sample < boundarySamples_; ++sample) {
      ++out.stats.boundarySamples;
      const double phi = 2.0 * kPi * sample / boundarySamples_;
      const Vec cp = e1 * std::cos(phi) + e2 * std::sin(phi);
      const Vec w = normalized(axis * cosAlpha + cp * sinAlpha);

      // Gate 1: receiver cosine must be positive.
      const double cosReceiver = dot(normal, w);
      if (cosReceiver <= kCosEps) continue;

      // Gate 2: the forward ray must meet the light plane inside the
      // rectangle.
      double tLight;
      if (!rectPlaneHit(light_, point, w, tLight) || tLight <= kDistEps)
        continue;
      const Vec hit = point + w * tLight;
      if (!rectContains(light_, hit)) continue;

      // Gate 3: the tangent point must be reached before the light.
      if (!(q < tLight - kDistEps)) continue;

      // Gate 4: no *other* sphere may cover the boundary arc.  The contour
      // sphere's own grazing contact at the tangent point is expected.
      ++out.stats.segmentQueries;
      if (!pointVisible(spheres_, point, w, tLight, specified_[k])) continue;

      ++out.stats.retainedArcs;
      // Inward normal velocity coefficient for a unit translation v:
      // v_n = v . (b/q - w) / R.
      const Vec velocityDir = (b * (1.0 / q) - w) * (1.0 / r);
      const Vec term = base * cosReceiver;
      std::array<Vec, 3>& grad = out.gradients[k];
      grad[0] = grad[0] + term * dot(velocityDir, Vec(1, 0, 0));
      grad[1] = grad[1] + term * dot(velocityDir, Vec(0, 1, 0));
      grad[2] = grad[2] + term * dot(velocityDir, Vec(0, 0, 1));
    }
  }
  return out;
}

Image VisibilityGradient::render(const Camera& camera, int width,
                                 int height) const {
  if (width <= 0 || height <= 0)
    throw std::invalid_argument(
        "visibility_gradient: image dimensions must be positive");
  if (!finiteVec(camera.origin) || !finiteVec(camera.forward) ||
      !finiteVec(camera.right) || !finiteVec(camera.up) ||
      length(camera.forward) <= kUnitEps)
    throw std::invalid_argument("visibility_gradient: illegal camera");

  const int kCount = static_cast<int>(specified_.size());
  Image image;
  image.width = width;
  image.height = height;
  image.color.assign(static_cast<std::size_t>(width) * height, Vec());
  image.gradient.assign(static_cast<std::size_t>(width) * height * kCount,
                        std::array<Vec, 3>{Vec(), Vec(), Vec()});

  for (int j = 0; j < height; ++j) {
    for (int i = 0; i < width; ++i) {
      const std::size_t pixel =
          static_cast<std::size_t>(j) * width + i;
      Vec dir = camera.forward +
                camera.right * ((i + 0.5) / width - 0.5) +
                camera.up * ((j + 0.5) / height - 0.5);
      const Ray ray(camera.origin, normalized(dir));

      // Nearest sphere vs. the rectangle light.
      double tSphere = 1e20;
      int idSphere = -1;
      for (std::size_t s = 0; s < spheres_.size(); ++s) {
        const double t = spheres_[s].intersect(ray);
        if (t > 0.0 && t < tSphere) {
          tSphere = t;
          idSphere = static_cast<int>(s);
        }
      }
      double tLight;
      const bool planeCloser = rectPlaneHit(light_, ray.o, ray.d, tLight) &&
                               tLight > kDistEps && tLight < tSphere;
      if (planeCloser && rectContains(light_, ray.o + ray.d * tLight)) {
        // Front face emits; back face is black.  A plane hit outside the
        // rectangle does not occlude the sphere behind it.
        if (dot(ray.d, light_.normal) < 0.0)
          image.color[pixel] = light_.emission;
        continue;  // light-hit pixels carry no shadow derivative
      }
      if (idSphere < 0) continue;  // miss -> black

      const bool isSpecified =
          std::find(specified_.begin(), specified_.end(), idSphere) !=
          specified_.end();
      if (isSpecified) continue;  // primary hit on an occluder: black, zero

      const Sphere& obj = spheres_[idSphere];
      const Vec hit = ray.o + ray.d * tSphere;
      const Vec n = normalized(hit - obj.p);
      const Vec nl = dot(n, ray.d) < 0.0 ? n : n * -1.0;

      // First-hit position, normal and material stay fixed for the
      // derivative pass inside query().
      const PointResult pr = query(hit, nl, obj.c);
      image.color[pixel] = pr.color;
      for (int k = 0; k < kCount; ++k)
        image.gradient[pixel * kCount + k] = pr.gradients[k];
      image.stats.boundarySamples += pr.stats.boundarySamples;
      image.stats.retainedArcs += pr.stats.retainedArcs;
      image.stats.segmentQueries += pr.stats.segmentQueries;
    }
  }
  return image;
}

}  // namespace vg
