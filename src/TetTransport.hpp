#ifndef TET_TRANSPORT_HPP
#define TET_TRANSPORT_HPP

// Finite-ray queries and analytic transport through a posed tetrahedral
// volume.
//
// TetCursor is a forward cursor that returns the tetrahedral intervals met by
// a finite ray in increasing entry parameter.  A cursor captures the pose
// generation at construction; reading a cursor after a successful pose update
// throws std::logic_error so stale candidates can never be mixed with fresh
// inverse maps.
//
// All queries take a raw origin, a non-zero direction that need not be
// normalized, and a parameter window [t0, t1].  The existing Ray class's
// normalization and origin offset are deliberately not used here.

#include <cstdint>
#include <memory>
#include <queue>
#include <vector>

#include "TetVolume.hpp"

namespace tetvol {

struct TetInterval {
   int cell;
   double tEnter;
   double tExit;
};

class TetCursor {
public:
   // Throws std::invalid_argument for a zero/non-finite direction or an
   // invalid window.  Only the BVH root is queued here; the ray is walked
   // lazily so the whole ray's cells are never enumerated up front.  The
   // pose generation and the per-query node/cell test counters are captured.
   TetCursor(std::shared_ptr<const TetPose> pose, const Vector& origin,
             const Vector& dir, double t0, double t1);

   ~TetCursor();

   // Returns the next positive-length tetrahedral interval in increasing
   // entry parameter, or false when the ray window is exhausted.  Throws
   // std::logic_error if the captured generation no longer matches the pose.
   bool next(TetInterval& out);

   std::uint64_t generation() const { return generation_; }
   std::uint64_t nodeTests() const { return nodeTests_; }
   std::uint64_t cellTests() const { return cellTests_; }

private:
   struct Work {
      double key;
      bool isCell;
      bool tested; // cell already ran its (failing/deferred) clip test
      int index;
      bool operator>(const Work& other) const {
         if (key != other.key) return key > other.key;
         if (isCell != other.isCell) return static_cast<int>(isCell) >
                                         static_cast<int>(other.isCell);
         return index > other.index;
      }
   };

   std::shared_ptr<const TetPose> pose_;
   Vector origin_;
   Vector dir_;
   double t0_;
   double t1_;
   std::uint64_t generation_;
   std::uint64_t nodeTests_;
   std::uint64_t cellTests_;
   std::priority_queue<Work, std::vector<Work>, std::greater<Work>> heap_;
};

// Per-channel result of analytic transport over one interval.
struct TetSegment {
   RGB transmittance; // T = exp(-sigma * q) per channel
   RGB emission;      // C = S * (1 - T) per channel
   double q;          // integral of barycentric density over world arc length
};

// Analytic transport for one interval.  Barycentric density is linear along
// the line, so q is integrated in double precision; ds is world arc length
// (the direction may be unnormalized).
TetSegment integrateSegment(const TetPose& pose, int cell, const Vector& origin,
                            const Vector& dir, double tEnter, double tExit);

// Accumulated transport over a whole finite ray, near to far, with no
// re-integration at shared cell boundaries.
struct TetRayTransport {
   RGB transmittance; // Tacc over the window
   RGB emission;      // Cacc over the window
};

TetRayTransport integrateRay(std::shared_ptr<const TetPose> pose,
                             const Vector& origin, const Vector& dir,
                             double t0, double t1);

} // namespace tetvol

#endif
