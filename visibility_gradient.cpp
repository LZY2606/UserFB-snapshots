#include "visibility_gradient.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace vg {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kUnitTol = 1e-9;      // unit-length / orthogonality tolerance
constexpr double kRayEps = 1e-9;       // positive ray parameter tolerance
constexpr double kCoordTol = 1e-9;     // rectangle interior tolerance
constexpr double kCosTol = 1e-10;      // receiver cosine tolerance
constexpr double kGeoTol = 1e-10;      // geometric separation tolerance

bool finiteVec(const Vec &v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

double length(const Vec &v) { return std::sqrt(v.dot(v)); }

Vec normalized(const Vec &v) {
  const double l = length(v);
  return Vec(v.x / l, v.y / l, v.z / l);
}

Vec cross(const Vec &a, const Vec &b) {
  return Vec(a.y * b.z - a.z * b.y,
             a.z * b.x - a.x * b.z,
             a.x * b.y - a.y * b.x);
}

// Distance tolerance for comparing two ray parameters on the same ray.
double paramSlack(double t) { return 1e-7 * std::max(1.0, t); }

} // namespace

VisibilityEngine::VisibilityEngine(std::vector<Sphere> spheres,
                                   RectLight light,
                                   std::vector<int> occluderSphereIndices,
                                   std::uint64_t seed)
    : spheres_(std::move(spheres)), light_(light),
      occluders_(std::move(occluderSphereIndices)), rng_(seed), unit_(0.0, 1.0) {
  validate();
}

void VisibilityEngine::validate() const {
  if (!finiteVec(light_.center) || !finiteVec(light_.axis1) ||
      !finiteVec(light_.axis2) || !finiteVec(light_.normal) ||
      !finiteVec(light_.emission))
    throw std::invalid_argument("light contains non-finite data");
  if (std::fabs(length(light_.axis1) - 1.0) > kUnitTol ||
      std::fabs(length(light_.axis2) - 1.0) > kUnitTol ||
      std::fabs(length(light_.normal) - 1.0) > kUnitTol)
    throw std::invalid_argument("light axes and normal must be unit vectors");
  if (std::fabs(light_.axis1.dot(light_.axis2)) > kUnitTol ||
      std::fabs(light_.axis1.dot(light_.normal)) > kUnitTol ||
      std::fabs(light_.axis2.dot(light_.normal)) > kUnitTol)
    throw std::invalid_argument("light frame must be orthonormal");
  if (!(light_.half1 > 0.0) || !(light_.half2 > 0.0))
    throw std::invalid_argument("light half lengths must be positive");
  if (light_.emission.x < 0.0 || light_.emission.y < 0.0 ||
      light_.emission.z < 0.0)
    throw std::invalid_argument("light emission must be non-negative");

  for (const Sphere &s : spheres_) {
    if (!(s.rad > 0.0) || !finiteVec(s.p) || !finiteVec(s.c) ||
        !finiteVec(s.e))
      throw std::invalid_argument("sphere has illegal radius or non-finite data");
    if (s.refl != DIFF)
      throw std::invalid_argument(
          "only diffuse receiver surfaces and opaque sphere occluders are supported");
    if (s.c.x < 0.0 || s.c.y < 0.0 || s.c.z < 0.0)
      throw std::invalid_argument("sphere albedo must be non-negative");
    // Sphere center must be strictly farther than its radius from the light
    // plane: the occluder may not cross the emitter plane.
    const double planeDistance =
        std::fabs((s.p - light_.center).dot(light_.normal));
    if (!(planeDistance > s.rad + kGeoTol * std::max(1.0, s.rad)))
      throw std::invalid_argument(
          "sphere crosses or touches the light plane (distance must exceed radius)");
  }

  std::vector<int> sorted = occluders_;
  std::sort(sorted.begin(), sorted.end());
  if (std::unique(sorted.begin(), sorted.end()) != sorted.end())
    throw std::invalid_argument("occluder sphere indices must be unique");
  for (int idx : occluders_) {
    if (idx < 0 || idx >= static_cast<int>(spheres_.size()))
      throw std::invalid_argument("occluder sphere index out of range");
  }

  // Occluders may overlap each other, but none may intersect a receiver
  // surface (every sphere not designated as an occluder).
  for (int oi : occluders_) {
    for (int j = 0; j < static_cast<int>(spheres_.size()); ++j) {
      if (std::binary_search(sorted.begin(), sorted.end(), j)) continue;
      const Sphere &a = spheres_[oi];
      const Sphere &b = spheres_[j];
      const double centerDistance = length(a.p - b.p);
      const double touch = a.rad + b.rad;
      if (!(centerDistance > touch + kGeoTol * std::max(1.0, touch)))
        throw std::invalid_argument(
            "occluder sphere must not intersect a receiver surface");
    }
  }
}

