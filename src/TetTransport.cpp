#include "TetTransport.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <stdexcept>

#include "RayTracer.h"

namespace tetvol {

namespace {

bool finiteVec(const Vector& v) {
   return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Barycentric coordinates of p(t) = origin + t * dir relative to a cell.
// k = 1..3 use the stored inverse rows; k = 0 is derived from the unit sum.
struct BaryLine {
   double b[4]; // intercept lambda_k(0)
   double s[4]; // slope d lambda_k / dt
};

BaryLine baryLine(const TetPose::InvMap& m, const Vector& origin,
                  const Vector& dir) {
   Vector rel = origin - m.p0;
   BaryLine L;
   L.b[1] = m.row1.dot(rel);
   L.b[2] = m.row2.dot(rel);
   L.b[3] = m.row3.dot(rel);
   L.b[0] = 1.0 - L.b[1] - L.b[2] - L.b[3];
   L.s[1] = m.row1.dot(dir);
   L.s[2] = m.row2.dot(dir);
   L.s[3] = m.row3.dot(dir);
   L.s[0] = -(L.s[1] + L.s[2] + L.s[3]);
   return L;
}

// Independent half-space clipping: intersect the four barycentric
// non-negativity half-spaces with the ray window.  Returns false for
// contacts without positive length.  A ray running exactly on a shared face
// or edge is attributed uniquely to the smallest owning cell.
bool clipCell(const TetPose& pose, std::size_t cell, const Vector& origin,
              const Vector& dir, double t0, double t1, double& tEnter,
              double& tExit) {
   const TetVolume& vol = pose.volume();
   const TetPose::InvMap& m = pose.inverseMap(cell);
   BaryLine L = baryLine(m, origin, dir);

   double scaleB = 1.0;
   double scaleS = 1.0;
   for (int k = 0; k < 4; ++k) {
      scaleB = std::max(scaleB, std::fabs(L.b[k]));
      scaleS = std::max(scaleS, std::fabs(L.s[k]));
   }
   const double epsB = 1e-13 * scaleB;
   const double epsS = 1e-13 * scaleS;

   double lo = t0;
   double hi = t1;
   int zeroCoord[4];
   int zeroCount = 0;
   for (int k = 0; k < 4; ++k) {
      if (std::fabs(L.s[k]) <= epsS) {
         if (L.b[k] < -epsB) return false;
         if (std::fabs(L.b[k]) <= epsB) zeroCoord[zeroCount++] = k;
         continue;
      }
      double t = -L.b[k] / L.s[k];
      if (L.s[k] > 0.0) {
         lo = std::max(lo, t);
      } else {
         hi = std::min(hi, t);
      }
      if (lo >= hi) return false;
   }
   if (!(hi > lo)) return false;

   if (zeroCount == 1) {
      // The ray lies in the face opposite the zero barycentric coordinate.
      int ids[3];
      int n = 0;
      for (int k = 0; k < 4; ++k) {
         if (k != zeroCoord[0]) ids[n++] = vol.cell(cell).v[k];
      }
      std::sort(ids, ids + 3);
      if (vol.faceOwner(ids[0], ids[1], ids[2]) !=
          static_cast<int>(cell)) {
         return false;
      }
   } else if (zeroCount >= 2) {
      // The ray lies on an edge: only the two non-zero coordinates survive.
      int ids[4];
      int n = 0;
      for (int k = 0; k < 4; ++k) {
         bool isZero = false;
         for (int z = 0; z < zeroCount; ++z) {
            if (zeroCoord[z] == k) isZero = true;
         }
         if (!isZero) ids[n++] = vol.cell(cell).v[k];
      }
      if (n != 2) {
         // A contact with fewer than two spanning vertices has no positive
         // length inside the cell.
         return false;
      }
      if (ids[0] > ids[1]) std::swap(ids[0], ids[1]);
      if (vol.edgeOwner(ids[0], ids[1]) != static_cast<int>(cell)) {
         return false;
      }
   }

   tEnter = lo;
   tExit = hi;
   return true;
}

} // namespace

TetCursor::TetCursor(std::shared_ptr<const TetPose> pose, const Vector& origin,
                     const Vector& dir, double t0, double t1)
   : pose_(std::move(pose)), origin_(origin), dir_(dir), t0_(t0), t1_(t1),
     nodeTests_(0), cellTests_(0) {
   if (!finiteVec(origin)) {
      throw std::invalid_argument("TetCursor: non-finite ray origin");
   }
   if (!finiteVec(dir) ||
       (dir.x == 0.0 && dir.y == 0.0 && dir.z == 0.0)) {
      throw std::invalid_argument("TetCursor: direction must be finite and non-zero");
   }
   if (!std::isfinite(t0) || !std::isfinite(t1) || t0 > t1) {
      throw std::invalid_argument("TetCursor: invalid parameter window");
   }
   generation_ = pose_->generation();

   // Lazy best-first traversal: only the root is queued.  Cells are tested in
   // entry order as next() pulls work out, so the whole ray's cells are never
   // enumerated up front.
   heap_.push(Work{t0_, false, false, pose_->rootIndex()});
}

TetCursor::~TetCursor() = default;

bool TetCursor::next(TetInterval& out) {
   if (pose_->generation() != generation_) {
      throw std::logic_error(
         "TetCursor: pose was updated; stale cursor cannot be read");
   }

   while (!heap_.empty()) {
      Work w = heap_.top();
      heap_.pop();

      if (!w.isCell) {
         const TetPose::NodeView node = pose_->nodeView(w.index);
         ++nodeTests_;
         double lo, hi;
         if (!rayAABB(origin_, dir_, t0_, t1_, node.bounds, lo, hi)) {
            continue;
         }
         if (node.leaf) {
            for (int i = node.first; i < node.last; ++i) {
               int cell = pose_->leafCell(i);
               double clo, chi;
               if (rayAABB(origin_, dir_, t0_, t1_, pose_->cellBounds(cell),
                           clo, chi)) {
                  heap_.push(Work{clo, true, false, cell});
               }
            }
         } else {
            heap_.push(Work{lo, false, false, node.right});
            heap_.push(Work{lo, false, false, node.left});
         }
         continue;
      }

      if (!w.tested) {
         ++cellTests_;
         double tEnter, tExit;
         if (!clipCell(*pose_, static_cast<std::size_t>(w.index), origin_,
                       dir_, t0_, t1_, tEnter, tExit)) {
            continue; // AABB overlap without a positive-length hit
         }
         // Any node/cell still queued with a smaller lower bound must be
         // expanded first so emitted intervals stay globally ordered.
         if (!heap_.empty() && heap_.top().key < tEnter) {
            heap_.push(Work{tEnter, true, true, w.index});
            continue;
         }
         out.cell = w.index;
         out.tEnter = tEnter;
         out.tExit = tExit;
         return true;
      }

      // Reached after every smaller-key candidate was expanded: emit the
      // interval whose clip result is fixed by the captured inverse maps.
      double tEnter, tExit;
      if (!clipCell(*pose_, static_cast<std::size_t>(w.index), origin_, dir_,
                    t0_, t1_, tEnter, tExit)) {
         continue;
      }
      out.cell = w.index;
      out.tEnter = tEnter;
      out.tExit = tExit;
      return true;
   }
   return false;
}

TetSegment integrateSegment(const TetPose& pose, int cellIndex,
                            const Vector& origin, const Vector& dir,
                            double tEnter, double tExit) {
   const TetVolume& vol = pose.volume();
   const TetCell& cell = vol.cell(static_cast<std::size_t>(cellIndex));
   const TetPose::InvMap& m =
      pose.inverseMap(static_cast<std::size_t>(cellIndex));
   BaryLine L = baryLine(m, origin, dir);

   // Density is a scalar carried by rest barycentric coordinates, so along
   // the line rho(t) is affine: rho(t) = rho0 + rho1 * t.
   double rho0 = 0.0;
   double rho1 = 0.0;
   for (int k = 0; k < 4; ++k) {
      double d = vol.vertexDensity(cell.v[k]);
      rho0 += d * L.b[k];
      rho1 += d * L.s[k];
   }

   double dt = tExit - tEnter;
   double arc = std::sqrt(dir.dot(dir)); // world arc length per unit t
   // Analytic double-precision integral of rho over world arc length.
   double q = arc * (rho0 * dt +
                     0.5 * rho1 * (tExit * tExit - tEnter * tEnter));

   TetSegment seg;
   seg.q = q;
   const double sig[3] = {cell.sigma.r, cell.sigma.g, cell.sigma.b};
   const double src[3] = {cell.source.r, cell.source.g, cell.source.b};
   double* T = &seg.transmittance.r;
   double* C = &seg.emission.r;
   for (int ch = 0; ch < 3; ++ch) {
      T[ch] = std::exp(-sig[ch] * q);
      C[ch] = src[ch] * (1.0 - T[ch]); // sig == 0 gives T == 1, C == 0
   }
   return seg;
}

TetRayTransport integrateRay(std::shared_ptr<const TetPose> pose,
                             const Vector& origin, const Vector& dir,
                             double t0, double t1) {
   TetRayTransport result;
   result.transmittance = RGB(1.0, 1.0, 1.0);
   result.emission = RGB(0.0, 0.0, 0.0);

   TetCursor cursor(pose, origin, dir, t0, t1);
   TetInterval interval;
   while (cursor.next(interval)) {
      TetSegment seg = integrateSegment(*pose, interval.cell, origin, dir,
                                        interval.tEnter, interval.tExit);
      result.emission.r += result.transmittance.r * seg.emission.r;
      result.emission.g += result.transmittance.g * seg.emission.g;
      result.emission.b += result.transmittance.b * seg.emission.b;
      result.transmittance.r *= seg.transmittance.r;
      result.transmittance.g *= seg.transmittance.g;
      result.transmittance.b *= seg.transmittance.b;
   }
   return result;
}

} // namespace tetvol

// Defined here rather than in RayTracer.cpp so the only reference to the
// tetvol transport entry point lives in this translation unit.  Builds that
// never call setVolume (and do not link TetTransport.cpp) keep the original
// surface-only linker surface.
void RayTracer::setVolume(std::shared_ptr<tetvol::TetPose> pose) {
   volumePose = std::move(pose);
   if (!volumePose) {
      integrateRayHook_ = nullptr;
      return;
   }
   std::shared_ptr<const tetvol::TetPose> captured = volumePose;
   integrateRayHook_ = [captured](const Vector& origin, const Vector& dir,
                                  double t0, double t1) {
      return tetvol::integrateRay(captured, origin, dir, t0, t1);
   };
}