double VisibilityEngine::uniform01() { return unit_(rng_); }

bool VisibilityEngine::frontLightHit(const Vec &origin, const Vec &dir,
                                     double &distance) const {
  const double denom = dir.dot(light_.normal);
  if (denom >= -kRayEps) return false;                 // back face or parallel
  const double t = (light_.center - origin).dot(light_.normal) / denom;
  if (!(t > kRayEps)) return false;
  const Vec hit = origin + dir * t;
  const Vec rel = hit - light_.center;
  const double u = rel.dot(light_.axis1);
  const double v = rel.dot(light_.axis2);
  if (std::fabs(u) > light_.half1 * (1.0 + kCoordTol) + kCoordTol ||
      std::fabs(v) > light_.half2 * (1.0 + kCoordTol) + kCoordTol)
    return false;
  distance = t;
  return true;
}

bool VisibilityEngine::segmentBlocked(const Vec &origin, const Vec &dir,
                                      double tMax, int excludedSphere) {
  for (int i = 0; i < static_cast<int>(spheres_.size()); ++i) {
    if (i == excludedSphere) continue;
    const double t = spheres_[i].intersect(Ray(origin, dir));
    if (t > 0.0 && t < tMax - paramSlack(tMax)) return true;
  }
  return false;
}

VisibilityEngine::DirectSample
VisibilityEngine::computeDirect(const Vec &point, const Vec &normal,
                                const Vec &albedo,
                                int areaSamples, int boundarySamples) {
  if (!finiteVec(point) || !finiteVec(normal) || !finiteVec(albedo))
    throw std::invalid_argument("query contains non-finite data");
  if (std::fabs(length(normal) - 1.0) > kUnitTol)
    throw std::invalid_argument("receiver normal must be a unit vector");
  if (albedo.x < 0.0 || albedo.y < 0.0 || albedo.z < 0.0)
    throw std::invalid_argument("receiver albedo must be non-negative");
  if (areaSamples <= 0 || boundarySamples <= 0)
    throw std::invalid_argument("sample counts must be positive");
  for (int idx : occluders_) {
    const Sphere &s = spheres_[idx];
    const double d = length(s.p - point);
    if (!(d > s.rad + kGeoTol * std::max(1.0, s.rad)))
      throw std::invalid_argument(
          "query point must be strictly outside every designated sphere");
  }

  DirectSample out;
  out.gradient.assign(occluders_.size(), RGBGradient{});

  // ---- Color: uniform area sampling of the rectangle ---------------------
  const double lightArea = 4.0 * light_.half1 * light_.half2;
  for (int s = 0; s < areaSamples; ++s) {
    const double u = (2.0 * uniform01() - 1.0) * light_.half1;
    const double v = (2.0 * uniform01() - 1.0) * light_.half2;
    const Vec lightPoint =
        light_.center + light_.axis1 * u + light_.axis2 * v;
    const Vec delta = lightPoint - point;
    const double dist2 = delta.dot(delta);
    if (!(dist2 > 0.0)) continue;
    const double dist = std::sqrt(dist2);
    const Vec toward = delta * (1.0 / dist);
    const double cosReceiver = normal.dot(toward);
    const double cosLight = -toward.dot(light_.normal);
    if (cosReceiver <= kCosTol || cosLight <= kCosTol) continue;
    ++out.segmentQueries;
    // Every sphere participates, including the sphere carrying the receiver
    // point: its near root is rejected by the positive-root / 1e-4 distance
    // tolerance inside Sphere::intersect rather than by skipping the sphere.
    if (segmentBlocked(point, toward, dist, -1)) continue;
    const double weight = lightArea * cosReceiver * cosLight /
                          (static_cast<double>(areaSamples) * kPi * dist2);
    out.color = out.color + albedo.mult(light_.emission) * weight;
  }

  // ---- Gradient: tangent-cone boundary integration -----------------------
  for (int k = 0; k < static_cast<int>(occluders_.size()); ++k) {
    const Sphere &sph = spheres_[occluders_[k]];
    const Vec d = sph.p - point;
    const double D = length(d);
    const double R = sph.rad;
    const double L = std::sqrt(D * D - R * R); // tangent length, > 0
    const Vec axis = d * (1.0 / D);
    const double sinAlpha = R / D;
    const double cosAlpha = L / D;

    Vec helper = std::fabs(axis.x) > 0.9 ? Vec(0.0, 1.0, 0.0)
                                         : Vec(1.0, 0.0, 0.0);
    const Vec e1 = normalized(cross(helper, axis));
    const Vec e2 = cross(axis, e1);

    static const Vec axisBasis[3] = {Vec(1, 0, 0), Vec(0, 1, 0),
                                     Vec(0, 0, 1)};
    const double arcStep = 2.0 * kPi / boundarySamples;

    for (int j = 0; j < boundarySamples; ++j) {
      ++out.boundarySamples;
      const double phi = arcStep * j;
      const Vec circle = e1 * std::cos(phi) + e2 * std::sin(phi);
      const Vec w = (axis * cosAlpha + circle * sinAlpha); // unit direction
      const double cosReceiver = normal.dot(w);
      if (cosReceiver <= kCosTol) continue;

      // The cone ray touches the sphere at distance L; the next gate enforces
      // that this tangent point is strictly before the light plane.
      double tLight = 0.0;
      if (!frontLightHit(point, w, tLight)) continue;        // rectangle / face
      if (!(tLight > L + paramSlack(tLight))) continue;      // tangent first
      ++out.segmentQueries;
      if (segmentBlocked(point, w, tLight, occluders_[k]))
        continue;                                            // covered boundary

      ++out.arcsKept;
      const double arcMeasure = arcStep * sinAlpha; // ds = (R/D) dphi
      double rhoLe[3] = {albedo.x * light_.emission.x,
                         albedo.y * light_.emission.y,
                         albedo.z * light_.emission.z};
      for (int a = 0; a < 3; ++a) {
        // Signed contour normal velocity on the unit sphere for a unit move
        // of the sphere center along basis axis a.  With A = dhat . q and
        // B = circle . q, the outward normal of the silhouette small circle
        // is n_out = circle cos(alpha) - dhat sin(alpha), and differentiating
        // the moving circle gives v_n = (L B - R A) / (D L).
        const double A = axis.dot(axisBasis[a]);
        const double B = circle.dot(axisBasis[a]);
        const double normalVelocity = (L * B - R * A) / (D * L);
        const double contribution =
            cosReceiver * normalVelocity * arcMeasure / kPi;
        double *g = &out.gradient[k][a].x;
        for (int c = 0; c < 3; ++c)
          g[c] -= rhoLe[c] * contribution;
      }
    }
  }

  totalBoundary_ += out.boundarySamples;
  totalArcs_ += out.arcsKept;
  totalSegments_ += out.segmentQueries;
  return out;
}

QueryResult VisibilityEngine::query(const Vec &point, const Vec &normal,
                                    const Vec &albedo,
                                    int areaSamples, int boundarySamples) {
  const DirectSample s = computeDirect(point, normal, albedo,
                                       areaSamples, boundarySamples);
  QueryResult r;
  r.color = s.color;
  r.gradient = s.gradient;
  r.boundarySamples = s.boundarySamples;
  r.arcsKept = s.arcsKept;
  r.segmentQueries = s.segmentQueries;
  return r;
}

Image VisibilityEngine::render(const Camera &camera, int width, int height,
                               int areaSamples, int boundarySamples) {
  if (width <= 0 || height <= 0)
    throw std::invalid_argument("image dimensions must be positive");
  if (areaSamples <= 0 || boundarySamples <= 0)
    throw std::invalid_argument("sample counts must be positive");
  if (!finiteVec(camera.origin) || !finiteVec(camera.forward) ||
      !finiteVec(camera.viewUp))
    throw std::invalid_argument("camera contains non-finite data");
  if (std::fabs(length(camera.forward) - 1.0) > kUnitTol)
    throw std::invalid_argument("camera forward must be a unit vector");
  if (!(camera.fovY > 0.0 && camera.fovY < 180.0))
    throw std::invalid_argument("camera fovY must lie in (0, 180) degrees");
  const Vec fwd = normalized(camera.forward);
  if (length(cross(camera.viewUp, fwd)) < kUnitTol)
    throw std::invalid_argument("camera viewUp must not be parallel to forward");
  const Vec right = normalized(cross(camera.viewUp, fwd));
  const Vec up = cross(fwd, right);
  const double tanHalf = std::tan(0.5 * camera.fovY * kPi / 180.0);
  const double aspect = static_cast<double>(width) / height;

  Image image;
  image.width = width;
  image.height = height;
  image.color.assign(width * height, Vec());
  image.gradient.assign(occluders_.size(),
                        std::vector<RGBGradient>(width * height, RGBGradient{}));

  const std::uint64_t boundaryBefore = totalBoundary_;
  const std::uint64_t arcsBefore = totalArcs_;
  const std::uint64_t segmentsBefore = totalSegments_;

  for (int py = 0; py < height; ++py) {
    for (int px = 0; px < width; ++px) {
      const int pixel = py * width + px;
      const double sx =
          (2.0 * (px + 0.5) / width - 1.0) * tanHalf * aspect;
      const double sy =
          (1.0 - 2.0 * (py + 0.5) / height) * tanHalf;
      const Vec dir = normalized(fwd + right * sx + up * sy);

      double tSphere = 1e20;
      int hitId = -1;
      for (int i = 0; i < static_cast<int>(spheres_.size()); ++i) {
        const double t = spheres_[i].intersect(Ray(camera.origin, dir));
        if (t > 0.0 && t < tSphere) {
          tSphere = t;
          hitId = i;
        }
      }
      double tLight = 0.0;
      const bool lightHit = frontLightHit(camera.origin, dir, tLight);

      if (lightHit &&
          (hitId < 0 || tLight < tSphere - paramSlack(tSphere))) {
        image.color[pixel] = light_.emission;       // light front face
        continue;
      }
      if (hitId < 0) continue;                       // miss / light back: black

      bool isOccluder = false;
      for (int idx : occluders_)
        if (idx == hitId) isOccluder = true;
      if (isOccluder) continue;                      // black, zero gradient

      const Sphere &obj = spheres_[hitId];
      const Vec hitPoint = camera.origin + dir * tSphere;
      Vec geomNormal = normalized(hitPoint - obj.p);
      if (geomNormal.dot(dir) > 0.0)
        geomNormal = geomNormal * -1.0;              // face-forward normal

      bool inside = false;
      for (int idx : occluders_) {
        if (length(spheres_[idx].p - hitPoint) <= spheres_[idx].rad)
          inside = true;
      }
      if (inside) continue;                          // no legal query point

      // First-hit position, normal, material and primary visibility are held
      // fixed: only the designated occluders move inside this query.
      const DirectSample s =
          computeDirect(hitPoint, geomNormal, obj.c,
                        areaSamples, boundarySamples);
      image.color[pixel] = s.color;
      for (int k = 0; k < static_cast<int>(occluders_.size()); ++k)
        image.gradient[k][pixel] = s.gradient[k];
    }
  }

  image.boundarySamples = totalBoundary_ - boundaryBefore;
  image.arcsKept = totalArcs_ - arcsBefore;
  image.segmentQueries = totalSegments_ - segmentsBefore;
  return image;
}

} // namespace vg
